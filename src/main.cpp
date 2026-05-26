#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_MPU6050.h>
#include <DHT.h>
#include "secrets.h"

#define SSD1306_WHITE SH110X_WHITE
#define SSD1306_BLACK SH110X_BLACK

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
#define DHT_PIN D2
#define DHT_TYPE DHT22
#define TOUCH_PIN D3
#define TOUCH_ACTIVE_HIGH 1
#define FACE_SENSOR_REACTIONS 1
#define SENSOR_TEST_MODE 0
#define I2C_SCAN_TEST 1
#define LOVE_ME_NOT_MODE 0
#define LOVE_STORY_MODE 0
#define SONG_CAPTIONS 0
#define ROYAL_CROWN 0
#define GEMINI_AI_MODE 1
#define GEMINI_AUTO_MODE 0
#define GEMINI_MODEL "gemini-2.5-flash"
#define GEMINI_INTERVAL_MS 45000UL
#define WIFI_CONNECT_TIMEOUT_MS 12000UL
#define CUTE_COMPANION_MODE 1
#define WEB_UPLOAD_MODE 0
#define SERIAL_FRAME_MODE 1
#define WEB_AP_MODE 1
#define WEB_AP_SSID "Capstone-OLED"
#define WEB_AP_PASSWORD "12345678"
#define WEB_FRAME_TIMEOUT_MS 12000UL

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_MPU6050 mpu;
DHT dht(DHT_PIN, DHT_TYPE);
WebServer webServer(80);

const float PI_F = 3.14159265f;
const float TWO_PI_F = PI_F * 2.0f;
const uint16_t FRAME_MS = 25;
const uint16_t MPU_READ_MS = 20;
const uint16_t DHT_READ_MS = 2000;
const float TEMP_HOT_C = 30.0f;
const float TEMP_COLD_C = 22.0f;
const float HUMIDITY_HOT_PCT = 72.0f;
const unsigned long DEFAULT_REMINDER_INTERVAL_MS = 60000UL;
const unsigned long REMINDER_VISIBLE_MS = 12000UL;
const unsigned long REMINDER_RETRY_MS = 30000UL;
const unsigned long MENU_TIMEOUT_MS = 15000UL;
const uint16_t TOUCH_DEBOUNCE_MS = 35;
const uint16_t TOUCH_LONG_MS = 720;
const uint16_t TOUCH_DOUBLE_TAP_MS = 330;
const uint8_t REMINDER_TEXT_MAX = 32;
const long GMT_OFFSET_SEC = 7L * 3600L;
const int DAYLIGHT_OFFSET_SEC = 0;

enum Mood : uint8_t {
  MOOD_IDLE = 0,
  MOOD_HAPPY,
  MOOD_PLEASED,
  MOOD_GIGGLE,
  MOOD_SHY,
  MOOD_CURIOUS,
  MOOD_FOCUS,
  MOOD_WINK,
  MOOD_SLEEPY,
  MOOD_YAWN,
  MOOD_SURPRISED,
  MOOD_WORRIED,
  MOOD_SAD,
  MOOD_NERVOUS,
  MOOD_PROUD,
  MOOD_EXCITED,
  MOOD_RELIEVED,
  MOOD_ANNOYED,
  MOOD_BORED,
  MOOD_DIZZY,
  MOOD_LOVE,
  MOOD_SMIRK,
  MOOD_SKEPTICAL,
  MOOD_LAUGH,
  MOOD_CRY,
  MOOD_DANCE,
  MOOD_DREAMY,
  MOOD_DETERMINED,
  MOOD_PANIC,
  MOOD_COOL,
  MOOD_GLANCE_LEFT,
  MOOD_GLANCE_RIGHT,
  MOOD_PEEK_UP,
  MOOD_PEEK_DOWN,
  MOOD_SOFT_BLINK,
  MOOD_BREATHE,
  MOOD_DOUBLE_BLINK,
  MOOD_LOOK_AROUND,
  MOOD_TINY_NOD,
  MOOD_SIDE_EYE,
  MOOD_SOFT_SMILE,
  MOOD_HEART_POP,
  MOOD_HOT,
  MOOD_COLD,
  MOOD_REMINDER
};

enum ScreenPage : uint8_t {
  SCREEN_FACE = 0,
  SCREEN_TEMP,
  SCREEN_MINIGAME,
  SCREEN_MENU
};

enum MenuPage : uint8_t {
  MENU_TEMP_CHECK = 0,
  MENU_MINIGAME,
  MENU_REMINDER,
  MENU_HOT_LIMIT,
  MENU_COLD_LIMIT,
  MENU_REMINDER_INTERVAL
};

enum TouchEvent : uint8_t {
  TOUCH_NONE = 0,
  TOUCH_TAP,
  TOUCH_DOUBLE_TAP,
  TOUCH_STROKE,
  TOUCH_LONG
};

enum MiniGameMode : uint8_t {
  GAME_SELECT = 0,
  GAME_PINGPONG,
  GAME_SPACE
};

struct SensorState {
  float tiltX;
  float tiltY;
  float motion;
  float tempC;
  float humidity;
  bool mpuReady;
  bool dhtValid;
  unsigned long lastMpuRead;
  unsigned long lastDhtRead;
  unsigned long lastShakeMs;
};

struct FacePose {
  float eyeLX;
  float eyeLY;
  float eyeRX;
  float eyeRY;
  float eyeW;
  float eyeLH;
  float eyeRH;
  float blinkL;
  float blinkR;
  float mouthX;
  float mouthY;
  float mouthW;
  float mouthDepth;
  float mouthOpen;
  float brow;
  float blush;
  float sweat;
  float shiver;
  float question;
  float zzz;
  float sparkle;
  float steam;
  float tear;
  float stress;
  float proud;
  float dizzy;
  float hearts;
  float cool;
};

struct ReminderState {
  bool active;
  unsigned long nextAtMs;
  unsigned long activeUntilMs;
  unsigned long lastTriggeredMs;
  unsigned long lastAckMs;
  uint8_t count;
};

struct TouchState {
  bool rawTouched;
  bool stableTouched;
  bool longSent;
  bool tapWaiting;
  unsigned long rawChangedMs;
  unsigned long pressedAtMs;
  unsigned long pendingTapMs;
  unsigned long strokeWindowMs;
  TouchEvent pendingEvent;
  uint8_t strokeCount;
};

SensorState sensors = {
  0.0f, 0.0f, 0.0f, 26.0f, 55.0f,
  false, false, 0UL, 0UL, 0UL
};

ReminderState reminder = {
  false, 0UL, 0UL, 0UL, 0UL, 0
};

TouchState touch = {
  false, false, false, false, 0UL, 0UL, 0UL, 0UL, TOUCH_NONE, 0
};

FacePose currentPose;
bool poseReady = false;

Mood activeMood = MOOD_IDLE;
ScreenPage activeScreen = SCREEN_FACE;
MenuPage activeMenu = MENU_HOT_LIMIT;
unsigned long moodUntilMs = 0UL;
unsigned long menuUntilMs = 0UL;
unsigned long nextIdleEventMs = 0UL;
unsigned long lastHotMs = 0UL;
unsigned long lastColdMs = 0UL;
unsigned long lastMotionMs = 0UL;
unsigned long lastTiltMs = 0UL;
unsigned long lastDemoMs = 0UL;
unsigned long lastGeminiMs = 0UL;
unsigned long nextWifiTryMs = 0UL;
unsigned long pendingAngryAtMs = 0UL;
uint8_t demoStep = 0;
uint8_t touchMoodStep = 0;
float hotThresholdC = TEMP_HOT_C;
float coldThresholdC = TEMP_COLD_C;
bool reminderEnabled = true;
bool reminderClockMode = false;
bool timeConfigured = false;
uint8_t reminderIntervalIndex = 0;
uint8_t reminderHour = 7;
uint8_t reminderMinute = 30;
int reminderLastYday = -1;
bool wifiStarted = false;
bool geminiActive = false;
String geminiStatus = "AI OFF";
bool webIpShown = false;
bool mpuAdafruitReady = false;
uint8_t mpuI2cAddress = 0x68;
uint8_t foundI2cAddrs[8];
uint8_t foundI2cCount = 0;
unsigned long lastI2cScanMs = 0UL;
uint8_t webFrame[SCREEN_WIDTH * SCREEN_HEIGHT / 8];
bool webFrameActive = false;
unsigned long lastWebFrameMs = 0UL;
uint16_t serialFrameIndex = 0;
bool serialFrameReceiving = false;
unsigned long serialFrameStartedMs = 0UL;
bool serialHexReceiving = false;
uint16_t serialHexIndex = 0;
int8_t serialHexHighNibble = -1;
bool serialReminderReceiving = false;
char serialReminderBuffer[REMINDER_TEXT_MAX + 1];
uint8_t serialReminderIndex = 0;
char reminderText[REMINDER_TEXT_MAX + 1] = "enroll lagi ya deck";
String webCommand = "";
unsigned long webCommandUntilMs = 0UL;
const uint8_t PING_WIN_SCORE = 5;
MiniGameMode selectedGame = GAME_PINGPONG;
MiniGameMode runningGame = GAME_SELECT;
unsigned long gameLastMs = 0UL;
int pingPaddleY = 25;
int pingBotPaddleY = 25;
int pingBallX = 70;
int pingBallY = 30;
int pingVelX = 2;
int pingVelY = 1;
uint8_t pingPlayerScore = 0;
uint8_t pingBotScore = 0;
bool pingGameOver = false;
bool pingPlayerWon = false;
int shipY = 31;
int bulletX = -1;
int bulletY = 0;
int alienX = 116;
int alienY = 18;
int alienVelY = 1;
uint8_t spaceScore = 0;

const uint8_t REMINDER_INTERVAL_MINUTES[] = {1, 5, 15, 30, 60};

