# Rubi: external INMP441 microphone on a second I2S port (I2S1): BCLK=GPIO2, WS=GPIO14, SD=GPIO3 (IO21 is the camera Wi-Fi line).
# Falls back to the on-board ES8311 mic when nothing is detected on the external port.
def patch(path, pairs, append=None):
    s = open(path, encoding='utf-8').read()
    for old, new in pairs:
        assert s.count(old) == 1, (path, old[:70], s.count(old))
        s = s.replace(old, new, 1)
    if append:
        s += append
    open(path, 'w', encoding='utf-8').write(s)

patch('main/audio/codecs/es8311_audio_codec.h', [
    ('#include <mutex>\n', '#include <mutex>\n#include <vector>\n#include <cmath>\n#include <esp_heap_caps.h>\n'),
    ('    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);\n',
     '    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);\n'
     '    i2s_chan_handle_t ext_rx_ = nullptr;   // Rubi: external INMP441 (I2S1)\n'
     '    int ext_state_ = 0;                    // 0 = not probed yet, 1 = use external mic, 2 = absent -> on-board mic\n'
     '    void ExtMicInit();\n'
     '    void ExtMicProbe();\n'
     '    int ReadMic(int16_t* dest, int samples);\n'),
])

patch('main/audio/codecs/es8311_audio_codec.cc', [
    ('    CreateDuplexChannels(mclk, bclk, ws, dout, din);\n',
     '    CreateDuplexChannels(mclk, bclk, ws, dout, din);\n    ExtMicInit();\n'),
    ('        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t)));\n        if (!g_filters_ready) {',
     '        ReadMic(dest, samples);\n        if (!g_filters_ready) {'),
], append='''
// ---- Rubi: external INMP441 microphone ----
// INMP441: 24-bit samples MSB-first in a 32-bit slot, L/R pin to GND = left slot. Its output is quiet
// (-26 dBFS at 94 dB SPL), so a fixed digital gain is applied before the voice filters.
static const float kRubiExtGain = 48.0f;

void Es8311AudioCodec::ExtMicInit() {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num = 6;
    cc.dma_frame_num = 240;
    if (i2s_new_channel(&cc, nullptr, &ext_rx_) != ESP_OK) {
        ext_rx_ = nullptr;
        ext_state_ = 2;
        ESP_LOGW(TAG, "external mic: channel create failed");
        return;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_NUM_2,
            .ws = GPIO_NUM_14,
            .dout = I2S_GPIO_UNUSED,
            .din = GPIO_NUM_3,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(ext_rx_, &sc) != ESP_OK || i2s_channel_enable(ext_rx_) != ESP_OK) {
        i2s_del_channel(ext_rx_);
        ext_rx_ = nullptr;
        ext_state_ = 2;
        ESP_LOGW(TAG, "external mic: init failed");
        return;
    }
    ESP_LOGI(TAG, "external mic: I2S1 started (BCLK 2, WS 14, SD 3)");
}

// Reads ~0.3 s from the external port. A real INMP441 gives a small but non-zero noise floor; a missing mic gives
// constant values (std-dev 0) or full-scale random bits (huge std-dev).
void Es8311AudioCodec::ExtMicProbe() {
    if (ext_rx_ == nullptr) { ext_state_ = 2; return; }
    const int n = 2400;
    int32_t* buf = (int32_t*)heap_caps_malloc(n * sizeof(int32_t), MALLOC_CAP_8BIT);
    if (buf == nullptr) { ext_state_ = 2; return; }
    size_t got = 0;
    double sum = 0, sum2 = 0;
    int cnt = 0;
    for (int round = 0; round < 3; round++) {
        got = 0;
        if (i2s_channel_read(ext_rx_, buf, n * sizeof(int32_t), &got, 300) != ESP_OK) { got = 0; }
        if (round == 0) continue;  // first block may hold start-up garbage
        for (size_t i = 0; i < got / 4; i++) {
            double v = (double)(buf[i] >> 8);
            sum += v; sum2 += v * v; cnt++;
        }
    }
    free(buf);
    if (cnt < 1000) { ext_state_ = 2; ESP_LOGW(TAG, "external mic: no data, using on-board mic"); return; }
    double mean = sum / cnt;
    double sd = sqrt(sum2 / cnt - mean * mean);
    ext_state_ = (sd > 30.0 && sd < 2000000.0) ? 1 : 2;
    ESP_LOGI(TAG, "external mic probe: std-dev %.0f -> %s", sd, ext_state_ == 1 ? "USING external mic" : "not found, using on-board mic");
}

int Es8311AudioCodec::ReadMic(int16_t* dest, int samples) {
    if (ext_state_ == 0) ExtMicProbe();
    if (ext_state_ != 1) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t)));
        return samples;
    }
    static std::vector<int32_t> buf;
    buf.resize(samples);
    size_t got = 0;
    if (i2s_channel_read(ext_rx_, buf.data(), samples * sizeof(int32_t), &got, 500) != ESP_OK) got = 0;
    int n = (int)(got / sizeof(int32_t));
    for (int i = 0; i < samples; i++) {
        if (i >= n) { dest[i] = 0; continue; }
        float v = (float)(buf[i] >> 16) * kRubiExtGain;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dest[i] = (int16_t)v;
    }
    return samples;
}
''')
print('external mic patch applied')
