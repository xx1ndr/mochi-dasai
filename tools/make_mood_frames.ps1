# Konversi 5 GIF ekspresi jadi frame OLED 1-bit 128x64 untuk animasi mood.
# Pakai: powershell -File tools\make_mood_frames.ps1
# Urutan set mengikuti enum Mood di main.cpp: Happy, Sad, Angry, Sleepy, Hungry.
param(
  [string]$Dir   = 'D:\Download\example gift\example gift',
  [string]$Out   = 'include\mood_frames.h',
  [int]$CropX    = 24, [int]$CropY = 24,
  [int]$CropW    = 432, [int]$CropH = 216,   # rasio 2:1 = OLED 128x64
  [int]$Threshold = 280   # GIF berwarna: 120 membuat hampir semua area jadi putih
)

Add-Type -AssemblyName System.Drawing

# (nama file, jumlah frame, keterangan) -- urutan HARUS sama dengan enum Mood.
$gifs = @(
  'happy_2.gif',      # Senang
  'love.gif',         # Sedih
  'angry.gif',        # Marah
  'embarrassed.gif',  # Ngantuk (request user)
  'excited_2.gif',    # Lapar
  'sleepy_3.gif'      # Idle (wajah/default, bukan mood)
)

$allBytes  = New-Object 'System.Collections.Generic.List[byte]'
$allDelays = New-Object 'System.Collections.Generic.List[byte]'   # dalam 10 ms
$counts    = @()
$offsets   = @()

$small = New-Object System.Drawing.Bitmap(128, 64)
$g = [System.Drawing.Graphics]::FromImage($small)
# GIF ekspresi punya gradient/dithering. Nearest-neighbor + threshold bikin
# bintik-bintik putih acak ("gambar pecah"). Bicubic merata-ratakan warna
# sebelum threshold supaya garisnya bersih di OLED 1-bit.
$g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.CompositingMode    = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
$dest = New-Object System.Drawing.Rectangle(0, 0, 128, 64)
$src  = New-Object System.Drawing.Rectangle($CropX, $CropY, $CropW, $CropH)

