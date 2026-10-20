// Rubi microphone test firmware: records from the on-board mic and streams it
// to a laptop over the console (USB) as base64 lines. No Wi-Fi, no cloud.
#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "board.h"
#include "audio_codec.h"

static const int kGains[] = {30, 36, 42};
static const int kSeconds = 6;

extern "C" void app_main(void) {
    nvs_flash_init();
    auto* codec = Board::GetInstance().GetAudioCodec();
    codec->Start();
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(500));

    const int rate = codec->input_sample_rate();
    const size_t total = (size_t)rate * kSeconds;
    int16_t* buf = (int16_t*)heap_caps_malloc(total * 2, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (int16_t*)heap_caps_malloc(total * 2, MALLOC_CAP_8BIT);
    if (!buf) { for (;;) { printf("@ERR nomem\n"); fflush(stdout); vTaskDelay(pdMS_TO_TICKS(2000)); } }

    printf("\n@HELLO rubi-mictest rate=%d\n", rate); fflush(stdout);
    for (;;) {
        for (int g : kGains) {
            for (int c = 3; c > 0; c--) {
                printf("@CUE gain=%d start_in=%d\n", g, c); fflush(stdout);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            codec->SetInputGain((float)g);
            codec->EnableInput(true);
            vTaskDelay(pdMS_TO_TICKS(300));
            printf("@REC gain=%d seconds=%d\n", g, kSeconds); fflush(stdout);
            std::vector<int16_t> chunk(480);
            size_t got = 0;
            while (got < total) {
                chunk.resize(480);
                if (!codec->InputData(chunk)) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
                size_t n = chunk.size();
                if (n > total - got) n = total - got;
                memcpy(buf + got, chunk.data(), n * 2);
                got += n;
            }
            codec->EnableInput(false);
            printf("@BEGIN gain=%d rate=%d samples=%u\n", g, rate, (unsigned)total);
            const uint8_t* p = (const uint8_t*)buf;
            size_t bytes = total * 2;
            for (size_t off = 0; off < bytes; off += 192) {
                size_t len = bytes - off < 192 ? bytes - off : 192;
                unsigned char out[260]; size_t olen = 0;
                mbedtls_base64_encode(out, sizeof(out) - 1, &olen, p + off, len);
                out[olen] = 0;
                printf("@D %s\n", (char*)out);
                if ((off / 192) % 20 == 0) vTaskDelay(1);
            }
            printf("@END gain=%d\n", g); fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}
