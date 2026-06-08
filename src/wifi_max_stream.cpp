#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>
#include "dasai_bitmaps.h"
#include "lopaka_screens.h"
#include "game_images.h"
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include "driver/i2s.h"
#include "secrets.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

#define OLED_WHITE SH110X_WHITE
#define OLED_BLACK SH110X_BLACK

#define MAX_BCLK_PIN D0
#define MAX_LRC_PIN D8
#define MAX_DIN_PIN D7
#define MIC_SD_PIN D10
#define DHT_PIN D2
#define DHT_TYPE DHT22
#define TOUCH_PIN D3

const uint16_t AUDIO_PORT = 7777;
const uint16_t TELEMETRY_PORT = 7788;
const uint32_t AUDIO_RATE = 16000;
const uint16_t AUDIO_FRAMES = 128;
const uint16_t AUDIO_BLOCK_BYTES = AUDIO_FRAMES * 2;
const uint16_t AUDIO_RING_SIZE = 24576;
const uint16_t AUDIO_PREBUFFER_BYTES = 6144;
const uint16_t READ_CHUNK = 1024;

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;
DHT dht(DHT_PIN, DHT_TYPE);
WiFiServer audioServer(AUDIO_PORT);
WiFiClient audioClient;
WiFiUDP telemetryUdp;
WiFiUDP micUdp;

bool oledReady = false;
bool mpuReady = false;
bool rawMpuMode = false;
uint8_t mpuAddr = 0x68;
uint8_t mpuWho = 0xFF;
bool dhtReady = false;
bool playing = false;
bool wasPlaying = false;
unsigned long lastAudioMs = 0;
unsigned long lastDrawMs = 0;
unsigned long lastMpuMs = 0;
unsigned long lastDhtMs = 0;
unsigned long lastTelemetryMs = 0;
uint32_t totalBytes = 0;
uint32_t playedBytes = 0;
float levelSmooth = 0.0f;
int16_t lastSample = 0;
// INMP441 Mic voice reactivity
volatile float micSmooth = 0.0f;    // 0.0 - 1.0 ambient mic loudness
volatile float micPeak  = 0.0f;    // short-term peak
unsigned long micActiveUntilMs = 0; // timestamp while voice is detected
unsigned long micShoutUntilMs  = 0; // timestamp when shout/loud detected
float faceLookX = 0.0f;
float faceLookY = 0.0f;
float faceLean = 0.0f;
float faceBob = 0.0f;
float faceDanceX = 0.0f;
float eyeSmoothH = 37.0f;
float cueSmooth = 0.0f;
float tiltX = 0.0f;
float tiltY = 0.0f;
float shakeSmooth = 0.0f;
float lastAx = 0.0f;
float lastAy = 0.0f;
float lastAz = 9.8f;
float rawAx = 0.0f;
float rawAy = 0.0f;
float rawAz = 9.8f;
float rawGx = 0.0f;
float rawGy = 0.0f;
float rawGz = 0.0f;
float tempC = NAN;
float humPct = NAN;
float tempMood = 0.0f;
float humidMood = 0.0f;
float shakeMeter = 0.0f;
float spinSmooth = 0.0f;
float spinMeter = 0.0f;
bool angryMode = false;
bool superAngryMode = false;       // triggered after sustained hard shaking
unsigned long angryUntilMs = 0;
unsigned long superAngryUntilMs = 0;
unsigned long dizzyUntilMs = 0;
unsigned long sadUntilMs = 0;
unsigned long annoyedUntilMs = 0;
unsigned long shakeActiveStartMs = 0; // when continuous shaking started
bool touchDown = false;
bool lastTouchReading = false;
unsigned long touchChangedMs = 0;
unsigned long lastTouchDebounce = 0;

// Chat State
char chatText[256] = "";
unsigned long chatStartMs = 0;
unsigned long chatDisplayUntilMs = 0;
unsigned long touchDownMs = 0;
unsigned long lastTapMs = 0;
unsigned long touchHappyUntilMs = 0;
unsigned long touchLoveUntilMs = 0;
unsigned long touchSleepyUntilMs = 0;
bool nodDetected = false;
unsigned long nodUntilMs = 0;
bool headShakeDetected = false;
unsigned long headShakeUntilMs = 0;
bool surprisedMode = false;
unsigned long surprisedUntilMs = 0;
bool curiousMode = false;
unsigned long curiousUntilMs = 0;
bool faceDownMode = false;
bool freefallMode = false;
unsigned long freefallUntilMs = 0;
unsigned long laughUntilMs = 0;
unsigned long glitchUntilMs = 0;
unsigned long pantUntilMs = 0;
unsigned long cryUntilMs = 0;
unsigned long lastActivityMs = 0;
bool isSleepingWithBubble = false;
int manualJoyX = 0;
int manualJoyY = 0;
unsigned long lastJoyMs = 0;
int manualMood = -1;
unsigned long manualMoodMs = 0;
float pitchHistory[4] = {0};
float yawHistory[4] = {0};
uint8_t histIdx = 0;
unsigned long lastHistMs = 0;
float sustainedTiltX = 0.0f;
unsigned long sustainedTiltStartMs = 0;
// Natural blink system
unsigned long nextBlinkMs = 0;
unsigned long blinkStartMs = 0;
bool isBlinking = false;
float blinkProgress = 0.0f;
bool winkLeft = false;
unsigned long nextWinkMs = 0;
unsigned long winkStartMs = 0;
bool isWinking = false;
float winkProgress = 0.0f;
// Idle glance & expression system
float idleGlanceTargetX = 0.0f;
float idleGlanceTargetY = 0.0f;
unsigned long nextGlanceMs = 0;
int idleExpr = 0;          // 0=normal,1=curious,2=shy,3=bored,4=excited,5=squint,6=thinking,7=smug
unsigned long nextIdleExprMs = 0;
// Breathing idle animation
float breathPhase = 0.0f;

// Pingpong Game state
enum GamePhase { GAME_IDLE, GAME_PLAYING, GAME_WAIT, GAME_OVER };
GamePhase gamePhase = GAME_IDLE;
unsigned long gameWaitStartMs = 0;
float ballX = 64.0f, ballY = 32.0f;
float ballVX = 3.0f, ballVY = 2.0f;
float paddlePlayerY = 32.0f, paddleAiY = 32.0f;
int scorePlayer = 0, scoreAi = 0;
unsigned long lastGameTickMs = 0;
unsigned long touchGameMs = 0;

void initPingpong(bool resetScore = false); // Forward declaration for updateTouch

// ─── TIMING ────────────────────────────────────────────────
#define MORPH_STEPS      20   // jumlah frame per transisi
#define HOLD_FRAMES     120   // frame tahan sebelum ganti emosi

// ─── STRUCT EMOSI ──────────────────────────────────────────
struct EyeParam {
  int8_t cx, cy, rx, ry; // center x/y, radius x/y
  int8_t lx;             // look x offset (pupil shift)
  uint8_t type;          // 0=normal, 1=heart, 2=star, 3=X, 4=squint, 5=wink, 6=dollar, 7=note, 8=lightning
};

struct BrowParam {
  int8_t x, y, w;        // start x, y, width
  int8_t angle;          // sudut × 10 (contoh: 25 = 0.25 rad)
};

struct MouthParam {
  int8_t x1, y1, x2, y2;// titik awal & akhir
  int8_t dep;            // depth (kedalaman kurva)
  int8_t cx, cy, rx, ry; // untuk oval/scream
  uint8_t type;          // 0=line, 1=smile, 2=frown, 3=oval, 4=wavy, 5=smirk, 6=teeth, 7=scream, 8=zigzag
};

struct EmoParam {
  const char* name;
  EyeParam  eyeL, eyeR;
  BrowParam browL, browR;
  uint8_t   lid;   // kelopak mata × 10 (0–10)
  MouthParam mouth;
  bool      tearL, tearR;
};

const EmoParam EMOTIONS[] PROGMEM = {
  // 0 NORMAL
  {"NORMAL",
    {41,31,11,14, 0,0}, {89,31,11,14, 0,0},
    {30,17,20, 0}, {78,17,20, 0}, 0,
    {52,50,76,50, 0, 0,0,0,0, 0}, false, false},
  // 1 HAPPY
  {"HAPPY",
    {41,29,11,11, 0,0}, {89,29,11,11, 0,0},
    {28,15,22,-2}, {80,15,22, 2}, 0,
    {48,49,80,49, 8, 0,0,0,0, 1}, false, false},
  // 2 SAD
  {"SAD",
    {41,33,11,12, 0,0}, {89,33,11,12, 0,0},
    {28,18,22, 3}, {80,18,22,-3}, 2,
    {50,53,78,53, 6, 0,0,0,0, 2}, false, false},
  // 3 ANGRY
  {"ANGRY",
    {41,31,10,13, 0,0}, {89,31,10,13, 0,0},
    {28,17,23, 4}, {79,17,23,-4}, 3,
    {49,52,79,52, 4, 0,0,0,0, 2}, false, false},
  // 4 LOVE
  {"LOVE",
    {41,30, 0, 0, 0,1}, {89,30, 0, 0, 0,1},
    {28,15,22,-2}, {80,15,22, 2}, 0,
    {48,49,80,49, 9, 0,0,0,0, 1}, false, false},
  // 5 SURPRISED
  {"SURPRISED",
    {41,28,13,16, 0,0}, {89,28,13,16, 0,0},
    {26,12,24,-1}, {80,12,24, 1}, 0,
    { 0, 0, 0, 0, 0,64,51, 8, 7, 3}, false, false},
  // 6 SLEEPY
  {"SLEEPY",
    {41,33,11, 8, 0,0}, {89,33,11, 8, 0,0},
    {28,20,22, 1}, {80,20,22,-1}, 6,
    {52,51,76,51, 0, 0,0,0,0, 0}, false, false},
  // 7 EXCITED
  {"EXCITED",
    {41,29, 0, 0, 0,2}, {89,29, 0, 0, 0,2},
    {26,13,24,-3}, {80,13,24, 3}, 0,
    {46,49,82,49,10, 0,0,0,0, 1}, false, false},
  // 8 SCARED
  {"SCARED",
    {38,30,13,15, 2,0}, {90,30,13,15,-2,0},
    {25,13,24, 4}, {81,13,24,-4}, 0,
    {50,54,78,54, 5, 0,0,0,0, 2}, false, false},
  // 9 CONFUSED
  {"CONFUSED",
    {41,31,10,13, 0,0}, {89,30,11, 8, 0,4},
    {27,17,22,-2}, {80,15,22, 4}, 0,
    {50,51,78,51, 0, 0,0,0,0, 4}, false, false},
  // 10 SMUG
  {"SMUG",
    {41,31,10,12, 1,0}, {89,31,10,12, 1,0},
    {27,18,22, 1}, {80,16,22,-2}, 3,
    {54,50,78,50, 0, 0,0,0,0, 5}, false, false},
  // 11 DEAD
  {"DEAD",
    {41,30, 0, 0, 0,3}, {89,30, 0, 0, 0,3},
    {28,18,22, 0}, {80,18,22, 0}, 0,
    {50,52,78,52, 0, 0,0,0,0, 4}, false, false},
  // 12 LAUGHING
  {"LAUGHING",
    {41,32,11, 6, 0,0}, {89,32,11, 6, 0,0},
    {27,16,22,-2}, {81,16,22, 2}, 6,
    {46,49,82,49,10, 0,0,0,0, 6}, false, false},
  // 13 CRYING
  {"CRYING",
    {41,34,11,11, 0,0}, {89,34,11,11, 0,0},
    {26,17,24, 4}, {80,17,24,-4}, 2,
    {47,56,81,56, 8, 0,0,0,0, 2}, true, true},
  // 14 WINK
  {"WINK",
    {41,30,11,13, 0,0}, {89,30, 0, 0, 0,5},
    {28,16,22, 0}, {80,16,22,-1}, 0,
    {54,49,78,49, 0, 0,0,0,0, 5}, false, false},
  // 15 ROLLING EYES
  {"ROLLING",
    {41,31,11,13,-3,0}, {89,31,11,13,-3,0},
    {28,17,22, 1}, {80,17,22,-1}, 4,
    {52,52,76,52, 0, 0,0,0,0, 0}, false, false},
  // 16 PAIN
  {"PAIN",
    {41,31,10,13, 0,0}, {89,31,10,13, 0,0},
    {27,17,22, 5}, {81,17,22,-5}, 1,
    {50,52,78,52, 0, 0,0,0,0, 8}, false, false},
  // 17 GREEDY
  {"GREEDY",
    {41,30, 0, 0, 0,6}, {89,30, 0, 0, 0,6},
    {27,16,22,-1}, {81,16,22, 1}, 0,
    {48,49,80,49, 7, 0,0,0,0, 1}, false, false},
  // 18 DIZZY
  {"DIZZY",
    {41,30, 0, 0, 0,7}, {89,30, 0, 0, 0,7},
    {27,18,22, 1}, {81,18,22,-1}, 0,
    {50,51,78,51, 0, 0,0,0,0, 4}, false, false},
  // 19 RAGE
  {"RAGE",
    {41,31,10,12, 0,0}, {89,31,10,12, 0,0},
    {27,16,23, 6}, {80,16,23,-6}, 4,
    { 0, 0, 0, 0, 0,64,52, 9, 5, 7}, false, false},
  // 20 MUSICAL
  {"MUSICAL",
    {38,30, 0, 0, 0,7}, {88,30, 0, 0, 0,7},
    {28,15,22,-1}, {80,15,22, 1}, 0,
    {50,49,78,49, 7, 0,0,0,0, 1}, false, false},
  // 21 ELECTRIC
  {"ELECTRIC",
    {41,30, 0, 0, 0,8}, {89,30, 0, 0, 0,8},
    {27,15,22,-2}, {81,15,22, 2}, 0,
    {48,49,80,49, 6, 0,0,0,0, 1}, false, false},
  // 22 BORED
  {"BORED",
    {41,33,11, 7, 0,0}, {89,33,11, 7, 0,0},
    {29,21,20, 1}, {81,21,20,-1}, 7,
    { 0, 0, 0, 0, 0,64,53, 5, 5, 3}, false, false},
  // 23 PROUD
  {"PROUD",
    {41,30,11,13, 0,0}, {89,30,11,13, 0,0},
    {27,14,24,-3}, {79,14,24, 3}, 2,
    {50,50,78,50, 5, 0,0,0,0, 1}, false, false},
};
#define EMO_COUNT (sizeof(EMOTIONS) / sizeof(EMOTIONS[0]))

uint8_t  currentEmo   = 0;
uint8_t  targetEmo    = 1;
int8_t   morphStep    = 0;
bool     morphing     = false;


enum AppState { STATE_FACE, STATE_MENU, STATE_GAMES, STATE_SENSOR, STATE_REMINDER, STATE_DRAW, STATE_MUSIC, STATE_CHAT };
AppState currentState = STATE_FACE;
bool req_lovestory = false;
uint8_t req_song = 0;
int menuCursor = 0;
int musicCursor = 0;
int gameCursor = 0;
char globalReminderText[64] = "Belum ada reminder";
uint8_t drawFrame[1024];

uint8_t audioRing[AUDIO_RING_SIZE];
uint16_t ringHead = 0;
uint16_t ringTail = 0;
uint16_t ringCount = 0;
uint8_t readBuffer[READ_CHUNK];
int16_t stereoSamples[AUDIO_FRAMES * 2];

enum CueMood : uint8_t {
  CUE_IDLE,
  CUE_DREAMY,
  CUE_SHY,
  CUE_WORRIED,
  CUE_PLEADING,
  CUE_RUN,
  CUE_PRINCE,
  CUE_HAPPY,
  CUE_LONELY,
  CUE_RING,
  CUE_FINALE
};

struct LyricCue {
  uint16_t start10;
  uint16_t end10;
  CueMood mood;
};

