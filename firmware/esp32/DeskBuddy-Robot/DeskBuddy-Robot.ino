// ============================================================
//  DeskBuddy Robot — Minimal 5-Gesture Companion Firmware
//  Gestures:
//  1 Tap — Love Animation ❤️ (heart eyes + floating hearts)
//  2 Taps — Timer & Stopwatch ⏱️ (minimal menu + large digits)
//  3 Taps — Crazy Face 🤪 (star eyes + screen shake)
//  4 Taps — Clock Format 🕒 (12H, 12H+sec, 24H, 24H+sec — persisted)
//  5 Taps — Time Synchronization 🔄 (background Wi-Fi + NTP sync)
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>

// Wi-Fi Configuration
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define DESKBUDDY_WIFI_SSID ""
#define DESKBUDDY_WIFI_PASSWORD ""
#endif

// ============================================================
//  PIN MAPPING (ESP32-C3 DevKitM-1)
// ============================================================
#define SDA_PIN          8   // I2C data  → SH1106 OLED
#define SCL_PIN          9   // I2C clock → SH1106 OLED
#define BUZZER_PIN       5   // Passive piezo buzzer
#define TOUCH_PIN        7   // Touch sensor / push button
#define TOUCH_ACTIVE_LOW 0   // 0 = active HIGH, 1 = active LOW

// BLE Configuration
static const char* BLE_DEVICE_NAME       = "DeskBuddy";
static const char* BLE_TIME_SERVICE_UUID = "a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001";
static const char* BLE_TIME_CHAR_UUID    = "a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001";

// Hardware Objects
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences settings;

// ============================================================
//  STATE MACHINE
// ============================================================
enum State {
  STATE_HOME,        // Minimal Home: Animated eyes + small clock
  STATE_LOVE,        // 1 Tap: Cute love animation (heart eyes + floating hearts)
  STATE_TIMER_MENU,  // 2 Taps: Minimal 2-item menu (Pomodoro / Stopwatch)
  STATE_POMODORO,    // Large digit countdown timer
  STATE_STOPWATCH,   // Large digit millisecond stopwatch
  STATE_CRAZY        // 3 Taps: Crazy funny face (star eyes + screen shake)
};

State currentState = STATE_HOME;
unsigned long stateStartTime = 0;

// ============================================================
//  CLOCK & TIMEZONE (IST UTC+05:30)
//  clockFormat: 0=12H, 1=12H+sec (default), 2=24H, 3=24H+sec
// ============================================================
int clockFormat = 1;
bool clockSynchronized = false;

// Sync status indicators (0=idle, 1=syncing, 2=success, 3=failed)
enum SyncIndicator { SYNC_IDLE = 0, SYNC_RUNNING, SYNC_SUCCESS, SYNC_FAILED };
SyncIndicator syncIndicator = SYNC_IDLE;
unsigned long syncIndicatorTime = 0;

// ============================================================
//  TIMER & STOPWATCH DATA
// ============================================================
int menuSelection = 0; // 0 = Pomodoro, 1 = Stopwatch

// Pomodoro Timer (Timestamp-based)
const int pomodoroOptions[] = {50, 25, 10, 5, 1}; // in minutes
int currentOptionIndex      = 1;                  // Default: 25 minutes
long pomodoroDurationSec    = 25 * 60;
bool pomodoroRunning        = false;
unsigned long pomodoroRunStartedAt  = 0;
unsigned long pomodoroAccumulatedMs = 0;
bool pomodoroCompleted      = false;
uint32_t lastWarningSec     = 0;

uint32_t getPomodoroRemainingSeconds(unsigned long now) {
  if (pomodoroCompleted) return 0;
  unsigned long elapsedMs = pomodoroAccumulatedMs;
  if (pomodoroRunning) {
    elapsedMs += (now - pomodoroRunStartedAt);
  }
  long elapsedSec = elapsedMs / 1000UL;
  if (elapsedSec >= pomodoroDurationSec) {
    return 0;
  }
  return (uint32_t)(pomodoroDurationSec - elapsedSec);
}

