#ifndef RUBI_MEDIA_H
#define RUBI_MEDIA_H

// Rubi touch menu + music player from the SD card (WAV files in /rubi/music).
// Hold the screen about 1 s to open the menu, then tap an icon (like on a phone).
// The touch panel is calibrated once (two taps) and the result is saved.
// While the menu is open the wake word is switched off so the music cannot wake Rubi.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

#include "application.h"
#include "board.h"
#include "rubi_face.h"
#include "rubi_icons.h"
#include "settings.h"

class RubiMedia {
public:
    static RubiMedia& Get() {
        static RubiMedia m;
        return m;
    }

    void SetDisplay(RubiDisplay* d) { display_ = d; }
    void SetWifiCallback(std::function<void()> cb) { wifi_cb_ = std::move(cb); }
    bool IsOpen() const { return screen_ != Screen::kNone; }

    void OpenMenu() {
        if (display_ == nullptr || IsOpen()) return;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        LoadCalibration();
        if (cal_.valid) ShowMain(); else StartCalibration();
    }

    // Called when a finger is lifted. raw_x/raw_y = last raw touch point. Returns true if consumed.
    bool HandleTouch(uint32_t press_ms, uint16_t raw_x, uint16_t raw_y) {
        if (!IsOpen()) return false;
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        switch (screen_) {
        case Screen::kCalib1:
            raw1_x_ = raw_x; raw1_y_ = raw_y;
            screen_ = Screen::kCalib2;
            if (display_) display_->MenuShowMessage("Chạm vào dấu + thứ hai", 305, 225);
            return true;
        case Screen::kCalib2:
            FinishCalibration(raw_x, raw_y);
            return true;
        default:
            break;
        }
        int sx = 0, sy = 0;
        Map(raw_x, raw_y, sx, sy);
        switch (screen_) {
        case Screen::kMain: {
            int t = RubiDisplay::TileAt(sx, sy, 4);
            if (t == 0) { LoadSongs(); page_ = 0; ShowMusic(); }
            else if (t == 1) { Close(); if (wifi_cb_) wifi_cb_(); }
            else if (t == 2) { StartCalibration(); }
            else if (t == 3) { Close(); }
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

private:
    enum class Screen { kNone, kCalib1, kCalib2, kMain, kMusic, kPlaying };
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
    Calib cal_;
    bool cal_loaded_ = false;
    uint16_t raw1_x_ = 0, raw1_y_ = 0;

    // ---------------- calibration ----------------
    void LoadCalibration() {
        if (cal_loaded_) return;
        cal_loaded_ = true;
        Settings st("rubi", false);
        if (st.GetInt("cal_ok", 0) != 1) return;
        cal_.valid = true;
        cal_.swap = st.GetInt("cal_swap", 0) != 0;
        cal_.ax = st.GetInt("cal_ax", 1000) / 1000.0f;
        cal_.bx = st.GetInt("cal_bx", 0) / 1000.0f;
        cal_.ay = st.GetInt("cal_ay", 1000) / 1000.0f;
        cal_.by = st.GetInt("cal_by", 0) / 1000.0f;
    }

    void StartCalibration() {
        screen_ = Screen::kCalib1;
        if (display_) display_->MenuShowMessage("Chỉnh cảm ứng: chạm vào dấu + đỏ", 15, 15);
    }

    void FinishCalibration(uint16_t rx2, uint16_t ry2) {
        float dx = (float)rx2 - (float)raw1_x_;
        float dy = (float)ry2 - (float)raw1_y_;
        if (fabsf(dx) < 40 && fabsf(dy) < 40) {
            StartCalibration();   // too close, start again
            return;
        }
        Calib c;
        c.swap = fabsf(dy) > fabsf(dx);
        const float span_x = 305 - 15, span_y = 225 - 15;
        float ra_x = c.swap ? ry2 - raw1_y_ : dx;   // raw axis that maps to screen x
        float ra_y = c.swap ? dx : dy;              // raw axis that maps to screen y
        float r1_x = c.swap ? raw1_y_ : raw1_x_;
        float r1_y = c.swap ? raw1_x_ : raw1_y_;
        if (fabsf(ra_x) < 20 || fabsf(ra_y) < 20) {
            StartCalibration();
            return;
        }
        c.ax = span_x / ra_x; c.bx = 15 - c.ax * r1_x;
        c.ay = span_y / ra_y; c.by = 15 - c.ay * r1_y;
        c.valid = true;
        cal_ = c;
        Settings st("rubi", true);
        st.SetInt("cal_ok", 1);
        st.SetInt("cal_swap", c.swap ? 1 : 0);
        st.SetInt("cal_ax", (int)lroundf(c.ax * 1000));
        st.SetInt("cal_bx", (int)lroundf(c.bx * 1000));
        st.SetInt("cal_ay", (int)lroundf(c.ay * 1000));
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
            {&rubi_icon_nghe_nhac, "Nghe nhạc"},
            {&rubi_icon_wifi, "Wi-Fi"},
            {&rubi_icon_chinh_cam_ung, "Chỉnh cảm ứng"},
            {&rubi_icon_dong, "Thoát"},
        });
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
        rows.push_back("<< Quay lại");
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

    void Close() {
        StopPlayback();
        UnmountSd();
        screen_ = Screen::kNone;
        if (display_) display_->MenuHide();
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
        for (int i = now_index_; i < (int)songs_.size() && !stop_; i++) {
            {
                if (stop_) break;
                std::lock_guard<std::recursive_mutex> lk(mutex_);
                if (stop_) break;
                now_index_ = i;
                ShowPlaying();
            }
            PlayFile(std::string(kMusicDir) + "/" + songs_[i], out_rate, as);
        }
        bool finished_by_itself = !stop_;
        playing_ = false;
        if (finished_by_itself) {
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
        const double step = (double)in_rate / (double)out_rate;
        std::vector<int16_t> raw(1024 * channels);
        std::vector<float> in;      // mono input waiting to be resampled
        std::vector<int16_t> out;   // output waiting to be pushed
        out.reserve(1300);
        double pos = 0;
        uint32_t remaining = data_size;
        while (!stop_ && remaining > 0) {
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
                out.push_back((int16_t)(in[idx] * (1.0f - frac) + in[idx + 1] * frac));
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
