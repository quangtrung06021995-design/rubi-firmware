import re

def patch(path, pairs, append=None):
    s = open(path, encoding='utf-8').read()
    for old, new in pairs:
        assert s.count(old) == 1, (path, old[:60], s.count(old))
        s = s.replace(old, new, 1)
    if append:
        s += append
    open(path, 'w', encoding='utf-8').write(s)

B = 'main/boards/freenove-esp32s3-display-2.8-lcd/freenove-esp32s3-display-2.8-lcd.cc'
patch(B, [
    ('#include "display/rubi_face.h"\n', '#include "display/rubi_face.h"\n#include "display/rubi_media.h"\n\n// Called by application.cc for every speech-to-text result (spoken music commands)\nvoid rubi_handle_speech(const char* text) { RubiMedia::Get().HandleSpeech(text); }\n\n// Tells the DevKit (and so the web page) that a firmware update is running (U1, repeated) or has ended (U0)\nvoid rubi_updating(bool on) { RubiBattery::Send(on ? \"U1\\n\" : \"U0\\n\", 3); }\n'),
    ('        bool down = false;\n',
     '        bool down = false;\n        uint16_t lx = 0, ly = 0, fx = 0, fy = 0, tx = 0, ty = 0;\n        uint16_t sxs[16], sys_[16];\n        int sn = 0;\n'),
    ('                if (!down) {\n                    down = true;\n                    down_start = now;\n                }\n',
     '                if (!down) {\n                    down = true;\n                    down_start = now;\n                    sn = 0;\n                }\n                if (sn == 0) { fx = x; fy = y; }\n                tx = x; ty = y;\n                if (sn < 16) { sxs[sn] = x; sys_[sn] = y; sn++; }\n'),
    ('                // long tap\n                if (press > 3000) {\n                    self->EnterWifiConfigMode();\n                } else {',
     '                // Rubi: the last sample before the finger lifts drifts, so use the middle sample of the press\n'
     '                if (sn > 0) {\n'
     '                    if (sn <= 2) { lx = sxs[0]; ly = sys_[0]; }\n'
     '                    else {\n'
     '                        for (int a = 1; a < sn; a++) for (int b = a; b > 0 && sxs[b-1] > sxs[b]; b--) { uint16_t t0 = sxs[b]; sxs[b] = sxs[b-1]; sxs[b-1] = t0; }\n'
     '                        for (int a = 1; a < sn; a++) for (int b = a; b > 0 && sys_[b-1] > sys_[b]; b--) { uint16_t t0 = sys_[b]; sys_[b] = sys_[b-1]; sys_[b-1] = t0; }\n'
     '                        lx = sxs[sn / 2]; ly = sys_[sn / 2];\n'
     '                    }\n'
     '                }\n'
     '                // Rubi: icon menu first; tap the bottom-right button opens the menu; a hold of 1 s only before calibration\n'
     '                if (RubiMedia::Get().HandleSwipe(press, fx, fy, tx, ty) || RubiMedia::Get().HandleTouch(press, lx, ly)) {\n'
     '                    last_tap = 0;\n'
     '                } else if (press >= 1000 && !RubiMedia::Get().CalibrationValid()) {\n'
     '                    RubiMedia::Get().OpenMenu();\n'
     '                    last_tap = 0;\n'
     '                } else {'),
    ('        xTaskCreatePinnedToCore(TouchTask, "touch_task", 4096, this, 5, nullptr, 0);',
     '        RubiMedia::Get().SetDisplay(static_cast<RubiDisplay*>(display_));\n'
     '        RubiMedia::Get().SetWifiCallback([this]() { EnterWifiConfigMode(); });\n'
     '        xTaskCreatePinnedToCore(TouchTask, "touch_task", 8192, this, 5, nullptr, 0);'),
])

