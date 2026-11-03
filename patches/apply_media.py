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
# stuttered). Only when the speaker has been silent for 250 ms (a new reply / new sentence, or a real drop-out)
# Rubi first collects 5 packets (300 ms of speech, at most 400 ms wait) before playing. While speech is flowing it
# never waits, so it cannot add gaps in the middle of a sentence (the earlier pre-buffer re-armed on every short
# empty moment and made gaps). Local clips (PushPcmToPlayback) do not use the decode queue and are not affected.
patch('main/audio/audio_service.cc', [
    ('        /* Decode the audio from decode queue */\n        if (!audio_decode_queue_.empty() &&\n            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {\n',
     '        if (!audio_decode_queue_.empty() && audio_encode_queue_.empty() && audio_playback_queue_.empty() &&\n'
     '            !output_in_flight_) {\n'
     '            int64_t now_ms = esp_timer_get_time() / 1000;\n'
     '            if (now_ms - rubi_last_out_ms_ > 250 && audio_decode_queue_.size() < 5) {\n'
     '                if (rubi_jb_t0_ == 0) rubi_jb_t0_ = now_ms;\n'
     '                if (now_ms - rubi_jb_t0_ < 400) {\n'
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
     '            const float kBoost = 2.0f, kKnee = 20000.0f, kMax = 32700.0f;\n'
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
print('media patches applied')