// Approximate Love Story phrase map, stored as timing cues instead of lyrics.
const LyricCue LOVE_STORY_CUES[] = {
  {  0, 120, CUE_DREAMY}, {120, 220, CUE_DREAMY}, {220, 320, CUE_SHY},
  {320, 430, CUE_HAPPY},  {430, 560, CUE_WORRIED}, {560, 690, CUE_PLEADING},
  {690, 820, CUE_RUN},    {820, 960, CUE_PRINCE},  {960,1080, CUE_HAPPY},
  {1080,1210, CUE_SHY},   {1210,1350, CUE_WORRIED}, {1350,1500, CUE_PLEADING},
  {1500,1640, CUE_RUN},   {1640,1800, CUE_PRINCE}, {1800,1940, CUE_HAPPY},
  {1940,2080, CUE_WORRIED}, {2080,2220, CUE_RUN},  {2220,2370, CUE_PRINCE},
  {2370,2510, CUE_HAPPY}, {2510,2660, CUE_DREAMY}, {2660,2810, CUE_LONELY},
  {2810,2960, CUE_WORRIED}, {2960,3090, CUE_PLEADING}, {3090,3210, CUE_RING},
  {3210,3370, CUE_HAPPY}, {3370,3530, CUE_PRINCE}, {3530,3670, CUE_FINALE},
  {3670,3820, CUE_FINALE}, {3820,3960, CUE_DREAMY}
};
const uint8_t LOVE_STORY_CUE_COUNT = sizeof(LOVE_STORY_CUES) / sizeof(LOVE_STORY_CUES[0]);

CueMood cueMoodAt(float sec) {
  uint16_t t = (uint16_t)(sec * 10.0f);
  for (uint8_t i = 0; i < LOVE_STORY_CUE_COUNT; i++) {
    if (t >= LOVE_STORY_CUES[i].start10 && t < LOVE_STORY_CUES[i].end10) {
      return LOVE_STORY_CUES[i].mood;
    }
  }
  return CUE_IDLE;
}

float clampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

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

int16_t be16(uint8_t hi, uint8_t lo) {
  return (int16_t)(((uint16_t)hi << 8) | lo);
}

bool readRawMotion(uint8_t addr, int16_t& ax, int16_t& ay, int16_t& az,
                   int16_t& gx, int16_t& gy, int16_t& gz) {
  Wire.beginTransmission(addr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, 14) != 14) return false;
  uint8_t b[14];
  for (uint8_t i = 0; i < 14; i++) b[i] = Wire.read();
  ax = be16(b[0], b[1]);
  ay = be16(b[2], b[3]);
  az = be16(b[4], b[5]);
  gx = be16(b[8], b[9]);
  gy = be16(b[10], b[11]);
  gz = be16(b[12], b[13]);
  return true;
}

bool setupRawMPU() {
  for (uint8_t addr : { (uint8_t)0x68, (uint8_t)0x69 }) {
    bool ok = false;
    uint8_t who = readReg8(addr, 0x75, ok);
    if (ok && (who == 0x68 || who == 0x70 || who == 0x71)) {
      mpuAddr = addr;
      mpuWho = who;
      rawMpuMode = true;
      writeReg8(addr, 0x6B, 0x00);
      delay(50);
      writeReg8(addr, 0x1A, 0x03);
      writeReg8(addr, 0x1B, 0x08);
      writeReg8(addr, 0x1C, 0x08);
      return true;
    }
  }
  return false;
}

void setupMPU() {
  rawMpuMode = false;
  if (mpu.begin(0x68, &Wire)) {
    mpuReady = true;
    mpuAddr = 0x68;
    mpuWho = 0x68;
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 ready at 0x68");
    return;
  }

  mpuReady = setupRawMPU();
  if (mpuReady) {
    Serial.print("Raw MPU ready addr=0x");
    Serial.print(mpuAddr, HEX);
    Serial.print(" who=0x");
    Serial.println(mpuWho, HEX);
  } else {
    Serial.println("MPU not found at 0x68/0x69");
  }
}

void setupDHT() {
  dht.begin();
  dhtReady = true;
  Serial.println("DHT22 ready on D2");
}

// Forward declarations needed for updateTouch()
void initMonsterBattle();
void playerAttack();
void enterDrawMode(bool clearCanvas);

void updateTouch() {
  unsigned long now = millis();
  bool reading = digitalRead(TOUCH_PIN) == HIGH;
  if (reading != lastTouchReading) {
    lastTouchReading = reading;
    touchChangedMs = now;
  }
  if (now - touchChangedMs < 100UL) return;
  if (reading == touchDown) return;

  touchDown = reading;
  if (touchDown) {
    touchDownMs = now;
    // Instantly go to menu on touch
    if (currentState == STATE_FACE) {
      currentState = STATE_MENU;
      menuCursor = 0;
    }
    return;
  }

  unsigned long held = now - touchDownMs;

  if (currentState == STATE_FACE) {
    // Already handled in touchDown above
  } else if (currentState == STATE_MENU) {
    if (held > 600UL) {
      if (menuCursor == 0) currentState = STATE_GAMES;
      else if (menuCursor == 1) currentState = STATE_SENSOR;
      else if (menuCursor == 2) currentState = STATE_REMINDER;
      else if (menuCursor == 3) {
        currentState = STATE_MUSIC;
        musicCursor = 0;
      }
      else if (menuCursor == 4) enterDrawMode(true);
      else if (menuCursor == 5) currentState = STATE_FACE;
      if (currentState == STATE_GAMES) gamePhase = GAME_IDLE;
    } else {
      menuCursor = (menuCursor + 1) % 6;
    }
  } else if (currentState == STATE_MUSIC) {
    if (held > 600UL) {
      if (musicCursor == 0) {
        currentState = STATE_FACE;
        req_lovestory = true;
        req_song = 1;
      } else if (musicCursor == 1) {
        currentState = STATE_FACE;
        req_song = 2;
      } else if (musicCursor == 2) {
        currentState = STATE_FACE;
        req_song = 3;
      } else {
        currentState = STATE_MENU;
        menuCursor = 3;
      }
    } else {
      musicCursor = (musicCursor + 1) % 4;
    }
  } else if (currentState == STATE_GAMES) {
    if (held > 600UL) {
      // Long hold = back to FACE as requested
      currentState = STATE_FACE;
      gamePhase = GAME_IDLE;
    } else {
      // Short tap = start game or hit
      if (gamePhase == GAME_IDLE || gamePhase == GAME_OVER) {
        initPingpong(true);
      } else if (gamePhase == GAME_PLAYING) {
        // Option: short tap to give the ball a random spin if it's on player's side
        if (ballX < 64.0f) {
           ballVY += (float)random(-10, 11) * 0.1f;
        }
      }
    }
  } else if (currentState == STATE_SENSOR || currentState == STATE_REMINDER || currentState == STATE_DRAW || currentState == STATE_CHAT) {
    if (held > 600UL) {
      currentState = STATE_FACE;
    } else {
      currentState = STATE_MENU;
    }
  }
}

void updateMPU() {
  if (!mpuReady) return;
  unsigned long now = millis();
  if (now - lastMpuMs < 20UL) return;
  lastMpuMs = now;

  float ax;
  float ay;
  float az;
  float gx;
  float gy;
  float gz;

  static bool mpuWasError = false;

  if (rawMpuMode) {
    int16_t rax, ray, raz, rgx, rgy, rgz;
    if (!readRawMotion(mpuAddr, rax, ray, raz, rgx, rgy, rgz)) {
      mpuWasError = true;
      setupRawMPU();
      return;
    }
    ax = ((float)rax / 8192.0f) * 9.80665f;
    ay = ((float)ray / 8192.0f) * 9.80665f;
    az = ((float)raz / 8192.0f) * 9.80665f;
    gx = ((float)rgx / 65.5f) * 0.0174533f;
    gy = ((float)rgy / 65.5f) * 0.0174533f;
    gz = ((float)rgz / 65.5f) * 0.0174533f;
  } else {
    sensors_event_t a;
    sensors_event_t g;
    sensors_event_t temp;
    mpu.getEvent(&a, &g, &temp);
    ax = a.acceleration.x;
    ay = a.acceleration.y;
    az = a.acceleration.z;
    gx = g.gyro.x;
    gy = g.gyro.y;
    gz = g.gyro.z;
  }

  rawAx = ax; rawAy = ay; rawAz = az;
  rawGx = gx; rawGy = gy; rawGz = gz;

  // Sanity check: if I2C glitched, ax/ay/az will be 0 or garbage
  // A real sensor at rest has gravity ~9.8 m/s². Skip if total accel is impossible.
  float totalAccel = fabsf(ax) + fabsf(ay) + fabsf(az);
  if (totalAccel < 1.0f || totalAccel > 60.0f) {
      lastAx = ax; lastAy = ay; lastAz = az;
      return; // bad read, skip
  }

  if (mpuWasError) {
      lastAx = ax; lastAy = ay; lastAz = az;
      mpuWasError = false;
      return;
  }

  float targetTiltX = clampFloat(ax / 9.8f, -1.0f, 1.0f);
  float targetTiltY = clampFloat(ay / 9.8f, -1.0f, 1.0f);

  float jerk = fabsf(ax - lastAx) + fabsf(ay - lastAy) + fabsf(az - lastAz);

  // Large deadzone — ignore all small movements and I2C noise
  // Only real physical shaking (>3.0 delta) counts
  if (jerk < 3.0f) jerk = 0.0f;

  // Only use jerk for shake — gyro causes false positives from I2C noise
  float targetShake = clampFloat(jerk * 0.10f, 0.0f, 1.0f);
  float gyroMag = fabsf(gx) + fabsf(gy) + fabsf(gz);
  float targetSpin = gyroMag > 1.65f ? clampFloat((gyroMag - 1.65f) * 0.22f, 0.0f, 1.0f) : 0.0f;

  tiltX += (targetTiltX - tiltX) * 0.18f;
  tiltY += (targetTiltY - tiltY) * 0.18f;
  shakeSmooth = shakeSmooth * 0.80f + targetShake * 0.20f;
  spinSmooth = spinSmooth * 0.82f + targetSpin * 0.18f;

  if (targetShake > 0.0f) {
    shakeMeter += targetShake * 0.05f;
  } else {
    shakeMeter -= 0.04f;  // Fast decay — returns to calm quickly
  }
  shakeMeter = clampFloat(shakeMeter, 0.0f, 1.0f);

  if (targetSpin > 0.05f) {
    spinMeter += targetSpin * 0.045f;
  } else {
    spinMeter -= 0.035f;
  }
  spinMeter = clampFloat(spinMeter, 0.0f, 1.0f);

  // Continuous rotation should feel like dizziness, not anger.
  if (spinMeter > 0.72f) {
    dizzyUntilMs = now + 2600UL;
  } else if (spinMeter > 0.42f) {
    dizzyUntilMs = now + 1400UL;
  }

  // Very high thresholds — only extreme continuous shaking triggers angry
  if (shakeMeter > 0.95f) {
    dizzyUntilMs = now + 3000UL;
    angryUntilMs = now + 3000UL;
    if (shakeActiveStartMs == 0) shakeActiveStartMs = now;
    // Super angry after 2+ seconds of heavy shaking
    if (now - shakeActiveStartMs > 2000UL) {
      superAngryUntilMs = now + 4000UL;
      cryUntilMs = now + 6000UL; // Crying after very heavy shaking
    }
  } else if (shakeMeter > 0.80f) {
    angryUntilMs = now + 2000UL;
    if (shakeActiveStartMs == 0) shakeActiveStartMs = now;
  } else if (shakeMeter > 0.60f) {
    dizzyUntilMs = now + 1500UL;
    shakeActiveStartMs = 0; // reset sustained tracker if calmer
  } else if (shakeMeter > 0.40f) {
    annoyedUntilMs = now + 600UL;
    shakeActiveStartMs = 0;
  } else {
    shakeActiveStartMs = 0; // fully calm, reset
  }
  
  if (faceDownMode) {
    if (now - sustainedTiltStartMs > 1500UL) glitchUntilMs = now + 1500UL; // Glitch when completely face down
    if (now - sustainedTiltStartMs > 3000UL) sadUntilMs = now + 2000UL;
  }

  angryMode = now < angryUntilMs;
  superAngryMode = now < superAngryUntilMs;

  if (now - lastHistMs > 100UL) {
    pitchHistory[histIdx] = ay;
    yawHistory[histIdx] = gz;
    histIdx = (histIdx + 1) % 4;
    lastHistMs = now;
  }
  float pMax = pitchHistory[0], pMin = pitchHistory[0];
  float yMax = yawHistory[0], yMin = yawHistory[0];
  for (int i = 1; i < 4; i++) {
    if (pitchHistory[i] > pMax) pMax = pitchHistory[i];
    if (pitchHistory[i] < pMin) pMin = pitchHistory[i];
    if (yawHistory[i] > yMax) yMax = yawHistory[i];
    if (yawHistory[i] < yMin) yMin = yawHistory[i];
  }
  if (pMax - pMin > 5.0f && !angryMode) {
    nodDetected = true;
    nodUntilMs = now + 1200UL;
  }
  if (yMax - yMin > 4.0f && !angryMode) {
    headShakeDetected = true;
    headShakeUntilMs = now + 1000UL;
  }
  if (jerk > 8.0f) {
    surprisedMode = true;
    surprisedUntilMs = now + 900UL;
  }
  if (fabsf(targetTiltX) > 0.45f) {
    if (sustainedTiltStartMs == 0) sustainedTiltStartMs = now;
    else if (now - sustainedTiltStartMs > 700UL) {
      curiousMode = true;
      curiousUntilMs = now + 500UL;
    }
  } else {
    sustainedTiltStartMs = 0;
  }
  faceDownMode = (az > 14.0f);
  
  float gForce = sqrt(ax*ax + ay*ay + az*az);
  if (gForce < 2.5f) {
      freefallMode = true;
      freefallUntilMs = now + 1000UL;
  }

  lastAx = ax;
  lastAy = ay;
  lastAz = az;
}

void sendTelemetry() {
  unsigned long now = millis();
  if (now - lastTelemetryMs < 200UL) return;
  lastTelemetryMs = now;
  if (WiFi.status() != WL_CONNECTED) return;

  // Determine current expression name and speech text
  const char* exprName = "NORMAL";
  const char* speech = "";
  if (now < freefallUntilMs) { exprName = "FREEFALL"; speech = "AAAA!"; }
  else if (now < glitchUntilMs) { exprName = "GLITCH"; speech = "ERR.."; }
  else if (now < cryUntilMs) { exprName = "MENANGIS"; speech = "HUHU.."; }
  else if (superAngryMode) { exprName = "SUPER MARAH"; speech = "GRRR!!"; }
  else if (angryMode) { exprName = "MARAH"; speech = "HEH!"; }
  else if (now < laughUntilMs) { exprName = "KETAWA"; speech = "HAHA!"; }
  else if (now < dizzyUntilMs) { exprName = "PUSING"; speech = "PUSING~"; }
  else if (now < pantUntilMs) { exprName = "KEPANASAN"; speech = "PANAS!"; }
  else if (now < sadUntilMs) { exprName = "SEDIH"; speech = "HMM.."; }
  else if (now < surprisedUntilMs) { exprName = "KAGET"; speech = "WAH!"; }
  else if (now < nodUntilMs) { exprName = "ANGGUK"; speech = "IYA!"; }
  else if (now < headShakeUntilMs) { exprName = "GELENG"; speech = "NGGAK!"; }
  else if (now < touchLoveUntilMs) { exprName = "SAYANG"; speech = "<3"; }
  else if (now < touchHappyUntilMs) { exprName = "SENANG"; speech = "HEH~"; }
  else if (now < annoyedUntilMs) { exprName = "KESAL"; speech = "ISH.."; }
  else if (now < touchSleepyUntilMs) { exprName = "NGANTUK"; speech = "zzz"; }
  else if (now < micShoutUntilMs) { exprName = "TERIAKAN"; speech = "HEI!"; }
  else if (playing) { exprName = "MUSIK"; speech = "~LA~"; }
  else if (faceDownMode) { exprName = "SEDIH"; speech = "HMM.."; }

  char telemetryJson[800];
  snprintf(telemetryJson, sizeof(telemetryJson),
    "{"
    "\"mpu\":%d,\"dht\":%d,\"ax\":%.2f,\"ay\":%.2f,\"az\":%.2f,"
    "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"tiltX\":%.2f,\"tiltY\":%.2f,"
    "\"shake\":%.2f,\"shakeMeter\":%.2f,\"angry\":%d,\"temp\":%.1f,\"hum\":%.0f,"
    "\"nod\":%d,\"headShake\":%d,\"surprised\":%d,\"curious\":%d,\"faceUp\":%d,\"faceDown\":%d,\"touch\":%d,"
    "\"game\":%d,\"scoreP\":%d,\"scoreA\":%d,"
    "\"laugh\":%d,\"glitch\":%d,\"pant\":%d,\"cry\":%d,\"sleep\":%d,"
    "\"dizzy\":%d,\"sad\":%d,\"annoyed\":%d,\"love\":%d,"
    "\"inmp\":%d,\"inmpPeak\":%d,\"micActive\":%d,\"max\":%d,\"req_lovestory\":%d,\"req_song\":%d,"
    "\"state\":%d,"
    "\"expr\":\"%s\""
    "}",
    (mpuReady || rawMpuMode) ? 1 : 0, dhtReady ? 1 : 0,
    rawAx, rawAy, rawAz, rawGx, rawGy, rawGz,
    tiltX, tiltY, shakeSmooth, shakeMeter, angryMode ? 1 : 0,
    isnan(tempC) ? -99.0f : tempC,
    isnan(humPct) ? -1.0f : humPct,
    (now < nodUntilMs) ? 1 : 0,
    (now < headShakeUntilMs) ? 1 : 0,
    (now < surprisedUntilMs) ? 1 : 0,
    (now < curiousUntilMs) ? 1 : 0,
    0,
    faceDownMode ? 1 : 0,
    touchDown ? 1 : 0,
    (currentState == STATE_GAMES ? (int)gamePhase : 0), scorePlayer, scoreAi,
    (now < laughUntilMs) ? 1 : 0,
    (now < glitchUntilMs) ? 1 : 0,
    (now < pantUntilMs) ? 1 : 0,
    (now < cryUntilMs) ? 1 : 0,
    (now < touchSleepyUntilMs) ? 1 : 0,
    (now < dizzyUntilMs) ? 1 : 0,
    (now < sadUntilMs) ? 1 : 0,
    (now < annoyedUntilMs) ? 1 : 0,
    (now < touchLoveUntilMs) ? 1 : 0,
    (int)(micSmooth * 100), (int)(micPeak * 100), (now < micActiveUntilMs) ? 1 : 0,
    playing ? 1 : 0, req_lovestory ? 1 : 0, (int)req_song,
    (int)currentState,
    exprName
  );

  if (req_lovestory) req_lovestory = false;
  req_song = 0;

  telemetryUdp.beginPacket(IPAddress(255, 255, 255, 255), TELEMETRY_PORT);
  telemetryUdp.write((const uint8_t*)telemetryJson, strlen(telemetryJson));
  telemetryUdp.endPacket();
}

