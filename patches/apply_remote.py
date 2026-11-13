# Remote talking: tap the microphone samples (16 kHz) so the parents can listen when they ask for it.
def patch(path, pairs):
    s = open(path, encoding='utf-8').read()
    for old, new in pairs:
        assert s.count(old) == 1, (path, old[:70], s.count(old))
        s = s.replace(old, new, 1)
    open(path, 'w', encoding='utf-8').write(s)

B = 'main/boards/freenove-esp32s3-display-2.8-lcd/freenove-esp32s3-display-2.8-lcd.cc'
patch(B, [
    ('void rubi_updating(bool on) {',
     '// Called by the audio input task with every block of microphone samples (only does something while the parents listen)\n'
     'void rubi_mic_tap(const int16_t* d, int n, int ch) { RubiRemote::Get().MicTap(d, n, ch); }\n\n'
     'void rubi_updating(bool on) {'),
])
patch('main/audio/audio_service.cc', [
    ('    /* Update the last input time */\n    last_input_time_ = std::chrono::steady_clock::now();\n',
     '    /* Update the last input time */\n    last_input_time_ = std::chrono::steady_clock::now();\n'
     '    if (sample_rate == 16000) rubi_mic_tap(data.data(), (int)data.size(), codec_->input_channels());\n'),
    ('bool AudioService::ReadAudioData(',
     'void rubi_mic_tap(const int16_t* d, int n, int ch);  // defined in the board file\n\nbool AudioService::ReadAudioData('),
])
