#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <driver/gpio.h>
#include <math.h>
#include <DHT.h>

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>
#include "freertos/ringbuf.h"
#include "freertos/stream_buffer.h"
#include "secrets.h"

#include <HardwareSerial.h>
#include <DFRobotDFPlayerMini.h>

HardwareSerial mySoftwareSerial(2); // Use UART2
DFRobotDFPlayerMini myDFPlayer;


WebSocketsClient webSocket;
HTTPClient audioHttp;
WiFiClient* audioStream = nullptr;
Preferences reminderPrefs;

volatile bool voiceRecording = false;
bool isDrawMode = false;

enum AppState {
  APP_FACE,
  APP_MENU,
  APP_PINGPONG,
  APP_SUHU,
  APP_PENGINGAT,
  APP_DRAW,
  APP_MUSIK,
  APP_LISTENING
};
AppState currentState = APP_FACE;

unsigned long voiceStartedMs = 0;
unsigned long voiceLastSpeechMs = 0;
bool voiceHeardSpeech = false;
StreamBufferHandle_t micStreamBuf = NULL;
const size_t MIC_STREAM_SIZE = 32768;
unsigned long lastMicSendMs = 0;

TFT_eSPI display = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&display);
float voiceLevel = 0.0f;
volatile float micPeakLevel = 0.0f;
volatile float micRawRms = 0.0f;
volatile bool micReady = false;
volatile uint32_t micReadErrors = 0;
volatile uint8_t micSelectedSlot = 0;

RingbufHandle_t audioRingBuf = NULL;
const size_t RING_BUF_SIZE = 98304;
const float AI_OUTPUT_GAIN = 1.25f;
const int32_t AI_OUTPUT_SOFT_LIMIT = 30000;
const size_t TTS_PREBUFFER_BYTES = 24000;
const unsigned long TTS_PREBUFFER_MAX_MS = 2000UL;

void playBeep(float freq, int duration_ms);
void dhtTask(void *pvParameters);

// GemBot2 ESP32-S3 DevKitC-1 pin map.
// TFT ILI9341 pins are defined in platformio.ini:
// MISO=13, MOSI=11, SCLK=12, CS=10, DC=9, RST=14.
// Avoid USB pins 19/20, boot strap pins 0/45/46, and TFT pins 9-14.
#define I2S_BCLK 15
#define I2S_LRC 16
#define I2S_DOUT 17

#define I2S_MIC_SCK 4
#define I2S_MIC_WS 5
#define I2S_MIC_SD 6

#define DFPLAYER_RX 18 // from DFPlayer TX
#define DFPLAYER_TX 21 // from DFPlayer RX

#define I2C_SDA 1
#define I2C_SCL 2

// Touch Sensor Pin
#define TOUCH_PIN 7
bool lastTouchState = false;

extern unsigned long nodUntilMs;
extern unsigned long curiousUntilMs;
void handleTouchAction(bool isHold);
void handleTouchRelease();
void startVoiceRecording();
void stopVoiceRecording();
void enterDrawMode(uint16_t bg);
void exitDrawMode();
bool isAiOutputActive();
bool isVoiceOrAudioBusy();
void updateAiSpeakingState();
void clearMicStream();
void resetPingPong();
void handleDfPlayerCommand(const String& text);
void handleReminderMessage(const String& text);
void playDfTrack(int track);
void drawNowPlaying();
void loadRemindersFromFlash();
void saveRemindersToFlash();
bool ensureSpriteReady();
void pushSpriteSafe();
void recoverDisplayIfNeeded();
void applyDfVolume(bool force = false);
void waitDfCommandGap();
unsigned long touchStartTime = 0;
bool touchHandled = false;

// DHT22 Pin
#define DHTPIN 8
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
float currentSuhu = 0.0f;
float currentLembap = 0.0f;
unsigned long lastDhtRead = 0;

// Pingpong game variables
float ballX = 120.0f, ballY = 160.0f;
float ballVX = 3.0f, ballVY = -4.0f;
float paddleX = 120.0f;
float aiPaddleX = 120.0f;
int score = 0;
int scoreAI = 0;
bool gameOver = false;
bool playerWon = false;

float currentToneFreq = 0.0f;
unsigned long toneEndMs = 0;
uint audioBytesRead = 0;
unsigned long lastAudioPlayedMs = 0;
bool aiSpeaking = false;
unsigned long aiSpeakingUntilMs = 0;
volatile float ttsMouthLevel = 0.0f;
volatile bool audioPrebuffering = false;
volatile unsigned long audioPrebufferStartedMs = 0;
volatile uint32_t wsAudioPackets = 0;
volatile uint32_t wsAudioBytes = 0;
unsigned long lastWsAudioDebugMs = 0;

int currentExpressionId = -1;
int displayedExpressionId = 0;
int sensorExpressionId = -1;
unsigned long manualExpressionUntilMs = 0;
unsigned long sensorExpressionUntilMs = 0;

String currentChatText = "";
int chatTextX = 240;
bool isChatActive = false;
bool isWsConnected = false;
unsigned long lastTelemetryMs = 0;
unsigned long lastScreenDrawMs = 0;
const uint16_t UI_FRAME_MS = 45;
unsigned long lastDisplayPushMs = 0;
unsigned long lastDisplayRecoverMs = 0;

bool dfPlayerReady = false;
bool dfMusicPlaying = false;
bool dfMusicPaused = false;
int dfCurrentTrack = 0;
int dfVolume = 18;
int dfAppliedVolume = -1;
unsigned long lastDfCommandMs = 0;
const int DFPLAYER_MAX_CLEAN_VOLUME = 20;
unsigned long dfTrackStartedMs = 0;
unsigned long dfPausedAtMs = 0;
const int dfTrackCount = 4;
const char* dfTrackTitles[dfTrackCount + 1] = {
  "", "Love Story", "MBG", "YELLOW", "REDRED"
};
const char* dfTrackArtists[dfTrackCount + 1] = {
  "", "Taylor Swift", "Local MP3", "Coldplay", "CORTIS"
};
const uint16_t dfTrackDurationSec[dfTrackCount + 1] = {
  0, 234, 15, 269, 163
};

const int MAX_REMINDERS = 5;
struct ReminderItem {
  char time[6];
  char date[11];
  char text[33];
  bool active;
  int lastTriggeredYDay;
};
ReminderItem reminderItems[MAX_REMINDERS];
int reminderCount = 0;
unsigned long reminderSyncedMs = 0;
unsigned long reminderScreenUntilMs = 0;
unsigned long reminderAlertUntilMs = 0;
unsigned long lastReminderCheckMs = 0;
unsigned long reminderLastBeepMs = 0;
int reminderAlertIndex = -1;
AppState reminderReturnState = APP_FACE;


void audioTask(void *pvParameters) {
  int16_t buffer[512];
  size_t bytes_written;
  static int tone_i = 0;
  static float lastToneFreq = 0;
  
  while (true) {
    if (millis() < toneEndMs && currentToneFreq > 0) {
      if (currentToneFreq != lastToneFreq) {
        tone_i = 0;
        lastToneFreq = currentToneFreq;
      }
      for (int k = 0; k < 512; k+=2) {
        int16_t sample16 = (int16_t)(sin(2 * PI * currentToneFreq * tone_i / 16000.0) * 8000);
        buffer[k] = sample16;
        buffer[k+1] = sample16;
        tone_i++;
      }
      i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
    } else {
       lastToneFreq = 0;
       if (audioPrebuffering) {
           size_t bufferedBytes = RING_BUF_SIZE - xRingbufferGetCurFreeSize(audioRingBuf);
           if (bufferedBytes < TTS_PREBUFFER_BYTES && millis() - audioPrebufferStartedMs < TTS_PREBUFFER_MAX_MS) {
               vTaskDelay(pdMS_TO_TICKS(4));
               continue;
           }
           audioPrebuffering = false;
       }
       size_t item_size = 0;
       void *data = xRingbufferReceive(audioRingBuf, &item_size, pdMS_TO_TICKS(30));
       if (data != NULL) {
           int16_t *mono = (int16_t*)data;
           int samples = item_size / 2;
           uint64_t levelSum = 0;
           for (int i = 0; i < samples; i++) levelSum += abs((int32_t)mono[i]);
           if (samples > 0) {
               float playedLevel = constrain(((float)levelSum / samples) / 3000.0f, 0.0f, 1.5f);
               ttsMouthLevel = ttsMouthLevel * 0.40f + playedLevel * 0.60f;
           }
           int k = 0;
           for (int i=0; i<samples; i++) {
               int32_t outputSample = mono[i];
               if (aiSpeaking) {
                   outputSample = (int32_t)(outputSample * AI_OUTPUT_GAIN);
                   if (outputSample > AI_OUTPUT_SOFT_LIMIT) {
                       outputSample = AI_OUTPUT_SOFT_LIMIT + (outputSample - AI_OUTPUT_SOFT_LIMIT) / 8;
                   } else if (outputSample < -AI_OUTPUT_SOFT_LIMIT) {
                       outputSample = -AI_OUTPUT_SOFT_LIMIT + (outputSample + AI_OUTPUT_SOFT_LIMIT) / 8;
                   }
               }
               int16_t amplified = (int16_t)constrain(outputSample, -32767L, 32767L);
               buffer[k++] = amplified; // left channel
               buffer[k++] = amplified; // right channel
               if (k >= 512) {
                   i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
                   lastAudioPlayedMs = millis();
                   k = 0;
               }
           }
           if (k > 0) {
               i2s_write(I2S_NUM_0, buffer, k * sizeof(int16_t), &bytes_written, portMAX_DELAY);
               lastAudioPlayedMs = millis();
           }
           vRingbufferReturnItem(audioRingBuf, data);
       } else {
           ttsMouthLevel *= 0.80f;
           // Manually write silence to prevent DMA looping bugs in ESP-IDF
           memset(buffer, 0, sizeof(buffer));
           i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
       }
    }
  }
}

void clearAudioRingBuffer() {
  if (audioRingBuf == NULL) return;
  size_t itemSize = 0;
  void *item = nullptr;
  while ((item = xRingbufferReceive(audioRingBuf, &itemSize, 0)) != nullptr) {
    vRingbufferReturnItem(audioRingBuf, item);
  }
}

void drawScreen();
void handleTouch();

void startAudioStream(String file, String vol) {
  audioHttp.end();
  audioStream = nullptr;
  String url = "http://" + String(WS_HOST) + ":" + String(WS_PORT) + "/stream?file=" + file + "&vol=" + vol;
  Serial.println("Streaming audio: " + url);
  audioHttp.begin(url);
  int httpCode = audioHttp.GET();
  if (httpCode == HTTP_CODE_OK) {
      audioStream = audioHttp.getStreamPtr();
      xRingbufferReceiveUpTo(audioRingBuf, NULL, 0, 0); // clear ringbuffer
  } else {
      audioHttp.end();
  }
}

void readAudioStream() {
  if (!audioStream) return;
  if (!audioStream->connected()) {
      audioHttp.end();
      audioStream = nullptr;
      return;
  }
  int avail = audioStream->available();
  if (avail > 0) {
     uint8_t readBuffer[512];
     int freeSpace = xRingbufferGetCurFreeSize(audioRingBuf);
     int want = min(avail, 512);
     if (want > freeSpace) want = freeSpace;
     if (want > 0) {
         int got = audioStream->read(readBuffer, want);
         if (got > 0) {
             xRingbufferSend(audioRingBuf, readBuffer, got, pdMS_TO_TICKS(10));
         }
     }
  }
}

bool isAiOutputActive() {
  unsigned long now = millis();
  bool recentlyPlayed = lastAudioPlayedMs > 0 && (now - lastAudioPlayedMs) < 1200UL;
  return aiSpeaking || audioPrebuffering || now < aiSpeakingUntilMs || recentlyPlayed;
}

bool isVoiceOrAudioBusy() {
  return voiceRecording || currentState == APP_LISTENING || isAiOutputActive();
}

void updateAiSpeakingState() {
  unsigned long now = millis();
  if (aiSpeaking && now > aiSpeakingUntilMs && (now - lastAudioPlayedMs) > 700UL) {
    aiSpeaking = false;
    audioPrebuffering = false;
  }
}

bool ensureSpriteReady() {
  if (spr.getPointer() != nullptr) return true;
  spr.deleteSprite();
  spr.setColorDepth(8);
  spr.createSprite(240, 320);
  if (spr.getPointer() == nullptr) {
    Serial.println("[TFT] Sprite allocation failed");
    return false;
  }
  Serial.printf("[TFT] Sprite recovered: %p depth:%d\n", spr.getPointer(), spr.getColorDepth());
  return true;
}

void pushSpriteSafe() {
  if (!ensureSpriteReady()) {
    unsigned long now = millis();
    if (now - lastDisplayRecoverMs > 2000UL) {
      lastDisplayRecoverMs = now;
      display.fillScreen(TFT_BLACK);
    }
    return;
  }
  display.startWrite();
  spr.pushSprite(0, 0);
  display.endWrite();
  lastDisplayPushMs = millis();
}

void recoverDisplayIfNeeded() {
  unsigned long now = millis();
  if (now - lastDisplayRecoverMs < 3000UL) return;
  if (spr.getPointer() != nullptr) return;

  lastDisplayRecoverMs = now;
  Serial.println("[TFT] Recovering sprite (no full reinit)");
  display.fillScreen(TFT_BLACK);
  ensureSpriteReady();
  lastScreenDrawMs = 0;
}

void enterDrawMode(uint16_t bg) {
  isDrawMode = true;
  currentState = APP_DRAW;
  currentChatText = "";
  isChatActive = false;
  spr.fillSprite(bg);
  pushSpriteSafe();
  display.fillScreen(bg);
}

void exitDrawMode() {
  isDrawMode = false;
  currentState = APP_FACE;
  currentExpressionId = -1;
  manualExpressionUntilMs = 0;
  display.fillScreen(TFT_BLACK);
  lastScreenDrawMs = 0;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  lastInteractionMs = millis();
  if (type == WStype_CONNECTED) {
    isWsConnected = true;
    Serial.printf("[WS] Connected to url: %s\n", payload);
    webSocket.sendTXT("{\"type\":\"auth\",\"role\":\"owibot\"}");
  } else if (type == WStype_TEXT) {
    String text = (char*)payload;
    if (text.startsWith("AUDIO:")) {
       voiceRecording = false;
       clearMicStream();
       audioPrebuffering = true;
       audioPrebufferStartedMs = millis();
       int firstColon = text.indexOf(':', 6);
       if (firstColon != -1) {
          String file = text.substring(6, firstColon);
          String vol = text.substring(firstColon + 1);
          startAudioStream(file, vol);
       } else {
          startAudioStream(text.substring(6), "0.50");
       }
    } else if (text == "VOICE:SPEAKING") {
       clearAudioRingBuffer();
       voiceRecording = false;
       clearMicStream();
       aiSpeaking = true;
       aiSpeakingUntilMs = millis() + 15000UL;
       ttsMouthLevel = 0.0f;
       audioPrebuffering = true;
       audioPrebufferStartedMs = millis();
       if (currentState != APP_DRAW && currentState != APP_PINGPONG && currentState != APP_MUSIK) currentState = APP_FACE;
     } else if (text == "VOICE:DONE") {
        aiSpeakingUntilMs = millis() + 1800UL;
     } else if (text == "VOICE:THINKING") {
        if (!voiceRecording) {
          currentState = APP_FACE;
          currentFace = 14; // Focus face
          targetFace = 14;
          manualExpressionUntilMs = millis() + 5000UL;
          currentChatText = "GemBot lagi mikir...";
          chatTextX = 240;
          isChatActive = true;
          // Play a short SFX
          setDfVolume(18);
          waitDfCommandGap();
          myDFPlayer.playMp3Folder(2);
        }
     } else if (text.startsWith("VOICE:ERROR")) {
        if (!voiceRecording) {
          currentState = APP_FACE;
          currentChatText = "Suara belum jelas, coba ulangi";
          chatTextX = 240;
          isChatActive = true;
        }
     } else if (text.startsWith("REMINDER:")) {
        handleReminderMessage(text);
     } else if (text.startsWith("CMD:DFP:")) {
        handleDfPlayerCommand(text);
     } else if (text.startsWith("CMD:VOL:")) {
        setDfVolume(text.substring(8).toInt());
     } else if (text.startsWith("CMD:W")) {
        uint16_t bg = TFT_BLACK;
        int bgSep = text.indexOf(':', 5);
        if (bgSep > 0) {
          bg = (uint16_t)text.substring(bgSep + 1).toInt();
        }
        enterDrawMode(bg);
    } else if (text.startsWith("CMD:DRW:")) {
        if (!isDrawMode || currentState != APP_DRAW) enterDrawMode(TFT_BLACK);
        int start = 8;
        while (start < (int)text.length()) {
          int end = text.indexOf('|', start);
          if (end < 0) end = text.length();
          String cmd = text.substring(start, end);
          int p1 = cmd.indexOf(',');
          int p2 = cmd.indexOf(',', p1 + 1);
          int p3 = cmd.indexOf(',', p2 + 1);
          int p4 = cmd.indexOf(',', p3 + 1);
          int p5 = cmd.indexOf(',', p4 + 1);
          if (p1 > 0 && p2 > p1 && p3 > p2 && p4 > p3 && p5 > p4) {
            int x1 = constrain(cmd.substring(0, p1).toInt(), 0, 239);
            int y1 = constrain(cmd.substring(p1 + 1, p2).toInt(), 0, 319);
            int x2 = constrain(cmd.substring(p2 + 1, p3).toInt(), 0, 239);
            int y2 = constrain(cmd.substring(p3 + 1, p4).toInt(), 0, 319);
            uint16_t color = (uint16_t)cmd.substring(p4 + 1, p5).toInt();
            int brush = constrain(cmd.substring(p5 + 1).toInt(), 1, 20);
            display.drawLine(x1, y1, x2, y2, color);
            if (brush > 1) {
              display.fillCircle(x2, y2, brush / 2, color);
            }
          }
          start = end + 1;
        }
    } else if (text == "CMD:VOICE:START") {
       startVoiceRecording();
    } else if (text == "CMD:VOICE:STOP") {
       stopVoiceRecording();
    } else if (text.startsWith("CMD:T:")) {
         currentChatText = text.substring(6);
         chatTextX = 240;
         isChatActive = true;
    } else if (text.startsWith("CMD:M")) {
       isDrawMode = false;
       String idStr = text.substring(5);
       currentExpressionId = idStr.toInt();
       manualExpressionUntilMs = millis() + 10000UL;
    } else if (text == "CMD:C" || text == "CMD:CLEAR") {
       exitDrawMode();
    } else if (text == "CMD:P") {
       handleTouchAction(false);
    } else if (text == "CMD:O") {
       handleTouchAction(true);
    } else if (text == "CMD:G") {
       currentState = APP_PINGPONG;
       resetPingPong();
    } else if (text == "CMD:D") {
       nodUntilMs = millis() + 1200;
       currentExpressionId = 51; // Haha
       manualExpressionUntilMs = millis() + 5000UL;
    } else if (text == "CMD:E") {
       curiousUntilMs = millis() + 1000;
       currentExpressionId = 50; // Love
       manualExpressionUntilMs = millis() + 5000UL;
      } else if (text == "CMD:F") {
         static int rot = 0;
         rot = (rot == 0) ? 2 : 0;
         display.setRotation(rot);
      }
  } else if (type == WStype_BIN) {
      if (length == 9606 && memcmp(payload, "FRAME:", 6) == 0) {
          if (isDrawMode) {
              spr.drawBitmap(0, 0, payload + 6, 240, 320, TFT_WHITE, TFT_BLACK);
              pushSpriteSafe();
          }
      } else if (length > 0) {
          // Raw PCM chunk from the server (Test Tone / TTS)
          aiSpeaking = true;
          aiSpeakingUntilMs = millis() + 1800UL;
          if (currentState != APP_DRAW && currentState != APP_PINGPONG && currentState != APP_MUSIK) currentState = APP_FACE;
          
          // Try to send with a slightly longer timeout to act as backpressure
          BaseType_t sent = xRingbufferSend(audioRingBuf, payload, length, pdMS_TO_TICKS(50));
          if (sent != pdTRUE) {
              // Only drop if REALLY full
              size_t oldSize = 0;
              void *oldAudio = xRingbufferReceive(audioRingBuf, &oldSize, 0);
              if (oldAudio != nullptr) vRingbufferReturnItem(audioRingBuf, oldAudio);
              sent = xRingbufferSend(audioRingBuf, payload, length, pdMS_TO_TICKS(10));
          }
          
          if (sent == pdTRUE) {
            wsAudioPackets++;
            wsAudioBytes += length;
          }
          unsigned long now = millis();
          if (now - lastWsAudioDebugMs >= 600UL) {
            lastWsAudioDebugMs = now;
            size_t bufferedBytes = audioRingBuf ? (RING_BUF_SIZE - xRingbufferGetCurFreeSize(audioRingBuf)) : 0;
            Serial.printf("[AUDIO] bin=%u ok=%d packets=%lu bytes=%lu rb=%u\n",
                          (unsigned)length, sent == pdTRUE ? 1 : 0,
                          (unsigned long)wsAudioPackets, (unsigned long)wsAudioBytes,
                          (unsigned)bufferedBytes);
          }
      }
  } else if (type == WStype_DISCONNECTED) {
      isWsConnected = false;
      Serial.println("[WS] Disconnected");
  }
}

  void initI2S() {
    i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_BCLK,
      .ws_io_num = I2S_LRC,
      .data_out_num = I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    xTaskCreatePinnedToCore(audioTask, "AudioTask", 4096, NULL, 2, NULL, 0);
  }

