// Rubi - DONG HO CUA BE (bo ESP32-2424S012C-I: ESP32-C3, man tron GC9A01 240x240, cam ung CST816D)
//
// Chuc nang:
//   - Noi Bluetooth voi DevKit trong robot (tu dong tim, tu noi lai), gui lenh nhu web dieu khien.
//   - Man hinh: Dong ho (gio lay tu Rubi) -> Menu: Lai xe, Nhay, Nhac, Truyen, Hoc, Cai dat.
//   - "Di theo": dong ho phat ma hong ngoai 38 kHz (ma 0x3C) qua chan IR_PIN de robot di theo be.
//   - Cap nhat OTA qua Wi-Fi tu GitHub (Cai dat -> CAP NHAT): dong ho xin Wi-Fi tu Rubi qua Bluetooth.
//
// Nap lan dau bang cap USB-C (Arduino IDE): Board "ESP32C3 Dev Module", USB CDC On Boot: Enabled,
//   Flash Size 4MB, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)".
//   Hoac nap file merged .bin tu GitHub Actions o dia chi 0x0.
// Thu vien: "GFX Library for Arduino" (moi nhat), "NimBLE-Arduino" 1.4.3, "JPEGDEC".

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#include <JPEGDEC.h>
#include <esp_system.h>
#include "faces.h"
#include "dials.h"
#include "icons.h"
#include "volicons.h"

// ---------------- Chan (theo ban ve cua bo ESP32-2424S012) ----------------
#define TFT_DC   2
#define TFT_CS   10
#define TFT_SCK  6
#define TFT_MOSI 7
#define TFT_BL   3
#define TP_SDA   4
#define TP_SCL   5
#define TP_INT   0
#define TP_RST   1
#define IR_PIN   21          // chan hong ngoai (dau TX cua cong 4 chan JST-SH tren bo). Doi neu hang long vao chan khac.

// ---------------- Cau hinh (sua o day neu man hinh/cam ung bi nguoc) ----------------
const bool BL_ACTIVE_HIGH = true;        // den nen sang khi chan BL = HIGH. Neu man hinh den thui, doi thanh false
const int  SCREEN_ROT     = 0;           // 0..3
const bool TOUCH_FLIP_X   = false;
const bool TOUCH_FLIP_Y   = false;
const bool TOUCH_SWAP_XY  = false;
const uint8_t IR_ID       = 0x3C;        // ma cua dong ho (de sac la 0xA5)
const int  DRIVE_LIMIT    = 60;          // % toc do toi da khi be lai xe tu dong ho
const uint32_t DIM_MS     = 20000;       // vang tay sau bay nhieu ms thi giam do sang
const uint32_t MENU_IDLE_MS = 15000;     // khong cham o menu bay nhieu ms thi tu ve mat dong ho
const uint32_t OFF_MS     = 60000;       // (khong dung nua: thay bang man hinh luon hien gio)
const uint32_t AOD_MS     = 20000;       // vang tay bay nhieu ms o mat dong ho thi chuyen sang man hinh toi chi hien gio
const int      AOD_BL     = 10;          // do sang den nen luc man hinh toi (0-255): thap de tiet kiem pin
struct DHand { int ux, uy, len, tail, hw; };   // kim dong ho (dat o day de Arduino tu sinh prototype duoc)
#define NFACE (6 + DIAL_COUNT)           // 0-5 mat so anh, tu 6: mat kim nen anh (dials.h)
#define FACE_ANALOG(f) ((f) >= 6)
const uint32_t FACE_ROTATE_MS = 300000;  // tu doi mat dong ho moi 5 phut
const char* WATCH_KEY     = "RB4821WT";  // phai trung WATCH_KEY trong firmware DevKit
#define TOUCH_DEBUG 0   // 1 = hien toa do cham tren man hinh chinh de chan doan (tat khi xong)
const char* FW_VERSION    = "dev";       // GitHub Actions doi thanh 1.0.<so lan build>
const char* DEFAULT_WIFI_SSID = "TP-Link_7FA8";   // Wi-Fi nha, dung khi chua co Wi-Fi tu Rubi
const char* DEFAULT_WIFI_PASS = "";
const char* OTA_MANIFEST_URL = "https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/ota-watch/manifest.json";

#define BLE_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// ---------------- Man hinh ----------------
Arduino_DataBus* bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX* gfx = new Arduino_GC9A01(bus, GFX_NOT_DEFINED, SCREEN_ROT, true);

uint16_t C_BG, C_TXT, C_DIM, C_OK, C_WARN, C_BAD, C_BLUE, C_GREEN, C_ORANGE, C_PURPLE, C_TEAL, C_GREY;

void setBacklight(int v) {
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    ledcWrite(TFT_BL, BL_ACTIVE_HIGH ? v : 255 - v);
}

// ---------------- Trang thai chung (BLE task <-> giao dien) ----------------
volatile bool g_bleConn = false, g_bleAuthed = false, g_otaMode = false, g_bleOff = false;
volatile int  g_robotPct = -1, g_musicN = 0, g_storyN = 0, g_robotWifi = 0;
volatile bool g_haveWifi = false;
String g_wifiSsid, g_wifiPass;
int g_tzMin = 540;   // mui gio (phut); Rubi gui mui gio that qua Bluetooth, luu lai trong NVS
int g_ledColor = 0, g_ledEff = 0;     // mau (0 xanh,1 hong,2 vang,3 luc,4 cau vong), 0 dung yen / 1 nhay
int g_ledMask = 15;                    // den LED tren robot (bit0 nguc, bit1/2 chan bun, bit3 ang-ten); 0 = tat het
bool g_follow = false;                 // dang phat tin hieu cho robot di theo
QueueHandle_t g_txQ = nullptr;

void sendCmd(const char* s) {
    if (!g_txQ) return;
    char b[32];
    strncpy(b, s, 31);
    b[31] = 0;
    xQueueSend(g_txQ, b, 0);
}
void sendCmdf(const char* fmt, int a, int b = 0) {
    char m[32];
    snprintf(m, sizeof(m), fmt, a, b);
    sendCmd(m);
}

// ---------------- BLE (chay trong task rieng de man hinh khong bi dung) ----------------
static NimBLEClient* g_cli = nullptr;
static NimBLERemoteCharacteristic* g_rx = nullptr;
static NimBLERemoteCharacteristic* g_tx = nullptr;
static char g_in[160];
static size_t g_inN = 0;

static void xuLyDong(const char* l) {
    if (l[0] == 'A') {
        g_bleAuthed = (l[1] == '1');
        if (g_bleAuthed && g_txQ) { char w[32] = "W"; xQueueSend(g_txQ, w, 0); }   // xin Wi-Fi de tu cap nhat
    } else if (l[0] == 'S') {
        int p = -1, m = 0, t = 0, w = 0;
        if (sscanf(l + 1, "%d,%d,%d,%d", &p, &m, &t, &w) == 4) { g_robotPct = p; g_musicN = m; g_storyN = t; g_robotWifi = w; }
    } else if (l[0] == 'E') {
        int m = atoi(l + 1);
        if (m >= 0 && m <= 15) g_ledMask = m;
    } else if (l[0] == 'Y') {
        int c = 0, e = 0;
        if (sscanf(l + 1, "%d,%d", &c, &e) == 2 && c >= 0 && c <= 4) { g_ledColor = c; g_ledEff = (e != 0); }
    } else if (l[0] == 'T') {
        long ep = 0; int tz = 0;
        if (sscanf(l + 1, "%ld,%d", &ep, &tz) == 2 && ep > 1700000000L) {
            struct timeval tv; tv.tv_sec = ep; tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            if (tz != g_tzMin) {
                g_tzMin = tz;
                Preferences pr; pr.begin("watch", false); pr.putInt("tz", tz); pr.end();
            }
        }
    } else if (l[0] == 'w') {
        String s(l + 1);
        int k = s.indexOf('\t');
        if (k > 0) {
            g_wifiSsid = s.substring(0, k); g_wifiPass = s.substring(k + 1); g_haveWifi = true;
            Preferences pr;   // luu Wi-Fi cua Rubi: dong ho tu kiem tra ban moi moi lan khoi dong
            pr.begin("watch", false);
            if (pr.getString("ssid", "") != g_wifiSsid || pr.getString("pass", "") != g_wifiPass) {
                pr.putString("ssid", g_wifiSsid); pr.putString("pass", g_wifiPass);
            }
            pr.end();
        }
    }
}

static void bleNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)d[i];
        if (c == '\n' || c == '\r') {
            if (g_inN > 0) { g_in[g_inN] = 0; xuLyDong(g_in); }
            g_inN = 0;
        } else if (g_inN < sizeof(g_in) - 1) {
            g_in[g_inN++] = c;
        }
    }
}

static bool bleConnect() {
    NimBLEScan* sc = NimBLEDevice::getScan();
    sc->setActiveScan(true);
    sc->setInterval(100);
    sc->setWindow(60);
    NimBLEScanResults rs = sc->start(3, false);
    NimBLEAdvertisedDevice found;
    bool ok = false;
    for (int i = 0; i < rs.getCount(); i++) {
        NimBLEAdvertisedDevice d = rs.getDevice(i);
        if (d.isAdvertisingService(NimBLEUUID(BLE_SVC))) { found = d; ok = true; break; }
    }
    sc->clearResults();
    if (!ok) return false;
    if (!g_cli) g_cli = NimBLEDevice::createClient();
    if (!g_cli->connect(&found)) return false;
    NimBLERemoteService* s = g_cli->getService(BLE_SVC);
    if (!s) { g_cli->disconnect(); return false; }
    g_rx = s->getCharacteristic(BLE_RX);
    g_tx = s->getCharacteristic(BLE_TX);
    if (!g_rx || !g_tx || !g_tx->canNotify()) { g_cli->disconnect(); return false; }
    g_inN = 0;
    g_tx->subscribe(true, bleNotify);
    std::string k = std::string("K") + WATCH_KEY;
    g_rx->writeValue((const uint8_t*)k.data(), k.size(), true);
    return true;
}

