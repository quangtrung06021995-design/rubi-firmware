// Rubi - firmware cho ESP32 DevKit 30 chan (dieu khien 2 motor qua TB6612 + do pin).
// Nap bang Arduino IDE:  Board = "ESP32 Dev Module",  Upload Speed 115200,  Serial Monitor 115200 baud, "Newline".
//
// KHI THU LAN DAU: NHAC BANH XE KHOI MAT BAN (hoac lat nguoc robot) de banh quay tu do.
//
// Chan DevKit (theo so do day):
//   Motor A: PWMA=D25  AIN1=D26  AIN2=D27     encoder E1A=D34  E1B=D35
//   Motor B: PWMB=D33  BIN1=D13  BIN2=D14     encoder E2A=VN(GPIO39)  E2B=D32
//   STBY=D23 (muc cao = driver chay)
//   Do pin: module chia ap 5:1, S -> GPIO36 (VP)   (dat CO_MODULE_DO_PIN = true khi da noi module)
//   UART2 sang bo man hinh: TX2=GPIO17, RX2=GPIO16 (chi gui "B<mV>" moi giay, nhu pin_gui_phan_tram.ino)
//
// Lenh go trong Serial Monitor (gui bang Enter):
//   t        thu tu dong: tung motor chay tien/lui 2 giay, in ket qua encoder va chan doan
//   a50      motor A chay 50% (am = lui, vi du a-50)     b50   motor B
//   d40      ca hai motor cung chay 40%                  s     dung
//   r        dat lai bo dem encoder                       e     in bo dem + toc do
//   ?        hien huong dan
// Moi lenh a/b/d chi chay toi da 5 giay roi tu dung (de khong bao gio chay mai).
//
// Wi-Fi + CAP NHAT QUA MANG (OTA):
//   wTenWifi|MatKhau   them Wi-Fi 2.4 GHz: robot thu noi, NOI DUOC MOI LUU (nho toi da 10 mang, bo mang it dung nhat khi day).
//                      Bo man hinh gui cung lenh nay (hoac dong W<ten>\t<mk>\t<cs> nhu gui cho camera) qua UART2; DevKit bao lai "I<ip>" moi 5 giay, "F" neu mang moi khong noi duoc.
//   p4826 = dat ma PIN dieu khien web (4-8 so); p- = xoa PIN; chua dat thi ai cung lai xe duoc. /api/stop luon mo.
//   l = xem cac mang da nho   q2 = quen mang so 2   w- = quen het.  Bat nguon: quet va noi mang manh nhat trong so da nho.
//   w                  xem trang thai Wi-Fi          w-   xoa Wi-Fi          i   xem IP/trang thai
//   u                  TU TAI BAN MOI TU GITHUB (nhanh ota-devkit) roi tu nap, khoi dong lai. u! = ep nap lai dung ca khi cung phien ban.
//   v                  xem phien ban dang chay.  Bo man hinh cung co the gui dong "u" qua UART2 de kich hoat.
//   Web dieu khien: http://rubi.local (hoac http://<IP>) - giao dien tai tu Vercel, mat Internet thi hien trang co ban.
//   Tools -> Partition Scheme: "Minimal SPIFFS (1.9MB APP with OTA)". Lan dau nap bang cap USB,
//   tu lan sau: Tools -> Port -> "rubi-devkit at <IP>" (mat khau OTA: xem OTA_MAT_KHAU ben duoi).

#include <Arduino.h>
#include <driver/gpio.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_random.h>

// ---------------- Chan ----------------
const int PIN_PWMA = 25, PIN_AIN1 = 26, PIN_AIN2 = 27;
const int PIN_PWMB = 33, PIN_BIN1 = 13, PIN_BIN2 = 14;
const int PIN_STBY = 23;
const int PIN_E1A = 34, PIN_E1B = 35;
const int PIN_E2A = 39, PIN_E2B = 32;
const int PIN_VBAT = 36;

// ---------------- Cau hinh (sua o day neu can) ----------------
// Dao chieu neu "tien" ma banh quay lui:
const bool DAO_CHIEU_A = true;
const bool DAO_CHIEU_B = true;
// Dao dau encoder neu tien ma bo dem giam:
const bool DAO_ENC_A = false;
const bool DAO_ENC_B = true;

const int PWM_FREQ = 10000;      // 10 kHz, em tai, khong ru
const int PWM_BITS = 10;         // 0..1023
const int PWM_MAX = (1 << PWM_BITS) - 1;
const int GIOI_HAN_TOC_DO = 85;  // % toi da, de chua du pin va giam dong dien

// Encoder: 13 xung/vong truc motor, giam toc 1:20, doc 4 canh => dem moi vong banh
const float COUNTS_PER_REV = 13.0f * 4.0f * 20.0f;   // 1040
const float WHEEL_DIAMETER_MM = 48.0f;

// Tang toc mem: moi 10 ms tang toi da bay nhieu (%): 2 = tu 0 len 100 mat 0.5 giay
const int SLEW_STEP = 2;

// Tu ngat khi motor ket: PWM >= STALL_MIN_PCT ma encoder dung yen STALL_MS
const int STALL_MIN_PCT = 20;
const uint32_t STALL_MS = 800;
const int STALL_MIN_COUNTS = 6;

// Do pin: chi bat (true) khi da noi module do ap (S -> GPIO36, - -> GND). Chua noi thi de false,
// neu khong chan GPIO36 de ho se doc ra so ngau nhien va chuong trinh tuong pin yeu.
const bool CO_MODULE_DO_PIN = false;
const float TI_LE_CHIA = 5.0f;           // module 30k/7.5k
const float HE_SO_HIEU_CHINH = 1.000f;   // do pin bang dong ho van nang roi sua so nay
const int PIN_THAP_MV = 6200;            // duoi muc nay (2S, 3.1 V/cell) -> khong cho motor chay
const uint32_t LENH_TOI_DA_MS = 5000;

// ---------------- PWM tuong thich Arduino-ESP32 2.x va 3.x ----------------
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define PWM_SETUP() do { ledcAttach(PIN_PWMA, PWM_FREQ, PWM_BITS); ledcAttach(PIN_PWMB, PWM_FREQ, PWM_BITS); } while (0)
  #define PWM_WRITE(ch, duty) ledcWrite((ch) == 0 ? PIN_PWMA : PIN_PWMB, (duty))
