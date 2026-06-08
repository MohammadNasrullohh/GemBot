const http = require("http");
const dgram = require("dgram");
const net = require("net");
const { spawn } = require("child_process");
const ffmpegPath = require("ffmpeg-static");
require("dotenv").config();
const { GoogleGenAI } = require("@google/genai");

const PORT = 3000;
const SERIAL_PORT = process.env.SERIAL_PORT || "COM4";
const BAUD = 115200;
const AI_DAILY_LIMIT = Number(process.env.AI_DAILY_LIMIT || 30);
const AI_PROVIDER = String(process.env.AI_PROVIDER || "gemini").toLowerCase();
const KOBOLLM_BASE_URL = (process.env.KOBOLLM_BASE_URL || process.env.KOBOILLM_BASE_URL || "https://lite.koboillm.com/v1").replace(/\/+$/, "");
const KOBOLLM_MODEL = process.env.KOBOLLM_MODEL || process.env.KOBOILLM_MODEL || "openai/gpt-4o-mini";
const GEMINI_MODEL = process.env.GEMINI_MODEL || "gemini-2.5-flash";
const AI_MAX_TOKENS = Number(process.env.AI_MAX_TOKENS || 256);
const OWI_SYSTEM_PROMPT = "Kamu adalah Owi, robot desktop kecil yang lucu, ceria, dan sedikit jail. Jawab bahasa Indonesia santai, ramah, dan singkat. Maksimal 1-2 kalimat pendek agar muat di OLED 128x64. Jawab langsung tanpa proses berpikir.";

let serial = null;
let serialJustOpened = false;
const logs = [];
let aiUsage = { date: new Date().toISOString().slice(0, 10), count: 0 };

function getKoboiKey() {
  return process.env.KOBOLLM_API_KEY || process.env.KOBOILLM_API_KEY || "";
}

function getGeminiKey() {
  return process.env.GEMINI_API_KEY || "";
}

let geminiChat = null;
try {
  if (getGeminiKey()) {
    const ai = new GoogleGenAI({ apiKey: getGeminiKey() });
    geminiChat = ai.chats.create({
      model: GEMINI_MODEL,
      config: {
        systemInstruction: OWI_SYSTEM_PROMPT
      }
    });
  }
} catch (e) {
  console.log("Gemini initialization failed:", e.message);
}

function getAiLimitStatus() {
  const today = new Date().toISOString().slice(0, 10);
  if (aiUsage.date !== today) aiUsage = { date: today, count: 0 };
  return {
    date: aiUsage.date,
    used: aiUsage.count,
    limit: AI_DAILY_LIMIT,
    remaining: Math.max(0, AI_DAILY_LIMIT - aiUsage.count),
    enabled: !!(getKoboiKey() || getGeminiKey()),
    provider: getGeminiKey() && AI_PROVIDER !== "kobollm" && AI_PROVIDER !== "koboillm" ? "Gemini" : (getKoboiKey() ? "KoboiLLM" : "off"),
    model: getGeminiKey() && AI_PROVIDER !== "kobollm" && AI_PROVIDER !== "koboillm" ? GEMINI_MODEL : (getKoboiKey() ? KOBOLLM_MODEL : ""),
  };
}

function sanitizeOledText(text) {
  let cleaned = String(text || "")
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[\r\n\t]+/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  cleaned = cleaned.replace(/[^\x20-\x7E]/g, "");
  if (cleaned.length > 200) cleaned = cleaned.substring(0, 197) + "...";
  return cleaned || "Aku belum dapat jawabannya.";
}

async function askKoboiLLM(userMsg) {
  const key = getKoboiKey();
  if (!key) throw new Error("KOBOLLM_API_KEY belum ada di .env");
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 25000);
  try {
    const response = await fetch(`${KOBOLLM_BASE_URL}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${key}`,
      },
      body: JSON.stringify({
        model: KOBOLLM_MODEL,
        temperature: 0.7,
        max_tokens: AI_MAX_TOKENS,
        messages: [
          { role: "system", content: OWI_SYSTEM_PROMPT },
          { role: "user", content: userMsg },
        ],
      }),
      signal: controller.signal,
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      const msg = data?.error?.message || data?.message || `KoboiLLM HTTP ${response.status}`;
      throw new Error(msg);
    }
    return data?.choices?.[0]?.message?.content || data?.choices?.[0]?.text || "";
  } finally {
    clearTimeout(timeout);
  }
}

async function askGemini(userMsg) {
  if (!getGeminiKey() || !geminiChat) throw new Error("GEMINI_API_KEY belum ada di .env");
  const response = await geminiChat.sendMessage({ message: userMsg });
  return response.text || "";
}

async function askOwi(userMsg) {
  if ((AI_PROVIDER === "kobollm" || AI_PROVIDER === "koboillm") && getKoboiKey()) {
    return { provider: "KoboiLLM", model: KOBOLLM_MODEL, text: await askKoboiLLM(userMsg) };
  }
  if (getGeminiKey()) return { provider: "Gemini", model: GEMINI_MODEL, text: await askGemini(userMsg) };
  if (getKoboiKey()) return { provider: "KoboiLLM", model: KOBOLLM_MODEL, text: await askKoboiLLM(userMsg) };
  throw new Error("API key belum dipasang. Buat .env lalu isi KOBOLLM_API_KEY atau GEMINI_API_KEY.");
}

let latestTelemetry = {};
const udpServer = dgram.createSocket("udp4");
udpServer.on("message", (msg, rinfo) => {
  try {
    latestTelemetry = JSON.parse(msg.toString());
    latestTelemetry.lastUpdate = Date.now();
    latestTelemetry.ip = rinfo.address;
    if (!isStreamingAudio) {
      if (latestTelemetry.req_song === 1 || latestTelemetry.req_lovestory === 1) {
        streamAudio(latestTelemetry.ip, "0.28", "lovestory.mp3");
      } else if (latestTelemetry.req_song === 2) {
        streamAudio(latestTelemetry.ip, "0.32", "mbg.mp3");
      } else if (latestTelemetry.req_song === 3) {
        streamAudio(latestTelemetry.ip, "0.45", "hai_owi.wav");
      }
    }
  } catch(e){}
});
udpServer.bind(7788);

let isStreamingAudio = false;

function clampVolume(value, fallback = 0.22) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.max(0.04, Math.min(0.55, parsed));
}

async function streamAudio(ip, volume = "0.30", mp3Path = "lovestory.mp3") {
  if (isStreamingAudio) return;
  if (!ip) return;
  isStreamingAudio = true;
  logEvent(`stream audio ${mp3Path} ke ${ip}:7777 vol ${volume}`);

  try {
    const port = 7777;
    const sampleRate = 16000;
    const bytesPerSecond = sampleRate * 2;
    const safeVolume = clampVolume(volume).toFixed(2);
    const chunkSize = 512;

    const socket = net.createConnection({ host: ip, port });
    socket.setNoDelay(true);
    await new Promise((resolve, reject) => {
      socket.once("connect", resolve);
      socket.once("error", reject);
    });

    const ffmpeg = spawn(ffmpegPath, [
      "-hide_banner",
      "-loglevel", "error",
      "-i", mp3Path,
      "-f", "s16le",
      "-acodec", "pcm_s16le",
      "-ac", "1",
      "-ar", String(sampleRate),
      "-filter:a", `highpass=f=95,lowpass=f=7200,loudnorm=I=-20:TP=-2.5:LRA=8,acompressor=threshold=-24dB:ratio=2.2:attack=18:release=240,alimiter=limit=0.38,volume=${safeVolume}`,
      "pipe:1",
    ], { stdio: ["ignore", "pipe", "pipe"] });

    let sent = 0;
    let started = Date.now();
    const leadBytes = 8192;

    for await (const chunk of ffmpeg.stdout) {
      for (let offset = 0; offset < chunk.length; offset += chunkSize) {
        const slice = chunk.subarray(offset, Math.min(offset + chunkSize, chunk.length));
        await new Promise((resolve, reject) => socket.write(slice, (err) => err ? reject(err) : resolve()));
        sent += slice.length;

        if (sent > leadBytes) {
          const targetMs = ((sent - leadBytes) / bytesPerSecond) * 1000;
          const elapsedMs = Date.now() - started;
          const waitMs = targetMs - elapsedMs;
          if (waitMs > 1) await sleep(Math.min(waitMs, 24));
        } else {
          started = Date.now();
        }
      }
    }

    await sleep(500);
    socket.end();
  } catch (err) {
    logEvent(`stream audio err: ${err.message}`);
  } finally {
    isStreamingAudio = false;
    logEvent("stream audio selesai");
  }
}