static void bleTask(void*) {
    NimBLEDevice::init("RubiWatch");
    for (;;) {
        if (g_otaMode) {
            if (!g_bleOff) {
                if (g_cli && g_cli->isConnected()) g_cli->disconnect();
                NimBLEDevice::deinit(true);
                g_cli = nullptr;
                g_bleOff = true;
            }
            g_bleConn = false;
            g_bleAuthed = false;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (g_cli == nullptr || !g_cli->isConnected()) {
            g_bleConn = false;
            g_bleAuthed = false;
            if (!bleConnect()) vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        g_bleConn = true;
        char m[32];
        if (xQueueReceive(g_txQ, m, pdMS_TO_TICKS(30)) == pdTRUE) {
            g_rx->writeValue((const uint8_t*)m, strlen(m), false);
        }
    }
}

// ---------------- Hong ngoai: phat ma 0x3C cho robot di theo ----------------
// Khung: mark 3000us, space 1500us, 8 bit ID, 8 bit ID dao, mark 600us. Bit: mark 600us + space 600 (0) / 1200 (1). Song mang 38 kHz.
static void irMark(uint32_t us)  { ledcWrite(IR_PIN, 85); delayMicroseconds(us); }
static void irSpace(uint32_t us) { ledcWrite(IR_PIN, 0);  delayMicroseconds(us); }
static void irByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) { irMark(600); irSpace(((b >> i) & 1) ? 1200 : 600); }
}
static void irTask(void*) {
    for (;;) {
        if (g_follow && !g_otaMode) {
            irMark(3000); irSpace(1500);
            irByte(IR_ID); irByte((uint8_t)~IR_ID);
            irMark(600);
            ledcWrite(IR_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(70));
        } else {
            ledcWrite(IR_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ---------------- Cam ung CST816D ----------------
static bool touchRaw(int& x, int& y) {
    Wire.beginTransmission(0x15);
    Wire.write(0x01);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(0x15, 6) != 6) return false;
    Wire.read();                       // gesture
    uint8_t n = Wire.read();
    uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
    if ((n & 0x0F) != 1) return false;          // dung 1 ngon tay; so khac la nhieu/rac
    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;
    if (x > 239 || y > 239) return false;       // toa do ngoai man hinh = doc rac
    if (TOUCH_SWAP_XY) { int t = x; x = y; y = t; }
    if (TOUCH_FLIP_X) x = 239 - x;
    if (TOUCH_FLIP_Y) y = 239 - y;
    return true;
}

// Su kien: 1 = cham xuong, 2 = keo, 3 = nha tay
static int g_tEvent = 0, g_tx0 = 0, g_ty0 = 0;
static bool g_touching = false;
static uint8_t g_miss = 0;
static uint32_t g_tLast = 0;

// Chip cam ung keo chan INT xuong moi khi co so lieu cham moi (lien tuc khi dang cham). Chi tin so lieu doc ra khi
// vua co xung INT: doc mu luc khong co INT co the ra so cu/rac (cham ao). Neu chua bao gio thay INT (day khong noi)
// thi doc nhu cu.
static volatile uint32_t g_tpIntAt = 0;
static volatile bool g_tpIntSeen = false;
static void IRAM_ATTR tpIsr() { g_tpIntAt = millis(); g_tpIntSeen = true; }

static void touchNoSleep() {
    for (int i = 0; i < 5; i++) {
        Wire.beginTransmission(0x15); Wire.write(0xFE); Wire.write(0x01);   // khong tu ngu
        if (Wire.endTransmission() == 0) break;
        delay(20);
    }
    Wire.beginTransmission(0x15); Wire.write(0xFA); Wire.write(0x60); Wire.endTransmission();   // INT: khi cham + khi doi
}

static void touchPoll() {
    uint32_t now = millis();
    if (now - g_tLast < 12) return;
    g_tLast = now;
    static uint32_t keepAwakeAt = 0;
    if (!g_touching && now - keepAwakeAt > 3000) {   // nhac chip cam ung dung tu ngu (ngu roi thi khong doc duoc)
        keepAwakeAt = now;
        Wire.beginTransmission(0x15); Wire.write(0xFE); Wire.write(0x01); Wire.endTransmission();
    }
    int x, y;
    bool t = touchRaw(x, y);
    if (t && g_tpIntSeen && millis() - g_tpIntAt > 80) t = false;   // khong co xung INT gan day: so cu/rac
    if (t) {
        g_miss = 0;
        g_tx0 = x; g_ty0 = y;
        g_tEvent = g_touching ? 2 : 1;
        g_touching = true;
    } else if (g_touching) {
        if (++g_miss >= 3) { g_touching = false; g_miss = 0; g_tEvent = 3; }
    }
}

// ---------------- Giao dien ----------------
enum { P_HOME, P_MENU, P_DRIVE, P_MUSIC, P_STORY, P_LEARN, P_SET, P_TOUCH, P_OTA, P_SET2, P_LED };
static int g_page = P_HOME;
static uint32_t g_lastTouchAt = 0;
static int g_bright = 255;
static int g_mIdx = 0, g_sIdx = 0;
static bool g_drvActive = false;
static int g_drvL = 0, g_drvR = 0, g_knobX = 120, g_knobY = 100;
static uint32_t g_drvSent = 0;
static uint32_t g_homeDownAt = 0;
static bool g_homeSwitched = false;
static bool g_battHold = false;
static int g_homeX0 = 0, g_homeY0 = 0;

// Pin dong ho: uoc luong theo thoi gian da chay ke tu moc "pin day" (khong do duoc dien ap that)
const float BATT_FULL_MIN = 90.0f;        // so phut (quy doi) chay duoc voi 1 lan sac day (pin 140 mAh) - chinh sau khi do thuc te
static float g_usedMin = 0;
static float g_battFull = BATT_FULL_MIN;   // dung luong that do dong ho tu hoc (phut quy doi), mac dinh BATT_FULL_MIN
static int battPct() {
    float p = 100.0f - g_usedMin * 100.0f / g_battFull;
    return p < 0 ? 0 : (int)p;
}
static void battSave() {
    Preferences pr;
    pr.begin("watch", false);
    pr.putFloat("used", g_usedMin);
    pr.end();
}
// Bam giu bieu tuong pin = da sac day: ve 100% va xoa co "pin da can"
static void battReset() {
    g_usedMin = 0;
    Preferences pr;
    pr.begin("watch", false);
    pr.putFloat("used", 0);
    pr.putBool("dead", false);
    pr.end();
}
// Luc khoi dong: doc luong pin da dung va TU HOC dung luong that.
// - SUT NGUON (pin can nen dien ap tut): thoi gian chay vua roi = dung luong that (chi tin neu hop ly, tranh sut do dong dot bien)
// - BAT NGUON ma da chay qua 100%: coi nhu pin da can (mach bao ve cat), hoc roi coi nhu da sac lai
// - BAT NGUON sau khi pin da can (co "dead"): coi nhu da sac, ve 100%
static void battBoot(esp_reset_reason_t rr) {
    Preferences pr;
    pr.begin("watch", false);
    g_battFull = pr.getFloat("bfull", BATT_FULL_MIN);
    g_usedMin = pr.getFloat("used", 0);
    bool dead = pr.getBool("dead", false);
    if (rr == ESP_RST_POWERON && dead) {
        g_usedMin = 0; pr.putFloat("used", 0); pr.putBool("dead", false);
    } else if (!dead && (rr == ESP_RST_BROWNOUT || (rr == ESP_RST_POWERON && g_usedMin >= g_battFull))) {
        float ran = g_usedMin + (rr == ESP_RST_BROWNOUT ? 2.5f : 0.0f);   // gia tri luu cham toi 5 phut
        if (ran >= 0.4f * g_battFull && ran <= 3.0f * g_battFull) {
            float nf = 0.4f * g_battFull + 0.6f * ran;
            g_battFull = nf < 30.0f ? 30.0f : (nf > 400.0f ? 400.0f : nf);
            pr.putFloat("bfull", g_battFull);
        }
        if (rr == ESP_RST_BROWNOUT) { g_usedMin = g_battFull; pr.putFloat("used", g_usedMin); pr.putBool("dead", true); }
        else { g_usedMin = 0; pr.putFloat("used", 0); }
    }
    pr.end();
    Serial.printf("pin: dung luong %.0f phut, da dung %.1f, %d%%\n", g_battFull, g_usedMin, battPct());
}

struct TileDef { const char* label; uint16_t color; };

static void tileRect(int i, int& x, int& y, int& w, int& h) {
    x = (i % 2) ? 132 : 52;
    y = 34 + (i / 2) * 60;
    w = 56; h = 56;
}

static void textCenter(const char* s, int cx, int cy, int size, uint16_t col) {
    int w = (int)strlen(s) * 6 * size, h = 8 * size;
    gfx->setTextSize(size);
    gfx->setTextColor(col);
    gfx->setCursor(cx - w / 2, cy - h / 2);
    gfx->print(s);
}

// Icon-only tile: icon != nullptr -> draw the web icon, else a colored tile
// with a simple code-drawn symbol (sym: '+', '-', 'S' stop, 'T' touch).
static void drawTile(int i, const uint16_t* icon, uint16_t color, char sym = 0, char badge = 0, bool on = false) {
    int x, y, w, h;
    tileRect(i, x, y, w, h);
    int cx = x + w / 2, cy = y + h / 2;
    if (icon) gfx->draw16bitRGBBitmap(x, y, icon, ICON_SIZE, ICON_SIZE);
    else {
        gfx->fillRoundRect(x, y, w, h, 11, color);
        if (sym == '+' || sym == '-') {
            gfx->fillRect(cx - 14, cy - 3, 28, 6, C_TXT);
            if (sym == '+') gfx->fillRect(cx - 3, cy - 14, 6, 28, C_TXT);
        } else if (sym == 'S') {
            gfx->fillRect(cx - 12, cy - 12, 24, 24, C_TXT);
        } else if (sym == 'N') {   // mui ten sang phai = trang ke tiep
            gfx->fillTriangle(cx - 8, cy - 14, cx - 8, cy + 14, cx + 12, cy, C_TXT);
        } else if (sym == 'B') {   // bong den
            gfx->fillCircle(cx, cy - 4, 12, C_TXT);
            gfx->fillRect(cx - 6, cy + 8, 12, 7, C_TXT);
        } else if (sym == 'T') {
            gfx->fillCircle(cx, cy, 14, C_TXT);
            gfx->fillCircle(cx, cy, 6, color);
        }
    }
    if (badge) {
        gfx->fillCircle(x + w - 10, y + h - 10, 11, C_BLUE);
        char b[2] = {badge, 0};
        textCenter(b, x + w - 10, y + h - 10, 2, C_TXT);
    }
    if (on) gfx->drawRoundRect(x - 2, y - 2, w + 4, h + 4, 12, C_OK);
}

static int tileHit(int n, int px, int py) {
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        tileRect(i, x, y, w, h);
        if (px >= x - 10 && px < x + w + 10 && py >= y - 2 && py < y + h + 2) return i;
    }
    return -1;
}

static void drawBack() {
    gfx->fillCircle(120, 224, 13, C_GREY);
    textCenter("<", 120, 224, 2, C_TXT);
}
static bool backHit(int px, int py) {
    int dx = px - 120, dy = py - 224;
    return dx * dx + dy * dy <= 22 * 22;
}

static void drawTitle(const char* t) {
    textCenter(t, 120, 22, 2, C_DIM);
}

static bool getTime(struct tm& tmv) {
    time_t t = time(nullptr);
    if (t < 1700000000) return false;
    t += (time_t)g_tzMin * 60;
    gmtime_r(&t, &tmv);
    return true;
}

// ---- Mat dong ho (6 anh JPEG trong faces.h) ----
static int g_face = 0;
static JPEGDEC g_jpeg;
static int jpegDraw(JPEGDRAW* p) {
    gfx->draw16bitBeRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
    return 1;
}
static void drawFaceImage(int idx) {
    if (idx < 0 || idx > 5) idx = 0;
    if (g_jpeg.openFLASH((uint8_t*)FACE_JPG[idx], (int)FACE_JPG_LEN[idx], jpegDraw)) {
        g_jpeg.setPixelType(RGB565_BIG_ENDIAN);
        g_jpeg.decode(0, 0, 0);
        g_jpeg.close();
    } else {
        gfx->fillScreen(C_BG);
    }
}
static uint16_t pillColor() {
    return gfx->color565(FACE_PILL[g_face][0], FACE_PILL[g_face][1], FACE_PILL[g_face][2]);
}
static uint16_t navyColor() { return gfx->color565(20, 30, 90); }

// Hai nut am luong tron tren mat dong ho (icon ve tay), goc tren trai/phai o gio
static const int VOL_CY = 84, VOL_LX = 33, VOL_RX = 207;
// Nut am luong tren mat kim: nho hon, dat o giua mat so (khong de len so)
static const int VOL_SMALL = 32, VOL_SY = 96, VOL_SLX = 86, VOL_SRX = 154;
static void drawVolIconSmall(Arduino_GFX* t, const uint16_t* ic, int cx) {
    int R = VOL_SMALL / 2;
    uint16_t row[VOL_SMALL];
    for (int dy = 0; dy < VOL_SMALL; dy++) {
        float d = dy + 0.5f - R;
        int hw = (int)sqrtf((float)(R * R) - d * d);
        if (hw <= 0) continue;
        int sy = dy * VOL_SIZE / VOL_SMALL;
        for (int dx = 0; dx < 2 * hw; dx++) row[dx] = ic[sy * VOL_SIZE + (R - hw + dx) * VOL_SIZE / VOL_SMALL];
        t->draw16bitRGBBitmap(cx - hw, VOL_SY - R + dy, row, hw * 2, 1);
    }
}
static void drawVolIcon(const uint16_t* ic, int cx, Arduino_GFX* t = nullptr) {
    if (!t) t = gfx;
    int R = VOL_SIZE / 2;
    for (int dy = 0; dy < VOL_SIZE; dy++) {
        float d = dy + 0.5f - R;
        int hw = (int)sqrtf((float)(R * R) - d * d);
        int off = R - hw;
        if (hw <= 0) continue;
        t->draw16bitRGBBitmap(cx - hw, VOL_CY - R + dy, (uint16_t*)(ic + dy * VOL_SIZE + off), hw * 2, 1);
    }
}
static void drawVolBtns(Arduino_GFX* t = nullptr) {
    if (FACE_ANALOG(g_face)) return;   // tren mat kim 2 nut nam san trong anh nen (xem analogFrame)
    drawVolIcon(VOL_DOWN, VOL_LX, t);
    drawVolIcon(VOL_UP, VOL_RX, t);
}
// -1 = giam, +1 = tang, 0 = khong trung nut
static int volHit(int x, int y) {
    if (FACE_ANALOG(g_face)) {
        int d2 = (y - VOL_SY) * (y - VOL_SY);
        if (d2 + (x - VOL_SLX) * (x - VOL_SLX) <= 22 * 22) return -1;
        if (d2 + (x - VOL_SRX) * (x - VOL_SRX) <= 22 * 22) return 1;
        return 0;
    }
    int dy = y - VOL_CY;
    if (dy * dy + (x - VOL_LX) * (x - VOL_LX) <= 27 * 27) return -1;
    if (dy * dy + (x - VOL_RX) * (x - VOL_RX) <= 27 * 27) return 1;
    return 0;
}

static void analogStatus();
static void drawHomeStatus() {
    if (FACE_ANALOG(g_face)) { analogStatus(); return; }
    uint16_t pc = pillColor();
    gfx->fillRect(44, 141, 152, 13, pc);
    {   // bieu tuong pin dong ho (4 vach), do khi con <= 20%
        int pct = battPct();
        int bars = pct <= 0 ? 0 : (pct + 24) / 25;
        uint16_t bc = (pct <= 20) ? C_BAD : navyColor();
        gfx->drawRect(58, 144, 20, 9, bc);
        gfx->fillRect(78, 146, 2, 5, bc);
        for (int i = 0; i < bars; i++) gfx->fillRect(60 + i * 4, 146, 3, 5, bc);
    }
    uint16_t dot = g_bleAuthed ? C_OK : (g_bleConn ? C_WARN : C_BAD);
    char b[16];
    b[0] = 0;
    if (g_robotPct >= 0) snprintf(b, sizeof(b), "Pin %d%%", g_robotPct);
    else if (g_follow) strcpy(b, "Di theo");
    int w = (int)strlen(b) * 6;
    int total = 10 + (w ? 6 + w : 0);
    int x0 = 134 - total / 2;
    gfx->fillCircle(x0 + 4, 148, 4, dot);
    if (w) {
        gfx->setTextSize(1);
        gfx->setTextColor(navyColor());
        gfx->setCursor(x0 + 16, 144);
        gfx->print(b);
    }
}

static void drawHomeTime(bool full);
static void drawHome();

// ---- Mat dong ho kim: nen la anh JPEG (khong co kim), kim ve bang so hoc vao tung dai 8 dong ----
// Khong dung canvas ca khung 115 KB (RAM lien khoi khong du). Chi giu ban sao vung giua 160x160 (51 KB) cua nen
// de ve kim len roi day ra man hinh 1 lan: khong nhap nhay. Cap nhat moi phut (khong co kim giay).
static const int DC_SZ = 160, DC_ORG = 40;   // vung giua: x,y tu 40 den 199
static uint16_t* g_dc = nullptr;              // nen vung giua, diem anh kieu big-endian (nhu JPEGDEC tra ve)
static int g_dcFace = -1;
static bool dcAlloc() {
    if (!g_dc) g_dc = (uint16_t*)malloc(DC_SZ * DC_SZ * 2);
    return g_dc != nullptr;
}
static int jpegDrawDial(JPEGDRAW* p) {
    gfx->draw16bitBeRGBBitmap(p->x, p->y, p->pPixels, p->iWidth, p->iHeight);
    if (g_dc) {
        for (int r = 0; r < p->iHeight; r++) {
            int y = p->y + r - DC_ORG;
            if (y < 0 || y >= DC_SZ) continue;
            for (int c = 0; c < p->iWidth; c++) {
                int x = p->x + c - DC_ORG;
                if (x < 0 || x >= DC_SZ) continue;
                g_dc[y * DC_SZ + x] = p->pPixels[r * p->iWidth + c];
            }
        }
    }
    return 1;
}
static inline uint16_t be565(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
// ve nut am luong nho (native RGB565 cua icon) vao ban sao nen
static void dcBlitVolSmall(const uint16_t* ic, int cx) {
    int R = VOL_SMALL / 2;
    for (int dy = 0; dy < VOL_SMALL; dy++) {
        float d = dy + 0.5f - R;
        int hw = (int)sqrtf((float)(R * R) - d * d);
        if (hw <= 0) continue;
        int sy = dy * VOL_SIZE / VOL_SMALL;
        int y = VOL_SY - R + dy - DC_ORG;
        if (y < 0 || y >= DC_SZ) continue;
        for (int dx = 0; dx < 2 * hw; dx++) {
            int x = cx - hw + dx - DC_ORG;
            if (x < 0 || x >= DC_SZ) continue;
            g_dc[y * DC_SZ + x] = be565(ic[sy * VOL_SIZE + (R - hw + dx) * VOL_SIZE / VOL_SMALL]);
        }
    }
}
// trang thai: pin dong ho + cham BLE + pin robot, tren nen xanh dam o phia tren giua mat
static void analogStatus() {
    uint16_t navy = gfx->color565(12, 24, 70), tick = gfx->color565(235, 240, 255);
    int pct = battPct(), bars = pct <= 0 ? 0 : (pct + 24) / 25;
    uint16_t bc = (pct <= 20) ? gfx->color565(255, 90, 90) : tick;
    int sy = 56;
    gfx->fillRoundRect(84, sy - 4, 72, 17, 8, navy);
    gfx->drawRect(88, sy, 20, 9, bc); gfx->fillRect(108, sy + 2, 2, 5, bc);
    for (int i = 0; i < bars; i++) gfx->fillRect(90 + i * 4, sy + 2, 3, 5, bc);
    uint16_t dot = g_bleAuthed ? C_OK : (g_bleConn ? C_WARN : C_BAD);
    gfx->fillCircle(126, sy + 4, 4, dot);
    if (g_robotPct >= 0) {
        char b[12]; snprintf(b, sizeof(b), "%d%%", g_robotPct);
        gfx->setTextSize(1); gfx->setTextColor(tick); gfx->setCursor(134, sy); gfx->print(b);
    }
}
static void dhPrep(DHand& h, float deg, int len, int tail, int hw) {
    float r = deg * 0.0174533f;
    h.ux = (int)lroundf(256.0f * sinf(r));
    h.uy = (int)lroundf(-256.0f * cosf(r));
    h.len = len; h.tail = tail; h.hw = hw;
}
static void analogFrame(bool full) {
    static int lastMin = -2;
    struct tm tmv;
    bool ok = getTime(tmv);
    int cur = ok ? tmv.tm_hour * 60 + tmv.tm_min : -1;
    if (!full && cur == lastMin) return;
    lastMin = cur;
    if (!g_dc) { g_face = 0; drawHome(); return; }
    if (full || g_dcFace != g_face) {
        int di = g_face - 6;
        if (di < 0 || di >= DIAL_COUNT) di = 0;
        if (g_jpeg.openFLASH((uint8_t*)DIAL_JPG[di], (int)DIAL_JPG_LEN[di], jpegDrawDial)) {
            g_jpeg.setPixelType(RGB565_BIG_ENDIAN);
            g_jpeg.decode(0, 0, 0);
            g_jpeg.close();
            dcBlitVolSmall(VOL_DOWN, VOL_SLX);
            dcBlitVolSmall(VOL_UP, VOL_SRX);
            g_dcFace = g_face;
            gfx->draw16bitBeRGBBitmap(DC_ORG, DC_ORG, g_dc, DC_SZ, DC_SZ);   // da kem 2 nut am luong
        } else {
            g_face = 0; drawHome(); return;
        }
    }
    DHand hh, mh;
    uint16_t cHourF = be565(gfx->color565(25, 40, 120)), cMinF = be565(gfx->color565(40, 70, 190));
    uint16_t cOut = be565(gfx->color565(255, 255, 255)), cGold = be565(gfx->color565(255, 210, 0)), cDark = be565(gfx->color565(12, 24, 70));
    if (ok) {
        dhPrep(hh, (tmv.tm_hour % 12) * 30.0f + tmv.tm_min * 0.5f, 46, 8, 4);
        dhPrep(mh, tmv.tm_min * 6.0f, 68, 10, 3);
    }
    uint16_t band[DC_SZ * 8];
    for (int y0 = 0; y0 < DC_SZ; y0 += 8) {
        memcpy(band, g_dc + y0 * DC_SZ, sizeof(band));
        if (ok) {
            for (int r = 0; r < 8; r++) {
                int dy = DC_ORG + y0 + r - 120;
                uint16_t* row = band + r * DC_SZ;
                for (int x = 0; x < DC_SZ; x++) {
                    int dx = DC_ORG + x - 120;
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= 9) { row[x] = cDark; continue; }
                    if (d2 <= 49) { row[x] = cGold; continue; }
                    int ma = (dx * mh.ux + dy * mh.uy) >> 8, mp = (dx * mh.uy - dy * mh.ux) >> 8;
                    if (mp < 0) mp = -mp;
                    if (ma >= -mh.tail && ma <= mh.len && mp <= mh.hw) { row[x] = cMinF; continue; }
                    int ha = (dx * hh.ux + dy * hh.uy) >> 8, hp = (dx * hh.uy - dy * hh.ux) >> 8;
                    if (hp < 0) hp = -hp;
                    if (ha >= -hh.tail && ha <= hh.len && hp <= hh.hw) { row[x] = cHourF; continue; }
                    if ((ma >= -mh.tail - 2 && ma <= mh.len + 2 && mp <= mh.hw + 2) ||
                        (ha >= -hh.tail - 2 && ha <= hh.len + 2 && hp <= hh.hw + 2)) row[x] = cOut;
                }
            }
        }
        gfx->draw16bitBeRGBBitmap(DC_ORG, DC_ORG + y0, band, DC_SZ, 8);
    }
    analogStatus();
}

static void drawHomeTime(bool full) {
    if (FACE_ANALOG(g_face)) { analogFrame(full); return; }
    static int lastMin = -2;
    struct tm tmv;
    bool ok = getTime(tmv);
    int cur = ok ? tmv.tm_hour * 60 + tmv.tm_min : -1;
    if (!full && cur == lastMin) return;
    lastMin = cur;
    gfx->fillRoundRect(36, 100, 168, 56, 12, pillColor());
    char b[8];
    if (ok) snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else strcpy(b, "--:--");
    textCenter(b, 120, 121, 5, navyColor());
    drawHomeStatus();
    drawVolBtns();
}

static void drawHome() {
    if (!FACE_ANALOG(g_face)) drawFaceImage(g_face);
    drawHomeTime(true);
}

// Man hinh luon hien gio (always on): nen den, chi co gio to mau xam va pin, den nen rat toi.
static bool g_aod = false;
static void drawAodTime(bool full) {
    static int lastMin = -2;
    struct tm tmv;
    bool ok = getTime(tmv);
    int cur = ok ? tmv.tm_hour * 60 + tmv.tm_min : -1;
    if (!full && cur == lastMin) return;
    lastMin = cur;
    char b[8];
    if (ok) snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    else strcpy(b, "--:--");
    gfx->fillRect(20, 85, 200, 60, C_BG);
    textCenter(b, 120, 112, 6, gfx->color565(200, 200, 210));
    gfx->fillRect(60, 160, 120, 14, C_BG);
    int pct = battPct();
    char p[12];
    snprintf(p, sizeof(p), "%d%%", pct < 0 ? 0 : pct);
    textCenter(p, 120, 166, 1, pct <= 20 ? C_BAD : C_DIM);
}
static void enterAod() {
    g_aod = true;
    g_bright = AOD_BL;
    gfx->fillScreen(C_BG);
    drawAodTime(true);
    setBacklight(AOD_BL);
}
static void leaveAod() {
    g_aod = false;
    g_bright = 255;
    drawHome();
    setBacklight(255);
}

// Doi mat xen ke: so, kim, so, kim... (trong moi loai di lan luot de thay du cac mat)
static int g_nextD = 0, g_nextA = 0;
static int nextFaceIdx() {
    if (!g_dc) {   // khong du RAM cho mat kim: chi xoay vong 6 mat so
        if (g_face < 0 || g_face >= 5) return 0;
        return g_face + 1;
    }
    if (FACE_ANALOG(g_face)) { int f = g_nextD; g_nextD = (g_nextD + 1) % 6; return f; }
    int f = 6 + g_nextA; g_nextA = (g_nextA + 1) % (NFACE - 6); return f;
}

static void prevFace() {   // vuot sang phai: ve mat truoc (chi xoay vong 6 mat so khi khong co mat kim)
    if (!g_dc) g_face = (g_face <= 0 || g_face > 5) ? 5 : g_face - 1;
    else g_face = nextFaceIdx();
    Preferences pr;
    pr.begin("watch", false);
    pr.putInt("face", g_face);
    pr.end();
    drawHome();
}

static void nextFace() {
    g_face = nextFaceIdx();
    Preferences pr;
    pr.begin("watch", false);
    pr.putInt("face", g_face);
    pr.end();
    drawHome();
}

// ---- Menu dang luoi tron (tong ong) kieu Apple Watch: keo ngon tay de truot, icon o giua to, ra mep nho dan ----
// Ve ca khung vao bo nho (canvas) roi day ra man hinh 1 lan => khong nhap nhay khi vuot.
enum { M_MUSIC, M_STORY, M_MATH, M_ENGLISH, M_DRIVE, M_DANCE, M_SLEEP, M_FOLLOW, M_LED, M_SET, M_COUNT };
static const uint16_t* const MENU_ICON[M_COUNT] = { ICON_MUSIC, ICON_STORY, ICON_LEARN, ICON_ENGLISH, ICON_DRIVE, ICON_DANCE, ICON_SLEEP, ICON_FOLLOW, ICON_LED, ICON_SET };
// vi tri tam moi icon (px, py) so voi tam luoi; hang le lech nua o => hinh tong ong
static const int16_t MENU_POS[M_COUNT][2] = {
    {0, 0},      // M_MUSIC (o giua)
    {64, 0},     // M_STORY
    {32, 55},    // M_MATH
    {-32, 55},   // M_ENGLISH
    {-64, 0},    // M_DRIVE
    {-32, -55},  // M_DANCE
    {0, 110},    // M_SLEEP
    {32, -55},   // M_FOLLOW
    {96, -55},   // M_LED
    {96, 55},    // M_SET
};
static int g_mox = 0, g_moy = 0;   // do lech luoi do ngon tay keo
static uint32_t g_menuOpenAt = 0;  // luc vua mo menu
static const int MENU_R = 30;      // ban kinh bong tron khi o giua
static void menuClampOff(int& ox, int& oy) {
    if (ox < -96) ox = -96;
    if (ox > 64) ox = 64;
    if (oy < -110) oy = -110;
    if (oy > 55) oy = 55;
}
static void menuLayout(int i, int& cx, int& cy, float& sc) {
    cx = 120 + MENU_POS[i][0] + g_mox;
    cy = 120 + MENU_POS[i][1] + g_moy;
    float dx = cx - 120, dy = cy - 120;
    float d = sqrtf(dx * dx + dy * dy);
    sc = d <= 60 ? 1.0f : 1.0f - 0.55f * (d - 60) / 65.0f;
    if (sc < 0.45f) sc = 0.45f;
}
// Ve tung dai 8 dong vao bo dem nho (3,8 KB) roi day ra man hinh: moi diem anh chi ghi 1 lan => khong nhap nhay,
// va khong can bo nho 115 KB cho ca khung (bo nho trong cua ESP32-C3 khong du khi dang chay BLE + Wi-Fi).
struct MenuBub { int cx, cy, R, half; uint16_t bgc; const uint16_t* ic; bool follow; uint8_t lut[64]; };
static uint16_t g_band[240 * 8];
static void menuDrawTiles() {
    const int BH = 8;
    uint16_t ring = 0xFFFF;
    MenuBub bub[M_COUNT];
    int order[M_COUNT]; float scs[M_COUNT];
    int cxs[M_COUNT], cys[M_COUNT];
    for (int i = 0; i < M_COUNT; i++) { order[i] = i; menuLayout(i, cxs[i], cys[i], scs[i]); }
    for (int a = 1; a < M_COUNT; a++)   // nho ve truoc, to ve sau (to de len tren)
        for (int b = a; b > 0 && scs[order[b - 1]] > scs[order[b]]; b--) { int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }
    int n = 0;
    for (int k = 0; k < M_COUNT; k++) {
        int i = order[k];
        MenuBub& m = bub[n];
        m.cx = cxs[i]; m.cy = cys[i];
        m.R = (int)(MENU_R * scs[i] + 0.5f);
        if (m.R < 4 || m.cx + m.R + 3 < 0 || m.cx - m.R - 3 >= 240 || m.cy + m.R + 3 < 0 || m.cy - m.R - 3 >= 240) continue;
        m.ic = MENU_ICON[i];
        m.bgc = m.ic[(ICON_SIZE / 2) * ICON_SIZE + 2];   // mau nen cua chinh icon do
        m.half = m.R - 2;   // tranh vien trang; anh icon (bo vien 4 diem moi canh) phu kin ca vong tron
        if (m.half < 2) m.half = 2;
        if (m.half > 31) m.half = 31;
        for (int d = 0; d < 2 * m.half; d++) m.lut[d] = (uint8_t)(4 + (d * (ICON_SIZE - 8)) / (2 * m.half));
        m.follow = (i == M_FOLLOW && g_follow);
        n++;
    }
    for (int y0 = 0; y0 < 240; y0 += BH) {
        for (int j = 0; j < 240 * BH; j++) g_band[j] = C_BG;
        for (int k = 0; k < n; k++) {
            const MenuBub& m = bub[k];
            int R = m.R, ro = m.follow ? R + 3 : R;
            if (y0 + BH - 1 < m.cy - ro || y0 > m.cy + ro) continue;
            int R2 = R * R, Ri2 = (R - 2) * (R - 2), Ro2 = ro * ro, Rg2 = (R + 1) * (R + 1);
            for (int r = 0; r < BH; r++) {
                int y = y0 + r, dy = y - m.cy;
                if (dy < -ro || dy > ro) continue;
                uint16_t* row = g_band + r * 240;
                int x0 = m.cx - ro, x1 = m.cx + ro;
                if (x0 < 0) x0 = 0;
                if (x1 > 239) x1 = 239;
                for (int x = x0; x <= x1; x++) {
                    int dx = x - m.cx;
                    int d2 = dx * dx + dy * dy;
                    if (d2 > Ro2) continue;
                    if (d2 > R2) { if (m.follow && d2 >= Rg2) row[x] = C_OK; continue; }
                    uint16_t col;
                    if (d2 >= Ri2) col = ring;
                    else if (dx >= -m.half && dx < m.half && dy >= -m.half && dy < m.half) col = m.ic[m.lut[dy + m.half] * ICON_SIZE + m.lut[dx + m.half]];
                    else col = m.bgc;
                    row[x] = col;
                }
            }
        }
        if (y0 + BH > 208) {   // nut quay lai (tron xam + dau <)
            for (int r = 0; r < BH; r++) {
                int y = y0 + r, dy = y - 224;
                if (dy < -13 || dy > 13) continue;
                uint16_t* row = g_band + r * 240;
                for (int x = 107; x <= 133; x++) {
                    int dx = x - 120;
                    if (dx * dx + dy * dy > 169) continue;
                    int ax = x - 114, ady = dy < 0 ? -dy : dy;
                    bool arrow = ady <= 7 && ax - ady >= -1 && ax - ady <= 1;
                    row[x] = arrow ? C_TXT : C_GREY;
                }
            }
        }
        gfx->draw16bitRGBBitmap(0, y0, g_band, 240, BH);
    }
}
static void drawMenu() {
    menuDrawTiles();
}
static int menuHit(int px, int py) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < M_COUNT; i++) {
        int cx, cy; float sc;
        menuLayout(i, cx, cy, sc);
        float dx = px - cx, dy = py - cy;
        float d = sqrtf(dx * dx + dy * dy);
        if (d <= MENU_R * sc + 6 && d < bd) { bd = d; best = i; }
    }
    return best;
}

