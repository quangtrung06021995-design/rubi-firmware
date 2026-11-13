#ifndef RUBI_REMOTE_H
#define RUBI_REMOTE_H

// Talking with the child from another network (parents' phone page "Xem Rubi").
//
//   phone --wss--> Cloudflare relay <--wss-- this board (role "dev")
//
// * Parent talks:   the phone sends 16 kHz mono PCM frames (0xA0 + samples). They are played on Rubi's speaker.
// * Parent listens: while the parent has pressed "Nghe Rubi" the microphone is sent to the phone (0xA1 + samples).
//
// Privacy rules built in:
//   - Nothing is sent from the microphone unless a parent is connected (relay says "V<n>", n > 0) AND has asked to listen
//     ("l:1", repeated every ~20 s by the phone page; no refresh for 60 s = stop).
//   - The screen always shows who is doing what (watching / talking / listening), see RubiDisplay::SetRemoteIndicator.
//   - The key (room name on the relay) comes from the DevKit over UART ("R<key>"), it is never stored in the firmware image.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "rubi_battery.h"

class RubiRemote {
public:
    static RubiRemote& Get() {
        static RubiRemote r;
        return r;
    }

    // Set by the display: 0 = nothing, 1 = parents watching, 2 = parent talking, 3 = parents listening.
    static std::function<void(int)>& Indicator() { static std::function<void(int)> f; return f; }
    // Set by the media player: stop the music so the parent's voice is not mixed with it.
    static std::function<void()>& Takeover() { static std::function<void()> f; return f; }

    static void Start() {
        static bool started = false;
        if (started) return;
        started = true;
        xTaskCreate([](void*) { Get().Loop(); vTaskDelete(nullptr); }, "rubi_remote", 8192, nullptr, 3, nullptr);
    }

    // Called by the audio input task for every 10 ms block of microphone samples (16 kHz, `ch` interleaved channels).
    void MicTap(const int16_t* d, int samples_total, int ch) {
        if (!listening_.load(std::memory_order_relaxed) || d == nullptr || ch < 1) return;
        uint32_t w = wr_.load(std::memory_order_relaxed);
        for (int i = 0; i + ch <= samples_total; i += ch) ring_[(w++) & (kRing - 1)] = d[i];
        wr_.store(w, std::memory_order_release);
    }

private:
    static constexpr const char* kTag = "RubiRemote";
    static constexpr const char* kCfgUrl =
        "https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/relay-cfg/relay.json";
    static constexpr uint32_t kRing = 16384;          // 1 s of microphone, power of two
    static constexpr int kFrame = 960;                // 60 ms at 16 kHz
    static constexpr int kPrebufferMs = 300;          // jitter buffer before the parent's voice starts playing

    static int64_t NowMs() { return esp_timer_get_time() / 1000; }

    // ---- shared state ----
    std::atomic<int> viewers_{0};
    std::atomic<bool> listening_{false};
    std::atomic<int64_t> listen_until_{0};
    std::atomic<int64_t> last_rx_{0};                 // last message from the relay (any kind)
    std::atomic<int64_t> talk_audio_at_{0};           // last parent audio frame
    std::atomic<bool> ws_dropped_{false};
    std::mutex pcm_mutex_;
    std::vector<int16_t> pcm_in_;                     // parent's voice, 16 kHz, waiting to be played
    int16_t ring_[kRing];
    std::atomic<uint32_t> wr_{0};
    uint32_t rd_ = 0;
    float gain_ = 1.0f;
    int shown_mode_ = -1;
    std::string host_;
    int64_t host_at_ = 0;

    void ShowMode(int m) {
        if (m == shown_mode_) return;
        shown_mode_ = m;
        if (Indicator()) Indicator()(m);
    }

    static bool WifiUp() {
        wifi_ap_record_t ap;
        return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    }

