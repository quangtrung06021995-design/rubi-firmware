// Rubi camera (ESP32-S3-CAM, OV2640): Wi-Fi setup page, live video for the phone, OTA updates.
//
// First boot (no Wi-Fi saved): the board opens a Wi-Fi network "Rubi-Cam-Setup".
//   Join it, open http://192.168.4.1 and type your home Wi-Fi + a viewing key.
// Normal boot: joins home Wi-Fi, then open  http://rubi-cam.local/?k=YOUR_KEY
//   (or http://<ip>/?k=YOUR_KEY). Hold the BOOT button (GPIO0) for 3 s while powering on to
//   forget the Wi-Fi and start the setup again.
// Updates: at every boot the board reads manifest.json from GitHub (branch ota-cam) and installs
//   a newer firmware by itself.
// Remote viewing (from any network): the board also connects OUT to the Rubi relay (a Cloudflare Worker, address read from
//   branch relay-cfg) with a "relay key" (10-40 letters/digits, set on the camera page). While somebody watches through
//   the relay it sends JPEG frames; when nobody watches it sends nothing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_camera.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mdns.h"
#include "esp_websocket_client.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "rubicam";

#ifndef MANIFEST_URL
#define MANIFEST_URL "https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/ota-cam/manifest.json"
#endif

// ---- Camera pins of the ESP32-S3-CAM board (from the seller's pinout picture) ----
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 4
#define CAM_PIN_SIOC 5
#define CAM_PIN_D0 11  // CAM_Y2
#define CAM_PIN_D1 9   // CAM_Y3
#define CAM_PIN_D2 8   // CAM_Y4
#define CAM_PIN_D3 10  // CAM_Y5
#define CAM_PIN_D4 12  // CAM_Y6
#define CAM_PIN_D5 18  // CAM_Y7
#define CAM_PIN_D6 17  // CAM_Y8
#define CAM_PIN_D7 16  // CAM_Y9
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 7
#define CAM_PIN_PCLK 13

#ifndef RELAY_CFG_URL
#define RELAY_CFG_URL "https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/relay-cfg/relay.json"
#endif

#define AP_SSID "Rubi-Cam-Setup"
#define AP_PASS "rubicam123"
#define MIN_KEY_LEN 6

static char g_ssid[33];
static char g_pass[65];
static char g_key[33];
static char g_rkey[48];                 // relay key (remote viewing); empty = remote viewing off
static char g_rhost[96];                // relay host, e.g. rubi-relay.name.workers.dev
static volatile int g_rviewers = 0;     // people watching through the relay right now (told by the relay)
static volatile bool g_rconn = false;   // connected to the relay
static volatile int64_t g_rlast_us = 0; // when the relay last said anything (heartbeat watchdog)
static volatile bool g_updating = false;
static bool g_cam_ok = false;
static EventGroupHandle_t g_wifi_events;
#define WIFI_GOT_IP BIT0
static httpd_handle_t g_srv_main = NULL;
static httpd_handle_t g_srv_stream = NULL;
static volatile int g_fps = 0;        // frames sent per second (last second)
static volatile int g_frame_kb = 0;   // size of the last JPEG frame in KB

// ---------------------------------------------------------------- storage