static const uint8_t LED_RGB[5][3] = { {40,110,255}, {255,70,160}, {255,210,0}, {40,210,70}, {255,255,255} };
static void drawLedPage() {
    gfx->fillScreen(C_BG);
    drawTitle("DEN LED");
    drawTile(0, ICON_CHEST, 0, 0, 0, (g_ledMask & 1) != 0);
    drawTile(1, ICON_FENDER, 0, 0, 0, (g_ledMask & 6) != 0);
    drawTile(2, ICON_ANT, 0, 0, 0, (g_ledMask & 8) != 0);
    drawTile(3, ICON_COLOR, 0);
    int x, y, w, h; tileRect(3, x, y, w, h);
    uint16_t cc = gfx->color565(LED_RGB[g_ledColor][0], LED_RGB[g_ledColor][1], LED_RGB[g_ledColor][2]);
    if (g_ledColor == 4) { gfx->fillCircle(x + w - 9, y + h - 9, 10, C_TXT); gfx->fillCircle(x + w - 9, y + h - 9, 7, C_PURPLE); }
    else { gfx->fillCircle(x + w - 9, y + h - 9, 10, C_TXT); gfx->fillCircle(x + w - 9, y + h - 9, 7, cc); }
    drawTile(4, ICON_BLINK, 0, 0, 0, g_ledEff != 0);
    drawTile(5, nullptr, (g_ledMask & 15) ? C_ORANGE : C_GREY, 'B', 0, (g_ledMask & 15) != 0);   // tat/bat het
    drawBack();
}