void updateDHT() {
  if (!dhtReady) return;
  unsigned long now = millis();
  if (now - lastDhtMs < 2200UL) return;
  lastDhtMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (isnan(h) || isnan(t)) {
    Serial.println("DHT read failed");
    return;
  }

  tempC = t;
  humPct = h;

  float hot = clampFloat((t - 28.0f) / 7.0f, 0.0f, 1.0f);
  float cold = clampFloat((22.0f - t) / 7.0f, 0.0f, 1.0f);
  float targetTempMood = hot - cold;
  float targetHumidMood = clampFloat((h - 68.0f) / 25.0f, 0.0f, 1.0f);

  tempMood += (targetTempMood - tempMood) * 0.35f;
  humidMood += (targetHumidMood - humidMood) * 0.35f;
  
  if (t > 31.0f) {
      pantUntilMs = now + 5000UL;
  }

  Serial.print("DHT T=");
  Serial.print(t, 1);
  Serial.print("C H=");
  Serial.print(h, 0);
  Serial.println("%");
}

void drawStatus(const char* title, const char* line1, const char* line2 = "") {
  if (!oledReady) return;
  display.clearDisplay();
  display.setTextWrap(false);
  display.fillRect(0, 0, SCREEN_WIDTH, 12, OLED_WHITE);
  display.setTextColor(OLED_BLACK);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print(title);
  display.setTextColor(OLED_WHITE);
  display.setCursor(3, 22);
  display.print(line1);
  display.setCursor(3, 38);
  display.print(line2);
  display.display();
}

// Legacy drawing helpers removed.

// ─── UTIL: integer lerp ────────────────────────────────────
int16_t lerpI(int16_t a, int16_t b, uint8_t step, uint8_t total) {
  return a + ((int32_t)(b - a) * step) / total;
}

// ─── CUSTOM ELLIPSE DRAWING ────────────────────────────────
void drawEllipseLocal(int16_t x0, int16_t y0, int16_t rx, int16_t ry, uint16_t color) {
  int a2 = rx * rx;
  int b2 = ry * ry;
  int fa2 = 4 * a2, fb2 = 4 * b2;
  int x, y, sigma;

  for (x = 0, y = ry, sigma = 2*b2+a2*(1-2*ry); b2*x <= a2*y; x++) {
    display.drawPixel(x0 + x, y0 + y, color);
    display.drawPixel(x0 - x, y0 + y, color);
    display.drawPixel(x0 + x, y0 - y, color);
    display.drawPixel(x0 - x, y0 - y, color);
    if (sigma >= 0) {
      sigma += fa2 * (1 - y);
      y--;
    }
    sigma += b2 * ((4 * x) + 6);
  }
  for (x = rx, y = 0, sigma = 2*a2+b2*(1-2*rx); a2*y <= b2*x; y++) {
    display.drawPixel(x0 + x, y0 + y, color);
    display.drawPixel(x0 - x, y0 + y, color);
    display.drawPixel(x0 + x, y0 - y, color);
    display.drawPixel(x0 - x, y0 - y, color);
    if (sigma >= 0) {
      sigma += fb2 * (1 - x);
      x--;
    }
    sigma += a2 * ((4 * y) + 6);
  }
}

void fillEllipseLocal(int16_t x0, int16_t y0, int16_t rx, int16_t ry, uint16_t color) {
  int a2 = rx * rx;
  int b2 = ry * ry;
  int fa2 = 4 * a2, fb2 = 4 * b2;
  int x, y, sigma;

  for (x = 0, y = ry, sigma = 2*b2+a2*(1-2*ry); b2*x <= a2*y; x++) {
    display.drawLine(x0 - x, y0 + y, x0 + x, y0 + y, color);
    display.drawLine(x0 - x, y0 - y, x0 + x, y0 - y, color);
    if (sigma >= 0) {
      sigma += fa2 * (1 - y);
      y--;
    }
    sigma += b2 * ((4 * x) + 6);
  }
  for (x = rx, y = 0, sigma = 2*a2+b2*(1-2*rx); a2*y <= b2*x; y++) {
    display.drawLine(x0 - x, y0 + y, x0 + x, y0 + y, color);
    display.drawLine(x0 - x, y0 - y, x0 + x, y0 - y, color);
    if (sigma >= 0) {
      sigma += fb2 * (1 - x);
      x--;
    }
    sigma += a2 * ((4 * y) + 6);
  }
}

// ─── PRIMITIVE DRAWING FUNCTIONS ───────────────────────────
void drawBrow(int16_t x, int16_t y, int16_t w, int16_t angleX10, int dx, int dy) {
  float angle = angleX10 / 10.0f;
  int16_t cx = x + w / 2, cy = y;
  for (int16_t i = 0; i < w; i++) {
    float t = (float)i / (w - 1) - 0.5f;
    int16_t bx = cx + (int16_t)(t * w) + dx;
    int16_t by = cy + (int16_t)(t * w * angle) + dy;
    display.drawPixel(bx, by,     OLED_WHITE);
    display.drawPixel(bx, by + 1, OLED_WHITE);
  }
}

void drawEyeNormal(int16_t cx, int16_t cy, int16_t rx, int16_t ry,
                   int16_t lookX, uint8_t lid, bool blink, int dx, int dy, int gLookX, int gLookY) {
  cx += dx; cy += dy;
  if (blink) {
    for (int16_t x = cx - rx; x <= cx + rx; x++) {
      display.drawPixel(x, cy,     OLED_WHITE);
      display.drawPixel(x, cy + 1, OLED_WHITE);
    }
    return;
  }
  for (int16_t y = cy - ry; y <= cy + ry; y++) {
    for (int16_t x = cx - rx; x <= cx + rx; x++) {
      float fdx = (float)(x - cx) / rx;
      float fdy = (float)(y - cy) / ry;
      if (fdx * fdx + fdy * fdy <= 1.0f)
        display.drawPixel(x, y, OLED_WHITE);
    }
  }
  int16_t ps = min(rx, ry) * 45 / 100;
  int16_t pcx = cx + lookX + gLookX, pcy = cy + gLookY;
  for (int16_t y = pcy - ps; y <= pcy + ps; y++) {
    for (int16_t x = pcx - ps; x <= pcx + ps; x++) {
      float fdx = (float)(x - pcx) / ps;
      float fdy = (float)(y - pcy) / ps;
      if (fdx * fdx + fdy * fdy <= 1.0f)
        display.drawPixel(x, y, OLED_BLACK);
    }
  }
  display.drawPixel(pcx - ps / 3, pcy - ps / 3, OLED_WHITE);
  if (lid > 0) {
    int16_t lidH = (ry * 2 * lid) / 10;
    int16_t yTop = cy - ry;
    for (int16_t y = yTop; y < yTop + lidH; y++)
      for (int16_t x = cx - rx - 1; x <= cx + rx + 1; x++)
        display.drawPixel(x, y, OLED_BLACK);
  }
}

void drawHeart(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  for (int16_t y = -sz; y <= sz; y++) {
    for (int16_t x = -sz; x <= sz; x++) {
      float nx = (float)x / sz, ny = (float)y / sz;
      float v = pow(nx*nx + ny*ny - 1.0f, 3) - nx*nx*ny*ny*ny;
      if (v <= 0.0f) display.drawPixel(cx + x, cy + y - sz/10, OLED_WHITE);
    }
  }
}

void drawStar(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  for (uint8_t i = 0; i < 10; i++) {
    float a0 = i * PI / 5.0f - PI / 2.0f;
    float a1 = (i + 1) * PI / 5.0f - PI / 2.0f;
    float r0 = (i % 2 == 0) ? sz : sz * 0.4f;
    float r1 = (i % 2 == 0) ? sz * 0.4f : sz;
    int16_t x0 = cx + (int16_t)(cos(a0) * r0);
    int16_t y0 = cy + (int16_t)(sin(a0) * r0);
    int16_t x1 = cx + (int16_t)(cos(a1) * r1);
    int16_t y1 = cy + (int16_t)(sin(a1) * r1);
    display.drawLine(x0, y0, x1, y1, OLED_WHITE);
  }
}

void drawXEyes(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  display.drawLine(cx - sz, cy - sz, cx + sz, cy + sz, OLED_WHITE);
  display.drawLine(cx - sz, cy + sz, cx + sz, cy - sz, OLED_WHITE);
  display.drawLine(cx - sz + 1, cy - sz, cx + sz + 1, cy + sz, OLED_WHITE);
  display.drawLine(cx - sz + 1, cy + sz, cx + sz + 1, cy - sz, OLED_WHITE);
}

void drawSquint(int16_t cx, int16_t cy, int16_t rx, int16_t ry, int dx, int dy) {
  cx += dx; cy += dy;
  for (int16_t y = cy - ry; y <= cy - ry / 2; y++) {
    for (int16_t x = cx - rx; x <= cx + rx; x++) {
      float fdx = (float)(x - cx) / rx;
      float fdy = (float)(y - cy) / ry;
      if (fdx*fdx + fdy*fdy <= 1.0f) display.drawPixel(x, y, OLED_WHITE);
    }
  }
  for (int16_t x = cx - rx; x <= cx + rx; x++) display.drawPixel(x, cy + ry / 3, OLED_WHITE);
}

void drawWink(int16_t cx, int16_t cy, int dx, int dy) {
  cx += dx; cy += dy;
  display.drawLine(cx - 11, cy + 1, cx + 11, cy + 1, OLED_WHITE);
  display.drawLine(cx - 11, cy + 2, cx + 11, cy + 2, OLED_WHITE);
}

void drawDollarSign(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  display.drawCircle(cx, cy, sz / 2, OLED_WHITE);
  display.drawLine(cx, cy - sz, cx, cy + sz, OLED_WHITE);
  display.drawLine(cx + 1, cy - sz, cx + 1, cy + sz, OLED_WHITE);
}

void drawSpiral(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  float prev_x = cx, prev_y = cy;
  for (float a = 0; a < TWO_PI * 3; a += 0.2f) {
    float r = sz * (a / (TWO_PI * 3));
    int16_t nx = cx + (int16_t)(cos(a) * r);
    int16_t ny = cy + (int16_t)(sin(a) * r);
    display.drawLine(prev_x, prev_y, nx, ny, OLED_WHITE);
    prev_x = nx; prev_y = ny;
  }
}

void drawLightning(int16_t cx, int16_t cy, int16_t sz, int dx, int dy) {
  cx += dx; cy += dy;
  display.drawLine(cx,        cy - sz,    cx + sz/2, cy,       OLED_WHITE);
  display.drawLine(cx + sz/2, cy,         cx,        cy + sz/4, OLED_WHITE);
  display.drawLine(cx,        cy + sz/4,  cx + sz/2, cy + sz,  OLED_WHITE);
}

void drawEyeDispatch(EyeParam ep, uint8_t lid, bool blink, int dx, int dy, int lookX, int lookY) {
  switch (ep.type) {
    case 0: drawEyeNormal(ep.cx, ep.cy, ep.rx, ep.ry, ep.lx, lid, blink, dx, dy, lookX, lookY); break;
    case 1: drawHeart(ep.cx, ep.cy, 10, dx, dy); break;
    case 2: drawStar(ep.cx, ep.cy, 11, dx, dy); break;
    case 3: drawXEyes(ep.cx, ep.cy, 10, dx, dy); break;
    case 4: drawSquint(ep.cx, ep.cy, ep.rx, ep.ry, dx, dy); break;
    case 5: drawWink(ep.cx, ep.cy, dx, dy); break;
    case 6: drawDollarSign(ep.cx, ep.cy, 10, dx, dy); break;
    case 7: drawSpiral(ep.cx, ep.cy, 9, dx, dy); break;
    case 8: drawLightning(ep.cx, ep.cy, 9, dx, dy); break;
  }
}

void drawMouth(MouthParam m, int dx, int dy) {
  int16_t x1 = m.x1 + dx, y1 = m.y1 + dy;
  int16_t x2 = m.x2 + dx, y2 = m.y2 + dy;
  int16_t mx = (x1 + x2) / 2;
  int16_t cx = m.cx + dx, cy = m.cy + dy;

  switch (m.type) {
    case 0: // line
      display.drawLine(x1, y1, x2, y1, OLED_WHITE);
      display.drawLine(x1, y1 + 1, x2, y1 + 1, OLED_WHITE);
      break;
    case 1: // smile
    case 2: { // frown
      int8_t dir = (m.type == 1) ? 1 : -1;
      int16_t prevX = x1, prevY = y1;
      for (uint8_t i = 1; i <= 20; i++) {
        float t = i / 20.0f;
        int16_t bx = (int16_t)((1-t)*(1-t)*x1 + 2*(1-t)*t*mx + t*t*x2);
        int16_t by = (int16_t)((1-t)*(1-t)*y1 + 2*(1-t)*t*(y1 + dir*m.dep) + t*t*y2);
        display.drawLine(prevX, prevY, bx, by, OLED_WHITE);
        prevX = bx; prevY = by;
      }
      break;
    }
    case 3: // oval
      drawEllipseLocal(cx, cy, m.rx, m.ry, OLED_WHITE);
      break;
    case 4: // wavy
      for (int16_t x = x1; x <= x2; x++) {
        float t = (float)(x - x1) / (x2 - x1);
        int16_t wy = y1 + (int16_t)(sin(t * PI * 3) * 2);
        display.drawPixel(x, wy, OLED_WHITE);
        display.drawPixel(x, wy + 1, OLED_WHITE);
      }
      break;
    case 5: { // smirk
      int16_t prevX = x1, prevY = y1 + 2;
      for (uint8_t i = 1; i <= 20; i++) {
        float t = i / 20.0f;
        int16_t bx = (int16_t)((1-t)*(1-t)*x1 + 2*(1-t)*t*(mx+8) + t*t*x2);
        int16_t by = (int16_t)((1-t)*(1-t)*(y1+2) + 2*(1-t)*t*(y1+5) + t*t*y2);
        display.drawLine(prevX, prevY, bx, by, OLED_WHITE);
        prevX = bx; prevY = by;
      }
      break;
    }
    case 6: { // teeth
      int16_t prevX = x1, prevY = y1;
      for (uint8_t i = 1; i <= 20; i++) {
        float t = i / 20.0f;
        int16_t bx = (int16_t)((1-t)*(1-t)*x1 + 2*(1-t)*t*mx + t*t*x2);
        int16_t by = (int16_t)((1-t)*(1-t)*y1 + 2*(1-t)*t*(y1+m.dep) + t*t*y2);
        display.drawLine(prevX, prevY, bx, by, OLED_WHITE);
        prevX = bx; prevY = by;
      }
      uint8_t tw = x2 - x1 - 4;
      for (uint8_t i = 0; i < 4; i++) {
        int16_t tx = x1 + 2 + i * (tw / 4);
        display.drawLine(tx, y1, tx, y1 + m.dep / 2, OLED_BLACK);
      }
      break;
    }
    case 7: // scream
      drawEllipseLocal(cx, cy, m.rx, m.ry, OLED_WHITE);
      fillEllipseLocal(cx, cy, m.rx - 1, m.ry - 1, OLED_BLACK);
      break;
    case 8: { // zigzag
      int16_t steps = 8;
      for (int16_t i = 0; i < steps; i++) {
        int16_t nx0 = x1 + (x2 - x1) * i / steps;
        int16_t nx1 = x1 + (x2 - x1) * (i + 1) / steps;
        int16_t ny0 = y1 + ((i % 2 == 0) ? 0 : 4);
        int16_t ny1 = y1 + ((i % 2 == 0) ? 4 : 0);
        display.drawLine(nx0, ny0, nx1, ny1, OLED_WHITE);
      }
      break;
    }
  }
}

