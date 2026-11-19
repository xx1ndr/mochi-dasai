# Konversi GIF ekspresi jadi frame OLED 1-bit 128x64 untuk animasi mood + idle.
# Pakai: powershell -File tools\make_mood_frames.ps1
# Urutan set: Happy, Sad, Angry, Sleepy, Hungry (enum Mood), lalu Idle.
param(
  [string]$Dir   = 'D:\Download\example gift\example gift',
  [string]$Out   = 'include\mood_frames.h',
  [int]$CropX    = 0, [int]$CropY = 32,
  [int]$CropW    = 480, [int]$CropH = 240,   # rasio 2:1 = OLED 128x64
  [double]$TargetWhite = 0.34,               # proporsi piksel putih frame median
  [int]$MinThreshold   = 50,                 # jangan sampai background gelap ikut putih
  [int]$DitherAmp      = 45,                 # amplitudo dithering (skala B+G+R 0..765)
  [int]$DitherBand     = 15                  # dithering hanya di piksel yang dekat ambang
)

Add-Type -AssemblyName System.Drawing

# (nama file, keterangan) -- urutan HARUS sama dengan enum Mood, idle di akhir.
$gifs = @(
  'happy_2.gif',      # Senang
  'love.gif',         # Sedih
  'angry.gif',        # Marah
  'embarrassed.gif',  # Ngantuk
  'excited_2.gif',    # Lapar
  'happy.gif'         # Idle (wajah/default sesuai request user)
)

$allBytes  = New-Object 'System.Collections.Generic.List[byte]'
$allDelays = New-Object 'System.Collections.Generic.List[byte]'   # dalam 10 ms
$counts    = @()
$offsets   = @()

$small = New-Object System.Drawing.Bitmap(128, 64)
$g = [System.Drawing.Graphics]::FromImage($small)
# GIF ekspresi punya gradient/dithering. Bicubic merata-ratakan warna sebelum
# processing supaya garisnya bersih (nearest-neighbor bikin bintik besar).
$g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
$g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.CompositingMode    = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
$dest = New-Object System.Drawing.Rectangle(0, 0, 128, 64)
$src  = New-Object System.Drawing.Rectangle($CropX, $CropY, $CropW, $CropH)

# Matriks Bayer 4x4 untuk ordered dithering. Dipetakan ke [-A, +A].
$bayer = @(0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5)
$bayerOff = @()
foreach ($b in $bayer) { $bayerOff += (($b - 7.5) / 7.5) * $DitherAmp }

