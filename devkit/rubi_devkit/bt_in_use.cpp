// Loi "esp_bt_controller_init ESP_ERR_INVALID_STATE": core Arduino ESP32 moi giai phong bo nho Bluetooth
// luc khoi dong neu khong thay ma nao bao dang dung BT. Bao ro la co dung BLE (NimBLE) de no giu lai.
// (De trong file .cpp rieng vi bo tien xu ly cua Arduino lam hong dong extern "C" trong file .ino.)
#include <Arduino.h>

extern "C" bool btInUse(void) { return true; }