void drawTear(int16_t x, int16_t y, int dx, int dy) {
  x += dx; y += dy;
  for (int16_t i = 0; i < 8; i++) {
    display.drawPixel(x, y + i, OLED_WHITE);
    if (i > 4) display.drawPixel(x - 1, y + i, OLED_WHITE);
  }
}

// ─── RENDER EMOTION FRAME ──────────────────────────────────
void renderEmotion(uint8_t emoIdx, uint8_t morphFrac, uint8_t fromIdx,
                   bool blink, uint8_t morphSteps, int dx, int dy, int lookX, int lookY) {
  EmoParam cur, tgt;
  memcpy_P(&cur, &EMOTIONS[fromIdx], sizeof(EmoParam));
  memcpy_P(&tgt, &EMOTIONS[emoIdx],  sizeof(EmoParam));

  uint8_t step = morphFrac;
  uint8_t total = morphSteps;

  drawBrow(
    lerpI(cur.browL.x, tgt.browL.x, step, total),
    lerpI(cur.browL.y, tgt.browL.y, step, total),
    lerpI(cur.browL.w, tgt.browL.w, step, total),
    lerpI(cur.browL.angle, tgt.browL.angle, step, total),
    dx, dy
  );
  drawBrow(
    lerpI(cur.browR.x, tgt.browR.x, step, total),
    lerpI(cur.browR.y, tgt.browR.y, step, total),
    lerpI(cur.browR.w, tgt.browR.w, step, total),
    lerpI(cur.browR.angle, tgt.browR.angle, step, total),
    dx, dy
  );

  uint8_t lidBase = lerpI(cur.lid, tgt.lid, step, total);
  uint8_t lid = blink ? 10 : lidBase;

  EyeParam eL, eR;
  if (cur.eyeL.type == tgt.eyeL.type && tgt.eyeL.type == 0) {
    eL = {
      (int8_t)lerpI(cur.eyeL.cx, tgt.eyeL.cx, step, total),
      (int8_t)lerpI(cur.eyeL.cy, tgt.eyeL.cy, step, total),
      (int8_t)lerpI(cur.eyeL.rx, tgt.eyeL.rx, step, total),
      (int8_t)lerpI(cur.eyeL.ry, tgt.eyeL.ry, step, total),
      (int8_t)lerpI(cur.eyeL.lx, tgt.eyeL.lx, step, total),
      0
    };
  } else {
    eL = (step < total / 2) ? cur.eyeL : tgt.eyeL;
  }
  if (cur.eyeR.type == tgt.eyeR.type && tgt.eyeR.type == 0) {
    eR = {
      (int8_t)lerpI(cur.eyeR.cx, tgt.eyeR.cx, step, total),
      (int8_t)lerpI(cur.eyeR.cy, tgt.eyeR.cy, step, total),
      (int8_t)lerpI(cur.eyeR.rx, tgt.eyeR.rx, step, total),
      (int8_t)lerpI(cur.eyeR.ry, tgt.eyeR.ry, step, total),
      (int8_t)lerpI(cur.eyeR.lx, tgt.eyeR.lx, step, total),
      0
    };
  } else {
    eR = (step < total / 2) ? cur.eyeR : tgt.eyeR;
  }

  drawEyeDispatch(eL, lid, blink, dx, dy, lookX, lookY);
  drawEyeDispatch(eR, lid, blink, dx, dy, lookX, lookY);

  MouthParam mouth;
  if (cur.mouth.type == tgt.mouth.type) {
    mouth = {
      (int8_t)lerpI(cur.mouth.x1, tgt.mouth.x1, step, total),
      (int8_t)lerpI(cur.mouth.y1, tgt.mouth.y1, step, total),
      (int8_t)lerpI(cur.mouth.x2, tgt.mouth.x2, step, total),
      (int8_t)lerpI(cur.mouth.y2, tgt.mouth.y2, step, total),
      (int8_t)lerpI(cur.mouth.dep, tgt.mouth.dep, step, total),
      (int8_t)lerpI(cur.mouth.cx, tgt.mouth.cx, step, total),
      (int8_t)lerpI(cur.mouth.cy, tgt.mouth.cy, step, total),
      (int8_t)lerpI(cur.mouth.rx, tgt.mouth.rx, step, total),
      (int8_t)lerpI(cur.mouth.ry, tgt.mouth.ry, step, total),
      cur.mouth.type
    };
  } else {
    mouth = (step < total / 2) ? cur.mouth : tgt.mouth;
  }
  drawMouth(mouth, dx, dy);

  if (tgt.tearL && step > total / 2) { drawTear(36, 33, dx, dy); drawTear(34, 35, dx, dy); }
  if (tgt.tearR && step > total / 2) { drawTear(84, 33, dx, dy); drawTear(86, 35, dx, dy); }
}


enum BitmapMpuFace : uint8_t {
  BMP_FACE_NORMAL,
  BMP_FACE_CONTENT,
  BMP_FACE_HAPPY,
  BMP_FACE_LOVE,
  BMP_FACE_SHY,
  BMP_FACE_LAUGH,
  BMP_FACE_PLEADING,
  BMP_FACE_PROUD,
  BMP_FACE_CURIOUS,
  BMP_FACE_ANNOYED,
  BMP_FACE_SURPRISED,
  BMP_FACE_DIZZY,
  BMP_FACE_SLEEPY,
  BMP_FACE_HOT,
  BMP_FACE_COLD,
  BMP_FACE_SING,
  BMP_FACE_ANGRY
};

struct BitmapLayerSpec {
  const unsigned char* bits;
  uint8_t srcW;
  uint8_t srcH;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  bool enabled;
};

struct BitmapFaceSpec {
  BitmapLayerSpec left;
  BitmapLayerSpec right;
  BitmapLayerSpec mouth;
  BitmapLayerSpec extra;
};

BitmapLayerSpec layerSpec(const unsigned char* bits, uint8_t srcW, uint8_t srcH,
                          int16_t x, int16_t y, int16_t w, int16_t h) {
  BitmapLayerSpec l;
  l.bits = bits;
  l.srcW = srcW;
  l.srcH = srcH;
  l.x = x;
  l.y = y;
  l.w = w;
  l.h = h;
  l.enabled = bits != nullptr;
  return l;
}

BitmapLayerSpec noLayerSpec() {
  return layerSpec(nullptr, 0, 0, 0, 0, 0, 0);
}

bool bitmapBitOn(const unsigned char* bits, int w, int x, int y) {
  int rowBytes = (w + 7) / 8;
  uint8_t b = pgm_read_byte(bits + y * rowBytes + x / 8);
  return (b & (0x80 >> (x & 7))) != 0;
}

void drawBitmapScaled(const unsigned char* bits, int srcW, int srcH,
                      int x, int y, int dstW, int dstH) {
  if (!bits || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
  for (int yy = 0; yy < dstH; yy++) {
    int py = y + yy;
    if (py < 0 || py >= SCREEN_HEIGHT) continue;
    int sy = (yy * srcH) / dstH;
    for (int xx = 0; xx < dstW; xx++) {
      int px = x + xx;
      if (px < 0 || px >= SCREEN_WIDTH) continue;
      int sx = (xx * srcW) / dstW;
      if (bitmapBitOn(bits, srcW, sx, sy)) display.drawPixel(px, py, OLED_WHITE);
    }
  }
}

float smoothStep01(float t) {
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return t * t * (3.0f - 2.0f * t);
}

int lerpBitmapInt(int a, int b, float t) {
  return (int)((float)a + ((float)b - (float)a) * t + (t >= 0.0f ? 0.5f : -0.5f));
}

BitmapFaceSpec faceSpecFor(BitmapMpuFace face) {
  BitmapFaceSpec f;
  f.left = noLayerSpec();
  f.right = noLayerSpec();
  f.mouth = noLayerSpec();
  f.extra = noLayerSpec();

  switch (face) {
    case BMP_FACE_CONTENT:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 29, 15, 24, 31);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 77, 15, 24, 31);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 56, 49, 18, 6);
      break;
    case BMP_FACE_HAPPY:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 27, 12, 27, 37);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 76, 12, 27, 37);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 55, 48, 20, 7);
      break;
    case BMP_FACE_LOVE:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 27, 11, 28, 39);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 75, 11, 28, 39);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 54, 48, 22, 7);
      break;
    case BMP_FACE_SHY:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 30, 16, 24, 33);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 76, 16, 24, 33);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 58, 50, 13, 5);
      break;
    case BMP_FACE_LAUGH:
      f.left = layerSpec(lpk_sleepy_layer_6, 26, 9, 28, 33, 28, 10);
      f.right = layerSpec(lpk_sleepy_layer_6, 26, 9, 74, 33, 28, 10);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 52, 48, 27, 8);
      break;
    case BMP_FACE_PLEADING:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 26, 9, 30, 42);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 74, 9, 30, 42);
      f.mouth = layerSpec(lpk_sleepy_layer_9, 16, 5, 58, 51, 13, 4);
      break;
    case BMP_FACE_PROUD:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 27, 15, 27, 32);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 76, 15, 27, 32);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 55, 48, 21, 6);
      break;
    case BMP_FACE_CURIOUS:
      f.left = layerSpec(lpk_question_layer_9_1, 26, 32, 28, 18, 26, 32);
      f.right = layerSpec(lpk_question_layer_9_copy_1, 26, 39, 76, 11, 26, 39);
      f.mouth = layerSpec(lpk_question_layer_7, 16, 6, 57, 48, 16, 6);
      break;
    case BMP_FACE_ANNOYED:
      f.left = layerSpec(lpk_sleepy_layer_6, 26, 9, 28, 34, 27, 11);
      f.right = layerSpec(lpk_sleepy_layer_6, 26, 9, 75, 34, 27, 11);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 57, 49, 16, 5);
      break;
    case BMP_FACE_SURPRISED:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 25, 9, 30, 42);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 73, 9, 30, 42);
      f.mouth = layerSpec(img_sing_mouth, 20, 12, 58, 47, 12, 8);
      break;
    case BMP_FACE_DIZZY:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 30, 14, 25, 36);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 74, 14, 25, 36);
      f.mouth = layerSpec(lpk_question_layer_7, 16, 6, 57, 49, 16, 6);
      break;
    case BMP_FACE_SLEEPY:
      f.left = layerSpec(lpk_sleepy_layer_6, 26, 9, 28, 41, 26, 9);
      f.right = layerSpec(lpk_sleepy_layer_6, 26, 9, 77, 41, 26, 9);
      f.mouth = layerSpec(lpk_sleepy_layer_9, 16, 5, 57, 49, 16, 5);
      f.extra = layerSpec(lpk_sleepy_layer_11, 10, 14, 105, 14, 10, 14);
      break;
    case BMP_FACE_HOT:
      f.left = layerSpec(lpk_sleepy_layer_6, 26, 9, 28, 35, 27, 12);
      f.right = layerSpec(lpk_sleepy_layer_6, 26, 9, 75, 35, 27, 12);
      f.mouth = layerSpec(img_sing_mouth, 20, 12, 56, 46, 16, 10);
      break;
    case BMP_FACE_COLD:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 31, 18, 21, 28);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 78, 18, 21, 28);
      f.mouth = layerSpec(lpk_sleepy_layer_9, 16, 5, 58, 51, 13, 4);
      break;
    case BMP_FACE_SING:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 28, 13, 26, 37);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 76, 13, 26, 37);
      f.mouth = layerSpec(img_sing_mouth, 20, 12, 56, 46, 16, 10);
      break;
    case BMP_FACE_ANGRY:
      f.left = layerSpec(lpk_scary_eyes, 61, 20, 33, 16, 61, 20);
      f.mouth = layerSpec(lpk_scary_mouth, 59, 15, 38, 44, 51, 11);
      break;
    case BMP_FACE_NORMAL:
    default:
      f.left = layerSpec(lpk_normal_layer_9, 26, 37, 28, 13, 26, 37);
      f.right = layerSpec(lpk_normal_layer_8, 26, 37, 76, 13, 26, 37);
      f.mouth = layerSpec(lpk_normal_layer_7, 16, 6, 57, 48, 16, 6);
      break;
  }
  return f;
}

void drawMorphLayer(const BitmapLayerSpec& from, const BitmapLayerSpec& to, float t,
                    int dx, int dy) {
  if (!from.enabled && !to.enabled) return;
  const BitmapLayerSpec& a = from.enabled ? from : to;
  const BitmapLayerSpec& b = to.enabled ? to : from;
  int ax = a.x + a.w / 2;
  int ay = a.y + a.h / 2;
  int bx = b.x + b.w / 2;
  int by = b.y + b.h / 2;
  if (from.enabled && t < 0.82f) {
    float outT = smoothStep01(clampFloat(t / 0.82f, 0.0f, 1.0f));
    int cx = lerpBitmapInt(ax, bx, t * 0.78f);
    int cy = lerpBitmapInt(ay, by, t * 0.78f);
    int w = lerpBitmapInt(from.w, to.enabled ? to.w : from.w, t);
    int h = lerpBitmapInt(from.h, to.enabled ? to.h : from.h, t);
    w = max(1, (int)(w * (1.0f - outT * 0.28f)));
    h = max(1, (int)(h * (1.0f - outT * 0.22f)));
    drawBitmapScaled(from.bits, from.srcW, from.srcH, cx - w / 2 + dx, cy - h / 2 + dy, w, h);
  }
  if (to.enabled && t > 0.18f) {
    float inT = smoothStep01(clampFloat((t - 0.18f) / 0.82f, 0.0f, 1.0f));
    int cx = lerpBitmapInt(ax, bx, 0.22f + inT * 0.78f);
    int cy = lerpBitmapInt(ay, by, 0.22f + inT * 0.78f);
    int w = lerpBitmapInt(from.enabled ? max(1, from.w / 2) : 1, to.w, inT);
    int h = lerpBitmapInt(from.enabled ? max(1, from.h / 2) : 1, to.h, inT);
    drawBitmapScaled(to.bits, to.srcW, to.srcH, cx - w / 2 + dx, cy - h / 2 + dy, w, h);
  }
}

void drawBitmapFaceMorph(BitmapMpuFace from, BitmapMpuFace to, float t, int dx, int dy) {
  BitmapFaceSpec a = faceSpecFor(from);
  BitmapFaceSpec b = faceSpecFor(to);
  drawMorphLayer(a.left, b.left, t, dx, dy);
  drawMorphLayer(a.right, b.right, t, dx, dy);
  drawMorphLayer(a.mouth, b.mouth, t, dx, dy);
  drawMorphLayer(a.extra, b.extra, t, dx, dy);
}

void drawBitmapLayerDynamic(const BitmapLayerSpec& l, int dx, int dy,
                            int ox, int oy, int addW, int addH) {
  if (!l.enabled) return;
  int w = max(1, (int)l.w + addW);
  int h = max(1, (int)l.h + addH);
  int x = l.x + dx + ox - addW / 2;
  int y = l.y + dy + oy - addH / 2;
  drawBitmapScaled(l.bits, l.srcW, l.srcH, x, y, w, h);
}

void drawMiniHeart(int x, int y) {
  display.fillCircle(x - 1, y, 1, OLED_WHITE);
  display.fillCircle(x + 1, y, 1, OLED_WHITE);
  display.drawPixel(x, y + 2, OLED_WHITE);
}