float clampFloat(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

float clamp01(float value) {
  return clampFloat(value, 0.0f, 1.0f);
}

float lerpFloat(float from, float to, float amount) {
  return from + (to - from) * amount;
}

float wave(unsigned long ms, float periodMs) {
  return sinf((float)ms * TWO_PI_F / periodMs);
}

float pulse(unsigned long ms, unsigned long period, unsigned long start, unsigned long duration) {
  unsigned long phase = ms % period;
  if (phase < start || phase > start + duration) return 0.0f;

  float t = (float)(phase - start) / (float)duration;
  return sinf(t * PI_F);
}

float smooth01(float value) {
  float t = clamp01(value);
  return t * t * (3.0f - 2.0f * t);
}

int hexNibble(uint8_t b) {
  if (b >= '0' && b <= '9') return b - '0';
  if (b >= 'a' && b <= 'f') return b - 'a' + 10;
  if (b >= 'A' && b <= 'F') return b - 'A' + 10;
  return -1;
}

bool isHotCondition();
bool isColdCondition();
void showMenu(MenuPage page, unsigned long now);
void showTempScreen(unsigned long now);
void showMinigameScreen(unsigned long now);
void showFaceScreen();
void toggleReminder(unsigned long now);
void setReminderText(const char* text);
void setReminderSchedule(const char* payload, unsigned long now);
void startSelectedGame(unsigned long now);

bool secretsConfigured() {
  return strcmp(WIFI_SSID, "NAMA_WIFI_KAMU") != 0 &&
         strcmp(WIFI_PASSWORD, "PASSWORD_WIFI_KAMU") != 0 &&
         strcmp(GEMINI_API_KEY, "API_KEY_GEMINI_KAMU") != 0;
}

bool wifiConfigured() {
  return strcmp(WIFI_SSID, "NAMA_WIFI_KAMU") != 0 &&
         strcmp(WIFI_PASSWORD, "PASSWORD_WIFI_KAMU") != 0;
}

const char* moodName(Mood mood) {
  switch (mood) {
    case MOOD_HAPPY: return "HAPPY";
    case MOOD_PLEASED: return "PLEASED";
    case MOOD_GIGGLE: return "GIGGLE";
    case MOOD_SHY: return "SHY";
    case MOOD_CURIOUS: return "CURIOUS";
    case MOOD_FOCUS: return "FOCUS";
    case MOOD_WINK: return "WINK";
    case MOOD_SLEEPY: return "SLEEPY";
    case MOOD_YAWN: return "YAWN";
    case MOOD_SURPRISED: return "SURPRISED";
    case MOOD_WORRIED: return "WORRIED";
    case MOOD_SAD: return "SAD";
    case MOOD_NERVOUS: return "NERVOUS";
    case MOOD_PROUD: return "PROUD";
    case MOOD_EXCITED: return "EXCITED";
    case MOOD_RELIEVED: return "RELIEVED";
    case MOOD_ANNOYED: return "ANNOYED";
    case MOOD_BORED: return "BORED";
    case MOOD_DIZZY: return "DIZZY";
    case MOOD_LOVE: return "LOVE";
    case MOOD_SMIRK: return "SMIRK";
    case MOOD_SKEPTICAL: return "SKEPTICAL";
    case MOOD_LAUGH: return "LAUGH";
    case MOOD_CRY: return "CRY";
    case MOOD_DANCE: return "DANCE";
    case MOOD_DREAMY: return "DREAMY";
    case MOOD_DETERMINED: return "DETERMINED";
    case MOOD_PANIC: return "PANIC";
    case MOOD_COOL: return "COOL";
    case MOOD_HOT: return "HOT";
    case MOOD_COLD: return "COLD";
    default: return "IDLE";
  }
}

Mood moodFromGeminiText(String text) {
  text.toUpperCase();

  if (text.indexOf("HAPPY") >= 0) return MOOD_HAPPY;
  if (text.indexOf("PLEASED") >= 0) return MOOD_PLEASED;
  if (text.indexOf("GIGGLE") >= 0) return MOOD_GIGGLE;
  if (text.indexOf("SHY") >= 0) return MOOD_SHY;
  if (text.indexOf("CURIOUS") >= 0) return MOOD_CURIOUS;
  if (text.indexOf("FOCUS") >= 0) return MOOD_FOCUS;
  if (text.indexOf("WINK") >= 0) return MOOD_WINK;
  if (text.indexOf("SLEEPY") >= 0) return MOOD_SLEEPY;
  if (text.indexOf("YAWN") >= 0) return MOOD_YAWN;
  if (text.indexOf("SURPRISED") >= 0) return MOOD_SURPRISED;
  if (text.indexOf("WORRIED") >= 0) return MOOD_WORRIED;
  if (text.indexOf("SAD") >= 0) return MOOD_SAD;
  if (text.indexOf("NERVOUS") >= 0) return MOOD_NERVOUS;
  if (text.indexOf("PROUD") >= 0) return MOOD_PROUD;
  if (text.indexOf("EXCITED") >= 0) return MOOD_EXCITED;
  if (text.indexOf("RELIEVED") >= 0) return MOOD_RELIEVED;
  if (text.indexOf("ANNOYED") >= 0) return MOOD_ANNOYED;
  if (text.indexOf("BORED") >= 0) return MOOD_BORED;
  if (text.indexOf("DIZZY") >= 0) return MOOD_DIZZY;
  if (text.indexOf("LOVE") >= 0) return MOOD_LOVE;
  if (text.indexOf("SMIRK") >= 0) return MOOD_SMIRK;
  if (text.indexOf("SKEPTICAL") >= 0) return MOOD_SKEPTICAL;
  if (text.indexOf("LAUGH") >= 0) return MOOD_LAUGH;
  if (text.indexOf("CRY") >= 0) return MOOD_CRY;
  if (text.indexOf("DANCE") >= 0) return MOOD_DANCE;
  if (text.indexOf("DREAMY") >= 0) return MOOD_DREAMY;
  if (text.indexOf("DETERMINED") >= 0) return MOOD_DETERMINED;
  if (text.indexOf("PANIC") >= 0) return MOOD_PANIC;
  if (text.indexOf("COOL") >= 0) return MOOD_COOL;
  if (text.indexOf("HOT") >= 0) return MOOD_HOT;
  if (text.indexOf("COLD") >= 0) return MOOD_COLD;

  return MOOD_IDLE;
}

uint16_t moodDuration(Mood mood) {
  switch (mood) {
    case MOOD_SURPRISED: return 900;
    case MOOD_WINK: return 1800;
    case MOOD_GIGGLE: return 2400;
    case MOOD_EXCITED: return 2600;
    case MOOD_CURIOUS: return 3200;
    case MOOD_SHY: return 2800;
    case MOOD_PROUD: return 3000;
    case MOOD_RELIEVED: return 2600;
    case MOOD_SLEEPY: return 3600;
    case MOOD_YAWN: return 2600;
    case MOOD_WORRIED: return 3000;
    case MOOD_NERVOUS: return 2800;
    case MOOD_SAD: return 3200;
    case MOOD_ANNOYED: return 2600;
    case MOOD_BORED: return 3200;
    case MOOD_DIZZY: return 2600;
    case MOOD_LOVE: return 3200;
    case MOOD_SMIRK: return 2600;
    case MOOD_SKEPTICAL: return 2800;
    case MOOD_LAUGH: return 3000;
    case MOOD_CRY: return 3200;
    case MOOD_DANCE: return 3600;
    case MOOD_DREAMY: return 3400;
    case MOOD_DETERMINED: return 3000;
    case MOOD_PANIC: return 2400;
    case MOOD_COOL: return 3000;
    case MOOD_GLANCE_LEFT: return 1500;
    case MOOD_GLANCE_RIGHT: return 1500;
    case MOOD_PEEK_UP: return 1600;
    case MOOD_PEEK_DOWN: return 1600;
    case MOOD_SOFT_BLINK: return 1200;
    case MOOD_BREATHE: return 2200;
    case MOOD_DOUBLE_BLINK: return 1500;
    case MOOD_LOOK_AROUND: return 2200;
    case MOOD_TINY_NOD: return 1900;
    case MOOD_SIDE_EYE: return 1700;
    case MOOD_SOFT_SMILE: return 2300;
    case MOOD_HEART_POP: return 1900;
    case MOOD_HOT: return 4200;
    case MOOD_COLD: return 4200;
    case MOOD_REMINDER: return REMINDER_VISIBLE_MS;
    case MOOD_HAPPY: return 3000;
    case MOOD_PLEASED: return 3000;
    case MOOD_FOCUS: return 3200;
    case MOOD_IDLE:
    default: return 2400;
  }
}

void activateMood(Mood mood, unsigned long now, uint16_t duration = 0) {
  activeMood = mood;
  moodUntilMs = now + (duration > 0 ? duration : moodDuration(mood));
}

Mood nextDemoMood() {
  static const Mood sequence[] = {
    MOOD_PLEASED, MOOD_HAPPY, MOOD_GIGGLE, MOOD_SHY,
    MOOD_CURIOUS, MOOD_FOCUS, MOOD_WINK, MOOD_PROUD,
    MOOD_EXCITED, MOOD_RELIEVED, MOOD_SLEEPY, MOOD_YAWN,
    MOOD_WORRIED, MOOD_NERVOUS, MOOD_SAD, MOOD_ANNOYED,
    MOOD_BORED, MOOD_DIZZY, MOOD_LOVE, MOOD_SMIRK,
    MOOD_SKEPTICAL, MOOD_LAUGH, MOOD_CRY, MOOD_DANCE,
    MOOD_DREAMY, MOOD_DETERMINED, MOOD_PANIC, MOOD_COOL,
    MOOD_GLANCE_LEFT, MOOD_GLANCE_RIGHT, MOOD_PEEK_UP,
    MOOD_PEEK_DOWN, MOOD_SOFT_BLINK, MOOD_BREATHE,
    MOOD_DOUBLE_BLINK, MOOD_LOOK_AROUND, MOOD_TINY_NOD,
    MOOD_SIDE_EYE, MOOD_SOFT_SMILE, MOOD_HEART_POP,
    MOOD_HOT, MOOD_COLD
  };

  Mood mood = sequence[demoStep % (sizeof(sequence) / sizeof(sequence[0]))];
  demoStep++;
  return mood;
}

Mood randomIdleMood() {
  static const Mood idlePool[] = {
    MOOD_IDLE, MOOD_BREATHE, MOOD_SOFT_BLINK,
    MOOD_GLANCE_LEFT, MOOD_GLANCE_RIGHT, MOOD_PEEK_UP, MOOD_PEEK_DOWN,
    MOOD_DOUBLE_BLINK, MOOD_LOOK_AROUND, MOOD_TINY_NOD,
    MOOD_SIDE_EYE, MOOD_SOFT_SMILE, MOOD_HEART_POP,
    MOOD_BREATHE, MOOD_SOFT_BLINK, MOOD_LOOK_AROUND,
    MOOD_HEART_POP, MOOD_SOFT_SMILE, MOOD_LOVE, MOOD_SHY,
    MOOD_PLEASED, MOOD_HAPPY, MOOD_GIGGLE, MOOD_SHY,
    MOOD_CURIOUS, MOOD_FOCUS, MOOD_WINK, MOOD_PROUD,
    MOOD_EXCITED, MOOD_RELIEVED, MOOD_SLEEPY, MOOD_YAWN,
    MOOD_WORRIED, MOOD_NERVOUS, MOOD_SAD, MOOD_ANNOYED,
    MOOD_BORED, MOOD_DIZZY, MOOD_LOVE, MOOD_SMIRK,
    MOOD_SKEPTICAL, MOOD_LAUGH, MOOD_CRY, MOOD_DANCE,
    MOOD_DREAMY, MOOD_DETERMINED, MOOD_PANIC, MOOD_COOL,
    MOOD_HOT, MOOD_COLD
  };

  return idlePool[random(0, sizeof(idlePool) / sizeof(idlePool[0]))];
}

bool writeMpuRegister(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readMpuBytes(uint8_t addr, uint8_t reg, uint8_t* buffer, uint8_t length) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;

  uint8_t received = Wire.requestFrom(addr, length);
  if (received != length) return false;

  for (uint8_t i = 0; i < length; i++) {
    buffer[i] = Wire.read();
  }
  return true;
}

int16_t readInt16BE(const uint8_t* data, uint8_t index) {
  return (int16_t)((data[index] << 8) | data[index + 1]);
}

bool initRawMpu(uint8_t addr) {
  uint8_t who = 0;
  if (!readMpuBytes(addr, 0x75, &who, 1)) return false;
  if (who != 0x68 && who != 0x70 && who != 0x71 && who != 0x73) return false;

  bool ok = true;
  ok &= writeMpuRegister(addr, 0x6B, 0x00); // PWR_MGMT_1: wake
  delay(20);
  ok &= writeMpuRegister(addr, 0x1A, 0x03); // CONFIG: light low-pass
  ok &= writeMpuRegister(addr, 0x1B, 0x08); // GYRO_CONFIG: +/-500 dps
  ok &= writeMpuRegister(addr, 0x1C, 0x08); // ACCEL_CONFIG: +/-4g
  if (ok) mpuI2cAddress = addr;
  return ok;
}

void updateMpu(unsigned long now) {
  if (!sensors.mpuReady || now - sensors.lastMpuRead < MPU_READ_MS) return;

  float ax;
  float ay;
  float az;
  float gyroMag;

  if (mpuAdafruitReady) {
    sensors_event_t accel;
    sensors_event_t gyro;
    sensors_event_t temp;
    mpu.getEvent(&accel, &gyro, &temp);

    ax = accel.acceleration.x;
    ay = accel.acceleration.y;
    az = accel.acceleration.z;
    gyroMag = sqrtf(gyro.gyro.x * gyro.gyro.x +
                    gyro.gyro.y * gyro.gyro.y +
                    gyro.gyro.z * gyro.gyro.z);
  } else {
    uint8_t data[14];
    if (!readMpuBytes(mpuI2cAddress, 0x3B, data, sizeof(data))) {
      sensors.mpuReady = false;
      return;
    }

    int16_t rawAx = readInt16BE(data, 0);
    int16_t rawAy = readInt16BE(data, 2);
    int16_t rawAz = readInt16BE(data, 4);
    int16_t rawGx = readInt16BE(data, 8);
    int16_t rawGy = readInt16BE(data, 10);
    int16_t rawGz = readInt16BE(data, 12);

    ax = ((float)rawAx / 8192.0f) * 9.80665f;
    ay = ((float)rawAy / 8192.0f) * 9.80665f;
    az = ((float)rawAz / 8192.0f) * 9.80665f;
    float gx = ((float)rawGx / 65.5f) * PI_F / 180.0f;
    float gy = ((float)rawGy / 65.5f) * PI_F / 180.0f;
    float gz = ((float)rawGz / 65.5f) * PI_F / 180.0f;
    gyroMag = sqrtf(gx * gx + gy * gy + gz * gz);
  }

  float accelMag = sqrtf(ax * ax + ay * ay + az * az);

  float targetTiltX = clampFloat(ay / 9.8f, -1.0f, 1.0f);
  float targetTiltY = clampFloat(ax / 9.8f, -0.9f, 0.9f);
  float instantMotion = gyroMag * 1.35f + fabsf(accelMag - 9.8f) * 0.42f +
                        fabsf(targetTiltX - sensors.tiltX) * 2.4f +
                        fabsf(targetTiltY - sensors.tiltY) * 2.0f;

  sensors.tiltX = lerpFloat(sensors.tiltX, targetTiltX, 0.26f);
  sensors.tiltY = lerpFloat(sensors.tiltY, targetTiltY, 0.24f);
  sensors.motion = lerpFloat(sensors.motion, instantMotion, 0.34f);
  sensors.lastMpuRead = now;

  if (sensors.motion > 2.55f || fabsf(targetTiltX - sensors.tiltX) > 0.20f) {
    sensors.lastShakeMs = now;
  }
}

void updateDht(unsigned long now) {
  if (now - sensors.lastDhtRead < DHT_READ_MS) return;

  float humidity = dht.readHumidity();
  float tempC = dht.readTemperature();
  sensors.lastDhtRead = now;

  if (isnan(humidity) || isnan(tempC)) return;

  if (!sensors.dhtValid) {
    sensors.tempC = tempC;
    sensors.humidity = humidity;
  } else {
    sensors.tempC = lerpFloat(sensors.tempC, tempC, 0.25f);
    sensors.humidity = lerpFloat(sensors.humidity, humidity, 0.25f);
  }
  sensors.dhtValid = true;
}

unsigned long reminderIntervalMs() {
  uint8_t count = sizeof(REMINDER_INTERVAL_MINUTES) / sizeof(REMINDER_INTERVAL_MINUTES[0]);
  if (reminderIntervalIndex >= count) reminderIntervalIndex = 0;
  return (unsigned long)REMINDER_INTERVAL_MINUTES[reminderIntervalIndex] * 60000UL;
}

void scheduleReminder(unsigned long now, unsigned long delayMs) {
  reminder.active = false;
  reminder.activeUntilMs = 0UL;
  reminder.nextAtMs = now + delayMs;
}

void acknowledgeReminder(unsigned long now) {
  reminder.lastAckMs = now;
  if (reminderClockMode) {
    reminder.active = false;
    reminder.activeUntilMs = 0UL;
  } else {
    scheduleReminder(now, reminderIntervalMs());
  }
}

void updateReminder(unsigned long now) {
  if (!reminderEnabled) {
    reminder.active = false;
    reminder.nextAtMs = 0UL;
    return;
  }

  if (reminderClockMode) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 20)) {
      return;
    } else {
      if (!reminder.active &&
          timeinfo.tm_hour == reminderHour &&
          timeinfo.tm_min == reminderMinute &&
          timeinfo.tm_yday != reminderLastYday) {
        reminder.active = true;
        reminder.lastTriggeredMs = now;
        reminder.activeUntilMs = now + REMINDER_VISIBLE_MS;
        reminder.count++;
        reminderLastYday = timeinfo.tm_yday;
      }

      if (reminder.active) {
        bool acknowledgedByShake = sensors.lastShakeMs > reminder.lastTriggeredMs &&
                                   now - sensors.lastShakeMs < 320UL;
        if (acknowledgedByShake) {
          acknowledgeReminder(now);
          return;
        }
        if (now >= reminder.activeUntilMs) {
          reminder.active = false;
          reminder.activeUntilMs = 0UL;
        }
      }
      return;
    }
  }

  if (reminder.nextAtMs == 0UL) {
    reminder.nextAtMs = now + reminderIntervalMs();
  }

  if (reminder.active) {
    bool acknowledgedByShake = sensors.lastShakeMs > reminder.lastTriggeredMs &&
                               now - sensors.lastShakeMs < 320UL;

    if (acknowledgedByShake) {
      acknowledgeReminder(now);
      return;
    }

    if (now >= reminder.activeUntilMs) {
      scheduleReminder(now, REMINDER_RETRY_MS);
    }
    return;
  }

  if (now >= reminder.nextAtMs) {
    reminder.active = true;
    reminder.lastTriggeredMs = now;
    reminder.activeUntilMs = now + REMINDER_VISIBLE_MS;
    reminder.count++;
  }
}

void updateSensors(unsigned long now) {
  updateMpu(now);
  updateDht(now);
  updateReminder(now);
}

void updateWifi(unsigned long now) {
#if GEMINI_AI_MODE || WEB_UPLOAD_MODE
#if WEB_UPLOAD_MODE && WEB_AP_MODE
  if (!wifiStarted) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WEB_AP_SSID, WEB_AP_PASSWORD);
    wifiStarted = true;
    webIpShown = true;
    Serial.print("WEB URL: http://");
    Serial.println(WiFi.softAPIP());
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 6);
    display.print("WEB READY");
    display.setCursor(0, 22);
    display.print(WEB_AP_SSID);
    display.setCursor(0, 34);
    display.print("PASS ");
    display.print(WEB_AP_PASSWORD);
    display.setCursor(0, 50);
    display.print("http://192.168.4.1");
    display.display();
    delay(2200);
  }
  geminiActive = false;
  return;
#endif

  if (!wifiConfigured()) {
    geminiStatus = "WIFI SETUP";
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    geminiActive = true;
    if (!timeConfigured) {
      configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
      timeConfigured = true;
      Serial.println("NTP CONFIGURED GMT+7");
    }
    if (!webIpShown) {
      webIpShown = true;
      Serial.print("WEB URL: http://");
      Serial.println(WiFi.localIP());
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 12);
      display.print("WEB READY");
      display.setCursor(0, 28);
      display.print("http://");
      display.print(WiFi.localIP());
      display.display();
      delay(1500);
    }
    return;
  }

  geminiActive = false;
#if GEMINI_AI_MODE
  if (!secretsConfigured()) {
    geminiStatus = "AI SETUP";
  }
#endif
  if (now < nextWifiTryMs) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiStarted = true;
  nextWifiTryMs = now + WIFI_CONNECT_TIMEOUT_MS;
  geminiStatus = "WIFI...";
#else
  (void)now;
#endif
}