#else
  #define PWM_SETUP() do { ledcSetup(0, PWM_FREQ, PWM_BITS); ledcAttachPin(PIN_PWMA, 0); \
                            ledcSetup(1, PWM_FREQ, PWM_BITS); ledcAttachPin(PIN_PWMB, 1); } while (0)
  #define PWM_WRITE(ch, duty) ledcWrite((ch), (duty))
#endif

// ---------------- Encoder (4 lan moi xung) ----------------
static const int8_t QEM[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
volatile int32_t g_cnt[2] = {0, 0};
volatile uint8_t g_st[2] = {0, 0};

static inline void IRAM_ATTR encStep(int ch, int a, int b) {
    uint8_t s = (uint8_t)(((g_st[ch] << 2) | (gpio_get_level((gpio_num_t)a) << 1) | gpio_get_level((gpio_num_t)b)) & 0x0F);
    g_cnt[ch] += QEM[s];
    g_st[ch] = s & 3;
}
void IRAM_ATTR isrA() { encStep(0, PIN_E1A, PIN_E1B); }
void IRAM_ATTR isrB() { encStep(1, PIN_E2A, PIN_E2B); }

int32_t doDem(int ch) {
    noInterrupts();
    int32_t v = g_cnt[ch];
    interrupts();
    bool dao = (ch == 0) ? DAO_ENC_A : DAO_ENC_B;
    return dao ? -v : v;
}

// ---------------- Motor ----------------
int g_target[2] = {0, 0};    // % yeu cau (-100..100)
int g_cmd[2] = {0, 0};       // % dang xuat ra sau tang toc mem
bool g_fault[2] = {false, false};
uint32_t g_stall_since[2] = {0, 0};
int32_t g_stall_cnt[2] = {0, 0};
uint32_t g_cmd_deadline = 0; // het gio thi tu dung (0 = khong dung gio)
bool g_pin_thap = false;

void xuatMotor(int ch, int pct) {
    int in1 = (ch == 0) ? PIN_AIN1 : PIN_BIN1;
    int in2 = (ch == 0) ? PIN_AIN2 : PIN_BIN2;
    bool dao = (ch == 0) ? DAO_CHIEU_A : DAO_CHIEU_B;
    bool tien = pct > 0;
    if (dao) tien = !tien;
    if (pct == 0) {                 // tha troi (coast)
        digitalWrite(in1, LOW);
        digitalWrite(in2, LOW);
        PWM_WRITE(ch, 0);
        return;
    }
    digitalWrite(in1, tien ? HIGH : LOW);
    digitalWrite(in2, tien ? LOW : HIGH);
    int a = abs(pct);
    if (a > GIOI_HAN_TOC_DO) a = GIOI_HAN_TOC_DO;
    PWM_WRITE(ch, (a * PWM_MAX) / 100);
}

void datToc(int ch, int pct) {
    if (pct > 100) pct = 100;
    if (pct < -100) pct = -100;
    if (pct != 0 && g_pin_thap) {
        Serial.println("! Pin yeu, khong chay motor");
        pct = 0;
    }
    if (pct != 0 && g_fault[ch]) pct = 0;   // dang bi khoa vi ket, can lenh dung (s) de mo
    g_target[ch] = pct;
}

void dungHet() {
    for (int ch = 0; ch < 2; ch++) {
        g_target[ch] = 0;
        g_cmd[ch] = 0;
        g_fault[ch] = false;
        g_stall_since[ch] = 0;
        xuatMotor(ch, 0);
    }
    g_cmd_deadline = 0;
}

// Goi moi 10 ms
void capNhatMotor() {
    uint32_t now = millis();
    for (int ch = 0; ch < 2; ch++) {
        int t = g_fault[ch] ? 0 : g_target[ch];
        if (g_cmd[ch] < t) g_cmd[ch] = min(g_cmd[ch] + SLEW_STEP, t);
        else if (g_cmd[ch] > t) g_cmd[ch] = max(g_cmd[ch] - SLEW_STEP, t);
        xuatMotor(ch, g_cmd[ch]);

        // chong ket
        if (abs(g_cmd[ch]) >= STALL_MIN_PCT) {
            int32_t c = doDem(ch);
            if (g_stall_since[ch] == 0) {
                g_stall_since[ch] = now;
                g_stall_cnt[ch] = c;
            } else if (abs(c - g_stall_cnt[ch]) >= STALL_MIN_COUNTS) {
                g_stall_since[ch] = now;
                g_stall_cnt[ch] = c;
            } else if (now - g_stall_since[ch] >= STALL_MS) {
                g_fault[ch] = true;
                g_target[ch] = 0;
                g_cmd[ch] = 0;
                xuatMotor(ch, 0);
                Serial.printf("! Motor %c bi ket (khong quay %u ms): da ngat. Go 's' de mo lai\n", ch == 0 ? 'A' : 'B', (unsigned)STALL_MS);
            }
        } else {
            g_stall_since[ch] = 0;
        }
    }
    if (g_cmd_deadline != 0 && (int32_t)(now - g_cmd_deadline) >= 0) {
        g_target[0] = g_target[1] = 0;
        g_cmd_deadline = 0;
        Serial.println("(het gio lenh, tu dung)");
    }
}

bool g_huy = false;   // nguoi dung go 's' trong luc thu tu dong

void doPin();

void giuNhip(uint32_t ms) {   // cho ms ma van cap nhat motor + do pin, va nghe lenh 's'
    uint32_t t0 = millis(), last = t0;
    while (millis() - t0 < ms && !g_huy) {
        if (millis() - last >= 10) {
            last += 10;
            capNhatMotor();
        }
        doPin();
        while (Serial.available()) {
            if (Serial.read() == 's') { g_huy = true; dungHet(); Serial.println("Da dung (huy thu)"); }
        }
        delay(1);
    }
}

// ---------------- Do pin ----------------
uint32_t g_bat_sum = 0;
int g_bat_n = 0;
uint32_t g_bat_last_sample = 0, g_bat_last_send = 0;
float g_bat_mv = 0;

void doPin() {
    if (!CO_MODULE_DO_PIN) return;
    uint32_t now = millis();
    if (now - g_bat_last_sample >= 15) {
        g_bat_last_sample = now;
        g_bat_sum += analogReadMilliVolts(PIN_VBAT);
        g_bat_n++;
    }
    if (now - g_bat_last_send >= 1000 && g_bat_n > 0) {
        g_bat_last_send = now;
        g_bat_mv = (g_bat_sum / (float)g_bat_n) * TI_LE_CHIA * HE_SO_HIEU_CHINH;
        g_bat_sum = 0;
        g_bat_n = 0;
        Serial2.printf("B%d\n", (int)g_bat_mv);
        // chi khoa motor khi co so do hop ly (module chua noi thi doc ~0, bo qua)
        g_pin_thap = (g_bat_mv > 4000 && g_bat_mv < PIN_THAP_MV);
        if (g_pin_thap) {
            g_target[0] = g_target[1] = 0;
            static uint32_t lastMsg = 0;
            if (millis() - lastMsg > 5000) { lastMsg = millis(); Serial.printf("! Pin yeu: %.2f V\n", g_bat_mv / 1000.0f); }
        }
    }
}

// ---------------- In thong tin ----------------
void inDem() {
    static int32_t last[2] = {0, 0};
    static uint32_t lastT = 0;
    uint32_t now = millis();
    float dt = (lastT == 0) ? 1.0f : (now - lastT) / 1000.0f;
    int32_t c0 = doDem(0), c1 = doDem(1);
    float rpm0 = (c0 - last[0]) / COUNTS_PER_REV * 60.0f / dt;
    float rpm1 = (c1 - last[1]) / COUNTS_PER_REV * 60.0f / dt;
    float mm = 3.14159265f * WHEEL_DIAMETER_MM / COUNTS_PER_REV;
    Serial.printf("A: dem=%ld (%.0f vong/phut, %.1f mm)   B: dem=%ld (%.0f vong/phut, %.1f mm)   pin=%.2f V\n",
                  (long)c0, rpm0, c0 * mm, (long)c1, rpm1, c1 * mm, g_bat_mv / 1000.0f);
    last[0] = c0; last[1] = c1; lastT = now;
}

void inHuongDan() {
    Serial.println("Lenh: t=thu tu dong | a50 / b-50 = motor A / B | d40 = ca hai | s=dung | r=reset dem | e=in dem | ?=huong dan");
    Serial.println("Wi-Fi/OTA: wTenWifi|MatKhau = them mang (chi luu neu noi duoc) | l = cac mang da nho | q2 = quen mang so 2 | w- = quen het | w = trang thai | i = IP | u = tai ban moi | v = phien ban | p4826 = dat PIN | p- = xoa PIN");
}

// ---------------- Thu tu dong ----------------
void chanDoan(char name, int32_t tien, int32_t lui) {
    Serial.printf("  Motor %c: tien %ld, lui %ld\n", name, (long)tien, (long)lui);
    if (abs(tien) < 20 && abs(lui) < 20) {
        Serial.printf("  -> Motor %c KHONG dem duoc. Kiem tra day encoder (E%cA/E%cB), cap 3V3, hoac motor khong quay.\n", name, name == 'A' ? '1' : '2', name == 'A' ? '1' : '2');
    } else if (tien < 0 && lui > 0) {
        Serial.printf("  -> Encoder %c dem nguoc: dat DAO_ENC_%c = true.  (Neu banh cung quay lui khi 'tien' thi dat DAO_CHIEU_%c = true)\n", name, name, name);
    } else if (tien > 0 && lui < 0) {
        Serial.printf("  -> Motor %c OK: tien dem duong, lui dem am. Nhin banh: neu 'tien' ma banh quay lui thi dat DAO_CHIEU_%c = true va DAO_ENC_%c = true.\n", name, name, name);
    } else {
        Serial.printf("  -> Motor %c: ket qua la. Ghi lai 2 so tren va gui cho minh.\n", name);
    }
}

void thuTuDong() {
    Serial.println("== THU TU DONG: tung motor tien 2 giay, nghi 1 giay, lui 2 giay ==");
    Serial.println("Banh xe phai nhac khoi mat ban. Nhan 's' de dung bat cu luc nao.");
    dungHet();
    g_huy = false;
    giuNhip(1500);
    for (int ch = 0; ch < 2 && !g_huy; ch++) {
        char name = ch == 0 ? 'A' : 'B';
        int32_t c0 = doDem(ch);
        datToc(ch, 35);
        giuNhip(2000);
        datToc(ch, 0);
        giuNhip(1000);
        int32_t c1 = doDem(ch);
        datToc(ch, -35);
        giuNhip(2000);
        datToc(ch, 0);
        giuNhip(1000);
        int32_t c2 = doDem(ch);
        if (g_huy) break;
        chanDoan(name, c1 - c0, c2 - c1);
        if (g_fault[ch]) { Serial.printf("  Motor %c bi khoa vi ket trong luc thu\n", name); dungHet(); }
    }
    if (!g_huy) Serial.println("== Xong ==");
}

// ---------------- Wi-Fi + cap nhat OTA ----------------
const char* OTA_TEN_MAY = "rubi-devkit";
const char* OTA_MAT_KHAU = "rubi1234";   // Arduino IDE hoi mat khau nay khi nap qua Wi-Fi
Preferences g_pref;
String g_ssid, g_pass;            // mang dang noi / dang thu
bool g_ota_started = false;
bool g_wifi_was = false;
uint32_t g_auto_at = 0;      // thoi diem tu kiem tra GitHub (0 = chua dat / da xong)
bool g_auto_done = false;
uint32_t g_wifi_try = 0;

// Danh sach toi da WIFI_MAX mang DA NOI THANH CONG (mang sai mat khau khong duoc luu). g_nq lon = dung gan day nhat.
const int WIFI_MAX = 10;
String g_ns[WIFI_MAX], g_np[WIFI_MAX];
uint32_t g_nq[WIFI_MAX];
int g_nn = 0;
uint32_t g_seq = 0;
bool g_cho = false;                    // dang thu mot mang moi (chua luu)
String g_cho_ssid, g_cho_pass;
bool g_loi_co = false;                 // mang moi vua that bai: bo qua lenh lap lai trong 60 giay
String g_loi_ssid, g_loi_pass;
uint32_t g_loi_at = 0;
uint32_t g_ip_send = 0;

void wifiLuuDS() {
    g_pref.begin("rubiwf", false);
    g_pref.clear();
    g_pref.putUInt("n", (uint32_t)g_nn);
    g_pref.putUInt("seq", g_seq);
    for (int i = 0; i < g_nn; i++) {
        String k = String(i);
        g_pref.putString(("s" + k).c_str(), g_ns[i]);
        g_pref.putString(("p" + k).c_str(), g_np[i]);
        g_pref.putUInt(("q" + k).c_str(), g_nq[i]);
    }
    g_pref.end();
}

int wifiTim(const String& s) {
    for (int i = 0; i < g_nn; i++) if (g_ns[i] == s) return i;
    return -1;
}

void wifiThem(const String& s, const String& p) {
    int i = wifiTim(s);
    if (i < 0) {
        if (g_nn < WIFI_MAX) i = g_nn++;
        else { i = 0; for (int k = 1; k < g_nn; k++) if (g_nq[k] < g_nq[i]) i = k; }   // day: bo mang it dung nhat
        g_ns[i] = s;
    }
    g_np[i] = p;
    g_nq[i] = ++g_seq;
    wifiLuuDS();
}

void wifiQuenMuc(int i) {
    if (i < 0 || i >= g_nn) return;
    for (int k = i; k < g_nn - 1; k++) { g_ns[k] = g_ns[k + 1]; g_np[k] = g_np[k + 1]; g_nq[k] = g_nq[k + 1]; }
    g_nn--;
    wifiLuuDS();
}

void wifiQuenHet() {
    g_nn = 0; g_cho = false; g_loi_co = false;
    g_ssid = ""; g_pass = "";
    wifiLuuDS();
    WiFi.disconnect();
}

void wifiNapDS() {
    g_pref.begin("rubiwf", true);
    g_nn = (int)g_pref.getUInt("n", 0);
    if (g_nn < 0 || g_nn > WIFI_MAX) g_nn = 0;
    g_seq = g_pref.getUInt("seq", 0);
    for (int i = 0; i < g_nn; i++) {
        String k = String(i);
        g_ns[i] = g_pref.getString(("s" + k).c_str(), "");
        g_np[i] = g_pref.getString(("p" + k).c_str(), "");
        g_nq[i] = g_pref.getUInt(("q" + k).c_str(), 0);
    }
    g_pref.end();
    if (g_nn == 0) {   // chuyen mang duy nhat cua ban 1.0/1.1 sang danh sach
        g_pref.begin("rubi", true);
        String s = g_pref.getString("ssid", ""), p = g_pref.getString("pass", "");
        g_pref.end();
        if (s.length() > 0) wifiThem(s, p);
    }
}

void wifiBegin(const String& s, const String& p) {
    g_ssid = s; g_pass = p;
    WiFi.disconnect();
    WiFi.begin(s.c_str(), p.c_str());
    g_wifi_try = millis();
}

// Chon mang da nho co song manh nhat; khong thay mang nao thi thu mang dung gan day nhat (co the la mang an).
void wifiKetNoi() {
    if (g_nn == 0) return;
    int best = -1, bestRssi = -1000;
    WiFi.disconnect();
    int n = WiFi.scanNetworks(false, true);      // chan 2-4 giay; chi goi khi motor dang dung
    for (int j = 0; j < n; j++) {
        int i = wifiTim(WiFi.SSID(j));
        if (i >= 0 && WiFi.RSSI(j) > bestRssi) { bestRssi = WiFi.RSSI(j); best = i; }
    }
    WiFi.scanDelete();
    if (best < 0) {
        best = 0;
        for (int k = 1; k < g_nn; k++) if (g_nq[k] > g_nq[best]) best = k;
        Serial.println("Wi-Fi: khong thay mang nao da nho, thu mang dung gan nhat");
    }
    Serial.printf("Wi-Fi: dang noi \"%s\"...\n", g_ns[best].c_str());
    wifiBegin(g_ns[best], g_np[best]);
}

// Mang MOI (tu man hinh hoac go tay): thu noi, chi luu khi noi duoc. tuUART = lenh lap lai tu bo man hinh.
void wifiMangMoi(const String& s, const String& p, bool tuUART) {
    if (s.length() == 0 || s.length() > 32 || p.length() > 63) { Serial.println("Ten Wi-Fi 1-32 ky tu, mat khau toi da 63"); return; }
    int i = wifiTim(s);
    if (i >= 0 && g_np[i] == p) {                 // da biet roi
        if (!tuUART) wifiKetNoi();                // go tay: noi ngay
        return;                                   // tu man hinh (gui lap moi 5 giay): im lang, wifiTick lo
    }
    if (g_cho && g_cho_ssid == s && g_cho_pass == p) return;     // dang thu dung mang nay
    if (tuUART && g_loi_co && g_loi_ssid == s && g_loi_pass == p && millis() - g_loi_at < 60000) return;
    g_cho = true; g_cho_ssid = s; g_cho_pass = p; g_loi_co = false;
    Serial.printf("Mang moi \"%s\": dang thu noi (chi luu neu noi duoc)...\n", s.c_str());
    wifiBegin(s, p);
}

// Dong tu bo man hinh: W<ten>\t<mat khau>\t<2 chu so hex tong byte>  (cung dinh dang gui cho camera)
bool phanTichW(const String& line, String& ssid, String& pass) {
    if (line.length() < 5 || line[0] != 'W') return false;
    int t1 = line.indexOf('\t'), t2 = line.lastIndexOf('\t');
    if (t1 < 2 || t2 <= t1 || (int)line.length() - t2 != 3) return false;
    unsigned sum = 0;
    for (int k = 1; k < t2; k++) sum += (uint8_t)line[k];
    if ((sum & 0xFF) != (unsigned)strtoul(line.substring(t2 + 1).c_str(), nullptr, 16)) return false;
    ssid = line.substring(1, t1);
    pass = line.substring(t1 + 1, t2);
    return ssid.length() > 0;
}

void wifiTrangThai() {
    if (g_nn == 0 && !g_cho) { Serial.println("Wi-Fi: chua co mang nao. Go: wTenWifi|MatKhau (hoac cai tren man hinh Rubi)"); return; }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("Wi-Fi: dang noi \"%s\", IP %s, da nho %d mang, OTA: %s (mat khau %s)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), g_nn, OTA_TEN_MAY, OTA_MAT_KHAU);
    else
        Serial.printf("Wi-Fi: chua noi duoc (da nho %d mang; can Wi-Fi 2.4 GHz, kiem tra ten/mat khau)\n", g_nn);
}

void wifiLiet() {
    if (g_nn == 0) { Serial.println("Chua nho mang nao"); return; }
    for (int i = 0; i < g_nn; i++)
        Serial.printf("  %d. %s%s\n", i + 1, g_ns[i].c_str(), (WiFi.status() == WL_CONNECTED && WiFi.SSID() == g_ns[i]) ? "   <- dang noi" : "");
}

void otaBatDau() {
    ArduinoOTA.setHostname(OTA_TEN_MAY);
    ArduinoOTA.setPassword(OTA_MAT_KHAU);
    ArduinoOTA.onStart([]() {
        dungHet();
        digitalWrite(PIN_STBY, LOW);   // tat driver trong luc nap
        Serial.println("OTA: bat dau nap, da tat motor");
    });
    ArduinoOTA.onEnd([]() { Serial.println("OTA: xong, khoi dong lai"); });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("OTA loi %u\n", (unsigned)e);
        digitalWrite(PIN_STBY, HIGH);
    });
    ArduinoOTA.begin();
    g_ota_started = true;
}

