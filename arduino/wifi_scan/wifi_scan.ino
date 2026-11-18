// Diagnostik WiFi: scan jaringan lalu coba connect ke SSID config.
// Board: ESP32C3 Dev Module, USB CDC On Boot: Enabled, Serial 115200.

#include <WiFi.h>

// Isi kredensial di sini HANYA di komputer lokal. Jangan commit password.
// (Untuk firmware utama, taruh di include/config_local.h yang di-gitignore.)
#define WIFI_SSID "NAMA_WIFI_KAMU"
#define WIFI_PASS "PASSWORD_WIFI_KAMU"

static const char* STATUS_NAMA(int s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID (SSID tidak ketemu / 5GHz?)";
    case WL_SCAN_COMPLETED:  return "SCAN_DONE";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (password salah?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "?";
  }
}

static const char* ENC_NAMA(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "LAIN";
  }
}

static void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("  [event] DISC reason=%u\n", (unsigned)info.wifi_sta_disconnected.reason);
  } else if (ev == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("  [event] tersambung ke AP");
  } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("  [event] dapat IP %s\n", WiFi.localIP().toString().c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== mochi: wifi scan ===");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWifiEvent);
  WiFi.disconnect(true);
  delay(200);

  int n = WiFi.scanNetworks();
  Serial.printf("%d jaringan 2.4GHz ditemukan:\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %-28s %4d dBm  ch%2d  %-10s %s\n",
                  WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), (int)WiFi.channel(i),
                  ENC_NAMA(WiFi.encryptionType(i)),
                  WiFi.SSID(i) == WIFI_SSID ? "<== SSID di config" : "");
  }
  WiFi.scanDelete();

  Serial.printf("=== connect ke \"%s\" ===\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 25; ++i) {
    delay(1000);
    int st = (int)WiFi.status();
    Serial.printf("  %2ds status=%d %s\n", i + 1, st, STATUS_NAMA(st));
    if (st == WL_CONNECTED) {
      Serial.printf("  OK  ip=%s  rssi=%d dBm\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      break;
    }
  }
  Serial.println("=== selesai ===");
}

void loop() {
  delay(5000);
  Serial.printf("status=%d %s\n", (int)WiFi.status(), STATUS_NAMA(WiFi.status()));
}
