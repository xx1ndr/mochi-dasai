#pragma once

// ================================================================
//  mochi dasai — konfigurasi
//  Edit file ini saja, jangan ubah src/main.cpp kalau tidak perlu.
// ================================================================

// ---------------- WiFi ----------------
// Setelan ini dipakai ESP32 untuk nyambung ke router (opsional, buat internet:
// jam NTP, cuaca, Telegram). Kalau router susah, biarkan saja — ESP32 tetap
// bisa dipakai lewat hotspot sendiri (lihat bagian AP di bawah).
#define WIFI_SSID "WiFI_RUMAH"
#define WIFI_PASS "PASSWORD_WIFI"

// ---------------- Hotspot dari ESP32 (AP) ----------------
// HP connect ke WiFi "mochi-dasai", lalu buka http://192.168.4.1
// Password minimal 8 karakter. Kosongkan "" kalau mau tanpa password.
#define AP_SSID "mochi-dasai"
#define AP_PASS "mochi1234"

// 1 = hanya hotspot ESP32 (tanpa internet; jam ikut waktu HP lewat browser)
// 0 = hotspot ESP32 + tetap mencoba nyambung ke WiFi rumah (dapat jam & cuaca)
#define AP_ONLY 0

// ---------------- Zona waktu ----------------
// WIB = "WIB-7"   WITA = "WITA-8"   WIT = "WIT-9"
#define TZ_STRING "WIB-7"

// ---------------- Lokasi cadangan ----------------
// Dipakai untuk cuaca kalau GPS belum dapat fix.
#define HOME_LAT  -6.200000
#define HOME_LON 106.816666

// ---------------- MQTT (opsional) ----------------
// Kosongkan MQTT_HOST kalau tidak pakai broker MQTT.
#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#define DEVICE_ID "mochi-01"

// ---------------- Telegram (opsional) ----------------
// Token dari @BotFather, contoh: "123456789:AAH..."
// Kosongkan kalau tidak dipakai.
#define TELEGRAM_TOKEN ""

// ---------------- Wiring / pin ----------------
#define PIN_SDA    20   // OLED SDA
#define PIN_SCL    21   // OLED SCL
#define PIN_GPS_RX 4    // ESP32 RX  <- GPS TX
#define PIN_GPS_TX 5    // ESP32 TX  -> GPS RX (boleh tidak disambung)
#define PIN_BTN    3    // TTP223 OUT (aktif HIGH)
#define PIN_BUZZ   10   // buzzer aktif (+ ke GPIO, - ke GND)

// TTP223 keluaran HIGH saat disentuh. Kalau pakai tombol ke GND, ubah ke LOW.
#define BTN_ACTIVE HIGH

#define GPS_BAUD   9600 // NEO-6M/ATGM336H = 9600, u-blox M10 = 38400
#define OLED_ADDR  0x3C // kalau blank coba 0x3D

// ---------------- Fitur opsional ----------------
#define HAS_BUTTON 1      // 1 kalau sensor sentuh / tombol sudah dipasang
#define HAS_BUZZER 0      // 1 kalau buzzer sudah dipasang
#define AUTO_ROTATE_MS 0      // 0 = mati (disarankan saat pakai sensor sentuh)

// ---------------- Mood & rasa lapar ----------------
// Berapa detik sejak terakhir diberi makan sampai mochi jadi lapar.
// Default 1 jam. Nanti bisa diubah lewat menu / kasih makan.
#define HUNGER_SEC 3600UL

// Kecepatan animasi wajah: 60 ms per frame = 16 fps.
// Satu frame penuh = 1024 byte lewat I2C, jadi kalau terlalu cepat WiFi
// (hotspot) bisa kalah prioritas dan HP tidak menemukan jaringan.
// 40 = 25 fps, 60 = 16 fps, 83 = 12 fps, 120 = 8 fps.
#define FACE_FRAME_MS 60

// 0 = SSD1306 (paling umum), 1 = SH1106 (kalau tampilan bergeser/noise)
#define OLED_SH1106 0
