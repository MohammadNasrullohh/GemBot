const http = require("http");

const PORT = 3000;
const SERIAL_PORT = process.env.SERIAL_PORT || "COM4";
const BAUD = 115200;

let serial = null;
let serialJustOpened = false;
const logs = [];

function logEvent(message) {
  const line = `${new Date().toLocaleTimeString()} ${message}`;
  logs.push(line);
  while (logs.length > 80) logs.shift();
  console.log(line);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function closeSerial() {
  return new Promise((resolve) => {
    if (!serial) return resolve();
    const port = serial;
    serial = null;
    if (!port.isOpen) return resolve();
    port.close(() => resolve());
  });
}

async function openSerial() {
  if (serial && serial.isOpen && serial.writable) return serial;
  logEvent(`open ${SERIAL_PORT}`);
  const { SerialPort } = await import("serialport");
  serial = new SerialPort({ path: SERIAL_PORT, baudRate: BAUD, autoOpen: false, rtscts: false });
  serial.on("error", () => {
    logEvent("serial error");
    serial = null;
  });
  await new Promise((resolve, reject) => {
    serial.open((err) => (err ? reject(err) : resolve()));
  });
  await new Promise((resolve) => {
    serial.set({ dtr: false, rts: false }, () => resolve());
  });
  serialJustOpened = true;
  logEvent("serial opened");
  return serial;
}

function writeChunk(port, chunk) {
  return new Promise((resolve, reject) => {
    if (!port || !port.isOpen || !port.writable) {
      reject(new Error("Serial belum siap, coba klik lagi."));
      return;
    }
    port.write(chunk, (err) => {
      if (err) return reject(err);
      port.drain((drainErr) => (drainErr ? reject(drainErr) : resolve()));
    });
  });
}

async function sendPacket(packet, options = {}) {
  const chunkSize = options.chunkSize || 64;
  const chunkDelay = options.chunkDelay ?? 1;
  const openDelay = options.openDelay ?? 80;
  logEvent(`send ${packet.length} bytes`);
  let port = await openSerial();
  if (serialJustOpened) {
    serialJustOpened = false;
    await sleep(openDelay);
  }
  for (let i = 0; i < packet.length; i += chunkSize) {
    if (!port || !port.isOpen || !port.writable) {
      await closeSerial();
      await sleep(120);
      port = await openSerial();
    }
    await writeChunk(port, packet.subarray(i, i + chunkSize));
    if (chunkDelay > 0) await sleep(chunkDelay);
  }
}

function bytesToHex(buffer) {
  let out = "";
  for (const b of buffer) out += b.toString(16).padStart(2, "0");
  return out;
}

async function sendToSerial(buffer) {
  logEvent(`frame start ${buffer.length} bytes`);
  const packet = Buffer.from("H" + bytesToHex(buffer) + "\n", "ascii");
  try {
    await sendPacket(packet, { chunkSize: 128, chunkDelay: 1, openDelay: 180 });
  } catch (err) {
    await closeSerial();
    await sleep(250);
    await sendPacket(packet, { chunkSize: 128, chunkDelay: 1, openDelay: 180 });
  } finally {
    await sleep(20);
    await closeSerial();
    logEvent("serial closed");
  }
}

async function sendCommand(command) {
  const allowed = new Set(["C", "M", "R", "T", "G", "F", "P", "D", "E", "L", "1", "2"]);
  if (!allowed.has(command)) throw new Error("Command tidak valid");
  logEvent(`cmd ${command}`);
  try {
    await sendPacket(Buffer.from(command), { chunkSize: 1, chunkDelay: 0, openDelay: 40 });
  } finally {
    await sleep(10);
    await closeSerial();
    logEvent("serial closed");
  }
}

async function sendReminderText(text) {
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, 32) || "enroll lagi ya deck";
  logEvent(`reminder "${clean}"`);
  try {
    await sendPacket(Buffer.from("S" + clean + "\n", "ascii"), { chunkSize: 34, chunkDelay: 0, openDelay: 40 });
  } finally {
    await sleep(10);
    await closeSerial();
    logEvent("serial closed");
  }
}