void micTask(void *pvParameters);

  void initMic() {
    i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_MIC_SCK,
      .ws_io_num = I2S_MIC_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SD
    };
    i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pin_config);
    
    xTaskCreatePinnedToCore(micTask, "MicTask", 4096, NULL, 1, NULL, 0);
  }

void micTask(void *pvParameters) {
  micReady = true;

  constexpr int MIC_WORD_COUNT = 128;
  constexpr int MIC_FRAME_COUNT = MIC_WORD_COUNT / 2;
  size_t bytes_read = 0;
  int32_t samples[MIC_WORD_COUNT];
  int32_t filtered[2][MIC_FRAME_COUNT];
  int16_t pcm[MIC_FRAME_COUNT];
  float dcEstimate[2] = {0.0f, 0.0f};
  float slotRmsSmooth[2] = {0.0f, 0.0f};
  unsigned long lastDiagnosticMs = 0;

  while (true) {
    esp_err_t res = i2s_read(I2S_NUM_1, &samples, sizeof(samples), &bytes_read, portMAX_DELAY);
    if (res != ESP_OK) {
      micReadErrors++;
      micReady = false;
      Serial.printf("[MIC] i2s_read error=%d count=%lu\n", res, (unsigned long)micReadErrors);
      vTaskDelay(pdMS_TO_TICKS(100)); // Prevent watchdog reset if failing rapidly
      continue;
    }
    micReady = true;

    int frameCount = bytes_read / sizeof(int32_t); // Mono 32-bit (ONLY_LEFT)
    if (frameCount > MIC_FRAME_COUNT) frameCount = MIC_FRAME_COUNT;

    if (frameCount <= 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    
    if (isAiOutputActive()) {
      voiceLevel *= 0.82f;
      micPeakLevel *= 0.80f;
      micRawRms = 0.0f;
      clearMicStream();
      continue;
    }

    double sumSquares = 0.0;
    int32_t peak = 0;
    for (int frame = 0; frame < frameCount; frame++) {
      // INMP441 sends signed 24-bit audio left-justified in a 32-bit slot.
      int32_t sample24 = samples[frame] >> 8;
      dcEstimate[0] += ((float)sample24 - dcEstimate[0]) * 0.008f;
      int32_t ac = sample24 - (int32_t)dcEstimate[0];
      filtered[0][frame] = ac;
      sumSquares += (double)ac * (double)ac;
      int32_t magnitude = abs(ac);
      if (magnitude > peak) peak = magnitude;
    }

    float rms = sqrtf((float)(sumSquares / frameCount));
    slotRmsSmooth[0] = slotRmsSmooth[0] * 0.88f + rms * 0.12f;
    micRawRms = rms;

    // dBFS produces a useful percentage across quiet speech and loud speech
    const float fullScale24 = 8388607.0f;
    const float rmsDbfs = 20.0f * log10f(max(rms, 1.0f) / fullScale24);
    const float peakDbfs = 20.0f * log10f(max((float)peak, 1.0f) / fullScale24);
    const float level = constrain((rmsDbfs + 68.0f) / 45.0f, 0.0f, 1.0f);
    const float peakLevel = constrain((peakDbfs + 62.0f) / 42.0f, 0.0f, 1.0f);
    const float levelMix = level > voiceLevel ? 0.34f : 0.10f;
    voiceLevel += (level - voiceLevel) * levelMix;
    micPeakLevel = max(peakLevel, micPeakLevel * 0.86f);

    for (int frame = 0; frame < frameCount; frame++) {
      // Convert signed 24-bit audio to 16-bit PCM with extra gain for STT.
      int32_t sample16 = filtered[0][frame] >> 4;
      pcm[frame] = (int16_t)constrain(sample16, -32767, 32767);
    }

    // Keep WebSocket access on the main task; this task only captures clean PCM.
    if (voiceRecording && micStreamBuf != NULL) {
      xStreamBufferSend(micStreamBuf, pcm, frameCount * sizeof(int16_t), 0);
    }

    // If a loud sound is detected (clap, shout), trigger surprised.
    if (level > 0.72f && !voiceRecording) {
      extern unsigned long surprisedUntilMs;
      if (surprisedUntilMs < millis()) surprisedUntilMs = millis() + 900;
    }

    unsigned long now = millis();
    if (!voiceRecording && now - lastDiagnosticMs >= 5000UL) {
      lastDiagnosticMs = now;
      Serial.printf(
        "[MIC] ready=1 rms=%.0f level=%d%% peak=%d%% errors=%lu\n",
        rms, (int)(voiceLevel * 100.0f), (int)(micPeakLevel * 100.0f),
        (unsigned long)micReadErrors
      );
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}


void clearMicStream() {
  if (micStreamBuf == NULL) return;
  uint8_t discard[256];
  while (xStreamBufferReceive(micStreamBuf, discard, sizeof(discard), 0) > 0) {}
}

void sendPendingMicAudio(bool flushAll = false) {
  if (micStreamBuf == NULL || !webSocket.isConnected()) return;
  unsigned long now = millis();
  
  // 1024 bytes = 64ms of audio at 16kHz 8-bit.
  if (!flushAll && xStreamBufferBytesAvailable(micStreamBuf) < 1024) return;

  uint8_t temp[1024];
  int packetLimit = flushAll ? 8 : 1;
  for (int i = 0; i < packetLimit; i++) {
    size_t bytesReceived = xStreamBufferReceive(micStreamBuf, temp, sizeof(temp), 0);
    if (bytesReceived == 0) break;
    webSocket.sendBIN(temp, bytesReceived);
  }
  lastMicSendMs = now;
}

void playBeep(float freq, int duration_ms) {
  currentToneFreq = freq;
  toneEndMs = millis() + duration_ms;
}

// (TFT and sprite instantiated at top)

bool rawMpuMode = false;
uint8_t mpuAddr = 0x68;

uint8_t readReg8(uint8_t addr, uint8_t reg, bool& ok) {
  ok = false;
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((int)addr, 1) != 1) return 0xFF;
  ok = true;
  return Wire.read();
}

void writeReg8(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

int16_t be16(uint8_t h, uint8_t l) {
  return (int16_t)((h << 8) | l);
}

bool readRawMotion(uint8_t addr, float& ax, float& ay, float& az, float& gx, float& gy, float& gz) {
  Wire.beginTransmission(addr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 14) != 14) return false;
  uint8_t b[14];
  for (uint8_t i = 0; i < 14; i++) b[i] = Wire.read();
  
  int16_t rax = be16(b[0], b[1]);
  int16_t ray = be16(b[2], b[3]);
  int16_t raz = be16(b[4], b[5]);
  int16_t rgx = be16(b[8], b[9]);
  int16_t rgy = be16(b[10], b[11]);
  int16_t rgz = be16(b[12], b[13]);

  ax = ((float)rax / 8192.0f) * 9.80665f;
  ay = ((float)ray / 8192.0f) * 9.80665f;
  az = ((float)raz / 8192.0f) * 9.80665f;
  gx = ((float)rgx / 65.5f) * 0.0174533f; // Convert deg/s to rad/s
  gy = ((float)rgy / 65.5f) * 0.0174533f;
  gz = ((float)rgz / 65.5f) * 0.0174533f;
  return true;
}

bool setupRawMPU() {
  for (uint8_t addr : { (uint8_t)0x68, (uint8_t)0x69 }) {
    bool ok = false;
    uint8_t who = readReg8(addr, 0x75, ok);
    Serial.printf("MPU Probe 0x%02X: OK=%d WHO=0x%02X\n", addr, ok, who);
    // Many MPU6050 clones return 0x98, 0x72, or other undocumented values
    if (ok) {
      mpuAddr = addr;
      rawMpuMode = true;
      writeReg8(addr, 0x6B, 0x00);
      delay(50);
      writeReg8(addr, 0x1A, 0x03);
      return true;
    }
  }
  return false;
}

bool mpuReady = false;

// MPU Logic Variables
float currentJerk = 0.0f;
float currentRoll = 0.0f;
float currentPitch = 0.0f;
float currentYaw = 0.0f;

// Gesture States
bool nodDetected = false;
unsigned long nodUntilMs = 0;
bool headShakeDetected = false;
unsigned long headShakeUntilMs = 0;
bool surprisedMode = false;
unsigned long surprisedUntilMs = 0;
unsigned long curiousUntilMs = 0;

void handleTouchAction(bool isHold);
void handleTouchRelease();
bool faceUpMode = false;
bool faceDownMode = false;

// History for gestures
float pitchHistory[4] = {0};
float yawHistory[4] = {0};
uint8_t histIdx = 0;
unsigned long lastHistMs = 0;
float sustainedTiltX = 0.0f;
unsigned long sustainedTiltStartMs = 0;

// Face Drawing Variables
float targetLookX = 0;
float targetLookY = 0;
float targetBob = 0;

float lookX = 0;
float lookY = 0;
float bobY = 0;

unsigned long lastMpuMs = 0;


void updateMPU() {
  unsigned long now = millis();
  uint16_t mpuIntervalMs = (isAiOutputActive() || voiceRecording) ? 60 : 25;
  if (now - lastMpuMs < mpuIntervalMs) return;
  lastMpuMs = now;

  if (!mpuReady) {
    // Try to reconnect every 2 seconds to prevent WDT crash on loose wires
    static unsigned long lastReconnectMs = 0;
    if (now - lastReconnectMs > 2000) {
       lastReconnectMs = now;
       if (setupRawMPU()) {
         mpuReady = true;
       }
    }
    return;
  }

  float ax, ay, az, gx, gy, gz;
  if (!readRawMotion(mpuAddr, ax, ay, az, gx, gy, gz)) {
    mpuReady = false; // Mark disconnected, will try again later
    return;
  }

  float accelNorm = sqrtf(ax * ax + ay * ay + az * az);
  static bool motionReady = false;
  static float lastAx = 0.0f;
  static float lastAy = 0.0f;
  static float lastAz = 0.0f;
  static float gravityNorm = 0.0f;
  if (!motionReady) {
    motionReady = true;
    lastAx = ax;
    lastAy = ay;
    lastAz = az;
    gravityNorm = accelNorm;
  }
  gravityNorm += (accelNorm - gravityNorm) * 0.015f;
  float accelDelta = sqrtf((ax - lastAx) * (ax - lastAx) + (ay - lastAy) * (ay - lastAy) + (az - lastAz) * (az - lastAz));
  lastAx = ax;
  lastAy = ay;
  lastAz = az;
  float jerk = accelDelta * 3.0f + max(0.0f, fabsf(accelNorm - gravityNorm) - 1.8f);
  currentJerk = jerk;

  // Auto-calibrate gyro offset when robot is still
  static float gyroOffsetX = 0.0f, gyroOffsetY = 0.0f, gyroOffsetZ = 0.0f;
  static bool gyroCalibrated = false;
  if (!gyroCalibrated) {
      gyroOffsetX = gx; gyroOffsetY = gy; gyroOffsetZ = gz;
      gyroCalibrated = true;
  }
  if (jerk < 2.0f) {
      gyroOffsetX = gyroOffsetX * 0.98f + gx * 0.02f;
      gyroOffsetY = gyroOffsetY * 0.98f + gy * 0.02f;
      gyroOffsetZ = gyroOffsetZ * 0.98f + gz * 0.02f;
  }
  gx -= gyroOffsetX;
  gy -= gyroOffsetY;
  gz -= gyroOffsetZ;



  // Track History
  if (now - lastHistMs > 100) {
    lastHistMs = now;
    pitchHistory[histIdx] = ay;
    yawHistory[histIdx] = gz;
    histIdx = (histIdx + 1) % 4;
  }

  currentRoll = atan2f(ay, az) * 57.2958f;
  currentPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2958f;
  currentYaw = gz;

  auto reactWithExpression = [&](int expressionId, unsigned long durationMs) {
    if (currentState != APP_FACE || now < manualExpressionUntilMs) return;
    sensorExpressionId = expressionId;
    sensorExpressionUntilMs = max(sensorExpressionUntilMs, now + durationMs);
  };

  // Organic reactions are separate from manual web choices.
  static unsigned long continuousShakeStart = 0;
  static unsigned long continuousSpinStart = 0;
  float gyroSpeed = sqrtf(gx * gx + gy * gy + gz * gz);

  // Sustained rotation becomes dizzy instead of a one-frame shake.
  if (gyroSpeed > 3.8f) {
      if (continuousSpinStart == 0) {
          continuousSpinStart = now;
      }
      else if (now - continuousSpinStart > 550) {
          reactWithExpression(30, 3600);
      }
  } else {
      continuousSpinStart = 0;
  }

  // Continuous shaking escalates from angry to crying.
  if (jerk > 7.2f) {
      if (continuousShakeStart == 0) {
          continuousShakeStart = now;
      }
      else {
          unsigned long shakeDuration = now - continuousShakeStart;
          if (shakeDuration > 2200) reactWithExpression(29, 5000);
          else if (shakeDuration > 650) reactWithExpression(3, 3200);
      }
  } else {
      continuousShakeStart = 0;
  }

  // Smaller movements still make GemBot react without feeling random.
  if (gyroSpeed > 1.05f && gyroSpeed <= 3.8f && jerk < 7.2f) {
    if (fabs(gz) > fabs(gx) && fabs(gz) > fabs(gy)) reactWithExpression(27, 950);
    else if (gy > 0.8f) reactWithExpression(28, 900);
    else if (gy < -0.8f) reactWithExpression(25, 900);
    else reactWithExpression(gx > 0 ? 31 : 26, 900);
  }

  // Tilt/Look calculations

  // Surprise tap (Gravity is ~9.8, so >25 means a very strong tap, prevents random O mouth)
  if (jerk > 12.0f) {
    surprisedUntilMs = now + 900;
    reactWithExpression(4, 1000);
  }

  float tiltX = -ax;
  float tiltY = ay;
  // Increased from 0.65 to 2.5 to prevent random curious expression on slightly uneven tables/breadboards
  if (fabs(tiltX) > 2.5f) { 
    if (sustainedTiltStartMs == 0) sustainedTiltStartMs = now;
    else if (now - sustainedTiltStartMs > 1000) { 
        curiousUntilMs = now + 900;
        // Use more neutral curious expressions (26 = daydream, 14 = focus) instead of 31 (nakal)
        reactWithExpression(tiltX > 0 ? 26 : 14, 1100);
    }
  } else {
    sustainedTiltStartMs = 0;
  }

  // Face Up/Down
  faceUpMode = (az < 5.0f);
  faceDownMode = (az > 14.0f);

  // Nod (pitch change)
  float pMax = pitchHistory[0], pMin = pitchHistory[0];
  for(int i=1;i<4;i++){ if(pitchHistory[i]>pMax) pMax=pitchHistory[i]; if(pitchHistory[i]<pMin) pMin=pitchHistory[i]; }
  if (pMax - pMin > 5.0f) {
      nodUntilMs = now + 1200;
      reactWithExpression(51, 1400);
  }

  // Shake (yaw change)
  float yMax = yawHistory[0], yMin = yawHistory[0];
  for(int i=1;i<4;i++){ if(yawHistory[i]>yMax) yMax=yawHistory[i]; if(yawHistory[i]<yMin) yMin=yawHistory[i]; }
  if (yMax - yMin > 3.2f) {
      headShakeUntilMs = now + 1000;
      reactWithExpression(27, 1200);
  }

  // Touch logic handled in loop now

  // Smooth Targets
  targetLookX = constrain(currentRoll * 0.22f, -18.0f, 18.0f);
  targetLookY = constrain(currentPitch * 0.20f, -15.0f, 15.0f);
}

int menuCursor = 0;
const int menuCount = 6;
const char* menuItems[] = {"Pingpong", "Suhu", "Pengingat", "Draw", "Musik", "Kembali"};

int musicCursor = 0;
const int musicCount = 5;
const char* musicItems[] = {"Love Story", "MBG", "YELLOW", "REDRED", "Kembali"};

int normalizeDfTrack(int track) {
  if (track < 1) return dfTrackCount;
  if (track > dfTrackCount) return 1;
  return track;
}

void clearReminders() {
  reminderCount = 0;
  for (int i = 0; i < MAX_REMINDERS; i++) {
    reminderItems[i].time[0] = '\0';
    reminderItems[i].date[0] = '\0';
    reminderItems[i].text[0] = '\0';
    reminderItems[i].active = false;
    reminderItems[i].lastTriggeredYDay = -1;
  }
}

void storeReminder(int index, String timeValue, String dateValue, bool active, String textValue) {
  if (index < 0 || index >= MAX_REMINDERS) return;
  timeValue.trim();
  dateValue.trim();
  textValue.trim();
  if (timeValue.length() != 5 || timeValue.charAt(2) != ':') timeValue = "07:30";
  if (dateValue.length() != 10 || dateValue.charAt(4) != '-' || dateValue.charAt(7) != '-') dateValue = "";
  if (textValue.length() == 0) textValue = "enroll lagi ya deck";
  timeValue.toCharArray(reminderItems[index].time, sizeof(reminderItems[index].time));
  dateValue.toCharArray(reminderItems[index].date, sizeof(reminderItems[index].date));
  textValue.substring(0, 32).toCharArray(reminderItems[index].text, sizeof(reminderItems[index].text));
  reminderItems[index].active = active;
  reminderItems[index].lastTriggeredYDay = -1;
  if (reminderCount < index + 1) reminderCount = index + 1;
  reminderSyncedMs = millis();
}

void parseReminderItem(String item, int index) {
  item.trim();
  if (item.length() == 0) return;
  int p1 = item.indexOf('|');
  if (p1 < 0) {
    storeReminder(index, "07:30", "", true, item);
    return;
  }
  int p2 = item.indexOf('|', p1 + 1);
  int p3 = p2 >= 0 ? item.indexOf('|', p2 + 1) : -1;
  if (p2 < 0 || p3 < 0) {
    storeReminder(index, item.substring(0, p1), "", true, item.substring(p1 + 1));
    return;
  }
  storeReminder(index,
                item.substring(0, p1),
                item.substring(p1 + 1, p2),
                item.substring(p2 + 1, p3) != "0",
                item.substring(p3 + 1));
}

void saveRemindersToFlash() {
  if (!reminderPrefs.begin("gembot", false)) return;
  uint8_t count = (uint8_t)constrain(reminderCount, 0, MAX_REMINDERS);
  reminderPrefs.putUChar("remCount", count);
  for (int i = 0; i < MAX_REMINDERS; i++) {
    char key[12];
    snprintf(key, sizeof(key), "rt%d", i);
    reminderPrefs.putString(key, i < count ? reminderItems[i].time : "");
    snprintf(key, sizeof(key), "rd%d", i);
    reminderPrefs.putString(key, i < count ? reminderItems[i].date : "");
    snprintf(key, sizeof(key), "rx%d", i);
    reminderPrefs.putString(key, i < count ? reminderItems[i].text : "");
    snprintf(key, sizeof(key), "ra%d", i);
    reminderPrefs.putBool(key, i < count && reminderItems[i].active);
  }
  reminderPrefs.end();
}

void loadRemindersFromFlash() {
  if (!reminderPrefs.begin("gembot", true)) return;
  uint8_t count = reminderPrefs.getUChar("remCount", 0);
  if (count > MAX_REMINDERS) count = MAX_REMINDERS;
  clearReminders();
  for (int i = 0; i < count; i++) {
    char key[12];
    snprintf(key, sizeof(key), "rt%d", i);
    String timeValue = reminderPrefs.getString(key, "");
    snprintf(key, sizeof(key), "rd%d", i);
    String dateValue = reminderPrefs.getString(key, "");
    snprintf(key, sizeof(key), "rx%d", i);
    String textValue = reminderPrefs.getString(key, "");
    snprintf(key, sizeof(key), "ra%d", i);
    bool active = reminderPrefs.getBool(key, true);
    if (timeValue.length()) storeReminder(i, timeValue, dateValue, active, textValue);
  }
  reminderPrefs.end();
  reminderSyncedMs = 0;
  reminderScreenUntilMs = 0;
  reminderAlertUntilMs = 0;
  reminderAlertIndex = -1;
}

void handleReminderMessage(const String& text) {
  if (currentState != APP_PENGINGAT && currentState != APP_LISTENING && currentState != APP_DRAW) {
    reminderReturnState = currentState;
  }
  clearReminders();
  if (text.startsWith("REMINDER:TEXT:")) {
    storeReminder(0, "07:30", "", true, text.substring(14));
  } else if (text.startsWith("REMINDER:SCHED:")) {
    parseReminderItem(text.substring(15), 0);
  } else if (text.startsWith("REMINDER:LIST:")) {
    String payload = text.substring(14);
    if (payload.startsWith("A:")) payload = payload.substring(2);
    int start = 0;
    int index = 0;
    while (start < payload.length() && index < MAX_REMINDERS) {
      int end = payload.indexOf(';', start);
      if (end < 0) end = payload.length();
      parseReminderItem(payload.substring(start, end), index);
      start = end + 1;
      index++;
    }
  }
  if (reminderCount == 0) storeReminder(0, "07:30", "", true, "enroll lagi ya deck");
  saveRemindersToFlash();
  reminderAlertIndex = -1;
  reminderAlertUntilMs = 0;
  reminderScreenUntilMs = 0;
}

void updateReminders() {
  unsigned long now = millis();

  if (reminderAlertIndex >= 0 && now >= reminderAlertUntilMs) {
    reminderAlertIndex = -1;
    reminderAlertUntilMs = 0;
    if (currentState == APP_PENGINGAT) currentState = APP_FACE;
  } else if (reminderAlertIndex >= 0) {
    if (now - reminderLastBeepMs >= 1250UL) {
      reminderLastBeepMs = now;
      playBeep(980.0f, 140);
    }
  } else if (reminderAlertIndex < 0 && reminderScreenUntilMs > 0 && now >= reminderScreenUntilMs) {
    reminderScreenUntilMs = 0;
    if (currentState == APP_PENGINGAT) currentState = reminderReturnState;
  }

  if (now - lastReminderCheckMs < 1000UL || reminderCount <= 0) return;
  lastReminderCheckMs = now;

  struct tm localTime;
  if (!getLocalTime(&localTime, 20)) return;

  char nowTime[6];
  char nowDate[11];
  strftime(nowTime, sizeof(nowTime), "%H:%M", &localTime);
  strftime(nowDate, sizeof(nowDate), "%Y-%m-%d", &localTime);

  for (int i = 0; i < reminderCount && i < MAX_REMINDERS; i++) {
    ReminderItem &item = reminderItems[i];
    if (!item.active || strcmp(item.time, nowTime) != 0) continue;
    if (item.date[0] != '\0' && strcmp(item.date, nowDate) != 0) continue;
    if (item.lastTriggeredYDay == localTime.tm_yday) continue;
    if (isVoiceOrAudioBusy() || currentState == APP_DRAW) continue;

    item.lastTriggeredYDay = localTime.tm_yday;
    reminderReturnState = currentState == APP_PENGINGAT ? APP_FACE : currentState;
    reminderAlertIndex = i;
    reminderAlertUntilMs = now + 30000UL;
    reminderLastBeepMs = now;
    reminderScreenUntilMs = 0;
    currentState = APP_PENGINGAT;
    playBeep(880.0f, 260);
    break;
  }
}

void waitDfCommandGap() {
  unsigned long now = millis();
  if (lastDfCommandMs > 0 && now - lastDfCommandMs < 150UL) {
    delay(150UL - (now - lastDfCommandMs));
  }
  lastDfCommandMs = millis();
}

void applyDfVolume(bool force) {
  if (!dfPlayerReady) return;
  dfVolume = constrain(dfVolume, 0, DFPLAYER_MAX_CLEAN_VOLUME);
  if (!force && dfAppliedVolume == dfVolume) return;
  waitDfCommandGap();
  myDFPlayer.volume(dfVolume);
  dfAppliedVolume = dfVolume;
}

void playDfTrack(int track) {
  dfCurrentTrack = normalizeDfTrack(track);
  if (dfPlayerReady) {
    applyDfVolume(false);
    waitDfCommandGap();
    myDFPlayer.playMp3Folder(dfCurrentTrack);
  }
  dfMusicPlaying = true;
  dfMusicPaused = false;
  dfTrackStartedMs = millis();
  dfPausedAtMs = 0;
  currentState = APP_MUSIK;
  isDrawMode = false;
}

void pauseDfMusic() {
  if (!dfMusicPlaying || dfMusicPaused) return;
  if (dfPlayerReady) {
    waitDfCommandGap();
    myDFPlayer.pause();
  }
  dfMusicPaused = true;
  dfPausedAtMs = millis();
}

void resumeDfMusic() {
  if (!dfMusicPlaying || !dfMusicPaused) return;
  if (dfPlayerReady) {
    waitDfCommandGap();
    myDFPlayer.start();
  }
  dfMusicPaused = false;
  if (dfPausedAtMs > dfTrackStartedMs) {
    dfTrackStartedMs += millis() - dfPausedAtMs;
  }
  dfPausedAtMs = 0;
  currentState = APP_MUSIK;
}

void stopDfMusic() {
  if (dfPlayerReady) {
    waitDfCommandGap();
    myDFPlayer.stop();
  }
  dfMusicPlaying = false;
  dfMusicPaused = false;
  dfPausedAtMs = 0;
  dfTrackStartedMs = 0;
  if (currentState == APP_MUSIK) currentState = APP_FACE;
}

void updateDfMusicEnd() {
  if (!dfMusicPlaying || dfMusicPaused || dfTrackStartedMs == 0) return;
  int track = normalizeDfTrack(dfCurrentTrack);
  uint16_t dur = dfTrackDurationSec[track] ? dfTrackDurationSec[track] : 180;
  unsigned long elapsedMs = millis() - dfTrackStartedMs;
  unsigned long endMs = (unsigned long)dur * 1000UL + 1200UL;
  if (elapsedMs < endMs) return;

  if (dfPlayerReady) {
    waitDfCommandGap();
    myDFPlayer.stop();
  }
  dfMusicPlaying = false;
  dfMusicPaused = false;
  dfPausedAtMs = 0;
  dfTrackStartedMs = 0;
  currentState = APP_MUSIK;
  musicCursor = track - 1;
}

void setDfVolume(int volume) {
  dfVolume = constrain(volume, 0, DFPLAYER_MAX_CLEAN_VOLUME);
  applyDfVolume(false);
}

void handleDfPlayerCommand(const String& text) {
  if (text.startsWith("CMD:DFP:PLAY:")) {
    playDfTrack(text.substring(13).toInt());
  } else if (text == "CMD:DFP:PAUSE") {
    pauseDfMusic();
  } else if (text == "CMD:DFP:RESUME") {
    resumeDfMusic();
  } else if (text == "CMD:DFP:STOP") {
    stopDfMusic();
  } else if (text == "CMD:DFP:NEXT") {
    playDfTrack(normalizeDfTrack(dfCurrentTrack + 1));
  } else if (text == "CMD:DFP:PREV") {
    playDfTrack(normalizeDfTrack(dfCurrentTrack - 1));
  } else if (text == "CMD:DFP:SCREEN") {
    currentState = APP_MUSIK;
    isDrawMode = false;
  } else if (text.startsWith("CMD:DFP:VOL:")) {
    setDfVolume(text.substring(12).toInt());
  }
}

void resetPingPong() {
  ballX = 120.0f;
  ballY = 160.0f;
  ballVX = (random(0, 2) == 0) ? -2.8f : 2.8f;
  ballVY = (random(0, 2) == 0) ? -3.4f : 3.4f;
  paddleX = 120.0f;
  aiPaddleX = 120.0f;
  score = 0;
  scoreAI = 0;
  gameOver = false;
  playerWon = false;
}

void startVoiceRecording() {
  if (voiceRecording) return;
  // Draw owns the TFT and touch interaction. Never turn a draw gesture into voice input.
  if (currentState == APP_DRAW || isDrawMode) return;
  if (isAiOutputActive()) return;
  if (!isWsConnected) {
    currentChatText = "GemBot belum terhubung";
    chatTextX = 240;
    isChatActive = true;
    return;
  }
  clearMicStream();
  lastMicSendMs = 0;
  voiceStartedMs = millis();
  voiceLastSpeechMs = voiceStartedMs;
  voiceHeardSpeech = false;
  currentState = APP_LISTENING;
  voiceRecording = true;
  playBeep(1320.0f, 70);
  webSocket.sendTXT("{\"event\":\"start_record\"}");
}

void stopVoiceRecording() {
  if (!voiceRecording) return;
  voiceRecording = false;
  sendPendingMicAudio(true);
  playBeep(760.0f, 80);
  if (webSocket.isConnected()) webSocket.sendTXT("{\"event\":\"stop_record\"}");
  currentState = APP_FACE;
  currentChatText = "GemBot lagi mikir...";
  chatTextX = 240;
  isChatActive = true;
}

void handleTouchAction(bool isHold) {
  if (currentState == APP_DRAW || isDrawMode) {
    // Tap stays reserved for drawing; hold exits draw mode.
    if (isHold) {
      playBeep(620.0f, 80);
      exitDrawMode();
    }
    return;
  }
  if (currentState == APP_FACE) {
    if (isHold) {
      startVoiceRecording();
    } else {
      playBeep(980.0f, 55);
      currentState = APP_MENU;
      menuCursor = 0;
    }
    return;
  }
  if (currentState == APP_MENU) {
    if (!isHold) {
      menuCursor = (menuCursor + 1) % menuCount;
      playBeep(1180.0f + menuCursor * 55.0f, 45);
    } else {
      playBeep(1560.0f, 80);
      if (menuCursor == 0) {
        currentState = APP_PINGPONG;
        resetPingPong();
      } else if (menuCursor == 1) currentState = APP_SUHU;
      else if (menuCursor == 2) currentState = APP_PENGINGAT;
      else if (menuCursor == 3) enterDrawMode(TFT_BLACK);
      else if (menuCursor == 4) currentState = APP_MUSIK;
      else if (menuCursor == 5) currentState = APP_FACE;
    }
    return;
  }
  if (currentState == APP_PENGINGAT) {
    if (isHold) {
      playBeep(620.0f, 90);
      reminderAlertIndex = -1;
      reminderAlertUntilMs = 0;
      reminderScreenUntilMs = 0;
      currentState = reminderReturnState == APP_PENGINGAT ? APP_FACE : reminderReturnState;
    }
    return;
  }
  if (currentState == APP_MUSIK) {
    if (dfMusicPlaying) {
      if (!isHold) {
        playBeep(1240.0f, 45);
        playDfTrack(normalizeDfTrack(dfCurrentTrack + 1));
      }
      else if (dfMusicPaused) {
        playBeep(1420.0f, 70);
        resumeDfMusic();
      }
      else {
        playBeep(680.0f, 70);
        pauseDfMusic();
      }
      return;
    }
    if (!isHold) {
      musicCursor = (musicCursor + 1) % musicCount;
      playBeep(1040.0f + musicCursor * 45.0f, 45);
    } else {
      playBeep(1500.0f, 80);
      if (musicCursor == musicCount - 1) {
        currentState = APP_MENU;
      } else {
        if (musicCursor == 0) {
            playDfTrack(1); // Love Story di /mp3/0001.mp3
        } else if (musicCursor == 1) {
            playDfTrack(2); // MBG di /mp3/0002.mp3
        } else if (musicCursor == 2) {
            playDfTrack(3); // YELLOW di /mp3/0003.mp3
        } else if (musicCursor == 3) {
            playDfTrack(4); // REDRED di /mp3/0004.mp3
        }
      }
    }
    return;
  }
  if (currentState == APP_PINGPONG && isHold && gameOver) {
    playBeep(1350.0f, 85);
    resetPingPong();
    return;
  }
  if (isHold) {
    playBeep(620.0f, 70);
    currentState = APP_FACE;
    isDrawMode = false;
  }
}


void handleTouchRelease() {
  if (currentState == APP_LISTENING && voiceRecording) stopVoiceRecording();
}

void handleTouch() {
  int touchVal = digitalRead(TOUCH_PIN);
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    Serial.printf("Touch: %s\n", touchVal == HIGH ? "ON" : "OFF");
    lastPrint = millis();
  }
  static bool rawTouch = false;
  static bool stableTouch = false;
  static bool touchArmed = false;
  static unsigned long rawChangedMs = 0;
  const unsigned long TOUCH_DEBOUNCE_MS = 75;
  const unsigned long TAP_MIN_MS = 45;
  const unsigned long HOLD_MS = 520;
  const unsigned long MUSIC_STOP_HOLD_MS = 1800;
  const unsigned long VOICE_RELEASE_GRACE_MS = 320;
  const unsigned long VOICE_AUTO_SEND_MS = 15000;
  bool sensedTouch = touchVal == HIGH;
  unsigned long now = millis();
  if (sensedTouch) lastInteractionMs = now;
  if (sensedTouch != rawTouch) {
    rawTouch = sensedTouch;
    rawChangedMs = now;
  }
  if (now - rawChangedMs >= TOUCH_DEBOUNCE_MS) stableTouch = rawTouch;

  if (!touchArmed) {
    if (!stableTouch) touchArmed = true;
    else return;
  }

  if (currentState == APP_MUSIK && dfMusicPlaying) {
    if (stableTouch) {
      if (touchStartTime == 0) {
        touchStartTime = now;
        touchHandled = false;
      } else if (!touchHandled && (now - touchStartTime >= MUSIC_STOP_HOLD_MS)) {
        touchHandled = true;
        stopDfMusic();
        currentChatText = "Musik berhenti";
        chatTextX = 240;
        isChatActive = true;
      }
    } else {
      if (touchStartTime > 0) {
        unsigned long heldMs = now - touchStartTime;
        if (!touchHandled && heldMs >= TAP_MIN_MS) {
          handleTouchAction(heldMs >= HOLD_MS);
        }
        touchStartTime = 0;
      }
    }
    return;
  }

  if (stableTouch) {
    if (touchStartTime == 0) {
      touchStartTime = now;
      touchHandled = false;
    } else if (!touchHandled && (now - touchStartTime >= HOLD_MS)) {
      touchHandled = true;
      handleTouchAction(true);
    }
  } else {
    if (touchStartTime > 0) {
      if (voiceRecording && !rawTouch && (now - rawChangedMs < VOICE_RELEASE_GRACE_MS)) {
        return;
      }
      if (voiceRecording) {
        handleTouchRelease();
      } else if (!touchHandled && (now - touchStartTime >= TAP_MIN_MS)) {
        handleTouchAction(false);
      } else if (touchHandled) {
        handleTouchRelease();
      }
      touchStartTime = 0;
    }
  }

  // A brief LOW pulse from the touch module must not stop recording.
  // The debounced release branch above is the normal send action.
  if (voiceRecording && now - voiceStartedMs >= VOICE_AUTO_SEND_MS) {
    handleTouchRelease();
    touchHandled = true;
  }
}

unsigned long lastBlinkMs = 0;
bool isBlinking = false;
float currentEyeScaleY = 2.5f;
float currentEyeScaleX = 2.5f;
float currentCuriousOffsetL = 0;
float currentCuriousOffsetR = 0;
float currentMouthOpen = 0;
float currentMouthTalk = 0;
unsigned long lastIdleMs = 0;
bool isSleeping = false;
unsigned long lastInteractionMs = 0;
float currentSleepAmount = 0;
float currentListenPulse = 0;

static const unsigned char PROGMEM image_Layer_7_bits[] = {0xc0,0x03,0xe0,0x07,0xf8,0x1f,0x7f,0xfe,0x3f,0xfc,0x0f,0xf0};

static const unsigned char PROGMEM image_Layer_8_bits[] = {0x3f,0xff,0xf8,0x00,0x7f,0xff,0xfe,0x00,0xff,0xff,0xff,0x00,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x80,0xff,0xff,0xff,0x00,0x7f,0xff,0xff,0x00,0x7f,0xff,0xff,0x00,0x7f,0xff,0xfe,0x00,0x7f,0xff,0xfe,0x00,0x3f,0xff,0xfc,0x00,0x1f,0xff,0xf8,0x00,0x0f,0xff,0xe0,0x00,0x07,0xff,0x00,0x00};

static const unsigned char PROGMEM image_Layer_9_bits[] = {0x07,0xff,0xff,0x00,0x1f,0xff,0xff,0x80,0x3f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0xff,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x7f,0xff,0xff,0xc0,0x3f,0xff,0xff,0xc0,0x3f,0xff,0xff,0x80,0x3f,0xff,0xff,0x80,0x1f,0xff,0xff,0x80,0x1f,0xff,0xff,0x80,0x0f,0xff,0xff,0x00,0x07,0xff,0xfe,0x00,0x01,0xff,0xfc,0x00,0x00,0x3f,0xf8,0x00};



void drawBitmapScaled(int x, int y, const uint8_t *bitmap, int w, int h, float scaleX, float scaleY, uint16_t color) {
    int scaledW = (int)(w * scaleX);
    int scaledH = (int)(h * scaleY);
    if (scaledW <= 0 || scaledH <= 0) return;
    int byteWidth = (w + 7) / 8;
    for (int j = 0; j < scaledH; j++) {
        int origY = (int)(j / scaleY);
        if (origY >= h) origY = h - 1;
        for (int i = 0; i < scaledW; i++) {
            int origX = (int)(i / scaleX);
            if (origX >= w) origX = w - 1;
            uint8_t b = pgm_read_byte(&bitmap[origY * byteWidth + (origX / 8)]);
            if (b & (0x80 >> (origX % 8))) {
                spr.drawPixel(x + i, y + j, color);
            }
        }
    }
}

void drawFace() {
  static float currentEyeScaleX = 1.0f, currentEyeScaleY = 1.0f, currentEyeOffsetY = 0.0f;
  static float currentMouthScaleX = 1.0f, currentMouthScaleY = 1.0f, currentMouthOffsetY = 0.0f;
  static float currentPupilScale = 1.0f;

  float targetEyeScaleX = 1.0f, targetEyeScaleY = 1.0f, targetEyeOffsetY = 0.0f;
  float targetMouthScaleX = 1.0f, targetMouthScaleY = 1.0f, targetMouthOffsetY = 0.0f;
  float targetPupilScale = 1.0f;
  float exprLookX = 0.0f, exprLookY = 0.0f;
  float exprBobY = 0.0f;

  unsigned long now = millis();
  
  // Mapping 14 Ekspresi ke transformasi matematika
  switch (currentExpressionId) {
      case 0: // Normal
          break;
      case 1: case 7: case 17: case 24: // Senang / Excited / Party / Delight
          targetEyeScaleY = 0.4f;
          targetMouthScaleX = 1.5f;
          targetMouthScaleY = 1.5f;
          targetMouthOffsetY = -4.0f;
          exprBobY = sin(now * 0.005f) * 6.0f; 
          break;
      case 2: case 10: case 18: // Love / Cozy / Relieved
          targetEyeScaleY = 0.4f;
          targetEyeScaleX = 1.2f;
          targetMouthScaleX = 1.2f;
          targetMouthOffsetY = 2.0f;
          exprBobY = sin(now * 0.003f) * 4.0f;
          break;
      case 3: case 27: // Marah / Grumpy
          targetEyeScaleY = 0.35f;
          targetEyeOffsetY = 5.0f;
          targetMouthScaleX = 0.7f;
          targetMouthScaleY = 0.5f;
          targetMouthOffsetY = 8.0f;
          targetPupilScale = 0.6f;
          break;
      case 4: case 22: case 28: // Kaget / Wow / Amazed
          targetEyeScaleX = 1.3f;
          targetEyeScaleY = 1.3f;
          targetMouthScaleX = 0.5f;
          targetMouthScaleY = 2.5f;
          targetMouthOffsetY = 5.0f;
          targetPupilScale = 0.5f;
          break;
      case 5: case 15: // Ngantuk / Bored
          targetEyeScaleY = 0.15f;
          targetMouthScaleX = 0.8f;
          targetPupilScale = 0.9f;
          break;
      case 6: case 29: // Sedih / Nangis
          targetEyeScaleY = 0.5f;
          targetEyeOffsetY = 8.0f;
          targetMouthScaleX = 0.6f;
          targetMouthScaleY = 0.8f;
          targetMouthOffsetY = 12.0f;
          targetPupilScale = 0.8f;
          exprLookY = 6.0f; 
          break;
      case 8: case 12: case 31: // Smug / Cheeky / Nakal
          targetEyeScaleY = 0.45f;
          targetMouthScaleX = 1.4f;
          targetMouthOffsetY = -6.0f;
          exprLookX = 8.0f;
          exprLookY = -4.0f;
          break;
      case 9: case 25: // Takut / Guilty
          targetEyeScaleX = 0.8f;
          targetEyeScaleY = 0.8f;
          targetEyeOffsetY = -4.0f;
          targetMouthScaleX = 0.5f;
          targetMouthScaleY = 0.5f;
          targetPupilScale = 0.4f;
          exprLookX = sin(now * 0.04f) * 4.0f; // Shivering
          break;
      case 11: case 23: case 30: // Woozy / Melt / Pusing
          targetEyeScaleX = 0.6f;
          targetEyeScaleY = 0.3f;
          targetMouthScaleX = 1.8f;
          targetMouthScaleY = 0.3f;
          exprLookX = sin(now * 0.015f) * 12.0f;
          exprLookY = cos(now * 0.015f) * 12.0f;
          break;
      case 13: case 20: // Bashful / Giggle
          targetEyeScaleY = 0.3f;
          targetMouthScaleX = 1.2f;
          targetMouthScaleY = 0.8f;
          targetMouthOffsetY = -2.0f;
          exprBobY = sin(now * 0.01f) * 3.0f;
          break;
      case 14: case 21: // Focus / Determined
          targetEyeScaleX = 0.9f;
          targetEyeScaleY = 0.7f;
          targetEyeOffsetY = 4.0f;
          targetMouthScaleX = 0.6f;
          targetMouthOffsetY = 5.0f;
          targetPupilScale = 0.7f;
          break;
      case 16: // Nope
          targetEyeScaleX = 0.9f;
          targetEyeScaleY = 0.5f;
          targetEyeOffsetY = 2.0f;
          targetMouthScaleX = 0.4f;
          targetMouthOffsetY = 10.0f;
          exprLookX = -12.0f;
          break;
      case 19: // Suspicious
          targetEyeScaleY = 0.4f;
          targetEyeScaleX = 0.8f;
          targetEyeOffsetY = 5.0f;
          targetMouthScaleX = 0.7f;
          exprLookX = 14.0f;
          targetPupilScale = 0.8f;
          break;
      case 26: // Daydream
          targetEyeScaleY = 0.5f;
          targetEyeScaleX = 0.9f;
          targetMouthScaleX = 0.8f;
          exprLookY = -12.0f;
          exprBobY = sin(now * 0.003f) * 5.0f;
          break;
      case 50: // Heart / Curious
          targetEyeScaleX = 1.1f;
          targetEyeScaleY = 1.1f;
          targetMouthScaleX = 1.2f;
          targetMouthOffsetY = 2.0f;
          exprBobY = sin(now * 0.006f) * 4.0f;
          break;
      case 51: // Haha / Nod
          targetEyeScaleY = 0.3f;
          targetMouthScaleX = 1.5f;
          targetMouthScaleY = 1.5f;
          targetMouthOffsetY = -4.0f;
          exprBobY = sin(now * 0.015f) * 8.0f;
          break;
      default: // Normal
          break;
  }

  if (aiSpeaking && now > aiSpeakingUntilMs && (now - lastAudioPlayedMs) > 500UL) {
      aiSpeaking = false;
  }
  bool speakingNow = aiSpeaking || ((now - lastAudioPlayedMs) < 220UL);
  if (speakingNow) {
      float talkWave = fabs(sin(now * 0.035f));
      float speakMouthY = 0.75f + talkWave * 1.35f + ttsMouthLevel * 0.9f;
      float speakMouthX = 1.05f + ttsMouthLevel * 0.35f;
      if (targetMouthScaleY < speakMouthY) targetMouthScaleY = speakMouthY;
      if (targetMouthScaleX < speakMouthX) targetMouthScaleX = speakMouthX;
      targetMouthOffsetY += sin(now * 0.022f) * 2.5f;
      exprBobY += sin(now * 0.006f) * 2.0f;
  }
  ttsMouthLevel *= 0.90f;

  // Blinking animation (only if eye is mostly open)
  // --- Alive Animation Logic ---
  unsigned long blinkCycle = now % 4500;
  
  // Blink Anticipation (Widen before blink)
  if (targetEyeScaleY > 0.5f) {
      if (blinkCycle > 4250 && blinkCycle <= 4350) {
          targetEyeScaleY *= 1.2f; // Anticipation widen
      } else if (blinkCycle > 4350 && blinkCycle <= 4500) {
          targetEyeScaleY = 0.05f; // Squashed closed
      }
  }

  // Physics Variables for Spring-Damper
  static float vEyeScaleX = 0, vEyeScaleY = 0;
  static float vMouthScaleX = 0, vMouthScaleY = 0;
  
  float spring = 0.15f;
  float damp = 0.70f;

  vEyeScaleX += (targetEyeScaleX - currentEyeScaleX) * spring;
  vEyeScaleY += (targetEyeScaleY - currentEyeScaleY) * spring;
  vMouthScaleX += (targetMouthScaleX - currentMouthScaleX) * spring;
  vMouthScaleY += (targetMouthScaleY - currentMouthScaleY) * spring;

  vEyeScaleX *= damp;
  vEyeScaleY *= damp;
  vMouthScaleX *= damp;
  vMouthScaleY *= damp;

  currentEyeScaleX += vEyeScaleX;
  currentEyeScaleY += vEyeScaleY;
  currentMouthScaleX += vMouthScaleX;
  currentMouthScaleY += vMouthScaleY;

  // Prevent negative scale
  if (currentEyeScaleX < 0.05f) currentEyeScaleX = 0.05f;
  if (currentEyeScaleY < 0.05f) currentEyeScaleY = 0.05f;
  if (currentMouthScaleX < 0.05f) currentMouthScaleX = 0.05f;
  if (currentMouthScaleY < 0.05f) currentMouthScaleY = 0.05f;

  // Linear LERP for position to avoid jitter
  float lerpSpeed = 0.15f;
  currentEyeOffsetY += (targetEyeOffsetY - currentEyeOffsetY) * lerpSpeed;
  currentMouthOffsetY += (targetMouthOffsetY - currentMouthOffsetY) * lerpSpeed;
  currentPupilScale += (targetPupilScale - currentPupilScale) * lerpSpeed;

  // Saccadic Eye Darts (Randomly changing target positions instead of sine waves)
  static float saccadeTargetX = 0;
  static float saccadeTargetY = 0;
  static unsigned long nextSaccadeMs = 0;
  if (now > nextSaccadeMs) {
      saccadeTargetX = (random(100) / 100.0f - 0.5f) * 12.0f; // Dart between -6 and +6
      saccadeTargetY = (random(100) / 100.0f - 0.5f) * 8.0f;
      nextSaccadeMs = now + random(500, 2500); // Wait 0.5s to 2.5s before darting again
  }
  static float currentSaccadeX = 0, currentSaccadeY = 0;
  currentSaccadeX += (saccadeTargetX - currentSaccadeX) * 0.3f; // Fast snap
  currentSaccadeY += (saccadeTargetY - currentSaccadeY) * 0.3f;

  float baseBobY = sin(now * 0.002f) * 4.0f; // Body still breathes smoothly
  float baseIdleLookX = currentSaccadeX;
  float baseIdleLookY = currentSaccadeY;
  
  float breathingScale = 1.0f + (sin(now * 0.0015f) * 0.03f); // 3% breathing scale

  // Combine looks with MPU
  float finalLookX = baseIdleLookX + exprLookX + targetLookX;
  float finalLookY = baseIdleLookY + exprLookY + targetLookY;
  float finalBobY = baseBobY + exprBobY + targetBob;

  spr.fillSprite(TFT_BLACK);
  uint16_t faceColor = spr.color565(0, 200, 255); // Biru (Cyan-ish Blue)
  float baseScale = 2.4f * breathingScale; // MUCH LARGER!
  float verticalSpread = 1.2f; // Adjusted for the larger scale

  int faceTotalHeight = (int)(64 * verticalSpread * baseScale);
  int offsetX = (240 - (int)(128 * baseScale)) / 2 + (int)finalLookX;
  int offsetY = (320 - faceTotalHeight) / 2 + (int)finalBobY + (int)finalLookY;

  // Centering offsets
  int eyeCenterOffsetY = (int)((37 * baseScale - 37 * baseScale * currentEyeScaleY) / 2.0f);
  int eyeCenterOffsetX = (int)((26 * baseScale - 26 * baseScale * currentEyeScaleX) / 2.0f);

  int mouthOffsetX = (240 - (int)(128 * baseScale)) / 2 + (int)(finalLookX * 0.8f);
  int mouthOffsetY = (320 - faceTotalHeight) / 2 + (int)(finalBobY * 1.1f) + (int)(finalLookY * 0.8f);
  int mCenterOffsetX = (int)((16 * baseScale - 16 * baseScale * currentMouthScaleX) / 2.0f);
  int mCenterOffsetY = (int)((6 * baseScale - 6 * baseScale * currentMouthScaleY) / 2.0f);

  // Layer 7 (Mouth)
  drawBitmapScaled(mouthOffsetX + (int)(57 * baseScale) + mCenterOffsetX, 
                   mouthOffsetY + (int)(48 * verticalSpread * baseScale) + (int)(currentMouthOffsetY * baseScale) + mCenterOffsetY, 
                   image_Layer_7_bits, 16, 6, 
                   baseScale * currentMouthScaleX, baseScale * currentMouthScaleY, faceColor);

  // Layer 8 (Right Eye)
  drawBitmapScaled(offsetX + (int)(76 * baseScale) + eyeCenterOffsetX, 
                   offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY, 
                   image_Layer_8_bits, 26, 37, 
                   baseScale * currentEyeScaleX, baseScale * currentEyeScaleY, faceColor);
  
  // Layer 9 (Left Eye)
  drawBitmapScaled(offsetX + (int)(28 * baseScale) + eyeCenterOffsetX, 
                   offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY, 
                   image_Layer_9_bits, 26, 37, 
                   baseScale * currentEyeScaleX, baseScale * currentEyeScaleY, faceColor);

  // Pupils (Hitam)
  int pupilRadius = (int)(4 * baseScale * currentPupilScale);
  int pX = (int)(13 * baseScale * currentEyeScaleX); 
  int pY = (int)(18 * baseScale * currentEyeScaleY); 
  
  int pLookX = (int)(finalLookX * 0.6f); 
  int pLookY = (int)(finalLookY * 0.6f);

  if (currentEyeScaleY > 0.2f && currentExpressionId != 50 && currentExpressionId != 51) { 
      spr.fillCircle(offsetX + (int)(28 * baseScale) + eyeCenterOffsetX + pX + pLookX, 
                     offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY, 
                     pupilRadius, TFT_BLACK);
      spr.fillCircle(offsetX + (int)(76 * baseScale) + eyeCenterOffsetX + pX + pLookX, 
                     offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY, 
                     pupilRadius, TFT_BLACK);
  } else if (currentExpressionId == 50) {
      spr.setTextSize(3);
      spr.setTextColor(TFT_RED);
      spr.drawString("<3", offsetX + (int)(28 * baseScale) + eyeCenterOffsetX + pX + pLookX - 16, 
                          offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY - 8);
      spr.drawString("<3", offsetX + (int)(76 * baseScale) + eyeCenterOffsetX + pX + pLookX - 16, 
                          offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY - 8);
  } else if (currentExpressionId == 51) {
      spr.setTextSize(3);
      spr.setTextColor(TFT_BLACK);
      spr.drawString(">", offsetX + (int)(28 * baseScale) + eyeCenterOffsetX + pX + pLookX - 8, 
                          offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY - 8);
      spr.drawString("<", offsetX + (int)(76 * baseScale) + eyeCenterOffsetX + pX + pLookX - 8, 
                          offsetY + (int)(13 * verticalSpread * baseScale) + (int)(currentEyeOffsetY * baseScale) + eyeCenterOffsetY + pY + pLookY - 8);
  }

  // Tears Animation for Nangis (29)
  if (currentExpressionId == 29 || currentExpressionId == 6) { // Nangis or Sedih
      uint16_t tearColor = spr.color565(0, 150, 255); // Deep light blue
      
      // Left Tear
      int lTearCycle = now % 1500;
      if (lTearCycle < 1000) {
          int lDropScale = (lTearCycle > 800) ? (1000 - lTearCycle) / 200.0f * 3.0f * baseScale : 3.0f * baseScale;
          int lTearY = offsetY + (int)(35 * verticalSpread * baseScale) + (int)((lTearCycle / 1000.0f) * 40 * baseScale);
          int lTearX = offsetX + (int)(40 * baseScale) + eyeCenterOffsetX;
          if (lDropScale > 0) {
              spr.fillCircle(lTearX, lTearY, lDropScale, tearColor);
              spr.fillTriangle(lTearX - lDropScale, lTearY, lTearX + lDropScale, lTearY, lTearX, lTearY - lDropScale * 2, tearColor);
          }
      }
      
      // Right Tear
      int rTearCycle = (now + 600) % 1300;
      if (rTearCycle < 900) {
          int rDropScale = (rTearCycle > 700) ? (900 - rTearCycle) / 200.0f * 3.0f * baseScale : 3.0f * baseScale;
          int rTearY = offsetY + (int)(35 * verticalSpread * baseScale) + (int)((rTearCycle / 900.0f) * 40 * baseScale);
          int rTearX = offsetX + (int)(88 * baseScale) + eyeCenterOffsetX;
          if (rDropScale > 0) {
              spr.fillCircle(rTearX, rTearY, rDropScale, tearColor);
              spr.fillTriangle(rTearX - rDropScale, rTearY, rTearX + rDropScale, rTearY, rTearX, rTearY - rDropScale * 2, tearColor);
          }
      }
  }

  // Draw Chatbot Running Text
  if (isChatActive) {
      spr.fillRect(0, 290, 240, 30, TFT_BLACK);
      spr.setTextSize(2);
      spr.setTextColor(TFT_YELLOW);
      
      int textW = spr.textWidth(currentChatText);
      spr.setCursor(chatTextX, 296);
      spr.print(currentChatText);
      
      chatTextX -= 3;
      if (chatTextX < -textW) {
          isChatActive = false;
      }
  }
}

float morphValue(float current, float target, float speed) {
  return current + (target - current) * speed;
}

void drawFaceLine(int x1, int y1, int x2, int y2, int thickness, uint16_t color) {
  int half = max(0, thickness / 2);
  for (int offset = -half; offset <= half; offset++) {
    spr.drawLine(x1, y1 + offset, x2, y2 + offset, color);
  }
}

void drawMorphMouth(int cx, int cy, float widthValue, float curve, float openValue,
                    float slant, uint16_t color) {
  int width = max(10, (int)widthValue);
  int open = max(0, (int)openValue);
  if (open > 5) {
    int mouthW = max(10, (int)(width * (0.68f + min(open / 36.0f, 1.0f) * 0.22f)));
    int mouthH = min(38, open);
    int x = cx - mouthW / 2;
    int y = cy - mouthH / 2;
    spr.fillRoundRect(x, y, mouthW, mouthH, max(3, mouthH / 2), color);
    if (mouthW > 16 && mouthH > 11) {
      spr.fillRoundRect(x + 5, y + 5, mouthW - 10, mouthH - 8,
                        max(2, (mouthH - 8) / 2), TFT_BLACK);
      if (curve > 5.0f && mouthH > 17) {
        spr.fillRoundRect(cx - mouthW / 4, y + mouthH - 7,
                          mouthW / 2, 5, 2, spr.color565(255, 105, 145));
      }
    }
    return;
  }

  const int segments = 16;
  int previousX = cx - width / 2;
  int previousY = cy - (int)(slant * 0.5f);
  for (int i = 1; i <= segments; i++) {
    float t = (float)i / segments;
    float parabola = 4.0f * t * (1.0f - t);
    int x = cx - width / 2 + (int)(width * t);
    int y = cy + (int)(curve * parabola + slant * (t - 0.5f));
    drawFaceLine(previousX, previousY, x, y, 4, color);
    previousX = x;
    previousY = y;
  }
}

void drawMorphBrow(int cx, int eyeTop, int nominalWidth, float angle,
                   float lift, float visibility, bool rightEye, uint16_t color) {
  if (visibility < 0.08f) return;
  int halfWidth = max(4, (int)(nominalWidth * 0.38f * visibility));
  int y = eyeTop - 9 - (int)lift;
  int delta = (int)(angle * visibility);
  int y1 = rightEye ? y + delta : y - delta;
  int y2 = rightEye ? y - delta : y + delta;
  drawFaceLine(cx - halfWidth, y1, cx + halfWidth, y2,
               1 + (int)(visibility * 3.0f), color);
}

void drawLidMask(int x, int y, int w, int h, float amount, bool rightEye) {
  int cut = min(h - 1, (int)(fabs(amount) * h));
  if (cut <= 0) return;
  bool cutInner = amount > 0.0f;
  if ((cutInner && !rightEye) || (!cutInner && rightEye)) {
    spr.fillTriangle(x, y, x + w, y, x + w, y + cut, TFT_BLACK);
  } else {
    spr.fillTriangle(x, y, x + w, y, x, y + cut, TFT_BLACK);
  }
}

void drawSpiralPupil(int cx, int cy, int radius) {
  radius = max(4, radius);
  for (int r = radius; r >= 3; r -= 4) spr.drawCircle(cx, cy, r, TFT_BLACK);
  spr.drawLine(cx, cy, cx + radius, cy, TFT_BLACK);
}

void drawHeartPupil(int cx, int cy, int radius) {
  int r = max(3, radius / 2);
  spr.fillCircle(cx - r, cy - r / 2, r, TFT_RED);
  spr.fillCircle(cx + r, cy - r / 2, r, TFT_RED);
  spr.fillTriangle(cx - radius, cy, cx + radius, cy, cx, cy + radius + 2, TFT_RED);
}

void drawFaceAlive() {
  static float eyeLX = 1.0f, eyeLY = 1.0f, eyeRX = 1.0f, eyeRY = 1.0f;
  static float eyeOffsetY = 0.0f;
  static float pupilScale = 0.9f;
  static float mouthWidth = 38.0f, mouthCurve = 7.0f, mouthOpen = 0.0f, mouthSlant = 0.0f;
  static float browAngle = 0.0f, browLift = 0.0f, browVisibility = 0.0f;
  static float lidL = 0.0f, lidR = 0.0f;
  static float saccadeX = 0.0f, saccadeY = 0.0f;
  static float saccadeTargetX = 0.0f, saccadeTargetY = 0.0f;
  static unsigned long nextSaccadeMs = 0;
  static unsigned long nextBlinkMs = 0;
  static unsigned long blinkStartedMs = 0;
  static unsigned long nextIdleEmotionMs = 0;
  static unsigned long idleEmotionUntilMs = 0;
  static int idleEmotion = 0;
  static unsigned long firstFriendlyFaceMs = 0;

  unsigned long now = millis();
  if (firstFriendlyFaceMs == 0) firstFriendlyFaceMs = now;
  int expression = 0;
  if (now < manualExpressionUntilMs && currentExpressionId >= 0) {
    expression = currentExpressionId;
  } else if (now < sensorExpressionUntilMs && sensorExpressionId >= 0) {
    expression = sensorExpressionId;
  } else {
    currentExpressionId = -1;
    sensorExpressionId = -1;
    unsigned long friendlyAge = now - firstFriendlyFaceMs;
    if (friendlyAge < 1300UL) {
      expression = 31; // wink hello
    } else if (friendlyAge < 3000UL) {
      expression = 1; // warm greeting smile
    } else {
      if (nextIdleEmotionMs == 0) nextIdleEmotionMs = now + random(2500, 5500);
      if (now >= nextIdleEmotionMs) {
        const int idleChoices[] = {1, 2, 4, 7, 11, 24, 28, 32, 33, 34, 50, 51};
        idleEmotion = idleChoices[random(0, sizeof(idleChoices) / sizeof(idleChoices[0]))];
        idleEmotionUntilMs = now + random(1500, 3000);
        nextIdleEmotionMs = idleEmotionUntilMs + random(4000, 8000);
      }
      expression = now < idleEmotionUntilMs ? idleEmotion : 0;
    }
  }
  displayedExpressionId = expression;

  float tEyeLX = 1.0f, tEyeLY = 1.0f, tEyeRX = 1.0f, tEyeRY = 1.0f;
  float tEyeOffsetY = 0.0f, tPupilScale = 0.9f;
  float tMouthWidth = 38.0f, tMouthCurve = 7.0f, tMouthOpen = 0.0f, tMouthSlant = 0.0f;
  float tBrowAngle = 0.0f, tBrowLift = 0.0f, tBrowVisibility = 0.0f;
  float tLidL = 0.0f, tLidR = 0.0f;
  float exprLookX = 0.0f, exprLookY = 0.0f, exprBobY = 0.0f, exprLeanX = 0.0f;
  bool spiralPupils = false;
  bool heartPupils = false;
  bool pupilHighlights = false;

  switch (expression) {
    case 1: // Senang
      tEyeLY = tEyeRY = 0.32f;
      tMouthWidth = 54.0f; tMouthCurve = 13.0f;
      exprBobY = sin(now * 0.006f) * 4.0f;
      break;
    case 2: case 10: case 18: // Love / cozy / relieved
      tEyeLX = tEyeRX = 1.06f; tEyeLY = tEyeRY = 0.62f;
      tMouthWidth = 44.0f; tMouthCurve = 10.0f;
      tBrowLift = 3.0f; tBrowVisibility = 0.35f;
      exprBobY = sin(now * 0.0035f) * 3.0f;
      break;
    case 3: // Marah: tatapan tajam, alis menukik, mulut tertahan
      tEyeLX = tEyeRX = 0.98f; tEyeLY = tEyeRY = 0.64f;
      tEyeOffsetY = 4.0f; tPupilScale = 0.52f;
      tBrowAngle = 11.0f; tBrowLift = -2.0f; tBrowVisibility = 1.0f;
      tLidL = tLidR = 0.27f;
      tMouthWidth = 42.0f; tMouthCurve = -5.0f;
      exprLookY = -2.0f;
      exprLeanX = sin(now * 0.0032f) * 2.0f;
      exprBobY = -fabs(sin(now * 0.0045f)) * 2.0f;
      break;
    case 4: case 22: // Kaget / wow
      tEyeLX = tEyeRX = 1.15f; tEyeLY = tEyeRY = 1.18f;
      tPupilScale = 0.38f; tBrowLift = 12.0f; tBrowVisibility = 0.9f;
      tMouthWidth = 23.0f; tMouthCurve = 0.0f; tMouthOpen = 32.0f;
      exprBobY = -4.0f + sin(now * 0.009f) * 1.5f;
      break;
    case 5: // Ngantuk
      tEyeLX = tEyeRX = 1.04f;
      tEyeLY = 0.19f + fabs(sin(now * 0.0009f)) * 0.07f;
      tEyeRY = tEyeLY * 0.90f;
      tEyeOffsetY = 7.0f; tPupilScale = 0.75f;
      tMouthWidth = 25.0f; tMouthCurve = 2.0f;
      if ((now / 6000UL) % 3UL == 2UL) tMouthOpen = 8.0f + fabs(sin(now * 0.002f)) * 10.0f;
      exprLookY = 4.0f; exprBobY = sin(now * 0.0015f) * 2.0f;
      break;
    case 6: // Sedih: mata tetap terbuka, menunduk, tanpa air mata
      tEyeLX = tEyeRX = 0.92f; tEyeLY = tEyeRY = 0.74f;
      tEyeOffsetY = 4.0f; tPupilScale = 0.82f;
      tBrowAngle = -8.0f; tBrowLift = 3.0f; tBrowVisibility = 0.95f;
      tLidL = tLidR = -0.10f;
      tMouthWidth = 34.0f; tMouthCurve = -10.0f;
      exprLookY = 7.0f; exprBobY = sin(now * 0.0018f) * 1.5f;
      break;
    case 7: case 17: case 20: // Excited / party / giggle
      tEyeLX = tEyeRX = 1.08f; tEyeLY = tEyeRY = 0.42f;
      tMouthWidth = 52.0f; tMouthCurve = 11.0f; tMouthOpen = 13.0f;
      exprBobY = sin(now * 0.010f) * 7.0f;
      exprLeanX = sin(now * 0.006f) * 4.0f;
      break;
    case 8: case 12: // Smug / cheeky
      tEyeLY = 0.58f; tEyeRY = 0.72f;
      tPupilScale = 0.78f; tBrowAngle = 3.0f; tBrowVisibility = 0.6f;
      tMouthWidth = 41.0f; tMouthCurve = 7.0f; tMouthSlant = -5.0f;
      exprLookX = 7.0f;
      break;
    case 9: // Takut
      tEyeLX = tEyeRX = 0.92f; tEyeLY = tEyeRY = 1.06f;
      tPupilScale = 0.42f; tBrowAngle = -7.0f; tBrowLift = 8.0f; tBrowVisibility = 1.0f;
      tMouthWidth = 25.0f; tMouthCurve = -3.0f; tMouthOpen = 8.0f;
      exprLookX = sin(now * 0.010f) * 3.0f;
      break;
    case 11: case 23: case 30: // Woozy / melt / pusing
      tEyeLX = tEyeRX = 0.96f; tEyeLY = tEyeRY = 0.86f;
      tPupilScale = 1.0f; tMouthWidth = 37.0f; tMouthCurve = -2.0f; tMouthSlant = 5.0f;
      spiralPupils = true;
      exprLookX = sin(now * 0.008f) * 9.0f;
      exprLookY = cos(now * 0.008f) * 7.0f;
      exprLeanX = sin(now * 0.004f) * 5.0f;
      break;
    case 13: // Bashful
      tEyeLY = tEyeRY = 0.48f; tPupilScale = 0.72f;
      tMouthWidth = 28.0f; tMouthCurve = 7.0f;
      exprLookX = -5.0f; exprLookY = 4.0f;
      break;
    case 14: case 21: // Focus / determined
      tEyeLX = tEyeRX = 0.96f; tEyeLY = tEyeRY = 0.70f;
      tPupilScale = 0.62f; tBrowAngle = 6.0f; tBrowVisibility = 0.85f;
      tLidL = tLidR = 0.12f;
      tMouthWidth = 30.0f; tMouthCurve = 0.0f;
      break;
    case 15: // Bored
      tEyeLY = 0.40f; tEyeRY = 0.34f; tEyeOffsetY = 5.0f;
      tMouthWidth = 31.0f; tMouthCurve = -1.0f; tMouthSlant = 3.0f;
      exprLookX = -6.0f;
      break;
    case 16: // Nope
      tEyeLY = tEyeRY = 0.56f; tBrowAngle = 4.0f; tBrowVisibility = 0.7f;
      tMouthWidth = 28.0f; tMouthCurve = -6.0f;
      exprLookX = -10.0f;
      break;
    case 19: // Suspicious
      tEyeLY = 0.52f; tEyeRY = 0.78f; tPupilScale = 0.70f;
      tBrowAngle = 5.0f; tBrowVisibility = 0.8f; tLidL = 0.10f;
      tMouthWidth = 29.0f; tMouthCurve = -1.0f; tMouthSlant = -4.0f;
      exprLookX = 8.0f;
      break;
    case 24: // Delight: tertawa lepas, bukan sekadar happy
      tEyeLX = tEyeRX = 1.12f; tEyeLY = tEyeRY = 0.25f;
      tMouthWidth = 55.0f; tMouthCurve = 12.0f; tMouthOpen = 14.0f;
      exprBobY = sin(now * 0.008f) * 5.0f;
      break;
    case 25: // Guilty: tatapan turun asimetris dan mulut kecil
      tEyeLX = 0.92f; tEyeRX = 0.86f; tEyeLY = 0.72f; tEyeRY = 0.62f;
      tEyeOffsetY = 4.0f; tPupilScale = 0.68f;
      tBrowAngle = -7.0f; tBrowLift = 4.0f; tBrowVisibility = 0.95f;
      tMouthWidth = 24.0f; tMouthCurve = -4.0f; tMouthSlant = 2.0f;
      exprLookX = -5.0f; exprLookY = 7.0f;
      break;
    case 26: // Daydream: melayang pelan dan menatap ke atas
      tEyeLX = tEyeRX = 0.98f; tEyeLY = 0.55f; tEyeRY = 0.61f;
      tPupilScale = 0.66f; tLidL = tLidR = 0.08f;
      tMouthWidth = 31.0f; tMouthCurve = 7.0f;
      exprLookX = sin(now * 0.0014f) * 5.0f;
      exprLookY = -8.0f + cos(now * 0.0011f) * 2.0f;
      exprBobY = sin(now * 0.0022f) * 5.0f;
      break;
    case 27: // Grumpy: berat dan menyamping, lebih lunak dari marah
      tEyeLX = tEyeRX = 1.02f; tEyeLY = 0.50f; tEyeRY = 0.44f;
      tEyeOffsetY = 4.0f; tPupilScale = 0.76f;
      tBrowAngle = 5.0f; tBrowLift = -1.0f; tBrowVisibility = 0.78f;
      tLidL = tLidR = 0.12f;
      tMouthWidth = 34.0f; tMouthCurve = -3.0f; tMouthSlant = 5.0f;
      exprLookX = -5.0f;
      break;
    case 28: // Amazed: mata berbinar dan senyum terbuka
      tEyeLX = tEyeRX = 1.16f; tEyeLY = tEyeRY = 1.12f;
      tPupilScale = 0.78f; tBrowLift = 10.0f; tBrowVisibility = 0.75f;
      tMouthWidth = 39.0f; tMouthCurve = 10.0f; tMouthOpen = 13.0f;
      pupilHighlights = true;
      exprBobY = -2.0f + sin(now * 0.006f) * 3.0f;
      break;
    case 29: // Nangis: mata terpejam, tersedu, air mata jelas
      tEyeLX = tEyeRX = 1.06f; tEyeLY = 0.24f; tEyeRY = 0.28f;
      tEyeOffsetY = 6.0f; tPupilScale = 0.2f;
      tBrowAngle = -11.0f; tBrowLift = 2.0f; tBrowVisibility = 1.0f;
      tMouthWidth = 42.0f; tMouthCurve = -11.0f;
      tMouthOpen = 2.0f + fabs(sin(now * 0.009f)) * 4.0f;
      exprLookX = sin(now * 0.014f) * 1.5f;
      exprBobY = fabs(sin(now * 0.009f)) * 4.0f;
      break;
    case 31: // Nakal: wink, alis satu naik, smirk
      tEyeLX = 1.0f; tEyeRX = 1.03f; tEyeLY = 0.16f; tEyeRY = 0.68f;
      tPupilScale = 0.72f; tBrowAngle = -4.0f; tBrowLift = 2.0f; tBrowVisibility = 0.75f;
      tMouthWidth = 42.0f; tMouthCurve = 8.0f; tMouthSlant = -7.0f;
      exprLookX = 7.0f; exprLeanX = sin(now * 0.0025f) * 2.0f;
      break;
    case 50: // Love dari touch
      tEyeLX = tEyeRX = 1.05f; tEyeLY = tEyeRY = 0.95f;
      tMouthWidth = 46.0f; tMouthCurve = 10.0f;
      heartPupils = true; exprBobY = sin(now * 0.006f) * 4.0f;
      break;
    case 51: // Laugh
      tEyeLY = tEyeRY = 0.22f;
      tMouthWidth = 54.0f; tMouthCurve = 12.0f; tMouthOpen = 18.0f;
      exprBobY = sin(now * 0.013f) * 7.0f;
      break;
    case 32: // Menyapa ramah (Greeting)
      tEyeLY = tEyeRY = 0.55f;
      tPupilScale = 0.85f; tBrowLift = 4.0f; tBrowVisibility = 0.8f;
      tMouthWidth = 46.0f; tMouthCurve = 9.0f;
      exprBobY = sin(now * 0.005f) * 3.0f;
      break;
    case 33: // Santai (Relaxed)
      tEyeLX = tEyeRX = 0.96f; tEyeLY = tEyeRY = 0.70f;
      tMouthWidth = 38.0f; tMouthCurve = 5.0f;
      exprBobY = sin(now * 0.003f) * 2.0f;
      break;
    case 34: // Menatap lembut (Soft gaze)
      tEyeLX = tEyeRX = 1.05f; tEyeLY = tEyeRY = 0.80f;
      tPupilScale = 0.90f; tBrowLift = 2.0f; tBrowVisibility = 0.6f;
      tMouthWidth = 34.0f; tMouthCurve = 3.0f;
      exprBobY = sin(now * 0.004f) * 1.5f;
      break;
    default:
      break;
  }

  if (now >= nextSaccadeMs) {
    saccadeTargetX = (random(100) / 99.0f - 0.5f) * 10.0f;
    saccadeTargetY = (random(100) / 99.0f - 0.5f) * 6.0f;
    nextSaccadeMs = now + random(900, 2800);
  }
  saccadeX = morphValue(saccadeX, saccadeTargetX, 0.22f);
  saccadeY = morphValue(saccadeY, saccadeTargetY, 0.22f);

  if (nextBlinkMs == 0) nextBlinkMs = now + random(2200, 5200);
  if (blinkStartedMs == 0 && now >= nextBlinkMs && tEyeLY > 0.5f && tEyeRY > 0.5f) {
    blinkStartedMs = now;
    nextBlinkMs = now + random(2600, 5600);
  }
  float blinkScale = 1.0f;
  if (blinkStartedMs > 0) {
    unsigned long elapsed = now - blinkStartedMs;
    if (elapsed < 90) blinkScale = 1.0f - elapsed / 90.0f * 0.92f;
    else if (elapsed < 145) blinkScale = 0.08f;
    else if (elapsed < 280) blinkScale = 0.08f + (elapsed - 145) / 135.0f * 0.92f;
    else blinkStartedMs = 0;
  }
  tEyeLY *= blinkScale;
  tEyeRY *= blinkScale;

  if (aiSpeaking && now > aiSpeakingUntilMs && (now - lastAudioPlayedMs) > 500UL) aiSpeaking = false;
  bool speechFace = aiSpeaking || ((now - lastAudioPlayedMs) < 450UL);
  if (speechFace) {
    float cuteBlink = 0.5f + 0.5f * sin(now * 0.0045f);
    tEyeLX = max(tEyeLX, 1.03f);
    tEyeRX = max(tEyeRX, 1.03f);
    tEyeLY = max(tEyeLY, 0.78f + cuteBlink * 0.10f);
    tEyeRY = max(tEyeRY, 0.78f + cuteBlink * 0.10f);
    tPupilScale = max(tPupilScale, 0.74f);
    tBrowLift = max(tBrowLift, 3.0f);
    tBrowVisibility = max(tBrowVisibility, 0.28f);
    tMouthWidth = max(tMouthWidth, 38.0f);
    tMouthCurve = max(tMouthCurve, 5.0f);
    pupilHighlights = true;
    exprBobY += sin(now * 0.006f) * 2.0f;
    exprLookY += sin(now * 0.0038f) * 1.4f;
  }
  bool audioActive = (now - lastAudioPlayedMs) < 180UL && ttsMouthLevel > 0.018f;
  if (audioActive) {
    float mouthDrive = constrain(ttsMouthLevel, 0.0f, 1.0f);
    float syllable = 0.72f + 0.28f * sin(now * 0.040f);
    tMouthOpen = max(tMouthOpen, 4.0f + mouthDrive * 18.0f * syllable);
    tMouthWidth = max(tMouthWidth, 36.0f + mouthDrive * 16.0f);
    tMouthCurve = max(tMouthCurve, 6.0f - mouthDrive * 1.5f);
    exprBobY += mouthDrive * 1.8f;
    exprLeanX += sin(now * 0.007f) * mouthDrive * 1.8f;
  }

  eyeLX = morphValue(eyeLX, tEyeLX, 0.20f);
  eyeLY = morphValue(eyeLY, tEyeLY, 0.24f);
  eyeRX = morphValue(eyeRX, tEyeRX, 0.20f);
  eyeRY = morphValue(eyeRY, tEyeRY, 0.24f);
  eyeOffsetY = morphValue(eyeOffsetY, tEyeOffsetY, 0.16f);
  pupilScale = morphValue(pupilScale, tPupilScale, 0.18f);
  mouthWidth = morphValue(mouthWidth, tMouthWidth, 0.16f);
  mouthCurve = morphValue(mouthCurve, tMouthCurve, 0.15f);
  mouthOpen = morphValue(mouthOpen, tMouthOpen, 0.20f);
  mouthSlant = morphValue(mouthSlant, tMouthSlant, 0.16f);
  browAngle = morphValue(browAngle, tBrowAngle, 0.16f);
  browLift = morphValue(browLift, tBrowLift, 0.16f);
  browVisibility = morphValue(browVisibility, tBrowVisibility, 0.16f);
  lidL = morphValue(lidL, tLidL, 0.17f);
  lidR = morphValue(lidR, tLidR, 0.17f);

  float breathing = 1.0f + sin(now * 0.0015f) * 0.018f;
  float baseScale = 2.38f * breathing;
  float verticalSpread = 1.18f;
  float bodyBob = sin(now * 0.002f) * 2.4f;
  float headShiftX = targetLookX * 0.35f + exprLeanX;
  float headShiftY = targetLookY * 0.25f + bodyBob + exprBobY;
  int originX = (240 - (int)(128 * baseScale)) / 2 + (int)headShiftX;
  int originY = (320 - (int)(64 * verticalSpread * baseScale)) / 2 + (int)headShiftY;

  int nominalEyeW = max(10, (int)(26 * baseScale));
  int nominalEyeH = max(14, (int)(37 * verticalSpread * baseScale));
  int leftCX = originX + (int)(41 * baseScale);
  int rightCX = originX + (int)(89 * baseScale);
  int eyeCY = originY + (int)((13 + 18.5f) * verticalSpread * baseScale) + (int)(eyeOffsetY * baseScale);
  int leftW = max(5, (int)(nominalEyeW * eyeLX));
  int leftH = max(3, (int)(nominalEyeH * eyeLY));
  int rightW = max(5, (int)(nominalEyeW * eyeRX));
  int rightH = max(3, (int)(nominalEyeH * eyeRY));
  int leftX = leftCX - leftW / 2;
  int leftY = eyeCY - leftH / 2;
  int rightX = rightCX - rightW / 2;
  int rightY = eyeCY - rightH / 2;

  spr.fillSprite(TFT_BLACK);
  uint16_t faceColor = spr.color565(0, 205, 255);
  drawBitmapScaled(leftX, leftY, image_Layer_9_bits, 26, 37,
                   baseScale * eyeLX, verticalSpread * baseScale * eyeLY, faceColor);
  drawBitmapScaled(rightX, rightY, image_Layer_8_bits, 26, 37,
                   baseScale * eyeRX, verticalSpread * baseScale * eyeRY, faceColor);
  drawLidMask(leftX, leftY, leftW, leftH, lidL, false);
  drawLidMask(rightX, rightY, rightW, rightH, lidR, true);

  drawMorphBrow(leftCX, leftY, nominalEyeW, browAngle, browLift,
                browVisibility, false, faceColor);
  drawMorphBrow(rightCX, rightY, nominalEyeW, browAngle, browLift,
                browVisibility, true, faceColor);

  float pupilLookX = saccadeX + exprLookX + targetLookX * 1.4f;
  float pupilLookY = saccadeY + exprLookY + targetLookY * 1.2f;
  int basePupilRadius = max(3, (int)(4.0f * baseScale * pupilScale));
  auto drawPupilForEye = [&](int cx, int cy, int w, int h, bool visible) {
    if (!visible) return;
    int radius = min(basePupilRadius, max(2, h / 2 - 3));
    int maxMoveX = max(0, w / 2 - radius - 5);
    int maxMoveY = max(0, h / 2 - radius - 5);
    int px = cx + constrain((int)pupilLookX, -maxMoveX, maxMoveX);
    int py = cy + constrain((int)pupilLookY, -maxMoveY, maxMoveY);
    if (heartPupils) drawHeartPupil(px, py, radius);
    else if (spiralPupils) drawSpiralPupil(px, py, radius);
    else {
      spr.fillCircle(px, py, radius, TFT_BLACK);
      if (pupilHighlights && radius >= 5) spr.fillCircle(px - radius / 3, py - radius / 3, max(1, radius / 4), TFT_WHITE);
    }
  };
  drawPupilForEye(leftCX, eyeCY, leftW, leftH, eyeLY > 0.32f);
  drawPupilForEye(rightCX, eyeCY, rightW, rightH, eyeRY > 0.32f);

  int mouthCX = originX + (int)(65 * baseScale);
  int mouthCY = originY + (int)(51 * verticalSpread * baseScale);
  drawMorphMouth(mouthCX, mouthCY, mouthWidth, mouthCurve, mouthOpen, mouthSlant, faceColor);

  if (expression == 25) {
    int sweatY = leftY - 2 + (int)(fabs(sin(now * 0.003f)) * 6.0f);
    spr.fillCircle(leftX - 8, sweatY + 8, 4, faceColor);
    spr.fillTriangle(leftX - 12, sweatY + 7, leftX - 4, sweatY + 7, leftX - 8, sweatY, faceColor);
  }

  if (expression == 29) {
    uint16_t tearColor = spr.color565(0, 145, 255);
    int tearTravel = (int)((now % 900UL) / 900.0f * 55.0f);
    int tearTravelR = (int)(((now + 430UL) % 1050UL) / 1050.0f * 52.0f);
    int tearStartY = eyeCY + max(leftH, rightH) / 2 - 2;
    spr.fillRoundRect(leftCX - 8, tearStartY, 6, 24 + tearTravel / 3, 3, tearColor);
    spr.fillCircle(leftCX - 5, tearStartY + 20 + tearTravel, 6, tearColor);
    spr.fillTriangle(leftCX - 11, tearStartY + 18 + tearTravel,
                     leftCX + 1, tearStartY + 18 + tearTravel,
                     leftCX - 5, tearStartY + 8 + tearTravel, tearColor);
    spr.fillRoundRect(rightCX + 2, tearStartY, 6, 18 + tearTravelR / 3, 3, tearColor);
    spr.fillCircle(rightCX + 5, tearStartY + 18 + tearTravelR, 5, tearColor);
  }

  if (expression == 5 || isSleeping) {
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(spr.color565(150, 200, 255));
    spr.setTextSize(3);
    int z1Offset = (now / 15) % 50;
    if (z1Offset < 40) spr.drawString("Z", originX + 160 + (int)(sin(now*0.003)*5), originY - z1Offset);
    spr.setTextSize(2);
    int z2Offset = ((now + 500) / 15) % 50;
    if (z2Offset < 40) spr.drawString("z", originX + 175 + (int)(sin(now*0.003+1)*5), originY + 10 - z2Offset);
    int z3Offset = ((now + 1000) / 15) % 50;
    if (z3Offset < 40) spr.drawString("z", originX + 185 + (int)(sin(now*0.003+2)*5), originY + 20 - z3Offset);
    spr.setTextDatum(TL_DATUM);
  }

  if (isChatActive) {
    spr.fillRect(0, 288, 240, 32, TFT_BLACK);
    spr.drawFastHLine(0, 288, 240, spr.color565(25, 72, 94));
    spr.setTextSize(2);
    spr.setTextColor(TFT_WHITE);
    int textW = spr.textWidth(currentChatText);
    spr.setCursor(chatTextX, 298);
    spr.print(currentChatText);
    chatTextX -= 3;
    if (chatTextX < -textW) isChatActive = false;
  }
}

void drawMenu() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(3);
  spr.setCursor(10, 20);
  spr.print("MENU GEMBOT");
  spr.drawLine(10, 50, 230, 50, TFT_WHITE);
  
  for (int i = 0; i < menuCount; i++) {
    int y = 70 + (i * 40);
    if (i == menuCursor) {
      spr.fillRect(5, y - 5, 230, 35, spr.color565(50, 100, 250));
    }
    spr.setCursor(15, y);
    spr.print(menuItems[i]);
  }
}

