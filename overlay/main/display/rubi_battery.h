#ifndef RUBI_BATTERY_H
#define RUBI_BATTERY_H

// Battery level from the motor DevKit. The DevKit measures the 2S pack through the 5:1 voltage module and
// sends one text line per second over UART1:  "B<millivolts>\n"  (e.g. "B7620\n" = 7.62 V).
// Wiring: DevKit GPIO17 (TX) -> screen board UART socket pin RX0 (GPIO44);  DevKit GPIO16 (RX) <- screen board UART socket pin TX0 (GPIO43);  common GND.
// (Moved off GPIO14/21 so the 4-pin expansion socket stays free for the external I2S microphone.)
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RubiBattery {
public:
    static constexpr gpio_num_t kRx = GPIO_NUM_44, kTx = GPIO_NUM_43;

    // Same UART, other direction: write a text line to the DevKit (used to hand it the Wi-Fi). False until the UART is up.
    static bool Send(const char* data, size_t len) {
        if (!Ready().load()) return false;
        return uart_write_bytes(UART_NUM_1, data, len) >= 0;
    }
    // The DevKit reports its own address once it is on Wi-Fi: "I<ip>\n" (every 5 s). Empty string = none heard recently.
    static std::string Ip() {
        if (esp_timer_get_time() / 1000 - IpAt().load() > 20000) return std::string();
        char b[20];
        memcpy(b, IpBuf(), sizeof(b));
        b[sizeof(b) - 1] = 0;
        return std::string(b);
    }
    static void ForgetIp() { IpAt().store(-100000); }

    // cb(percent): 0..100, or -1 when no data has arrived for a while.
    static void Start(std::function<void(int)> cb) {
        static std::function<void(int)> g_cb;
        g_cb = std::move(cb);
        xTaskCreate([](void*) { Loop(g_cb); vTaskDelete(nullptr); }, "rubi_bat", 4096, nullptr, 2, nullptr);
    }

    // 2S Li-ion: pack millivolts -> percent (resting curve per cell)
    static int Percent(float pack_mv) {
        static const float v[] = {3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
        static const float p[] = {0, 5, 10, 20, 35, 50, 65, 80, 90, 100};
        float c = pack_mv / 2.0f;
        if (c <= v[0]) return 0;
        if (c >= v[9]) return 100;
        for (int i = 1; i < 10; i++) {
            if (c <= v[i]) return (int)(p[i - 1] + (p[i] - p[i - 1]) * (c - v[i - 1]) / (v[i] - v[i - 1]) + 0.5f);
        }
        return 100;
    }

    static int PinSet() { return PinSetRef().load(); }
    static void PinReplyClear() { PinReplyRef().store(-1); }
    static int PinReply() { return PinReplyRef().load(); }

private:
    static std::atomic<bool>& Ready() { static std::atomic<bool> r{false}; return r; }
    static std::atomic<int>& PinSetRef() { static std::atomic<int> v{-1}; return v; }     // -1 unknown, 0 none, 1 set
    static std::atomic<int>& PinReplyRef() { static std::atomic<int> v{-1}; return v; }   // -1 none yet, 0 refused, 1 done
    static std::atomic<int64_t>& IpAt() { static std::atomic<int64_t> t{-100000}; return t; }
    static char* IpBuf() { static char b[20] = {0}; return b; }
    static void Loop(std::function<void(int)> cb) {
        uart_config_t c = {};
        c.baud_rate = 115200;
        c.data_bits = UART_DATA_8_BITS;
        c.parity = UART_PARITY_DISABLE;
        c.stop_bits = UART_STOP_BITS_1;
        c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        c.source_clk = UART_SCLK_DEFAULT;
        if (uart_driver_install(UART_NUM_1, 512, 0, 0, nullptr, 0) != ESP_OK ||
            uart_param_config(UART_NUM_1, &c) != ESP_OK ||
            uart_set_pin(UART_NUM_1, kTx, kRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            ESP_LOGW("RubiBat", "uart init failed");
            return;
        }
        Ready().store(true);
        char line[24];
        int n = 0;
        float mv_avg = 0;
        int64_t last_ok = 0;
        int shown = -2;
        for (;;) {
            uint8_t b;
            int r = uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(200));
            int64_t now = esp_timer_get_time() / 1000;
            if (r == 1) {
                if (b == '\n' || b == '\r') {
                    line[n] = 0;
                    if (n >= 8 && n <= 16 && line[0] == 'I' && line[1] >= '0' && line[1] <= '9') {
                        memcpy(IpBuf(), line + 1, n);   // includes the terminating 0
                        IpAt().store(now);
                    }
                    if (n == 2 && line[0] == 'K' && (line[1] == '0' || line[1] == '1')) PinSetRef().store(line[1] - '0');
                    if (n == 2 && line[0] == 'P' && (line[1] == '0' || line[1] == '1')) PinReplyRef().store(line[1] - '0');
                    if (n >= 2 && line[0] == 'B') {
                        int mv = atoi(line + 1);
                        if (mv > 4000 && mv < 9500) {   // plausible 2S pack
                            mv_avg = (mv_avg == 0) ? (float)mv : mv_avg * 0.9f + mv * 0.1f;
                            last_ok = now;
                            int pct = Percent(mv_avg);
                            if (pct != shown) { shown = pct; cb(pct); }
                        }
                    }
                    n = 0;
                } else if (n < (int)sizeof(line) - 1) {
                    line[n++] = (char)b;
                } else {
                    n = 0;
                }
            }
            if (last_ok != 0 && now - last_ok > 6000 && shown != -1) { shown = -1; cb(-1); }
        }
    }
};

#endif
