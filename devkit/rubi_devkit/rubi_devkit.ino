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
#include <NimBLEDevice.h>   // Bluetooth cho dong ho cua be (thu vien NimBLE-Arduino 1.4.3)
#include <time.h>

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
const int PIN_YEU_PCT = 30;              // pin duoi muc nay -> chay cham lai (do sut ap khi motor khoi dong)
const int PIN_YEU_THOAT_PCT = 40;        // chi tro lai toc do thuong khi pin (luc motor dung) len tren muc nay
const int GIOI_HAN_PIN_YEU = 60;         // % toc do toi da khi pin yeu
const int SLEW_PIN_YEU = 1;              // tang toc cham hon khi pin yeu (SLEW_STEP binh thuong)
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
extern String g_pin;   // dinh nghia o duoi (PIN dieu khien)
bool g_nhay = false;   // dang nhay (lenh "D" tu man hinh); lenh lai xe bat ky se huy
bool g_pin_thap = false;
bool g_pin_yeu = false;      // pin duoi PIN_YEU_PCT: chay cham

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
    if (g_pin_yeu && a > GIOI_HAN_PIN_YEU) a = GIOI_HAN_PIN_YEU;
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
    g_nhay = false;
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
        int buoc = g_pin_yeu ? SLEW_PIN_YEU : SLEW_STEP;
        if (g_cmd[ch] < t) g_cmd[ch] = min(g_cmd[ch] + buoc, t);
        else if (g_cmd[ch] > t) g_cmd[ch] = max(g_cmd[ch] - buoc, t);
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