String fitToWidth(String text, int maxWidth, uint8_t font) {
  if (spr.textWidth(text, font) <= maxWidth) return text;
  while (text.length() > 1 && spr.textWidth(text + ".", font) > maxWidth) {
    text.remove(text.length() - 1);
  }
  return text + ".";
}

void drawAppHeader(const String& title, const String& subtitle, uint16_t accent) {
  spr.setTextSize(1);
  spr.fillRoundRect(12, 10, 216, 46, 10, spr.color565(28, 28, 28));
  spr.drawRoundRect(12, 10, 216, 46, 10, spr.color565(58, 58, 58));
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(accent);
  spr.drawString(fitToWidth(title, 190, 2), 24, 18, 2);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(fitToWidth(subtitle, 190, 1), 24, 40, 1);
}

void drawTinyFooter(const String& text, uint16_t color = TFT_WHITE) {
  spr.setTextSize(1);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(color);
  spr.drawString(fitToWidth(text, 210, 1), 120, 306, 1);
}

void drawPingPong() {
  static unsigned long lastFrameMs = 0;
  static float smoothPaddleX = 120;

  unsigned long now = millis();
  if (now - lastFrameMs < 25) return;
  float dt = constrain((now - lastFrameMs) / 25.0f, 0.5f, 2.0f);
  lastFrameMs = now;

  if (gameOver) {
    spr.fillSprite(TFT_BLACK);
    spr.setTextSize(1);
    spr.setTextDatum(MC_DATUM);
    uint16_t resultColor = playerWon ? TFT_GREEN : TFT_RED;
    drawAppHeader("PINGPONG", "Skor akhir", TFT_CYAN);
    spr.setTextDatum(MC_DATUM);
    spr.fillRoundRect(14, 78, 212, 174, 14, spr.color565(24, 24, 24));
    spr.drawRoundRect(14, 78, 212, 174, 14, resultColor);
    spr.setTextColor(resultColor);
    spr.drawString(playerWon ? "KAMU" : "GEMBOT", 120, 108, 2);
    spr.drawString("MENANG", 120, 132, 2);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(String(score) + " : " + String(scoreAI), 120, 172, 4);
    spr.setTextColor(TFT_CYAN);
    spr.drawString(playerWon ? "Mantap!" : "Coba lagi", 120, 224, 2);
    drawTinyFooter("Tahan untuk ulang", TFT_YELLOW);
    pushSpriteSafe();
    return;
  }

  float rawPaddleTarget = 120.0f + targetLookX * 12.0f;
  if (rawPaddleTarget < 35) rawPaddleTarget = 35;
  if (rawPaddleTarget > 205) rawPaddleTarget = 205;
  smoothPaddleX += (rawPaddleTarget - smoothPaddleX) * 0.28f;
  paddleX = smoothPaddleX;

  float aiPredict = ballX + ballVX * constrain((ballY - 34.0f) / 42.0f, -5.0f, 6.0f);
  float aiTarget = constrain(aiPredict, 35.0f, 205.0f);
  float aiStep = constrain(2.0f + score * 0.10f - scoreAI * 0.06f, 1.7f, 2.7f) * dt;
  aiPaddleX += constrain(aiTarget - aiPaddleX, -aiStep, aiStep);

  ballX += ballVX * dt;
  ballY += ballVY * dt;

  if (ballX <= 8) { ballX = 8; ballVX = fabs(ballVX); playBeep(1200.0f, 30); }
  if (ballX >= 232) { ballX = 232; ballVX = -fabs(ballVX); playBeep(1200.0f, 30); }

  if (ballY >= 268 && ballY <= 293 && ballVY > 0) {
    float diff = ballX - paddleX;
    if (fabs(diff) < 42) {
      ballY = 268;
      ballVY = -fabs(ballVY);
      ballVX = constrain(diff * 0.11f, -5.2f, 5.2f);
      ballVY *= 1.03f;
      playBeep(2000.0f, 50);
    }
  }
  if (ballY <= 60 && ballY >= 38 && ballVY < 0) {
    float diff = ballX - aiPaddleX;
    if (fabs(diff) < 38) {
      ballY = 60;
      ballVY = fabs(ballVY);
      ballVX = constrain(diff * 0.10f, -4.8f, 4.8f);
      playBeep(1500.0f, 40);
    }
  }
  if (ballY < -8) {
    score++;
    ballX = 120; ballY = 160; ballVX = -2.8f; ballVY = -3.4f;
    playBeep(1900.0f, 100);
  }
  if (ballY > 328) {
    scoreAI++;
    ballX = 120; ballY = 160; ballVX = 2.8f; ballVY = 3.4f;
    playBeep(400.0f, 140);
  }
  if (score >= 5 || scoreAI >= 5) {
    playerWon = score >= 5;
    gameOver = true;
  }

  spr.fillSprite(TFT_BLACK);
  spr.setTextSize(1);
  spr.drawRoundRect(10, 58, 220, 228, 10, spr.color565(55, 55, 55));
  for (int y = 68; y < 274; y += 18) spr.drawFastHLine(114, y, 12, spr.color565(55, 55, 55));
  spr.fillRoundRect(12, 10, 216, 40, 8, spr.color565(28, 28, 28));
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_ORANGE);
  spr.drawString("GEMBOT", 50, 18, 1);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(String(scoreAI), 50, 36, 2);
  spr.setTextColor(TFT_CYAN);
  spr.drawString("KAMU", 190, 18, 1);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(String(score), 190, 36, 2);
  spr.setTextColor(TFT_DARKGREY);
  spr.drawString("5 poin menang", 120, 304, 1);

  spr.fillRoundRect((int)aiPaddleX - 35, 61, 70, 11, 4, TFT_ORANGE);
  spr.fillRoundRect((int)paddleX - 38, 274, 76, 12, 4, TFT_CYAN);
  spr.drawCircle((int)ballX, (int)ballY, 10, spr.color565(65, 100, 125));
  spr.fillCircle((int)ballX, (int)ballY, 7, TFT_WHITE);
  pushSpriteSafe();
}

