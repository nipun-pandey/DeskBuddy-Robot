// ============================================================
//  DeskBuddy Robot — Polished Minimal Desktop Companion
//  ESP32-C3 DevKitM-1 + SH1106 128x64 OLED
//  
//  Architecture:
//  - handleInput()           : Debounced edge detection & long press
//  - processGestures()       : Settle-window tap counting FSM
//  - updateApplicationState(): Navigation and mode state machine
//  - updateAnimations()      : Non-blocking animated eyes & special faces
//  - updateTimer()           : Monotonic timestamp Pomodoro countdown
//  - updateStopwatch()       : Monotonic timestamp persistent stopwatch
//  - updateClock()           : IST clock formatting & NVS persistence
//  - handleTimeSync()        : Non-blocking background Wi-Fi & NTP sync
//  - renderScreen()          : Crisp, flicker-free 60 FPS OLED rendering
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
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
//  PIN MAPPING (ESP32-C3)
// ============================================================
#define SDA_PIN          8   // I2C data  → SH1106 OLED
#define SCL_PIN          9   // I2C clock → SH1106 OLED
#define BUZZER_PIN       5   // Passive piezo buzzer
#define TOUCH_PIN        7   // Touch sensor / push button
#define DHT_PIN          6   // DHT11 temperature & humidity sensor
#define TOUCH_ACTIVE_LOW 0   // 0 = active HIGH, 1 = active LOW

// BLE Configuration
static const char* BLE_DEVICE_NAME       = "DeskBuddy";
static const char* BLE_TIME_SERVICE_UUID = "a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001";
static const char* BLE_TIME_CHAR_UUID    = "a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001";

// Hardware Objects
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
DHT dht(DHT_PIN, DHT11);
Preferences settings;

// ============================================================
//  APPLICATION STATES
// ============================================================
enum AppState {
  STATE_HOME = 0,    // Animated expressive eyes + bottom clock
  STATE_LOVE,        // 1 Tap: Heart-eye love animation + floating hearts
  STATE_MENU,        // 2 Taps: Minimal menu (TIMER / STOPWATCH / BACK)
  STATE_POMODORO,    // Pomodoro countdown timer (large digits)
  STATE_STOPWATCH,   // Persistent millisecond stopwatch (large digits)
  STATE_CRAZY        // 3 Taps: Playful crazy face (star eyes + screen shake)
};

AppState currentAppState = STATE_HOME;
unsigned long appStateStartTime = 0;

// ============================================================
//  CLOCK & SETTINGS (IST UTC+05:30)
//  0=12H, 1=12H+sec (default), 2=24H, 3=24H+sec
// ============================================================
int clockFormat = 1;
bool clockSynchronized = false;

// Toast Notification Overlay
char toastMessage[24] = "";
unsigned long toastEndTime = 0;

void showToast(const char* msg, unsigned long durationMs = 1800) {
  strncpy(toastMessage, msg, sizeof(toastMessage) - 1);
  toastMessage[sizeof(toastMessage) - 1] = '\0';
  toastEndTime = millis() + durationMs;
}

// ============================================================
//  TIMER & STOPWATCH DATA
// ============================================================
int menuIndex = 0; // 0=TIMER, 1=STOPWATCH, 2=BACK

// Pomodoro Timer (Timestamp-based)
const int pomodoroOptions[] = {50, 25, 10, 5, 1}; // in minutes
int currentOptionIndex      = 1;                  // Default: 25 minutes
long pomodoroDurationSec    = 25 * 60;
bool pomodoroRunning        = false;
unsigned long pomodoroRunStartedAt  = 0;
unsigned long pomodoroAccumulatedMs = 0;
bool pomodoroCompleted      = false;
unsigned long pomodoroCompleteUntil = 0;
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

// DHT11 Ambient Data (Sampled non-blockingly)
float indoorTemp = NAN;
float indoorHum  = NAN;
unsigned long lastSensorSample = 0;
const unsigned long SENSOR_SAMPLE_INTERVAL = 3000;