static void drawLearn() {
    gfx->fillScreen(C_BG);
    drawTitle("HOC");
    drawTile(0, ICON_LEARN, 0, 0, '1');
    drawTile(1, ICON_LEARN, 0, 0, '2');
    drawTile(2, ICON_LEARN, 0, 0, '3');
    drawTile(3, ICON_LEARN, 0, 0, '4');
    drawTile(4, nullptr, C_BAD, 'S');
    drawBack();
}

static void drawSet() {
    gfx->fillScreen(C_BG);
    drawTitle("CAI DAT");
    drawTile(0, ICON_UPDATE, 0);
    drawTile(1, nullptr, C_GREY, 'T');                                              // thu cam ung
    drawBack();
}

static void drawKnob(bool erase) {
    gfx->fillCircle(g_knobX, g_knobY, 16, erase ? C_BG : C_BLUE);
}
static void drawDrive() {
    gfx->fillScreen(C_BG);
    gfx->drawCircle(120, 100, 72, C_DIM);
    gfx->drawCircle(120, 100, 71, C_DIM);
    g_knobX = 120; g_knobY = 100;
    drawKnob(false);
    gfx->fillRoundRect(70, 182, 100, 24, 8, C_BAD);
    textCenter("DUNG", 120, 194, 2, C_TXT);
    drawBack();
}

