// Rubi - firmware cho ESP32 DevKit 30 chan (dieu khien 2 motor qua TB6612 + do pin).
// Nap bang Arduino IDE:  Board = "ESP32 Dev Module",  Upload Speed 115200,  Serial Monitor 115200 baud, "Newline".
//
// KHI THU LAN DAU: NHAC BANH XE KHOI MAT BAN (hoac lat nguoc robot) de banh quay tu do.
//
// Chan DevKit (theo so do day):
//   Motor A: PWMA=D25  AIN1=D26  AIN2=D27     encoder E1A=D34  E1B=D35
//   Motor B: PWMB=D33  BIN1=D13  BIN2=D14     encoder E2A=VN(GPIO39)  E2B=D32
//   STBY=D23 (muc cao = driver chay)
//   Do pin: module chia ap 5:1, S -> GPIO36 (VP)
//   UART2 sang bo man hinh: TX2=GPIO17, RX2=GPIO16 (chi gui "B<mV>" moi giay, nhu pin_gui_phan_tram.ino)
//
// Lenh go trong Serial Monitor (gui bang Enter):
//   t        thu tu dong: tung motor chay tien/lui 2 giay, in ket qua encoder va chan doan
//   a50      motor A chay 50% (am = lui, vi du a-50)     b50   motor B
//   d40      ca hai motor cung chay 40%                  s     dung
//   r        dat lai bo dem encoder                       e     in bo dem + toc do
//   ?        hien huong dan
// Moi lenh a/b/d chi chay toi da 5 giay roi tu dung (de khong bao gio chay mai).

#include <Arduino.h>
#include <driver/gpio.h>

// ---------------- Chan ----------------
const int PIN_PWMA = 25, PIN_AIN1 = 26, PIN_AIN2 = 27;
const int PIN_PWMB = 33, PIN_BIN1 = 13, PIN_BIN2 = 14;
const int PIN_STBY = 23;
const int PIN_E1A = 34, PIN_E1B = 35;
const int PIN_E2A = 39, PIN_E2B = 32;
const int PIN_VBAT = 36;

// ---------------- Cau hinh (sua o day neu can) ----------------
// Dao chieu neu "tien" ma banh quay lui:
const bool DAO_CHIEU_A = false;
const bool DAO_CHIEU_B = false;
// Dao dau encoder neu tien ma bo dem giam:
const bool DAO_ENC_A = false;
const bool DAO_ENC_B = false;

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

// Do pin
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
        Serial.println("(het 5 giay, tu dung)");
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
        if (g_pin_thap) { g_target[0] = g_target[1] = 0; Serial.printf("! Pin yeu: %.2f V\n", g_bat_mv / 1000.0f); }
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
    Serial.println();
    Serial.println("Rubi DevKit san sang. Motor dang dung.");
    inHuongDan();
}

void loop() {
    static uint32_t last10 = 0;
    static String line;
    uint32_t now = millis();
    if (now - last10 >= 10) {
        last10 += 10;
        if (now - last10 > 100) last10 = now;
        capNhatMotor();
    }
    doPin();
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') { xuLyLenh(line); line = ""; }
        else if (line.length() < 24) line += ch;
    }
    delay(1);
}
