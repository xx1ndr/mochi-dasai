// Bridge WhatsApp -> mochi dasai
//
// Cara pakai:
//   1) npm install
//   2) set ESP_IP (ip ESP32 atau mochi-01.local)
//   3) npm start   lalu scan QR dari WhatsApp di HP
//
// PERINGATAN: whatsapp-web.js adalah library tidak resmi.
// Nomor yang dipakai bisa kena ban oleh WhatsApp. Pakai nomor cadangan.
// Kalau tidak mau risiko, pakai MacroDroid (Android) -> HTTP request ke ESP32.

const fs = require('fs');
const { Client, LocalAuth } = require('whatsapp-web.js');
const qrcode = require('qrcode-terminal');

// IP LAN ESP32-C3 (dibaca dari Serial Monitor saat boot). Bisa ditimpa lewat
// env: ESP_IP=192.168.1.xx npm start
const ESP_IP        = process.env.ESP_IP || '192.168.1.8';
const IGNORE_GROUPS = process.env.IGNORE_GROUPS === '1';
const IGNORE_SELF   = process.env.IGNORE_SELF !== '0';

// Pakai Chrome yang sudah terpasang di PC (Puppeteer tidak perlu download Chromium).
// Ubah path ini kalau Chrome-mu di lokasi lain, atau hapus baris executablePath
// kalau puppeteer sudah berhasil mengunduh browser sendiri.
const CHROME_PATH = process.env.CHROME_PATH ||
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';

const client = new Client({
  authStrategy: new LocalAuth({ dataPath: './session' }),
  // WhatsApp Web kadang lama load di PC yang internetnya lambat.
  authTimeoutMs: 120000,
  // Pakai UA Chrome asli (bukan default library yang sudah tahun 2022).
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
  console.log('\nScan QR ini di WhatsApp > Perangkat tertaut:\n');
  qrcode.generate(qr, { small: true });
});

client.on('auth_failure', (msg) => {
  console.error('[wa] gagal login:', msg);
});

client.on('authenticated', () => {
  console.log('[wa] tersimpan, menunggu siap...');
});

client.on('ready', () => {
  console.log(`[wa] siap. mengirim notifikasi ke http://${ESP_IP}/notify`);
});

client.on('disconnected', (reason) => {
  console.log('[wa] terputus:', reason);
  process.exit(1);
});

client.on('message', async (msg) => {
  if (IGNORE_GROUPS && msg.isGroup) return;
  if (IGNORE_SELF && msg.fromMe) return;

  let from = msg.from.replace(/@c\.us$/, '').replace(/@g\.us$/, '');
  try {
    const contact = await msg.getContact();
    if (contact && (contact.name || contact.pushname)) {
      from = contact.name || contact.pushname;
    }
  } catch (_) {}

  const text = (msg.body || '').trim().slice(0, 120) || '[media / stiker]';

  try {
    const url = `http://${ESP_IP}/notify?from=${encodeURIComponent(from)}` +
                `&text=${encodeURIComponent(text)}`;
    const res = await fetch(url);
    console.log(`[wa] ${from}: ${text} -> ${res.status}`);
  } catch (err) {
    console.error('[wa] gagal kirim ke ESP32:', err.message);
  }
});

client.initialize();