// Persistent Stopwatch (Timestamp-based)
uint64_t stopwatchAccumulatedMs     = 0;
unsigned long stopwatchRunStartedAt = 0;
bool stopwatchRunning               = false;

uint64_t getStopwatchTotalMs(unsigned long now) {
  return stopwatchAccumulatedMs + (stopwatchRunning ? (uint32_t)(now - stopwatchRunStartedAt) : 0);
}

void saveStopwatch() {
  settings.putULong64("sw_ms", stopwatchAccumulatedMs);
}

// ============================================================
//  BUZZER (NON-BLOCKING SCHEDULE)
// ============================================================
void beepClick(int durationMs = 8) {
  digitalWrite(BUZZER_PIN, HIGH);
  delayMicroseconds(durationMs * 100);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepAlarmPulse(int pitchUs) {
  for (int i = 0; i < 30; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(pitchUs);
    digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(pitchUs);
  }
}

void beepTapFeedback(int tapNum) {
  const uint16_t halfPeriodsUs[] = {1000, 850, 720, 620, 520};
  uint16_t halfPeriod = halfPeriodsUs[constrain(tapNum - 1, 0, 4)];
  for (int cycle = 0; cycle < 10; cycle++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(halfPeriod);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(halfPeriod);
  }
}

// ============================================================
//  TOUCH & 5-GESTURE STATE MACHINE
// ============================================================
unsigned long touchStartTime = 0;
unsigned long lastTapRelease = 0;
int tapCount = 0;

bool touchCurrentState   = LOW;
bool touchDebouncedState = LOW;
unsigned long touchLastDebounceTime = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 25;

bool longPressHandled = false;
const unsigned long LONG_PRESS_MS = 800;
const unsigned long TAP_GAP_MS    = 420;
const unsigned long FRAME_INTERVAL_MS = 16;
unsigned long lastFrameAt = 0;

// ============================================================
//  BACKGROUND ASYNC WI-FI & NTP SYNC (FreeRTOS Task)
// ============================================================
TaskHandle_t networkTaskHandle = NULL;
volatile bool netSyncRequested = false;
volatile bool netTaskBusy       = false;

portMUX_TYPE bleTimeMux = portMUX_INITIALIZER_UNLOCKED;
volatile int64_t pendingBleEpoch = 0;

void networkWorkerTask(void* parameter) {
  for (;;) {
    if (netSyncRequested) {
      netSyncRequested = false;
      netTaskBusy = true;
      syncIndicator = SYNC_RUNNING;

      const char* ssid = DESKBUDDY_WIFI_SSID;
      const char* pass = DESKBUDDY_WIFI_PASSWORD;

      if (strlen(ssid) > 0) {
        Serial.println("[NET] Connecting Wi-Fi...");
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.begin(ssid, pass);

        unsigned long startConnect = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startConnect < 8000)) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[NET] Connected. Synchronizing NTP...");
          sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
          configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

          unsigned long startNtp = millis();
          struct tm timeinfo = {};
          bool success = false;

          while (millis() - startNtp < 5000) {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
                getLocalTime(&timeinfo, 100) &&
                timeinfo.tm_year >= (2024 - 1900)) {
              success = true;
              break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
          }

          if (success) {
            Serial.println("[NET] NTP synchronized successfully.");
            clockSynchronized = true;
            syncIndicator = SYNC_SUCCESS;
          } else {
            Serial.println("[NET] NTP sync timed out.");
            syncIndicator = SYNC_FAILED;
          }
        } else {
          Serial.println("[NET] Wi-Fi connection failed.");
          syncIndicator = SYNC_FAILED;
        }

        // Always power down Wi-Fi radio immediately after sync
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      } else {
        Serial.println("[NET] No Wi-Fi SSID configured.");
        syncIndicator = SYNC_FAILED;
      }

      syncIndicatorTime = millis();
      netTaskBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void triggerNetworkSync() {
  if (!netTaskBusy) {
    netSyncRequested = true;
  }
}

// ============================================================
//  BLUETOOTH LOW ENERGY (NimBLE Time Sync)
// ============================================================
class DeskBuddyTimeCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    std::string raw = characteristic->getValue();
    if (raw.empty()) return;
    char* end = nullptr;
    long long supplied = strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || (*end != '\0' && *end != '\n' && *end != '\r')) return;
    if (supplied > 100000000000LL) supplied /= 1000;
    if (supplied < 1700000000LL || supplied > 4102444800LL) return;

    portENTER_CRITICAL(&bleTimeMux);
    pendingBleEpoch = supplied;
    portEXIT_CRITICAL(&bleTimeMux);
  }
};

