# Baterai iPhone di OLED mochi dasai

ESP32 punya layar **BATERAI HP** (layar ke-7, ikut auto-rotate) dan juga
menampilkan persentase kecil di status bar. Data bisa dikirim dari iPhone lewat
**Shortcuts/Pintasan**, atau manual lewat browser/kabel USB.

IP yang dipakai contoh di bawah adalah `192.168.4.1` (AP fallback
`mochi-dasai`, password `mochi1234`). Kalau ESP32 berhasil join WiFi `WiFI_RUMAH`,
ganti dengan IP LAN-nya (lihat layar **SISTEM** baris `ip ...` atau
`/api/status`).

## Opsi terbaik: bot Telegram (tanpa hotspot)

ESP32 sudah bisa tanya Telegram Bot API tiap 4 detik — tidak perlu PC, tidak
perlu kabel, dan iPhone boleh pakai sinyal seluler.

1. Buat bot lewat **@BotFather** → `/newbot`, lalu isi token di
   `include/config.h`:
   ```c
   #define TELEGRAM_TOKEN "123456789:AAH..."
   ```
   Upload ulang firmware. (Wajib: ESP32 harus tersambung WiFi `WiFI_RUMAH` agar
   punya internet.)
2. Di Telegram, buka bot kamu dan tekan **Start**.
3. Di Shortcuts, tambahkan aksi **Kirim Pesan** Telegram → penerima = bot kamu,
   pesan:
   ```
   #batt Bat Chg
   ```
   (`Bat` dan `Chg` adalah Magic Variable dari langkah sebelumnya.)
4. Pasang otomatisasi seperti di bawah — pesan masuk ke bot, ESP32 membacanya
   maksimal ~4 detik kemudian.

Perintah yang dikenal: `#batt 85 1` (85%, mengisi) atau `#batt 42 0`.
Pesan lain tetap tampil sebagai notifikasi `[TG]` biasa.

Alternatif tanpa token: kalau bridge WA jalan di PC, kirim pesan WA dengan isi
yang sama (`#batt 85 1`) ke nomor sendiri — bridge meneruskannya lewat kabel
COM8. Ini berguna kalau ESP32 belum punya internet.

## Cara cepat uji

Buka Safari di HP yang sudah tersambung ke hotspot `mochi-dasai`:

```
http://192.168.4.1/battery?level=85&charging=1
```

Layar OLED langsung berubah (atau tekan tombol sampai ke layar **baterai**).
`charging=1` berarti sedang mengisi, `0` tidak.

## Pasang Shortcut iPhone (otomatis)

Buat satu shortcut bernama **Kirim Baterai ke Mochi**:

| Langkah | Aksi (Indonesia / English) | Isi |
|---|---|---|
| 1 | **Ambil Level Baterai** / *Get Battery Level* | — |
| 2 | **Atur Variabel** / *Set Variable* | nama `Bat`, isi = hasil langkah 1 |
| 3 | **Ambil Status Pengisian** / *Get Charging State* | — |
| 4 | **Jika** / *If* | `Status Pengisian` **adalah** `Mengisi` / *Charging* |
| 5 | → di dalam **Jika**: Atur Variabel | `Chg` = `1` |
| 6 | → di **Selain itu** (*Otherwise*): Atur Variabel | `Chg` = `0` |
| 7a | **Ambil Isi URL** / *Get Contents of URL* (lembah WiFi) | Method `GET`, URL di bawah |
| 7b | atau **Kirim Pesan** Telegram ke bot (sinyal seluler) | `#batt Bat Chg` |

URL (pilih `Bat` dan `Chg` lewat **Magic Variable**, jangan diketik sebagai
teks biasa):

```
http://192.168.4.1/battery?level=Bat&charging=Chg
```

> Memakai **Ambil Isi URL** tidak membuka Safari, jadi shortcut bisa jalan di
> latar belakang.

## Supaya otomatis

Di tab **Otomatisasi** / *Automation*:

1. **+** → **Saat Charger Terhubung** (*When Charger is Connected*) →
   **Jalankan Segera** (*Run Immediately*) → pilih shortcut di atas.
2. Ulangi untuk **Saat Charger Terputus** (*When Charger is Disconnected*).
3. Tambah satu otomatisasi **Waktu Tertentu** (misal 08.00) atau
   **Saat Baterai Turun Di Bawah 30%** supaya datanya tetap segar.

Kalau iPhone tidak terhubung ke hotspot `mochi-dasai`/WiFi yang sama dengan
ESP32, pengiriman gagal — data terakhir tetap tersimpan di NVS dan ditandai
**tersimpan** di layar.

## Cara lain

- Dari halaman web `http://192.168.4.1/`, bagian **baterai iPhone** ada form
  isi level + status (berguna untuk tes tanpa iPhone).
- Lewat kabel serial COM8: `B85|1\n` (85%, mengisi) atau `B42|0\n`.
- Cek lewat API: `/api/status` → `battery_level`, `battery_charging`,
  `battery_age_s`.