static bool load_settings(void) {
    nvs_handle_t h;
    if (nvs_open("rubicam", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n1 = sizeof(g_ssid), n2 = sizeof(g_pass), n3 = sizeof(g_key), n4 = sizeof(g_rkey);
    bool ok = nvs_get_str(h, "ssid", g_ssid, &n1) == ESP_OK;
    if (nvs_get_str(h, "pass", g_pass, &n2) != ESP_OK) g_pass[0] = 0;
    if (nvs_get_str(h, "key", g_key, &n3) != ESP_OK) g_key[0] = 0;
    if (nvs_get_str(h, "rkey", g_rkey, &n4) != ESP_OK) g_rkey[0] = 0;
    nvs_close(h);
    return ok && g_ssid[0] != 0;
}

static void save_settings(const char *ssid, const char *pass, const char *key) {
    nvs_handle_t h;
    if (nvs_open("rubicam", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_set_str(h, "key", key);
    nvs_commit(h);
    nvs_close(h);
}

static void save_relay_key(const char *rk) {
    nvs_handle_t h;
    if (nvs_open("rubicam", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "rkey", rk);
    nvs_commit(h);
    nvs_close(h);
}

static void erase_settings(void) {
    nvs_handle_t h;
    if (nvs_open("rubicam", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- helpers

static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static bool key_ok(httpd_req_t *req) {
    if (g_key[0] == 0) return false;
    char q[160];
    char v[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    if (httpd_query_key_value(q, "k", v, sizeof(v)) != ESP_OK) return false;
    url_decode(v);
    return strcmp(v, g_key) == 0;
}

static esp_err_t deny(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Sai khoa xem / Wrong key. Mo dung dia chi co ?k=KHOA_CUA_BAN");
}

static const char *version(void) { return esp_app_get_description()->version; }

// compare "a.b.c" strings: returns >0 when a is newer than b
static int ver_cmp(const char *a, const char *b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}

// ---------------------------------------------------------------- camera

static void camera_start(void) {
    camera_config_t c = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d0 = CAM_PIN_D0,
        .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2,
        .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4,
        .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6,
        .pin_d7 = CAM_PIN_D7,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_CIF,  // 400x296: much faster over Wi-Fi than VGA; fine on a phone
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        g_cam_ok = false;
        return;
    }
    g_cam_ok = true;
    sensor_t *s = esp_camera_sensor_get();
    if (s) ESP_LOGI(TAG, "Camera sensor id PID=0x%x", s->id.PID);
}

// ---------------------------------------------------------------- web: setup mode

static const char SETUP_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Rubi Cam</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
    "input{width:100%;padding:10px;margin:6px 0 14px;font-size:16px;box-sizing:border-box}"
    "button{width:100%;padding:12px;font-size:17px}</style></head><body>"
    "<h2>Rubi Cam - cai dat Wi-Fi</h2>"
    "<form method=post action=/save>"
    "Ten Wi-Fi nha<input name=ssid required maxlength=32>"
    "Mat khau Wi-Fi<input name=pass maxlength=64>"
    "Khoa xem camera (it nhat 6 ky tu, chi chu va so)<input name=key required minlength=6 maxlength=32>"
    "<button>Luu va khoi dong lai</button></form></body></html>";

static esp_err_t setup_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static bool key_valid(const char *k) {
    if (strlen(k) < MIN_KEY_LEN) return false;
    for (; *k; k++) {
        char c = *k;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

static esp_err_t setup_post(httpd_req_t *req) {
    char body[400];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;
    char ssid[33] = "", pass[65] = "", key[48] = "";
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "pass", pass, sizeof(pass));
    httpd_query_key_value(body, "key", key, sizeof(key));
    url_decode(ssid);
    url_decode(pass);
    url_decode(key);
    if (ssid[0] == 0 || !key_valid(key)) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Thieu ten Wi-Fi hoac khoa xem khong hop le (6+ chu/so). Quay lai thu lai.");
    }
    save_settings(ssid, pass, key);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Da luu. Camera dang khoi dong lai. Noi dien thoai ve Wi-Fi nha va mo http://rubi-cam.local/?k=KHOA");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------- web: normal mode

static esp_err_t index_get(httpd_req_t *req) {
    if (!key_ok(req)) return deny(req);
    char host[64] = "rubi-cam.local";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    char *page = malloc(3000);
    if (!page) return ESP_FAIL;
    snprintf(page, 3000,
             "<!doctype html><html><head><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,initial-scale=1'><title>Rubi Cam</title>"
             "<style>body{margin:0;background:#111;color:#eee;font-family:sans-serif;text-align:center}"
             "img{width:100%%;max-width:720px}a{color:#8cf}</style></head><body>"
             "<h3>Rubi Cam %s</h3>"
             "<img src='http://%s:81/stream?k=%s' alt='video'>"
             "<p><a href='/snap?k=%s'>Chup anh</a> &middot; <a href='/status?k=%s'>Trang thai</a></p>"
             "<p>Camera: %s</p>"
             "<hr><p>Xem tu xa (ngoai Wi-Fi nha): <b>%s</b></p>"
             "<form method=get action=/relay><input type=hidden name=k value='%s'>"
             "<input name=rk placeholder='Khoa tu xa 10-40 chu/so (de trong = tat)' maxlength=40 size=34>"
             " <button>Luu</button></form></body></html>",
             version(), host, g_key, g_key, g_key, g_cam_ok ? "OK" : "LOI - kiem tra day camera",
             g_rkey[0] ? (g_rconn ? "dang bat, da noi may chu" : "dang bat, chua noi may chu") : "dang tat", g_key);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return r;
}

static esp_err_t snap_get(httpd_req_t *req) {
    if (!key_ok(req)) return deny(req);
    if (!g_cam_ok || g_updating) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
        return ESP_OK;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t r = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return r;
}

static esp_err_t status_get(httpd_req_t *req) {
    if (!key_ok(req)) return deny(req);
    char out[400];
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    snprintf(out, sizeof(out),
             "{\"version\":\"%s\",\"camera\":%s,\"updating\":%s,\"free_heap\":%u,\"rssi\":%d,\"fps\":%d,\"frame_kb\":%d,"
             "\"relay\":%s,\"relay_conn\":%s,\"relay_viewers\":%d}",
             version(), g_cam_ok ? "true" : "false", g_updating ? "true" : "false", (unsigned)esp_get_free_heap_size(), rssi,
             g_fps, g_frame_kb, g_rkey[0] ? "true" : "false", g_rconn ? "true" : "false", (int)g_rviewers);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static bool relay_key_valid(const char *k) {
    size_t n = strlen(k);
    if (n < 10 || n > 40) return false;
    for (; *k; k++) {
        char c = *k;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

// /relay?k=<view key>&rk=<relay key>   saves the relay key (empty rk = switch remote viewing off)
static esp_err_t relay_set_get(httpd_req_t *req) {
    if (!key_ok(req)) return deny(req);
    char q[240], rk[56] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "rk", rk, sizeof(rk));
    url_decode(rk);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (rk[0] != 0 && !relay_key_valid(rk)) {
        return httpd_resp_sendstr(req, "Khoa tu xa phai dai 10-40 ky tu, chi chu va so. Quay lai thu lai.");
    }
    save_relay_key(rk);
    strncpy(g_rkey, rk, sizeof(g_rkey) - 1);
    g_rkey[sizeof(g_rkey) - 1] = 0;
    return httpd_resp_sendstr(req, rk[0] ? "Da luu khoa tu xa. Camera se tu noi may chu trong it giay."
                                         : "Da tat xem tu xa.");
}

static esp_err_t stream_get(httpd_req_t *req) {
    if (!key_ok(req)) return deny(req);
    if (!g_cam_ok || g_updating) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera not ready");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=rubiframe");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char part[80];
    esp_err_t res = ESP_OK;
    int frames = 0;
    int64_t t0 = esp_timer_get_time();
    while (res == ESP_OK && !g_updating) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        int n = snprintf(part, sizeof(part), "\r\n--rubiframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                         (unsigned)fb->len);
        res = httpd_resp_send_chunk(req, part, n);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        g_frame_kb = (int)(fb->len / 1024);
        esp_camera_fb_return(fb);
        frames++;
        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            g_fps = frames;
            frames = 0;
            t0 = now;
        }
    }
    g_fps = 0;
    return res;
}


// ---------------------------------------------------------------- Wi-Fi sync from the screen board (UART)
// The screen board sends  W<ssid>\t<password>\t<checksum>\n  every 5 s while it is online. If the Wi-Fi differs from
// the saved one, store it and restart, so the camera follows the Wi-Fi chosen on the screen.
// Wiring: screen board IO21 (TX) -> camera GPIO42;  common GND.
#define SYNC_RX_PIN GPIO_NUM_42

static void wifisync_handle(char *line) {
    if (line[0] != 'W') return;
    char *ssid = line + 1;
    char *t1 = strchr(ssid, '\t');
    if (!t1) return;
    *t1 = 0;
    char *pass = t1 + 1;
    char *t2 = strchr(pass, '\t');
    if (!t2) return;
    *t2 = 0;
    unsigned sum = 0;
    for (char *p = ssid; *p; p++) sum += (unsigned char)*p;
    sum += '\t';
    for (char *p = pass; *p; p++) sum += (unsigned char)*p;
    if (strtoul(t2 + 1, NULL, 16) != (sum & 0xFF)) return;           // damaged line
    if (ssid[0] == 0 || strlen(ssid) > 32 || strlen(pass) > 64) return;
    if (strcmp(ssid, g_ssid) == 0 && strcmp(pass, g_pass) == 0) return;   // already the same
    ESP_LOGW(TAG, "Wi-Fi changed on the screen board -> saving '%s' and restarting", ssid);
    save_settings(ssid, pass, g_key);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void wifisync_task(void *arg) {
    uart_config_t c = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(UART_NUM_1, 512, 0, 0, NULL, 0) != ESP_OK || uart_param_config(UART_NUM_1, &c) != ESP_OK ||
        uart_set_pin(UART_NUM_1, UART_PIN_NO_CHANGE, SYNC_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGW(TAG, "wifi sync uart init failed");
        vTaskDelete(NULL);
        return;
    }
    char line[160];
    int n = 0;
    for (;;) {
        uint8_t b;
        if (uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(500)) != 1) continue;
        if (b == '\n' || b == '\r') {
            line[n] = 0;
            if (n > 3) wifisync_handle(line);
            n = 0;
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = (char)b;
        } else {
            n = 0;
        }
    }
}

// ---------------------------------------------------------------- web servers

static void start_servers(bool setup_mode) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&g_srv_main, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed");
        return;
    }
    if (setup_mode) {
        httpd_uri_t u1 = {.uri = "/", .method = HTTP_GET, .handler = setup_get};
        httpd_uri_t u2 = {.uri = "/save", .method = HTTP_POST, .handler = setup_post};
        httpd_register_uri_handler(g_srv_main, &u1);
        httpd_register_uri_handler(g_srv_main, &u2);
        return;
    }
    httpd_uri_t u1 = {.uri = "/", .method = HTTP_GET, .handler = index_get};
    httpd_uri_t u2 = {.uri = "/snap", .method = HTTP_GET, .handler = snap_get};
    httpd_uri_t u3 = {.uri = "/status", .method = HTTP_GET, .handler = status_get};
    httpd_register_uri_handler(g_srv_main, &u1);
    httpd_register_uri_handler(g_srv_main, &u2);
    httpd_register_uri_handler(g_srv_main, &u3);
    httpd_uri_t u4 = {.uri = "/relay", .method = HTTP_GET, .handler = relay_set_get};
    httpd_register_uri_handler(g_srv_main, &u4);

    // The video stream runs on its own server (port 81) so it never blocks the other pages.
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port = 81;
    scfg.ctrl_port += 1;
    scfg.stack_size = 6144;
    if (httpd_start(&g_srv_stream, &scfg) == ESP_OK) {
        httpd_uri_t us = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
        httpd_register_uri_handler(g_srv_stream, &us);
    }
}

// ---------------------------------------------------------------- Wi-Fi

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_GOT_IP);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(g_wifi_events, WIFI_GOT_IP);
    }
}

static void wifi_init_common(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
}

static void wifi_start_ap(void) {
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, AP_SSID);
    strcpy((char *)ap.ap.password, AP_PASS);
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Setup mode: join Wi-Fi '%s' (password %s), open http://192.168.4.1", AP_SSID, AP_PASS);
}

static void wifi_start_sta(void) {
    esp_netif_create_default_wifi_sta();
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, g_ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, g_pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = g_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // smoother video
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);  // 20 MHz channel: more stable than 40 MHz next to other networks
}

// ---------------------------------------------------------------- OTA

static bool fetch_manifest(char *ver, size_t vlen, char *url, size_t ulen) {
    char buf[512];
    esp_http_client_config_t hc = {
        .url = MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int n = esp_http_client_read(c, buf, sizeof(buf) - 1);
        if (n > 0 && esp_http_client_get_status_code(c) == 200) {
            buf[n] = 0;
            // tiny parser for {"version":"x.y.z","url":"https://..."}
            char *pv = strstr(buf, "\"version\":\"");
            char *pu = strstr(buf, "\"url\":\"");
            if (pv && pu) {
                pv += 11;
                pu += 7;
                char *ev = strchr(pv, '"');
                char *eu = strchr(pu, '"');
                if (ev && eu && (size_t)(ev - pv) < vlen && (size_t)(eu - pu) < ulen) {
                    memcpy(ver, pv, ev - pv);
                    ver[ev - pv] = 0;
                    memcpy(url, pu, eu - pu);
                    url[eu - pu] = 0;
                    ok = true;
                }
            }
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void ota_task(void *arg) {
    xEventGroupWaitBits(g_wifi_events, WIFI_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_ota_mark_app_valid_cancel_rollback();  // the new firmware started and has Wi-Fi: keep it

    char ver[24], url[256];
    if (!fetch_manifest(ver, sizeof(ver), url, sizeof(url))) {
        ESP_LOGW(TAG, "OTA: no manifest (offline or not published yet)");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "OTA: running %s, published %s", version(), ver);
    if (ver_cmp(ver, version()) <= 0) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "OTA: installing %s", ver);
    g_updating = true;  // stops the video stream
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_http_client_config_t hc = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t oc = {.http_config = &hc};
    esp_err_t r = esp_https_ota(&oc);
    if (r == ESP_OK) {
        ESP_LOGW(TAG, "OTA done, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: 0x%x", r);
    g_updating = false;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- remote viewing through the relay

// Reads {"host":"rubi-relay.name.workers.dev"} published by the relay deploy (branch relay-cfg).
static bool fetch_relay_host(char *host, size_t hl) {
    char buf[200];
    esp_http_client_config_t hc = {.url = RELAY_CFG_URL, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000};
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int n = esp_http_client_read(c, buf, sizeof(buf) - 1);
        if (n > 0 && esp_http_client_get_status_code(c) == 200) {
            buf[n] = 0;
            char *p = strstr(buf, "\"host\":\"");
            if (p) {
                p += 8;
                char *e = strchr(p, '"');
                if (e && e > p && (size_t)(e - p) < hl) {
                    bool clean = true;
                    for (char *x = p; x < e; x++) {
                        char ch = *x;
                        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-')) clean = false;
                    }
                    if (clean) {
                        memcpy(host, p, e - p);
                        host[e - p] = 0;
                        ok = true;
                    }
                }
            }
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void relay_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;
    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            g_rconn = true;
            g_rlast_us = esp_timer_get_time();
            ESP_LOGI(TAG, "relay: connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
            if (g_rconn) ESP_LOGW(TAG, "relay: disconnected");
            g_rconn = false;
            g_rviewers = 0;
            break;
        case WEBSOCKET_EVENT_DATA:
            g_rlast_us = esp_timer_get_time();
            // the relay tells the camera "V<n>": n people are watching
            if (d->op_code == 0x1 && d->data_len >= 2 && d->data_ptr[0] == 'V') {
                char t[8] = {0};
                int n = d->data_len - 1 < 7 ? d->data_len - 1 : 7;
                memcpy(t, d->data_ptr + 1, n);
                int v = atoi(t);
                g_rviewers = v < 0 ? 0 : (v > 9 ? 9 : v);
                ESP_LOGI(TAG, "relay: %d viewer(s)", (int)g_rviewers);
            }
            break;
        default:
            break;
    }
}

static void relay_task(void *arg) {
    xEventGroupWaitBits(g_wifi_events, WIFI_GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(20000));  // let the update check at boot finish first
    esp_websocket_client_handle_t cl = NULL;
    static char cur_uri[220];
    cur_uri[0] = 0;
    int64_t host_at = 0;
    int64_t frame_at = 0;
    int64_t beat_at = 0;
    for (;;) {
        bool want = g_rkey[0] != 0 && !g_updating && g_cam_ok;
        if (!want) {
            if (cl) {
                esp_websocket_client_destroy(cl);
                cl = NULL;
                cur_uri[0] = 0;
                g_rconn = false;
                g_rviewers = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (g_rhost[0] == 0 || now - host_at > 6LL * 3600 * 1000000LL) {
            if (fetch_relay_host(g_rhost, sizeof(g_rhost))) {
                host_at = now;
            } else if (g_rhost[0] == 0) {
                ESP_LOGW(TAG, "relay: address not published yet (deploy the relay first)");
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            } else {
                host_at = now - 5LL * 3600 * 1000000LL;  // retry in an hour
            }
        }
        char uri[220];
        snprintf(uri, sizeof(uri), "wss://%s/ws?role=cam&k=%s", g_rhost, g_rkey);
        if (cl == NULL || strcmp(uri, cur_uri) != 0) {
            if (cl) esp_websocket_client_destroy(cl);
            g_rconn = false;
            g_rviewers = 0;
            strncpy(cur_uri, uri, sizeof(cur_uri) - 1);
            esp_websocket_client_config_t wc = {
                .uri = cur_uri,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 2048,
                .task_stack = 6144,
                .ping_interval_sec = 20,
                .network_timeout_ms = 10000,
                .reconnect_timeout_ms = 5000,
                .keep_alive_enable = true,   // TCP keep-alive: a dead link is noticed in about 25 s
                .keep_alive_idle = 10,
                .keep_alive_interval = 5,
                .keep_alive_count = 3,
            };
            cl = esp_websocket_client_init(&wc);
            if (cl == NULL) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            esp_websocket_register_events(cl, WEBSOCKET_EVENT_ANY, relay_event, NULL);
            esp_websocket_client_start(cl);
        }
        // Heartbeat: every 15 s say "h"; the relay answers. No word from the relay for 45 s = the link died silently
        // (router / mobile carrier dropped it): throw the client away, the loop above builds a new one.
        if (g_rconn && esp_websocket_client_is_connected(cl)) {
            if (now - beat_at >= 15000000LL) {
                beat_at = now;
                esp_websocket_client_send_text(cl, "h", 1, pdMS_TO_TICKS(2000));
            }
            if (now - g_rlast_us > 45000000LL) {
                ESP_LOGW(TAG, "relay: silent for 45 s, reconnecting");
                esp_websocket_client_destroy(cl);
                cl = NULL;
                cur_uri[0] = 0;
                g_rconn = false;
                g_rviewers = 0;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        if (g_rconn && g_rviewers > 0 && esp_websocket_client_is_connected(cl)) {
            if (now - frame_at >= 200000) {  // about 5 frames per second
                frame_at = now;
                camera_fb_t *fb = esp_camera_fb_get();
                if (fb) {
                    esp_websocket_client_send_bin(cl, (const char *)fb->buf, fb->len, pdMS_TO_TICKS(3000));
                    g_frame_kb = (int)(fb->len / 1024);
                    esp_camera_fb_return(fb);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
}

// ---------------------------------------------------------------- main

static bool boot_button_held(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << GPIO_NUM_0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    for (int i = 0; i < 30; i++) {  // 3 seconds
        if (gpio_get_level(GPIO_NUM_0) != 0) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

// Tells the DevKit (http://rubi.local/api/cam) which version is running and whether an update is in progress,
// so the control page can show the camera's real version and lock itself during an update.
static void report_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        esp_ip4_addr_t addr;
        addr.addr = 0;
        if (mdns_query_a("rubi", 2000, &addr) == ESP_OK && addr.addr != 0) {
            char url[112];
            snprintf(url, sizeof(url), "http://" IPSTR "/api/cam?v=%s&u=%d", IP2STR(&addr), version(), g_updating ? 1 : 0);
            esp_http_client_config_t hc = {.url = url, .timeout_ms = 3000};
            esp_http_client_handle_t cl = esp_http_client_init(&hc);
            if (cl != NULL) {
                esp_http_client_perform(cl);
                esp_http_client_cleanup(cl);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(g_updating ? 3000 : 10000));
    }
}

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "Rubi Cam %s", version());

    if (boot_button_held()) {
        ESP_LOGW(TAG, "BOOT held: forgetting Wi-Fi");
        erase_settings();
    }

    wifi_init_common();
    if (!load_settings() || !key_valid(g_key)) {
        wifi_start_ap();
        start_servers(true);
        return;
    }

    xTaskCreate(wifisync_task, "wsync", 4096, NULL, 3, NULL);
    camera_start();
    wifi_start_sta();
    // Saved Wi-Fi unreachable (router changed / switched off)? Do not wait forever: after 45 s open the
    // setup network "Rubi-Cam-Setup" so the Wi-Fi can be changed from a phone WITHOUT opening the head.
    // If nothing is saved within 10 minutes, restart and try the saved Wi-Fi again.
    EventBits_t got = xEventGroupWaitBits(g_wifi_events, WIFI_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(45000));
    if (!(got & WIFI_GOT_IP)) {
        ESP_LOGW(TAG, "Saved Wi-Fi not reachable: opening setup network for 10 minutes");
        esp_netif_create_default_wifi_ap();
        wifi_config_t ap = {0};
        strcpy((char *)ap.ap.ssid, AP_SSID);
        strcpy((char *)ap.ap.password, AP_PASS);
        ap.ap.ssid_len = strlen(AP_SSID);
        ap.ap.max_connection = 2;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        start_servers(true);
        vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
        esp_restart();
        return;
    }

    mdns_init();
    mdns_hostname_set("rubi-cam");
    mdns_instance_name_set("Rubi Cam");
    start_servers(false);
    ESP_LOGI(TAG, "Open http://rubi-cam.local/?k=<your key>");

    xTaskCreate(ota_task, "ota", 8192, NULL, 4, NULL);
    xTaskCreate(report_task, "report", 4096, NULL, 3, NULL);
    xTaskCreate(relay_task, "relay", 6144, NULL, 3, NULL);
}