patch('main/audio/audio_service.h', [
    ('    void ResetDecoder();\n', '    void ResetDecoder();\n    bool PushPcmToPlayback(std::vector<int16_t>&& pcm);  // Rubi: raw PCM at the codec output rate\n'
     '    int64_t rubi_last_out_ms_ = 0;   // Rubi jitter buffer: when the speaker last got a frame\n'
     '    int64_t rubi_jb_t0_ = 0;         // Rubi jitter buffer: when the current wait started (0 = not waiting)\n'),
])
# Rubi: jitter buffer for the cloud voice (the Xiaozhi server is far away, packets arrive unevenly and the speech
# stuttered). Only when the speaker has been silent for 150 ms (a new reply / new sentence, or a real drop-out)
# Rubi first collects 8 packets (480 ms of speech, at most 600 ms wait) before playing. While speech is flowing it
# never waits, so it cannot add gaps in the middle of a sentence (the earlier pre-buffer re-armed on every short
# empty moment and made gaps). Local clips (PushPcmToPlayback) do not use the decode queue and are not affected.
patch('main/audio/audio_service.cc', [
    ('        /* Decode the audio from decode queue */\n        if (!audio_decode_queue_.empty() &&\n            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {\n',
     '        if (!audio_decode_queue_.empty() && audio_encode_queue_.empty() && audio_playback_queue_.empty() &&\n'
     '            !output_in_flight_) {\n'
     '            int64_t now_ms = esp_timer_get_time() / 1000;\n'
     '            if (now_ms - rubi_last_out_ms_ > 150 && audio_decode_queue_.size() < 8) {\n'
     '                if (rubi_jb_t0_ == 0) rubi_jb_t0_ = now_ms;\n'
     '                if (now_ms - rubi_jb_t0_ < 600) {\n'
     '                    audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(20));\n'
     '                    continue;\n'
     '                }\n'
     '            }\n'
     '        }\n'
     '        rubi_jb_t0_ = 0;\n'
     '\n'
     '        /* Decode the audio from decode queue */\n        if (!audio_decode_queue_.empty() &&\n            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {\n'),
    ('        output_in_flight_ = false;\n        notify_drained = MarkPlaybackDrainedLocked();\n',
     '        output_in_flight_ = false;\n        rubi_last_out_ms_ = esp_timer_get_time() / 1000;\n        notify_drained = MarkPlaybackDrainedLocked();\n'),
], append='''
bool AudioService::PushPcmToPlayback(std::vector<int16_t>&& pcm) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
        return audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE || service_stopped_.load();
    });
    if (service_stopped_.load() || audio_playback_queue_.full()) {
        return false;
    }
    AudioTask task;
    task.type = kAudioTaskTypeDecodeToPlaybackQueue;
    task.pcm = std::move(pcm);
    task.rubi_boost = true;
    audio_playback_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
    return true;
}
''')

# Rubi: loudness boost only for local clips/music (PushPcmToPlayback). Cloud-assistant voice is already loud and
# sounded rough/choppy with the boost, so it is left untouched.
patch('main/audio/audio_service.h', [
    ('    uint32_t media_position_ms = 0;\n};', '    uint32_t media_position_ms = 0;\n    bool rubi_boost = false;  // Rubi: apply loudness boost (local clips only)\n};'),
])
patch('main/audio/audio_service.cc', [
    ('        codec_->OutputData(task.pcm);\n',
     '        if (task.rubi_boost) {\n'
     '            const float kBoost = 1.4f, kKnee = 22000.0f, kMax = 32700.0f;   // was x2: sounded harsh (re re) at the same volume\n'
     '            for (auto& smp : task.pcm) {\n'
     '                float v = (float)smp * kBoost;\n'
     '                float a = v < 0 ? -v : v;\n'
     '                if (a > kKnee) {\n'
     '                    float over = (a - kKnee) / (kMax - kKnee);\n'
     '                    a = kKnee + (kMax - kKnee) * (over / (1.0f + over));\n'
     '                }\n'
     '                smp = (int16_t)(v < 0 ? -a : a);\n'
     '            }\n'
     '        }\n'
     '        codec_->OutputData(task.pcm);\n'),
])

patch('main/CMakeLists.txt', [
    ('                        ${MAIN_PRIV_REQUIRES_EXTRA}\n',
     '                        esp_driver_sdmmc\n                        sdmmc\n                        ${MAIN_PRIV_REQUIRES_EXTRA}\n'),
])