// pack mV (2S) -> % (cung duong cong voi man hinh)
int pinPhanTram(float pack_mv) {
    static const float v[] = {3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
    static const float p[] = {0, 5, 10, 20, 35, 50, 65, 80, 90, 100};
    float c = pack_mv / 2.0f;
    if (c <= v[0]) return 0;
    if (c >= v[9]) return 100;
    for (int i = 1; i < 10; i++)
        if (c <= v[i]) return (int)(p[i - 1] + (p[i] - p[i - 1]) * (c - v[i - 1]) / (v[i] - v[i - 1]) + 0.5f);
    return 100;
}

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
        if (g_bat_mv > 4000) {
            int pc = pinPhanTram(g_bat_mv);
            bool dung = (g_cmd[0] == 0 && g_cmd[1] == 0);   // chi do "luc nghi" moi tin de thoat che do cham
            if (pc < PIN_YEU_PCT) g_pin_yeu = true;
            else if (pc >= PIN_YEU_THOAT_PCT && dung) g_pin_yeu = false;
        }
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
uint32_t g_upd_ds_den = 0;   // bo man hinh bao dang cap nhat ("U1"), het han sau 60 s neu khong nhac lai
bool g_upd_ds = false;
String g_c3_v; uint32_t g_c3_at = 0;   // phien ban bo C3 Zero (den LED) bao qua UART1
String g_ds_v;                  // phien ban dang chay cua bo man hinh ("V3.0.86" qua UART)
uint32_t g_ds_at = 0;
String g_cam_v;                 // phien ban camera (camera tu bao qua /api/cam)
bool g_cam_u = false;
uint32_t g_cam_at = 0;
// Danh sach truyen tren the nho (bo man hinh gui: "TC<n>" roi "T<thu muc>\t<ten>")
#define MAX_TRUYEN 16
String g_ty_id[MAX_TRUYEN], g_ty_ten[MAX_TRUYEN];
int g_ty_n = 0, g_ty_dang = 0;
bool g_ty_co = false;
// Danh sach bai hat tren the nho (bo man hinh gui: "YC<n>" roi "Y<so thu tu>\t<ten>")
#define MAX_NHAC 30
String g_nh_ten[MAX_NHAC];
int g_nh_n = 0, g_nh_dang = 0;
bool g_nh_co = false;
// Dung luong the nho (bo man hinh gui: "S<tong_kb>\t<con_trong_kb>\t<nhac_kb>\t<nhac_n>\t<truyen_kb>\t<truyen_n>\t<hoc_kb>\t<hoc_n>")
uint32_t g_sd[8];
uint32_t g_sd_at = 0;
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

// ---------------- Nhay mua ----------------
// Chi XOAY TAI CHO (hai banh nguoc chieu) nen robot khong tien/lui, khong roi khoi ban. Moi vong ~9 giay, 3 vong roi tu dung.
struct BuocNhay { int8_t l, r; uint16_t ms; };
const BuocNhay NHAY[] = {
    {45, -45, 700}, {-45, 45, 700}, {0, 0, 150},
    {50, -50, 220}, {-50, 50, 220}, {50, -50, 220}, {-50, 50, 220}, {0, 0, 150},
    {55, -55, 1300}, {0, 0, 250}, {-55, 55, 1300}, {0, 0, 400},
};
const int NHAY_SO_BUOC = sizeof(NHAY) / sizeof(NHAY[0]);
const int NHAY_SO_VONG = 65;   // moi vong ~9 giay: nhay den khi bam Dung, toi da ~10 phut cho an toan
int g_nhay_buoc = 0, g_nhay_vong = 0;
uint32_t g_nhay_den = 0;

void nhayDatBuoc(int i) {
    int l = NHAY[i].l, r = NHAY[i].r;
    datToc(0, A_LA_BANH_TRAI ? l : r);
    datToc(1, A_LA_BANH_TRAI ? r : l);
    g_nhay_den = millis() + NHAY[i].ms;
    g_cmd_deadline = millis() + NHAY[i].ms + 1500;   // neu vong lap ket thi motor van tu dung
}
void nhayBat() {
    if (g_pin_thap) { Serial.println("! Pin yeu, khong nhay"); return; }
    for (int ch = 0; ch < 2; ch++) g_fault[ch] = false;
    g_nhay = true; g_nhay_buoc = 0; g_nhay_vong = 0;
    nhayDatBuoc(0);
    Serial.println("Bat dau nhay");
}
void nhayTick() {   // goi moi 10 ms
    if (!g_nhay) return;
    if (g_fault[0] || g_fault[1]) { dungHet(); Serial.println("Nhay dung: motor ket"); return; }
    if (g_pin_thap) { dungHet(); Serial.println("Nhay dung: pin yeu"); return; }
    if ((int32_t)(millis() - g_nhay_den) < 0) return;
    if (++g_nhay_buoc >= NHAY_SO_BUOC) {
        g_nhay_buoc = 0;
        if (++g_nhay_vong >= NHAY_SO_VONG) { dungHet(); Serial.println("Nhay xong"); return; }
    }
    nhayDatBuoc(g_nhay_buoc);
}
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
<div id="basic"><h1>Rubi</h1><p id="st">Đang tải giao diện đầy đủ...</p>
<div class="g">
<span></span><button data-l="60" data-r="60">▲</button><span></span>
<button data-l="-45" data-r="45">◀</button><button class="s" data-l="0" data-r="0">■</button><button data-l="45" data-r="-45">▶</button>
<span></span><button data-l="-50" data-r="-50">▼</button><span></span></div></div>
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
(function(){
 var basic=document.getElementById('basic'),links=[],box=null,done=false;
 function fail(){if(done)return;done=true;links.forEach(function(l){l.remove()});if(box)box.remove();basic.style.display='';document.getElementById('st').textContent='Chế độ cơ bản (chưa tải được giao diện đầy đủ)'}
 var c=new AbortController(),t=setTimeout(function(){c.abort()},6000);
 fetch(UI+'index.html',{signal:c.signal,cache:'no-cache'}).then(function(r){if(!r.ok)throw 0;return r.text()}).then(function(h){
  clearTimeout(t);
  var d=new DOMParser().parseFromString(h,'text/html');
  d.querySelectorAll('link[rel=stylesheet]').forEach(function(l){var n=document.createElement('link');n.rel='stylesheet';n.href=new URL(l.getAttribute('href'),UI).href;document.head.appendChild(n);links.push(n)});
  var sc=[].map.call(d.body.querySelectorAll('script'),function(x){var u=x.getAttribute('src');x.remove();return u}).filter(function(u){return u});
  d.body.querySelectorAll('[src]').forEach(function(e){var u=e.getAttribute('src');if(u&&!/^(https?:|data:|#)/.test(u))e.setAttribute('src',new URL(u,UI).href)});
  box=document.createElement('div');box.id='ui';box.innerHTML=d.body.innerHTML;document.body.appendChild(box);
  basic.style.display='none';
  var s=document.createElement('script');s.src=new URL(sc[0],UI).href;
  s.onload=function(){setTimeout(function(){if(!document.querySelector('#ui .view.on'))fail();else done=true},1500)};
  s.onerror=fail;document.body.appendChild(s);
 }).catch(fail)})();
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

// ---- Den LED (bo ESP32-C3 Zero tren robot, UART1: TX=GPIO19 -> RX C3 Zero GPIO20, RX=GPIO18 <- TX C3 Zero GPIO21) ----
uint8_t g_ledMask = 0x0F;      // bit0 nguc, bit1 chan bun trai, bit2 chan bun phai, bit3 hai ang-ten
uint8_t g_ledColor = 0;        // 0 xanh duong, 1 hong, 2 vang, 3 luc, 4 cau vong
uint8_t g_ledEff = 0;          // 0 dung yen, 1 nhay cham
uint32_t g_ledSentAt = 0;
void bleGui(const String& msg);

void ledGui() { Serial1.printf("L%d,%d,%d\n", g_ledMask, g_ledColor, g_ledEff); g_ledSentAt = millis(); }

void ledLuu() {
    g_pref.begin("rubi", false);
    g_pref.putUChar("ledm", g_ledMask);
    g_pref.putUChar("ledc", g_ledColor);
    g_pref.end();
}

void ledNap() {
    g_pref.begin("rubi", true);
    g_ledMask = g_pref.getUChar("ledm", 0x0F) & 0x0F;
    g_ledColor = g_pref.getUChar("ledc", 0);
    if (g_ledColor > 4) g_ledColor = 0;
    g_pref.end();
}

void ledDat(int m, int c, int e) {   // am = giu nguyen
    if (m >= 0) g_ledMask = m & 0x0F;
    if (c >= 0 && c <= 4) g_ledColor = c;
    if (e >= 0 && e <= 1) g_ledEff = e;
    ledLuu();
    ledGui();
    bleGui(String("E") + g_ledMask);
    bleGui(String("Y") + g_ledColor + "," + g_ledEff);
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
    g_nhay = false;
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
static String jsonThoat(const String& v) {
    String o;
    for (unsigned i = 0; i < v.length(); i++) {
        char c = v[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c >= 32) o += c;
    }
    return o;
}

void webTruyen() {   // /api/truyen : danh sach truyen tren the nho; /api/truyen?mo=<thu muc> : mo truyen do tren man hinh Rubi
    if (!webAuth()) { webTuChoi(); return; }
    String mo = g_web.arg("mo");
    if (mo.length()) {
        for (int i = 0; i < g_ty_n; i++) {
            if (g_ty_id[i] == mo) {
                dungHet();
                Serial2.printf("O%s\n", mo.c_str());
                webJson("{\"ok\":true}");
                return;
            }
        }
        webJson("{\"ok\":false,\"err\":\"khong thay truyen\"}");
        return;
    }
    String j = String("{\"ok\":true,\"co\":") + (g_ty_co ? "true" : "false") + ",\"list\":[";
    for (int i = 0; i < g_ty_n; i++) {
        if (i) j += ",";
        j += String("{\"id\":\"") + jsonThoat(g_ty_id[i]) + "\",\"t\":\"" + jsonThoat(g_ty_ten[i]) + "\"}";
    }
    j += "]}";
    webJson(j);
}

void webNhac() {   // /api/nhac : danh sach bai hat tren the nho
    if (!webAuth()) { webTuChoi(); return; }
    String j = String("{\"ok\":true,\"co\":") + (g_nh_co ? "true" : "false") + ",\"list\":[";
    for (int i = 0; i < g_nh_n; i++) {
        if (i) j += ",";
        j += String("{\"i\":") + i + ",\"t\":\"" + jsonThoat(g_nh_ten[i]) + "\"}";
    }
    j += "]}";
    webJson(j);
}

void webLenh() {   // /api/lenh?c=<lenh> : chuyen lenh cho bo man hinh (sleep, clock, t5, m1, h2, p3, huy, vup, vdown)
    if (!webAuth()) { webTuChoi(); return; }
    String c = g_web.arg("c");
    bool ok = c.length() >= 1 && c.length() <= 10;
    for (unsigned i = 0; ok && i < c.length(); i++) ok = isalnum((unsigned char)c[i]);
    if (ok) {
        char k = c[0];
        ok = c == "sleep" || c == "clock" || c == "en" || c == "huy" || c == "vup" || c == "vdown" || k == 't' || k == 'm' || k == 'h' || k == 'p';
    }
    if (!ok) { webJson("{\"ok\":false,\"err\":\"lenh\"}"); return; }
    dungHet();
    Serial2.printf("C%s\n", c.c_str());
    webJson("{\"ok\":true}");
}

void webLed() {   // /api/led?m=<0-15>&c=<0-4>&e=<0|1> (thieu tham so nao thi giu nguyen); m: 1 nguc, 2 chan bun trai, 4 chan bun phai, 8 ang-ten
    if (!webAuth()) { webTuChoi(); return; }
    int m = g_web.hasArg("m") ? constrain(g_web.arg("m").toInt(), 0, 15) : -1;
    int c = g_web.hasArg("c") ? constrain(g_web.arg("c").toInt(), 0, 4) : -1;
    int e = g_web.hasArg("e") ? constrain(g_web.arg("e").toInt(), 0, 1) : -1;
    if (m >= 0 || c >= 0 || e >= 0) ledDat(m, c, e);
    webJson(String("{\"ok\":true,\"led\":") + g_ledMask + ",\"lc\":" + g_ledColor + ",\"le\":" + g_ledEff + "}");
}

void webDance() {   // /api/dance[?t=<ma>] : spin on the spot for ~28 s, /api/stop or any drive command cancels
    if (!webAuth()) { webTuChoi(); return; }
    if (g_pin_thap) { webJson("{\"ok\":false,\"err\":\"pin\"}"); return; }
    nhayBat();
    webJson("{\"ok\":true}");
}

String updJson() {
    if (g_upd_ds && (int32_t)(g_upd_ds_den - millis()) <= 0) g_upd_ds = false;
    bool camMoi = g_cam_at != 0 && millis() - g_cam_at < 40000;
    if (g_upd_ds) return String(",\"upd\":\"ds\"");
    if (camMoi && g_cam_u) return String(",\"upd\":\"cam\"");
    return String("");
}

String sdJson() {   // dung luong the nho neu bo man hinh bao trong 3 phut gan day: "sd":[tong_kb,trong_kb,nhac_kb,nhac_n,truyen_kb,truyen_n,hoc_kb,hoc_n]
    if (g_sd_at == 0 || millis() - g_sd_at > 180000) return String("");
    String j = ",\"sd\":[";
    for (int i = 0; i < 8; i++) { if (i) j += ","; j += String((unsigned long)g_sd[i]); }
    return j + "]";
}

String phienBanJson() {   // phien ban dang chay cua bo man hinh va camera (neu gan day co bao)
    String j;
    if (g_ds_at != 0 && millis() - g_ds_at < 120000) j += String(",\"dsv\":\"") + g_ds_v + "\"";
    if (g_cam_at != 0 && millis() - g_cam_at < 120000) j += String(",\"cv\":\"") + g_cam_v + "\"";
    return j;
}

void webCam() {   // /api/cam?v=<ban>&u=<0|1> : camera tu bao phien ban va dang cap nhat hay khong
    String v = g_web.arg("v");
    bool ok = v.length() > 0 && v.length() < 16;
    for (unsigned i = 0; ok && i < v.length(); i++) ok = isalnum((unsigned char)v[i]) || v[i] == '.' || v[i] == '-';
    if (ok) { g_cam_v = v; g_cam_u = g_web.arg("u") == "1"; g_cam_at = millis() | 1; }
    webJson("{\"ok\":true}");
}

String tgJson();   // dinh nghia o phan Telegram phia duoi
void webStatus() {
    bool ok = webAuth();
    if (!ok) {   // chua dang nhap: chi cho biet phien ban va can PIN
        webJson(String("{\"ver\":\"") + FW_VERSION + "\",\"pin\":true,\"auth\":false" + updJson() + "}");
        return;
    }
    String j = String("{\"ver\":\"") + FW_VERSION + "\",\"pin\":" + (g_pin.length() ? "true" : "false") + ",\"auth\":true,\"ip\":\"" + WiFi.localIP().toString() +
               "\",\"ssid\":\"" + g_ssid + "\",\"rssi\":" + String(WiFi.RSSI()) + updJson() + phienBanJson() + sdJson() +
               ",\"bat_mv\":" + String((int)g_bat_mv) + tgJson() +
               ",\"led\":" + String(g_ledMask) + ",\"lc\":" + String(g_ledColor) +
               ((g_c3_at != 0 && millis() - g_c3_at < 120000) ? String(",\"c3v\":\"") + g_c3_v + "\"" : String("")) +
               ",\"l\":" + String(A_LA_BANH_TRAI ? g_cmd[0] : g_cmd[1]) +
               ",\"r\":" + String(A_LA_BANH_TRAI ? g_cmd[1] : g_cmd[0]) +
               ",\"fault\":" + String((g_fault[0] || g_fault[1]) ? "true" : "false") + "}";
    webJson(j);
}


extern int g_tzMin;

// ---------------- Thong bao cho BO ME qua Telegram ----------------
// Token bot + ma cuoc tro chuyen luu trong bo nho cua DevKit (KHONG nam trong code / GitHub). Cai o trang Cai dat cua web Rubi.
// Tin duoc dua vao hang doi va gui bang mot tac vu rieng nen khong lam dung vong lap lai xe.
String g_tgToken, g_tgChat, g_tgBot;
QueueHandle_t g_tgQ = nullptr;
bool g_tgDaThay = false;      // dong ho da tung ket noi tu luc bat nguon
bool g_tgXa = false;          // da bao "mat ket noi" (be ra xa) va chua bao quay lai
uint32_t g_tgMatLuc = 0, g_tgBaoXaLuc = 0;
const uint32_t TG_XA_SAU_MS = 90000;       // mat ket noi tu 90 giay moi coi la be ra xa
const uint32_t TG_XA_CACH_MS = 300000;     // hai tin "ra xa" cach nhau it nhat 5 phut

bool tgHopLeToken(const String& t) {
    if (t.length() < 20 || t.length() > 70) return false;
    for (unsigned i = 0; i < t.length(); i++) { char c = t[i]; if (!(isalnum((unsigned char)c) || c == ':' || c == '_' || c == '-')) return false; }
    return t.indexOf(':') > 0;
}
bool tgCoCauHinh() { return g_tgToken.length() > 0 && g_tgChat.length() > 0; }
String tgJson() { return String(",\"tg\":") + (tgCoCauHinh() ? "true" : "false") + ",\"tgtok\":" + (g_tgToken.length() ? "true" : "false") + ",\"tgbot\":\"" + g_tgBot + "\""; }
void tgNap() {
    g_pref.begin("rubitg", true);
    g_tgToken = g_pref.getString("tok", "");
    g_tgChat = g_pref.getString("chat", "");
    g_tgBot = g_pref.getString("bot", "");
    g_pref.end();
}
void tgLuu() {
    g_pref.begin("rubitg", false);
    g_pref.putString("tok", g_tgToken);
    g_pref.putString("chat", g_tgChat);
    g_pref.putString("bot", g_tgBot);
    g_pref.end();
}
String tgMaHoa(const String& s) {   // ma hoa % cho form (UTF-8 giu nguyen tung byte)
    String o;
    char b[4];
    for (unsigned i = 0; i < s.length(); i++) {
        uint8_t c = (uint8_t)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
        else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
    }
    return o;
}
String tgGio() {
    time_t t = time(nullptr);
    if (t < 1700000000) return String("");
    t += (time_t)g_tzMin * 60;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return String(b);
}
void tgGuiTin(const String& msg) {   // dua vao hang doi; khong chan
    if (!g_tgQ || !tgCoCauHinh()) return;
    char buf[200];
    size_t n = msg.length() < 199 ? msg.length() : 199;
    memcpy(buf, msg.c_str(), n);
    buf[n] = 0;
    xQueueSend(g_tgQ, buf, 0);
}
static int tgHttp(const String& url, const String* body, String* out) {   // tra ma HTTP (<0 la loi mang)
    WiFiClientSecure cl;
    cl.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(6000);
    if (!http.begin(cl, url)) return -1;
    int code;
    if (body) { http.addHeader("Content-Type", "application/x-www-form-urlencoded"); code = http.POST(*body); }
    else code = http.GET();
    if (out && code > 0) *out = http.getString();
    http.end();
    return code;
}
void tgTask(void*) {
    char buf[200];
    for (;;) {
        if (xQueueReceive(g_tgQ, buf, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        bool xong = false;
        for (int lan = 0; lan < 40 && !xong; lan++) {   // chua co Wi-Fi/Internet thi thu lai toi ~3 phut
            if (!tgCoCauHinh()) break;
            if (WiFi.status() == WL_CONNECTED) {
                String body = String("chat_id=") + tgMaHoa(g_tgChat) + "&text=" + tgMaHoa(String(buf));
                int code = tgHttp(String("https://api.telegram.org/bot") + g_tgToken + "/sendMessage", &body, nullptr);
                Serial.printf("Telegram gui tin: HTTP %d\n", code);
                if (code == 200 || code == 400 || code == 401 || code == 403 || code == 404) xong = true;   // sai cau hinh thi bo, khong thu lai
            }
            if (!xong) vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}
void tgBatDau() {
    tgNap();
    g_tgQ = xQueueCreate(4, 200);
    xTaskCreate(tgTask, "tg", 12288, nullptr, 1, nullptr);
}
// goi tu bleTick: cn = so thiet bi BLE dang noi, auth = dong ho da dang nhap
void tgSuKien(int cn, bool auth, uint32_t now) {
    if (auth && cn > 0) {
        g_tgDaThay = true;
        g_tgMatLuc = 0;
        if (g_tgXa) {
            g_tgXa = false;
            String g = tgGio();
            tgGuiTin(String("\xE2\x9C\x85 B\xC3\xA9 \xC4\x91\xC3\xA3 quay l\xE1\xBA\xA1i g\xE1\xBA\xA7n Rubi (\xC4\x91\xE1\xBB\x93ng h\xE1\xBB\x93 \xC4\x91\xC3\xA3 k\xE1\xBA\xBFt n\xE1\xBB\x91i l\xE1\xBA\xA1i)") + (g.length() ? String(" l\xC3\xBAc ") + g : String("")) + ".");
        }
        return;
    }
    if (cn == 0 && g_tgDaThay && !g_tgXa) {
        if (g_tgMatLuc == 0) g_tgMatLuc = now | 1;
        else if (now - g_tgMatLuc >= TG_XA_SAU_MS && (g_tgBaoXaLuc == 0 || now - g_tgBaoXaLuc >= TG_XA_CACH_MS)) {
            g_tgXa = true;
            g_tgBaoXaLuc = now | 1;
            String g = tgGio();
            tgGuiTin(String("\xE2\x9A\xA0\xEF\xB8\x8F \xC4\x90\xE1\xBB\x93ng h\xE1\xBB\x93 c\xE1\xBB\xA7a b\xC3\xA9 \xC4\x91\xC3\xA3 m\xE1\xBA\xA5t k\xE1\xBA\xBFt n\xE1\xBB\x91i v\xE1\xBB\x9Bi Rubi") + (g.length() ? String(" t\xE1\xBB\xAB ") + g : String("")) + ". B\xC3\xA9 c\xC3\xB3 th\xE1\xBB\x83 \xC4\x91ang \xE1\xBB\x9F xa Rubi (Bluetooth ch\xE1\xBB\x89 xa kho\xE1\xBA\xA3ng 10-20 m) ho\xE1\xBA\xB7c \xC4\x91\xE1\xBB\x93ng h\xE1\xBB\x93 h\xE1\xBA\xBFt pin/t\xE1\xBA\xAFt.");
        }
    }
}

String tgTimChat(const String& tok) {   // doc tin nhan moi nhat gui cho bot de lay ma cuoc tro chuyen
    String r;
    int code = tgHttp(String("https://api.telegram.org/bot") + tok + "/getUpdates?limit=1&offset=-1", nullptr, &r);
    if (code != 200) return String("");
    int i = r.indexOf("\"chat\":{\"id\":");
    if (i < 0) return String("");
    i += 13;
    String id;
    while (i < (int)r.length() && (isdigit((unsigned char)r[i]) || (r[i] == '-' && id.length() == 0)) && id.length() < 20) id += r[i++];
    return id;
}
void webTg() {   // /api/tg?token=<token>  luu token, tu tim ma cuoc tro chuyen (be/me phai nhan tin cho bot truoc); /api/tg?off=1 xoa
    if (!webAuth()) { webTuChoi(); return; }
    if (g_web.hasArg("off")) {
        g_tgToken = ""; g_tgChat = ""; g_tgBot = ""; g_tgXa = false; tgLuu();
        webJson("{\"ok\":true,\"tg\":false}");
        return;
    }
    String tok = g_web.arg("token");
    tok.trim();
    if (!tgHopLeToken(tok)) { webJson("{\"ok\":false,\"err\":\"token\"}"); return; }
    if (WiFi.status() != WL_CONNECTED) { webJson("{\"ok\":false,\"err\":\"wifi\"}"); return; }
    dungHet();   // cho nay chan vong lap 1-3 giay: dung robot cho an toan
    String r;
    int code = tgHttp(String("https://api.telegram.org/bot") + tok + "/getMe", nullptr, &r);
    if (code != 200) { webJson(String("{\"ok\":false,\"err\":\"") + (code == 401 || code == 404 ? "token" : "mang") + "\"}"); return; }
    String bot;
    int i = r.indexOf("\"username\":\"");
    if (i >= 0) { i += 12; int j = r.indexOf('"', i); if (j > i && j - i < 40) bot = r.substring(i, j); }
    String chat = tgTimChat(tok);
    g_tgToken = tok; g_tgBot = bot;
    if (chat.length()) g_tgChat = chat;
    tgLuu();
    webJson(String("{\"ok\":true,\"tg\":") + (tgCoCauHinh() ? "true" : "false") + ",\"bot\":\"" + bot + "\",\"chat\":" + (tgCoCauHinh() ? "true" : "false") + "}");
}
void webTgTest() {   // /api/tgtest : gui thu mot tin
    if (!webAuth()) { webTuChoi(); return; }
    if (!tgCoCauHinh()) { webJson("{\"ok\":false,\"err\":\"chua\"}"); return; }
    tgGuiTin(String("\xF0\x9F\xA4\x96 Rubi: tin g\xE1\xBB\xADi th\xE1\xBB\xAD th\xC3\xA0nh c\xC3\xB4ng. T\xE1\xBB\xAB gi\xE1\xBB\x9D b\xE1\xBB\x91 m\xE1\xBA\xB9 s\xE1\xBA\xBD nh\xE1\xBA\xADn th\xC3\xB4ng b\xC3\xA1o t\xE1\xBA\xA1i \xC4\x91\xC3\xA2y."));
    webJson("{\"ok\":true}");
}


extern bool g_bleAuth;
// ---------------- Thoi gian bieu cua BE ----------------
// Toi da 10 viec moi ngay (gio HHMM + loai 0-7: danh rang, an com, hoc bai, di tam, di ngu, don do choi, uong nuoc, doc sach).
// Den gio: Rubi noi nhac (lenh "Cn<loai>" sang bo man hinh), dong ho hien "DEN GIO ..." (BLE "R<loai>") va cho be bam XONG ("D").
// Sau 5 phut chua XONG thi nhac them 1 lan, sau 10 phut thi thoi. Can co gio (Wi-Fi/NTP hoac dong ho) moi chay.
const int TB_MAX = 10;
int g_tbGio[TB_MAX];
int g_tbLoai[TB_MAX];
long g_tbNgay[TB_MAX];
int g_tbN = 0;
int g_tbCho = -1;
uint32_t g_tbChoLuc = 0;
int g_tbLan = 0;

String tbDanhSach() {
    String o;
    for (int i = 0; i < g_tbN; i++) {
        char b[12];
        snprintf(b, sizeof(b), "%04d:%d", g_tbGio[i], g_tbLoai[i]);
        if (i) o += ',';
        o += b;
    }
    return o;
}
bool tbDat(const String& spec, bool luu) {   // "2030:0,2100:4"; sai dinh dang thi giu nguyen danh sach cu
    int gio[TB_MAX], loai[TB_MAX], n = 0;
    int i = 0, len = spec.length();
    while (i < len) {
        int j = spec.indexOf(',', i);
        if (j < 0) j = len;
        String tok = spec.substring(i, j);
        tok.trim();
        if (tok.length()) {
            int v = -1, k = -1;
            if (sscanf(tok.c_str(), "%d:%d", &v, &k) != 2) return false;
            if (v < 0 || v > 2359 || v % 100 > 59 || k < 0 || k > 7 || n >= TB_MAX) return false;
            gio[n] = v; loai[n] = k; n++;
        }
        i = j + 1;
    }
    for (int a = 1; a < n; a++) {   // xep theo gio
        int g = gio[a], l = loai[a], b = a - 1;
        while (b >= 0 && gio[b] > g) { gio[b + 1] = gio[b]; loai[b + 1] = loai[b]; b--; }
        gio[b + 1] = g; loai[b + 1] = l;
    }
    for (int a = 0; a < n; a++) { g_tbGio[a] = gio[a]; g_tbLoai[a] = loai[a]; g_tbNgay[a] = -1; }
    g_tbN = n;
    if (luu) { g_pref.begin("rubitb", false); g_pref.putString("ds", tbDanhSach()); g_pref.end(); Serial2.printf("Z%s\n", tbDanhSach().c_str()); }
    return true;
}
void tbNap() {
    g_pref.begin("rubitb", true);
    String s = g_pref.getString("ds", "");
    g_pref.end();
    tbDat(s, false);
}
void tbNhac(int k) {
    g_tbCho = k; g_tbChoLuc = millis() | 1; g_tbLan = 1;
    Serial2.printf("Cn%d\n", k);
    if (g_bleAuth) bleGui(String("R") + k);
    Serial.printf("Thoi gian bieu: nhac viec %d\n", k);
}
void tbKhiNoi() { if (g_tbCho >= 0) bleGui(String("R") + g_tbCho); }   // dong ho vua ket noi ma con viec chua xong
void tbXong() { if (g_tbCho >= 0) { Serial.printf("Be bam XONG viec %d\n", g_tbCho); g_tbCho = -1; } }
void tbTick(uint32_t now) {
    static uint32_t last = 0, guiMan = 0;
    if (now - guiMan >= 30000 || guiMan == 0) { guiMan = now | 1; Serial2.printf("Z%s\n", tbDanhSach().c_str()); }   // bo man hinh Rubi hien danh sach trong menu
    if (now - last < 1000) return;
    last = now;
    time_t t = time(nullptr);
    if (t < 1700000000) return;
    long loc = (long)t + (long)g_tzMin * 60;
    long ngay = loc / 86400;
    int sec = (int)(loc % 86400);
    int hhmm = (sec / 3600) * 100 + (sec % 3600) / 60;
    for (int i = 0; i < g_tbN; i++)
        if (g_tbGio[i] == hhmm && g_tbNgay[i] != ngay) { g_tbNgay[i] = ngay; tbNhac(g_tbLoai[i]); }
    if (g_tbCho >= 0) {
        if (g_tbLan < 2 && now - g_tbChoLuc >= 300000UL) {
            g_tbLan = 2;
            Serial2.printf("Cn%d\n", g_tbCho);
            if (g_bleAuth) bleGui(String("R") + g_tbCho);
        } else if (now - g_tbChoLuc >= 600000UL) g_tbCho = -1;
    }
}
void webTb() {   // /api/tb[?set=2030:0,2100:4] : xem hoac dat thoi gian bieu
    if (!webAuth()) { webTuChoi(); return; }
    if (g_web.hasArg("set") && !tbDat(g_web.arg("set"), true)) { webJson("{\"ok\":false,\"err\":\"dinh dang\"}"); return; }
    webJson(String("{\"ok\":true,\"ds\":\"") + tbDanhSach() + "\",\"gio\":" + (time(nullptr) > 1700000000 ? "true" : "false") + "}");
}
void webTbTest() {   // /api/tbtest?k=0..7 : nhac thu ngay
    if (!webAuth()) { webTuChoi(); return; }
    int k = g_web.arg("k").toInt();
    if (k < 0 || k > 7) { webJson("{\"ok\":false}"); return; }
    tbNhac(k);
    webJson("{\"ok\":true}");
}

// ---------------- Bluetooth cho DONG HO CUA BE ----------------
// Dong ho noi vao DevKit (BLE), gui lenh dang chu, cac lenh tuong duong web dieu khien:
//   K<khoa>    dang nhap (khoa = WATCH_KEY, phai trung firmware dong ho); chua dang nhap thi bo qua moi lenh khac
//   L<l>,<r>   lai xe (-100..100), het 0.5 giay khong co lenh moi thi tu dung (nhu web)
//   X          dung               A   nhay mua
//   C<lenh>    lenh cho bo man hinh (sleep, clock, en, huy, vup, vdown, t5, m0-m3, h0-h3, p<n>)
//   T<n>       mo truyen thu n (tinh tu 0)
//   W          xin Wi-Fi dang dung de dong ho tu cap nhat (tra "w<ten>\t<mat khau>")
//   Q          xin tinh trang ngay
// DevKit bao ve dong ho (moi 2 giay): "S<pin%>,<so bai>,<so truyen>,<wifi 0/1>"; "T<epoch>,<phut lech mui gio>" moi 20 giay.
// Mat ket noi thi robot tu dung.
const char* WATCH_KEY = "RB4821WT";
#define BLE_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define BLE_TX  "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
NimBLEServer* g_bleSrv = nullptr;
NimBLECharacteristic* g_bleTx = nullptr;
QueueHandle_t g_bleQ = nullptr;
bool g_bleAuth = false;
int g_bleConnPrev = 0;
uint32_t g_bleStatAt = 0, g_bleTimeAt = 0, g_bleAdvAt = 0;
int g_tzMin = 420;            // phut lech so voi UTC (lay tu Vercel theo vi tri mang; mac dinh Viet Nam)
bool g_timeCfg = false;
uint32_t g_tzNext = 0;

class BleRxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        char buf[48];
        size_t n = v.size() < 47 ? v.size() : 47;
        memcpy(buf, v.data(), n);
        buf[n] = 0;
        if (g_bleQ) xQueueSend(g_bleQ, buf, 0);
    }
};

void bleGui(const String& msg) {   // gui 1 dong, cat thanh khuc 20 byte, ket thuc bang \n
    if (!g_bleTx || !g_bleSrv || g_bleSrv->getConnectedCount() == 0) return;
    String m = msg + "\n";
    for (unsigned i = 0; i < m.length(); i += 20) {
        String part = m.substring(i, i + 20);
        g_bleTx->setValue((uint8_t*)part.c_str(), part.length());
        g_bleTx->notify();
        delay(4);
    }
}

void bleTrangThai() {
    int pc = -1;
    if (CO_MODULE_DO_PIN && g_bat_mv > 4000) pc = pinPhanTram(g_bat_mv);
    bleGui(String("S") + pc + "," + g_nh_n + "," + g_ty_n + "," + (WiFi.status() == WL_CONNECTED ? 1 : 0));
}

void bleGioGui() {
    time_t t = time(nullptr);
    if (t > 1700000000) bleGui(String("T") + (long)t + "," + g_tzMin);
}

void bleLenh(const char* m) {
    String s(m);
    s.trim();
    if (s.length() == 0) return;
    char k = s[0];
    if (k == 'K') {
        g_bleAuth = (s.substring(1) == WATCH_KEY);
        bleGui(g_bleAuth ? "A1" : "A0");
        if (g_bleAuth) { bleTrangThai(); bleGioGui(); bleGui(String("E") + g_ledMask); bleGui(String("Y") + g_ledColor + "," + g_ledEff); tbKhiNoi(); }
        return;
    }
    if (!g_bleAuth) return;
    if (k == 'L') {
        int c = s.indexOf(',');
        if (c > 1) {
            int l = constrain(s.substring(1, c).toInt(), -100, 100);
            int r = constrain(s.substring(c + 1).toInt(), -100, 100);
            g_nhay = false;
            datToc(0, A_LA_BANH_TRAI ? l : r);
            datToc(1, A_LA_BANH_TRAI ? r : l);
            g_cmd_deadline = millis() + WEB_DEADMAN_MS;
        }
    } else if (k == 'X') {
        dungHet();
    } else if (k == 'D') {
        tbXong();
    } else if (k == 'A') {
        if (!g_pin_thap) nhayBat();
    } else if (k == 'C') {
        String c = s.substring(1);
        bool ok = c.length() >= 1 && c.length() <= 10;
        for (unsigned i = 0; ok && i < c.length(); i++) ok = isalnum((unsigned char)c[i]);
        if (ok) {
            char k2 = c[0];
            ok = c == "sleep" || c == "clock" || c == "en" || c == "huy" || c == "vup" || c == "vdown" || k2 == 't' || k2 == 'm' || k2 == 'h' || k2 == 'p';
        }
        if (ok) { dungHet(); Serial2.printf("C%s\n", c.c_str()); }
    } else if (k == 'T') {
        int i = s.substring(1).toInt();
        if (i >= 0 && i < g_ty_n) { dungHet(); Serial2.printf("O%s\n", g_ty_id[i].c_str()); }
    } else if (k == 'E') {
        int m = s.substring(1).toInt();
        if (m >= 0 && m <= 15) ledDat(m, -1, -1);
    } else if (k == 'Y') {
        int c = -1, e = -1;
        if (sscanf(s.c_str() + 1, "%d,%d", &c, &e) == 2) ledDat(-1, c, e);
    } else if (k == 'W') {
        if (g_ssid.length()) bleGui(String("w") + g_ssid + "\t" + g_pass);
    } else if (k == 'Q') {
        bleTrangThai();
        bleGioGui();
    }
}

void bleBatDau() {
    g_bleQ = xQueueCreate(8, 48);
    NimBLEDevice::init("Rubi");
    g_bleSrv = NimBLEDevice::createServer();
    NimBLEService* svc = g_bleSrv->createService(BLE_SVC);
    g_bleTx = svc->createCharacteristic(BLE_TX, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = svc->createCharacteristic(BLE_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(new BleRxCb());
    svc->start();
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SVC);
    adv->setScanResponse(true);
    adv->start();
    Serial.println("Bluetooth san sang (dong ho ket noi: ten 'Rubi')");
}

void bleGioTick(uint32_t now) {   // dong bo gio qua Internet khi co Wi-Fi: NTP + mui gio theo vi tri mang
    if (WiFi.status() != WL_CONNECTED) return;
    if (!g_timeCfg) { configTime(0, 0, "pool.ntp.org", "time.google.com"); g_timeCfg = true; }
    if ((int32_t)(now - g_tzNext) < 0) return;
    if (g_target[0] != 0 || g_target[1] != 0 || g_nhay) { g_tzNext = now + 5000; return; }   // dang chay thi de sau
    bool ok = false;
    {
        WiFiClientSecure cl;
        cl.setInsecure();
        HTTPClient http;
        http.setTimeout(4000);
        http.setConnectTimeout(4000);
        if (http.begin(cl, "https://rubi-web-eight.vercel.app/api/time")) {
            int code = http.GET();
            if (code == 200) {
                String b = http.getString();
                int i = b.indexOf("\"tz\":");
                if (i >= 0) { g_tzMin = b.substring(i + 5).toInt(); ok = true; }
            }
            http.end();
        }
    }
    g_tzNext = now + (ok ? 3600000UL : 120000UL);
}

void bleTick() {
    if (!g_bleSrv) return;
    uint32_t now = millis();
    char buf[48];
    while (xQueueReceive(g_bleQ, buf, 0) == pdTRUE) bleLenh(buf);
    int cn = g_bleSrv->getConnectedCount();
    if (cn == 0 && g_bleConnPrev > 0) { g_bleAuth = false; dungHet(); Serial.println("Dong ho mat ket noi: dung robot"); }
    g_bleConnPrev = cn;
    tgSuKien(cn, g_bleAuth, now);
    if (cn == 0 && now - g_bleAdvAt > 2000) {
        g_bleAdvAt = now;
        NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
        if (!a->isAdvertising()) a->start();
    }
    if (cn > 0 && g_bleAuth) {
        if (now - g_bleStatAt > 2000) { g_bleStatAt = now; bleTrangThai(); }
        if (now - g_bleTimeAt > 20000) { g_bleTimeAt = now; bleGioGui(); }
    }
    bleGioTick(now);
}

void webBatDau() {
    g_web.on("/", HTTP_GET, []() { g_web.sendHeader("Cache-Control", "no-store"); g_web.send_P(200, "text/html; charset=utf-8", PAGE_ROOT); });
    g_web.on("/api/drive", HTTP_GET, webDrive);
    g_web.on("/api/stop", HTTP_GET, webStop);
    g_web.on("/api/dance", HTTP_GET, webDance);
    g_web.on("/api/truyen", HTTP_GET, webTruyen);
    g_web.on("/api/nhac", HTTP_GET, webNhac);
    g_web.on("/api/cam", HTTP_GET, webCam);
    g_web.on("/api/lenh", HTTP_GET, webLenh);
    g_web.on("/api/led", HTTP_GET, webLed);
    g_web.on("/api/login", HTTP_GET, webLogin);
    g_web.on("/api/status", HTTP_GET, webStatus);
    g_web.on("/api/tb", HTTP_GET, webTb);
    g_web.on("/api/tbtest", HTTP_GET, webTbTest);
    g_web.on("/api/tg", HTTP_GET, webTg);
    g_web.on("/api/tgtest", HTTP_GET, webTgTest);
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
        case 'D': nhayBat(); break;
        case 't': thuTuDong(); break;
        case 'a': g_nhay = false; datToc(0, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
        case 'b': g_nhay = false; datToc(1, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
        case 'd': g_nhay = false; datToc(0, v); datToc(1, v); g_cmd_deadline = millis() + LENH_TOI_DA_MS; break;
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
    Serial1.begin(115200, SERIAL_8N1, 18, 19);   // sang bo C3 Zero: RX=18, TX=19
    ledNap();
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
    bleBatDau();
    tgBatDau();
    tbNap();
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
        nhayTick();
    }
    doPin();
    wifiTick();
    bleTick();
    tbTick(millis());
    if (millis() - g_ledSentAt > 2000) ledGui();   // gui lai trang thai den moi 2 giay (C3 Zero moi bat nguon cung nhan duoc)
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
    while (Serial1.available()) {   // bo C3 Zero: "V<ban>" (phien ban) va "Q" (xin Wi-Fi de tu cap nhat)
        static String l1;
        char ch = (char)Serial1.read();
        if (ch == '\n' || ch == '\r') {
            if (l1.length() > 1 && l1[0] == 'V' && l1.length() < 20) { g_c3_v = l1.substring(1); g_c3_at = millis() | 1; }
            else if (l1 == "Q" && g_ssid.length() && WiFi.status() == WL_CONNECTED) Serial1.printf("W%s\t%s\n", g_ssid.c_str(), g_pass.c_str());
            l1 = "";
        } else if (l1.length() < 40) l1 += ch;
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
            else if (line2.length() >= 3 && line2[0] == 'T' && line2[1] == 'C') { g_ty_dang = 0; g_ty_n = 0; g_ty_co = true; }   // bat dau danh sach moi
            else if (line2.length() >= 3 && line2[0] == 'Y' && line2[1] == 'C') { g_nh_dang = 0; g_nh_n = 0; g_nh_co = true; }
            else if (line2.length() > 3 && line2[0] == 'Y') {
                int k = line2.indexOf('\t');
                if (k > 1 && g_nh_dang < MAX_NHAC) { g_nh_ten[g_nh_dang] = line2.substring(k + 1); g_nh_dang++; g_nh_n = g_nh_dang; }
            }
            else if (line2.length() > 3 && line2[0] == 'T') {
                int k = line2.indexOf('\t');
                if (k > 1 && g_ty_dang < MAX_TRUYEN) { g_ty_id[g_ty_dang] = line2.substring(1, k); g_ty_ten[g_ty_dang] = line2.substring(k + 1); g_ty_dang++; g_ty_n = g_ty_dang; }
            }
            else if (line2.length() > 8 && line2[0] == 'S') {   // dung luong the nho
                unsigned long a[8] = {0};
                int n = sscanf(line2.c_str() + 1, "%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu\t%lu", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7]);
                if (n == 8) { for (int i = 0; i < 8; i++) g_sd[i] = a[i]; g_sd_at = millis() | 1; }
            }
            else if (line2.length() > 2 && line2.length() < 20 && line2[0] == 'V') { g_ds_v = line2.substring(1); g_ds_at = millis() | 1; }
            else if (line2 == "U1") { g_upd_ds = true; g_upd_ds_den = millis() + 60000; dungHet(); }   // man hinh dang nap ban moi
            else if (line2 == "U0") { g_upd_ds = false; }
            else if (line2 == "D") nhayBat();       // nhay mua (chi xoay tai cho)
            else if (line2 == "X") dungHet();       // dung (huy nhay)
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
