// Bridge WhatsApp -> mochi dasai, lewat KABEL USB (serial COM8).
// Memakai Baileys (protokol WA Web langsung, tanpa Chrome/Puppeteer).
//
// Cara pakai: npm start  lalu scan QR di WhatsApp > Perangkat tertaut.
// Format ke firmware: N<pengirim>|<pesan>\n

const fs = require('fs');
const pino = require('pino');
const QRCode = require('qrcode');
const { SerialPort } = require('serialport');
const {
  makeWASocket,
  useMultiFileAuthState,
  fetchLatestBaileysVersion,
  DisconnectReason,
} = require('@whiskeysockets/baileys');

const ESP_PORT      = process.env.ESP_PORT || 'COM8';
const BAUD          = 115200;
const IGNORE_GROUPS = process.env.IGNORE_GROUPS === '1';
const IGNORE_SELF   = process.env.IGNORE_SELF === '1';   // default: pesan sendiri diteruskan

const WX_LAT        = process.env.WX_LAT ? parseFloat(process.env.WX_LAT) : null;
const WX_LON        = process.env.WX_LON ? parseFloat(process.env.WX_LON) : null;
const DEFAULT_LAT   = -6.2000;
const DEFAULT_LON   = 106.8167;
const SYNC_INTERVAL = 10 * 60 * 1000;

// ---------------- status ----------------
function tulisStatus(teks) {
  try {
    fs.writeFileSync('./bridge_status.txt',
      `${new Date().toLocaleString('id-ID')} - ${teks}\n`);
  } catch (_) {}
}

// ---------------- serial ke ESP32 ----------------
let esp = null;

function bukaSerial() {
  if (esp && esp.isOpen) return;
  try {
    esp = new SerialPort({
      path: ESP_PORT,
      baudRate: BAUD,
      autoOpen: true,
      dtr: false,
      rts: false,
    });
    esp.on('open', () => {
      console.log(`[serial] terhubung ${ESP_PORT} @ ${BAUD}`);
      kirimJamCuaca();
    });
    esp.on('error', (e) => console.error(`[serial] ${e.message}`));
    esp.on('close', () => console.error('[serial] terputus'));
  } catch (e) {
    console.error('[serial] gagal buka:', e.message);
  }
}

bukaSerial();
setInterval(() => { if (!esp || !esp.isOpen) bukaSerial(); }, 3000);

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
    const temp  = Number(c.temperature_2m).toFixed(1);
    const rh    = Number(c.relative_humidity_2m || 0).toFixed(0);
    const feels = Number(c.apparent_temperature || c.temperature_2m).toFixed(1);
    const wind  = Number(c.wind_speed_10m || 0).toFixed(1);
    const code  = Number.isFinite(c.weather_code) ? c.weather_code : -1;
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
  const f = String(from).replace(/[\r\n|]/g, ' ').slice(0, 60);
  const t = String(text).replace(/[\r\n|]/g, ' ').slice(0, 120);
  esp.write(`N${f}|${t}\n`, (err) => {
    if (err) console.error('[serial] gagal kirim:', err.message);
    else console.log(`[wa] ${f}: ${t} -> ${ESP_PORT}`);
  });
}

function isiPesan(m) {
  const msg = m.message || {};
  return msg.conversation ||
    msg.extendedTextMessage?.text ||
    msg.imageMessage?.caption ||
    msg.videoMessage?.caption ||
    msg.buttonsMessage?.contentText ||
    msg.listMessage?.description ||
    (msg.protocolMessage ? '' : '');
}

async function prosesPesan(m) {
  const body = (isiPesan(m) || '').trim();
  const grup = /@g\.us$/.test(m.key.remoteJid || '');
  const dari = String(m.key.remoteJid || '').replace(/(@c\.us|@g\.us|@s\.whatsapp\.net)$/, '');
  console.log(`[wa] pesan masuk: ${dari} self=${!!m.key.fromMe} grup=${grup} "${body.slice(0, 60)}"`);

  // Baterai dari Shortcuts iPhone: "#batt 85 1". Diproses sebelum filter self.
  const batt = !grup && body.match(/^#batt(?:erai)?\s+(\d{1,3})\s*([01]?)$/i);
  if (batt) {
    const lvl = Math.max(0, Math.min(100, parseInt(batt[1], 10)));
    const chg = batt[2] === '1' ? 1 : 0;
    if (tulisEsp(`B${lvl}|${chg}\n`)) {
      console.log(`[baterai] WA -> ESP32: ${lvl}% ${chg ? '(mengisi)' : ''}`);
    }
    return;
  }

  if (IGNORE_GROUPS && grup) { console.log('[wa] diabaikan: pesan grup'); return; }
  if (IGNORE_SELF && m.key.fromMe) { console.log('[wa] diabaikan: pesan sendiri'); return; }

  const from = m.pushName || dari;
  const text = body.slice(0, 120) || '[media / stiker]';
  kirimKeEsp(from, text);
}

// ---------------- Baileys ----------------
async function mulai() {
  const { state, saveCreds } = await useMultiFileAuthState('./baileys_session');
  const { version } = await fetchLatestBaileysVersion();

  const sock = makeWASocket({
    version,
    auth: state,
    printQRInTerminal: false,
    browser: ['Mochi Dasai', 'Chrome', '1.0.0'],
    logger: pino({ level: 'silent' }),
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', async ({ connection, lastDisconnect, qr }) => {
    if (qr) {
      try { fs.writeFileSync('./qr.txt', qr); } catch (_) {}
      tulisStatus('PERLU QR - scan bridge/qr.txt di WhatsApp > Perangkat tertaut');
      console.log('\n[wa] Sesi perlu dihubungkan ulang. Scan QR di WhatsApp > Perangkat tertaut.');
      console.log('     QR juga tampil otomatis di halaman live bridge.\n');
      // QR juga disimpan sebagai PNG supaya gampang ditampilkan.
      try { await QRCode.toFile('./qr.png', qr, { width: 800, margin: 2 }); } catch (_) {}
    }

    if (connection === 'close') {
      const code = lastDisconnect?.error?.output?.statusCode;
      const loggedOut = code === DisconnectReason.loggedOut;
      console.log('[wa] terputus:', lastDisconnect?.error?.message || code);
      tulisStatus(loggedOut
        ? 'TERPUTUS - sesi logout, hapus baileys_session lalu jalankan ulang'
        : 'TERPUTUS - menyambung ulang...');
      if (!loggedOut) {
        setTimeout(mulai, 3000);
      }
    } else if (connection === 'open') {
      console.log(`[wa] siap. notifikasi dikirim lewat kabel ${ESP_PORT}`);
      tulisStatus(`SIAP - menunggu pesan WA, dikirim ke ${ESP_PORT}`);
    }
  });

  sock.ev.on('messages.upsert', async ({ messages, type }) => {
    if (type !== 'notify') return;
    for (const m of messages) {
      try { await prosesPesan(m); } catch (e) {
        console.error('[wa] gagal proses pesan:', e.message);
      }
    }
  });
}

tulisStatus('MENYIAPKAN - menghubungkan WhatsApp...');
mulai().catch((e) => {
  console.error('[wa] gagal start:', e);
  tulisStatus('GAGAL START - lihat terminal bridge');
});
