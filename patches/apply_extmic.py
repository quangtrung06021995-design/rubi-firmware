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
static const float kRubiExtGain = 64.0f;

// Diagnostics shown on the Settings screen (state: 0 not probed, 1 external, 2 on-board; sd = probe std-dev; peak = loudest sample in the last 1 s)
volatile int g_rubi_mic_state = 0, g_rubi_mic_sd = 0, g_rubi_mic_peak = 0, g_rubi_mic_rms = 0;
volatile int g_rubi_mic_sdl = 0, g_rubi_mic_sdr = 0;   // probe std-dev of the left / right I2S slot
static int64_t g_rubi_peak_t0 = 0;
// Mic choice from the menu (0 = automatic, 1 = external, 2 = on-board), saved in NVS "rubi"/"mic".
volatile int g_rubi_mic_force = 0;
static int g_rubi_ext_slot = 0;   // 0 = left, 1 = right I2S slot carries the external mic
// Mic test: when g_rubi_cap_buf is set, the samples Rubi hears (24 kHz mono, before the voice filters) are copied here.
int16_t* volatile g_rubi_cap_buf = nullptr;
volatile int g_rubi_cap_len = 0, g_rubi_cap_pos = 0, g_rubi_cap_clip = 0;
static void RubiCapture(const int16_t* d, int n) {
    int16_t* b = g_rubi_cap_buf;
    if (b == nullptr) return;
    int pos = g_rubi_cap_pos, len = g_rubi_cap_len;
    for (int i = 0; i < n && pos < len; i++) {
        b[pos++] = d[i];
        if (d[i] >= 32000 || d[i] <= -32000) g_rubi_cap_clip = g_rubi_cap_clip + 1;
    }
    g_rubi_cap_pos = pos;
}
static void RubiMicLevel(const int16_t* d, int n) {
    int pk = 0;
    double e = 0;
    for (int i = 0; i < n; i++) { int a = d[i] < 0 ? -d[i] : d[i]; if (a > pk) pk = a; e += (double)d[i] * d[i]; }
    int64_t now = esp_timer_get_time();
    if (pk >= g_rubi_mic_peak || now - g_rubi_peak_t0 > 1000000) { g_rubi_mic_peak = pk; g_rubi_peak_t0 = now; }
    g_rubi_mic_rms = (int)sqrt(e / (n > 0 ? n : 1));
}

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
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
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
    // INMP441 datasheet: SD needs a pull-down (~100k) because the mic lets the line float during the other slot.
    // Without it the empty slot (and the edge bits of the mic's own slot) read random noise -> "xoet xoet" crackle.
    gpio_pulldown_en(GPIO_NUM_3);
    gpio_pullup_dis(GPIO_NUM_3);
    ESP_LOGI(TAG, "external mic: I2S1 started (BCLK 2, WS 14, SD 3, SD pull-down)");
}

// Reads ~0.3 s from the external port. A real INMP441 gives a small but non-zero noise floor; a missing mic gives
// constant values (std-dev 0) or full-scale random bits (huge std-dev).
void Es8311AudioCodec::ExtMicProbe() {
    {
        Settings st("rubi", false);
        g_rubi_mic_force = st.GetInt("mic", 0);
    }
    if (ext_rx_ == nullptr) { ext_state_ = 2; return; }
    const int n = 2400;
    int32_t* buf = (int32_t*)heap_caps_malloc(n * sizeof(int32_t), MALLOC_CAP_8BIT);
    if (buf == nullptr) { ext_state_ = 2; return; }
    size_t got = 0;
    // Stereo frames (left, right). The INMP441 talks in ONE slot (L/R pin low = left, high = right); the other slot
    // is empty. Measure both and keep the one with the signal.
    double sum[2] = {0, 0}, sum2[2] = {0, 0};
    int cnt = 0;
    for (int round = 0; round < 3; round++) {
        got = 0;
        if (i2s_channel_read(ext_rx_, buf, n * sizeof(int32_t), &got, 300) != ESP_OK) { got = 0; }
        if (round == 0) continue;  // first block may hold start-up garbage
        for (size_t i = 0; i + 1 < got / 4; i += 2) {
            for (int c = 0; c < 2; c++) {
                double v = (double)(buf[i + c] >> 8);
                sum[c] += v; sum2[c] += v * v;
            }
            cnt++;
        }
    }
    free(buf);
    if (cnt < 500) { ext_state_ = 2; g_rubi_mic_state = 2; g_rubi_mic_sd = -1; ESP_LOGW(TAG, "external mic: no data, using on-board mic"); return; }
    double sdc[2];
    for (int c = 0; c < 2; c++) { double m = sum[c] / cnt; double q = sum2[c] / cnt - m * m; sdc[c] = q > 0 ? sqrt(q) : 0; }
    // The INMP441 (L/R pin to GND) talks in the LEFT slot only; in the other slot its data pin floats (high impedance) and
    // the port reads electrical noise. Choosing "the louder slot" could therefore pick the floating slot (clicks, no voice).
    // So: prefer the left slot; use the right one only when the left one is dead and the right one clearly has a signal.
    g_rubi_mic_sdl = (int)sdc[0];
    g_rubi_mic_sdr = (int)sdc[1];
    // With the SD pull-down the empty slot reads ~0. Random floating bits give a std-dev in the millions.
    auto alive = [](double v) { return v > 3.0 && v < 1000000.0; };
    if (alive(sdc[0])) g_rubi_ext_slot = 0;
    else if (alive(sdc[1])) g_rubi_ext_slot = 1;
    else g_rubi_ext_slot = 0;
    double sd = sdc[g_rubi_ext_slot];
    ESP_LOGI(TAG, "external mic slots: left sd %.0f, right sd %.0f -> using %s", sdc[0], sdc[1], g_rubi_ext_slot ? "right" : "left");
    ext_state_ = (sd > 3.0 && sd < 1000000.0) ? 1 : 2;
    g_rubi_mic_sd = (int)sd;
    g_rubi_mic_state = ext_state_;
    ESP_LOGI(TAG, "external mic probe: std-dev %.0f -> %s", sd, ext_state_ == 1 ? "USING external mic" : "not found, using on-board mic");
}