void drawSoftSparkle(int x, int y, int r) {
  display.drawPixel(x, y, OLED_WHITE);
  display.drawFastHLine(x - r, y, r * 2 + 1, OLED_WHITE);
  display.drawFastVLine(x, y - r, r * 2 + 1, OLED_WHITE);
}

void drawBitmapFaceStable(BitmapMpuFace face, unsigned long now, bool active,
                          int dx, int dy, int lookX, int lookY,
                          float blinkL, float blinkR,
                          float motionPower, float voicePower,
                          float reactionPulse, uint8_t actionKind,
                          float actionPulse) {
  BitmapFaceSpec f = faceSpecFor(face);
  float t = now * 0.001f;
  float breathe = sinf(t * 1.55f);
  float soft = sinf(t * 2.10f + 0.7f);
  float quick = sinf(t * 5.80f);
  float song = active ? sinf(t * 8.00f) : 0.0f;
  int baseBob = (int)(breathe * 1.2f + song * 0.9f);
  int baseSway = (int)(sinf(t * 1.05f + 1.6f) * 1.0f);
  int eyeAddW = 0;
  int eyeAddH = 0;
  int mouthAddW = 0;
  int mouthAddH = 0;
  int eyeExtraX = 0;
  int eyeExtraY = 0;
  int mouthExtraX = 0;
  int mouthExtraY = 0;
  int extraExtraY = 0;
  int leftEyeExtraX = 0;
  int rightEyeExtraX = 0;
  int leftEyeExtraY = 0;
  int rightEyeExtraY = 0;
  int leftEyeAddW = 0;
  int rightEyeAddW = 0;
  int leftEyeAddH = 0;
  int rightEyeAddH = 0;

  float alive = 0.55f + motionPower * 1.25f + voicePower * 1.35f + actionPulse * 0.95f;
  if (alive > 2.8f) alive = 2.8f;

  dx += baseSway;
  dy += baseBob;
  dy -= (int)(reactionPulse * 2.2f);

  leftEyeExtraX += (int)(sinf(t * 2.7f + 0.3f) * alive * 0.65f);
  rightEyeExtraX += (int)(sinf(t * 2.4f + 1.8f) * alive * 0.65f);
  leftEyeExtraY += (int)(cosf(t * 2.0f + 0.4f) * alive * 0.42f);
  rightEyeExtraY += (int)(cosf(t * 2.25f + 1.1f) * alive * 0.42f);
  mouthExtraY += (int)(sinf(t * 1.9f + 2.0f) * alive * 0.30f);

  if (actionKind == 1) {
    dy -= (int)(actionPulse * 3.0f);
    eyeAddH += (int)(actionPulse * 2.0f);
    mouthAddW += (int)(actionPulse * 4.0f);
    drawSoftSparkle(18, 19 - (int)(actionPulse * 5.0f), 1);
    drawSoftSparkle(111, 17 - (int)(actionPulse * 4.0f), 1);
  } else if (actionKind == 2) {
    int peek = (int)(sinf(actionPulse * 3.1415926f) * 4.0f);
    eyeExtraX += peek;
    mouthExtraX -= peek / 2;
  } else if (actionKind == 3) {
    mouthAddW += (int)(actionPulse * 3.0f);
    leftEyeAddW += (int)(actionPulse * 2.0f);
    rightEyeAddH -= (int)(actionPulse * 2.0f);
  } else if (actionKind == 4) {
    int rise = (int)(actionPulse * 8.0f);
    drawMiniHeart(20, 48 - rise);
    drawMiniHeart(110, 47 - rise / 2);
  }

  if (face == BMP_FACE_NORMAL) {
    eyeExtraY += (int)(soft * 0.8f);
    mouthExtraY += (int)(sinf(t * 2.0f + 0.4f) * 0.8f);
    if (actionPulse > 0.01f) {
      mouthAddW += (int)(actionPulse * 2.0f);
      leftEyeExtraY -= (int)(actionPulse * 1.4f);
      rightEyeExtraY -= (int)(actionPulse * 1.0f);
    }
  } else if (face == BMP_FACE_CONTENT) {
    eyeExtraY += (int)(breathe * 0.9f);
    mouthAddW += 1;
    mouthExtraY += (int)(sinf(t * 1.9f + 0.5f) * 0.8f);
    leftEyeExtraX -= (int)(actionPulse * 1.6f);
    rightEyeExtraX += (int)(actionPulse * 1.6f);
  } else if (face == BMP_FACE_HAPPY) {
    int bounce = (int)(-fabsf(sinf(t * 4.7f)) * (2.0f + actionPulse * 2.0f));
    dy += bounce;
    eyeAddW = 1 + (int)(fabsf(sinf(t * 3.2f)) * 1.2f);
    eyeAddH = (int)(fabsf(sinf(t * 3.2f + 0.5f)) * 1.2f);
    mouthAddW = 2 + (int)(fabsf(sinf(t * 4.0f)) * 2.0f);
    mouthExtraY = (int)(sinf(t * 4.4f) * 1.0f);
    leftEyeExtraY -= (int)(fabsf(sinf(t * 5.0f)) * 1.2f);
    rightEyeExtraY -= (int)(fabsf(sinf(t * 5.0f + 0.7f)) * 1.2f);
    int heartRise = (int)((now / 90UL) % 12UL);
    drawMiniHeart(19, 48 - heartRise / 2);
    drawMiniHeart(111, 47 - (heartRise + 5) / 2);
  } else if (face == BMP_FACE_LOVE) {
    float glow = fabsf(sinf(t * 2.8f));
    dy -= (int)(glow * 1.4f);
    eyeAddW = 1 + (int)(glow * 2.0f);
    eyeAddH = 1 + (int)(glow * 2.0f);
    mouthAddW = 2 + (int)(glow * 3.0f);
    int rise = (int)((now / 80UL) % 18UL);
    drawMiniHeart(17, 49 - rise / 2);
    drawMiniHeart(112, 46 - ((rise + 8) % 18) / 2);
    drawMiniHeart(64 + (int)(sinf(t * 2.0f) * 6.0f), 12 + (int)(cosf(t * 1.8f) * 3.0f));
  } else if (face == BMP_FACE_SHY) {
    lookX -= 1;
    lookY += 1;
    dx += (int)(sinf(t * 1.7f) * 1.0f);
    mouthExtraY += 1;
    for (int i = 0; i < 3; i++) {
      display.drawLine(20 + i * 4, 46 + i % 2, 23 + i * 4, 45 + i % 2, OLED_WHITE);
      display.drawLine(103 + i * 4, 45 + i % 2, 106 + i * 4, 46 + i % 2, OLED_WHITE);
    }
  } else if (face == BMP_FACE_LAUGH) {
    int laughBounce = (int)(-fabsf(sinf(t * 6.0f)) * 3.0f);
    dy += laughBounce;
    mouthAddW = 3 + (int)(fabsf(sinf(t * 6.0f)) * 4.0f);
    mouthAddH = 1;
    mouthExtraY = (int)(sinf(t * 6.0f + 0.5f) * 1.0f);
    drawSoftSparkle(18, 18 + (int)(soft * 2.0f), 1);
    drawSoftSparkle(112, 18 - (int)(soft * 2.0f), 1);
  } else if (face == BMP_FACE_PLEADING) {
    lookY -= 1;
    eyeAddW = 1 + (int)(fabsf(sinf(t * 2.4f)) * 2.0f);
    eyeAddH = 1 + (int)(fabsf(sinf(t * 2.4f)) * 2.0f);
    mouthExtraY += 1;
    display.drawPixel(36, 48 + (int)(soft * 1.0f), OLED_WHITE);
    display.drawPixel(92, 48 - (int)(soft * 1.0f), OLED_WHITE);
  } else if (face == BMP_FACE_PROUD) {
    dy -= 1;
    lookY -= 1;
    eyeAddH -= 1;
    mouthAddW += 2;
    drawSoftSparkle(20, 18 + (int)(sinf(t * 2.0f) * 2.0f), 2);
    drawSoftSparkle(110, 20 - (int)(sinf(t * 1.8f) * 2.0f), 1);
  } else if (face == BMP_FACE_CURIOUS) {
    float scan = sinf(t * 1.8f);
    float think = sinf(t * 3.1f + 0.5f);
    dx += (int)(scan * 1.2f);
    dy += (int)(-fabsf(scan) * 1.0f);
    eyeExtraX += (int)(scan * 3.0f);
    eyeExtraY += (int)(think * 1.4f);
    leftEyeExtraY -= (int)(fabsf(scan) * 1.2f);
    rightEyeExtraY += (int)(think * 1.1f);
    leftEyeAddH += (int)(fabsf(think) * 1.0f);
    rightEyeAddW += (int)(fabsf(scan) * 1.0f);
    mouthExtraX += (int)(sinf(t * 2.4f + 1.0f) * 1.0f);
    mouthExtraY += (int)(think * 1.0f);
    uint8_t thoughtPhase = (uint8_t)((now / 900UL) % 4UL);
    if (thoughtPhase == 1) {
      int qx = 104 + (int)(sinf(t * 4.2f) * 3.0f);
      int qy = 4 + (int)(cosf(t * 3.2f) * 2.0f);
      drawBitmapScaled(lpk_question_layer_4, 6, 10, qx, qy, 6, 10);
    } else {
      int dotY = 10 + (int)(sinf(t * 2.0f) * 2.0f);
      display.drawPixel(105, dotY, OLED_WHITE);
      display.drawPixel(110, dotY + 2, OLED_WHITE);
      if (thoughtPhase == 3) drawSoftSparkle(113, dotY + 5, 1);
    }
  } else if (face == BMP_FACE_ANNOYED) {
    lookX += (int)(sinf(t * 2.6f) * 1.8f);
    eyeAddW += (int)(fabsf(soft) * 1.0f);
    mouthAddW -= 2;
    mouthExtraX += (int)(sinf(t * 2.1f + 2.0f) * 1.0f);
    int browLift = (int)(sinf(t * 2.2f) * 1.0f);
    display.drawLine(29, 29 + browLift, 54, 27 + browLift, OLED_WHITE);
    display.drawLine(76, 27 - browLift, 101, 29 - browLift, OLED_WHITE);
  } else if (face == BMP_FACE_SURPRISED) {
    float pop = fabsf(sinf(t * 5.2f));
    eyeAddW = 1 + (int)(pop * 3.0f);
    eyeAddH = 1 + (int)(pop * 2.0f);
    mouthAddW = (int)(pop * 3.0f);
    mouthAddH = (int)(pop * 2.0f);
    dy -= (int)(pop * 2.0f);
    drawSoftSparkle(18, 14 + (int)(soft * 2.0f), 2);
    drawSoftSparkle(111, 17 - (int)(soft * 2.0f), 1);
  } else if (face == BMP_FACE_DIZZY) {
    float dizzyPower = clampFloat(spinSmooth + shakeSmooth * 0.55f, 0.25f, 1.0f);
    float orbit = t * (5.2f + dizzyPower * 8.0f);
    eyeExtraX += (int)(sinf(orbit) * (2.0f + dizzyPower * 3.0f));
    eyeExtraY += (int)(cosf(orbit * 0.9f) * (1.0f + dizzyPower * 2.0f));
    leftEyeExtraX += (int)(sinf(orbit + 1.1f) * (1.0f + dizzyPower * 2.0f));
    rightEyeExtraX += (int)(cosf(orbit + 0.5f) * (1.0f + dizzyPower * 2.0f));
    leftEyeAddH += (int)(sinf(orbit * 0.8f) * 1.0f);
    rightEyeAddW += (int)(cosf(orbit * 0.7f) * 1.0f);
    mouthExtraX += (int)(sinf(orbit * 0.7f + 1.0f) * (1.0f + dizzyPower));
    mouthExtraY += (int)(cosf(orbit * 0.6f) * (1.0f + dizzyPower));
    dx += (int)(sinf(t * (2.2f + dizzyPower)) * (0.8f + dizzyPower * 1.6f));
    dy += (int)(cosf(t * (2.0f + dizzyPower)) * (0.8f + dizzyPower * 1.2f));
    for (int i = 0; i < 4; i++) {
      float a = orbit + i * 2.1f;
      int px = 18 + (int)(cosf(a) * (4 + i + dizzyPower * 4.0f));
      int py = 13 + (int)(sinf(a) * (3 + i + dizzyPower * 2.0f));
      display.drawCircle(px, py, 1 + (i & 1), OLED_WHITE);
    }
    display.drawCircle(106 + (int)(sinf(orbit) * (3.0f + dizzyPower * 2.0f)),
                       13 + (int)(cosf(orbit) * (2.0f + dizzyPower * 2.0f)), 3, OLED_WHITE);
    display.drawPixel(64 + (int)(cosf(orbit * 0.55f) * 14.0f), 8 + (int)(sinf(orbit * 0.55f) * 3.0f), OLED_WHITE);
  } else if (face == BMP_FACE_SLEEPY) {
    eyeExtraY += (int)(sinf(t * 1.4f) * 1.0f);
    mouthExtraY += (int)(sinf(t * 1.8f + 0.8f) * 1.0f);
    extraExtraY += -(int)((now / 180UL) % 10UL);
  } else if (face == BMP_FACE_HOT) {
    float pant = fabsf(sinf(t * 4.7f));
    dy += (int)(pant * 1.0f);
    mouthAddW += (int)(pant * 3.0f);
    mouthAddH += (int)(pant * 2.0f);
    eyeExtraY += 1;
    int sx = 20 + (int)(sinf(t * 2.5f) * 1.0f);
    display.fillCircle(sx, 18, 2, OLED_WHITE);
    display.fillTriangle(sx - 2, 17, sx + 2, 17, sx, 10, OLED_WHITE);
  } else if (face == BMP_FACE_COLD) {
    float tremble = sinf(t * 5.2f) * 1.2f;
    dx += (int)tremble;
    eyeAddW -= 1;
    mouthExtraX += (int)(-tremble);
    display.drawLine(18, 17, 22, 13, OLED_WHITE);
    display.drawLine(22, 13, 26, 17, OLED_WHITE);
    display.drawLine(106, 17, 110, 13, OLED_WHITE);
    display.drawLine(110, 13, 114, 17, OLED_WHITE);
  } else if (face == BMP_FACE_SING) {
    float beat = fabsf(sinf(t * 7.2f));
    float voiceBeat = max(beat * 0.55f, voicePower);
    dy += (int)(sinf(t * 3.6f) * (1.2f + voicePower * 2.0f));
    dx += (int)(sinf(t * 2.1f) * voicePower * 2.0f);
    eyeExtraX += (int)(sinf(t * 2.2f) * (1.0f + voicePower * 2.2f));
    leftEyeExtraY -= (int)(voiceBeat * 1.5f);
    rightEyeExtraY -= (int)(sinf(t * 6.5f + 0.8f) * voicePower * 1.3f);
    mouthAddW += (int)(voiceBeat * 5.0f);
    mouthAddH += (int)(voiceBeat * 4.0f);
    int nY = 18 - (int)((now / 120UL) % 8UL);
    display.drawCircle(17, nY + 8, 2, OLED_WHITE);
    display.drawFastVLine(20, nY, 8, OLED_WHITE);
    display.drawFastHLine(20, nY, 5, OLED_WHITE);
    display.drawCircle(109, nY + 11, 2, OLED_WHITE);
    display.drawFastVLine(112, nY + 3, 8, OLED_WHITE);
  } else if (face == BMP_FACE_ANGRY) {
    float charge = fabsf(sinf(t * 3.5f));
    float recoil = powf(fabsf(sinf(t * 2.2f)), 5.0f);
    dx += (int)(sinf(t * 4.4f) * (0.8f + charge * 0.9f));
    dy -= (int)(charge * 1.4f + recoil * 2.0f);
    mouthAddW += (int)(charge * 3.0f);
    mouthExtraY += (int)(recoil * 1.4f);
    eyeAddW += (int)(charge * 2.0f);
    eyeAddH -= 1;
    leftEyeExtraX += (int)(recoil * -2.0f);
    rightEyeExtraX += (int)(recoil * 2.0f);
    display.drawBitmap(13 + (int)(sinf(t * 8.0f) * 1.0f), 8 + (int)(cosf(t * 5.0f) * 1.0f),
                       img_angry_vein, 8, 8, OLED_WHITE);
    display.drawLine(104, 11, 121, 8 + (int)(sinf(t * 4.0f) * 2.0f), OLED_WHITE);
    display.drawLine(106, 16, 122, 15 + (int)(cosf(t * 3.6f) * 2.0f), OLED_WHITE);
    if (((now / 180UL) % 2UL) == 0) {
      display.drawLine(24, 7, 18, 4, OLED_WHITE);
      display.drawLine(116, 29, 123, 27, OLED_WHITE);
    }
  }

  bool blinkable = face != BMP_FACE_DIZZY && face != BMP_FACE_ANGRY &&
                   face != BMP_FACE_SLEEPY && face != BMP_FACE_ANNOYED &&
                   face != BMP_FACE_LAUGH;
  int leftBlinkH = blinkable && f.left.h > 12 ? -(int)((float)(f.left.h - 5) * blinkL) : 0;
  int rightBlinkH = blinkable && f.right.h > 12 ? -(int)((float)(f.right.h - 5) * blinkR) : 0;

  drawBitmapLayerDynamic(f.left, dx, dy, lookX + eyeExtraX + leftEyeExtraX, lookY + eyeExtraY + leftEyeExtraY,
                         eyeAddW + leftEyeAddW, eyeAddH + leftEyeAddH + leftBlinkH);
  drawBitmapLayerDynamic(f.right, dx, dy, lookX + eyeExtraX + rightEyeExtraX, lookY + eyeExtraY + rightEyeExtraY,
                         eyeAddW + rightEyeAddW, eyeAddH + rightEyeAddH + rightBlinkH);
  drawBitmapLayerDynamic(f.mouth, dx, dy, mouthExtraX, mouthExtraY,
                         mouthAddW, mouthAddH);
  drawBitmapLayerDynamic(f.extra, dx, dy, 0, extraExtraY, 0, 0);
}

