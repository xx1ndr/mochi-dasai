// Bridge WhatsApp -> mochi dasai, lewat KABEL USB (serial COM8).
//
// Cara pakai:
//   1) npm install
//   2) colokkan ESP32 ke PC (port COM8, bisa ditimpa: ESP_PORT=COM3 npm start)
//   3) npm start   lalu scan QR dari WhatsApp di HP (sekali saja)
//
// Format yang dikirim ke firmware:  N<pengirim>|<pesan>\n
// Firmware menerimanya di updateSerialInput() lalu memunculkan notifikasi WA.
// Tidak perlu WiFi / IP sama sekali untuk notifikasi.

const fs = require('fs');
const { Client, LocalAuth } = require('whatsapp-web.js');
const qrcode = require('qrcode-terminal');
const { SerialPort } = require('serialport');

const ESP_PORT     = process.env.ESP_PORT || 'COM8';
const BAUD         = 115200;
const IGNORE_GROUPS = process.env.IGNORE_GROUPS === '1';
// Default: pesan ke diri sendiri TETAP diteruskan (biar notifikasi mudah diuji).
// Set IGNORE_SELF=1 kalau memang mau mengabaikannya.
const IGNORE_SELF   = process.env.IGNORE_SELF === '1';

// Jam & cuaca juga dikirim lewat kabel (ESP32 tidak perlu WiFi WiFi rumah).
const WX_LAT        = process.env.WX_LAT ? parseFloat(process.env.WX_LAT) : null;
const WX_LON        = process.env.WX_LON ? parseFloat(process.env.WX_LON) : null;
const DEFAULT_LAT   = -6.2000;   // Jakarta (sama dengan HOME_LAT di firmware)
const DEFAULT_LON   = 106.8167;
const SYNC_INTERVAL = 10 * 60 * 1000;   // kirim ulang tiap 10 menit

// Pakai Chrome yang sudah terpasang di PC (Puppeteer tidak perlu download Chromium).
const CHROME_PATH = process.env.CHROME_PATH ||
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';

// Status bisa dicek tanpa terminal: bridge/bridge_status.txt
function tulisStatus(teks) {
  try {
    fs.writeFileSync('./bridge_status.txt',
      `${new Date().toLocaleString('id-ID')} - ${teks}\n`);
  } catch (_) {}
}

// ---------------- koneksi serial ke ESP32 ----------------
let esp = null;

function bukaSerial() {
  if (esp && esp.isOpen) return;
  try {
    esp = new SerialPort({
      path: ESP_PORT,
      baudRate: BAUD,
      autoOpen: true,
      dtr: false,
      rts: false,           // jangan toggle DTR/RTS (bisa reset ESP32)
    });
    esp.on('open', () => {
      console.log(`[serial] terhubung ${ESP_PORT} @ ${BAUD}`);
      kirimJamCuaca();                    // jam + cuaca langsung setelah kabel tersambung
    });
    esp.on('error', (e) => console.error(`[serial] ${e.message}`));
    esp.on('close', () => console.error('[serial] terputus'));
  } catch (e) {
    console.error('[serial] gagal buka:', e.message);
  }
}

bukaSerial();
// Buka ulang otomatis kalau kabel dicolok lagi / port sempat error.
setInterval(() => { if (!esp || !esp.isOpen) bukaSerial(); }, 3000);

// ---------------- jam & cuaca dari PC -> ESP32 lewat kabel ----------------
function tulisEsp(baris) {
  if (!esp || !esp.isOpen) return false;
  esp.write(baris, (err) => {
    if (err) console.error('[serial] gagal kirim:', err.message);
  });
  return true;
}

async function fetchJson(url, ms = 8000) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), ms);
  try {
    const res = await fetch(url, { signal: ac.signal });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
  } finally {
    clearTimeout(timer);
  }
}

let lastLat = null, lastLon = null;

async function lokasiCuaca() {
  if (WX_LAT !== null && WX_LON !== null &&
      Number.isFinite(WX_LAT) && Number.isFinite(WX_LON)) {
    return [WX_LAT, WX_LON];
  }
  if (lastLat !== null) return [lastLat, lastLon];
  try {
    const g = await fetchJson('https://ipwho.is/');
    if (g && g.success && Number.isFinite(g.latitude) && Number.isFinite(g.longitude)) {
      lastLat = g.latitude;
      lastLon = g.longitude;
      console.log(`[cuaca] lokasi IP: ${lastLat.toFixed(4)},${lastLon.toFixed(4)} (${g.city || '?'})`);
      return [lastLat, lastLon];
    }
  } catch (e) {
    console.error(`[cuaca] geolokasi gagal: ${e.message}`);
  }
  return [DEFAULT_LAT, DEFAULT_LON];
}

let syncing = false;