    bool FetchHost() {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) return false;
        auto http = network->CreateHttp(0);
        if (!http) return false;
        if (auto opened = http->Open("GET", kCfgUrl); !opened) return false;
        auto status = http->GetStatusCode();
        if (!status || *status != 200) { http->Close(); return false; }
        std::string body = http->ReadAll();
        http->Close();
        size_t p = body.find("\"host\":\"");
        if (p == std::string::npos) return false;
        p += 8;
        size_t e = body.find('"', p);
        if (e == std::string::npos || e <= p || e - p > 90) return false;
        std::string h = body.substr(p, e - p);
        for (char c : h) {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-')) return false;
        }
        host_ = h;
        host_at_ = NowMs();
        return true;
    }

    // Parent's voice arrives on the websocket task: only copy it into the buffer.
    void OnParentAudio(const char* data, size_t len) {
        size_t n = (len - 1) / 2;
        if (n == 0) return;
        std::lock_guard<std::mutex> lk(pcm_mutex_);
        size_t old = pcm_in_.size();
        pcm_in_.resize(old + n);
        memcpy(pcm_in_.data() + old, data + 1, n * 2);   // little-endian PCM16 on a little-endian chip
        if (pcm_in_.size() > 32000) pcm_in_.erase(pcm_in_.begin(), pcm_in_.begin() + (pcm_in_.size() - 32000));   // keep at most 2 s
        talk_audio_at_.store(NowMs());
    }

    void OnText(const char* data, size_t len) {
        std::string t(data, len);
        if (t.size() >= 2 && t[0] == 'V') {
            viewers_.store(std::max(0, std::min(9, atoi(t.c_str() + 1))));
            if (viewers_.load() == 0) listening_.store(false);
        } else if (t == "l:1") {
            if (viewers_.load() > 0) {
                listening_.store(true);
                listen_until_.store(NowMs() + 60000);
            }
        } else if (t == "l:0") {
            listening_.store(false);
        } else if (t == "t:1") {
            talk_audio_at_.store(NowMs());
        }
    }

    // Plays the parent's voice: linear-resample 16 kHz -> codec rate and push to the speaker queue.
    // Returns true while a parent is (still) talking.
    bool PlayParent(AudioService& as, int out_rate, bool& playing, double& pos, std::vector<float>& in) {
        int64_t now = NowMs();
        {
            std::lock_guard<std::mutex> lk(pcm_mutex_);
            if (!playing && (int)pcm_in_.size() < kPrebufferMs * 16) {
                return now - talk_audio_at_.load() < 1500;   // still collecting the first 300 ms
            }
            if (!pcm_in_.empty()) {
                for (int16_t s : pcm_in_) in.push_back((float)s);
                pcm_in_.clear();
            }
        }
        if (!playing) {
            playing = true;
            pos = 0;
            if (Takeover()) Takeover()();   // stop the music first (blocks up to 2 s, the buffer keeps filling)
        }
        const double step = 16000.0 / (double)out_rate;
        std::vector<int16_t> out;
        out.reserve(1300);
        while (pos + 1 < (double)in.size()) {
            size_t idx = (size_t)pos;
            float frac = (float)(pos - idx);
            float smp = in[idx] * (1.0f - frac) + in[idx + 1] * frac;
            out.push_back((int16_t)(smp > 32767.0f ? 32767 : (smp < -32768.0f ? -32768 : smp)));
            pos += step;
            if (out.size() >= 1200) {
                int tries = 0;
                while (!as.PushPcmToPlayback(std::move(out)) && ++tries < 5) {}
                out.clear();
                out.reserve(1300);
            }
        }
        if (!out.empty()) as.PushPcmToPlayback(std::move(out));
        size_t consumed = (size_t)pos;
        if (consumed > 0 && consumed <= in.size()) {
            in.erase(in.begin(), in.begin() + consumed);
            pos -= (double)consumed;
        }
        if (in.size() < 2 && now - talk_audio_at_.load() > 1200) {   // the parent stopped: be ready for the next time
            playing = false;
            in.clear();
            pos = 0;
            return false;
        }
        return true;
    }

    // Sends the microphone to the parents, 60 ms per frame, with a gentle automatic gain (the board's mic is quiet).
    template <class W>
    bool SendMic(W& ws) {
        uint32_t w = wr_.load(std::memory_order_acquire);
        if (w - rd_ > kRing) rd_ = w - kRing / 2;   // we fell behind: skip
        int frames = 0;
        while (w - rd_ >= (uint32_t)kFrame && frames < 4) {
            static uint8_t buf[1 + kFrame * 2];
            int peak = 1;
            int16_t tmp[kFrame];
            for (int i = 0; i < kFrame; i++) {
                tmp[i] = ring_[(rd_ + i) & (kRing - 1)];
                int a = tmp[i] < 0 ? -tmp[i] : tmp[i];
                if (a > peak) peak = a;
            }
            rd_ += kFrame;
            float target = peak >= 200 ? std::min(8.0f, std::max(1.0f, 12000.0f / (float)peak)) : 1.0f;   // do not amplify hiss
            gain_ = gain_ * 0.7f + target * 0.3f;
            buf[0] = 0xA1;
            for (int i = 0; i < kFrame; i++) {
                int v = (int)((float)tmp[i] * gain_);
                v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
                buf[1 + 2 * i] = (uint8_t)(v & 0xFF);
                buf[2 + 2 * i] = (uint8_t)((v >> 8) & 0xFF);
            }
            if (!ws.Send(buf, sizeof(buf), true)) return false;
            frames++;
        }
        return true;
    }

    void Loop() {
        auto& as = Application::GetInstance().GetAudioService();
        int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
        if (out_rate < 8000) out_rate = 16000;
        int fail = 0;
        for (;;) {
            std::string key = RubiBattery::RKey();
            if (key.empty() || !WifiUp()) {
                ShowMode(0);
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            // TLS needs some internal memory: do not start when the board is short of it
            if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 70 * 1024 ||
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 32 * 1024) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            if (host_.empty() || NowMs() - host_at_ > 6LL * 3600 * 1000) {
                if (!FetchHost() && host_.empty()) {
                    vTaskDelay(pdMS_TO_TICKS(30000));
                    continue;
                }
            }
            auto network = Board::GetInstance().GetNetwork();
            auto ws = network->CreateWebSocket(1);
            if (ws == nullptr) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
            ws->OnData([this](const char* data, size_t len, bool binary) {
                last_rx_.store(NowMs());
                if (binary) {
                    if (len > 1 && (uint8_t)data[0] == 0xA0) OnParentAudio(data, len);
                } else {
                    OnText(data, len);
                }
            });
            ws_dropped_.store(false);
            ws->OnDisconnected([this]() { ws_dropped_.store(true); });
            std::string url = "wss://" + host_ + "/ws?role=dev&k=" + key;
            viewers_.store(0);
            listening_.store(false);
            if (auto connected = ws->Connect(url.c_str()); !connected) {
                ESP_LOGW(kTag, "relay connect failed");
                ws.reset();
                fail = std::min(fail + 1, 6);
                vTaskDelay(pdMS_TO_TICKS(5000 * fail));
                continue;
            }
            fail = 0;
            ESP_LOGW(kTag, "relay connected");
            last_rx_.store(NowMs());
            int64_t beat_at = 0;
            bool playing = false;
            double pos = 0;
            std::vector<float> in;
            while (!ws_dropped_.load() && ws->IsConnected()) {
                int64_t now = NowMs();
                if (now - beat_at >= 15000) {
                    beat_at = now;
                    ws->Send(std::string("h"));
                }
                if (now - last_rx_.load() > 45000) { ESP_LOGW(kTag, "relay silent, reconnecting"); break; }
                if (RubiBattery::RKey() != key) break;   // the parents changed the key
                if (listening_.load() && now > listen_until_.load()) listening_.store(false);   // no refresh: stop listening
                bool talking = PlayParent(as, out_rate, playing, pos, in);
                if (listening_.load()) {
                    if (!SendMic(*ws)) break;
                } else {
                    rd_ = wr_.load();   // start from "now" the next time listening begins
                    gain_ = 1.0f;
                }
                int mode = listening_.load() ? 3 : (talking ? 2 : (viewers_.load() > 0 ? 1 : 0));
                ShowMode(mode);
                vTaskDelay(pdMS_TO_TICKS(talking ? 10 : 20));
            }
            listening_.store(false);
            viewers_.store(0);
            ShowMode(0);
            ws.reset();
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
};

#endif
