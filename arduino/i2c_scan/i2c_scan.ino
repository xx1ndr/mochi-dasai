// Sketch diagnostik: cek OLED / I2C di ESP32-C3 Super Mini
// Upload dengan Board: ESP32C3 Dev Module + USB CDC On Boot: Enabled
// Buka Serial Monitor 115200.

#include <Wire.h>

static int scanBus(uint8_t sda, uint8_t scl) {
  Wire.end();
  delay(20);
  Wire.begin((int)sda, (int)scl, 100000);

  Serial.printf("[scan] SDA=%u SCL=%u -> ", sda, scl);
  int found = 0;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("0x%02X ", a);
      ++found;
    }
  }
  if (!found) Serial.print("(kosong)");
  Serial.printf(" | %d device\n", found);
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("=== mochi dasai: i2c scan ===");
  Serial.printf("chip=%s  rev=%d  cpu=%u MHz\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(),
                (unsigned)(ESP.getCpuFreqMHz()));

  scanBus(20, 21); // sesuai wiring sekarang: SDA=20 SCL=21
  scanBus(21, 20); // kalau SDA/SCL ketukar
  scanBus(4, 5);   // wiring normal mochi
  scanBus(5, 4);   // kalau SDA/SCL ketukar
  scanBus(6, 7);   // cadangan
  scanBus(8, 9);   // cadangan

  Serial.println("=== scan wajarnya 0x3C atau 0x3D ===");
  Serial.println("=== mengulang tiap 3 detik ===");
}

void loop() {
  delay(3000);
  scanBus(20, 21);
}