async function streamTestTone(ip, volume = "0.35") {
  if (isStreamingAudio) return;
  if (!ip) return;
  isStreamingAudio = true;
  const safeVolume = clampVolume(volume, 0.35);
  logEvent(`test MAX tone ke ${ip}:7777 vol ${safeVolume.toFixed(2)}`);

  try {
    const port = 7777;
    const sampleRate = 16000;
    const durationMs = 1800;
    const frequency = 880;
    const frames = Math.floor(sampleRate * durationMs / 1000);
    const chunkFrames = 256;
    const leadBytes = 4096;
    let sent = 0;
    let started = Date.now();

    const socket = net.createConnection({ host: ip, port });
    socket.setNoDelay(true);
    await new Promise((resolve, reject) => {
      socket.once("connect", resolve);
      socket.once("error", reject);
    });

    for (let frame = 0; frame < frames; frame += chunkFrames) {
      const n = Math.min(chunkFrames, frames - frame);
      const chunk = Buffer.alloc(n * 2);
      for (let i = 0; i < n; i++) {
        const t = (frame + i) / sampleRate;
        const envelope = Math.min(1, Math.min((frame + i) / 1200, (frames - frame - i) / 1200));
        const sample = Math.round(Math.sin(2 * Math.PI * frequency * t) * 26000 * safeVolume * envelope);
        chunk.writeInt16LE(sample, i * 2);
      }

      await new Promise((resolve, reject) => socket.write(chunk, (err) => err ? reject(err) : resolve()));
      sent += chunk.length;
      if (sent > leadBytes) {
        const targetMs = ((sent - leadBytes) / (sampleRate * 2)) * 1000;
        const elapsedMs = Date.now() - started;
        const waitMs = targetMs - elapsedMs;
        if (waitMs > 1) await sleep(Math.min(waitMs, 18));
      } else {
        started = Date.now();
      }
    }

    await sleep(250);
    socket.end();
  } catch (err) {
    logEvent(`test MAX err: ${err.message}`);
  } finally {
    isStreamingAudio = false;
    logEvent("test MAX selesai");
  }
}

function logEvent(message) {
  const line = `${new Date().toLocaleTimeString()} ${message}`;
  logs.push(line);
  while (logs.length > 80) logs.shift();
  console.log(line);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function timedSerialOperation(label, ms, executor) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new Error(`${label} timeout`));
    }, ms);
    const finish = (err, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      err ? reject(err) : resolve(value);
    };
    try {
      executor(finish);
    } catch (err) {
      finish(err);
    }
  });
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
  if (serial) await closeSerial();
  logEvent(`open ${SERIAL_PORT}`);
  const { SerialPort } = await import("serialport");
  const nextPort = new SerialPort({ path: SERIAL_PORT, baudRate: BAUD, autoOpen: false, rtscts: false });
  serial = nextPort;
  nextPort.on("error", () => {
    logEvent("serial error");
    if (serial === nextPort) serial = null;
    if (nextPort.isOpen) nextPort.close(() => {});
  });
  await timedSerialOperation("serial open", 2500, (done) => {
    nextPort.open((err) => done(err));
  });
  await timedSerialOperation("serial set", 1200, (done) => {
    nextPort.set({ dtr: false, rts: false }, (err) => done(err));
  });
  serialJustOpened = true;
  logEvent("serial opened");
  return nextPort;
}

