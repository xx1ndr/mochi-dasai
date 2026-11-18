/*
 *  mochi dasai — jam, cuaca, GPS, notifikasi WhatsApp & Telegram
 *  Target : ESP32-C3 Super Mini   + OLED SSD1306 128x64 (I2C)
 *  GPS    : NEO-6M / ATGM336H / MAX-M10S
 *
 *  Layar  : tombol pendek = ganti layar, tombol lama = refresh / hapus notif
 *           tanpa tombol: http://<ip>/next  /clear  /refresh  (ada tombolnya di web)
 *  Notif  : GET/POST http://<ip>/notify?from=..&text=..
 *  Status : GET http://<ip>/api/status
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <esp_wifi.h>          // esp_wifi_set_max_tx_power / esp_wifi_set_ps
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>        // esp_random() untuk ekspresi acak
#include <driver/gpio.h>       // gpio_set_drive_capability() untuk buzzer
#include <math.h>
#include <string.h>

#include "../include/config.h"
#include "../include/dasai_frames.h"   // 90 frame animasi DASAI asli (PROGMEM)
#include "../include/mood_frames.h"    // 5 set frame dari GIF ekspresi (animasi OLED per mood)
#include "../include/mochi_web_gif.h"  // GIF asli untuk endpoint /mochi.gif

// ------------------------------------------------------------ objek global
// clock/data ikut diberikan supaya U8g2 memakai pin I2C dari config.h
// (urutan argumen U8g2: rotation, reset, clock/SCL, data/SDA)
#if OLED_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);
#endif
TinyGPSPlus    gps;
HardwareSerial gpsSerial(1);
WebServer      web(80);
WiFiClient     mqttNet;
PubSubClient   mqtt(mqttNet);

// ------------------------------------------------------------ konstanta
static const char* DAY_ID[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
static const char* MON_ID[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
                               "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};

enum Screen : uint8_t {
  SCR_FACE, SCR_HOME, SCR_GPS, SCR_WX, SCR_MSG, SCR_SYS, SCR_BATT,
  SCR_MENU, SCR_MOOD, SCR_EAT, SCR_COUNT
};
static Screen screen = SCR_FACE;

static const char* SCREEN_NAME[] = {
  "wajah", "jam", "gps", "cuaca", "notif", "sistem", "baterai",
  "menu", "ubah mood", "makan"
};

// ---- mood & interaksi ----
enum class Mood : uint8_t { Happy = 0, Sad, Angry, Sleepy, Hungry, Count };
enum class React : uint8_t { None = 0, Love, Shake };

static const char* MOOD_NAME[] = {"Senang", "Sedih", "Marah", "Ngantuk", "Lapar"};
static const char* moodName(Mood m) {
  uint8_t i = (uint8_t)m;
  return i < (uint8_t)Mood::Count ? MOOD_NAME[i] : MOOD_NAME[0];
}

static Mood  mood        = Mood::Happy;
static bool  moodIdle    = true;   // true = animasi idle (relaxed.gif), belum ada mood dipilih
static React react       = React::None;
static uint32_t reactUntil = 0;   // millis() akhir animasi reaksi
static uint32_t faceHintUntil = 0;// millis() akhir petunjuk "tahan = menu"
static uint32_t lastInput  = 0;   // input terakhir (auto-close menu)
static uint32_t screenSince= 0;   // layar mulai (auto-close makan)
static int    angryPets   = 0;    // elusan sebelum marah -> sedih
static int    menuIndex   = 0;
static int    moodIndex   = 0;
static uint8_t noteIndex  = 0;

static uint32_t lastFed   = 0;    // epoch terakhir diberi makan
static bool     fedOffline= false;// sudah makan tapi jam belum siap
static bool     hungerAnnounced = false;
static Preferences prefs;         // NVS: mood + waktu makan bertahan saat reboot

// Frame lanjutan setelah splash boot, biar animasi tidak loncat ke frame 0.
static uint16_t faceStartFrame = 0;

static const uint8_t INBOX_MAX = 8;

struct Note {
  char src[4];
  char from[20];
  char text[72];
  uint32_t epoch;
  bool unread;
};

static Note    inbox[INBOX_MAX];
static uint8_t inboxCount = 0;
static uint8_t unreadCount = 0;

struct Weather {
  bool     valid;
  float    temp, feels, rh, wind;
  int      code;
  uint32_t updated;
};
static Weather wx = {false, NAN, NAN, NAN, NAN, -1, 0};
static bool     wxFromSerial = false;   // data cuaca dikirim PC lewat kabel USB

// Lokasi untuk cuaca. Urutan prioritas:
//   GPS -> lokasi dari web/HP (NVS) -> geolokasi IP publik -> HOME.
static float    wxLat = HOME_LAT, wxLon = HOME_LON;
static bool     wxLocSet = false;

// Lokasi otomatis dari IP publik ESP32 (tidak disimpan di NVS, refresh 30 mnt).
static float    wxIpLat = NAN, wxIpLon = NAN;
static uint32_t wxIpAge = 0;

static const char* wxSource() {
  if (wxFromSerial) return "serial";
  if (gps.location.isValid() && gps.location.age() < 5000) return "gps";
  if (wxLocSet) return "web";
  if (!isnan(wxIpLat)) return "ip";
  return "default";
}

static uint32_t gpsChars = 0;
static double   tripKm = 0;
static double   lastLat = 0, lastLon = 0;
static bool     haveLastPos = false;
static bool     forceWeather = false;

// ---- perintah belokan (/nav manual) + rute otomatis (/goto, OSRM tanpa API key)
enum NavKind : uint8_t {
  NAV_STRAIGHT, NAV_LEFT, NAV_RIGHT, NAV_SLIGHT_LEFT,
  NAV_SLIGHT_RIGHT, NAV_UTURN, NAV_ROUNDABOUT, NAV_ARRIVE
};

struct NavStep {
  bool     valid;
  uint8_t  kind;
  uint16_t dist;      // meter
  char     road[26];
  uint32_t at;        // millis() saat diterima
};
static NavStep nav = {false, NAV_STRAIGHT, 0, "", 0};
static const uint32_t NAV_TTL_MS = 5UL * 60UL * 1000UL;   // 5 menit

// Rute otomatis: petunjuk diambil dari OSRM, maju mengikuti posisi GPS.
struct RouteStep {
  uint8_t  kind;
  uint32_t atM;       // jarak kumulatif dari titik awal sampai belokan ini
  char     road[26];
};
static RouteStep routeSteps[24];
static uint8_t  routeCount   = 0;
static uint8_t  routeIdx     = 0;
static bool     routeActive  = false;
static uint32_t routeTravelled = 0;
static uint32_t routeTotal   = 0;
static double   routeLastLat = 0, routeLastLon = 0;

static bool     apOk    = false;   // hasil WiFi.softAP()
static uint32_t frameMs = 0;       // lama kirim 1 frame ke OLED

// Baterai iPhone, dikirim Shortcuts lewat HTTP (atau lewat kabel: B<level>|<0/1>).
static int8_t   phoneBatt     = -1;   // -1 = belum pernah ada data
static bool     phoneCharging = false;
static uint32_t phoneBattAt   = 0;

// ------------------------------------------------------------ utilitas
static bool localNow(struct tm* out) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  if (out) localtime_r(&now, out);
  return true;
}

/* buang newline, emoji, dan spasi berlebih supaya aman di OLED */
static void sanitize(char* dst, size_t n, const char* src) {
  if (!src) src = "";
  size_t o = 0;
  bool lastSpace = true;
  char lastCh = 0;
  for (size_t i = 0; src[i] && o + 1 < n; ++i) {
    unsigned char c = (unsigned char)src[i];
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (c < 32 || c > 126) c = '?';
    if (c == ' ' && lastSpace) continue;
    if (c == '?' && lastCh == '?') continue;
    dst[o++] = (char)c;
    lastSpace = (c == ' ');
    lastCh = (char)c;
  }
  while (o > 0 && dst[o - 1] == ' ') o--;
  dst[o] = 0;
}

static const char* wxText(int code) {
  if (code < 0)                              return "belum ada data";
  if (code == 0)                             return "Cerah";
  if (code <= 2)                             return "Cerah berawan";
  if (code == 3)                             return "Mendung";
  if (code == 45 || code == 48)              return "Berkabut";
  if (code >= 51 && code <= 57)              return "Gerimis";
  if (code >= 61 && code <= 67)              return "Hujan";
  if (code >= 71 && code <= 77)              return "Salju";
  if (code >= 80 && code <= 82)              return "Hujan lebat";
  if (code == 85 || code == 86)              return "Hujan salju";
  if (code >= 95)                            return "Badai guntur";
  return "Cuaca tak dikenal";
}

// Buzzer aktif butuh DC, pasif butuh frekuensi. Dua-duanya didukung.
// Nyalakan semua pin uji sekaligus: mana pun yang dipakai, tetap bunyi.
static const uint8_t BUZZ_PINS_ALL[BUZZ_PIN_COUNT] = BUZZ_PINS;
static void buzzOn(uint16_t hz) {
#if BUZZER_ACTIVE
  for (uint8_t i = 0; i < BUZZ_PIN_COUNT; ++i) digitalWrite(BUZZ_PINS_ALL[i], HIGH);
#else
  for (uint8_t i = 0; i < BUZZ_PIN_COUNT; ++i) tone(BUZZ_PINS_ALL[i], hz ? hz : 2000);
#endif
}
static void buzzOff() {
#if BUZZER_ACTIVE
  for (uint8_t i = 0; i < BUZZ_PIN_COUNT; ++i) digitalWrite(BUZZ_PINS_ALL[i], LOW);
#else
  for (uint8_t i = 0; i < BUZZ_PIN_COUNT; ++i) noTone(BUZZ_PINS_ALL[i]);
#endif
}

static void beep() {
  if (!HAS_BUZZER) return;
  buzzOn(2600);
  delay(70);
  buzzOff();
}

// /buzz non-blocking: pin tetap HIGH sampai task matikan di loop().
static uint32_t webBuzzOffAt = 0;
static void webBuzzTask() {
  if (webBuzzOffAt && (int32_t)(millis() - webBuzzOffAt) >= 0) {
    webBuzzOffAt = 0;
    buzzOff();
  }
}

// Notifikasi: dua pulse supaya beda dari bunyi tombol/mood.
static uint32_t notifyToneAt = 0;
static uint8_t  notifyStep   = 0;

static void notifyBeep() {
  if (!HAS_BUZZER) return;
  notifyStep = 1;
  notifyToneAt = millis();
  buzzOn(1800);
}

static void notifyBeepTask() {
  if (!HAS_BUZZER || notifyStep == 0) return;
  uint32_t now = millis();
  if (notifyStep == 1 && now - notifyToneAt >= 120) {
    buzzOff();
    notifyStep = 2;
    notifyToneAt = now;
  } else if (notifyStep == 2 && now - notifyToneAt >= 60) {
    buzzOn(2400);
    notifyStep = 3;
    notifyToneAt = now;
  } else if (notifyStep == 3 && now - notifyToneAt >= 120) {
    buzzOff();
    notifyStep = 0;
  }
}

static void addNotification(const char* src, const char* from, const char* text) {
  for (int i = INBOX_MAX - 1; i > 0; --i) inbox[i] = inbox[i - 1];
  sanitize(inbox[0].src, sizeof(inbox[0].src), (src && *src) ? src : "?");
  sanitize(inbox[0].from, sizeof(inbox[0].from), (from && *from) ? from : "Tanpa nama");
  sanitize(inbox[0].text, sizeof(inbox[0].text), (text && *text) ? text : "(kosong)");
  inbox[0].epoch  = (uint32_t)time(nullptr);
  inbox[0].unread = true;
  if (inboxCount < INBOX_MAX) inboxCount++;
  if (unreadCount < 99) unreadCount++;
  notifyBeep();
  Serial.printf("[%s] %s: %s\n", inbox[0].src, inbox[0].from, inbox[0].text);
}

static void markAllRead() {
  unreadCount = 0;
  for (uint8_t i = 0; i < INBOX_MAX; ++i) inbox[i].unread = false;
}

static void clearInbox() {
  inboxCount = 0;
  unreadCount = 0;
  noteIndex = 0;
  for (uint8_t i = 0; i < INBOX_MAX; ++i) inbox[i].text[0] = 0;
}

// ------------------------------------------------------------ mood / NVS
static void nvsBegin() { prefs.begin("mochi", false); }
static uint32_t nvsGet(const char* key, uint32_t def) { return prefs.getUInt(key, def); }
static void nvsSet(const char* key, uint32_t val) { prefs.putUInt(key, val); }

/* Simpan baterai HP. level -1 berarti "hapus data". */
static void setPhoneBattery(int level, bool charging) {
  if (level < 0 || level > 100) return;
  phoneBatt     = (int8_t)level;
  phoneCharging = charging;
  phoneBattAt   = millis();
  nvsSet("batt", (uint32_t)level);
  nvsSet("battchg", charging ? 1u : 0u);
  Serial.printf("[baterai] iPhone %d%% %s\n", level, charging ? "(mengisi)" : "");
}

// Koordinat cuaca disimpan supaya tetap dipakai setelah reboot walau GPS tidak fix.
static void saveWxLocation(float lat, float lon) {
  prefs.putBool("wxlocset", true);
  prefs.putFloat("wxlat", lat);
  prefs.putFloat("wxlon", lon);
  wxLat = lat; wxLon = lon; wxLocSet = true;
}
static void clearWxLocation() {
  prefs.remove("wxlocset");
  wxLat = HOME_LAT; wxLon = HOME_LON; wxLocSet = false;
}

static void goFace() {
  screen = SCR_FACE;
  faceHintUntil = millis() + 6000;
  beep();
}

static void openMenu() {
  screen = SCR_MENU;
  menuIndex = 0;
  lastInput = millis();
  beep();
}

static void setMood(Mood next) {
  mood = next;
  moodIdle = false;               // mood dipilih -> hentikan animasi idle
  nvsSet("mood", (uint32_t)next);
  if (next != Mood::Angry) angryPets = 0;
  Serial.printf("[mood] %s\n", moodName(next));
}

static void markFed() {
  fedOffline = true;           // tandai dulu kalau jam belum siap
  hungerAnnounced = false;
  if (localNow(nullptr)) {
    lastFed = (uint32_t)time(nullptr);
    nvsSet("fed", lastFed);
    fedOffline = false;
  }
}