class DeskBuddyServerCallbacks : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer* pServer) override {
    NimBLEDevice::startAdvertising(); // Auto-restart advertising
  }
};

void startBleService() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new DeskBuddyServerCallbacks());

  NimBLEService* service = server->createService(BLE_TIME_SERVICE_UUID);
  NimBLECharacteristic* timeChar = service->createCharacteristic(
    BLE_TIME_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  timeChar->setCallbacks(new DeskBuddyTimeCallbacks());
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_TIME_SERVICE_UUID);
  advertising->setScanResponse(true);
  NimBLEDevice::startAdvertising();
}

// ============================================================
//  OLED GRAPHICS HELPERS
// ============================================================
void drawTinyHeart(int x, int y, int size) {
  int lobe = max(1, size / 2);
  u8g2.drawDisc(x - lobe, y, lobe);
  u8g2.drawDisc(x + lobe, y, lobe);
  u8g2.drawTriangle(x - size, y, x + size, y, x, y + size + 2);
}

void drawHeartEye(int x, int y, int sz, bool closed, int pupilOffset) {
  if (closed) {
    u8g2.drawCircle(x, y - 4, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.drawCircle(x, y - 5, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  } else {
    u8g2.drawDisc(x - (sz / 2), y, sz / 2);
    u8g2.drawDisc(x + (sz / 2), y, sz / 2);
    u8g2.drawTriangle(x - sz, y + 1, x + sz, y + 1, x, y + sz + 8);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + pupilOffset, y + 2, 3);
    u8g2.setDrawColor(1);
    u8g2.drawPixel(x - (sz / 2) - 1, y - 2);
    u8g2.drawPixel(x - 3, y - 4);
  }
}

void drawStarEye(int x, int y, int radius) {
  u8g2.drawRBox(x - radius, y - radius, radius * 2, radius * 2, 4);
  u8g2.setDrawColor(0);
  if ((millis() / 60) % 2 == 0) {
    u8g2.drawBox(x - 6, y - 1, 12, 2); u8g2.drawBox(x - 1, y - 6, 2, 12);
  } else {
    u8g2.drawLine(x - 5, y - 5, x + 5, y + 5);
    u8g2.drawLine(x + 5, y - 5, x - 5, y + 5);
  }
  u8g2.setDrawColor(1);
}

// ============================================================
//  SCREEN 1: MINIMAL HOME (ANIMATED EYES + SMALL CLOCK)
// ============================================================
void drawHome(unsigned long now) {
  // ── A. Animated Robot Eyes (Upper Portion: Y = 2 to 46) ──
  static unsigned long lastBlinkTime = 0;
  static bool blinking = false;
  static unsigned long blinkStartTime = 0;

  if (!blinking && (now - lastBlinkTime > 3200)) {
    blinking = true;
    blinkStartTime = now;
    lastBlinkTime = now;
  }

  float closure = 0.0f;
  if (blinking) {
    unsigned long bElapsed = now - blinkStartTime;
    if (bElapsed < 100) closure = (float)bElapsed / 100.0f;
    else if (bElapsed < 180) closure = 1.0f;
    else if (bElapsed < 280) closure = 1.0f - ((float)(bElapsed - 180) / 100.0f);
    else blinking = false;
  }

  // Gentle idle gaze and breath scale
  int gazeOffset = (int)(sinf(now * 0.0015f) * 4.0f);
  int eyeHeight = (int)((1.0f - closure) * 26.0f);
  if (eyeHeight < 2) eyeHeight = 2;

  // Left Eye (centered at X=36, Y=22)
  u8g2.drawRBox(36 - 15, 22 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));
  // Right Eye (centered at X=92, Y=22)
  u8g2.drawRBox(92 - 15, 22 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));

  // Pupils (cut out of open eyes)
  if (closure < 0.35f) {
    u8g2.setDrawColor(0);
    u8g2.drawRBox(36 - 3 + gazeOffset, 20, 6, 8, 1);
    u8g2.drawRBox(92 - 3 + gazeOffset, 20, 6, 8, 1);
    u8g2.setDrawColor(1);
  }

  // Divider line
  u8g2.drawLine(16, 46, 112, 46);

  // ── B. Small Centered Clock (Lower Portion: Y = 48 to 64) ──
  struct tm ti = {};
  bool timeValid = clockSynchronized && getLocalTime(&ti);

  char timeStr[20];
  if (!timeValid) {
    if (clockFormat == 1 || clockFormat == 3) strncpy(timeStr, "--:--:--", sizeof(timeStr));
    else strncpy(timeStr, "--:--", sizeof(timeStr));
  } else {
    int h12 = ti.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    const char* suffix = (ti.tm_hour >= 12) ? "PM" : "AM";
    const char* sChar  = (ti.tm_hour >= 12) ? "P"  : "A";

    if (clockFormat == 0) {
      snprintf(timeStr, sizeof(timeStr), "%d:%02d %s", h12, ti.tm_min, suffix);
    } else if (clockFormat == 1) {
      snprintf(timeStr, sizeof(timeStr), "%d:%02d:%02d %s", h12, ti.tm_min, ti.tm_sec, sChar);
    } else if (clockFormat == 2) {
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    }
  }

  u8g2.setFont(u8g2_font_helvB10_tf);
  int clockWidth = u8g2.getStrWidth(timeStr);
  int clockX = (128 - clockWidth) / 2;
  u8g2.drawStr(clockX, 60, timeStr);

  // Small unobtrusive sync status indicator
  if (syncIndicator == SYNC_RUNNING) {
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(108, 60, "SYNC");
  } else if (syncIndicator == SYNC_SUCCESS && (now - syncIndicatorTime < 3000)) {
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(112, 60, "OK");
  } else if (syncIndicator == SYNC_FAILED && (now - syncIndicatorTime < 3000)) {
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(112, 60, "ERR");
  } else {
    syncIndicator = SYNC_IDLE;
  }
}