function writeChunk(port, chunk) {
  return timedSerialOperation("serial write", 1800, (done) => {
    if (!port || !port.isOpen || !port.writable) {
      done(new Error("Serial belum siap, coba klik lagi."));
      return;
    }
    port.write(chunk, (err) => {
      if (err) return done(err);
      port.drain((drainErr) => done(drainErr));
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
  }
}

async function sendCommand(command) {
  const allowed = new Set(["C", "M", "R", "T", "G", "F", "P", "O", "D", "E", "L", "W", "1", "2"]);
  if (!allowed.has(command)) throw new Error("Command tidak valid");
  logEvent(`cmd ${command}`);
  await sendPacket(Buffer.from(command + "\n"), { chunkSize: 8, chunkDelay: 0, openDelay: 40 });
}

async function sendChatText(text) {
  const clean = sanitizeOledText(text).slice(0, 200);
  logEvent(`chat "${clean}"`);
  await sendPacket(Buffer.from("T:" + clean + "\n", "ascii"), { chunkSize: 48, chunkDelay: 0, openDelay: 40 });
}

async function sendReminderText(text) {
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, 32) || "enroll lagi ya deck";
  logEvent(`reminder "${clean}"`);
  await sendPacket(Buffer.from("S" + clean + "\n", "ascii"), { chunkSize: 34, chunkDelay: 0, openDelay: 40 });
}

async function sendReminderSchedule(time, text) {
  const safeTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(String(time || "")) ? String(time) : "07:30";
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").trim().slice(0, 32) || "enroll lagi ya deck";
  const payload = `${safeTime}|${clean}`;
  logEvent(`reminder ${payload}`);
  await sendPacket(Buffer.from("S" + payload + "\n", "ascii"), { chunkSize: 40, chunkDelay: 0, openDelay: 40 });
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
  await sendPacket(Buffer.from("S" + payload + "\n", "ascii"), { chunkSize: 96, chunkDelay: 0, openDelay: 40 });
}

function pageHtml() {
  return `<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Owi Bot</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@700;900&family=Roboto+Mono:wght@500;700&display=swap');
    :root {
      --bg: #f5f5f5;
      --text: #000000;
      --border: 3px solid #000;
      --shadow: 4px 4px 0 #000;
      --hover-shadow: 2px 2px 0 #000;
      --accent: #ff0000;
      --accent-alt: #0000ff;
    }
    * { box-sizing: border-box; }
    html { scroll-behavior: smooth; }
    body {
      margin: 0; background: var(--bg); color: var(--text);
      font-family: 'Inter', sans-serif; text-transform: uppercase;
      overflow-x: hidden;
    }
    h1, h2, h3 { font-weight: 900; margin: 0 0 1rem; letter-spacing: -1px; }
    p { line-height: 1.5; font-family: 'Roboto Mono', monospace; font-weight: 500; margin: 0 0 1.5rem; text-transform: none; }
    a { color: var(--text); text-decoration: none; border-bottom: 3px solid transparent; transition: 0.2s; }
    a:hover { border-bottom: 3px solid #000; }

    /* Ticker */
    .ticker {
      border-bottom: var(--border); padding: 10px 0; background: #fff;
      font-family: 'Roboto Mono', monospace; font-weight: 700; overflow: hidden; white-space: nowrap;
      display: flex;
    }
    .ticker span { padding-left: 100%; animation: marq 20s linear infinite; }
    @keyframes marq { to { transform: translateX(-100%); } }

    /* Header */
    header { border-bottom: var(--border); background: var(--bg); position: sticky; top: 0; z-index: 50; }
    .nav { max-width: 1200px; margin: 0 auto; padding: 1rem 2rem; display: flex; justify-content: space-between; align-items: center; }
    .brand { font-size: 2.5rem; font-weight: 900; background: var(--accent); color: #fff; padding: 0 10px; border: var(--border); box-shadow: var(--shadow); }
    .nav-links { display: flex; gap: 2rem; font-family: 'Roboto Mono', monospace; font-weight: 700; }
    .nav-links a { padding: 0.5rem 1rem; border: var(--border); background: #fff; box-shadow: var(--shadow); transition: 0.1s; border-bottom: var(--border); }
    .nav-links a:hover { transform: translate(2px, 2px); box-shadow: var(--hover-shadow); background: var(--accent-alt); color: #fff; }
    .nav-links a#navControl { background: var(--accent); color: #fff; }
    
    /* Forms & Buttons */
    button, input, textarea {
      font-family: 'Roboto Mono', monospace; font-weight: 700; text-transform: uppercase;
      border-radius: 0; outline: none; border: var(--border); color: #000;
    }
    button {
      background: #fff; padding: 1rem 2rem; cursor: pointer;
      box-shadow: var(--shadow); transition: transform 0.1s, box-shadow 0.1s;
    }
    button:hover { transform: translate(2px, 2px); box-shadow: var(--hover-shadow); }
    button:active { transform: translate(4px, 4px); box-shadow: 0 0 0 #000; }
    button.primary { background: var(--accent); color: #fff; }

    input, textarea {
      width: 100%; padding: 1rem; background: #fff;
      box-shadow: var(--shadow); margin-bottom: 1.5rem;
    }
    input:focus, textarea:focus { background: #e0e0e0; }

    /* Layout */
    main { max-width: 1200px; margin: 0 auto; padding: 4rem 2rem; }
    .hero { display: grid; grid-template-columns: 1.2fr 1fr; gap: 4rem; align-items: center; margin-bottom: 6rem; }
    .hero-text h1 { font-size: 5rem; line-height: 0.9; margin-bottom: 2rem; }
    .eyebrow { font-family: 'Roboto Mono', monospace; font-weight: 700; background: #000; color: #fff; padding: 5px 15px; display: inline-block; margin-bottom: 1rem; border: var(--border); box-shadow: var(--shadow); }
    .actions { display: flex; gap: 1.5rem; flex-wrap: wrap; margin-top: 2rem; }

    /* Device Preview */
    .device-container { display: flex; justify-content: center; perspective: none; }
    .device {
      border: var(--border); background: #fff; padding: 2rem;
      box-shadow: 10px 10px 0 #000; width: 100%; max-width: 400px; aspect-ratio: 1;
      display: grid; place-items: center;
    }
    .oled {
      width: 100%; aspect-ratio: 2/1; background: #000; border: var(--border);
      position: relative; overflow: hidden;
    }
    .face { width: 100%; height: 100%; position: absolute; }
    .eye { position: absolute; top: 20%; width: 15%; height: 35%; background: var(--accent); }
    .eye.left { left: 20%; } .eye.right { right: 20%; }
    .mouth { position: absolute; bottom: 20%; left: 35%; width: 30%; height: 10%; background: var(--accent); }

    /* Sections */
    .section { margin-bottom: 6rem; }
    .section-head { text-align: left; max-width: 800px; margin-bottom: 3rem; }
    .section-head h2 { font-size: 3.5rem; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 2rem; }
    .panel { border: var(--border); background: #fff; padding: 2rem; box-shadow: var(--shadow); }

    /* Auth */
    .auth-container { max-width: 500px; margin: 0 auto; }
    .auth-tabs { display: flex; gap: 1rem; margin-bottom: 2rem; }
    .auth-tabs button { flex: 1; box-shadow: none; transform: none; background: transparent; border: 3px solid transparent; border-bottom: var(--border); }
    .auth-tabs button.active { background: #000; color: #fff; border: var(--border); }
    
    .status-msg { margin-top: 1rem; font-family: 'Roboto Mono', monospace; font-weight: 700; padding: 1rem; border: var(--border); display: none; }
    .status-msg.show { display: block; }
    .danger { background: var(--accent); color: #fff; }
    .success { background: #00ff00; color: #000; }
    .hidden { display: none !important; }

    footer { text-align: center; padding: 3rem; border-top: var(--border); font-family: 'Roboto Mono', monospace; font-weight: 700; background: #fff; }

    @media (max-width: 768px) {
      .hero { grid-template-columns: 1fr; gap: 2rem; }
      .hero-text h1 { font-size: 3.5rem; }
      .nav { flex-direction: column; gap: 1.5rem; }
      .nav-links { flex-wrap: wrap; justify-content: center; }
    }
  </style>
</head>
<body>
  <div class="ticker"><span>OWI BOT • BRUTALIST EDITION • NO BLURS • NO GRADIENTS • JUST PURE PIXELS AND HARD EDGES •</span></div>
  
  <header>
    <div class="nav">
      <div class="brand">OWI BOT</div>
      <nav class="nav-links">
        <a href="#features">FITUR</a>
        <a id="navLogin" href="#login">LOGIN</a>
        <a id="navControl" class="hidden" href="/control">CONTROL PANEL</a>
      </nav>
    </div>
  </header>

  <main>
    <section class="hero" id="top">
      <div class="hero-text">
        <span class="eyebrow">OWI GENERATION 1</span>
        <h1>SMALL OLED.<br>BIG MOOD.</h1>
        <p>Owi Bot is a tiny desk companion that reacts to its environment. Built with raw, unapologetic brutalist aesthetics. No soft corners.</p>
        <div class="actions">
          <a href="#features"><button class="primary">EXPLORE</button></a>
          <a href="#login"><button>LOGIN</button></a>
        </div>
      </div>
      <div class="device-container">
        <div class="device">
          <div class="oled">
            <div class="face">
              <div class="eye left"></div>
              <div class="eye right"></div>
              <div class="mouth"></div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="section" id="features">
      <div class="section-head">
        <h2>MADE TO FEEL ALIVE</h2>
        <p>Raw sensors. Direct feedback. High contrast interactions.</p>
      </div>
      <div class="grid">
        <div class="panel">
          <h3>SENSOR AWARE</h3>
          <p>Touch and motion sensors directly dictate Owi's expression. Zero latency, pure response.</p>
        </div>
        <div class="panel">
          <h3>PERSONAL LOOKS</h3>
          <p>Upload raw images. Destroy them into pure 1-bit high-contrast arrays. Feed them to Owi.</p>
        </div>
        <div class="panel">
          <h3>PRIVATE CONTROL</h3>
          <p>Locked down interface. Authenticate to gain raw access to the control mechanisms.</p>
        </div>
      </div>
    </section>

    <section class="section" id="login">
      <div class="auth-container panel">
        <div class="auth-tabs">
          <button id="loginTab" class="active">LOGIN</button>
          <button id="registerTab">REGISTER</button>
        </div>
        <div>
          <input id="authName" placeholder="USERNAME" type="text" autocomplete="off">
          <input id="authPass" placeholder="PASSWORD" type="password">
          <button id="authSubmit" class="primary" style="width: 100%;">ENTER</button>
          <button id="logoutBtn" style="display:none; width: 100%; margin-top: 1.5rem;">LOGOUT</button>
          <div id="authStatus" class="status-msg"></div>
        </div>
      </div>
    </section>
  </main>

  <footer>
    OWI BOT COMPANION WEB &copy; 2026. BRUTALIST EDITION.
  </footer>

  <script>
    let authMode = 'login';
    let currentUser = localStorage.getItem('owi_current_user') || '';
    
    function getUsers() { try { return JSON.parse(localStorage.getItem('owi_users') || '{}') } catch { return {} } }
    function saveUsers(users) { localStorage.setItem('owi_users', JSON.stringify(users)) }
    function setAuthStatus(text, bad) {
      const el = document.getElementById('authStatus');
      el.textContent = text;
      el.className = 'status-msg show ' + (bad ? 'danger' : 'success');
    }
    
    function updateAuthUi() {
      const logged = !!currentUser;
      document.getElementById('loginTab').classList.toggle('active', authMode === 'login');
      document.getElementById('registerTab').classList.toggle('active', authMode === 'register');
      document.getElementById('authSubmit').textContent = authMode === 'login' ? 'ENTER' : 'CREATE ACCOUNT';
      document.getElementById('authSubmit').style.display = logged ? 'none' : 'block';
      document.getElementById('authName').style.display = logged ? 'none' : 'block';
      document.getElementById('authPass').style.display = logged ? 'none' : 'block';
      document.getElementById('logoutBtn').style.display = logged ? 'block' : 'none';
      document.getElementById('navControl').classList.toggle('hidden', !logged);
      document.getElementById('navLogin').classList.toggle('hidden', logged);
      
      const st = document.getElementById('authStatus');
      if (logged) setAuthStatus('ACCESS GRANTED: ' + currentUser, false);
      else st.classList.remove('show');
    }
    
    document.getElementById('loginTab').onclick = () => { authMode = 'login'; updateAuthUi(); };
    document.getElementById('registerTab').onclick = () => { authMode = 'register'; updateAuthUi(); };
    
    document.getElementById('authSubmit').onclick = () => {
      const name = document.getElementById('authName').value.trim();
      const pass = document.getElementById('authPass').value;
      if (name.length < 3 || pass.length < 4) {
        setAuthStatus('MIN 3 CHAR USER, MIN 4 CHAR PASS.', true);
        return;
      }
      const users = getUsers();
      if (authMode === 'register') {
        if (users[name]) { setAuthStatus('USER EXISTS.', true); return; }
        users[name] = { pass };
        saveUsers(users);
      } else {
        if (!users[name] || users[name].pass !== pass) {
          setAuthStatus('INVALID CREDENTIALS.', true);
          return;
        }
      }
      currentUser = name;
      localStorage.setItem('owi_current_user', name);
      updateAuthUi();
      location.href = '/control';
    };
    
    document.getElementById('logoutBtn').onclick = () => {
      currentUser = '';
      localStorage.removeItem('owi_current_user');
      updateAuthUi();
    };
    
    updateAuthUi();
  </script>
</body>
</html>`;
}

function controlPageHtml() {
  return `<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Owi Bot Control</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Inter:wght@700;900&family=Roboto+Mono:wght@500;700&display=swap');
    :root {
      --bg: #f5f5f5;
      --text: #000;
      --border: 3px solid #000;
      --shadow: 4px 4px 0 #000;
      --hover-shadow: 2px 2px 0 #000;
      --accent: #ff0000;
      --accent-alt: #0000ff;
      --success: #00ff00;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    html { scroll-behavior: smooth; }
    body {
      background: var(--bg); color: var(--text);
      font-family: 'Inter', sans-serif; text-transform: uppercase;
      overflow-x: hidden; min-height: 100vh;
    }
    h2, h3 { font-weight: 900; letter-spacing: -1px; }
    p { font-family: 'Roboto Mono', monospace; text-transform: none; font-weight: 500; margin: 0; }

    .top-bar {
      display: flex; justify-content: space-between; align-items: center;
      padding: 1rem 2rem; border-bottom: var(--border); background: #fff;
      position: sticky; top: 0; z-index: 50;
    }
    .brand { font-size: 1.8rem; font-weight: 900; background: var(--accent); color: #fff; padding: 4px 12px; border: var(--border); box-shadow: var(--shadow); display: inline-block; }
    .sub-brand { font-size: 0.7rem; background: #000; color: #fff; padding: 2px 8px; display: inline-block; margin-left: 0.5rem; vertical-align: middle; }

    button, input {
      font-family: 'Roboto Mono', monospace; font-size: 0.85rem; font-weight: 700;
      outline: none; border: var(--border); text-transform: uppercase; border-radius: 0;
    }
    button {
      padding: 0.7rem 1.2rem; cursor: pointer; background: #fff; color: #000;
      box-shadow: var(--shadow); transition: transform 0.1s, box-shadow 0.1s;
    }
    button:hover { transform: translate(2px, 2px); box-shadow: var(--hover-shadow); }
    button:active { transform: translate(4px, 4px); box-shadow: 0 0 0 #000; }
    button.primary { background: var(--accent); color: #fff; }
    button.blue { background: var(--accent-alt); color: #fff; }
    button.sm { padding: 0.5rem 0.8rem; font-size: 0.75rem; }

    input[type="text"], input[type="time"] {
      width: 100%; padding: 0.7rem; background: #fff; color: #000; box-shadow: var(--shadow);
    }
    input:focus { background: #e0e0e0; }

    main { max-width: 1100px; margin: 0 auto; padding: 1.5rem; }
    .row { display: flex; gap: 0.8rem; flex-wrap: wrap; align-items: center; }
    .panel { border: var(--border); background: #fff; padding: 1.5rem; box-shadow: var(--shadow); }

    .hero-dash {
      display: grid; grid-template-columns: 300px 1fr; gap: 1.5rem; margin-bottom: 1.5rem;
    }
    @media (max-width: 800px) { .hero-dash { grid-template-columns: 1fr; } }

    .face-box {
      border: var(--border); background: #000; padding: 1.5rem;
      box-shadow: 8px 8px 0 #000; display: grid; place-items: center;
    }
    .oled {
      width: 100%; aspect-ratio: 2/1; background: #000; border: 2px solid #333;
      position: relative; overflow: hidden;
    }
    .face { width: 100%; height: 100%; position: absolute; transition: transform 0.15s ease-out; }
    .eye { position: absolute; top: 20%; width: 15%; height: 35%; background: var(--accent); transition: all 0.2s; }
    .eye.left { left: 20%; } .eye.right { right: 20%; }
    .mouth { position: absolute; bottom: 20%; left: 35%; width: 30%; height: 10%; background: var(--accent); transition: all 0.2s; }
    @keyframes breathe { 0%,100%{transform:scale(1)} 50%{transform:scale(1.02)} }
    .face-box .oled { animation: breathe 3s ease-in-out infinite; }
    .face-label { margin-top: 0.8rem; text-align: center; font-family: 'Roboto Mono', monospace; font-weight: 700; color: #555; font-size: 0.75rem; }
    .ip-label { margin-top: 0.3rem; text-align: center; font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.7rem; color: var(--accent); }

    .ctrl-stack { display: flex; flex-direction: column; gap: 1rem; justify-content: space-between; }
    .ctrl-stack h2 { font-size: 1.6rem; margin-bottom: 0.3rem; }

    .badge {
      background: #000; color: #fff; padding: 6px 10px;
      font-family: 'Roboto Mono', monospace; font-weight: 700;
      font-size: 0.75rem; border: 2px solid #000; transition: box-shadow 0.2s;
    }
    .badge.ok { box-shadow: 2px 2px 0 var(--success); }
    .badge.err { box-shadow: 2px 2px 0 var(--accent); }
    .badge.active { background: var(--accent); }

    .gesture-row { display: flex; gap: 6px; flex-wrap: wrap; min-height: 28px; }
    .gesture-badge {
      background: #eee; color: #999; padding: 3px 8px;
      font-family: 'Roboto Mono', monospace; font-weight: 700;
      font-size: 0.65rem; border: 2px solid #ccc; transition: all 0.15s;
    }
    .gesture-badge.on { background: #000; color: #fff; border-color: #000; }

    .status-bar {
      font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.75rem;
      padding: 0.5rem 0.8rem; background: #000; color: var(--success); border: var(--border);
    }
    .status-bar.err { color: var(--accent); }

    .sensor-grid {
      display: grid; grid-template-columns: repeat(4, 1fr); gap: 1rem; margin-bottom: 1.5rem;
    }
    @media (max-width: 800px) { .sensor-grid { grid-template-columns: repeat(2, 1fr); } }
    .sensor-card {
      border: var(--border); background: #fff; padding: 1rem;
      box-shadow: var(--shadow); text-align: center;
    }
    .sensor-card .label { font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.65rem; color: #666; margin-bottom: 0.3rem; }
    .sensor-card .value { font-family: 'Inter', sans-serif; font-weight: 900; font-size: 2rem; line-height: 1; letter-spacing: -2px; }
    .sensor-card .unit { font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.7rem; color: #999; }

    .tools-grid {
      display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 1.5rem;
    }
    .tools-grid h3 { font-size: 1.1rem; margin-bottom: 0.8rem; border-bottom: var(--border); padding-bottom: 0.5rem; }

    .reminderRow { display: grid; grid-template-columns: 100px 1fr auto; gap: 0.5rem; margin-bottom: 0.5rem; }
    .reminderRow input { margin: 0; box-shadow: 2px 2px 0 #000; font-size: 0.8rem; padding: 0.5rem; }

    .pingpong-card { text-align: center; }
    .score-display { display: flex; justify-content: center; align-items: center; gap: 1.5rem; margin: 1rem 0; }
    .score-num { font-family: 'Inter', sans-serif; font-weight: 900; font-size: 3.5rem; line-height: 1; letter-spacing: -3px; }
    .score-vs { font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.8rem; color: #999; }
    .score-label { font-family: 'Roboto Mono', monospace; font-weight: 700; font-size: 0.65rem; color: #666; }
  </style>
</head>
<body>
  <div class="top-bar">
    <div>
      <span class="brand">OWI BOT</span>
      <span class="sub-brand">CONTROL PANEL</span>
    </div>
    <div class="row">
      <a href="/"><button class="sm">PUBLIC WEB</button></a>
      <button id="logoutBtn" class="sm primary">LOGOUT</button>
    </div>
  </div>

  <main>
    <section class="hero-dash">
      <div class="face-box">
        <div class="oled">
          <div class="face">
            <div class="eye left"></div>
            <div class="eye right"></div>
            <div class="mouth"></div>
          </div>
        </div>
        <div class="face-label" id="faceLabel">MENUNGGU KONEKSI...</div>
        <div class="ip-label" id="ipLabel">IP: --</div>
      </div>

      <div class="panel ctrl-stack">
        <div>
          <h2>LIVE STATUS</h2>
          <div class="row" style="margin-bottom:0.8rem;">
            <span id="badgeMpu" class="badge">MPU: --</span>
            <span id="badgeInmp" class="badge">INMP: --</span>
            <span id="badgeMax" class="badge">MAX: --</span>
          </div>
          <div id="gestureRow" class="gesture-row">
            <span class="gesture-badge" data-g="touch">TOUCH</span>
            <span class="gesture-badge" data-g="nod">NOD</span>
            <span class="gesture-badge" data-g="headShake">GELENG</span>
            <span class="gesture-badge" data-g="surprised">KAGET</span>
            <span class="gesture-badge" data-g="curious">CURIOUS</span>
            <span class="gesture-badge" data-g="angry">ANGRY</span>
            <span class="gesture-badge" data-g="laugh">LAUGH</span>
            <span class="gesture-badge" data-g="sleep">SLEEP</span>
            <span class="gesture-badge" data-g="dizzy">PUSING</span>
            <span class="gesture-badge" data-g="sad">SEDIH</span>
            <span class="gesture-badge" data-g="love">LOVE</span>
            <span class="gesture-badge" data-g="cry">CRY</span>
            <span class="gesture-badge" data-g="pant">PANAS</span>
          </div>
        </div>
        <div>
          <div class="row" style="margin-bottom:0.8rem;">
            <span style="font-size:0.75rem;font-weight:700;font-family:'Roboto Mono',monospace;color:#999;">POSISI OWI SAAT INI:</span>
            <span id="menuStateLabel" class="badge" style="background:#000;color:var(--success);">--</span>
          </div>
          <div class="row" style="margin-bottom:0.8rem;">
            <button type="button" class="primary" data-cmd="P">TAP (NEXT)</button>
            <button type="button" class="primary" data-cmd="O">HOLD (OK)</button>
            <button type="button" data-cmd="E">PET</button>
            <button type="button" id="btnLoveStory" class="blue">&#9835; LOVE STORY</button>
            <button type="button" id="btnMbg" class="blue">&#9835; MBG</button>
            <button type="button" id="btnTestMax">TEST MAX</button>
          </div>
          <div class="row" style="margin-bottom:0.8rem;">
            <span style="font-size:0.7rem;font-weight:700;font-family:'Roboto Mono',monospace;">VOL MUSIK:</span>
            <input type="range" id="volLoveStory" min="4" max="55" value="30" style="width:100px;">
          </div>
          <div style="border:var(--border);padding:0.7rem;margin-bottom:0.8rem;background:#fff7d1;box-shadow:2px 2px 0 #000;">
            <div style="font-family:'Roboto Mono',monospace;font-size:0.7rem;font-weight:900;margin-bottom:0.4rem;">AI LIMIT HARI INI</div>
            <div class="row" style="gap:0.5rem;">
              <span id="aiLimitBadge" class="badge">AI: --</span>
              <span id="aiKeyBadge" class="badge">KEY: --</span>
            </div>
          </div>
          <div id="status" class="status-bar">SYSTEM READY</div>
        </div>
      </div>
    </section>

    <section class="sensor-grid">
      <div class="sensor-card" style="border-color:var(--accent);">
        <div class="label">EKSPRESI</div>
        <div class="value" id="valExpr" style="font-size:1.2rem;letter-spacing:0;">--</div>
        <div class="unit" id="valSpeech" style="color:var(--accent);font-size:0.85rem;">...</div>
      </div>
      <div class="sensor-card">
        <div class="label">SUHU</div>
        <div class="value" id="valTemp">--</div>
        <div class="unit">&deg;C</div>
      </div>
      <div class="sensor-card">
        <div class="label">KELEMBABAN</div>
        <div class="value" id="valHum">--</div>
        <div class="unit">%RH</div>
      </div>
      <div class="sensor-card">
        <div class="label">GUNCANGAN</div>
        <div class="value" id="valShake">0</div>
        <div class="unit">METER</div>
      </div>
    </section>

    <section class="tools-grid">
      <div class="panel">
        <h3>REMINDERS</h3>
        <div id="reminderList"></div>
        <div class="row" style="margin-top:0.8rem;">
          <button id="addReminder" class="sm">+ TAMBAH</button>
          <button id="sendReminder" class="sm primary">SYNC</button>
          <button id="sendReminderText" class="sm">KIRIM TEKS</button>
        </div>
      </div>
      <div class="panel pingpong-card">
        <h3>PINGPONG</h3>
        <div class="score-display">
          <div><div class="score-label">KAMU</div><div class="score-num" id="scoreP">0</div></div>
          <div class="score-vs">VS</div>
          <div><div class="score-label">AI</div><div class="score-num" id="scoreA">0</div></div>
        </div>
        <div class="row" style="justify-content:center;">
          <button class="blue" data-cmd="G">&#127955; MULAI GAME</button>
          <button class="sm" data-cmd="C">KEMBALI</button>
        </div>
      </div>
      <div class="panel" style="grid-column:1/-1;">
        <h3>DRAW OLED</h3>
        <div class="row" style="align-items:flex-start;">
          <canvas id="drawCanvas" width="128" height="64" style="width:512px;max-width:100%;image-rendering:pixelated;background:#000;border:var(--border);box-shadow:var(--shadow);touch-action:none;"></canvas>
          <div style="display:flex;flex-direction:column;gap:0.7rem;min-width:170px;">
            <button type="button" id="enterDraw" class="primary">MASUK DRAW</button>
            <button type="button" id="clearDraw">CLEAR</button>
            <button type="button" id="drawBack" class="sm" data-cmd="C">BALIK WAJAH</button>
            <div id="drawSyncState" style="font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:900;color:var(--success);">LIVE DRAW SIAP</div>
            <label style="font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;">BRUSH
              <input type="range" id="brushSize" min="1" max="7" value="3" style="width:100%;margin-top:0.4rem;">
            </label>
          </div>
        </div>
      </div>
      <div class="panel" style="grid-column:1/-1;">
        <h3>&#127908; SPEECH RECOGNITION (INMP441)</h3>
        <div style="display:grid;grid-template-columns:120px 1fr 74px;gap:0.7rem;align-items:center;margin-bottom:0.8rem;">
          <div style="font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;">MIC LEVEL</div>
          <div style="height:14px;border:var(--border);background:#111;overflow:hidden;">
            <div id="inmpLevelBar" style="height:100%;width:0%;background:linear-gradient(90deg,#37ff8b,#ffe66d,#ff5b7c);transition:width 90ms linear;"></div>
          </div>
          <div id="inmpLevelText" style="font-family:'Roboto Mono',monospace;font-size:0.8rem;font-weight:900;text-align:right;">0%</div>
        </div>
        <div style="display:flex;gap:0.5rem;flex-wrap:wrap;margin-bottom:0.8rem;">
          <span id="inmpActiveBadge" class="gesture-badge">IDLE</span>
          <span id="inmpPeakBadge" class="gesture-badge">PEAK 0%</span>
        </div>
        <div class="row" style="margin-bottom:0.8rem;">
          <button id="startSpeech" class="primary">MULAI DENGAR</button>
          <button id="stopSpeech" class="sm">STOP</button>
          <span id="speechStatus" style="font-family:'Roboto Mono',monospace;font-weight:700;font-size:0.75rem;color:#999;">IDLE</span>
        </div>
        <div id="speechLive" style="font-family:'Roboto Mono',monospace;font-weight:700;font-size:1.1rem;min-height:2rem;padding:0.8rem;border:var(--border);background:#000;color:var(--success);margin-bottom:0.8rem;text-transform:none;">...</div>
        <div id="speechLog" style="font-family:'Roboto Mono',monospace;font-size:0.75rem;max-height:150px;overflow-y:auto;padding:0.5rem;border:var(--border);background:#f9f9f9;text-transform:none;color:#333;"></div>
      </div>
      <div class="panel" style="grid-column:1/-1;">
        <h3>&#129302; CHATBOT OWI (GEMINI)</h3>
        <p style="font-size:0.8rem;margin-bottom:0.8rem;color:#555;">Ketik atau pakai tombol dengar. Owi akan paham lewat model Gemini dan jawab langsung ke OLED.</p>
        <div id="chatHistory" style="font-family:sans-serif;font-size:0.85rem;height:180px;overflow-y:auto;padding:0.5rem;border:var(--border);background:#fff;margin-bottom:0.8rem;display:flex;flex-direction:column;gap:0.5rem;">
          <!-- chat messages -->
        </div>
        <div style="display:flex;gap:0.5rem;">
          <input type="text" id="chatInput" placeholder="Ketik pesan..." style="flex:1;padding:0.5rem;border:var(--border);font-family:inherit;font-size:0.9rem;">
          <button id="sendChatBtn" class="primary" style="padding:0 1rem;">KIRIM</button>
        </div>
      </div>
    </section>
  </main>

  <script>
    if(!localStorage.getItem('owi_current_user')) location.href='/#login';
    const st=document.getElementById('status');
    const reminderList=document.getElementById('reminderList');
    function setStatus(t,bad){st.textContent=t;st.className=bad?'status-bar err':'status-bar';}

    function addReminderRow(time,text){
      time=time||'07:30';text=text||'enroll lagi ya deck';
      if(reminderList.children.length>=5){setStatus('MAX 5 REMINDERS.',true);return;}
      const row=document.createElement('div');row.className='reminderRow';
      row.innerHTML='<input class="reminderTime" type="time" value="'+time+'"><input class="reminderText" maxlength="32" value="'+text.replace(/"/g,'&quot;')+'"><button type="button" class="sm" style="padding:0.5rem">X</button>';
      row.querySelector('button').onclick=()=>{if(reminderList.children.length>1)row.remove();};
      reminderList.appendChild(row);
    }
    function collectReminders(){
      return Array.from(reminderList.querySelectorAll('.reminderRow')).slice(0,5).map(r=>({
        time:r.querySelector('.reminderTime').value,
        text:r.querySelector('.reminderText').value
      }));
    }
    document.getElementById('addReminder').onclick=()=>addReminderRow('12:00','enroll lagi ya deck');
    document.getElementById('sendReminder').onclick=async()=>{
      try{const r=await fetch('/reminder',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reminders:collectReminders()})});setStatus(await r.text());}
      catch(e){setStatus(e.message,true);}
    };
    document.getElementById('sendReminderText').onclick=async()=>{
      try{const list=collectReminders();const r=await fetch('/reminder',{method:'POST',headers:{'Content-Type':'text/plain'},body:(list[0]&&list[0].text)||'enroll lagi ya deck'});setStatus(await r.text());}
      catch(e){setStatus(e.message,true);}
    };
    addReminderRow();

    async function playMusicClick(ev, file) {
      ev.preventDefault();
      ev.stopPropagation();
      try {
        const vol = document.getElementById('volLoveStory').value;
        const r = await fetch('/play_audio', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ volume: (vol/100).toFixed(2), file }) });
        setStatus(await r.text());
      } catch(e) { setStatus(e.message, true); }
    }
    document.getElementById('btnLoveStory').onclick = (ev) => playMusicClick(ev, 'lovestory.mp3');
    document.getElementById('btnMbg').onclick = (ev) => playMusicClick(ev, 'mbg.mp3');
    document.getElementById('btnTestMax').onclick = async (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      try {
        const vol = document.getElementById('volLoveStory').value;
        const r = await fetch('/test_max', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ volume: (vol/100).toFixed(2) }) });
        setStatus(await r.text());
      } catch(e) { setStatus(e.message, true); }
    };

    const drawCanvas=document.getElementById('drawCanvas');
    const drawCtx=drawCanvas.getContext('2d',{willReadFrequently:true});
    const drawSyncState=document.getElementById('drawSyncState');
    drawCtx.fillStyle='#000';drawCtx.fillRect(0,0,128,64);
    drawCtx.strokeStyle='#fff';drawCtx.fillStyle='#fff';drawCtx.lineCap='round';drawCtx.lineJoin='round';
    let drawing=false,lastPt=null,drawModeReady=false,drawSyncTimer=null,drawSyncBusy=false,drawSyncPending=false;
    function setDrawSyncState(text,bad){
      drawSyncState.textContent=text;
      drawSyncState.style.color=bad?'var(--danger)':'var(--success)';
    }
    async function enterDrawMode(){
      if(drawModeReady)return;
      const r=await fetch('/cmd/W',{method:'POST'});
      const text=await r.text();
      if(!r.ok)throw new Error(text||'Gagal masuk draw');
      drawModeReady=true;
      setDrawSyncState('LIVE DRAW AKTIF',false);
    }
    function canvasPoint(ev){
      const r=drawCanvas.getBoundingClientRect();
      const src=ev.touches&&ev.touches[0]?ev.touches[0]:ev;
      return {x:Math.max(0,Math.min(127,Math.floor((src.clientX-r.left)*128/r.width))),y:Math.max(0,Math.min(63,Math.floor((src.clientY-r.top)*64/r.height)))};
    }
    function drawAt(pt){
      const b=Number(document.getElementById('brushSize').value||3);
      drawCtx.lineWidth=b;
      drawCtx.strokeStyle='#fff';drawCtx.fillStyle='#fff';
      if(lastPt){drawCtx.beginPath();drawCtx.moveTo(lastPt.x,lastPt.y);drawCtx.lineTo(pt.x,pt.y);drawCtx.stroke();}
      drawCtx.beginPath();drawCtx.arc(pt.x,pt.y,Math.max(0.5,b/2),0,Math.PI*2);drawCtx.fill();
      lastPt=pt;
      scheduleDrawSync();
    }
    function down(ev){ev.preventDefault();drawing=true;lastPt=null;drawAt(canvasPoint(ev));}
    function move(ev){if(!drawing)return;ev.preventDefault();drawAt(canvasPoint(ev));}
    function up(){drawing=false;lastPt=null;}
    drawCanvas.addEventListener('pointerdown',down);
    drawCanvas.addEventListener('pointermove',move);
    window.addEventListener('pointerup',up);
    drawCanvas.addEventListener('touchstart',down,{passive:false});
    drawCanvas.addEventListener('touchmove',move,{passive:false});
    window.addEventListener('touchend',up);
    function canvasToOledBytes(){
      const img=drawCtx.getImageData(0,0,128,64).data;
      const out=new Uint8Array(1024);
      for(let y=0;y<64;y++){
        for(let xb=0;xb<16;xb++){
          let v=0;
          for(let bit=0;bit<8;bit++){
            const x=xb*8+bit;
            const idx=(y*128+x)*4;
            const on=img[idx]+img[idx+1]+img[idx+2]>384;
            if(on)v|=(0x80>>bit);
          }
          out[y*16+xb]=v;
        }
      }
      return out;
    }
    async function sendDrawFrame(showStatus){
      await enterDrawMode();
      const bytes=canvasToOledBytes();
      const r=await fetch('/frame',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:bytes});
      const text=await r.text();
      if(!r.ok)throw new Error(text||'Frame gagal');
      if(showStatus)setStatus(text,false);
      setDrawSyncState('LIVE SYNC '+new Date().toLocaleTimeString('id-ID',{hour12:false}),false);
    }
    function scheduleDrawSync(){
      drawSyncPending=true;
      if(drawSyncTimer)return;
      drawSyncTimer=setTimeout(flushDrawSync,140);
    }
    async function flushDrawSync(){
      drawSyncTimer=null;
      if(drawSyncBusy)return;
      if(!drawSyncPending)return;
      drawSyncPending=false;
      drawSyncBusy=true;
      try{await sendDrawFrame(false);}
      catch(e){setDrawSyncState(e.message,true);}
      finally{
        drawSyncBusy=false;
        if(drawSyncPending)scheduleDrawSync();
      }
    }
    document.getElementById('enterDraw').onclick=async()=>{try{drawModeReady=false;await enterDrawMode();await sendDrawFrame(true);}catch(e){setStatus(e.message,true);setDrawSyncState(e.message,true);}};
    document.getElementById('clearDraw').onclick=async()=>{drawCtx.fillStyle='#000';drawCtx.fillRect(0,0,128,64);try{await sendDrawFrame(true);}catch(e){setStatus(e.message,true);setDrawSyncState(e.message,true);}};

    document.querySelectorAll('[data-cmd]').forEach(btn=>btn.onclick=async()=>{
      try{
        let r = await fetch('/cmd/'+btn.dataset.cmd,{method:'POST'});
        setStatus(await r.text());
        if(btn.dataset.cmd==='C'){
          drawModeReady=false;
          setDrawSyncState('LIVE DRAW SIAP',false);
        }
      }catch(e){setStatus(e.message,true);}
    });
    document.getElementById('logoutBtn').onclick=()=>{localStorage.removeItem('owi_current_user');location.href='/';};

    async function refreshSensors(){
      try{
        const r=await fetch('/api/sensors');const s=await r.json();
        if(!s.lastUpdate)return;
        const bMpu=document.getElementById('badgeMpu');
        bMpu.textContent='MPU: '+(s.mpu==1?'OK':'ERR');
        bMpu.className='badge '+(s.mpu==1?'ok':'err');
        const bInmp=document.getElementById('badgeInmp');
        const inmpPct=s.inmp||0;
        bInmp.textContent='INMP: '+inmpPct+'%';
        bInmp.className='badge '+(inmpPct>0?'ok':'');
        const inmpPeak=s.inmpPeak||0;
        document.getElementById('inmpLevelBar').style.width=Math.max(0,Math.min(100,inmpPct))+'%';
        document.getElementById('inmpLevelText').textContent=inmpPct+'%';
        const inmpActive=document.getElementById('inmpActiveBadge');
        inmpActive.textContent=s.micActive?'MENDENGAR':'IDLE';
        inmpActive.classList.toggle('on',!!s.micActive);
        const inmpPeakEl=document.getElementById('inmpPeakBadge');
        inmpPeakEl.textContent='PEAK '+inmpPeak+'%';
        inmpPeakEl.classList.toggle('on',inmpPeak>25);
        const bMax=document.getElementById('badgeMax');
        bMax.textContent=(s.max==1?'🔊 MAX: PLAY':'🔈 MAX: IDLE');
        bMax.className='badge '+(s.max==1?'active':'');

        const gMap={touch:s.touch,nod:s.nod,headShake:s.headShake,surprised:s.surprised,curious:s.curious,angry:s.angry,laugh:s.laugh,sleep:s.sleep,dizzy:s.dizzy,sad:s.sad,love:s.love,cry:s.cry,pant:s.pant};
        document.querySelectorAll('.gesture-badge').forEach(el=>{el.classList.toggle('on',!!gMap[el.dataset.g]);});

        const temp=s.temp;
        document.getElementById('valTemp').textContent=(temp&&temp>-90)?temp.toFixed(1):'--';
        document.getElementById('valHum').textContent=(s.hum&&s.hum>=0)?s.hum.toFixed(0):'--';
        document.getElementById('valShake').textContent=Number(s.shakeMeter||0).toFixed(1);

        // Expression
        if(s.expr) {
          document.getElementById('valExpr').textContent=s.expr;
          document.getElementById('faceLabel').textContent=s.expr;
        }

        const stateMap = ["WAJAH NORMAL", "MENU UTAMA", "GAMES PINGPONG", "SENSOR SUHU", "REMINDER ALARM", "DRAW OLED", "PILIH LAGU"];
        if(s.state !== undefined && s.state >= 0 && s.state < stateMap.length) {
          document.getElementById('menuStateLabel').textContent = stateMap[s.state];
        }

        if(s.scoreP!==undefined)document.getElementById('scoreP').textContent=s.scoreP;
        if(s.scoreA!==undefined)document.getElementById('scoreA').textContent=s.scoreA;

        const faceEl=document.querySelector('.face');
        if(faceEl)faceEl.style.transform='translate('+(s.tiltX*40||0)+'px, '+(s.tiltY*30||0)+'px)';
        if(s.ip)document.getElementById('ipLabel').textContent='IP: '+s.ip;
        setStatus('TILT X:'+Number(s.tiltX||0).toFixed(2)+' Y:'+Number(s.tiltY||0).toFixed(2)+' | SHAKE:'+Number(s.shakeMeter||0).toFixed(2));
      }catch(e){}
    }
    setInterval(refreshSensors,250);

    async function refreshAiLimit(){
      try{
        const r=await fetch('/api/ai-limit');const s=await r.json();
        const b=document.getElementById('aiLimitBadge');
        b.textContent='AI: '+s.used+'/'+s.limit+' SISA '+s.remaining;
        b.className='badge '+(s.remaining<=3?'err':s.remaining<=8?'active':'ok');
        const k=document.getElementById('aiKeyBadge');
        k.textContent=s.enabled?'KEY: SIAP':'KEY: BELUM';
        k.className='badge '+(s.enabled?'ok':'err');
      }catch(e){}
    }
    refreshAiLimit();
    setInterval(refreshAiLimit,5000);

    // ─── SPEECH RECOGNITION (Web Speech API - id-ID) ───
    let recognition = null;
    let isListening = false;
    const speechLive = document.getElementById('speechLive');
    const speechLog = document.getElementById('speechLog');
    const speechStatus = document.getElementById('speechStatus');

    function initSpeech() {
      const SR = window.SpeechRecognition || window.webkitSpeechRecognition;
      if (!SR) { speechStatus.textContent = 'TIDAK DIDUKUNG'; return null; }
      const r = new SR();
      r.lang = 'id-ID';
      r.continuous = true;
      r.interimResults = true;
      r.maxAlternatives = 1;
      r.onstart = () => { isListening = true; speechStatus.textContent = 'MENDENGAR...'; speechStatus.style.color = 'var(--accent)'; };
      r.onend = () => { if (isListening) { try { r.start(); } catch(e){} } else { speechStatus.textContent = 'IDLE'; speechStatus.style.color = '#999'; } };
      r.onerror = (e) => { if (e.error !== 'no-speech' && e.error !== 'aborted') { speechStatus.textContent = 'ERR: ' + e.error; } };
      r.onresult = (e) => {
        let interim = '';
        for (let i = e.resultIndex; i < e.results.length; i++) {
          const t = e.results[i][0].transcript;
          if (e.results[i].isFinal) {
            const ts = new Date().toLocaleTimeString('id-ID',{hour:'2-digit',minute:'2-digit',second:'2-digit'});
            const line = document.createElement('div');
            line.textContent = '[' + ts + '] ' + t;
            speechLog.prepend(line);
            speechLive.textContent = t;
            speechLive.style.color = 'var(--success)';
            // DENGAR -> PAHAM -> JAWAB: transcript final masuk ke chatbot, bukan reminder.
            if (chatInput && sendChatBtn) {
              chatInput.value = t.trim();
              sendChatBtn.click();
            }
          } else {
            interim += t;
          }
        }
        if (interim) { speechLive.textContent = interim; speechLive.style.color = '#ffff00'; }
      };
      return r;
    }

    document.getElementById('startSpeech').onclick = () => {
      if (!recognition) recognition = initSpeech();
      if (!recognition) return;
      isListening = true;
      try { recognition.start(); } catch(e) {}
    };
    document.getElementById('stopSpeech').onclick = () => {
      isListening = false;
      if (recognition) try { recognition.stop(); } catch(e) {}
      speechStatus.textContent = 'IDLE';
      speechLive.textContent = '...';
    };

    // Chatbot UI Logic
    const chatInput = document.getElementById('chatInput');
    const sendChatBtn = document.getElementById('sendChatBtn');
    const chatHistory = document.getElementById('chatHistory');

    function appendChat(sender, msg, color, bg) {
      const bubble = document.createElement('div');
      bubble.style.padding = '0.5rem 0.8rem';
      bubble.style.borderRadius = '8px';
      bubble.style.maxWidth = '85%';
      bubble.style.background = bg;
      bubble.style.color = color;
      bubble.style.alignSelf = sender === 'User' ? 'flex-end' : 'flex-start';
      bubble.style.boxShadow = '1px 1px 0 #000';
      const strong = document.createElement('strong');
      strong.textContent = sender;
      bubble.appendChild(strong);
      bubble.appendChild(document.createElement('br'));
      bubble.appendChild(document.createTextNode(msg));
      chatHistory.appendChild(bubble);
      chatHistory.scrollTop = chatHistory.scrollHeight;
    }

    sendChatBtn.onclick = () => {
      const msg = chatInput.value.trim();
      if (!msg) return;
      chatInput.value = '';
      sendChatBtn.disabled = true;
      appendChat('Kamu', msg, '#fff', 'var(--accent)');
      
      fetch('/api/chat', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ message: msg })
      })
      .then(r => r.json())
      .then(res => {
        sendChatBtn.disabled = false;
        if (res.error) {
          appendChat('Error', res.error, '#fff', 'var(--error)');
        } else {
          appendChat('Owi (' + (res.model || res.provider || 'AI') + ')', res.response, '#000', '#f1f1f1');
          if (res.oledSent === false) appendChat('OLED', 'Belum terkirim ke OLED: ' + (res.oledError || 'serial error'), '#fff', 'var(--error)');
          refreshAiLimit();
        }
      })
      .catch(e => {
        sendChatBtn.disabled = false;
        appendChat('Error', 'Gagal memanggil API', '#fff', 'var(--error)');
      });
    };
    
    chatInput.addEventListener('keypress', function (e) {
      if (e.key === 'Enter') sendChatBtn.onclick();
    });

  </script>
</body>
</html>`;
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
    res.end(logs.join("\\n"));
    return;
  }
  if (req.method === "GET" && req.url === "/api/sensors") {
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify(latestTelemetry));
    return;
  }
  if (req.method === "GET" && req.url === "/api/ai-limit") {
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify(getAiLimitStatus()));
    return;
  }
  if (req.method === "POST" && req.url === "/api/chat") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        const userMsg = sanitizeOledText(data.message || "Halo");
        const limitStatus = getAiLimitStatus();
        if (limitStatus.remaining <= 0) {
          res.writeHead(429, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ error: `Limit AI hari ini habis (${limitStatus.used}/${limitStatus.limit}).` }));
          return;
        }

        const aiReply = await askOwi(userMsg);
        let reply = sanitizeOledText(aiReply.text);
        aiUsage.count += 1;
        
        let oledSent = true;
        let oledError = "";
        try {
          await sendChatText(reply);
        } catch (serialErr) {
          oledSent = false;
          oledError = serialErr.message;
          logEvent(`chat oled err: ${oledError}`);
          await closeSerial();
        }
        
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ response: reply, provider: aiReply.provider, model: aiReply.model, oledSent, oledError, ai: getAiLimitStatus() }));
      } catch (err) {
        res.writeHead(500, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: err.message }));
      }
    });
    return;
  }
  if (req.method === "POST" && req.url === "/clear") {
    sendCommand("C").then(() => res.end("Balik ke wajah")).catch((err) => {res.writeHead(500);res.end(err.message);});
    return;
  }
  if (req.method === "POST" && req.url === "/play_audio") {
    let vol = "0.30";
    let file = "lovestory.mp3";
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        if (data.volume) vol = data.volume;
        if (data.file) file = data.file;
      } catch(e) {}
      
      if (!latestTelemetry.ip) {
        res.writeHead(400); res.end("IP belum diketahui");
        return;
      }
      if (isStreamingAudio) {
        res.writeHead(400); res.end("Sedang stream");
        return;
      }
      streamAudio(latestTelemetry.ip, vol, file);
      res.end(`Memutar ${file}`);
    });
    return;
  }
  if (req.method === "POST" && req.url === "/test_max") {
    let vol = "0.35";
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        if (data.volume) vol = data.volume;
      } catch(e) {}

      if (!latestTelemetry.ip) {
        res.writeHead(400); res.end("IP Owi belum kebaca");
        return;
      }
      if (isStreamingAudio) {
        res.writeHead(400); res.end("Masih ada audio berjalan");
        return;
      }
      streamTestTone(latestTelemetry.ip, vol);
      res.end("Test MAX: nada 880Hz dikirim");
    });
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