static void drawPicker(const char* title, const char* what, int idx, int n, uint16_t color) {
    gfx->fillScreen(C_BG);
    drawTitle(title);
    char b[24];
    if (n > 0) snprintf(b, sizeof(b), "%s %d/%d", what, idx + 1, n);
    else snprintf(b, sizeof(b), "Chua co %s", what);
    textCenter(b, 120, 70, 2, C_TXT);
    gfx->fillCircle(60, 125, 26, C_GREY);   textCenter("<", 60, 125, 3, C_TXT);
    gfx->fillCircle(120, 125, 30, color);   textCenter("MO", 120, 125, 2, C_TXT);
    gfx->fillCircle(180, 125, 26, C_GREY);  textCenter(">", 180, 125, 3, C_TXT);
    gfx->fillRoundRect(70, 168, 100, 28, 8, C_BAD);
    textCenter("DUNG", 120, 182, 2, C_TXT);
    drawBack();
}

static void drawTouchTest() {
    gfx->fillScreen(C_BG);
    textCenter("CHAM VAO MAN HINH", 120, 30, 1, C_DIM);
    drawBack();
}

static void drawPage() {
    switch (g_page) {
        case P_HOME:  drawHome(); break;
        case P_MENU:  drawMenu(); break;
        case P_DRIVE: drawDrive(); break;
        case P_MUSIC: drawPicker("NHAC", "bai", g_mIdx, g_musicN, C_ORANGE); break;
        case P_STORY: drawPicker("TRUYEN", "truyen", g_sIdx, g_storyN, C_TEAL); break;
        case P_LEARN: drawLearn(); break;
        case P_SET:   drawSet(); break;
        case P_LED:   drawLedPage(); break;
        case P_TOUCH: drawTouchTest(); break;
    }
}

