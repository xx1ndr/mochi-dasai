# mochi dasai

Gadget meja kecil: **mochi dasai interaktif** di ESP32-C3 Super Mini dengan OLED
0.96" (SSD1306 128x64, I2C). Bisa elus, marah, sedih, ngantuk, lapar, kasih
makan, plus jam, cuaca, GPS, dan notifikasi WhatsApp & Telegram.

```
+----------------------------+
| Min 15 Nov          MSG 2  |
|                            |
| 14:05        27 C          |
|              Cerah berawan |
|                            |
| wifi sat08 mq       MSG2   |
+----------------------------+
```

Sensor sentuh **TTP223** dipasang di GPIO3:
- **Sentuh pendek** di wajah = elus. Mocha senang memberi hati; kalau marah,
  elus 3 kali membuatnya sedih; sedih/ngantuk langsung senang; kalau lapar,
  sekali sentuh langsung diberi makan.
- **Tahan ±0,7 detik** (dari layar mana pun) = menu: Jam, Cuaca, **GPS/Peta**,
  Notif, Ubah mood, Kasih makan, Tutup. Di menu, sentuh = pindah item, tahan = pilih.
  Di pemilih mood ada pilihan **Batal (ke menu)** supaya selalu bisa kembali.
- Mood dan waktu terakhir makan **tersimpan di NVS**, tetap ingat setelah reboot.
  Setelah 1 jam tidak diberi makan, mochi jadi lapar (diatur `HUNGER_SEC`).

Semua kontrol juga bisa dari HP lewat halaman web — lihat bagian 4. Untuk tes
tanpa sensor, buka Serial Monitor dan ketik `s` (= sentuh pendek) atau `l`
(= tahan); reaksi mood/menu sama persis seperti sensor sentuh.

Layar utama: Wajah mochi -> Jam+cuaca -> GPS -> Cuaca -> Pesan (WA/TG) -> Sistem.
Layar tambahan: Menu, Ubah mood, dan Makan (muncul lewat sentuhan).

---

## 1. Yang perlu disiapkan

| Bagian | Keterangan |
|---|---|
| ESP32-C3 Super Mini | 4 MB flash, tanpa PSRAM. Cukup. |
| OLED 0.96" SSD1306 | 128x64, I2C, alamat 0x3C |
| GPS module | NEO-6M / ATGM336H / u-blox MAX-M10S (UART) |
| Sensor sentuh | TTP223 (OUT aktif HIGH) ke GPIO3 — layar tetap bisa dikontrol dari web |
| Kabel | 8-10 jumper female-female |

Catatan:
- Kode ini untuk OLED **128x64**. Kalau kamu punya yang 0.42" (72x40), bilang saja,
  layout-nya harus diubah.
- GPS butuh pandangan langit. Di dalam ruangan biasanya **tidak dapat fix**.

---

## 2. Wiring

| Modul | Pin modul | ESP32-C3 Super Mini |
|---|---|---|
| OLED | VDD / VCC | 3V3 |
| OLED | GND | GND |
| OLED | SDA | GPIO20 |
| OLED | SCK / SCL | GPIO21 |
| GPS | VCC | 5V (modul NEO-6M punya regulator) atau 3V3 |
| GPS | GND | GND |
| GPS | TX | GPIO4 (RX ESP32) |
| GPS | RX | GPIO5 (TX ESP32, opsional) |
| TTP223 | OUT | GPIO3 |
| TTP223 | VCC | 3V3 |
| TTP223 | GND | GND |

Kalau pin bentrok dengan kebutuhanmu, ubah di `include/config.h`.
Di ESP32-C3 pin I2C/UART bebas dipilih, jadi susunan ini cuma kesepakatan:
OLED di GPIO20/21, GPS di GPIO4/5.

> GPS NEO-6M menarik ~50 mA saat mencari satelit. Dari USB aman.
> Kalau pakai baterai LiPo 3.7V, jangan langsung ke pin 5V/3V3 — pakai boost/regulator.