static void updateHunger() {
  if (!localNow(nullptr)) return;
  uint32_t nowEpoch = (uint32_t)time(nullptr);

  if (fedOffline || lastFed == 0) {
    lastFed = nowEpoch;
    nvsSet("fed", lastFed);
    fedOffline = false;
    hungerAnnounced = false;
    return;
  }

  bool due = nowEpoch > lastFed && (nowEpoch - lastFed) >= HUNGER_SEC;
  if (!due) { hungerAnnounced = false; return; }

  if (!hungerAnnounced) {
    hungerAnnounced = true;
    if (screen == SCR_FACE && (mood == Mood::Happy || mood == Mood::Sleepy))
      setMood(Mood::Hungry);
  }
}

static void onFaceTap() {
  uint32_t now = millis();
  if (mood == Mood::Hungry) {              // lapar -> langsung kasih makan
    screen = SCR_EAT;
    screenSince = now;
    markFed();
    return;
  }
  if (mood == Mood::Angry) {               // elus 3x -> jadi sedih
    angryPets++;
    react = React::Shake;
    reactUntil = now + 700;
    if (angryPets >= 3) setMood(Mood::Sad);
    return;
  }
  if (mood == Mood::Sad || mood == Mood::Sleepy) setMood(Mood::Happy);
  react = React::Love;
  reactUntil = now + 1400;
}

enum MenuItem : uint8_t {
  MENU_CLOCK = 0, MENU_WEATHER, MENU_GPS, MENU_NOTIF,
  MENU_MOOD, MENU_FEED, MENU_CLOSE, MENU_COUNT
};
static const char* MENU_LABEL[] = {
  "Jam", "Cuaca", "GPS/Peta", "Notif", "Ubah mood", "Kasih makan", "Tutup"
};

// Pilihan tambahan di picker mood supaya selalu ada jalan keluar ke menu.
static const int MOOD_CANCEL = (int)Mood::Count;

static void onMenuSelect() {
  switch (menuIndex) {
    case MENU_CLOCK:
      screen = SCR_HOME;
      break;
    case MENU_WEATHER:
      screen = SCR_WX;
      forceWeather = true;
      configTzTime(TZ_STRING, "pool.ntp.org", "id.pool.ntp.org");
      break;
    case MENU_GPS:
      screen = SCR_GPS;
      break;
    case MENU_NOTIF:
      screen = SCR_MSG;
      noteIndex = 0;
      markAllRead();
      break;
    case MENU_MOOD:
      screen = SCR_MOOD;
      moodIndex = (int)mood;
      if (moodIndex < 0 || moodIndex >= (int)Mood::Count) moodIndex = 0;
      break;
    case MENU_FEED:
      screen = SCR_EAT;
      screenSince = millis();
      markFed();
      return;
    default:
      goFace();
      return;
  }
  lastInput = millis();
}

// ------------------------------------------------------------ WiFi / waktu
static void onWifiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("[wifi] putus, reason=%u\n",
                  (unsigned)info.wifi_sta_disconnected.reason);
  } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("[wifi] dapat IP %s\n", WiFi.localIP().toString().c_str());
  }
}

static bool    wifiHaveAp  = false;
static uint8_t wifiChannel = 0;
static uint8_t wifiBssid[6] = {0};

// Scan sekali untuk mendapat channel + BSSID. Menyambung dengan BSSID yang
// dipatok menghindari "reason=201 NO_AP_FOUND" yang muncul saat driver
// gagal menemukan AP-nya sendiri di tengah proses connect.
static void findAp() {
  Serial.println("[wifi] scan AP...");
  int n = WiFi.scanNetworks(false, true);
  int best = -1, bestRssi = -127;
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == WIFI_SSID && (int)WiFi.RSSI(i) > bestRssi) {
      best = i;
      bestRssi = (int)WiFi.RSSI(i);
    }
  }
  if (best >= 0) {
    wifiChannel = (uint8_t)WiFi.channel(best);
    memcpy(wifiBssid, WiFi.BSSID(best), 6);
    wifiHaveAp = true;
    Serial.printf("[wifi] AP ketemu ch%u rssi=%d bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  wifiChannel, bestRssi,
                  wifiBssid[0], wifiBssid[1], wifiBssid[2],
                  wifiBssid[3], wifiBssid[4], wifiBssid[5]);
  } else {
    Serial.println("[wifi] AP tidak ada di scan (5GHz-only? terlalu jauh?)");
  }
  WiFi.scanDelete();
  delay(100);
}

static void wifiTask() {
  if (AP_ONLY) return;                 // mode hotspot saja: tidak perlu router
  static uint32_t lastTry = 0;
  static uint8_t  tries   = 0;
  static bool     started = false;

  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t now = millis();
  if (started && now - lastTry < 15000) return;
  started = true;
  lastTry = now;
  tries++;

  Serial.printf("[wifi] menyambung ke \"%s\" (coba %u)...\n", WIFI_SSID, tries);
  if (tries > 1) {
    WiFi.disconnect();
    delay(150);
  }
  if (tries == 1) findAp();

  if (wifiHaveAp) WiFi.begin(WIFI_SSID, WIFI_PASS, wifiChannel, wifiBssid, true);
  else            WiFi.begin(WIFI_SSID, WIFI_PASS);

  if (tries == 6)
    Serial.println("[wifi] masih gagal -> coba hotspot HP (2.4 GHz) & hapus NVS");
}

// konversi UTC -> epoch tanpa bergantung pada timegm()
static time_t utcEpoch(int year, int mon, int day, int hour, int minute, int second) {
  static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int64_t days = 0;
  for (int y = 1970; y < year; ++y)
    days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
  for (int m = 0; m < mon - 1; ++m) {
    days += mdays[m];
    if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days += 1;
  }
  days += day - 1;
  return (time_t)(days * 86400LL + hour * 3600LL + minute * 60LL + second);
}

static void timeTask() {
  static bool     ntpStarted = false;
  static uint32_t lastSync   = 0;

  if (WiFi.status() != WL_CONNECTED) { ntpStarted = false; return; }

  if (!ntpStarted) {
    // configTzTime penting: configTime(0,0,...) menimpa TZ jadi UTC0, sehingga
    // jam yang dikirim browser/GPS tampil 7 jam lebih awal (WIB jadi UTC).
    configTzTime(TZ_STRING, "pool.ntp.org", "id.pool.ntp.org", "time.google.com");
    ntpStarted = true;
    lastSync   = millis();
    return;
  }

  if (localNow(nullptr)) {
    if (millis() - lastSync > 3600000UL) {   // resync tiap jam
      configTzTime(TZ_STRING, "pool.ntp.org", "id.pool.ntp.org");
      lastSync = millis();
    }
    return;
  }

  // belum dapat NTP -> ambil jam dari GPS (GPS = UTC)
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.time.age() < 2000 && gps.date.age() < 2000) {
    time_t epoch = utcEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                            gps.time.hour(), gps.time.minute(), gps.time.second());
    if (epoch > 1700000000) {
      struct timeval tv = { epoch, 0 };
      settimeofday(&tv, nullptr);
      tzset();
      Serial.println("[jam] diset dari GPS");
    }
  }
}

// ------------------------------------------------------------ cuaca
static bool fetchWeather(float lat, float lon) {
  WiFiClientSecure tls;
  tls.setInsecure();                       // open-meteo tanpa pinning

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);

  char url[260];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
           "weather_code,wind_speed_10m&timezone=auto",
           lat, lon);

  if (!http.begin(tls, url)) {
    Serial.println("[cuaca] begin gagal");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[cuaca] HTTP %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    Serial.printf("[cuaca] json: %s\n", err.c_str());
    return false;
  }

  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  float t = cur["temperature_2m"] | NAN;
  if (isnan(t)) return false;

  wx.temp    = t;
  wx.rh      = cur["relative_humidity_2m"] | 0.0f;
  wx.feels   = cur["apparent_temperature"]  | t;
  wx.wind    = cur["wind_speed_10m"]        | 0.0f;
  wx.code    = cur["weather_code"]          | -1;
  wx.updated = millis();
  wx.valid   = true;

  Serial.printf("[cuaca] %.1fC  %d%%  %s\n", wx.temp, (int)wx.rh, wxText(wx.code));
  return true;
}

// Tanya layanan geolokasi IP publik supaya cuaca mengikuti kota tempat ESP32
// tersambung WiFi. Gratis, tanpa API key, dan jalan di background walaupun HP
// tidak membuka halaman web. Akurasinya setingkat kota (bukan titik GPS).
#if AUTO_IP_GEO
static void geoIpTask() {
  static uint32_t lastTry = 0;
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now      = millis();
  uint32_t interval = isnan(wxIpLat) ? 60000UL : 1800000UL;  // coba / refresh
  if (lastTry != 0 && now - lastTry < interval) return;
  lastTry = now;

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(tls, "https://ipwho.is/")) return;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[geo] HTTP %d\n", code);
    http.end();
    return;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err || !(doc["success"] | false)) return;

  float la = doc["latitude"] | NAN;
  float lo = doc["longitude"] | NAN;
  if (isnan(la) || isnan(lo)) return;

  wxIpLat = la;
  wxIpLon = lo;
  wxIpAge = now;
  Serial.printf("[geo] IP -> %.4f,%.4f (%s)\n", la, lo, doc["city"] | "?");
}
#endif // AUTO_IP_GEO

static void weatherTask() {
  static uint32_t lastTry = 0;
  static float    usedLat = NAN, usedLon = NAN;

  // GPS > lokasi web/HP > IP publik > default HOME.
  float lat, lon;
  if (gps.location.isValid() && gps.location.age() < 15000) {
    lat = (float)gps.location.lat();
    lon = (float)gps.location.lng();
  } else if (wxLocSet) {
    lat = wxLat;
    lon = wxLon;
  } else if (!isnan(wxIpLat)) {
    lat = wxIpLat;
    lon = wxIpLon;
  } else {
    lat = HOME_LAT;
    lon = HOME_LON;
  }

  // Lokasi berubah jauh -> ambil cuaca baru sekarang, bukan tunggu 10 menit.
  if (isnan(usedLat) || fabs(lat - usedLat) > 0.01f || fabs(lon - usedLon) > 0.01f) {
    usedLat = lat;
    usedLon = lon;
    forceWeather = true;
  }

  if (forceWeather) { forceWeather = false; lastTry = 0; }
  if (WiFi.status() != WL_CONNECTED) return;

  uint32_t now      = millis();
  uint32_t interval = wx.valid ? 600000UL : 15000UL;  // 10 menit / coba 15 detik
  if (lastTry != 0 && now - lastTry < interval) return;
  lastTry = now;

  fetchWeather(lat, lon);
}

// ------------------------------------------------------------ MQTT
static bool mqttEnabled() { return strlen(MQTT_HOST) > 0; }

static void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  (void)topic;
  char buf[96];
  size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = 0;

  JsonDocument doc;
  if (deserializeJson(doc, buf) == DeserializationError::Ok &&
      doc["text"].is<const char*>()) {
    const char* from = doc["from"] | "MQTT";
    addNotification("MQ", from, doc["text"] | "");
  } else {
    addNotification("MQ", "MQTT", buf);
  }
}

static void mqttTask() {
  if (!mqttEnabled()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  static uint32_t lastTry = 0;

  if (!mqtt.connected()) {
    uint32_t now = millis();
    if (lastTry != 0 && now - lastTry < 5000) return;
    lastTry = now;

    String cid = String("mochi-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    bool ok = strlen(MQTT_USER) > 0
                  ? mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS)
                  : mqtt.connect(cid.c_str());

    if (ok) {
      String sub = String("mochi/") + DEVICE_ID + "/notify";
      String st  = String("mochi/") + DEVICE_ID + "/status";
      mqtt.subscribe(sub.c_str());
      mqtt.publish(st.c_str(), "online", true);
      Serial.printf("[mqtt] tersambung, subscribe %s\n", sub.c_str());
    }
  }
  mqtt.loop();
}

// ------------------------------------------------------------ Telegram
// Token bisa diisi dari web (/telegram?token=...) dan disimpan di NVS.
static char     tgToken[96] = TELEGRAM_TOKEN;
static uint32_t tgGen       = 0;     // naik tiap token diganti -> reset polling

static bool telegramEnabled() { return strlen(tgToken) > 20; }

static void setTelegramToken(const char* tok) {
  if (!tok) return;
  size_t n = strlen(tok);
  if (n > sizeof(tgToken) - 1) n = sizeof(tgToken) - 1;
  memcpy(tgToken, tok, n);
  tgToken[n] = 0;
  tgGen++;
  prefs.putString("tgtok", tgToken);
  Serial.printf("[tg] token disimpan (%zu char)\n", n);
}

/*
 *  Long polling ringan: tiap 4 detik tanya getUpdates (timeout=0, koneksi
 *  TLS dipakai ulang). Saat pertama kali connect, tumpukan update lama
 *  dibuang supaya inbox tidak dibanjiri pesan 24 jam terakhir.
 */
static void telegramTask() {
  if (!telegramEnabled()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  static uint32_t lastTry = 0;
  static uint32_t wait    = 4000;
  static long     offset  = 0;
  static bool     primed  = false;
  static WiFiClientSecure tls;
  static HTTPClient http;
  static bool     tlsInit = false;
  static uint32_t seenGen = 0;

  if (seenGen != tgGen) {              // token baru -> buang state lama
    seenGen = tgGen;
    offset = 0; primed = false; wait = 4000; lastTry = 0;
    if (tlsInit) { http.end(); tls.stop(); }
    tlsInit = false;
  }

  uint32_t now = millis();
  if (lastTry != 0 && now - lastTry < wait) return;
  lastTry = now;

  if (!tlsInit) {
    tls.setInsecure();
    http.setReuse(true);
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    tlsInit = true;
  }

  char url[200];
  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/getUpdates?timeout=0&limit=10&offset=%ld",
           tgToken, primed ? offset : -1L);

  if (!http.begin(tls, url)) return;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    Serial.printf("[tg] HTTP %d\n", code);
    if (code < 0) tlsInit = false;          // paksa handshake baru lain kali
    wait = wait < 30000 ? wait * 2 : 60000; // backoff kalau server bermasalah
    return;
  }
  wait = 4000;

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;
  if (!(doc["ok"] | false)) {
    Serial.printf("[tg] %s\n", (const char*)(doc["description"] | "error"));
    return;
  }

  JsonArray res = doc["result"];
  for (JsonVariant uv : res) {
    JsonObject u = uv.as<JsonObject>();
    if (u.isNull()) continue;
    long id = u["update_id"] | 0L;
    if (id >= offset) offset = id + 1;
    if (!primed) continue;            // buang backlog saat boot

    JsonObject m = u["message"];
    if (m.isNull()) continue;

    JsonObject chat = m["chat"];
    const char* name = chat["title"] | "";
    if (!*name) name = m["from"]["first_name"] | "";
    if (!*name) name = m["from"]["username"]  | "";
    if (!*name) name = "Telegram";

    const char* text = m["text"] | "";
    if (!*text) text = "[media]";

    // Perintah baterai dari Shortcuts iPhone: "#batt 85 1"
    // (1 = sedang mengisi). Tidak perlu PC/hotspot,ESP32 baca langsung dari bot.
    if (strncmp(text, "#batt", 5) == 0) {
      int lvl = -1, chg = 0;
      if (sscanf(text + 5, " %d %d", &lvl, &chg) >= 1 && lvl >= 0 && lvl <= 100) {
        setPhoneBattery(lvl, chg > 0);
        continue;
      }
    }

    addNotification("TG", name, text);
  }
  primed = true;
}