void drawSuhu() {
  spr.fillSprite(TFT_BLACK);
  
  // Draw modern UI for DHT22
  spr.fillRoundRect(20, 40, 200, 100, 10, spr.color565(40, 40, 40));
  spr.fillRoundRect(20, 160, 200, 100, 10, spr.color565(40, 40, 40));

  spr.setTextColor(TFT_ORANGE);
  spr.setTextSize(2);
  spr.setCursor(30, 50);
  spr.print("SUHU RUANGAN");
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(4);
  spr.setCursor(30, 90);
  if (currentSuhu > 0) {
    spr.print(currentSuhu, 1);
    spr.print(" C");
  } else {
    spr.print("--.- C");
  }

  spr.setTextColor(TFT_CYAN);
  spr.setTextSize(2);
  spr.setCursor(30, 170);
  spr.print("KELEMBAPAN");
  spr.setTextColor(TFT_WHITE);
  spr.setTextSize(4);
  spr.setCursor(30, 210);
  if (currentLembap > 0) {
    spr.print(currentLembap, 1);
    spr.print(" %");
  } else {
    spr.print("--.- %");
  }
}


void drawPengingat() {
  uint16_t bg = TFT_BLACK;
  uint16_t card = spr.color565(30, 30, 30);
  uint16_t accent = TFT_CYAN;
  spr.fillSprite(bg);
  spr.setTextSize(1);

  drawAppHeader(reminderAlertIndex >= 0 ? "WAKTUNYA" : "PENGINGAT",
                reminderAlertIndex >= 0 ? "Ada jadwal" : String(reminderCount) + " jadwal aktif",
                TFT_ORANGE);

  spr.fillRoundRect(14, 70, 212, 212, 12, card);
  spr.drawRoundRect(14, 70, 212, 212, 12, spr.color565(82, 82, 82));

  if (reminderAlertIndex >= 0 && reminderAlertIndex < reminderCount) {
    ReminderItem &item = reminderItems[reminderAlertIndex];
    float pulse = (sin(millis() * 0.012f) + 1.0f) * 0.5f;
    int bellRadius = 19 + (int)(pulse * 3.0f);
    spr.fillCircle(120, 107, bellRadius, accent);
    spr.fillRoundRect(111, 94, 18, 22, 9, TFT_WHITE);
    spr.fillRect(107, 110, 26, 5, TFT_WHITE);
    spr.fillCircle(120, 119, 3, TFT_WHITE);

    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_YELLOW);
    spr.drawString(String(item.time), 120, 154, 4);
    String message = String(item.text);
    String line1 = message;
    String line2 = "";
    if (message.length() > 20) {
      int split = message.lastIndexOf(' ', 20);
      if (split < 8) split = 20;
      line1 = message.substring(0, split);
      line2 = message.substring(split + 1);
    }
    line1 = fitToWidth(line1, 174, 2);
    line2 = fitToWidth(line2, 174, 2);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(line1, 120, 204, 2);
    if (line2.length()) spr.drawString(line2, 120, 225, 2);
    spr.setTextColor(TFT_CYAN);
    String dateLabel = item.date[0] ? String(item.date) : "Setiap hari";
    spr.drawString(dateLabel, 120, 256, 1);
  } else if (reminderCount <= 0) {
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_WHITE);
    spr.drawString("Belum ada", 120, 144, 2);
    spr.setTextColor(TFT_CYAN);
    spr.drawString("Atur dari web", 120, 175, 2);
  } else {
    for (int i = 0; i < reminderCount && i < MAX_REMINDERS; i++) {
      int y = 82 + i * 37;
      bool active = reminderItems[i].active;
      uint16_t rowColor = active ? spr.color565(22, 54, 72) : spr.color565(38, 38, 38);
      uint16_t rowText = active ? TFT_WHITE : spr.color565(175, 185, 190);
      spr.fillRoundRect(24, y, 192, 32, 7, rowColor);
      spr.drawRoundRect(24, y, 192, 32, 7, active ? accent : spr.color565(82, 82, 82));
      spr.fillCircle(34, y + 16, 3, active ? accent : spr.color565(115, 132, 142));
      spr.setTextDatum(TL_DATUM);
      spr.setTextColor(rowText);
      spr.drawString(String(reminderItems[i].time), 44, y + 5, 2);
      String msg = String(reminderItems[i].text);
      msg = fitToWidth(msg, 98, 1);
      spr.drawString(msg, 108, y + 5, 1);
      String dateLabel = reminderItems[i].date[0] ? String(reminderItems[i].date).substring(5) : "setiap hari";
      spr.setTextColor(active ? TFT_CYAN : rowText);
      spr.drawString(fitToWidth(dateLabel, 98, 1), 108, y + 19, 1);
    }
  }

  if (reminderAlertIndex >= 0) {
    drawTinyFooter("Tahan untuk tutup", TFT_YELLOW);
  } else if (millis() - reminderSyncedMs < 5200UL) {
    drawTinyFooter("Tersimpan", TFT_GREEN);
  } else {
    drawTinyFooter("Tahan untuk kembali", TFT_WHITE);
  }
}

