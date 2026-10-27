// ============================================================
//  DeskBuddy Robot — ESP32-C3 Desk Companion Firmware
//  Features:
//  - SH1106 128x64 OLED display with smooth 60fps animations
//  - DHT11 temperature and humidity sensor with room alerts
//  - Passive piezo buzzer with audible tap and alarm feedback
//  - Capacitive touch / push button gesture state machine
//  - Option 4: Dedicated Menu for Pomodoro Timer & Stopwatch
//  - Non-blocking timestamp-based Pomodoro timer & millisecond stopwatch
//  - Automatic background Wi-Fi connection and NTP time sync
//  - OpenWeatherMap integration (current weather & 3-hour forecast)
//  - Bluetooth Low Energy (NimBLE) for instant phone time sync
//  - Emotional expressions (Blink, Heart fireworks, Excited,
//    Petting rabbit, Compliments, Sleepy, Curious, Sad, Celebration)
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <time.h>

// Optional local configuration. Copy secrets.example.h to secrets.h and fill in
// your values; secrets.h is intentionally ignored by Git.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define DESKBUDDY_WIFI_SSID ""
#define DESKBUDDY_WIFI_PASSWORD ""
#define DESKBUDDY_WEATHER_API_KEY ""
#define DESKBUDDY_CITY "Dimapur,IN"
#endif

// ============================================================
//  PIN MAPPING (Preserved for ESP32-C3 DevKitM-1)
// ============================================================
#define SDA_PIN          8   // I2C data  → SH1106 OLED
#define SCL_PIN          9   // I2C clock → SH1106 OLED
#define DHT_PIN          6   // DHT11 single-wire data pin
#define BUZZER_PIN       5   // Passive buzzer
#define TOUCH_PIN        7   // Touch sensor / push button input
#define TOUCH_ACTIVE_LOW 0   // Set to 1 for an active-low push button

// ============================================================
//  CREDENTIALS & CONFIGURATION
// ============================================================
static const char* wifiSsid       = DESKBUDDY_WIFI_SSID;
static const char* wifiPassword   = DESKBUDDY_WIFI_PASSWORD;
static const char* weatherApiKey  = DESKBUDDY_WEATHER_API_KEY;
static const char* weatherCity    = DESKBUDDY_CITY;

// BLE UUIDs
static const char* BLE_DEVICE_NAME       = "DeskBuddy";
static const char* BLE_TIME_SERVICE_UUID = "a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001";
static const char* BLE_TIME_CHAR_UUID    = "a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001";

// ============================================================
//  HARDWARE OBJECTS
// ============================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
DHT dht(DHT_PIN, DHT11);
Preferences settings;

// ============================================================
//  STATE MACHINE DEFINITIONS
// ============================================================
enum State {
  HOME,           // Clock + date + sensor dashboard
  BLINK,          // Eased eye blink + compliment (1 tap)
  HEART,          // Pulsing heart eyes + skyrocket fireworks (2 taps)
  EXCITED,        // Shaking star eyes + playful noise (3 taps)
  TIMER_MENU,     // Dedicated Menu: Pomodoro / Stopwatch / Back (4 taps)
  POMODORO_VIEW,  // Pomodoro countdown timer screen
  STOPWATCH_VIEW, // Persistent millisecond stopwatch screen
  PETTING,        // Rabbit petting mode (long press > 1s)
  COMPLIMENT,     // Bold motivational phrase after BLINK
  SECRET_GIFT,    // Secret love message (3 taps + hold)
  WEATHER_MODE,   // Detailed weather + 3h forecast (6 taps)
  SLEEPY,         // Relaxed sleepy face
  CURIOUS,        // Curious tilting face
  SAD,            // Sad sympathetic face
  LISTENING,      // Attentive listening face
  CELEBRATION     // Celebration confetti face
};

State currentState  = HOME;
State previousState = HOME;

// ============================================================
//  CLOCK & SYNCHRONIZATION
//  clockFormat: 0=12H  1=12H+sec (default)  2=24H  3=24H+sec
// ============================================================
int clockFormat = 1;
bool clockSynchronized = false;

enum ClockSyncSource {
  CLOCK_NONE = 0,
  CLOCK_WIFI_NTP,
  CLOCK_BLE
};
ClockSyncSource clockSyncSource = CLOCK_NONE;

// Auto-blink idle animation every 60 seconds on HOME
const unsigned long AUTO_BLINK_INTERVAL = 60000;
unsigned long lastAutoBlink = 0;
bool isAutoBlink = false;

// ============================================================
//  OPTION 4: POMODORO & STOPWATCH MENU NAVIGATION
// ============================================================
int menuSelection = 0; // 0 = Pomodoro Timer, 1 = Stopwatch, 2 = Back

// ============================================================
//  NON-BLOCKING POMODORO TIMER (TIMESTAMP-BASED)
// ============================================================
const int pomodoroOptions[] = {50, 25, 10, 5, 1}; // Durations in minutes
int currentOptionIndex      = 1;                  // Default: 25 minutes
long pomodoroDurationSec    = 25 * 60;            // Full duration in seconds
bool pomodoroRunning        = false;
unsigned long pomodoroRunStartedAt = 0;
unsigned long pomodoroAccumulatedMs = 0;
bool pomodoroCompleted      = false;
unsigned long pomodoroCompleteAt   = 0;
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

// ============================================================
//  NON-BLOCKING STOPWATCH (TIMESTAMP-BASED & PERSISTENT)
// ============================================================
uint64_t stopwatchAccumulatedMs  = 0;
unsigned long stopwatchRunStartedAt = 0;
bool stopwatchRunning            = false;

uint64_t getStopwatchTotalMs(unsigned long now) {
  return stopwatchAccumulatedMs + (stopwatchRunning ? (uint32_t)(now - stopwatchRunStartedAt) : 0);
}

void saveStopwatch() {
  settings.putULong64("sw_ms", stopwatchAccumulatedMs);
}

// ============================================================
//  ENVIRONMENTAL SENSORS (DHT11)
// ============================================================
float indoorTemperature = NAN;
float indoorHumidity    = NAN;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000; // Sample every 2 seconds

uint8_t sensorAlertFlags = 0; // Bit 0: HOT, Bit 1: COLD, Bit 2: HUMID, Bit 3: DRY
const float TEMP_HIGH_ALERT_C   = 30.0f;
const float TEMP_LOW_ALERT_C    = 16.0f;
const float HUMIDITY_HIGH_ALERT = 70.0f;
const float HUMIDITY_LOW_ALERT  = 25.0f;

// ============================================================
//  WEATHER DATA CACHE
// ============================================================
bool   hasWeatherData     = false;
char   currentWeather[24] = "Clear";
int    currentTemp        = 0;
bool   hasForecastData    = false;
char   forecastWeather[24]= "";
int    forecastTemp       = 0;
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_INTERVAL = 1800000; // 30 minutes

// Expression cycle helper (7 taps)
uint8_t nextExpression = 0;