// ============================================================
//  BUZZER (NON-BLOCKING)
// ============================================================
void beepClick(int durationMs = 8) {
  digitalWrite(BUZZER_PIN, HIGH);
  delayMicroseconds(durationMs * 100);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepAlarmPulse(int pitchUs) {
  for (int i = 0; i < 28; i++) {
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
//  MODULE 1: handleInput() — Debounced Edge & Long-Press Detection
// ============================================================
struct InputEvent {
  bool touching;
  bool pressedEdge;
  bool releasedEdge;
  bool longPressHeld;
  bool longPressReleased;
};

InputEvent rawInput;
bool touchDebouncedState = LOW;
bool touchLastRawState   = LOW;
unsigned long touchDebounceTime = 0;
unsigned long touchPressStartTime = 0;
bool touchLongPressActive = false;

const unsigned long DEBOUNCE_MS   = 25;
const unsigned long LONG_PRESS_MS = 750;

void handleInput(unsigned long now = 0) {
  if (now == 0) now = millis();
  bool physicalState = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  if (physicalState != touchLastRawState) {
    touchLastRawState = physicalState;
    touchDebounceTime = now;
  }

  bool debounced = touchDebouncedState;
  if ((now - touchDebounceTime) >= DEBOUNCE_MS) {
    debounced = touchLastRawState;
  }

  rawInput.pressedEdge  = (debounced && !touchDebouncedState);
  rawInput.releasedEdge = (!debounced && touchDebouncedState);
  touchDebouncedState   = debounced;
  rawInput.touching     = debounced;

  rawInput.longPressHeld = false;
  rawInput.longPressReleased = false;

  if (rawInput.pressedEdge) {
    touchPressStartTime = now;
    touchLongPressActive = false;
  }

  // Detect long press threshold reached
  if (rawInput.touching && !touchLongPressActive && (now - touchPressStartTime >= LONG_PRESS_MS)) {
    touchLongPressActive = true;
    rawInput.longPressHeld = true; // One-shot trigger
  }

  // Detect release of long press (suppresses tap count)
  if (rawInput.releasedEdge) {
    if (touchLongPressActive) {
      rawInput.longPressReleased = true;
      touchLongPressActive = false;
    }
  }
}

// ============================================================
//  MODULE 2: processGestures() — Settle-Window Tap Counting FSM
// ============================================================
enum GestureType {
  GESTURE_NONE = 0,
  GESTURE_TAPS,
  GESTURE_LONG_PRESS
};

GestureType activeGesture = GESTURE_NONE;
int activeTapCount = 0;

int accumulatedTaps = 0;
unsigned long lastTapReleaseTime = 0;
const unsigned long TAP_GAP_MS = 380; // Settle window for multi-taps

void processGestures(unsigned long now = 0) {
  if (now == 0) now = millis();
  activeGesture = GESTURE_NONE;
  activeTapCount = 0;

  // Handle immediate long press
  if (rawInput.longPressHeld) {
    activeGesture = GESTURE_LONG_PRESS;
    accumulatedTaps = 0; // Clear any pending tap accumulation
    return;
  }

  // Accumulate clean tap releases (ignoring long-press releases)
  if (rawInput.releasedEdge && !rawInput.longPressReleased) {
    accumulatedTaps++;
    lastTapReleaseTime = now;
    beepTapFeedback(accumulatedTaps); // Audio click per physical tap
  }

  // Settle window timeout: Dispatch tap sequence ONLY after TAP_GAP_MS of silence
  if (accumulatedTaps > 0 && !rawInput.touching && (now - lastTapReleaseTime >= TAP_GAP_MS)) {
    activeGesture  = GESTURE_TAPS;
    activeTapCount = accumulatedTaps;
    accumulatedTaps = 0; // Reset accumulator
  }
}

// ============================================================
//  MODULE 3: updateApplicationState() — Mode & Navigation FSM
// ============================================================
void triggerNetworkSync(); // Forward declaration

void updateApplicationState(unsigned long now = 0) {
  if (now == 0) now = millis();
  if (activeGesture == GESTURE_NONE) return;

  switch (currentAppState) {
    // ── 1. HOME SCREEN (GLOBAL GESTURES) ────────────────────
    case STATE_HOME:
      if (activeGesture == GESTURE_TAPS) {
        if (activeTapCount == 1) {
          // 1 Tap — Love Animation
          currentAppState = STATE_LOVE;
          appStateStartTime = now;
        } else if (activeTapCount == 2) {
          // 2 Taps — Timer & Stopwatch Menu
          currentAppState = STATE_MENU;
          menuIndex = 0;
          appStateStartTime = now;
          beepClick(15);
        } else if (activeTapCount == 3) {
          // 3 Taps — Crazy Face
          currentAppState = STATE_CRAZY;
          appStateStartTime = now;
        } else if (activeTapCount == 4) {
          // 4 Taps — Cycle Clock Format & Persist
          clockFormat = (clockFormat + 1) % 4;
          settings.putInt("clk_fmt", clockFormat);
          const char* fmtLabels[] = {"12-HOUR", "12H + SEC", "24-HOUR", "24H + SEC"};
          showToast(fmtLabels[clockFormat]);
          beepClick(20);
        } else if (activeTapCount == 5) {
          // 5 Taps — Time Synchronization
          triggerNetworkSync();
          showToast("SYNCING TIME...");
          beepClick(25);
        }
      }
      break;

    // ── 2. MINIMAL MENU (TIMER / STOPWATCH / BACK) ──────────
    case STATE_MENU:
      if (activeGesture == GESTURE_TAPS) {
        if (activeTapCount == 1) {
          // 1 Tap: Advance menu selection
          menuIndex = (menuIndex + 1) % 3;
          beepClick(10);
        } else if (activeTapCount == 2) {
          // 2 Taps: Open selected option
          if (menuIndex == 0) {
            currentAppState = STATE_POMODORO;
          } else if (menuIndex == 1) {
            currentAppState = STATE_STOPWATCH;
          } else {
            currentAppState = STATE_HOME;
          }
          beepClick(20);
        } else if (activeTapCount >= 3) {
          currentAppState = STATE_HOME;
          beepClick(15);
        }
      } else if (activeGesture == GESTURE_LONG_PRESS) {
        // Long Press: Select current option or Back
        if (menuIndex == 0) {
          currentAppState = STATE_POMODORO;
        } else if (menuIndex == 1) {
          currentAppState = STATE_STOPWATCH;
        } else {
          currentAppState = STATE_HOME;
        }
        beepClick(20);
      }
      break;

    // ── 3. POMODORO TIMER SCREEN ────────────────────────────
    case STATE_POMODORO:
      if (activeGesture == GESTURE_TAPS) {
        if (activeTapCount == 1) {
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
        } else if (activeTapCount == 2) {
          // 2 Taps: RESET
          pomodoroRunning = false;
          pomodoroAccumulatedMs = 0;
          pomodoroCompleted = false;
          beepClick(25);
        } else if (activeTapCount == 3) {
          // 3 Taps: CYCLE DURATION
          pomodoroRunning = false;
          pomodoroAccumulatedMs = 0;
          pomodoroCompleted = false;
          currentOptionIndex = (currentOptionIndex + 1) % 5;
          pomodoroDurationSec = (long)pomodoroOptions[currentOptionIndex] * 60;
          char durToast[16];
          snprintf(durToast, sizeof(durToast), "%d MIN TIMER", pomodoroOptions[currentOptionIndex]);
          showToast(durToast, 1200);
          beepClick(20);
        } else if (activeTapCount == 4) {
          // 4 Taps: Quick-switch to Stopwatch
          currentAppState = STATE_STOPWATCH;
          beepClick(15);
        } else if (activeTapCount >= 5) {
          // 5+ Taps: Direct Return to Home
          currentAppState = STATE_HOME;
          beepClick(15);
        }
      } else if (activeGesture == GESTURE_LONG_PRESS) {
        // Long Press: Return to Menu
        currentAppState = STATE_MENU;
        beepClick(15);
      }
      break;

    // ── 4. STOPWATCH SCREEN ─────────────────────────────────
    case STATE_STOPWATCH:
      if (activeGesture == GESTURE_TAPS) {
        if (activeTapCount == 1) {
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
        } else if (activeTapCount == 2) {
          // 2 Taps: RESET
          stopwatchRunning = false;
          stopwatchAccumulatedMs = 0;
          saveStopwatch();
          beepClick(25);
        } else if (activeTapCount == 3) {
          // 3 Taps: Quick-switch to Pomodoro
          if (stopwatchRunning) {
            stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
            stopwatchRunning = false;
            saveStopwatch();
          }
          currentAppState = STATE_POMODORO;
          beepClick(15);
        } else if (activeTapCount >= 4) {
          // 4+ Taps: Direct Return to Home
          if (stopwatchRunning) {
            stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
            stopwatchRunning = false;
            saveStopwatch();
          }
          currentAppState = STATE_HOME;
          beepClick(15);
        }
      } else if (activeGesture == GESTURE_LONG_PRESS) {
        // Long Press: Return to Menu
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentAppState = STATE_MENU;
        beepClick(15);
      }
      break;

    // ── 5. TEMPORARY ANIMATION SCREENS ──────────────────────
    case STATE_LOVE:
    case STATE_CRAZY:
      // Any touch or tap interrupts animation and returns Home immediately
      currentAppState = STATE_HOME;
      beepClick(10);
      break;
  }
}

// ============================================================
//  MODULE 4: updateAnimations() — Non-blocking Eye & Face Animations
// ============================================================
float easeInOut(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void updateAnimations(unsigned long now = 0) {
  if (now == 0) now = millis();
  // Check completion of temporary animations
  if (currentAppState == STATE_LOVE && (now - appStateStartTime > 3200)) {
    currentAppState = STATE_HOME;
  }
  if (currentAppState == STATE_CRAZY && (now - appStateStartTime > 2500)) {
    currentAppState = STATE_HOME;
  }
}

// ============================================================
//  MODULE 5: updateTimer() — Pomodoro Countdown & Auto-Reset
// ============================================================
void updateTimer(unsigned long now = 0) {
  if (now == 0) now = millis();
  if (!pomodoroRunning) return;

  uint32_t rem = getPomodoroRemainingSeconds(now);
  if (rem == 0) {
    // Countdown Finished
    pomodoroRunning = false;
    pomodoroCompleted = true;
    pomodoroCompleteUntil = now + 2500; // Show FINISHED indicator for 2.5s
    
    // Triple completion alarm
    for (int b = 0; b < 3; b++) {
      beepAlarmPulse(450);
      delay(40);
    }

    // Auto-reset back to selected duration in paused state
    pomodoroAccumulatedMs = 0;
    pomodoroCompleted = false;
    lastWarningSec = 0;
  } else if (rem <= 5) {
    // 5-second countdown warning beeps at seconds 5, 4, 3, 2, 1
    if (lastWarningSec != rem) {
      lastWarningSec = rem;
      int pitch = map(rem, 5, 1, 800, 400);
      beepAlarmPulse(pitch);
    }
  }
}

// ============================================================
//  MODULE 6: updateStopwatch() — Monotonic Stopwatch
// ============================================================
void updateStopwatch(unsigned long now = 0) {
  if (now == 0) now = millis();
  // Monotonic calculation is handled cleanly inside getStopwatchTotalMs(now).
  // No busy-wait or loop counter accumulation needed.
}

// ============================================================
//  MODULE 7: updateClock() — RTC & Sensor Background Sampling
// ============================================================
void updateClock(unsigned long now = 0) {
  if (now == 0) now = millis();
  // Sample DHT11 sensor periodically without blocking
  if (now - lastSensorSample >= SENSOR_SAMPLE_INTERVAL) {
    lastSensorSample = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) indoorTemp = t;
    if (!isnan(h)) indoorHum = h;
  }
}

// ============================================================
//  MODULE 8: handleTimeSync() — Asynchronous Wi-Fi & NTP Task
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

      const char* ssid = DESKBUDDY_WIFI_SSID;
      const char* pass = DESKBUDDY_WIFI_PASSWORD;

      if (strlen(ssid) > 0) {
        Serial.println("[NET] Asynchronous Wi-Fi connecting...");
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.begin(ssid, pass);

        unsigned long startConnect = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startConnect < 8000)) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[NET] Connected. Synchronizing NTP (IST)...");
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
            clockSynchronized = true;
            showToast("✓ TIME SYNCED", 2000);
            Serial.println("[NET] Time successfully synchronized.");
          } else {
            showToast("! NTP TIMEOUT", 2000);
            Serial.println("[NET] NTP sync timed out.");
          }
        } else {
          showToast("! WIFI FAILED", 2000);
          Serial.println("[NET] Wi-Fi connection failed.");
        }

        // Always power off Wi-Fi modem immediately after sync to save power
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
      } else {
        showToast("! NO WIFI SSID", 2000);
      }

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