float trackSectionEnergy(int track, int elapsedSec) {
  if (track == 1) {
    if (elapsedSec < 18) return 0.32f;
    if (elapsedSec < 52) return 0.54f;
    if (elapsedSec < 86) return 0.92f;
    if (elapsedSec < 118) return 0.58f;
    if (elapsedSec < 154) return 0.98f;
    if (elapsedSec < 181) return 0.48f + (elapsedSec - 154) * 0.018f;
    if (elapsedSec < 224) return 1.05f;
    return 0.66f;
  }
  if (track == 2) {
    int section = elapsedSec % 15;
    return section < 3 ? 0.55f : (section < 11 ? 0.95f : 0.72f);
  }
  if (track == 3) {
    int section = elapsedSec % 64;
    return section < 18 ? 0.46f : (section < 42 ? 0.86f : 0.62f);
  }
  if (track == 4) {
    int section = elapsedSec % 32;
    return section < 8 ? 0.70f : (section < 24 ? 1.08f : 0.82f);
  }
  return 0.7f;
}

float musicVisualEnergy() {
  static float envelope = 0.0f;
  if (!dfMusicPlaying || dfMusicPaused || dfTrackStartedMs == 0) {
    envelope *= 0.72f;
    if (envelope < 0.012f) envelope = 0.0f;
    return envelope;
  }

  unsigned long playMs = millis() - dfTrackStartedMs;
  int elapsedSec = (int)(playMs / 1000UL);
  float section = trackSectionEnergy(normalizeDfTrack(dfCurrentTrack), elapsedSec);
  float beatFast = 0.5f + 0.5f * sin(playMs * 0.019f);
  float beatMid = 0.5f + 0.5f * sin(playMs * 0.0067f + dfCurrentTrack);
  float beatSlow = 0.5f + 0.5f * sin(playMs * 0.0028f + dfCurrentTrack * 0.7f);
  float raw = constrain(section * (0.46f + beatFast * 0.34f + beatMid * 0.14f + beatSlow * 0.06f), 0.0f, 1.12f);
  float speed = raw > envelope ? 0.38f : 0.13f;
  envelope += (raw - envelope) * speed;
  return envelope;
}