async function kirimJamCuaca() {
  if (syncing) return;
  syncing = true;
  try {
    if (!esp || !esp.isOpen) return;

    // Jam: PC sudah tersinkron NTP, kirim epoch UTC; firmware yang ubah ke WIB.
    tulisEsp(`T${Math.floor(Date.now() / 1000)}\n`);

    const [lat, lon] = await lokasiCuaca();
    const url = `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}` +
      `&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m`;
    const j = await fetchJson(url);
    const c = j && j.current;
    if (!c || !Number.isFinite(c.temperature_2m)) {
      console.error('[cuaca] respons Open-Meteo tidak valid');
      return;
    }

    tulisEsp(`G${lat.toFixed(4)}|${lon.toFixed(4)}\n`);
    const temp   = Number(c.temperature_2m).toFixed(1);
    const rh     = Number(c.relative_humidity_2m || 0).toFixed(0);
    const feels  = Number(c.apparent_temperature || c.temperature_2m).toFixed(1);
    const wind   = Number(c.wind_speed_10m || 0).toFixed(1);
    const code   = Number.isFinite(c.weather_code) ? c.weather_code : -1;
    tulisEsp(`W${temp}|${rh}|${feels}|${wind}|${code}\n`);
    console.log(`[cuaca] dikirim: ${temp}C RH${rh}% code=${code} (${lat.toFixed(2)},${lon.toFixed(2)})`);
  } catch (e) {
    console.error(`[cuaca] gagal ambil/kirim: ${e.message}`);
  } finally {
    syncing = false;
  }
}

setInterval(() => { if (esp && esp.isOpen) kirimJamCuaca(); }, SYNC_INTERVAL);

function kirimKeEsp(from, text) {
  if (!esp || !esp.isOpen) {
    console.error('[serial] port belum terbuka, pesan dilewatkan');
    return;
  }
  // Bersihkan karakter yang dipakai firmware sebagai pemisah / akhir baris.
  const f = String(from).replace(/[\r\n|]/g, ' ').slice(0, 60);
  const t = String(text).replace(/[\r\n|]/g, ' ').slice(0, 120);
  esp.write(`N${f}|${t}\n`, (err) => {
    if (err) console.error('[serial] gagal kirim:', err.message);
    else console.log(`[wa] ${f}: ${t} -> ${ESP_PORT}`);
  });
}

// ---------------- WhatsApp ----------------
const client = new Client({
  authStrategy: new LocalAuth({ dataPath: './session' }),
  authTimeoutMs: 120000,
  userAgent: false,
  puppeteer: {
    headless: true,
    executablePath: CHROME_PATH,
    args: [
      '--no-sandbox',
      '--disable-setuid-sandbox',
      '--disable-dev-shm-usage',
      '--disable-background-timer-throttling',
    ],
  },
});

console.log('[wa] menyiapkan Chrome & WhatsApp Web...');

client.on('qr', (qr) => {
  // Cadangan kalau karakter QR di terminal berantakan (encoding Windows):
  // isi file ini bisa di-scan lewat aplikasi QR scanner mana pun.
  try { fs.writeFileSync('./qr.txt', qr); } catch (_) {}
  tulisStatus('PERLU QR - scan bridge/qr.txt di WhatsApp > Perangkat tertaut');
  console.log('\n[wa] Sesi perlu dihubungkan ulang. Scan QR di WhatsApp > Perangkat tertaut.');
  console.log('     Kalau QR di terminal berantakan, scan isi file bridge/qr.txt.\n');
  qrcode.generate(qr, { small: true });
});

client.on('auth_failure', (msg) => {
  console.error('[wa] gagal login:', msg);
  tulisStatus('GAGAL LOGIN - hapus folder session lalu jalankan ulang');
});

client.on('authenticated', () => {
  console.log('[wa] tersimpan, menunggu siap...');
  tulisStatus('TERAUTENTIKASI - menunggu WhatsApp Web siap');
});

client.on('ready', () => {
  console.log(`[wa] siap. notifikasi dikirim lewat kabel ${ESP_PORT}`);
  tulisStatus(`SIAP - menunggu pesan WA, dikirim ke ${ESP_PORT}`);
});

client.on('disconnected', (reason) => {
  console.log('[wa] terputus:', reason);
  tulisStatus('TERPUTUS - jalankan start_bridge.bat lagi');
  process.exit(1);
});

client.on('message', async (msg) => {
  const body = (msg.body || '').trim();
  console.log(`[wa] pesan masuk: ${msg.from} self=${msg.fromMe} grup=${msg.isGroup} "${body.slice(0, 60)}"`);

  // Baterai dari Shortcuts iPhone: "#batt 85 1" (level, 1=mengisi).
  // Diproses sebelum filter IGNORE_SELF karena shortcut kirim ke diri sendiri.
  const batt = !msg.isGroup && body.match(/^#batt(?:erai)?\s+(\d{1,3})\s*([01]?)$/i);
  if (batt) {
    const lvl = Math.max(0, Math.min(100, parseInt(batt[1], 10)));
    const chg = batt[2] === '1' ? 1 : 0;
    if (tulisEsp(`B${lvl}|${chg}\n`)) {
      console.log(`[baterai] WA -> ESP32: ${lvl}% ${chg ? '(mengisi)' : ''}`);
      try { if (msg.react) await msg.react('🔋'); } catch (_) {}
    }
    return;
  }

  if (IGNORE_GROUPS && msg.isGroup) { console.log('[wa] diabaikan: pesan grup'); return; }
  if (IGNORE_SELF && msg.fromMe) { console.log('[wa] diabaikan: pesan sendiri'); return; }

  let from = msg.from.replace(/@c\.us$/, '').replace(/@g\.us$/, '');
  try {
    const contact = await msg.getContact();
    if (contact && (contact.name || contact.pushname)) {
      from = contact.name || contact.pushname;
    }
  } catch (_) {}

  const text = (msg.body || '').trim().slice(0, 120) || '[media / stiker]';
  kirimKeEsp(from, text);
});

client.initialize();
