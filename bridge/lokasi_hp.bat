@echo off
REM Lokasi HP butuh HTTPS supaya navigator.geolocation diizinkan browser.
REM cloudflared membuat URL https:// sementara yang diteruskan ke ESP32.
REM Buka URL yang muncul di HP, lalu izinkan akses lokasi.
set ESP_WEB=http://192.168.1.8
echo.
echo Membuka tunnel HTTPS ke %ESP_WEB% ...
echo URL https://... di bawah, buka di HP lalu izinkan lokasi.
echo Tekan Ctrl+C untuk berhenti.
echo.
"C:\Program Files (x86)\cloudflared\cloudflared.exe" tunnel --url %ESP_WEB%
pause