void drawNowPlaying() {
  unsigned long now = millis();
  int track = normalizeDfTrack(dfCurrentTrack);
  uint16_t bg = TFT_BLACK;
  uint16_t card = spr.color565(30, 30, 30);
  uint16_t accent = spr.color565(46, 155, 255);
  uint16_t soft = spr.color565(205, 225, 235);
  uint16_t dur = dfTrackDurationSec[track] ? dfTrackDurationSec[track] : 180;
  unsigned long elapsedMs = 0;
  if (dfTrackStartedMs > 0) {
    elapsedMs = (dfMusicPaused && dfPausedAtMs > dfTrackStartedMs) ? (dfPausedAtMs - dfTrackStartedMs) : (now - dfTrackStartedMs);
  }
  int elapsedSec = constrain((int)(elapsedMs / 1000UL), 0, (int)dur);
  float energy = dfMusicPaused ? 0.0f : musicVisualEnergy();
  bool audioVisible = energy > 0.025f;

  spr.fillSprite(bg);
  spr.setTextSize(1);
  drawAppHeader("MUSIK", dfMusicPaused ? "Dijeda" : "Sedang diputar", TFT_CYAN);

  String title = dfTrackTitles[track];
  String artist = dfTrackArtists[track];
  title = fitToWidth(title, 188, 2);
  artist = fitToWidth(artist, 188, 1);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE);
  spr.drawString(title, 120, 72, 2);
  spr.setTextColor(soft);
  spr.drawString(artist, 120, 92, 1);

  spr.fillRoundRect(16, 106, 208, 98, 14, card);
  spr.drawRoundRect(16, 106, 208, 98, 14, spr.color565(72, 72, 72));

  int cx = 120;
  int cy = 150;
  int pulse = audioVisible ? (int)(energy * 9.0f) : 0;
  spr.fillCircle(cx, cy, 25 + pulse / 2, spr.color565(12, 24, 38));
  spr.drawCircle(cx, cy, 31, accent);
  spr.drawCircle(cx, cy, 24, spr.color565(70, 120, 160));
  spr.fillCircle(cx, cy, 9, accent);

  static float barLevel[13] = {0};
  const float weights[13] = {0.35f, 0.58f, 0.80f, 1.00f, 0.74f, 0.92f, 1.18f, 0.90f, 0.70f, 1.04f, 0.82f, 0.60f, 0.42f};
  for (int i = 0; i < 13; i++) {
    float phaseA = now * (0.0065f + i * 0.00055f) + i * 0.73f + track * 0.41f;
    float phaseB = now * (0.0145f + energy * 0.009f) + i * 1.19f;
    float wave = 0.56f + 0.28f * sin(phaseA) + 0.16f * sin(phaseB);
    float target = audioVisible ? constrain(energy * weights[i] * wave, 0.05f, 1.0f) : 0.08f;
    float follow = target > barLevel[i] ? 0.34f : 0.12f;
    barLevel[i] += (target - barLevel[i]) * follow;
    int h = 5 + (int)(barLevel[i] * 39.0f);
    h = constrain(h, 5, 43);
    int x = 36 + i * 14;
    uint16_t barColor = spr.color565(70 + (int)(barLevel[i] * 40), 180 + (int)(barLevel[i] * 45), 255);
    spr.fillRoundRect(x, 196 - h, 8, h, 4, barColor);
    spr.fillCircle(x + 4, 196 - h, 3, TFT_WHITE);
  }

  spr.setTextColor(soft);
  spr.drawString("VOL " + String(dfVolume), 120, 253, 1);

  int progressW = map(elapsedSec, 0, dur, 0, 188);
  spr.fillRoundRect(26, 276, 188, 5, 2, spr.color565(70, 95, 115));
  spr.fillRoundRect(26, 276, progressW, 5, 2, accent);
  spr.setTextColor(soft);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(String(elapsedSec / 60) + ":" + (elapsedSec % 60 < 10 ? "0" : "") + String(elapsedSec % 60), 26, 286, 1);
  spr.setTextDatum(TR_DATUM);
  spr.drawString(String(dur / 60) + ":" + (dur % 60 < 10 ? "0" : "") + String(dur % 60), 214, 286, 1);

  spr.setTextDatum(MC_DATUM);
  spr.drawCircle(70, 226, 15, soft);
  spr.fillTriangle(75, 218, 75, 234, 64, 226, soft);
  spr.drawFastVLine(60, 218, 16, soft);
  spr.fillCircle(120, 226, 18, TFT_WHITE);
  if (dfMusicPaused) {
    spr.fillTriangle(115, 216, 115, 236, 130, 226, bg);
  } else {
    spr.fillRoundRect(113, 217, 5, 18, 2, bg);
    spr.fillRoundRect(122, 217, 5, 18, 2, bg);
  }
  spr.drawCircle(170, 226, 15, soft);
  spr.fillTriangle(165, 218, 165, 234, 176, 226, soft);
  spr.drawFastVLine(180, 218, 16, soft);
}

