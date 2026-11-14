#ifndef RUBI_MEDIA_H
#define RUBI_MEDIA_H

// Rubi touch menu + music player from the SD card (WAV files in /rubi/music).
// Tap the menu button (bottom-right) to open the menu, then tap an icon (like on a phone).
// The touch panel is calibrated once (two taps) and the result is saved.
// While the menu is open the wake word is switched off so the music cannot wake Rubi.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>

// Microphone diagnostics, defined in the audio codec (patches/apply_extmic.py)
extern volatile int g_rubi_mic_state, g_rubi_mic_sd, g_rubi_mic_peak, g_rubi_mic_rms, g_rubi_mic_sdl, g_rubi_mic_sdr;
extern volatile int g_rubi_mic_force, g_rubi_cap_len, g_rubi_cap_pos, g_rubi_cap_clip;
extern int16_t* volatile g_rubi_cap_buf;
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

#include "application.h"
#include <esp_app_desc.h>
#include "assets/lang_config.h"
#include <esp_wifi.h>
#include <esp_netif.h>
#include <ssid_manager.h>
#include <wifi_manager.h>
#include "board.h"
#include "rubi_battery.h"
#include "rubi_face.h"
#include "rubi_remote.h"
#include "rubi_icons.h"
#include "rubi_viec_icons.h"
#include "rubi_learn_data.h"
#include "rubi_upload.h"
#include "rubi_sync.h"
#include "rubi_math.h"
#include "settings.h"

class RubiMedia {
public:
    static RubiMedia& Get() {
        static RubiMedia m;
        return m;
    }

    void SetDisplay(RubiDisplay* d) {
        display_ = d;
        static bool started = false;
        if (!started && d != nullptr) {
            started = true;
            xTaskCreatePinnedToCore(&RubiMedia::AutoSyncTask, "rubi_autosync", 10240, this, 2, nullptr, 1);
            xTaskCreatePinnedToCore(&RubiMedia::StoryReportTask, "rubi_storyrep", 6144, this, 2, nullptr, 1);
            RubiBattery::OnCommand() = [](const std::string& c) { RubiMedia::Get().RemoteAction(c); };
            RubiBattery::OnOpenStory() = [](const std::string& dir) { RubiMedia::Get().RemoteOpenStory(dir); };
            // A parent starts talking from far away: stop the music so the voice is heard alone
            RubiRemote::Takeover() = []() { RubiMedia::Get().StopPlayback(); };
        }
    }
    void SetWifiCallback(std::function<void()> cb) { wifi_cb_ = std::move(cb); }
    bool IsOpen() const { return screen_ != Screen::kNone; }
    bool CalibrationValid() { LoadCalibration(); return cal_.valid; }

    void OpenMenu() {
        if (display_ == nullptr || IsOpen()) return;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
        sync_cancel_ = true;   // a background store sync must stop before the menu uses the SD card
        for (int i = 0; i < 150 && syncing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (syncing_) return;
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        LoadCalibration();
        if (cal_.valid) ShowMain(); else StartCalibration();
    }

    // Swipe up from the bottom edge (the home bar) leaves the menu, like the iPhone. Returns true if it was a swipe.
    // fx,fy = first raw touch point, tx,ty = last raw touch point.
    bool HandleSwipe(uint32_t press_ms, uint16_t fx, uint16_t fy, uint16_t tx, uint16_t ty) {
        if (!IsOpen() || !cal_.valid || press_ms > 2500) return false;
        if (screen_ >= Screen::kCalib1 && screen_ <= Screen::kCalib4) return false;
        int x0, y0, x1, y1;
        Map(fx, fy, x0, y0);
        Map(tx, ty, x1, y1);
        int dy = y0 - y1, dx = x1 - x0;
        if (dx < 0) dx = -dx;
        if (y0 < 205 || dy < 40 || dx > dy) return false;
        ESP_LOGI("RubiTouch", "swipe up %d,%d -> %d,%d (screen %d)", x0, y0, x1, y1, (int)screen_);
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        Close();
        return true;
    }

    // Called when a finger is lifted. raw_x/raw_y = last raw touch point. Returns true if consumed.
    bool HandleTouch(uint32_t press_ms, uint16_t raw_x, uint16_t raw_y) {
        if (voice_mode_ && playing_) {   // music started by voice: any touch stops it
            VoiceStop();
            return true;
        }
        if (!IsOpen()) {
            // Closed: a tap on the menu button (bottom-right) opens the menu. Needs the touch calibration.
            if (!CalibrationValid() || display_ == nullptr) return false;
            int bx = 0, by = 0;
            Map(raw_x, raw_y, bx, by);
            if (RubiDisplay::InMenuButton(bx, by) &&
                Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                OpenMenu();
                return true;
            }
            return false;
        }
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        switch (screen_) {
        case Screen::kCalib1:
        case Screen::kCalib2:
        case Screen::kCalib3:
        case Screen::kCalib4: {
            int idx = (int)screen_ - (int)Screen::kCalib1;
            cal_raw_[idx][0] = raw_x; cal_raw_[idx][1] = raw_y;
            ESP_LOGI("RubiTouch", "calibration point %d raw %u,%u", idx + 1, (unsigned)raw_x, (unsigned)raw_y);
            if (idx < 3) {
                screen_ = (Screen)((int)screen_ + 1);
                ShowCalibTarget(idx + 1);
            } else {
                FinishCalibration();
            }
            return true;
        }
        default:
            break;
        }
        int sx = 0, sy = 0;
        Map(raw_x, raw_y, sx, sy);
        ESP_LOGI("RubiTouch", "raw %u,%u -> screen %d,%d (screen %d)", (unsigned)raw_x, (unsigned)raw_y, sx, sy, (int)screen_);
        switch (screen_) {
        case Screen::kMain: {
            int t = RubiDisplay::TileAt(sx, sy, 8);
            if (t == 0) { LoadSongs(); page_ = 0; ShowMusic(); }
            else if (t == 1) { ShowLearnTopics(); }
            else if (t == 2) { spage_ = 0; LoadStories(); ShowStoryList(); }
            else if (t == 3) { ShowMathMenu(); }
            else if (t == 4) { ShowClock(); }
            else if (t == 5) { ShowPlay(); }
            else if (t == 6) { ShowSettings(); }
            else if (t == 7) { ShowEnglish(); }
            break;
        }
        case Screen::kSettings: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            if (r == 0) ShowVolume();
            else if (r == 1) StartWifiScan();
            else if (r == 2) ShowControl();
            else if (r == 3) ShowMicMenu();
            else if (r == 4) ShowMain();
            break;
        }
        case Screen::kMicMenu: {
            int r = RubiDisplay::RowAt(sx, sy, 4);
            if (r == 0) StartMicTest();
            else if (r == 1) {
                int f = (g_rubi_mic_force + 1) % 3;   // automatic -> external -> on-board -> automatic
                g_rubi_mic_force = f;
                Settings st("rubi", true);
                st.SetInt("mic", f);
                ShowMicMenu();
            }
            else if (r == 2) StartCalibration();
            else if (r == 3) ShowSettings();
            break;
        }
        case Screen::kMicDone:
            ShowMicMenu();
            break;
        case Screen::kIp:
            ShowControl();
            break;
        case Screen::kPlay: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            if (r == 0) StartDance();
            else if (r == 1) StartSleep();
            else if (r == 2) ShowTimerMenu();
            else if (r == 3) ShowSched();
            else if (r == 4) ShowMain();
            break;
        }
        case Screen::kSched:
            ShowPlay();
            break;
        case Screen::kRemind:   // the child touched the screen: reminder done, close the menu
            tick_gen_++;
            Close();
            break;
        case Screen::kClock:
            tick_gen_++;
            if (back_play_) ShowPlay(); else ShowMain();
            break;
        case Screen::kTimerDone:
            tick_gen_++;
            ShowPlay();
            break;
        case Screen::kTimerMenu: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            static const int mins[4] = {2, 5, 10, 20};
            if (r >= 0 && r <= 3) StartTimer(mins[r]);
            else if (r == 4) ShowPlay();
            break;
        }
        case Screen::kTimerRun:
            tick_gen_++;
            ShowTimerMenu();
            break;
        case Screen::kSleep:
            EndSleep();
            break;
        case Screen::kUpload:
            RubiUpload::Stop();
            LoadSongs();
            ShowControl();
            break;
        case Screen::kMathMenu: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            if (r >= 0 && r <= 3) StartMath((RubiMath::Mode)r);
            else if (r == 4) ShowMain();
            break;
        }
        case Screen::kMathStory:
            RenderMath("");
            break;
        case Screen::kMathKeys:
            HandleMathTouch(sx, sy);
            break;
        case Screen::kMathBusy:
            break;
        case Screen::kMathDone:
            ShowMathMenu();
            break;
        case Screen::kSync:
            sync_cancel_ = true;
            for (int i = 0; i < 150 && syncing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
            LoadSongs();
            ShowControl();
            break;
        case Screen::kDance:
            RubiBattery::Send("X\n", 2);   // stop the dance
            ShowPlay();
            break;
        case Screen::kCtl: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            if (r == 0) ShowRobotAddress();
            else if (r == 1) StartPinChange();
            else if (r == 2) StartUpload();
            else if (r == 3) StartSync();
            else if (r == 4) ShowSettings();
            break;
        }
        case Screen::kPinKeys:
            HandlePinTouch(sx, sy);
            break;
        case Screen::kPinBusy:
            break;
        case Screen::kStoryList: {
            int r = RubiDisplay::RowAt(sx, sy, (int)story_row_action_.size());
            if (r < 0) break;
            int act = story_row_action_[r];
            if (act >= 0) OpenStory(act);
            else if (act == -1) { spage_++; ShowStoryList(); }
            else ShowMain();
            break;
        }
        case Screen::kStoryPage: {
            int b = RubiDisplay::StoryAt(sx, sy);
            StopPlayback();
            if (b == 1) { scene_gen_++; ShowStoryList(); }
            else if (b == 0) { if (scene_ > 0) scene_--; ShowScene(); }
            else if (b == 2) {
                if (scene_ + 1 < (int)scenes_.size()) { scene_++; ShowScene(); }
                else { scene_gen_++; ShowStoryList(); }
            }
            else { ShowScene(); }   // tap in the middle: read this page again
            break;
        }
        case Screen::kWifiBusy:
            break;   // working: ignore touches
        case Screen::kWifiList: {
            int r = RubiDisplay::RowAt(sx, sy, (int)wifi_row_action_.size());
            if (r < 0) break;
            int act = wifi_row_action_[r];
            if (act >= 0 && act < (int)nets_.size()) {
                sel_ssid_ = nets_[act].ssid;
                sel_open_ = nets_[act].open;
                pass_.clear();
                kb_layer_ = 0;
                show_pw_ = false;
                if (sel_open_) ConnectWifi(); else RenderKeyboard("");
            }
            else if (act == -1) { wpage_++; ShowWifiList(); }
            else if (act == -2) { StartWifiScan(); }
            else if (act == -3) { ShowSettings(); }
            else if (act == -4) { Close(); if (wifi_cb_) wifi_cb_(); }
            break;
        }
        case Screen::kWifiKeys:
            HandleKeyboardTouch(sx, sy);
            break;
        case Screen::kVolume: {
            int r = RubiDisplay::RowAt(sx, sy, 4);
            auto* codec = Board::GetInstance().GetAudioCodec();
            int v = codec->output_volume();
            if (r == 1) { codec->SetOutputVolume(std::min(100, v + 10)); ShowVolume(); }
            else if (r == 2) { codec->SetOutputVolume(std::max(10, v - 10)); ShowVolume(); }
            else if (r == 3) { ShowSettings(); }
            break;
        }
        case Screen::kLearnTopics: {
            int t = RubiDisplay::TileAt(sx, sy, 5);
            if (t >= 0 && t < 4) OpenTopic(t);
            else if (t == 4) ShowMain();
            break;
        }
        case Screen::kLearnEmpty:
            ShowLearnTopics();
            break;
        case Screen::kStoryEmpty:
            ShowStoryList();
            break;
        case Screen::kSoon:
            ShowMain();
            break;
        case Screen::kEngList: {
            int n = (int)eng_ids_.size();
            int r = RubiDisplay::RowAt(sx, sy, n + 1);
            if (r >= 0 && r < n) { eng_sel_ = r; ShowEngMenu(); }
            else if (r == n) ShowMain();
            break;
        }
        case Screen::kEngMenu: {
            int r = RubiDisplay::RowAt(sx, sy, 3);
            if (r == 0) OpenEngLesson();
            else if (r == 1) StartQuiz();
            else if (r == 2) ShowEnglish();
            break;
        }
        case Screen::kEngQuiz: {
            int r = RubiDisplay::RowAt(sx, sy, 5);
            if (r >= 0 && r < 3) AnswerQuiz(r);
            else if (r == 3) SpeakQuiz();
            else if (r == 4) { StopPlayback(); ShowEngMenu(); }
            break;
        }
        case Screen::kEngDone:
            ShowEngMenu();
            break;
        case Screen::kLearnCard: {
            int b = RubiDisplay::CardAt(sx, sy);
            if (b == 0 || b == 1 || b == 2) StopPlayback();   // stop a word that is still being read
            if (b == 0 && !words_.empty()) { word_ = (word_ + (int)words_.size() - 1) % (int)words_.size(); ShowCard(); }
            else if (b == 2 && !words_.empty()) { word_ = (word_ + 1) % (int)words_.size(); ShowCard(); }
            else if (b == 1) { if (eng_mode_) ShowEngMenu(); else ShowLearnTopics(); }
            else if (b == 3) { SpeakWord(); }
            break;
        }
        case Screen::kMusic: {
            int r = RubiDisplay::RowAt(sx, sy, (int)row_action_.size());
            if (r < 0) break;
            int act = row_action_[r];
            if (act >= 0) StartPlayback(act);
            else if (act == -1) { page_++; ShowMusic(); }
            else ShowMain();
            break;
        }
        case Screen::kPlaying: {
            int keep = now_index_;
            StopPlayback();
            page_ = keep / kSongsPerPage;
            ShowMusic();
            break;
        }
        default:
            break;
        }
        return true;
    }

    // ---------------- voice control (called from MCP tools) ----------------
    std::string VoiceList() {
        std::lock_guard<std::recursive_mutex> lk(voice_mutex_);
        if (IsOpen()) return "Menu đang mở trên màn hình, chưa dùng được lệnh nhạc.";
        if (!playing_) {
            LoadSongs();
            UnmountSd();
        }
        if (songs_.empty()) return "Thẻ nhớ chưa có bài nhạc nào hoặc chưa đọc được thẻ.";
        std::string r = "Có " + std::to_string(songs_.size()) + " bài: ";
        for (size_t i = 0; i < songs_.size() && i < 30; i++) {
            std::string n = songs_[i];
            if (n.size() > 4) n.resize(n.size() - 4);
            if (i) r += ", ";
            r += n;
        }
        return r;
    }

    std::string VoicePlay(const std::string& query) {
        std::lock_guard<std::recursive_mutex> lk(voice_mutex_);
        if (IsOpen()) return "Menu đang mở trên màn hình, chưa dùng được lệnh nhạc.";
        if (playing_ && voice_mode_ && (esp_timer_get_time() / 1000 - voice_start_ms_) < 15000) {
            // the same request arrived twice (spoken command + assistant tool): keep the first one
            if (query.empty() || FindSong(query) == now_index_) return "Nhạc đang phát rồi. Không cần nói gì thêm.";
        }
        if (playing_) VoiceStopLocked();
        LoadSongs();
        if (songs_.empty()) {
            UnmountSd();
            return "Thẻ nhớ chưa có bài nhạc nào hoặc chưa đọc được thẻ.";
        }
        int idx = 0;
        if (!query.empty()) {
            idx = FindSong(query);
            if (idx < 0) {
                UnmountSd();
                return "Không tìm thấy bài nào giống \"" + query + "\". Hãy dùng công cụ liệt kê bài rồi hỏi bé chọn bài.";
            }
        }
        VoiceStartAt(idx);
        std::string n = songs_[idx];
        if (n.size() > 4) n.resize(n.size() - 4);
        return "Đã bắt đầu phát bài: " + n + ". Chỉ nói ngắn gọn một câu rồi im lặng để nhạc phát.";
    }

    std::string VoiceStop() {
        std::lock_guard<std::recursive_mutex> lk(voice_mutex_);
        if (!playing_) return "Hiện không có nhạc nào đang phát.";
        VoiceStopLocked();
        return "Đã dừng nhạc.";
    }

    std::string VoiceStep(int delta) {
        std::lock_guard<std::recursive_mutex> lk(voice_mutex_);
        if (!playing_ || !voice_mode_ || songs_.empty()) return "Hiện không có nhạc nào đang phát.";
        int n = (int)songs_.size();
        int idx = ((now_index_ + delta) % n + n) % n;
        StopPlayback();
        VoiceStartAt(idx);
        std::string nm = songs_[idx];
        if (nm.size() > 4) nm.resize(nm.size() - 4);
        return "Đã chuyển sang bài: " + nm + ". Chỉ nói ngắn gọn một câu.";
    }

    // Called with every speech-to-text result, so "mở nhạc" works even if the cloud assistant
    // does not call the music tool. Runs the action in its own small task.
    void HandleSpeech(const std::string& text) {
        std::string f = Fold(text);
        auto has = [&](const char* w) { return f.find(w) != std::string::npos; };
        int cmd = 0;
        std::string q;
        if (has("tat nhac") || has("dung nhac") || has("ngung nhac") || has("thoi nhac") ||
            has("tat bai") || has("dung bai")) {
            cmd = 2;
        } else if (has("mo nhac") || has("bat nhac") || has("phat nhac") || has("nghe nhac") ||
                   has("mo bai") || has("bat bai") || has("phat bai")) {
            cmd = 1;
            // words after "nhac" / "bai" are the song name
            size_t p = std::string::npos;
            for (const char* k : {"nhac ", "bai "}) {
                size_t x = f.find(k);
                if (x != std::string::npos && (p == std::string::npos || x < p)) p = x + strlen(k);
            }
            if (p != std::string::npos) {
                static const char* kFiller[] = {"di", "nhe", "nha", "len", "voi", "ne", "nao", "cho", "be",
                                                "minh", "toi", "nghe", "duoc", "khong", "a"};
                bool first = true;
                for (auto& w : Words(f.substr(p))) {
                    bool filler = first && w == "hat";   // "mở bài hát X"
                    first = false;
                    for (auto* k : kFiller) if (w == k) { filler = true; break; }
                    if (!filler) q += (q.empty() ? "" : " ") + w;
                }
            }
        }
        if (cmd == 0) return;
        ESP_LOGI(kTag, "voice command %d, song query '%s'", cmd, q.c_str());
        struct Job { RubiMedia* self; int cmd; std::string q; };
        auto* job = new Job{this, cmd, q};
        BaseType_t ok = xTaskCreate([](void* a) {
            auto* j = static_cast<Job*>(a);
            if (j->cmd == 1) j->self->VoicePlay(j->q); else j->self->VoiceStop();
            delete j;
            vTaskDelete(nullptr);
        }, "rubi_voice", 6144, job, 3, nullptr);
        if (ok != pdPASS) delete job;
    }

