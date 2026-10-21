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
    ('#include "display/rubi_face.h"\n', '#include "display/rubi_face.h"\n#include "display/rubi_media.h"\n'),
    ('        bool down = false;\n',
     '        bool down = false;\n        uint16_t lx = 0, ly = 0;\n'),
    ('                if (!down) {\n                    down = true;\n                    down_start = now;\n                }\n',
     '                if (!down) {\n                    down = true;\n                    down_start = now;\n                }\n                lx = x;\n                ly = y;\n'),
    ('                // long tap\n                if (press > 3000) {\n                    self->EnterWifiConfigMode();\n                } else {',
     '                // Rubi: icon menu first; a hold of 1 s opens the menu\n'
     '                if (RubiMedia::Get().HandleTouch(press, lx, ly)) {\n'
     '                    last_tap = 0;\n'
     '                } else if (press >= 1000) {\n'
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