static void gotoPage(int p) {
    if (g_aod) { g_aod = false; g_bright = 255; setBacklight(255); }
    g_page = p;
    g_drvActive = false;
    drawPage();
    if (p == P_MENU) g_menuOpenAt = millis();
}

// ---------------- Cap nhat OTA ----------------
static void otaMsg(const char* a, const char* b = "", uint16_t col = 0) {
    if (col == 0) col = C_TXT;
    gfx->fillRect(10, 90, 220, 70, C_BG);
    textCenter(a, 120, 105, 2, col);
    if (b[0]) textCenter(b, 120, 135, 1, C_DIM);
}

static String jsonGet(const String& s, const char* key) {
    String k = String("\"") + key + "\"";
    int i = s.indexOf(k);
    if (i < 0) return "";
    i = s.indexOf(':', i + k.length());
    if (i < 0) return "";
    i++;
    while (i < (int)s.length() && (s[i] == ' ' || s[i] == '"')) i++;
    int j = i;
    while (j < (int)s.length() && s[j] != '"' && s[j] != ',' && s[j] != '}' && s[j] != '\n' && s[j] != '\r') j++;
    String v = s.substring(i, j);
    v.trim();
    return v;
}

static void otaFinish(const char* a, const char* b) {
    otaMsg(a, b, C_BAD);
    delay(4000);
    ESP.restart();                      // khoi dong lai de Bluetooth chay lai
}

static void otaInstall(const String& ssid, const String& pass, bool quiet);
static void otaRun() {
    g_page = P_OTA;
    g_follow = false;
    gfx->fillScreen(C_BG);
    drawTitle("CAP NHAT");
    uint32_t t0 = millis();
    String ssid, pass;
    if (g_bleAuthed) {                  // dang noi Rubi: xin Wi-Fi moi nhat cua Rubi
        otaMsg("Xin Wi-Fi tu Rubi...");
        g_haveWifi = false;
        sendCmd("W");
        while (!g_haveWifi && millis() - t0 < 4000) delay(50);
        if (g_haveWifi) { ssid = g_wifiSsid; pass = g_wifiPass; }
    }
    if (ssid.length() == 0) {           // khong co Rubi: dung Wi-Fi da luu, chua co thi Wi-Fi nha
        Preferences pr;
        pr.begin("watch", true);
        ssid = pr.getString("ssid", ""); pass = pr.getString("pass", "");
        pr.end();
        if (ssid.length() == 0) { ssid = DEFAULT_WIFI_SSID; pass = DEFAULT_WIFI_PASS; }
    }
    g_otaMode = true;                   // tat Bluetooth de du bo nho cho Wi-Fi
    t0 = millis();
    while (!g_bleOff && millis() - t0 < 4000) delay(50);
    otaInstall(ssid, pass, false);
}

// Tu cap nhat: noi Wi-Fi, hoi GitHub, co ban moi thi tai, nap va khoi dong lai.
// quiet = true (luc khoi dong): khong co ban moi / loi thi tat Wi-Fi va tra ve, khong khoi dong lai.
static bool g_otaQuiet = false;
static void otaFail(const char* a, const char* b) {
    if (g_otaQuiet) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); return; }
    otaFinish(a, b);
}
static void otaInstall(const String& ssid, const String& pass, bool quiet) {
    g_otaQuiet = quiet;
    uint32_t t0;
    if (!quiet) otaMsg("Noi Wi-Fi...", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < (quiet ? 8000u : 20000u)) delay(200);
    if (WiFi.status() != WL_CONNECTED) { otaFail("Khong noi duoc Wi-Fi", ssid.c_str()); return; }
    if (time(nullptr) < 1700000000) {   // lay gio tu Internet luon (khong can doi Rubi)
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        uint32_t tn = millis();
        while (time(nullptr) < 1700000000 && millis() - tn < 4000) delay(100);
    }

    if (!quiet) otaMsg("Hoi GitHub...");
    String body;
    {
        WiFiClientSecure cl;
        cl.setInsecure();
        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        if (!http.begin(cl, OTA_MANIFEST_URL)) { otaFail("Loi dia chi", ""); return; }
        int code = http.GET();
        if (code != 200) { char b[24]; snprintf(b, sizeof(b), "ma %d", code); http.end(); otaFail("Chua co ban moi", b); return; }
        body = http.getString();
        http.end();
    }
    String ver = jsonGet(body, "version"), url = jsonGet(body, "url"), md5 = jsonGet(body, "md5");
    if (ver.length() == 0 || url.length() == 0) { otaFail("Manifest loi", ""); return; }
    if (ver == FW_VERSION) {
        if (quiet) { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); return; }
        otaMsg("Da la ban moi nhat", ver.c_str(), C_OK); delay(3000); ESP.restart(); return;
    }
    if (quiet) {   // co ban moi: hien man hinh cap nhat
        g_page = P_OTA;
        setBacklight(255);
        gfx->fillScreen(C_BG);
        drawTitle("CAP NHAT");
    }

    otaMsg("Tai ban moi", ver.c_str());
    WiFiClientSecure cl2;
    cl2.setInsecure();
    HTTPClient h2;
    h2.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    h2.setTimeout(15000);
    if (!h2.begin(cl2, url)) { otaFail("Loi dia chi file", ""); return; }
    int code2 = h2.GET();
    int len = h2.getSize();
    if (code2 != 200 || len <= 0) { h2.end(); otaFail("Loi tai file", ""); return; }
    if (!Update.begin(len)) { h2.end(); otaFail("Khong du cho nap", Update.errorString()); return; }
    if (md5.length() == 32) Update.setMD5(md5.c_str());
    WiFiClient* st = h2.getStreamPtr();
    uint8_t buf[1024];
    int got = 0, lastPct = -10;
    uint32_t lastData = millis();
    bool loi = false;
    while (got < len) {
        size_t avail = st->available();
        if (avail) {
            size_t n = avail > sizeof(buf) ? sizeof(buf) : avail;
            int r = st->readBytes(buf, n);
            if (r <= 0) continue;
            if (Update.write(buf, r) != (size_t)r) { loi = true; break; }
            got += r;
            lastData = millis();
            int pct = (int)((int64_t)got * 100 / len);
            if (pct >= lastPct + 5) {
                lastPct = pct;
                char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
                gfx->fillRect(60, 150, 120, 30, C_BG);
                textCenter(b, 120, 165, 3, C_OK);
            }
        } else {
            if (!h2.connected()) break;
            if (millis() - lastData > 20000) { loi = true; break; }
            delay(2);
        }
    }
    h2.end();
    if (loi || got != len) { Update.abort(); otaFail("Tai loi, giu ban cu", ""); return; }
    if (!Update.end(true)) { otaFail("Nap loi, giu ban cu", Update.errorString()); return; }
    otaMsg("Xong! Khoi dong lai", ver.c_str(), C_OK);
    delay(1500);
    ESP.restart();
}

