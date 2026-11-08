// Rubi - firmware bo ESP32-C3 Zero tren robot: dieu khien den LED (phan 1).
// Nap bang Arduino IDE: Board = "ESP32C3 Dev Module", USB CDC On Boot = Enabled. Thu vien: Adafruit NeoPixel.
//
// Chan:
//   GPIO8  = DIN den 2 ang-ten APA106 (qua 330 ohm)
//   GPIO10 = DIN day LED WS2812B (nguc 3 + chan bun trai 6 + chan bun phai 6 = 15 den, noi tiep theo thu tu nay) (qua 330 ohm)
//   GPIO21 (TX) -> GPIO18 (RX) DevKit,  GPIO20 (RX) <- GPIO19 (TX) DevKit,  UART 115200, muc 3,3 V
//
// Lenh tu DevKit (mot dong, ket thuc bang \n), gui lai moi 2 giay:
//   L<mask>,<mau>,<kieu>
//     mask : bit0 = den nguc, bit1 = chan bun trai, bit2 = chan bun phai, bit3 = 2 den ang-ten (15 = bat het, 0 = tat het)
//     mau  : 0 xanh duong, 1 hong, 2 vang, 3 luc, 4 cau vong
//     kieu : 0 dung yen, 1 nhay cham (dang nghe / di theo)
// Mat lenh tu DevKit qua 10 giay: giu trang thai cuoi, khong tat den.
#include <Adafruit_NeoPixel.h>

#define PIN_ANT    8
#define PIN_STRIP  10
#define N_ANT      2
#define N_CHEST    3
#define N_FL       6
#define N_FR       6
#define N_STRIP    (N_CHEST + N_FL + N_FR)

// Neu mau hien sai (vd. do thanh xanh la) doi NEO_GRB thanh NEO_RGB cho day tuong ung
Adafruit_NeoPixel ant(N_ANT, PIN_ANT, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel strip(N_STRIP, PIN_STRIP, NEO_GRB + NEO_KHZ800);

uint8_t g_mask = 0x0F;
uint8_t g_color = 0;
uint8_t g_eff = 0;
const uint8_t BRIGHT = 60;

static const uint8_t PALETTE[4][3] = {
    {0, 90, 255},     // xanh duong
    {255, 60, 140},   // hong
    {255, 190, 20},   // vang
    {30, 220, 90},    // luc
};

uint32_t colorAt(Adafruit_NeoPixel& s, uint8_t color, int idx, int n, uint32_t now) {
    if (color == 4) {   // cau vong troi theo thoi gian
        uint16_t hue = (uint16_t)((now * 8) + idx * (65536 / (n > 0 ? n : 1)));
        return s.gamma32(s.ColorHSV(hue));
    }
    const uint8_t* p = PALETTE[color < 4 ? color : 0];
    return s.Color(p[0], p[1], p[2]);
}

void render(uint32_t now) {
    // nhay cham: sang 700 ms, mo 700 ms
    bool lit = (g_eff == 0) || ((now / 700) % 2 == 0);
    for (int i = 0; i < N_ANT; i++)
        ant.setPixelColor(i, (lit && (g_mask & 8)) ? colorAt(ant, g_color, i, N_ANT, now) : 0);
    ant.show();
    for (int i = 0; i < N_STRIP; i++) {
        bool on = false;
        if (i < N_CHEST) on = g_mask & 1;
        else if (i < N_CHEST + N_FL) on = g_mask & 2;
        else on = g_mask & 4;
        strip.setPixelColor(i, (lit && on) ? colorAt(strip, g_color, i, N_STRIP, now) : 0);
    }
    strip.show();
}

String g_line;

void handleLine(const String& s) {
    if (s.length() < 2 || s[0] != 'L') return;
    int c1 = s.indexOf(',');
    int c2 = s.indexOf(',', c1 + 1);
    if (c1 < 2 || c2 < 0) return;
    g_mask = constrain(s.substring(1, c1).toInt(), 0, 15);
    g_color = constrain(s.substring(c1 + 1, c2).toInt(), 0, 4);
    g_eff = constrain(s.substring(c2 + 1).toInt(), 0, 1);
}

void setup() {
    Serial.begin(115200);
    Serial1.begin(115200, SERIAL_8N1, 20, 21);   // RX=20, TX=21
    ant.begin();   ant.setBrightness(BRIGHT);
    strip.begin(); strip.setBrightness(BRIGHT);
    render(millis());
}

void loop() {
    while (Serial1.available()) {
        char ch = (char)Serial1.read();
        if (ch == '\n' || ch == '\r') { handleLine(g_line); g_line = ""; }
        else if (g_line.length() < 40) g_line += ch;
    }
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last >= 40) { last = now; render(now); }
}
