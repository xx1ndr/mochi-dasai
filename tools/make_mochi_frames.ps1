# Konversi GIF animasi mochi (320x180, background hitam) jadi frame OLED 1-bit 128x64.
# Pakai: powershell -File tools\make_mochi_frames.ps1 -Gif D:\Download\mi_bef.gif
param(
  [string]$Gif = 'D:\Download\mi_bef.gif',
  [string]$Out = 'include\mochi_frames.h',
  [int]$CropX = 42, [int]$CropY = 40,
  [int]$CropW = 235, [int]$CropH = 113,   # karakter tanpa watermark kanan-bawah
  [int]$Threshold = 120   # jumlah R+G+B; background hitam -> 0
)

Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile($Gif)
$count = $img.GetFrameCount([System.Drawing.Imaging.FrameDimension]::Time)

# Baca delay dari Graphic Control Extension GIF (dalam 1/100 detik)
$raw = [System.IO.File]::ReadAllBytes($Gif)
$delays = @()
for ($i = 0; $i -lt $raw.Length - 7; $i++) {
  if ($raw[$i] -eq 0x21 -and $raw[$i+1] -eq 0xF9 -and $raw[$i+2] -eq 0x04) {
    $delays += ($raw[$i+4] + 256 * $raw[$i+5]) * 10
  }
}
if ($delays.Count -ne $count) { $delays = @(70) * $count }

$lines = @()
$lines += '// GENERATED oleh tools/make_mochi_frames.ps1 dari GIF animasi mochi.'
$lines += '// Frame 128x64, 1 bit/px, LSB = piksel paling kiri (format u8g2 drawXBMP).'
$lines += '// Jangan diedit manual.'
$lines += '#pragma once'
$lines += '#include <Arduino.h>'
$lines += ''
$lines += '#define MOCHI_FRAME_W     128'
$lines += '#define MOCHI_FRAME_H     64'
$lines += '#define MOCHI_FRAME_BYTES 1024'
$lines += "#define MOCHI_FRAME_COUNT $count"
$lines += ''
$lines += '// Disimpan sebagai string escape: GCC jauh lebih cepat parse daripada'
$lines += '// puluhan ribu literal 0xHH yang dipisah koma.'
$lines += 'static const char MOCHI_FRAMES[] PROGMEM ='

$allBytes = New-Object 'System.Collections.Generic.List[byte]'

for ($f = 0; $f -lt $count; $f++) {
  $img.SelectActiveFrame([System.Drawing.Imaging.FrameDimension]::Time, $f) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap($img)
  $row = New-Object 'System.Collections.Generic.List[byte]'
  for ($ty = 0; $ty -lt 64; $ty++) {
    for ($bx = 0; $bx -lt 16; $bx++) {
      $byte = 0
      for ($bit = 0; $bit -lt 8; $bit++) {
        $tx = $bx * 8 + $bit
        $sx = [int]($CropX + ($tx + 0.5) * $CropW / 128)
        $sy = [int]($CropY + ($ty + 0.5) * $CropH / 64)
        if ($sx -ge 320) { $sx = 319 }
        if ($sy -ge 180) { $sy = 179 }
        $c = $bmp.GetPixel($sx, $sy)
        # u8g2 drawXBMP = LSB dulu (bit 0 = piksel paling kiri).
        if (($c.R + $c.G + $c.B) -ge $Threshold) { $byte = $byte -bor (1 -shl $bit) }
      }
      $row.Add([byte]$byte)
      $allBytes.Add([byte]$byte)
    }
  }
  $bmp.Dispose()
}

# Tulis data sebagai potongan string "\xHH" (32 byte per baris).
for ($i = 0; $i -lt $allBytes.Count; $i += 32) {
  $end = [Math]::Min($i + 31, $allBytes.Count - 1)
  $esc = ($allBytes[$i..$end] | ForEach-Object { '\x{0:X2}' -f $_ }) -join ''
  $lines += "  `"$esc`""
}
$lines += '  ;'
$lines += ''
$lines += 'static const uint16_t MOCHI_DELAYS[MOCHI_FRAME_COUNT] PROGMEM = {'
$lines += '  ' + (($delays | ForEach-Object { "$_" }) -join ',') + '};'
$lines += ''
$lines += 'static inline const uint8_t* mochiFrame(uint16_t n) {'
$lines += '  if (n >= MOCHI_FRAME_COUNT) n = 0;'
$lines += '  return (const uint8_t*)MOCHI_FRAMES + (uint32_t)n * MOCHI_FRAME_BYTES;'
$lines += '}'
$lines += ''

[System.IO.File]::WriteAllLines((Join-Path (Get-Location) $Out), $lines)
$img.Dispose()
Write-Output "OK frames=$count bytes=$($allBytes.Count) -> $Out"

# Preview ASCII frame 10 (untuk cek bentuk tanpa melihat gambar)
$img2 = [System.Drawing.Image]::FromFile($Gif)
$img2.SelectActiveFrame([System.Drawing.Imaging.FrameDimension]::Time, 10) | Out-Null
$b2 = New-Object System.Drawing.Bitmap($img2)
Write-Output '--- preview frame 10 (downsample 64x32) ---'
for ($ty = 0; $ty -lt 32; $ty++) {
  $s = ''
  for ($tx = 0; $tx -lt 64; $tx++) {
    $sx = [int]($CropX + ($tx + 0.5) * $CropW / 64)
    $sy = [int]($CropY + ($ty + 0.5) * $CropH / 32)
    $c = $b2.GetPixel($sx, $sy)
    $s += $(if (($c.R + $c.G + $c.B) -ge $Threshold) { '#' } else { '.' })
  }
  Write-Output $s
}
$b2.Dispose(); $img2.Dispose()