extern WebServer g_web;
extern bool g_web_started;
void webBatDau();
void mdnsBatDau();

void wifiTick() {
    if (g_nn == 0 && !g_cho) return;
    // dang thu mang moi thi chi tinh la "noi duoc" khi dung ten mang do (tranh doc nham mang cu truoc luc ngat)
    bool ok = (WiFi.status() == WL_CONNECTED) && (!g_cho || WiFi.SSID() == g_cho_ssid);
    uint32_t now = millis();
    if (ok) {
        bool moi = !g_wifi_was;                        // vua noi (hoac vua doi sang mang khac)
        if (g_cho) {
            wifiThem(g_cho_ssid, g_cho_pass);
            g_cho = false; moi = true;
            Serial.printf("Da luu mang moi \"%s\" (nho %d mang)\n", g_cho_ssid.c_str(), g_nn);
        } else if (moi) {
            int i = wifiTim(WiFi.SSID());
            if (i >= 0 && g_nq[i] != g_seq) { g_nq[i] = ++g_seq; wifiLuuDS(); }   // danh dau vua dung
        }
        if (moi) {
            wifiTrangThai(); mdnsBatDau();
            if (!g_auto_done && g_auto_at == 0) g_auto_at = millis() + 8000;
            g_ip_send = now - 5000;                    // bao IP ngay
        }
        if (!g_ota_started) otaBatDau();
        if (!g_web_started) webBatDau();
        g_web.handleClient();
        ArduinoOTA.handle();
        if (now - g_ip_send >= 5000) {            // bao dia chi cho bo man hinh (dong ngan "I<ip>")
            g_ip_send = now;
            Serial2.printf("I%s\n", WiFi.localIP().toString().c_str());
            Serial2.printf("K%d\n", g_pin.length() ? 1 : 0);   // co dat PIN chua
        }
    } else {
        if (g_wifi_was) Serial.println("Wi-Fi mat ket noi");
        bool dung = (g_cmd[0] == 0 && g_cmd[1] == 0);   // chi thu noi lai khi motor dang nghi
        if (g_cho && now - g_wifi_try > 15000) {
            Serial.printf("Mang moi \"%s\" KHONG noi duoc (sai mat khau, hoac la Wi-Fi 5 GHz). Khong luu.\n", g_cho_ssid.c_str());
            Serial2.println("F");
            g_loi_co = true; g_loi_ssid = g_cho_ssid; g_loi_pass = g_cho_pass; g_loi_at = now;
            g_cho = false;
            if (dung) wifiKetNoi();                    // quay lai mang da nho
        } else if (!g_cho && dung && now - g_wifi_try > 20000) {
            wifiKetNoi();
        }
    }
    g_wifi_was = ok;
}