// ------------------------------------------------------------ GPS
static void pumpGps() {
  uint16_t budget = 256;
  while (gpsSerial.available() > 0 && budget--) {
    gps.encode((char)gpsSerial.read());
    gpsChars++;
  }

  if (gps.location.isUpdated()) {
    if (haveLastPos) {
      double d = TinyGPSPlus::distanceBetween(lastLat, lastLon,
                                              gps.location.lat(), gps.location.lng());
      if (d > 3.0 && d < 500.0) tripKm += d / 1000.0;
    }
    lastLat = gps.location.lat();
    lastLon = gps.location.lng();
    haveLastPos = true;
  }
}

// ------------------------------------------------------------ gambar
static void drawTrunc(int x, int y, const char* s, int maxChars) {
  char b[80];
  strncpy(b, s, sizeof(b) - 1);
  b[sizeof(b) - 1] = 0;
  int n = (int)strlen(b);
  if (n > maxChars && maxChars >= 4) {
    b[maxChars - 3] = '.';
    b[maxChars - 2] = '.';
    b[maxChars - 1] = '.';
    b[maxChars]     = 0;
  }
  oled.drawStr(x, y, b);
}

static void drawStatusBar() {
  oled.setFont(u8g2_font_5x7_tf);
  char b[32];
  int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  snprintf(b, sizeof(b), "%s sat%02d%s",
           WiFi.status() == WL_CONNECTED ? "wifi" : "ap",
           sats,
           mqtt.connected() ? " mq" : "");
  oled.drawStr(0, 63, b);

  char u[10] = "";
  if (unreadCount) {
    snprintf(u, sizeof(u), "msg%u", (unsigned)unreadCount);
    oled.drawStr(127 - oled.getStrWidth(u), 63, u);
  }
  if (phoneBatt >= 0) {
    char p[10];
    snprintf(p, sizeof(p), "%d%%%s", (int)phoneBatt, phoneCharging ? "+" : "");
    int px = 127 - oled.getStrWidth(p);
    if (unreadCount) px -= oled.getStrWidth(u) + 4;
    oled.drawStr(px, 63, p);
  }
}

// ------------------------------------------------------------ wajah mochi
// Animasi ASLI mochi dasai: 90 frame bitmap 128x64 dari klip RIVE resmi,
// diekspor ke include/dasai_frames.h (sumber: github.com/upiir/esp32s3_oled_dasai_mochi).
//
// Frame 0-12 itu fade in dan frame 59-89 fade out, jadi yang diputar hanya
// 13..58. Potongan acak dari rentang ini + jeda acak dipakai supaya perilakunya
// mirip web The Mochi: ekspresi berubah-ubah, ada delay antar ekspresi.
// Frame asli tidak diubah satu bit pun.
#define FACE_LOOP_FIRST 13
#define FACE_LOOP_LAST  58

static int16_t  faceFrozen = -1;   // >= 0 = tampilkan 1 frame saja (diagnosa /face)

// ---- transisi halus antar wajah ----
// Frame DASAI tidak diubah satu bit pun. Yang diperbaiki: kecepatan ping-pong
// (ease in/out) dan crossfade saat pindah ke/dari wajah mood.
#define FACE_BLEND_MS 180
static uint8_t facePrevBuf[DASAI_FRAME_BYTES];  // snapshot frame sebelumnya (1024 byte)
static uint32_t faceBlendUntil = 0;

static uint8_t faceModeNow() {
  if (faceFrozen >= 0) return 3;                     // frame diagnosa
  return 0;                                          // selalu animasi dari GIF
}

// Crossfade 1-bit memakai pola Bayer 4x4 supaya tidak ada garis-garis.
static void blendFromPrev(uint32_t now) {
  if (now >= faceBlendUntil) return;
  uint32_t remain = faceBlendUntil - now;
  int progress = 255 - (int)(remain * 255UL / FACE_BLEND_MS);
  if (progress < 0) progress = 0;
  if (progress > 255) progress = 255;

  static const uint8_t kBayer[16] = {
     0, 8, 2,10, 12, 4,14, 6, 3,11, 1, 9, 15, 7,13, 5
  };
  uint8_t* buf = oled.getBufferPtr();
  if (!buf) return;

  for (int y = 0; y < DASAI_FRAME_H; ++y) {
    for (int x = 0; x < DASAI_FRAME_W; ++x) {
      int th = kBayer[(y & 3) * 4 + (x & 3)] * 17;  // 0..255
      if (th <= progress) continue;                 // pakai pixel baru
      // framebuffer u8g2 = vertikal, LSB di atas: byte = baris 8 px, bit = y%8
      int idx = (y & ~7) * (DASAI_FRAME_W / 8) + x;
      uint8_t mask = (uint8_t)(1u << (y & 7));
      if (facePrevBuf[idx] & mask) buf[idx] |= mask;
      else                         buf[idx] &= (uint8_t)~mask;
    }
  }
}

// ---- ekspresi mood: ikon kecil di kiri atas, karakter DASAI tetap utuh ----
static void moodHeart(int x, int y, int r) {
  oled.drawDisc(x - r, y, r);
  oled.drawDisc(x + r, y, r);
  oled.drawTriangle(x - 2 * r, y, x + 2 * r, y, x, y + 3 * r);
}

static void drawMoodBadge(uint32_t now) {
  bool eating = (screen == SCR_EAT);
  bool hearts = (react == React::Love && now < reactUntil && !eating);
  bool shake  = (react == React::Shake && now < reactUntil);
  if (mood == Mood::Happy && !eating && !hearts && !shake) return;

  const char* label = moodName(mood);
  if (eating)      label = "Nyam";
  else if (hearts) label = "Elus";
  else if (shake)  label = "Hmph";

  // Badge diletakkan di kiri-bawah: area itu hitam di semua frame GIF, jadi
  // teks putih tidak lagi tertutup karakter (di kiri-atas dulu jadi tidak
  // terbaca). Latar hitam juga dipaksa supaya aman kalau GIF diganti.
  oled.setDrawColor(0);
  oled.drawBox(0, 55, 64, 9);
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_5x7_tf);

  int ix = 3, iy = 59;
  if (hearts) {
    moodHeart(ix, iy, 1);
  } else if (mood == Mood::Sad) {
    oled.drawTriangle(ix, iy - 3, ix - 2, iy, ix + 2, iy);
    oled.drawDisc(ix, iy, 1);
  } else if (mood == Mood::Angry || shake) {
    oled.drawLine(ix - 3, iy - 3, ix + 3, iy + 3);
    oled.drawLine(ix + 3, iy - 3, ix - 3, iy + 3);
  } else if (mood == Mood::Sleepy) {
    oled.drawStr(0, 63, "z");
  } else {
    // lapar / makan: mangkuk kecil
    oled.drawBox(ix - 3, iy + 1, 6, 2);
    oled.drawBox(ix - 2, iy - 1, 4, 1);
  }
  oled.drawStr(10, 63, label);
}
static void drawFace() {
  // Animasi mengikuti mood: 5 GIF ekspresi, masing-masing ~75 frame dengan
  // delay aslinya 60/70 ms. Urutan set = enum Mood (lihat mood_frames.h).
  static uint8_t  whichNow = 0xFF;   // set frame yang sedang diputar
  static uint16_t frame = 0;
  static uint32_t last  = 0;
  uint32_t now = millis();

  // habis reaksi, kembalikan ke animasi kalau mood-nya Senang
  if (react != React::None && now >= reactUntil) react = React::None;

  if (faceFrozen >= 0) {
    oled.drawXBMP(0, 0, DASAI_FRAME_W, DASAI_FRAME_H,
                  dasaiFrame((uint8_t)faceFrozen));
    return;
  }

  // Idle = relaxed.gif sampai mood dipilih; setelah itu animasi mood-nya.
  uint8_t which = moodIdle ? MOOD_IDLE_SET : (uint8_t)mood;
  if (which >= MOOD_GIF_COUNT) which = 0;
  if (which != whichNow) {              // ganti ekspresi -> mulai dari frame 0
    whichNow = which;
    frame = faceStartFrame;             // 0 normal, atau lanjutan setelah splash
    faceStartFrame = 0;
    last  = now;
  }

  uint16_t cnt  = pgm_read_word(&MOOD_FRAME_COUNTS[which]);
  uint16_t base = (uint16_t)(pgm_read_dword(&MOOD_FRAME_OFFSETS[which]) /
                             MOOD_FRAME_BYTES);
  uint16_t delay = (uint16_t)pgm_read_byte(&MOOD_DELAYS[base + frame]) * 10;
  if (delay == 0) delay = 70;

  if (now - last >= delay) {
    uint32_t steps = (now - last) / delay;
    if (steps > 8) steps = 8;                   // kejar waktu setelah jeda panjang
    last = now;
    frame = (frame + steps) % cnt;
  }

  oled.drawXBMP(0, 0, MOOD_FRAME_W, MOOD_FRAME_H, moodFrame(which, frame));
  drawMoodBadge(now);
}

static void drawHome() {
  struct tm t;
  bool tOK = localNow(&t);
  char b[40];

  oled.setFont(u8g2_font_6x10_tf);
  if (tOK)
    snprintf(b, sizeof(b), "%s %02d %s", DAY_ID[t.tm_wday], t.tm_mday, MON_ID[t.tm_mon]);
  else
    snprintf(b, sizeof(b), "sinkron waktu...");
  oled.drawStr(0, 9, b);

  if (unreadCount) {
    snprintf(b, sizeof(b), "MSG %u", (unsigned)unreadCount);
    oled.drawStr(127 - oled.getStrWidth(b), 9, b);
  }

  // jam besar
  char hm[8];
  if (tOK) snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
  else     snprintf(hm, sizeof(hm), "--:--");

  oled.setFont(u8g2_font_logisoso26_tn);
  int w = oled.getStrWidth(hm);
  oled.drawStr(0, 40, hm);

  // suhu di kanan
  int tempX = 128;
  if (wx.valid) {
    char tb[8];
    snprintf(tb, sizeof(tb), "%.0f", wx.temp);
    oled.setFont(u8g2_font_helvB14_tf);
    int tw = oled.getStrWidth(tb);
    tempX  = 127 - tw - 12;
    oled.drawStr(tempX, 40, tb);
    oled.drawDisc(tempX + tw + 3, 30, 1);
    oled.drawStr(tempX + tw + 6, 40, "C");
  }

  // detik kalau masih ada ruang
  if (tOK && w + 22 < tempX) {
    oled.setFont(u8g2_font_6x10_tf);
    snprintf(b, sizeof(b), "%02d", t.tm_sec);
    oled.drawStr(w + 4, 40, b);
  }

  // baris cuaca
  oled.setFont(u8g2_font_6x10_tf);
  if (wx.valid) {
    oled.drawStr(0, 53, wxText(wx.code));
    snprintf(b, sizeof(b), "%d%%", (int)roundf(wx.rh));
    oled.drawStr(127 - oled.getStrWidth(b), 53, b);
  } else {
    oled.drawStr(0, 53, "cuaca: menunggu");
  }

  drawStatusBar();
}

// ---- ikon belokan: panah besar, digambar dari segitiga + kotak
static void thickLine(int x0, int y0, int x1, int y1, int w) {
  int dx = x1 - x0, dy = y1 - y0;
  for (int i = -w / 2; i <= w / 2; i++) {
    if (dx > dy) oled.drawLine(x0, y0 + i, x1, y1 + i);
    else         oled.drawLine(x0 + i, y0, x1 + i, y1);
  }
}