// Personalization compliments (100 short uplifting phrases)
int selectedCompliment = 0;
const char* const compliments[] = {
  "STAY BRIGHT","CHASE DREAMS","PURE TALENT","BORN STAR","ALWAYS KIND",
  "KEEP GLOWING","SO SMART","TRUE GENIUS","SHINE ON","VERY BRAVE",
  "STAY UNIQUE","REALLY WISE","TOP TIER","SIMPLY BEST","STAY STRONG",
  "SO CREATIVE","HIGH ENERGY","PURE HEART","STAY BOLD","VERY CALM",
  "KEEP RISING","STAY FIERCE","SO HUMBLE","REALLY COOL","STAY REAL",
  "KEEP GOING","SO CAPABLE","TRUE LEADER","STAY RADIANT","SO VIBRANT",
  "REALLY GIFTED","KEEP WINNING","STAY HAPPY","SO POLITE","REALLY FUN",
  "STAY CALM","KEEP SMILING","SO HELPFUL","TRUE ICON","STAY FRESH",
  "KEEP TRYING","SO FOCUSED","REALLY NEAT","STAY ACTIVE","KEEP FLYING",
  "SO HONEST","TRUE GEM","STAY SWEET","KEEP WALKING","SO GRACEFUL",
  "REALLY QUICK","STAY STEADY","KEEP ASKING","SO CHEERFUL","TRUE PRO",
  "STAY WARM","KEEP SHINING","SO LIVELY","REALLY DEEP","STAY TOUGH",
  "KEEP CARING","SO PATIENT","TRUE ORIGINAL","STAY OPEN","KEEP LEARNING",
  "SO GENTLE","REALLY BRIGHT","STAY LOVELY","KEEP HELPING","SO LOYAL",
  "REALLY FAIR","STAY PROUD","KEEP BELIEVING","SO FRIENDLY","TRUE WONDER",
  "STAY READY","KEEP DREAMING","SO KEEN","REALLY SMART","STAY ALERT",
  "KEEP WORKING","SO SHARP","REALLY FINE","STAY VIGILANT","KEEP SMILING",
  "SO RADIANT","REALLY STUNNING","STAY PEACEFUL","KEEP GROWING","SO MAGICAL",
  "REALLY SOLID","STAY CLASSY","KEEP BUILDING","SO FEARLESS","TRUE POWER",
  "STAY BRIGHT","KEEP INSPIRING","SO DRIVEN","REALLY AWESOME","STAY GOLD"
};

// ============================================================
//  SKY-ROCKET FIREWORK SYSTEM (HEART STATE)
// ============================================================
#define MAX_ROCKETS 3

struct Rocket {
  float x, y, vy;
  bool burst;
  float burstAge;
  float burstX[12];
  float burstY[12];
  float burstVx[12];
  float burstVy[12];
  bool active;
};

Rocket rockets[MAX_ROCKETS];
unsigned long lastRocketLaunch = 0;
const unsigned long ROCKET_INTERVAL = 600;

void initRockets() {
  for (int i = 0; i < MAX_ROCKETS; i++) rockets[i].active = false;
  lastRocketLaunch = millis();
}

void launchRocket() {
  for (int i = 0; i < MAX_ROCKETS; i++) {
    if (!rockets[i].active) {
      rockets[i].active   = true;
      rockets[i].burst    = false;
      rockets[i].burstAge = 0;
      rockets[i].x        = random(15, 113);
      rockets[i].y        = 63;
      rockets[i].vy       = -(random(8, 14) / 10.0f);
      break;
    }
  }
}

void burstRocket(int i) {
  rockets[i].burst    = true;
  rockets[i].burstAge = 0;
  for (int p = 0; p < 12; p++) {
    float angle = p * (2.0f * PI / 12.0f);
    float speed = random(8, 18) / 10.0f;
    rockets[i].burstX[p]  = rockets[i].x;
    rockets[i].burstY[p]  = rockets[i].y;
    rockets[i].burstVx[p] = cosf(angle) * speed;
    rockets[i].burstVy[p] = sinf(angle) * speed;
  }
}

void updateAndDrawRockets() {
  unsigned long now = millis();
  static unsigned long previousUpdate = 0;
  float dt = (previousUpdate == 0) ? 1.0f : (float)(now - previousUpdate) / 25.0f;
  if (dt > 3.0f) dt = 3.0f;
  previousUpdate = now;

  if (now - lastRocketLaunch > ROCKET_INTERVAL) {
    launchRocket();
    lastRocketLaunch = now;
  }

  for (int i = 0; i < MAX_ROCKETS; i++) {
    if (!rockets[i].active) continue;

    if (!rockets[i].burst) {
      rockets[i].y += rockets[i].vy * dt;
      int rx = (int)rockets[i].x;
      int ry = (int)rockets[i].y;
      u8g2.drawPixel(rx, ry);
      u8g2.drawPixel(rx, ry + 2);
      u8g2.drawPixel(rx, ry + 4);

      if (ry < 20 || (ry < 30 && random(10) == 0)) {
        burstRocket(i);
      }
    } else {
      rockets[i].burstAge += dt;
      for (int p = 0; p < 12; p++) {
        rockets[i].burstX[p] += rockets[i].burstVx[p] * dt;
        rockets[i].burstY[p] += rockets[i].burstVy[p] * dt;
        rockets[i].burstVy[p] += 0.12f * dt;

        int px = (int)rockets[i].burstX[p];
        int py = (int)rockets[i].burstY[p];
        if (px > 0 && px < 127 && py > 0 && py < 63) {
          u8g2.drawPixel(px, py);
          u8g2.drawPixel(px + 1, py);
          u8g2.drawPixel(px - 1, py);
          u8g2.drawPixel(px, py - 1);
        }
      }
      if (rockets[i].burstAge > 18) rockets[i].active = false;
    }
  }
}

