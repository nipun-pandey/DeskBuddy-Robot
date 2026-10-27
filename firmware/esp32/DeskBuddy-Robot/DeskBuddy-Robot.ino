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
#include <esp_wifi.h>
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

// Configurable Wi-Fi Credentials (NVS-backed)
char configuredSsid[33] = "";
char configuredPass[65] = "";

// Helper: Compact Time String for Headers (e.g. "4:15P" or "16:15")
void getCompactClockStr(char* buf, size_t maxLen) {
  struct tm ti = {};
  if (!clockSynchronized || !getLocalTime(&ti, 50)) {
    strncpy(buf, "--:--", maxLen);
    buf[maxLen - 1] = '\0';
    return;
  }
  int h12 = ti.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  const char* suffix = (ti.tm_hour >= 12) ? "P" : "A";

  if (clockFormat == 0 || clockFormat == 1) {
    snprintf(buf, maxLen, "%d:%02d%s", h12, ti.tm_min, suffix);
  } else {
    snprintf(buf, maxLen, "%02d:%02d", ti.tm_hour, ti.tm_min);
  }
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
unsigned long pomodoroMinuteBlinkUntil = 0; // 2-second eye blink overlay every minute
long pomodoroMinuteLastChecked = 25;
unsigned long homeMinuteBlinkUntil = 0;     // 2-second eye blink overlay every minute on Home screen
int lastHomeMinute = -1;

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
      if (now < homeMinuteBlinkUntil) {
        homeMinuteBlinkUntil = 0; // Dismiss eye blink immediately on touch
      }

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
      // If 1-minute completion eye animation is showing, any interaction dismisses it
      if (now < pomodoroMinuteBlinkUntil) {
        pomodoroMinuteBlinkUntil = 0;
      }

      {
        bool isPomoReset = (!pomodoroRunning && pomodoroAccumulatedMs == 0);

        if (activeGesture == GESTURE_TAPS) {
          if (isPomoReset) {
            // When RESET:
            if (activeTapCount == 1) {
              // 1 Tap: Change timing
              currentOptionIndex = (currentOptionIndex + 1) % 5;
              pomodoroDurationSec = (long)pomodoroOptions[currentOptionIndex] * 60;
              pomodoroMinuteLastChecked = pomodoroDurationSec / 60;
              char durToast[16];
              snprintf(durToast, sizeof(durToast), "%d MIN TIMER", pomodoroOptions[currentOptionIndex]);
              showToast(durToast, 1200);
              beepClick(20);
            } else if (activeTapCount == 2) {
              // 2 Taps: START
              pomodoroRunning = true;
              pomodoroRunStartedAt = now;
              pomodoroCompleted = false;
              pomodoroMinuteLastChecked = pomodoroDurationSec / 60;
              beepClick(20);
            } else if (activeTapCount >= 3) {
              // 3 Taps: EXIT to Home
              currentAppState = STATE_HOME;
              beepClick(15);
            }
          } else {
            // When RUNNING or PAUSED:
            if (activeTapCount == 1) {
              // 1 Tap: START / STOP (Pause / Resume)
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
              pomodoroMinuteLastChecked = pomodoroDurationSec / 60;
              showToast("RESET", 1000);
              beepClick(25);
            } else if (activeTapCount >= 3) {
              // 3 Taps: EXIT to Home
              currentAppState = STATE_HOME;
              beepClick(15);
            }
          }
        } else if (activeGesture == GESTURE_LONG_PRESS) {
          if (isPomoReset) {
            // Long Press when reset: START
            pomodoroRunning = true;
            pomodoroRunStartedAt = now;
            pomodoroCompleted = false;
            pomodoroMinuteLastChecked = pomodoroDurationSec / 60;
            beepClick(20);
          } else {
            // Long Press when running/paused: EXIT to Home
            currentAppState = STATE_HOME;
            beepClick(15);
          }
        }
      }
      break;

    // ── 4. STOPWATCH SCREEN ─────────────────────────────────
    case STATE_STOPWATCH:
      if (activeGesture == GESTURE_TAPS) {
        if (activeTapCount == 1) {
          // 1 Tap: START / STOP
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
          showToast("RESET", 1000);
          beepClick(25);
        } else if (activeTapCount >= 3) {
          // 3 Taps: EXIT to Home
          if (stopwatchRunning) {
            stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
            stopwatchRunning = false;
            saveStopwatch();
          }
          currentAppState = STATE_HOME;
          beepClick(15);
        }
      } else if (activeGesture == GESTURE_LONG_PRESS) {
        // Long Press: EXIT to Home
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentAppState = STATE_HOME;
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

  // Home screen 1-minute completion robotic eye blink trigger (2 seconds)
  if (currentAppState == STATE_HOME) {
    struct tm ti = {};
    if (clockSynchronized && getLocalTime(&ti, 20)) {
      if (ti.tm_min != lastHomeMinute) {
        lastHomeMinute = ti.tm_min;
        homeMinuteBlinkUntil = now + 2000;
        beepClick(10);
      }
    } else {
      static unsigned long lastHomeSecTick = 0;
      if (now - lastHomeSecTick >= 60000) {
        lastHomeSecTick = now;
        homeMinuteBlinkUntil = now + 2000;
        beepClick(10);
      }
    }
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
    pomodoroMinuteLastChecked = pomodoroDurationSec / 60;
  } else {
    // Check 1-minute completion during countdown
    long currentMinute = rem / 60;
    if (rem > 0 && currentMinute < pomodoroMinuteLastChecked) {
      pomodoroMinuteLastChecked = currentMinute;
      pomodoroMinuteBlinkUntil = now + 2000; // 2 seconds robotic blinking eye!
      beepClick(12);
    }

    // 5-second countdown warning beeps at seconds 5, 4, 3, 2, 1
    if (rem <= 5) {
      if (lastWarningSec != rem) {
        lastWarningSec = rem;
        int pitch = map(rem, 5, 1, 800, 400);
        beepAlarmPulse(pitch);
      }
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

static const char* BLE_WIFI_CHAR_UUID    = "a7e10003-8f5d-4b4a-9d42-4e8b7a0c0001";

void networkWorkerTask(void* parameter) {
  for (;;) {
    if (netSyncRequested) {
      netSyncRequested = false;
      netTaskBusy = true;

      const char* ssid = (strlen(configuredSsid) > 0) ? configuredSsid : DESKBUDDY_WIFI_SSID;
      const char* pass = (strlen(configuredPass) > 0) ? configuredPass : DESKBUDDY_WIFI_PASSWORD;

      if (strlen(ssid) > 0) {
        Serial.printf("[NET] Connecting to Wi-Fi '%s'...\n", ssid);
        WiFi.mode(WIFI_STA);

        // Configure PMF and WPA2/WPA3 compatibility for modern smartphone hotspots (Samsung S23, etc.)
        wifi_config_t conf;
        memset(&conf, 0, sizeof(conf));
        strncpy((char*)conf.sta.ssid, ssid, sizeof(conf.sta.ssid) - 1);
        strncpy((char*)conf.sta.password, pass, sizeof(conf.sta.password) - 1);
        conf.sta.pmf_cfg.capable = true;
        conf.sta.pmf_cfg.required = false;
        esp_wifi_set_config(WIFI_IF_STA, &conf);

        WiFi.begin(ssid, pass);

        unsigned long startConnect = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startConnect < 15000)) {
          vTaskDelay(pdMS_TO_TICKS(200));
          Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
          Serial.print("[NET] Connected! IP: ");
          Serial.println(WiFi.localIP());
          Serial.printf("[NET] Gateway: %s, RSSI: %d dBm\n", WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
          Serial.println("[NET] Synchronizing NTP (IST UTC+05:30)...");
          configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

          unsigned long startNtp = millis();
          struct tm timeinfo = {};
          bool success = false;

          while (millis() - startNtp < 10000) {
            if (getLocalTime(&timeinfo, 200) && timeinfo.tm_year >= (2024 - 1900)) {
              success = true;
              break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
          }

          if (success) {
            clockSynchronized = true;
            showToast("✓ TIME SYNCED", 2000);
            Serial.printf("[NET] Time successfully synced: %02d:%02d:%02d\n",
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
          } else {
            showToast("! NTP TIMEOUT", 2000);
            Serial.println("[NET] NTP sync timed out.");
          }
        } else {
          int st = (int)WiFi.status();
          const char* stNames[] = {"IDLE", "NO_SSID_AVAIL", "SCAN_COMPLETED", "CONNECTED", "CONNECT_FAILED", "CONNECTION_LOST", "DISCONNECTED"};
          const char* stStr = (st >= 0 && st <= 6) ? stNames[st] : "UNKNOWN";
          Serial.printf("[NET] Wi-Fi connection failed. Status: %s (%d)\n", stStr, st);
          if (st == 1) {
            Serial.println("[NET] -> SSID not found on 2.4 GHz.");
            showToast("! NO SSID FOUND", 2500);
          } else if (st == 4) {
            Serial.println("[NET] -> Password rejected by Wi-Fi network.");
            showToast("! BAD PASSWORD", 2000);
          } else {
            showToast("! WIFI FAILED", 2000);
          }
        }

        // Keep Wi-Fi connected or disconnect without de-initializing the radio
        WiFi.disconnect();
      } else {
        showToast("! NO WIFI SET", 2000);
        Serial.println("[NET] No Wi-Fi SSID configured.");
        Serial.println("[NET] Use 'WIFI:ssid,password' in USB Serial or write to BLE characteristic a7e10003.");
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

class DeskBuddyWifiCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    std::string raw = characteristic->getValue();
    if (raw.empty()) return;
    size_t comma = raw.find(',');
    if (comma != std::string::npos) {
      std::string s = raw.substr(0, comma);
      std::string p = raw.substr(comma + 1);
      strncpy(configuredSsid, s.c_str(), sizeof(configuredSsid) - 1);
      strncpy(configuredPass, p.c_str(), sizeof(configuredPass) - 1);
      settings.putString("wifi_ssid", configuredSsid);
      settings.putString("wifi_pass", configuredPass);
      showToast("WIFI SAVED", 1500);
      Serial.printf("[BLE] Saved Wi-Fi SSID '%s'. Connecting...\n", configuredSsid);
      triggerNetworkSync();
    }
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

  NimBLECharacteristic* wifiChar = service->createCharacteristic(
    BLE_WIFI_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  wifiChar->setCallbacks(new DeskBuddyWifiCallbacks());

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_TIME_SERVICE_UUID);
  advertising->setScanResponse(true);
  NimBLEDevice::startAdvertising();
}

static char serialCmdBuf[96];
static uint8_t serialCmdIdx = 0;

void handleTimeSync(unsigned long now = 0) {
  if (now == 0) now = millis();

  // 1. Process pending BLE epoch synchronization
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
    Serial.printf("[BLE] Time synchronized to epoch %lld\n", (long long)bleEpoch);
  }

  // 2. Process USB CDC Serial Commands
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialCmdIdx > 0) {
        serialCmdBuf[serialCmdIdx] = '\0';
        if (strncmp(serialCmdBuf, "WIFI:", 5) == 0) {
          char* comma = strchr(serialCmdBuf + 5, ',');
          if (comma) {
            *comma = '\0';
            strncpy(configuredSsid, serialCmdBuf + 5, sizeof(configuredSsid) - 1);
            strncpy(configuredPass, comma + 1, sizeof(configuredPass) - 1);
            settings.putString("wifi_ssid", configuredSsid);
            settings.putString("wifi_pass", configuredPass);
            Serial.printf("[SERIAL] Saved Wi-Fi SSID '%s'. Triggering sync...\n", configuredSsid);
            showToast("WIFI SAVED", 1500);
            triggerNetworkSync();
          } else {
            Serial.println("[SERIAL] Format error. Use: WIFI:ssid,password");
          }
        } else if (strncmp(serialCmdBuf, "TIME:", 5) == 0 || isdigit(serialCmdBuf[0])) {
          const char* numStr = (strncmp(serialCmdBuf, "TIME:", 5) == 0) ? serialCmdBuf + 5 : serialCmdBuf;
          char* end = nullptr;
          long long epoch = strtoll(numStr, &end, 10);
          if (epoch > 100000000000LL) epoch /= 1000;
          if (epoch >= 1700000000LL && epoch <= 4102444800LL) {
            struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            clockSynchronized = true;
            showToast("✓ TIME SYNCED", 2000);
            beepClick(25);
            Serial.printf("[SERIAL] Time synced to Unix epoch %lld\n", epoch);
          } else {
            Serial.println("[SERIAL] Invalid epoch timestamp.");
          }
        } else if (strcasecmp(serialCmdBuf, "SYNC") == 0) {
          triggerNetworkSync();
          showToast("SYNCING TIME...");
          Serial.println("[SERIAL] Triggered Wi-Fi and NTP sync.");
        } else if (strcasecmp(serialCmdBuf, "SCAN") == 0) {
          Serial.println("[WIFI] Scanning 2.4 GHz Wi-Fi networks...");
          WiFi.mode(WIFI_STA);
          WiFi.disconnect();
          delay(100);
          int n = WiFi.scanNetworks();
          Serial.printf("[WIFI] Scan complete. Found %d networks:\n", n);
          for (int i = 0; i < n; i++) {
            Serial.printf("  [%d] SSID: '%s' | RSSI: %d dBm | Ch: %d | %s\n",
                          i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open" : "Encrypted");
          }
        } else if (strcasecmp(serialCmdBuf, "STATUS") == 0) {
          struct tm ti = {};
          getLocalTime(&ti, 50);
          Serial.printf("[STATUS] SSID: '%s', Clock Synced: %d, Time: %02d:%02d:%02d, Temp: %.1f C, Hum: %.1f %%\n",
                        configuredSsid, clockSynchronized, ti.tm_hour, ti.tm_min, ti.tm_sec, indoorTemp, indoorHum);
        }
        serialCmdIdx = 0;
      }
    } else if (serialCmdIdx < sizeof(serialCmdBuf) - 1) {
      serialCmdBuf[serialCmdIdx++] = c;
    }
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

// ── Helper: Home Screen 1-Minute Completion Robotic Blinking Eyes ──
void renderHomeBlinkingEyes(unsigned long now) {
  unsigned long animElapsed = now - (homeMinuteBlinkUntil - 2000);
  float closure = 0.0f;
  if (animElapsed < 250) closure = (float)animElapsed / 250.0f;
  else if (animElapsed < 500) closure = 1.0f;
  else if (animElapsed < 750) closure = 1.0f - ((float)(animElapsed - 500) / 250.0f);
  else if (animElapsed < 1250) closure = 0.0f;
  else if (animElapsed < 1500) closure = (float)(animElapsed - 1250) / 250.0f;
  else if (animElapsed < 1750) closure = 1.0f;
  else closure = 1.0f - ((float)(animElapsed - 1750) / 250.0f);

  int gazeOffset = (int)(sinf(now * 0.005f) * 4.0f);
  int eyeHeight  = (int)((1.0f - closure) * 26.0f);
  if (eyeHeight < 2) eyeHeight = 2;

  // Left & Right Expressive Robot Eyes
  u8g2.drawRBox(36 - 15, 25 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));
  u8g2.drawRBox(92 - 15, 25 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));

  // Pupils (visible when open)
  if (closure < 0.4f) {
    u8g2.setDrawColor(0);
    u8g2.drawRBox(36 - 3 + gazeOffset, 23, 6, 8, 1);
    u8g2.drawRBox(92 - 3 + gazeOffset, 23, 6, 8, 1);
    u8g2.setDrawColor(1);
  }

  // Footer banner during the 2-second blink: shows Time, Temp & Hum
  u8g2.drawLine(0, 48, 128, 48);
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  char cBuf[12];
  getCompactClockStr(cBuf, sizeof(cBuf));
  char banner[40];
  if (!isnan(indoorTemp) && !isnan(indoorHum)) {
    snprintf(banner, sizeof(banner), "%s  |  %.0fC  |  %.0f%%", cBuf, indoorTemp, indoorHum);
  } else {
    snprintf(banner, sizeof(banner), "%s  |  DESKBUDDY", cBuf);
  }
  int bW = u8g2.getStrWidth(banner);
  u8g2.drawStr((128 - bW) / 2, 59, banner);
}

// ── Screen: Home (Digital Clock, Temperature & Humidity) ─────────────
void renderHome(unsigned long now) {
  // If 1-minute completion eye animation is active, show robotic blinking eyes for 2 seconds!
  if (now < homeMinuteBlinkUntil) {
    renderHomeBlinkingEyes(now);
    return;
  }

  struct tm ti = {};
  bool timeValid = clockSynchronized && getLocalTime(&ti, 50);

  // 1. Top Bar: Date / Header & Status
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  if (timeValid) {
    const char* days[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    char dateBuf[20];
    snprintf(dateBuf, sizeof(dateBuf), "%s, %d %s", days[ti.tm_wday], ti.tm_mday, months[ti.tm_mon]);
    u8g2.drawStr(2, 9, dateBuf);
  } else {
    u8g2.drawStr(2, 9, "DESKBUDDY");
  }

  const char* tzLabel = clockSynchronized ? "IST" : "--";
  int tzW = u8g2.getStrWidth(tzLabel);
  u8g2.drawStr(126 - tzW, 9, tzLabel);
  u8g2.drawLine(0, 11, 128, 11);

  // 2. Middle: Large Digital Clock
  char timeStr[20];
  if (!timeValid) {
    if (clockFormat == 1 || clockFormat == 3) strncpy(timeStr, "--:--:--", sizeof(timeStr));
    else strncpy(timeStr, "--:--", sizeof(timeStr));
    u8g2.setFont(u8g2_font_helvB14_tf);
    int w = u8g2.getStrWidth(timeStr);
    u8g2.drawStr((128 - w) / 2, 33, timeStr);
  } else {
    int h12 = ti.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    const char* suffix = (ti.tm_hour >= 12) ? "PM" : "AM";

    if (clockFormat == 0) {
      // 12-Hour without seconds: Extra large digits
      snprintf(timeStr, sizeof(timeStr), "%d:%02d", h12, ti.tm_min);
      u8g2.setFont(u8g2_font_logisoso24_tn);
      int w = u8g2.getStrWidth(timeStr);
      int startX = (128 - (w + 18)) / 2;
      u8g2.drawStr(startX, 37, timeStr);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(startX + w + 4, 25, suffix);
    } else if (clockFormat == 1) {
      // 12-Hour with seconds: Clean bold font
      snprintf(timeStr, sizeof(timeStr), "%d:%02d:%02d %s", h12, ti.tm_min, ti.tm_sec, suffix);
      u8g2.setFont(u8g2_font_helvB12_tf);
      int w = u8g2.getStrWidth(timeStr);
      u8g2.drawStr((128 - w) / 2, 33, timeStr);
    } else if (clockFormat == 2) {
      // 24-Hour without seconds: Extra large digits
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d", ti.tm_hour, ti.tm_min);
      u8g2.setFont(u8g2_font_logisoso26_tn);
      int w = u8g2.getStrWidth(timeStr);
      u8g2.drawStr((128 - w) / 2, 37, timeStr);
    } else {
      // 24-Hour with seconds: Clean bold font
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
      u8g2.setFont(u8g2_font_helvB14_tf);
      int w = u8g2.getStrWidth(timeStr);
      u8g2.drawStr((128 - w) / 2, 33, timeStr);
    }
  }

  // 3. Divider Line
  u8g2.drawLine(0, 44, 128, 44);

  // 4. Bottom Area: Temperature & Humidity from DHT11 Sensor
  u8g2.setFont(u8g2_font_7x14_tf);
  char envLine[32];
  if (!isnan(indoorTemp) && !isnan(indoorHum)) {
    snprintf(envLine, sizeof(envLine), "%.1f C   |   %.0f%%", indoorTemp, indoorHum);
  } else if (!isnan(indoorTemp)) {
    snprintf(envLine, sizeof(envLine), "%.1f C   |   --%%", indoorTemp);
  } else if (!isnan(indoorHum)) {
    snprintf(envLine, sizeof(envLine), "-- C   |   %.0f%%", indoorHum);
  } else {
    snprintf(envLine, sizeof(envLine), "-- C   |   --%%");
  }

  int envW = u8g2.getStrWidth(envLine);
  int envX = (128 - envW) / 2;
  int envY = 59;
  u8g2.drawStr(envX, envY, envLine);

  // Draw degree symbol circle above the space before 'C'
  if (!isnan(indoorTemp)) {
    char numBuf[8];
    snprintf(numBuf, sizeof(numBuf), "%.1f", indoorTemp);
    int numW = u8g2.getStrWidth(numBuf);
    u8g2.drawCircle(envX + numW + 2, envY - 8, 1);
  } else {
    int numW = u8g2.getStrWidth("--");
    u8g2.drawCircle(envX + numW + 2, envY - 8, 1);
  }
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

// ── Helper: Pomodoro 1-Minute Completion Robotic Blinking Eyes Overlay ──
void renderRoboticMinuteEyes(unsigned long now, uint32_t remainingSec) {
  // Eye Animation Calculation
  unsigned long animElapsed = now - (pomodoroMinuteBlinkUntil - 2000);
  float closure = 0.0f;
  if (animElapsed < 250) closure = (float)animElapsed / 250.0f;
  else if (animElapsed < 500) closure = 1.0f;
  else if (animElapsed < 750) closure = 1.0f - ((float)(animElapsed - 500) / 250.0f);
  else if (animElapsed < 1250) closure = 0.0f;
  else if (animElapsed < 1500) closure = (float)(animElapsed - 1250) / 250.0f;
  else if (animElapsed < 1750) closure = 1.0f;
  else closure = 1.0f - ((float)(animElapsed - 1750) / 250.0f);

  int gazeOffset = (int)(sinf(now * 0.005f) * 4.0f);
  int eyeHeight  = (int)((1.0f - closure) * 24.0f);
  if (eyeHeight < 2) eyeHeight = 2;

  // Header banner: Left = POMO, Center = Clock, Right = remaining countdown
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(2, 9, "POMODORO FOCUS");

  char clockBuf[12];
  getCompactClockStr(clockBuf, sizeof(clockBuf));
  u8g2.drawStr(70, 9, clockBuf);

  char remBuf[10];
  snprintf(remBuf, sizeof(remBuf), "%02lu:%02lu", remainingSec / 60UL, remainingSec % 60UL);
  int remW = u8g2.getStrWidth(remBuf);
  u8g2.drawStr(126 - remW, 9, remBuf);
  u8g2.drawLine(0, 11, 128, 11);

  // Left & Right Eyes
  u8g2.drawRBox(36 - 15, 33 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));
  u8g2.drawRBox(92 - 15, 33 - (eyeHeight / 2), 30, eyeHeight, min(5, eyeHeight / 2));

  // Pupils (visible when open)
  if (closure < 0.4f) {
    u8g2.setDrawColor(0);
    u8g2.drawRBox(36 - 3 + gazeOffset, 31, 6, 8, 1);
    u8g2.drawRBox(92 - 3 + gazeOffset, 31, 6, 8, 1);
    u8g2.setDrawColor(1);
  }

  // Footer
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.drawStr(20, 61, "GREAT WORK! KEEP GOING");
}

// ── Screen: Pomodoro Timer ───────────────────────────────────
void renderPomodoro(unsigned long now) {
  uint32_t remainingSec = getPomodoroRemainingSeconds(now);

  // If 1-minute completion eye animation is active, render robotic blinking eyes!
  if (pomodoroRunning && now < pomodoroMinuteBlinkUntil) {
    renderRoboticMinuteEyes(now, remainingSec);
    return;
  }

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);

  // Header: Left = Duration (e.g. 25M), Center = Clock (e.g. 4:15P), Right = Status
  char titleBuf[16];
  snprintf(titleBuf, sizeof(titleBuf), "%dM", pomodoroOptions[currentOptionIndex]);
  u8g2.drawStr(2, 9, titleBuf);

  char clockBuf[12];
  getCompactClockStr(clockBuf, sizeof(clockBuf));
  int clockW = u8g2.getStrWidth(clockBuf);
  u8g2.drawStr((128 - clockW) / 2, 9, clockBuf);

  const char* status = (now < pomodoroCompleteUntil) ? "FIN!" :
                       (pomodoroRunning ? "RUN" :
                       (pomodoroAccumulatedMs > 0 ? "PAUS" : "READY"));
  int statusW = u8g2.getStrWidth(status);
  u8g2.drawStr(126 - statusW, 9, status);
  u8g2.drawLine(0, 12, 128, 12);

  // Large Centered Digits MM:SS
  char timeBuf[12];
  snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", remainingSec / 60UL, remainingSec % 60UL);
  u8g2.setFont(u8g2_font_logisoso26_tn);
  int w = u8g2.getStrWidth(timeBuf);

  // Flashing inverted block on last 5 seconds
  if (remainingSec <= 5 && remainingSec > 0 && pomodoroRunning && ((now / 250) % 2 == 0)) {
    u8g2.setDrawColor(1);
    u8g2.drawRBox((128 - w) / 2 - 4, 16, w + 8, 34, 3);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr((128 - w) / 2, 47, timeBuf);
  u8g2.setDrawColor(1);

  // Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  if (pomodoroRunning) {
    u8g2.drawStr(2, 61, "1:STOP   2:RESET   3:EXIT");
  } else if (pomodoroAccumulatedMs > 0) {
    u8g2.drawStr(2, 61, "1:START  2:RESET   3:EXIT");
  } else {
    // Reset state: 1 tap to change timing!
    u8g2.drawStr(2, 61, "1:CHG TIME  2/H:START  3:EXIT");
  }
}

// ── Screen: Stopwatch ────────────────────────────────────────
void renderStopwatch(unsigned long now) {
  uint64_t totalMs = getStopwatchTotalMs(now);
  uint64_t totalSec = totalMs / 1000ULL;
  uint64_t hours = totalSec / 3600ULL;
  uint64_t minutes = (totalSec / 60ULL) % 60ULL;
  uint64_t seconds = totalSec % 60ULL;

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(2, 9, "STOPWATCH");

  char clockBuf[12];
  getCompactClockStr(clockBuf, sizeof(clockBuf));
  int clockW = u8g2.getStrWidth(clockBuf);
  u8g2.drawStr((128 - clockW) / 2, 9, clockBuf);

  const char* status = stopwatchRunning ? "RUN" :
                       (stopwatchAccumulatedMs > 0 ? "PAUS" : "READY");
  int statusW = u8g2.getStrWidth(status);
  u8g2.drawStr(126 - statusW, 9, status);
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

  // Footer Controls: Exactly 3 options as requested!
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.drawStr(2, 61, "1:START/STOP  2:RESET  3:EXIT");
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

  // Load configured Wi-Fi from NVS or fallback to macro
  String storedSsid = settings.getString("wifi_ssid", "");
  String storedPass = settings.getString("wifi_pass", "");
  if (storedSsid.length() > 0) {
    strncpy(configuredSsid, storedSsid.c_str(), sizeof(configuredSsid) - 1);
    strncpy(configuredPass, storedPass.c_str(), sizeof(configuredPass) - 1);
  } else {
    strncpy(configuredSsid, DESKBUDDY_WIFI_SSID, sizeof(configuredSsid) - 1);
    strncpy(configuredPass, DESKBUDDY_WIFI_PASSWORD, sizeof(configuredPass) - 1);
  }

  // Start BLE time & Wi-Fi sync service
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
  if (strlen(configuredSsid) > 0) {
    triggerNetworkSync();
  } else {
    Serial.println("[SYSTEM] No Wi-Fi configured yet. Send 'WIFI:SSID,PASSWORD' in Serial to connect.");
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