static void drawTurnIcon(int cx, int cy, uint8_t kind) {
  switch (kind) {
    case NAV_LEFT:
      oled.drawTriangle(cx - 17, cy, cx - 1, cy - 12, cx - 1, cy + 12);
      oled.drawBox(cx - 2, cy - 5, 19, 10);
      break;

    case NAV_RIGHT:
      oled.drawTriangle(cx + 17, cy, cx + 1, cy - 12, cx + 1, cy + 12);
      oled.drawBox(cx - 17, cy - 5, 19, 10);
      break;

    case NAV_SLIGHT_LEFT:
      thickLine(cx + 12, cy + 17, cx - 6, cy - 5, 9);
      oled.drawTriangle(cx - 13, cy - 15, cx - 1, cy - 9, cx - 9, cy + 2);
      break;

    case NAV_SLIGHT_RIGHT:
      thickLine(cx - 12, cy + 17, cx + 6, cy - 5, 9);
      oled.drawTriangle(cx + 13, cy - 15, cx + 1, cy - 9, cx + 9, cy + 2);
      break;

    case NAV_UTURN:
      oled.drawEllipse(cx, cy - 4, 11, 10,
                       U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      oled.drawBox(cx - 12, cy - 5, 6, 20);          // kaki kiri turun
      oled.drawBox(cx + 6, cy - 5, 6, 9);            // kaki kanan pendek
      oled.drawTriangle(cx + 9, cy + 14, cx + 2, cy + 4, cx + 16, cy + 4);
      break;

    case NAV_ROUNDABOUT:
      oled.drawCircle(cx - 2, cy, 11);
      oled.drawDisc(cx - 2, cy, 4);
      oled.drawBox(cx + 12, cy - 4, 12, 8);          // jalan keluar
      oled.drawTriangle(cx + 19, cy + 9, cx + 8, cy + 2, cx + 8, cy + 16);
      break;

    case NAV_ARRIVE:
      oled.drawDisc(cx, cy - 6, 5);                   // tiang bendera
      oled.drawBox(cx - 1, cy - 1, 3, 15);
      oled.drawBox(cx + 2, cy - 12, 12, 8);           // bendera
      break;

    default:  // NAV_STRAIGHT
      oled.drawTriangle(cx, cy - 17, cx - 12, cy + 1, cx + 12, cy + 1);
      oled.drawBox(cx - 5, cy - 1, 10, 19);
      break;
  }
}

static const char* navText(uint8_t kind) {
  switch (kind) {
    case NAV_LEFT:         return "BELOK KIRI";
    case NAV_RIGHT:        return "BELOK KANAN";
    case NAV_SLIGHT_LEFT:  return "AGAK KIRI";
    case NAV_SLIGHT_RIGHT: return "AGAK KANAN";
    case NAV_UTURN:        return "PUTAR BALIK";
    case NAV_ROUNDABOUT:   return "BUNDARAN";
    case NAV_ARRIVE:       return "TIBA DI TUJUAN";
    default:               return "LURUS";
  }
}

static bool navActive() {
  return nav.valid && (routeActive || (millis() - nav.at) < NAV_TTL_MS);
}

static void drawGps() {
  bool fix = gps.location.isValid() && gps.location.age() < 5000;
  char b[40];

  if (navActive()) {
    // ---- mode navigasi: panah besar + jarak + nama jalan
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(0, 10, navText(nav.kind));

    if (nav.dist >= 1000)
      snprintf(b, sizeof(b), "%.1f", nav.dist / 1000.0f);
    else
      snprintf(b, sizeof(b), "%u", (unsigned)nav.dist);

    oled.setFont(u8g2_font_logisoso24_tn);
    int w = oled.getStrWidth(b);
    oled.drawStr(127 - w, 40, b);

    const char* unit = nav.dist >= 1000 ? "km" : "m";
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(127 - w - 2 - oled.getStrWidth(unit), 40, unit);

    drawTurnIcon(22, 42, nav.kind);

    oled.setFont(u8g2_font_5x7_tf);
    drawTrunc(48, 54, nav.road, 13);

    snprintf(b, sizeof(b), "%s%.0f km/j", fix ? "" : "-",
             fix ? gps.speed.kmph() : 0.0f);
    oled.drawStr(48, 63, b);
    return;
  }

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "GPS");
  const char* st = fix ? "FIX" : "NO FIX";
  oled.drawStr(127 - oled.getStrWidth(st), 9, st);

  // kecepatan besar
  snprintf(b, sizeof(b), "%.0f", fix ? gps.speed.kmph() : 0.0f);
  oled.setFont(u8g2_font_logisoso26_tn);
  int w = oled.getStrWidth(b);
  oled.drawStr(0, 40, b);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(w + 5, 40, "km/j");

  oled.setFont(u8g2_font_5x7_tf);
  snprintf(b, sizeof(b), "sat%02d hd%.1f alt%.0fm",
           gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
           gps.hdop.isValid() ? gps.hdop.hdop() : 0.0,
           gps.altitude.isValid() ? gps.altitude.meters() : 0.0);
  oled.drawStr(0, 50, b);

  snprintf(b, sizeof(b), "%.1fkm", tripKm);
  oled.drawStr(127 - oled.getStrWidth(b), 50, b);

  if (fix) {
    snprintf(b, sizeof(b), "lat %9.5f", gps.location.lat());
    oled.drawStr(0, 57, b);
    snprintf(b, sizeof(b), "lon %9.5f", gps.location.lng());
    oled.drawStr(0, 64, b);
  } else if (wxFromSerial && !isnan(wxLat) && !isnan(wxLon)) {
    // Tidak ada fix satelit (misal di dalam ruangan): tampilkan lokasi yang
    // dikirim PC lewat kabel supaya layar GPS tetap informasi.
    oled.drawStr(0, 57, "lokasi dari PC:");
    snprintf(b, sizeof(b), "%.5f,%.5f", wxLat, wxLon);
    oled.drawStr(0, 64, b);
  } else {
    oled.drawStr(0, 57, "cari satelit... taruh di luar ruangan");
    oled.drawStr(0, 64, "atau kirim rute dari web");
  }
}

static void drawWx() {
  char b[40];

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "CUACA");

  if (!wx.valid) {
    oled.drawStr(0, 38, "mengambil data...");
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(0, 52, WiFi.status() == WL_CONNECTED ? "coba lagi sebentar"
                                                     : "wifi belum tersambung");
    return;
  }

  oled.setFont(u8g2_font_5x7_tf);
  snprintf(b, sizeof(b), "%lum lalu", (unsigned long)((millis() - wx.updated) / 60000));
  oled.drawStr(127 - oled.getStrWidth(b), 9, b);

  // suhu besar
  snprintf(b, sizeof(b), "%.1f", wx.temp);
  oled.setFont(u8g2_font_helvB18_tf);
  int w = oled.getStrWidth(b);
  oled.drawStr(0, 36, b);
  oled.drawDisc(w + 5, 20, 2);
  oled.drawStr(w + 10, 36, "C");

  oled.setFont(u8g2_font_helvB10_tf);
  oled.drawStr(0, 49, wxText(wx.code));

  oled.setFont(u8g2_font_5x7_tf);
  snprintf(b, sizeof(b), "terasa %.0fC  RH%d%%  %.0fkm/j",
           wx.feels, (int)roundf(wx.rh), wx.wind);
  oled.drawStr(0, 59, b);
}

static void drawMsg() {
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "PESAN");

  if (inboxCount == 0) {
    oled.drawStr(16, 34, "belum ada pesan");
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(16, 46, "web: /next /clear");
    oled.drawStr(16, 55, "notif: /notify");
    oled.drawStr(16, 63, "atau bot Telegram");
    return;
  }

  oled.setFont(u8g2_font_5x7_tf);
  char b[12];
  snprintf(b, sizeof(b), "%u/%u", (unsigned)inboxCount, (unsigned)INBOX_MAX);
  oled.drawStr(127 - oled.getStrWidth(b), 9, b);

  for (int i = 0; i < 2 && (int)noteIndex + i < inboxCount; ++i) {
    int idx = noteIndex + i;
    int y = 21 + i * 23;
    char line[36];
    snprintf(line, sizeof(line), "[%s] %s", inbox[idx].src, inbox[idx].from);
    oled.setFont(u8g2_font_6x10_tf);
    drawTrunc(0, y, line, 19);
    if (inbox[idx].unread) oled.drawDisc(124, y - 3, 1);
    oled.setFont(u8g2_font_6x10_tf);
    drawTrunc(4, y + 11, inbox[idx].text, 20);
  }

  if (inboxCount > 2) {
    oled.setFont(u8g2_font_5x7_tf);
    snprintf(b, sizeof(b), "+%d lagi", inboxCount - 2);
    oled.drawStr(0, 63, b);
  }
}

static void drawBattery() {
  char b[32];

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "BATERAI HP");
  oled.drawStr(127 - oled.getStrWidth("iPhone"), 9, "iPhone");

  if (phoneBatt < 0) {
    oled.setFont(u8g2_font_helvB14_tf);
    oled.drawStr(0, 42, "-");
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(20, 42, "belum ada data");
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(0, 62, "kirim lewat Shortcuts / /battery");
    return;
  }

  snprintf(b, sizeof(b), "%d", (int)phoneBatt);
  oled.setFont(u8g2_font_logisoso26_tn);
  int w = oled.getStrWidth(b);
  oled.drawStr(0, 40, b);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(w + 4, 40, "%");

  // ikon baterai: bingkai + isian sesuai level
  const int bx = 90, by = 26, bw = 28, bh = 14;
  oled.drawFrame(bx, by, bw, bh);
  oled.drawBox(bx + bw + 1, by + 4, 3, 6);
  int fill = (bw - 4) * (int)phoneBatt / 100;
  if (fill > 0) oled.drawBox(bx + 2, by + 2, fill, bh - 4);
  if (phoneCharging) {
    // kilat kecil di atas ikon
    oled.drawLine(bx + 16, by - 4, bx + 12, by + 1);
    oled.drawLine(bx + 12, by + 1, bx + 16, by + 1);
    oled.drawLine(bx + 16, by + 1, bx + 12, by + 6);
  }

  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 53, phoneCharging ? "mengisi" : "tidak mengisi");
  if (phoneBattAt == 0)
    snprintf(b, sizeof(b), "tersimpan");
  else {
    uint32_t age = (millis() - phoneBattAt) / 1000;
    if (age < 60)        snprintf(b, sizeof(b), "baru");
    else if (age < 3600) snprintf(b, sizeof(b), "%lum lalu", (unsigned long)(age / 60));
    else                 snprintf(b, sizeof(b), "%luh lalu", (unsigned long)(age / 3600));
  }
  oled.drawStr(127 - oled.getStrWidth(b), 53, b);
  oled.drawStr(0, 63, "dari iPhone (WiFi / kabel)");
}

static void drawSys() {
  char b[48];
  int y = 21;

  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 9, "SISTEM");

  oled.setFont(u8g2_font_5x7_tf);

  snprintf(b, sizeof(b), "ap   %s", WiFi.softAPIP().toString().c_str());
  oled.drawStr(0, y, b); y += 9;

  snprintf(b, sizeof(b), "ip   %s", WiFi.localIP().toString().c_str());
  oled.drawStr(0, y, b); y += 9;

  snprintf(b, sizeof(b), "wifi %d dBm   %s",
           (int)WiFi.RSSI(), WiFi.status() == WL_CONNECTED ? "ok" : "putus");
  oled.drawStr(0, y, b); y += 9;

  snprintf(b, sizeof(b), "heap %u B   mqtt %s",
           (unsigned)ESP.getFreeHeap(), mqtt.connected() ? "ok" : "-");
  oled.drawStr(0, y, b); y += 9;

  uint32_t s = millis() / 1000;
  snprintf(b, sizeof(b), "up %luh%02lum  gps %lu B",
           (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60),
           (unsigned long)gpsChars);
  oled.drawStr(0, y, b);
}

// ---- menu sentuh (tahan lama dari wajah) ----
static void drawMenuScreen() {
  oled.setFont(u8g2_font_5x7_tf);
  for (int i = 0; i < MENU_COUNT; ++i) {
    int baseline = 8 + i * 8;              // 7 item muat: 8,16,...,56
    if (i == menuIndex) {
      oled.setDrawColor(1);
      oled.drawBox(0, baseline - 7, 128, 8);
      oled.setDrawColor(0);
    } else {
      oled.setDrawColor(1);
    }
    oled.drawStr(6, baseline, MENU_LABEL[i]);
  }
  oled.setDrawColor(1);
  oled.drawStr(0, 63, "tap=pindah tahan=pilih");
}

static void drawMoodPicker() {
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(0, 8, "tap pindah, tahan pilih");
  for (int i = 0; i <= MOOD_CANCEL; ++i) {
    int baseline = 14 + i * 8;
    if (i == moodIndex) {
      oled.setDrawColor(1);
      oled.drawBox(0, baseline - 7, 128, 8);
      oled.setDrawColor(0);
    } else {
      oled.setDrawColor(1);
    }
    oled.drawStr(6, baseline, i == MOOD_CANCEL ? "Batal (ke menu)" : MOOD_NAME[i]);
  }
  oled.setDrawColor(1);
}

static void render() {
  uint32_t now = millis();

  // Simpan isi buffer terakhir SEBELUM clear, dipakai crossfade wajah.
  uint8_t* buf = oled.getBufferPtr();
  if (buf) memcpy(facePrevBuf, buf, DASAI_FRAME_BYTES);

  uint8_t mode = (screen == SCR_FACE || screen == SCR_EAT) ? faceModeNow() : 255;
  static uint8_t prevMode = 255;
  if (mode != 255 && prevMode != 255 && mode != prevMode)
    faceBlendUntil = now + FACE_BLEND_MS;
  prevMode = mode;

  oled.clearBuffer();
  switch (screen) {
    case SCR_FACE: drawFace(); break;
    case SCR_HOME: drawHome(); break;
    case SCR_GPS:  drawGps();  break;
    case SCR_WX:   drawWx();   break;
    case SCR_MSG:  drawMsg();  break;
    case SCR_SYS:  drawSys();  break;
    case SCR_BATT: drawBattery(); break;
    case SCR_MENU: drawMenuScreen(); break;
    case SCR_MOOD: drawMoodPicker(); break;
    case SCR_EAT:  drawFace(); break;   // wajah "Nyam"
    default: break;
  }
  if (mode != 255) blendFromPrev(now);
  uint32_t t0 = micros();
  oled.sendBuffer();
  frameMs = (micros() - t0) / 1000;
}

// ------------------------------------------------------------ tombol
static const uint32_t LONG_PRESS_MS = 700;
static bool     btnDown   = false;
static uint32_t btnAt     = 0;
static bool     longFired = false;

static void onShortPress() {
  uint32_t now = millis();
  lastInput = now;
  switch (screen) {
    case SCR_FACE:
      onFaceTap();                       // elus / kasih makan kalau lapar
      break;
    case SCR_MENU:
      menuIndex = (menuIndex + 1) % MENU_COUNT;
      break;
    case SCR_MOOD:
      moodIndex = (moodIndex + 1) % (MOOD_CANCEL + 1);
      break;
    case SCR_EAT:
      break;                             // sedang makan, abaikan tap
    case SCR_MSG:
      markAllRead();
      if (inboxCount > 1) noteIndex = (noteIndex + 1) % inboxCount;
      break;
    case SCR_WX:
      forceWeather = true;               // tap = segar cuaca
      break;
    default:
      screen = (Screen)(((int)screen + 1) % (SCR_BATT + 1));
      if (screen == SCR_MSG) markAllRead();
      break;
  }
  Serial.printf("[ui] tap screen=%s mood=%s\n", SCREEN_NAME[screen], moodName(mood));
}