// ============================================================
//  SCREEN 2: 1 TAP — LOVE ANIMATION (HEART EYES + FLOATING HEARTS)
// ============================================================
void drawLoveAnimation(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;

  // Heart pulse size using smooth sine beat
  float beat = sinf(elapsed * 0.007f);
  int heartSize = 14 + (int)(beat * 3.0f);

  // Pulsing Heart Eyes
  drawHeartEye(36, 26, heartSize, false, 0);
  drawHeartEye(92, 26, heartSize, false, 0);

  // Floating Tiny Hearts drifting upward
  for (int i = 0; i < 4; i++) {
    unsigned long phase = (elapsed + (unsigned long)i * 650) % 2400;
    int y = 60 - (int)(phase * 52 / 2400);
    int x = 64 + (int)(sinf((elapsed + i * 400) * 0.005f) * 18.0f) + (i % 2 == 0 ? -15 : 15);
    drawTinyHeart(x, y, 3);
  }

  // Heartbeat sound pulse
  if (elapsed % 750 < 20 || (elapsed % 750 > 130 && elapsed % 750 < 150)) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(140);
    digitalWrite(BUZZER_PIN, LOW);
  }

  // Return automatically to HOME after 3.2 seconds
  if (elapsed > 3200) {
    currentState = STATE_HOME;
  }
}