void drawMusik() {
  if (dfMusicPlaying) {
    drawNowPlaying();
    return;
  }

  spr.fillSprite(TFT_BLACK);
  spr.setTextSize(1);
  drawAppHeader("MUSIK", "Pilih lagu", TFT_CYAN);
  
  for (int i = 0; i < musicCount; i++) {
    int y = 72 + (i * 42);
    if (i == musicCursor) {
      spr.fillRoundRect(16, y - 5, 208, 34, 7, spr.color565(38, 110, 150));
    } else {
      spr.fillRoundRect(16, y - 5, 208, 34, 7, spr.color565(30, 30, 30));
    }
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(fitToWidth(String(i + 1) + ". " + musicItems[i], 180, 2), 28, y + 4, 2);
  }
  drawTinyFooter("Tahan untuk pilih", TFT_DARKGREY);
}


void drawListening() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextSize(1);
  drawAppHeader("MENDENGARKAN", "Lepas untuk tanya AI", TFT_CYAN);
  spr.setTextDatum(MC_DATUM);

  // Smooth animation using sin()
  unsigned long ms = millis();
  float pulse = (sin(ms * 0.005) + 1.0) / 2.0; // 0.0 to 1.0
  float level = constrain(voiceLevel, 0.0f, 1.0f);
  int radius1 = 30 + (pulse * 12) + (int)(level * 22);
  int radius2 = 52 + (pulse * 18) + (int)(level * 28);
  int radius3 = 74 + (pulse * 24) + (int)(level * 32);

  spr.fillRoundRect(20, 78, 200, 184, 14, spr.color565(30, 30, 30));
  spr.drawRoundRect(20, 78, 200, 184, 14, spr.color565(72, 72, 72));
  spr.drawCircle(120, 168, radius3, spr.color565(0, 95, 170));
  spr.drawCircle(120, 168, radius2, spr.color565(0, 165, 220));
  spr.fillCircle(120, 168, radius1, spr.color565(0, 220, 240));
  
  spr.setTextColor(TFT_BLACK);
  spr.drawString("MIC", 120, 168, 2);
  spr.setTextColor(TFT_WHITE);
  spr.drawString("INMP " + String((int)constrain(voiceLevel * 100.0f, 0.0f, 100.0f)) + "%", 120, 238, 2);
  drawTinyFooter("Tahan terus, lepas untuk kirim", TFT_DARKGREY);
}

