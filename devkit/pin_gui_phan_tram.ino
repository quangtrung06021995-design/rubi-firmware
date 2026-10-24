// DevKit ESP32 30 chan: do dien ap pin 2S qua module do ap (chia 5:1) va gui cho bo man hinh qua UART2.
//   Module do ap:  S -> GPIO36 (VP);  "-" -> GND;  "+" de trong;  hai dau vit -> +VSW / GND (sau cong tac)
//   UART2: GPIO17 (TX) -> GPIO14 bo man hinh;  GPIO16 (RX) <- GPIO21 bo man hinh;  chung GND
// Gui moi giay mot dong "B<mV>\n", vi du "B7620\n" = 7,62 V.
// HE SO_HIEU_CHINH: do pin bang dong ho van nang, sua so nay cho khop (mac dinh 1.000).
const int PIN_ADC = 36;
const float TI_LE = 5.0f;            // module 30k/7.5k: 8,4 V -> 1,68 V
const float HE_SO_HIEU_CHINH = 1.000f;

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, 16, 17);   // RX=16, TX=17
  analogSetPinAttenuation(PIN_ADC, ADC_11db);
}

void loop() {
  uint32_t sum = 0;
  const int N = 64;
  for (int i = 0; i < N; i++) { sum += analogReadMilliVolts(PIN_ADC); delay(2); }
  float mv_pin = (sum / (float)N) * TI_LE * HE_SO_HIEU_CHINH;
  Serial2.printf("B%d\n", (int)mv_pin);
  Serial.printf("Pin: %.2f V\n", mv_pin / 1000.0f);
  delay(870);
}
