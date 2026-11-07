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
#include "faces.h"
#include "icons.h"

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
const uint32_t OFF_MS     = 60000;       // vang tay sau bay nhieu ms thi tat man hinh (cham la sang lai)
const char* WATCH_KEY     = "RB4821WT";  // phai trung WATCH_KEY trong firmware DevKit
const char* FW_VERSION    = "dev";       // GitHub Actions doi thanh 1.0.<so lan build>
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
int g_tzMin = 420;
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
    } else if (l[0] == 'S') {
        int p = -1, m = 0, t = 0, w = 0;
        if (sscanf(l + 1, "%d,%d,%d,%d", &p, &m, &t, &w) == 4) { g_robotPct = p; g_musicN = m; g_storyN = t; g_robotWifi = w; }
    } else if (l[0] == 'T') {
        long ep = 0; int tz = 0;
        if (sscanf(l + 1, "%ld,%d", &ep, &tz) == 2 && ep > 1700000000L) {
            struct timeval tv; tv.tv_sec = ep; tv.tv_usec = 0;
            settimeofday(&tv, nullptr);
            g_tzMin = tz;
        }
    } else if (l[0] == 'w') {
        String s(l + 1);
        int k = s.indexOf('\t');
        if (k > 0) { g_wifiSsid = s.substring(0, k); g_wifiPass = s.substring(k + 1); g_haveWifi = true; }
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
    if ((n & 0x0F) == 0) return false;
    x = ((xh & 0x0F) << 8) | xl;
    y = ((yh & 0x0F) << 8) | yl;
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

static void touchPoll() {
    uint32_t now = millis();
    if (now - g_tLast < 12) return;
    g_tLast = now;
    int x, y;
    if (touchRaw(x, y)) {
        g_miss = 0;
        g_tx0 = x; g_ty0 = y;
        g_tEvent = g_touching ? 2 : 1;
        g_touching = true;
    } else if (g_touching) {
        if (++g_miss >= 3) { g_touching = false; g_miss = 0; g_tEvent = 3; }
    }
}

// ---------------- Giao dien ----------------
enum { P_HOME, P_MENU, P_DRIVE, P_MUSIC, P_STORY, P_LEARN, P_SET, P_TOUCH, P_OTA };
static int g_page = P_HOME;
static uint32_t g_lastTouchAt = 0;
static int g_bright = 255;
static int g_mIdx = 0, g_sIdx = 0;
static bool g_drvActive = false;
static int g_drvL = 0, g_drvR = 0, g_knobX = 120, g_knobY = 100;
static uint32_t g_drvSent = 0;
static uint32_t g_homeDownAt = 0;
static bool g_homeSwitched = false;

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

static void drawHomeStatus() {
    uint16_t pc = pillColor();
    gfx->fillRect(50, 141, 140, 13, pc);
    uint16_t dot = g_bleAuthed ? C_OK : (g_bleConn ? C_WARN : C_BAD);
    char b[16];
    b[0] = 0;
    if (g_robotPct >= 0) snprintf(b, sizeof(b), "Pin %d%%", g_robotPct);
    else if (g_follow) strcpy(b, "Di theo");
    int w = (int)strlen(b) * 6;
    int total = 10 + (w ? 6 + w : 0);
    int x0 = 120 - total / 2;
    gfx->fillCircle(x0 + 4, 148, 4, dot);
    if (w) {
        gfx->setTextSize(1);
        gfx->setTextColor(navyColor());
        gfx->setCursor(x0 + 16, 144);
        gfx->print(b);
    }
}

static void drawHomeTime(bool full) {
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
}

static void drawHome() {
    drawFaceImage(g_face);
    drawHomeTime(true);
}

static void nextFace() {
    g_face = (g_face + 1) % 6;
    Preferences pr;
    pr.begin("watch", false);
    pr.putInt("face", g_face);
    pr.end();
    drawHome();
}

static void drawMenu() {
    gfx->fillScreen(C_BG);
    drawTitle("MENU");
    drawTile(0, ICON_DRIVE, 0);
    drawTile(1, ICON_DANCE, 0);
    drawTile(2, ICON_MUSIC, 0);
    drawTile(3, ICON_STORY, 0);
    drawTile(4, ICON_LEARN, 0);
    drawTile(5, ICON_SET, 0);
    drawBack();
}

static void drawLearn() {
    gfx->fillScreen(C_BG);
    drawTitle("HOC");
    drawTile(0, ICON_LEARN, 0, 0, '1');
    drawTile(1, ICON_LEARN, 0, 0, '2');
    drawTile(2, ICON_LEARN, 0, 0, '3');
    drawTile(3, ICON_LEARN, 0, 0, '4');
    drawTile(4, ICON_ENGLISH, 0);
    drawTile(5, nullptr, C_BAD, 'S');
    drawBack();
}

static void drawSet() {
    gfx->fillScreen(C_BG);
    drawTitle("CAI DAT");
    drawTile(0, nullptr, C_BLUE, '+');
    drawTile(1, nullptr, C_BLUE, '-');
    drawTile(2, ICON_SLEEP, 0);
    drawTile(3, ICON_FOLLOW, 0, 0, 0, g_follow);
    drawTile(4, ICON_UPDATE, 0);
    drawTile(5, nullptr, C_GREY, 'T');
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
        case P_TOUCH: drawTouchTest(); break;
    }
}