void drawMochi(bool active) {
  if (!oledReady) return;
  unsigned long now = millis();
  static float bitmapSmoothLookX = 0.0f;
  static float bitmapSmoothLookY = 0.0f;
  static float bitmapTargetLookX = 0.0f;
  static float bitmapTargetLookY = 0.0f;
  static unsigned long nextNormalGlanceMs = 0;
  static BitmapMpuFace bitmapShownFace = BMP_FACE_NORMAL;
  static BitmapMpuFace bitmapPreviousFace = BMP_FACE_NORMAL;
  static unsigned long bitmapMorphStartMs = 0;
  static BitmapMpuFace bitmapIdleFace = BMP_FACE_NORMAL;
  static unsigned long nextCuteIdleMs = 0;
  static unsigned long bitmapBlinkStartMs = 0;
  static unsigned long nextBitmapBlinkMs = 900;
  static unsigned long bitmapWinkStartMs = 0;
  static unsigned long nextBitmapWinkMs = 6500;
  static bool bitmapWinkLeft = false;
  static unsigned long bitmapActionStartMs = 0;
  static unsigned long nextBitmapActionMs = 2600;
  static uint8_t bitmapActionKind = 0;

  bool bitmapMpuReady = mpuReady || rawMpuMode;
  bool bitmapCalm = !active && (!bitmapMpuReady || (shakeSmooth < 0.18f && spinSmooth < 0.12f && fabsf(tiltX) < 0.28f && fabsf(tiltY) < 0.32f));
  if (!active && now >= nextCuteIdleMs) {
    switch (random(0, 8)) {
      case 0: bitmapIdleFace = BMP_FACE_NORMAL; break;
      case 1: bitmapIdleFace = BMP_FACE_CONTENT; break;
      case 2: bitmapIdleFace = BMP_FACE_HAPPY; break;
      case 3: bitmapIdleFace = BMP_FACE_SHY; break;
      case 4: bitmapIdleFace = BMP_FACE_PROUD; break;
      case 5: bitmapIdleFace = BMP_FACE_PLEADING; break;
      case 6: bitmapIdleFace = BMP_FACE_LOVE; break;
      default: bitmapIdleFace = BMP_FACE_CONTENT; break;
    }
    nextCuteIdleMs = now + random(4200, 9000);
  }

  float bitmapActionPulse = 0.0f;
  if (bitmapCalm && bitmapActionStartMs == 0 && now >= nextBitmapActionMs) {
    bitmapActionStartMs = now;
    bitmapActionKind = (uint8_t)random(1, 5);
    nextBitmapActionMs = now + random(4200, 8500);
  }
  if (bitmapActionStartMs != 0) {
    unsigned long actionAge = now - bitmapActionStartMs;
    if (actionAge < 1150UL) {
      float p = (float)actionAge / 1150.0f;
      bitmapActionPulse = sinf(p * 3.1415926f);
    } else {
      bitmapActionStartMs = 0;
      bitmapActionKind = 0;
    }
  }

  BitmapMpuFace bitmapTargetFace = bitmapIdleFace;
  if (bitmapMpuReady && (superAngryMode || angryMode)) {
    bitmapTargetFace = BMP_FACE_ANGRY;
  } else if (bitmapMpuReady && (now < dizzyUntilMs || now < headShakeUntilMs || shakeSmooth > 0.62f || spinSmooth > 0.32f || spinMeter > 0.38f)) {
    bitmapTargetFace = BMP_FACE_DIZZY;
  } else if (bitmapMpuReady && (now < freefallUntilMs || now < surprisedUntilMs)) {
    bitmapTargetFace = BMP_FACE_SURPRISED;
  } else if (active) {
    bitmapTargetFace = BMP_FACE_SING;
  } else if (now < touchLoveUntilMs) {
    bitmapTargetFace = BMP_FACE_LOVE;
  } else if (now < laughUntilMs) {
    bitmapTargetFace = BMP_FACE_LAUGH;
  } else if (now < touchHappyUntilMs) {
    bitmapTargetFace = BMP_FACE_HAPPY;
  } else if (faceDownMode || now < sadUntilMs) {
    bitmapTargetFace = BMP_FACE_SLEEPY;
  } else if (now < pantUntilMs || tempMood > 0.45f) {
    bitmapTargetFace = BMP_FACE_HOT;
  } else if (tempMood < -0.45f) {
    bitmapTargetFace = BMP_FACE_COLD;
  } else if (bitmapMpuReady && (now < annoyedUntilMs || shakeSmooth > 0.30f)) {
    bitmapTargetFace = BMP_FACE_ANNOYED;
  } else if (bitmapMpuReady && now < nodUntilMs) {
    bitmapTargetFace = BMP_FACE_HAPPY;
  } else if (bitmapMpuReady && (now < curiousUntilMs || fabsf(tiltX) > 0.32f || fabsf(tiltY) > 0.34f)) {
    bitmapTargetFace = BMP_FACE_CURIOUS;
  } else if (now < micActiveUntilMs) {
    bitmapTargetFace = BMP_FACE_CURIOUS;
  }

  if (bitmapTargetFace != bitmapShownFace) {
    bitmapPreviousFace = bitmapShownFace;
    bitmapShownFace = bitmapTargetFace;
    bitmapMorphStartMs = now;
  }

  if (now >= nextNormalGlanceMs) {
    uint8_t glanceKind = (uint8_t)random(0, 10);
    if (glanceKind < 3) {
      bitmapTargetLookX = 0.0f;
      bitmapTargetLookY = 0.0f;
    } else if (glanceKind < 7) {
      bitmapTargetLookX = (float)random(-5, 6);
      bitmapTargetLookY = (float)random(-1, 2);
    } else {
      bitmapTargetLookX = (float)random(-3, 4);
      bitmapTargetLookY = (float)random(-2, 3);
    }
    nextNormalGlanceMs = now + (active ? random(700, 1800) : random(1100, 3200));
  }

  if (bitmapBlinkStartMs == 0 && bitmapWinkStartMs == 0 && now >= nextBitmapBlinkMs) {
    bitmapBlinkStartMs = now;
    nextBitmapBlinkMs = now + random(2300, active ? 4200 : 5800);
  }

  if (!active && bitmapBlinkStartMs == 0 && bitmapWinkStartMs == 0 && now >= nextBitmapWinkMs) {
    bitmapWinkStartMs = now;
    bitmapWinkLeft = random(0, 2) == 0;
    nextBitmapWinkMs = now + random(12000, 22000);
  }

  float bitmapBlinkL = 0.0f;
  float bitmapBlinkR = 0.0f;
  if (bitmapBlinkStartMs != 0) {
    unsigned long blinkAge = now - bitmapBlinkStartMs;
    if (blinkAge < 210UL) {
      float p = (float)blinkAge / 210.0f;
      float b = sinf(p * 3.1415926f);
      bitmapBlinkL = b;
      bitmapBlinkR = b;
    } else {
      bitmapBlinkStartMs = 0;
    }
  }
  if (bitmapWinkStartMs != 0) {
    unsigned long winkAge = now - bitmapWinkStartMs;
    if (winkAge < 520UL) {
      float p = (float)winkAge / 520.0f;
      float w = sinf(p * 3.1415926f);
      if (bitmapWinkLeft) bitmapBlinkL = max(bitmapBlinkL, w);
      else bitmapBlinkR = max(bitmapBlinkR, w);
    } else {
      bitmapWinkStartMs = 0;
    }
  }

  float bitmapMpuLookX = bitmapMpuReady ? clampFloat(tiltX * 2.2f, -2.5f, 2.5f) : 0.0f;
  float bitmapMpuLookY = bitmapMpuReady ? clampFloat(tiltY * 1.5f, -1.5f, 1.5f) : 0.0f;
  float bitmapMicroLookX = sinf(now * (active ? 0.0038f : 0.0018f)) * (active ? 1.8f : 0.8f);
  float bitmapMicroLookY = sinf(now * 0.0013f + 1.1f) * 0.5f;
  float bitmapBreathe = sinf(now * 0.0016f);
  float bitmapTinySway = sinf(now * 0.0011f + 1.4f);
  float bitmapSongPulse = active ? sinf(now * 0.0075f) * 0.8f : 0.0f;
  float bitmapShakeBob = bitmapMpuReady ? clampFloat(shakeSmooth * 2.2f, 0.0f, 2.2f) : 0.0f;
  float bitmapSpinFloat = bitmapMpuReady ? clampFloat(spinSmooth * 3.0f, 0.0f, 3.0f) : 0.0f;
  float bitmapMotionPower = bitmapMpuReady ? clampFloat(shakeSmooth + spinSmooth * 0.8f + fabsf(tiltX) * 0.22f + fabsf(tiltY) * 0.16f, 0.0f, 1.0f) : 0.0f;
  float bitmapVoicePower = active ? clampFloat(levelSmooth * 1.8f, 0.0f, 1.0f) : 0.0f;
  float bitmapLookEase = (active || bitmapShownFace == BMP_FACE_CURIOUS || bitmapShownFace == BMP_FACE_DIZZY) ? 0.14f : 0.085f;
  bitmapSmoothLookX += (bitmapTargetLookX + bitmapMpuLookX + bitmapMicroLookX - bitmapSmoothLookX) * bitmapLookEase;
  bitmapSmoothLookY += (bitmapTargetLookY + bitmapMpuLookY + bitmapMicroLookY - bitmapSmoothLookY) * bitmapLookEase;

  int bitmapBodyX = (int)(bitmapTinySway * 0.8f + (bitmapMpuReady ? tiltX * 1.2f : 0.0f) + sinf(now * 0.006f) * bitmapSpinFloat + sinf(now * 0.0025f) * bitmapActionPulse * 2.0f);
  int bitmapBodyY = (int)(bitmapBreathe * 1.2f + bitmapSongPulse + bitmapShakeBob + cosf(now * 0.005f) * bitmapSpinFloat * 0.6f - bitmapActionPulse * 1.8f);
  int bitmapEyeX = (int)bitmapSmoothLookX;
  int bitmapEyeY = (int)bitmapSmoothLookY;

  const unsigned long bitmapMorphDurationMs = 560UL;
  float bitmapMorphT = bitmapMorphStartMs == 0 ? 1.0f
                       : (float)(now - bitmapMorphStartMs) / (float)bitmapMorphDurationMs;
  bool bitmapMorphing = bitmapMorphT < 1.0f;
  bitmapMorphT = smoothStep01(bitmapMorphT);
  float bitmapReactionPulse = 0.0f;
  if (bitmapMorphStartMs != 0) {
    unsigned long reactionAge = now - bitmapMorphStartMs;
    if (reactionAge < 920UL) {
      bitmapReactionPulse = sinf(((float)reactionAge / 920.0f) * 3.1415926f);
    }
  }

  display.clearDisplay();
  if (bitmapMorphing) {
    int morphPopY = -(int)(bitmapReactionPulse * 2.0f);
    int morphPopX = (int)(sinf(bitmapMorphT * 6.2831852f) * bitmapReactionPulse * 1.2f);
    drawBitmapFaceMorph(bitmapPreviousFace, bitmapShownFace, bitmapMorphT, bitmapBodyX + morphPopX, bitmapBodyY + morphPopY);
  } else {
    drawBitmapFaceStable(bitmapShownFace, now, active, bitmapBodyX, bitmapBodyY,
                         bitmapEyeX, bitmapEyeY, bitmapBlinkL, bitmapBlinkR,
                         bitmapMotionPower, bitmapVoicePower, bitmapReactionPulse,
                         bitmapActionKind, bitmapActionPulse);
  }
  display.display();
  return;

  float songSec = (float)playedBytes / (float)(AUDIO_RATE * 2UL);
  CueMood cue = active ? cueMoodAt(songSec) : CUE_IDLE;
  float targetCue = active ? 1.0f : 0.0f;
  cueSmooth += (targetCue - cueSmooth) * 0.12f;
  float voice = active ? levelSmooth : 0.0f;
  float beat = active ? fabsf(sinf(now * 0.0105f)) : 0.0f;
  float phrase = active ? fabsf(sinf(now * 0.0017f)) : fabsf(sinf(now * 0.0009f));
  float danceBeat = active ? sinf(now * 0.0125f) : 0.0f;
  float danceStep = active ? (danceBeat >= 0.0f ? 1.0f : -1.0f) : 0.0f;
  float motion = mpuReady ? shakeSmooth : 0.0f;
  float tiltLookX = mpuReady ? tiltX * 4.0f : 0.0f;
  float tiltLookY = mpuReady ? tiltY * 2.2f : 0.0f;

  if (!active && motion < 0.1f && now >= nextIdleExprMs) {
    idleExpr = random(0, 14); // 0 to 13 to include new expressions
    nextIdleExprMs = now + random(4000, 10000);
    // Occasionally wink during idle
    if (random(0, 3) == 0 && !isWinking) {
      isWinking = true;
      winkLeft = random(0, 2) == 0;
      winkStartMs = now + random(500, 2000);
      winkProgress = 0.0f;
    }
  }

  // Wink system (separate from blink)
  if (isWinking && now >= winkStartMs && !isBlinking) {
    long elapsed = now - winkStartMs;
    if (elapsed < 80) winkProgress = elapsed / 80.0f;
    else if (elapsed < 500) winkProgress = 1.0f;
    else if (elapsed < 600) winkProgress = 1.0f - (elapsed - 500) / 100.0f;
    else { isWinking = false; winkProgress = 0.0f; nextWinkMs = now + random(8000, 20000); }
  }

  if (now > nextBlinkMs && !isBlinking) {
    isBlinking = true;
    blinkStartMs = now;
    nextBlinkMs = now + random(2000, 6000);
  }
  if (isBlinking) {
    long elapsed = now - blinkStartMs;
    if (elapsed < 60) blinkProgress = elapsed / 60.0f;
    else if (elapsed < 140) blinkProgress = 1.0f - (elapsed - 60) / 80.0f;
    else { isBlinking = false; blinkProgress = 0.0f; }
  }

  float hotMood = tempMood > 0.0f ? tempMood : 0.0f;
  float coldMood = tempMood < 0.0f ? -tempMood : 0.0f;
  float humid = humidMood;
  bool sad = now < sadUntilMs;
  bool angry = !sad && mpuReady && (angryMode || superAngryMode);
  bool superAngry = !sad && mpuReady && superAngryMode;
  bool dizzy = !sad && !angry && (now < dizzyUntilMs);
  bool annoyed = !sad && !angry && !dizzy && (now < annoyedUntilMs);
  bool touchHappy = now < touchHappyUntilMs;
  bool touchLove = now < touchLoveUntilMs;
  bool touchSleepy = now < touchSleepyUntilMs || (touchDown && (now - touchDownMs > 2000UL));
  bool nodding = now < nodUntilMs;
  bool headShaking = now < headShakeUntilMs;
  bool surprised = now < surprisedUntilMs;
  bool curious = now < curiousUntilMs;
  bool freefalling = now < freefallUntilMs;
  float targetBob = active ? sinf(now * 0.007f) * (1.8f + voice * 2.0f) - beat * voice * 3.0f
                           : sinf(now * 0.0018f) * 1.0f;
  // Beat sync: extra drop on strong beat
  if (active) {
    float beatPunch = fabsf(sinf(now * 0.0105f));
    if (beatPunch > 0.85f) targetBob -= voice * 5.0f; // sharp head drop on beat
  }
  // Random organic idle glance system
  if (!active && now >= nextGlanceMs) {
    idleGlanceTargetX = (float)((int)((now / 7) % 13) - 6) * 1.4f;
    idleGlanceTargetY = (float)((int)((now / 11) % 7) - 3) * 0.7f;
    nextGlanceMs = now + 1800UL + (now % 3000UL);
  }
  float targetLookX = active ? sinf(now * 0.0022f) * (4.0f + voice * 3.0f)
                             : idleGlanceTargetX + sinf(now * 0.0009f) * 1.2f;
  float targetLookY = active ? sinf(now * 0.0016f + 1.2f) * (1.2f + voice * 2.0f)
                             : idleGlanceTargetY + sinf(now * 0.0007f) * 0.6f;
  float targetLean = active ? sinf(now * 0.0032f) * voice * 4.0f
                            : sinf(now * 0.0015f) * 1.5f;
  float targetDanceX = active ? danceStep * (1.5f + voice * 5.5f) : 0.0f;
  targetLookX += tiltLookX;
  targetLookY += tiltLookY;
  targetLean += tiltX * 5.0f;
  targetBob += tiltY * 3.5f;
  if (!active) targetBob += motion * 2.0f;
  targetLookY += hotMood * 2.0f - coldMood * 0.7f;
  targetBob += hotMood * 1.2f;
  targetDanceX += sinf(now * 0.045f) * coldMood * 2.8f;
  if (motion > 0.18f) {
    targetLookX += sinf(now * 0.018f) * motion * 3.2f;
    targetLookY += cosf(now * 0.016f) * motion * 1.6f;
    targetDanceX += sinf(now * 0.020f) * motion * 2.4f;
  }
  if (angry) {
    targetLookY += 1.0f;
    targetDanceX *= 0.35f;
    targetLean += sinf(now * 0.024f) * 0.9f;
  }
  if (touchHappy || touchLove) {
    targetLookY -= 1.4f;
    targetBob -= fabsf(sinf(now * 0.012f)) * 1.5f;
    targetDanceX += sinf(now * 0.013f) * (touchLove ? 3.0f : 1.5f);
  }
  if (touchSleepy) {
    targetLookY += 2.0f;
    targetDanceX *= 0.15f;
    targetBob += 1.2f;
  }
  if (nodding) {
    targetBob -= fabsf(sinf(now * 0.015f)) * 3.0f;
  }
  if (headShaking) {
    targetLookX += sinf(now * 0.025f) * 5.0f;
    targetDanceX += sinf(now * 0.028f) * 3.0f;
  }
  if (faceDownMode) {
    targetLookY += 2.5f;
    targetBob += 1.5f;
  }

  switch (cue) {
    case CUE_DREAMY:
      targetLookY -= 1.0f;
      targetBob *= 0.75f;
      targetDanceX *= 0.35f;
      break;
    case CUE_SHY:
      targetLookX -= 3.0f;
      targetLookY += 1.2f;
      targetDanceX *= 0.45f;
      break;
    case CUE_WORRIED:
      targetLookX += sinf(now * 0.0055f) * 2.0f;
      targetLookY += 1.5f;
      targetDanceX *= 0.30f;
      break;
    case CUE_PLEADING:
      targetLookY -= 2.2f;
      targetBob -= 1.0f;
      targetDanceX *= 0.25f;
      break;
    case CUE_RUN:
      targetDanceX *= 1.45f;
      targetLean *= 1.35f;
      targetBob -= beat * 2.0f;
      break;
    case CUE_PRINCE:
      targetLookY -= 0.8f;
      targetDanceX *= 1.15f;
      break;
    case CUE_HAPPY:
      targetDanceX *= 1.25f;
      targetBob -= beat * 1.4f;
      break;
    case CUE_LONELY:
      targetLookY += 2.0f;
      targetDanceX *= 0.20f;
      targetBob += 1.0f;
      break;
    case CUE_RING:
      targetLookY -= 3.0f;
      targetDanceX *= 0.55f;
      targetBob -= 1.5f;
      break;
    case CUE_FINALE:
      targetDanceX *= 1.60f;
      targetBob -= beat * 2.2f;
      break;
    default:
      break;
  }

  if (now - lastJoyMs < 3000UL) {
    targetLookX = (float)manualJoyX;
    targetLookY = (float)manualJoyY;
  }
  if (active) {
    targetBob -= voice * 20.0f;
  }
  // Mic bob: gentle head-nod when hearing voice (not playing music)
  if (!active && now < micActiveUntilMs) {
    float micBob = sinf(now * 0.011f) * micSmooth * 4.0f;
    targetBob += micBob;
    targetLookX += sinf(now * 0.007f) * micSmooth * 2.0f;
  }

  float smoothX = 0.10f;
  float smoothY = 0.09f;
  faceBob += (targetBob - faceBob) * 0.14f;
  faceLookX += (targetLookX - faceLookX) * smoothX;
  faceLookY += (targetLookY - faceLookY) * smoothY;
  faceLean += (targetLean - faceLean) * 0.12f;
  faceDanceX += (targetDanceX - faceDanceX) * 0.16f;

  int dx = (int)(faceLookX + faceDanceX);
  int dy = (int)(faceLookY + faceBob);
  dx = constrain(dx, -10, 10);
  dy = constrain(dy, -8, 8);

  // --- 1. Map State to targetEmo ---
  uint8_t newTargetEmo = 0; // NORMAL

  if (freefallUntilMs > millis()) {
      newTargetEmo = 5; // SURPRISED
  } else if (now < laughUntilMs) {
      newTargetEmo = 12; // LAUGHING
  } else if (now < glitchUntilMs) {
      newTargetEmo = 11; // DEAD (glitch representation)
  } else if (now < cryUntilMs) {
      newTargetEmo = 13; // CRYING
  } else if (now < pantUntilMs) {
      newTargetEmo = 16; // PAIN (panting)
  } else if (active || voice > 0.15f) {
      newTargetEmo = 20; // MUSICAL
  } else if (superAngryMode) {
      newTargetEmo = 19; // RAGE
      dx += (int)(sinf(now * 0.05f) * 2.0f); dy += (int)(fabsf(sinf(now * 0.05f)) * 1.0f);
  } else if (angryMode) {
      newTargetEmo = 3; // ANGRY
  } else if (now < sadUntilMs || faceDownMode) {
      newTargetEmo = 2; // SAD
  } else if (now < surprisedUntilMs) {
      newTargetEmo = 5; // SURPRISED
  } else if (nodding || touchHappy) {
      newTargetEmo = 1; // HAPPY
  } else if (now < dizzyUntilMs || headShakeDetected) {
      newTargetEmo = 18; // DIZZY
  } else if (touchSleepy) {
      newTargetEmo = 6; // SLEEPY
  } else if (now < annoyedUntilMs) {
      newTargetEmo = 22; // BORED
  } else if (!active && now < micShoutUntilMs) {
      newTargetEmo = 8; // SCARED (startled)
      dy -= 3;
  } else if (!active && now < micActiveUntilMs) {
      newTargetEmo = 7; // EXCITED
  } else if (touchLove) {
      newTargetEmo = 4; // LOVE
  } else if (coldMood > 0.3f) {
      newTargetEmo = 16; // PAIN (shivering)
  } else {
      // Normal or Idle Expressions mapping
      switch (idleExpr) {
        case 1: newTargetEmo = 9; break; // CONFUSED
        case 2: newTargetEmo = 10; break; // SMUG
        case 3: newTargetEmo = 22; break; // BORED
        case 4: newTargetEmo = 7; break; // EXCITED
        case 5: newTargetEmo = 15; break; // ROLLING EYES
        case 6: newTargetEmo = 23; break; // PROUD
        case 7: newTargetEmo = 10; break; // SMUG
        case 8: newTargetEmo = 1; break; // HAPPY
        case 9: newTargetEmo = 6; break; // SLEEPY
        default: newTargetEmo = 0; break; // NORMAL
      }
  }

  // Handle Morphing State Machine
  if (newTargetEmo != targetEmo) {
    if (!morphing) {
        currentEmo = targetEmo;
    } else {
        // If already morphing, let it finish or snap it
        // We'll just snap current to whatever intermediate frame it is to avoid complex mid-morph math
        // But the PixelFace logic uses strict 0-MORPH_STEPS from `current` to `target`.
        // Easiest is to snap to the old target and start new morph.
        currentEmo = targetEmo; 
    }
    targetEmo = newTargetEmo;
    morphStep = 0;
    morphing = true;
  }

  if (morphing) {
    morphStep++;
    if (morphStep >= MORPH_STEPS) {
      morphStep = MORPH_STEPS;
      morphing = false;
      currentEmo = targetEmo;
    }
  }

  // Render the Frame!
  display.clearDisplay();
  
  bool isBlinkingNow = (isBlinking && blinkProgress < 0.6f) || (isWinking && winkLeft && winkProgress < 0.6f);
  // (Note: PixelFace handles only global blink lid=10. For full Wink we'd use WINK state, but we can just use normal blink logic)

  renderEmotion(targetEmo, morphStep, currentEmo, isBlinkingNow, MORPH_STEPS, dx, dy, faceLookX, faceLookY);

  display.display();
}

