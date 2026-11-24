@echo off
cd /d "%~dp0"
title mochi WA bridge
echo ============================================
echo  mochi dasai - bridge WhatsApp ke ESP32
echo ============================================
echo.
echo Jangan tutup jendela ini selama bridge jalan.
echo Cek status di file: bridge_status.txt
echo Kalau statusnya PERLU QR, scan file qr.txt di
echo WhatsApp - Perangkat tertaut.
echo.
npm start
echo.
echo Bridge berhenti. Cek bridge_status.txt untuk alasannya.
pause