// ============================================================
//  SCREEN 3: 2 TAPS — MINIMAL TIMER & STOPWATCH MENU
// ============================================================
void drawTimerMenu() {
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.drawStr(14, 9, "TIMER & STOPWATCH");
  u8g2.setDrawColor(1);

  const char* items[] = {
    "1. POMODORO TIMER",
    "2. STOPWATCH"
  };

  u8g2.setFont(u8g2_font_7x14_tf);
  for (int i = 0; i < 2; i++) {
    int y = 28 + (i * 15);
    if (menuSelection == i) {
      u8g2.drawRBox(4, y - 10, 120, 13, 2);
      u8g2.setDrawColor(0);
      u8g2.drawStr(8, y, items[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(8, y, items[i]);
    }
  }

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.drawStr(4, 61, "1:NEXT   2/HOLD:SELECT   3:HOME");
}

// ============================================================
//  SCREEN 4: POMODORO TIMER (LARGE DIGITS)
// ============================================================
void drawPomodoroScreen(unsigned long now) {
  uint32_t remainingSec = getPomodoroRemainingSeconds(now);

  // Top Bar: Duration & Small Running/Paused Indicator
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  char titleBuf[24];
  snprintf(titleBuf, sizeof(titleBuf), "POMO %dM", pomodoroOptions[currentOptionIndex]);
  u8g2.drawStr(4, 9, titleBuf);

  const char* status = pomodoroRunning ? "RUNNING" :
                       (pomodoroAccumulatedMs > 0 ? "PAUSED" : "READY");
  u8g2.drawStr(86, 9, status);
  u8g2.drawLine(0, 12, 128, 12);

  // Large Readable Countdown Digits MM:SS
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", remainingSec / 60UL, remainingSec % 60UL);
  u8g2.setFont(u8g2_font_logisoso28_tn);
  int w = u8g2.getStrWidth(timeBuf);

  // Visual flash on final 5 seconds
  if (remainingSec <= 5 && remainingSec > 0 && pomodoroRunning && ((now / 250) % 2 == 0)) {
    u8g2.setDrawColor(1);
    u8g2.drawRBox(10, 16, 108, 34, 3);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr((128 - w) / 2, 47, timeBuf);
  u8g2.setDrawColor(1);

  // Minimal Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  const char* toggleCmd = pomodoroRunning ? "1:PAUSE" : (pomodoroAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footBuf[36];
  snprintf(footBuf, sizeof(footBuf), "%s 2:RST 3:DUR", toggleCmd);
  u8g2.drawStr(2, 61, footBuf);
  u8g2.drawStr(96, 61, "H:MENU");
}

// ============================================================
//  SCREEN 5: STOPWATCH (LARGE DIGITS)
// ============================================================
void drawStopwatchScreen(unsigned long now) {
  uint64_t totalMs = getStopwatchTotalMs(now);
  uint64_t totalSec = totalMs / 1000ULL;
  uint64_t hours = totalSec / 3600ULL;
  uint64_t minutes = (totalSec / 60ULL) % 60ULL;
  uint64_t seconds = totalSec % 60ULL;

  // Top Bar: Title & Small Running/Paused Indicator
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(4, 9, "STOPWATCH");
  const char* status = stopwatchRunning ? "RUNNING" :
                       (stopwatchAccumulatedMs > 0 ? "PAUSED" : "READY");
  u8g2.drawStr(86, 9, status);
  u8g2.drawLine(0, 12, 128, 12);

  // Large Readable Elapsed Digits
  char watchBuf[16];
  if (hours > 0) {
    snprintf(watchBuf, sizeof(watchBuf), "%02llu:%02llu:%02llu",
             (unsigned long long)(hours % 100ULL),
             (unsigned long long)minutes, (unsigned long long)seconds);
    u8g2.setFont(u8g2_font_logisoso20_tn);
    int w = u8g2.getStrWidth(watchBuf);
    u8g2.drawStr((128 - w) / 2, 45, watchBuf);
  } else {
    // MM:SS format with tenths of a second
    uint64_t tenths = (totalMs % 1000ULL) / 100ULL;
    snprintf(watchBuf, sizeof(watchBuf), "%02llu:%02llu.%llu",
             (unsigned long long)minutes, (unsigned long long)seconds, (unsigned long long)tenths);
    u8g2.setFont(u8g2_font_logisoso24_tn);
    int w = u8g2.getStrWidth(watchBuf);
    u8g2.drawStr((128 - w) / 2, 46, watchBuf);
  }

  // Minimal Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  const char* toggleCmd = stopwatchRunning ? "1:PAUSE" : (stopwatchAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footBuf[36];
  snprintf(footBuf, sizeof(footBuf), "%s 2:RST 3:POMO", toggleCmd);
  u8g2.drawStr(2, 61, footBuf);
  u8g2.drawStr(96, 61, "H:MENU");
}

// ============================================================
//  SCREEN 6: 3 TAPS — CRAZY FACE (STAR EYES + SCREEN SHAKE)
// ============================================================
void drawCrazyFace(unsigned long now) {
  unsigned long elapsed = now - stateStartTime;

  // Screen-shake offset
  int sX = random(-2, 3);
  int sY = random(-2, 3);

  // Star Eyes
  int eyeRadius = 13 + ((elapsed % 200 < 100) ? 2 : 0);
  drawStarEye(36 + sX, 22 + sY, eyeRadius);
  drawStarEye(92 + sX, 22 + sY, eyeRadius);

  // Open funny triangle mouth with tongue
  u8g2.drawTriangle(46 + sX, 44 + sY, 82 + sX, 44 + sY, 64 + sX, 60 + sY);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(64 + sX, 55 + sY, 4);
  u8g2.setDrawColor(1);

  // Playful audio squeaks
  digitalWrite(BUZZER_PIN, HIGH);
  delayMicroseconds(random(40, 140));
  digitalWrite(BUZZER_PIN, LOW);

  // Return automatically to HOME after 2.5 seconds
  if (elapsed > 2500) {
    currentState = STATE_HOME;
  }
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(TOUCH_PIN, TOUCH_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  u8g2.begin();

  // Configure India Standard Time (UTC+05:30)
  setenv("TZ", "IST-5:30", 1);
  tzset();

  settings.begin("deskbuddy", false);
  // Restore persisted clock format & stopwatch
  clockFormat = settings.getInt("clk_fmt", 1);
  stopwatchAccumulatedMs = settings.getULong64("sw_ms", 0);

  // Start BLE time sync service
  startBleService();

  // Create background network worker task
  xTaskCreate(
    networkWorkerTask,
    "net_worker",
    4096,
    NULL,
    1,
    &networkTaskHandle
  );

  // Trigger initial network sync if Wi-Fi is configured
  if (strlen(DESKBUDDY_WIFI_SSID) > 0) {
    triggerNetworkSync();
  }

  touchCurrentState = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  touchDebouncedState = touchCurrentState;
  lastFrameAt = millis();

  beepClick(15);
  Serial.println("[SYSTEM] DeskBuddy Minimal Firmware Started.");
}

// ============================================================
//  loop()
// ============================================================
void loop() {
  unsigned long now = millis();
  u8g2.clearBuffer();

  // ── 1. BLE TIME SYNCHRONIZATION EVENT ───────────────────
  int64_t bleEpoch = 0;
  portENTER_CRITICAL(&bleTimeMux);
  if (pendingBleEpoch > 0) {
    bleEpoch = pendingBleEpoch;
    pendingBleEpoch = 0;
  }
  portEXIT_CRITICAL(&bleTimeMux);

  if (bleEpoch > 0) {
    struct timeval tv = { .tv_sec = (time_t)bleEpoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    clockSynchronized = true;
    syncIndicator = SYNC_SUCCESS;
    syncIndicatorTime = now;
    beepClick(25);
    Serial.printf("[BLE] Time set: %lld\n", (long long)bleEpoch);
  }

  // ── 2. TOUCH SENSING & DEBOUNCING ────────────────────────
  bool rawReading = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  if (rawReading != touchCurrentState) {
    touchCurrentState = rawReading;
    touchLastDebounceTime = now;
  }

  bool prevDebounced = touchDebouncedState;
  if ((now - touchLastDebounceTime) >= TOUCH_DEBOUNCE_MS) {
    touchDebouncedState = touchCurrentState;
  }
  bool touching = touchDebouncedState;

  // Rising Edge (Touch Down)
  if (touching && !prevDebounced) {
    touchStartTime = now;
    longPressHandled = false;
  }

  // Long Press Detection (>800ms)
  if (touching && !longPressHandled && (now - touchStartTime >= LONG_PRESS_MS)) {
    longPressHandled = true;

    if (currentState == STATE_TIMER_MENU) {
      // Long press in menu selects highlighted item
      if (menuSelection == 0) currentState = STATE_POMODORO;
      else currentState = STATE_STOPWATCH;
      beepClick(20);
      tapCount = 0;
    } else if (currentState == STATE_POMODORO || currentState == STATE_STOPWATCH) {
      // Long press in Pomodoro/Stopwatch returns to Menu
      currentState = STATE_TIMER_MENU;
      beepClick(15);
      tapCount = 0;
    }
  }

  // Falling Edge (Touch Released)
  if (!touching && prevDebounced) {
    if (longPressHandled) {
      // Prevent long-press release from counting as a tap
      longPressHandled = false;
      tapCount = 0;
    } else {
      tapCount++;
      lastTapRelease = now;
      beepTapFeedback(tapCount); // Immediate audible acknowledgment
    }
  }

  // ── 3. TAP SEQUENCE RESOLUTION ───────────────────────────
  if (tapCount > 0 && !touching && (now - lastTapRelease >= TAP_GAP_MS)) {
    if (currentState == STATE_TIMER_MENU) {
      if (tapCount == 1) {
        // 1 Tap: Toggle selection between Pomodoro and Stopwatch
        menuSelection = (menuSelection + 1) % 2;
        beepClick(10);
      } else if (tapCount == 2) {
        // 2 Taps: Open selected
        if (menuSelection == 0) currentState = STATE_POMODORO;
        else currentState = STATE_STOPWATCH;
        beepClick(20);
      } else if (tapCount >= 3) {
        // 3 Taps: Return to Home
        currentState = STATE_HOME;
        beepClick(15);
      }
    } else if (currentState == STATE_POMODORO) {
      if (tapCount == 1) {
        // 1 Tap: START / PAUSE / RESUME
        if (pomodoroRunning) {
          pomodoroAccumulatedMs += (now - pomodoroRunStartedAt);
          pomodoroRunning = false;
        } else {
          pomodoroRunning = true;
          pomodoroRunStartedAt = now;
          pomodoroCompleted = false;
        }
        beepClick(15);
      } else if (tapCount == 2) {
        // 2 Taps: RESET
        pomodoroRunning = false;
        pomodoroAccumulatedMs = 0;
        pomodoroCompleted = false;
        beepClick(25);
      } else if (tapCount == 3) {
        // 3 Taps: CYCLE DURATION (50 -> 25 -> 10 -> 5 -> 1 min)
        pomodoroRunning = false;
        pomodoroAccumulatedMs = 0;
        pomodoroCompleted = false;
        currentOptionIndex = (currentOptionIndex + 1) % 5;
        pomodoroDurationSec = (long)pomodoroOptions[currentOptionIndex] * 60;
        beepClick(20);
      } else if (tapCount == 4) {
        // 4 Taps: Switch directly to Stopwatch
        currentState = STATE_STOPWATCH;
        beepClick(15);
      } else if (tapCount >= 5) {
        // 5 Taps: Return to Menu
        currentState = STATE_TIMER_MENU;
        beepClick(15);
      }
    } else if (currentState == STATE_STOPWATCH) {
      if (tapCount == 1) {
        // 1 Tap: START / PAUSE / RESUME
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        } else {
          stopwatchRunning = true;
          stopwatchRunStartedAt = now;
        }
        beepClick(15);
      } else if (tapCount == 2) {
        // 2 Taps: RESET to 0
        stopwatchRunning = false;
        stopwatchAccumulatedMs = 0;
        saveStopwatch();
        beepClick(25);
      } else if (tapCount == 3) {
        // 3 Taps: Switch directly to Pomodoro
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentState = STATE_POMODORO;
        beepClick(15);
      } else if (tapCount >= 4) {
        // 4 Taps: Return to Menu
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentState = STATE_TIMER_MENU;
        beepClick(15);
      }
    } else {
      // ── GLOBAL 5 GESTURES (FROM HOME OR ACTIVE ANIMATION) ──
      if (tapCount == 1) {
        // 1 Tap: Love Animation ❤️
        currentState   = STATE_LOVE;
        stateStartTime = now;
      } else if (tapCount == 2) {
        // 2 Taps: Timer & Stopwatch Menu ⏱️
        currentState  = STATE_TIMER_MENU;
        menuSelection = 0;
        beepClick(15);
      } else if (tapCount == 3) {
        // 3 Taps: Crazy Face 🤪
        currentState   = STATE_CRAZY;
        stateStartTime = now;
      } else if (tapCount == 4) {
        // 4 Taps: Cycle Clock Format 🕒 (12H, 12H+sec, 24H, 24H+sec) & Persist
        clockFormat = (clockFormat + 1) % 4;
        settings.putInt("clk_fmt", clockFormat);
        beepClick(20);
      } else if (tapCount == 5) {
        // 5 Taps: Manual Wi-Fi & NTP Time Sync 🔄
        triggerNetworkSync();
        beepClick(25);
      }
    }
    tapCount = 0;
  }

  // ── 4. POMODORO TIMER TICK & AUTO-RESET ───────────────────
  if (pomodoroRunning) {
    uint32_t rem = getPomodoroRemainingSeconds(now);
    if (rem == 0) {
      // Timer finished: Triple alarm tone & auto-reset to selected duration
      pomodoroRunning = false;
      pomodoroCompleted = true;
      for (int b = 0; b < 3; b++) {
        beepAlarmPulse(450);
        delay(40);
      }
      pomodoroAccumulatedMs = 0;
      pomodoroCompleted = false;
      lastWarningSec = 0;
    } else if (rem <= 5) {
      // 5-second countdown warning beeps at 5, 4, 3, 2, 1
      if (lastWarningSec != rem) {
        lastWarningSec = rem;
        int pitch = map(rem, 5, 1, 800, 400);
        beepAlarmPulse(pitch);
      }
    }
  }

  // ── 5. STATE RENDERING PIPELINE ──────────────────────────
  switch (currentState) {
    case STATE_HOME:
      drawHome(now);
      break;

    case STATE_LOVE:
      drawLoveAnimation(now);
      break;

    case STATE_TIMER_MENU:
      drawTimerMenu();
      break;

    case STATE_POMODORO:
      drawPomodoroScreen(now);
      break;

    case STATE_STOPWATCH:
      drawStopwatchScreen(now);
      break;

    case STATE_CRAZY:
      drawCrazyFace(now);
      break;
  }

  u8g2.sendBuffer();

  // Smooth frame pacing
  unsigned long frameElapsed = millis() - lastFrameAt;
  if (frameElapsed < FRAME_INTERVAL_MS) {
    delay(FRAME_INTERVAL_MS - frameElapsed);
  }
  lastFrameAt = millis();
}