// BLE NimBLE Callbacks
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

void handleTimeSync(unsigned long now = 0) {
  if (now == 0) now = millis();

  // Process pending BLE epoch synchronization
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
    showToast("✓ BLE SYNCED", 2000);
    beepClick(25);
  }
}

// ============================================================
//  MODULE 9: renderScreen() — Crisp, Flicker-Free OLED Rendering
// ============================================================

// Helper: Heart Eye
void drawHeartEye(int x, int y, int sz) {
  u8g2.drawDisc(x - (sz / 2), y, sz / 2);
  u8g2.drawDisc(x + (sz / 2), y, sz / 2);
  u8g2.drawTriangle(x - sz, y + 1, x + sz, y + 1, x, y + sz + 8);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(x, y + 2, 3);
  u8g2.setDrawColor(1);
  u8g2.drawPixel(x - (sz / 2) - 1, y - 2);
}

// Helper: Floating Tiny Heart
void drawTinyHeart(int x, int y, int size) {
  int lobe = max(1, size / 2);
  u8g2.drawDisc(x - lobe, y, lobe);
  u8g2.drawDisc(x + lobe, y, lobe);
  u8g2.drawTriangle(x - size, y, x + size, y, x, y + size + 2);
}

// Helper: Star Eye
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

// ── Screen: Home (Animated Eyes + Compact Clock) ─────────────
void renderHome(unsigned long now) {
  // Eye Animation Calculation
  static unsigned long lastBlinkTime = 0;
  static bool isBlinking = false;
  static unsigned long blinkStart = 0;

  if (!isBlinking && (now - lastBlinkTime > 3400)) {
    isBlinking = true;
    blinkStart = now;
    lastBlinkTime = now;
  }

  float closure = 0.0f;
  if (isBlinking) {
    unsigned long bElapsed = now - blinkStart;
    if (bElapsed < 100) closure = (float)bElapsed / 100.0f;
    else if (bElapsed < 180) closure = 1.0f;
    else if (bElapsed < 280) closure = 1.0f - ((float)(bElapsed - 180) / 100.0f);
    else isBlinking = false;
  }

  int gazeOffset = (int)(sinf(now * 0.0018f) * 4.0f);
  int eyeHeight  = (int)((1.0f - closure) * 26.0f);
  if (eyeHeight < 2) eyeHeight = 2;

  // Left & Right Eyes
  u8g2.drawRBox(36 - 15, 22 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));
  u8g2.drawRBox(92 - 15, 22 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));

  // Pupils (visible when open)
  if (closure < 0.35f) {
    u8g2.setDrawColor(0);
    u8g2.drawRBox(36 - 3 + gazeOffset, 20, 6, 8, 1);
    u8g2.drawRBox(92 - 3 + gazeOffset, 20, 6, 8, 1);
    u8g2.setDrawColor(1);
  }

  // Divider
  u8g2.drawLine(16, 46, 112, 46);

  // Clock
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
  int w = u8g2.getStrWidth(timeStr);
  u8g2.drawStr((128 - w) / 2, 60, timeStr);
}