void drawScreen() {
  if (currentState == APP_DRAW) return; // Drawn externally via FRAME packets
  if (!ensureSpriteReady()) return;
  
  if (currentState == APP_PINGPONG) {
     drawPingPong();
     return; // Bypass spr.pushSprite for 60 FPS direct drawing
  }

  if (currentState == APP_FACE) drawFaceAlive();
  else if (currentState == APP_MENU) drawMenu();
  else if (currentState == APP_SUHU) drawSuhu();
  else if (currentState == APP_PENGINGAT) drawPengingat();
  else if (currentState == APP_MUSIK) drawMusik();
  else if (currentState == APP_LISTENING) drawListening();

  pushSpriteSafe();
}


void setup() {
  Serial.begin(115200);

  mySoftwareSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  if (!myDFPlayer.begin(mySoftwareSerial)) {
    Serial.println(F("Unable to begin DFPlayer:"));
    Serial.println(F("1.Please recheck the connection!"));
    Serial.println(F("2.Please insert the SD card!"));
    dfPlayerReady = false;
  } else {
    Serial.println(F("DFPlayer Mini online."));
    dfPlayerReady = true;
    waitDfCommandGap();
    myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
    waitDfCommandGap();
    myDFPlayer.EQ(DFPLAYER_EQ_POP);
    applyDfVolume(true);
  }

  xTaskCreatePinnedToCore(dhtTask, "DHT Task", 2048, NULL, 1, NULL, 1);
  delay(100);

  dht.begin();
  loadRemindersFromFlash();

  display.begin();
  display.setRotation(0); // Portrait
  display.fillScreen(TFT_BLACK);
  
  ensureSpriteReady();
  Serial.printf("Sprite: %p depth:%d\n", spr.getPointer(), spr.getColorDepth());

  // Setup I2S Audio Buffers
  audioRingBuf = xRingbufferCreate(RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
  micStreamBuf = xStreamBufferCreate(MIC_STREAM_SIZE, 1);
  playBeep(1000.0f, 200);

  // External touch module: VCC/GND/SIGNAL, active HIGH.
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  // Setup MPU6050
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setTimeOut(150);
  if (!setupRawMPU()) {
    Serial.println("Failed with SDA=1, SCL=2. Trying swapped pins SDA=2, SCL=1...");
    Wire.end();
    Wire.begin(I2C_SCL, I2C_SDA);
    Wire.setTimeOut(150);
    if (!setupRawMPU()) {
      Serial.println("Failed to find MPU6050 chip even with swapped pins.");
    } else {
      mpuReady = true;
      Serial.println("MPU6050 Found with swapped pins!");
    }
  } else {
    mpuReady = true;
    Serial.println("MPU6050 Found manually!");
  }

  // Setup WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  int attempts = 0;
  // Morphing face boot animation — eyes start as tiny dots and grow
  float bootEyeScale = 0.0f;    // 0.0 = tiny dots, 1.0 = full size
  float bootMouthVis = 0.0f;    // mouth visibility
  float bootPupilVis = 0.0f;    // pupil visibility
  float bootBreathing = 0.0f;   // breathing amplitude
  uint16_t bootFaceColor = spr.color565(0, 205, 255);
  
  while (WiFi.status() != WL_CONNECTED && attempts < 300) {
      spr.fillSprite(TFT_BLACK);
      float t = millis() / 1000.0f;
      
      // Smoothly grow eyes from dots (0.05 -> 0.55 during connect)
      float targetScale = constrain(attempts / 180.0f, 0.05f, 0.55f);
      bootEyeScale += (targetScale - bootEyeScale) * 0.08f;
      
      // Gentle pulse while connecting
      float pulse = 1.0f + sin(t * 4.0f) * 0.12f;
      float scale = bootEyeScale * pulse;
      
      // Body bob starts subtle
      float bob = sin(t * 2.0f) * (bootEyeScale * 4.0f);
      
      // Calculate face layout (same math as drawFaceAlive)
      float baseScale = 2.38f * scale;
      float verticalSpread = 1.18f;
      int originX = (240 - (int)(128 * baseScale)) / 2;
      int originY = (320 - (int)(64 * verticalSpread * baseScale)) / 2 + (int)bob;
      
      int nominalEyeW = max(4, (int)(26 * baseScale));
      int nominalEyeH = max(4, (int)(37 * verticalSpread * baseScale));
      int leftCX = originX + (int)(41 * baseScale);
      int rightCX = originX + (int)(89 * baseScale);
      int eyeCY = originY + (int)((13 + 18.5f) * verticalSpread * baseScale);
      
      if (nominalEyeW < 8) {
        // Very early: just pulsing circles
        int r = max(2, nominalEyeW / 2);
        spr.fillCircle(leftCX, eyeCY, r, bootFaceColor);
        spr.fillCircle(rightCX, eyeCY, r, bootFaceColor);
      } else {
        // Growing eyes using bitmap
        int leftX = leftCX - nominalEyeW / 2;
        int leftY = eyeCY - nominalEyeH / 2;
        int rightX = rightCX - nominalEyeW / 2;
        int rightY = eyeCY - nominalEyeH / 2;
        drawBitmapScaled(leftX, leftY, image_Layer_9_bits, 26, 37,
                         baseScale, verticalSpread * baseScale, bootFaceColor);
        drawBitmapScaled(rightX, rightY, image_Layer_8_bits, 26, 37,
                         baseScale, verticalSpread * baseScale, bootFaceColor);
      }
      
      // Status text below face
      spr.setTextSize(1);
      spr.setTextDatum(MC_DATUM);
      spr.setTextColor(spr.color565(0, 180, 220));
      String dots = "";
      for (int i = 0; i <= ((attempts/8) % 3); i++) dots += ".";
      spr.drawString("Menghubungkan" + dots, 120, 280, 2);
      
      // Subtle ring animation around face
      int ringR = 40 + (int)(sin(t * 3.0f) * 8.0f);
      uint8_t ringAlpha = (uint8_t)(40 + sin(t * 2.5f) * 30);
      spr.drawCircle(120, eyeCY, ringR, spr.color565(0, ringAlpha, ringAlpha + 20));
      
      display.startWrite();
      spr.pushSprite(0, 0);
      display.endWrite();
      
      delay(30);
      if (attempts % 10 == 0) Serial.print(".");
      attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected!");
      configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
      webSocket.begin(WS_HOST, WS_PORT, "/");
      webSocket.onEvent(webSocketEvent);
      webSocket.setReconnectInterval(5000);
      webSocket.enableHeartbeat(15000, 3000, 2);
      playBeep(1500.0f, 150);
  } else {
      Serial.println("\nWiFi failed!");
  }

  // Debug: print heap and sprite status
  Serial.printf("Free heap before I2S: %d bytes\n", ESP.getFreeHeap());
  
  // Initialize I2S AFTER WiFi so WiFi can secure its DMA memory for RX buffers
  initI2S();
  initMic();

  Serial.printf("Free heap after I2S: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Sprite pointer: %p\n", spr.getPointer());
  
  // Morphing "wake up" animation — face grows to full size and smiles
  unsigned long wakeStartMs = millis();
  for(int i = 0; i < 45; i++) {
      spr.fillSprite(TFT_BLACK);
      float progress = constrain(i / 44.0f, 0.0f, 1.0f);
      // Ease-out cubic for smooth deceleration
      float eased = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
      float t = millis() / 1000.0f;
      
      // Eye scale: grow from connecting size (0.55) to full (1.0)
      float scale = 0.55f + eased * 0.45f;
      bootMouthVis += (eased - bootMouthVis) * 0.15f;
      bootPupilVis += (eased - bootPupilVis) * 0.12f;
      bootBreathing += (1.0f - bootBreathing) * 0.06f;
      
      float breathing = 1.0f + sin(t * 1.5f) * 0.018f * bootBreathing;
      float baseScale = 2.38f * scale * breathing;
      float verticalSpread = 1.18f;
      float bob = sin(t * 2.0f) * 2.4f * eased;
      
      int originX = (240 - (int)(128 * baseScale)) / 2;
      int originY = (320 - (int)(64 * verticalSpread * baseScale)) / 2 + (int)bob;
      
      int nominalEyeW = max(10, (int)(26 * baseScale));
      int nominalEyeH = max(14, (int)(37 * verticalSpread * baseScale));
      int leftCX = originX + (int)(41 * baseScale);
      int rightCX = originX + (int)(89 * baseScale);
      int eyeCY = originY + (int)((13 + 18.5f) * verticalSpread * baseScale);
      int leftX = leftCX - nominalEyeW / 2;
      int leftY = eyeCY - nominalEyeH / 2;
      int rightX = rightCX - nominalEyeW / 2;
      int rightY = eyeCY - nominalEyeH / 2;
      
      // Draw eyes (full bitmap at this stage)
      drawBitmapScaled(leftX, leftY, image_Layer_9_bits, 26, 37,
                       baseScale, verticalSpread * baseScale, bootFaceColor);
      drawBitmapScaled(rightX, rightY, image_Layer_8_bits, 26, 37,
                       baseScale, verticalSpread * baseScale, bootFaceColor);
      
      // Pupils fade in
      if (bootPupilVis > 0.15f) {
        int pupilR = max(3, (int)(4.0f * baseScale * 0.9f * bootPupilVis));
        spr.fillCircle(leftCX, eyeCY, pupilR, TFT_BLACK);
        spr.fillCircle(rightCX, eyeCY, pupilR, TFT_BLACK);
        // Highlight
        if (pupilR >= 5 && bootPupilVis > 0.6f) {
          spr.fillCircle(leftCX - pupilR/3, eyeCY - pupilR/3, max(1, pupilR/4), TFT_WHITE);
          spr.fillCircle(rightCX - pupilR/3, eyeCY - pupilR/3, max(1, pupilR/4), TFT_WHITE);
        }
      }
      
      // Mouth morphs in — starts as a line, curves into a smile
      if (bootMouthVis > 0.1f) {
        int mouthCX = originX + (int)(65 * baseScale);
        int mouthCY = originY + (int)(51 * verticalSpread * baseScale);
        float mouthW = 20.0f + bootMouthVis * 34.0f;
        float mouthCurve = bootMouthVis * 13.0f;
        drawMorphMouth(mouthCX, mouthCY, mouthW, mouthCurve, 0.0f, 0.0f, bootFaceColor);
      }
      
      // "GEMBOT READY!" text fades in at the end
      if (progress > 0.5f) {
        float textAlpha = constrain((progress - 0.5f) * 2.0f, 0.0f, 1.0f);
        uint8_t g = (uint8_t)(255 * textAlpha);
        uint8_t b = (uint8_t)(255 * textAlpha);
        spr.setTextSize(1);
        spr.setTextDatum(MC_DATUM);
        spr.setTextColor(spr.color565(0, g, b));
        int textY = 280 - (int)((progress - 0.5f) * 16.0f);
        spr.drawString("GEMBOT READY!", 120, textY, 4);
      }
      
      display.startWrite();
      spr.pushSprite(0, 0);
      display.endWrite();
      delay(30);
  }
  delay(400);

}

void dhtTask(void *pvParameters) {
  while(true) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
        currentSuhu = t;
        currentLembap = h;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void sendTelemetry() {
  unsigned long now = millis();
  static uint8_t wsSendFailCount = 0;
  if (now - lastTelemetryMs < 1000UL) return;
  lastTelemetryMs = now;
  if (!webSocket.isConnected()) return;

  StaticJsonDocument<768> doc;
  doc["type"] = "telemetry";
  doc["state"] = (int)currentState;
  doc["expr"] = String(displayedExpressionId);
  doc["temp"] = currentSuhu;
  doc["hum"] = currentLembap;
  doc["mpu"] = mpuReady ? 1 : 0;
  doc["touch"] = touchStartTime > 0 ? 1 : 0;
  doc["shakeMeter"] = constrain(currentJerk / 12.0f, 0.0f, 1.0f);
  doc["tiltX"] = lookX / 30.0f;
  doc["tiltY"] = lookY / 30.0f;
  doc["inmp"] = (int)constrain(voiceLevel * 100.0f, 0.0f, 100.0f);
  doc["inmpPeak"] = (int)constrain(micPeakLevel * 100.0f, 0.0f, 100.0f);
  doc["micReady"] = micReady ? 1 : 0;
  doc["micSlot"] = micSelectedSlot;
  doc["micRms"] = (int)micRawRms;
  doc["micErrors"] = micReadErrors;
  doc["micActive"] = voiceRecording ? 1 : 0;
  doc["wsAudioPackets"] = wsAudioPackets;
  doc["wsAudioBytes"] = wsAudioBytes;
  doc["voiceStatus"] = voiceRecording ? "listening" : "idle";
  if (aiSpeaking || ((now - lastAudioPlayedMs) < 220UL)) doc["voiceStatus"] = "speaking";
  doc["scoreP"] = score;
  doc["scoreA"] = scoreAI;
  doc["dizzy"] = (displayedExpressionId == 11 || displayedExpressionId == 30) ? 1 : 0;
  doc["angry"] = displayedExpressionId == 3 ? 1 : 0;
  doc["cry"] = displayedExpressionId == 29 ? 1 : 0;
  doc["love"] = displayedExpressionId == 50 ? 1 : 0;
  doc["laugh"] = displayedExpressionId == 51 ? 1 : 0;
  doc["sleep"] = displayedExpressionId == 5 ? 1 : 0;
  doc["surprised"] = (displayedExpressionId == 4 || displayedExpressionId == 28) ? 1 : 0;
  doc["dfReady"] = dfPlayerReady ? 1 : 0;
  doc["musicPlaying"] = dfMusicPlaying ? 1 : 0;
  doc["musicPaused"] = dfMusicPaused ? 1 : 0;
  doc["musicTrack"] = dfCurrentTrack;
  doc["musicVolume"] = dfVolume;
  doc["reminderCount"] = reminderCount;
  doc["reminderText"] = reminderCount > 0 ? reminderItems[0].text : "";
  doc["reminderTime"] = reminderCount > 0 ? reminderItems[0].time : "";
  doc["reminderDate"] = reminderCount > 0 ? reminderItems[0].date : "";
  doc["reminderActive"] = reminderCount > 0 && reminderItems[0].active ? 1 : 0;
  doc["reminderAlert"] = reminderAlertIndex >= 0 ? 1 : 0;

  String json;
  serializeJson(doc, json);
  
  if (isWsConnected) {
    if (webSocket.sendTXT(json)) {
      wsSendFailCount = 0;
    } else if (++wsSendFailCount >= 4) {
      wsSendFailCount = 0;
      webSocket.disconnect();
    }
  } else {
    wsSendFailCount = 0;
  }
}

void loop() {

  handleTouch();
  recoverDisplayIfNeeded();
  updateMPU();
  updateDfMusicEnd();
  updateAiSpeakingState();
  updateReminders();
  unsigned long now = millis();
  if (currentState == APP_PINGPONG) {
    drawScreen();
  } else if (now - lastScreenDrawMs >= UI_FRAME_MS) {
    lastScreenDrawMs = now;
    drawScreen();
  }

  
  webSocket.loop();
  yield();
  if (voiceRecording) sendPendingMicAudio();
  readAudioStream();
  sendTelemetry();
  
  if (now - lastInteractionMs > 300000) {
    if (!isSleeping) {
      isSleeping = true;
      if (currentState != APP_FACE) currentState = APP_FACE;
    }
  } else {
    isSleeping = false;
  }
  
  delay(1); // Keep WebSocket/audio responsive; rendering is frame-limited above.
}