// ---------------- Web dieu khien (rubi.local) ----------------
// DevKit phuc vu 1 trang rat nho o http://rubi.local. Trang do tai giao dien day du tu Vercel (neu co Internet)
// va chay NGAY TREN dia chi rubi.local, nen lenh di thang toi robot qua Wi-Fi nha. Mat Internet thi hien nut co ban.
extern const char* FW_VERSION;
WebServer g_web(80);
const char* MDNS_TEN = "rubi";                       // -> http://rubi.local
const uint32_t WEB_DEADMAN_MS = 500;                 // het 0.5 giay khong co lenh moi thi robot tu dung
const bool A_LA_BANH_TRAI = false;                   // dat false neu motor A la banh PHAI (nhin tu phia sau robot)
bool g_web_started = false;
bool g_mdns_started = false;

const char PAGE_ROOT[] PROGMEM = R"RAW(<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-title" content="Rubi">
<meta name="theme-color" content="#101820"><title>Rubi</title>
<style>
body{margin:0;background:#101820;color:#eaf1f7;font:16px system-ui,sans-serif;text-align:center;padding:16px}
h1{font-size:20px;margin:8px 0 2px}p{margin:4px 0 14px;color:#9fb0c0}
.g{display:grid;grid-template-columns:repeat(3,84px);gap:10px;justify-content:center;margin:12px 0}
button{height:84px;border:0;border-radius:14px;background:#24384a;color:#fff;font-size:26px;touch-action:none}
button:active{background:#e0553a}.s{background:#a33}
</style></head><body>
<h1>Rubi</h1><p id="st">Đang tải giao diện đầy đủ...</p>
<div class="g">
<span></span><button data-l="60" data-r="60">▲</button><span></span>
<button data-l="-45" data-r="45">◀</button><button class="s" data-l="0" data-r="0">■</button><button data-l="45" data-r="-45">▶</button>
<span></span><button data-l="-50" data-r="-50">▼</button><span></span></div>
<script>
var UI='https://rubi-web-eight.vercel.app/',cur=[0,0],tm=null;
var TK='';try{TK=localStorage.getItem('rubi_t')||''}catch(e){}
function q(){return TK?'&t='+encodeURIComponent(TK):''}
function send(){fetch('/api/drive?l='+cur[0]+'&r='+cur[1]+q(),{cache:'no-store'}).then(function(r){if(r.status==401)ask()}).catch(function(){})}
var asking=false;
function ask(){if(asking)return;asking=true;halt();var p=prompt('Nhập mã PIN');if(!p){asking=false;return}fetch('/api/login?pin='+encodeURIComponent(p),{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){if(j.ok){TK=j.t||'';try{localStorage.setItem('rubi_t',TK)}catch(e){}}else alert(j.wait?'Thử lại sau '+j.wait+' giây':'Sai mã');asking=false}).catch(function(){asking=false})}
function halt(){clearInterval(tm);tm=null;cur=[0,0];fetch('/api/stop',{cache:'no-store'}).catch(function(){})}
document.querySelectorAll('button').forEach(function(b){
 b.addEventListener('pointerdown',function(e){e.preventDefault();var l=+b.dataset.l,r=+b.dataset.r;if(!l&&!r){halt();return}cur=[l,r];send();if(!tm)tm=setInterval(send,150)});
 ['pointerup','pointercancel','pointerleave'].forEach(function(n){b.addEventListener(n,function(){if(+b.dataset.l||+b.dataset.r)halt()})})});
(function(){var c=new AbortController(),t=setTimeout(function(){c.abort()},5000);
 fetch(UI+'index.html',{signal:c.signal,cache:'no-cache'}).then(function(r){if(!r.ok)throw 0;return r.text()}).then(function(h){
  clearTimeout(t);h=h.replace('<head>','<head><base href="'+UI+'">');document.open();document.write(h);document.close()
 }).catch(function(){document.getElementById('st').textContent='Chế độ cơ bản (chưa có Internet)'})})();
</script></body></html>)RAW";

static void webJson(const String& j) {
    g_web.sendHeader("Cache-Control", "no-store");
    g_web.send(200, "application/json", j);
}


// ---- Ma PIN dieu khien ----
// Chua dat PIN = ai cung dieu khien duoc (nhu truoc). Dat PIN: lenh p<4-8 chu so> (cap USB hoac tu bo man hinh qua UART2); p- = xoa (chi qua USB).
// Dien thoai goi /api/login?pin=.. nhan lai ma "t"; cac lenh lai xe phai kem &t=<ma>. Doi PIN thi ma "t" doi, moi dien thoai phai nhap lai.
// /api/stop LUON mo (dung khan cap khong can PIN). Sai 5 lan lien tiep thi khoa 30 giay, lan sau gap doi (toi da 4 phut).
String g_pin, g_tok;
int g_pin_bad = 0, g_pin_lock_n = 0;
uint32_t g_pin_lock_until = 0;
bool g_pin_locked = false;

void pinNap() {
    g_pref.begin("rubi", true);
    g_pin = g_pref.getString("pin", "");
    g_tok = g_pref.getString("tok", "");
    g_pref.end();
    if (g_pin.length() > 0 && g_tok.length() == 0) { g_tok = String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX); }
}

bool pinHopLe(const String& p) {
    if (p.length() < 4 || p.length() > 8) return false;
    for (unsigned i = 0; i < p.length(); i++) if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

void pinDat(const String& p) {          // p rong = xoa PIN
    g_pin = p;
    g_tok = p.length() ? (String((uint32_t)esp_random(), HEX) + String((uint32_t)esp_random(), HEX)) : String("");
    g_pref.begin("rubi", false);
    g_pref.putString("pin", g_pin);
    g_pref.putString("tok", g_tok);
    g_pref.end();
    g_pin_bad = 0; g_pin_lock_n = 0; g_pin_locked = false;
}

bool webAuth() { return g_pin.length() == 0 || (g_tok.length() > 0 && g_web.arg("t") == g_tok); }

void webTuChoi() {
    g_web.sendHeader("Cache-Control", "no-store");
    g_web.send(401, "application/json", "{\"ok\":false,\"err\":\"pin\"}");
}

void webLogin() {   // /api/login?pin=1234
    if (g_pin.length() == 0) { webJson("{\"ok\":true,\"t\":\"\"}"); return; }
    uint32_t now = millis();
    if (g_pin_locked && (int32_t)(g_pin_lock_until - now) > 0) {
        webJson(String("{\"ok\":false,\"wait\":") + String((g_pin_lock_until - now + 999) / 1000) + "}");
        return;
    }
    g_pin_locked = false;
    if (g_web.arg("pin") == g_pin) {
        g_pin_bad = 0; g_pin_lock_n = 0;
        webJson(String("{\"ok\":true,\"t\":\"") + g_tok + "\"}");
        return;
    }
    g_pin_bad++;
    if (g_pin_bad >= 5) {
        g_pin_bad = 0;
        uint32_t w = 30000UL << (g_pin_lock_n < 3 ? g_pin_lock_n : 3);
        g_pin_lock_n++;
        g_pin_lock_until = now + w;
        g_pin_locked = true;
        Serial.printf("! Nhap sai PIN nhieu lan: khoa %lu giay\n", (unsigned long)(w / 1000));
        webJson(String("{\"ok\":false,\"wait\":") + String(w / 1000) + "}");
        return;
    }
    webJson(String("{\"ok\":false,\"left\":") + String(5 - g_pin_bad) + "}");
}

void webDrive() {   // /api/drive?l=-100..100&r=-100..100[&t=<ma>]
    if (!webAuth()) { webTuChoi(); return; }
    int l = constrain(g_web.arg("l").toInt(), -100, 100);
    int r = constrain(g_web.arg("r").toInt(), -100, 100);
    datToc(0, A_LA_BANH_TRAI ? l : r);
    datToc(1, A_LA_BANH_TRAI ? r : l);
    g_cmd_deadline = millis() + WEB_DEADMAN_MS;
    webJson("{\"ok\":true}");
}

void webStop() {
    dungHet();
    webJson("{\"ok\":true}");
}

void webStatus() {
    bool ok = webAuth();
    if (!ok) {   // chua dang nhap: chi cho biet phien ban va can PIN
        webJson(String("{\"ver\":\"") + FW_VERSION + "\",\"pin\":true,\"auth\":false}");
        return;
    }
    String j = String("{\"ver\":\"") + FW_VERSION + "\",\"pin\":" + (g_pin.length() ? "true" : "false") + ",\"auth\":true,\"ip\":\"" + WiFi.localIP().toString() +
               "\",\"ssid\":\"" + g_ssid + "\",\"rssi\":" + String(WiFi.RSSI()) +
               ",\"bat_mv\":" + String((int)g_bat_mv) +
               ",\"l\":" + String(A_LA_BANH_TRAI ? g_cmd[0] : g_cmd[1]) +
               ",\"r\":" + String(A_LA_BANH_TRAI ? g_cmd[1] : g_cmd[0]) +
               ",\"fault\":" + String((g_fault[0] || g_fault[1]) ? "true" : "false") + "}";
    webJson(j);
}

void webBatDau() {
    g_web.on("/", HTTP_GET, []() { g_web.sendHeader("Cache-Control", "no-store"); g_web.send_P(200, "text/html; charset=utf-8", PAGE_ROOT); });
    g_web.on("/api/drive", HTTP_GET, webDrive);
    g_web.on("/api/stop", HTTP_GET, webStop);
    g_web.on("/api/login", HTTP_GET, webLogin);
    g_web.on("/api/status", HTTP_GET, webStatus);
    g_web.onNotFound([]() { g_web.send(404, "text/plain", "Khong co trang nay"); });
    g_web.begin();
    g_web_started = true;
}

void mdnsBatDau() {
    MDNS.end();
    if (MDNS.begin(MDNS_TEN)) {
        MDNS.addService("http", "tcp", 80);
        g_mdns_started = true;
        Serial.printf("Web dieu khien: http://%s.local  (hoac http://%s)\n", MDNS_TEN, WiFi.localIP().toString().c_str());
    } else {
        Serial.println("Khong bat duoc rubi.local (dung dia chi IP)");
    }
}

// ---------------- Cap nhat tu GitHub (nhanh ota-devkit) ----------------
const bool TU_CAP_NHAT_KHI_BAT = true;   // true = moi lan bat nguon, noi duoc Wi-Fi thi tu kiem tra GitHub va tu nap ban moi
const char* FW_VERSION = "dev";   // GitHub Actions doi thanh 1.0.<so lan build>
const char* OTA_MANIFEST_URL = "https://raw.githubusercontent.com/quangtrung06021995-design/rubi-firmware/ota-devkit/manifest.json";

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

void githubCapNhat(bool ep) {
    if (WiFi.status() != WL_CONNECTED) { Serial.println("Chua co Wi-Fi. Go: wTenWifi|MatKhau"); return; }
    dungHet();                               // tat motor trong luc cap nhat
    Serial.printf("Phien ban dang chay: %s. Dang hoi GitHub...\n", FW_VERSION);

    String body;
    {
        WiFiClientSecure cl;
        cl.setInsecure();                    // khong kiem tra chung chi (xem ghi chu bao mat khi gui file)
        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        if (!http.begin(cl, OTA_MANIFEST_URL)) { Serial.println("Loi: khong mo duoc dia chi manifest"); return; }
        int code = http.GET();
        if (code != 200) { Serial.printf("Loi: GitHub tra ve ma %d (chua co ban nao hoac mat mang)\n", code); http.end(); return; }
        body = http.getString();
        http.end();
    }
    String ver = jsonGet(body, "version"), url = jsonGet(body, "url"), md5 = jsonGet(body, "md5");
    if (ver.length() == 0 || url.length() == 0) { Serial.println("Loi: manifest khong doc duoc"); return; }
    Serial.printf("Ban tren GitHub: %s\n", ver.c_str());
    if (!ep && ver == FW_VERSION) { Serial.println("Da la ban moi nhat. (Go u! de ep nap lai)"); return; }

    WiFiClientSecure cl2;
    cl2.setInsecure();
    HTTPClient h2;
    h2.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    h2.setTimeout(15000);
    if (!h2.begin(cl2, url)) { Serial.println("Loi: khong mo duoc dia chi file"); return; }
    int code2 = h2.GET();
    int len = h2.getSize();
    if (code2 != 200 || len <= 0) { Serial.printf("Loi tai file: ma %d, dung luong %d\n", code2, len); h2.end(); return; }
    if (!Update.begin(len)) { Serial.printf("Loi: khong du cho nap (%s)\n", Update.errorString()); h2.end(); return; }
    if (md5.length() == 32) Update.setMD5(md5.c_str());

    Serial.printf("Dang tai %d byte...\n", len);
    WiFiClient* st = h2.getStreamPtr();
    uint8_t buf[1024];
    int got = 0, lastPct = -1;
    uint32_t lastData = millis();
    bool loi = false;
    while (got < len) {
        size_t avail = st->available();
        if (avail) {
            size_t n = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            int r = st->readBytes(buf, n);
            if (r <= 0) continue;
            if (Update.write(buf, r) != (size_t)r) { Serial.printf("Loi ghi: %s\n", Update.errorString()); loi = true; break; }
            got += r;
            lastData = millis();
            int pct = (int)((int64_t)got * 100 / len);
            if (pct / 10 != lastPct / 10) { lastPct = pct; Serial.printf("  %d%%\n", pct); }
        } else {
            if (!h2.connected()) break;
            if (millis() - lastData > 20000) { Serial.println("Loi: het gio cho du lieu"); loi = true; break; }
            delay(2);
        }
    }
    h2.end();
    if (loi || got != len) {
        Update.abort();
        Serial.printf("Cap nhat that bai (nhan %d/%d byte). Ban cu van chay binh thuong.\n", got, len);
        return;
    }
    if (!Update.end(true)) {
        Serial.printf("Cap nhat that bai: %s. Ban cu van chay binh thuong.\n", Update.errorString());
        return;
    }
    Serial.printf("Cap nhat xong, len ban %s. Khoi dong lai...\n", ver.c_str());
    delay(500);
    ESP.restart();
}

// ---------------- Lenh tu may tinh ----------------
void xuLyLenh(String s) {
    s.trim();
    if (s.length() == 0) return;
    char c = s[0];
    int v = s.substring(1).toInt();
    switch (c) {
        case 't': thuTuDong(); break;
        case 'a': datToc(0, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
        case 'b': datToc(1, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
        case 'd': datToc(0, v); datToc(1, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
        case 's': dungHet(); Serial.println("Da dung"); break;
        case 'r': noInterrupts(); g_cnt[0] = g_cnt[1] = 0; interrupts(); Serial.println("Da dat lai dem"); break;
        case 'e': inDem(); break;
        case 'i': wifiTrangThai(); break;
        case 'v': Serial.printf("Phien ban: %s\n", FW_VERSION); break;
        case 'u': githubCapNhat(s.length() > 1 && s[1] == '!'); break;
        case 'l': wifiLiet(); break;
        case 'p': {
            String r = s.substring(1);
            r.trim();
            if (r == "-") { pinDat(""); Serial.println("Da xoa PIN: ai cung dieu khien duoc"); break; }
            if (r.length() == 0) { Serial.println(g_pin.length() ? "Dang co ma PIN (go p<4-8 so> de doi, p- de xoa)" : "Chua dat PIN (go p<4-8 so>, vi du p4826)"); break; }
            if (!pinHopLe(r)) { Serial.println("PIN la 4-8 chu so, vi du p4826"); break; }
            pinDat(r);
            Serial.println("Da dat ma PIN. Cac dien thoai phai nhap lai ma.");
            break;
        }
        case 'q': {
            int n = s.substring(1).toInt();
            if (n < 1 || n > g_nn) { Serial.println("Cu phap: q<so>  (xem so bang lenh l)"); break; }
            Serial.printf("Da quen \"%s\"\n", g_ns[n - 1].c_str());
            wifiQuenMuc(n - 1);
            break;
        }
        case 'w': {
            String r = s.substring(1);
            r.trim();
            if (r.length() == 0) { wifiTrangThai(); break; }
            if (r == "-") { wifiQuenHet(); Serial.println("Da xoa het Wi-Fi da nho"); break; }
            int k = r.indexOf('|');
            if (k <= 0) { Serial.println("Cu phap: wTenWifi|MatKhau  (dung dau | giua ten va mat khau)"); break; }
            wifiMangMoi(r.substring(0, k), r.substring(k + 1), false);
            break;
        }
        default: inHuongDan(); break;
    }
}

// ---------------- setup / loop ----------------
void setup() {
    // STBY thap truoc het: driver tat trong luc khoi dong
    pinMode(PIN_STBY, OUTPUT);
    digitalWrite(PIN_STBY, LOW);
    pinMode(PIN_AIN1, OUTPUT); pinMode(PIN_AIN2, OUTPUT);
    pinMode(PIN_BIN1, OUTPUT); pinMode(PIN_BIN2, OUTPUT);
    digitalWrite(PIN_AIN1, LOW); digitalWrite(PIN_AIN2, LOW);
    digitalWrite(PIN_BIN1, LOW); digitalWrite(PIN_BIN2, LOW);
    PWM_SETUP();
    PWM_WRITE(0, 0);
    PWM_WRITE(1, 0);

    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, 16, 17);   // RX=16, TX=17
    analogSetPinAttenuation(PIN_VBAT, ADC_11db);

    pinMode(PIN_E1A, INPUT); pinMode(PIN_E1B, INPUT);
    pinMode(PIN_E2A, INPUT); pinMode(PIN_E2B, INPUT);
    g_st[0] = (uint8_t)((digitalRead(PIN_E1A) << 1) | digitalRead(PIN_E1B));
    g_st[1] = (uint8_t)((digitalRead(PIN_E2A) << 1) | digitalRead(PIN_E2B));
    attachInterrupt(digitalPinToInterrupt(PIN_E1A), isrA, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_E1B), isrA, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_E2A), isrB, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_E2B), isrB, CHANGE);

    delay(300);
    digitalWrite(PIN_STBY, HIGH);   // bat driver (motor van dung vi PWM = 0)
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);           // OTA on dinh hon khi khong ngu Wi-Fi
    WiFi.setAutoReconnect(true);
    pinNap();
    wifiNapDS();
    wifiKetNoi();
    Serial.println();
    Serial.printf("Rubi DevKit san sang. Phien ban %s. Motor dang dung.\n", FW_VERSION);
    inHuongDan();
}

void loop() {
    static uint32_t last10 = 0;
    static String line, line2;
    uint32_t now = millis();
    if (now - last10 >= 10) {
        last10 += 10;
        if (now - last10 > 100) last10 = now;
        capNhatMotor();
    }
    doPin();
    wifiTick();
    if (TU_CAP_NHAT_KHI_BAT && !g_auto_done && g_auto_at != 0 && (int32_t)(millis() - g_auto_at) >= 0) {
        g_auto_done = true;   // chi thu mot lan moi lan bat nguon
        Serial.println("Tu kiem tra ban moi tren GitHub...");
        githubCapNhat(false);
    }
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') { xuLyLenh(line); line = ""; }
        else if (line.length() < 100) line += ch;
    }
    while (Serial2.available()) {   // lenh tu bo man hinh
        char ch = (char)Serial2.read();
        if (ch == '\n' || ch == '\r') {
            // chi nhan Wi-Fi ("w..." hoac "W<ten>\t<mk>\t<cs>") va cap nhat (u): chan RX2 de ho co the nhan nhieu rac, khong cho rac dieu khien motor
            String ws, wp;
            if (line2.length() > 4 && line2[0] == 'W') { if (phanTichW(line2, ws, wp)) wifiMangMoi(ws, wp, true); }
            else if (line2.length() > 2 && line2[0] == 'P') {   // P<PIN cu>|<PIN moi> tu man hinh: da co PIN thi phai dung PIN cu; tra "P1" (xong) hoac "P0" (sai)
                static uint8_t sai = 0; static uint32_t khoa_den = 0;
                int k = line2.indexOf('|');
                String cu = k > 1 ? line2.substring(1, k) : String(""), moi = k > 0 ? line2.substring(k + 1) : String("");
                if (k > 0 && (int32_t)(khoa_den - millis()) > 0) { Serial2.println("P0"); }
                else if (k > 0 && pinHopLe(moi) && (g_pin.length() == 0 || cu == g_pin)) { pinDat(moi); sai = 0; Serial.println("PIN doi tu man hinh"); Serial2.println("P1"); }
                else { if (++sai >= 5) { sai = 0; khoa_den = millis() + 30000; } Serial2.println("P0"); }
            }
            else if ((line2.length() > 1 && line2[0] == 'w') || line2 == "u") {
                if (line2[0] == 'w') {   // dang w<ten>|<mk> tu man hinh: xu ly nhu mang moi (lap lai thi im lang)
                    String r = line2.substring(1); r.trim();
                    int k = r.indexOf('|');
                    if (k > 0) wifiMangMoi(r.substring(0, k), r.substring(k + 1), true);
                } else xuLyLenh(line2);
            }
            line2 = "";
        }
        else if (line2.length() < 120) line2 += ch;
    }
    delay(1);
}