// ---------------- Xu ly cham tung trang ----------------
static void onTouch(int ev, int x, int y) {
    switch (g_page) {
        case P_HOME:
            if (ev == 1 && volHit(x, y) != 0) {   // cham nut am luong: khong mo menu, khong doi mat
                sendCmd(volHit(x, y) > 0 ? "Cvup" : "Cvdown");
                g_homeDownAt = 0;
                break;
            }
            if (ev == 1) { g_homeDownAt = millis() | 1; g_homeSwitched = false; g_battHold = (x >= 44 && x <= 92 && y >= 134 && y <= 160); g_homeX0 = x; g_homeY0 = y; }
            else if (ev == 2 && g_homeDownAt != 0 && !g_homeSwitched) {
                // vuot ngang tren mat dong ho: doi mat (trai = mat ke tiep, phai = mat truoc); khong mo menu
                int dx = x - g_homeX0, dy = y - g_homeY0;
                if (abs(dx) > 40 && abs(dx) > 2 * abs(dy)) {
                    g_homeSwitched = true;
                    if (dx < 0) nextFace(); else prevFace();
                }
            }
            else if (ev == 3 && g_homeDownAt != 0) {
                if (!g_homeSwitched) { g_mox = 0; g_moy = 0; gotoPage(P_MENU); }   // cham (nhanh hay cham giu deu) = mo menu; mat dong ho tu doi moi 5 phut; chi giu 2 giay o bieu tuong pin la khong mo menu
                g_homeDownAt = 0;
                g_homeSwitched = false;
            }
            break;
        case P_MENU: {
            static int downX, downY, startX, startY, pnx, pny; static bool moved, tracking; static uint32_t lastDraw, pressAt;
            if (ev == 1) {
                if (backHit(x, y)) { tracking = false; gotoPage(P_HOME); break; }
                // 0,4 giay dau sau khi mo menu bo qua cham: tranh cham doi/nay tay tu man hinh chinh roi trung icon o giua
                if (millis() - g_menuOpenAt < 400) { tracking = false; break; }
                downX = x; downY = y; startX = g_mox; startY = g_moy; pnx = g_mox; pny = g_moy;
                moved = false; tracking = true; lastDraw = 0; pressAt = millis();
                break;
            }
            if (!tracking) break;
            if (ev == 2) {   // chi ev 2 moi tinh la keo (luc nha tay toa do hay troi)
                int dx = x - downX, dy = y - downY;
                if (!moved && (abs(dx) > 8 || abs(dy) > 8)) moved = true;
                if (moved) {
                    int nx = startX + dx, ny = startY + dy;
                    menuClampOff(nx, ny);
                    pnx = nx; pny = ny;
                    if ((nx != g_mox || ny != g_moy) && millis() - lastDraw > 40) {
                        g_mox = nx; g_moy = ny; menuDrawTiles(); lastDraw = millis();
                    }
                }
                break;
            }
            if (ev == 3) {
                tracking = false;
                if (moved) {
                    if (pnx != g_mox || pny != g_moy) { g_mox = pnx; g_moy = pny; menuDrawTiles(); }
                    break;
                }
                if (millis() - pressAt < 40) break;   // cham qua ngan = nhieu/cham ao
                switch (menuHit(downX, downY)) {
                    case M_MUSIC: gotoPage(P_MUSIC); break;
                    case M_STORY: gotoPage(P_STORY); break;
                    case M_MATH: gotoPage(P_LEARN); break;
                    case M_ENGLISH: sendCmd("Cen"); break;
                    case M_DRIVE: gotoPage(P_DRIVE); break;
                    case M_DANCE: sendCmd("A"); break;
                    case M_SLEEP: sendCmd("Csleep"); break;
                    case M_FOLLOW: g_follow = !g_follow; menuDrawTiles(); break;
                    case M_LED: gotoPage(P_LED); break;
                    case M_SET: gotoPage(P_SET); break;
                }
            }
            break;
        }
        case P_LEARN:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            switch (tileHit(6, x, y)) {
                case 0: sendCmd("Cm0"); break;
                case 1: sendCmd("Cm1"); break;
                case 2: sendCmd("Cm2"); break;
                case 3: sendCmd("Cm3"); break;
                case 4: sendCmd("Chuy"); break;
            }
            break;
        case P_LED:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            switch (tileHit(6, x, y)) {
                case 0: g_ledMask ^= 1; sendCmdf("E%d", g_ledMask); drawLedPage(); break;
                case 1: g_ledMask = (g_ledMask & 6) ? (g_ledMask & ~6) : (g_ledMask | 6); sendCmdf("E%d", g_ledMask); drawLedPage(); break;
                case 2: g_ledMask ^= 8; sendCmdf("E%d", g_ledMask); drawLedPage(); break;
                case 3: g_ledColor = (g_ledColor + 1) % 5; sendCmdf("Y%d,%d", g_ledColor, g_ledEff); drawLedPage(); break;
                case 4: g_ledEff = g_ledEff ? 0 : 1; sendCmdf("Y%d,%d", g_ledColor, g_ledEff); drawLedPage(); break;
                case 5: g_ledMask = (g_ledMask & 15) ? 0 : 15; sendCmdf("E%d", g_ledMask); drawLedPage(); break;
            }
            break;
        case P_SET:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            switch (tileHit(6, x, y)) {
                case 0: otaRun(); break;
                case 1: gotoPage(P_TOUCH); break;
            }
            break;
        case P_MUSIC:
        case P_STORY: {
            if (ev != 1) break;
            bool music = (g_page == P_MUSIC);
            int n = music ? g_musicN : g_storyN;
            int& idx = music ? g_mIdx : g_sIdx;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            if (y >= 168 && y < 196 && x >= 70 && x < 170) { sendCmd("Chuy"); break; }
            int dx = x - 60, dy = y - 125;
            if (dx * dx + dy * dy <= 30 * 30 && n > 0) { idx = (idx + n - 1) % n; drawPage(); break; }
            dx = x - 180;
            if (dx * dx + dy * dy <= 30 * 30 && n > 0) { idx = (idx + 1) % n; drawPage(); break; }
            dx = x - 120;
            if (dx * dx + dy * dy <= 34 * 34 && n > 0) {
                if (music) sendCmdf("Cp%d", idx);
                else sendCmdf("T%d", idx);
            }
            break;
        }
        case P_DRIVE: {
            if (ev == 1 && backHit(x, y)) { sendCmd("X"); gotoPage(P_MENU); break; }
            if (ev == 1 && y >= 182 && y < 206 && x >= 70 && x < 170) { sendCmd("X"); break; }
            if (ev == 3) {
                if (g_drvActive) { g_drvActive = false; sendCmd("L0,0"); sendCmd("X"); drawKnob(true); g_knobX = 120; g_knobY = 100; drawKnob(false); }
                break;
            }
            int dx = x - 120, dy = y - 100;
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (ev == 1 && d > 80) break;               // cham ngoai vong thi bo qua
            if (!g_drvActive && ev == 2) break;         // keo tu ngoai vao
            const float R = 72.0f;
            if (d > R) { dx = (int)(dx * R / d); dy = (int)(dy * R / d); d = R; }
            drawKnob(true);
            gfx->drawCircle(120, 100, 72, C_DIM);
            gfx->drawCircle(120, 100, 71, C_DIM);
            g_knobX = 120 + dx; g_knobY = 100 + dy;
            drawKnob(false);
            g_drvActive = true;
            float fwd = -dy / R, turn = dx / R;         // len = tien
            if (fabsf(fwd) < 0.12f) fwd = 0;
            int l = (int)((fwd + turn) * 100), r = (int)((fwd - turn) * 100);
            l = constrain(l, -100, 100); r = constrain(r, -100, 100);
            g_drvL = l * DRIVE_LIMIT / 100;
            g_drvR = r * DRIVE_LIMIT / 100;
            break;
        }
        case P_TOUCH:
            if (ev == 1 && backHit(x, y)) { gotoPage(P_SET); break; }
            if (ev == 1 || ev == 2) {
                gfx->fillRect(40, 190, 160, 14, C_BG);
                gfx->fillCircle(x, y, 6, C_WARN);
                char b[24]; snprintf(b, sizeof(b), "x=%d y=%d", x, y);
                textCenter(b, 120, 197, 1, C_TXT);
            }
            break;
    }
}

// ---------------- Khoi dong ----------------
// Boot diagnosis: the reason of the last reset is shown under the version on the splash screen, and after
// 3 crash/brown-out resets in a row the watch starts without Bluetooth (safe mode) so it stays usable and can
// still be updated (OTA). The counter clears after 30 s of normal running.
RTC_NOINIT_ATTR static uint32_t g_bootMagic;
RTC_NOINIT_ATTR static uint32_t g_crashCount;
static bool g_safeMode = false;
static const char* resetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "BAT NGUON";
        case ESP_RST_SW:       return "KHOI DONG LAI";
        case ESP_RST_PANIC:    return "LOI CHUONG TRINH";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "TREO (WDT)";
        case ESP_RST_BROWNOUT: return "SUT NGUON";
        case ESP_RST_USB:      return "USB";
        case ESP_RST_DEEPSLEEP: return "NGU SAU";
        default:               return "KHAC";
    }
}