# Rubi: voice-stutter diagnosis. Counts, for each reply, the packets received (Goi), packets lost or out of order on
# the network (Mat), packets thrown away because they came before the speaking state (Bo), and how many times the
# speaker ran dry for 70 ms - 1.5 s in the middle of speech (Hut). Written to the serial log (W) when
# Rubi stops speaking (the on-screen line was removed once WebSocket fixed the stutter). Mat/Bo high = network; Hut high with Mat = 0 = the board itself is too slow.
patch('main/audio/audio_service.h', [
    ('#include <esp_timer.h>\n',
     '#include <esp_timer.h>\n#include <atomic>\n'
     'extern std::atomic<int> g_rubi_pkts, g_rubi_lost, g_rubi_drop, g_rubi_hut, g_rubi_udp;\n'),
])
patch('main/audio/audio_service.cc', [
    ('        output_in_flight_ = true;\n        audio_queue_cv_.notify_all();\n        lock.unlock();\n',
     '        output_in_flight_ = true;\n'
     '        if (!task.rubi_boost && rubi_last_out_ms_ > 0) {\n'
     '            int64_t gap_ms = esp_timer_get_time() / 1000 - rubi_last_out_ms_;\n'
     '            if (gap_ms > 70 && gap_ms < 1500) g_rubi_hut++;\n'
     '        }\n'
     '        audio_queue_cv_.notify_all();\n        lock.unlock();\n'),
], append='''
std::atomic<int> g_rubi_pkts{0}, g_rubi_lost{0}, g_rubi_drop{0}, g_rubi_hut{0}, g_rubi_udp{0};
''')
patch('main/protocols/mqtt_protocol.cc', [
    ('            if (sequence <= remote_sequence_) {\n                ESP_LOGW(TAG, "Received duplicate/old',
     '            g_rubi_udp = 1;\n'
     '            if (sequence <= remote_sequence_) {\n                g_rubi_lost++;\n                ESP_LOGW(TAG, "Received duplicate/old'),
    ('            if (sequence != remote_sequence_ + 1) {\n',
     '            if (sequence != remote_sequence_ + 1) {\n'
     '                if (remote_sequence_ > 0 && sequence > remote_sequence_ + 1 && sequence - remote_sequence_ < 1000) g_rubi_lost += (int)(sequence - remote_sequence_ - 1);\n'),
])
patch('main/application.cc', [
    ('#include "application.h"\n', '#include "application.h"\n#include <esp_wifi.h>\n'),
    ('        if (GetDeviceState() == kDeviceStateSpeaking) {\n            audio_service_.PushPacketToDecodeQueue(std::move(packet));\n        }\n',
     '        if (GetDeviceState() == kDeviceStateSpeaking) {\n            g_rubi_pkts++;\n            audio_service_.PushPacketToDecodeQueue(std::move(packet));\n        } else {\n            g_rubi_drop++;\n        }\n'),
    ('    DeviceState new_state = state_machine_.GetState();\n    clock_ticks_ = 0;\n',
     '    DeviceState new_state = state_machine_.GetState();\n    clock_ticks_ = 0;\n'
     '    {\n'
     '        static DeviceState rubi_prev = kDeviceStateUnknown;\n'
     '        if (new_state == kDeviceStateSpeaking && rubi_prev != kDeviceStateSpeaking) {\n'
     '            g_rubi_pkts = 0; g_rubi_lost = 0; g_rubi_hut = 0;\n'
     '        }\n'
     '        if (rubi_prev == kDeviceStateSpeaking && new_state != kDeviceStateSpeaking && g_rubi_pkts > 0) {\n'
     '            char b[96];\n'
     '            wifi_ap_record_t rubi_ap = {};\n'
     '            int rubi_rssi = esp_wifi_sta_get_ap_info(&rubi_ap) == ESP_OK ? rubi_ap.rssi : 0;\n'
     '            snprintf(b, sizeof(b), "Gói %d Mất %d Hụt %d Wifi %d", g_rubi_pkts.load(), g_rubi_lost.load(),\n'
     '                     g_rubi_hut.load(), rubi_rssi);\n'
     '            // shown after this state change has finished: the Listening/Idle status set below would hide it at once\n'
     '            std::string rubi_msg(b);\n'
     '            ESP_LOGW(TAG, "Rubi voice: %s", rubi_msg.c_str());\n'
     '            // shown for 3 s after each reply (temporary, to find where voice dropouts come from)\n'
     '            Schedule([rubi_msg]() { Board::GetInstance().GetDisplay()->ShowNotification(rubi_msg.c_str(), 3000); });\n'
     '            g_rubi_drop = 0;\n'
     '        }\n'
     '        rubi_prev = new_state;\n'
     '    }\n'),
])
# Rubi: wake word. The ES8311 runs full duplex, so upstream never turns the output off while the mic listens:
# the speaker amplifier (PA, GPIO1) stayed on all the time and its hiss went into the mic ("Hey Rubi" was much
# better with the speaker unplugged). Now the PA pin is switched off after 1.5 s without sound and back on with the
# next sound. The I2S clock keeps running, so the mic is not affected.
patch('main/audio/audio_codec.h', [
    ('    virtual void EnableOutput(bool enable);\n',
     '    virtual void EnableOutput(bool enable);\n    virtual void RubiSetPa(bool on) {}  // Rubi: speaker amplifier on/off only\n'),
])
patch('main/audio/codecs/es8311_audio_codec.h', [
    ('public:\n', 'public:\n    void RubiSetPa(bool on) override;\n'),
])
patch('main/audio/codecs/es8311_audio_codec.cc', [], append='''
void Es8311AudioCodec::RubiSetPa(bool on) {
    if (pa_pin_ == GPIO_NUM_NC) return;
    if (on && !output_enabled_) return;
    int level = on ? 1 : 0;
    gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
}
''')
patch('main/audio/audio_service.cc', [
    ('        codec_->OutputData(task.pcm);\n', '        codec_->RubiSetPa(true);\n        codec_->OutputData(task.pcm);\n'),
    ('    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {\n',
     '    if (output_elapsed > 1500 && codec_->output_enabled() && !output_in_flight_ && audio_playback_queue_.empty()) {\n'
     '        codec_->RubiSetPa(false);  // Rubi: amplifier off while silent (its hiss hurt the wake word)\n'
     '    }\n'
     '    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {\n'),
])

