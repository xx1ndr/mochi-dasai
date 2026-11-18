# Baterai iPhone di OLED mochi dasai

ESP32 punya layar **BATERAI HP** (layar ke-7, ikut auto-rotate) dan juga
menampilkan persentase kecil di status bar. Data bisa dikirim dari iPhone lewat
**Shortcuts/Pintasan**, atau manual lewat browser/kabel USB.

IP yang dipakai contoh di bawah adalah `192.168.4.1` (AP fallback
`mochi-dasai`, password `mochi1234`). Kalau ESP32 berhasil join WiFi `WiFI_RUMAH`,
ganti dengan IP LAN-nya (lihat layar **SISTEM** baris `ip ...` atau
`/api/status`).

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
| 7 | **Ambil Isi URL** / *Get Contents of URL* | Method `GET`, URL di bawah |

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
