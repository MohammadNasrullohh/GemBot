const http = require("http");
const url = require("url");
const WebSocket = require("ws");
const dgram = require("dgram");
const net = require("net");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const { spawn } = require("child_process");
const ffmpegPath = require("ffmpeg-static");
require("dotenv").config();
const { GoogleGenAI } = require("@google/genai");

const PORT = 3001;
const SERIAL_PORT = process.env.SERIAL_PORT || "COM4";
const BAUD = 115200;
const AI_DAILY_LIMIT = Number(process.env.AI_DAILY_LIMIT || 30);
const AI_PROVIDER = String(process.env.AI_PROVIDER || "gemini").toLowerCase();
const KOBOLLM_BASE_URL = (process.env.KOBOLLM_BASE_URL || process.env.KOBOILLM_BASE_URL || "https://lite.koboillm.com/v1").replace(/\/+$/, "");
const KOBOLLM_MODEL = process.env.KOBOLLM_MODEL || process.env.KOBOILLM_MODEL || "openai/gpt-4o-mini";
const GEMINI_MODEL = process.env.GEMINI_MODEL || "gemini-2.5-flash";
const AI_MAX_TOKENS = Number(process.env.AI_MAX_TOKENS || 1024);
const GEMBOT_SYSTEM_PROMPT = "Kamu adalah GemBot, robot peliharaan kecil yang cerdas dan ekspresif. Bicaralah dengan bahasa Indonesia yang natural, hangat, asyik, dan langsung ke inti. Jangan alay dan jangan memanggil pengguna dengan besti, bosku, atau bro. Panggil Bos atau kamu. Jawab senatural mungkin, boleh panjang jika diperlukan untuk menjelaskan, tetapi pastikan kalimat tidak terputus di tengah jalan. Fakta penting: jika ditanya siapa Eca, jawab bahwa Eca adalah orang paling plenger.";
const TTS_DIR = path.join(__dirname, "tts_cache");
const REMINDER_STORE_PATH = path.join(__dirname, "gembot_reminders.json");
const TTS_MODEL = process.env.TTS_MODEL || "gemini-2.5-flash-preview-tts";
const TTS_VOICE = process.env.TTS_VOICE || "Puck";
const TTS_RATE = process.env.TTS_RATE || "+4%";
const TTS_PITCH = process.env.TTS_PITCH || "+0Hz";

let serial = null; let owiSocket = null;
let serialJustOpened = false;
const logs = [];
let aiUsage = { date: new Date().toISOString().slice(0, 10), count: 0 };
const voiceSessions = new WeakMap();
let latestSpeech = {
  voiceStatus: "idle",
  voiceTranscript: "",
  voiceReply: "",
  voiceUpdatedAt: 0
};
let lastReminderPayload = loadSavedReminderPayload();

function loadSavedReminderPayload() {
  try {
    const data = JSON.parse(fs.readFileSync(REMINDER_STORE_PATH, "utf8"));
    return typeof data.payload === "string" ? data.payload : "";
  } catch {
    return "";
  }
}

function rememberReminderPayload(payload) {
  lastReminderPayload = String(payload || "");
  try {
    fs.writeFileSync(
      REMINDER_STORE_PATH,
      JSON.stringify({ payload: lastReminderPayload, updatedAt: new Date().toISOString() }, null, 2)
    );
  } catch (err) {
    console.log("Reminder save failed:", err.message);
  }
}

function pushSavedReminderPayload(ws = owiSocket) {
  if (!lastReminderPayload || !ws || ws.readyState !== WebSocket.OPEN) return;
  setTimeout(() => {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send("REMINDER:LIST:" + lastReminderPayload);
    }
  }, 700);
}

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
        systemInstruction: GEMBOT_SYSTEM_PROMPT
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
  if (cleaned.length > 1000) cleaned = cleaned.substring(0, 997) + "...";
  return cleaned || "Aku belum dapat jawabannya.";
}