private:
    enum class Screen { kNone, kCalib1, kCalib2, kCalib3, kCalib4, kMain, kMusic, kPlaying, kLearnTopics, kLearnCard, kLearnEmpty, kVolume, kWifiList, kWifiKeys, kWifiBusy, kSettings, kPlay, kDance, kUpload, kSync, kMathMenu, kMathStory, kMathKeys, kMathBusy, kMathDone, kCtl, kPinKeys, kPinBusy, kIp, kStoryList, kStoryPage, kStoryEmpty, kSoon, kEngList, kEngMenu, kEngQuiz, kEngDone, kClock, kTimerMenu, kTimerRun, kTimerDone, kSleep, kMicMenu, kMicBusy, kMicDone, kSched, kRemind };
    static constexpr const char* kTag = "RubiMedia";
    static constexpr const char* kMusicDir = "/sdcard/rubi/music";
    static constexpr int kSongsPerPage = 3;

    struct Calib {
        bool valid = false;
        bool swap = false;
        float ax = 1, bx = 0, ay = 1, by = 0;
    };

    RubiDisplay* display_ = nullptr;
    std::function<void()> wifi_cb_;
    std::recursive_mutex mutex_;
    Screen screen_ = Screen::kNone;
    std::vector<std::string> songs_;
    std::vector<int> row_action_;   // per visible row: song index, -1 = next page, -2 = back
    int page_ = 0;
    int now_index_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    sdmmc_card_t* card_ = nullptr;
    // SD usage report for the web page: total/free from the FAT, folder sizes cached and refreshed about once a minute
    uint64_t sd_kb_[3] = {0, 0, 0};
    int sd_n_[3] = {0, 0, 0};
    int sd_tick_ = 0;
    static void DirSum(const std::string& path, int depth, uint64_t& bytes, int& files) {
        DIR* d = opendir(path.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string full = path + "/" + n;
            struct stat st;
            if (stat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { if (depth < 4) DirSum(full, depth + 1, bytes, files); }
            else { bytes += (uint64_t)st.st_size; files++; }
        }
        closedir(d);
    }
    static int CountDirs(const std::string& path) {
        int c = 0;
        if (DIR* d = opendir(path.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                struct stat st;
                if (stat((path + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) c++;
            }
            closedir(d);
        }
        return c;
    }
    Calib cal_;
    bool cal_loaded_ = false;
    std::recursive_mutex voice_mutex_;
    std::atomic<bool> voice_mode_{false};
    int64_t voice_start_ms_ = 0;

    // ---------------- voice helpers ----------------
    void VoiceStartAt(int idx) {
        MountSd();
        voice_mode_ = true;
        stop_ = false;
        playing_ = true;
        now_index_ = idx;
        voice_start_ms_ = esp_timer_get_time() / 1000;
        xTaskCreatePinnedToCore(&RubiMedia::PlayTask, "rubi_play", 8192, this, 4, nullptr, 1);
    }

    void VoiceStopLocked() {
        StopPlayback();
        UnmountSd();
        voice_mode_ = false;
        VoiceHideScreen();
    }

    // voice music must never leave the touch menu "open" (that would block touch and later voice commands)
    void VoiceHideScreen() {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (screen_ == Screen::kPlaying || screen_ == Screen::kMusic) {
            screen_ = Screen::kNone;
            if (display_) display_->MenuHide();
        }
    }

    // lower-case ASCII with Vietnamese accents removed
    static std::string Fold(const std::string& in) {
        static const struct { const char* chars; char base; } kGroups[] = {
            {"àáảãạăằắẳẵặâầấẩẫậÀÁẢÃẠĂẰẮẲẴẶÂẦẤẨẪẬ", 'a'},
            {"èéẻẽẹêềếểễệÈÉẺẼẸÊỀẾỂỄỆ", 'e'},
            {"ìíỉĩịÌÍỈĨỊ", 'i'},
            {"òóỏõọôồốổỗộơờớởỡợÒÓỎÕỌÔỒỐỔỖỘƠỜỚỞỠỢ", 'o'},
            {"ùúủũụưừứửữựÙÚỦŨỤƯỪỨỬỮỰ", 'u'},
            {"ỳýỷỹỵỲÝỶỸỴ", 'y'},
            {"đĐ", 'd'},
        };
        std::string out;
        for (size_t i = 0; i < in.size();) {
            unsigned char c = in[i];
            size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (len == 1) {
                out += (char)tolower(c);
            } else {
                std::string ch = in.substr(i, len);
                char base = 0;
                for (auto& g : kGroups) {
                    if (std::string(g.chars).find(ch) != std::string::npos) { base = g.base; break; }
                }
                out += base ? base : ' ';
            }
            i += len;
        }
        return out;
    }

    static std::vector<std::string> Words(const std::string& folded) {
        std::vector<std::string> w;
        std::string cur;
        for (char c : folded) {
            if (isalnum((unsigned char)c)) cur += c;
            else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) w.push_back(cur);
        return w;
    }

    int FindSong(const std::string& query) {
        std::vector<std::string> qw = Words(Fold(query));
        if (qw.empty()) return -1;
        int best = -1;
        float best_score = 0;
        for (size_t i = 0; i < songs_.size(); i++) {
            std::string n = songs_[i];
            if (n.size() > 4) n.resize(n.size() - 4);
            std::string fn = Fold(n);
            int hit = 0;
            for (auto& w : qw) if (fn.find(w) != std::string::npos) hit++;
            float score = (float)hit / (float)qw.size();
            if (score > best_score) { best_score = score; best = (int)i; }
        }
        return best_score >= 0.5f ? best : -1;
    }

    // ---------------- calibration ----------------
    void LoadCalibration() {
        if (cal_loaded_) return;
        cal_loaded_ = true;
        Settings st("rubi", false);
        if (st.GetInt("cal_v2", 0) != 1) return;   // older calibrations used 2 corner taps: not accurate enough
        cal_.valid = true;
        cal_.swap = st.GetInt("cal_swap", 0) != 0;
        cal_.ax = st.GetInt("cal_ax", 10000) / 10000.0f;
        cal_.bx = st.GetInt("cal_bx", 0) / 1000.0f;
        cal_.ay = st.GetInt("cal_ay", 10000) / 10000.0f;
        cal_.by = st.GetInt("cal_by", 0) / 1000.0f;
    }

    static constexpr int kCalX[4] = {30, 290, 30, 290};
    static constexpr int kCalY[4] = {30, 30, 210, 210};
    uint16_t cal_raw_[4][2] = {};

    void ShowCalibTarget(int i) {
        if (!display_) return;
        char msg[64];
        snprintf(msg, sizeof(msg), "Chỉnh cảm ứng %d/4: chạm vào dấu + đỏ", i + 1);
        display_->MenuShowMessage(msg, kCalX[i], kCalY[i]);
    }

    void StartCalibration() {
        screen_ = Screen::kCalib1;
        ShowCalibTarget(0);
    }

    // Four taps near the corners, a straight line fitted per axis (least squares).
    void FinishCalibration() {
        float dxr = (float)cal_raw_[1][0] - (float)cal_raw_[0][0];
        float dyr = (float)cal_raw_[1][1] - (float)cal_raw_[0][1];
        Calib c;
        c.swap = fabsf(dyr) > fabsf(dxr);
        float a[4], b[4];
        for (int i = 0; i < 4; i++) {
            a[i] = c.swap ? cal_raw_[i][1] : cal_raw_[i][0];   // raw axis that maps to screen x
            b[i] = c.swap ? cal_raw_[i][0] : cal_raw_[i][1];   // raw axis that maps to screen y
        }
        auto fit = [](const float* r, const int* t, float& k, float& o, float& maxres) {
            float mr = 0, mt = 0;
            for (int i = 0; i < 4; i++) { mr += r[i]; mt += t[i]; }
            mr /= 4; mt /= 4;
            float cov = 0, var = 0;
            for (int i = 0; i < 4; i++) { cov += (r[i] - mr) * (t[i] - mt); var += (r[i] - mr) * (r[i] - mr); }
            if (var < 1.0f) { k = 0; o = 0; maxres = 999; return; }
            k = cov / var; o = mt - k * mr;
            maxres = 0;
            for (int i = 0; i < 4; i++) maxres = std::max(maxres, fabsf(t[i] - (k * r[i] + o)));
        };
        float rx, ry;
        fit(a, kCalX, c.ax, c.bx, rx);
        fit(b, kCalY, c.ay, c.by, ry);
        ESP_LOGI("RubiTouch", "calibration swap=%d ax=%.3f bx=%.1f ay=%.3f by=%.1f residual %.1f/%.1f px",
                 c.swap ? 1 : 0, c.ax, c.bx, c.ay, c.by, rx, ry);
        if (rx > 18 || ry > 18 || fabsf(c.ax) < 0.05f || fabsf(c.ay) < 0.05f) {
            StartCalibration();   // taps did not agree: start again
            return;
        }
        c.valid = true;
        cal_ = c;
        Settings st("rubi", true);
        st.SetInt("cal_v2", 1);
        st.SetInt("cal_swap", c.swap ? 1 : 0);
        st.SetInt("cal_ax", (int)lroundf(c.ax * 10000));
        st.SetInt("cal_bx", (int)lroundf(c.bx * 1000));
        st.SetInt("cal_ay", (int)lroundf(c.ay * 10000));
        st.SetInt("cal_by", (int)lroundf(c.by * 1000));
        ShowMain();
    }

    void Map(uint16_t rx, uint16_t ry, int& sx, int& sy) {
        float a = cal_.swap ? ry : rx;   // raw axis for screen x
        float b = cal_.swap ? rx : ry;   // raw axis for screen y
        sx = (int)(cal_.ax * a + cal_.bx);
        sy = (int)(cal_.ay * b + cal_.by);
    }

    // ---------------- screens ----------------
    void ShowMain() {
        screen_ = Screen::kMain;
        if (!display_) return;
        display_->MenuShowTiles({
            {&rubi_icon_nghe_nhac, ""},
            {&rubi_icon_song_ngu, ""},
            {&rubi_icon_truyen_tranh, ""},
            {&rubi_icon_toan_lop1, ""},
            {&rubi_icon_dong_ho, ""},
            {&rubi_icon_choi_cung, ""},
            {&rubi_icon_cai_dat, ""},
            {&rubi_icon_tieng_anh, ""},
        });
    }

    // ---------------- English lessons (SD: /sdcard/rubi/anh/<id>/ten.txt, bai.txt "file|English|Tiếng Việt", <file>.wav) ----------------
    static constexpr int kEngMaxLessons = 4;
    bool eng_mode_ = false;
    int eng_sel_ = 0;
    std::vector<std::string> eng_ids_, eng_titles_;
    std::vector<int> quiz_order_;
    int quiz_i_ = 0, quiz_score_ = 0;
    int quiz_opt_[3] = {0, 0, 0};
    int quiz_ok_ = 0;
    bool quiz_first_ = true;
    std::string quiz_msg_;

    static std::string FirstLine(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) return "";
        char line[128] = {0};
        if (fgets(line, sizeof(line), f) == nullptr) line[0] = 0;
        fclose(f);
        return Trim(line);
    }

    void ShowEnglish() {
        eng_mode_ = false;
        eng_ids_.clear();
        eng_titles_.clear();
        if (MountSd()) {
            DIR* d = opendir("/sdcard/rubi/anh");
            if (d != nullptr) {
                std::vector<std::string> ids;
                while (struct dirent* e = readdir(d)) {
                    if (e->d_name[0] == '.') continue;
                    ids.push_back(e->d_name);
                }
                closedir(d);
                std::sort(ids.begin(), ids.end());
                for (const auto& id : ids) {
                    if ((int)eng_ids_.size() >= kEngMaxLessons) break;
                    std::string dir = "/sdcard/rubi/anh/" + id;
                    FILE* f = fopen((dir + "/bai.txt").c_str(), "rb");
                    if (f == nullptr) continue;
                    fclose(f);
                    std::string t = FirstLine(dir + "/ten.txt");
                    eng_ids_.push_back(id);
                    eng_titles_.push_back(t.empty() ? id : t);
                }
            }
        }
        if (eng_ids_.empty()) {
            screen_ = Screen::kSoon;
            if (display_) display_->MenuShowMessage("Chưa có bài Tiếng Anh\n(chép thư mục rubi/anh vào thẻ nhớ)\n\nChạm để quay lại", -1, -1);
            return;
        }
        screen_ = Screen::kEngList;
        std::vector<std::string> rows = eng_titles_;
        rows.push_back("Quay lại");
        if (display_) display_->MenuShowList("TIẾNG ANH", rows, "");
    }

    void ShowEngMenu() {
        eng_mode_ = false;
        if (eng_sel_ < 0 || eng_sel_ >= (int)eng_ids_.size()) { ShowEnglish(); return; }
        screen_ = Screen::kEngMenu;
        if (display_) display_->MenuShowList(eng_titles_[eng_sel_].c_str(), {"Học câu mẫu", "Nghe và chọn", "Quay lại"}, "");
    }

    std::string EngDir() { return "/sdcard/rubi/anh/" + eng_ids_[eng_sel_]; }

    bool LoadEngWords() {
        words_.clear();
        FILE* f = fopen((EngDir() + "/bai.txt").c_str(), "rb");
        if (f == nullptr) return false;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string l = Trim(line);
            size_t a = l.find('|');
            size_t b = a == std::string::npos ? a : l.find('|', a + 1);
            if (b == std::string::npos) continue;
            words_.push_back({Trim(l.substr(0, a)), Trim(l.substr(a + 1, b - a - 1)), Trim(l.substr(b + 1))});
        }
        fclose(f);
        return !words_.empty();
    }

    void OpenEngLesson() {
        if (!LoadEngWords()) { ShowEnglish(); return; }
        eng_mode_ = true;
        word_ = 0;
        ShowCard();
    }

    void StartSpeakPaths(const std::vector<std::string>& paths) {
        if (paths.empty() || playing_) return;
        speak_paths_ = paths;
        speak_gap_ms_ = 300;
        stop_ = false;
        playing_ = true;
        xTaskCreatePinnedToCore(&RubiMedia::SpeakTask, "rubi_say", 8192, this, 4, nullptr, 1);
    }

    void StartQuiz() {
        if (!LoadEngWords() || words_.size() < 3) {
            screen_ = Screen::kSoon;
            if (display_) display_->MenuShowMessage("Bài này chưa đủ câu để chơi\n\nChạm để quay lại", -1, -1);
            return;
        }
        quiz_order_.clear();
        for (int i = 0; i < (int)words_.size(); i++) quiz_order_.push_back(i);
        for (int i = (int)quiz_order_.size() - 1; i > 0; i--) std::swap(quiz_order_[i], quiz_order_[esp_random() % (i + 1)]);
        if (quiz_order_.size() > 8) quiz_order_.resize(8);
        quiz_i_ = 0;
        quiz_score_ = 0;
        quiz_msg_ = "Nghe rồi chọn nghĩa đúng nhé";
        AskQuiz();
    }

    void AskQuiz() {
        StopPlayback();
        int correct = quiz_order_[quiz_i_];
        std::vector<int> pool;
        for (int i = 0; i < (int)words_.size(); i++)
            if (i != correct && words_[i].vi != words_[correct].vi) pool.push_back(i);
        for (int i = (int)pool.size() - 1; i > 0; i--) std::swap(pool[i], pool[esp_random() % (i + 1)]);
        quiz_opt_[0] = correct;
        quiz_opt_[1] = pool.size() > 0 ? pool[0] : correct;
        quiz_opt_[2] = pool.size() > 1 ? pool[1] : quiz_opt_[1];
        for (int i = 2; i > 0; i--) std::swap(quiz_opt_[i], quiz_opt_[esp_random() % (i + 1)]);
        quiz_ok_ = 0;
        for (int i = 0; i < 3; i++) if (quiz_opt_[i] == correct) quiz_ok_ = i;
        quiz_first_ = true;
        ShowQuizScreen();
        SpeakQuiz();
    }

    void ShowQuizScreen() {
        screen_ = Screen::kEngQuiz;
        if (!display_) return;
        char title[48];
        snprintf(title, sizeof(title), "NGHE VÀ CHỌN %d/%d", quiz_i_ + 1, (int)quiz_order_.size());
        std::vector<std::string> rows;
        for (int i = 0; i < 3; i++) rows.push_back(words_[quiz_opt_[i]].vi);
        rows.push_back("Nghe lại");
        rows.push_back("Quay lại");
        display_->MenuShowList(title, rows, quiz_msg_.c_str());
    }

    void SpeakQuiz() {
        StopPlayback();
        StartSpeakPaths({EngDir() + "/" + words_[quiz_order_[quiz_i_]].file + ".wav"});
    }

    void AnswerQuiz(int r) {
        StopPlayback();
        if (r == quiz_ok_) {
            if (quiz_first_) quiz_score_++;
            quiz_i_++;
            if (quiz_i_ >= (int)quiz_order_.size()) { ShowQuizDone(); return; }
            quiz_msg_ = "Đúng rồi! Giỏi quá!";
            AskQuiz();
        } else {
            quiz_first_ = false;
            quiz_msg_ = "Chưa đúng, con nghe lại nhé!";
            ShowQuizScreen();
            SpeakQuiz();
        }
    }

    void ShowQuizDone() {
        screen_ = Screen::kEngDone;
        int n = (int)quiz_order_.size();
        const char* khen = quiz_score_ * 10 >= n * 9 ? "Giỏi quá, tuyệt vời!" : (quiz_score_ * 10 >= n * 7 ? "Giỏi lắm!" : (quiz_score_ * 10 >= n * 5 ? "Khá lắm, luyện thêm nhé!" : "Cố lên, mình chơi lại nhé!"));
        char msg[160];
        snprintf(msg, sizeof(msg), "Xong rồi!\nĐúng ngay %d/%d câu\n\n%s\n\nChạm để quay lại", quiz_score_, n, khen);
        if (display_) display_->MenuShowMessage(msg, -1, -1);
    }

    static std::string MicHint() {
        char b[96];
        snprintf(b, sizeof(b), "%s r=%d đ=%d đo=%d/%d",
                 g_rubi_mic_state == 1 ? "NGOÀI" : (g_rubi_mic_state == 2 ? "TRONG" : "?"),
                 g_rubi_mic_rms, g_rubi_mic_peak, g_rubi_mic_sdl, g_rubi_mic_sdr);
        return b;
    }

    // One line for finding faults: free internal memory (now / lowest ever), the remote link, and the sound packets of the last answer
    static std::string DiagHint() {
        char b[112];
        snprintf(b, sizeof(b), "RAM %d/%dk Xa:%s Gói%d Bỏ%d",
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (int)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 RubiRemote::Get().StatusText(), g_rubi_pkts.load(), g_rubi_drop.load());
        return b;
    }

    void ShowSettings() {
        screen_ = Screen::kSettings;
        StartTicker();   // refreshes the mic line once a second
        if (display_) {
            static const std::vector<const lv_image_dsc_t*> ico = {&rubi_icon_am_luong, &rubi_icon_wifi_s, &rubi_icon_dieu_khien, &rubi_icon_cam_ung_s, &rubi_icon_quay_lai};
            display_->MenuShowList("CÀI ĐẶT", {"Âm lượng", "Wi-Fi", "Điều khiển (QR, PIN)", "Mic & cảm ứng", "Quay lại"}, "", nullptr,
            &ico);
        }
    }

    // ---------------- microphone: choose external / on-board, record 4 s and play it back ----------------
    void ShowMicMenu() {
        screen_ = Screen::kMicMenu;
        StartTicker();   // refreshes the mic line once a second
        if (!display_) return;
        static const std::vector<const lv_image_dsc_t*> ico = {nullptr, nullptr, &rubi_icon_cam_ung_s, &rubi_icon_quay_lai};
        const char* f = g_rubi_mic_force == 1 ? "Dùng mic: NGOÀI" : (g_rubi_mic_force == 2 ? "Dùng mic: TRONG" : "Dùng mic: TỰ ĐỘNG");
        display_->MenuShowList("MIC", {"Thử mic (nói 4 giây)", f, "Chỉnh cảm ứng", "Quay lại"}, "", nullptr, &ico);
    }

    void StartMicTest() {
        screen_ = Screen::kMicBusy;   // touches are ignored until the test is done
        if (display_) display_->MenuShowMessage("Nói to, rõ vào mic\ntrong 4 giây\n\n(ví dụ: một, hai, ba, bốn, năm)", -1, -1);
        RunTask([this]() {
            const int rate = 24000, len = rate * 4;
            auto& as = Application::GetInstance().GetAudioService();
            int16_t* b = (int16_t*)heap_caps_malloc(len * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int got = 0;
            float real_s = 0;
            if (b != nullptr) {
                as.EnableWakeWordDetection(true);   // keeps the microphone running while the menu is open
                vTaskDelay(pdMS_TO_TICKS(300));
                g_rubi_cap_pos = 0;
                g_rubi_cap_clip = 0;
                g_rubi_cap_len = len;
                int64_t t0 = esp_timer_get_time();
                g_rubi_cap_buf = b;
                for (int i = 0; i < 120 && g_rubi_cap_pos < len; i++) vTaskDelay(pdMS_TO_TICKS(50));
                real_s = (esp_timer_get_time() - t0) / 1000000.0f;   // wall time to collect 4 s of samples (should be ~4.0)
                g_rubi_cap_buf = nullptr;
                vTaskDelay(pdMS_TO_TICKS(50));
                as.EnableWakeWordDetection(false);
                got = g_rubi_cap_pos;
            }
            int pk = 0;
            double e = 0;
            for (int i = 0; i < got; i++) { int a = b[i] < 0 ? -b[i] : b[i]; if (a > pk) pk = a; e += (double)b[i] * b[i]; }
            int rms = got > 0 ? (int)sqrt(e / got) : 0;
            int clip = g_rubi_cap_clip;
            if (got > rate / 2) {
                {
                    std::lock_guard<std::recursive_mutex> lk(mutex_);
                    if (display_) display_->MenuShowMessage("Đang phát lại\nnhững gì Rubi nghe được...", -1, -1);
                }
                for (int o = 0; o < got; o += 1200) {
                    int n = std::min(1200, got - o);
                    std::vector<int16_t> v(b + o, b + o + n);
                    for (int t = 0; t < 10 && !as.PushPcmToPlayback(std::move(v)); t++) {}
                }
                for (int i = 0; i < 150 && !as.IsPlaybackIdle(); i++) vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (b != nullptr) heap_caps_free(b);
            char msg[240];
            if (got <= rate / 2) {
                snprintf(msg, sizeof(msg), "Mic không gửi tiếng\n(chỉ %d mẫu)\n\nChạm để quay lại", got);
            } else {
                snprintf(msg, sizeof(msg), "Mic %s: %.1f giây\n(đo thật: %.1f giây)\nĐỉnh %d  TB %d\nMéo (chạm trần): %d\n\nNghe có rõ không?\nChạm để quay lại",
                         g_rubi_mic_state == 1 ? "NGOÀI" : "TRONG", got / (float)rate, real_s, pk, rms, clip);
            }
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            screen_ = Screen::kMicDone;
            if (display_) display_->MenuShowMessage(msg, -1, -1);
        }, "rubi_mictest");
    }

    // ---------------- phone -> Rubi music upload ----------------
    static std::string OwnIp() {
        esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (!nif || esp_netif_get_ip_info(nif, &info) != ESP_OK || info.ip.addr == 0) return std::string();
        char b[20];
        snprintf(b, sizeof(b), "%d.%d.%d.%d", (int)(info.ip.addr & 0xFF), (int)((info.ip.addr >> 8) & 0xFF), (int)((info.ip.addr >> 16) & 0xFF), (int)((info.ip.addr >> 24) & 0xFF));
        return std::string(b);
    }

    void StartUpload() {
        screen_ = Screen::kUpload;   // any touch goes back (also after an error message)
        if (!display_) return;
        std::string ip = OwnIp();
        if (ip.empty()) { display_->MenuShowMessage("Rubi chưa có Wi-Fi.\nVào Cài đặt > Wi-Fi\n\nChạm để quay lại", -1, -1); return; }
        if (!MountSd()) { display_->MenuShowMessage("Không đọc được thẻ nhớ.\n\nChạm để quay lại", -1, -1); return; }
        char code[8];
        snprintf(code, sizeof(code), "%04u", (unsigned)(esp_random() % 10000));
        if (!RubiUpload::Start(code)) { display_->MenuShowMessage("Không bật được máy chủ.\n\nChạm để quay lại", -1, -1); return; }
        display_->MenuShowQr(("http://" + ip).c_str(), (std::string("Quét bằng\nđiện thoại\n(cùng Wi-Fi)\nMã: ") + code + "\n\nChạm để\nthoát").c_str());
    }

    // ---------------- store ("Kho") sync: Rubi downloads new songs / stories by itself ----------------
    std::atomic<bool> syncing_{false};
    std::atomic<bool> sync_cancel_{false};
    int64_t last_sync_ok_ms_ = 0;     // 0 = never
    int64_t last_sync_try_ms_ = 0;

    static std::string SyncText(const RubiSync::Result& r) {
        if (!r.catalog_ok) return "Chưa tải được kho:\n" + r.err;
        std::string t = r.interrupted ? "Đã dừng giữa chừng\n" : "Xong!\n";
        t += "Tải mới: " + std::to_string(r.downloaded) + "\nĐã xóa: " + std::to_string(r.deleted);
        if (r.failed) t += "\nLỗi: " + std::to_string(r.failed) + " tệp";
        if (r.low_space) t += "\nThẻ nhớ sắp đầy!";
        return t;
    }

    // Runs one sync. manual = started from the menu (the SD card is already mounted and the menu is open).
    void RunSync(bool manual) {
        bool expected = false;
        if (!syncing_.compare_exchange_strong(expected, true)) return;
        sync_cancel_ = false;
        bool mounted_here = (card_ == nullptr);
        RubiSync::Result res;
        if (MountSd()) {
            res = RubiSync::Once([this, manual]() {
                if (sync_cancel_) return false;
                if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return false;
                if (!manual && (IsOpen() || playing_)) return false;
                return true;
            });
        } else {
            res.err = "khong doc duoc the nho";
        }
        last_sync_try_ms_ = esp_timer_get_time() / 1000;
        if (res.catalog_ok && !res.interrupted) last_sync_ok_ms_ = last_sync_try_ms_;
        if (mounted_here && !IsOpen() && !playing_) UnmountSd();
        syncing_ = false;
        if (manual && screen_ == Screen::kSync && display_ && !sync_cancel_) {
            display_->MenuShowMessage((SyncText(res) + "\n\nChạm để quay lại").c_str(), -1, -1);
        }
        ESP_LOGI("RubiSync", "done: ok=%d new=%d del=%d fail=%d int=%d low=%d %s", (int)res.catalog_ok, res.downloaded, res.deleted, res.failed, (int)res.interrupted, (int)res.low_space, res.err.c_str());
    }

    void StartSync() {
        screen_ = Screen::kSync;
        if (!display_) return;
        if (OwnIp().empty()) { display_->MenuShowMessage("Rubi chưa có Wi-Fi.\nVào Cài đặt > Wi-Fi\n\nChạm để quay lại", -1, -1); return; }
        if (syncing_) { display_->MenuShowMessage("Đang đồng bộ...\n\nChạm để dừng", -1, -1); return; }
        display_->MenuShowMessage("Đang đồng bộ kho...\nRubi tải nhạc, truyện mới\n\nChạm để dừng", -1, -1);
        BaseType_t ok = xTaskCreatePinnedToCore([](void* a) {
            static_cast<RubiMedia*>(a)->RunSync(true);
            vTaskDelete(nullptr);
        }, "rubi_sync", 10240, this, 3, nullptr, 1);
        if (ok != pdPASS) display_->MenuShowMessage("Không bắt đầu được.\n\nChạm để quay lại", -1, -1);
    }

    // Quietly checks the store every ~6 hours (retry after 30 min on failure) while Rubi is idle and the menu is closed.
    static void AutoSyncTask(void* arg) {
        RubiMedia* self = static_cast<RubiMedia*>(arg);
        vTaskDelay(pdMS_TO_TICKS(90000));
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(30000));
            int64_t now = esp_timer_get_time() / 1000;
            bool due = self->last_sync_ok_ms_ == 0 ? (now - self->last_sync_try_ms_ > 1800000 || self->last_sync_try_ms_ == 0)
                                                    : (now - self->last_sync_ok_ms_ > 6 * 3600 * 1000LL);
            if (!due || self->IsOpen() || self->playing_ || self->syncing_) continue;
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) continue;
            if (OwnIp().empty()) continue;
            self->RunSync(false);
        }
    }

    // ---------------- web control page: story list out, "open this story" in ----------------
    // Every ~20 s while idle Rubi tells the DevKit which stories are on the card ("TC<n>" then "T<dir>\t<title>" lines),
    // so the web page can list them. The web page asks the DevKit to open one with "O<dir>".
    static void StoryReportTask(void* arg) {
        RubiMedia* self = static_cast<RubiMedia*>(arg);
        vTaskDelay(pdMS_TO_TICKS(5000));
        while (true) {
            std::string v = std::string("V") + esp_app_get_description()->version + "\n";
            RubiBattery::Send(v.c_str(), v.size());
            vTaskDelay(pdMS_TO_TICKS(15000));
            self->ReportStories();
        }
    }

    void ReportStories() {
        if (IsOpen() || playing_ || syncing_) return;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
        bool expected = false;
        if (!syncing_.compare_exchange_strong(expected, true)) return;   // shares the "SD busy" flag with the store sync
        sync_cancel_ = false;
        std::vector<Story> keep = stories_;
        std::vector<std::string> keep_songs = songs_;
        bool mounted_here = (card_ == nullptr);
        bool ok = !IsOpen() && MountSd();
        if (ok) { LoadStories(); LoadSongs(); }
        uint64_t sd_total = 0, sd_free = 0;
        bool sd_ok = false;
        if (ok) {
            sd_ok = esp_vfs_fat_info("/sdcard", &sd_total, &sd_free) == ESP_OK;
            if (sd_ok && (sd_tick_++ % 4) == 0) {   // folder scan only every 4th report (about once a minute)
                const char* sub[3] = {"music", "truyen", "hoc"};
                for (int i = 0; i < 3; i++) {
                    uint64_t b = 0; int f = 0;
                    std::string dir = std::string("/sdcard/rubi/") + sub[i];
                    DirSum(dir, 0, b, f);
                    sd_kb_[i] = b / 1024;
                    sd_n_[i] = (i == 1) ? CountDirs(dir) : f;
                }
            }
        }
        std::vector<Story> list = stories_;
        std::vector<std::string> songs = songs_;
        stories_ = keep;
        songs_ = keep_songs;
        if (mounted_here && !IsOpen() && !playing_) UnmountSd();
        syncing_ = false;
        if (!ok) return;
        std::vector<Story> use;
        for (auto& st : list) {
            if (st.dir.size() > 40 || st.dir.find('\t') != std::string::npos) continue;
            bool ascii = true;
            for (unsigned char c : st.dir) if (c < 33 || c > 126) ascii = false;
            if (ascii && use.size() < 16) use.push_back(st);
        }
        std::string head = "TC" + std::to_string(use.size()) + "\n";
        if (!RubiBattery::Send(head.c_str(), head.size())) return;
        for (auto& st : use) {
            std::string t = st.title;
            for (auto& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            if (t.size() > 48) {   // cut on a UTF-8 character boundary
                size_t n = 48;
                while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
                t.resize(n);
            }
            std::string line = "T" + st.dir + "\t" + t + "\n";
            RubiBattery::Send(line.c_str(), line.size());
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        // songs: "YC<n>" then "Y<index>\t<name>" (the index is the position in the sorted music folder)
        int ns = (int)std::min<size_t>(songs.size(), 30);
        std::string sh = "YC" + std::to_string(ns) + "\n";
        RubiBattery::Send(sh.c_str(), sh.size());
        for (int i = 0; i < ns; i++) {
            std::string t = songs[i];
            if (t.size() > 4) t.resize(t.size() - 4);   // drop ".wav"
            for (auto& c : t) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            if (t.size() > 48) {
                size_t n = 48;
                while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
                t.resize(n);
            }
            std::string line = "Y" + std::to_string(i) + "\t" + t + "\n";
            RubiBattery::Send(line.c_str(), line.size());
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        // SD usage: "S<total_kb>\t<free_kb>\t<music_kb>\t<music_n>\t<truyen_kb>\t<truyen_n>\t<hoc_kb>\t<hoc_n>"
        if (sd_ok) {
            char buf[160];
            int len = snprintf(buf, sizeof(buf), "S%llu\t%llu\t%llu\t%d\t%llu\t%d\t%llu\t%d\n",
                               (unsigned long long)(sd_total / 1024), (unsigned long long)(sd_free / 1024),
                               (unsigned long long)sd_kb_[0], sd_n_[0], (unsigned long long)sd_kb_[1], sd_n_[1],
                               (unsigned long long)sd_kb_[2], sd_n_[2]);
            if (len > 0) RubiBattery::Send(buf, len);
        }
    }

    // Opens the touch menu for a command that came from the web page (same checks as the menu button).
    bool RemotePrepare() {
        if (display_ == nullptr) return false;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return false;
        if (!IsOpen()) {
            sync_cancel_ = true;
            for (int i = 0; i < 150 && syncing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
            if (syncing_) return false;
            Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
            LoadCalibration();
        } else if (screen_ == Screen::kCalib1 || screen_ == Screen::kCalib2 || screen_ == Screen::kCalib3 || screen_ == Screen::kCalib4) {
            return false;
        }
        return true;
    }

    // Web page commands: vup/vdown volume, huy (stop everything and close), sleep, clock, t<min>, m<0-3> maths, h<0-3> learning topic, p<n> play song n.
    void RemoteAction(const std::string& c) {
        RunTask([this, c]() {
            if (c == "vup" || c == "vdown") {
                auto* codec = Board::GetInstance().GetAudioCodec();
                int v = codec->output_volume();
                codec->SetOutputVolume(c == "vup" ? std::min(100, v + 10) : std::max(10, v - 10));
                return;
            }
            if (c == "huy") {
                if (!IsOpen()) return;
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                Close();
                return;
            }
            if (c.size() == 2 && c[0] == 'n' && c[1] >= '0' && c[1] <= '7') {   // child's schedule reminder: picture card on the screen + Rubi says the sentence
                int k = c[1] - '0';
                if (RemotePrepare()) {
                    std::lock_guard<std::recursive_mutex> lk(mutex_);
                    sleep_mode_ = false;
                    StopPlayback();
                    RestoreBacklight();
                    screen_ = Screen::kRemind;
                    remind_at_ = NowMs();
                    display_->MenuShowCard(rubi_viec_big[k], "ĐẾN GIỜ", RemindName(k), RemindText(k), "", false, true);
                    StartTicker();
                }
                RemindSay(k);
                return;
            }
            if (!RemotePrepare()) return;
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            tick_gen_++;
            sleep_mode_ = false;
            StopPlayback();
            RestoreBacklight();
            if (c == "sleep") StartSleep();
            else if (c == "clock") ShowClock();
            else if (c == "en") ShowEnglish();
            else if (c.size() >= 2 && c[0] == 't') { int m = atoi(c.c_str() + 1); if (m == 2 || m == 5 || m == 10 || m == 20) StartTimer(m); }
            else if (c.size() == 2 && c[0] == 'm' && c[1] >= '0' && c[1] <= '3') StartMath((RubiMath::Mode)(c[1] - '0'));
            else if (c.size() == 2 && c[0] == 'h' && c[1] >= '0' && c[1] <= '3') { ShowLearnTopics(); OpenTopic(c[1] - '0'); }
            else if (c.size() >= 2 && c[0] == 'p') {
                int idx = atoi(c.c_str() + 1);
                MountSd();
                LoadSongs();
                if (idx >= 0 && idx < (int)songs_.size()) { page_ = idx / kSongsPerPage; StartPlayback(idx); }
            }
        }, "rubi_remote2");
    }

    static const char* RemindName(int k) {
        static const char* const n[8] = {"Đánh răng", "Ăn cơm", "Học bài", "Đi tắm", "Đi ngủ", "Dọn đồ chơi", "Uống nước", "Đọc sách"};
        return (k >= 0 && k < 8) ? n[k] : "";
    }
    static const char* RemindText(int k) {
        static const char* const t[8] = {
            "Bé ơi, đến giờ đánh răng rồi!", "Bé ơi, đến giờ ăn cơm rồi!", "Bé ơi, đến giờ học bài rồi!", "Bé ơi, đến giờ đi tắm rồi!",
            "Bé ơi, đến giờ đi ngủ rồi, chúc bé ngủ ngon!", "Bé ơi, đến giờ dọn đồ chơi rồi!", "Bé ơi, uống nước nào!", "Bé ơi, đến giờ đọc sách rồi!"};
        return (k >= 0 && k < 8) ? t[k] : "";
    }

    // Today's schedule as set on the web page (the DevKit pushes it as "Z<HHMM:kind,...>").
    void ShowSched() {
        screen_ = Screen::kSched;
        std::string ds = RubiBattery::Sched();
        std::string txt = "Thời gian biểu của bé\n\n";
        int n = 0, total = 0;
        size_t i = 0;
        while (i < ds.size()) {
            size_t j = ds.find(',', i);
            if (j == std::string::npos) j = ds.size();
            int hhmm = 0, k = -1;
            if (sscanf(ds.substr(i, j - i).c_str(), "%d:%d", &hhmm, &k) == 2 && k >= 0 && k < 8) {
                total++;
                if (n < 6) {
                    char b[64];
                    snprintf(b, sizeof(b), "%02d:%02d  %s\n", hhmm / 100, hhmm % 100, RemindName(k));
                    txt += b;
                    n++;
                }
            }
            i = j + 1;
        }
        if (total == 0) txt += "Chưa có việc nào\n(đặt ở trang web của Rubi)\n";
        else if (total > n) txt += "... và " + std::to_string(total - n) + " việc nữa\n";
        txt += "\nChạm để quay lại";
        if (display_) display_->MenuShowMessage(txt.c_str(), -1, -1);
    }

    // Spoken reminder from the child's schedule (the DevKit sends "Cn<0-7>"): the assistant reads one fixed sentence aloud.
    void RemindSay(int k) {
        if (k < 0 || k > 7) return;
        auto& app = Application::GetInstance();
        for (int i = 0; i < 20; i++) {   // bring the assistant back to idle first
            if (app.GetDeviceState() == kDeviceStateIdle) break;
            app.WakeWordInvoke("");
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (app.GetDeviceState() != kDeviceStateIdle) return;
        app.WakeWordInvoke(std::string("Hãy đọc to, đúng từng chữ, không thêm lời nào khác: ") + RemindText(k));
        bool spoke = false;
        for (int i = 0; i < 80; i++) {
            vTaskDelay(pdMS_TO_TICKS(250));
            auto st = app.GetDeviceState();
            if (st == kDeviceStateSpeaking) spoke = true;
            if (spoke && st == kDeviceStateListening) { app.WakeWordInvoke(""); break; }   // finished: stop listening
            if (spoke && st == kDeviceStateIdle) break;
            if (!spoke && i == 48) break;
        }
    }

    void RemoteOpenStory(const std::string& dir) {
        RunTask([this, dir]() {
            if (display_ == nullptr) return;
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
            if (!IsOpen()) {
                sync_cancel_ = true;
                for (int i = 0; i < 150 && syncing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
                if (syncing_) return;
                Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
                LoadCalibration();
            } else if (screen_ == Screen::kCalib1 || screen_ == Screen::kCalib2 || screen_ == Screen::kCalib3 || screen_ == Screen::kCalib4) {
                return;
            }
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            StopPlayback();
            scene_gen_++;
            if (!MountSd()) { screen_ = Screen::kStoryEmpty; display_->MenuShowMessage("Không đọc được thẻ nhớ\n\nChạm để quay lại", -1, -1); return; }
            LoadStories();
            for (int i = 0; i < (int)stories_.size(); i++) {
                if (stories_[i].dir == dir) { OpenStory(i); return; }
            }
            screen_ = Screen::kStoryEmpty;
            display_->MenuShowMessage("Không thấy truyện này\n\nChạm để quay lại", -1, -1);
        }, "rubi_remote");
    }

    // ---------------- maths for a first grader ----------------
    static constexpr int kMathRound = 10;
    RubiMath::Mode math_mode_ = RubiMath::kWithin10;
    RubiMath::Q math_q_;
    std::string math_buf_;
    int math_idx_ = 0, math_score_ = 0, math_tries_ = 0;

    void ShowMathMenu() {
        screen_ = Screen::kMathMenu;
        if (display_) display_->MenuShowList("HỌC TOÁN", {"Cộng trừ trong 10", "Cộng trừ trong 20", "Cộng trừ trong 100", "Toán lời văn", "Quay lại"}, "Mỗi lượt 10 câu");
    }

    void StartMath(RubiMath::Mode m) {
        math_mode_ = m; math_idx_ = 0; math_score_ = 0;
        NextMath();
    }

    // Asks the cloud assistant (xiaozhi.me) to read the question aloud. Needs Wi-Fi; silent if the cloud is unreachable.
    std::atomic<int> say_gen_{0};

    // The story texts end "... còn lại mấy ..." on one screen; the clips only have "bao nhiêu".
    static std::string MathStorySpeech(std::string t) {
        for (size_t p = 0; (p = t.find("mấy", p)) != std::string::npos; p += 9) t.replace(p, strlen("mấy"), "bao nhiêu");
        return t;
    }

    // Reads a maths question from the pre-recorded clips in /sdcard/rubi/hoc/toan (made with tools/tao_giong_toan.py):
    // numbers are digits in the text, everything else is matched against the clip table, longest phrase first.
    // Returns false (nothing played) if a word or a file is missing, so the caller can fall back to the cloud voice.
    bool TrySayMathClips(const std::string& text) {
        struct Clip { const char* text; const char* file; };
        static const Clip kClips[] = {
        {"cộng", "tu_cong"},
        {"trừ", "tu_tru"},
        {"bằng", "tu_bang"},
        {"mấy", "tu_may"},
        {"bằng mấy", "tu_bangmay"},
        {"Lan", "ten_lan"},
        {"Nam", "ten_nam"},
        {"Mai", "ten_mai"},
        {"Hùng", "ten_hung"},
        {"Na", "ten_na"},
        {"Tí", "ten_ti"},
        {"Hoa", "ten_hoa"},
        {"Minh", "ten_minh"},
        {"An", "ten_an"},
        {"Bình", "ten_binh"},
        {"Mẹ", "nguoi_me"},
        {"Bố", "nguoi_bo"},
        {"Bà", "nguoi_ba"},
        {"Ông", "nguoi_ong"},
        {"Chị", "nguoi_chi"},
        {"quả cam", "vat_quacam"},
        {"viên bi", "vat_vienbi"},
        {"cái kẹo", "vat_caikeo"},
        {"bông hoa", "vat_bonghoa"},
        {"quyển truyện", "vat_quyentruyen"},
        {"cái bánh", "vat_caibanh"},
        {"quả táo", "vat_quatao"},
        {"chiếc bút", "vat_chiecbut"},
        {"con tem", "vat_contem"},
        {"có", "cau_co"},
        {"cho thêm", "cau_chothem"},
        {"cho bạn", "cau_chocban"},
        {"Hỏi", "cau_hoi"},
        {"có tất cả bao nhiêu", "cau_cotatcabaonhieu"},
        {"còn lại bao nhiêu", "cau_conlaibaonhieu"},
        {"hai bạn có tất cả bao nhiêu", "cau_haibancotatcabaonhieu"},
        {"có nhiều hơn", "cau_conhieuhon"},
        {"có ít hơn", "cau_coithon"},
        {"có bao nhiêu", "cau_cobaonhieu"},
        {"Trong rổ có", "cau_trongroco"},
        {"ăn", "cau_an"},
        {"Hỏi trong rổ còn lại bao nhiêu", "cau_trongroconlaibaonhieu"},
        {"nhiều hơn", "cau_nhieuhon"},
        {"bao nhiêu", "cau_baonhieu"}
        };
        std::string t = text;
        for (char& c : t) if (c == '.' || c == ',' || c == '?' || c == '\n' || c == '!') c = ' ';
        std::vector<std::string> w;
        for (size_t i = 0; i < t.size();) {
            while (i < t.size() && t[i] == ' ') i++;
            size_t j = i;
            while (j < t.size() && t[j] != ' ') j++;
            if (j > i) w.push_back(t.substr(i, j - i));
            i = j;
        }
        if (w.empty() || !MountSd()) return false;
        std::vector<std::string> paths;
        const std::string dir = "/sdcard/rubi/hoc/toan/";
        for (size_t i = 0; i < w.size();) {
            const char* file = nullptr;
            size_t used = 0;
            bool num = !w[i].empty() && w[i].size() <= 3 && isdigit((unsigned char)w[i][0]);
            if (num) { paths.push_back(dir + "so_" + std::to_string(atoi(w[i].c_str())) + ".wav"); i++; continue; }
            for (const auto& c : kClips) {
                std::string p = c.text;
                size_t n = 1;
                for (char ch : p) if (ch == ' ') n++;
                if (n <= used || i + n > w.size()) continue;
                std::string cand = w[i];
                for (size_t k = 1; k < n; k++) cand += " " + w[i + k];
                if (cand == p) { file = c.file; used = n; }
            }
            if (file == nullptr) return false;
            paths.push_back(dir + file + ".wav");
            i += used;
        }
        return StartClipPaths(paths);
    }

    // Plays the given WAV files one after another (all must exist, otherwise nothing is played and false is returned).
    bool StartClipPaths(const std::vector<std::string>& paths) {
        for (const auto& p : paths) {
            FILE* f = fopen(p.c_str(), "rb");
            if (f == nullptr) return false;
            fclose(f);
        }
        stop_ = true;
        for (int i = 0; i < 100 && playing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (playing_) return false;
        speak_paths_ = paths;
        speak_gap_ms_ = 70;
        stop_ = false;
        playing_ = true;
        xTaskCreatePinnedToCore(&RubiMedia::SpeakTask, "rubi_say", 8192, this, 4, nullptr, 1);
        return true;
    }

    // Spoken feedback of the maths game: clip names without folder and ".wav" (made by tao_giong_toan*.py, names ph_*, so_*).
    bool SayMathNames(const std::vector<std::string>& names) {
        if (!MountSd()) return false;
        std::vector<std::string> paths;
        for (const auto& n : names) paths.push_back("/sdcard/rubi/hoc/toan/" + n + ".wav");
        return StartClipPaths(paths);
    }

    void SayMath(const std::string& text) {
        int gen = ++say_gen_;
        if (TrySayMathClips(math_q_.story.empty() ? text : MathStorySpeech(text))) return;
        std::string prompt = "Hãy đọc to, đúng từng chữ, không giải bài, không thêm lời nào khác: " + text;
        RunTask([this, prompt, gen]() {
            auto& app = Application::GetInstance();
            auto fail = [this, gen](const std::string& why) {
                if (gen != say_gen_) return;
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                if (screen_ == Screen::kMathKeys) RenderMath("Chưa có giọng: " + why);
            };
            for (int i = 0; i < 20; i++) {   // bring the assistant back to idle first (stops a previous reading)
                if (gen != say_gen_) return;
                if (app.GetDeviceState() == kDeviceStateIdle) break;
                app.WakeWordInvoke("");
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            if (gen != say_gen_) return;
            if (app.GetDeviceState() != kDeviceStateIdle) { fail("không về trạng thái rảnh, state=" + std::to_string((int)app.GetDeviceState())); return; }
            app.WakeWordInvoke(prompt);
            bool spoke = false;
            std::string trace = "I";   // states seen, e.g. I C L S: Idle, Connecting, Listening, Speaking
            auto last = kDeviceStateIdle;
            for (int i = 0; i < 120 && gen == say_gen_; i++) {
                vTaskDelay(pdMS_TO_TICKS(250));
                auto st = app.GetDeviceState();
                if (st != last && trace.size() < 14) {
                    last = st;
                    trace += ' ';
                    trace += st == kDeviceStateIdle ? 'I' : st == kDeviceStateConnecting ? 'C' : st == kDeviceStateListening ? 'L' : st == kDeviceStateSpeaking ? 'S' : '?';
                }
                if (st == kDeviceStateSpeaking) spoke = true;
                if (spoke && st == kDeviceStateListening) { app.WakeWordInvoke(""); break; }   // reading finished: stop listening
                if (spoke && st == kDeviceStateIdle) break;
                if (!spoke && i == 48) break;   // 12 s and still no voice
            }
            if (!spoke && gen == say_gen_) {   // tell what happened, so it can be fixed
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                if (screen_ == Screen::kMathKeys) RenderMath("Chưa có giọng: " + trace);
            }
        }, "math_say");
    }

    void StopSayMath() {
        ++say_gen_;
        if (playing_ && screen_ != Screen::kPlaying) stop_ = true;   // clip reading
        auto st = Application::GetInstance().GetDeviceState();
        if (st == kDeviceStateSpeaking || st == kDeviceStateListening) Application::GetInstance().WakeWordInvoke("");
    }

    void NextMath() {
        if (math_idx_ >= kMathRound) {
            screen_ = Screen::kMathDone;
            const char* praise = math_score_ >= 9 ? "Giỏi quá, tuyệt vời!" : (math_score_ >= 7 ? "Giỏi lắm!" : (math_score_ >= 5 ? "Khá lắm, luyện thêm nhé!" : "Cố lên, mình chơi lại nhé!"));
            std::string t = "Xong rồi!\nĐúng ngay " + std::to_string(math_score_) + "/" + std::to_string(kMathRound) + " câu\n" + praise + "\n\nChạm để chơi tiếp";
            if (display_) display_->MenuShowMessage(t.c_str(), -1, -1, true);
            StopSayMath();
            SayMathNames({"ph_xong", "ph_condung", "so_" + std::to_string(math_score_), "ph_cau",
                          math_score_ >= 9 ? "ph_khen9" : (math_score_ >= 7 ? "ph_khen7" : (math_score_ >= 5 ? "ph_khen5" : "ph_khen0"))});
            return;
        }
        math_q_ = RubiMath::Make(math_mode_, math_idx_);
        math_buf_.clear();
        math_tries_ = 0;
        if (!math_q_.story.empty()) {
            screen_ = Screen::kMathStory;
            if (display_) display_->MenuShowMessage((std::string("Câu ") + std::to_string(math_idx_ + 1) + "/" + std::to_string(kMathRound) + "\n" + math_q_.story + "\n\nChạm để trả lời").c_str(), -1, -1, true);
        } else {
            RenderMath("");
        }
        SayMath(math_q_.say);
    }

    void RenderMath(const std::string& note) {
        screen_ = Screen::kMathKeys;
        keys_.clear();
        // Hit areas match the picture buttons drawn by RubiDisplay::MenuShowMath.
        keys_.push_back({4, 2, 40, 38, "", 6, "", 0});
        for (int d = 1; d <= 9; d++)
            keys_.push_back({97 + ((d - 1) % 3) * 64, 80 + ((d - 1) / 3) * 34, 62, 32, std::string(1, (char)('0' + d)), 0, std::string(1, (char)('0' + d)), 0});
        keys_.push_back({97, 182, 62, 34, "Xóa", 2, "", 0});
        keys_.push_back({161, 182, 60, 34, "0", 0, "0", 0});
        keys_.push_back({221, 182, 68, 34, "OK", 5, "", 0});
        keys_.push_back({10, 80, 82, 36, "Nghe", 8, "", 0});
        keys_.push_back({266, 40, 44, 38, "Nghe", 8, "", 0});
        bool story = !math_q_.story.empty();
        if (story) keys_.push_back({10, 118, 82, 36, "Đề", 7, "", 0});
        std::string title = note.empty() ? ("Toán lớp 1   " + std::to_string(math_idx_ + 1) + "/" + std::to_string(kMathRound)) : note;
        if (display_) display_->MenuShowMath(title, math_q_.ask, story, math_buf_, story);
    }

    void HandleMathTouch(int sx, int sy) {
        for (const auto& k : keys_) {
            if (sx < k.x - 1 || sx >= k.x + k.w + 1 || sy < k.y - 1 || sy >= k.y + k.h + 1) continue;
            switch (k.kind) {
            case 0: if (math_buf_.size() < 3) math_buf_ += k.val; RenderMath(""); break;
            case 2: if (!math_buf_.empty()) math_buf_.pop_back(); RenderMath(""); break;
            case 6: StopSayMath(); ShowMathMenu(); break;
            case 8: SayMath(math_q_.say); break;
            case 7:
                screen_ = Screen::kMathStory;
                if (display_) display_->MenuShowMessage((math_q_.story + "\n\nChạm để trả lời").c_str(), -1, -1, true);
                break;
            case 5: if (!math_buf_.empty()) AnswerMath(); break;
            }
            return;
        }
    }

    void AnswerMath() {
        int v = atoi(math_buf_.c_str());
        StopSayMath();
        screen_ = Screen::kMathBusy;
        bool right = v == math_q_.answer;
        bool reveal = false;
        if (right) { if (math_tries_ == 0) math_score_++; }
        else { math_tries_++; reveal = math_tries_ >= 2; }
        std::string msg = right ? "Đúng rồi!\nGiỏi quá!" : (reveal ? "Đáp án là " + std::to_string(math_q_.answer) + "\nMình làm câu tiếp nhé" : "Chưa đúng,\nthử lại nhé!");
        if (display_) display_->MenuShowMessage(msg.c_str(), -1, -1, true);
        if (right) SayMathNames({"ph_dung" + std::to_string(1 + (int)(esp_random() % 3))});
        else if (reveal) SayMathNames({"ph_dapan", "so_" + std::to_string(math_q_.answer), "ph_tiep"});
        else SayMathNames({"ph_sai" + std::to_string(1 + (int)(esp_random() % 2))});
        bool next = right || reveal;
        RunTask([this, next, ms = (right ? 900 : (reveal ? 2200 : 1100))]() {
            vTaskDelay(pdMS_TO_TICKS(ms));
            for (int i = 0; i < 60 && playing_; i++) vTaskDelay(pdMS_TO_TICKS(100));   // let the spoken feedback finish
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            if (screen_ != Screen::kMathBusy) return;
            if (next) { math_idx_++; NextMath(); }
            else { math_buf_.clear(); RenderMath(""); }
        }, "math_next");
    }

    // ---------------- play with Rubi ----------------
    // ---------------- clock, countdown timer, bedtime ----------------
    // The time comes from the server at start-up (already in local time); without it the year is 1970.
    std::atomic<int> tick_gen_{0};
    int64_t timer_end_ms_ = 0, sleep_end_ms_ = 0, remind_at_ = 0;
    int alarm_n_ = 0;
    bool sleep_mode_ = false, dimmed_ = false, back_play_ = false;
    uint8_t saved_bright_ = 100;

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    static std::string ClockText() {
        time_t t = time(nullptr);
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_year + 1900 < 2024) return "Chưa có giờ\n(cần Wi-Fi, chờ\nvài phút sau khi bật)\n\nChạm để quay lại";
        static const char* const wd[] = {"Chủ nhật", "Thứ hai", "Thứ ba", "Thứ tư", "Thứ năm", "Thứ sáu", "Thứ bảy"};
        char b[96];
        snprintf(b, sizeof(b), "%02d:%02d\n%s\n%02d/%02d/%04d\n\nChạm để quay lại", tm.tm_hour, tm.tm_min, wd[tm.tm_wday % 7], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        return b;
    }

    void StartTicker() {
        int gen = ++tick_gen_;
        RunTask([this, gen]() {
            while (tick_gen_ == gen) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                if (tick_gen_ != gen) break;
                TickOnce(gen);
            }
        }, "rubi_tick");
    }

    void TickOnce(int gen) {
        if (display_ == nullptr) return;
        switch (screen_) {
        // (mic / RAM diagnostic lines hidden at the user's request; MicHint()/DiagHint() kept for debugging)
        case Screen::kRemind:   // nobody touched it for 90 s: close, so the wake word works again
            if (NowMs() - remind_at_ > 90000) { tick_gen_++; Close(); }
            break;
        case Screen::kClock:
            display_->MenuSetMessage(ClockText().c_str());
            break;
        case Screen::kTimerRun: {
            int remain = (int)((timer_end_ms_ - NowMs() + 999) / 1000);
            if (remain <= 0) {
                screen_ = Screen::kTimerDone;
                alarm_n_ = 0;
                display_->MenuShowMessage("Hết giờ rồi!\n\nChạm để tắt", -1, -1);
                break;
            }
            char b[64];
            snprintf(b, sizeof(b), "Hẹn giờ\n%02d:%02d\n\nChạm để hủy", remain / 60, remain % 60);
            display_->MenuSetMessage(b);
            break;
        }
        case Screen::kTimerDone:
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_VIBRATION);
            if (++alarm_n_ >= 15) { tick_gen_++; ShowPlay(); }
            break;
        case Screen::kSleep:
            if (NowMs() >= sleep_end_ms_ || !playing_) EndSleep();
            break;
        default:
            if (tick_gen_ == gen) tick_gen_++;
            break;
        }
    }

    void ShowClock() {
        back_play_ = false;
        screen_ = Screen::kClock;
        if (display_) display_->MenuShowMessage(ClockText().c_str(), -1, -1);
        StartTicker();
    }

    void ShowTimerMenu() {
        screen_ = Screen::kTimerMenu;
        if (display_) {
            static const std::vector<const lv_image_dsc_t*> ico = {&rubi_icon_hen_gio, &rubi_icon_hen_gio, &rubi_icon_hen_gio, &rubi_icon_hen_gio};
            display_->MenuShowList("HẸN GIỜ", {"2 phút", "5 phút", "10 phút", "20 phút", "Quay lại"}, "Chạm để bắt đầu đếm ngược", nullptr, &ico);
        }
    }

    void StartTimer(int minutes) {
        timer_end_ms_ = NowMs() + (int64_t)minutes * 60000;
        screen_ = Screen::kTimerRun;
        if (display_) display_->MenuShowMessage("Hẹn giờ\nĐang bắt đầu...\n\nChạm để hủy", -1, -1);
        StartTicker();
    }

    // Bedtime: quiet the screen, sing lullabies (files in music/ whose name starts with "ru"; all songs if there are none) for 30 minutes.
    void StartSleep() {
        LoadSongs();
        std::vector<std::string> ru;
        for (auto& n : songs_) {
            if (n.size() >= 2 && tolower((unsigned char)n[0]) == 'r' && tolower((unsigned char)n[1]) == 'u') ru.push_back(n);
        }
        if (!ru.empty()) songs_ = ru;
        if (songs_.empty()) {
            back_play_ = true;
            screen_ = Screen::kClock;   // a touch goes back to the play menu
            if (display_) display_->MenuShowMessage("Chưa có nhạc ru ngủ\n(đặt tên tệp bắt đầu bằng ru)\n\nChạm để quay lại", -1, -1);
            return;
        }
        sleep_mode_ = true;
        sleep_end_ms_ = NowMs() + 30 * 60000LL;
        screen_ = Screen::kSleep;
        if (display_) display_->MenuShowMessage("Chúc bé ngủ ngon\nRubi hát ru 30 phút\n\nChạm để dừng", -1, -1);
        if (Backlight* bl = Board::GetInstance().GetBacklight()) {
            if (!dimmed_) { saved_bright_ = bl->brightness(); dimmed_ = true; }
            bl->SetBrightness(6);
        }
        stop_ = false;
        playing_ = true;
        now_index_ = 0;
        xTaskCreatePinnedToCore(&RubiMedia::PlayTask, "rubi_play", 8192, this, 4, nullptr, 1);
        StartTicker();
    }

    void RestoreBacklight() {
        if (!dimmed_) return;
        dimmed_ = false;
        if (Backlight* bl = Board::GetInstance().GetBacklight()) bl->SetBrightness(saved_bright_ < 10 ? 100 : saved_bright_);
    }

    void EndSleep() {
        sleep_mode_ = false;
        tick_gen_++;
        StopPlayback();
        RestoreBacklight();
        ShowPlay();
    }

    void ShowPlay() {
        screen_ = Screen::kPlay;
        if (display_) {
            static const std::vector<const lv_image_dsc_t*> ico = {&rubi_icon_nhay_mua, &rubi_icon_hat_ru, &rubi_icon_hen_gio, &rubi_icon_lich, &rubi_icon_quay_lai};
            display_->MenuShowList("CHƠI CÙNG RUBI", {"Nhảy múa", "Đi ngủ (hát ru)", "Hẹn giờ", "Thời gian biểu", "Quay lại"}, "Nhảy múa: Rubi xoay tại chỗ", nullptr,
            &ico);
        }
    }

    // The DevKit does the moving (spins on the spot only, so Rubi cannot drive off a table); any touch stops it.
    void StartDance() {
        screen_ = Screen::kDance;
        if (!RubiBattery::Send("D\n", 2)) {
            if (display_) display_->MenuShowMessage("Chưa nối được với robot.\nKiểm tra dây TX0/RX0\n\nChạm để quay lại", -1, -1);
            return;
        }
        if (display_) display_->MenuShowMessage("Rubi đang nhảy!\n\nChạm để dừng", -1, -1);
    }

    // ---------------- control: QR code and family PIN ----------------
    void ShowControl() {
        screen_ = Screen::kCtl;
        int has = RubiBattery::PinSet();
        std::string hint = has == 1 ? "Robot đang có mã PIN" : (has == 0 ? "CHƯA có PIN: ai cũng điều khiển được!" : "Chưa biết PIN (robot chưa trả lời)");
        if (display_) { static const std::vector<const lv_image_dsc_t*> ico = {&rubi_icon_dieu_khien}; display_->MenuShowList("ĐIỀU KHIỂN", {"Mã QR / địa chỉ", has == 1 ? "Đổi mã PIN" : "Đặt mã PIN", "Tải nhạc, truyện từ điện thoại", "Đồng bộ kho ngay", "Quay lại"}, hint.c_str(), nullptr, &ico); }
    }

    std::string pin_old_, pin_new_, pin_buf_;
    int pin_step_ = 0;   // 0 old PIN, 1 new PIN, 2 repeat new PIN

    void StartPinChange() {
        pin_old_.clear(); pin_new_.clear(); pin_buf_.clear();
        pin_step_ = RubiBattery::PinSet() == 1 ? 0 : 1;
        RenderPin("");
    }

    void RenderPin(const std::string& note) {
        screen_ = Screen::kPinKeys;
        keys_.clear();
        const uint32_t kNum = 0xE6F0FF, kSpecial = 0xBBD0F0, kOk = 0xA5D6A7, kCancel = 0xFFC9C9;
        const int KH = 38, KW = 56, G = 4, X0 = 76;
        keys_.push_back({4, 2, 64, 22, "< Hủy", 6, "", kCancel});
        static const char* rows[3] = {"123", "456", "789"};
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                keys_.push_back({X0 + c * (KW + G), 56 + r * 41, KW, KH, std::string(1, rows[r][c]), 0, std::string(1, rows[r][c]), kNum});
        keys_.push_back({X0, 179, KW, KH, "Xóa", 2, "", kSpecial});
        keys_.push_back({X0 + KW + G, 179, KW, KH, "0", 0, "0", kNum});
        keys_.push_back({X0 + 2 * (KW + G), 179, KW, KH, "OK", 5, "", kOk});
        static const char* titles[3] = {"PIN hiện tại", "PIN mới (4-8 số)", "Nhập lại PIN mới"};
        std::string shown(pin_buf_.size(), '*');
        shown += "_";
        if (display_) display_->MenuShowKeyboard(note.empty() ? titles[pin_step_] : note, shown, keys_, false);
    }

    void HandlePinTouch(int sx, int sy) {
        for (const auto& k : keys_) {
            if (sx < k.x - 1 || sx >= k.x + k.w + 1 || sy < k.y - 1 || sy >= k.y + k.h + 1) continue;
            switch (k.kind) {
            case 0: if (pin_buf_.size() < 8) pin_buf_ += k.val; RenderPin(""); break;
            case 2: if (!pin_buf_.empty()) pin_buf_.pop_back(); RenderPin(""); break;
            case 6: ShowControl(); break;
            case 5:
                if (pin_buf_.size() < 4) { RenderPin("PIN cần 4-8 chữ số"); break; }
                if (pin_step_ == 0) { pin_old_ = pin_buf_; pin_buf_.clear(); pin_step_ = 1; RenderPin(""); }
                else if (pin_step_ == 1) { pin_new_ = pin_buf_; pin_buf_.clear(); pin_step_ = 2; RenderPin(""); }
                else if (pin_buf_ != pin_new_) { pin_buf_.clear(); pin_step_ = 1; pin_new_.clear(); RenderPin("Hai lần không giống nhau"); }
                else SendPin();
                break;
            }
            return;
        }
    }

    void SendPin() {
        screen_ = Screen::kPinBusy;
        if (display_) display_->MenuShowMessage("Đang đặt PIN...", -1, -1);
        RunTask([this, o = pin_old_, n = pin_new_]() {
            RubiBattery::PinReplyClear();
            std::string line = "P" + o + "|" + n + "\n";
            RubiBattery::Send(line.c_str(), line.size());
            int r = -1;
            for (int i = 0; i < 30 && r < 0; i++) { vTaskDelay(pdMS_TO_TICKS(100)); r = RubiBattery::PinReply(); }
            if (display_) display_->MenuShowMessage(r == 1 ? "Đã đặt PIN!\nChỉ người biết PIN\nđiều khiển được robot." : (r == 0 ? "PIN hiện tại sai\n(hoặc đang bị khóa 30 giây)" : "Robot không trả lời.\nKiểm tra dây TX0/RX0"), -1, -1);
            vTaskDelay(pdMS_TO_TICKS(2500));
            pin_old_.clear(); pin_new_.clear(); pin_buf_.clear();
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            ShowControl();
        }, "pin_set");
    }

    // The robot (motor DevKit) tells us its address on the Wi-Fi we gave it; show it so the phone can be pointed there.
    void ShowRobotAddress() {
        screen_ = Screen::kIp;
        std::string ip = RubiBattery::Ip();
        if (!display_) return;
        if (ip.empty())
            display_->MenuShowMessage("Robot chưa báo địa chỉ.\nKiểm tra dây TX0/RX0\nvà Wi-Fi của robot.\n\nChạm để quay lại", -1, -1);
        else
            display_->MenuShowQr(("http://" + ip).c_str(), ("Quét bằng\nđiện thoại\n(cùng Wi-Fi)\n" + ip + "\n\nChạm để\nquay lại").c_str());
    }

    // ---------------- picture books ----------------
    // SD layout:  /sdcard/rubi/truyen/<story>/ten.txt (title), NN.bin (320x240 RGB565 page), NN.wav (spoken text).
    // Pages are the *.bin files in name order. The page is read aloud, then the next page opens by itself.
    struct Story { std::string dir, title; };
    static constexpr const char* kStoryDir = "/sdcard/rubi/truyen";
    static constexpr int kStoriesPerPage = 3;
    static constexpr size_t kStoryBytes = 320 * 240 * 2;
    std::vector<Story> stories_;
    std::vector<int> story_row_action_;   // per row: story index, -1 next page, -2 back
    int spage_ = 0;
    int story_idx_ = 0;
    std::vector<std::string> scenes_;     // full path of each page without extension
    int scene_ = 0;
    std::atomic<int> scene_gen_{0};
    uint8_t* story_buf_[2] = {nullptr, nullptr};
    lv_image_dsc_t story_dsc_[2] = {};
    int story_cur_ = 0;

    static bool HasExt(const char* name, const char* ext) {
        size_t n = strlen(name), m = strlen(ext);
        if (n <= m) return false;
        for (size_t i = 0; i < m; i++) if (tolower((unsigned char)name[n - m + i]) != ext[i]) return false;
        return true;
    }

    void LoadStories() {
        stories_.clear();
        if (!MountSd()) return;
        DIR* d = opendir(kStoryDir);
        if (d == nullptr) return;
        while (dirent* e = readdir(d)) {
            if (e->d_type != DT_DIR || e->d_name[0] == '.') continue;
            Story st;
            st.dir = e->d_name;
            st.title = e->d_name;
            std::string tf = std::string(kStoryDir) + "/" + e->d_name + "/ten.txt";
            if (FILE* f = fopen(tf.c_str(), "rb")) {
                char line[128] = {};
                if (fgets(line, sizeof(line), f)) {
                    std::string t = Trim(line);
                    if (!t.empty()) st.title = t;
                }
                fclose(f);
            }
            stories_.push_back(st);
        }
        closedir(d);
        std::sort(stories_.begin(), stories_.end(), [](const Story& a, const Story& b) { return a.dir < b.dir; });
    }

    void ShowStoryList() {
        screen_ = Screen::kStoryList;
        std::vector<std::string> rows;
        story_row_action_.clear();
        int total = (int)stories_.size();
        int pages = total == 0 ? 1 : (total + kStoriesPerPage - 1) / kStoriesPerPage;
        if (spage_ >= pages) spage_ = 0;
        if (total == 0) {
            rows.push_back("(Chưa có truyện nào)");
            story_row_action_.push_back(-2);
        }
        for (int i = spage_ * kStoriesPerPage; i < total && i < (spage_ + 1) * kStoriesPerPage; i++) {
            rows.push_back(stories_[i].title);
            story_row_action_.push_back(i);
        }
        if (pages > 1) {
            rows.push_back("Trang sau >");
            story_row_action_.push_back(-1);
        }
        rows.push_back("Quay lại");
        story_row_action_.push_back(-2);
        if (display_) display_->MenuShowList("TRUYỆN TRANH - chạm để xem", rows, total == 0 ? "Chép thư mục rubi/truyen vào thẻ nhớ" : "");
    }

    void OpenStory(int idx) {
        if (idx < 0 || idx >= (int)stories_.size()) return;
        story_idx_ = idx;
        scenes_.clear();
        std::string dir = std::string(kStoryDir) + "/" + stories_[idx].dir;
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                if (e->d_type != DT_DIR && HasExt(e->d_name, ".bin")) {
                    std::string n = e->d_name;
                    n.resize(n.size() - 4);
                    scenes_.push_back(dir + "/" + n);
                }
            }
            closedir(d);
        }
        std::sort(scenes_.begin(), scenes_.end());
        if (scenes_.empty()) {
            if (display_) display_->MenuShowMessage("Truyện này chưa có trang nào\n(thiếu file .bin)\n\nChạm để quay lại", -1, -1);
            screen_ = Screen::kStoryEmpty;
            return;
        }
        scene_ = 0;
        ShowScene();
    }

    bool LoadStoryImage(const std::string& path, std::string& err) {
        int next = 1 - story_cur_;
        if (story_buf_[next] == nullptr) {
            story_buf_[next] = (uint8_t*)heap_caps_malloc(kStoryBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (story_buf_[next] == nullptr) story_buf_[next] = (uint8_t*)heap_caps_malloc(kStoryBytes, MALLOC_CAP_8BIT);
            if (story_buf_[next] == nullptr) { err = "hết bộ nhớ"; return false; }
        }
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) { err = "không mở được hình"; return false; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if ((size_t)size != kStoryBytes) {   // wrong picture file: never draw it
            fclose(f);
            err = "hình sai kích thước " + std::to_string(size);
            return false;
        }
        size_t got = fread(story_buf_[next], 1, kStoryBytes, f);
        fclose(f);
        if (got != kStoryBytes) { err = "đọc hình bị lỗi"; return false; }
        lv_image_dsc_t& d = story_dsc_[next];
        memset(&d, 0, sizeof(d));
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_RGB565;
        d.header.w = 320;
        d.header.h = 240;
        d.header.stride = 640;
        d.data_size = kStoryBytes;
        d.data = story_buf_[next];
        story_cur_ = next;
        return true;
    }

    // Show page scene_ and start reading it. Caller holds mutex_ and has stopped any earlier reading.
    void ShowScene() {
        screen_ = Screen::kStoryPage;
        scene_gen_++;
        std::string err;
        if (scene_ < 0 || scene_ >= (int)scenes_.size()) return;
        if (!LoadStoryImage(scenes_[scene_] + ".bin", err)) {
            if (display_) display_->MenuShowMessage(("Không hiện được trang\n" + err).c_str(), -1, -1);
            return;
        }
        if (display_) display_->MenuShowStoryPage(&story_dsc_[story_cur_]);
        std::string wav = scenes_[scene_] + ".wav";
        FILE* f = fopen(wav.c_str(), "rb");
        if (f == nullptr) return;   // silent page: stay until the child taps
        fclose(f);
        stop_ = false;
        playing_ = true;
        xTaskCreatePinnedToCore(&RubiMedia::StoryTask, "rubi_story", 8192, this, 4, nullptr, 1);
    }

    static void StoryTask(void* arg) {
        static_cast<RubiMedia*>(arg)->StoryLoop();
        vTaskDelete(nullptr);
    }

    void StoryLoop() {
        auto& as = Application::GetInstance().GetAudioService();
        int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        int my_gen = scene_gen_.load();
        std::string wav = scene_ < (int)scenes_.size() ? scenes_[scene_] + ".wav" : "";
        PlayFile(wav, out_rate, as);
        bool natural = !stop_;
        playing_ = false;
        if (!natural) return;
        vTaskDelay(pdMS_TO_TICKS(900));   // short pause between pages
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        if (screen_ != Screen::kStoryPage || my_gen != scene_gen_.load()) return;   // the child already moved on
        if (scene_ + 1 < (int)scenes_.size()) {
            scene_++;
            ShowScene();
        } else {
            scene_gen_++;
            ShowStoryList();   // end of the story
        }
    }

    // ---------------- Wi-Fi: scan, pick a network, type the password ----------------
    struct WifiNet { std::string ssid; int rssi; bool open; };
    std::vector<WifiNet> nets_;
    std::vector<int> wifi_row_action_;   // per row: net index, -1 next page, -2 rescan, -3 back, -4 phone setup
    int wpage_ = 0;
    bool scan_failed_ = false;
    std::string sel_ssid_, pass_;
    bool sel_open_ = false, show_pw_ = false;
    int kb_layer_ = 0;   // 0 lower, 1 upper (one letter), 2 symbols
    std::vector<RubiDisplay::KbKey> keys_;

    static void RunTask(std::function<void()> fn, const char* name) {
        auto* f = new std::function<void()>(std::move(fn));
        if (xTaskCreate([](void* a) {
                auto* g = static_cast<std::function<void()>*>(a);
                (*g)();
                delete g;
                vTaskDelete(nullptr);
            }, name, 6144, f, 4, nullptr) != pdPASS) {
            delete f;
        }
    }

    void StartWifiScan() {
        screen_ = Screen::kWifiBusy;
        if (display_) display_->MenuShowMessage("Đang quét Wi-Fi...", -1, -1);
        RunTask([this]() {
            // The Wi-Fi library's own station handler takes the scan results (and frees them)
            // on every SCAN_DONE event, so a scan started beside it always reads 0 networks.
            // Stop the station, scan on our own, then start the station again.
            auto& wm = WifiManager::GetInstance();
            wm.StopStation();
            vTaskDelay(pdMS_TO_TICKS(300));
            wifi_scan_config_t cfg = {};
            cfg.show_hidden = false;
            esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
            if (e == ESP_OK) e = esp_wifi_start();
            if (e == ESP_ERR_WIFI_CONN) e = ESP_OK;
            if (e == ESP_OK) {
                e = ESP_FAIL;
                for (int i = 0; i < 3 && e != ESP_OK; i++) {
                    e = esp_wifi_scan_start(&cfg, true);
                    if (e != ESP_OK) vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            std::vector<WifiNet> out;
            if (e == ESP_OK) {
                uint16_t n = 0;
                esp_wifi_scan_get_ap_num(&n);
                if (n > 24) n = 24;
                std::vector<wifi_ap_record_t> rec(n);
                if (n > 0 && esp_wifi_scan_get_ap_records(&n, rec.data()) == ESP_OK) {
                    for (int i = 0; i < n; i++) {
                        std::string name = reinterpret_cast<const char*>(rec[i].ssid);
                        if (name.empty()) continue;
                        bool dup = false;
                        for (auto& o : out) {
                            if (o.ssid == name) { dup = true; if (rec[i].rssi > o.rssi) o.rssi = rec[i].rssi; }
                        }
                        if (!dup) out.push_back({name, rec[i].rssi, rec[i].authmode == WIFI_AUTH_OPEN});
                    }
                }
                std::sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
            }
            esp_wifi_scan_stop();
            esp_wifi_stop();
            wm.StartStation();   // reconnects to the saved network in the background
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            nets_ = out;
            wpage_ = 0;
            scan_failed_ = (e != ESP_OK);
            ShowWifiList();
        }, "wifi_scan");
    }

    void ShowWifiList() {
        screen_ = Screen::kWifiList;
        wifi_row_action_.clear();
        std::vector<std::string> rows;
        std::vector<bool> locks;
        if (nets_.empty()) {
            rows.push_back(scan_failed_ ? "Không quét được Wi-Fi" : "Không thấy Wi-Fi nào");
            wifi_row_action_.push_back(-2);
            rows.push_back("Quét lại");
            wifi_row_action_.push_back(-2);
            rows.push_back("Cấu hình bằng điện thoại");
            wifi_row_action_.push_back(-4);
            rows.push_back("Quay lại");
            wifi_row_action_.push_back(-3);
        } else {
            const int per = 3;
            int total = (int)nets_.size();
            int pages = (total + per - 1) / per;
            if (wpage_ >= pages) wpage_ = 0;
            std::string cur = WifiManager::GetInstance().IsConnected() ? WifiManager::GetInstance().GetSsid() : "";
            for (int i = wpage_ * per; i < total && i < (wpage_ + 1) * per; i++) {
                int pct = std::max(0, std::min(100, 2 * (nets_[i].rssi + 100)));
                char line[120];
                snprintf(line, sizeof(line), "%s%s  %d%%", nets_[i].ssid.c_str(), nets_[i].ssid == cur ? " (đang dùng)" : "", pct);
                rows.push_back(line);
                locks.resize(rows.size(), false);
                locks[rows.size() - 1] = !nets_[i].open;
                wifi_row_action_.push_back(i);
            }
            if (pages > 1) { rows.push_back("Trang sau >"); wifi_row_action_.push_back(-1); }
            else { rows.push_back("Quét lại"); wifi_row_action_.push_back(-2); }
            rows.push_back("Quay lại");
            wifi_row_action_.push_back(-3);
        }
        locks.resize(rows.size(), false);
        if (display_) display_->MenuShowList("CHỌN WI-FI", rows, "", &locks);
    }

    void BuildKeys() {
        keys_.clear();
        static const char* kR1[3] = {"qwertyuiop", "QWERTYUIOP", "1234567890"};
        static const char* kR2[3] = {"asdfghjkl", "ASDFGHJKL", "@#$%&*-+()"};
        static const char* kR3[3] = {"zxcvbnm", "ZXCVBNM", "_=/.,?!"};
        const uint32_t kChar = 0xE6F0FF, kSpecial = 0xBBD0F0, kOk = 0xA5D6A7, kCancel = 0xFFC9C9;
        const int KH = 38, KW = 28, G = 3;
        int L = kb_layer_;
        keys_.push_back({4, 2, 64, 22, "< Hủy", 6, "", kCancel});
        auto addRow = [&](const char* chars, int y, int x0) {
            for (int i = 0; chars[i]; i++) {
                keys_.push_back({x0 + i * (KW + G), y, KW, KH, std::string(1, chars[i]), 0, std::string(1, chars[i]), kChar});
            }
        };
        addRow(kR1[L], 56, 6);
        addRow(kR2[L], 97, 22);
        // row 3: shift / letters / backspace
        keys_.push_back({10, 138, 40, KH, L == 2 ? "abc" : "Aa", L == 2 ? 3 : 1, "", L == 1 ? 0xFFE082 : kSpecial});
        addRow(kR3[L], 138, 10 + 40 + G);
        keys_.push_back({10 + 40 + G + 7 * (KW + G), 138, 40, KH, "Xóa", 2, "", kSpecial});
        // row 4
        keys_.push_back({7, 179, 60, KH, L == 2 ? "abc" : "123", 3, "", kSpecial});
        keys_.push_back({7 + 60 + G, 179, 170, KH, "Cách", 4, " ", kChar});
        keys_.push_back({7 + 60 + G + 170 + G, 179, 70, KH, "OK", 5, "", kOk});
    }

    void RenderKeyboard(const std::string& note) {
        screen_ = Screen::kWifiKeys;
        BuildKeys();
        std::string shown;
        if (show_pw_) shown = pass_; else shown.assign(pass_.size(), '*');
        shown += "_";
        std::string head = note.empty() ? sel_ssid_ : note;
        if (display_) display_->MenuShowKeyboard(head, shown, keys_, show_pw_);
    }

    void HandleKeyboardTouch(int sx, int sy) {
        if (sy >= 26 && sy < 54) {   // tap the password field: show / hide the characters
            show_pw_ = !show_pw_;
            RenderKeyboard("");
            return;
        }
        for (const auto& k : keys_) {
            if (sx < k.x - 1 || sx >= k.x + k.w + 1 || sy < k.y - 1 || sy >= k.y + k.h + 1) continue;
            switch (k.kind) {
            case 0:
                if (pass_.size() < 63) pass_ += k.val;
                if (kb_layer_ == 1) kb_layer_ = 0;
                RenderKeyboard("");
                break;
            case 1: kb_layer_ = (kb_layer_ == 1) ? 0 : 1; RenderKeyboard(""); break;
            case 2: if (!pass_.empty()) pass_.pop_back(); RenderKeyboard(""); break;
            case 3: kb_layer_ = (kb_layer_ == 2) ? 0 : 2; RenderKeyboard(""); break;
            case 4: if (pass_.size() < 63) pass_ += ' '; RenderKeyboard(""); break;
            case 5:
                if (pass_.size() < 8) RenderKeyboard("Mật khẩu cần từ 8 ký tự");
                else ConnectWifi();
                break;
            case 6: ShowWifiList(); break;
            }
            return;
        }
    }

    // "192.168.0." of this screen board's own Wi-Fi address (assumes a /24 home network), or empty.
    static std::string OwnPrefix() {
        esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info;
        if (!nif || esp_netif_get_ip_info(nif, &info) != ESP_OK || info.ip.addr == 0) return std::string();
        char b[24];
        snprintf(b, sizeof(b), "%d.%d.%d.", (int)(info.ip.addr & 0xFF), (int)((info.ip.addr >> 8) & 0xFF), (int)((info.ip.addr >> 16) & 0xFF));
        return std::string(b);
    }

    void ConnectWifi() {
        screen_ = Screen::kWifiBusy;
        if (display_) display_->MenuShowMessage(("Đang kết nối\n" + sel_ssid_).c_str(), -1, -1);
        RunTask([this, ssid = sel_ssid_, pass = sel_open_ ? std::string() : pass_, open = sel_open_]() {
            auto& sm = SsidManager::GetInstance();
            bool had = false;
            std::string old_pass;
            for (auto& it : sm.GetSsidList()) if (it.ssid == ssid) { had = true; old_pass = it.password; }
            sm.AddSsid(ssid, pass);
            const auto& lst = sm.GetSsidList();
            for (int i = 0; i < (int)lst.size(); i++) if (lst[i].ssid == ssid) { sm.SetDefaultSsid(i); break; }
            auto& wm = WifiManager::GetInstance();
            wm.StopStation();
            vTaskDelay(pdMS_TO_TICKS(500));
            wm.StartStation();
            bool ok = false;
            for (int i = 0; i < 40 && !ok; i++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                ok = wm.IsConnected() && wm.GetSsid() == ssid;
            }
            if (ok) {
                if (display_) display_->MenuShowMessage(("Đã kết nối!\n" + ssid + "\nĐang tìm robot...").c_str(), -1, -1);
                // The Wi-Fi now goes to the DevKit too; wait (up to ~30 s) for its new address, ignoring one left from the old network.
                std::string mine = OwnPrefix();
                std::string robot;
                RubiBattery::ForgetIp();
                for (int i = 0; i < 30 && robot.empty(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    std::string ip = RubiBattery::Ip();
                    if (!ip.empty() && !mine.empty() && ip.compare(0, mine.size(), mine) == 0) robot = ip;
                }
                if (display_) {
                    if (!robot.empty()) display_->MenuShowQr(("http://" + robot).c_str(), ("Đã kết nối!\nQuét bằng\nđiện thoại để\nđiều khiển\n" + robot).c_str());
                    else display_->MenuShowMessage(("Đã kết nối!\n" + ssid + "\n(robot chưa báo địa chỉ)").c_str(), -1, -1);
                }
                vTaskDelay(pdMS_TO_TICKS(robot.empty() ? 2500 : 20000));
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                ShowSettings();
                return;
            }
            // failed: put the old settings back and reconnect to whatever worked before
            if (had) {
                sm.AddSsid(ssid, old_pass);
            } else {
                const auto& l2 = sm.GetSsidList();
                for (int i = 0; i < (int)l2.size(); i++) if (l2[i].ssid == ssid) { sm.RemoveSsid(i); break; }
            }
            wm.StopStation();
            vTaskDelay(pdMS_TO_TICKS(500));
            wm.StartStation();
            if (display_) display_->MenuShowMessage("Không kết nối được.\nKiểm tra lại mật khẩu", -1, -1);
            vTaskDelay(pdMS_TO_TICKS(2500));
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            if (open) ShowWifiList(); else RenderKeyboard("");
        }, "wifi_conn");
    }

    void ShowVolume() {
        screen_ = Screen::kVolume;
        if (!display_) return;
        int v = Board::GetInstance().GetAudioCodec()->output_volume();
        char line[48];
        snprintf(line, sizeof(line), "Âm lượng hiện tại: %d%%", v);
        static const std::vector<const lv_image_dsc_t*> ico = {&rubi_icon_am_luong, nullptr, nullptr, &rubi_icon_quay_lai};
        display_->MenuShowList("ÂM LƯỢNG", {line, "+  Tăng", "-  Giảm", "Quay lại"}, "", nullptr, &ico);
    }

    void ShowMusic() {
        screen_ = Screen::kMusic;
        std::vector<std::string> rows;
        row_action_.clear();
        int total = (int)songs_.size();
        int pages = total == 0 ? 1 : (total + kSongsPerPage - 1) / kSongsPerPage;
        if (page_ >= pages) page_ = 0;
        if (total == 0) {
            rows.push_back("(Chưa có bài nhạc nào)");
            row_action_.push_back(-2);
        }
        for (int i = page_ * kSongsPerPage; i < total && i < (page_ + 1) * kSongsPerPage; i++) {
            std::string n = songs_[i];
            if (n.size() > 4) n.resize(n.size() - 4);   // drop ".wav"
            rows.push_back(n);
            row_action_.push_back(i);
        }
        if (pages > 1) {
            rows.push_back("Trang sau >");
            row_action_.push_back(-1);
        }
        rows.push_back("Quay lại");
        row_action_.push_back(-2);
        if (display_) display_->MenuShowList("NGHE NHẠC - chạm bài để phát", rows, "");
    }

    void ShowPlaying() {
        screen_ = Screen::kPlaying;
        std::string name = now_index_ < (int)songs_.size() ? songs_[now_index_] : "";
        if (name.size() > 4) name.resize(name.size() - 4);
        std::string msg = "Đang phát:\n" + name + "\n\n(Chạm màn hình để dừng)";
        if (display_) display_->MenuShowMessage(msg.c_str(), -1, -1);
    }

    // ---------------- bilingual learning ----------------
    struct Topic { const char* dir; const char* name; const lv_image_dsc_t* icon; };
    struct Word { std::string file, en, vi; };
    static constexpr int kCardPx = 140;
    static constexpr int kCardMaxPx = kCardPx;
    std::vector<Word> words_;
    int topic_ = 0;
    int word_ = 0;
    uint8_t* card_buf_[2] = {nullptr, nullptr};
    lv_image_dsc_t card_dsc_[2] = {};
    int card_cur_ = 0;
    std::string card_err_;

    static const Topic* Topics() {
        static const Topic t[4] = {
            {"trai_cay", "Trái cây", &rubi_icon_hoc_trai_cay},
            {"dong_vat", "Động vật", &rubi_icon_hoc_dong_vat},
            {"do_vat", "Đồ vật", &rubi_icon_hoc_do_vat},
            {"hinh_khoi", "Hình khối", &rubi_icon_hoc_hinh_khoi},
        };
        return t;
    }

    void ShowLearnTopics() {
        screen_ = Screen::kLearnTopics;
        if (!display_) return;
        const Topic* t = Topics();
        display_->MenuShowTiles({
            {t[0].icon, t[0].name}, {t[1].icon, t[1].name}, {t[2].icon, t[2].name}, {t[3].icon, t[3].name},
            {&rubi_icon_dong, "Quay lại"},
        });
    }

    static std::string Trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
        size_t i = 0;
        if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) i = 3;
        while (i < s.size() && s[i] == ' ') i++;
        return s.substr(i);
    }

    void OpenTopic(int t) {
        eng_mode_ = false;
        topic_ = t;
        word_ = 0;
        words_.clear();
        if (MountSd()) {
            std::string path = std::string("/sdcard/rubi/hoc/") + Topics()[t].dir + "/tu.txt";
            FILE* f = fopen(path.c_str(), "rb");
            if (f != nullptr) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    std::string l = Trim(line);
                    size_t a = l.find('|');
                    size_t b = a == std::string::npos ? a : l.find('|', a + 1);
                    if (b == std::string::npos) continue;
                    words_.push_back({Trim(l.substr(0, a)), Trim(l.substr(a + 1, b - a - 1)), Trim(l.substr(b + 1))});
                }
                fclose(f);
            }
        }
        if (words_.empty()) {
            screen_ = Screen::kLearnEmpty;
            if (display_) display_->MenuShowMessage("Chưa có hình cho chủ đề này\n(chép thư mục rubi/hoc vào thẻ nhớ)\n\nChạm để quay lại", -1, -1);
            return;
        }
        ShowCard();
    }

    // The picture file is a raw RGB565 square (128x128 or 140x140 ...); its side is worked out from the file size.
    bool LoadCardImage(const Word& w) {
        int next = 1 - card_cur_;
        const size_t max_bytes = kCardMaxPx * kCardMaxPx * 2;
        card_err_.clear();
        if (card_buf_[next] == nullptr) {
            card_buf_[next] = (uint8_t*)heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (card_buf_[next] == nullptr) card_buf_[next] = (uint8_t*)heap_caps_malloc(max_bytes, MALLOC_CAP_8BIT);
            if (card_buf_[next] == nullptr) { card_err_ = "hết bộ nhớ"; return false; }
        }
        // 1) picture built into the firmware
        {
            std::string key = std::string(Topics()[topic_].dir) + "/" + w.file;
            for (int i = 0; i < kRubiLearnPicCount; i++) {
                if (key != kRubiLearnPics[i].key) continue;
                const size_t px = (size_t)kCardPx * kCardPx;
                if (!RubiDecodePal(kRubiLearnPics[i].data, kRubiLearnPics[i].size,
                                   reinterpret_cast<uint16_t*>(card_buf_[next]), px)) break;
                lv_image_dsc_t& d = card_dsc_[next];
                memset(&d, 0, sizeof(d));
                d.header.magic = LV_IMAGE_HEADER_MAGIC;
                d.header.cf = LV_COLOR_FORMAT_RGB565;
                d.header.w = kCardPx;
                d.header.h = kCardPx;
                d.header.stride = kCardPx * 2;
                d.data_size = px * 2;
                d.data = card_buf_[next];
                card_cur_ = next;
                return true;
            }
        }
        // 2) otherwise a picture file on the SD card (exactly 140x140)
        std::string path = std::string("/sdcard/rubi/hoc/") + Topics()[topic_].dir + "/" + w.file + ".bin";
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) { card_err_ = "không mở được"; return false; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        const int side = kCardPx;
        if ((size_t)size != (size_t)side * side * 2) {   // damaged or wrong picture file: never draw it
            fclose(f);
            card_err_ = "sai kích thước " + std::to_string(size);
            return false;
        }
        size_t bytes = (size_t)size;
        size_t got = fread(card_buf_[next], 1, bytes, f);
        fclose(f);
        if (got != bytes) { card_err_ = "đọc " + std::to_string(got) + "/" + std::to_string(bytes); return false; }
        lv_image_dsc_t& d = card_dsc_[next];
        memset(&d, 0, sizeof(d));
        d.header.magic = LV_IMAGE_HEADER_MAGIC;
        d.header.cf = LV_COLOR_FORMAT_RGB565;
        d.header.w = side;
        d.header.h = side;
        d.header.stride = side * 2;
        d.data_size = bytes;
        d.data = card_buf_[next];
        card_cur_ = next;
        return true;
    }

    void ShowCard() {
        screen_ = Screen::kLearnCard;
        if (words_.empty() || !display_) return;
        const Word& w = words_[word_];
        if (eng_mode_) {   // English sentence card: text only, full width
            char c2[24];
            snprintf(c2, sizeof(c2), "%d/%d", word_ + 1, (int)words_.size());
            display_->MenuShowCard(nullptr, eng_titles_[eng_sel_], w.en, w.vi, c2, true);
            return;
        }
        bool ok = LoadCardImage(w);
        char cnt[24];
        snprintf(cnt, sizeof(cnt), ok ? "%d/%d" : "%d/%d  (hình lỗi)", word_ + 1, (int)words_.size());
        std::string title = Topics()[topic_].name;
        display_->MenuShowCard(ok ? &card_dsc_[card_cur_] : nullptr, title, w.en, w.vi, cnt);
    }

    // Tap on a card: play <word>_en.wav then <word>_vi.wav from the topic folder (made with tao_giong.py).
    // If those files are not there, fall back to asking the cloud assistant to read the words.
    void SpeakWord() {
        if (words_.empty() || playing_) return;
        const Word& w = words_[word_];
        std::string base = eng_mode_ ? EngDir() + "/" + w.file : std::string("/sdcard/rubi/hoc/") + Topics()[topic_].dir + "/" + w.file;
        speak_paths_.clear();
        speak_gap_ms_ = 300;
        const std::vector<const char*> suffixes = eng_mode_ ? std::vector<const char*>{".wav", "_vi.wav"} : std::vector<const char*>{"_en.wav", "_vi.wav"};
        for (const char* suffix : suffixes) {
            std::string p = base + suffix;
            FILE* f = fopen(p.c_str(), "rb");
            if (f != nullptr) { fclose(f); speak_paths_.push_back(p); }
        }
        if (!speak_paths_.empty()) {
            stop_ = false;
            playing_ = true;
            xTaskCreatePinnedToCore(&RubiMedia::SpeakTask, "rubi_say", 8192, this, 4, nullptr, 1);
            return;
        }
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) return;
        app.WakeWordInvoke("Hãy đọc to: " + w.en + ". Tiếng Việt: " + w.vi);
    }

    std::vector<std::string> speak_paths_;
    int speak_gap_ms_ = 300;
    bool norm_gain_ = false;   // PlayFile: even out the loudness of spoken clips

    static void SpeakTask(void* arg) {
        static_cast<RubiMedia*>(arg)->SpeakLoop();
        vTaskDelete(nullptr);
    }

    void SpeakLoop() {
        auto& as = Application::GetInstance().GetAudioService();
        int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        std::vector<std::string> paths = speak_paths_;
        norm_gain_ = true;
        for (size_t i = 0; i < paths.size() && !stop_; i++) {
            PlayFile(paths[i], out_rate, as);
            if (i + 1 < paths.size() && !stop_) vTaskDelay(pdMS_TO_TICKS(speak_gap_ms_));
        }
        norm_gain_ = false;
        playing_ = false;
    }

    void Close() {
        eng_mode_ = false;
        sleep_mode_ = false;
        tick_gen_++;
        StopPlayback();
        RestoreBacklight();
        UnmountSd();
        screen_ = Screen::kNone;
        if (display_) display_->MenuHide();
        vTaskDelay(pdMS_TO_TICKS(100));
        scene_gen_++;
        for (int i = 0; i < 2; i++) {
            if (card_buf_[i] != nullptr) { heap_caps_free(card_buf_[i]); card_buf_[i] = nullptr; }
            if (story_buf_[i] != nullptr) { heap_caps_free(story_buf_[i]); story_buf_[i] = nullptr; }
        }
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
    }

    // ---------------- SD card ----------------
    bool MountSd() {
        if (card_ != nullptr) return true;
        esp_vfs_fat_sdmmc_mount_config_t mc = {};
        mc.format_if_mount_failed = false;
        mc.max_files = 4;
        mc.allocation_unit_size = 16 * 1024;
        for (int width : {4, 1}) {
            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.max_freq_khz = SDMMC_FREQ_DEFAULT;
            sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
            slot.width = width;
            slot.clk = GPIO_NUM_38;
            slot.cmd = GPIO_NUM_40;
            slot.d0 = GPIO_NUM_39;
            slot.d1 = GPIO_NUM_41;
            slot.d2 = GPIO_NUM_48;
            slot.d3 = GPIO_NUM_47;
            slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
            esp_err_t r = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card_);
            if (r == ESP_OK) {
                ESP_LOGI(kTag, "SD mounted, %d-bit", width);
                return true;
            }
            ESP_LOGW(kTag, "SD mount (%d-bit) failed: %s", width, esp_err_to_name(r));
            card_ = nullptr;
        }
        return false;
    }

    void UnmountSd() {
        if (card_ != nullptr) {
            esp_vfs_fat_sdcard_unmount("/sdcard", card_);
            card_ = nullptr;
        }
    }

    static bool IsWav(const char* name) {
        size_t n = strlen(name);
        if (n < 5) return false;
        const char* e = name + n - 4;
        return tolower(e[0]) == '.' && tolower(e[1]) == 'w' && tolower(e[2]) == 'a' && tolower(e[3]) == 'v';
    }

    void LoadSongs() {
        songs_.clear();
        if (!MountSd()) return;
        DIR* d = opendir(kMusicDir);
        if (d == nullptr) return;
        while (dirent* e = readdir(d)) {
            if (e->d_type != DT_DIR && IsWav(e->d_name)) songs_.push_back(e->d_name);
        }
        closedir(d);
        std::sort(songs_.begin(), songs_.end());
    }

    // ---------------- playback ----------------
    void StartPlayback(int index) {
        stop_ = false;
        playing_ = true;
        now_index_ = index;
        ShowPlaying();
        xTaskCreatePinnedToCore(&RubiMedia::PlayTask, "rubi_play", 8192, this, 4, nullptr, 1);
    }

    void StopPlayback() {
        if (!playing_) return;
        stop_ = true;
        for (int i = 0; i < 100 && playing_; i++) vTaskDelay(pdMS_TO_TICKS(20));
        Application::GetInstance().GetAudioService().ResetDecoder();
    }

    static void PlayTask(void* arg) {
        static_cast<RubiMedia*>(arg)->PlayLoop();
        vTaskDelete(nullptr);
    }

    void PlayLoop() {
        auto& as = Application::GetInstance().GetAudioService();
        int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        if (voice_mode_) {
            // wait until Rubi has finished answering (Idle) before the music starts
            // After answering, the assistant normally stays in "listening". Once it has finished
            // speaking (or nothing was said for 8 s) stop listening so the state becomes Idle.
            bool seen_speaking = false, asked_stop = false;
            for (int w = 0; w < 1000 && !stop_; w++) {
                auto st = Application::GetInstance().GetDeviceState();
                if (st == kDeviceStateIdle) break;
                if (st == kDeviceStateSpeaking) seen_speaking = true;
                if (st == kDeviceStateListening && !asked_stop && w >= 25 &&
                    ((seen_speaking && as.IsPlaybackIdle()) || w >= 400)) {
                    asked_stop = true;
                    Application::GetInstance().StopListening();
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) stop_ = true;
        }
        bool voice_again = false;
        do {
            int64_t cycle_t0 = NowMs();
            for (int i = now_index_; i < (int)songs_.size() && !stop_; i++) {
                {
                    if (stop_) break;
                    std::lock_guard<std::recursive_mutex> lk(mutex_);
                    if (stop_) break;
                    now_index_ = i;
                    if (!sleep_mode_) ShowPlaying();
                }
                PlayFile(std::string(kMusicDir) + "/" + songs_[i], out_rate, as);
                if (sleep_mode_) vTaskDelay(pdMS_TO_TICKS(1500));   // a short pause between lullabies (also avoids a busy loop on a bad file)
            }
            now_index_ = 0;
            // Music started by voice keeps going (next song, back to the first after the last) until the child
            // touches the screen or says "tắt nhạc". The 3 s guard avoids a busy loop if every file fails to play.
            voice_again = false;
            if (voice_mode_ && !stop_) {
                vTaskDelay(pdMS_TO_TICKS(300));
                voice_again = (NowMs() - cycle_t0) > 3000;
            }
        } while ((sleep_mode_ && !stop_ && NowMs() < sleep_end_ms_) || voice_again);
        bool finished_by_itself = !stop_;
        bool was_voice = voice_mode_;
        if (was_voice) {
            UnmountSd();
            voice_mode_ = false;
            VoiceHideScreen();
        }
        playing_ = false;
        if (finished_by_itself && !was_voice && screen_ == Screen::kPlaying) {
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            if (screen_ == Screen::kPlaying) { page_ = now_index_ / kSongsPerPage; ShowMusic(); }
        }
    }

    static uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
    static uint16_t Le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

    void PlayFile(const std::string& path, int out_rate, AudioService& as) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) { ESP_LOGW(kTag, "cannot open %s", path.c_str()); return; }
        uint8_t hdr[12];
        if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
            fclose(f);
            return;
        }
        int channels = 0, bits = 0;
        uint32_t in_rate = 0, data_size = 0;
        uint16_t fmt = 0;
        bool have_data = false;
        uint8_t ch[8];
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t sz = Le32(ch + 4);
            if (memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
                uint8_t b[16];
                if (fread(b, 1, 16, f) != 16) break;
                fmt = Le16(b);
                channels = Le16(b + 2);
                in_rate = Le32(b + 4);
                bits = Le16(b + 14);
                if (sz > 16) fseek(f, sz - 16 + (sz & 1), SEEK_CUR);
            } else if (memcmp(ch, "data", 4) == 0) {
                data_size = sz;
                have_data = true;
                break;
            } else {
                fseek(f, sz + (sz & 1), SEEK_CUR);
            }
        }
        if (!have_data || bits != 16 || (channels != 1 && channels != 2) || in_rate < 8000 ||
            (fmt != 1 && fmt != 0xFFFE)) {
            ESP_LOGW(kTag, "unsupported wav %s (fmt=%d ch=%d bits=%d rate=%u)", path.c_str(), (int)fmt, channels, bits, (unsigned)in_rate);
            fclose(f);
            return;
        }
        // Spoken clips (maths, word cards) are normalised so that every clip is equally loud: gain = 29000 / peak, at most x6.
        float gain = 1.0f;
        if (norm_gain_) {
            long data_pos = ftell(f);
            int peak = 1;
            std::vector<int16_t> tmp(1024);
            uint32_t left = data_size;
            while (left > 0) {
                size_t got = fread(tmp.data(), 1, std::min<size_t>(tmp.size() * 2, left), f);
                if (got == 0) break;
                left -= got;
                for (size_t k = 0; k < got / 2; k++) { int v = tmp[k] < 0 ? -tmp[k] : tmp[k]; if (v > peak) peak = v; }
            }
            fseek(f, data_pos, SEEK_SET);
            gain = std::min(6.0f, 29000.0f / (float)peak);
            if (gain < 1.0f) gain = 1.0f;
        }
        const double step = (double)in_rate / (double)out_rate;
        std::vector<int16_t> raw(1024 * channels);
        std::vector<float> in;      // mono input waiting to be resampled
        std::vector<int16_t> out;   // output waiting to be pushed
        out.reserve(1300);
        double pos = 0;
        uint32_t remaining = data_size;
        while (!stop_ && remaining > 0) {
            if (voice_mode_ && Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                stop_ = true;   // Rubi was woken up or is talking: stop the music
                break;
            }
            size_t want = std::min<size_t>(raw.size() * 2, remaining);
            size_t got = fread(raw.data(), 1, want, f);
            if (got == 0) break;
            remaining -= got;
            size_t frames = got / (2 * channels);
            for (size_t k = 0; k < frames; k++) {
                int v = channels == 1 ? raw[k] : (raw[2 * k] + raw[2 * k + 1]) / 2;
                in.push_back((float)v);
            }
            while (pos + 1 < (double)in.size()) {
                size_t idx = (size_t)pos;
                float frac = (float)(pos - idx);
                float smp = (in[idx] * (1.0f - frac) + in[idx + 1] * frac) * gain;
                out.push_back((int16_t)(smp > 32767.0f ? 32767 : (smp < -32768.0f ? -32768 : smp)));
                pos += step;
                if (out.size() >= 1200) {
                    while (!stop_ && !as.PushPcmToPlayback(std::move(out))) {
                        // queue full: wait a little (PushPcmToPlayback already waits up to 500 ms)
                    }
                    out.clear();
                    out.reserve(1300);
                }
            }
            size_t consumed = (size_t)pos;
            if (consumed > 0 && consumed <= in.size()) {
                in.erase(in.begin(), in.begin() + consumed);
                pos -= consumed;
            }
        }
        if (!stop_ && !out.empty()) as.PushPcmToPlayback(std::move(out));
        fclose(f);
        // wait for the queue to drain a little so the end of the song is not cut
        for (int i = 0; i < 100 && !stop_ && !as.IsPlaybackIdle(); i++) vTaskDelay(pdMS_TO_TICKS(20));
    }
};

#endif  // RUBI_MEDIA_H