int Es8311AudioCodec::ReadMic(int16_t* dest, int samples) {
    if (ext_state_ == 0) ExtMicProbe();
    int use = g_rubi_mic_force != 0 ? g_rubi_mic_force : ext_state_;
    if (use == 1 && ext_rx_ == nullptr) use = 2;
    g_rubi_mic_state = use;
    if (use != 1) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t)));
        RubiMicLevel(dest, samples);
        RubiCapture(dest, samples);
        return samples;
    }
    static std::vector<int32_t> fr;
    static std::vector<int32_t> buf;
    fr.resize(samples * 2);
    buf.resize(samples);
    size_t got = 0;
    if (i2s_channel_read(ext_rx_, fr.data(), samples * 2 * sizeof(int32_t), &got, 500) != ESP_OK) got = 0;
    int n = (int)(got / (2 * sizeof(int32_t)));   // one sample per stereo frame, from the mic's slot only
    for (int i = 0; i < n; i++) buf[i] = fr[2 * i + g_rubi_ext_slot];
    // INMP441 has a DC offset; remove it (slow tracking) BEFORE the gain, otherwise offset x gain hits full scale.
    static float dc = 0.0f;
    static float env = 0.0f;
    static bool dc_init = false;
    for (int i = 0; i < samples; i++) {
        if (i >= n) { dest[i] = 0; continue; }
        float raw = (float)(buf[i] >> 8) / 256.0f;   // 24-bit sample expressed in 16-bit units
        if (!dc_init) { dc = raw; dc_init = true; }
        dc += (raw - dc) * 0.002f;
        // Automatic gain control: loud (close) speech -> low gain, quiet (far) speech -> high gain, never hard-clipped.
        // (Tried a fixed gain 48 in 3.0.133: "Hey Rubi" woke worse than with this, so this stays.)
        float x = raw - dc;
        float ax = x < 0 ? -x : x;
        env += (ax - env) * (ax > env ? 0.01f : 0.00005f);   // fast attack (~4 ms), slow release (~0.8 s)
        // max gain 12 (was 40): in a long quiet spell x40 lifted the room noise almost to voice level and
        // "Hey Rubi" from 1 m was only caught when spoken right at the mic
        float gn = 6000.0f / (env > 500.0f ? env : 500.0f);   // aim for ~6000 peak; max gain 12
        if (gn < 1.5f) gn = 1.5f;
        float v = x * gn;
        if (v > 28000.0f) v = 28000.0f + (v - 28000.0f) * 0.1f;
        if (v < -28000.0f) v = -28000.0f + (v + 28000.0f) * 0.1f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        dest[i] = (int16_t)v;
    }
    RubiMicLevel(dest, samples);
    RubiCapture(dest, samples);
    return samples;
}
''')

patch('main/audio/codecs/es8311_audio_codec.cc', [
    ('#include <freertos/task.h>\n', '#include <freertos/task.h>\n#include <esp_timer.h>\n#include <driver/gpio.h>\n#include "settings.h"\n'),
])
print('external mic patch applied')