async function sendReminderSchedule(time, text) {
  const safeTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(String(time || "")) ? String(time) : "07:30";
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, 32) || "enroll lagi ya deck";
  const payload = `${safeTime}|${clean}`;
  logEvent(`reminder ${payload}`);
  try {
    await sendPacket(Buffer.from("S" + payload + "\n", "ascii"), { chunkSize: 40, chunkDelay: 0, openDelay: 40 });
  } finally {
    await sleep(10);
    await closeSerial();
    logEvent("serial closed");
  }
}

async function sendReminderSchedules(reminders) {
  const items = Array.isArray(reminders) ? reminders.slice(0, 5) : [];
  const payloadItems = items.map((item) => {
    const safeTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(String(item.time || "")) ? String(item.time) : "07:30";
    const clean = String(item.text || "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, 32) || "enroll lagi ya deck";
    return `${safeTime}|${clean}`;
  });
  if (payloadItems.length === 0) payloadItems.push("07:30|enroll lagi ya deck");
  const payload = `A:${payloadItems.join(";")}`;
  logEvent(`reminders ${payloadItems.length}`);
  try {
    await sendPacket(Buffer.from("S" + payload + "\n", "ascii"), { chunkSize: 96, chunkDelay: 0, openDelay: 40 });
  } finally {
    await sleep(10);
    await closeSerial();
    logEvent("serial closed");
  }
}

function pageHtml() {
  return `<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Owi Bot</title>
  <style>
    :root{color-scheme:dark;--bg:#050505;--ink:#f8f8f5;--muted:#b9b9b2;--line:#2c2c2a;--panel:#11110f;--panel2:#181815;--cream:#f4efe2;--black:#050505;--green:#78e3a0;--red:#ff7474}
    *{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--ink);font-family:Inter,ui-sans-serif,system-ui,Arial}
    button,input,textarea{font:inherit}button{min-height:40px;border:1px solid var(--line);border-radius:4px;background:var(--panel2);color:var(--ink);padding:10px 14px;cursor:pointer;text-transform:uppercase;font-size:12px;letter-spacing:.04em}button:hover{border-color:#777}
    button.primary{background:var(--cream);color:#111;border-color:var(--cream);font-weight:900}button.good{background:#12351f;border-color:#29794a;color:#e9fff1}
    input,textarea{width:100%;border:1px solid var(--line);border-radius:4px;background:#090909;color:var(--ink);padding:11px}textarea{min-height:220px;font-family:Consolas,Menlo,monospace;font-size:12px;line-height:1.45;color:#dfeadf}
    .ticker{height:34px;display:flex;align-items:center;overflow:hidden;border-bottom:1px solid var(--line);background:#0c0c0b;color:var(--cream);white-space:nowrap;font-size:12px;letter-spacing:.08em;text-transform:uppercase}.ticker span{display:inline-block;padding-left:100%;animation:marq 24s linear infinite}@keyframes marq{to{transform:translateX(-100%)}}
    header{position:sticky;top:0;z-index:5;background:rgba(5,5,5,.9);backdrop-filter:blur(12px);border-bottom:1px solid var(--line)}.nav{height:68px;display:grid;grid-template-columns:1fr auto 1fr;align-items:center;max-width:1180px;margin:0 auto;padding:0 18px;gap:16px}.links{display:flex;gap:18px}.links a,.accountMini a{color:var(--ink);text-decoration:none;font-size:12px;text-transform:uppercase;letter-spacing:.08em}.brand{font-size:30px;font-weight:950;letter-spacing:.02em}.accountMini{display:flex;justify-content:flex-end;gap:16px;align-items:center}
    main{max-width:1180px;margin:0 auto;padding:0 18px}.hero{min-height:calc(100vh - 102px);display:grid;grid-template-columns:1fr 420px;gap:36px;align-items:center}.eyebrow{color:var(--muted);text-transform:uppercase;letter-spacing:.12em;font-size:12px;margin-bottom:12px}.hero h1{font-size:clamp(52px,8vw,112px);line-height:.86;margin:0 0 20px;text-transform:uppercase;letter-spacing:0}.hero p{font-size:18px;color:var(--muted);max-width:560px;line-height:1.55}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:24px}
    .device{aspect-ratio:1/1.1;border:1px solid var(--line);border-radius:8px;background:radial-gradient(circle at 50% 20%,#282824,#0b0b0a 60%);display:grid;place-items:center;padding:28px}.oled{width:310px;max-width:100%;aspect-ratio:2/1;background:#000;border:10px solid #176ca6;border-radius:4px;display:grid;place-items:center;box-shadow:0 28px 60px rgba(0,0,0,.45);overflow:hidden}.face{width:180px;height:74px;position:relative;animation:floatFace 2.8s ease-in-out infinite}.eye{position:absolute;top:8px;width:34px;height:54px;border-radius:10px;background:#fff;animation:blinkEye 4.2s infinite}.eye.left{left:22px}.eye.right{right:22px}.mouth{position:absolute;left:68px;top:52px;width:44px;height:18px;border-bottom:6px solid #fff;border-radius:0 0 40px 40px;animation:smileMove 2.8s ease-in-out infinite}@keyframes floatFace{0%,100%{transform:translateY(0)}50%{transform:translateY(-5px)}}@keyframes blinkEye{0%,92%,100%{height:54px;top:8px}95%{height:8px;top:32px}}@keyframes smileMove{0%,100%{transform:translateY(0) scaleX(1)}50%{transform:translateY(2px) scaleX(1.12)}}
    .dashboardHero{display:grid;grid-template-columns:420px 1fr;gap:24px;align-items:center;margin-bottom:18px}.dashboardHero .device{min-height:330px}.moodCard{border:1px solid var(--line);background:linear-gradient(135deg,#151512,#0d0d0c);border-radius:8px;padding:22px}.moodCard h2{font-size:46px;margin:0 0 10px;text-transform:uppercase}.moodCard p{color:var(--muted);line-height:1.55}
    .band{border-top:1px solid var(--line);padding:58px 0}.sectionHead{display:flex;justify-content:space-between;gap:18px;align-items:end;margin-bottom:22px}.sectionHead h2{font-size:34px;margin:0;text-transform:uppercase}.sectionHead p{color:var(--muted);max-width:520px;line-height:1.5}.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}.card,.panel,.tile{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:16px}.card h3,.tile h3{margin:0 0 6px;text-transform:uppercase}.card p,.tile p,.muted{color:var(--muted);line-height:1.5}.tile{display:grid;grid-template-columns:1fr auto;gap:14px;align-items:center}.tools{display:grid;grid-template-columns:360px 1fr;gap:16px}.stack{display:grid;gap:12px}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.preview{display:grid;place-items:center;background:#000;border:1px solid var(--line);padding:10px;margin:10px 0}canvas{width:100%;max-width:384px;aspect-ratio:2/1;height:auto;image-rendering:pixelated;background:#000;border:1px solid #3b3b38}.status{color:var(--green);font-size:13px;margin-top:10px}.danger{color:var(--red)}.control{display:grid;grid-template-columns:92px 1fr 42px;gap:10px;align-items:center}.auth{display:grid;grid-template-columns:1fr 1fr;gap:12px}.authBox{border:1px solid var(--line);background:var(--panel);padding:16px;border-radius:6px}.authTabs{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:10px}.authTabs button.active{background:var(--cream);color:#111;border-color:var(--cream);font-weight:900}
    .locked{opacity:.55;filter:grayscale(.4)}footer{border-top:1px solid var(--line);padding:28px 0;color:var(--muted);font-size:13px}
    .hidden{display:none!important}
    @media(max-width:900px){.nav{grid-template-columns:1fr}.brand{order:-1}.links,.accountMini{justify-content:flex-start;flex-wrap:wrap}.hero{grid-template-columns:1fr;min-height:auto;padding:50px 0}.grid,.tools,.auth{grid-template-columns:1fr}.hero h1{font-size:54px}}
  </style>
</head>
<body>
  <div class="ticker"><span>OWI BOT • A TINY DESK FRIEND WITH MANY MOODS • MAKE IT SMILE • MAKE IT PLAY •</span></div>
  <header>
    <div class="nav">
      <nav class="links"><a href="#shop">Public</a><a href="#features">Features</a><a id="navControl" class="hidden" href="/control">Control Panel</a></nav>
      <div class="brand">OWI BOT</div>
      <div class="accountMini"><a id="navLogin" href="#login">Login</a><a id="navDashboard" class="hidden" href="/control">Control Panel</a></div>
    </div>
  </header>
  <main>
    <section class="hero" id="top">
      <div>
        <div class="eyebrow">Welcome to Owi Generation 1</div>
        <h1>Small OLED. Big mood.</h1>
        <p>Owi Bot adalah teman meja kecil yang bisa berekspresi, main game, dan menampilkan gambar pilihanmu.</p>
        <div class="actions"><a href="#shop"><button class="primary">Explore Owi</button></a><a href="#login"><button>Login to Control Panel</button></a></div>
      </div>
      <div class="device"><div class="oled"><div class="face"><div class="eye left"></div><div class="eye right"></div><div class="mouth"></div></div></div></div>
    </section>

    <section class="band" id="shop">
      <div class="sectionHead"><h2>Public showcase</h2><p>Bagian public dibuat seperti product page agar enak dipresentasikan. Area kontrol tetap dipisah di bawah untuk user.</p></div>
      <div class="grid">
        <div class="card"><h3>Personality Engine</h3><p>Idle animation, happy, dizzy, angry, sleepy, dan ekspresi sensor lain.</p></div>
        <div class="card"><h3>Custom Looks</h3><p>Pilih gambar favoritmu dan biarkan Owi menampilkannya dengan gaya layar mungilnya.</p></div>
        <div class="card"><h3>Mini Games</h3><p>Pingpong dan Pesawat vs Alien untuk demo interaksi touch.</p></div>
      </div>
    </section>

    <section class="band" id="features">
      <div class="sectionHead"><h2>Made to feel alive</h2><p>Owi dibuat untuk terasa seperti karakter kecil: bisa merespons suasana, disentuh, diajak main, dan diganti tampilannya.</p></div>
      <div class="grid">
        <div class="card"><h3>Sensor aware</h3><p>Suhu, kelembapan, tilt, dan guncangan memengaruhi ekspresi.</p></div>
        <div class="card"><h3>Personal looks</h3><p>Ubah gambar menjadi tampilan mungil yang cocok untuk wajah Owi.</p></div>
        <div class="card"><h3>Private control</h3><p>Area kontrol hanya muncul setelah kamu masuk sebagai user.</p></div>
      </div>
    </section>

    <section class="band" id="login">
      <div class="sectionHead"><h2>Account</h2><p>Login lokal untuk membuka area user. Akun demo tersimpan di browser laptop.</p></div>
      <div class="auth">
        <div class="authBox">
          <div class="authTabs"><button id="loginTab" class="active">Login</button><button id="registerTab">Register</button></div>
          <div class="stack">
            <input id="authName" placeholder="Nama pengguna">
            <input id="authPass" placeholder="Password" type="password">
            <button id="authSubmit" class="primary">Masuk</button>
            <button id="logoutBtn" style="display:none">Logout</button>
            <p id="authStatus" class="muted">Masuk untuk membuka user tools.</p>
          </div>
        </div>
        <div class="panel">
          <h3>User access</h3>
          <p class="muted">Setelah login, kamu bisa mengubah wajah Owi, mengirim gambar, bermain, dan memberi sentuhan virtual.</p>
          <p id="homeStatus" class="status">Ready</p>
        </div>
      </div>
    </section>

    <footer>Owi Bot companion web</footer>
  </main>
<script>
let authMode='login',currentUser=localStorage.getItem('owi_current_user')||'';
function getUsers(){try{return JSON.parse(localStorage.getItem('owi_users')||'{}')}catch{return {}}}
function saveUsers(users){localStorage.setItem('owi_users',JSON.stringify(users))}
function setAuthStatus(text,bad){const el=document.getElementById('authStatus');el.textContent=text;el.className=bad?'danger':'muted'}
function updateAuthUi(){const logged=!!currentUser;document.getElementById('loginTab').classList.toggle('active',authMode==='login');document.getElementById('registerTab').classList.toggle('active',authMode==='register');document.getElementById('authSubmit').textContent=authMode==='login'?'Masuk':'Buat Akun';document.getElementById('logoutBtn').style.display=logged?'block':'none';document.getElementById('navControl').classList.toggle('hidden',!logged);document.getElementById('navDashboard').classList.toggle('hidden',!logged);document.getElementById('navLogin').classList.toggle('hidden',logged);if(logged)setAuthStatus('Hai, '+currentUser+'. Control Panel aktif.',false)}
document.getElementById('loginTab').onclick=()=>{authMode='login';updateAuthUi()};
document.getElementById('registerTab').onclick=()=>{authMode='register';updateAuthUi()};
document.getElementById('authSubmit').onclick=()=>{const name=document.getElementById('authName').value.trim(),pass=document.getElementById('authPass').value;if(name.length<3||pass.length<4){setAuthStatus('Nama minimal 3 huruf, password minimal 4.',true);return}const users=getUsers();if(authMode==='register'){if(users[name]){setAuthStatus('Nama itu sudah dipakai.',true);return}users[name]={pass};saveUsers(users)}else if(!users[name]||users[name].pass!==pass){setAuthStatus('Nama atau password belum cocok.',true);return}currentUser=name;localStorage.setItem('owi_current_user',name);updateAuthUi();location.href='/control'};
document.getElementById('logoutBtn').onclick=()=>{currentUser='';localStorage.removeItem('owi_current_user');updateAuthUi();location.hash='top'};
updateAuthUi();
</script>
</body>
</html>`;
}

function controlPageHtml() {
  return `<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Owi Bot Control</title>
  <style>:root{color-scheme:dark;--bg:#050505;--ink:#f8f8f5;--muted:#b9b9b2;--line:#2c2c2a;--panel:#11110f;--panel2:#181815;--cream:#f4efe2;--green:#78e3a0;--red:#ff7474}*{box-sizing:border-box}body{margin:0;background:#050505;color:var(--ink);font-family:Inter,ui-sans-serif,system-ui,Arial}main{max-width:1120px;margin:0 auto;padding:20px}button,input,textarea{font:inherit}button{min-height:40px;border:1px solid var(--line);border-radius:4px;background:var(--panel2);color:var(--ink);padding:10px 14px;cursor:pointer;text-transform:uppercase;font-size:12px;letter-spacing:.04em}button.primary{background:var(--cream);color:#111;border-color:var(--cream);font-weight:900}button.good{background:#12351f;border-color:#29794a;color:#e9fff1}input,textarea{width:100%;border:1px solid var(--line);border-radius:4px;background:#090909;color:var(--ink);padding:11px}textarea{min-height:220px;font-family:Consolas,Menlo,monospace;font-size:12px}.top{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid var(--line);padding-bottom:14px;margin-bottom:18px}.brand{font-size:28px;font-weight:950}.muted{color:var(--muted);line-height:1.5}.status{color:var(--green);font-size:13px}.danger{color:var(--red)}.dashboard{display:grid;grid-template-columns:390px 1fr;gap:18px;margin-bottom:18px}.panel{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:16px}.device{min-height:300px;border:1px solid var(--line);border-radius:8px;background:radial-gradient(circle at 50% 20%,#282824,#0b0b0a 60%);display:grid;place-items:center}.oled{width:300px;max-width:100%;aspect-ratio:2/1;background:#000;border:10px solid #176ca6;border-radius:4px;display:grid;place-items:center;overflow:hidden}.face{width:180px;height:74px;position:relative;animation:floatFace 2.8s ease-in-out infinite}.eye{position:absolute;top:8px;width:34px;height:54px;border-radius:10px;background:#fff;animation:blinkEye 4.2s infinite}.eye.left{left:22px}.eye.right{right:22px}.mouth{position:absolute;left:68px;top:52px;width:44px;height:18px;border-bottom:6px solid #fff;border-radius:0 0 40px 40px;animation:smileMove 2.8s ease-in-out infinite}@keyframes floatFace{0%,100%{transform:translateY(0)}50%{transform:translateY(-5px)}}@keyframes blinkEye{0%,92%,100%{height:54px;top:8px}95%{height:8px;top:32px}}@keyframes smileMove{0%,100%{transform:translateY(0) scaleX(1)}50%{transform:translateY(2px) scaleX(1.12)}}.tools{display:grid;grid-template-columns:360px 1fr;gap:16px}.stack{display:grid;gap:12px}.row{display:flex;flex-wrap:wrap;gap:8px;align-items:center}.preview{display:grid;place-items:center;background:#000;border:1px solid var(--line);padding:10px;margin:10px 0}canvas{width:100%;max-width:384px;aspect-ratio:2/1;height:auto;image-rendering:pixelated;background:#000;border:1px solid #3b3b38}.control{display:grid;grid-template-columns:92px 1fr 42px;gap:10px;align-items:center}@media(max-width:850px){.dashboard,.tools{grid-template-columns:1fr}.top{display:grid;gap:10px}}</style></head>
  <body><main><div class="top"><div><div class="brand">OWI BOT</div><p class="muted">Control Panel</p></div><div class="row"><a href="/"><button>Public</button></a><button id="logoutBtn">Logout</button></div></div>
  <section class="dashboard"><div class="device"><div class="oled"><div class="face"><div class="eye left"></div><div class="eye right"></div><div class="mouth"></div></div></div></div><div class="panel"><p class="muted">Owi sedang standby dan siap diajak main.</p><h1>Happy idle</h1><div class="row"><button class="primary" data-cmd="P">Tap Owi</button><button class="primary" data-cmd="D">Double Klik</button><button data-cmd="E">Elus Owi</button><button data-cmd="F">Balik Wajah</button></div><p id="status" class="status">Ready</p></div></section>
  <section class="tools"><div class="panel"><h3>Send a look</h3><input id="file" type="file" accept="image/*,video/*"><div class="preview"><canvas id="c" width="128" height="64"></canvas></div><div class="row"><button id="send" class="primary">Tampilkan</button><button id="play">Gerakkan</button><button id="stop">Berhenti</button><button id="clear">Balik Wajah</button></div></div><div class="stack"><div class="panel"><h3>Look maker</h3><div class="control"><span class="muted">Detail</span><input id="threshold" type="range" min="40" max="220" value="118"><strong id="thresholdValue">118</strong></div><div class="row"><label><input id="invert" type="checkbox"> Invert</label><label><input id="cropFill" type="checkbox"> Crop fill</label></div><input id="bitmapName" value="owi_look"><div class="row"><button id="copyBitmap" class="primary">Copy Look</button><button id="downloadBitmap">Download</button><button id="refreshBitmap">Refresh</button></div><textarea id="bitmapOutput" spellcheck="false"></textarea></div><div class="panel"><h3>Controls</h3><div class="row"><button data-cmd="P">Tap</button><button class="primary" data-cmd="D">Double Tap</button><button data-cmd="E">Elus</button><button data-cmd="L">Hold</button><button data-cmd="F">Balik Wajah</button></div></div><div class="panel"><h3>Reminder</h3><div id="reminderList" class="stack"></div><div class="row"><button id="addReminder">Tambah</button><button id="sendReminder" class="primary">Simpan Semua</button><button id="sendReminderText">Preview Teks</button><button data-cmd="R">On / Off</button></div></div><div class="panel"><h3>Games</h3><div class="row"><button class="good" data-cmd="1">Pingpong</button><button class="good" data-cmd="2">Pesawat vs Alien</button></div></div><div class="panel"><h3>Web log</h3><textarea id="webLog" readonly spellcheck="false"></textarea></div></div></section></main>
  <video id="v" muted playsinline loop style="display:none"></video><img id="img" style="display:none"><script>
  if(!localStorage.getItem('owi_current_user')) location.href='/#login';
  const c=document.getElementById('c'),ctx=c.getContext('2d',{willReadFrequently:true}),file=document.getElementById('file'),img=document.getElementById('img'),v=document.getElementById('v'),st=document.getElementById('status'),threshold=document.getElementById('threshold'),thresholdValue=document.getElementById('thresholdValue'),invert=document.getElementById('invert'),cropFill=document.getElementById('cropFill'),bitmapName=document.getElementById('bitmapName'),bitmapOutput=document.getElementById('bitmapOutput'),reminderList=document.getElementById('reminderList');let source=null,timer=null;function setStatus(t,b){st.textContent=t;st.className=b?'status danger':'status'}function fitDraw(el){ctx.fillStyle='#000';ctx.fillRect(0,0,128,64);const sw=el.videoWidth||el.naturalWidth,sh=el.videoHeight||el.naturalHeight;if(!sw||!sh)return;const scale=cropFill.checked?Math.max(128/sw,64/sh):Math.min(128/sw,64/sh),w=sw*scale,h=sh*scale;ctx.drawImage(el,(128-w)/2,(64-h)/2,w,h)}function makeFrame(){if(source)fitDraw(source);const data=ctx.getImageData(0,0,128,64).data,out=new Uint8Array(1024),limit=Number(threshold.value),inv=invert.checked;for(let y=0;y<64;y++)for(let x=0;x<128;x++){const i=(y*128+x)*4,lum=(data[i]*30+data[i+1]*59+data[i+2]*11)/100;if(inv?lum<limit:lum>limit)out[y*16+(x>>3)]|=128>>(x&7)}return out}function cleanName(n){return(n||'owi_look').replace(/[^a-zA-Z0-9_]/g,'_').replace(/^[0-9]/,'_$&')||'owi_look'}function bitmapCode(bytes){const name=cleanName(bitmapName.value),lines=['#include <Arduino.h>','','const uint8_t '+name+'[] PROGMEM = {'];for(let i=0;i<bytes.length;i+=16)lines.push('  '+Array.from(bytes.slice(i,i+16)).map(b=>'0x'+b.toString(16).padStart(2,'0').toUpperCase()).join(', ')+(i+16<bytes.length?',':''));lines.push('};');return lines.join('\\n')}function updateBitmapOutput(){thresholdValue.textContent=threshold.value;bitmapOutput.value=bitmapCode(makeFrame())}function addReminderRow(time='07:30',text='enroll lagi ya deck'){if(reminderList.children.length>=5){setStatus('Maksimal 5 reminder.',true);return}const row=document.createElement('div');row.className='row reminderRow';row.innerHTML='<input class="reminderTime" type="time" value="'+time+'"><input class="reminderText" maxlength="32" value="'+text.replace(/"/g,'&quot;')+'"><button type="button" class="removeReminder">Hapus</button>';row.querySelector('.removeReminder').onclick=()=>{if(reminderList.children.length>1)row.remove()};reminderList.appendChild(row)}function collectReminders(){return Array.from(reminderList.querySelectorAll('.reminderRow')).slice(0,5).map(row=>({time:row.querySelector('.reminderTime').value,text:row.querySelector('.reminderText').value}))}async function sendFrame(){try{const frame=makeFrame();updateBitmapOutput();setStatus('Mengirim tampilan ke Owi...');const res=await fetch('/frame',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:frame});await res.text();setStatus('Tampilan baru sudah muncul di Owi.')}catch(e){setStatus(e.message,true)}}file.onchange=()=>{clearInterval(timer);timer=null;const f=file.files[0];if(!f)return;const url=URL.createObjectURL(f),done=()=>{fitDraw(source);updateBitmapOutput()};if(f.type.startsWith('video/')){source=v;v.src=url;v.play();v.onloadeddata=done}else{source=img;img.src=url;img.onload=done}};document.getElementById('send').onclick=sendFrame;document.getElementById('play').onclick=()=>{if(!source)return;clearInterval(timer);timer=setInterval(sendFrame,180)};document.getElementById('stop').onclick=()=>{clearInterval(timer);timer=null;setStatus('Berhenti')};document.getElementById('clear').onclick=async()=>{clearInterval(timer);timer=null;const r=await fetch('/clear',{method:'POST'});setStatus(await r.text())};document.getElementById('addReminder').onclick=()=>addReminderRow('12:00','enroll lagi ya deck');document.getElementById('sendReminder').onclick=async()=>{clearInterval(timer);timer=null;try{const r=await fetch('/reminder',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reminders:collectReminders()})});setStatus(await r.text())}catch(e){setStatus(e.message,true)}};document.getElementById('sendReminderText').onclick=async()=>{clearInterval(timer);timer=null;try{const list=collectReminders();const r=await fetch('/reminder',{method:'POST',headers:{'Content-Type':'text/plain'},body:(list[0]&&list[0].text)||'enroll lagi ya deck'});setStatus(await r.text())}catch(e){setStatus(e.message,true)}};document.getElementById('refreshBitmap').onclick=updateBitmapOutput;document.getElementById('copyBitmap').onclick=async()=>{updateBitmapOutput();await navigator.clipboard.writeText(bitmapOutput.value);setStatus('Look tersalin.')};document.getElementById('downloadBitmap').onclick=()=>{updateBitmapOutput();const blob=new Blob([bitmapOutput.value],{type:'text/plain'}),a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=cleanName(bitmapName.value)+'.h';a.click();URL.revokeObjectURL(a.href)};[threshold,invert,cropFill,bitmapName].forEach(el=>el.addEventListener('input',updateBitmapOutput));document.querySelectorAll('[data-cmd]').forEach(btn=>btn.onclick=async()=>{clearInterval(timer);timer=null;try{const r=await fetch('/cmd/'+btn.dataset.cmd,{method:'POST'});setStatus(await r.text())}catch(e){setStatus(e.message,true)}});document.getElementById('logoutBtn').onclick=()=>{localStorage.removeItem('owi_current_user');location.href='/'};addReminderRow();updateBitmapOutput();
  async function refreshLog(){try{const r=await fetch('/logs');document.getElementById('webLog').value=await r.text();}catch(e){}}
  setInterval(refreshLog,1000);refreshLog();
  </script></body></html>`;
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    res.end(pageHtml());
    return;
  }
  if (req.method === "GET" && req.url === "/control") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    res.end(controlPageHtml());
    return;
  }
  if (req.method === "GET" && req.url === "/logs") {
    res.writeHead(200, { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" });
    res.end(logs.join("\n"));
    return;
  }
  if (req.method === "POST" && req.url === "/clear") {
    sendCommand("C").then(() => res.end("Balik ke wajah")).catch((err) => {res.writeHead(500);res.end(err.message);});
    return;
  }
  if (req.method === "POST" && req.url.startsWith("/cmd/")) {
    const cmd = decodeURIComponent(req.url.slice("/cmd/".length)).slice(0, 1);
    sendCommand(cmd).then(() => res.end("Command " + cmd + " terkirim")).catch((err) => {res.writeHead(500);res.end(err.message);});
    return;
  }
  if (req.method === "POST" && req.url === "/reminder") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      const text = Buffer.concat(chunks).toString("utf8");
      try {
        if ((req.headers["content-type"] || "").includes("application/json")) {
          const data = JSON.parse(text || "{}");
          if (Array.isArray(data.reminders)) {
            await sendReminderSchedules(data.reminders);
            res.end("Semua reminder tersimpan");
          } else {
            await sendReminderSchedule(data.time, data.text);
            res.end("Reminder jam tersimpan");
          }
        } else {
          await sendReminderText(text);
          res.end("Reminder teks tersimpan");
        }
      } catch (err) {
        res.writeHead(500);
        res.end(err.message);
      }
    });
    return;
  }
  if (req.method === "POST" && req.url === "/frame") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      const body = Buffer.concat(chunks);
      if (body.length !== 1024) {res.writeHead(400);res.end("Frame harus 1024 byte");return}
      try {await sendToSerial(body);res.end("Terkirim ke OLED")} catch (err) {res.writeHead(500);res.end(err.message)}
    });
    return;
  }
  res.writeHead(404);res.end("Not found");
});

server.listen(PORT, () => {
  console.log("Web: http://localhost:" + PORT);
  console.log("Serial: " + SERIAL_PORT + " @ " + BAUD);
});