# Rubi: tap-to-talk without the wait. With WebSocket every new conversation first opens a TLS connection to the
# far Xiaozhi server (2-3 s). Now a tap starts recording at once; the encoded speech waits in the send queue
# (up to 5 s) and is sent right after "listen start" when the channel is open, so the child can talk immediately.
patch('main/audio/audio_service.h', [
    ('#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)', '#define MAX_SEND_PACKETS_IN_QUEUE (5000 / OPUS_FRAME_DURATION_MS)'),
])
patch('main/application.h', [
    ('    bool pending_listening_start_ = false;',
     '    bool rubi_prerecord_ = false;  // Rubi: recording started on tap while the channel opens\n    bool pending_listening_start_ = false;'),
])
patch('main/application.cc', [
    ('            SetDeviceState(kDeviceStateConnecting);\n            // Schedule to let the state change be processed first (UI update)\n            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });\n',
     '            SetDeviceState(kDeviceStateConnecting);\n'
     '            rubi_prerecord_ = true;\n'
     '            audio_service_.EnableWakeWordDetection(false);\n'
     '            audio_service_.EnableVoiceProcessing(true);\n'
     '            // Schedule to let the state change be processed first (UI update)\n            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });\n'),
    ('        if (bits & MAIN_EVENT_SEND_AUDIO) {\n', '        if ((bits & MAIN_EVENT_SEND_AUDIO) && !rubi_prerecord_) {\n'),
    ('            audio_service_.EnableVoiceProcessing(false);\n            audio_service_.EnableWakeWordDetection(true);\n            break;\n        case kDeviceStateConnecting:',
     '            audio_service_.EnableVoiceProcessing(false);\n'
     '            rubi_prerecord_ = false;\n'
     '            while (audio_service_.PopPacketFromSendQueue()) {\n            }\n'
     '            audio_service_.EnableWakeWordDetection(true);\n            break;\n        case kDeviceStateConnecting:'),
    ('        case kDeviceStateListening:\n            display->SetStatus(Lang::Strings::LISTENING);\n            display->SetEmotion("neutral");\n',
     '        case kDeviceStateListening:\n            display->SetStatus(Lang::Strings::LISTENING);\n            display->SetEmotion("neutral");\n'
     '            if (rubi_prerecord_) {\n'
     '                // recording already runs since the tap: tell the server, then send what was said meanwhile\n'
     '                rubi_prerecord_ = false;\n'
     '                protocol_->SendStartListening(listening_mode_);\n'
     '                ConfigureWakeWordForListening();\n'
     '                xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);\n'
     '                break;\n'
     '            }\n'),
])

# Rubi: WebSocket delivers late audio in bursts after a network stall. The decode queue held only 1.2 s and
# silently dropped packets when full (missing words). Hold 6 s instead.
patch('main/audio/audio_service.h', [
    ('#define MAX_DECODE_PACKETS_IN_QUEUE (1200 / OPUS_FRAME_DURATION_MS)', '#define MAX_DECODE_PACKETS_IN_QUEUE (6000 / OPUS_FRAME_DURATION_MS)'),
])
# Rubi: after "bye bye" the server sends {"type":"goodbye"}. The MQTT protocol closes the channel on it, the
# WebSocket protocol ignored it: Rubi stayed in "listening" on a finished session with the wake word switched off,
# so "Hey Rubi" did nothing until the connection timed out (about 2 minutes). Now WebSocket closes on goodbye too
# (same as a tap to end), Rubi goes back to idle and the wake word works at once.
patch('main/protocols/websocket_protocol.cc', [
    ('                if (strcmp(type->valuestring, "hello") == 0) {\n                    ParseServerHello(root);\n                } else {\n',
     '                if (strcmp(type->valuestring, "hello") == 0) {\n                    ParseServerHello(root);\n'
     '                } else if (strcmp(type->valuestring, "goodbye") == 0) {\n'
     '                    ESP_LOGI(TAG, "Received goodbye, closing the audio channel");\n'
     '                    Application::GetInstance().Schedule([this]() {\n'
     '                        if (IsAudioChannelOpened()) CloseAudioChannel(false);\n'
     '                    });\n'
     '                } else {\n'),
])

print('media patches applied')
