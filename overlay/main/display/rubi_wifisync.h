#ifndef RUBI_WIFISYNC_H
#define RUBI_WIFISYNC_H

// Wi-Fi sync to the camera board (one-way UART). Whenever the screen board is connected to a Wi-Fi network it sends
// the network name and password every 5 s over UART2 TX as one text line:
//     W<ssid>\t<password>\t<checksum 2 hex digits>\n        (checksum = sum of the bytes of "<ssid>\t<password>" mod 256)
// The camera board stores them if they differ from what it has, then restarts. So Wi-Fi is set up once (on the screen)
// and the camera follows.
// The same line also goes to the motor DevKit over the UART socket (TX0 = GPIO43 -> DevKit RX2 = GPIO16), see rubi_battery.h.
// Wiring: screen board expansion socket pin IO21 (TX) -> camera board GPIO42;  common GND.
#include <cstdio>
#include <cstring>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "rubi_battery.h"

class RubiWifiSync {
public:
    static constexpr gpio_num_t kTx = GPIO_NUM_21;

    static void Start() {
        xTaskCreate([](void*) { Loop(); vTaskDelete(nullptr); }, "rubi_wsync", 4096, nullptr, 1, nullptr);
    }

private:
    static void Loop() {
        uart_config_t c = {};
        c.baud_rate = 115200;
        c.data_bits = UART_DATA_8_BITS;
        c.parity = UART_PARITY_DISABLE;
        c.stop_bits = UART_STOP_BITS_1;
        c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        c.source_clk = UART_SCLK_DEFAULT;
        if (uart_driver_install(UART_NUM_2, 512, 0, 0, nullptr, 0) != ESP_OK ||
            uart_param_config(UART_NUM_2, &c) != ESP_OK ||
            uart_set_pin(UART_NUM_2, kTx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            ESP_LOGW("RubiWsync", "uart init failed");
            return;
        }
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) continue;      // not connected: nothing to share
            wifi_config_t cfg = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) continue;
            char ssid[33] = {0}, pass[65] = {0};
            memcpy(ssid, cfg.sta.ssid, strnlen((const char*)cfg.sta.ssid, 32));
            memcpy(pass, cfg.sta.password, strnlen((const char*)cfg.sta.password, 64));
            if (ssid[0] == 0) continue;
            unsigned sum = 0;
            for (const char* p = ssid; *p; p++) sum += (unsigned char)*p;
            sum += '\t';
            for (const char* p = pass; *p; p++) sum += (unsigned char)*p;
            char line[128];
            int n = snprintf(line, sizeof(line), "W%s\t%s\t%02X\n", ssid, pass, sum & 0xFF);
            if (n > 0 && n < (int)sizeof(line)) {
                uart_write_bytes(UART_NUM_2, line, n);          // -> camera (IO21)
                RubiBattery::Send(line, n);                      // -> DevKit (UART socket TX0 / GPIO43)
            }
        }
    }
};

#endif