// ============================================================
//  BUZZER & AUDIO FEEDBACK (NON-BLOCKING SCHEDULE)
// ============================================================
void beepClick(int durationMs = 8) {
  digitalWrite(BUZZER_PIN, HIGH);
  delayMicroseconds(durationMs * 100);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepAlarmPulse(int pitchUs) {
  for (int i = 0; i < 35; i++) {
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
//  TOUCH SENSING & GESTURE STATE MACHINE
// ============================================================
unsigned long stateStartTime = 0;
unsigned long touchStartTime = 0;
unsigned long lastTapRelease = 0;
int  tapCount = 0;

bool touchCurrentState   = LOW;
bool touchDebouncedState = LOW;
unsigned long touchLastDebounceTime = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 25;

bool longPressHandled = false;
const unsigned long LONG_PRESS_MS = 1000;
const unsigned long TAP_GAP_MS    = 480; // Settle timeout to resolve multi-taps
const unsigned long SINGLE_TAP_ANIMATION_MS = 2000;
const unsigned long FRAME_INTERVAL_MS = 16;
unsigned long lastFrameAt = 0;

// ============================================================
//  ASYNCHRONOUS NETWORK ENGINE (FreeRTOS Background Task)
// ============================================================
TaskHandle_t networkTaskHandle = NULL;
volatile bool netSyncRequested = false;
volatile bool netTaskBusy       = false;
volatile bool netLastSyncFailed = false;

portMUX_TYPE bleTimeMux = portMUX_INITIALIZER_UNLOCKED;
volatile int64_t pendingBleEpoch = 0;
bool bleConnected = false;

// Safe weather fetching function running strictly inside network task
bool fetchWeatherHttp() {
  if (WiFi.status() != WL_CONNECTED || strlen(weatherApiKey) == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[180];
  snprintf(url, sizeof(url),
    "https://api.openweathermap.org/data/2.5/weather?q=%s&units=metric&appid=%s",
    weatherCity, weatherApiKey);

  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, url)) return false;

  int httpCode = http.GET();
  bool success = false;
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    if (!err) {
      float temp = doc["main"]["temp"] | NAN;
      const char* cond = doc["weather"][0]["main"] | "";
      if (!isnan(temp) && strlen(cond) > 0) {
        currentTemp = (int)lroundf(temp);
        strncpy(currentWeather, cond, sizeof(currentWeather) - 1);
        currentWeather[sizeof(currentWeather) - 1] = '\0';
        hasWeatherData = true;
        success = true;
      }
    }
  }
  http.end();

  if (success) {
    HTTPClient forecastHttp;
    char fUrl[180];
    snprintf(fUrl, sizeof(fUrl),
      "https://api.openweathermap.org/data/2.5/forecast?q=%s&units=metric&appid=%s",
      weatherCity, weatherApiKey);

    if (forecastHttp.begin(client, fUrl)) {
      forecastHttp.setConnectTimeout(4000);
      forecastHttp.setTimeout(4000);
      if (forecastHttp.GET() == HTTP_CODE_OK) {
        StaticJsonDocument<160> filter;
        filter["list"][0]["main"]["temp"] = true;
        filter["list"][0]["weather"][0]["main"] = true;

        StaticJsonDocument<256> fDoc;
        DeserializationError fErr = deserializeJson(
          fDoc, forecastHttp.getStream(), DeserializationOption::Filter(filter));
        if (!fErr) {
          float nextTemp = fDoc["list"][0]["main"]["temp"] | NAN;
          const char* nextCond = fDoc["list"][0]["weather"][0]["main"] | "";
          if (!isnan(nextTemp) && strlen(nextCond) > 0) {
            forecastTemp = (int)lroundf(nextTemp);
            strncpy(forecastWeather, nextCond, sizeof(forecastWeather) - 1);
            forecastWeather[sizeof(forecastWeather) - 1] = '\0';
            hasForecastData = true;
          }
        }
      }
      forecastHttp.end();
    }
  }
  return success;
}

// Background FreeRTOS task handling Wi-Fi, NTP and Weather without blocking UI
void networkWorkerTask(void* parameter) {
  for (;;) {
    if (netSyncRequested) {
      netSyncRequested = false;
      netTaskBusy = true;
      netLastSyncFailed = false;

      if (strlen(wifiSsid) > 0) {
        Serial.println("[NET] Background Wi-Fi connecting...");
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.begin(wifiSsid, wifiPassword);

        unsigned long startConnect = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - startConnect < 9000)) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("[NET] Wi-Fi connected. Synchronizing NTP (IST UTC+05:30)...");
          sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
          configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

          unsigned long startNtp = millis();
          struct tm timeinfo = {};
          bool ntpSuccess = false;

          while (millis() - startNtp < 6000) {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED &&
                getLocalTime(&timeinfo, 100) &&
                timeinfo.tm_year >= (2024 - 1900)) {
              ntpSuccess = true;
              break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
          }

          if (ntpSuccess) {
            Serial.printf("[NET] NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d IST\n",
              timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
              timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            clockSynchronized = true;
            clockSyncSource = CLOCK_WIFI_NTP;
          } else {
            Serial.println("[NET] NTP sync timed out.");
            netLastSyncFailed = true;
          }

          if (strlen(weatherApiKey) > 0) {
            Serial.println("[NET] Updating weather...");
            fetchWeatherHttp();
          }

        } else {
          Serial.println("[NET] Wi-Fi connection timed out.");
          netLastSyncFailed = true;
        }

        // Sleep Wi-Fi radio immediately to save power and prevent BLE interference
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        lastWeatherUpdate = millis();
      }

      netTaskBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void triggerNetworkSync() {
  if (!netTaskBusy) {
    netSyncRequested = true;
  }
}

// ============================================================
//  BLUETOOTH LOW ENERGY (NimBLE Server & Callbacks)
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
  void onConnect(NimBLEServer* pServer) override {
    bleConnected = true;
    Serial.println("[BLE] Client connected.");
  }

  void onDisconnect(NimBLEServer* pServer) override {
    bleConnected = false;
    Serial.println("[BLE] Client disconnected. Resuming advertising...");
    NimBLEDevice::startAdvertising(); // Resume advertising automatically!
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
  Serial.println("[BLE] Advertising started as 'DeskBuddy'.");
}

// ============================================================
//  SENSOR SAMPLING (DHT11)
// ============================================================
void updateSensors(unsigned long now) {
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) indoorTemperature = t;
  if (!isnan(h)) indoorHumidity = h;

  uint8_t alerts = 0;
  if (!isnan(indoorTemperature)) {
    if (indoorTemperature >= TEMP_HIGH_ALERT_C) alerts |= 0x01;
    if (indoorTemperature <= TEMP_LOW_ALERT_C)  alerts |= 0x02;
  }
  if (!isnan(indoorHumidity)) {
    if (indoorHumidity >= HUMIDITY_HIGH_ALERT) alerts |= 0x04;
    if (indoorHumidity <= HUMIDITY_LOW_ALERT)  alerts |= 0x08;
  }
  sensorAlertFlags = alerts;
}

// ============================================================
//  DISPLAY GRAPHICS HELPERS
// ============================================================
float easeInOut(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void drawWeatherIcon(int x, int y) {
  struct tm t;
  bool isNight = false;
  if (getLocalTime(&t)) {
    isNight = (t.tm_hour >= 18 || t.tm_hour < 6);
  }

  if (strcmp(currentWeather, "Rain") == 0 || strcmp(currentWeather, "Drizzle") == 0) {
    u8g2.drawCircle(x, y - 2, 3); u8g2.drawCircle(x + 3, y - 2, 3);
    u8g2.drawLine(x, y + 2, x - 1, y + 4); u8g2.drawLine(x + 3, y + 2, x + 2, y + 4);
  } else if (isNight && strcmp(currentWeather, "Clear") == 0) {
    u8g2.drawCircle(x, y, 4);
    u8g2.setDrawColor(0); u8g2.drawCircle(x + 2, y - 2, 4); u8g2.setDrawColor(1);
  } else if (strcmp(currentWeather, "Clouds") == 0) {
    u8g2.drawCircle(x - 2, y, 3); u8g2.drawCircle(x + 2, y, 4); u8g2.drawBox(x - 2, y + 1, 5, 2);
  } else {
    // Sunny / Default
    u8g2.drawDisc(x, y, 3);
    for (int i = 0; i < 360; i += 90) {
      float angle = i * PI / 180.0f;
      u8g2.drawLine(x, y, x + (int)lroundf(cosf(angle) * 6), y + (int)lroundf(sinf(angle) * 6));
    }
  }
}

void drawMovingEye(int x, int y, bool closed, int pupilOffset) {
  if (closed) {
    u8g2.drawRBox(x - 17, y - 3, 34, 6, 2);
  } else {
    u8g2.drawRBox(x - 17, y - 17, 34, 34, 5);
    u8g2.setDrawColor(0);
    u8g2.drawRBox(x - 2 + pupilOffset, y - 10, 4, 12, 1);
    u8g2.setDrawColor(1);
  }
}

void drawSoftBlinkEye(int x, int y, int pupilOffset, int pupilYOffset, float closure, float scale) {
  closure = constrain(closure, 0.0f, 1.0f);
  scale = constrain(scale, 0.82f, 1.12f);
  int eyeWidth = (int)lroundf(34.0f * scale);
  if (closure > 0.94f) {
    int closedWidth = (int)lroundf(28.0f * scale);
    u8g2.drawRBox(x - closedWidth / 2, y - 2, closedWidth, 4, 2);
    return;
  }
  int eyeHeight = (int)lroundf((34.0f - closure * 30.0f) * scale);
  int eyeTop = y - eyeHeight / 2;
  u8g2.drawRBox(x - eyeWidth / 2, eyeTop, eyeWidth, eyeHeight, min(6, eyeHeight / 2));
  if (closure < 0.48f) {
    int pupilRadius = max(2, (int)lroundf(3.0f * scale));
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + pupilOffset, y + pupilYOffset, pupilRadius);
    u8g2.setDrawColor(1);
    u8g2.drawPixel(x + pupilOffset - 1, y + pupilYOffset - 1);
  }
}

void drawBlinkBrow(int x, int y, bool lifted) {
  int lift = lifted ? 3 : 0;
  u8g2.drawLine(x - 12, y - 20 - lift, x - 2, y - 22 - lift);
  u8g2.drawLine(x + 2, y - 22 - lift, x + 12, y - 20 - lift);
}

void drawUltraMouth(int x, int y, bool surprised) {
  if (surprised) {
    u8g2.drawEllipse(x, y - 8, 10, 12);
    u8g2.setDrawColor(0); u8g2.drawEllipse(x, y - 8, 7, 9); u8g2.setDrawColor(1);
  } else {
    u8g2.drawRBox(x - 15, y - 10, 30, 12, 4);
    u8g2.setDrawColor(0); u8g2.drawBox(x - 15, y - 12, 30, 8); u8g2.setDrawColor(1);
    u8g2.drawPixel(x - 16, y - 10); u8g2.drawPixel(x + 16, y - 10);
  }
}

void drawTinyHeart(int x, int y, int size) {
  int lobe = max(1, size / 2);
  u8g2.drawDisc(x - lobe, y, lobe);
  u8g2.drawDisc(x + lobe, y, lobe);
  u8g2.drawTriangle(x - size, y, x + size, y, x, y + size + 2);
}

void drawLoveSparkle(int x, int y, bool bright) {
  u8g2.drawPixel(x, y);
  if (bright) {
    u8g2.drawLine(x - 2, y, x + 2, y);
    u8g2.drawLine(x, y - 2, x, y + 2);
  }
}

void drawRomanticAccents(unsigned long elapsed) {
  for (int i = 0; i < 3; i++) {
    unsigned long phase = (elapsed + (unsigned long)i * 620) % 2100;
    int y = 51 - (int)(phase * 34 / 2100);
    int x = 64 + (int)lroundf(sinf((elapsed + i * 430) * 0.004f) * 7.0f);
    drawTinyHeart(x, y, 3);
  }
  bool twinkle = (elapsed / 180) % 2 == 0;
  drawLoveSparkle(9, 16, twinkle);
  drawLoveSparkle(118, 18, !twinkle);
  drawLoveSparkle(64, 7, twinkle);

  u8g2.drawLine(52, 46, 56, 48); u8g2.drawLine(51, 49, 55, 51);
  u8g2.drawLine(76, 48, 80, 46); u8g2.drawLine(77, 51, 81, 49);
  u8g2.drawLine(59, 57, 64, 60); u8g2.drawLine(64, 60, 69, 57);
  u8g2.drawPixel(47, 60); u8g2.drawPixel(81, 60);
}

void drawHeartEye(int x, int y, int sz, bool closed, int pupilOffset) {
  if (closed) {
    u8g2.drawCircle(x, y - 5, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.drawCircle(x, y - 6, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  } else {
    u8g2.drawDisc(x - (sz / 2), y, sz / 2);
    u8g2.drawDisc(x + (sz / 2), y, sz / 2);
    u8g2.drawTriangle(x - sz, y + 1, x + sz, y + 1, x, y + sz + 10);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + pupilOffset, y + 2, 4);
    u8g2.setDrawColor(1);
    u8g2.drawPixel(x - (sz / 2) - 1, y - 3);
    u8g2.drawPixel(x - 4, y - 5);
    u8g2.drawPixel(x - 5, y - 4);
  }
}

void drawCrazyEye(int x, int y, int radius) {
  u8g2.drawRBox(x - radius, y - radius, radius * 2, radius * 2, 5);
  u8g2.setDrawColor(0);
  if ((millis() / 50) % 2 == 0) {
    u8g2.drawBox(x - 8, y - 1, 16, 2); u8g2.drawBox(x - 1, y - 8, 2, 16);
  } else {
    u8g2.drawLine(x - 6, y - 6, x + 6, y + 6); u8g2.drawLine(x + 6, y - 6, x - 6, y + 6);
  }
  u8g2.setDrawColor(1);
}

void drawCrazyMouth(int x, int y) {
  u8g2.drawTriangle(x - 20, y - 10, x + 20, y - 10, x, y + 10);
  u8g2.setDrawColor(0); u8g2.drawDisc(x, y + 6, 4); u8g2.setDrawColor(1);
}

void drawPettingEye(int x, int y) {
  u8g2.drawRBox(x - 17, y - 17, 34, 34, 6);
  u8g2.setDrawColor(0);
  u8g2.drawCircle(x, y - 2, 8, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawCircle(x, y - 3, 8, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.setDrawColor(1);
}

void drawRabbitEars(int x, int y, int offset) {
  u8g2.drawRBox(x - 8, y - 30 - offset, 16, 25, 5);
  u8g2.setDrawColor(0); u8g2.drawRBox(x - 4, y - 26 - offset, 8, 15, 2); u8g2.setDrawColor(1);
}

void drawRabbitMouth(int x, int y, bool twitchUp) {
  int yOff = twitchUp ? -2 : 0;
  u8g2.drawLine(x, y + yOff, x - 3, y - 3 + yOff);
  u8g2.drawLine(x, y + yOff, x + 3, y - 3 + yOff);
  u8g2.drawLine(x, y + yOff, x, y + 4 + yOff);
  u8g2.drawLine(x - 10, y, x - 25, y - 5); u8g2.drawLine(x - 10, y + 2, x - 25, y + 5);
  u8g2.drawLine(x + 10, y, x + 25, y - 5); u8g2.drawLine(x + 10, y + 2, x + 25, y + 5);
}

void drawSleepyEye(int x, int y, bool closed) {
  if (closed) {
    u8g2.drawRBox(x - 17, y + 5, 34, 4, 1);
  } else {
    u8g2.drawRBox(x - 17, y - 17, 34, 34, 5);
    u8g2.setDrawColor(0); u8g2.drawBox(x - 18, y - 18, 36, 18); u8g2.drawDisc(x, y + 5, 4);
    u8g2.setDrawColor(1);
  }
}

// ============================================================
//  DRAW HOME (MAIN DASHBOARD)
// ============================================================
void drawHome() {
  struct tm ti = {};
  bool timeValid = clockSynchronized && getLocalTime(&ti);

  // Top header: Date / Sync status and heart badge
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  if (timeValid) {
    const char* days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                            "JUL","AUG","SEP","OCT","NOV","DEC"};
    char dateBuf[20];
    snprintf(dateBuf, sizeof(dateBuf), "%s  %02d %s", days[ti.tm_wday],
             ti.tm_mday, months[ti.tm_mon]);
    u8g2.drawStr(4, 10, dateBuf);
  } else {
    if (netTaskBusy) {
      u8g2.drawStr(4, 10, "SYNCING NTP...");
    } else {
      u8g2.drawStr(4, 10, "DESKBUDDY (NO SYNC)");
    }
  }

  // Heart badge at top right
  drawTinyHeart(117, 7, 4);
  u8g2.drawLine(4, 14, 124, 14);

  // Center Main Clock
  char timeStr[16];
  if (!timeValid) {
    // Unsynchronized clock display (never fake a date)
    if (clockFormat == 1 || clockFormat == 3) {
      strncpy(timeStr, "--:--:--", sizeof(timeStr));
      u8g2.setFont(u8g2_font_logisoso20_tn);
    } else {
      strncpy(timeStr, "--:--", sizeof(timeStr));
      u8g2.setFont(u8g2_font_logisoso28_tn);
    }
    int timeWidth = u8g2.getStrWidth(timeStr);
    u8g2.drawStr((128 - timeWidth) / 2, (clockFormat == 1 || clockFormat == 3) ? 45 : 47, timeStr);
  } else {
    int h12 = ti.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    const char* suffix = (ti.tm_hour >= 12) ? "PM" : "AM";

    if (clockFormat == 0 || clockFormat == 2) {
      u8g2.setFont(u8g2_font_logisoso28_tn);
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d",
               clockFormat == 0 ? h12 : ti.tm_hour, ti.tm_min);
      int timeWidth = u8g2.getStrWidth(timeStr);
      if (clockFormat == 0) {
        u8g2.setFont(u8g2_font_haxrcorp4089_tr);
        int suffixWidth = u8g2.getStrWidth(suffix);
        int left = (128 - timeWidth - suffixWidth - 5) / 2;
        u8g2.setFont(u8g2_font_logisoso28_tn);
        u8g2.drawStr(left, 47, timeStr);
        u8g2.setFont(u8g2_font_haxrcorp4089_tr);
        u8g2.drawStr(left + timeWidth + 5, 45, suffix);
      } else {
        u8g2.drawStr((128 - timeWidth) / 2, 47, timeStr);
      }
    } else {
      u8g2.setFont(u8g2_font_logisoso20_tn);
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
               clockFormat == 1 ? h12 : ti.tm_hour, ti.tm_min, ti.tm_sec);
      int timeWidth = u8g2.getStrWidth(timeStr);
      if (clockFormat == 1) {
        u8g2.setFont(u8g2_font_haxrcorp4089_tr);
        int suffixWidth = u8g2.getStrWidth("P");
        int left = (128 - timeWidth - suffixWidth - 4) / 2;
        u8g2.setFont(u8g2_font_logisoso20_tn);
        u8g2.drawStr(left, 45, timeStr);
        u8g2.setFont(u8g2_font_haxrcorp4089_tr);
        u8g2.drawStr(left + timeWidth + 4, 44, (ti.tm_hour >= 12) ? "P" : "A");
      } else {
        u8g2.drawStr((128 - timeWidth) / 2, 45, timeStr);
      }
    }
  }

  // Alerts & Forecast hints in header
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  if (sensorAlertFlags) {
    const char* alert = (sensorAlertFlags & 0x01) ? "HOT!" :
      (sensorAlertFlags & 0x02) ? "COLD" :
      (sensorAlertFlags & 0x04) ? "HUM!" : "DRY!";
    u8g2.drawStr(80, 10, alert);
  } else if (hasForecastData &&
             (strcmp(forecastWeather, "Rain") == 0 ||
              strcmp(forecastWeather, "Drizzle") == 0 ||
              strcmp(forecastWeather, "Thunderstorm") == 0)) {
    u8g2.drawStr(82, 10, "RAIN");
  }

  // Footer: Indoor Temp, Humidity, Weather
  u8g2.drawLine(4, 50, 124, 50);
  u8g2.drawLine(43, 53, 43, 63);
  u8g2.drawLine(84, 53, 84, 63);

  // Temp
  u8g2.drawRFrame(5, 53, 6, 8, 2); u8g2.drawDisc(8, 61, 2);
  u8g2.drawLine(8, 55, 8, 59);
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.setCursor(14, 62);
  if (isnan(indoorTemperature)) u8g2.print("--.-C");
  else { u8g2.print(indoorTemperature, 1); u8g2.print((char)176); u8g2.print("C"); }

  // Humidity
  u8g2.drawCircle(50, 58, 3);
  u8g2.drawTriangle(47, 56, 53, 56, 50, 52);
  u8g2.setCursor(57, 62);
  if (isnan(indoorHumidity)) u8g2.print("--%");
  else { u8g2.print((int)indoorHumidity); u8g2.print("%"); }

  // Weather icon + Temp
  if (hasWeatherData) drawWeatherIcon(91, 58);
  else u8g2.drawCircle(91, 58, 4);
  u8g2.setCursor(99, 62);
  if (hasWeatherData) { u8g2.print(currentTemp); u8g2.print((char)176); u8g2.print("C"); }
  else u8g2.print("--");
}

// ============================================================
//  DRAW OPTION 4: POMODORO & STOPWATCH MENU
// ============================================================
void drawTimerMenu() {
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  // Inverted Title Bar
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.drawStr(12, 9, "POMODORO & STOPWATCH");
  u8g2.setDrawColor(1);

  // Menu Options
  const char* menuItems[] = {
    "1. POMODORO TIMER",
    "2. STOPWATCH",
    "3. BACK TO HOME"
  };

  u8g2.setFont(u8g2_font_7x14_tf);
  for (int i = 0; i < 3; i++) {
    int y = 25 + (i * 13);
    if (menuSelection == i) {
      // Highlight selected row
      u8g2.drawRBox(2, y - 10, 124, 12, 2);
      u8g2.setDrawColor(0);
      u8g2.drawStr(6, y, menuItems[i]);
      u8g2.setDrawColor(1);
    } else {
      u8g2.drawStr(6, y, menuItems[i]);
    }
  }

  // Footer Navigation Hint
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.drawStr(4, 61, "1=NEXT   2/HOLD=SELECT");
}

// ============================================================
//  DRAW POMODORO SCREEN
// ============================================================
void drawPomodoroScreen(unsigned long now) {
  uint32_t remainingSec = getPomodoroRemainingSeconds(now);

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  // Title with current duration setting
  char titleBuf[32];
  snprintf(titleBuf, sizeof(titleBuf), "POMODORO (%dM)", pomodoroOptions[currentOptionIndex]);
  u8g2.drawStr(4, 10, titleBuf);

  // Status
  const char* statusText = pomodoroRunning ? "RUNNING" :
                           (pomodoroAccumulatedMs > 0 ? "PAUSED" : "READY");
  if (remainingSec <= 5 && remainingSec > 0 && pomodoroRunning) {
    statusText = (now / 250) % 2 == 0 ? "!ALMOST!" : "RUNNING";
  }
  u8g2.drawStr(80, 10, statusText);
  u8g2.drawLine(0, 13, 128, 13);

  // Central Countdown Digits MM:SS
  char tBuf[12];
  snprintf(tBuf, sizeof(tBuf), "%02lu:%02lu", remainingSec / 60UL, remainingSec % 60UL);
  u8g2.setFont(u8g2_font_logisoso24_tn);
  int w = u8g2.getStrWidth(tBuf);

  // Flashing inverted block on last 5 seconds
  if (remainingSec <= 5 && remainingSec > 0 && pomodoroRunning && ((now / 250) % 2 == 0)) {
    u8g2.setDrawColor(1);
    u8g2.drawRBox(12, 18, 104, 30, 3);
    u8g2.setDrawColor(0);
  }
  u8g2.drawStr((128 - w) / 2, 43, tBuf);
  u8g2.setDrawColor(1);

  // Footer Controls
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 51, 128, 51);
  const char* toggleCmd = pomodoroRunning ? "1:PAUSE" : (pomodoroAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footerBuf[36];
  snprintf(footerBuf, sizeof(footerBuf), "%s 2:RST 3:TIME", toggleCmd);
  u8g2.drawStr(2, 61, footerBuf);
  u8g2.drawStr(98, 61, "H:MENU");
}

// ============================================================
//  DRAW STOPWATCH SCREEN
// ============================================================
void drawStopwatchScreen(unsigned long now) {
  uint64_t totalMs = getStopwatchTotalMs(now);
  uint64_t totalSeconds = totalMs / 1000ULL;
  uint64_t hours = totalSeconds / 3600ULL;
  uint64_t minutes = (totalSeconds / 60ULL) % 60ULL;
  uint64_t seconds = totalSeconds % 60ULL;

  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(4, 10, "STOPWATCH");
  const char* statusText = stopwatchRunning ? "RUNNING" :
                           (stopwatchAccumulatedMs > 0 ? "PAUSED" : "READY");
  u8g2.drawStr(80, 10, statusText);
  u8g2.drawLine(0, 13, 128, 13);

  char watchText[16];
  if (hours > 0) {
    snprintf(watchText, sizeof(watchText), "%02llu:%02llu:%02llu",
             (unsigned long long)(hours % 100ULL),
             (unsigned long long)minutes, (unsigned long long)seconds);
    u8g2.setFont(u8g2_font_logisoso20_tn);
    int w = u8g2.getStrWidth(watchText);
    u8g2.drawStr((128 - w) / 2, 42, watchText);
  } else {
    // MM:SS format with lively tenths of second
    uint64_t tenths = (totalMs % 1000ULL) / 100ULL;
    snprintf(watchText, sizeof(watchText), "%02llu:%02llu.%llu",
             (unsigned long long)minutes, (unsigned long long)seconds, (unsigned long long)tenths);
    u8g2.setFont(u8g2_font_logisoso22_tn);
    int w = u8g2.getStrWidth(watchText);
    u8g2.drawStr((128 - w) / 2, 42, watchText);
  }

  // Footer: 1:TOGGLE 2:RESET HOLD:BACK
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawLine(0, 51, 128, 51);
  const char* toggleCmd = stopwatchRunning ? "1:PAUSE" : (stopwatchAccumulatedMs > 0 ? "1:RESUME" : "1:START");
  char footerBuf[36];
  snprintf(footerBuf, sizeof(footerBuf), "%s  2:RESET", toggleCmd);
  u8g2.drawStr(4, 61, footerBuf);
  u8g2.drawStr(98, 61, "H:MENU");
}

void drawWeatherScreen() {
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(43, 12, "WEATHER");
  if (!hasWeatherData) {
    u8g2.drawStr(12, 34, "NO WEATHER DATA");
    u8g2.drawStr(5, 50, "SET WIFI + API KEY");
    return;
  }
  u8g2.setFont(u8g2_font_helvB12_tf);
  u8g2.drawStr(6, 31, "NOW");
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(6, 44); u8g2.print(currentWeather);
  u8g2.setFont(u8g2_font_helvB12_tf);
  u8g2.setCursor(6, 62); u8g2.print(currentTemp); u8g2.print((char)176); u8g2.print("C");
  u8g2.drawLine(64, 20, 64, 63);
  u8g2.setFont(u8g2_font_helvB12_tf);
  u8g2.drawStr(72, 31, "IN 3H");
  u8g2.setFont(u8g2_font_6x10_tf);
  if (hasForecastData) {
    u8g2.setCursor(72, 44); u8g2.print(forecastWeather);
    u8g2.setFont(u8g2_font_helvB12_tf);
    u8g2.setCursor(72, 62); u8g2.print(forecastTemp); u8g2.print((char)176); u8g2.print("C");
  } else {
    u8g2.drawStr(72, 49, "UNAVAILABLE");
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
  dht.begin();

  // Configure India Standard Time (UTC+05:30)
  setenv("TZ", "IST-5:30", 1);
  tzset();

  settings.begin("deskbuddy", false);
  stopwatchAccumulatedMs = settings.getULong64("sw_ms", 0);

  initRockets();

  // --- BOOT ANIMATION: "HELLO BUDDY" ---
  u8g2.setFont(u8g2_font_logisoso20_tf);
  const char* b1 = "HELLO";
  const char* b2 = "BUDDY";
  for (int i = 1; i <= 5; i++) {
    u8g2.clearBuffer();
    char buf[8]; strncpy(buf, b1, i); buf[i] = '\0';
    u8g2.drawStr(25, 35, buf);
    u8g2.sendBuffer();
    delay(40);
  }
  for (int i = 1; i <= 5; i++) {
    u8g2.clearBuffer();
    u8g2.drawStr(25, 35, "HELLO");
    char buf[8]; strncpy(buf, b2, i); buf[i] = '\0';
    u8g2.drawStr(25, 60, buf);
    u8g2.sendBuffer();
    delay(40);
  }
  for (int j = 0; j < 10; j++) {
    u8g2.clearBuffer();
    u8g2.drawStr(25, 35, "HELLO");
    u8g2.drawStr(25, 60, "BUDDY");
    int lineW = (sinf(j / 2.0f) * 25) + 40;
    u8g2.drawBox(64 - (lineW / 2), 62, lineW, 2);
    u8g2.sendBuffer();
    delay(20);
  }

  // Start BLE time service
  startBleService();

  // Create FreeRTOS background task for non-blocking Wi-Fi, NTP, & Weather
  xTaskCreate(
    networkWorkerTask,
    "net_worker",
    4096,
    NULL,
    1,
    &networkTaskHandle
  );

  // Trigger non-blocking network sync if Wi-Fi credentials exist
  if (strlen(wifiSsid) > 0) {
    triggerNetworkSync();
  }

  touchCurrentState = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  touchDebouncedState = touchCurrentState;
  lastAutoBlink = millis();
  lastFrameAt = millis();

  beepClick(15);
  Serial.println("[SYSTEM] DeskBuddy Robot initialized.");
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
    clockSyncSource = CLOCK_BLE;
    beepClick(25);
    Serial.printf("[BLE] Clock synchronized: epoch %lld\n", (long long)bleEpoch);
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

  // Edge A: Finger Pressed (Rising Edge)
  if (touching && !prevDebounced) {
    touchStartTime = now;
    longPressHandled = false;
  }

  // Edge B: Long Press Detection (> 1000ms hold)
  if (touching && !longPressHandled && (now - touchStartTime >= LONG_PRESS_MS)) {
    longPressHandled = true;

    // Check if 3 taps preceded this hold -> SECRET_GIFT (3 Taps + Hold)
    if (tapCount == 3 || (tapCount == 2 && (now - lastTapRelease < 1500))) {
      currentState   = SECRET_GIFT;
      stateStartTime = now;
      tapCount       = 0;
      beepClick(30);
    } else if (currentState == TIMER_MENU) {
      // Long press in menu selects currently highlighted item
      if (menuSelection == 0) {
        currentState = POMODORO_VIEW;
      } else if (menuSelection == 1) {
        currentState = STOPWATCH_VIEW;
      } else {
        currentState = HOME;
      }
      beepClick(20);
      tapCount = 0;
    } else if (currentState == POMODORO_VIEW || currentState == STOPWATCH_VIEW) {
      // Long press in Pomodoro/Stopwatch returns to menu
      currentState = TIMER_MENU;
      beepClick(15);
      tapCount = 0;
    } else if (currentState != PETTING && currentState != SECRET_GIFT) {
      // Normal Long Press from Home -> PETTING (rabbit face)
      previousState   = currentState;
      currentState    = PETTING;
      stateStartTime  = now;
      tapCount        = 0;
    }
  }

  // Edge C: Finger Released (Falling Edge)
  if (!touching && prevDebounced) {
    if (longPressHandled) {
      // Releasing finger after a long press must NEVER register as a tap!
      longPressHandled = false;
      tapCount = 0;

      if (currentState == PETTING) {
        currentState = previousState;
      } else if (currentState == SECRET_GIFT) {
        currentState = HOME;
      }
    } else {
      // Normal tap release
      tapCount++;
      lastTapRelease = now;
      beepTapFeedback(tapCount); // Immediate audible acknowledgment
    }
  }

  // ── 3. TAP SEQUENCE RESOLUTION ───────────────────────────
  // Evaluated once TAP_GAP_MS has passed since the last tap release
  if (tapCount > 0 && !touching && (now - lastTapRelease >= TAP_GAP_MS)) {
    isAutoBlink = false;

    if (currentState == TIMER_MENU) {
      if (tapCount == 1) {
        // 1 Tap: Advance menu selection
        menuSelection = (menuSelection + 1) % 3;
        beepClick(10);
      } else if (tapCount == 2) {
        // 2 Taps: Select current menu item
        if (menuSelection == 0) {
          currentState = POMODORO_VIEW;
        } else if (menuSelection == 1) {
          currentState = STOPWATCH_VIEW;
        } else {
          currentState = HOME;
        }
        beepClick(20);
      } else if (tapCount >= 3) {
        // 3+ Taps: Return to Home
        currentState = HOME;
        beepClick(15);
      }
    } else if (currentState == POMODORO_VIEW) {
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
      } else if (tapCount >= 4) {
        // 4 Taps: Return to Timer Menu
        currentState = TIMER_MENU;
        beepClick(15);
      }
    } else if (currentState == STOPWATCH_VIEW) {
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
        // 2 Taps: RESET to zero
        stopwatchRunning = false;
        stopwatchAccumulatedMs = 0;
        saveStopwatch();
        beepClick(25);
      } else if (tapCount >= 3) {
        // 3 Taps: Return to Timer Menu
        if (stopwatchRunning) {
          stopwatchAccumulatedMs += (uint32_t)(now - stopwatchRunStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentState = TIMER_MENU;
        beepClick(15);
      }
    } else {
      // Global Gestures from HOME or active expression views:
      if (tapCount == 1) {
        currentState   = BLINK;
        stateStartTime = now;
      } else if (tapCount == 2) {
        initRockets();
        currentState   = HEART;
        stateStartTime = now;
      } else if (tapCount == 3) {
        currentState   = EXCITED;
        stateStartTime = now;
      } else if (tapCount == 4) {
        currentState   = TIMER_MENU;
        menuSelection  = 0;
        beepClick(15);
      } else if (tapCount == 5) {
        clockFormat = (clockFormat + 1) % 4;
        beepClick(15);
      } else if (tapCount == 6) {
        currentState   = WEATHER_MODE;
        stateStartTime = now;
      } else if (tapCount == 7) {
        switch (nextExpression) {
          case 0: currentState = SLEEPY; break;
          case 1: currentState = CURIOUS; break;
          case 2: currentState = SAD; break;
          case 3: currentState = LISTENING; break;
          default: currentState = CELEBRATION; break;
        }
        nextExpression = (nextExpression + 1) % 5;
        stateStartTime = now;
      } else if (tapCount == 8) {
        triggerNetworkSync();
        beepClick(25);
      }
    }
    tapCount = 0;
  }

  // ── 4. BACKGROUND SENSOR & SCHEDULER UPDATES ─────────────
  if (currentState != BLINK && currentState != HEART && currentState != EXCITED &&
      currentState != PETTING && currentState != COMPLIMENT && !touching) {
    updateSensors(now);
  }

  // Periodic weather refresh (every 30 mins) & NTP hourly sync while in HOME
  if (currentState == HOME && !touching && !netTaskBusy) {
    if (now - lastWeatherUpdate >= WEATHER_INTERVAL) {
      triggerNetworkSync();
    }
  }

  // Auto-Blink idle timer (every 60s in HOME)
  if (currentState == HOME && (now - lastAutoBlink >= AUTO_BLINK_INTERVAL)) {
    isAutoBlink = true;
    currentState = BLINK;
    stateStartTime = now;
    lastAutoBlink = now;
  }

  // ── 5. POMODORO TIMER COMPLETION & WARNINGS ──────────────
  if (pomodoroRunning) {
    uint32_t rem = getPomodoroRemainingSeconds(now);
    if (rem == 0) {
      // Countdown reached zero: completion alarm and auto-reset
      pomodoroRunning = false;
      pomodoroCompleted = true;
      pomodoroCompleteAt = now;
      for (int b = 0; b < 3; b++) {
        beepAlarmPulse(450);
        delay(40);
      }
      // Automatically reset countdown to selected duration after completion
      pomodoroAccumulatedMs = 0;
      pomodoroCompleted = false;
      lastWarningSec = 0;
    } else if (rem <= 5) {
      // 5-second countdown warning beeps (at 5, 4, 3, 2, 1)
      if (lastWarningSec != rem) {
        lastWarningSec = rem;
        int pitch = map(rem, 5, 1, 800, 400);
        beepAlarmPulse(pitch);
      }
    }
  }

  // ── 6. STATE RENDERING PIPELINE ──────────────────────────
  switch (currentState) {
    case HOME:
      drawHome();
      break;

    case TIMER_MENU:
      drawTimerMenu();
      break;

    case POMODORO_VIEW:
      drawPomodoroScreen(now);
      break;

    case STOPWATCH_VIEW:
      drawStopwatchScreen(now);
      break;

    case WEATHER_MODE:
      drawWeatherScreen();
      if (now - stateStartTime > 8000) currentState = HOME;
      break;

    case SLEEPY: {
      unsigned long elapsed = now - stateStartTime;
      bool closed = (elapsed % 1800) > 350;
      drawSleepyEye(30, 28, closed);
      drawSleepyEye(98, 28, closed);
      drawUltraMouth(64, 60, false);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(54, 12, "SLEEPY");
      if (elapsed > 4500) currentState = HOME;
      break;
    }

    case CURIOUS: {
      int look = (int)(sinf((now - stateStartTime) * 0.004f) * 5.0f);
      drawMovingEye(30, 27, false, look);
      drawMovingEye(98, 27, false, -look);
      drawUltraMouth(64, 60, true);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(51, 12, "CURIOUS?");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case SAD: {
      drawMovingEye(30, 28, false, 0);
      drawMovingEye(98, 28, false, 0);
      u8g2.drawLine(15, 7, 43, 13);
      u8g2.drawLine(85, 13, 113, 7);
      u8g2.drawCircle(64, 59, 9, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(57, 12, "OH...");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case LISTENING: {
      drawMovingEye(30, 29, false, 0);
      drawMovingEye(98, 29, false, 0);
      u8g2.drawCircle(64, 57, 7);
      u8g2.setDrawColor(0); u8g2.drawCircle(64, 57, 4); u8g2.setDrawColor(1);
      u8g2.drawCircle(8, 32, 8, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
      u8g2.drawCircle(120, 32, 8, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_LOWER_LEFT);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(43, 12, "LISTENING");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case CELEBRATION: {
      unsigned long elapsed = now - stateStartTime;
      int size = 15 + (int)(sinf(elapsed * 0.012f) * 3.0f);
      drawHeartEye(32, 25, size, false, 0);
      drawHeartEye(96, 25, size, false, 0);
      drawUltraMouth(64, 60, false);
      for (int i = 0; i < 6; i++) {
        int x = (i * 23 + (elapsed / 40) % 23) % 128;
        int y = (i * 13 + (elapsed / 25) % 64);
        u8g2.drawPixel(x, y);
      }
      if (elapsed > 4500) currentState = HOME;
      break;
    }

    case COMPLIMENT: {
      unsigned long elapsed = now - stateStartTime;
      const char* phrase = compliments[selectedCompliment];
      char w1[16] = "", w2[16] = "", w3[16] = "";
      int wordCount = 0;

      const char* p = phrase;
      char* target = w1;
      int idx = 0;
      while (*p && wordCount < 3) {
        if (*p == ' ') {
          target[idx] = '\0';
          wordCount++;
          if (wordCount == 1) target = w2;
          else if (wordCount == 2) target = w3;
          idx = 0;
        } else {
          if (idx < 15) target[idx++] = *p;
        }
        p++;
      }
      target[idx] = '\0';
      wordCount++;

      if (wordCount == 1) {
        u8g2.setFont(u8g2_font_logisoso20_tf);
        int w = u8g2.getStrWidth(w1);
        u8g2.drawStr(64 - (w / 2), 42, w1);
      } else if (wordCount == 2) {
        u8g2.setFont(u8g2_font_helvB12_tf);
        u8g2.drawStr(64 - (u8g2.getStrWidth(w1) / 2), 30, w1);
        u8g2.drawStr(64 - (u8g2.getStrWidth(w2) / 2), 52, w2);
      } else {
        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.drawStr(64 - (u8g2.getStrWidth(w1) / 2), 18, w1);
        u8g2.drawStr(64 - (u8g2.getStrWidth(w2) / 2), 38, w2);
        u8g2.drawStr(64 - (u8g2.getStrWidth(w3) / 2), 58, w3);
      }

      if (elapsed >= 1000) currentState = HOME;
      break;
    }

    case PETTING: {
      unsigned long elapsed = now - stateStartTime;
      int bounce = abs((int)(3.0f * sinf(elapsed * 0.004f)));
      drawRabbitEars(30, 17 - bounce, bounce);
      drawRabbitEars(98, 17 - bounce, bounce);
      drawPettingEye(30, 17 - bounce);
      drawPettingEye(98, 17 - bounce);
      drawRabbitMouth(64, 56, (elapsed % 500 < 250));

      // Gentle purring sound
      if (elapsed % 150 < 30) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(100);
        digitalWrite(BUZZER_PIN, LOW);
      }
      break;
    }

    case BLINK: {
      unsigned long elapsed = now - stateStartTime;
      float gaze = 0.0f;
      float closure = 0.0f;
      float eyeScale = 1.0f;

      if (elapsed < 400) {
        gaze = -7.0f * easeInOut(elapsed / 400.0f);
      } else if (elapsed < 800) {
        gaze = -7.0f + 14.0f * easeInOut((elapsed - 400) / 400.0f);
      } else if (elapsed < 1000) {
        gaze = 7.0f * (1.0f - easeInOut((elapsed - 800) / 200.0f));
      } else if (elapsed < 1400) {
        float breath = sinf((elapsed - 1400) * (PI / 600.0f));
        eyeScale = 1.0f + 0.035f * breath;
      }

      if (elapsed >= 1000 && elapsed < 1120) {
        closure = easeInOut((elapsed - 1000) / 120.0f);
      } else if (elapsed >= 1120 && elapsed < 1260) {
        closure = 1.0f;
      } else if (elapsed >= 1260 && elapsed < 1400) {
        closure = 1.0f - easeInOut((elapsed - 1260) / 140.0f);
      }

      if (touching && currentState == BLINK) {
        gaze = 0.0f; closure = 0.0f; eyeScale = 1.04f;
      }

      bool surprised = elapsed < 160;
      int pX = (int)lroundf(gaze);
      drawBlinkBrow(30, 25, surprised || elapsed >= 1400);
      drawBlinkBrow(98, 25, surprised || elapsed >= 1400);
      drawSoftBlinkEye(30, 25, pX, 0, closure, eyeScale);
      drawSoftBlinkEye(98, 25, pX, 0, closure, eyeScale);

      int smileBob = (elapsed >= 1400) ? (int)lroundf(sinf((elapsed - 1400) * (PI / 300.0f))) : 0;
      drawUltraMouth(64, 60 + smileBob, surprised);

      if (elapsed >= 1400) {
        u8g2.drawLine(51, 49, 55, 51); u8g2.drawPixel(50, 47);
        u8g2.drawLine(73, 51, 77, 49); u8g2.drawPixel(78, 47);
        bool sparkleBright = ((elapsed / 140) % 2 == 0);
        drawLoveSparkle(9, 40, sparkleBright);
        drawLoveSparkle(118, 40, sparkleBright);
      }

      if (elapsed >= 1000 && elapsed < 1020 && !isAutoBlink) beepClick(2);

      if (elapsed >= SINGLE_TAP_ANIMATION_MS) {
        currentState = isAutoBlink ? HOME : COMPLIMENT;
        if (!isAutoBlink) {
          selectedCompliment = random(0, 100);
          stateStartTime = millis();
        }
      }
      break;
    }

    case HEART: {
      unsigned long elapsed = now - stateStartTime;
      int pX = 0;
      bool eClosed = false;

      if      (elapsed < 400)  pX = -6;
      else if (elapsed < 800)  pX = 6;
      else if (elapsed < 1000) pX = 0;
      else if (elapsed < 1400) eClosed = true;

      float breath = sinf(elapsed * (2.0f * PI / 1900.0f));
      float beatPhase = fmodf((float)elapsed, 920.0f) / 920.0f;
      float d1 = (beatPhase - 0.12f) / 0.055f;
      float d2 = (beatPhase - 0.31f) / 0.065f;
      float beat1 = expf(-0.5f * d1 * d1);
      float beat2 = expf(-0.5f * d2 * d2);
      float pulseSize = 16.0f + 1.6f * breath + 3.0f * beat1 + 1.5f * beat2;
      int heartSize = (int)lroundf(pulseSize);

      updateAndDrawRockets();

      int heartY = 25 - (int)lroundf((beat1 * 1.4f) + (breath * 0.5f));
      drawHeartEye(32, heartY, heartSize, eClosed, pX);
      drawHeartEye(96, heartY, heartSize, eClosed, pX);
      drawRomanticAccents(elapsed);

      // Heartbeat audio "lub-dub"
      if (elapsed % 700 < 20 || (elapsed % 700 > 120 && elapsed % 700 < 140)) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(160);
        digitalWrite(BUZZER_PIN, LOW);
      }

      if (elapsed > 4500) currentState = HOME;
      break;
    }

    case EXCITED: {
      unsigned long elapsed = now - stateStartTime;
      int sX = random(-2, 3), sY = random(-2, 3);
      drawCrazyMouth(64 + sX, 52 + sY);
      int ePulse = (elapsed % 200 < 100) ? 2 : 0;
      drawCrazyEye(30 + sX, 22 + sY, 17 + ePulse);
      drawCrazyEye(98 + sX, 22 + sY, 17 - ePulse);

      for (int i = 0; i < 4; i++) {
        int rx = random(0, 128), ry = random(0, 64);
        u8g2.drawLine(rx, ry, rx + random(-5, 6), ry + random(-5, 6));
      }

      digitalWrite(BUZZER_PIN, HIGH);
      delayMicroseconds(random(40, 160));
      digitalWrite(BUZZER_PIN, LOW);

      if (elapsed > 2500) currentState = HOME;
      break;
    }

    case SECRET_GIFT: {
      u8g2.setFont(u8g2_font_logisoso20_tf);
      const char* sgTitle = "DESKBUDDY";
      u8g2.drawStr(64 - (u8g2.getStrWidth(sgTitle) / 2), 26, sgTitle);
      u8g2.setFont(u8g2_font_helvB14_tf);
      const char* sgMsg = "LOVE YOU!";
      u8g2.drawStr(64 - (u8g2.getStrWidth(sgMsg) / 2), 54, sgMsg);

      // Decorative pulsing hearts around message
      int pulse = (int)(sinf((now - stateStartTime) * 0.01f) * 2.0f);
      drawTinyHeart(14, 48, 4 + pulse);
      drawTinyHeart(114, 48, 4 + pulse);

      if ((now / 200) % 2 == 0) {
        digitalWrite(BUZZER_PIN, HIGH);
        delayMicroseconds(120);
        digitalWrite(BUZZER_PIN, LOW);
      }
      if (now - stateStartTime > 5000) currentState = HOME;
      break;
    }
  }

  u8g2.sendBuffer();

  unsigned long frameElapsed = millis() - lastFrameAt;
  if (frameElapsed < FRAME_INTERVAL_MS) {
    delay(FRAME_INTERVAL_MS - frameElapsed);
  }
  lastFrameAt = millis();
}