---

## 3. Build & upload

Pilih salah satu: **Arduino IDE 2** (paling gampang kalau belum pernah pakai PlatformIO)
atau **PlatformIO**. Kode dan config-nya sama, cuma beda cara buka.

### A. Arduino IDE 2

**Sekali saja (setup):**

1. Install **Arduino IDE 2**.
2. `File > Preferences` -> **Additional boards manager URLs**, tempel:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
3. Buka **Boards Manager** (ikon board di sidebar kiri), cari `esp32`,
   install **esp32 by Espressif Systems** (versi 3.x). Tunggu selesai.
4. Buka **Library Manager** (ikon buku), install satu per satu:
   - `U8g2` (olikraus)
   - `TinyGPSPlus` (Mikal Hart)
   - `ArduinoJson` (Benoit Blanchon) — **harus v7**, kalau dapatnya v6 kode error
   - `PubSubClient` (Nick O'Leary)
5. Set di menu **Tools**:
   - Board: **ESP32C3 Dev Module**
   - Port: COMx yang muncul setelah board dicolok
   - **USB CDC On Boot: Enabled** (wajib, kalau tidak Serial kosong)
   - Flash Mode: **DIO**
   - Flash Size: **4 MB**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
   - Upload Speed: **921600** (kalau gagal, turunkan ke 115200)

**Tiap kali mau upload:**

1. Buka sketch: `File > Open` -> `C:\mochi-dasai\arduino\mochi_dasai\mochi_dasai.ino`
2. Edit `C:\mochi-dasai\include\config.h` (buka pakai Notepad / VS Code):
   - `AP_SSID`, `AP_PASS` (hotspot dari ESP32), `TZ_STRING`, `TELEGRAM_TOKEN`
   - **WiFi router:** jangan ditaruh di `config.h` (repo ini public). Buat file
     `C:\mochi-dasai\include\config_local.h` (sudah diabaikan git) berisi:
     ```c
     #pragma once
     #define WIFI_SSID "nama_wifi_kamu"
     #define WIFI_PASS "password_wifi_kamu"
     ```
3. Klik **Upload** (ikon →).
4. Kalau muncul `Failed to connect` / port hilang: tahan **BOOT**, tekan **RST**,
   lepas BOOT, lalu Upload lagi.

> File `.ino` itu cuma pintu masuk; isi aslinya di `src/main.cpp`.
> Jangan pindahkan `config.h` — path-nya sudah diatur.

### B. PlatformIO (VS Code)

1. Install VS Code + extension **PlatformIO IDE**.
2. `File > Open Folder` -> `C:\mochi-dasai`.
3. Edit `include/config.h`.
4. Klik **Upload** di status bar bawah.
5. Buka Serial Monitor 115200. IP akan muncul di situ dan di layar **SISTEM**.

---

## 4. Cara pakai

- Layar pertama = **animasi idle `happy.gif`** (75 frame 128x64 1-bit,
  60–70 ms per frame, ≈15 fps). Saat mood dipilih, animasi berganti sesuai mood
  (Ngantuk = `embarrassed.gif`). Mood tetap tersimpan dan tampil sebagai label
  kecil di sudut kiri-bawah. GIF idle bisa dilihat di
  `http://192.168.1.8/mochi.gif`.
- **ESP32 membuat hotspot sendiri** — tidak perlu router:

  | | |
  |---|---|
  | Nama WiFi | `mochi-dasai` |
  | Password | `mochi1234` |
  | Alamat web | `http://192.168.4.1` |

  Di HP: **Setelan → WiFi → mochi-dasai** → buka browser → `192.168.4.1`.
  (iPhone akan bilang "Tidak ada internet" — pilih **Tetap Sambung**, itu normal.)
- Jam otomatis ikut **jam HP** begitu halaman web dibuka (dikirim lewat
  `/time?epoch=...`), jadi tetap akurat walau tanpa internet. Zona waktu memakai
  `configTzTime(TZ_STRING, ...)`, bukan `configTime(0,0,...)` — yang terakhir
  diam-diam mengubah TZ jadi UTC sehingga jam tampil 7 jam lebih awal (dulu
  terjadi setelah tombol **Refresh** / sinkron ulang tiap jam).
- **Cuaca & navigasi mengikuti lokasi.** Urutan prioritas: lokasi HP (GPS HP lewat
  web) → GPS modul (kalau ada fix) → lokasi dari PC lewat kabel serial →
  geolokasi IP → `HOME_LAT`/`HOME_LON` di `config.h`. Bisa juga isi koordinat
  manual di halaman web (**lokasi HP / GPS** → *Simpan lokasi*). Lokasi ini
  tersimpan di NVS dan dipakai juga oleh peta di `/maps`. Cek lewat
  `/location?reset=1` kalau mau kembali ke koordinat default.

  > **GPS HP butuh HTTPS.** Browser hanya mengizinkan `navigator.geolocation`
  > di origin aman (`https://` atau `localhost`). Kalau halaman dibuka lewat
  > `http://192.168.x.x`, tombol *Mulai lokasi HP* otomatis jatuh ke lokasi IP.
  > Jalankan `bridge\lokasi_hp.bat` (cloudflared sudah terpasang), lalu buka URL
  > `https://...` yang muncul di HP dan izinkan akses lokasi. Setelah itu
  > `watchPosition()` mengirim koordinat ke `/location?src=hp` setiap kali
  > bergeser ≥5 m (maksimal 1 kali per 5 detik). Pengiriman ini tidak memicu
  > fetch cuaca: cuaca hanya diambil ulang kalau posisi berubah jauh, paling
  > cepat tiap 5 menit — jadi web, OLED, dan navigasi tetap responsif.

  > Tanpa internet, cuaca & Telegram tidak jalan. `AP_ONLY` sudah `0`, jadi ESP32
  > jadi hotspot **dan** ikut WiFi rumah sekaligus. Isi `WIFI_SSID`/`WIFI_PASS`
  > di `include/config_local.h` dengan WiFi 2.4 GHz yang dipakai PC bridge.
- `http://192.168.4.1/api/status` -> JSON status (layar aktif, `time`, `tz`,
  `wx_lat`/`wx_lon`, gps, cuaca, heap, dll) — cocok untuk cek jam & lokasi.
- Bisa juga `http://mochi-01.local/` kalau mDNS jalan di jaringanmu.

### Kontrol sensor sentuh & konfigurasi

Sensor TTP223 sudah diaktifkan di `include/config.h`:

| Define | Nilai | Arti |
|---|---|---|
| `HAS_BUTTON` | `1` | sensor/button aktif di GPIO3 |
| `BTN_ACTIVE` | `HIGH` | TTP223 aktif HIGH (tombol ke GND → ubah ke `LOW`) |
| `AUTO_ROTATE_MS` | `0` | `0` = mati (disarankan supaya tidak bertabrakan dengan menu) |
| `HUNGER_SEC` | `3600` | detik sampai mochi lapar setelah terakhir makan |
| `AP_ONLY` | `0` | hotspot + STA, supaya bridge WA di PC bisa mengirim notif lewat IP LAN |

Kalau sensor belum terpasang, layar tetap bisa dikontrol dari HP:

| Endpoint | Fungsi |
|---|---|
| `/next` | pindah ke layar berikutnya |
| `/refresh` | ambil ulang cuaca + sinkron jam (NTP, zona waktu tetap WIB) |
| `/location?lat=-6.2&lon=106.8` | set lokasi manual (tersimpan di NVS) |
| `/location?lat=-6.2&lon=106.8&src=hp` | set lokasi dari GPS HP (dilaporkan `wx_src: hp`) |
| `/location?reset=1` | kembali ke `HOME_LAT`/`HOME_LON` |
| `/clear` | hapus semua notifikasi di inbox |
| `/notify?from=..&text=..` | kirim notifikasi |
| `/goto?lat=&lon=` | mulai navigasi otomatis (turn-by-turn, OSRM) |
| `/find?q=Monas Jakarta` | cari nama tempat jadi koordinat (Nominatim) |
| `/goto?stop=1` | hentikan navigasi otomatis |
| `/nav?kind=left&dist=200&road=..` | kirim perintah belokan manual ke layar GPS |
| `/face?frame=45` | diagnosa: diamkan animasi DASAI di satu frame |
| `/face?auto=1` | lanjutkan animasi wajah normal |
| `/maps` (atau `/map`) | monitor GPS: peta Google Maps live + marker, auto-refresh 3 detik |
| `/mochi.gif` | animasi idle `happy.gif` (versi kecil 128x81) |
| `/mood?set=marah` | ubah mood (`senang`, `sedih`, `marah`, `ngantuk`, `lapar`); `idle` kembali ke animasi `happy.gif` tanpa badge |
| `/feed` | kasih makan (langsung tampil animasi "Nyam") |
| `/battery?level=85&charging=1` | set baterai iPhone (0–100, `charging` 0/1) |
| `/telegram?token=...` | simpan token bot Telegram di NVS (`?clear=1` = hapus) |

Cukup buka `http://192.168.4.1/` di Safari/Chrome HP, tombolnya sudah ada di sana
(termasuk tombol mood dan Kasih makan). Notifikasi baru tetap langsung muncul di
layar Pesan + badge `MSG n`.

Tes cepat dari terminal:

```bash
curl "http://192.168.1.8/notify?from=Budi&text=Halo%20dari%20curl"
curl -X POST -H "Content-Type: application/json" ^
     -d "{\"from\":\"Budi\",\"text\":\"tes json\"}" http://192.168.1.8/notify
```

(`^` itu line continuation di Windows CMD; di PowerShell pakai backtick.)

### Baterai iPhone

Layar **BATERAI HP** (dan persentase kecil di status bar) menampilkan daya
iPhone yang dikirim lewat Shortcuts. Cara pasang otomatisasi ada di
[`docs/IPHONE_BATTERY.md`](docs/IPHONE_BATTERY.md). Tes cepat:

```
http://192.168.4.1/battery?level=85&charging=1
```

Data terakhir disimpan di NVS, jadi tetap tampil setelah reboot (ditandai
**tersimpan** sampai ada kiriman baru). Bisa juga dikirim lewat kabel:
`B85|1\n`.

---

## 5. Notifikasi WhatsApp

ESP32 **tidak bisa** membaca WhatsApp sendiri. Harus ada yang meneruskan.
Pilih satu sesuai perangkatmu.

### Opsi A — iPhone: Shortcut "Kirim ke Mochi" (manual, aman)

iOS tidak mengizinkan aplikasi lain membaca notifikasi WhatsApp, jadi ini
berbasis **share sheet**: kamu teruskan pesan yang mau ditampilkan.

1. Buka app **Shortcuts** -> tab **Shortcuts** -> **+**.
2. Tambah action **Get Contents of URL**:
   - URL: `http://192.168.1.8/notify`
   - Tap **Show More** -> Method: **POST**
   - Request Body: **JSON**, tambah dua field:
     - `text` (Text) = **Shortcut Input**
     - `from` (Text) = `WhatsApp`
3. Tap ikon **ⓘ** di shortcut -> nyalakan **Show in Share Sheet**,
   Share Sheet Types: **Text**. (Tanpa ini shortcut tidak muncul di menu Share.)
4. Beri nama shortcut: **Kirim ke Mochi**.
5. Di WhatsApp, tekan lama pesan -> **Share** -> pilih **Kirim ke Mochi**.

Catatan:
- iPhone dan ESP32 harus di WiFi yang sama.
- Kalau IP ESP32 berubah-ubah, pakai `http://mochi-01.local/notify` atau
  pasang IP statis di router.
- Untuk notifikasi Telegram tidak perlu ini — bot langsung masuk otomatis
  (bagian 6).

### Opsi B — MacroDroid di Android (otomatis, tanpa risiko ban)

1. Install **MacroDroid**, buat macro baru.
2. Trigger: **Notification Received** -> app **WhatsApp**.
3. Action: **HTTP Request**
   - Method: `GET`
   - URL: `http://192.168.1.8/notify?from=[notification_title]&text=[notification_text]`
4. Simpan. Setiap notifikasi WhatsApp masuk -> muncul di mochi.

Tidak butuh PC menyala, tidak melanggar ToS.

### Opsi C — Bridge Node.js (`bridge/`)

Pakai `whatsapp-web.js` (tidak resmi, **nomor bisa kena ban** — pakai nomor cadangan).

```bash
cd bridge
set PUPPETEER_SKIP_DOWNLOAD=1 && npm install   # Windows CMD
# PowerShell: $env:PUPPETEER_SKIP_DOWNLOAD=1; npm install
npm start
```

`bridge.js` memakai Chrome yang sudah terpasang di
`C:\Program Files\Google\Chrome\Application\chrome.exe` (Lewati unduhan Chromium
Puppeteer yang sering gagal). Kalau Chrome di path lain, set env `CHROME_PATH`.

**Notifikasi, jam, dan cuaca dikirim lewat KABEL USB (serial), bukan WiFi.**
`bridge.js` membuka port `COM8` (115200 baud) dan menulis:

- `N<pengirim>|<pesan>\n` untuk tiap pesan WhatsApp masuk,
- `T<epoch>\n` jam dari PC (firmware yang konversi ke WIB),
- `G<lat>|<lon>\n` lokasi cuaca,
- `W<temp>|<rh>|<feels>|<wind>|<code>\n` cuaca dari Open-Meteo,
- `M<0..4>\n` ganti mood lewat kabel (0 Senang, 1 Sedih, 2 Marah, 3 Ngantuk,
  4 Lapar) — berguna untuk uji animasi tanpa WiFi/HP,
- `B<level>|<0/1>\n` baterai iPhone (%) + status mengisi (juga bisa dari web).

Notifikasi WA/Telegram/MQTT langsung membuka layar **NOTIF** di OLED (kecuali
sedang makan atau memilih mood), jadi pesan tidak perlu dicari dengan tombol.

Pesan ke nomor sendiri ikut diteruskan secara default, supaya mudah diuji. Set
`IGNORE_SELF=1` kalau memang ingin diabaikan.

Jam & cuaca dikirim begitu kabel tersambung lalu diulang tiap 10 menit. Jadi
ESP32 tidak perlu join WiFi `WiFi rumah` supaya jam/cuaca/notif WA jalan. GPS tetap
membaca modulnya sendiri (butuh fix satelit, taruh di luar ruangan); kalau tidak
ada fix, layar GPS menampilkan lokasi yang dikirim PC lewat kabel. Kalau port
bedanya:

```bash
set ESP_PORT=COM3 && npm start   # Windows CMD
$env:ESP_PORT="COM3"; npm start  # PowerShell
```

> Port COM8 harus bebas saat bridge jalan. Kalau mau upload firmware, matikan
> bridge dulu (`Ctrl+C`), karena Arduino CLI butuh port yang sama.

Scan QR yang muncul di terminal (WhatsApp > Perangkat tertaut). Kalau karakter
QR berantakan di terminal Windows, buka file `bridge/qr.txt` dan scan isinya
dengan aplikasi QR scanner. Sesi login disimpan di folder `bridge/session`,
jadi tidak perlu scan QR tiap kali.

> **Bridge nyangkut / timeout di `Client.inject`?**
> Profil sesi lama bisa bikin WhatsApp Web redirect ke halaman `/login/` yang
> tidak dikenali library. Hapus folder `bridge/session` lalu jalankan ulang
> `npm start` (QR baru akan muncul). Ini aman kalau belum pernah berhasil login.

### Opsi D — MQTT (kalau sudah punya broker)

Set `MQTT_HOST` di `config.h`. ESP32 subscribe ke:

```
mochi/<DEVICE_ID>/notify
```

Payload boleh JSON `{"from":"Budi","text":"halo"}` atau teks biasa.
Cocok kalau nanti mau disambung ke WhatsApp Cloud API (resmi) lewat webhook.

---

## 6. Telegram (paling gampang, resmi)

Tidak butuh PC menyala dan tidak butuh bridge — ESP32 langsung tanya
Telegram Bot API.

1. Chat **@BotFather** di Telegram -> `/newbot` -> ikuti langkahnya.
2. Masukkan token lewat halaman web ESP32 `http://192.168.4.1/`, bagian
   **bot Telegram** → kolom token → **Simpan token**. Tersimpan di NVS, jadi
   tidak perlu edit `config.h` atau upload ulang. Bisa juga lewat kabel:
   `K<token>\n`. Token tidak pernah dikirim balik ke browser/log.
3. Buka Telegram, cari bot kamu, tekan **Start**, lalu kirim pesan. Pesan akan
   muncul di layar **PESAN** dengan tag `[TG]`.

> ESP32 harus tersambung WiFi yang bisa internet (`WiFi rumah`) agar `getUpdates`
> jalan. Tanpa itu bot tetap tersimpan tapi tidak menerima pesan.

Catatan:
- Bot hanya menerima chat pribadi yang mengirim pesan ke dia. Untuk grup,
  tambahkan bot ke grup (matikan privacy mode lewat `/setprivacy` di BotFather
  kalau ingin bot membaca semua pesan grup).
- Saat boot, tumpukan update lama (maksimal 24 jam dari Telegram) dibuang,
  jadi inbox mulai bersih. Pesan yang datang setelah itu yang ditampilkan.
- ESP32 tanya server tiap 4 detik (`getUpdates`, koneksi TLS dipakai ulang),
  jadi tidak ada port forwarding / webhook yang perlu disetel.
- Pesan lebih dari ~70 karakter dipotong di layar.
- Kirim `#batt 85 1` ke bot untuk mengisi baterai iPhone di OLED
  (`1` = sedang mengisi). Ini jalur paling enak untuk sync otomatis dari
  Shortcuts tanpa hotspot — lihat `docs/IPHONE_BATTERY.md`. Pesan lain tetap
  jadi notifikasi `[TG]` biasa.

---

## 7. Kalau ada masalah

| Gejala | Cek |
|---|---|
| OLED blank | alamat I2C (0x3C/0x3D), SDA/SCL tertukar, pin di `config.h` |
| OLED bergeser/noise | layar pakai driver SH1106 -> set `OLED_SH1106 1` |
| OLED garis-garis acak | cek `/face?frame=45`: kalau tetap rusak, masalah kabel/I2C (turunkan `Wire.setClock` ke `100000`) |
| Hotspot muncul sebentar lalu hilang | daya USB kurang: pakai kabel pendek/adaptor 5V 1A; firmware sudah memakai TX 8.5 dBm + watchdog AP |
| Board tidak terdeteksi | kabel USB data (bukan charge-only), tahan BOOT saat upload |
| GPS `NO FIX` | antena menghadap langit, di luar ruangan, TX GPS -> GPIO4, baud 9600 |
| GPS dapat data tapi koordinat 0 | tunggu 1-3 menit pertama (cold start) |
| Jam `--:--` | buka `http://192.168.4.1` di HP sekali (jam dikirim dari browser) |
| Cuaca kosong | butuh internet; set `AP_ONLY 0` + isi WiFi router |
| Telegram tidak masuk | token salah/kedaluwarsa, bot belum di-`/start`, cek Serial `[tg]` |
| ESP restart sendiri | catu daya lemah atau brownout; pakai kabel USB pendek |
| Layar patah-patah | normal; refresh 10 fps, fetching cuaca sesekali memblokir ~1-2 s |
| Hotspot muncul lalu hilang | kabel USB/catu daya lemah; firmware membatasi tx 8,5 dBm dan menyalakan ulang AP otomatis. Cek log `[ap]` di Serial Monitor |
| Wajah jadi garis-garis | buka `http://192.168.4.1/face?frame=45` — kalau tetap garis, cek kabel SDA/SCL; kalau normal, masalahnya di transisi frame |

---

## 8. Batasan & ide lanjutan

- C3 single core, tanpa PSRAM: animasi wajah ±15 fps (delay 60/70 ms dari GIF), layar lain 10 fps supaya bus I2C tidak penuh.
- Cuaca: Open-Meteo (gratis, tanpa API key), update tiap 10 menit.
- GPS: kecepatan, satelit, HDOP, altitude, jarak tempuh (trip).
- Ide: tampilkan pesan penuh dengan scroll, grafik suhu, jam alarm, casing 3D print,
  tombol fisik supaya tidak perlu buka HP untuk pindah layar.

### Animasi wajah (idle + mood)

Frame ada di `include/mood_frames.h` (6 set × 75 frame, 128x64 1-bit, PROGMEM):

| Kondisi | GIF | Set |
|---|---|---|
| Idle (belum ada mood dipilih) | `happy.gif` | 5 |
| Senang | `happy_2.gif` | 0 |
| Sedih | `love.gif` | 1 |
| Marah | `angry.gif` | 2 |
| Ngantuk | `embarrassed.gif` | 3 |
| Lapar | `excited_2.gif` | 4 |

Diputar looping di `drawFace()` (`src/main.cpp`) dengan delay asli 60/70 ms per
frame. Splash restart dan diagnosa `/face?frame=N` memakai set idle. Generator
memilih **satu ambang stabil per GIF di "lembah" histogram** (bukan di puncak),
plus **ordered dithering Bayer 4x4 hanya di piksel dekat ambang** (±15).

Ambang di lembah penting: kalau ambang jatuh di puncak histogram, pergeseran
kecerahan 0,5 level antar frame bisa membalik ribuan piksel sekaligus sehingga
wajah kelihatan berubah-ubah/berkedip. Dengan ambang di lembah, bentuk wajah
stabil; dithering melunakkan pinggirannya di OLED 1-bit. Catatan: OLED 128x64
cuma bisa hitam/putih, jadi gradasi GIF tetap tampil sebagai pola titik halus.

File animasi lama (`dasai_frames.h`, `mochi_gif.h`, `mochi_frames.h`) sudah
dihapus. `include/mochi_web_gif.h` berisi versi kecil `happy.gif` untuk
endpoint `/mochi.gif`.

| Mau ubah | Di mana |
|---|---|
| ganti GIF mood/idle | edit `$gifs` di `tools/make_mood_frames.ps1`, lalu jalankan `powershell -File tools\make_mood_frames.ps1` |
| crop / target kecerahan | `-CropX -CropY -CropW -CropH -TargetWhite -MinThreshold` di script tersebut |
| kehalusan pinggiran | `-DitherAmp` / `-DitherBand` di script tersebut (0 = matikan dithering) |
| kecepatan animasi | `MOOD_DELAYS` di `include/mood_frames.h` (unit 10 ms) |
| label mood kecil | `drawMoodBadge()` di `src/main.cpp` |

### Navigasi otomatis (turn-by-turn)

Cukup masukkan tujuan, ESP32 yang menentukan belokannya:

1. Modul GPS **tidak wajib**. Posisi diambil berurutan dari: lokasi HP yang
   dikirim halaman web (`/location?src=hp`) → modul GPS → geolokasi IP → titik
   default `HOME_LAT/HOME_LON`. Buka halaman web di HP lewat tunnel HTTPS
   (`lokasi_hp.bat`) dan biarkan **Ikuti lokasi HP otomatis** aktif; posisi HP
   dikirim otomatis lewat `watchPosition()` setiap bergeser ≥5 m (maks. 1× per
   5 detik).
2. Buka halaman web → **navigasi otomatis**.
3. Ketik nama tempat di **Cari tempat** (misal `Monas Jakarta`) → klik **Cari**;
   koordinat tujuan otomatis terisi. Boleh juga isi `latitude`/`longitude`
   langsung.
4. Klik **Mulai navigasi**. OLED langsung menampilkan panah belokan pertama.

Selanjutnya **otomatis**: saat kamu bergerak, jarak ke belokan dihitung mundur
dari posisi terbaik yang tersedia, dan panah berganti ke belokan berikutnya.
Sesampainya di tujuan muncul **TIBA DI TUJUAN**. Tombol **Hentikan navigasi**
mengakhiri rute.

Rute diambil dari **OSRM** (`router.project-osrm.org`) dan geocoding dari
**Nominatim** — keduanya gratis tanpa API key. Kalau server rute tidak bisa
diajak bicara (WiFi belum join `WiFi rumah`, internet mati, atau OSRM sibuk), firmware
otomatis memakai **panah kompas langsung ke tujuan** (8 arah + jarak + derajat),
jadi navigasi tetap bisa dipakai. Tombol **Panah kompas (tanpa rute/GPS)**
memaksa mode ini. Akurasi geocoding IP setingkat kota.

Kalau pencarian gagal, pesan di halaman web sekarang menyebut alasan aslinya
(`WiFi mati`, `Server sibuk`, atau `Tempat tidak ditemukan`). Query pendek
sering miss; tambahkan nama kota, misal `Monas` → `Monas Jakarta`. ESP32 juga
mencoba ulang otomatis dengan `, Indonesia` kalau hasil pertama kosong.
Nominatim membatasi 1 request/detik, jadi jeda sekitar 1 detik antar pencarian.

Endpoint:

```
/goto?lat=-6.1754&lon=106.8272     mulai rute otomatis (OSRM)
/goto?nogeo=1&lat=-6.1754&lon=106.8272  panah kompas tanpa OSRM / tanpa GPS
/find?q=Monas%20Jakarta            cari tempat -> lat/lon
/goto?stop=1                       hentikan rute
```

Status rute ada di `/api/status`: `route_active`, `route_total`, `nav`, `nav_dist`.

### Rute manual (cadangan)

Tanpa rute otomatis, panah bisa dikirim satu per satu dari HP:

```
http://192.168.4.1/nav?kind=left&dist=200&road=Jl.%20Sudirman
```

> Alamat: `192.168.4.1` saat ESP32 masuk AP fallback `mochi-dasai`
> (password `mochi1234`) — HP/PC harus tersambung ke hotspot ini. Kalau ESP32
> berhasil join WiFi `WiFi rumah`, pakai IP LAN yang tampil di layar sistem
> (`ip ...`) atau di `/api/status`, misalnya `http://192.168.1.8/`.

| Parameter | Isi |
|---|---|
| `kind` | `straight` `left` `right` `slightleft` `slightright` `uturn` `roundabout` |
| `dist` | jarak ke belokan dalam meter (`50`, `200`, atau `1200` → otomatis tampil km) |
| `road` | nama jalan, maksimal ~24 karakter |

Selama rute aktif (30 menit sejak kiriman terakhir) layar tidak ikut pindah
otomatis, jadi panah belokan tetap kelihatan. Mode kompas juga memperbarui
`nav.at` tiap detik supaya tidak kedaluwarsa selama posisi masih dikirim.