// ── Screen: Love Animation (1 Tap) ───────────────────────────
void renderLove(unsigned long now) {
  unsigned long elapsed = now - appStateStartTime;
  float beat = sinf(elapsed * 0.007f);
  int heartSize = 14 + (int)(beat * 3.0f);

  // Pulsing Heart Eyes
  drawHeartEye(36, 26, heartSize);
  drawHeartEye(92, 26, heartSize);

  // Upward Floating Hearts
  for (int i = 0; i < 4; i++) {
    unsigned long phase = (elapsed + (unsigned long)i * 650) % 2400;
    int y = 60 - (int)(phase * 52 / 2400);
    int x = 64 + (int)(sinf((elapsed + i * 400) * 0.005f) * 18.0f) + (i % 2 == 0 ? -16 : 16);
    drawTinyHeart(x, y, 3);
  }

  // Soft Heartbeat Audio Tick
  if (elapsed % 750 < 20 || (elapsed % 750 > 130 && elapsed % 750 < 150)) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(140);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// ── Screen: Crazy Face (3 Taps) ──────────────────────────────
void renderCrazy(unsigned long now) {
  unsigned long elapsed = now - appStateStartTime;

  int sX = random(-2, 3);
  int sY = random(-2, 3);

  // Star Eyes
  int eyeRadius = 13 + ((elapsed % 200 < 100) ? 2 : 0);
  drawStarEye(36 + sX, 22 + sY, eyeRadius);
  drawStarEye(92 + sX, 22 + sY, eyeRadius);

  // Funny open mouth with tongue
  u8g2.drawTriangle(46 + sX, 44 + sY, 82 + sX, 44 + sY, 64 + sX, 60 + sY);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(64 + sX, 55 + sY, 4);
  u8g2.setDrawColor(1);

  // Playful audio squeaks
  digitalWrite(BUZZER_PIN, HIGH);
  delayMicroseconds(random(40, 130));
  digitalWrite(BUZZER_PIN, LOW);
}

// ── Screen: Minimal Menu (2 Taps) ────────────────────────────
void renderMenu() {
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.drawStr(16, 9, "TIMER & STOPWATCH");
  u8g2.setDrawColor(1);

  const char* items[] = {
    "1. TIMER",
    "2. STOPWATCH",
    "3. BACK"
  };

  u8g2.setFont(u8g2_font_7x14_tf);
  for (int i = 0; i < 3; i++) {
    int y = 26 + (i * 13);
    if (menuIndex == i) {
      u8g2.drawRBox(4, y - 10, 120, 12, 2);
      u8g2.setDrawColor(0);
      u8g2.drawStr(8, y, items[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(8, y, items[i]);
    }
  }

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.drawStr(4, 61, "1:NEXT   2/HOLD:SELECT   H:BACK");
}

// ── Screen: Pomodoro Timer ───────────────────────────────────
void renderPomodoro(unsigned long now) {
  uint32_t remainingSec = getPomodoroRemainingSeconds(now);

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  char titleBuf[24];
  snprintf(titleBuf, sizeof(titleBuf), "TIMER %dM", pomodoroOptions[currentOptionIndex]);
  u8g2.drawStr(4, 9, titleBuf);

  const char* status = (now < pomodoroCompleteUntil) ? "FINISHED!" :
                       (pomodoroRunning ? "RUNNING" :
                       (pomodoroAccumulatedMs > 0 ? "PAUSED" : "READY"));
  u8g2.drawStr(82, 9, status);
  u8g2.drawLine(0, 12, 128, 12);

  // Large Centered Digits MM:SS
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", remainingSec / 60UL, remainingSec % 60UL);
  u8g2.setFont(u8g2_font_logisoso28_tn);
  int w = u8g2.getStrWidth(timeBuf);

  // Flashing inverted block on last 5 seconds
  if (remainingSec <= 5 && remainingSec > 0 && pomodoroRunning && ((now / 250) % 2 == 0)) {
    u8g2.setDrawColor(1);
    u8g2.drawRBox(10, 16, 108, 34, 3);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr((128 - w) / 2, 47, timeBuf);
  u8g2.setDrawColor(1);

  // Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  const char* toggleCmd = pomodoroRunning ? "1:PAUSE" : (pomodoroAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footBuf[36];
  snprintf(footBuf, sizeof(footBuf), "%s 2:RST 3:DUR", toggleCmd);
  u8g2.drawStr(2, 61, footBuf);
  u8g2.drawStr(96, 61, "H:MENU");
}

// ── Screen: Stopwatch ────────────────────────────────────────
void renderStopwatch(unsigned long now) {
  uint64_t totalMs = getStopwatchTotalMs(now);
  uint64_t totalSec = totalMs / 1000ULL;
  uint64_t hours = totalSec / 3600ULL;
  uint64_t minutes = (totalSec / 60ULL) % 60ULL;
  uint64_t seconds = totalSec % 60ULL;

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(4, 9, "STOPWATCH");
  const char* status = stopwatchRunning ? "RUNNING" :
                       (stopwatchAccumulatedMs > 0 ? "PAUSED" : "READY");
  u8g2.drawStr(82, 9, status);
  u8g2.drawLine(0, 12, 128, 12);

  // Large Centered Digits
  char watchBuf[16];
  if (hours > 0) {
    snprintf(watchBuf, sizeof(watchBuf), "%02llu:%02llu:%02llu",
             (unsigned long long)(hours % 100ULL),
             (unsigned long long)minutes, (unsigned long long)seconds);
    u8g2.setFont(u8g2_font_logisoso20_tn);
    int w = u8g2.getStrWidth(watchBuf);
    u8g2.drawStr((128 - w) / 2, 45, watchBuf);
  } else {
    // MM:SS format with tenths
    uint64_t tenths = (totalMs % 1000ULL) / 100ULL;
    snprintf(watchBuf, sizeof(watchBuf), "%02llu:%02llu.%llu",
             (unsigned long long)minutes, (unsigned long long)seconds, (unsigned long long)tenths);
    u8g2.setFont(u8g2_font_logisoso24_tn);
    int w = u8g2.getStrWidth(watchBuf);
    u8g2.drawStr((128 - w) / 2, 46, watchBuf);
  }

  // Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  const char* toggleCmd = stopwatchRunning ? "1:PAUSE" : (stopwatchAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footBuf[36];
  snprintf(footBuf, sizeof(footBuf), "%s 2:RST 3:POMO", toggleCmd);
  u8g2.drawStr(2, 61, footBuf);
  u8g2.drawStr(96, 61, "H:MENU");
}

// ── Screen: Toast Notification Overlay ───────────────────────
void renderToast(unsigned long now) {
  if (now < toastEndTime && strlen(toastMessage) > 0) {
    u8g2.setFont(u8g2_font_7x14_tf);
    int w = u8g2.getStrWidth(toastMessage);
    int boxW = w + 16;
    int boxX = (128 - boxW) / 2;
    int boxY = 24;

    // Inverted rounded badge overlay
    u8g2.setDrawColor(1);
    u8g2.drawRBox(boxX, boxY, boxW, 18, 3);
    u8g2.setDrawColor(0);
    u8g2.drawStr(boxX + 8, boxY + 14, toastMessage);
    u8g2.setDrawColor(1);
  }
}

// ── Master Screen Render Dispatcher ──────────────────────────
void renderScreen(unsigned long now = 0) {
  if (now == 0) now = millis();
  u8g2.clearBuffer();

  switch (currentAppState) {
    case STATE_HOME:      renderHome(now);      break;
    case STATE_LOVE:      renderLove(now);      break;
    case STATE_MENU:      renderMenu();         break;
    case STATE_POMODORO:  renderPomodoro(now);  break;
    case STATE_STOPWATCH: renderStopwatch(now); break;
    case STATE_CRAZY:     renderCrazy(now);     break;
  }

  // Render non-blocking toast overlay on top if active
  renderToast(now);

  u8g2.sendBuffer();
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
  dht.begin();

  // Configure India Standard Time (UTC+05:30)
  setenv("TZ", "IST-5:30", 1);
  tzset();

  settings.begin("deskbuddy", false);
  clockFormat = settings.getInt("clk_fmt", 1);
  stopwatchAccumulatedMs = settings.getULong64("sw_ms", 0);

  // Start BLE time sync service
  startBleService();

  // Create background network worker task for async Wi-Fi & NTP
  xTaskCreate(
    networkWorkerTask,
    "net_worker",
    4096,
    NULL,
    1,
    &networkTaskHandle
  );

  // Initial network sync at boot if Wi-Fi SSID is configured
  if (strlen(DESKBUDDY_WIFI_SSID) > 0) {
    triggerNetworkSync();
  }

  beepClick(15);
  Serial.println("[SYSTEM] DeskBuddy Refactored Firmware Running.");
}

// ============================================================
//  loop() — Clean Modular Non-Blocking Main Loop
// ============================================================
unsigned long lastFrameTimestamp = 0;
const unsigned long TARGET_FRAME_MS = 16; // ~60 FPS

void loop() {
  unsigned long now = millis();

  // Modular Non-Blocking Execution Pipeline
  handleInput(now);
  processGestures(now);
  updateApplicationState(now);
  updateAnimations(now);
  updateTimer(now);
  updateStopwatch(now);
  updateClock(now);
  handleTimeSync(now);
  renderScreen(now);

  // Frame Pacing (Target ~60 FPS)
  unsigned long frameElapsed = millis() - lastFrameTimestamp;
  if (frameElapsed < TARGET_FRAME_MS) {
    delay(TARGET_FRAME_MS - frameElapsed);
  }
  lastFrameTimestamp = millis();
}