# Baca piksel 128x64 dari canvas ke array B,G,R,A (format Format32bppArgb).
function Read-Canvas($data) {
  $buf = New-Object byte[] ($data.Stride * 64)
  [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
  return ,$buf
}

# Gambar frame f ke canvas 128x64, kembalikan array luminansi (B+G+R, 0..765).
function Get-Luma($img, $f) {
  $img.SelectActiveFrame([System.Drawing.Imaging.FrameDimension]::Time, $f) | Out-Null
  $fr = New-Object System.Drawing.Bitmap($img)
  $g.Clear([System.Drawing.Color]::Black)
  $g.DrawImage($fr, $dest, $src, [System.Drawing.GraphicsUnit]::Pixel)
  $fr.Dispose()
  $data = $small.LockBits($dest, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $buf = Read-Canvas $data
  $stride = $data.Stride
  $small.UnlockBits($data)
  $sums = New-Object 'int[]' (128 * 64)
  $pi = 0
  for ($y = 0; $y -lt 64; $y++) {
    $row = $y * $stride
    for ($x = 0; $x -lt 128; $x++) {
      $p = $row + $x * 4
      $s = [int]$buf[$p] + [int]$buf[$p+1] + [int]$buf[$p+2]
      if ($s -gt 765) { $s = 765 }
      $sums[$pi] = $s
      $pi++
    }
  }
  return ,$sums
}

# Ambang untuk satu frame: cari nilai yang bikin ~TargetWhite piksel putih.
function Get-Threshold($sums) {
  $hist = New-Object 'int[]' 766
  foreach ($s in $sums) { $hist[$s]++ }
  $acc = 0
  $need = 128 * 64 * $TargetWhite
  for ($t = 765; $t -ge $MinThreshold; $t--) {
    $acc += $hist[$t]
    if ($acc -ge $need) { return $t }
  }
  return $MinThreshold
}

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

  # PILIH AMBANG DI "LEMBAH" HISTOGRAM -- ini kunci anti-berkedip.
  # Kalau ambang jatuh di puncak histogram (banyak piksel dengan luminansi
  # hampir sama), pergeseran 0,5 level antar frame bisa membalik ribuan piksel
  # sekaligus -> wajah kelihatan berubah-ubah tiap frame. Ambang di lembah
  # (histogram lokal minimum) membuat bentuk wajah stabil.
  $hist = New-Object 'int[]' 766
  $histTotal = 0
  $step = [Math]::Max(1, [int]($count / 25))
  for ($f = 0; $f -lt $count; $f += $step) {
    foreach ($s in (Get-Luma $img $f)) { $hist[$s]++; $histTotal++ }
  }
  $whiteMin = 0.16
  $whiteMax = $TargetWhite
  $bestDensity = [int]::MaxValue
  $thr = 200
  for ($t = 80; $t -le 700; $t++) {
    $white = 0
    for ($v = $t + 1; $v -le 765; $v++) { $white += $hist[$v] }
    $frac = $white / [double]$histTotal
    if ($frac -lt $whiteMin -or $frac -gt $whiteMax) { continue }
    $lo = [Math]::Max(0, $t - 15)
    $hi = [Math]::Min(765, $t + 15)
    $density = 0
    for ($v = $lo; $v -le $hi; $v++) { $density += $hist[$v] }
    if ($density -lt $bestDensity) { $bestDensity = $density; $thr = $t }
  }

  $offsets += $allBytes.Count
  $counts  += $count

  for ($f = 0; $f -lt $count; $f++) {
    $sums = Get-Luma $img $f
    $pi = 0
    for ($ty = 0; $ty -lt 64; $ty++) {
      for ($bx = 0; $bx -lt 16; $bx++) {
        $byte = 0
        for ($bit = 0; $bit -lt 8; $bit++) {
          $tx = $bx * 8 + $bit
          $lum = $sums[$pi]
          # Ordered dithering (Bayer 4x4) HANYA di piksel yang dekat ambang
          # (|<15|). Interior dan background yang jauh dari ambang tetap
          # solid/bersih, pinggiran jadi halus tanpa bintik di area gelap.
          # Band sempit penting: kalau lebar, background GIF gelap ikut
          # berbintik karena ambangnya rendah (mis. embarrassed=76).
          $t = $thr
          if ([Math]::Abs($lum - $thr) -le $DitherBand) {
            $t = $thr + [int]$bayerOff[(($ty % 4) * 4) + ($tx % 4)]
          }
          if ($lum -ge $t) { $byte = $byte -bor (1 -shl $bit) }
          $pi++
        }
        $allBytes.Add([byte]$byte)
      }
    }
    $allDelays.Add([byte]$delays[$f])
  }

  $img.Dispose()
  Write-Output "  $name : $count frame, delay $(($delays | Select-Object -Unique) -join '/') x10ms, threshold=$thr"
}

$g.Dispose(); $small.Dispose()

# ---- tulis header ----
$lines = @()
$lines += '// GENERATED oleh tools/make_mood_frames.ps1 dari 6 GIF ekspresi.'
$lines += '// Urutan set: Senang, Sedih, Marah, Ngantuk, Lapar (enum Mood), lalu Idle.'
$lines += '// Ambang stabil per GIF dipilih di LEMBAH histogram (anti-berkedip).'
$lines += '// + ordered dithering Bayer 4x4 di dekat ambang untuk pinggiran lebih halus.'
$lines += '// Frame 128x64, 1 bit/px, LSB-first (format u8g2 drawXBMP). Jangan diedit manual.'
$lines += '#pragma once'
$lines += '#include <Arduino.h>'
$lines += ''
$lines += '#define MOOD_GIF_COUNT    6'
$lines += '#define MOOD_IDLE_SET     5   // happy.gif - animasi wajah/default'
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