#if WEB_UPLOAD_MODE
const char WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>OLED Upload</title>
  <style>
    body{font-family:system-ui,Arial;margin:18px;background:#111;color:#eee}
    button,input{font:inherit;margin:8px 0;padding:10px;border-radius:8px;border:1px solid #555;background:#222;color:#eee}
    canvas{width:256px;height:128px;image-rendering:pixelated;border:1px solid #666;background:#000;display:block;margin:12px 0}
    .row{display:flex;gap:8px;flex-wrap:wrap}.muted{color:#aaa;font-size:13px}
  </style>
</head>
<body>
  <h2>OLED 128x64 Upload</h2>
  <input id="file" type="file" accept="image/*,video/*">
  <canvas id="c" width="128" height="64"></canvas>
  <div class="row">
    <button id="send">Kirim Frame</button>
    <button id="play">Play / Stream</button>
    <button id="stop">Stop</button>
    <button id="clear">Balik Wajah</button>
  </div>
  <p id="status" class="muted">Pilih foto, GIF, atau video.</p>
  <video id="v" muted playsinline loop style="display:none"></video>
  <img id="img" style="display:none">
<script>
const c=document.getElementById('c'),ctx=c.getContext('2d',{willReadFrequently:true});
const file=document.getElementById('file'),img=document.getElementById('img'),v=document.getElementById('v'),st=document.getElementById('status');
let source=null,timer=null;
function fitDraw(el){
  ctx.fillStyle='#000';ctx.fillRect(0,0,128,64);
  const sw=el.videoWidth||el.naturalWidth, sh=el.videoHeight||el.naturalHeight;
  if(!sw||!sh)return;
  const scale=Math.min(128/sw,64/sh), w=sw*scale,h=sh*scale;
  ctx.drawImage(el,(128-w)/2,(64-h)/2,w,h);
}
function bytesToHex(bytes){let s='';for(const b of bytes)s+=b.toString(16).padStart(2,'0');return s;}
function makeFrame(){
  const data=ctx.getImageData(0,0,128,64).data, out=new Uint8Array(1024);
  for(let y=0;y<64;y++)for(let x=0;x<128;x++){
    const i=(y*128+x)*4, r=data[i],g=data[i+1],b=data[i+2];
    const lum=(r*30+g*59+b*11)/100;
    if(lum>115) out[y*16+(x>>3)]|=128>>(x&7);
  }
  return out;
}
async function sendFrame(){
  if(source) fitDraw(source);
  const res=await fetch('/frame',{method:'POST',headers:{'Content-Type':'text/plain'},body:bytesToHex(makeFrame())});
  st.textContent=await res.text();
}
file.onchange=()=>{
  clearInterval(timer);timer=null;
  const f=file.files[0]; if(!f)return;
  const url=URL.createObjectURL(f);
  if(f.type.startsWith('video/')){
    source=v; v.src=url; v.play(); v.onloadeddata=()=>{fitDraw(v);sendFrame();};
  }else{
    source=img; img.src=url; img.onload=()=>{fitDraw(img);sendFrame();};
  }
};
document.getElementById('send').onclick=sendFrame;
document.getElementById('play').onclick=()=>{ if(!source)return; clearInterval(timer); timer=setInterval(sendFrame,180); };
document.getElementById('stop').onclick=()=>{ clearInterval(timer); timer=null; };
document.getElementById('clear').onclick=async()=>{ clearInterval(timer); timer=null; const r=await fetch('/clear',{method:'POST'}); st.textContent=await r.text(); };
</script>
</body>
</html>
)HTML";

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void handleWebRoot() {
  webServer.send_P(200, "text/html", WEB_PAGE);
}

void handleWebFrame() {
  String hex = webServer.arg("plain");
  hex.trim();
  if (hex.length() != sizeof(webFrame) * 2) {
    webServer.send(400, "text/plain", "Frame harus 1024 byte / 2048 hex");
    return;
  }

  for (size_t i = 0; i < sizeof(webFrame); i++) {
    int hi = hexValue(hex[i * 2]);
    int lo = hexValue(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      webServer.send(400, "text/plain", "Hex tidak valid");
      return;
    }
    webFrame[i] = (uint8_t)((hi << 4) | lo);
  }

  webFrameActive = true;
  lastWebFrameMs = millis();
  webServer.send(200, "text/plain", "Muncul di OLED");
}

void handleWebClear() {
  webFrameActive = false;
  webServer.send(200, "text/plain", "Balik ke wajah");
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, handleWebRoot);
  webServer.on("/frame", HTTP_POST, handleWebFrame);
  webServer.on("/clear", HTTP_POST, handleWebClear);
  webServer.begin();
}

void renderWebFrame(unsigned long now) {
  if (webFrameActive && now - lastWebFrameMs > WEB_FRAME_TIMEOUT_MS) {
    webFrameActive = false;
    return;
  }

  display.clearDisplay();
  display.drawBitmap(0, 0, webFrame, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
  display.display();
}
#else
void setupWebServer() {}
void renderWebFrame(unsigned long now) { (void)now; }
#endif

void updateSerialFrame(unsigned long now) {
#if SERIAL_FRAME_MODE
  if ((serialFrameReceiving || serialHexReceiving || serialReminderReceiving) && now - serialFrameStartedMs > 3500UL) {
    serialFrameReceiving = false;
    serialHexReceiving = false;
    serialReminderReceiving = false;
    serialFrameIndex = 0;
    serialHexIndex = 0;
    serialHexHighNibble = -1;
    serialReminderIndex = 0;
    Serial.println("FRAME TIMEOUT");
  }

  while (Serial.available() > 0) {
    uint8_t b = (uint8_t)Serial.read();
    if (serialReminderReceiving) {
      if (b == '\n' || b == '\r') {
        serialReminderBuffer[serialReminderIndex] = '\0';
        setReminderSchedule(serialReminderBuffer, now);
        serialReminderReceiving = false;
        Serial.print("REMINDER TEXT: ");
        Serial.println(reminderText);
        if (!reminderClockMode) {
          reminder.active = true;
          reminder.lastTriggeredMs = now;
          reminder.activeUntilMs = now + REMINDER_VISIBLE_MS;
        }
        continue;
      }
      if (serialReminderIndex < REMINDER_TEXT_MAX && b >= 32 && b <= 126) {
        serialReminderBuffer[serialReminderIndex++] = (char)b;
      }
      continue;
    }

    if (serialHexReceiving) {
      if (b == '\n' || b == '\r' || b == ' ') continue;
      int nibble = hexNibble(b);
      if (nibble < 0) {
        serialHexReceiving = false;
        serialHexIndex = 0;
        serialHexHighNibble = -1;
        Serial.println("HEX ERR");
        continue;
      }
      if (serialHexHighNibble < 0) {
        serialHexHighNibble = (int8_t)nibble;
      } else {
        webFrame[serialHexIndex++] = (uint8_t)((serialHexHighNibble << 4) | nibble);
        serialHexHighNibble = -1;
        if (serialHexIndex >= sizeof(webFrame)) {
          serialHexReceiving = false;
          webFrameActive = true;
          lastWebFrameMs = now;
          Serial.println("HEX OK");
        }
      }
      continue;
    }

    if (!serialFrameReceiving && b == 'H') {
      serialHexReceiving = true;
      serialHexIndex = 0;
      serialHexHighNibble = -1;
      serialFrameStartedMs = now;
      continue;
    }

    if (!serialFrameReceiving && b == 'S') {
      serialReminderReceiving = true;
      serialReminderIndex = 0;
      serialFrameStartedMs = now;
      continue;
    }

    if (!serialFrameReceiving && b == 0xA5) {
      serialFrameReceiving = true;
      serialFrameIndex = 0;
      serialFrameStartedMs = now;
      continue;
    }

    if (!serialFrameReceiving) {
      if (b == 'C') {
        Serial.println("CMD C clear");
        webFrameActive = false;
      } else if (b == 'M') {
        Serial.println("CMD M menu");
        showMenu(MENU_TEMP_CHECK, now);
      } else if (b == 'R') {
        Serial.println("CMD R reminder");
        toggleReminder(now);
        showMenu(MENU_REMINDER, now);
      } else if (b == 'T') {
        Serial.println("CMD T temp");
        showTempScreen(now);
      } else if (b == 'G') {
        Serial.println("CMD G game");
        webFrameActive = false;
        showMinigameScreen(now);
      } else if (b == '1') {
        Serial.println("CMD 1 pingpong");
        webFrameActive = false;
        activeScreen = SCREEN_MINIGAME;
        selectedGame = GAME_PINGPONG;
        startSelectedGame(now);
      } else if (b == '2') {
        Serial.println("CMD 2 space");
        webFrameActive = false;
        activeScreen = SCREEN_MINIGAME;
        selectedGame = GAME_SPACE;
        startSelectedGame(now);
      } else if (b == 'O') {
        Serial.println("CMD O removed -> face");
        webFrameActive = false;
        showFaceScreen();
        activateMood(MOOD_HAPPY, now, 1600);
      } else if (b == 'F') {
        Serial.println("CMD F face");
        webFrameActive = false;
        showFaceScreen();
        activateMood(MOOD_HAPPY, now, 1600);
      } else if (b == 'P') {
        Serial.println("CMD P tap");
        touch.pendingEvent = TOUCH_TAP;
      } else if (b == 'D') {
        Serial.println("CMD D double");
        touch.pendingEvent = TOUCH_DOUBLE_TAP;
      } else if (b == 'E') {
        Serial.println("CMD E stroke");
        touch.pendingEvent = TOUCH_STROKE;
      } else if (b == 'L') {
        Serial.println("CMD L long");
        touch.pendingEvent = TOUCH_LONG;
      }
      continue;
    }

    webFrame[serialFrameIndex++] = b;
    if (serialFrameIndex >= sizeof(webFrame)) {
      serialFrameReceiving = false;
      webFrameActive = true;
      lastWebFrameMs = now;
      Serial.println("FRAME OK");
    }
  }
#else
  (void)now;
#endif
}

String geminiPrompt() {
  String prompt;
  prompt.reserve(520);
  prompt += "You control a cute monochrome OLED pet face. ";
  prompt += "Return exactly one uppercase mood token, no explanation. ";
  prompt += "Allowed: HAPPY, PLEASED, GIGGLE, SHY, CURIOUS, FOCUS, WINK, ";
  prompt += "SLEEPY, YAWN, SURPRISED, WORRIED, SAD, NERVOUS, PROUD, ";
  prompt += "EXCITED, RELIEVED, ANNOYED, BORED, DIZZY, LOVE, SMIRK, ";
  prompt += "SKEPTICAL, LAUGH, CRY, DANCE, DREAMY, DETERMINED, PANIC, COOL, HOT, COLD. ";
  prompt += "Current sensors: ";
  prompt += "tempC=";
  prompt += sensors.dhtValid ? String(sensors.tempC, 1) : String("unknown");
  prompt += ", humidity=";
  prompt += sensors.dhtValid ? String(sensors.humidity, 0) : String("unknown");
  prompt += ", motion=";
  prompt += String(sensors.motion, 2);
  prompt += ", tiltX=";
  prompt += String(sensors.tiltX, 2);
  prompt += ", tiltY=";
  prompt += String(sensors.tiltY, 2);
  prompt += ", currentMood=";
  prompt += moodName(activeMood);
  prompt += ". Pick a lively but appropriate mood.";
  return prompt;
}

bool askGeminiMood(unsigned long now, bool force = false) {
#if GEMINI_AI_MODE
  if (!secretsConfigured() || WiFi.status() != WL_CONNECTED) return false;
  if (!force && now - lastGeminiMs < GEMINI_INTERVAL_MS) return false;
  lastGeminiMs = now;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += GEMINI_MODEL;
  url += ":generateContent";

  if (!http.begin(client, url)) {
    geminiStatus = "AI HTTP";
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);
  http.setTimeout(9000);

  JsonDocument req;
  req["generationConfig"]["temperature"] = 0.35;
  req["generationConfig"]["maxOutputTokens"] = 8;
  JsonArray contents = req["contents"].to<JsonArray>();
  JsonObject content = contents.add<JsonObject>();
  JsonArray parts = content["parts"].to<JsonArray>();
  parts.add<JsonObject>()["text"] = geminiPrompt();

  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  if (code <= 0) {
    geminiStatus = "AI NET";
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();

  if (code < 200 || code >= 300) {
    geminiStatus = "AI ERR";
    return false;
  }

  JsonDocument res;
  DeserializationError err = deserializeJson(res, response);
  if (err) {
    geminiStatus = "AI JSON";
    return false;
  }

  const char* text = res["candidates"][0]["content"]["parts"][0]["text"] | "";
  Mood mood = moodFromGeminiText(String(text));
  if (mood == MOOD_IDLE) {
    geminiStatus = "AI IDLE";
    return false;
  }

  activateMood(mood, now, 5200);
  geminiStatus = "AI ";
  geminiStatus += moodName(mood);
  return true;
#else
  (void)now;
  return false;
#endif
}

Mood loveMeNotMood(unsigned long now) {
  const uint16_t beatMs = 526; // 114 BPM
  uint16_t beat = (now / beatMs) % 384;

  if (beat < 16) return MOOD_GLANCE_RIGHT;
  if (beat < 32) return MOOD_GLANCE_LEFT;
  if (beat < 48) return MOOD_SMIRK;
  if (beat < 64) return MOOD_WORRIED;

  if (beat < 80) return MOOD_DREAMY;
  if (beat < 96) return MOOD_SAD;
  if (beat < 112) return MOOD_CURIOUS;
  if (beat < 128) return MOOD_SHY;

  if (beat < 136) return MOOD_LOVE;
  if (beat < 144) return MOOD_WINK;
  if (beat < 152) return MOOD_HAPPY;
  if (beat < 160) return MOOD_SKEPTICAL;
  if (beat < 168) return MOOD_LOVE;
  if (beat < 176) return MOOD_WINK;
  if (beat < 184) return MOOD_HAPPY;
  if (beat < 192) return MOOD_SKEPTICAL;

  if (beat < 208) return MOOD_NERVOUS;
  if (beat < 224) return MOOD_PANIC;
  if (beat < 240) return MOOD_LOVE;
  if (beat < 256) return MOOD_RELIEVED;

  if (beat < 272) return MOOD_SHY;
  if (beat < 288) return MOOD_WORRIED;
  if (beat < 304) return MOOD_DIZZY;
  if (beat < 320) return MOOD_DANCE;

  if (beat < 336) return MOOD_ANNOYED;
  if (beat < 352) return MOOD_RELIEVED;
  if (beat < 368) return MOOD_LOVE;
  return MOOD_DIZZY;
}

Mood loveStoryMood(unsigned long now) {
  const uint16_t beatMs = 575; // faster visual pulse
  uint16_t beat = (now / beatMs) % 384;

  if (beat < 24) return MOOD_DREAMY;
  if (beat < 48) return MOOD_SHY;
  if (beat < 72) return MOOD_LOVE;
  if (beat < 96) return MOOD_WINK;
  if (beat < 120) return MOOD_DETERMINED;
  if (beat < 144) return MOOD_EXCITED;
  if (beat < 168) return MOOD_LOVE;
  if (beat < 192) return MOOD_DANCE;
  if (beat < 216) return MOOD_SMIRK;
  if (beat < 240) return MOOD_SURPRISED;
  if (beat < 264) return MOOD_RELIEVED;
  if (beat < 288) return MOOD_LOVE;
  if (beat < 312) return MOOD_HAPPY;
  if (beat < 336) return MOOD_PROUD;
  if (beat < 360) return MOOD_DANCE;
  return MOOD_PLEASED;
}

Mood chooseMood(unsigned long now) {
#if LOVE_STORY_MODE
  activeMood = loveStoryMood(now);
  moodUntilMs = now + 700UL;
  return activeMood;
#endif

#if LOVE_ME_NOT_MODE
  activeMood = loveMeNotMood(now);
  moodUntilMs = now + 600UL;
  return activeMood;
#endif

  bool activeExpired = now >= moodUntilMs;

  if (reminder.lastAckMs > 0UL && now - reminder.lastAckMs < 1800UL) {
    return activeMood;
  }

  if (pendingAngryAtMs > 0UL && now >= pendingAngryAtMs) {
    pendingAngryAtMs = 0UL;
    activateMood(MOOD_ANNOYED, now, 3200);
    return activeMood;
  }

#if FACE_SENSOR_REACTIONS
  if ((sensors.motion > 3.25f || fabsf(sensors.tiltX) > 0.70f) &&
      sensors.lastShakeMs > 0UL && now - sensors.lastShakeMs < 760UL) {
    activateMood(MOOD_DIZZY, now, 3600);
    pendingAngryAtMs = now + 3600UL;
    return activeMood;
  }

  if (sensors.lastShakeMs > 0UL && now - sensors.lastShakeMs < 320UL) {
    activateMood(MOOD_SURPRISED, now, 820);
    return activeMood;
  }

  if (sensors.dhtValid && isHotCondition() && now - lastHotMs > 8500UL) {
    lastHotMs = now;
    activateMood(MOOD_HOT, now, 4200);
    return activeMood;
  }

  if (sensors.dhtValid && isColdCondition() && now - lastColdMs > 8500UL) {
    lastColdMs = now;
    activateMood(MOOD_COLD, now, 4200);
    return activeMood;
  }

  if (sensors.dhtValid && !isHotCondition() && !isColdCondition() &&
      now - lastHotMs > 18000UL && now - lastColdMs > 18000UL) {
    if (random(0, 12) == 0) {
      lastHotMs = now;
      lastColdMs = now;
      activateMood(MOOD_HEART_POP, now, 2200);
      return activeMood;
    }
  }

  if (fabsf(sensors.tiltX) > 0.34f && now - lastTiltMs > 2200UL) {
    lastTiltMs = now;
    activateMood(sensors.tiltX > 0.0f ? MOOD_GLANCE_RIGHT : MOOD_GLANCE_LEFT, now, 1450);
    return activeMood;
  }

  if (fabsf(sensors.tiltY) > 0.34f && now - lastTiltMs > 2200UL) {
    lastTiltMs = now;
    activateMood(sensors.tiltY > 0.0f ? MOOD_PEEK_DOWN : MOOD_PEEK_UP, now, 1450);
    return activeMood;
  }

  if (sensors.motion > 1.9f && now - lastMotionMs > 2400UL) {
    lastMotionMs = now;
    activateMood(MOOD_DANCE, now, 2400);
    return activeMood;
  }

  if (sensors.motion > 0.85f && now - lastMotionMs > 3200UL) {
    lastMotionMs = now;
    activateMood(MOOD_EXCITED, now, 2000);
    return activeMood;
  }
#endif

  if (!activeExpired) {
    return activeMood;
  }

  if (nextIdleEventMs == 0UL) {
    nextIdleEventMs = now + random(900UL, 2600UL);
  }

  if (now >= nextIdleEventMs) {
    Mood idleMood = randomIdleMood();
    nextIdleEventMs = now + random(1300UL, 3600UL);
    if (idleMood != MOOD_IDLE) {
      activateMood(idleMood, now);
      return activeMood;
    }
  }

  if (now - lastDemoMs > 7600UL) {
    lastDemoMs = now;
    activateMood(nextDemoMood(), now);
    return activeMood;
  }

  activeMood = MOOD_IDLE;
  moodUntilMs = now + 1200UL;
  return activeMood;
}

FacePose basePose() {
  FacePose p;
  p.eyeLX = 17.0f;
  p.eyeLY = 6.0f;
  p.eyeRX = 83.0f;
  p.eyeRY = 6.0f;
  p.eyeW = 28.0f;
  p.eyeLH = 38.0f;
  p.eyeRH = 38.0f;
  p.blinkL = 0.0f;
  p.blinkR = 0.0f;
  p.mouthX = 64.0f;
  p.mouthY = 40.0f;
  p.mouthW = 28.0f;
  p.mouthDepth = 6.0f;
  p.mouthOpen = 0.0f;
  p.brow = 0.0f;
  p.blush = 0.0f;
  p.sweat = 0.0f;
  p.shiver = 0.0f;
  p.question = 0.0f;
  p.zzz = 0.0f;
  p.sparkle = 0.0f;
  p.steam = 0.0f;
  p.tear = 0.0f;
  p.stress = 0.0f;
  p.proud = 0.0f;
  p.dizzy = 0.0f;
  p.hearts = 0.0f;
  p.cool = 0.0f;
  return p;
}

FacePose targetPose(unsigned long now) {
  Mood mood = chooseMood(now);
  FacePose p = basePose();

  float breathe = wave(now, 3600.0f);
  float bob = wave(now + 400UL, 2800.0f);
#if LOVE_ME_NOT_MODE || LOVE_STORY_MODE
  const uint16_t songBeatMs = LOVE_STORY_MODE ? 575 : 526;
  float beatPulse = pulse(now, songBeatMs, 0UL, 170UL);
  float offBeatPulse = pulse(now, songBeatMs, songBeatMs / 2, 130UL);
  bob += beatPulse * 1.75f - offBeatPulse * 0.55f;
#endif
  float blink = pulse(now, 4200UL, 1650UL, 190UL);
  float secondBlink = pulse(now, 16800UL, 7800UL, 150UL);
  if (secondBlink > blink) blink = secondBlink;

  float lookX = 0.0f;
  float lookY = 0.0f;
  bool softIdle = mood == MOOD_IDLE || mood == MOOD_SOFT_BLINK || mood == MOOD_BREATHE;
  if (softIdle) {
    unsigned long gaze = now % 11200UL;
    if (gaze < 1900UL) {
      lookX = 5.8f;
    } else if (gaze < 3300UL) {
      lookX = 0.0f;
    } else if (gaze < 5200UL) {
      lookX = -5.8f;
    } else if (gaze < 6500UL) {
      lookX = 0.0f;
    } else if (gaze < 7900UL) {
      lookY = -4.2f;
    } else if (gaze < 9200UL) {
      lookY = 4.2f;
    }
  }

  p.eyeLX += lookX + sensors.tiltX * 2.0f;
  p.eyeRX += lookX + sensors.tiltX * 2.0f;
  p.eyeLY += lookY + bob * 0.8f + sensors.tiltY;
  p.eyeRY += lookY + bob * 0.8f + sensors.tiltY;
  p.mouthX += lookX * 0.12f;
  p.mouthY += lookY * 0.10f;
  p.eyeLH += breathe * 1.2f;
  p.eyeRH += breathe * 1.2f;
  p.mouthY += bob * 0.7f;
  p.blinkL = blink;
  p.blinkR = blink;

#if CUTE_COMPANION_MODE
  float cutePulse = pulse(now, 5200UL, 420UL, 900UL);
  float tinyPulse = pulse(now, 3100UL, 1200UL, 420UL);
  p.blush = max(p.blush, 0.18f + cutePulse * 0.28f);
  p.sparkle = max(p.sparkle, tinyPulse * 0.35f);
  if (cutePulse > 0.45f) {
    p.hearts = max(p.hearts, cutePulse * 0.55f);
  }
#endif

#if LOVE_ME_NOT_MODE || LOVE_STORY_MODE
  p.eyeLY += beatPulse * 1.35f;
  p.eyeRY += beatPulse * 1.35f;
  p.mouthW += beatPulse * 2.4f;
  p.mouthDepth += beatPulse * 1.2f;
#endif

#if LOVE_STORY_MODE
  p.hearts = 0.75f + beatPulse * 0.25f;
  p.sparkle = 0.55f + offBeatPulse * 0.45f;
  p.blush = max(p.blush, 0.35f + beatPulse * 0.35f);
#endif

  switch (mood) {
    case MOOD_HAPPY:
      p.eyeLY += 2.0f;
      p.eyeRY += 2.0f;
      p.eyeLH = 18.0f + breathe;
      p.eyeRH = 18.0f + breathe;
      p.mouthW = 36.0f;
      p.mouthDepth = 9.0f;
      p.mouthY = 38.0f + bob;
      p.blush = 1.0f;
      p.sparkle = 0.55f;
      break;
    case MOOD_PLEASED:
      p.eyeLY += 5.0f;
      p.eyeRY += 5.0f;
      p.eyeLH = 14.0f + breathe;
      p.eyeRH = 14.0f + breathe;
      p.mouthW = 32.0f;
      p.mouthDepth = 7.0f;
      p.mouthY = 39.0f;
      p.blush = 0.55f;
      break;
    case MOOD_GIGGLE:
      p.eyeLY += 4.0f + wave(now, 210.0f) * 1.5f;
      p.eyeRY += 4.0f - wave(now, 210.0f) * 1.5f;
      p.eyeLH = 16.0f + breathe;
      p.eyeRH = 16.0f + breathe;
      p.mouthW = 34.0f + wave(now, 180.0f) * 2.0f;
      p.mouthDepth = 10.0f;
      p.mouthY = 38.0f + wave(now, 180.0f);
      p.blush = 0.85f;
      p.sparkle = 0.7f;
      p.hearts = 0.35f;
      break;
    case MOOD_SHY:
      p.eyeLX -= 2.0f;
      p.eyeRX -= 2.0f;
      p.eyeLY += 2.0f;
      p.eyeRY += 2.0f;
      p.eyeLH = 30.0f;
      p.eyeRH = 30.0f;
      p.mouthW = 18.0f;
      p.mouthDepth = 3.0f;
      p.mouthX -= 3.0f;
      p.blush = 1.0f;
      p.hearts = 0.45f;
      break;
    case MOOD_CURIOUS:
      p.eyeLX += sensors.tiltX * 4.0f;
      p.eyeRX += sensors.tiltX * 4.0f;
      p.eyeLH = 42.0f;
      p.eyeRH = 34.0f;
      if (sensors.tiltX < 0.0f) {
        p.eyeLH = 34.0f;
        p.eyeRH = 42.0f;
      }
      p.mouthW = 22.0f;
      p.mouthDepth = 3.0f;
      p.brow = sensors.tiltX >= 0.0f ? 1.0f : -1.0f;
      p.question = 1.0f;
      break;
    case MOOD_FOCUS:
      p.eyeW = 30.0f;
      p.eyeLH = 24.0f;
      p.eyeRH = 24.0f;
      p.eyeLY += 6.0f;
      p.eyeRY += 6.0f;
      p.mouthW = 18.0f;
      p.mouthDepth = 0.0f;
      p.brow = 1.45f;
      break;
    case MOOD_WINK:
      p.eyeLY += 1.0f;
      p.eyeRY += 4.0f;
      p.eyeLH = 35.0f;
      p.eyeRH = 12.0f;
      p.blinkR = 0.65f + pulse(now, 3300UL, 850UL, 380UL) * 0.35f;
      p.mouthW = 33.0f;
      p.mouthDepth = 9.0f;
      p.mouthX -= 2.0f;
      p.blush = 0.55f;
      p.sparkle = 1.0f;
      p.hearts = 0.45f;
      break;
    case MOOD_SLEEPY:
      p.eyeLH = 10.0f;
      p.eyeRH = 10.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 8.0f;
      p.mouthW = 18.0f;
      p.mouthDepth = 1.0f;
      p.mouthY = 41.0f;
      p.blinkL = 0.45f;
      p.blinkR = 0.45f;
      p.zzz = 1.0f;
      break;
    case MOOD_YAWN:
      p.eyeLH = 12.0f;
      p.eyeRH = 12.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 8.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = 4.0f + pulse(now, 3500UL, 600UL, 1500UL) * 2.0f;
      p.mouthY = 41.0f;
      p.blinkL = 0.48f;
      p.blinkR = 0.48f;
      p.zzz = 0.65f;
      break;
    case MOOD_SURPRISED:
      p.eyeW = 32.0f;
      p.eyeLH = 44.0f;
      p.eyeRH = 44.0f;
      p.eyeLY -= 2.0f;
      p.eyeRY -= 2.0f;
      p.mouthOpen = 1.0f;
      p.mouthY = 42.0f;
      break;
    case MOOD_WORRIED:
      p.eyeW = 27.0f;
      p.eyeLH = 35.0f;
      p.eyeRH = 35.0f;
      p.eyeLY += 2.0f;
      p.eyeRY += 2.0f;
      p.mouthW = 23.0f;
      p.mouthDepth = -3.0f;
      p.brow = -1.4f;
      p.sweat = 0.55f;
      break;
    case MOOD_SAD:
      p.eyeW = 27.0f;
      p.eyeLH = 32.0f;
      p.eyeRH = 32.0f;
      p.eyeLY += 4.0f;
      p.eyeRY += 4.0f;
      p.mouthW = 23.0f;
      p.mouthDepth = -4.0f;
      p.mouthY = 42.0f;
      p.brow = -1.2f;
      p.tear = 0.9f;
      break;
    case MOOD_NERVOUS:
      p.eyeW = 27.0f + wave(now, 130.0f) * 1.0f;
      p.eyeLH = 30.0f + wave(now, 160.0f) * 1.5f;
      p.eyeRH = 30.0f - wave(now, 160.0f) * 1.5f;
      p.eyeLY += 3.0f;
      p.eyeRY += 3.0f;
      p.mouthW = 20.0f;
      p.mouthDepth = -1.0f + wave(now, 130.0f);
      p.sweat = 0.75f;
      p.stress = 1.0f;
      break;
    case MOOD_PROUD:
      p.eyeLY += 4.0f;
      p.eyeRY += 4.0f;
      p.eyeLH = 16.0f + breathe;
      p.eyeRH = 16.0f + breathe;
      p.mouthW = 33.0f;
      p.mouthDepth = 7.0f;
      p.mouthY = 39.0f;
      p.brow = 0.65f;
      p.proud = 1.0f;
      p.sparkle = 0.55f;
      break;
    case MOOD_EXCITED:
      p.eyeW = 33.0f + wave(now, 220.0f) * 1.0f;
      p.eyeLH = 42.0f + wave(now, 180.0f) * 2.0f;
      p.eyeRH = 42.0f - wave(now, 180.0f) * 2.0f;
      p.eyeLY -= 1.0f + fabsf(wave(now, 210.0f)) * 2.0f;
      p.eyeRY -= 1.0f + fabsf(wave(now, 210.0f)) * 2.0f;
      p.mouthW = 37.0f;
      p.mouthDepth = 10.0f;
      p.mouthY = 38.0f + wave(now, 200.0f);
      p.sparkle = 1.0f;
      p.blush = 0.45f;
      break;
    case MOOD_RELIEVED:
      p.eyeLY += 5.0f;
      p.eyeRY += 5.0f;
      p.eyeLH = 13.0f + breathe;
      p.eyeRH = 13.0f + breathe;
      p.mouthW = 30.0f;
      p.mouthDepth = 5.0f;
      p.mouthY = 40.0f;
      p.steam = 0.45f;
      break;
    case MOOD_ANNOYED:
      p.eyeLH = 17.0f;
      p.eyeRH = 17.0f;
      p.eyeLY += 10.0f;
      p.eyeRY += 10.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = -2.0f;
      p.brow = 2.0f;
      break;
    case MOOD_BORED:
      p.eyeW = 30.0f;
      p.eyeLH = 13.0f;
      p.eyeRH = 13.0f;
      p.eyeLY += 9.0f;
      p.eyeRY += 9.0f;
      p.mouthW = 21.0f;
      p.mouthDepth = 0.0f;
      p.mouthY = 41.0f;
      p.brow = 0.25f;
      break;
    case MOOD_DIZZY:
      p.eyeW = 27.0f + wave(now, 260.0f) * 2.0f;
      p.eyeLH = 33.0f + wave(now, 310.0f) * 3.0f;
      p.eyeRH = 33.0f - wave(now, 310.0f) * 3.0f;
      p.eyeLX += wave(now, 420.0f) * 3.0f;
      p.eyeRX -= wave(now, 420.0f) * 3.0f;
      p.mouthW = 22.0f;
      p.mouthDepth = wave(now, 240.0f) * 2.0f;
      p.dizzy = 1.0f;
      p.stress = 0.8f;
      break;
    case MOOD_LOVE:
      p.eyeLY += 2.0f + wave(now, 900.0f);
      p.eyeRY += 2.0f + wave(now + 180UL, 900.0f);
      p.eyeLH = 30.0f + pulse(now, 1500UL, 200UL, 700UL) * 5.0f;
      p.eyeRH = 30.0f + pulse(now, 1500UL, 200UL, 700UL) * 5.0f;
      p.mouthW = 31.0f;
      p.mouthDepth = 8.0f;
      p.mouthY = 39.0f + wave(now, 1200.0f);
      p.blush = 1.0f;
      p.hearts = 1.0f;
      p.sparkle = 0.75f;
      break;
    case MOOD_SMIRK:
      p.eyeLH = 19.0f;
      p.eyeRH = 31.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 3.0f;
      p.mouthW = 28.0f;
      p.mouthDepth = 5.0f;
      p.mouthX += 4.0f;
      p.mouthY = 40.0f;
      p.brow = 0.75f;
      break;
    case MOOD_SKEPTICAL:
      p.eyeLH = 27.0f;
      p.eyeRH = 18.0f;
      p.eyeLY += 5.0f;
      p.eyeRY += 9.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = -1.0f;
      p.mouthX -= 2.0f;
      p.brow = 1.35f;
      p.question = 0.55f;
      break;
    case MOOD_LAUGH:
      p.eyeLY += 5.0f + wave(now, 160.0f);
      p.eyeRY += 5.0f - wave(now, 160.0f);
      p.eyeLH = 13.0f;
      p.eyeRH = 13.0f;
      p.mouthW = 38.0f + wave(now, 150.0f) * 2.0f;
      p.mouthDepth = 11.0f;
      p.mouthY = 38.0f + fabsf(wave(now, 150.0f)) * 2.0f;
      p.blush = 0.8f;
      p.sparkle = 0.75f;
      break;
    case MOOD_CRY:
      p.eyeW = 27.0f;
      p.eyeLH = 35.0f + pulse(now, 1100UL, 100UL, 420UL) * 3.0f;
      p.eyeRH = 35.0f + pulse(now, 1100UL, 320UL, 420UL) * 3.0f;
      p.eyeLY += 3.0f;
      p.eyeRY += 3.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = -5.0f;
      p.mouthY = 42.0f;
      p.brow = -1.45f;
      p.tear = 1.0f;
      p.stress = 0.45f;
      break;
    case MOOD_DANCE:
      p.eyeLX += wave(now, 520.0f) * 5.0f;
      p.eyeRX += wave(now, 520.0f) * 5.0f;
      p.eyeLY += fabsf(wave(now, 260.0f)) * 4.0f;
      p.eyeRY += fabsf(wave(now + 130UL, 260.0f)) * 4.0f;
      p.eyeLH = 31.0f + wave(now, 320.0f) * 4.0f;
      p.eyeRH = 31.0f - wave(now, 320.0f) * 4.0f;
      p.mouthW = 34.0f;
      p.mouthDepth = 8.0f + wave(now, 260.0f) * 2.0f;
      p.mouthX += wave(now, 520.0f) * 3.0f;
      p.mouthY = 39.0f + fabsf(wave(now, 260.0f)) * 2.0f;
      p.sparkle = 1.0f;
      p.proud = 0.45f;
      break;
    case MOOD_DREAMY:
      p.eyeLY += 6.0f;
      p.eyeRY += 6.0f;
      p.eyeLH = 16.0f + breathe;
      p.eyeRH = 16.0f + breathe;
      p.eyeLX += wave(now, 2200.0f) * 2.0f;
      p.eyeRX += wave(now, 2200.0f) * 2.0f;
      p.mouthW = 29.0f;
      p.mouthDepth = 6.0f;
      p.mouthY = 40.0f;
      p.blush = 0.45f;
      p.hearts = 0.45f;
      p.sparkle = 0.45f;
      break;
    case MOOD_DETERMINED:
      p.eyeW = 30.0f;
      p.eyeLH = 20.0f;
      p.eyeRH = 20.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 8.0f;
      p.mouthW = 25.0f;
      p.mouthDepth = 1.0f;
      p.brow = 1.9f;
      p.proud = 0.55f;
      break;
    case MOOD_PANIC:
      p.eyeW = 32.0f + wave(now, 120.0f) * 1.5f;
      p.eyeLH = 43.0f;
      p.eyeRH = 43.0f;
      p.eyeLX += wave(now, 95.0f) * 2.0f;
      p.eyeRX -= wave(now, 95.0f) * 2.0f;
      p.eyeLY -= 1.0f;
      p.eyeRY -= 1.0f;
      p.mouthW = 22.0f;
      p.mouthDepth = -2.0f + wave(now, 130.0f);
      p.sweat = 1.0f;
      p.stress = 1.0f;
      break;
    case MOOD_COOL:
      p.eyeLH = 15.0f;
      p.eyeRH = 15.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 8.0f;
      p.mouthW = 29.0f;
      p.mouthDepth = 4.0f;
      p.mouthX += 2.0f;
      p.cool = 1.0f;
      p.sparkle = 0.35f;
      break;
    case MOOD_GLANCE_LEFT:
      p.eyeLX -= 5.0f + pulse(now, 1800UL, 180UL, 520UL) * 1.0f;
      p.eyeRX -= 5.0f + pulse(now, 1800UL, 180UL, 520UL) * 1.0f;
      p.mouthX -= 1.5f;
      p.mouthW = 27.0f;
      p.mouthDepth = 5.0f;
      break;
    case MOOD_GLANCE_RIGHT:
      p.eyeLX += 5.0f + pulse(now, 1800UL, 180UL, 520UL) * 1.0f;
      p.eyeRX += 5.0f + pulse(now, 1800UL, 180UL, 520UL) * 1.0f;
      p.mouthX += 1.5f;
      p.mouthW = 27.0f;
      p.mouthDepth = 5.0f;
      break;
    case MOOD_PEEK_UP:
      p.eyeLY -= 4.0f;
      p.eyeRY -= 4.0f;
      p.eyeLH = 40.0f;
      p.eyeRH = 40.0f;
      p.mouthY = 39.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = 4.0f;
      p.question = 0.35f;
      break;
    case MOOD_PEEK_DOWN:
      p.eyeLY += 6.0f;
      p.eyeRY += 6.0f;
      p.eyeLH = 31.0f;
      p.eyeRH = 31.0f;
      p.mouthY = 42.0f;
      p.mouthW = 25.0f;
      p.mouthDepth = 3.0f;
      break;
    case MOOD_SOFT_BLINK:
      p.eyeLY += 3.0f;
      p.eyeRY += 3.0f;
      p.eyeLH = 20.0f;
      p.eyeRH = 20.0f;
      p.blinkL = 0.25f + pulse(now, 1500UL, 260UL, 520UL) * 0.6f;
      p.blinkR = 0.25f + pulse(now, 1500UL, 300UL, 520UL) * 0.6f;
      p.mouthW = 29.0f;
      p.mouthDepth = 6.0f;
      break;
    case MOOD_BREATHE:
      p.eyeLY += 2.0f + breathe * 1.5f;
      p.eyeRY += 2.0f + breathe * 1.5f;
      p.eyeLH = 34.0f + breathe * 3.0f;
      p.eyeRH = 34.0f + breathe * 3.0f;
      p.mouthY = 40.0f + breathe;
      p.mouthW = 28.0f + breathe * 2.0f;
      p.mouthDepth = 5.0f;
      break;
    case MOOD_DOUBLE_BLINK: {
      float b1 = pulse(now, 1500UL, 120UL, 180UL);
      float b2 = pulse(now, 1500UL, 460UL, 180UL);
      float db = b1 > b2 ? b1 : b2;
      p.eyeLY += 3.0f;
      p.eyeRY += 3.0f;
      p.eyeLH = 25.0f;
      p.eyeRH = 25.0f;
      p.blinkL = db;
      p.blinkR = db;
      p.mouthW = 27.0f;
      p.mouthDepth = 5.0f;
      break;
    }
    case MOOD_LOOK_AROUND:
      p.eyeLX += wave(now, 900.0f) * 5.0f;
      p.eyeRX += wave(now, 900.0f) * 5.0f;
      p.eyeLY += wave(now + 300UL, 1300.0f) * 2.0f;
      p.eyeRY += wave(now + 300UL, 1300.0f) * 2.0f;
      p.mouthX += wave(now, 900.0f) * 0.8f;
      p.mouthW = 26.0f;
      p.mouthDepth = 4.0f;
      p.question = 0.25f;
      break;
    case MOOD_TINY_NOD:
      p.eyeLY += 2.0f + fabsf(wave(now, 520.0f)) * 4.0f;
      p.eyeRY += 2.0f + fabsf(wave(now, 520.0f)) * 4.0f;
      p.mouthY = 40.0f + fabsf(wave(now, 520.0f)) * 2.0f;
      p.mouthW = 29.0f;
      p.mouthDepth = 5.0f;
      break;
    case MOOD_SIDE_EYE:
      p.eyeLX -= 4.5f;
      p.eyeRX -= 4.5f;
      p.eyeLH = 24.0f;
      p.eyeRH = 28.0f;
      p.eyeLY += 5.0f;
      p.eyeRY += 3.0f;
      p.mouthX -= 3.0f;
      p.mouthW = 24.0f;
      p.mouthDepth = 2.0f;
      p.brow = 0.55f;
      break;
    case MOOD_SOFT_SMILE:
      p.eyeLY += 4.0f;
      p.eyeRY += 4.0f;
      p.eyeLH = 22.0f + breathe;
      p.eyeRH = 22.0f + breathe;
      p.mouthW = 34.0f;
      p.mouthDepth = 7.0f;
      p.mouthY = 39.0f + bob;
      p.blush = 0.65f;
      p.hearts = 0.35f;
      break;
    case MOOD_HEART_POP:
      p.eyeLY += 2.0f;
      p.eyeRY += 2.0f;
      p.eyeLH = 32.0f + pulse(now, 1700UL, 120UL, 360UL) * 3.0f;
      p.eyeRH = 32.0f + pulse(now, 1700UL, 120UL, 360UL) * 3.0f;
      p.mouthW = 30.0f;
      p.mouthDepth = 7.0f;
      p.hearts = 1.0f;
      p.blush = 0.75f;
      p.sparkle = 0.8f;
      break;
    case MOOD_HOT:
      p.eyeLH = 24.0f;
      p.eyeRH = 24.0f;
      p.eyeLY += 8.0f;
      p.eyeRY += 8.0f;
      p.mouthW = 23.0f;
      p.mouthDepth = 1.0f;
      p.sweat = 1.0f;
      p.steam = 1.0f;
      break;
    case MOOD_COLD:
      p.eyeW = 24.0f;
      p.eyeLH = 26.0f;
      p.eyeRH = 26.0f;
      p.mouthW = 22.0f;
      p.mouthDepth = -1.0f;
      p.shiver = 1.0f;
      break;
    case MOOD_REMINDER:
      p.eyeW = 31.0f;
      p.eyeLH = 40.0f + pulse(now, 1200UL, 120UL, 420UL) * 4.0f;
      p.eyeRH = 40.0f + pulse(now, 1200UL, 120UL, 420UL) * 4.0f;
      p.eyeLY -= 1.0f;
      p.eyeRY -= 1.0f;
      p.mouthOpen = 0.35f + pulse(now, 1500UL, 180UL, 650UL) * 0.35f;
      p.mouthY = 42.0f;
      p.brow = 1.0f;
      p.sparkle = 1.0f;
      p.question = 0.65f;
      break;
    case MOOD_IDLE:
    default:
      break;
  }

  float shake = 0.0f;
  if (FACE_SENSOR_REACTIONS && sensors.lastShakeMs > 0UL && now - sensors.lastShakeMs < 900UL) {
    float t = 1.0f - smooth01((float)(now - sensors.lastShakeMs) / 900.0f);
    shake = wave(now, 95.0f) * 4.0f * t;
  }

  float coldShake = p.shiver * wave(now, 115.0f) * 2.0f;
  p.eyeLX += shake + coldShake;
  p.eyeRX += shake + coldShake;
  p.mouthX += shake * 0.4f + coldShake * 0.4f;

  return p;
}

void blendPose(FacePose target) {
  if (!poseReady) {
    currentPose = target;
    poseReady = true;
    return;
  }

  const float a = 0.105f;
  const float poseA =
#if LOVE_STORY_MODE
    0.13f;
#else
    a;
#endif
  currentPose.eyeLX = lerpFloat(currentPose.eyeLX, target.eyeLX, poseA);
  currentPose.eyeLY = lerpFloat(currentPose.eyeLY, target.eyeLY, poseA);
  currentPose.eyeRX = lerpFloat(currentPose.eyeRX, target.eyeRX, poseA);
  currentPose.eyeRY = lerpFloat(currentPose.eyeRY, target.eyeRY, poseA);
  currentPose.eyeW = lerpFloat(currentPose.eyeW, target.eyeW, poseA);
  currentPose.eyeLH = lerpFloat(currentPose.eyeLH, target.eyeLH, poseA);
  currentPose.eyeRH = lerpFloat(currentPose.eyeRH, target.eyeRH, poseA);
  currentPose.blinkL = lerpFloat(currentPose.blinkL, target.blinkL, 0.28f);
  currentPose.blinkR = lerpFloat(currentPose.blinkR, target.blinkR, 0.28f);
  currentPose.mouthX = lerpFloat(currentPose.mouthX, target.mouthX, poseA);
  currentPose.mouthY = lerpFloat(currentPose.mouthY, target.mouthY, poseA);
  currentPose.mouthW = lerpFloat(currentPose.mouthW, target.mouthW, poseA);
  currentPose.mouthDepth = lerpFloat(currentPose.mouthDepth, target.mouthDepth, poseA);
  currentPose.mouthOpen = lerpFloat(currentPose.mouthOpen, target.mouthOpen,
                                    target.mouthOpen > currentPose.mouthOpen ? 0.16f : 0.26f);
  currentPose.brow = lerpFloat(currentPose.brow, target.brow, poseA);
  currentPose.blush = lerpFloat(currentPose.blush, target.blush, poseA);
  currentPose.sweat = lerpFloat(currentPose.sweat, target.sweat, poseA);
  currentPose.shiver = lerpFloat(currentPose.shiver, target.shiver, poseA);
  currentPose.question = lerpFloat(currentPose.question, target.question, poseA);
  currentPose.zzz = lerpFloat(currentPose.zzz, target.zzz, poseA);
  currentPose.sparkle = lerpFloat(currentPose.sparkle, target.sparkle, poseA);
  currentPose.steam = lerpFloat(currentPose.steam, target.steam, poseA);
  currentPose.tear = lerpFloat(currentPose.tear, target.tear, poseA);
  currentPose.stress = lerpFloat(currentPose.stress, target.stress, poseA);
  currentPose.proud = lerpFloat(currentPose.proud, target.proud, poseA);
  currentPose.dizzy = lerpFloat(currentPose.dizzy, target.dizzy, poseA);
  currentPose.hearts = lerpFloat(currentPose.hearts, target.hearts, poseA);
  currentPose.cool = lerpFloat(currentPose.cool, target.cool, poseA);
}

void drawPill(float fx, float fy, float fw, float fh, float blink) {
  int w = (int)(fw + blink * 4.0f);
  int h = (int)(fh - blink * (fh - 5.0f));
  if (h < 5) h = 5;

  int x = (int)(fx - (w - fw) / 2.0f);
  int y = (int)(fy + (fh - h) / 2.0f);
  int r = constrain((int)(w * 0.32f), 5, 11);
  display.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);
}

void drawSmile(float fcx, float fy, float fw, float fdepth) {
  int cx = (int)fcx;
  int y = (int)fy;
  int w = constrain((int)fw, 14, 38);
  int depth = constrain((int)fdepth, -5, 11);
  int half = w / 2;
  int prevX = cx - half;
  int prevY = y;

  for (int dx = -half; dx <= half; dx++) {
    long arch = (long)(half * half - dx * dx);
    int px = cx + dx;
    int py = y + (int)((long)depth * arch / (long)(half * half));
    display.drawLine(prevX, prevY, px, py, SSD1306_WHITE);
    display.drawLine(prevX, prevY + 1, px, py + 1, SSD1306_WHITE);
    prevX = px;
    prevY = py;
  }

  display.fillCircle(cx - half, y, 1, SSD1306_WHITE);
  display.fillCircle(cx + half, y, 1, SSD1306_WHITE);
}

void drawMouthO(float fcx, float fcy, float amount) {
  int cx = (int)fcx;
  int cy = (int)fcy;
  int r = 4 + (int)(amount * 5.0f);
  display.fillRoundRect(cx - r, cy - r, r * 2, r * 2 + 1, r, SSD1306_WHITE);
  display.fillRoundRect(cx - r + 3, cy - r + 3, r * 2 - 6, r * 2 - 5, r - 3, SSD1306_BLACK);
}

void drawBrows(const FacePose& p) {
  if (fabsf(p.brow) < 0.12f) return;

  int ly = (int)p.eyeLY - 4;
  int ry = (int)p.eyeRY - 4;
  int lx = (int)p.eyeLX + 2;
  int rx = (int)p.eyeRX + 2;
  int tilt = (int)(p.brow * 4.0f);

  display.drawLine(lx, ly + tilt, lx + 20, ly - tilt, SSD1306_WHITE);
  display.drawLine(rx, ry - tilt, rx + 20, ry + tilt, SSD1306_WHITE);
}

void drawBlush(const FacePose& p) {
  if (p.blush < 0.25f) return;

  int y = (int)(p.mouthY + 8);
  display.drawLine(16, y, 23, y - 2, SSD1306_WHITE);
  display.drawLine(22, y + 3, 29, y + 1, SSD1306_WHITE);
  display.drawLine(99, y - 2, 106, y, SSD1306_WHITE);
  display.drawLine(105, y + 1, 112, y + 3, SSD1306_WHITE);
}

void drawSweat(const FacePose& p) {
  if (p.sweat < 0.25f) return;

  int x = 110;
  int y = 32;
  display.fillTriangle(x, y, x - 3, y + 7, x + 3, y + 7, SSD1306_WHITE);
  display.fillCircle(x, y + 7, 3, SSD1306_WHITE);
}

void drawShiver(const FacePose& p, unsigned long now) {
  if (p.shiver < 0.25f) return;

  int offset = (int)(wave(now, 120.0f) * 2.0f);
  display.drawLine(9 + offset, 25, 14 + offset, 28, SSD1306_WHITE);
  display.drawLine(9 + offset, 33, 14 + offset, 36, SSD1306_WHITE);
  display.drawLine(114 - offset, 25, 119 - offset, 28, SSD1306_WHITE);
  display.drawLine(114 - offset, 33, 119 - offset, 36, SSD1306_WHITE);
}

void drawQuestion(const FacePose& p, unsigned long now) {
  if (p.question < 0.25f) return;

  int bob = (int)(wave(now, 1200.0f) * 1.5f);
  int x = 111;
  int y = 13 + bob;
  display.drawCircle(x + 3, y + 3, 3, SSD1306_WHITE);
  display.fillRect(x + 4, y, 3, 3, SSD1306_BLACK);
  display.drawLine(x + 3, y + 6, x + 3, y + 8, SSD1306_WHITE);
  display.drawPixel(x + 3, y + 11, SSD1306_WHITE);
}

void drawZzz(const FacePose& p, unsigned long now) {
  if (p.zzz < 0.25f) return;

  int rise = (now / 240UL) % 7UL;
  int x = 92 + rise;
  int y = 17 - rise;
  display.drawLine(x, y, x + 5, y, SSD1306_WHITE);
  display.drawLine(x + 5, y, x, y + 5, SSD1306_WHITE);
  display.drawLine(x, y + 5, x + 5, y + 5, SSD1306_WHITE);

  x += 10;
  y -= 6;
  display.drawLine(x, y, x + 4, y, SSD1306_WHITE);
  display.drawLine(x + 4, y, x, y + 4, SSD1306_WHITE);
  display.drawLine(x, y + 4, x + 4, y + 4, SSD1306_WHITE);
}

void drawSparkle(const FacePose& p, unsigned long now) {
  if (p.sparkle < 0.25f) return;

  int flash = ((now / 180UL) % 2UL) ? 1 : 0;
  int x = 112;
  int y = 12;
  display.drawPixel(x, y - 2 - flash, SSD1306_WHITE);
  display.drawPixel(x, y + 2 + flash, SSD1306_WHITE);
  display.drawPixel(x - 2 - flash, y, SSD1306_WHITE);
  display.drawPixel(x + 2 + flash, y, SSD1306_WHITE);
  display.drawPixel(x, y, SSD1306_WHITE);

  x = 14;
  y = 14;
  display.drawPixel(x, y - 1, SSD1306_WHITE);
  display.drawPixel(x, y + 1, SSD1306_WHITE);
  display.drawPixel(x - 1, y, SSD1306_WHITE);
  display.drawPixel(x + 1, y, SSD1306_WHITE);
}

void drawSteam(const FacePose& p, unsigned long now) {
  if (p.steam < 0.25f) return;

  int drift = (int)(wave(now, 1000.0f) * 2.0f);
  for (int i = 0; i < 2; i++) {
    int x = 98 + i * 8 + drift;
    int y = 7;
    display.drawLine(x, y + 9, x + 2, y + 6, SSD1306_WHITE);
    display.drawLine(x + 2, y + 6, x, y + 3, SSD1306_WHITE);
    display.drawLine(x, y + 3, x + 2, y, SSD1306_WHITE);
  }
}

void drawTear(const FacePose& p, unsigned long now) {
  if (p.tear < 0.25f) return;

  int fall = (now / 180UL) % 5UL;
  int x = 95;
  int y = 36 + fall;
  display.drawLine(x, y, x - 2, y + 5, SSD1306_WHITE);
  display.drawLine(x, y, x + 2, y + 5, SSD1306_WHITE);
  display.fillCircle(x, y + 6, 2, SSD1306_WHITE);
}

void drawStress(const FacePose& p, unsigned long now) {
  if (p.stress < 0.25f) return;

  int wiggle = (int)(wave(now, 140.0f) * 1.0f);
  display.drawLine(15 + wiggle, 11, 20 + wiggle, 15, SSD1306_WHITE);
  display.drawLine(20 + wiggle, 11, 15 + wiggle, 15, SSD1306_WHITE);
  display.drawLine(108 - wiggle, 11, 113 - wiggle, 15, SSD1306_WHITE);
  display.drawLine(113 - wiggle, 11, 108 - wiggle, 15, SSD1306_WHITE);
}

void drawProudMark(const FacePose& p) {
  if (p.proud < 0.25f) return;

  display.drawLine(56, 9, 72, 9, SSD1306_WHITE);
  display.drawLine(58, 7, 70, 7, SSD1306_WHITE);
}

void drawDizzyMarks(const FacePose& p, unsigned long now) {
  if (p.dizzy < 0.25f) return;

  int offset = (now / 160UL) % 6UL;
  display.drawCircle(13 + offset, 12, 3, SSD1306_WHITE);
  display.drawCircle(112 - offset, 14, 3, SSD1306_WHITE);
}

void drawTinyHeart(int x, int y) {
  display.fillCircle(x - 2, y, 2, SSD1306_WHITE);
  display.fillCircle(x + 2, y, 2, SSD1306_WHITE);
  display.fillTriangle(x - 5, y + 1, x + 5, y + 1, x, y + 7, SSD1306_WHITE);
}

void drawHearts(const FacePose& p, unsigned long now) {
  if (p.hearts < 0.25f) return;

  int rise = (now / 180UL) % 14UL;
  drawTinyHeart(106, 23 - rise);
  if (p.hearts > 0.75f) {
    drawTinyHeart(19, 28 - ((rise + 5) % 14));
    drawTinyHeart(92, 48 - ((rise + 9) % 16));
  }
  if (p.hearts > 0.45f) {
    int twinkle = (now / 220UL) % 2UL;
    display.drawPixel(34, 14 + twinkle, SSD1306_WHITE);
    display.drawPixel(117, 37 - twinkle, SSD1306_WHITE);
  }
}

void drawCoolShade(const FacePose& p) {
  if (p.cool < 0.25f) return;

  int ly = (int)p.eyeLY + 5;
  int ry = (int)p.eyeRY + 5;
  int lx = (int)p.eyeLX - 1;
  int rx = (int)p.eyeRX - 1;
  display.fillRoundRect(lx, ly, 31, 10, 3, SSD1306_BLACK);
  display.drawRoundRect(lx, ly, 31, 10, 3, SSD1306_WHITE);
  display.fillRoundRect(rx, ry, 31, 10, 3, SSD1306_BLACK);
  display.drawRoundRect(rx, ry, 31, 10, 3, SSD1306_WHITE);
  display.drawLine(lx + 31, ly + 5, rx, ry + 5, SSD1306_WHITE);
}

const char* loveMeNotCaption(unsigned long now) {
  const uint16_t beatMs = 526; // 114 BPM
  uint16_t beat = (now / beatMs) % 384;

  if (beat < 16) return "I NEED YOU";
  if (beat < 32) return "SLOW DOWN";
  if (beat < 48) return "BE COOL";
  if (beat < 64) return "BREAKING DOWN";

  if (beat < 80) return "RIGHT HERE";
  if (beat < 96) return "HARD TO LEAVE";
  if (beat < 112) return "EVERYWHERE";
  if (beat < 128) return "MISS YOU";

  if (beat < 136) return "LOVE ME NOT";
  if (beat < 144) return "LOVES ME";
  if (beat < 152) return "HOLD TIGHT";
  if (beat < 160) return "LET GO";
  if (beat < 168) return "LOVE ME NOT";
  if (beat < 176) return "LOVES ME";
  if (beat < 184) return "HOLD TIGHT";
  if (beat < 192) return "LET GO";

  if (beat < 208) return "LOSE CONNECT";
  if (beat < 224) return "GETTIN MESSY";
  if (beat < 240) return "HOLD ON ME";
  if (beat < 256) return "BACK TOGETHER";

  if (beat < 272) return "FAR AWAY";
  if (beat < 288) return "DONT BREAK";
  if (beat < 304) return "UP AND DOWN";
  if (beat < 320) return "ROUND AGAIN";

  if (beat < 336) return "SORRY TONIGHT";
  if (beat < 352) return "MORNING OK";
  if (beat < 368) return "HOLD ME TIGHT";
  return "OUT OF MY MIND";
}

void drawLoveMeNotCaption(unsigned long now) {
#if LOVE_ME_NOT_MODE && SONG_CAPTIONS
  const char* text = loveMeNotCaption(now);
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  int x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  display.fillRect(0, 55, SCREEN_WIDTH, 9, SSD1306_BLACK);
  display.setCursor(x, 56);
  display.print(text);
#else
  (void)now;
#endif
}

const char* loveStoryCaption(unsigned long now) {
  const uint16_t beatMs = 652; // ~92 BPM
  uint16_t beat = (now / beatMs) % 384;

  if (beat < 16) return "YOUNG LOVE";
  if (beat < 32) return "FLASHBACK";
  if (beat < 48) return "SUMMER AIR";
  if (beat < 64) return "HELLO";

  if (beat < 80) return "ROMEO";
  if (beat < 96) return "PEBBLES";
  if (beat < 112) return "STAY AWAY";
  if (beat < 128) return "PLEASE DONT GO";

  if (beat < 144) return "TAKE ME AWAY";
  if (beat < 160) return "WE CAN RUN";
  if (beat < 176) return "PRINCE";
  if (beat < 192) return "SAY YES";

  if (beat < 208) return "GARDEN";
  if (beat < 224) return "KEEP QUIET";
  if (beat < 240) return "ESCAPE TOWN";
  if (beat < 256) return "REAL LOVE";

  if (beat < 272) return "TIRED WAITING";
  if (beat < 288) return "SO ALONE";
  if (beat < 304) return "A RING";
  if (beat < 320) return "MARRY ME";

  if (beat < 336) return "WHITE DRESS";
  if (beat < 352) return "LOVE STORY";
  if (beat < 368) return "BABY SAY YES";
  return "FIRST SAW YOU";
}

void drawCenteredCaption(const char* text) {
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  int x = (SCREEN_WIDTH - (int)w) / 2;
  if (x < 0) x = 0;
  display.fillRect(0, 55, SCREEN_WIDTH, 9, SSD1306_BLACK);
  display.setCursor(x, 56);
  display.print(text);
}

void drawLoveStoryCrown(unsigned long now) {
#if LOVE_STORY_MODE && ROYAL_CROWN
  int shine = (now / 260UL) % 2UL;
  int x = 54;
  int y = 1;

  display.drawLine(x, y + 9, x + 20, y + 9, SSD1306_WHITE);
  display.drawLine(x + 1, y + 8, x + 19, y + 8, SSD1306_WHITE);
  display.drawTriangle(x + 1, y + 8, x + 5, y + 1, x + 9, y + 8, SSD1306_WHITE);
  display.drawTriangle(x + 6, y + 8, x + 10, y + 0, x + 14, y + 8, SSD1306_WHITE);
  display.drawTriangle(x + 11, y + 8, x + 15, y + 1, x + 19, y + 8, SSD1306_WHITE);
  if (shine) {
    display.drawPixel(x + 10, y + 12, SSD1306_WHITE);
    display.drawPixel(x + 25, y + 3, SSD1306_WHITE);
    display.drawPixel(x - 5, y + 4, SSD1306_WHITE);
  }
#else
  (void)now;
#endif
}

void drawLoveStoryCaption(unsigned long now) {
#if LOVE_STORY_MODE && SONG_CAPTIONS
  drawCenteredCaption(loveStoryCaption(now));
#else
  (void)now;
#endif
}

bool isHotCondition() {
  return sensors.dhtValid &&
         (sensors.tempC > hotThresholdC || sensors.humidity > HUMIDITY_HOT_PCT);
}

bool isColdCondition() {
  return sensors.dhtValid && sensors.tempC < coldThresholdC;
}

int roundFloatToInt(float value) {
  return (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

const char* temperatureLabel() {
  if (!sensors.dhtValid) return "BACA";
  if (isHotCondition()) return "PANAS";
  if (isColdCondition()) return "DINGIN";
  return "OK";
}

const char* reminderMessage() {
  return reminderText;
}

void setReminderText(const char* text) {
  uint8_t out = 0;
  for (uint8_t i = 0; text[i] != '\0' && out < REMINDER_TEXT_MAX; i++) {
    char c = text[i];
    if (c >= 32 && c <= 126) {
      reminderText[out++] = c;
    }
  }
  reminderText[out] = '\0';
  if (out == 0) {
    strncpy(reminderText, "enroll lagi ya deck", REMINDER_TEXT_MAX);
    reminderText[REMINDER_TEXT_MAX] = '\0';
  }
}

void setReminderSchedule(const char* payload, unsigned long now) {
  if (payload[0] >= '0' && payload[0] <= '2' &&
      payload[1] >= '0' && payload[1] <= '9' &&
      payload[2] == ':' &&
      payload[3] >= '0' && payload[3] <= '5' &&
      payload[4] >= '0' && payload[4] <= '9' &&
      payload[5] == '|') {
    uint8_t hh = (payload[0] - '0') * 10 + (payload[1] - '0');
    uint8_t mm = (payload[3] - '0') * 10 + (payload[4] - '0');
    if (hh < 24 && mm < 60) {
      reminderHour = hh;
      reminderMinute = mm;
      reminderClockMode = true;
      reminderEnabled = true;
      reminder.active = false;
      reminder.activeUntilMs = 0UL;
      reminder.nextAtMs = 0UL;
      reminderLastYday = -1;
      setReminderText(payload + 6);
      webCommand = "REMINDER JAM";
      webCommandUntilMs = now + 900UL;
      return;
    }
  }

  reminderClockMode = false;
  setReminderText(payload);
}

void drawCenteredTextLine(const char* text, int y, uint8_t size) {
  int width = strlen(text) * 6 * size;
  int x = (SCREEN_WIDTH - width) / 2;
  if (x < 0) x = 0;
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(text);
}

void drawReminderMessageBlock(int y) {
  const char* msg = reminderMessage();
  uint8_t len = strlen(msg);
  if (len <= 10) {
    drawCenteredTextLine(msg, y, 2);
    return;
  }

  if (len <= 21) {
    drawCenteredTextLine(msg, y + 4, 1);
    return;
  }

  char line1[22];
  char line2[22];
  uint8_t split = 21;
  for (int i = 20; i > 8; i--) {
    if (msg[i] == ' ') {
      split = i;
      break;
    }
  }
  strncpy(line1, msg, split);
  line1[split] = '\0';
  strncpy(line2, msg + split + (msg[split] == ' ' ? 1 : 0), 21);
  line2[21] = '\0';
  drawCenteredTextLine(line1, y, 1);
  drawCenteredTextLine(line2, y + 10, 1);
}

void drawTemperatureStatus(unsigned long now) {
  (void)now;
  display.fillRect(0, 55, SCREEN_WIDTH, 9, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor(1, 56);

  if (!sensors.dhtValid) {
    display.print("T:--C H:--% ");
    display.print(temperatureLabel());
    return;
  }

  display.print("T:");
  display.print(roundFloatToInt(sensors.tempC));
  display.print("C H:");
  display.print(roundFloatToInt(sensors.humidity));
  display.print("% ");
  display.print(temperatureLabel());
}

void drawReminderOverlay(unsigned long now) {
  if (!reminder.active) return;

  int blink = ((now / 350UL) % 2UL) ? 1 : 0;
  display.fillRect(0, 47, SCREEN_WIDTH, 17, SSD1306_BLACK);
  display.drawRect(0, 47, SCREEN_WIDTH, 17, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor(4, 49);
  display.print("REMINDER");
  display.setCursor(4, 56);
  display.print(reminderMessage());
  display.fillCircle(119, 53, 2 + blink, SSD1306_WHITE);
}

void renderReminderAlert(unsigned long now) {
  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setTextWrap(false);
  display.setCursor(3, 2);
  display.print("REMINDER");
  display.setCursor(92, 2);
  display.print("ALARM");
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  drawReminderMessageBlock(20);
  display.setTextSize(1);
  display.setCursor(18, 45);
  display.print("SENTUH UNTUK OK");
  display.fillCircle(118, 9, 2 + (int)((now / 350UL) % 2UL), SSD1306_BLACK);
  display.display();
}

bool readTouchSensor() {
#if TOUCH_ACTIVE_HIGH
  return digitalRead(TOUCH_PIN) == HIGH;
#else
  return digitalRead(TOUCH_PIN) == LOW;
#endif
}

void updateTouch(unsigned long now) {
  bool raw = readTouchSensor();
  if (raw != touch.rawTouched) {
    touch.rawTouched = raw;
    touch.rawChangedMs = now;
  }

  if (now - touch.rawChangedMs < TOUCH_DEBOUNCE_MS) return;

  if (raw != touch.stableTouched) {
    touch.stableTouched = raw;
    if (touch.stableTouched) {
      touch.pressedAtMs = now;
      touch.longSent = false;
    } else if (!touch.longSent && touch.pendingEvent == TOUCH_NONE) {
      unsigned long heldMs = now - touch.pressedAtMs;
      if (now - touch.strokeWindowMs > 1600UL) {
        touch.strokeWindowMs = now;
        touch.strokeCount = 0;
      }
      touch.strokeCount++;

      if ((heldMs >= 230UL && heldMs < TOUCH_LONG_MS) || touch.strokeCount >= 3) {
        touch.tapWaiting = false;
        touch.pendingEvent = TOUCH_STROKE;
        touch.strokeCount = 0;
        touch.strokeWindowMs = now;
      } else if (activeScreen == SCREEN_MINIGAME && runningGame != GAME_SELECT) {
        touch.pendingEvent = TOUCH_TAP;
      } else if (touch.tapWaiting && now - touch.pendingTapMs <= TOUCH_DOUBLE_TAP_MS) {
        touch.tapWaiting = false;
        touch.pendingEvent = TOUCH_DOUBLE_TAP;
      } else {
        touch.tapWaiting = true;
        touch.pendingTapMs = now;
      }
    }
  }

  if (touch.tapWaiting && now - touch.pendingTapMs > TOUCH_DOUBLE_TAP_MS &&
      touch.pendingEvent == TOUCH_NONE) {
    touch.tapWaiting = false;
    touch.pendingEvent = TOUCH_TAP;
  }

  if (touch.stableTouched && !touch.longSent &&
      now - touch.pressedAtMs >= TOUCH_LONG_MS &&
      touch.pendingEvent == TOUCH_NONE) {
    touch.longSent = true;
    touch.tapWaiting = false;
    touch.pendingEvent = TOUCH_LONG;
  }
}

TouchEvent consumeTouchEvent() {
  TouchEvent event = touch.pendingEvent;
  touch.pendingEvent = TOUCH_NONE;
  return event;
}

void showMenu(MenuPage page, unsigned long now) {
  activeScreen = SCREEN_MENU;
  activeMenu = page;
  menuUntilMs = now + MENU_TIMEOUT_MS;
}

void showTempScreen(unsigned long now) {
  activeScreen = SCREEN_TEMP;
  menuUntilMs = now + MENU_TIMEOUT_MS;
}

void showMinigameScreen(unsigned long now) {
  activeScreen = SCREEN_MINIGAME;
  menuUntilMs = now + MENU_TIMEOUT_MS;
  runningGame = GAME_SELECT;
  activateMood(MOOD_EXCITED, now, 2400);
}

void showFaceScreen() {
  activeScreen = SCREEN_FACE;
  menuUntilMs = 0UL;
}

void showNextScreen(unsigned long now) {
  if (activeScreen == SCREEN_FACE) {
    showMenu(MENU_TEMP_CHECK, now);
    return;
  }

  if (activeScreen == SCREEN_TEMP || activeScreen == SCREEN_MINIGAME) {
    showFaceScreen();
    return;
  }

  if (activeMenu >= MENU_REMINDER) {
    showFaceScreen();
    return;
  }

  showMenu((MenuPage)(activeMenu + 1), now);
}

void incrementHotThreshold() {
  hotThresholdC += 1.0f;
  if (hotThresholdC > 40.0f) hotThresholdC = 26.0f;
  if (hotThresholdC <= coldThresholdC) hotThresholdC = coldThresholdC + 1.0f;
}

void incrementColdThreshold() {
  coldThresholdC += 1.0f;
  if (coldThresholdC >= hotThresholdC) coldThresholdC = 10.0f;
}

void cycleReminderInterval(unsigned long now) {
  uint8_t count = sizeof(REMINDER_INTERVAL_MINUTES) / sizeof(REMINDER_INTERVAL_MINUTES[0]);
  reminderIntervalIndex = (reminderIntervalIndex + 1) % count;
  if (reminderEnabled) scheduleReminder(now, reminderIntervalMs());
}

void toggleReminder(unsigned long now) {
  reminderEnabled = !reminderEnabled;
  reminder.active = false;
  reminder.nextAtMs = reminderEnabled ? now + reminderIntervalMs() : 0UL;
}

void adjustCurrentMenu(unsigned long now) {
  if (activeScreen == SCREEN_TEMP) {
    showMenu(MENU_HOT_LIMIT, now);
    return;
  }

  if (activeScreen != SCREEN_MENU) {
    return;
  }

  switch (activeMenu) {
    case MENU_TEMP_CHECK:
      showTempScreen(now);
      break;
    case MENU_MINIGAME:
      showMinigameScreen(now);
      break;
    case MENU_REMINDER:
      toggleReminder(now);
      showMenu(MENU_REMINDER, now);
      break;
    case MENU_HOT_LIMIT:
      incrementHotThreshold();
      showMenu(MENU_HOT_LIMIT, now);
      break;
    case MENU_COLD_LIMIT:
      incrementColdThreshold();
      showMenu(MENU_COLD_LIMIT, now);
      break;
    case MENU_REMINDER_INTERVAL:
      cycleReminderInterval(now);
      showMenu(MENU_REMINDER_INTERVAL, now);
      break;
    default:
      showMenu(MENU_HOT_LIMIT, now);
      break;
  }
}

void startSelectedGame(unsigned long now) {
  runningGame = selectedGame;
  gameLastMs = now;
  menuUntilMs = now + 60000UL;

  if (runningGame == GAME_PINGPONG) {
    pingPaddleY = 25;
    pingBotPaddleY = 25;
    pingBallX = 76;
    pingBallY = 30;
    pingVelX = -2;
    pingVelY = 1;
    pingPlayerScore = 0;
    pingBotScore = 0;
    pingGameOver = false;
    pingPlayerWon = false;
  } else if (runningGame == GAME_SPACE) {
    shipY = 31;
    bulletX = -1;
    bulletY = 0;
    alienX = 116;
    alienY = 18;
    alienVelY = 1;
    spaceScore = 0;
  }
}

void cycleSelectedGame(unsigned long now) {
  selectedGame = (selectedGame == GAME_PINGPONG) ? GAME_SPACE : GAME_PINGPONG;
  runningGame = GAME_SELECT;
  menuUntilMs = now + MENU_TIMEOUT_MS;
}

void handleGameTap(unsigned long now) {
  menuUntilMs = now + 60000UL;
  if (runningGame == GAME_SELECT) {
    cycleSelectedGame(now);
  } else if (runningGame == GAME_PINGPONG) {
    if (pingGameOver) {
      startSelectedGame(now);
      return;
    }
    if (pingBallY < pingPaddleY + 7) pingPaddleY -= 8;
    else pingPaddleY += 8;
    if (pingPaddleY < 15) pingPaddleY = 15;
    if (pingPaddleY > 45) pingPaddleY = 45;
  } else if (runningGame == GAME_SPACE) {
    shipY += 12;
    if (shipY > 52) shipY = 17;
    if (bulletX < 0) {
      bulletX = 23;
      bulletY = shipY;
    }
  }
}

void handleGameEnter(unsigned long now) {
  if (runningGame == GAME_SELECT) {
    startSelectedGame(now);
  } else {
    runningGame = GAME_SELECT;
    menuUntilMs = now + MENU_TIMEOUT_MS;
  }
}

void handleTouch(unsigned long now) {
  TouchEvent event = consumeTouchEvent();
  if (event == TOUCH_NONE) return;

  if (webFrameActive) {
    webFrameActive = false;
    webCommand = "KELUAR MEDIA";
    webCommandUntilMs = now + 900UL;
    showFaceScreen();
    activateMood(MOOD_CURIOUS, now, 1200);
    return;
  }

  if (reminder.active) {
    acknowledgeReminder(now);
    activateMood(MOOD_RELIEVED, now, 2200);
    return;
  }

  if (activeScreen == SCREEN_MINIGAME) {
    if (event == TOUCH_TAP) {
      handleGameTap(now);
    } else if (event == TOUCH_DOUBLE_TAP) {
      handleGameEnter(now);
    } else if (event == TOUCH_LONG) {
      showFaceScreen();
    }
    return;
  }

  if (event == TOUCH_TAP) {
    if (activeScreen == SCREEN_FACE) {
      showMenu(MENU_TEMP_CHECK, now);
    } else if (activeScreen == SCREEN_MENU) {
      showNextScreen(now);
    } else {
      showFaceScreen();
    }
    return;
  }

  if (event == TOUCH_DOUBLE_TAP) {
    if (activeScreen == SCREEN_FACE) {
      showMenu(MENU_TEMP_CHECK, now);
    } else if (activeScreen == SCREEN_MENU) {
      adjustCurrentMenu(now);
    } else {
      showFaceScreen();
    }
    return;
  }

  if (event == TOUCH_STROKE) {
    static const Mood strokeMoods[] = {
      MOOD_LOVE,
      MOOD_HEART_POP,
      MOOD_RELIEVED,
      MOOD_GIGGLE,
      MOOD_SHY,
      MOOD_SOFT_SMILE
    };

    Mood mood = strokeMoods[touchMoodStep % (sizeof(strokeMoods) / sizeof(strokeMoods[0]))];
    touchMoodStep++;
    activateMood(mood, now, 4200);
    nextIdleEventMs = now + random(5200UL, 9000UL);
    return;
  }

  if (event == TOUCH_LONG && activeScreen == SCREEN_MENU) {
    adjustCurrentMenu(now);
    return;
  }

  if (event == TOUCH_LONG &&
      (activeScreen == SCREEN_TEMP || activeScreen == SCREEN_MINIGAME)) {
    showFaceScreen();
    return;
  }

  if (activeMood == MOOD_SLEEPY && now < moodUntilMs) {
    activateMood(MOOD_RELIEVED, now, 2200);
  } else if (askGeminiMood(now, true)) {
    nextIdleEventMs = now + random(5000UL, 9000UL);
  } else {
    activateMood(MOOD_SLEEPY, now, 5200);
  }
  nextIdleEventMs = now + random(5000UL, 9000UL);
}

void updateMenuTimeout(unsigned long now) {
  if (activeScreen != SCREEN_FACE && now >= menuUntilMs) {
    showFaceScreen();
  }
}

void drawMenuHeader(const char* title, uint8_t index) {
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setTextWrap(false);
  display.setCursor(3, 2);
  display.print(title);
  display.setCursor(108, 2);
  display.print(index);
  display.print("/4");
  display.setTextColor(SSD1306_WHITE);
}

void drawScreenHeader(const char* title, const char* label) {
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setTextWrap(false);
  display.setCursor(3, 2);
  display.print(title);
  display.setCursor(82, 2);
  display.print(label);
  display.setTextColor(SSD1306_WHITE);
}

void drawMenuFooter() {
  display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor(2, 56);
  display.print("MENU");
  if (touch.stableTouched) {
    display.setCursor(88, 56);
    display.print("TOUCH");
  }
}

void drawTempMenu() {
  drawScreenHeader("SUHU", "SENSOR");
  if (!sensors.dhtValid) {
    display.setTextSize(1);
    display.setCursor(20, 28);
    display.print("MEMBACA DHT22");
    drawMenuFooter();
    return;
  }

  display.setTextSize(2);
  display.setCursor(4, 18);
  display.print(roundFloatToInt(sensors.tempC));
  display.print("C");

  display.setTextSize(1);
  display.setCursor(70, 18);
  display.print("H:");
  display.print(roundFloatToInt(sensors.humidity));
  display.print("%");
  display.setCursor(70, 31);
  display.print(temperatureLabel());
  display.setCursor(4, 43);
  display.print("Panas>");
  display.print(roundFloatToInt(hotThresholdC));
  display.print(" Dingin<");
  display.print(roundFloatToInt(coldThresholdC));
  drawMenuFooter();
}

void drawMinigameScreen(unsigned long now) {
  drawScreenHeader("MINIGAME", runningGame == GAME_SELECT ? "2x GO" : "PLAY");
  display.setTextSize(1);

  if (runningGame == GAME_SELECT) {
    bool ping = selectedGame == GAME_PINGPONG;
    display.fillRoundRect(4, 17, 120, 15, 3, ping ? SSD1306_WHITE : SSD1306_BLACK);
    display.setTextColor(ping ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(12, 21);
    display.print("> Pingpong");
    display.setCursor(90, 21);
    display.print("REFLEX");

    display.fillRoundRect(4, 36, 120, 15, 3, !ping ? SSD1306_WHITE : SSD1306_BLACK);
    display.setTextColor(!ping ? SSD1306_BLACK : SSD1306_WHITE);
    display.setCursor(12, 40);
    display.print("> Pesawat");
    display.setCursor(91, 40);
    display.print("ALIEN");
    display.setTextColor(SSD1306_WHITE);
    display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
    display.setCursor(2, 56);
    display.print("Tap pilih  2x mulai");
    return;
  }

  if (runningGame == GAME_PINGPONG) {
    if (pingGameOver) {
      display.setTextColor(SSD1306_WHITE);
      display.drawRoundRect(6, 15, 116, 45, 5, SSD1306_WHITE);
      display.setTextSize(2);
      display.setCursor(pingPlayerWon ? 20 : 14, 21);
      display.print(pingPlayerWon ? "YOU WIN" : "YOU LOSE");
      display.setTextSize(1);
      display.setCursor(31, 43);
      display.print("YOU ");
      display.print(pingPlayerScore);
      display.print(" - ");
      display.print(pingBotScore);
      display.print(" BOT");
      return;
    }

    if (now - gameLastMs > 28UL) {
      gameLastMs = now;

      int botCenter = pingBotPaddleY + 8;
      int botTarget = pingBallY + ((pingVelX > 0) ? pingVelY * 5 : 0);
      if (botTarget < botCenter - 2) pingBotPaddleY -= 2;
      if (botTarget > botCenter + 2) pingBotPaddleY += 2;
      if (pingBotPaddleY < 15) pingBotPaddleY = 15;
      if (pingBotPaddleY > 45) pingBotPaddleY = 45;

      pingBallX += pingVelX;
      pingBallY += pingVelY;
      if (pingBallY <= 15 || pingBallY >= 61) pingVelY = -pingVelY;

      if (pingBallX >= 116 && pingBallY >= pingBotPaddleY && pingBallY <= pingBotPaddleY + 17) {
        pingVelX = -abs(pingVelX);
        if (random(0, 4) == 0) pingVelY += (pingVelY > 0 ? 1 : -1);
      }

      if (pingBallX <= 13 && pingBallY >= pingPaddleY && pingBallY <= pingPaddleY + 16) {
        pingVelX = abs(pingVelX);
        if (random(0, 3) == 0) pingVelY += (pingVelY > 0 ? 1 : -1);
      }
      if (pingVelY > 3) pingVelY = 3;
      if (pingVelY < -3) pingVelY = -3;

      if (pingBallX < 0) {
        pingBotScore++;
        if (pingBotScore >= PING_WIN_SCORE) {
          pingGameOver = true;
          pingPlayerWon = false;
        } else {
          pingBallX = 72;
          pingBallY = 30;
          pingVelX = 2;
          pingVelY = (random(0, 2) == 0) ? -1 : 1;
        }
      }
      if (pingBallX > 127) {
        pingPlayerScore++;
        if (pingPlayerScore >= PING_WIN_SCORE) {
          pingGameOver = true;
          pingPlayerWon = true;
        } else {
          pingBallX = 56;
          pingBallY = 30;
          pingVelX = -2;
          pingVelY = (random(0, 2) == 0) ? -1 : 1;
        }
      }
    }

    display.drawRect(0, 13, 128, 51, SSD1306_WHITE);
    display.fillRoundRect(6, pingPaddleY, 5, 17, 2, SSD1306_WHITE);
    display.fillRoundRect(117, pingBotPaddleY, 5, 17, 2, SSD1306_WHITE);
    display.fillCircle(pingBallX, pingBallY, 2, SSD1306_WHITE);
    display.setCursor(5, 3);
    display.print("YOU");
    display.setCursor(31, 3);
    display.print(pingPlayerScore);
    display.setCursor(88, 3);
    display.print(pingBotScore);
    display.setCursor(106, 3);
    display.print("BOT");
    return;
  }

  if (runningGame == GAME_SPACE) {
    if (now - gameLastMs > 40UL) {
      gameLastMs = now;
      alienY += alienVelY;
      if (alienY <= 17 || alienY >= 55) alienVelY = -alienVelY;
      alienX -= 1;
      if (alienX < 32) {
        alienX = 116;
        alienY = random(18, 54);
      }
      if (bulletX >= 0) bulletX += 4;
      if (bulletX > 127) bulletX = -1;
      if (bulletX >= alienX - 2 && bulletX <= alienX + 8 &&
          bulletY >= alienY - 5 && bulletY <= alienY + 5) {
        spaceScore++;
        bulletX = -1;
        alienX = 116;
        alienY = random(18, 54);
      }
    }

    display.drawLine(0, 13, 127, 13, SSD1306_WHITE);
    display.fillTriangle(6, shipY, 21, shipY - 6, 21, shipY + 6, SSD1306_WHITE);
    display.drawPixel(26, shipY - 2, SSD1306_WHITE);
    display.drawPixel(30, shipY + 3, SSD1306_WHITE);
    display.fillCircle(alienX, alienY, 4, SSD1306_WHITE);
    display.drawPixel(alienX - 2, alienY - 1, SSD1306_BLACK);
    display.drawPixel(alienX + 2, alienY - 1, SSD1306_BLACK);
    if (bulletX >= 0) display.drawFastHLine(bulletX, bulletY, 6, SSD1306_WHITE);
    display.setCursor(5, 3);
    display.print("PESAWAT");
    display.setCursor(91, 3);
    display.print("S:");
    display.print(spaceScore);
  }
}

void drawMainMenuItem(uint8_t row, MenuPage page, const char* label, const char* hint) {
  int y = 15 + row * 10;
  bool selected = activeMenu == page;
  if (selected) {
    display.fillRoundRect(2, y - 1, 124, 9, 2, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
    display.drawPixel(6, y + 3, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(8, y);
  display.print(selected ? ">" : " ");
  display.setCursor(17, y);
  display.print(label);
  display.setCursor(92, y);
  display.print(hint);
  display.setTextColor(SSD1306_WHITE);
}

void drawMainMenu(unsigned long now) {
  uint8_t dots = (now / 300UL) % 4UL;
  drawScreenHeader("MENU", "2x OK");
  drawMainMenuItem(0, MENU_TEMP_CHECK, "Cek suhu", "DHT");
  drawMainMenuItem(1, MENU_MINIGAME, "Minigames", "PLAY");
  drawMainMenuItem(2, MENU_REMINDER, "Reminder", reminderEnabled ? "ON" : "OFF");
  display.drawLine(0, 54, SCREEN_WIDTH, 54, SSD1306_WHITE);
  display.setCursor(2, 56);
  display.print("Tap geser  2x enter");
  for (uint8_t i = 0; i < dots; i++) {
    display.drawPixel(119 + i * 3, 59, SSD1306_WHITE);
  }
}

void drawHotLimitMenu() {
  drawMenuHeader("BATAS PANAS", 1);
  display.setTextSize(2);
  display.setCursor(28, 20);
  display.print(roundFloatToInt(hotThresholdC));
  display.print("C");
  display.setTextSize(1);
  display.setCursor(15, 43);
  display.print("Lembap>");
  display.print(roundFloatToInt(HUMIDITY_HOT_PCT));
  display.print("% tetap PANAS");
  drawMenuFooter();
}

void drawColdLimitMenu() {
  drawMenuHeader("BATAS DINGIN", 2);
  display.setTextSize(2);
  display.setCursor(28, 20);
  display.print(roundFloatToInt(coldThresholdC));
  display.print("C");
  display.setTextSize(1);
  display.setCursor(18, 43);
  display.print("Rentang 10C sampai panas");
  drawMenuFooter();
}

void drawReminderMenu(unsigned long now) {
  drawMenuHeader("REMINDER", 3);
  display.setTextSize(2);
  display.setCursor(18, 18);
  display.print(reminderEnabled ? "AKTIF" : "MATI");
  display.setTextSize(1);
  display.setCursor(4, 42);
  if (!reminderEnabled) {
    display.print("Tidak ada alarm");
  } else if (reminder.active) {
    display.print(reminderMessage());
  } else if (reminderClockMode) {
    display.print("Jam ");
    if (reminderHour < 10) display.print("0");
    display.print(reminderHour);
    display.print(":");
    if (reminderMinute < 10) display.print("0");
    display.print(reminderMinute);
  } else if (reminder.nextAtMs > now) {
    unsigned long remainSec = (reminder.nextAtMs - now) / 1000UL;
    display.print("Berikutnya ");
    display.print(remainSec);
    display.print("s");
  } else {
    display.print("Menunggu jadwal");
  }
  drawMenuFooter();
}

void drawReminderIntervalMenu() {
  drawMenuHeader("JEDA", 4);
  display.setTextSize(2);
  display.setCursor(14, 20);
  display.print(REMINDER_INTERVAL_MINUTES[reminderIntervalIndex]);
  display.print(" MENIT");
  display.setTextSize(1);
  display.setCursor(24, 43);
  display.print("Reminder minum/rehat");
  drawMenuFooter();
}

void renderMenu(unsigned long now) {
  display.clearDisplay();

  switch (activeMenu) {
    case MENU_TEMP_CHECK:
    case MENU_MINIGAME:
    case MENU_REMINDER:
      drawMainMenu(now);
      break;
    case MENU_HOT_LIMIT:
      drawHotLimitMenu();
      break;
    case MENU_COLD_LIMIT:
      drawColdLimitMenu();
      break;
    case MENU_REMINDER_INTERVAL:
      drawReminderIntervalMenu();
      break;
    default:
      break;
  }

  display.display();
}

void renderTempScreen() {
  display.clearDisplay();
  drawTempMenu();
  display.display();
}

void renderWebCommand(unsigned long now) {
  if (webCommand.length() == 0 || now >= webCommandUntilMs) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor(13, 18);
  display.print("WEB CONTROL");
  display.setCursor(12, 36);
  display.print(webCommand);
  display.display();
}

void renderSensorTest(unsigned long now) {
#if I2C_SCAN_TEST
  if (now - lastI2cScanMs > 1500UL) {
    foundI2cCount = 0;
    for (uint8_t addr = 1; addr < 127 && foundI2cCount < 8; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        foundI2cAddrs[foundI2cCount++] = addr;
      }
    }
    lastI2cScanMs = now;
  }
#endif

  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, 12, SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setTextWrap(false);
  display.setCursor(3, 2);
  display.print("CEK SENSOR");
  display.setCursor(94, 2);
  display.print((now / 500UL) % 2UL ? "*" : " ");

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 14);
  display.print("DHT22: ");
  display.print(sensors.dhtValid ? "OK" : "BACA/ERR");
  display.setCursor(0, 24);
  if (sensors.dhtValid) {
    display.print("T:");
    display.print(sensors.tempC, 1);
    display.print("C H:");
    display.print(sensors.humidity, 0);
    display.print("%");
  } else {
    display.print("+:3V3 OUT:D2 -:GND");
  }

  display.setCursor(0, 35);
  display.print("MPU6050: ");
  display.print(sensors.mpuReady ? "OK" : "ERR");
  display.setCursor(0, 45);
#if I2C_SCAN_TEST
  display.print("I2C:");
  if (foundI2cCount == 0) {
    display.print(" none");
  } else {
    for (uint8_t i = 0; i < foundI2cCount; i++) {
      display.print(" 0x");
      if (foundI2cAddrs[i] < 16) display.print("0");
      display.print(foundI2cAddrs[i], HEX);
    }
  }
#else
  display.print("SDA:D4 SCL:D5 3V3/GND");
#endif

  display.setCursor(0, 56);
  if (sensors.mpuReady) {
    display.print("X:");
    display.print(sensors.tiltX, 1);
    display.print(" Y:");
    display.print(sensors.tiltY, 1);
    display.print(" M:");
    display.print(sensors.motion, 1);
  } else {
    display.print("Need: MPU 0x68/0x69");
  }

  display.display();
}

void renderFace(unsigned long now) {
  blendPose(targetPose(now));

  display.clearDisplay();
  drawPill(currentPose.eyeLX, currentPose.eyeLY, currentPose.eyeW, currentPose.eyeLH, currentPose.blinkL);
  drawPill(currentPose.eyeRX, currentPose.eyeRY, currentPose.eyeW, currentPose.eyeRH, currentPose.blinkR);
  drawBrows(currentPose);

  if (currentPose.mouthOpen > 0.18f) {
    drawMouthO(currentPose.mouthX, currentPose.mouthY + 4.0f, currentPose.mouthOpen);
  } else {
    drawSmile(currentPose.mouthX, currentPose.mouthY, currentPose.mouthW, currentPose.mouthDepth);
  }

  drawBlush(currentPose);
  drawSweat(currentPose);
  drawShiver(currentPose, now);
  drawQuestion(currentPose, now);
  drawZzz(currentPose, now);
  drawSparkle(currentPose, now);
  drawSteam(currentPose, now);
  drawTear(currentPose, now);
  drawStress(currentPose, now);
  drawProudMark(currentPose);
  drawDizzyMarks(currentPose, now);
  drawHearts(currentPose, now);
  drawCoolShade(currentPose);
  drawLoveStoryCrown(now);
  drawLoveMeNotCaption(now);
  drawLoveStoryCaption(now);
  display.display();
}

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("BOOT OWI");
  Wire.begin(D4, D5);
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);

  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("OLED BEGIN FAIL");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("OLED OK");

  setupWebServer();

  mpuAdafruitReady = mpu.begin(0x68, &Wire);
  if (mpuAdafruitReady) {
    mpuI2cAddress = 0x68;
  } else {
    mpuAdafruitReady = mpu.begin(0x69, &Wire);
    if (mpuAdafruitReady) mpuI2cAddress = 0x69;
  }

  sensors.mpuReady = mpuAdafruitReady || initRawMpu(0x68) || initRawMpu(0x69);
  if (sensors.mpuReady) {
    if (mpuAdafruitReady) {
      mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
      mpu.setGyroRange(MPU6050_RANGE_500_DEG);
      mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    }
  }

  dht.begin();
  randomSeed((uint32_t)micros());
  nextIdleEventMs = millis() + random(2500UL, 6000UL);
  reminder.nextAtMs = millis() + DEFAULT_REMINDER_INTERVAL_MS;
  currentPose = basePose();
  poseReady = true;
}

void loop() {
  unsigned long now = millis();
  updateSensors(now);
  updateWifi(now);
  webServer.handleClient();
  updateSerialFrame(now);

#if SENSOR_TEST_MODE
  renderSensorTest(now);
  static unsigned long lastSerialMs = 0UL;
  if (now - lastSerialMs >= 1000UL) {
    lastSerialMs = now;
    Serial.print("DHT=");
    Serial.print(sensors.dhtValid ? "OK" : "ERR");
    Serial.print(" T=");
    Serial.print(sensors.tempC, 1);
    Serial.print(" H=");
    Serial.print(sensors.humidity, 0);
    Serial.print(" MPU=");
    Serial.print(sensors.mpuReady ? "OK" : "ERR");
    Serial.print(" X=");
    Serial.print(sensors.tiltX, 2);
    Serial.print(" Y=");
    Serial.print(sensors.tiltY, 2);
    Serial.print(" M=");
    Serial.println(sensors.motion, 2);
  }
  delay(100);
  return;
#endif

  updateTouch(now);
  handleTouch(now);
#if GEMINI_AI_MODE && GEMINI_AUTO_MODE
  askGeminiMood(now);
#endif
  if (webFrameActive) {
    renderWebFrame(now);
    delay(FRAME_MS);
    return;
  }
  if (webCommand.length() > 0 && now < webCommandUntilMs) {
    renderWebCommand(now);
    delay(FRAME_MS);
    return;
  }
  if (activeScreen == SCREEN_TEMP) {
    renderTempScreen();
    updateMenuTimeout(now);
    delay(FRAME_MS);
    return;
  }
  if (activeScreen == SCREEN_MINIGAME) {
    display.clearDisplay();
    drawMinigameScreen(now);
    display.display();
    updateMenuTimeout(now);
    delay(FRAME_MS);
    return;
  }
  if (activeScreen == SCREEN_MENU) {
    renderMenu(now);
    updateMenuTimeout(now);
    delay(FRAME_MS);
    return;
  }
  renderFace(now);
  delay(FRAME_MS);
}
