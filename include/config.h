#pragma once

// ================================================================
//  mochi dasai — konfigurasi
//  Edit file ini saja, jangan ubah src/main.cpp kalau tidak perlu.
// ================================================================

// ---------------- WiFi ----------------
// Kredensial WiFi TIDAK disimpan di repo (repo ini public).
// Isi file include/config_local.h (diabaikan git) dengan:
//   #define WIFI_SSID "nama_wifi_kamu"
//   #define WIFI_PASS "password_kamu"
// Kalau file itu tidak ada, nilai di bawah dipakai (kosong = ESP32 hanya AP).
#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

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
// Dipakai untuk cuaca kalau lokasi belum dikirim dari HP/browser.
// Modul GPS tidak dipasang, jadi tidak ada pin/kabel GPS.
#define HOME_LAT  -6.200000
#define HOME_LON 106.816666

// 1 = kalau lokasi HP belum ada, tebak kota dari IP publik ESP32 (gratis,
//     tanpa API key). Jalan otomatis tiap 30 menit. Akurasi setingkat kota.
// 0 = pakai HOME_LAT/HOME_LON saja sampai lokasi diset manual dari web.
#define AUTO_IP_GEO 1

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
#define PIN_BTN    3    // TTP223 OUT (aktif HIGH)
// Tidak ada PIN_GPS_: modul GPS tidak dipakai. Lokasi cukup dari HP/browser.

// Baterai Li-ion + modul TP4056.
// PIN_BAT_ADC = pembagi tegangan baterai (100k atas / 100k bawah) -> GPIO1 (ADC1).
// PIN_CHRG    = pin CHRG modul TP4056 (open-drain, aktif LOW) -> GPIO2.
#define PIN_BAT_ADC  1
#define PIN_CHRG     2
#define HAS_BAT_ADC  1
#define HAS_CHRG_PIN 1
#define BAT_DIV_RATIO 2.0f   // (Ratas + Rbawah) / Rbawah
#define BAT_MIN_MV   3000    // baterai "kosong"
#define BAT_MAX_MV   4200    // baterai penuh
#define BAT_SAMPLES  16

// TTP223 keluaran HIGH saat disentuh. Kalau pakai tombol ke GND, ubah ke LOW.
#define BTN_ACTIVE HIGH

#define OLED_ADDR  0x3C // kalau blank coba 0x3D

// Kecepatan bus I2C OLED. 800 kHz pernah dipakai tetapi bikin tampilan
// bergaris/noise dan mengganggu sensor sentuh. Kembali ke 400 kHz (standar).
#define OLED_I2C_HZ 400000

// ---------------- Fitur opsional ----------------
#define HAS_BUTTON 1      // 1 kalau sensor sentuh / tombol sudah dipasang
#define AUTO_ROTATE_MS 0      // 0 = mati (disarankan saat pakai sensor sentuh)

// ---------------- Mood & rasa lapar ----------------
// Berapa detik sejak terakhir diberi makan sampai mochi jadi lapar.
// Default 1 jam. Nanti bisa diubah lewat menu / kasih makan.
#define HUNGER_SEC 3600UL

// Kecepatan animasi wajah: 60 ms per frame = 16 fps.
// Satu frame penuh = 1024 byte lewat I2C, jadi kalau terlalu cepat WiFi
// (hotspot) bisa kalah prioritas dan HP tidak menemukan jaringan.
// 40 = 25 fps, 60 = 16 fps, 83 = 12 fps, 120 = 8 fps.
// Ekspresi dipilih acak + jeda acak (lihat drawFace() di src/main.cpp).
#define FACE_FRAME_MS 60

// OLED 1.3" 128x64 umumnya SH1106 (0.96" biasanya SSD1306).
// 0 = SSD1306, 1 = SH1106. Kalau gambar bergeser/noise, ganti ke 0.
#define OLED_SH1106 1