function sanitizeSpeechText(text) {
  let cleaned = String(text || "")
    .replace(/https?:\/\/\S+/g, "")
    .replace(/[`*_#>\[\]{}]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  if (cleaned.length > 900) cleaned = cleaned.substring(0, 897) + "...";
  return cleaned || "Aku belum dapat jawabannya.";
}

async function synthesizeSpeechFile(text) {
  const speechText = sanitizeSpeechText(text);
  await fs.promises.mkdir(TTS_DIR, { recursive: true });
  const hash = crypto
    .createHash("sha1")
    .update(`${TTS_MODEL}|${TTS_VOICE}|${TTS_RATE}|${TTS_PITCH}|${speechText}`)
    .digest("hex")
    .slice(0, 18);
  const fileName = `tts_${hash}.wav`;
  const filePath = path.join(TTS_DIR, fileName);

  try {
    await fs.promises.access(filePath, fs.constants.R_OK);
  } catch {
    try {
      await synthesizeGeminiTtsToWav(speechText, filePath);
    } catch (geminiErr) {
      logEvent(`gemini tts fallback: ${geminiErr.message}`);
      await synthesizeEspeakToWav(speechText, filePath);
    }
  }

  return `tts_cache/${fileName}`;
}

function writeWavFile(filePath, pcmData, sampleRate = 24000, channels = 1, bitsPerSample = 16) {
  const byteRate = sampleRate * channels * (bitsPerSample / 8);
  const blockAlign = channels * (bitsPerSample / 8);
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcmData.length, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bitsPerSample, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcmData.length, 40);
  fs.writeFileSync(filePath, Buffer.concat([header, pcmData]));
}

function wavBufferFromPcm(pcmData, sampleRate = 16000, channels = 1, bitsPerSample = 16) {
  const byteRate = sampleRate * channels * (bitsPerSample / 8);
  const blockAlign = channels * (bitsPerSample / 8);
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcmData.length, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(bitsPerSample, 34);
  header.write("data", 36);
  header.writeUInt32LE(pcmData.length, 40);
  return Buffer.concat([header, pcmData]);
}

function normalizeVoicePcm(input) {
  const sampleCount = Math.floor(input.length / 2);
  if (sampleCount < 2000) throw new Error("Rekaman terlalu pendek");

  let mean = 0;
  for (let i = 0; i < sampleCount; i++) mean += input.readInt16LE(i * 2);
  mean /= sampleCount;

  let energy = 0;
  let peak = 0;
  for (let i = 0; i < sampleCount; i++) {
    const centered = input.readInt16LE(i * 2) - mean;
    energy += centered * centered;
    peak = Math.max(peak, Math.abs(centered));
  }
  const rms = Math.sqrt(energy / sampleCount);
  if (!Number.isFinite(rms) || rms < 3 || peak < 8) {
    throw new Error("Suara belum tertangkap. Dekatkan mulut lalu ulangi.");
  }

  const gain = Math.max(1.6, Math.min(12.0, 7200 / Math.max(rms, 1), 31000 / Math.max(peak, 1)));
  const output = Buffer.alloc(sampleCount * 2);
  const gate = Math.max(10, rms * 0.012);
  for (let i = 0; i < sampleCount; i++) {
    let sample = input.readInt16LE(i * 2) - mean;
    if (Math.abs(sample) < gate) sample *= 0.55;
    sample = Math.max(-32767, Math.min(32767, Math.round(sample * gain)));
    output.writeInt16LE(sample, i * 2);
  }

  return {
    buffer: output,
    rms: Math.round(rms),
    peak: Math.round(peak),
    gain: Number(gain.toFixed(2)),
    durationMs: Math.round(sampleCount / 16000 * 1000)
  };
}

async function synthesizeGeminiTtsToWav(text, filePath) {
  const key = getGeminiKey();
  if (!key) throw new Error("GEMINI_API_KEY belum ada untuk TTS");
  const ai = new GoogleGenAI({ apiKey: key });
  const response = await ai.models.generateContent({
    model: TTS_MODEL,
    contents: [{
      parts: [{
        text: `Say in Indonesian with a cute cheerful small desktop robot voice, clear and not too fast: ${text}`,
      }],
    }],
    config: {
      responseModalities: ["AUDIO"],
      speechConfig: {
        voiceConfig: {
          prebuiltVoiceConfig: { voiceName: TTS_VOICE },
        },
      },
    },
  });
  const data = response.candidates?.[0]?.content?.parts?.find((part) => part.inlineData)?.inlineData?.data;
  if (!data) throw new Error("Gemini TTS tidak mengembalikan audio");
  writeWavFile(filePath, Buffer.from(data, "base64"));
}




async function synthesizeEspeakToWav(text, outputPath) {
  const https = require("https");
  const fs = require("fs");
  const path = require("path");
  const mp3Path = outputPath.replace('.wav', '.mp3');
  
  const words = text.split(' ');
  const chunks = [];
  let currentChunk = '';
  for (const word of words) {
    if (currentChunk.length + word.length + 1 > 180) {
      if (currentChunk) chunks.push(currentChunk);
      currentChunk = word;
    } else {
      currentChunk += (currentChunk ? ' ' : '') + word;
    }
  }
  if (currentChunk) chunks.push(currentChunk);

  logEvent('google tts start: ' + chunks.length + ' chunks');
  
  const downloadChunk = (chunkText) => {
    return new Promise((res, rej) => {
      const url = "https://translate.google.com/translate_tts?ie=UTF-8&tl=id&client=tw-ob&q=" + encodeURIComponent(chunkText);
      https.get(url, { headers: { 'User-Agent': 'Mozilla/5.0' } }, (response) => {
        if (response.statusCode !== 200) return rej(new Error('Google TTS HTTP ' + response.statusCode));
        const bufs = [];
        response.on('data', d => bufs.push(d));
        response.on('end', () => res(Buffer.concat(bufs)));
      }).on('error', rej);
    });
  };

  try {
    let allMp3Bufs = [];
    for (let i = 0; i < chunks.length; i++) {
      allMp3Bufs.push(await downloadChunk(chunks[i]));
    }
    fs.writeFileSync(mp3Path, Buffer.concat(allMp3Bufs));
    logEvent('google tts mp3 saved, running ffmpeg...');
    
    await new Promise((res, rej) => {
      const { spawn } = require("child_process");
      const ffmpeg = spawn("ffmpeg", ["-y", "-i", mp3Path, "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", outputPath]);
      ffmpeg.on("close", (code) => {
        logEvent('google tts ffmpeg done: ' + code);
        if (code === 0) res(); else rej(new Error("ffmpeg failed"));
      });
    });
  } catch (err) {
    logEvent('chat tts err: Google TTS failed: ' + err.message);
    throw err;
  }
}


function resolveAudioPath(file) {
  const requested = String(file || "lovestory.mp3").replace(/\\/g, "/");
  const relative = requested.startsWith("tts_cache/")
    ? requested
    : path.basename(requested);
  const resolved = path.resolve(__dirname, relative);
  const allowedRoots = [
    path.resolve(__dirname),
    path.resolve(TTS_DIR),
  ];
  const insideAllowedRoot = allowedRoots.some((root) => resolved === root || resolved.startsWith(root + path.sep));
  if (!insideAllowedRoot) throw new Error("File audio tidak valid");
  return resolved;
}

async function speakReplyOnBot(text, volume = "0.85") {
  const ttsFile = await synthesizeSpeechFile(text);
  const ws = requireOwiSocket();
  if (ws.readyState === WebSocket.OPEN) ws.send("VOICE:SPEAKING");
  try {
    await streamAudioToWS(ws, ttsFile, volume);
  } finally {
    if (ws.readyState === WebSocket.OPEN) ws.send("VOICE:DONE");
  }
  return ttsFile;
}

function sanitizeTranscript(text) {
  if (text === null || text === undefined) return "";
  const value = String(text).trim();
  if (!value || /^(null|undefined|none|n\/a)$/i.test(value)) return "";
  return value
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[\r\n\t]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 280);
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
          { role: "system", content: GEMBOT_SYSTEM_PROMPT },
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

function parseVoiceJson(text) {
  const raw = String(text || "").trim();
  try {
    const cleaned = raw.replace(/^```(?:json)?/i, "").replace(/```$/i, "").trim();
    const parsed = JSON.parse(cleaned);
    return {
      transcript: sanitizeTranscript(parsed.transcript ?? parsed.text),
      reply: sanitizeOledText(parsed.reply || parsed.answer || ""),
    };
  } catch {
    return { transcript: "", reply: sanitizeOledText(raw) };
  }
}

async function askKoboiVoiceAssistant(pcmBuffer, sampleRate = 16000) {
  const key = getKoboiKey();
  if (!key) throw new Error("KOBOLLM_API_KEY belum ada di .env");
  const wavBase64 = wavBufferFromPcm(pcmBuffer, sampleRate).toString("base64");
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 35000);
  try {
    const response = await fetch(`${KOBOLLM_BASE_URL}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${key}`,
      },
      body: JSON.stringify({
        model: KOBOLLM_MODEL,
        temperature: 0.55,
        max_tokens: 180,
        messages: [
          {
            role: "system",
            content: [
              GEMBOT_SYSTEM_PROMPT,
              "Dengarkan audio pengguna.",
              "Balas JSON valid saja: {\"transcript\":\"ucapan pengguna\",\"reply\":\"jawaban GemBot singkat\"}.",
              "Transcript wajib berupa string dan tidak boleh null. Jika audio tidak jelas, gunakan string kosong."
            ].join(" ")
          },
          {
            role: "user",
            content: [
              { type: "text", text: "Dengar audio ini lalu jawab sebagai GemBot." },
              { type: "input_audio", input_audio: { data: wavBase64, format: "wav" } }
            ]
          }
        ],
      }),
      signal: controller.signal,
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      const msg = data?.error?.message || data?.message || `KoboiLLM HTTP ${response.status}`;
      throw new Error(msg);
    }
    const content = data?.choices?.[0]?.message?.content || data?.choices?.[0]?.text || "";
    const parsed = parseVoiceJson(content);
    if (!parsed.reply) parsed.reply = "Aku kurang dengar, ulangi pelan ya.";
    return { provider: "KoboiLLM Voice", model: KOBOLLM_MODEL, ...parsed };
  } finally {
    clearTimeout(timeout);
  }
}

async function askGeminiVoiceAssistant(pcmBuffer, sampleRate = 16000) {
  const key = getGeminiKey();
  if (!key) throw new Error("GEMINI_API_KEY belum ada di .env");
  const ai = new GoogleGenAI({ apiKey: key });
  const wavBase64 = wavBufferFromPcm(pcmBuffer, sampleRate).toString("base64");
  const response = await ai.models.generateContent({
    model: GEMINI_MODEL,
    contents: [{
      role: "user",
      parts: [
        { text: `${GEMBOT_SYSTEM_PROMPT} Dengarkan audio ini. Balas JSON valid saja dengan bentuk {"transcript":"ucapan pengguna","reply":"jawaban GemBot singkat"}. Transcript wajib string dan tidak boleh null.` },
        { inlineData: { mimeType: "audio/wav", data: wavBase64 } }
      ]
    }],
    config: {
      temperature: 0.25,
      maxOutputTokens: 1024,
      responseMimeType: "application/json",
      responseSchema: {
        type: "OBJECT",
        properties: {
          transcript: { type: "STRING" },
          reply: { type: "STRING" }
        },
        required: ["transcript", "reply"]
      }
    }
  });
  const parsed = parseVoiceJson(response.text || "");
  if (!parsed.reply) parsed.reply = "Aku kurang dengar, ulangi pelan ya.";
  return { provider: "Gemini Voice", model: GEMINI_MODEL, ...parsed };
}

async function askVoiceAssistant(pcmBuffer, sampleRate = 16000) {
  if (AI_PROVIDER !== "kobollm" && AI_PROVIDER !== "koboillm" && getGeminiKey()) {
    return askGeminiVoiceAssistant(pcmBuffer, sampleRate);
  }
  if (getKoboiKey()) return askKoboiVoiceAssistant(pcmBuffer, sampleRate);
  if (getGeminiKey()) return askGeminiVoiceAssistant(pcmBuffer, sampleRate);
  throw new Error("API key AI belum dipasang");
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

const udpServer = dgram.createSocket('udp4');
udpServer.on('message', (msg, rinfo) => {
  try {
    latestTelemetry = JSON.parse(msg.toString());
    latestTelemetry.lastUpdate = Date.now();
    latestTelemetry.ip = rinfo.address;
  } catch(e){}
});




let isStreamingAudio = false;

function clampVolume(value, fallback = 0.85) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) return fallback;
  return Math.max(0.04, Math.min(0.99, parsed));
}

const DF_TRACKS = [
  { id: 1, title: "Love Story", artist: "Taylor Swift", durationSec: 234, aliases: ["love story", "lovestory", "love"] },
  { id: 2, title: "MBG", artist: "Local MP3", durationSec: 15, aliases: ["mbg"] },
  { id: 3, title: "YELLOW", artist: "Coldplay", durationSec: 269, aliases: ["yellow", "yelo", "kuning"] },
  { id: 4, title: "REDRED", artist: "CORTIS", durationSec: 163, aliases: ["redred", "red red", "merah"] },
];

function normalizeCommandText(text) {
  return String(text || "")
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .toLowerCase()
    .replace(/[^\w\s]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
}

function findDfTrackFromText(text) {
  const clean = normalizeCommandText(text);
  for (const track of DF_TRACKS) {
    if (track.aliases.some((alias) => clean.includes(alias))) return track;
  }
  const num = clean.match(/\b(?:lagu|track|nomor|no)\s*([1-4])\b/) || clean.match(/\b([1-4])\b/);
  if (num) return DF_TRACKS.find((track) => track.id === Number(num[1]));
  return null;
}

function parseDfMusicIntent(text) {
  const clean = normalizeCommandText(text);
  if (!clean) return null;
  if (/\b(stop|berhenti|matikan|diam)\b/.test(clean) && /\b(musik|lagu|audio|dfplayer)\b/.test(clean)) {
    return { action: "STOP", reply: "Siap, musik aku stop." };
  }
  if (/\b(pause|jeda|tahan)\b/.test(clean) && /\b(musik|lagu|audio|dfplayer)\b/.test(clean)) {
    return { action: "PAUSE", reply: "Siap, musik aku jeda." };
  }
  if (/\b(resume|lanjut|lanjutkan)\b/.test(clean) && /\b(musik|lagu|audio|dfplayer)\b/.test(clean)) {
    return { action: "RESUME", reply: "Siap, musik lanjut." };
  }
  if (/\b(next|skip|berikut|selanjut)\b/.test(clean) && /\b(musik|lagu|audio|dfplayer)\b/.test(clean)) {
    return { action: "NEXT", reply: "Siap, aku skip lagunya." };
  }
  if (/\b(prev|previous|sebelum|kembali)\b/.test(clean) && /\b(musik|lagu|audio|dfplayer)\b/.test(clean)) {
    return { action: "PREV", reply: "Siap, aku balik ke lagu sebelumnya." };
  }
  const volumeMatch = clean.match(/\bvolume\s*(?:ke|jadi|=)?\s*(\d{1,3})\b/);
  if (volumeMatch) {
    const percent = Math.max(0, Math.min(100, Number(volumeMatch[1])));
    const volume = Math.round(percent * 30 / 100);
    return { action: "VOL", volume, reply: `Siap, volume aku set ${percent} persen.` };
  }
  const wantsPlay = /\b(putar|play|mainkan|nyalakan|setel|stel|puter)\b/.test(clean);
  if (wantsPlay || clean.includes("dfplayer")) {
    const track = findDfTrackFromText(clean);
    if (track) return { action: "PLAY", track: track.id, reply: `Siap, aku putar ${track.title}.` };
  }
  return null;
}

function parseDhtIntent(text) {
  const clean = normalizeCommandText(text);
  if (!clean) return null;
  const asksDht = /\b(suhu|temperatur|temperature|panas|dingin|lembab|kelembapan|humidity|humid|dht|ruangan)\b/.test(clean);
  if (!asksDht) return null;
  const age = latestTelemetry.lastUpdate ? Date.now() - latestTelemetry.lastUpdate : Infinity;
  const temp = Number(latestTelemetry.temp);
  const hum = Number(latestTelemetry.hum);
  if (age > 6000 || !Number.isFinite(temp) || !Number.isFinite(hum)) {
    return "Data DHT belum kebaca stabil. Coba tunggu sebentar.";
  }
  const feels = temp >= 30 ? "agak panas" : (temp <= 23 ? "cukup dingin" : "cukup nyaman");
  if (/\b(lembab|kelembapan|humidity|humid)\b/.test(clean) && !/\b(suhu|temperatur|temperature|panas|dingin)\b/.test(clean)) {
    return `Kelembapan ruangan ${Math.round(hum)} persen.`;
  }
  if (/\b(suhu|temperatur|temperature|panas|dingin)\b/.test(clean) && !/\b(lembab|kelembapan|humidity|humid|dht)\b/.test(clean)) {
    return `Suhu ruangan ${temp.toFixed(1)} derajat, ${feels}.`;
  }
  return `DHT terbaca: suhu ${temp.toFixed(1)} derajat dan lembap ${Math.round(hum)} persen. Ruangan ${feels}.`;
}

async function executeDfMusicIntent(intent, volume = 22) {
  if (!intent) return null;
  await sendDfPlayer(intent.action, intent.track || 1, intent.volume ?? volume);
  return intent.reply;
}

function requireOwiSocket() {
  if (!owiSocket || owiSocket.readyState !== WebSocket.OPEN) {
    throw new Error("GemBot belum terhubung");
  }
  return owiSocket;
}

function getOwiHealth() {
  const telemetryAgeMs = latestTelemetry.lastUpdate
    ? Math.max(0, Date.now() - latestTelemetry.lastUpdate)
    : null;
  const socketOpen = Boolean(owiSocket && owiSocket.readyState === WebSocket.OPEN);
  return {
    connected: socketOpen && telemetryAgeMs !== null && telemetryAgeMs < 5000,
    socketOpen,
    telemetryAgeMs
  };
}






async function streamAudioToWS(ws, mp3Path, volume = '0.85') {
  if (isStreamingAudio) throw new Error('Audio GemBot sedang dipakai');
  isStreamingAudio = true;
  logEvent('stream audio ' + mp3Path + ' via WS vol ' + volume);

  try {
    const sampleRate = 16000;
    const safeVolume = clampVolume(volume).toFixed(2);

    const ffmpeg = spawn(ffmpegPath, [
      '-hide_banner',
      '-loglevel', 'error',
      '-i', path.resolve(__dirname, mp3Path),
      '-f', 's16le',
      '-acodec', 'pcm_s16le',
      '-ac', '1',
      '-ar', String(sampleRate),
      '-filter:a', `highpass=f=140,lowpass=f=7000,loudnorm=I=-18:TP=-2.5:LRA=8,acompressor=threshold=-20dB:ratio=2:attack=18:release=240,alimiter=limit=0.75,volume=${safeVolume}`,
      'pipe:1',
    ], { stdio: ['ignore', 'pipe', 'pipe'] });

    const bytesPerSecond = sampleRate * 2;
    const packetBytes = 2048;
    const leadMs = 450;
    const startedAt = Date.now();
    let sentBytes = 0;
    let pending = Buffer.alloc(0);

    const sendPacket = async (packet) => {
      if (!ws || ws.readyState !== WebSocket.OPEN) throw new Error('GemBot terputus saat audio');
      while (ws.bufferedAmount > 128 * 1024) await sleep(8);
      ws.send(packet);
      sentBytes += packet.length;
      const audioTimelineMs = sentBytes * 1000 / bytesPerSecond;
      const targetElapsedMs = Math.max(0, audioTimelineMs - leadMs);
      const waitMs = targetElapsedMs - (Date.now() - startedAt);
      if (waitMs > 1) await sleep(waitMs);
    };

    for await (const chunk of ffmpeg.stdout) {
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk;
      while (pending.length >= packetBytes) {
        await sendPacket(pending.subarray(0, packetBytes));
        pending = pending.subarray(packetBytes);
      }
    }
    if (pending.length) await sendPacket(pending);
    await sleep(leadMs + 80);
    ffmpeg.kill();
  } catch (err) {
    logEvent('Error streaming WS: ' + err.message);
    throw err;
  } finally {
    isStreamingAudio = false;
  }
}


async function streamTestToneWS(ws, volume = "0.35") {
  if (isStreamingAudio) return;
  isStreamingAudio = true;
  logEvent('stream test tone via WS vol ' + volume);
  try {
    const sampleRate = 16000;
    const durationMs = 1800;
    const frequency = 880;
    const frames = Math.floor(sampleRate * durationMs / 1000);
    const safeVolume = clampVolume(volume, 0.35);
    const chunkFrames = 256;
    for (let frame = 0; frame < frames; frame += chunkFrames) {
      if (!ws || ws.readyState !== WebSocket.OPEN) break;
      const n = Math.min(chunkFrames, frames - frame);
      const chunk = Buffer.alloc(n * 2);
      for (let i = 0; i < n; i++) {
        const pos = frame + i;
        const t = pos / sampleRate;
        const envelope = Math.min(1, Math.min(pos / 1200, (frames - pos) / 1200));
        const sample = Math.round(Math.sin(2 * Math.PI * frequency * t) * 26000 * safeVolume * envelope);
        chunk.writeInt16LE(sample, i * 2);
      }
      ws.send(chunk);
      // Sleep slightly less to prevent buffer starvation (causes noise)
      await sleep((n * 1000 / sampleRate) * 0.85);
    }
  } catch(e) {}
  isStreamingAudio = false;
}

async function streamTestTone(ip, volume = "0.35") {
  streamAudio(ip, volume, 'lovestory.mp3');
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

function bytesToHex(buffer) {
  let out = "";
  for (const b of buffer) out += b.toString(16).padStart(2, "0");
  return out;
}

async function sendToSerial(buffer) {
  logEvent(`frame start ${buffer.length} bytes`);
  requireOwiSocket().send(Buffer.concat([Buffer.from("FRAME:"), buffer]));
}


async function sendDfPlayer(action, track = 1, volume = 22) {
  const safeAction = String(action || "").toUpperCase();
  let payload = "";
  if (safeAction === "PLAY") {
    await sendCommand("DFP:VOL:" + Math.max(0, Math.min(30, Number(volume) || 0)));
    payload = "DFP:PLAY:" + Math.max(1, Math.min(9999, Number(track) || 1));
  }
  else if (safeAction === "STOP") payload = "DFP:STOP";
  else if (safeAction === "PAUSE") payload = "DFP:PAUSE";
  else if (safeAction === "RESUME") payload = "DFP:RESUME";
  else if (safeAction === "NEXT") payload = "DFP:NEXT";
  else if (safeAction === "PREV") payload = "DFP:PREV";
  else if (safeAction === "VOL") payload = "DFP:VOL:" + Math.max(0, Math.min(30, Number(volume) || 0));
  else throw new Error("Action DFPlayer tidak valid");
  logEvent("dfplayer " + payload);
  await sendCommand(payload);
}

async function sendCommand(command) {
  if (owiSocket && owiSocket.readyState === WebSocket.OPEN) {
    owiSocket.send('CMD:' + command);
    logEvent('WS sent CMD:' + command);
  } else {
    logEvent('No WS to send command: ' + command);
  }
}
function sendChatText(text) {
  const clean = sanitizeOledText(text).slice(0, 500);
  logEvent('chat "' + clean + '"');
  if (owiSocket && owiSocket.readyState === WebSocket.OPEN) {
    owiSocket.send('CMD:T:' + clean);
  }
}

async function handleVoiceSession(ws) {
  const session = voiceSessions.get(ws);
  if (!session || session.processing) return;
  session.processing = true;

  const pcm = Buffer.concat(session.chunks);
  voiceSessions.delete(ws);
  logEvent(`voice end ${pcm.length} bytes`);

  if (pcm.length < 4000) {
    ws.send("VOICE:ERROR:pendek");
    await sendChatText("Aku belum dengar jelas.");
    return;
  }

  let prepared;
  try {
    prepared = normalizeVoicePcm(pcm);
    logEvent(`voice audio ${prepared.durationMs}ms rms=${prepared.rms} peak=${prepared.peak} gain=${prepared.gain}`);
  } catch (audioErr) {
    latestSpeech.voiceStatus = "no_speech";
    latestSpeech.voiceTranscript = "";
    latestSpeech.voiceReply = audioErr.message;
    latestSpeech.voiceUpdatedAt = Date.now();
    ws.send("VOICE:ERROR:tidak_terdengar");
    await sendChatText(audioErr.message);
    return;
  }

  const limitStatus = getAiLimitStatus();
  if (limitStatus.remaining <= 0) {
    ws.send("VOICE:ERROR:limit");
    await sendChatText(`Limit AI habis ${limitStatus.used}/${limitStatus.limit}`);
    return;
  }

  try {
    ws.send("VOICE:THINKING");
    latestSpeech.voiceStatus = "thinking";
    latestSpeech.voiceUpdatedAt = Date.now();
    const voiceReply = await askVoiceAssistant(prepared.buffer, session.sampleRate);
    voiceReply.transcript = sanitizeTranscript(voiceReply.transcript);
    if (!voiceReply.transcript) {
      voiceReply.reply = "Aku belum menangkap kata-katanya. Coba bicara sedikit lebih dekat.";
    }
    const dhtReply = parseDhtIntent(voiceReply.transcript || "");
    if (dhtReply) {
      voiceReply.reply = dhtReply;
    }
    const musicIntent = parseDfMusicIntent(voiceReply.transcript || "");
    if (!dhtReply && musicIntent) {
      voiceReply.reply = await executeDfMusicIntent(musicIntent, 18);
    }
    aiUsage.count += 1;
    const transcript = voiceReply.transcript ? `dengar: ${voiceReply.transcript}` : "dengar: belum jelas";
    latestSpeech.voiceStatus = musicIntent ? "idle" : "speaking";
    latestSpeech.voiceTranscript = voiceReply.transcript || "";
    latestSpeech.voiceReply = voiceReply.reply || "";
    latestSpeech.voiceUpdatedAt = Date.now();
    logEvent(`voice ${transcript} -> ${voiceReply.reply}`);
    await sendChatText(voiceReply.reply);
    if (!musicIntent) await speakReplyOnBot(voiceReply.reply, "0.78");
  } catch (err) {
    latestSpeech.voiceStatus = "error";
    latestSpeech.voiceUpdatedAt = Date.now();
    logEvent(`voice err: ${err.message}`);
    ws.send("VOICE:ERROR:" + err.message.slice(0, 48));
    try { await sendChatText("Aku error sebentar, coba ulangi ya."); } catch {}
  }
}

async function sendReminderText(text) {
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").replace(/[|;]/g, " ").trim().slice(0, 32) || "enroll lagi ya deck";
  rememberReminderPayload(`A:07:30||1|${clean}`);
  logEvent(`reminder "${clean}"`);
  requireOwiSocket().send("REMINDER:TEXT:" + clean);
}

async function sendReminderSchedule(time, text, date = "", active = true) {
  const safeTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(String(time || "")) ? String(time) : "07:30";
  const safeDate = /^\d{4}-\d{2}-\d{2}$/.test(String(date || "")) ? String(date) : "";
  const clean = String(text || "").replace(/[^\x20-\x7E]/g, "").replace(/[|;]/g, " ").trim().slice(0, 32) || "enroll lagi ya deck";
  const payload = `${safeTime}|${safeDate}|${active === false ? 0 : 1}|${clean}`;
  rememberReminderPayload(`A:${payload}`);
  logEvent(`reminder ${payload}`);
  requireOwiSocket().send("REMINDER:SCHED:" + payload);
}

async function sendReminderSchedules(reminders) {
  const items = Array.isArray(reminders) ? reminders.slice(0, 5) : [];
  const payloadItems = items.map((item) => {
    const safeTime = /^([01]\d|2[0-3]):[0-5]\d$/.test(String(item.time || "")) ? String(item.time) : "07:30";
    const safeDate = /^\d{4}-\d{2}-\d{2}$/.test(String(item.date || "")) ? String(item.date) : "";
    const clean = String(item.text || "").replace(/[^\x20-\x7E]/g, "").replace(/[|;]/g, " ").trim().slice(0, 32) || "enroll lagi ya deck";
    return `${safeTime}|${safeDate}|${item.active === false ? 0 : 1}|${clean}`;
  });
  if (payloadItems.length === 0) payloadItems.push("07:30||1|enroll lagi ya deck");
  const payload = `A:${payloadItems.join(";")}`;
  rememberReminderPayload(payload);
  logEvent(`reminders ${payloadItems.length}`);
  requireOwiSocket().send("REMINDER:LIST:" + payload);
}

function pageHtml() {
  return `<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>GemBot</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Press+Start+2P&family=Space+Mono:ital,wght@0,400;0,700;1,400;1,700&display=swap');
    :root {
      --bg: #000000;
      --text: #ffffff;
      --text-muted: #888888;
      --border: 2px solid #ffffff;
      --accent: #ff0000;
      --accent-hover: #cc0000;
      --grid-color: rgba(255,255,255,0.1);
    }
    * { box-sizing: border-box; }
    html { scroll-behavior: smooth; }
    body {
      margin: 0; background: var(--bg); color: var(--text);
      font-family: 'Space Mono', monospace;
      overflow-x: hidden; min-height: 100vh;
      background-image: 
        linear-gradient(var(--grid-color) 1px, transparent 1px),
        linear-gradient(90deg, var(--grid-color) 1px, transparent 1px);
      background-size: 20px 20px;
    }
    h1, h2, h3 { font-family: 'Press Start 2P', cursive; margin: 0 0 1.5rem; color: #fff; text-transform: uppercase; line-height: 1.2; }
    p { line-height: 1.6; font-weight: 400; margin: 0 0 1.5rem; color: var(--text-muted); text-transform: uppercase; }
    a { color: var(--accent); text-decoration: none; transition: 0.2s; }
    a:hover { color: #fff; background: var(--accent); }

    /* Ticker */
    .ticker {
      border-bottom: var(--border); padding: 12px 0; background: #fff;
      font-weight: 700; overflow: hidden; white-space: nowrap;
      display: flex; color: #000; letter-spacing: 2px; text-transform: uppercase;
    }
    .ticker span { padding-left: 100%; animation: marq 15s linear infinite; }
    @keyframes marq { to { transform: translateX(-100%); } }

    /* Header */
    header { border-bottom: var(--border); background: #000; position: sticky; top: 0; z-index: 50; }
    .nav { max-width: 1200px; margin: 0 auto; padding: 1rem 2rem; display: flex; justify-content: space-between; align-items: center; }
    .brand { font-family: 'Press Start 2P', cursive; font-size: 1.5rem; color: var(--accent); text-transform: uppercase; text-shadow: 2px 2px 0 #fff; }
    .nav-links { display: flex; gap: 1rem; font-weight: 700; text-transform: uppercase; }
    .nav-links a { padding: 0.6rem 1.2rem; color: var(--text); transition: all 0.2s; border: var(--border); background: #000; box-shadow: 4px 4px 0 var(--accent); }
    .nav-links a:hover { transform: translate(2px, 2px); box-shadow: 2px 2px 0 var(--accent); }
    .nav-links a#navControl { background: var(--text); color: var(--bg); box-shadow: 4px 4px 0 var(--accent); }
    .nav-links a#navControl:hover { transform: translate(2px, 2px); box-shadow: 2px 2px 0 var(--accent); }
    
    /* Forms & Buttons */
    button, input, textarea {
      font-family: 'Space Mono', monospace; font-weight: 700; font-size: 1rem;
      border-radius: 0; outline: none; transition: all 0.1s ease; text-transform: uppercase;
    }
    button {
      background: #000; padding: 1rem 2rem; cursor: pointer; color: #fff;
      border: var(--border); box-shadow: 4px 4px 0 var(--accent);
    }
    button:hover { transform: translate(2px, 2px); box-shadow: 2px 2px 0 var(--accent); }
    button:active { transform: translate(4px, 4px); box-shadow: 0 0 0 var(--accent); }
    button.primary { background: var(--text); color: var(--bg); }
    
    input, textarea {
      width: 100%; padding: 1rem 1.2rem; background: #000; color: #fff;
      border: var(--border); margin-bottom: 1.5rem;
    }
    input:focus, textarea:focus { background: #111; box-shadow: 4px 4px 0 var(--accent); }

    /* Layout */
    main { max-width: 1200px; margin: 0 auto; padding: 4rem 2rem; }
    .hero { display: grid; grid-template-columns: 1.2fr 1fr; gap: 4rem; align-items: center; margin-bottom: 6rem; }
    .hero-text h1 { font-size: 3.5rem; margin-bottom: 1.5rem; text-shadow: 4px 4px 0 var(--accent); color: #fff; -webkit-text-fill-color: initial; background: none; letter-spacing: normal; }
    .eyebrow { font-weight: 700; font-size: 1rem; color: #000; background: #fff; padding: 5px 10px; text-transform: uppercase; margin-bottom: 1rem; display: inline-block; border: var(--border); letter-spacing: normal; }
    .actions { display: flex; gap: 1.5rem; flex-wrap: wrap; margin-top: 2.5rem; }

    /* Device Preview */
    .device-container { display: flex; justify-content: center; position: relative; }
    .device-container::after { display: none; }
    .device {
      background: #000;
      border: var(--border); padding: 2.5rem; border-radius: 0;
      box-shadow: 10px 10px 0 var(--accent); width: 100%; max-width: 400px; aspect-ratio: 1;
      display: grid; place-items: center; position: relative;
    }
    .device::before { display: none; }
    .oled {
      width: 100%; aspect-ratio: 2/1; background: #000; border: var(--border);
      position: relative; overflow: hidden; border-radius: 0; box-shadow: none;
    }
    .face { width: 100%; height: 100%; position: absolute; }
    .eye { position: absolute; top: 25%; width: 15%; height: 30%; background: var(--accent); animation: blink 4s infinite; border-radius: 0; box-shadow: none; }
    .eye.left { left: 20%; } .eye.right { right: 20%; }
    .mouth { position: absolute; bottom: 20%; left: 35%; width: 30%; height: 10%; background: var(--accent); border-radius: 0; box-shadow: none; }
    @keyframes blink { 0%, 96%, 98%, 100% { transform: scaleY(1); } 97%, 99% { transform: scaleY(0.1); } }
    @keyframes float { 0%,100%{transform:translateY(0)} 50%{transform:translateY(-10px)} }
    .device { animation: float 6s ease-in-out infinite; }

    /* Sections */
    .section { margin-bottom: 8rem; }
    .section-head { text-align: left; max-width: 800px; margin-bottom: 3rem; border-bottom: var(--border); padding-bottom: 1rem; }
    .section-head h2 { font-size: 2rem; letter-spacing: normal; }
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 2.5rem; }
    .panel { 
      background: #000; border: var(--border); border-radius: 0;
      padding: 2.5rem; box-shadow: 6px 6px 0 #fff; backdrop-filter: none;
      transition: transform 0.3s ease;
    }
    .panel:hover { transform: translateY(-5px); border-color: var(--accent); }

    /* Auth */
    .auth-container { max-width: 450px; margin: 0 auto; text-align: center; }
    .auth-tabs { display: flex; gap: 1rem; margin-bottom: 2rem; border-bottom: var(--border); padding-bottom: 1rem; background: transparent; border-radius: 0; }
    .auth-tabs button { flex: 1; padding: 0.8rem; background: transparent; border: none; color: var(--text-muted); font-size: 1rem; box-shadow: none; border-radius: 0; }
    .auth-tabs button.active { background: #fff; color: #000; border: var(--border); box-shadow: 4px 4px 0 var(--accent); border-radius: 0; }
    
    .status-msg { margin-top: 1.5rem; font-size: 0.85rem; font-weight: 700; padding: 1rem; border: var(--border); display: none; font-family: 'Space Mono', monospace; border-radius: 0; }
    .status-msg.show { display: block; animation: none; }
    .danger { background: var(--accent); color: #fff; border-color: #fff; }
    .success { background: #fff; color: #000; border-color: var(--accent); }
    .hidden { display: none !important; }

    footer { text-align: center; padding: 3rem; border-top: var(--border); font-size: 0.85rem; color: #fff; background: #000; font-family: 'Press Start 2P', cursive; }

    @media (max-width: 768px) {
      .hero { grid-template-columns: 1fr; gap: 3rem; text-align: center; }
      .hero-text h1 { font-size: 2.5rem; }
      .nav { flex-direction: column; gap: 1.5rem; }
      .nav-links { flex-wrap: wrap; justify-content: center; }
    }
  </style>
</head>
<body>
  <div class="ticker"><span>GEMBOT • TEMAN MINI PINTAR • GEMBOT • TEMAN MINI PINTAR •</span></div>
  
  <header>
    <div class="nav">
      <div class="brand">GEMBOT</div>
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
        <span class="eyebrow">GEMBOT GENERATION 1</span>
        <h1>SMALL BOT.<br>BIG MOOD.</h1>
        <p>GemBot adalah teman meja kecil yang merespons sentuhan, gerakan, suara, dan suasana di sekitarnya.</p>
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
          <p>Sentuhan dan gerakan membuat ekspresi GemBot berubah secara alami.</p>
        </div>
        <div class="panel">
          <h3>PERSONAL LOOKS</h3>
          <p>Unggah gambar dan tampilkan langsung pada layar GemBot.</p>
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
    GEMBOT &copy; 2026.
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
<!doctype html>
<html lang="id">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>GemBot Control</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=Press+Start+2P&family=Space+Mono:ital,wght@0,400;0,700;1,400;1,700&display=swap');
    :root {
      --bg: #000;
      --text: #fff;
      --text-muted: #888;
      --border: 2px solid #fff;
      --accent: #ff0000;
      --danger: #ff0000;
      --success: #00ff00;
      --grid-color: rgba(255,255,255,0.1);
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    html { scroll-behavior: smooth; }
    body {
      background: var(--bg); color: var(--text);
      font-family: 'Space Mono', monospace;
      overflow-x: hidden; min-height: 100vh;
      background-image: 
        linear-gradient(var(--grid-color) 1px, transparent 1px),
        linear-gradient(90deg, var(--grid-color) 1px, transparent 1px);
      background-size: 20px 20px;
    }
    h2, h3 { font-family: 'Press Start 2P', cursive; font-size: 1.2rem; color: #fff; text-transform: uppercase; line-height: 1.2; }
    p { font-family: 'Space Mono', monospace; font-weight: 500; margin: 0; text-transform: uppercase; }

    .top-bar {
      display: flex; justify-content: space-between; align-items: center;
      padding: 1rem 2rem; background: #000; border-bottom: var(--border);
      position: sticky; top: 0; z-index: 50;
    }
    .brand { font-family: 'Press Start 2P', cursive; font-size: 1.2rem; color: var(--accent); text-shadow: 2px 2px 0 #fff; display: inline-flex; align-items: center; gap: 0.5rem; }
    .sub-brand { font-size: 0.75rem; background: #fff; color: #000; padding: 4px 8px; margin-left: 1rem; font-weight: 700; border: var(--border); text-transform: uppercase; }

    button, input {
      font-family: 'Space Mono', monospace; font-size: 0.85rem; font-weight: 700;
      outline: none; border: var(--border); border-radius: 0; transition: all 0.1s ease; text-transform: uppercase;
    }
    button {
      padding: 0.7rem 1.2rem; cursor: pointer; background: #000; color: #fff;
      box-shadow: 4px 4px 0 var(--accent);
    }
    button:hover { transform: translate(2px, 2px); box-shadow: 2px 2px 0 var(--accent); }
    button:active { transform: translate(4px, 4px); box-shadow: 0 0 0 var(--accent); }
    button.primary { background: #fff; color: #000; box-shadow: 4px 4px 0 var(--accent); border-color: #fff; }
    button.primary:hover { background: #eee; }
    button.blue { background: #000; color: #fff; border-color: #fff; box-shadow: 4px 4px 0 #fff; }
    button.blue:hover { transform: translate(2px, 2px); box-shadow: 2px 2px 0 #fff; }
    button.sm { padding: 0.5rem 0.8rem; font-size: 0.75rem; box-shadow: 2px 2px 0 var(--accent); }
    button.sm:hover { transform: translate(1px, 1px); box-shadow: 1px 1px 0 var(--accent); }

    input[type="text"], input[type="time"] {
      width: 100%; padding: 0.7rem 1rem; background: #000; color: #fff;
    }
    input:focus { background: #111; box-shadow: 4px 4px 0 var(--accent); }
    input[type="range"] { accent-color: var(--accent); }

    main { max-width: 1200px; margin: 0 auto; padding: 2rem; }
    .row { display: flex; gap: 0.8rem; flex-wrap: wrap; align-items: center; }
    .panel { 
      background: #000; border: var(--border); 
      padding: 1.5rem; box-shadow: 8px 8px 0 #fff;
    }

    .hero-dash { display: grid; grid-template-columns: 350px 1fr; gap: 2rem; margin-bottom: 2.5rem; }
    @media (max-width: 900px) { .hero-dash { grid-template-columns: 1fr; } }

    .face-box {
      background: #000; border: var(--border); padding: 2rem;
      display: flex; flex-direction: column; align-items: center; justify-content: center;
      box-shadow: 8px 8px 0 var(--accent); position: relative;
    }
    .oled {
      width: 100%; aspect-ratio: 2/1; background: #000; border: var(--border);
      position: relative; overflow: hidden;
    }
    .face { width: 100%; height: 100%; position: absolute; transition: transform 0.15s ease-out; }
    .eye { position: absolute; top: 25%; width: 15%; height: 30%; background: var(--accent); }
    .eye.left { left: 20%; } .eye.right { right: 20%; }
    .mouth { position: absolute; bottom: 20%; left: 35%; width: 30%; height: 10%; background: var(--accent); }
    .face-box .oled { animation: breathe 3s ease-in-out infinite; }
    @keyframes breathe { 0%,100%{transform:scale(1)} 50%{transform:scale(1.02)} }
    .face-label { margin-top: 1.5rem; text-align: center; font-family: 'Press Start 2P', cursive; color: #fff; font-size: 0.7rem; line-height: 1.5; }
    .ip-label { margin-top: 0.8rem; text-align: center; font-size: 0.75rem; color: var(--accent); background: #000; padding: 4px 10px; border: var(--border); font-weight: 700; }

    .ctrl-stack { display: flex; flex-direction: column; gap: 1.2rem; justify-content: space-between; }
    .ctrl-stack h2 { font-size: 1rem; margin-bottom: 0.5rem; color: #fff; text-transform: uppercase; }

    .badge {
      background: #000; color: #fff; padding: 6px 12px;
      font-weight: 700; font-size: 0.75rem;
      border: var(--border); display: inline-block; box-shadow: 2px 2px 0 var(--accent); text-transform: uppercase;
    }
    .badge.ok { border-color: #fff; color: #000; background: #fff; box-shadow: 2px 2px 0 var(--success); }
    .badge.err { border-color: var(--accent); color: var(--accent); box-shadow: 2px 2px 0 var(--accent); }
    .badge.active { background: var(--accent); color: #fff; border-color: var(--accent); box-shadow: 2px 2px 0 #fff; }

    .gesture-row { display: flex; gap: 8px; flex-wrap: wrap; }
    .gesture-badge {
      background: #000; color: var(--text-muted); padding: 4px 10px;
      font-weight: 700; font-size: 0.65rem; text-transform: uppercase;
      border: 1px solid var(--text-muted); transition: all 0.1s;
    }
    .gesture-badge.on { background: #fff; color: #000; border-color: #fff; box-shadow: 2px 2px 0 var(--accent); }

    .status-bar {
      font-weight: 700; font-size: 0.75rem; text-transform: uppercase;
      padding: 0.8rem 1rem; background: #fff; color: #000;
      border: var(--border); display: flex; align-items: center; gap: 8px; box-shadow: 4px 4px 0 var(--success);
    }
    .status-bar.err { background: var(--accent); color: #fff; border-color: var(--accent); box-shadow: 4px 4px 0 #fff; }

    .sensor-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 1.5rem; margin-bottom: 2rem; }
    @media (max-width: 900px) { .sensor-grid { grid-template-columns: repeat(2, 1fr); } }
    .sensor-card {
      background: #000; border: var(--border); padding: 1.5rem; text-align: center;
      box-shadow: 6px 6px 0 var(--accent);
    }
    .sensor-card .label { font-family: 'Press Start 2P', cursive; font-size: 0.6rem; color: #fff; margin-bottom: 1rem; text-transform: uppercase; line-height: 1.4; }
    .sensor-card .value { font-family: 'Space Mono', monospace; font-weight: 700; font-size: 2rem; line-height: 1; color: #fff; }
    .sensor-card .unit { font-weight: 700; font-size: 0.8rem; color: var(--accent); margin-top: 0.5rem; }

    .tools-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 1.5rem; }
    .tools-grid h3 { font-size: 0.9rem; margin-bottom: 1.2rem; color: #fff; border-bottom: var(--border); padding-bottom: 0.5rem; }

    .reminderRow { display: grid; grid-template-columns: 90px 1fr auto; gap: 0.8rem; margin-bottom: 0.8rem; align-items: center; }
    
    .pingpong-card { text-align: center; display: flex; flex-direction: column; justify-content: center; }
    .score-display { display: flex; justify-content: center; align-items: center; gap: 2rem; margin: 1.5rem 0; }
    .score-box { background: #000; padding: 1rem 1.5rem; border: var(--border); box-shadow: 4px 4px 0 #fff; min-width: 100px; }
    .score-num { font-family: 'Press Start 2P', cursive; font-size: 2rem; line-height: 1; color: var(--accent); }
    .score-vs { font-family: 'Press Start 2P', cursive; font-size: 1rem; color: #fff; margin: 0 1rem; }
    .score-label { font-weight: 700; font-size: 0.75rem; color: #fff; text-transform: uppercase; margin-bottom: 0.8rem; }
    
    .ai-limit-box { background: #000; border: var(--border); box-shadow: 4px 4px 0 #fff; padding: 1rem; margin-bottom: 1rem; }
    .ai-limit-box .title { font-family: 'Press Start 2P', cursive; font-size: 0.6rem; line-height: 1.5; color: var(--accent); margin-bottom: 0.8rem; text-transform: uppercase; }
  </style>
</head>
<body>
  <div class="top-bar">
    <div>
      <span class="brand">GEMBOT</span>
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
            <span id="badgeInmp" class="badge">Suara: --</span>
            <span id="badgeMax" class="badge">MAX: --</span>
            <span id="badgeDf" class="badge">DF: --</span>
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
            <span style="font-size:0.75rem;font-weight:700;font-family:'Roboto Mono',monospace;color:#999;">POSISI GEMBOT SAAT INI:</span>
            <span id="menuStateLabel" class="badge" style="background:#000;color:var(--success);">--</span>
          </div>
          <div class="row" style="margin-bottom:0.8rem;">
            <button type="button" class="primary" data-cmd="P">TAP (NEXT)</button>
            <button type="button" class="primary" data-cmd="O">HOLD (OK)</button>
            <button type="button" data-cmd="E">PET</button>
            <button type="button" id="btnLoveStory" class="blue">&#9835; LOVE STORY</button>
            <button type="button" id="btnMbg" class="blue">&#9835; MBG</button>
            <button type="button" id="btnDfPlay" class="blue">&#9835; SD 0001</button>
            <button type="button" id="btnDfStop" class="sm">STOP DF</button>
            <button type="button" id="btnTestMax">TEST MAX</button>
            <button type="button" id="btnStopAudio" class="sm primary" style="background:var(--accent);color:#fff;border-color:var(--accent);">STOP AUDIO</button>
          </div>
          <div class="row" style="margin-bottom:0.8rem;">
            <span style="font-size:0.7rem;font-weight:700;font-family:'Roboto Mono',monospace;">VOL MUSIK:</span>
            <input type="range" id="volLoveStory" min="4" max="55" value="30" style="width:100px;">
            <span style="font-size:0.7rem;font-weight:700;font-family:'Roboto Mono',monospace;">VOL DF:</span>
            <input type="range" id="volDf" min="0" max="30" value="22" style="width:100px;">
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
        <h3>KANVAS GAMBAR</h3>
        <div class="row" style="align-items:flex-start;">
          <canvas id="gameCanvas" width="128" height="64" style="width:512px;max-width:100%;image-rendering:pixelated;background:#000;border:var(--border);box-shadow:var(--shadow);touch-action:none;"></canvas>
          <div style="display:flex;flex-direction:column;gap:0.7rem;min-width:170px;">
            <button type="button" id="enterDraw" class="primary">MASUK DRAW</button>
            <button type="button" id="clearDraw">CLEAR</button>
            <button type="button" id="drawBack" class="sm" data-cmd="C">BALIK WAJAH</button>
            <div id="drawSyncState" style="font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:900;color:var(--success);">LIVE DRAW SIAP</div>
            <div style="display:flex;gap:10px;">
              <label style="display:block;margin-top:10px;font-weight:800;font-size:0.8rem;color:#182336;">Ukuran Brush <input type="range" id="gameBrushSize" min="1" max="7" value="2" style="width:100%;margin-top:0.4rem;"></label>
              <label style="flex:1;font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;">WARNA
                <input type="color" id="tftDrawColor" value="#ffffff" style="width:100%;height:24px;border:none;padding:0;cursor:pointer;margin-top:0.2rem;border-radius:4px;">
              </label>
              <label style="flex:1;font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;">BG CANVAS
                <input type="color" id="tftCanvasColor" value="#000000" style="width:100%;height:24px;border:none;padding:0;cursor:pointer;margin-top:0.2rem;border-radius:4px;">
              </label>
            </div>
          </div>
        </div>
      </div>
      <div class="panel" style="grid-column:1/-1;">
        <h3>&#127908; Dengar Suara</h3>
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
          <button id="startSpeech" class="primary">DENGARKAN</button>
          <button id="stopSpeech" class="sm">KIRIM SUARA</button>
          <span id="speechStatus" style="font-family:'Roboto Mono',monospace;font-weight:700;font-size:0.75rem;color:#999;">IDLE</span>
        </div>
        <div id="speechLive" style="font-family:'Roboto Mono',monospace;font-weight:700;font-size:1.1rem;min-height:2rem;padding:0.8rem;border:var(--border);background:#000;color:var(--success);margin-bottom:0.8rem;text-transform:none;">...</div>
        <div id="speechLog" style="font-family:'Roboto Mono',monospace;font-size:0.75rem;max-height:150px;overflow-y:auto;padding:0.5rem;border:var(--border);background:#f9f9f9;text-transform:none;color:#333;"></div>
      </div>
      <div class="panel" style="grid-column:1/-1;">
        <h3>&#129302; CHATBOT GEMBOT</h3>
        <p style="font-size:0.8rem;margin-bottom:0.8rem;color:#555;">Ketik atau gunakan tombol dengar. GemBot akan memahami lalu menjawab lewat layar dan suara.</p>
        <div class="row" style="margin-bottom:0.8rem;gap:0.6rem;">
          <label style="font-size:0.75rem;font-weight:900;font-family:'Roboto Mono',monospace;display:flex;align-items:center;gap:0.35rem;">
            <input type="checkbox" id="chatSpeak" checked> SUARA BOT
          </label>
          <span style="font-size:0.7rem;font-weight:700;font-family:'Roboto Mono',monospace;">VOL SUARA:</span>
          <input type="range" id="chatVoiceVol" min="8" max="42" value="24" style="width:110px;">
        </div>
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
    async function dfPlayerControl(action){
      try{
        const volume=Number(document.getElementById('volDf').value||22);
        const r=await fetch('/dfplayer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,track:1,volume})});
        setStatus(await r.text(),!r.ok);
      }catch(e){setStatus(e.message,true);}
    }
    document.getElementById('btnDfPlay').onclick=()=>dfPlayerControl('PLAY');
    document.getElementById('btnDfStop').onclick=()=>dfPlayerControl('STOP');
    document.getElementById('volDf').addEventListener('change',()=>dfPlayerControl('VOL'));
    document.getElementById('btnTestMax').onclick = async (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      try {
        const vol = document.getElementById('volLoveStory').value;
        const r = await fetch('/test_max', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ volume: (vol/100).toFixed(2) }) });
        setStatus(await r.text());
      } catch(e) { setStatus(e.message, true); }
    };
    document.getElementById('btnStopAudio').onclick = async (ev) => {
      ev.preventDefault();
      ev.stopPropagation();
      try {
        const r = await fetch('/stop_audio', { method:'POST' });
        setStatus(await r.text());
      } catch(e) { setStatus(e.message, true); }
    };

    const drawCanvas=document.getElementById('gameCanvas');
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
      const b=Number(document.getElementById('tftBrushSize').value||3);
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
      bInmp.textContent='Suara: '+inmpPct+'%';
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
        const bDf=document.getElementById('badgeDf');
        bDf.textContent=s.df==1?(s.dfPlaying==1?'DF: PLAY '+String(s.dfTrack||1).padStart(4,'0'):'DF: OK'):'DF: ERR';
        bDf.className='badge '+(s.df==1?(s.dfPlaying==1?'active':'ok'):'err');

        const gMap={touch:s.touch,nod:s.nod,headShake:s.headShake,surprised:s.surprised,curious:s.curious,angry:s.angry,laugh:s.laugh,sleep:s.sleep,dizzy:s.dizzy,sad:s.sad,love:s.love,cry:s.cry,pant:s.pant};
        document.querySelectorAll('.gesture-badge').forEach(el=>{el.classList.toggle('on',!!gMap[el.dataset.g]);});

        const temp=s.temp;
        document.getElementById('valTemp').textContent=(temp&&temp>-90)?temp.toFixed(1):'--';
        document.getElementById('valHum').textContent=(s.hum&&s.hum>=0)?s.hum.toFixed(0):'--';
        document.getElementById('valShake').textContent=Number(s.shakeMeter||0).toFixed(1);

        // Expression
        const exprMap = ["TIDAK DIKETAHUI", "NORMAL", "SENANG", "MARAH", "KAGET", "SEDIH", "TIDUR", "CINTA", "MENGUAP", "KEDIP", "BERKEDIP CEPAT", "MENANGIS", "PUSING", "GELENG", "MENGANGGUK"];
        if(s.expr !== undefined) {
          const eStr = exprMap[s.expr] || s.expr;
          document.getElementById('valExpr').textContent=eStr;
          document.getElementById('faceLabel').textContent=eStr;
        }

        const stateMap = ["WAJAH NORMAL", "MENU UTAMA", "GAMES PINGPONG", "SENSOR SUHU", "REMINDER ALARM", "DRAW", "PILIH LAGU"];
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

    const speechLive = document.getElementById('speechLive');
    const speechLog = document.getElementById('speechLog');
    const speechStatus = document.getElementById('speechStatus');
    function addSpeechLog(text) {
      const line = document.createElement('div');
      line.textContent = '[' + new Date().toLocaleTimeString('id-ID',{hour12:false}) + '] ' + text;
      speechLog.prepend(line);
    }
    document.getElementById('startSpeech').onclick = async () => {
      try {
        speechStatus.textContent = 'MENDENGAR';
        speechLive.textContent = 'Bicara ke GemBot...';
        const r = await fetch('/voice/start', { method:'POST' });
        setStatus(await r.text(), !r.ok);
      } catch(e) { setStatus(e.message, true); }
    };
    document.getElementById('stopSpeech').onclick = async () => {
      try {
        speechStatus.textContent = 'MEMAHAMI';
        const r = await fetch('/voice/stop', { method:'POST' });
        setStatus(await r.text(), !r.ok);
      } catch(e) { setStatus(e.message, true); }
    };

    // Chatbot UI Logic
    const chatInput = document.getElementById('chatInput');
    const sendChatBtn = document.getElementById('sendChatBtn');
    const chatHistory = document.getElementById('chatHistory');
    const chatSpeak = document.getElementById('chatSpeak');
    const chatVoiceVol = document.getElementById('chatVoiceVol');

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
        body: JSON.stringify({
          message: msg,
          speak: !!(chatSpeak && chatSpeak.checked),
          voiceVolume: chatVoiceVol ? (Number(chatVoiceVol.value) / 100).toFixed(2) : '0.85'
        })
      })
      .then(r => r.json())
      .then(res => {
        sendChatBtn.disabled = false;
        if (res.error) {
          appendChat('Error', res.error, '#fff', 'var(--error)');
        } else {
          appendChat('GemBot', res.response, '#000', '#f1f1f1');
          if (res.oledSent === false) appendChat('Layar', 'Belum terkirim ke layar: ' + (res.oledError || 'koneksi error'), '#fff', 'var(--error)');
          if (res.speechError) appendChat('VOICE', 'Suara belum keluar: ' + res.speechError, '#fff', 'var(--error)');
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
function floatingChatbotHtml() {
  return '<div class="fabBotWrap"><div id="fabWindow" class="fabWindow"><div class="fabHeader"><div style="display:flex;align-items:center;gap:8px"><svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="10" rx="2"/><circle cx="12" cy="5" r="2"/><path d="M12 7v4M8 16h.01M16 16h.01"/></svg> GemBot</div><button id="fabClose" class="fabClose">&times;</button></div><div id="fabBody" class="fabBody"><div class="fabMsg bot">Halo! Ada yang bisa GemBot bantu hari ini?</div></div><div class="fabInput"><input id="fabInputTxt" placeholder="Ketik pesan..."><button id="fabSendBtn"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/></svg></button></div></div><div id="fabBtn" class="fabBtn"><svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"/></svg></div></div>';
}
function floatingChatbotJs() {
  return "document.getElementById('fabBtn').onclick=()=>{document.getElementById('fabWindow').classList.toggle('open')};document.getElementById('fabClose').onclick=()=>{document.getElementById('fabWindow').classList.remove('open')};function addFabMsg(text, isUser){const d=document.createElement('div');d.className='fabMsg '+(isUser?'user':'bot');d.textContent=text;document.getElementById('fabBody').appendChild(d);document.getElementById('fabBody').scrollTop=99999;}document.getElementById('fabSendBtn').onclick=async()=>{const txt=document.getElementById('fabInputTxt').value.trim();if(!txt)return;document.getElementById('fabInputTxt').value='';addFabMsg(txt, true);try {const r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({message:txt,speak:false,voiceVolume:'0'})});const j=await r.json();if(j.error) addFabMsg('Error: '+j.error, false);else addFabMsg(j.response, false);}catch(e){addFabMsg('Error: '+e.message, false);}};document.getElementById('fabInputTxt').onkeydown=e=>{if(e.key==='Enter') document.getElementById('fabSendBtn').click()};";
}
function appShellStyles() {
  return `
    @import url('https://fonts.googleapis.com/css2?family=Nunito:wght@400;600;700;800;900&display=swap');
    :root{--bg0:#0b2535;--bg1:#0b314a;--panel:#103b57;--line:rgba(255,255,255,0.1);--text:#ffffff;--muted:#a9c3d4;--blue:#0b8fe8;--card:#f7fbff;--ink:#152233;}
    *{box-sizing:border-box}body{margin:0;font-family:Nunito,sans-serif;color:var(--text);background:#0b314a;min-height:100vh}
    button,input{font:inherit}button{border:0;cursor:pointer}a{color:inherit;text-decoration:none}.hidden{display:none!important}
    .brandMark{display:flex;align-items:center;gap:12px}.botIcon{width:50px;height:50px;position:relative;display:grid;place-items:center}.botHead{width:36px;height:32px;border-radius:10px;background:#e7dce6;position:relative;box-shadow:0 6px 12px rgba(0,0,0,.15)}.botHead:before{content:"";position:absolute;left:6px;right:6px;top:6px;height:14px;border-radius:5px;background:#10202d}.botHead:after{content:"";position:absolute;left:14px;right:14px;bottom:5px;height:2px;border-radius:1px;background:#eb5874}.botEye{position:absolute;top:10px;width:5px;height:5px;border-radius:2px;background:#14c7ff}.botEye.l{left:10px}.botEye.r{right:10px}.ant{position:absolute;top:-2px;width:4px;height:16px;background:#e9445f;border-radius:3px}.ant.l{left:5px}.ant.r{right:5px}.ant:before{content:"";position:absolute;top:-2px;left:-1px;width:6px;height:6px;border-radius:50%;background:#ff4d67}.halo{position:absolute;top:1px;width:18px;height:6px;border-radius:4px;background:#ffc64e}.brandText strong{display:block;font-size:22px;font-weight:900;line-height:1;letter-spacing:0.5px}.brandText span{font-size:12px;color:var(--muted);font-weight:700}
    .fabBotWrap{position:fixed;bottom:24px;right:24px;z-index:9999;font-family:Nunito,sans-serif}.fabBtn{width:56px;height:56px;border-radius:28px;background:linear-gradient(135deg,#0b8fe8,#105f91);box-shadow:0 8px 24px rgba(11,143,232,0.4);color:#fff;display:flex;align-items:center;justify-content:center;cursor:pointer;transition:transform 0.3s cubic-bezier(0.175,0.885,0.32,1.275)}.fabBtn:hover{transform:scale(1.1)}.fabWindow{position:absolute;bottom:76px;right:0;width:350px;height:500px;background:#fff;border-radius:24px;box-shadow:0 12px 48px rgba(0,0,0,0.25);display:flex;flex-direction:column;overflow:hidden;transform-origin:bottom right;transition:all 0.4s cubic-bezier(0.175,0.885,0.32,1.275);opacity:0;pointer-events:none;transform:scale(0.8) translateY(20px);border:1px solid rgba(0,0,0,0.05)}.fabWindow.open{opacity:1;pointer-events:auto;transform:scale(1) translateY(0)}.fabHeader{height:65px;background:linear-gradient(135deg,#11486d,#0b8fe8);color:#fff;display:flex;align-items:center;padding:0 24px;font-weight:800;font-size:16px;justify-content:space-between;box-shadow:0 2px 10px rgba(0,0,0,0.1);z-index:10}.fabClose{background:transparent;border:0;color:#fff;font-size:28px;cursor:pointer;line-height:1;opacity:0.8;transition:opacity 0.2s;padding:0;display:grid;place-items:center}.fabClose:hover{opacity:1}.fabBody{flex:1;background:#f4f7fb;padding:20px;overflow-y:auto;display:flex;flex-direction:column;gap:16px}.fabMsg{max-width:85%;padding:14px 18px;border-radius:20px;font-size:14px;font-weight:700;line-height:1.5;word-wrap:break-word;box-shadow:0 2px 12px rgba(0,0,0,0.04)}.fabMsg.bot{background:#fff;color:#172435;align-self:flex-start;border-bottom-left-radius:6px;border:1px solid rgba(0,0,0,0.02)}.fabMsg.user{background:linear-gradient(135deg,#0b8fe8,#0779c9);color:#fff;align-self:flex-end;border-bottom-right-radius:6px}.fabInput{display:flex;padding:16px;background:#fff;border-top:1px solid #edf1f5;gap:12px;align-items:center}.fabInput input{flex:1;height:44px;border:1px solid #e2e8f0;border-radius:22px;padding:0 18px;color:#172435;font-weight:700;font-size:14px;outline:none;background:#f8fafc;transition:all 0.2s}.fabInput input:focus{border-color:#0b8fe8;background:#fff;box-shadow:0 0 0 3px rgba(11,143,232,0.1)}.fabInput button{width:44px;height:44px;border-radius:22px;background:#0b8fe8;color:#fff;display:flex;align-items:center;justify-content:center;border:0;cursor:pointer;transition:transform 0.2s;flex-shrink:0}.fabInput button:hover{transform:scale(1.05);background:#0779c9}@media(max-width:480px){.fabWindow{width:calc(100vw - 40px);right:-4px;height:460px}}
  `;
}

function pageHtml() {
  return `<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>GemBot</title><style>${appShellStyles()}
    body.auth{display:grid;place-items:center;padding:32px}.authWrap{width:min(100%,510px);display:flex;flex-direction:column;align-items:center}.authHero{display:flex;flex-direction:column;align-items:center;text-align:center;margin-bottom:34px}.authHero .brandText strong{font-size:36px}.authHero .brandText span{font-size:14px}
    .authCard{width:100%;background:rgba(17,47,66,.96);border-radius:22px;padding:34px 36px 28px;box-shadow:0 24px 70px rgba(0,0,0,.34)}.authTitle{font-weight:900;font-size:16px;margin:0 0 4px}.authSub{font-weight:700;color:var(--muted);font-size:12px;margin:0 0 25px}.field{margin:0 0 18px}.field label{display:block;font-size:13px;font-weight:900;margin:0 0 9px}.inputBox{height:43px;border:1.5px solid rgba(218,238,248,.52);border-radius:13px;display:flex;align-items:center;gap:9px;padding:0 13px;color:var(--muted)}.inputBox input{width:100%;border:0;outline:0;background:transparent;color:var(--text);font-size:13px;font-weight:700}.inputBox input::placeholder{color:rgba(218,238,248,.5)}.authBtn{width:100%;height:50px;margin-top:18px;border-radius:13px;background:#105f91;border:1px solid #28a5f8;color:#fff;font-weight:900;box-shadow:inset 0 0 0 1px rgba(255,255,255,.05)}.authSwitch{margin-top:16px;text-align:center;color:var(--muted);font-size:13px;font-weight:800}.authSwitch button{background:transparent;color:#fff;font-weight:900;padding:0}.authMsg{min-height:20px;margin-top:14px;text-align:center;color:#ffbdc9;font-weight:800;font-size:12px}
</style></head><body class="auth"><main class="authWrap"><section class="authHero"><div class="brandMark"><div class="botIcon"><span class="ant l"></span><span class="ant r"></span><span class="halo"></span><div class="botHead"><span class="botEye l"></span><span class="botEye r"></span></div></div><div class="brandText"><strong>GemBot</strong><span>Mini AI Companion</span></div></div></section><section class="authCard"><h2 id="authTitle" class="authTitle">Buat akun baru</h2><p id="authSub" class="authSub">Hanya butuh beberapa detik</p><div class="field" id="nameField"><label>Nama Panggilan</label><div class="inputBox"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg><input type="text" id="authName" placeholder="Nama kamu..."></div></div><div class="field"><label>Email Pintar</label><div class="inputBox"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4h16c1.1 0 2 .9 2 2v12c0 1.1-.9 2-2 2H4c-1.1 0-2-.9-2-2V6c0-1.1.9-2 2-2z"/><polyline points="22,6 12,13 2,6"/></svg><input type="email" id="authEmail" placeholder="kamu@email.com"></div></div><div class="field"><label>Kata Sandi</label><div class="inputBox"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2" ry="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg><input type="password" id="authPass" placeholder="••••••••"></div></div><button id="authSubmit" class="authBtn">Daftar Sekarang</button><div class="authSwitch"><span id="switchText">Sudah punya akun?</span> <button id="modeSwitch">Masuk di sini</button></div>
<div id="authStatus" class="authMsg"></div></section></main>${floatingChatbotHtml()}<script>
    ${floatingChatbotJs()}
    let authMode='register';let currentUser=localStorage.getItem('owi_current_user')||'';const $=id=>document.getElementById(id);
    const getUsers=()=>{try{return JSON.parse(localStorage.getItem('owi_users')||'{}')}catch{return {}}};const saveUsers=u=>localStorage.setItem('owi_users',JSON.stringify(u));
    function setMode(m){authMode=m;$('nameField').classList.toggle('hidden',m==='login');$('authTitle').textContent=m==='login'?'Selamat datang kembali':'Buat akun baru';$('authSub').textContent=m==='login'?'Masuk untuk mengendalikan robot kamu.':'Hanya butuh beberapa detik';$('authSubmit').textContent=m==='login'?'Masuk Sekarang':'Daftar Sekarang';$('switchText').textContent=m==='login'?'Belum punya akun?':'Sudah punya akun?';$('modeSwitch').textContent=m==='login'?'Daftar':'Masuk di sini';$('authStatus').textContent=''}
    $('modeSwitch').onclick=()=>setMode(authMode==='login'?'register':'login');
    $('authSubmit').onclick=()=>{const email=$('authEmail').value.trim().toLowerCase();const name=($('authName').value.trim()||email.split('@')[0]);const pass=$('authPass').value;const users=getUsers();if(!email.includes('@')){$('authStatus').textContent='Email belum valid';return}if(pass.length<8){$('authStatus').textContent='Password minimal 8 karakter';return}if(authMode==='register'){if(users[email]){$('authStatus').textContent='Email sudah terdaftar';return}users[email]={name,pass};saveUsers(users)}else if(!users[email]||users[email].pass!==pass){$('authStatus').textContent='Email atau password salah';return}localStorage.setItem('owi_current_user',users[email]?.name||name);location.href='/control'};
    if(currentUser)location.href='/control';setMode('register');
  </script></body></html>`;
}

function controlPageHtml() {
  return `<!doctype html><html lang="id"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>GemBot Control</title><style>${appShellStyles()}
    body{padding:40px 32px 70px}.app{max-width:960px;margin:0 auto}.top{display:flex;align-items:flex-start;justify-content:space-between;margin-bottom:40px}.pill{height:32px;border-radius:16px;border:1px solid rgba(255,255,255,0.2);background:transparent;color:#fff;font-weight:700;font-size:13px;display:inline-flex;align-items:center;padding:0 16px}.logout{height:32px;border-radius:16px;padding:0 16px;background:#fff;color:#17202d;font-weight:800;margin-left:12px}
    .statusCard,.tabPanel{border:1px solid var(--line);border-radius:20px;background:transparent;padding:32px}.statusCard{margin-bottom:32px}.title{font-size:18px;font-weight:800;margin:0 0 4px}.subtitle{font-size:14px;color:var(--muted);font-weight:600;margin:0 0 24px}.oled{height:160px;background:#fff;border-radius:16px;display:grid;place-items:center;margin-bottom:24px;overflow:hidden}.faceText{color:#151a27;font-size:64px;letter-spacing:10px;font-weight:800;font-family:monospace;transition:transform .2s}.batteryRow{display:flex;align-items:center;gap:12px;font-weight:700;font-size:14px;margin-bottom:16px;color:#fff}.bar{height:6px;border-radius:3px;background:rgba(255,255,255,0.2);flex:1;overflow:hidden}.bar span{display:block;height:100%;width:89%;background:#2e9bff}.metricGrid{display:grid;grid-template-columns:repeat(3,1fr);gap:24px}.metric{height:100px;border-radius:12px;background:#7e94a1;display:flex;flex-direction:column;align-items:center;justify-content:center;color:#fff}.metric svg{width:24px;height:24px;margin-bottom:8px;opacity:0.8}.metric strong{font-size:22px;font-weight:800;line-height:1.2}.metric span{font-size:12px;font-weight:600}
    .tabs{height:54px;background:#fff;border-radius:16px;display:grid;grid-template-columns:repeat(6,1fr);gap:4px;padding:4px;margin-bottom:24px}.tab{border-radius:12px;background:transparent;color:#12354c;font-weight:800;display:flex;align-items:center;justify-content:center;gap:6px;font-size:14px}.tab svg{width:18px;height:18px}.tab.active{background:#11486d;color:#fff}.tabPanel{min-height:280px}.pane{display:none}.pane.active{display:block}.gridExpr{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}.exprBtn{height:100px;border-radius:16px;background:#fff;color:#182336;font-weight:700}.exprBtn .big{display:block;font-size:32px;font-weight:800;line-height:1.2;font-family:monospace}.exprBtn small{display:block;margin-top:4px;font-size:13px;color:#4a5c70}
    .gridAnim{display:grid;grid-template-columns:repeat(3,1fr);gap:20px}.animBtn{height:58px;border-radius:29px;background:#fff;color:#12354c;font-weight:800;font-size:15px;display:flex;align-items:center;justify-content:center}
    .musicNow{background:#fff;color:#182336;border-radius:20px;padding:24px 32px;margin:26px 0;display:flex;flex-direction:column}.musicNow span{font-size:13px;color:#6d8498;font-weight:700}.musicNow strong{font-size:22px;font-weight:900;margin:6px 0 2px}.musicNow small{font-size:14px;color:#4a5c70;font-weight:600}
    .musicControls{display:flex;align-items:center;justify-content:center;gap:20px;margin:32px 0 28px}.circle{width:44px;height:44px;border-radius:50%;background:transparent;border:2px solid #fff;color:#fff;display:flex;align-items:center;justify-content:center;font-size:18px}.circle.main{width:52px;height:52px;background:#fff;color:#12354c;border:0}
    .volRow{display:flex;align-items:center;gap:16px;color:#fff;font-weight:800;font-size:14px;margin-bottom:40px}.volSlider{flex:1;height:6px;border-radius:3px;background:rgba(255,255,255,0.2);appearance:none;outline:0}.volSlider::-webkit-slider-thumb{appearance:none;width:14px;height:14px;border-radius:50%;background:#2e9bff;cursor:pointer}
    .plHeader{display:flex;align-items:center;gap:8px;font-weight:900;font-size:13px;letter-spacing:1px;color:#fff;margin-bottom:16px}.plHeader svg{width:18px;height:18px}
    .playlist{display:grid;gap:8px;max-height:220px;overflow-y:auto;padding-right:10px}.playlist::-webkit-scrollbar{width:10px}.playlist::-webkit-scrollbar-track{background:rgba(255,255,255,0.28);border-radius:12px}.playlist::-webkit-scrollbar-thumb{background:#fff;border-radius:12px}.song{display:grid;grid-template-columns:36px 1fr auto;align-items:center;min-height:48px;border-radius:24px;padding:0 18px;font-size:14px;font-weight:900;color:#fff;transition:background .18s,transform .18s}.song:hover{background:rgba(255,255,255,0.14);transform:translateX(2px)}.song.active{background:rgba(255,255,255,0.42);color:#fff}.song small{color:rgba(255,255,255,0.82);font-weight:700;text-align:right}
    .remForm{display:grid;grid-template-columns:160px 240px 1fr 110px;gap:16px;align-items:end;margin-top:24px}.field label{display:block;font-size:14px;font-weight:800;margin-bottom:12px;color:#fff}.field input{height:48px;border:0;border-radius:24px;background:#fff;color:#213044;padding:0 20px;font-weight:700;width:100%;font-family:inherit;font-size:14px;box-sizing:border-box}.addBtn{height:48px;border-radius:24px;background:transparent;color:#fff;font-weight:800;border:1px solid #fff;font-size:14px;transition:all .2s;cursor:pointer}.addBtn:hover{background:rgba(255,255,255,0.1)}.remList{margin-top:24px;display:grid;gap:12px}.remItem{min-height:78px;border-radius:16px;background:#fff;color:#172435;display:flex;align-items:center;justify-content:space-between;padding:16px 20px;font-weight:800;transition:opacity .2s,transform .2s}.remItem.off{opacity:.58}.remToggle{width:42px;height:24px;border-radius:12px;background:#cbd5e1;padding:3px;display:flex;align-items:center;transition:background .2s}.remToggle:before{content:"";width:18px;height:18px;border-radius:50%;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.25);transition:transform .2s}.remToggle.on{background:#0b8fe8}.remToggle.on:before{transform:translateX(18px)}.remMeta{display:flex;gap:7px;align-items:center;color:#6b8296;font-size:11px;font-weight:800}.remSync{display:inline-flex;align-items:center;min-height:32px;margin:18px 0 0 12px;color:#b9d4e3;font-size:13px;font-weight:800}
    .aiLayout{display:grid;grid-template-columns:1fr 1fr;gap:22px}.chatBox{height:220px;border-radius:16px;background:#fff;color:#172435;padding:14px;overflow:auto}.chatInput{display:flex;gap:10px;margin-top:12px}.chatInput input{height:45px;border:0;border-radius:12px;background:#fff;color:#172435;padding:0 14px;font-weight:800}.chatInput button,.primary{border-radius:12px;background:#075984;color:#fff;padding:0 18px;font-weight:900}
    .statusLine{margin-top:22px;font-weight:700;color:var(--muted);text-align:center}
    .drawLayout{display:flex;flex-direction:column;gap:20px;align-items:center}
    @media(min-width:761px){.drawLayout{flex-direction:row;align-items:flex-start}}
    .drawCanvas{width:100%;max-width:100%;aspect-ratio:3/4;background:#000;border-radius:12px;image-rendering:pixelated;touch-action:none;box-shadow:0 8px 30px rgba(0,0,0,0.4);border:2px solid rgba(255,255,255,0.1)}
    .drawTools{display:grid;grid-template-columns:1fr 1fr;gap:12px;width:100%}
    @media(max-width:760px){body{padding:16px 12px}.statusCard,.tabPanel{padding:20px}.metricGrid{grid-template-columns:repeat(3,1fr);gap:8px}.metric{height:80px}.metric svg{width:20px;height:20px;margin-bottom:4px}.metric strong{font-size:18px}.metric span{font-size:11px}.gridExpr,.gridAnim{grid-template-columns:repeat(2,1fr);gap:12px}.aiLayout{grid-template-columns:1fr;gap:12px}.remForm{grid-template-columns:1fr}.tabs{display:flex;overflow-x:auto;height:48px;padding:4px;scroll-snap-type:x mandatory;-webkit-overflow-scrolling:touch;scrollbar-width:none;border-radius:16px;margin-bottom:16px}.tabs::-webkit-scrollbar{display:none}.tab{flex:0 0 auto;padding:0 16px;scroll-snap-align:start;font-size:13px;border-radius:12px}.oled{height:100px;margin-bottom:16px}.faceText{font-size:40px}.exprBtn{height:80px}.exprBtn .big{font-size:24px}.exprBtn small{font-size:11px}}
  </style></head><body><div class="app"><header class="top"><div class="brandMark"><div class="botIcon"><span class="ant l"></span><span class="ant r"></span><span class="halo"></span><div class="botHead"><span class="botEye l"></span><span class="botEye r"></span></div></div><div class="brandText"><strong>GemBot</strong><span>Mini AI Companion</span></div></div><div><span id="connBadge" class="pill"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px"><path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/></svg> Terhubung</span><button id="logoutBtn" class="logout">Logout <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:-2px;margin-left:4px"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9"/></svg></button></div></header>
    <section class="statusCard"><h1 class="title">Status Robot</h1><p class="subtitle">Kondisi GemBot saat ini</p><div class="oled"><div id="faceText" class="faceText">⌒‿⌒</div></div><div class="batteryRow"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="7" width="16" height="10" rx="2" ry="2"/><line x1="22" y1="11" x2="22" y2="13"/></svg> Baterai <div class="bar"><span id="batBar"></span></div><span id="batText">89%</span></div><div class="metricGrid"><div class="metric"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/></svg><strong id="valTemp">26C</strong><span>Suhu</span></div><div class="metric"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z"/></svg><strong id="valHum">58%</strong><span>Lembab</span></div><div class="metric"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 12h-4l-3 9L9 3l-3 9H2"/></svg><strong id="valShake">0.0</strong><span>Gerakan</span></div></div></section>
    <nav class="tabs"><button class="tab active" data-tab="expr"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M8 14s1.5 2 4 2 4-2 4-2M9 9h.01M15 9h.01"/></svg> Ekspresi</button><button class="tab" data-tab="music"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 18V5l12-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="18" cy="16" r="3"/></svg> Musik</button><button class="tab" data-tab="rem"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9M13.73 21a2 2 0 0 1-3.46 0"/></svg> Pengingat</button><button class="tab" data-tab="ai"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="10" rx="2"/><circle cx="12" cy="5" r="2"/><path d="M12 7v4M8 16h.01M16 16h.01"/></svg> AI</button><button class="tab" data-tab="draw"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 19l7-7 3 3-7 7-3-3z"/><path d="M18 13l-1.5-7.5L2 2l3.5 14.5L13 18l5-5z"/><path d="M2 2l7.586 7.586"/><circle cx="11" cy="11" r="2"/></svg> Draw</button><button class="tab" data-tab="sys"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg> Sistem</button></nav>
    <section class="tabPanel"><div id="pane-expr" class="pane active"><h2 class="title">Kontrol Ekspresi</h2><p class="subtitle">Pilih mood GemBot secara langsung.</p><div class="gridExpr"><button class="exprBtn" data-cmd="M0"><span class="big">●‿●</span><small>Senyum</small></button><button class="exprBtn" data-cmd="M1"><span class="big">⌒▽⌒</span><small>Senang</small></button><button class="exprBtn" data-cmd="M6"><span class="big">●︵●</span><small>Sedih</small></button><button class="exprBtn" data-cmd="M3"><span class="big">◣︵◢</span><small>Marah</small></button><button class="exprBtn" data-cmd="M4"><span class="big">OｏO</span><small>Kaget</small></button><button class="exprBtn" data-cmd="M5"><span class="big">—‿—</span><small>Ngantuk</small></button><button class="exprBtn" data-cmd="M24"><span class="big">⌒▽⌒</span><small>Delight</small></button><button class="exprBtn" data-cmd="M25"><span class="big">●﹏●</span><small>Guilty</small></button><button class="exprBtn" data-cmd="M26"><span class="big">◔‿◔</span><small>Daydream</small></button><button class="exprBtn" data-cmd="M27"><span class="big">¬︵¬</span><small>Grumpy</small></button><button class="exprBtn" data-cmd="M28"><span class="big">O▽O</span><small>Amazed</small></button><button class="exprBtn" data-cmd="M29"><span class="big">╥︵╥</span><small>Nangis</small></button><button class="exprBtn" data-cmd="M30"><span class="big">@﹏@</span><small>Pusing</small></button><button class="exprBtn" data-cmd="M31"><span class="big">—‿●</span><small>Nakal</small></button><button class="exprBtn" data-cmd="M50"><span class="big">♥‿♥</span><small>Love</small></button><button class="exprBtn" data-cmd="M51"><span class="big">＞▽＜</span><small>Ketawa</small></button></div></div>
      <div id="pane-sys" class="pane"><h2 class="title">Kontrol Cepat</h2><p class="subtitle">Buka menu, pilih, kembali, atau mulai game dari web.</p><div class="gridExpr"><button class="exprBtn" data-cmd="P"><span class="big">TAP</span><small>Buka Menu/Next</small></button><button class="exprBtn" data-cmd="O"><span class="big">HOLD</span><small>Pilih/OK</small></button><button class="exprBtn" data-cmd="C"><span class="big">BACK</span><small>Balik Ekspresi</small></button><button class="exprBtn" data-cmd="G"><span class="big">GAME</span><small>Main Pingpong</small></button><button class="exprBtn" data-cmd="F"><span class="big">FLIP</span><small>Balik Layar</small></button></div></div>
      <div id="pane-music" class="pane"><h2 class="title">Kontrol Musik</h2><p class="subtitle">Jalankan lagu dari DFPlayer Mini.</p><div class="musicNow"><span>Sedang diputar</span><strong id="songTitle">Love Story</strong><small id="songArtist">Taylor Swift - 3:54</small></div><div class="musicControls"><button id="btnPrevSong" class="circle" title="Sebelumnya"><svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M11 5L4 12l7 7V5z"/><path d="M19 5l-7 7 7 7V5z"/></svg></button><button id="btnPlaySong" class="circle main" title="Play/Pause"><svg id="playIcon" width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg></button><button id="btnNextSong" class="circle" title="Berikutnya"><svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M13 5l7 7-7 7V5z"/><path d="M5 5l7 7-7 7V5z"/></svg></button><button id="btnStopAudio" class="circle" title="Stop"><svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="6" width="12" height="12" rx="2"/></svg></button></div><div class="volRow"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"/></svg><input id="volLoveStory" class="volSlider" type="range" min="4" max="55" value="40"><span>40</span></div><div class="plHeader"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><circle cx="12" cy="12" r="3"/></svg> PLAYLIST DFPLAYER</div><div class="playlist"><div class="song active" data-file="DFP:1"><span>1</span><strong>Love Story</strong><small>Taylor Swift 3:54</small></div><div class="song" data-file="DFP:2"><span>2</span><strong>MBG</strong><small>Local MP3 0:15</small></div><div class="song" data-file="DFP:3"><span>3</span><strong>YELLOW</strong><small>Coldplay 4:29</small></div><div class="song" data-file="DFP:4"><span>4</span><strong>REDRED</strong><small>CORTIS 2:43</small></div></div></div>
      <div id="pane-rem" class="pane"><h2 class="title">Alarm dan Pengingat</h2><p class="subtitle">Tambah jadwal, aktifkan yang diperlukan, lalu sinkronkan ke GemBot.</p><div class="remForm"><div class="field"><label>Jam</label><input id="remTime" type="time"></div><div class="field"><label>Tanggal</label><input id="remDate" type="date"></div><div class="field"><label>Keterangan</label><input id="remText" maxlength="32" placeholder="Masukkan keterangan pengingat"></div><button id="addReminder" class="addBtn">+ Tambah</button></div><div id="reminderList" class="remList"></div><button id="sendReminder" class="primary" style="height:44px;margin-top:18px">Sync ke GemBot</button><span id="remSyncState" class="remSync">Belum ada perubahan yang dikirim</span></div>
      <div id="pane-draw" class="pane"><h2 class="title">Kanvas Gambar</h2><div class="drawLayout"><canvas id="tftCanvas" class="drawCanvas" width="240" height="320"></canvas><div class="drawTools"><button id="enterDraw" class="primary" style="height:46px">Masuk Draw Mode</button><button id="clearDraw" class="primary" style="height:46px">Clear Canvas</button>
<div style="grid-column:1/-1; display:flex; gap:10px; margin-top:10px;">
  <label style="flex:1;font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;color:var(--text-light)">BRUSH
    <input type="range" id="tftBrushSize" min="1" max="15" value="3" style="width:100%;margin-top:0.4rem;">
  </label>
  <label style="flex:1;font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;color:var(--text-light)">WARNA
    <input type="color" id="tftDrawColor" value="#ffffff" style="width:100%;height:24px;border:none;padding:0;cursor:pointer;margin-top:0.2rem;border-radius:4px;">
  </label>
  <label style="flex:1;font-family:'Roboto Mono',monospace;font-size:0.75rem;font-weight:800;color:var(--text-light)">BG CANVAS
    <input type="color" id="tftCanvasColor" value="#000000" style="width:100%;height:24px;border:none;padding:0;cursor:pointer;margin-top:0.2rem;border-radius:4px;">
  </label>
</div>
<div style="grid-column:1/-1;text-align:center"><p id="drawSyncState" class="statusLine" style="margin-top:0">Live draw siap</p></div></div></div></div>
      <div id="pane-ai" class="pane"><h2 class="title">Teman Bicara</h2><p class="subtitle">Tanya atau ajak GemBot ngobrol.</p><div class="aiLayout"><div><div id="aiLimitBadge" class="pill" style="background:rgba(255,255,255,0.1);color:#fff">AI: --</div> <div id="aiKeyBadge" class="pill" style="display:none">KEY: --</div><div id="chatHistory" class="chatBox" style="height:220px;margin-top:12px;margin-bottom:12px"></div><div class="chatInput"><input id="chatInput" placeholder="Tanya GemBot..."><button id="sendChatBtn">Kirim</button></div><label style="display:block;margin-top:10px;font-weight:900;color:#fff"><input id="chatSpeak" type="checkbox" checked> Suara GemBot</label><input id="chatVoiceVol" type="range" min="20" max="85" value="72" style="width:100%;margin-top:12px"></div><div><p id="speechLive" class="statusLine" style="color:#fff;margin-top:0;text-align:left">Suara: <span id="inmpLevelText">0%</span> • <span id="speechStatus">SIAP</span></p><div class="chatBox" style="height:96px;margin-bottom:12px"><strong>Yang terdengar</strong><div id="heardText" style="margin-top:10px;font-size:18px;color:#172435;font-weight:900">Belum ada suara</div></div><div id="speechLog" class="chatBox" style="height:160px;margin-bottom:12px"></div><button id="startSpeech" class="primary">Dengarkan</button> <button id="stopSpeech" class="primary">Kirim Suara</button></div></div></div></section><div id="status" class="statusLine">System ready</div></div>${floatingChatbotHtml()}
  <script>
    if(!localStorage.getItem('owi_current_user')) location.href='/';
    const $=id=>document.getElementById(id);let currentFile='DFP:1';let musicPaused=false;let reminders=[];let manualFaceUntil=0,manualFace='●‿●';let lastServerTranscript='';const exprPreview={M0:['●‿●','Normal'],M1:['⌒▽⌒','Senang'],M2:['●♥●','Love'],M3:['◣︵◢','Marah'],M4:['OｏO','Kaget'],M5:['—‿—','Ngantuk'],M6:['●︵●','Sedih'],M7:['⌒▽⌒','Excited'],M8:['●⌣●','Smug'],M9:['•﹏•','Takut'],M10:['●‿●','Cozy'],M11:['@﹏@','Woozy'],M12:['●⌣●','Cheeky'],M13:['●﹏●','Bashful'],M14:['◉_◉','Focus'],M15:['¬_¬','Bored'],M16:['●︿●','Nope'],M17:['⌒▽⌒','Party'],M18:['●‿●','Relieved'],M19:['¬_●','Suspicious'],M20:['⌒▽⌒','Giggle'],M21:['◣_◢','Determined'],M22:['OｏO','Wow'],M23:['@﹏@','Melt'],M24:['⌒▽⌒','Delight'],M25:['●﹏●','Guilty'],M26:['◔‿◔','Daydream'],M27:['¬︵¬','Grumpy'],M28:['O▽O','Amazed'],M29:['╥︵╥','Nangis'],M30:['@﹏@','Pusing'],M31:['—‿●','Nakal'],M50:['♥‿♥','Love'],M51:['＞▽＜','Ketawa']};function setStatus(t,bad){$('status').textContent=t;$('status').style.color=bad?'#ff9aac':'#d7e9f2'}function setLiveFace(face,label,ttl=6000){manualFace=face;manualFaceUntil=Date.now()+ttl;$('faceText').textContent=face;$('faceText').classList.remove('livePulse');void $('faceText').offsetWidth;$('faceText').classList.add('livePulse');setTimeout(()=>$('faceText').classList.remove('livePulse'),180)}
    document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.pane').forEach(x=>x.classList.remove('active'));b.classList.add('active');$('pane-'+b.dataset.tab).classList.add('active');if(b.dataset.tab==='draw')activateDrawMode(true)});
    document.querySelectorAll('[data-cmd]').forEach(b=>b.onclick=async()=>{const p=exprPreview[b.dataset.cmd];if(p)setLiveFace(p[0],p[1]);try{const r=await fetch('/cmd/'+b.dataset.cmd,{method:'POST'});setStatus(await r.text(),!r.ok)}catch(e){setStatus(e.message,true)}});
    document.querySelectorAll('.song').forEach(s=>s.onclick=()=>{document.querySelectorAll('.song').forEach(x=>x.classList.remove('active'));s.classList.add('active');currentFile=s.dataset.file;$('songTitle').textContent=s.querySelector('strong').textContent;if($('songArtist'))$('songArtist').textContent=s.querySelector('small').textContent});
    function dfVol(){return Math.max(0,Math.min(20,Math.round((Number($('volLoveStory').value||40)-4)/(55-4)*20)))}
    function currentTrack(){return currentFile.startsWith('DFP:')?Number(currentFile.split(':')[1]||1):1}
    function setPlayIcon(mode){const icon=$('playIcon');if(!icon)return;icon.innerHTML=(mode==='pause')?'<path d="M6 4h4v16H6V4zm8 0h4v16h-4V4z"/>':'<path d="M8 5v14l11-7z"/>'}
    async function dfCommand(action,track=currentTrack()){const r=await fetch('/dfplayer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action,track,volume:dfVol()})});setStatus(await r.text(),!r.ok);return r.ok}
    async function playCurrent(){try{if(currentFile.startsWith('DFP:')){await dfCommand('VOL');const ok=await dfCommand('PLAY',currentTrack());musicPaused=false;setPlayIcon('pause');return ok}const vol=($('volLoveStory').value/100).toFixed(2);const r=await fetch('/play_audio',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file:currentFile,volume:vol})});setStatus(await r.text(),!r.ok);setPlayIcon('pause')}catch(e){setStatus(e.message,true)}}
    $('btnPlaySong').onclick=async()=>{if(currentFile.startsWith('DFP:')){try{if($('playIcon')?.innerHTML.includes('M6 4')){const ok=await dfCommand('PAUSE');if(ok){musicPaused=true;setPlayIcon('play')}}else if(musicPaused){const ok=await dfCommand('RESUME');if(ok){musicPaused=false;setPlayIcon('pause')}}else await playCurrent()}catch(e){setStatus(e.message,true)}}else await playCurrent()};
    $('btnPrevSong').onclick=()=>{const songs=[...document.querySelectorAll('.song')];let i=songs.findIndex(x=>x.classList.contains('active'));songs[(i+songs.length-1)%songs.length].click();playCurrent()};
    $('btnNextSong').onclick=()=>{const songs=[...document.querySelectorAll('.song')];let i=songs.findIndex(x=>x.classList.contains('active'));songs[(i+1)%songs.length].click();playCurrent()};
    $('btnStopAudio').onclick=async()=>{try{let r;if(currentFile.startsWith('DFP:')){r=await fetch('/dfplayer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'STOP'})})}else{r=await fetch('/stop_audio',{method:'POST'})}setStatus(await r.text(),!r.ok);musicPaused=false;setPlayIcon('play')}catch(e){setStatus(e.message,true)}};
    let volTimer=null;$('volLoveStory').oninput=()=>{$('volLoveStory').nextElementSibling.textContent=$('volLoveStory').value;if(currentFile.startsWith('DFP:')){clearTimeout(volTimer);volTimer=setTimeout(()=>dfCommand('VOL'),180)}};
    const reminderKey='gembot_reminders';
    const escapeReminder=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const reminderDateLabel=date=>date?new Date(date+'T00:00:00').toLocaleDateString('id-ID',{day:'2-digit',month:'short',year:'numeric'}):'Setiap hari';
    function saveReminderDraft(){localStorage.setItem(reminderKey,JSON.stringify(reminders))}
    function renderRem(){
      $('reminderList').innerHTML=reminders.map((r,i)=>'<div class="remItem '+(r.active===false?'off':'')+'"><div style="display:flex;align-items:center;gap:16px"><svg width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#11486d" stroke-width="2"><path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9M13.73 21a2 2 0 0 1-3.46 0"/></svg><div style="line-height:1.25"><span style="font-size:17px;font-weight:900;color:#182336">'+escapeReminder(r.time)+'</span><br><span style="font-size:13px;color:#597086;font-weight:800">'+escapeReminder(r.text)+'</span><div class="remMeta">'+escapeReminder(reminderDateLabel(r.date))+' · '+(r.active===false?'Nonaktif':'Aktif')+'</div></div></div><div style="display:flex;gap:12px;align-items:center"><button type="button" aria-label="Aktifkan pengingat" data-toggle="'+i+'" class="remToggle '+(r.active===false?'':'on')+'"></button><button type="button" aria-label="Hapus pengingat" data-del="'+i+'" style="background:none;border:none;color:#8ba3b8;cursor:pointer;padding:6px"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2 2v2"/></svg></button></div></div>').join('');
      document.querySelectorAll('[data-del]').forEach(b=>b.onclick=()=>{reminders.splice(+b.dataset.del,1);saveReminderDraft();renderRem();$('remSyncState').textContent='Perubahan belum disinkronkan'});
      document.querySelectorAll('[data-toggle]').forEach(b=>b.onclick=()=>{const i=+b.dataset.toggle;reminders[i].active=reminders[i].active===false;saveReminderDraft();renderRem();$('remSyncState').textContent='Perubahan belum disinkronkan'});
    }
    try{const stored=JSON.parse(localStorage.getItem(reminderKey)||'[]');if(Array.isArray(stored))reminders=stored.slice(0,5)}catch{}
    const todayIso=new Date().toLocaleDateString('en-CA');$('remDate').value=todayIso;
    $('addReminder').onclick=()=>{if(reminders.length>=5){setStatus('Maksimal 5 pengingat',true);return}const time=$('remTime').value||'07:30';const text=$('remText').value.trim()||'enroll lagi ya deck';const date=$('remDate').value||'';reminders.push({time,text:text.slice(0,32),date,active:true});saveReminderDraft();renderRem();$('remText').value='';$('remSyncState').textContent='Jadwal baru belum disinkronkan'};
    $('sendReminder').onclick=async()=>{const button=$('sendReminder');const list=reminders.length?reminders:[{time:$('remTime').value||'07:30',date:$('remDate').value||'',text:$('remText').value||'enroll lagi ya deck',active:true}];button.disabled=true;button.textContent='Menyinkronkan...';$('remSyncState').textContent='Mengirim ke GemBot';try{const r=await fetch('/reminder',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reminders:list})});const message=await r.text();setStatus(message,!r.ok);$('remSyncState').textContent=r.ok?'Tersinkron · tampil di layar selama 5 detik':'Gagal sinkron';if(r.ok)saveReminderDraft()}catch(e){setStatus(e.message,true);$('remSyncState').textContent='Gagal terhubung ke GemBot'}finally{button.disabled=false;button.textContent='Sync ke GemBot'}};
    renderRem();
    function appendChat(who,msg,mine){const d=document.createElement('div');d.style.cssText='margin:8px 0;padding:10px 12px;border-radius:12px;background:'+(mine?'#075984':'#eef6fb')+';color:'+(mine?'#fff':'#172435')+';font-weight:800';d.textContent=who+': '+msg;$('chatHistory').appendChild(d);$('chatHistory').scrollTop=99999}
    $('sendChatBtn').onclick=async()=>{const msg=$('chatInput').value.trim();if(!msg)return;$('chatInput').value='';appendChat('Kamu',msg,true);try{const r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({message:msg,speak:$('chatSpeak').checked,voiceVolume:($('chatVoiceVol').value/100).toFixed(2)})});const j=await r.json();if(j.error)appendChat('Error',j.error,false);else{appendChat('GemBot',j.response,false);refreshAiLimit()}}catch(e){appendChat('Error',e.message,false)}};
    $('chatInput').addEventListener('keydown',e=>{if(e.key==='Enter')$('sendChatBtn').click()});$('logoutBtn').onclick=()=>{localStorage.removeItem('owi_current_user');location.href='/'};
    let pendingDrawCmds = [];
    function hexToRGB565(hex) {
        let r = parseInt(hex.substring(1,3), 16) >> 3;
        let g = parseInt(hex.substring(3,5), 16) >> 2;
        let b = parseInt(hex.substring(5,7), 16) >> 3;
        return (r << 11) | (g << 5) | b;
    }
    const c=$('tftCanvas'),ctx=c.getContext('2d',{willReadFrequently:true});ctx.fillStyle='#000';ctx.fillRect(0,0,240,320);ctx.strokeStyle='#fff';ctx.fillStyle='#fff';ctx.lineCap='round';let drawing=false,last=null,busy=false,pending=false;function pt(e){const r=c.getBoundingClientRect(),s=e.touches?.[0]||e;return{x:Math.max(0,Math.min(239,Math.floor((s.clientX-r.left)*240/r.width))),y:Math.max(0,Math.min(319,Math.floor((s.clientY-r.top)*320/r.height)))}}
    function draw(p){
      const b=Number(document.getElementById('tftBrushSize').value||4);
      const col=document.getElementById('tftDrawColor').value||'#ffffff';
      ctx.lineWidth=b;
      ctx.strokeStyle=col;ctx.fillStyle=col;
      let x1 = last ? last.x : p.x;
      let y1 = last ? last.y : p.y;
      if(last){ctx.beginPath();ctx.moveTo(last.x,last.y);ctx.lineTo(p.x,p.y);ctx.stroke()}
      ctx.beginPath();ctx.arc(p.x,p.y,Math.max(0.5,b/2),0,7);ctx.fill();
      pendingDrawCmds.push(Math.round(x1)+","+Math.round(y1)+","+Math.round(p.x)+","+Math.round(p.y)+","+hexToRGB565(col)+","+b);
      last=p;
      syncSoon();
    }
    function down(e){e.preventDefault();drawing=true;last=null;draw(pt(e))}
    function move(e){if(!drawing)return;e.preventDefault();draw(pt(e))}
    function up(){drawing=false;last=null}
    c.onpointerdown=down;c.onpointermove=move;window.onpointerup=up;
    c.addEventListener('touchstart',down,{passive:false});
    c.addEventListener('touchmove',move,{passive:false});
    
    let drawSyncTimer=null,drawSyncBusy=false;
    function syncSoon(){
      if(drawSyncTimer)return;
      drawSyncTimer=setTimeout(flushDrawSync,80);
    }
    async function flushDrawSync(){
      drawSyncTimer=null;
      if(drawSyncBusy)return;
      if(pendingDrawCmds.length===0)return;
      drawSyncBusy=true;
      let cmdsToSend = pendingDrawCmds.join("|");
      pendingDrawCmds = [];
      try {
        const r=await fetch('/draw_cmds',{method:'POST',headers:{'Content-Type':'text/plain'},body:cmdsToSend});
        const text=await r.text();
        if(!r.ok)throw new Error(text);
        if($('drawSyncState')) {
            $('drawSyncState').textContent='LIVE DRAW TERKIRIM';
            $('drawSyncState').style.color='var(--success)';
        }
      } catch(e) {
        if($('drawSyncState')) {
            $('drawSyncState').textContent='GAGAL: '+e.message;
            $('drawSyncState').style.color='var(--danger)';
        }
      }
      drawSyncBusy=false;
      if(pendingDrawCmds.length>0)syncSoon();
    }
    
    async function enterDraw(){
        const bgCol = document.getElementById('tftCanvasColor').value || '#000000';
        await fetch('/cmd/W:' + hexToRGB565(bgCol),{method:'POST'});
    }
    async function activateDrawMode(clearLocal){
        const bgCol = '#000000';
        document.getElementById('tftCanvasColor').value = bgCol;
        if(clearLocal){
          ctx.fillStyle=bgCol;
          ctx.fillRect(0,0,240,320);
          pendingDrawCmds=[];
        }
        try{
          await fetch('/cmd/W:0',{method:'POST'});
          if($('drawSyncState')){$('drawSyncState').textContent='DRAW MODE AKTIF - layar hitam siap digambar';$('drawSyncState').style.color='var(--success)';}
          setStatus('Draw mode aktif');
        }catch(e){
          if($('drawSyncState')){$('drawSyncState').textContent='GAGAL: '+e.message;$('drawSyncState').style.color='var(--danger)';}
          setStatus(e.message,true);
        }
    }
    async function sync(){
        if(busy)return pending=true;
        busy=true;pending=false;
        try{
            await enterDraw();
            if(pendingDrawCmds.length>0) await flushDrawSync();
        }catch(e){
            if($('drawSyncState')){ $('drawSyncState').textContent='GAGAL: '+e.message; $('drawSyncState').style.color='var(--danger)'; }
        }
        busy=false;
    }
    $('enterDraw').onclick=()=>activateDrawMode(true);
    $('clearDraw').onclick=()=>{
        const bgCol = document.getElementById('tftCanvasColor').value || '#000000';
        ctx.fillStyle=bgCol;
        ctx.fillRect(0,0,240,320);
        pendingDrawCmds=[];
        sync();
    };
    async function testMax(){const r=await fetch('/test_max',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({volume:($('volLoveStory').value/100).toFixed(2)})});setStatus(await r.text(),!r.ok)}
    if ($('btnTestMax')) $('btnTestMax').onclick=testMax;
    if ($('btnMusicTestMax')) $('btnMusicTestMax').onclick=testMax;
    async function refreshSensors(){try{const r=await fetch('/api/sensors');const s=await r.json();if(!s.connected){$('connBadge').innerHTML='<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px;vertical-align:-2px"><path d="M10 20l4-16m4 4l4 4-4 4M6 16l-4-4 4-4"/></svg> '+(s.socketOpen?'Telemetri lambat':'Terputus');$('connBadge').style.background='#8e2940';$('connBadge').style.borderColor='#8e2940';return}$('connBadge').innerHTML='<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px;vertical-align:-2px"><path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/></svg> Terhubung';$('connBadge').style.background='rgba(16,95,145,.8)';$('connBadge').style.borderColor='transparent';$('valTemp').textContent=(s.temp&&s.temp>-90)?Math.round(s.temp)+'C':'--';$('valHum').textContent=(s.hum&&s.hum>=0)?Math.round(s.hum)+'%':'--';$('valShake').textContent=Number(s.shakeMeter||0).toFixed(1);$('inmpLevelText').textContent=(s.inmp||0)+'%';if(s.voiceStatus)$('speechStatus').textContent=String(s.voiceStatus).toUpperCase();if(s.voiceTranscript&&s.voiceTranscript!==lastServerTranscript){lastServerTranscript=s.voiceTranscript;setHeardText(s.voiceTranscript,false);$('speechLive').textContent='GemBot: '+s.voiceTranscript;addSpeechLine(s.voiceTranscript,'GemBot')}let batVal=s.battery!==undefined?s.battery:100;$('batText').textContent=batVal+'%';$('batBar').style.width=batVal+'%';let face=(exprPreview['M'+s.expr]?.[0])||'●‿●';if(s.angry)face='◣︵◢';else if(s.cry)face='╥︵╥';else if(s.laugh)face='＞▽＜';else if(s.love)face='♥‿♥';else if(s.sleep)face='—‿—';else if(s.surprised)face='O▽O';else if(s.dizzy||s.woozy)face='@﹏@';const sensorActive=s.angry||s.cry||s.laugh||s.love||s.sleep||s.surprised||s.dizzy||s.woozy||s.expr!==undefined;if(s.musicVolume!==undefined&&document.activeElement!==$('volLoveStory')){const v=Math.max(4,Math.min(55,Math.round((Number(s.musicVolume||0)/12)*51)+4));$('volLoveStory').value=v;$('volLoveStory').nextElementSibling.textContent=v}if(s.musicPlaying!==undefined){musicPaused=!!s.musicPaused;setPlayIcon(s.musicPlaying&&!s.musicPaused?'pause':'play');if(s.musicTrack&&currentFile.startsWith('DFP:')){currentFile='DFP:'+s.musicTrack;const song=[...document.querySelectorAll('.song')].find(x=>x.dataset.file===currentFile);if(song){document.querySelectorAll('.song').forEach(x=>x.classList.remove('active'));song.classList.add('active');$('songTitle').textContent=song.querySelector('strong').textContent;if($('songArtist'))$('songArtist').textContent=song.querySelector('small').textContent}}}if(sensorActive||Date.now()>manualFaceUntil)$('faceText').textContent=face}catch(e){$('connBadge').innerHTML='<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="margin-right:6px;vertical-align:-2px"><path d="M12 2v20M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6"/></svg> Server error';$('connBadge').style.background='#8e2940';$('connBadge').style.borderColor='#8e2940'}}setInterval(refreshSensors,500);refreshSensors();
    async function refreshAiLimit(){try{const r=await fetch('/api/ai-limit');const s=await r.json();$('aiLimitBadge').textContent='AI '+s.used+'/'+s.limit+' sisa '+s.remaining;$('aiKeyBadge').textContent=s.enabled?'Siap':'Belum siap'}catch{}}refreshAiLimit();setInterval(refreshAiLimit,5000);
    function addSpeechLine(text,tag='GemBot'){if(!text)return;const line=document.createElement('div');line.style.cssText='margin:6px 0;padding:8px 10px;border-radius:10px;background:#eef6fb;color:#172435;font-weight:900';line.textContent=tag+': '+text;$('speechLog').prepend(line)}function setHeardText(text,interim=false){$('heardText').textContent=text||'Belum ada suara';$('heardText').style.opacity=interim?.65:1;$('heardText').style.color=interim?'#52677a':'#172435'}$('startSpeech').onclick=async()=>{try{$('speechStatus').textContent='MENDENGAR';setHeardText('Silakan bicara ke GemBot...',true);const r=await fetch('/voice/start',{method:'POST'});setStatus(await r.text(),!r.ok)}catch(e){setStatus(e.message,true)}};$('stopSpeech').onclick=async()=>{try{$('speechStatus').textContent='MEMAHAMI';const r=await fetch('/voice/stop',{method:'POST'});setStatus(await r.text(),!r.ok)}catch(e){setStatus(e.message,true)}};
    ${floatingChatbotJs()}
  </script></body></html>`;
}

const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store, no-cache, must-revalidate" });
    res.end(pageHtml());
    return;
  }
  if (req.method === "GET" && req.url === "/control") {
    res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store, no-cache, must-revalidate" });
    res.end(controlPageHtml());
    return;
  }
  if (req.method === "GET" && req.url === "/logs") {
    res.writeHead(200, { "Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store" });
    res.end(logs.join("\\n"));
    return;
  }
  if (req.method === "GET" && req.url === "/api/sensors") {
    const health = getOwiHealth();
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify({ ...latestTelemetry, ...health, ...latestSpeech }));
    return;
  }
  if (req.method === "GET" && req.url === "/api/ai-limit") {
    res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
    res.end(JSON.stringify(getAiLimitStatus()));
    return;
  }
  
  if (req.method === "POST" && req.url === "/api/speak") {
    let body = "";
    req.on("data", chunk => body += chunk);
    req.on("end", async () => {
      try {
        const data = JSON.parse(body);
        const text = data.text || "Halo";
        logEvent("Test speak: " + text);
        const wavPath = await synthesizeSpeechFile(text);
        if (wavPath && fs.existsSync(wavPath)) {
            streamAudio(latestTelemetry?.ip, "1.0", wavPath);
            res.writeHead(200, { "Content-Type": "application/json" });
            res.end(JSON.stringify({ success: true, text }));
        } else {
            res.writeHead(500);
            res.end(JSON.stringify({ error: "Failed to synthesize" }));
        }
      } catch (err) {
        res.writeHead(500);
        res.end(JSON.stringify({ error: err.message }));
      }
    });
    return;
  }

  if (req.method === "POST" && req.url === "/api/chat") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        const userMsg = sanitizeOledText(data.message || "Halo");
        const speak = data.speak !== false;
        const voiceVolume = clampVolume(data.voiceVolume, 0.85).toFixed(2);
        const dhtReply = parseDhtIntent(userMsg);
        if (dhtReply) {
          let oledSent = true;
          let oledError = "";
          try {
            await sendChatText(dhtReply);
          } catch (serialErr) {
            oledSent = false;
            oledError = serialErr.message;
            logEvent(`chat oled err: ${oledError}`);
          }
          let speechSent = false;
          let speechError = "";
          if (speak) {
            try {
              await speakReplyOnBot(dhtReply, voiceVolume);
              speechSent = true;
            } catch (speechErr) {
              speechError = speechErr.message;
              logEvent(`chat tts err: ${speechError}`);
            }
          }
          res.writeHead(200, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ response: dhtReply, provider: "LocalSensor", model: "DHT22", oledSent, oledError, speechSent, speechError, ai: getAiLimitStatus() }));
          return;
        }
        const musicIntent = parseDfMusicIntent(userMsg);
        if (musicIntent) {
          const reply = await executeDfMusicIntent(musicIntent, Math.round(Number(voiceVolume) * 30));
          let oledSent = true;
          let oledError = "";
          try {
            await sendChatText(reply);
          } catch (serialErr) {
            oledSent = false;
            oledError = serialErr.message;
            logEvent(`chat oled err: ${oledError}`);
          }
          let speechSent = false;
          let speechError = "";
          if (speak) speechError = "Musik langsung tampil di layar, suara balasan dilewati.";
          res.writeHead(200, { "Content-Type": "application/json" });
          res.end(JSON.stringify({ response: reply, provider: "LocalCommand", model: "DFPlayer", oledSent, oledError, speechSent, speechError, ai: getAiLimitStatus() }));
          return;
        }
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
        }

        let speechSent = false;
        let speechError = "";
        if (speak) {
          try {
            await speakReplyOnBot(reply, voiceVolume);
            speechSent = true;
          } catch (speechErr) {
            speechError = speechErr.message;
            logEvent(`chat tts err: ${speechError}`);
          }
        }
        
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ response: reply, provider: aiReply.provider, model: aiReply.model, oledSent, oledError, speechSent, speechError, ai: getAiLimitStatus() }));
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
  if (req.method === "POST" && req.url === "/stop_audio") {
    try {
      requireOwiSocket().send("AUDIO:STOP");
      res.end("Audio dihentikan!");
    } catch (err) {
      res.writeHead(503);
      res.end(err.message);
    }
    return;
  }
  if (req.method === "POST" && req.url === "/play_audio") {
    let vol = "0.30";
    let file = "lovestory.mp3";
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        if (data.volume) vol = data.volume;
        if (data.file) file = data.file;
      } catch(e) {}
      
      if (isStreamingAudio) {
        res.writeHead(400); res.end("Sedang stream");
        return;
      }
      try {
        await streamAudioToWS(owiSocket, file, vol);
        res.end(`Memutar ${file}`);
      } catch (err) {
        res.writeHead(503);
        res.end(err.message);
      }
    });
    return;
  }
  if (req.method === "POST" && req.url === "/test_max") {
    let vol = "0.35";
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        if (data.volume) vol = data.volume;
      } catch(e) {}

      if (isStreamingAudio) {
        res.writeHead(400); res.end("Masih ada audio berjalan");
        return;
      }
      try {
        await streamAudioToWS(owiSocket, "tts_test.mp3", vol);
          res.end("Test MAX: audio dikirim");
      } catch (err) {
        res.writeHead(503);
        res.end(err.message);
      }
    });
    return;
  }
  if (req.method === "POST" && req.url === "/dfplayer") {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", async () => {
      try {
        const data = JSON.parse(Buffer.concat(chunks).toString() || "{}");
        await sendDfPlayer(data.action, data.track, data.volume);
        res.end(`DFPlayer ${String(data.action || "").toUpperCase()} terkirim`);
      } catch (err) {
        res.writeHead(500);
        res.end(err.message);
      }
    });
    return;
  }
  if (req.method === "POST" && req.url.startsWith("/cmd/")) {
    const cmd = decodeURIComponent(req.url.slice("/cmd/".length)).slice(0, 8);
    sendCommand(cmd).then(() => res.end("Command " + cmd + " terkirim")).catch((err) => {res.writeHead(500);res.end(err.message);});
    return;
  }
  if (req.method === "POST" && req.url === "/voice/start") {
    try {
      requireOwiSocket().send("CMD:VOICE:START");
      latestSpeech.voiceStatus = "starting";
      latestSpeech.voiceTranscript = "";
      latestSpeech.voiceReply = "";
      latestSpeech.voiceUpdatedAt = Date.now();
      res.end("GemBot mulai mendengarkan");
    } catch (err) {
      res.writeHead(500);
      res.end(err.message);
    }
    return;
  }
  if (req.method === "POST" && req.url === "/voice/stop") {
    try {
      requireOwiSocket().send("CMD:VOICE:STOP");
      latestSpeech.voiceStatus = "thinking";
      latestSpeech.voiceUpdatedAt = Date.now();
      res.end("Suara dikirim");
    } catch (err) {
      res.writeHead(500);
      res.end(err.message);
    }
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
            await sendReminderSchedule(data.time, data.text, data.date, data.active);
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
      if (body.length !== 9600) {res.writeHead(400);res.end("Frame harus 9600 byte");return}
      try {
        const framePayload = body;
        await sendToSerial(framePayload);
        res.end("Terkirim ke layar");
      } catch (err) {
        res.writeHead(500);
        res.end(err.message);
      }
    });
    return;
  }
  if (req.method === "POST" && req.url === "/draw_cmds") {
    let body = "";
    req.on('data', chunk => body += chunk.toString());
    req.on('end', () => {
        try {
            requireOwiSocket().send("CMD:DRW:" + body);
            res.writeHead(200);
            res.end("OK");
        } catch(e) {
            res.writeHead(500);
            res.end(e.message);
        }
    });
    return;
  }
  res.writeHead(404);res.end("Not found");
});

const wss = new WebSocket.Server({ server });
wss.on('connection', (ws) => {
  logEvent("Connected via WebSocket");
  ws.on('message', (message, isBinary) => {
    try {
      if (isBinary) {
        const buffer = Buffer.isBuffer(message) ? message : Buffer.from(message);
        // ESP32 sends raw audio
        const session = voiceSessions.get(ws);
        if (session && !session.processing) {
          session.chunks.push(buffer);
          session.bytes += buffer.length;
        }
        return;
      }
      const text = message.toString();
      
      if (text.startsWith("CMD:PLAY:")) {
          const track = text.split(":")[2];
          logEvent("GemBot requested play track " + track);
          const file = (track === "1") ? "mbg.mp3" : "lovestory.mp3";
          if (!isStreamingAudio && owiSocket) {
             streamAudioToWS(owiSocket, file, "0.30").catch(e => logEvent("Stream err: " + e.message));
          }
          return;
      } else if (text === "CMD:TEST_MAX") {
          logEvent("GemBot requested TEST_MAX");
          if (!isStreamingAudio && owiSocket) {
             streamAudioToWS(owiSocket, "tts_test.mp3", "0.99").catch(e => logEvent("Test stream err: " + e.message));
          }
          return;
      }

      let parsed;
      try {
        parsed = JSON.parse(text);
      } catch(e) {
        // Not JSON
        return;
      }

      if (parsed.type === "auth" && parsed.role === "owibot") {
         logEvent("GemBot Authenticated");
         owiSocket = ws;
         pushSavedReminderPayload(ws);
      } else if (parsed.event === "start_record") {
         voiceSessions.set(ws, { chunks: [], bytes: 0, sampleRate: 16000, processing: false });
         latestSpeech.voiceStatus = "listening";
         logEvent("Voice start record");
      } else if (parsed.event === "stop_record") {
         latestSpeech.voiceStatus = "thinking";
         logEvent("Voice stop record, thinking...");
         handleVoiceSession(ws).catch(e => logEvent(e.message));
      } else if (parsed.type === "telemetry") {
         latestTelemetry = parsed;
         latestTelemetry.lastUpdate = Date.now();
      }
    } catch(e) {
      logEvent("WS Message Error: " + e.message);
    }
  });
  ws.on('close', () => {
    logEvent("WebSocket Disconnected");
    if (owiSocket === ws) owiSocket = null;
  });
});

const oldHandler = server.listeners('request')[0];
// We keep the old stream logic for browser testing if needed

server.removeAllListeners('request');
server.on('request', (req, res) => {
  const parsedUrl = url.parse(req.url, true);
  if (req.method === "GET" && parsedUrl.pathname === "/stream") {
    const file = parsedUrl.query.file || "lovestory.mp3";
    const vol = parsedUrl.query.vol || "0.30";
    const isTest = file === "TEST";
    logEvent(`stream request ${file} vol ${vol}`);
    
    res.writeHead(200, {
      "Content-Type": "audio/x-raw",
      "Transfer-Encoding": "chunked",
      "Cache-Control": "no-store"
    });
    
    if (isTest) {
      const sampleRate = 16000;
      const durationMs = 1800;
      const frequency = 880;
      const frames = Math.floor(sampleRate * durationMs / 1000);
      const safeVolume = clampVolume(vol, 0.35);
      (async () => {
        try {
          const chunkFrames = 256;
          for (let frame = 0; frame < frames; frame += chunkFrames) {
            if (res.destroyed || res.writableEnded) return;
            const n = Math.min(chunkFrames, frames - frame);
            const chunk = Buffer.alloc(n * 2);
            for (let i = 0; i < n; i++) {
              const pos = frame + i;
              const t = pos / sampleRate;
              const envelope = Math.min(1, Math.min(pos / 1200, (frames - pos) / 1200));
              const sample = Math.round(Math.sin(2 * Math.PI * frequency * t) * 26000 * safeVolume * envelope);
              chunk.writeInt16LE(sample, i * 2);
            }
            if (!res.write(chunk)) await new Promise((resolve) => res.once("drain", resolve));
            await sleep((n * 1000 / sampleRate) * 0.85);
          }
        } catch (err) {
          logEvent(`test stream err: ${err.message}`);
        } finally {
          if (!res.destroyed && !res.writableEnded) res.end();
        }
      })();
    } else {
      let inputFile;
      try {
        inputFile = resolveAudioPath(file);
      } catch (err) {
        res.destroy(err);
        return;
      }
      const ffmpeg = spawn(ffmpegPath, [
        "-hide_banner",
        "-loglevel", "error",
        "-i", inputFile,
        "-f", "s16le",
        "-acodec", "pcm_s16le",
        "-ac", "1",
        "-ar", "16000",
        "-filter:a", `highpass=f=120,lowpass=f=6500,volume=${clampVolume(vol).toFixed(2)}`,
        "pipe:1"
      ], { stdio: ["ignore", "pipe", "pipe"] });
      req.on("close", () => {
        if (!ffmpeg.killed) ffmpeg.kill("SIGKILL");
      });
      ffmpeg.stderr.on("data", (chunk) => {
        const line = chunk.toString().trim();
        if (line) logEvent(`ffmpeg: ${line.slice(0, 120)}`);
      });
      (async () => {
        try {
          await writePacedPcm(ffmpeg.stdout, res, 16000, 8192, 512);
        } catch (err) {
          logEvent(`stream pacing err: ${err.message}`);
        } finally {
          if (!ffmpeg.killed) ffmpeg.kill("SIGKILL");
          if (!res.destroyed && !res.writableEnded) res.end();
        }
      })();
    }
    return;
  }
  oldHandler(req, res);
});

server.listen(PORT, () => {
  console.log("Web: http://localhost:" + PORT);
  console.log("Serial: " + SERIAL_PORT + " @ " + BAUD);
});