static void onLongPress() {
  uint32_t now = millis();
  lastInput = now;
  switch (screen) {
    case SCR_FACE:
      openMenu();                        // tahan = buka menu
      break;
    case SCR_MENU:
      onMenuSelect();                    // tahan = pilih item menu
      break;
    case SCR_MOOD:
      if (moodIndex == MOOD_CANCEL) {
        openMenu();                        // "Batal" = kembali ke menu
      } else {
        setMood((Mood)moodIndex);
        react = React::None;
        goFace();
      }
      break;
    case SCR_EAT:
      setMood(Mood::Happy);
      react = React::Love;
      reactUntil = now + 1200;
      goFace();
      break;
    case SCR_MSG:
      clearInbox();
      beep();
      Serial.println("[ui] notif dihapus");
      openMenu();                          // jangan sampai nyangkut di layar notif
      break;
    default:
      // PERBAIKAN: dari layar jam/cuaca/gps/sistem, tahan harus membuka menu.
      // Sebelumnya cabang ini cuma refresh cuaca sehingga menu tak terjangkau
      // dan ESP32 harus di-reset untuk bisa masuk menu lagi.
      openMenu();
      break;
  }
  Serial.printf("[ui] tahan screen=%s mood=%s\n", SCREEN_NAME[screen], moodName(mood));
}

static void updateButton() {
  if (!HAS_BUTTON) return;
  uint32_t now = millis();

  // Stabilkan sinyal (tombol mekanis maupun TTP223 bisa bergetar sebentar).
  static bool rawPrev = false;
  static uint32_t rawAt = 0;
  bool raw = digitalRead(PIN_BTN) == BTN_ACTIVE;
  if (raw != rawPrev) {
    rawPrev = raw;
    rawAt = now;
    return;
  }
  if ((uint32_t)(now - rawAt) < 25) return;
  bool down = raw;

  if (down && !btnDown) {
    btnDown   = true;
    btnAt     = now;
    longFired = false;
  } else if (down && btnDown && !longFired && now - btnAt > LONG_PRESS_MS) {
    longFired = true;
    onLongPress();
  } else if (!down && btnDown) {
    btnDown = false;
    if (!longFired && now - btnAt > 30) onShortPress();
  }
}

// Serial USB: 's' = sentuh pendek, 'l' = tahan (tes tanpa tombol).
// Perintah dari bridge PC lewat kabel USB (tanpa WiFi):
//   N<pengirim>|<pesan>\n             notifikasi WhatsApp
//   T<epoch UTC>\n                    jam dari PC
//   W<temp>|<rh>|<feels>|<wind>|<code>\n  cuaca dari PC
//   G<lat>|<lon>\n                    lokasi cuaca dari PC
//   M<0..4>\n                         ganti mood (0 Senang ... 4 Lapar)
//   B<level>|<0/1>\n                 baterai iPhone (%) + status mengisi
//   K<token>\n                       token bot Telegram (disimpan di NVS)
static void handleSerialLine(const char* line) {
  char kind = line[0];
  const char* p = line + 1;

  if (kind == 'N' || kind == 'n') {
    char from[sizeof(inbox[0].from)], text[sizeof(inbox[0].text)];
    const char* bar = strchr(p, '|');
    if (bar) {
      size_t nf = (size_t)(bar - p);
      if (nf >= sizeof(from)) nf = sizeof(from) - 1;
      memcpy(from, p, nf);
      from[nf] = 0;
      snprintf(text, sizeof(text), "%s", bar + 1);
    } else {
      snprintf(from, sizeof(from), "%s", p);
      text[0] = 0;
    }
    addNotification("WA", from, text);
    return;
  }

  if (kind == 'T' || kind == 't') {
    long long epoch = atoll(p);
    if (epoch > 1700000000LL) {
      struct timeval tv = { (time_t)epoch, 0 };
      settimeofday(&tv, nullptr);
      tzset();
      Serial.println("[jam] diset dari PC (serial)");
    }
    return;
  }

  if (kind == 'M' || kind == 'm') {
    int v = atoi(p);
    if (v >= 0 && v < (int)Mood::Count) {
      setMood((Mood)v);
      react = React::None;
      goFace();
    }
    return;
  }

  if (kind == 'B' || kind == 'b') {
    int lvl = atoi(p);
    const char* bar = strchr(p, '|');
    const char* c = bar ? bar + 1 : "";
    bool chg = atoi(c) > 0 || *c == 't' || *c == 'T' || *c == 'y' || *c == 'Y';
    setPhoneBattery(lvl, chg);
    return;
  }

  if (kind == 'K' || kind == 'k') {
    setTelegramToken(p);
    return;
  }

  if (kind == 'W' || kind == 'w') {
    char fld[5][32] = {{0}};
    uint8_t n = 0;
    const char* q = p;
    while (n < 5) {
      const char* bar = strchr(q, '|');
      size_t len = bar ? (size_t)(bar - q) : strlen(q);
      if (len >= sizeof(fld[n])) len = sizeof(fld[n]) - 1;
      memcpy(fld[n], q, len);
      fld[n][len] = 0;
      n++;
      if (!bar) break;
      q = bar + 1;
    }
    if (n >= 5) {
      float t = atof(fld[0]), rh = atof(fld[1]);
      float feels = atof(fld[2]), wind = atof(fld[3]);
      int code = atoi(fld[4]);
      if (isfinite(t) && !isnan(t)) {
        wx.temp    = t;
        wx.rh      = rh;
        wx.feels   = isnan(feels) ? t : feels;
        wx.wind    = wind;
        wx.code    = code;
        wx.updated = millis();
        wx.valid   = true;
        wxFromSerial = true;
        Serial.printf("[cuaca] dari PC: %.1fC %d%% %s\n",
                      wx.temp, (int)wx.rh, wxText(wx.code));
      }
    }
    return;
  }

  if (kind == 'G' || kind == 'g') {
    const char* bar = strchr(p, '|');
    if (bar) {
      char s[32];
      size_t len = (size_t)(bar - p);
      if (len >= sizeof(s)) len = sizeof(s) - 1;
      memcpy(s, p, len);
      s[len] = 0;
      float la = atof(s);
      snprintf(s, sizeof(s), "%s", bar + 1);
      float lo = atof(s);
      if (isfinite(la) && isfinite(lo) && la >= -90.f && la <= 90.f &&
          lo >= -180.f && lo <= 180.f) {
        saveWxLocation(la, lo);
        wxFromSerial = true;
        Serial.printf("[cuaca] lokasi dari PC: %.4f,%.4f\n", la, lo);
      }
    }
    return;
  }
}

static void updateSerialInput() {
  static char  line[160];
  static uint8_t n = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();

    // tombol tes via Serial Monitor (karakter tunggal, tanpa newline)
    if (n == 0 && (c == 's' || c == 'S')) { onShortPress(); continue; }
    if (n == 0 && (c == 'l' || c == 'L')) { onLongPress(); continue; }

    if (c == '\n' || c == '\r') {
      if (n) {
        line[n] = 0;
        handleSerialLine(line);
        n = 0;
      }
      continue;
    }
    if (n < sizeof(line) - 1) line[n++] = c;
    else n = 0;                       // baris terlalu panjang -> buang
  }
}