foreach ($name in $gifs) {
  $path = Join-Path $Dir $name
  if (-not (Test-Path -LiteralPath $path)) { throw "GIF tidak ditemukan: $path" }

  $img = [System.Drawing.Image]::FromFile($path)
  $count = $img.GetFrameCount([System.Drawing.Imaging.FrameDimension]::Time)

  # delay asli dari Graphic Control Extension, dalam 1/100 detik.
  $raw = [System.IO.File]::ReadAllBytes($path)
  $delays = @()
  for ($i = 0; $i -lt $raw.Length - 7; $i++) {
    if ($raw[$i] -eq 0x21 -and $raw[$i+1] -eq 0xF9 -and $raw[$i+2] -eq 0x04) {
      $cs = $raw[$i+4] + 256 * $raw[$i+5]     # centisecond: 7 = 70 ms
      $tens = $cs                              # simpan dalam unit 10 ms
      if ($tens -lt 1) { $tens = 1 }
      $delays += $tens
    }
  }
  if ($delays.Count -ne $count) { $delays = @(7) * $count }

  $offsets += $allBytes.Count
  $counts  += $count

  for ($f = 0; $f -lt $count; $f++) {
    $img.SelectActiveFrame([System.Drawing.Imaging.FrameDimension]::Time, $f) | Out-Null
    $frame = New-Object System.Drawing.Bitmap($img)
    $g.Clear([System.Drawing.Color]::Black)
    $g.DrawImage($frame, $dest, $src, [System.Drawing.GraphicsUnit]::Pixel)
    $frame.Dispose()

    $data = $small.LockBits($dest, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $buf = New-Object byte[] ($stride * 64)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $small.UnlockBits($data)

    for ($ty = 0; $ty -lt 64; $ty++) {
      $rowBase = $ty * $stride
      for ($bx = 0; $bx -lt 16; $bx++) {
        $byte = 0
        for ($bit = 0; $bit -lt 8; $bit++) {
          $tx = $bx * 8 + $bit
          $p = $rowBase + $tx * 4
          # Format32bppArgb di memory = B,G,R,A
          if (($buf[$p] + $buf[$p+1] + $buf[$p+2]) -ge $Threshold) {
            $byte = $byte -bor (1 -shl $bit)   # LSB-first untuk u8g2 drawXBMP
          }
        }
        $allBytes.Add([byte]$byte)
      }
    }
    $allDelays.Add([byte]$delays[$f])
  }

  $img.Dispose()
  Write-Output "  $name : $count frame, delay $(($delays | Select-Object -Unique) -join '/') x10ms"
}

$g.Dispose(); $small.Dispose()

# ---- tulis header ----
$lines = @()
$lines += '// GENERATED oleh tools/make_mood_frames.ps1 dari 6 GIF ekspresi.'
$lines += '// Urutan set: Senang, Sedih, Marah, Ngantuk, Lapar (enum Mood), lalu Idle.'
$lines += '// Frame 128x64, 1 bit/px, LSB-first (format u8g2 drawXBMP). Jangan diedit manual.'
$lines += '#pragma once'
$lines += '#include <Arduino.h>'
$lines += ''
$lines += '#define MOOD_GIF_COUNT    6'
$lines += '#define MOOD_IDLE_SET     5   // sleepy_3.gif - animasi wajah/default'
$lines += '#define MOOD_FRAME_W      128'
$lines += '#define MOOD_FRAME_H      64'
$lines += '#define MOOD_FRAME_BYTES  1024'
$lines += ''
$lines += 'static const uint16_t MOOD_FRAME_COUNTS[MOOD_GIF_COUNT] PROGMEM = { ' +
          (($counts | ForEach-Object { $_ }) -join ',') + ' };'
$lines += 'static const uint32_t MOOD_FRAME_OFFSETS[MOOD_GIF_COUNT] PROGMEM = { ' +
          (($offsets | ForEach-Object { $_ }) -join ',') + ' };'
$lines += ''
$lines += '// Delay tiap frame dalam 10 ms (60/70 ms sesuai GIF aslinya).'
$lines += 'static const uint8_t MOOD_DELAYS[] PROGMEM = {'
for ($i = 0; $i -lt $allDelays.Count; $i += 64) {
  $end = [Math]::Min($i + 63, $allDelays.Count - 1)
  $lines += '  ' + (($allDelays[$i..$end] | ForEach-Object { $_ }) -join ',') + ','
}
$lines += '};'
$lines += ''
$lines += '// Data frame: string escape jauh lebih cepat di-parse GCC daripada literal 0xHH.'
$lines += 'static const char MOOD_FRAMES[] PROGMEM ='
for ($i = 0; $i -lt $allBytes.Count; $i += 32) {
  $end = [Math]::Min($i + 31, $allBytes.Count - 1)
  $esc = ($allBytes[$i..$end] | ForEach-Object { '\x{0:X2}' -f $_ }) -join ''
  $lines += "  `"$esc`""
}
$lines += '  ;'
$lines += ''
$lines += 'static inline const uint8_t* moodFrame(uint8_t which, uint16_t n) {'
$lines += '  if (which >= MOOD_GIF_COUNT) which = 0;'
$lines += '  uint16_t cnt = pgm_read_word(&MOOD_FRAME_COUNTS[which]);'
$lines += '  if (n >= cnt) n = 0;'
$lines += '  uint32_t off = pgm_read_dword(&MOOD_FRAME_OFFSETS[which]);'
$lines += '  return (const uint8_t*)MOOD_FRAMES + off + (uint32_t)n * MOOD_FRAME_BYTES;'
$lines += '}'
$lines += ''

[System.IO.File]::WriteAllLines((Join-Path (Get-Location) $Out), $lines)
Write-Output "OK sets=$($gifs.Count) frames=$($counts.Count) bytes=$($allBytes.Count) -> $Out"