void setup() {
    Serial.begin(115200);
    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
    if (g_bootMagic != 0x52554249u) { g_bootMagic = 0x52554249u; g_crashCount = 0; }
    g_crashCount = crashed ? g_crashCount + 1 : 0;
    g_safeMode = g_crashCount >= 3;
    Serial.printf("reset reason %d (%s), crashes in a row %u\n", (int)rr, resetName(rr), (unsigned)g_crashCount);
    g_txQ = xQueueCreate(8, 32);

    ledcAttach(TFT_BL, 5000, 8);
    setBacklight(0);

    ledcAttach(IR_PIN, 38000, 8);
    ledcWrite(IR_PIN, 0);

    Wire.begin(TP_SDA, TP_SCL);
    Wire.setClock(400000);
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);  delay(10);
    digitalWrite(TP_RST, HIGH); delay(150);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), tpIsr, FALLING);
    touchNoSleep();   // khong tu ngu

    gfx->begin();
    C_BG = gfx->color565(0, 0, 0);
    C_TXT = gfx->color565(255, 255, 255);
    C_DIM = gfx->color565(150, 150, 160);
    C_OK = gfx->color565(60, 220, 100);
    C_WARN = gfx->color565(255, 190, 40);
    C_BAD = gfx->color565(220, 50, 50);
    C_BLUE = gfx->color565(40, 120, 230);
    C_GREEN = gfx->color565(40, 160, 90);
    C_ORANGE = gfx->color565(230, 120, 30);
    C_PURPLE = gfx->color565(140, 80, 200);
    C_TEAL = gfx->color565(30, 150, 160);
    C_GREY = gfx->color565(70, 75, 85);
    // Xin 51 KB cho nen mat dong ho kim NGAY LUC DAU (truoc khi Wi-Fi/Bluetooth lam RAM manh mun)
    static bool cvOk = false;
    cvOk = dcAlloc();
    gfx->fillScreen(C_BG);
    textCenter("RUBI", 120, 88, 4, C_BLUE);
    textCenter("WATCH", 120, 122, 3, C_BLUE);
    {
        char hb[32];
        snprintf(hb, sizeof(hb), "RAM %uK %s", (unsigned)(ESP.getFreeHeap() / 1024), cvOk ? "kim OK" : "kim tat");
        textCenter(hb, 120, 224, 1, C_DIM);
    }
    textCenter(FW_VERSION, 120, 140, 1, C_DIM);
    {
        char b[40];
        snprintf(b, sizeof(b), "%s %u", resetName(rr), (unsigned)g_crashCount);
        textCenter(b, 120, 165, 1, crashed ? C_BAD : C_DIM);
        if (g_safeMode) textCenter("AN TOAN: TAT BLUETOOTH", 120, 185, 1, C_WARN);
    }
    setBacklight(255);
    delay(crashed ? 2500 : 800);

    {   // Chua co gio that (vua reset/mat nguon): dung gio luu lan truoc de man hinh khong trong, se duoc Wi-Fi/Bluetooth chinh lai
        Preferences pr;
        pr.begin("watch", true);
        uint32_t lt = pr.getUInt("lastT", 0);
        pr.end();
        if (lt > 1700000000UL && time(nullptr) < 1700000000L) {
            struct timeval tv = { (time_t)lt, 0 };
            settimeofday(&tv, nullptr);
        }
    }
    {
        Preferences pr;
        pr.begin("watch", true);
        String ss = pr.getString("ssid", ""), pw = pr.getString("pass", "");
        pr.end();
        // Chua xin duoc Wi-Fi tu Rubi: dung Wi-Fi nha (khong mat khau). Khi noi Bluetooth voi Rubi, Wi-Fi cua Rubi
        // duoc luu de va dung tu lan sau.
        if (ss.length() == 0) { ss = DEFAULT_WIFI_SSID; pw = DEFAULT_WIFI_PASS; }
        if (ss.length() > 0) {
            textCenter("Kiem tra ban moi...", 120, 205, 1, C_DIM);
            otaInstall(ss, pw, true);   // co ban moi thi tu nap va khoi dong lai; khong thi di tiep
            gfx->fillRect(0, 196, 240, 18, C_BG);
        }
    }
    textCenter("BLE...", 120, 205, 1, C_DIM);
    if (!g_safeMode) xTaskCreate(bleTask, "ble", 12288, nullptr, 1, nullptr);
    delay(crashed ? 1500 : 0);
    xTaskCreate(irTask, "ir", 2048, nullptr, 2, nullptr);

    {
        Preferences pr;
        pr.begin("watch", true);
        g_face = pr.getInt("face", 0);
        g_tzMin = pr.getInt("tz", g_tzMin);
        pr.end();
        battBoot(rr);
        if (g_face < 0 || g_face >= NFACE) g_face = 0;
    }
    g_lastTouchAt = millis();
    gotoPage(P_HOME);
}

void loop() {
    uint32_t now = millis();
    if (g_crashCount != 0 && now > 30000) g_crashCount = 0;   // ran 30 s fine: not a crash loop
    touchPoll();
    static bool ignoreUntilUp = false;
    if (g_tEvent) {
        int ev = g_tEvent;
        g_tEvent = 0;
        bool sleeping = g_aod || (g_bright < 255);
        g_lastTouchAt = now;
        if (g_aod) leaveAod();                            // cham 1: sang man hinh, hien hinh nen; cham 2: vao menu
        else if (sleeping) { g_bright = 255; setBacklight(255); }
        if (sleeping && ev == 1) ignoreUntilUp = true;   // cham dau tien luc dang ngu chi de danh thuc
        if (ignoreUntilUp) { if (ev == 3) ignoreUntilUp = false; }
        else onTouch(ev, g_tx0, g_ty0);
#if TOUCH_DEBUG
        if (g_page == P_HOME && !g_aod) {   // tam thoi: hien cham o dau de chan doan
            char b[24];
            snprintf(b, sizeof(b), "ev%d %d,%d", ev, g_tx0, g_ty0);
            gfx->fillRect(78, 44, 84, 10, C_BG);
            textCenter(b, 120, 49, 1, C_WARN);
            if (ev != 3) gfx->fillCircle(g_tx0, g_ty0, 4, C_BAD);
        }
#endif
    }
    // giu lau tren man hinh chinh: doi mat dong ho
    if (g_page == P_HOME && !g_aod && g_touching && g_homeDownAt != 0 && !g_homeSwitched && !ignoreUntilUp && g_battHold && now - g_homeDownAt >= 2000u) {
        g_homeSwitched = true;
        battReset(); drawHomeStatus();   // giu lau len bieu tuong pin = da sac day
    }
    // luu gio hien tai moi 5 phut de sau khi reset van con gio gan dung
    static uint32_t timeSaveAt = 0;
    if (now - timeSaveAt >= 300000) {
        timeSaveAt = now;
        time_t tt = time(nullptr);
        if (tt > 1700000000L) {
            Preferences pr;
            pr.begin("watch", false);
            pr.putUInt("lastT", (uint32_t)tt);
            pr.end();
        }
    }
    // tinh luong pin da dung moi phut (man hinh sang=1, mo=0,5, tat=0,2)
    static uint32_t battAt = 0;
    static int battN = 0;
    if (now - battAt >= 60000) {
        battAt = now;
        g_usedMin += (g_bright >= 255) ? 1.0f : (g_bright > 40 ? 0.5f : 0.3f);
        if (g_bleConn) g_usedMin += 0.1f;     // radio Bluetooth dang ket noi
        if (g_otaMode) g_usedMin += 0.6f;     // Wi-Fi dang bat (cap nhat) ton nhieu dien
        if (++battN % 5 == 0) battSave();
    }
    // gui lenh lai xe lien tuc khi dang keo
    if (g_page == P_DRIVE && g_drvActive && now - g_drvSent > 120) {
        g_drvSent = now;
        char m[24];
        snprintf(m, sizeof(m), "L%d,%d", g_drvL, g_drvR);
        sendCmd(m);
    }
    // cap nhat man hinh chinh moi giay
    static uint32_t homeAt = 0;
    if (g_page == P_HOME && g_aod && now - homeAt > 1000) {
        homeAt = now;
        drawAodTime(false);
    }
    // tu doi mat dong ho moi 5 phut (luc dang toi thi lan sang sau se thay mat moi)
    static uint32_t faceAt = 0;
    if (now - faceAt > FACE_ROTATE_MS) {
        if (faceAt != 0) {
            g_face = nextFaceIdx();
            if (g_page == P_HOME && !g_aod) drawHome();
        }
        faceAt = now;
    }
    if (g_page == P_HOME && !g_aod && now - homeAt > 1000) {
        homeAt = now;
        drawHomeTime(false);
        static int lastState = -1;
        int st = g_bleAuthed ? 2 : g_bleConn ? 1 : 0;
        static int lastPct = -2, lastBatt = -2;
        int bp = battPct();
        if (st != lastState || g_robotPct != lastPct || bp != lastBatt) { lastState = st; lastPct = g_robotPct; lastBatt = bp; drawHomeStatus(); }
    }
    // 15 giay khong cham o menu/trang con thi tu quay ve mat dong ho (khong ap dung luc dang keo lai xe, luc cap nhat)
    if (g_page != P_HOME && g_page != P_OTA && !g_drvActive && now - g_lastTouchAt > MENU_IDLE_MS) {
        if (g_page == P_DRIVE) sendCmd("X");
        gotoPage(P_HOME);
    }
    // giam sang / tat man hinh khi khong cham (khong dung o trang lai xe dang keo)
    // vang tay 20 giay o mat dong ho: chuyen sang man hinh toi chi hien gio (khong tat han)
    if (g_page == P_HOME && !g_aod && !g_touching && now - g_lastTouchAt > AOD_MS) enterAod();
    delay(2);
}