void initPingpong(bool resetScore) {
  ballX = 64.0f; ballY = 32.0f;
  ballVX = 2.0f;
  ballVY = (float)random(-15, 16) * 0.1f;
  paddlePlayerY = 32.0f; paddleAiY = 32.0f;
  if (resetScore) {
    scorePlayer = 0;
    scoreAi = 0;
  }
  gamePhase = GAME_WAIT;
  gameWaitStartMs = millis();
  lastGameTickMs = millis();
}

void updateGame() {
  if (currentState != STATE_GAMES) return;
  if (gamePhase != GAME_PLAYING && gamePhase != GAME_WAIT) return;
  
  unsigned long now = millis();
  unsigned long elapsed = now - lastGameTickMs;
  if (elapsed < 16) return; // fixed ~60fps tick
  lastGameTickMs = now;
  float dt = elapsed * 0.06f;
  if (dt > 2.5f) dt = 2.5f;

  if (gamePhase == GAME_WAIT) {
    if (now - gameWaitStartMs > 3000UL) {
      gamePhase = GAME_PLAYING;
    }
  }

  // Move ball only if playing
  if (gamePhase == GAME_PLAYING) {
    ballX += ballVX * dt;
    ballY += ballVY * dt;

    // Bounce top/bottom
    if (ballY < 0.0f) { ballY = 0.0f; ballVY = -ballVY; }
    if (ballY > 63.0f) { ballY = 63.0f; ballVY = -ballVY; }
  }

  // Player paddle: use web joystick (manualJoyY) OR MPU tilt
  float targetPlayerY;
  if (millis() - lastJoyMs < 500 && manualJoyY != 0) {
    // Web control: manualJoyY -100..100 mapped to 8..56
    targetPlayerY = 32.0f + (manualJoyY / 100.0f) * 24.0f;
  } else {
    // MPU tilt control
    targetPlayerY = 32.0f + (tiltY * 40.0f);
  }
  if (targetPlayerY < 8.0f) targetPlayerY = 8.0f;
  if (targetPlayerY > 56.0f) targetPlayerY = 56.0f;
  paddlePlayerY += (targetPlayerY - paddlePlayerY) * 0.35f;

  // AI logic (tracks ball with limited speed)
  float aiSpeed = 1.6f * dt;
  if (paddleAiY < ballY - 4.0f) paddleAiY += aiSpeed;
  if (paddleAiY > ballY + 4.0f) paddleAiY -= aiSpeed;
  if (paddleAiY < 8.0f) paddleAiY = 8.0f;
  if (paddleAiY > 56.0f) paddleAiY = 56.0f;

  // Collision with Player Paddle (left: x=8)
  if (ballX < 12.0f && ballX > 6.0f) {
    if (ballY > paddlePlayerY - 10.0f && ballY < paddlePlayerY + 10.0f) {
      ballX = 12.0f;
      ballVX = -ballVX;
      // Add english/spin based on hit position
      ballVY += (ballY - paddlePlayerY) * 0.15f;
      // Increase speed slightly
      if (ballVX < 6.0f) ballVX *= 1.05f;
    }
  }

  // Collision with AI Paddle (right: x=120)
  if (ballX > 116.0f && ballX < 122.0f) {
    if (ballY > paddleAiY - 10.0f && ballY < paddleAiY + 10.0f) {
      ballX = 116.0f;
      ballVX = -ballVX;
      ballVY += (ballY - paddleAiY) * 0.15f;
      if (ballVX > -6.0f) ballVX *= 1.05f;
    }
  }

  // Score
  if (gamePhase == GAME_PLAYING) {
    if (ballX < 0.0f) {
      scoreAi++;
      if (scoreAi >= 5) gamePhase = GAME_OVER;
      else initPingpong(); // reset round
    } else if (ballX > 128.0f) {
      scorePlayer++;
      if (scorePlayer >= 5) gamePhase = GAME_OVER;
      else initPingpong(); // reset round
    }
  }
}

void drawPingpong() {
  if (!oledReady) return;
  display.clearDisplay();

  if (gamePhase == GAME_IDLE) {
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);
    display.setCursor(40, 10);
    display.print("PINGPONG");
    display.setCursor(25, 28);
    display.print("Hold: Keluar");
    display.setCursor(25, 40);
    display.print("Tap: Mulai");
    display.display();
    return;
  }
  
  if (gamePhase == GAME_OVER) {
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);
    display.setCursor(30, 20);
    if (scorePlayer >= 5) display.print("KAMU MENANG!");
    else display.print("KAMU KALAH!");
    display.setCursor(20, 40);
    display.print("Tap untuk Main");
    display.display();
    return;
  }

  // Draw Center Dashed Line
  for (int y = 0; y < 64; y += 4) {
    display.drawPixel(64, y, OLED_WHITE);
  }

  // Draw Scores
  display.setTextSize(2);
  display.setTextColor(OLED_WHITE);
  display.setCursor(30, 4);
  display.print(scorePlayer);
  display.setCursor(85, 4);
  display.print(scoreAi);

  // Draw Ball
  if (gamePhase == GAME_PLAYING) {
    display.fillRect((int)ballX - 1, (int)ballY - 1, 3, 3, OLED_WHITE);
  } else if (gamePhase == GAME_WAIT) {
    display.setTextSize(1);
    display.setCursor(42, 24);
    display.print("READY?");
    
    unsigned long now = millis();
    int timeLeft = 3 - ((now - gameWaitStartMs) / 1000);
    if (timeLeft < 1) timeLeft = 1;
    
    display.setTextSize(2);
    display.setCursor(58, 38);
    display.print(timeLeft);
  }

  // Draw Paddles
  // Player (Left)
  display.fillRect(8, (int)paddlePlayerY - 8, 3, 16, OLED_WHITE);
  // AI (Right)
  display.fillRect(117, (int)paddleAiY - 8, 3, 16, OLED_WHITE);

  display.display();
}

void drawMenu(const char* title, const char** options, int numOptions, int cursor) {
  display.setTextSize(1);
  display.setTextColor(OLED_WHITE);
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 10, SCREEN_WIDTH, 10, OLED_WHITE);
  
  for(int i=0; i<numOptions; i++) {
    int y = 15 + i*10;
    if (i == cursor) {
      display.fillRect(0, y-1, SCREEN_WIDTH, 10, OLED_WHITE);
      display.setTextColor(OLED_BLACK);
    } else {
      display.setTextColor(OLED_WHITE);
    }
    display.setCursor(5, y);
    display.print(options[i]);
  }
}

void enterDrawMode(bool clearCanvas = true) {
  currentState = STATE_DRAW;
  if (!oledReady) return;
  if (clearCanvas) {
    memset(drawFrame, 0, sizeof(drawFrame));
    display.clearDisplay();
    display.display();
  }
}