// ------------------------------------------------------------ web server
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>mochi dasai</title>
<style>
body{font-family:system-ui,sans-serif;background:#121212;color:#eee;margin:0;padding:20px;max-width:420px}
h3{margin:20px 0 8px}
h3:first-child{margin-top:0}
h3 span{color:#3ddc97;font-weight:400}
input,select,button{font:inherit;width:100%;box-sizing:border-box;margin:6px 0;padding:10px;
  border-radius:10px;border:1px solid #333;background:#1c1c1c;color:#eee}
button{background:#3ddc97;color:#0b0b0b;border:0;font-weight:700}
button.alt{background:#1c1c1c;color:#eee;border:1px solid #3a3a3a;font-weight:600}
a{color:#3ddc97}
</style>
<img src="/mochi.gif?v=2" alt="animasi mochi dasai" width=256
     style="image-rendering:pixelated;display:block;margin:0 auto 6px;border-radius:12px">
<p style="text-align:center;font-size:12px;color:#888;margin:0 0 12px">
  animasi mochi &middot; sleepy.gif &middot; <a href="/mochi.gif">buka GIF</a>
</p>
<h3>layar <span id=now>-</span></h3>
<button id=next>Layar berikutnya</button>
<button class=alt id=refresh>Refresh cuaca + jam</button>
<button class=alt id=clear>Hapus semua notif</button>
<h3>mochi <span id=moodNow>-</span></h3>
<button class=alt onclick="fetch('/mood?set=senang').then(upd)">Senang</button>
<button class=alt onclick="fetch('/mood?set=sedih').then(upd)">Sedih</button>
<button class=alt onclick="fetch('/mood?set=marah').then(upd)">Marah</button>
<button class=alt onclick="fetch('/mood?set=ngantuk').then(upd)">Ngantuk</button>
<button class=alt onclick="fetch('/mood?set=lapar').then(upd)">Lapar</button>
<button id=feed>Kasih makan</button>
<h3>kirim notifikasi</h3>
<form onsubmit="event.preventDefault();
  fetch('/notify?from='+encodeURIComponent(f.value)+'&text='+encodeURIComponent(t.value))
  .then(r=>r.json()).then(()=>{t.value='';upd()})">
  <input id=f value="Tes">
  <input id=t value="Halo dari browser">
  <button>Kirim</button>
</form>
<h3>baterai iPhone <span id=battNow>-</span></h3>
<p style="font-size:12px;color:#888;margin:0 0 6px">
  Otomatis dari iPhone lewat Shortcuts (cara pasang di README). Bisa juga isi
  manual di sini untuk mencoba.
</p>
<form onsubmit="event.preventDefault();
  fetch('/battery?level='+encodeURIComponent(bl.value)+'&charging='+encodeURIComponent(bc.value))
  .then(r=>r.json()).then(()=>battUpd())">
  <input id=bl type=number min=0 max=100 value=85 placeholder="level 0-100">
  <select id=bc>
    <option value=0>tidak mengisi</option>
    <option value=1>mengisi</option>
  </select>
  <button>Kirim baterai</button>
</form>
<h3>buzzer <span id=buzzNow>(uji)</span></h3>
<button class=alt onclick="fetch('/buzz?ms=700')">Bunyi 0.7 detik</button>
<button class=alt onclick="fetch('/buzz?ms=2000')">Bunyi 2 detik</button>
<button class=alt onclick="notifyBeepTest()">Bunyi notif (2 pulse)</button>
<script>
function notifyBeepTest(){
  fetch('/buzz?ms=120').then(()=>setTimeout(()=>fetch('/buzz?ms=120'),180));
}
</script>
<h3>bot Telegram <span id=tgNow>-</span></h3>
<p style="font-size:12px;color:#888;margin:0 0 6px">
  Token dari @BotFather. Tanpa ini, notifikasi Telegram &amp; perintah
  <code>#batt</code> tidak aktif. Butuh WiFi ESP32 yang bisa internet.
</p>
<form onsubmit="event.preventDefault();
  fetch('/telegram?token='+encodeURIComponent(tg.value))
  .then(r=>r.json()).then(d=>{tg.value='';tgUpd()})">
  <input id=tg type=password placeholder="token: 123456789:AAH...">
  <button>Simpan token</button>
</form>
<button class=alt onclick="fetch('/telegram?clear=1').then(r=>r.json()).then(()=>tgUpd())">Hapus token</button>
<h3>lokasi cuaca</h3>
<p style="font-size:12px;color:#888;margin:0 0 6px">
  Cuaca mengikuti GPS modul, lalu lokasi HP (otomatis saat halaman ini dibuka),
  lalu perkiraan kota dari IP WiFi ESP32. Tanpa sinyal GPS pun tetap berubah
  saat kamu pindah tempat. Isi manual kalau mau mengunci satu lokasi.
</p>
<form onsubmit="event.preventDefault();
  fetch('/location?lat='+encodeURIComponent(la.value)+'&lon='+encodeURIComponent(lo.value))
  .then(r=>r.json()).then(l=>{
    $('locNow').textContent=l.lat+', '+l.lon+(l.set?'':' (default)');
    $('autoLoc').checked=false; localStorage.setItem('mochiAutoLoc','0');
    $('geoErr').textContent='Lokasi manual dikunci (auto dimatikan)';
  })">
  <input id=la type=number step=0.0001 placeholder="latitude, mis -6.2000">
  <input id=lo type=number step=0.0001 placeholder="longitude, mis 106.8167">
  <button>Simpan lokasi</button>
</form>
<p style="font-size:12px">Sekarang: <b id=locNow>-</b> &middot;
  <a href="#" onclick="fetch('/location?reset=1').then(r=>r.json()).then(l=>{$('locNow').textContent=l.lat+', '+l.lon+' (default)'});return false">reset</a>
</p>
<label style="font-size:13px"><input id=autoLoc type=checkbox checked> Ikuti lokasi otomatis</label>
<p id=geoErr style="font-size:12px;color:#888"></p>
<button class=alt onclick="locateNow()">Cari lokasi sekarang</button>
<h3>navigasi otomatis</h3>
<p style="font-size:12px;color:#888;margin:0 0 6px">
  Masukkan tujuan (nama tempat atau koordinat). ESP32 mengambil rute dari posisi
  GPS, lalu otomatis mengganti panah belokan saat kamu bergerak. Butuh GPS fix + internet.
</p>
<form onsubmit="event.preventDefault();
  fetch('/find?q='+encodeURIComponent(q.value)).then(r=>r.json()).then(d=>{
    if(d.ok){ la2.value=d.lat; lo2.value=d.lon; $('routeErr').textContent='Ditemukan: '+d.name }
    else $('routeErr').textContent=d.msg || 'Tempat tidak ditemukan';
  })">
  <input id=q placeholder="cari tempat, misal: Monas Jakarta">
  <button>Cari tempat</button>
</form>
<form onsubmit="event.preventDefault();
  fetch('/goto?lat='+encodeURIComponent(la2.value)+'&lon='+encodeURIComponent(lo2.value))
  .then(r=>r.json()).then(d=>{ $('routeErr').textContent=d.msg; navUpd() })">
  <input id=la2 type=number step=0.0001 placeholder="latitude tujuan">
  <input id=lo2 type=number step=0.0001 placeholder="longitude tujuan">
  <button>Mulai navigasi</button>
</form>
<p id=routeInfo style="font-size:12px">-</p>
<p id=routeErr style="font-size:12px;color:#3ddc97"></p>
<button class=alt onclick="fetch('/goto?stop=1').then(r=>r.json()).then(d=>{$('routeErr').textContent=d.msg;navUpd()})">Hentikan navigasi</button>
<h3>rute manual (cadangan)</h3>
<form onsubmit="event.preventDefault();
  fetch('/nav?kind='+encodeURIComponent(k.value)+'&dist='+encodeURIComponent(d.value)+'&road='+encodeURIComponent(r.value))
  .then(()=>upd())">
  <select id=k>
    <option value=left>belok kiri</option>
    <option value=right>belok kanan</option>
    <option value=straight>lurus terus</option>
    <option value=slightleft>agak ke kiri</option>
    <option value=slightright>agak ke kanan</option>
    <option value=uturn>putar balik</option>
    <option value=roundabout>bundaran</option>
  </select>
  <input id=d type=number inputmode=numeric value=200 placeholder="jarak (meter)">
  <input id=r value="Jl. Sudirman" placeholder="nama jalan">
  <button>Kirim rute</button>
</form>
<p><a href="/maps">Peta GPS live (Google Maps)</a> &middot; <a href="/api/status">/api/status</a></p>
<script>
const $=id=>document.getElementById(id);
function upd(){fetch('/api/status').then(r=>r.json()).then(s=>{$('now').textContent='- '+s.screen;$('moodNow').textContent='- '+s.mood})}
function locUpd(){fetch('/location').then(r=>r.json()).then(l=>{$('locNow').textContent=l.lat+', '+l.lon+(l.set?'':' (default)')})}
function battUpd(){fetch('/battery').then(r=>r.json()).then(b=>{
  $('battNow').textContent = b.level<0 ? '-' : b.level+'% '+(b.charging?'(mengisi)':'');
})}
function tgUpd(){fetch('/telegram').then(r=>r.json()).then(t=>{
  $('tgNow').textContent = t.active ? 'aktif' : '-';
})}
function navUpd(){fetch('/api/status').then(r=>r.json()).then(s=>{
  const g=s.gps||{};
  $('routeInfo').textContent = s.route_active
    ? 'Belok '+s.nav+' · sisa '+(s.nav_dist||0)+' m · total '+(s.route_total||0)+' m · '+(g.sats||0)+' satelit'
    : 'Rute tidak aktif · GPS '+(g.fix?'fix':'cari satelit');
})}
function kirimLokasi(lat,lon,sumber){
  fetch('/location?lat='+Number(lat).toFixed(5)+'&lon='+Number(lon).toFixed(5))
    .then(r=>r.json()).then(l=>{
      $('locNow').textContent=l.lat+', '+l.lon+(l.set?'':' (default)');
      $('geoErr').style.color='#3ddc97';
      $('geoErr').textContent='Lokasi dari '+sumber+' tersimpan';
    }).catch(()=>{ $('geoErr').textContent='Gagal mengirim lokasi ke ESP32' });
}
function lokasiDariIP(){
  $('geoErr').style.color='#888';
  $('geoErr').textContent='GPS HP tidak tersedia, pakai lokasi IP...';
  fetch('https://ipwho.is/').then(r=>r.json()).then(d=>{
    if(d.success && isFinite(d.latitude) && isFinite(d.longitude))
      kirimLokasi(d.latitude,d.longitude,'IP ('+(d.city||'?')+')');
    else $('geoErr').textContent='Lokasi IP gagal, isi manual saja';
  }).catch(()=>{ $('geoErr').textContent='Lokasi IP gagal (HP tanpa internet?)' });
}
function locateNow(){
  if(!$('autoLoc').checked) return;
  if(navigator.geolocation){
    $('geoErr').style.color='#888';
    $('geoErr').textContent='Mencari lokasi HP...';
    navigator.geolocation.getCurrentPosition(
      p=>kirimLokasi(p.coords.latitude,p.coords.longitude,'GPS HP'),
      e=>lokasiDariIP(),
      {enableHighAccuracy:false, timeout:8000, maximumAge:300000}
    );
  } else lokasiDariIP();
}
$('autoLoc').onchange=()=>{
  localStorage.setItem('mochiAutoLoc',$('autoLoc').checked?'1':'0');
  if($('autoLoc').checked) locateNow();
  else $('geoErr').textContent='Lokasi otomatis dimatikan';
};
$('next').onclick=()=>fetch('/next').then(upd);
$('refresh').onclick=()=>fetch('/refresh');
$('clear').onclick=()=>fetch('/clear').then(upd);
$('feed').onclick=()=>fetch('/feed').then(upd);
function syncClock(){fetch('/time?epoch='+Math.floor(Date.now()/1000)).catch(()=>{})}
syncClock(); setInterval(syncClock, 60000);
$('autoLoc').checked = localStorage.getItem('mochiAutoLoc') !== '0';
upd(); locUpd(); navUpd(); battUpd(); tgUpd();
if($('autoLoc').checked) locateNow();
setInterval(()=>{ if($('autoLoc').checked) locateNow() }, 600000);
setInterval(navUpd, 3000);
</script>
)HTML";

// Halaman monitor GPS: peta Google Maps live tanpa API key.
// Peta di-render langsung di browser HP (butuh internet di HP), koordinat
// diambil dari /api/status yang dibaca ESP32 dari modul GPS.
static const char MAP_HTML[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>mochi gps monitor</title>
<style>
body{font-family:system-ui,sans-serif;background:#121212;color:#eee;margin:0;padding:14px}
h3{margin:0 0 8px} h3 span{color:#3ddc97;font-weight:400;font-size:14px}
#info{font-size:13px;line-height:1.6;background:#1c1c1c;border:1px solid #333;
  border-radius:10px;padding:8px 10px;margin-bottom:10px}
#info b{color:#3ddc97}
iframe{width:100%;height:65vh;min-height:340px;border:0;border-radius:10px;background:#1c1c1c}
p{font-size:13px} a{color:#3ddc97}
button{font:inherit;padding:8px 12px;border-radius:8px;border:0;background:#3ddc97;color:#0b0b0b;font-weight:700}
</style>
<h3>GPS monitor <span id=stat>-</span></h3>
<div id=info>Menunggu data GPS...</div>
<iframe id=map title="Peta Google Maps" loading="lazy"></iframe>
<p><a id=open target=_blank rel=noopener>Buka di Google Maps</a> &middot; <a href="/">kontrol</a></p>
<p><button onclick="location.reload()">Muat ulang</button> &middot; otomatis tiap 3 detik</p>
<script>
const $ = id => document.getElementById(id);
let defaultLL = null;   // diisi dari /api/status (lokasi cuaca / HOME)
let lastLL = '', lastFixLL = '';
function f(v, d){ return (typeof v === 'number' && isFinite(v)) ? v.toFixed(d) : '-'; }
function upd(){
  fetch('/api/status').then(r => r.json()).then(s => {
    const g = s.gps || {};
    const fix = !!g.fix;
    if (!fix && !lastFixLL && typeof s.wx_lat === 'number')
      defaultLL = f(s.wx_lat,6) + ',' + f(s.wx_lon,6);
    const ll = fix ? f(g.lat,6) + ',' + f(g.lon,6)
                   : (lastFixLL || defaultLL || '-6.2088,106.8456');
    if (fix) lastFixLL = ll;
    $('stat').textContent = '- ' + (fix ? 'GPS fix' : 'cari satelit');
    $('info').innerHTML =
      'Posisi: <b>' + (fix ? f(g.lat,6) + ', ' + f(g.lon,6) : 'belum tersedia') + '</b><br>' +
      'Satelit: <b>' + (g.sats||0) + '</b> &middot; HDOP: <b>' + f(g.hdop,1) + '</b> &middot; ' +
      'umur data: <b>' + (g.age_ms||0) + ' ms</b><br>' +
      'Kecepatan: <b>' + f(g.speed_kmh,1) + ' km/j</b> &middot; arah: <b>' + f(g.course,0) + '&deg;</b> &middot; ' +
      'altitude: <b>' + f(g.alt_m,0) + ' m</b><br>' +
      'Perjalanan: <b>' + f(g.trip_km,2) + ' km</b> &middot; WiFi: <b>' + (g.rssi_dbm||s.rssi||0) + ' dBm</b>';
    if (ll !== lastLL) {
      lastLL = ll;
      const q = encodeURIComponent(ll);
      $('map').src = 'https://www.google.com/maps?q=' + q + '&z=16&output=embed';
      $('open').href = 'https://www.google.com/maps?q=' + q + '&z=16';
    }
  }).catch(() => { $('stat').textContent = '- ESP32 offline'; });
}
upd(); setInterval(upd, 3000);
</script>
)HTML";

// ------------------------------------------------------------ navigasi otomatis
static String urlEncode(const String& s) {
  static const char* HEXCH = "0123456789ABCDEF";
  String out;
  for (unsigned i = 0; i < s.length(); ++i) {
    unsigned char c = (unsigned char)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      out += (char)c;
    else if (c == ' ') out += '+';
    else { out += '%'; out += HEXCH[c >> 4]; out += HEXCH[c & 15]; }
  }
  return out;
}

// Cari nama tempat lewat Nominatim (gratis, tanpa API key).
// true -> lat/lon/name terisi. false -> msg siap ditampilkan di web.
static bool geocodePlace(const String& q, double& la, double& lo,
                         String& name, String& msg) {
  if (WiFi.status() != WL_CONNECTED) {
    msg = "WiFi mati - sambungkan ESP32 ke WiFI_RUMAH";
    return false;
  }

  // 1 = sukses, 0 = tidak ada hasil, -1/-2 = gagal, >0 = kode HTTP
  auto fetchOne = [&](const String& query)->int {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(9000);
    http.setUserAgent("mochi-dasai/1.0 (ESP32)");
    String url = "https://nominatim.openstreetmap.org/search?q=" +
                 urlEncode(query) + "&format=jsonv2&limit=1";
    if (!http.begin(tls, url)) return -1;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return code; }
    String payload = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, payload)) return -2;
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() == 0) return 0;
    la   = atof(arr[0]["lat"] | "0");
    lo   = atof(arr[0]["lon"] | "0");
    name = arr[0]["display_name"] | "";
    return 1;
  };

  int r = fetchOne(q);
  // Nama pendek/daerah sering miss -> ulangi dengan ", Indonesia".
  String lower = q; lower.toLowerCase();
  if (r == 0 && lower.indexOf("indonesia") < 0) {
    delay(1100);                       // Nominatim: maksimal 1 request/detik
    r = fetchOne(q + ", Indonesia");
  }

  if (r == 1) { msg = ""; return true; }
  if (r == 0)      msg = "Tempat tidak ditemukan - coba lebih spesifik, misal \"Monas Jakarta\"";
  else if (r < 0)  msg = "Gagal hubungi server pencarian";
  else             msg = "Server sibuk, coba lagi beberapa saat";
  return false;
}

static uint8_t mapManeuver(const char* type, const char* modifier) {
  if (!type) type = "";
  String m = modifier ? modifier : "";
  m.toLowerCase();
  if (strcmp(type, "arrive") == 0) return NAV_ARRIVE;
  if (m.indexOf("uturn") >= 0) return NAV_UTURN;
  if (strcmp(type, "roundabout") == 0 || strcmp(type, "rotary") == 0 ||
      m.indexOf("roundabout") >= 0) return NAV_ROUNDABOUT;
  if (m.indexOf("slight") >= 0)
    return m.indexOf("left") >= 0 ? NAV_SLIGHT_LEFT : NAV_SLIGHT_RIGHT;
  if (m.indexOf("sharp") >= 0)
    return m.indexOf("left") >= 0 ? NAV_LEFT : NAV_RIGHT;
  if (m.indexOf("left") >= 0) return NAV_LEFT;
  if (m.indexOf("right") >= 0) return NAV_RIGHT;
  return NAV_STRAIGHT;
}

static double routeStartLat() {
  if (gps.location.isValid() && gps.location.age() < 5000) return gps.location.lat();
  if (wxLocSet) return wxLat;
  if (!isnan(wxIpLat)) return wxIpLat;
  return HOME_LAT;
}
static double routeStartLon() {
  if (gps.location.isValid() && gps.location.age() < 5000) return gps.location.lng();
  if (wxLocSet) return wxLon;
  if (!isnan(wxIpLon)) return wxIpLon;
  return HOME_LON;
}

static void applyRouteStep() {
  if (!routeActive || routeCount == 0) return;
  if (routeIdx >= routeCount) {
    nav.kind = NAV_ARRIVE;
    nav.dist = 0;
    snprintf(nav.road, sizeof(nav.road), "TIBA DI TUJUAN");
    nav.at = millis();
    nav.valid = true;
    routeActive = false;
    Serial.println("[nav] tiba di tujuan");
    return;
  }
  const RouteStep& s = routeSteps[routeIdx];
  long rem = (long)s.atM - (long)routeTravelled;
  if (rem < 0) rem = 0;
  if (rem > 60000) rem = 60000;
  nav.kind = s.kind;
  nav.dist = (uint16_t)rem;
  snprintf(nav.road, sizeof(nav.road), "%s", s.road);
  nav.at = millis();
  nav.valid = true;
}

// Coba beberapa server OSRM publik. ESP32 sering gagal di TLS handshake untuk
// host ini, jadi plain HTTP dicoba lebih dulu (HTTPS jadi cadangan).
static bool fetchRoute(const char* host, bool secure, double slon, double slat,
                       double destLon, double destLat, String& payload) {
  WiFiClientSecure tls;
  tls.setInsecure();
  WiFiClient plain;
  HTTPClient http;
  http.setConnectTimeout(7000);
  http.setTimeout(15000);
  http.setUserAgent("mochi-dasai/1.0");

  char url[240];
  snprintf(url, sizeof(url),
           "%s://%s/route/v1/driving/%.6f,%.6f;%.6f,%.6f"
           "?steps=true&overview=false&geometries=geojson",
           secure ? "https" : "http", host, slon, slat, destLon, destLat);
  bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
  if (!begun) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[nav] %s://%s -> HTTP %d\n",
                  secure ? "https" : "http", host, code);
    http.end();
    return false;
  }
  payload = http.getString();
  http.end();
  return payload.length() > 0;
}

// Ambil rute dari posisi sekarang ke tujuan, lalu simpan petunjuk belokannya.
static bool startRoute(double destLat, double destLon, char* msg, size_t msgLen) {
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(msg, msgLen, "WiFi belum tersambung");
    return false;
  }
  double slat = routeStartLat(), slon = routeStartLon();

  struct RouteHost { const char* host; bool secure; };
  static const RouteHost kRouteHosts[] = {
    {"routing.openstreetmap.de/routed-car", false},
    {"router.project-osrm.org",             false},
    {"routing.openstreetmap.de/routed-car", true },
    {"router.project-osrm.org",             true }
  };
  String payload;
  bool got = false;
  for (unsigned i = 0; i < sizeof(kRouteHosts) / sizeof(kRouteHosts[0]); ++i) {
    if (fetchRoute(kRouteHosts[i].host, kRouteHosts[i].secure,
                   slon, slat, destLon, destLat, payload)) {
      got = true;
      break;
    }
  }
  if (!got) {
    snprintf(msg, msgLen, "Server rute tidak bisa diakses");
    return false;
  }

  // Filter supaya ArduinoJson tidak menyimpan intersections & geometry.
  JsonDocument filter;
  filter["code"] = true;
  filter["routes"][0]["legs"][0]["steps"][0]["maneuver"]["type"] = true;
  filter["routes"][0]["legs"][0]["steps"][0]["maneuver"]["modifier"] = true;
  filter["routes"][0]["legs"][0]["steps"][0]["name"] = true;
  filter["routes"][0]["legs"][0]["steps"][0]["distance"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    snprintf(msg, msgLen, "JSON rute error");
    return false;
  }
  JsonArray steps = doc["routes"][0]["legs"][0]["steps"];
  if (steps.size() < 2) {
    snprintf(msg, msgLen, "Rute tidak ditemukan");
    return false;
  }

  routeCount = 0;
  uint32_t cum = 0;
  for (size_t i = 0; i + 1 < steps.size() && routeCount < 24; ++i) {
    JsonObject st = steps[i];
    cum += (uint32_t)(st["distance"] | 0.0);
    JsonObject mv = steps[i + 1]["maneuver"];
    const char* type = mv["type"] | "";
    const char* mod  = mv["modifier"] | "";
    const char* name = st["name"] | "";
    if (!name || !*name) name = steps[i + 1]["name"] | "";

    RouteStep& r = routeSteps[routeCount++];
    r.kind = mapManeuver(type, mod);
    r.atM  = cum;
    snprintf(r.road, sizeof(r.road), "%s", name ? name : "");
  }
  if (routeCount == 0) {
    snprintf(msg, msgLen, "Rute kosong");
    return false;
  }

  routeIdx = 0;
  routeTravelled = 0;
  routeTotal = cum;
  routeLastLat = slat;
  routeLastLon = slon;
  routeActive = true;
  screen = SCR_GPS;
  applyRouteStep();

  snprintf(msg, msgLen, "Rute aktif: %d belokan, %u m",
           (int)routeCount, (unsigned)routeTotal);
  Serial.printf("[nav] %s (dari %.5f,%.5f)\n", msg, slat, slon);
  return true;
}

// Maju ke petunjuk berikutnya mengikuti jarak yang benar-benar ditempuh GPS.
static void navTask() {
  if (!routeActive || routeCount == 0) return;
  if (!gps.location.isValid() || gps.location.age() > 5000) return;

  double lat = gps.location.lat();
  double lon = gps.location.lng();
  double d = TinyGPSPlus::distanceBetween(routeLastLat, routeLastLon, lat, lon);
  if (d >= 0 && d < 500.0) {
    routeTravelled += (uint32_t)d;
    routeLastLat = lat;
    routeLastLon = lon;
  } else if (d >= 500.0) {
    routeLastLat = lat;                 // lonjakan sinyal: reset acuan
    routeLastLon = lon;
  }

  static uint8_t  shownIdx = 255;
  static uint32_t lastUpd  = 0;
  while (routeIdx < routeCount && routeTravelled >= routeSteps[routeIdx].atM)
    ++routeIdx;

  if (routeIdx != shownIdx || millis() - lastUpd > 1000) {
    shownIdx = routeIdx;
    lastUpd  = millis();
    applyRouteStep();
  }
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", PAGE_HTML); });
  web.on("/maps", HTTP_GET, []() { web.send_P(200, "text/html", MAP_HTML); });
  web.on("/map", HTTP_GET, []() { web.send_P(200, "text/html", MAP_HTML); });
  // GIF animasi mochi (sama dengan yang dipakai di OLED).
  web.on("/mochi.gif", HTTP_GET, []() {
    // Jangan cache: dulu max-age=86400 bikin HP tetap tampil GIF lama.
    web.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    web.sendHeader("Pragma", "no-cache");
    web.sendHeader("Expires", "0");
    web.send_P(200, "image/gif", (const char*)MOCHI_WEB_GIF, MOCHI_WEB_GIF_LEN);
  });

  // HP (iOS/Android) suka membuka halaman "login WiFi" sendiri. Semua
  // permintaan asing kita arahkan ke halaman kontrol supaya popup muncul.
  web.onNotFound([]() {
    String host = web.hostHeader();
    if (host.length() == 0) host = "192.168.4.1";
    web.sendHeader("Location", "http://" + host + "/", true);
    web.send(302, "text/plain", "");
  });

  // jam dikirim dari browser HP: /time?epoch=1700000000
  // berguna saat ESP32 tidak punya internet (mode hotspot saja)
  web.on("/time", HTTP_GET, []() {
    if (web.hasArg("epoch")) {
      unsigned long long e = strtoull(web.arg("epoch").c_str(), nullptr, 10);
      if (e > 1700000000ULL) {
        struct timeval tv;
        tv.tv_sec  = (time_t)e;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        Serial.printf("[jam] diset dari browser: %llu\n", e);
      }
    }
    web.send(200, "text/plain", "ok");
  });

  // Lokasi cuaca: GPS > lokasi yang diset di sini (NVS) > HOME_LAT/LON.
  // Contoh: /location?lat=-6.2&lon=106.8   atau   /location?reset=1
  web.on("/location", HTTP_GET, []() {
    if (web.hasArg("reset")) {
      clearWxLocation();
      forceWeather = true;
      Serial.println("[cuaca] lokasi di-reset ke default");
    } else if (web.hasArg("lat") && web.hasArg("lon")) {
      float la = web.arg("lat").toFloat();
      float lo = web.arg("lon").toFloat();
      if (la >= -90.0f && la <= 90.0f && lo >= -180.0f && lo <= 180.0f) {
        saveWxLocation(la, lo);
        forceWeather = true;
        Serial.printf("[cuaca] lokasi diset: %.4f,%.4f\n", la, lo);
      }
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"lat\":%.4f,\"lon\":%.4f,\"set\":%s}",
             wxLat, wxLon, wxLocSet ? "true" : "false");
    web.send(200, "application/json", out);
  });

  // Cari nama tempat -> koordinat (Nominatim, gratis, tanpa API key).
  web.on("/find", HTTP_GET, []() {
    if (!web.hasArg("q") || web.arg("q").length() == 0) {
      web.send(200, "application/json",
               "{\"ok\":false,\"msg\":\"Ketik nama tempat dulu\"}");
      return;
    }
    double la = 0, lo = 0;
    String name, msg;
    bool ok = geocodePlace(web.arg("q"), la, lo, name, msg);

    JsonDocument out;
    out["ok"] = ok;
    if (ok) {
      out["lat"]  = la;
      out["lon"]  = lo;
      out["name"] = name;
    } else {
      out["msg"] = msg;
    }
    String body;
    serializeJson(out, body);
    web.send(200, "application/json", body);
  });

  // Navigasi otomatis: /goto?lat=&lon=  atau  /goto?stop=1
  web.on("/goto", HTTP_GET, []() {
    if (web.hasArg("stop")) {
      routeActive = false;
      routeCount  = 0;
      nav.valid   = false;
      web.send(200, "application/json",
               "{\"ok\":true,\"msg\":\"Navigasi dihentikan\"}");
      return;
    }
    if (!web.hasArg("lat") || !web.hasArg("lon")) {
      web.send(200, "application/json",
               "{\"ok\":false,\"msg\":\"Isi lat & lon tujuan\"}");
      return;
    }
    double la = web.arg("lat").toDouble();
    double lo = web.arg("lon").toDouble();
    if (la < -90 || la > 90 || lo < -180 || lo > 180) {
      web.send(200, "application/json",
               "{\"ok\":false,\"msg\":\"Koordinat tidak valid\"}");
      return;
    }
    char msg[96];
    bool ok = startRoute(la, lo, msg, sizeof(msg));

    JsonDocument out;
    out["ok"]    = ok;
    out["msg"]   = msg;
    out["steps"] = routeCount;
    out["total"] = routeTotal;
    String body;
    serializeJson(out, body);
    web.send(200, "application/json", body);
  });

  // notifikasi lewat GET: /notify?from=..&text=..
  web.on("/notify", HTTP_GET, []() {
    String from = web.arg("from");
    String text = web.arg("text");
    if (text.length() == 0) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"text kosong\"}");
      return;
    }
    addNotification("WA", from.c_str(), text.c_str());
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // notifikasi lewat POST: json {"from":"..","text":".."} atau form biasa
  web.on("/notify", HTTP_POST, []() {
    String from, text;
    if (web.hasArg("plain")) {
      String body = web.arg("plain");
      JsonDocument doc;
      if (body.length() && body[0] == '{' && !deserializeJson(doc, body)) {
        from = String((const char*)(doc["from"] | "WhatsApp"));
        text = String((const char*)(doc["text"] | ""));
      }
    }
    if (text.length() == 0) {
      from = web.arg("from");
      text = web.arg("text");
    }
    if (text.length() == 0) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"text kosong\"}");
      return;
    }
    addNotification("WA", from.c_str(), text.c_str());
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- kontrol dari HP (pengganti tombol fisik) ----
  web.on("/next", HTTP_GET, []() {
    screen = (Screen)(((int)screen + 1) % (SCR_BATT + 1));
    Serial.printf("[web] layar -> %s\n", SCREEN_NAME[screen]);
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- navigasi belokan: /nav?kind=left&dist=200&road=Jl.%20Sudirman
  web.on("/nav", HTTP_GET, []() {
    String k = web.arg("kind");
    k.toLowerCase();

    uint8_t kind = NAV_STRAIGHT;
    if      (k == "left"        || k == "kiri")        kind = NAV_LEFT;
    else if (k == "right"       || k == "kanan")       kind = NAV_RIGHT;
    else if (k == "slightleft"  || k == "agakkiri")    kind = NAV_SLIGHT_LEFT;
    else if (k == "slightright" || k == "agakkanan")   kind = NAV_SLIGHT_RIGHT;
    else if (k == "uturn"       || k == "putarbalik")  kind = NAV_UTURN;
    else if (k == "roundabout"  || k == "bundaran")    kind = NAV_ROUNDABOUT;

    long d = web.hasArg("dist") ? web.arg("dist").toInt() : 200;
    if (d < 0) d = 0;
    if (d > 60000) d = 60000;

    routeActive = false;                 // rute manual menimpa rute otomatis
    nav.kind  = kind;
    nav.dist  = (uint16_t)d;
    nav.at    = millis();
    nav.valid = true;
    sanitize(nav.road, sizeof(nav.road),
             web.hasArg("road") ? web.arg("road").c_str() : "");

    screen = SCR_GPS;                     // langsung tampilkan di OLED
    Serial.printf("[nav] %s %um %s\n", navText(kind), (unsigned)nav.dist, nav.road);

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"kind\":\"%s\",\"dist\":%u}",
             navText(kind), (unsigned)nav.dist);
    web.send(200, "application/json", out);
  });

  // diagnosa wajah: /face?frame=45 -> diam di 1 frame; /face?auto=1 -> animasi lagi
  web.on("/face", HTTP_GET, []() {
    if (web.hasArg("auto")) {
      faceFrozen = -1;
    } else if (web.hasArg("frame")) {
      int n = web.arg("frame").toInt();
      if (n < 0) n = 0;
      if (n >= DASAI_FRAME_COUNT) n = DASAI_FRAME_COUNT - 1;
      faceFrozen = n;
      screen = SCR_FACE;
    }
    Serial.printf("[web] face frame=%d\n", (int)faceFrozen);
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // baterai iPhone dari Shortcuts: /battery?level=85&charging=1
  web.on("/battery", HTTP_GET, []() {
    if (web.hasArg("level")) {
      int lvl = web.arg("level").toInt();
      String cs = web.arg("charging");
      cs.toLowerCase();
      bool chg = web.hasArg("charging") &&
                 (cs == "1" || cs == "true" || cs == "yes" || cs == "on");
      setPhoneBattery(lvl, chg);
    }
    char out[96];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"level\":%d,\"charging\":%s}",
             (int)phoneBatt, phoneCharging ? "true" : "false");
    web.send(200, "application/json", out);
  });

  // uji buzzer: /buzz?ms=600   (tanpa arg = 400ms)
  web.on("/buzz", HTTP_GET, []() {
    if (!HAS_BUZZER) {
      web.send(200, "application/json", "{\"ok\":false,\"msg\":\"buzzer mati\"}");
      return;
    }
    int ms = web.hasArg("ms") ? web.arg("ms").toInt() : 400;
    if (ms < 50) ms = 50;
    if (ms > 10000) ms = 10000;
    buzzOn(2000);
    webBuzzOffAt = millis() + ms;
    web.send(200, "application/json", "{\"ok\":true,\"ms\":" + String(ms) + "}");
  });

  // token bot Telegram: /telegram?token=...  atau /telegram?clear=1
  // Token tidak dikirim kembali ke browser ( hanya panjangnya ).
  web.on("/telegram", HTTP_GET, []() {
    if (web.hasArg("clear")) {
      tgToken[0] = 0; tgGen++;
      prefs.remove("tgtok");
      Serial.println("[tg] token dihapus");
    } else if (web.hasArg("token")) {
      String t = web.arg("token");
      t.trim();
      if (t.length() >= 20) setTelegramToken(t.c_str());
    }
    char out[80];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"active\":%s,\"len\":%zu}",
             telegramEnabled() ? "true" : "false", strlen(tgToken));
    web.send(200, "application/json", out);
  });

  // kontrol mood / kasih makan dari web: /mood?set=marah , /feed
  web.on("/mood", HTTP_GET, []() {
    String s = web.arg("set");
    s.toLowerCase();
    Mood m = mood;
    if      (s == "senang" || s == "happy")            m = Mood::Happy;
    else if (s == "sedih"  || s == "sad")              m = Mood::Sad;
    else if (s == "marah"  || s == "angry")            m = Mood::Angry;
    else if (s == "ngantuk"|| s == "sleepy")           m = Mood::Sleepy;
    else if (s == "lapar"  || s == "hungry")           m = Mood::Hungry;
    setMood(m);
    react = React::None;
    screen = SCR_FACE;
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"mood\":\"%s\"}", moodName(mood));
    web.send(200, "application/json", out);
  });

  web.on("/feed", HTTP_GET, []() {
    screen = SCR_EAT;
    screenSince = millis();
    markFed();
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/clear", HTTP_GET, []() {
    clearInbox();
    Serial.println("[web] notif dihapus");
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/refresh", HTTP_GET, []() {
    forceWeather = true;
    configTzTime(TZ_STRING, "pool.ntp.org", "id.pool.ntp.org");
    Serial.println("[web] refresh cuaca + jam");
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["uptime_s"] = millis() / 1000;
    doc["ap"]       = WiFi.softAPIP().toString();
    doc["ip"]       = WiFi.localIP().toString();
    doc["rssi"]     = WiFi.RSSI();
    doc["heap"]     = ESP.getFreeHeap();
    doc["buzz"]     = BUZZER_ACTIVE ? digitalRead(BUZZ_PINS_ALL[0]) : -1;
    doc["screen"]   = SCREEN_NAME[screen];
    doc["mood"]     = moodName(mood);
    {
      struct tm ct;
      char tbuf[24];
      if (localNow(&ct)) {
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                 ct.tm_hour, ct.tm_min, ct.tm_sec);
        doc["time"]  = tbuf;
        doc["epoch"] = (uint32_t)time(nullptr);
      } else {
        doc["time"] = "-";
      }
      doc["tz"] = TZ_STRING;
    }
    doc["wx_lat"]    = wxLat;
    doc["wx_lon"]    = wxLon;
    doc["wx_loc_set"] = wxLocSet;
    doc["wx_src"]     = wxSource();        // gps | web | ip | default
    if (!isnan(wxIpLat)) {
      doc["wx_ip_lat"] = wxIpLat;
      doc["wx_ip_lon"] = wxIpLon;
    }
    doc["last_fed"] = lastFed;
    doc["unread"]   = unreadCount;
    doc["messages"] = inboxCount;
    doc["battery_level"]    = (int)phoneBatt;
    doc["battery_charging"] = phoneCharging;
    doc["battery_age_s"]    = phoneBattAt ? (millis() - phoneBattAt) / 1000 : 0;
    doc["nav"]      = navActive() ? navText(nav.kind) : "";
    doc["nav_dist"] = navActive() ? nav.dist : 0;
    doc["route_active"] = routeActive;
    doc["route_total"]  = routeTotal;

    JsonObject g = doc["gps"].to<JsonObject>();
    bool fix = gps.location.isValid() && gps.location.age() < 5000;
    g["fix"]  = fix;
    g["sats"] = gps.satellites.isValid() ? gps.satellites.value() : 0;
    if (fix) {
      g["lat"] = gps.location.lat();
      g["lon"] = gps.location.lng();
    }
    // Ekstra data untuk halaman /maps.
    g["age_ms"]    = gps.location.age();
    g["hdop"]      = gps.hdop.isValid() ? gps.hdop.hdop() : 0;
    g["alt_m"]     = gps.altitude.isValid() ? gps.altitude.meters() : 0;
    g["speed_kmh"] = gps.speed.isValid() ? gps.speed.kmph() : 0;
    g["course"]    = gps.course.isValid() ? gps.course.deg() : 0;
    g["trip_km"] = tripKm;

    JsonObject w = doc["weather"].to<JsonObject>();
    w["valid"] = wx.valid;
    if (wx.valid) {
      w["temp"] = wx.temp;
      w["rh"]   = wx.rh;
      w["code"] = wx.code;
      w["text"] = wxText(wx.code);
    }

    String out;
    serializeJson(doc, out);
    web.send(200, "application/json", out);
  });

  web.onNotFound([]() { web.send(404, "text/plain", "404"); });
  web.begin();
}

// ------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== mochi dasai ===");

  // mood & waktu makan diambil dari NVS supaya tetap ingat setelah reboot
  nvsBegin();
  uint32_t storedMood = nvsGet("mood", 0);
  if (storedMood >= (uint32_t)Mood::Count) storedMood = 0;
  mood = (Mood)storedMood;
  lastFed = nvsGet("fed", 0);

  // Baterai terakhir tetap tampil setelah reboot (umur dianggap 0 =
  // "tersimpan" sampai Shortcuts mengirim data baru).
  {
    uint32_t b = nvsGet("batt", 255);
    if (b <= 100) {
      phoneBatt     = (int8_t)b;
      phoneCharging = nvsGet("battchg", 0) != 0;
      phoneBattAt   = 0;
    }
  }

  // Token Telegram dari web (kalau belum ada, pakai nilai di config.h).
  {
    String saved = prefs.getString("tgtok", "");
    if (saved.length() >= 20) {
      snprintf(tgToken, sizeof(tgToken), "%s", saved.c_str());
    }
    if (telegramEnabled()) Serial.println("[tg] aktif (token dari NVS/config)");
  }

  // Lokasi cuaca yang pernah diset dari web (dipakai kalau GPS tidak fix).
  wxLocSet = prefs.getBool("wxlocset", false);
  if (wxLocSet) {
    wxLat = prefs.getFloat("wxlat", HOME_LAT);
    wxLon = prefs.getFloat("wxlon", HOME_LON);
  }
  faceHintUntil = millis() + 8000;
  lastInput = millis();
  Serial.printf("[mood] boot: %s (lastFed=%lu)\n",
                moodName(mood), (unsigned long)lastFed);
  Serial.printf("[cuaca] lokasi: %.4f,%.4f %s\n", wxLat, wxLon,
                wxLocSet ? "(dari web)" : "(default)");

  if (HAS_BUTTON) {
    // Tombol ke GND aktif LOW; TTP223 aktif HIGH.
    pinMode(PIN_BTN, BTN_ACTIVE == LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
  if (HAS_BUZZER) {
    for (uint8_t i = 0; i < BUZZ_PIN_COUNT; ++i) {
      pinMode(BUZZ_PINS_ALL[i], OUTPUT);
      digitalWrite(BUZZ_PINS_ALL[i], LOW);
      // Naikkan drive strength: default ~20mA sering kurang untuk buzzer aktif.
      gpio_set_drive_capability((gpio_num_t)BUZZ_PINS_ALL[i], GPIO_DRIVE_CAP_3);
    }
  }

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  oled.setI2CAddress(OLED_ADDR << 1);
  oled.begin();
  oled.setBusClock(400000);
  oled.setContrast(180);

  // Splash restart: animasi mochi DASAI asli (90 frame, sebelum GIF gift).
  // Full contrast sejak awal -- tidak ada efek redup/kedip.
  for (int i = 0; i < 24; ++i) {
    oled.clearBuffer();
    oled.drawXBMP(0, 0, DASAI_FRAME_W, DASAI_FRAME_H,
                  dasaiFrame((uint8_t)(FACE_LOOP_FIRST + i)));
    oled.sendBuffer();
    delay(60);
  }
  faceStartFrame = FACE_LOOP_FIRST + 24; // lanjut tanpa loncat ke frame 0

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // ---- hotspot sendiri (selalu nyala, HP bisa langsung connect)
  // Catatan: dulu di sini ada WiFi.scanNetworks() untuk diagnosa. Scan bikin
  // radio pindah mode STA<->AP dan sering mengganggu AP, jadi dihapus.
  WiFi.persistent(false);              // jangan tulis kredensial ke NVS
  WiFi.mode(AP_ONLY ? WIFI_AP : WIFI_AP_STA);
  WiFi.setHostname(DEVICE_ID);

  // Turunkan daya pancar SEBELUM AP start. Di board Super Mini, 19 dBm bikin
  // arus puncak WiFi tinggi -> catu USB drop -> hotspot hilang sendiri.
  esp_err_t e1 = esp_wifi_set_max_tx_power((int8_t)WIFI_POWER_8_5dBm);
  Serial.printf("[ap] set tx (sebelum AP) = 0x%x %s\n", (int)e1, esp_err_to_name(e1));

  if (strlen(AP_PASS) >= 8) apOk = WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);
  else                      apOk = WiFi.softAP(AP_SSID);

  // Set ulang setelah AP start (driver bisa menimpa saat start).
  esp_err_t e2 = esp_wifi_set_max_tx_power((int8_t)WIFI_POWER_8_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);       // jangan sleep, beacon tetap teratur
  int8_t txNow = 0;
  esp_err_t e3 = esp_wifi_get_max_tx_power(&txNow);
  Serial.printf("[ap] set tx (sesudah AP) = 0x%x, get = 0x%x -> %d dBm\n",
                (int)e2, (int)e3, (int)(txNow / 4));

  Serial.printf("[ap] softAP=%d  \"%s\"  ch%d  ip=%s\n",
                (int)apOk, AP_SSID, WiFi.channel(),
                WiFi.softAPIP().toString().c_str());
  if (!apOk) Serial.println("[ap] GAGAL bikin hotspot!");

  // ukur kecepatan kirim 1 frame penuh ke OLED (penting buat fps)
  {
    uint32_t t0 = micros();
    for (int i = 0; i < 8; ++i) {
      oled.clearBuffer();
      oled.drawXBMP(0, 0, DASAI_FRAME_W, DASAI_FRAME_H,
                    dasaiFrame((uint8_t)(FACE_LOOP_FIRST + i)));
      oled.sendBuffer();
    }
    Serial.printf("[oled] 1 frame 128x64 = %lu ms -> maksimal %lu fps\n",
                  (unsigned long)((micros() - t0) / 8000),
                  (unsigned long)(8000000UL / ((micros() - t0) / 8 + 1)));
  }

  if (!AP_ONLY) {
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);
  }

  // zona waktu dipakai juga saat jam dikirim dari browser (tanpa internet)
  setenv("TZ", TZ_STRING, 1);
  tzset();

  mqtt.setBufferSize(512);
  mqtt.setCallback(onMqttMessage);
  if (mqttEnabled()) mqtt.setServer(MQTT_HOST, MQTT_PORT);

  setupWeb();
}

