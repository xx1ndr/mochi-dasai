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
const IGNORE_SELF   = process.env.IGNORE_SELF !== '0';

// Pakai Chrome yang sudah terpasang di PC (Puppeteer tidak perlu download Chromium).
const CHROME_PATH = process.env.CHROME_PATH ||
  'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';

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
    esp.on('open', () => console.log(`[serial] terhubung ${ESP_PORT} @ ${BAUD}`));
    esp.on('error', (e) => console.error(`[serial] ${e.message}`));
    esp.on('close', () => console.error('[serial] terputus'));
  } catch (e) {
    console.error('[serial] gagal buka:', e.message);
  }
}

bukaSerial();
// Buka ulang otomatis kalau kabel dicolok lagi / port sempat error.
setInterval(() => { if (!esp || !esp.isOpen) bukaSerial(); }, 3000);

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
  console.log(`[wa] siap. notifikasi dikirim lewat kabel ${ESP_PORT}`);
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
  kirimKeEsp(from, text);
});

client.initialize();
