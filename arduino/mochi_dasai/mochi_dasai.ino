/*
 *  mochi dasai — entry point untuk Arduino IDE 2
 *
 *  File ini cuma "pintu masuk". Semua kode asli ada di:
 *      ../../src/main.cpp
 *  dan konfigurasi ada di:
 *      ../../include/config.h
 *
 *  Jadi kamu cukup edit config.h, tidak perlu mengubah file ini.
 */

// Include ini sengaja ditulis di sini supaya Arduino CLI/IDE mengenali
// library yang dipakai main.cpp (pencarian library hanya membaca .ino).
#include <Wire.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "../../src/main.cpp"