void loop() {
  static uint32_t lastRender = 0;
  static bool     ipLogged   = false;

  pumpGps();
  navTask();
  updateButton();
  notifyBeepTask();
  webBuzzTask();
  updateSerialInput();
  wifiTask();
  mqttTask();
  telegramTask();
  timeTask();
  updateHunger();
#if AUTO_IP_GEO
  geoIpTask();
#endif
  weatherTask();
  web.handleClient();

  static bool radioReady = false;
  if (!radioReady) {
    radioReady = true;
    WiFi.setSleep(false);                // hotspot lebih responsif
    if (MDNS.begin(DEVICE_ID)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mdns] http://%s.local/\n", DEVICE_ID);
    } else {
      Serial.println("[mdns] tidak aktif (pakai IP saja)");
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!ipLogged) {
      ipLogged = true;
      Serial.printf("[wifi] tersambung  http://%s/\n",
                    WiFi.localIP().toString().c_str());
    }
  }

  // laporan kesehatan tiap 5 detik -> kelihatan di Serial Monitor
  {
    static uint32_t lastStat = 0;
    if (millis() - lastStat > 5000) {
      lastStat = millis();
      Serial.printf("[stat] ap=%d ch%d klien=%d heap=%u frame=%lums layar=%s\n",
                    (int)apOk, WiFi.channel(), WiFi.softAPgetStationNum(),
                    (unsigned)ESP.getFreeHeap(), (unsigned long)frameMs,
                    SCREEN_NAME[screen]);
    }
  }

  // jaga-jaga: kalau hotspot mati sendiri, hidupkan ulang
  {
    static uint32_t lastApCheck = 0;
    if (millis() - lastApCheck > 20000) {
      lastApCheck = millis();
      if (WiFi.channel() == 0 || !apOk) {
        Serial.println("[ap] hotspot mati, hidupkan ulang...");
        WiFi.mode(AP_ONLY ? WIFI_AP : WIFI_AP_STA);
        apOk = (strlen(AP_PASS) >= 8)
                 ? WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4)
                 : WiFi.softAP(AP_SSID);
        esp_wifi_set_max_tx_power((int8_t)WIFI_POWER_8_5dBm);
        esp_wifi_set_ps(WIFI_PS_NONE);
        Serial.printf("[ap] ulang softAP=%d ch%d\n", (int)apOk, WiFi.channel());
      }
    }
  }

  // auto-rotate hanya untuk 6 layar utama, tidak mengganggu menu/mood/makan
  if (AUTO_ROTATE_MS > 0 &&
      screen < SCR_MENU && !(navActive() && screen == SCR_GPS)) {
    static uint32_t lastRotate = 0;
    uint32_t hold = (screen == SCR_FACE) ? (uint32_t)AUTO_ROTATE_MS * 2
                                         : (uint32_t)AUTO_ROTATE_MS;
    if (millis() - lastRotate >= hold) {
      lastRotate = millis();
      screen = (Screen)(((int)screen + 1) % (SCR_BATT + 1));
    }
  }

  // menu / pilih mood ditutup otomatis setelah 12 detik tanpa input
  if ((screen == SCR_MENU || screen == SCR_MOOD) &&
      millis() - lastInput > 12000) {
    goFace();
  }

  // selesai makan otomatis jadi senang lagi
  if (screen == SCR_EAT && millis() - screenSince > 4200) {
    setMood(Mood::Happy);
    react = React::Love;
    reactUntil = millis() + 1200;
    goFace();
  }

  uint32_t interval = (screen == SCR_FACE || screen == SCR_EAT)
                        ? (uint32_t)FACE_FRAME_MS : 100;
  if (millis() - lastRender >= interval) {
    lastRender = millis();
    render();
  }

  delay(1);
}



