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
    ('#include "display/rubi_face.h"\n', '#include "display/rubi_face.h"\n#include "display/rubi_media.h"\n\n// Called by application.cc for every speech-to-text result (spoken music commands)\nvoid rubi_handle_speech(const char* text) { RubiMedia::Get().HandleSpeech(text); }\n'),
    ('        bool down = false;\n',
     '        bool down = false;\n        uint16_t lx = 0, ly = 0;\n        uint16_t sxs[16], sys_[16];\n        int sn = 0;\n'),
    ('                if (!down) {\n                    down = true;\n                    down_start = now;\n                }\n',
     '                if (!down) {\n                    down = true;\n                    down_start = now;\n                    sn = 0;\n                }\n                if (sn < 16) { sxs[sn] = x; sys_[sn] = y; sn++; }\n'),
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
     '                if (RubiMedia::Get().HandleTouch(press, lx, ly)) {\n'
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
    ('    void ResetDecoder();\n', '    void ResetDecoder();\n    bool PushPcmToPlayback(std::vector<int16_t>&& pcm);  // Rubi: raw PCM at the codec output rate\n'),
])
patch('main/audio/audio_service.cc', [], append='''
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
    audio_playback_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
    return true;
}
''')

patch('main/CMakeLists.txt', [
    ('                        ${MAIN_PRIV_REQUIRES_EXTRA}\n',
     '                        esp_driver_sdmmc\n                        sdmmc\n                        ${MAIN_PRIV_REQUIRES_EXTRA}\n'),
])
print('media patches applied')