static void gotoPage(int p) {
    g_page = p;
    g_drvActive = false;
    drawPage();
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

static void otaRun() {
    g_page = P_OTA;
    g_follow = false;
    gfx->fillScreen(C_BG);
    drawTitle("CAP NHAT");
    if (!g_bleAuthed) { otaFinish("Chua noi Rubi", "Den gan robot roi thu lai"); return; }
    otaMsg("Xin Wi-Fi tu Rubi...");
    g_haveWifi = false;
    sendCmd("W");
    uint32_t t0 = millis();
    while (!g_haveWifi && millis() - t0 < 6000) delay(50);
    if (!g_haveWifi) { otaFinish("Rubi chua co Wi-Fi", "Cai Wi-Fi cho Rubi truoc"); return; }
    String ssid = g_wifiSsid, pass = g_wifiPass;
    g_otaMode = true;                   // tat Bluetooth de du bo nho cho Wi-Fi
    t0 = millis();
    while (!g_bleOff && millis() - t0 < 4000) delay(50);
    otaMsg("Noi Wi-Fi...", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
    if (WiFi.status() != WL_CONNECTED) { otaFinish("Khong noi duoc Wi-Fi", ssid.c_str()); return; }

    otaMsg("Hoi GitHub...");
    String body;
    {
        WiFiClientSecure cl;
        cl.setInsecure();
        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        if (!http.begin(cl, OTA_MANIFEST_URL)) { otaFinish("Loi dia chi", ""); return; }
        int code = http.GET();
        if (code != 200) { char b[24]; snprintf(b, sizeof(b), "ma %d", code); http.end(); otaFinish("Chua co ban moi", b); return; }
        body = http.getString();
        http.end();
    }
    String ver = jsonGet(body, "version"), url = jsonGet(body, "url"), md5 = jsonGet(body, "md5");
    if (ver.length() == 0 || url.length() == 0) { otaFinish("Manifest loi", ""); return; }
    if (ver == FW_VERSION) { otaMsg("Da la ban moi nhat", ver.c_str(), C_OK); delay(3000); ESP.restart(); return; }

    otaMsg("Tai ban moi", ver.c_str());
    WiFiClientSecure cl2;
    cl2.setInsecure();
    HTTPClient h2;
    h2.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    h2.setTimeout(15000);
    if (!h2.begin(cl2, url)) { otaFinish("Loi dia chi file", ""); return; }
    int code2 = h2.GET();
    int len = h2.getSize();
    if (code2 != 200 || len <= 0) { h2.end(); otaFinish("Loi tai file", ""); return; }
    if (!Update.begin(len)) { h2.end(); otaFinish("Khong du cho nap", Update.errorString()); return; }
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
    if (loi || got != len) { Update.abort(); otaFinish("Tai loi, giu ban cu", ""); return; }
    if (!Update.end(true)) { otaFinish("Nap loi, giu ban cu", Update.errorString()); return; }
    otaMsg("Xong! Khoi dong lai", ver.c_str(), C_OK);
    delay(1500);
    ESP.restart();
}

// ---------------- Xu ly cham tung trang ----------------
static void onTouch(int ev, int x, int y) {
    switch (g_page) {
        case P_HOME:
            if (ev == 1) { g_homeDownAt = millis() | 1; g_homeSwitched = false; }
            else if (ev == 3 && g_homeDownAt != 0) {
                if (!g_homeSwitched) gotoPage(P_MENU);   // cham nhanh = mo menu; giu 0,7 giay = doi mat dong ho
                g_homeDownAt = 0;
                g_homeSwitched = false;
            }
            break;
        case P_MENU:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_HOME); break; }
            switch (tileHit(6, x, y)) {
                case 0: gotoPage(P_DRIVE); break;
                case 1: sendCmd("A"); break;
                case 2: gotoPage(P_MUSIC); break;
                case 3: gotoPage(P_STORY); break;
                case 4: gotoPage(P_LEARN); break;
                case 5: gotoPage(P_SET); break;
            }
            break;
        case P_LEARN:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            switch (tileHit(6, x, y)) {
                case 0: sendCmd("Cm0"); break;
                case 1: sendCmd("Cm1"); break;
                case 2: sendCmd("Cm2"); break;
                case 3: sendCmd("Cm3"); break;
                case 4: sendCmd("Cen"); break;
                case 5: sendCmd("Chuy"); break;
            }
            break;
        case P_SET:
            if (ev != 1) break;
            if (backHit(x, y)) { gotoPage(P_MENU); break; }
            switch (tileHit(6, x, y)) {
                case 0: sendCmd("Cvup"); break;
                case 1: sendCmd("Cvdown"); break;
                case 2: sendCmd("Csleep"); break;
                case 3: g_follow = !g_follow; drawSet(); break;
                case 4: otaRun(); break;
                case 5: gotoPage(P_TOUCH); break;
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
void setup() {
    Serial.begin(115200);
    g_txQ = xQueueCreate(8, 32);

    ledcAttach(TFT_BL, 5000, 8);
    setBacklight(0);

    ledcAttach(IR_PIN, 38000, 8);
    ledcWrite(IR_PIN, 0);

    Wire.begin(TP_SDA, TP_SCL);
    Wire.setClock(400000);
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);  delay(10);
    digitalWrite(TP_RST, HIGH); delay(60);
    pinMode(TP_INT, INPUT);
    Wire.beginTransmission(0x15); Wire.write(0xFE); Wire.write(0xFF); Wire.endTransmission();   // khong tu ngu

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
    gfx->fillScreen(C_BG);
    textCenter("RUBI", 120, 105, 4, C_BLUE);
    textCenter(FW_VERSION, 120, 140, 1, C_DIM);
    setBacklight(255);
    delay(800);

    xTaskCreate(bleTask, "ble", 8192, nullptr, 1, nullptr);
    xTaskCreate(irTask, "ir", 2048, nullptr, 2, nullptr);

    {
        Preferences pr;
        pr.begin("watch", true);
        g_face = pr.getInt("face", 0);
        pr.end();
        if (g_face < 0 || g_face > 5) g_face = 0;
    }
    g_lastTouchAt = millis();
    gotoPage(P_HOME);
}

void loop() {
    uint32_t now = millis();
    touchPoll();
    static bool ignoreUntilUp = false;
    if (g_tEvent) {
        int ev = g_tEvent;
        g_tEvent = 0;
        bool sleeping = (g_bright < 255);
        g_lastTouchAt = now;
        if (sleeping) { g_bright = 255; setBacklight(255); }
        if (sleeping && ev == 1) ignoreUntilUp = true;   // cham dau tien luc dang ngu chi de danh thuc
        if (ignoreUntilUp) { if (ev == 3) ignoreUntilUp = false; }
        else onTouch(ev, g_tx0, g_ty0);
    }
    // giu lau tren man hinh chinh: doi mat dong ho
    if (g_page == P_HOME && g_touching && g_homeDownAt != 0 && !g_homeSwitched && !ignoreUntilUp && now - g_homeDownAt >= 700) {
        g_homeSwitched = true;
        nextFace();
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
    if (g_page == P_HOME && now - homeAt > 1000) {
        homeAt = now;
        drawHomeTime(false);
        static int lastState = -1;
        int st = g_bleAuthed ? 2 : g_bleConn ? 1 : 0;
        static int lastPct = -2;
        if (st != lastState || g_robotPct != lastPct) { lastState = st; lastPct = g_robotPct; drawHomeStatus(); }
    }
    // 15 giay khong cham o menu/trang con thi tu quay ve mat dong ho (khong ap dung luc dang keo lai xe, luc cap nhat)
    if (g_page != P_HOME && g_page != P_OTA && !g_drvActive && now - g_lastTouchAt > MENU_IDLE_MS) {
        if (g_page == P_DRIVE) sendCmd("X");
        gotoPage(P_HOME);
    }
    // giam sang / tat man hinh khi khong cham (khong dung o trang lai xe dang keo)
    if (!g_drvActive && g_page != P_OTA) {
        uint32_t idle = now - g_lastTouchAt;
        if (idle > OFF_MS) { if (g_bright != 0) { g_bright = 0; setBacklight(0); } }
        else if (idle > DIM_MS) { if (g_bright > 40) { g_bright = 40; setBacklight(40); } }
    }
    delay(2);
}