void drawUI() {
  if (!oledReady) return;
  if (currentState == STATE_DRAW) return;
  display.clearDisplay();
  
  if (currentState == STATE_MENU) {
    const char* menuOpts[] = {"Games", "Dht", "Reminder", "Musik", "Draw", "Kembali"};
    drawMenu("MENU UTAMA", menuOpts, 6, menuCursor);
  } else if (currentState == STATE_MUSIC) {
    const char* musicOpts[] = {"Love Story", "MBG", "Hai", "Kembali"};
    drawMenu("PILIH LAGU", musicOpts, 4, musicCursor);
  } else if (currentState == STATE_GAMES) {
    drawPingpong();
    return; // drawPingpong calls display.display() itself
  } else if (currentState == STATE_SENSOR) {
    // Nice DHT22 display
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);
    display.setCursor(30, 0);
    display.print("DHT22");
    display.drawLine(0, 9, 128, 9, OLED_WHITE);
    // Temperature with big font
    display.drawRoundRect(2, 12, 60, 38, 4, OLED_WHITE);
    display.setCursor(8, 16);
    display.print("Suhu");
    display.setTextSize(2);
    display.setCursor(6, 26);
    if (!isnan(tempC)) {
      display.print((int)tempC);
      display.print("C");
    } else display.print("--C");
    display.setTextSize(1);
    // Humidity
    display.drawRoundRect(66, 12, 60, 38, 4, OLED_WHITE);
    display.setCursor(72, 16);
    display.print("Humid");
    display.setTextSize(2);
    display.setCursor(70, 26);
    if (!isnan(humPct)) {
      display.print((int)humPct);
      display.print("%");
    } else display.print("--%");
    display.setTextSize(1);
    // Status bar
    display.setCursor(20, 54);
    display.print("[tap balik]");
  } else if (currentState == STATE_CHAT) {
    display.clearDisplay();
    display.setFont(NULL);
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);

    const uint16_t totalChars = strlen(chatText);
    uint16_t visibleChars = (millis() - chatStartMs) / 24UL;
    if (visibleChars > totalChars) visibleChars = totalChars;

    int cursorX = 4;
    int cursorY = 6;
    uint16_t printed = 0;
    for (uint16_t i = 0; i < totalChars && printed < visibleChars; i++) {
      char c = chatText[i];
      if (c == '\r') continue;
      if (c == '\n') {
        cursorX = 4;
        cursorY += 10;
        if (cursorY > 54) break;
        continue;
      }
      if (c == ' ') {
        uint16_t nextWordLen = 0;
        for (uint16_t j = i + 1; j < totalChars && chatText[j] != ' ' && chatText[j] != '\n' && chatText[j] != '\r'; j++) {
          nextWordLen++;
        }
        if (cursorX + 6 + (int)nextWordLen * 6 > 124) {
          cursorX = 4;
          cursorY += 10;
          if (cursorY > 54) break;
          printed++;
          continue;
        }
      } else if (cursorX > 122) {
        cursorX = 4;
        cursorY += 10;
        if (cursorY > 54) break;
      }

      display.setCursor(cursorX, cursorY);
      display.write(c);
      cursorX += 6;
      printed++;
    }

    if (visibleChars >= totalChars && ((millis() / 360UL) % 2UL == 0) && cursorY <= 54) {
      display.setCursor(cursorX, cursorY);
      display.write('_');
    }

    if (visibleChars >= totalChars && millis() > chatDisplayUntilMs) {
      currentState = STATE_FACE;
    }
  } else if (currentState == STATE_REMINDER) {
    display.setTextSize(1);
    display.setTextColor(OLED_WHITE);
    display.setCursor(0, 0);
    display.print("REMINDER");
    display.drawLine(0, 10, SCREEN_WIDTH, 10, OLED_WHITE);
    
    display.setCursor(0, 20);
    display.setTextWrap(true);
    display.print(globalReminderText);
    display.setTextWrap(false);
  }
  
  display.display();
}

bool setupI2S() {
  i2s_config_t config = {};
  config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = AUDIO_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 12;
  config.dma_buf_len = AUDIO_FRAMES;
  config.use_apll = false; // ESP32-C3 does not support APLL
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = 0;

  i2s_pin_config_t pins = {};
  pins.bck_io_num = MAX_BCLK_PIN;
  pins.ws_io_num = MAX_LRC_PIN;
  pins.data_out_num = MAX_DIN_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

uint16_t ringFree() {
  return AUDIO_RING_SIZE - ringCount;
}

void ringReset() {
  ringHead = 0;
  ringTail = 0;
  ringCount = 0;
  playing = false;
  playedBytes = 0;
  levelSmooth = 0.0f;
  lastSample = 0;
  cueSmooth = 0.0f;
}

void ringPush(const uint8_t* data, uint16_t len) {
  uint16_t space = ringFree();
  if (len > space) len = space;
  for (uint16_t i = 0; i < len; i++) {
    audioRing[ringHead] = data[i];
    ringHead = (ringHead + 1) % AUDIO_RING_SIZE;
    ringCount++;
  }
}

bool ringPopByte(uint8_t& value) {
  if (ringCount == 0) return false;
  value = audioRing[ringTail];
  ringTail = (ringTail + 1) % AUDIO_RING_SIZE;
  ringCount--;
  return true;
}

void acceptClient() {
  if (audioClient && audioClient.connected()) return;
  WiFiClient next = audioServer.available();
  if (!next) return;

  if (audioClient) audioClient.stop();
  audioClient = next;
  audioClient.setNoDelay(true);
  ringReset();
  currentState = STATE_FACE;
  gamePhase = GAME_IDLE;
  totalBytes = 0;
  lastAudioMs = millis();
  Serial.println("audio client connected");
  drawStatus("WIFI AUDIO", "Client connected", "buffering...");
}

void readClientToRing() {
  if (!audioClient || !audioClient.connected()) return;
  while (audioClient.available() > 0 && ringFree() > 0) {
    int want = audioClient.available();
    if (want > READ_CHUNK) want = READ_CHUNK;
    if (want > ringFree()) want = ringFree();
    if (want <= 0) return;
    int got = audioClient.read(readBuffer, want);
    if (got <= 0) return;
    ringPush(readBuffer, got);
    totalBytes += got;
    lastAudioMs = millis();
  }
}

void writeAudioBlock() {
  unsigned long now = millis();
  if (playing && now - lastAudioMs > 1200UL && ringCount < AUDIO_PREBUFFER_BYTES) {
    ringReset();
    memset(stereoSamples, 0, sizeof(stereoSamples));
    size_t written = 0;
    i2s_write(I2S_NUM_0, stereoSamples, sizeof(stereoSamples), &written, portMAX_DELAY);
    return;
  }

  if (!playing) {
    if (ringCount < AUDIO_PREBUFFER_BYTES) {
      memset(stereoSamples, 0, sizeof(stereoSamples));
      size_t written = 0;
      i2s_write(I2S_NUM_0, stereoSamples, sizeof(stereoSamples), &written, portMAX_DELAY);
      return;
    }
    playing = true;
  }

  if (playing && ringCount < AUDIO_BLOCK_BYTES / 2 && now - lastAudioMs > 700UL) {
    playing = false;
  }

  double sum = 0.0;
  for (uint16_t i = 0; i < AUDIO_FRAMES; i++) {
    uint8_t lo = 0;
    uint8_t hi = 0;
    int16_t sample;
    if (ringPopByte(lo) && ringPopByte(hi)) {
      sample = (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
      lastSample = sample;
      playedBytes += 2;
    } else {
      lastSample = (int16_t)((int32_t)lastSample * 7 / 8);
      sample = lastSample;
    }
    stereoSamples[i * 2] = sample;
    stereoSamples[i * 2 + 1] = sample;
    sum += (double)sample * (double)sample;
  }

  size_t written = 0;
  i2s_write(I2S_NUM_0, stereoSamples, sizeof(stereoSamples), &written, portMAX_DELAY);

  float rms = sqrt(sum / AUDIO_FRAMES);
  float level = rms / 3600.0f;
  if (level > 1.0f) level = 1.0f;
  levelSmooth = levelSmooth * 0.84f + level * 0.16f;
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("WIFI AUDIO", "Connecting...", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000UL) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("OwiAudio", "12345678");
    drawStatus("WIFI AP", "SSID OwiAudio", "IP 192.168.4.1");
    Serial.println("AP mode: OwiAudio / 12345678");
  } else {
    char ip[22];
    snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    drawStatus("WIFI AUDIO", ip, "Port 7777");
    Serial.print("WiFi IP: ");
    Serial.println(ip);
  }
}

void micTask(void *pvParameters) {
  uint8_t micBuf[1024];
  size_t bytesRead;
  while(true) {
    if (i2s_read(I2S_NUM_0, micBuf, sizeof(micBuf), &bytesRead, 20 / portTICK_PERIOD_MS) == ESP_OK) {
      if (bytesRead > 0) {
        // --- Compute RMS of mic input for face expressions ---
        // INMP441 is 32-bit I2S, upper 24 bits used. Shift right 8.
        int32_t *samples32 = (int32_t*)micBuf;
        uint32_t count = bytesRead / 4;
        double sum = 0.0;
        int32_t peak = 0;
        for (uint32_t i = 0; i < count; i++) {
          int32_t s = samples32[i] >> 8;  // 24-bit value
          sum += (double)s * (double)s;
          if (abs(s) > peak) peak = abs(s);
        }
        float rms = (count > 0) ? sqrtf((float)(sum / count)) : 0.0f;
        // Normalize: INMP441 typical max ~800000 at loud speech
        float level = rms / 600000.0f;
        if (level > 1.0f) level = 1.0f;
        float peakLevel = (float)peak / 800000.0f;
        if (peakLevel > 1.0f) peakLevel = 1.0f;

        micSmooth = micSmooth * 0.80f + level * 0.20f;
        micPeak   = micPeak   * 0.90f + peakLevel * 0.10f;

        unsigned long now = millis();
        if (micSmooth > 0.04f) micActiveUntilMs = now + 600;
        if (micSmooth > 0.35f) micShoutUntilMs  = now + 800;
      }
    }
    vTaskDelay(8 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  delay(700);
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  Wire.begin(D4, D5);
  oledReady = display.begin(OLED_ADDR, true);
  if (oledReady) {
    display.clearDisplay();
    display.display();
  }
  setupMPU();
  setupDHT();

  if (!setupI2S()) {
    drawStatus("I2S ERR", "MAX D0 D8 D7", "SD wajib 3V3");
    while (true) delay(1000);
  }

  setupWiFi();
  telemetryUdp.begin(TELEMETRY_PORT);
  micUdp.begin(7799);
  audioServer.begin();
  audioServer.setNoDelay(true);
  Serial.println("TCP audio server on port 7777");
}

void handleSerial() {
  while (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() == 0) continue;
    unsigned long now = millis();
    if (cmd.startsWith("H")) {
      if (cmd.length() == 2049) {
        bool ok = true;
        for (int i = 0; i < 1024; i++) {
          char c1 = cmd.charAt(1 + i * 2);
          char c2 = cmd.charAt(2 + i * 2);
          int hi = (c1 >= '0' && c1 <= '9') ? c1 - '0' : (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 : (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
          int lo = (c2 >= '0' && c2 <= '9') ? c2 - '0' : (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 : (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
          if (hi < 0 || lo < 0) {
            ok = false;
            break;
          }
          drawFrame[i] = (uint8_t)((hi << 4) | lo);
        }
        if (ok && oledReady) {
          currentState = STATE_DRAW;
          display.clearDisplay();
          display.drawBitmap(0, 0, drawFrame, 128, 64, OLED_WHITE);
          display.display();
          Serial.println("cmd: Draw frame");
        }
      }
    } else if (cmd == "W") {
      enterDrawMode(true);
      Serial.println("cmd: Draw mode");
    } else if (cmd.startsWith("J")) {
      sscanf(cmd.c_str(), "J%d,%d", &manualJoyX, &manualJoyY);
      lastJoyMs = now;
    } else if (cmd.startsWith("M")) {
      sscanf(cmd.c_str(), "M%d", &manualMood);
      manualMoodMs = now;
    } else if (cmd.startsWith("R")) {
      strncpy(globalReminderText, cmd.c_str() + 1, sizeof(globalReminderText) - 1);
      globalReminderText[sizeof(globalReminderText) - 1] = '\0';
    } else if (cmd == "G") {
      currentState = STATE_GAMES;
      initPingpong();
      Serial.println("cmd: Start Pingpong");
    } else if (cmd == "A") {
      // Ignore attack in pingpong (handled by MPU tilt)
      Serial.println("cmd: Player Attack ignored in Pingpong");
    } else if (cmd == "P") {
      if (currentState == STATE_FACE) {
        currentState = STATE_MENU;
        menuCursor = 0;
      } else if (currentState == STATE_MENU) {
        menuCursor = (menuCursor + 1) % 6;
      } else if (currentState == STATE_MUSIC) {
        musicCursor = (musicCursor + 1) % 4;
      } else if (currentState == STATE_GAMES) {
        if (gamePhase == GAME_IDLE || gamePhase == GAME_OVER) {
          initPingpong(true);
        } else if (gamePhase == GAME_PLAYING && ballX < 64.0f) {
          ballVY += (float)random(-10, 11) * 0.1f;
        }
      } else {
        currentState = STATE_MENU;
        menuCursor = 0;
      }
      Serial.println("cmd: Tap Owi -> Menu/Next");
    } else if (cmd == "O") {
      // Simulate Hold
      if (currentState == STATE_MENU) {
        if (menuCursor == 0) currentState = STATE_GAMES;
        else if (menuCursor == 1) currentState = STATE_SENSOR;
        else if (menuCursor == 2) currentState = STATE_REMINDER;
        else if (menuCursor == 3) {
          currentState = STATE_MUSIC;
          musicCursor = 0;
        }
        else if (menuCursor == 4) enterDrawMode(true);
        else if (menuCursor == 5) currentState = STATE_FACE;
        if (currentState == STATE_GAMES) gamePhase = GAME_IDLE;
      } else if (currentState == STATE_MUSIC) {
        if (musicCursor == 0) {
          currentState = STATE_FACE;
          req_lovestory = true;
          req_song = 1;
        } else if (musicCursor == 1) {
          currentState = STATE_FACE;
          req_song = 2;
        } else if (musicCursor == 2) {
          currentState = STATE_FACE;
          req_song = 3;
        } else {
          currentState = STATE_MENU;
          menuCursor = 3;
        }
      } else if (currentState == STATE_GAMES) {
        currentState = STATE_FACE;
        gamePhase = GAME_IDLE;
      } else if (currentState == STATE_DRAW) {
        currentState = STATE_FACE;
      } else if (currentState == STATE_FACE) {
        touchSleepyUntilMs = now + 2600UL;
      }
      Serial.println("cmd: Hold Owi -> OK");
    } else if (cmd == "D") {
      laughUntilMs = now + 2500UL;
      currentState = STATE_FACE;
      Serial.println("cmd: Double Click -> Laugh");
    } else if (cmd == "E") {
      touchLoveUntilMs = now + 2000UL;
      Serial.println("cmd: Pet Owi -> Love");
    } else if (cmd == "F") {
      glitchUntilMs = now + 1500UL;
      Serial.println("cmd: Flip Face -> Glitch");
    } else if (cmd == "C") {
      currentState = STATE_FACE;
      gamePhase = GAME_IDLE;
      Serial.println("cmd: Back to Face");
    } else if (cmd.startsWith("K")) {
      // Web paddle: K-100..K100
      int val = cmd.substring(1).toInt();
      manualJoyY = constrain(val, -100, 100);
      lastJoyMs = now;
    } else if (cmd.startsWith("T:")) {
      strncpy(chatText, cmd.c_str() + 2, sizeof(chatText) - 1);
      chatText[sizeof(chatText) - 1] = '\0';
      currentState = STATE_CHAT;
      chatStartMs = now;
      chatDisplayUntilMs = now + max(6000, (int)strlen(chatText) * 120 + 2500);
      Serial.print("cmd: Chat -> ");
      Serial.println(chatText);
    }
  }
}

void loop() {
  handleSerial();
  updateTouch();
  updateMPU();
  updateDHT();
  updateGame();
  sendTelemetry();
  acceptClient();
  readClientToRing();
  writeAudioBlock();
  readClientToRing();

  unsigned long now = millis();
  unsigned long drawInterval = (currentState == STATE_GAMES) ? 25UL : (currentState == STATE_FACE ? 40UL : (currentState == STATE_CHAT ? 35UL : 60UL));
  if (playing != wasPlaying || now - lastDrawMs > drawInterval) {
    wasPlaying = playing;
    lastDrawMs = now;
    if (currentState == STATE_FACE) {
      drawMochi(playing);
    } else {
      drawUI();
    }
  }
}
