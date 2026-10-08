// ============================================================
//  R O M I  —  Robot Companion Firmware  v3
//  Hardware : ESP32-C3 + SH1106 128×64 OLED + DHT11 + Buzzer
//  Display  : U8g2 library (full-buffer mode)
//
//  CHANGES IN v3:
//    • Timer auto-resets to chosen duration when countdown ends
//    • 5-second countdown warning: escalating beeps at 5,4,3,2,1s
//    • Timer finish: loud triple alarm-clock beep sequence
//    • HEART state: larger pulsing hearts + breathing scale effect
//    • Fireworks replaced with sky-rocket: dot trails up → burst
//    • Touch tap feedback: distinct ascending beep tones per tap count
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include "time.h"

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
//  PIN MAPPING — change if you rewire hardware
// ============================================================
#define SDA_PIN    8   // I2C data  → OLED
#define SCL_PIN    9   // I2C clock → OLED
#define DHT_PIN    6   // DHT11 data pin
#define BUZZER_PIN 5   // Passive buzzer
#define TOUCH_PIN  7   // Touch / button input
#define TOUCH_ACTIVE_LOW 0 // Set to 1 for an active-low push button


// ============================================================
//  WiFi & API CREDENTIALS
// ============================================================
const char* ssid       = DESKBUDDY_WIFI_SSID;
const char* password   = DESKBUDDY_WIFI_PASSWORD;
const char* weatherKey = DESKBUDDY_WEATHER_API_KEY;
const char* city       = DESKBUDDY_CITY;


// ============================================================
//  HARDWARE OBJECTS
// ============================================================
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
DHT dht(DHT_PIN, DHT11);


// ============================================================
//  STATE MACHINE
//  Add new states here, then add a matching case in loop().
// ============================================================
enum State {
  HOME,         // Clock + date + sensor dashboard
  BLINK,        // Eased eyes + compliment               (1 tap)
  HEART,        // Pulsing heart eyes + rocket fireworks (2 taps)
  EXCITED,      // Shaking star eyes + noise             (5 taps)
  TIMER_MODE,   // Pomodoro countdown (from stopwatch)
  STOPWATCH_MODE,
  PETTING,      // Rabbit mode                     (long press)
  COMPLIMENT,   // Bold motivational text after BLINK
  SECRET_GIFT,  // Hidden love message (9 taps + hold)
  WEATHER_MODE,
  SLEEPY, CURIOUS, SAD, LISTENING, CELEBRATION
};

State currentState  = HOME;
State previousState = HOME;


// ============================================================
//  CLOCK SETTINGS
//  clockFormat: 0=12H  1=12H+sec (boot default)  2=24H  3=24H+sec
//  Cycle with 4 taps from HOME.
// ============================================================
int clockFormat = 1; // Boot in 12-hour format with seconds.

const unsigned long AUTO_BLINK_INTERVAL = 60000; // Auto-blink every 60s
unsigned long lastAutoBlink = 0;
bool isAutoBlink = false; // true = silent auto-blink (no compliment after)


// ============================================================
//  TIMER SETTINGS
// ============================================================
bool isTimerRunning  = false; // Countdown actively ticking
bool wasTimerRunning = false; // Saved state before PETTING — restored on exit

// Selectable durations (minutes). Cycle with triple-tap in TIMER_MODE.
// If you add/remove entries, update the % modulus in the tap handler.
int  timerOptions[]    = {50, 25, 10, 5, 1};
int  currentOptionIndex = 1;          // Default: 25 min
long remainingSeconds   = 25 * 60;    // Live countdown (seconds)
long timerFullSeconds   = 25 * 60;    // Full duration of the current selection
                                       // ← NEW: used to reset after completion

unsigned long lastTick = 0;           // Timestamp of last 1-second tick
Preferences settings;
uint64_t stopwatchElapsedMs = 0;
unsigned long stopwatchStartedAt = 0;
bool stopwatchRunning = false;

uint64_t getStopwatchElapsedMs(unsigned long now) {
  return stopwatchElapsedMs + (stopwatchRunning ? (uint32_t)(now - stopwatchStartedAt) : 0);
}

void saveStopwatch() {
  settings.putULong64("sw_ms", stopwatchElapsedMs);
}

// Warning & alarm state flags
bool warningBeepActive = false; // true while 5-second warning sequence is playing
int  lastWarningSecond = -1;    // Tracks which warning second we last beeped on


// ============================================================
//  TOUCH & GESTURE TRACKING
// ============================================================
unsigned long stateStartTime = 0; // When current state started (for animations)
unsigned long lastTouchTime  = 0; // Timestamp of last press or release event
int  tapCount       = 0;          // Tap counter — cleared after TAP_GAP_MS silence
bool lastTouchState = LOW;        // Previous frame touch state for edge detection
bool secretGiftArmed = false;
unsigned long secretGiftArmedAt = 0;
const unsigned long SECRET_GIFT_ARM_MS = 5000;

const unsigned long LONG_PRESS_MS = 1000; // Hold > 1s → PETTING
const unsigned long TAP_GAP_MS    = 850;  // Allow relaxed multi-taps before choosing an action
const unsigned long SINGLE_TAP_ANIMATION_MS = 2000;
const unsigned long TOUCH_DEBOUNCE_MS = 20;
bool touchRawState = LOW;
unsigned long touchRawChangedAt = 0;
const unsigned long FRAME_INTERVAL_MS = 16; // Up to 60 FPS; OLED I2C transfer may limit this
unsigned long lastFrameAt = 0;


// ============================================================
//  WEATHER
// ============================================================
bool   wifiEverConnected  = false;
bool   hasWeatherData     = false;
String currentWeather     = "Clear";
int    currentTemp        = 0;
bool hasForecastData = false;
String forecastWeather = "";
int forecastTemp = 0;
bool showForecast = false;
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_INTERVAL = 1800000; // 30 min
unsigned long lastClockSyncAttempt = 0;
const unsigned long CLOCK_SYNC_INTERVAL = 3600000; // Retry NTP every hour.
bool weatherConnectionInProgress = false;
unsigned long weatherConnectionStarted = 0;
const unsigned long WEATHER_CONNECTION_TIMEOUT = 10000;

float indoorTemperature = NAN;
float indoorHumidity = NAN;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000;
uint8_t sensorAlertFlags = 0;
const float TEMP_HIGH_ALERT_C = 30.0f;
const float TEMP_LOW_ALERT_C = 16.0f;
const float HUMIDITY_HIGH_ALERT = 70.0f;
const float HUMIDITY_LOW_ALERT = 25.0f;

enum ClockSyncSource { CLOCK_BUILD_TIME, CLOCK_SAVED_WIFI, CLOCK_OPEN_WIFI, CLOCK_BLE };
ClockSyncSource clockSyncSource = CLOCK_BUILD_TIME;
volatile int64_t pendingBleEpoch = 0;
bool clockSyncRetryRequested = false;
bool clockSyncFailed = false;
uint8_t nextExpression = 0;
const char *BLE_TIME_SERVICE_UUID = "a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001";
const char *BLE_TIME_CHAR_UUID = "a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001";
portMUX_TYPE bleTimeMux = portMUX_INITIALIZER_UNLOCKED;


// ============================================================
//  PERSONALIZATION
// ============================================================
int selectedCompliment = 0;


// ============================================================
//  SKY-ROCKET FIREWORK SYSTEM
//  Each rocket launches from the bottom, rises, then bursts.
//  Up to MAX_ROCKETS live simultaneously.
// ============================================================
#define MAX_ROCKETS 3  // Increase for more rockets, but costs more RAM

struct Rocket {
  float x;           // Horizontal position (pixels)
  float y;           // Current vertical position (starts near bottom)
  float vy;          // Vertical velocity (negative = moving up)
  bool  burst;       // false = rising, true = bursting
  float burstAge;    // Animation frames since burst started
  float burstX[12];  // X coords of burst particles
  float burstY[12];  // Y coords of burst particles
  float burstVx[12]; // Particle velocity X
  float burstVy[12]; // Particle velocity Y
  bool  active;      // Slot in use?
};

Rocket rockets[MAX_ROCKETS];
unsigned long lastRocketLaunch = 0;
// Launch a new rocket every 600ms during HEART state
const unsigned long ROCKET_INTERVAL = 600;


// ============================================================
//  BUZZER HELPERS
//  These small inline helpers keep buzzer logic readable.
//  UI beeps toggle the pin directly with microsecond delays.
// ============================================================

// Single short click — used for basic UI confirmation
void beepClick(int durationMs = 8) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

// Ascending chirp — plays a rising two-tone sequence
// Used as tap feedback: higher tap count → higher pitch feel
// freqDelay: lower = higher perceived pitch (shorter HIGH time)
void beepChirp(int freqDelay, int cycles = 80) {
  for (int i = 0; i < cycles; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(freqDelay);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(freqDelay);
  }
}

// Warning beep — single mid-pitch pulse, used for 5s countdown
void beepWarning() {
  for (int i = 0; i < 120; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(800);
    digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(800);
  }
}

// Alarm beep — loud triple burst, used when timer finishes
// Three groups of rapid buzzing separated by 80ms gaps
void beepAlarm() {
  for (int rep = 0; rep < 3; rep++) {
    for (int i = 0; i < 200; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(400);
      digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(400);
    }
    delay(80); // Brief pause between alarm bursts
  }
}

// Tap feedback beep — pitch rises with tap count
// 1 tap = lowest, 5 taps = highest
// baseDelay controls the wave period: smaller = higher pitch
void beepTapFeedback(int tapNum) {
  // Each successive tap gets a shorter delay = higher perceived pitch
  // Delays (µs): 1→1800, 2→1500, 3→1200, 4→900, 5→600
  int delays[] = {1800, 1500, 1200, 900, 600};
  int d = delays[constrain(tapNum - 1, 0, 4)];
  // Play two quick bursts to make it feel snappy
  beepChirp(d, 60);
  delay(15);
  beepChirp(d, 40);
}

// ============================================================
//  FUNCTION: updateWeather()
// ============================================================
bool updateWeather() {
  if (WiFi.status() != WL_CONNECTED || strlen(weatherKey) == 0) return false;

  WiFiClientSecure client;
  client.setInsecure(); // Encrypts traffic; use a pinned CA certificate for full validation.
  HTTPClient http;
  String url = "https://api.openweathermap.org/data/2.5/weather?q="
               + String(city) + "&units=metric&appid=" + String(weatherKey);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) return false;
  int httpCode = http.GET();
  bool success = false;
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<768> document;
    DeserializationError error = deserializeJson(document, http.getStream());
    float temperature = document["main"]["temp"] | NAN;
    const char* condition = document["weather"][0]["main"] | "";
    if (!error && !isnan(temperature) && strlen(condition) > 0) {
      currentTemp = round(temperature);
      currentWeather = condition;
      hasWeatherData = true;
      success = true;
    }
  }
  http.end();

  if (success) {
    HTTPClient forecastHttp;
    String forecastUrl = "https://api.openweathermap.org/data/2.5/forecast?q="
                       + String(city) + "&units=metric&appid=" + String(weatherKey);
    if (forecastHttp.begin(client, forecastUrl)) {
      forecastHttp.setConnectTimeout(5000);
      forecastHttp.setTimeout(5000);
      if (forecastHttp.GET() == HTTP_CODE_OK) {
        StaticJsonDocument<160> filter;
        filter["list"][0]["main"]["temp"] = true;
        filter["list"][0]["weather"][0]["main"] = true;
        StaticJsonDocument<256> forecastDoc;
        DeserializationError forecastError = deserializeJson(
          forecastDoc, forecastHttp.getStream(), DeserializationOption::Filter(filter));
        float nextTemp = forecastDoc["list"][0]["main"]["temp"] | NAN;
        const char *nextCondition = forecastDoc["list"][0]["weather"][0]["main"] | "";
        if (!forecastError && !isnan(nextTemp) && strlen(nextCondition) > 0) {
          forecastTemp = round(nextTemp);
          forecastWeather = nextCondition;
          hasForecastData = true;
        }
      }
      forecastHttp.end();
    }
  }
  return success;
}

void updateSensors(unsigned long now) {
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  if (!isnan(temperature)) indoorTemperature = temperature;
  if (!isnan(humidity)) indoorHumidity = humidity;

  uint8_t alerts = 0;
  if (!isnan(indoorTemperature)) {
    if (indoorTemperature >= TEMP_HIGH_ALERT_C) alerts |= 0x01;
    if (indoorTemperature <= TEMP_LOW_ALERT_C) alerts |= 0x02;
  }
  if (!isnan(indoorHumidity)) {
    if (indoorHumidity >= HUMIDITY_HIGH_ALERT) alerts |= 0x04;
    if (indoorHumidity <= HUMIDITY_LOW_ALERT) alerts |= 0x08;
  }
  sensorAlertFlags = alerts;
}

void startWeatherRefresh(unsigned long now) {
  if (strlen(ssid) == 0 || strlen(weatherKey) == 0 || weatherConnectionInProgress) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  weatherConnectionInProgress = true;
  weatherConnectionStarted = now;
}


// ============================================================
//  FUNCTION: drawWeatherIcon(x, y)
//  Small pixel-art icon for the bottom dashboard.
// ============================================================
void drawWeatherIcon(int x, int y) {
  struct tm t;
  getLocalTime(&t);
  bool isNight = (t.tm_hour >= 18 || t.tm_hour < 6);

  if (currentWeather == "Rain" || currentWeather == "Drizzle") {
    u8g2.drawCircle(x, y-2, 3); u8g2.drawCircle(x+3, y-2, 3);
    u8g2.drawLine(x, y+2, x-1, y+4); u8g2.drawLine(x+3, y+2, x+2, y+4);
  }
  else if (isNight && currentWeather == "Clear") {
    u8g2.drawCircle(x, y, 4);
    u8g2.setDrawColor(0); u8g2.drawCircle(x+2, y-2, 4); u8g2.setDrawColor(1);
  }
  else if (currentWeather == "Clouds") {
    u8g2.drawCircle(x-2, y, 3); u8g2.drawCircle(x+2, y, 4); u8g2.drawBox(x-2, y+1, 5, 2);
  }
  else { // Sunny / default
    u8g2.drawDisc(x, y, 3);
    for (int i = 0; i < 360; i += 90) {
      float angle = i * PI / 180.0f;
      u8g2.drawLine(x, y, x + (int)lroundf(cosf(angle) * 6),
                    y + (int)lroundf(sinf(angle) * 6));
    }
  }
}


// ============================================================
//  FUNCTION: setManualTime()
//  Fallback clock when NTP fails.  Update these values if needed.
// ============================================================
void setManualTime() {
  // Use the firmware build timestamp as a useful fallback instead of a stale
  // hard-coded date. Build time is local IST; settimeofday expects UTC.
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char monthText[4] = {__DATE__[0], __DATE__[1], __DATE__[2], '\0'};
  struct tm tm = {};
  tm.tm_mon = (strstr(months, monthText) - months) / 3;
  tm.tm_mday = atoi(__DATE__ + 4);
  tm.tm_year = atoi(__DATE__ + 7) - 1900;
  tm.tm_hour = atoi(__TIME__);
  tm.tm_min = atoi(__TIME__ + 3);
  tm.tm_sec = atoi(__TIME__ + 6);
  time_t t = mktime(&tm); // TZ is configured as IST before this is called.
  struct timeval tv = { .tv_sec = t };
  settimeofday(&tv, NULL);
}

bool waitForNetworkTime(unsigned long timeoutMs) {
  // A previously valid clock is not proof that this fresh NTP request succeeded.
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  unsigned long started = millis();
  struct tm timeinfo;
  while (millis() - started < timeoutMs) {
    sntp_sync_status_t status = sntp_get_sync_status();
    if ((status == SNTP_SYNC_STATUS_COMPLETED || status == SNTP_SYNC_STATUS_IN_PROGRESS) &&
        getLocalTime(&timeinfo, 250) && timeinfo.tm_year >= (2024 - 1900)) return true;
    delay(50);
  }
  return false;
}

bool connectToOpenWifiForTime() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int count = WiFi.scanNetworks(false, true);
  if (count <= 0) return false;

  // Try open access points from strongest to weakest. A network only counts
  // as useful when NTP succeeds (some open hotspots require a captive portal).
  bool tried[32] = {};
  int limit = count < 32 ? count : 32;
  const int maxOpenNetworksToTry = 4;
  for (int attempt = 0; attempt < maxOpenNetworksToTry; attempt++) {
    int best = -1;
    for (int i = 0; i < limit; i++) {
      if (tried[i] || WiFi.encryptionType(i) != WIFI_AUTH_OPEN) continue;
      if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best < 0) break;
    tried[best] = true;
    String candidate = WiFi.SSID(best);
    WiFi.begin(candidate.c_str());
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 5000) delay(100);
    if (WiFi.status() == WL_CONNECTED && waitForNetworkTime(5000)) {
      WiFi.scanDelete();
      return true;
    }
    WiFi.disconnect();
  }
  WiFi.scanDelete();
  return false;
}

class RomiTimeWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override {
    std::string raw = characteristic->getValue();
    if (raw.empty()) return;
    char *end = nullptr;
    long long supplied = strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || (*end != '\0' && *end != '\n' && *end != '\r')) return;
    if (supplied > 100000000000LL) supplied /= 1000; // accept Unix milliseconds too
    if (supplied < 1700000000LL || supplied > 4102444800LL) return;
    portENTER_CRITICAL(&bleTimeMux);
    pendingBleEpoch = supplied;
    portEXIT_CRITICAL(&bleTimeMux);
  }
};

void startBleTimeService() {
  NimBLEDevice::init("ROMI");
  NimBLEServer *server = NimBLEDevice::createServer();
  NimBLEService *service = server->createService(BLE_TIME_SERVICE_UUID);
  NimBLECharacteristic *timeCharacteristic = service->createCharacteristic(
    BLE_TIME_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  timeCharacteristic->setCallbacks(new RomiTimeWriteCallbacks());
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_TIME_SERVICE_UUID);
  advertising->setScanResponse(true);
  NimBLEDevice::startAdvertising();
}

bool retryClockSync() {
  bool synced = false;
  if (strlen(ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) delay(100);
    if (WiFi.status() == WL_CONNECTED && waitForNetworkTime(10000)) {
      clockSyncSource = CLOCK_SAVED_WIFI;
      synced = true;
      if (strlen(weatherKey) > 0) updateWeather();
    }
  }
  if (!synced && connectToOpenWifiForTime()) {
    clockSyncSource = CLOCK_OPEN_WIFI;
    synced = true;
    if (strlen(weatherKey) > 0) updateWeather();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  if (synced) beepClick(35);
  return synced;
}


// ============================================================
//  COMPLIMENTS — 100 short phrases shown after BLINK
//  Keep each entry ≤ 13 characters.  Update random(0,100) if count changes.
// ============================================================
const char* compliments[] = {
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
//  SKY-ROCKET FUNCTIONS
// ============================================================

// initRockets() — called once when HEART state starts
// Clears all rocket slots so no stale particles remain
void initRockets() {
  for (int i = 0; i < MAX_ROCKETS; i++) rockets[i].active = false;
  lastRocketLaunch = millis();
}

// launchRocket() — spawns a new rocket from a random x position at the bottom
void launchRocket() {
  for (int i = 0; i < MAX_ROCKETS; i++) {
    if (!rockets[i].active) {
      rockets[i].active   = true;
      rockets[i].burst    = false;
      rockets[i].burstAge = 0;
      rockets[i].x  = random(15, 113);   // Random horizontal launch point
      rockets[i].y  = 63;                // Start at screen bottom
      // Speed varies slightly so rockets don't all arrive at the same time
      rockets[i].vy = -(random(8, 14) / 10.0f); // -0.8 to -1.3 px per frame
      break; // Only launch one per call
    }
  }
}

// burstRocket() — converts a rising rocket into a burst of 12 particles
// Particles fan out in all directions from the burst point
void burstRocket(int i) {
  rockets[i].burst    = true;
  rockets[i].burstAge = 0;
  for (int p = 0; p < 12; p++) {
    float angle = p * (2.0f * PI / 12.0f); // Evenly space 12 particles in a circle
    float speed = random(8, 18) / 10.0f;   // Random speed 0.8–1.7 px/frame
    rockets[i].burstX[p]  = rockets[i].x;
    rockets[i].burstY[p]  = rockets[i].y;
    rockets[i].burstVx[p] = cos(angle) * speed;
    rockets[i].burstVy[p] = sin(angle) * speed;
  }
}

// updateAndDrawRockets() — advance physics and draw every active rocket
// Call once per frame from the HEART state render block
void updateAndDrawRockets() {
  unsigned long now = millis();
  static unsigned long previousUpdate = 0;
  float dt = previousUpdate == 0 ? 1.0f : (now - previousUpdate) / 25.0f;
  if (dt > 3.0f) dt = 3.0f;
  previousUpdate = now;

  // Launch a new rocket on schedule (if a slot is free)
  if (now - lastRocketLaunch > ROCKET_INTERVAL) {
    launchRocket();
    lastRocketLaunch = now;
  }

  for (int i = 0; i < MAX_ROCKETS; i++) {
    if (!rockets[i].active) continue;

    if (!rockets[i].burst) {
      // --- RISING PHASE ---
      rockets[i].y += rockets[i].vy * dt; // Move upward (vy is negative)

      // Draw the rocket: a bright pixel at the tip + a fading trail of 3 dots below
      int rx = (int)rockets[i].x;
      int ry = (int)rockets[i].y;
      u8g2.drawPixel(rx, ry);          // Rocket head
      u8g2.drawPixel(rx, ry + 2);      // Trail dot 1
      u8g2.drawPixel(rx, ry + 4);      // Trail dot 2 (dimmer — just 1 pixel)

      // Burst when rocket reaches the upper portion of the screen (y < 20)
      // or randomly in the 10–30px zone for natural variation
      if (ry < 20 || (ry < 30 && random(10) == 0)) {
        burstRocket(i);
      }
    } else {
      // --- BURST PHASE ---
      rockets[i].burstAge += dt;

      // Draw and move each of the 12 burst particles
      for (int p = 0; p < 12; p++) {
        rockets[i].burstX[p] += rockets[i].burstVx[p] * dt;
        rockets[i].burstY[p] += rockets[i].burstVy[p] * dt;
        rockets[i].burstVy[p] += 0.12f * dt; // Gravity pulls particles down

        int px = (int)rockets[i].burstX[p];
        int py = (int)rockets[i].burstY[p];

        // Draw a small cross "+" for each particle (more visible than a single pixel)
        if (px > 0 && px < 127 && py > 0 && py < 63) {
          u8g2.drawPixel(px, py);
          u8g2.drawPixel(px+1, py);
          u8g2.drawPixel(px-1, py);
          u8g2.drawPixel(px, py-1);
        }
      }

      // Deactivate after 18 frames so the slot can be reused
      if (rockets[i].burstAge > 18) rockets[i].active = false;
    }
  }
}


// ============================================================
//  FACE DRAWING HELPERS
// ============================================================

// drawMovingEye(x, y, closed, pupilOffset)
// Standard eye used in BLINK animation.
// pupilOffset: -8=far left, 0=centre, +8=far right
void drawMovingEye(int x, int y, bool closed, int pupilOffset) {
  if (closed) {
    u8g2.drawRBox(x - 17, y - 3, 34, 6, 2); // Thick closed-eye bar
  } else {
    u8g2.drawRBox(x - 17, y - 17, 34, 34, 5); // Big square eye
    u8g2.setDrawColor(0);
    // One centered pupil per eye; offset moves the gaze as a single unit.
    u8g2.drawRBox(x - 2 + pupilOffset, y - 10, 4, 12, 1);
    u8g2.setDrawColor(1);
  }
}

float easeInOut(float t) {
  t = constrain(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

void drawSoftBlinkEye(int x, int y, int pupilOffset, int pupilYOffset,
                      float closure, float scale) {
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

// drawUltraMouth(x, y, surprised)
// surprised=false → wide U smile   surprised=true → hollow O gasp
void drawUltraMouth(int x, int y, bool surprised) {
  if (surprised) {
    u8g2.drawEllipse(x, y - 8, 10, 12);
    u8g2.setDrawColor(0); u8g2.drawEllipse(x, y - 8, 7, 9); u8g2.setDrawColor(1);
  } else {
    u8g2.drawRBox(x - 15, y - 10, 30, 12, 4);
    u8g2.setDrawColor(0); u8g2.drawBox(x - 15, y - 12, 30, 8); u8g2.setDrawColor(1);
    u8g2.drawPixel(x - 16, y - 10); u8g2.drawPixel(x + 16, y - 10); // Dimple pixels
  }
}

// drawHeartEye(x, y, size, closed, pupilOffset)
// NEW: takes a 'size' parameter so hearts can pulse (breathe in/out).
// size controls half-width of the heart — default ~16, range 12–20 for pulse effect.
// closed=true draws a curved "squint" blink line instead of the full heart.
void drawHeartEye(int x, int y, int sz, bool closed, int pupilOffset) {
  if (closed) {
    // Happy curved squint — two overlapping arcs
    u8g2.drawCircle(x, y - 5, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.drawCircle(x, y - 6, sz + 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  } else {
    // Heart = two discs (lobes) + downward triangle (tip)
    u8g2.drawDisc(x - (sz / 2), y, sz / 2);                         // Left lobe
    u8g2.drawDisc(x + (sz / 2), y, sz / 2);                         // Right lobe
    u8g2.drawTriangle(x - sz, y + 1, x + sz, y + 1, x, y + sz + 10); // Bottom point

    // Pupil — small disc erased from inside the heart
    u8g2.setDrawColor(0);
    u8g2.drawDisc(x + pupilOffset, y + 2, 4);
    u8g2.setDrawColor(1);

    // Shimmer pixel — top-left glint for sparkle
    u8g2.drawPixel(x - (sz / 2) - 1, y - 3);

    // Inner highlight: tiny white disc to give the heart depth
    u8g2.drawPixel(x - 4, y - 5);
    u8g2.drawPixel(x - 5, y - 4);
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
  // Three tiny hearts drift upward through the open center of the face.
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

  // Blushing cheeks and a small, warm smile.
  u8g2.drawLine(52, 46, 56, 48); u8g2.drawLine(51, 49, 55, 51);
  u8g2.drawLine(76, 48, 80, 46); u8g2.drawLine(77, 51, 81, 49);
  u8g2.drawLine(59, 57, 64, 60); u8g2.drawLine(64, 60, 69, 57);
  u8g2.drawPixel(47, 60); u8g2.drawPixel(81, 60);
}

// drawCrazyEye(x, y, radius)
// EXCITED state — square eye with alternating + and × star pupil
void drawCrazyEye(int x, int y, int radius) {
  u8g2.drawRBox(x - radius, y - radius, radius * 2, radius * 2, 5);
  u8g2.setDrawColor(0);
  if ((millis() / 50) % 2 == 0) {
    u8g2.drawBox(x-8, y-1, 16, 2); u8g2.drawBox(x-1, y-8, 2, 16); // "+" star
  } else {
    u8g2.drawLine(x-6, y-6, x+6, y+6); u8g2.drawLine(x+6, y-6, x-6, y+6); // "×" star
  }
  u8g2.setDrawColor(1);
}

void drawCrazyMouth(int x, int y) {
  u8g2.drawTriangle(x-20, y-10, x+20, y-10, x, y+10); // Open triangle mouth
  u8g2.setDrawColor(0); u8g2.drawDisc(x, y+6, 4); u8g2.setDrawColor(1); // Tongue
}

// drawPettingEye(x, y) — blissful "U" shaped eye for PETTING state
void drawPettingEye(int x, int y) {
  u8g2.drawRBox(x - 17, y - 17, 34, 34, 6);
  u8g2.setDrawColor(0);
  u8g2.drawCircle(x, y-2, 8, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawCircle(x, y-3, 8, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.setDrawColor(1);
}

void drawRabbitEars(int x, int y, int offset) {
  u8g2.drawRBox(x-8, y-30-offset, 16, 25, 5);
  u8g2.setDrawColor(0); u8g2.drawRBox(x-4, y-26-offset, 8, 15, 2); u8g2.setDrawColor(1);
}

void drawRabbitMouth(int x, int y, bool twitchUp) {
  int yOff = twitchUp ? -2 : 0;
  u8g2.drawLine(x, y+yOff, x-3, y-3+yOff);
  u8g2.drawLine(x, y+yOff, x+3, y-3+yOff);
  u8g2.drawLine(x, y+yOff, x, y+4+yOff);
  u8g2.drawLine(x-10, y, x-25, y-5); u8g2.drawLine(x-10, y+2, x-25, y+5);
  u8g2.drawLine(x+10, y, x+25, y-5); u8g2.drawLine(x+10, y+2, x+25, y+5);
}

void drawSleepyEye(int x, int y, bool closed) {
  if (closed) {
    u8g2.drawRBox(x-17, y+5, 34, 4, 1);
  } else {
    u8g2.drawRBox(x-17, y-17, 34, 34, 5);
    u8g2.setDrawColor(0); u8g2.drawBox(x-18, y-18, 36, 18); u8g2.drawDisc(x, y+5, 4);
    u8g2.setDrawColor(1);
  }
}


// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TOUCH_PIN, TOUCH_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000); // Faster full-buffer OLED refresh for smoother motion
  u8g2.begin();
  dht.begin();
  setenv("TZ", "IST-5:30", 1);
  tzset();
  settings.begin("romi", false);
  stopwatchElapsedMs = settings.getULong64("sw_ms", 0);

  // Initialise all rocket slots as inactive
  for (int i = 0; i < MAX_ROCKETS; i++) rockets[i].active = false;

  // --- BOOT ANIMATION: type "HELLO ROMI" letter by letter ---
  u8g2.setFont(u8g2_font_logisoso20_tf);
  const char* line1 = "HELLO";
  const char* line2 = "ROMI";
  for (int i = 1; i <= 5; i++) {
    u8g2.clearBuffer(); char buf[10]; strncpy(buf, line1, i); buf[i] = '\0';
    u8g2.drawStr(25, 35, buf); u8g2.sendBuffer(); delay(150);
  }
  for (int i = 1; i <= 4; i++) {
    u8g2.clearBuffer(); u8g2.drawStr(25, 35, "HELLO");
    char buf[10]; strncpy(buf, line2, i); buf[i] = '\0';
    u8g2.drawStr(35, 60, buf); u8g2.sendBuffer(); delay(150);
  }
  // Animated sine underline
  for (int j = 0; j < 15; j++) {
    u8g2.clearBuffer(); u8g2.drawStr(25, 35, "HELLO"); u8g2.drawStr(35, 60, "ROMI");
    int lineW = (sin(j / 3.0) * 30) + 40;
    u8g2.drawBox(64-(lineW/2), 62, lineW, 2); u8g2.sendBuffer(); delay(50);
  }

  // BLE time setting remains available while Wi-Fi is scanning or syncing.
  startBleTimeService();

  // --- Connect to saved Wi-Fi first, then try nearby open Wi-Fi for NTP. ---
  bool timeSynced = false;
  if (strlen(ssid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(ssid, password);
    unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 10000) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(25, 18, "CONNECTING...");
      int pw = (int)(sin(millis()/300.0)*38)+50;
      u8g2.drawRBox(64-(pw/2), 58, pw, 5, 2); u8g2.sendBuffer();
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      timeSynced = waitForNetworkTime(10000);
      if (timeSynced) clockSyncSource = CLOCK_SAVED_WIFI;
    }
  }

  if (!timeSynced && connectToOpenWifiForTime()) {
    timeSynced = true;
    clockSyncSource = CLOCK_OPEN_WIFI;
  }
  if (timeSynced) {
    wifiEverConnected = true;
    if (clockSyncSource == CLOCK_OPEN_WIFI && strlen(weatherKey) > 0) updateWeather();
    beepClick(35);
  } else {
    setManualTime(); // Build-time fallback when no NTP-capable network is nearby.
  }
  lastWeatherUpdate = millis();
  lastClockSyncAttempt = millis();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  tapCount = 0;
  touchRawState = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  lastTouchState = touchRawState;
  touchRawChangedAt = millis();
  lastAutoBlink = millis();
}


// ============================================================
//  drawHome()
//  Renders the main clock + sensor dashboard screen.
// ============================================================
void drawHome() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;

  // Compact date header with a small ROMI heart badge.
  const char *days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                          "JUL","AUG","SEP","OCT","NOV","DEC"};
  char dateBuf[20];
  snprintf(dateBuf, sizeof(dateBuf), "%s  %02d %s", days[ti.tm_wday],
           ti.tm_mday, months[ti.tm_mon]);
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(4, 10, dateBuf);
  drawTinyHeart(117, 7, 4);
  u8g2.drawLine(4, 14, 124, 14);

  // Center every time format by its actual font width.
  char timeStr[16];
  int h12 = ti.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  const char *suffix = (ti.tm_hour >= 12) ? "PM" : "AM";
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

  // Compact status hints share the header row without competing with the time.
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  if (sensorAlertFlags) {
    const char *alert = (sensorAlertFlags & 0x01) ? "HOT!" :
      (sensorAlertFlags & 0x02) ? "COLD" :
      (sensorAlertFlags & 0x04) ? "HUM!" : "DRY!";
    u8g2.drawStr(77, 10, alert);
  } else if (hasForecastData &&
             (forecastWeather == "Rain" || forecastWeather == "Drizzle" ||
              forecastWeather == "Thunderstorm")) {
    u8g2.drawStr(82, 10, "RAIN");
  }

  // Three aligned footer cells for indoor temperature, humidity, and weather.
  u8g2.drawLine(4, 50, 124, 50);
  u8g2.drawLine(43, 53, 43, 63);
  u8g2.drawLine(84, 53, 84, 63);
  u8g2.drawRFrame(5, 53, 6, 8, 2); u8g2.drawDisc(8, 61, 2);
  u8g2.drawLine(8, 55, 8, 59);
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.setCursor(14, 62);
  if (isnan(indoorTemperature)) u8g2.print("--.-C");
  else { u8g2.print(indoorTemperature, 1); u8g2.print((char)176); u8g2.print("C"); }

  u8g2.drawCircle(50, 58, 3);
  u8g2.drawTriangle(47, 56, 53, 56, 50, 52);
  u8g2.setCursor(57, 62);
  if (isnan(indoorHumidity)) u8g2.print("--%");
  else { u8g2.print((int)indoorHumidity); u8g2.print("%"); }

  if (hasWeatherData) drawWeatherIcon(91, 58);
  else u8g2.drawCircle(91, 58, 4);
  u8g2.setCursor(99, 62);
  if (hasWeatherData) { u8g2.print(currentTemp); u8g2.print((char)176); u8g2.print("C"); }
  else u8g2.print("--");
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
//  loop()
// ============================================================
void loop() {
  u8g2.clearBuffer();
  unsigned long now = millis();
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
    clockSyncSource = CLOCK_BLE;
    clockSyncFailed = false;
    lastClockSyncAttempt = now;
    beepClick(35);
  }
  if (clockSyncRetryRequested) {
    clockSyncRetryRequested = false;
    clockSyncFailed = !retryClockSync();
    lastClockSyncAttempt = millis();
  }
  bool sampledTouch = (digitalRead(TOUCH_PIN) == (TOUCH_ACTIVE_LOW ? LOW : HIGH));
  if (sampledTouch != touchRawState) {
    touchRawState = sampledTouch;
    touchRawChangedAt = now;
  }
  bool touching = lastTouchState;
  if (now - touchRawChangedAt >= TOUCH_DEBOUNCE_MS) touching = touchRawState;


  // ── 1. BACKGROUND SENSOR AND WEATHER UPDATES ─────────────
  uint8_t oldSensorAlerts = sensorAlertFlags;
  // DHT reads block briefly; defer them during animated face states to keep
  // the one-tap eye motion at a steady frame rate.
  if (currentState != BLINK && currentState != HEART && currentState != EXCITED &&
      currentState != PETTING && currentState != COMPLIMENT) updateSensors(now);
  if (sensorAlertFlags && !oldSensorAlerts) beepClick(25);
  if (currentState == HOME && !touching && !weatherConnectionInProgress &&
      now - lastWeatherUpdate >= WEATHER_INTERVAL) {
    startWeatherRefresh(now);
  }
  if (weatherConnectionInProgress) {
    if (currentState != HOME || touching) {
      weatherConnectionInProgress = false;
      lastWeatherUpdate = now;
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    } else if (WiFi.status() == WL_CONNECTED) {
      updateWeather();
      lastWeatherUpdate = now;
      weatherConnectionInProgress = false;
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    } else if (now - weatherConnectionStarted >= WEATHER_CONNECTION_TIMEOUT) {
      weatherConnectionInProgress = false;
      lastWeatherUpdate = now; // Avoid retrying continuously when the network is unavailable.
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    }
  }

  // Retry NTP hourly while HOME is visible, so Wi-Fi scans do not interrupt
  // active face animations or timer/stopwatch controls.
  if (currentState == HOME && !weatherConnectionInProgress &&
      now - lastClockSyncAttempt >= CLOCK_SYNC_INTERVAL) {
    clockSyncFailed = !retryClockSync();
    lastClockSyncAttempt = millis();
    lastWeatherUpdate = lastClockSyncAttempt;
  }


  // ── 2. AUTO-BLINK (idle animation every 60s on HOME) ─────
  if (currentState == HOME && (now - lastAutoBlink >= AUTO_BLINK_INTERVAL)) {
    isAutoBlink = true; currentState = BLINK; stateStartTime = now; lastAutoBlink = now;
  }


  // ── 3. TOUCH SENSING ─────────────────────────────────────
  // A) Rising edge: finger just touched
  if (touching && !lastTouchState) lastTouchTime = now;

  // B) Long press → PETTING (or SECRET_GIFT if 9 taps precede it)
  if (touching && (now - lastTouchTime > LONG_PRESS_MS)) {
    bool secretReady = tapCount == 9 ||
      (secretGiftArmed && now - secretGiftArmedAt <= SECRET_GIFT_ARM_MS);
    if (secretReady) {
      currentState = SECRET_GIFT; stateStartTime = now; tapCount = 0;
      secretGiftArmed = false;
    } else if (currentState != PETTING && currentState != SECRET_GIFT) {
      previousState   = currentState;
      wasTimerRunning = isTimerRunning; // Save timer state before interrupting
      currentState    = PETTING;
      stateStartTime  = now;
    }
  }

  // C) Falling edge: finger lifted
  if (!touching && lastTouchState) {
    beepClick(8); // Immediate acknowledgement while a multi-tap is collected.
    if (currentState == PETTING) {
      currentState   = previousState;      // Return to prior state
      isTimerRunning = wasTimerRunning;    // Restore timer
      if (isTimerRunning) lastTick = now;  // Resync tick to avoid phantom decrement
    } else if (currentState == SECRET_GIFT) {
      currentState = HOME;
    } else {
      // A short tap after the nine-tap sequence cancels the secret gesture.
      secretGiftArmed = false;
      // Keep the current screen while collecting taps. Dispatch only after the
      // gesture gap so a multi-tap never starts the single-tap animation first.
      tapCount++;
    }
    lastTouchTime = now;
  }
  lastTouchState = touching;


  // ── 4. TIMER COUNTDOWN ───────────────────────────────────
  // Ticks even during PETTING so the background timer keeps running.
  if ((currentState == TIMER_MODE ||
       (currentState == PETTING && wasTimerRunning)) && isTimerRunning) {

    if (now - lastTick >= 1000) {
      lastTick += 1000; // preserve cadence after a slow frame

      if (remainingSeconds > 0) remainingSeconds--;

      if (remainingSeconds > 0) {
        // --- 5-SECOND WARNING: beep once each second at 5,4,3,2,1 ---
        // Only triggers when in TIMER_MODE (not in background during PETTING)
        // so it doesn't surprise the user with sounds during petting.
        if (currentState == TIMER_MODE && remainingSeconds <= 5 && remainingSeconds > 0) {
          // Beep pitch rises as seconds decrease (800µs→400µs period)
          int warnDelay = map(remainingSeconds, 5, 1, 800, 400);
          for (int i = 0; i < 100; i++) {
            digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(warnDelay);
            digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(warnDelay);
          }
        }

      } else {
        // --- TIMER FINISHED ---
        isTimerRunning = false;

        // Loud alarm-clock finish: 3 bursts of rapid buzzing
        beepAlarm();

        // *** AUTO-RESET: return to full duration, stay paused ***
        // This was the bug — previously remainingSeconds was left at 0.
        remainingSeconds = timerFullSeconds;
      }
    }
  }


  // ── 5. TAP SEQUENCE ACTION ───────────────────────────────
  // Fires once TAP_GAP_MS has passed with no new tap.
  if (tapCount > 0 && !touching && (now - lastTouchTime > TAP_GAP_MS)) {
    isAutoBlink = false;

    if (currentState == TIMER_MODE) {
      // --- Timer controls ---

      if (tapCount == 1) {
        isTimerRunning = !isTimerRunning;
        lastTick = now;
        beepChirp(isTimerRunning ? 600 : 1400, 80);
      }
      else if (tapCount == 2) {
        isTimerRunning = false;
        remainingSeconds = timerFullSeconds;
        beepChirp(1000, 100);
      }
      else if (tapCount == 3) {
        isTimerRunning = false;
        currentOptionIndex = (currentOptionIndex + 1) % 5;
        timerFullSeconds = (long)timerOptions[currentOptionIndex] * 60;
        remainingSeconds = timerFullSeconds;
        lastTick = now;
        beepChirp(1000, 100);
      }
      else if (tapCount == 4) {
        isTimerRunning = false;
        currentState = STOPWATCH_MODE;
        beepChirp(1000, 100);
      }
      else if (tapCount == 5) {
        isTimerRunning = false;
        currentState = HOME;
        beepChirp(500, 60);
      }

    } else if (currentState == STOPWATCH_MODE) {
      if (tapCount == 1) {
        if (stopwatchRunning) {
          stopwatchElapsedMs += (uint32_t)(now - stopwatchStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        } else {
          stopwatchStartedAt = now;
          stopwatchRunning = true;
        }
        beepChirp(stopwatchRunning ? 600 : 1400, 80);
      }
      else if (tapCount == 2) {
        stopwatchRunning = false;
        stopwatchElapsedMs = 0;
        saveStopwatch();
        beepChirp(1000, 100);
      }
      else if (tapCount == 3) {
        if (stopwatchRunning) {
          stopwatchElapsedMs += (uint32_t)(now - stopwatchStartedAt);
          stopwatchRunning = false;
          saveStopwatch();
        }
        currentState = TIMER_MODE;
        isTimerRunning = false;
        lastTick = now;
        beepChirp(1000, 100);
      }
      else if (tapCount == 4) {
        if (stopwatchRunning) {
          stopwatchElapsedMs += (uint32_t)(now - stopwatchStartedAt);
          stopwatchRunning = false;
        }
        saveStopwatch();
        currentState = HOME;
        beepChirp(500, 60);
      }

    } else {
      // --- Normal tap actions ---

      if (tapCount == 1) {
        beepTapFeedback(1);
        currentState   = BLINK;
        stateStartTime = millis();
      }
      else if (tapCount == 2) {
        beepTapFeedback(2);
        initRockets();               // Reset rocket system for HEART state
        currentState   = HEART;
        stateStartTime = now;
      }
      else if (tapCount == 3) {
        beepTapFeedback(3);
        currentState   = STOPWATCH_MODE;
        isTimerRunning = false;
        lastTick       = now;
      }
      else if (tapCount == 4) {
        beepTapFeedback(4);
        clockFormat = (clockFormat + 1) % 4;
      }
      else if (tapCount == 5) {
        beepTapFeedback(5);
        currentState   = EXCITED;
        stateStartTime = now;
      }
      else if (tapCount == 6) {
        currentState = WEATHER_MODE;
        stateStartTime = now;
      }
      else if (tapCount == 7) {
        switch (nextExpression) {
          case 0: currentState = SLEEPY; break;
          case 1: currentState = CURIOUS; break;
          case 2: currentState = SAD; break;
          case 3: currentState = LISTENING; break;
          default: currentState = CELEBRATION; break;
        }
        nextExpression = (nextExpression + 1) % 5;
        stateStartTime = now;
      }
      else if (tapCount == 8) {
        clockSyncRetryRequested = true;
      }
      else if (tapCount == 9) {
        secretGiftArmed = true;
        secretGiftArmedAt = millis();
      }
    }

    tapCount = 0; // Always clear after dispatching
  }


  // ── 6. STATE RENDERING ───────────────────────────────────
  switch (currentState) {

    // ── HOME ────────────────────────────────────────────────
    case HOME:
      drawHome();
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
      u8g2.setFont(u8g2_font_haxrcorp4089_tr); u8g2.drawStr(54, 12, "SLEEPY");
      if (elapsed > 4500) currentState = HOME;
      break;
    }

    case CURIOUS: {
      int look = (int)(sin((now - stateStartTime) * 0.004f) * 5);
      drawMovingEye(30, 27, false, look);
      drawMovingEye(98, 27, false, -look);
      drawUltraMouth(64, 60, true);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr); u8g2.drawStr(51, 12, "CURIOUS?");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case SAD: {
      drawMovingEye(30, 28, false, 0); drawMovingEye(98, 28, false, 0);
      u8g2.drawLine(15, 7, 43, 13); u8g2.drawLine(85, 13, 113, 7);
      u8g2.drawCircle(64, 59, 9, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr); u8g2.drawStr(57, 12, "OH...");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case LISTENING: {
      drawMovingEye(30, 29, false, 0); drawMovingEye(98, 29, false, 0);
      u8g2.drawCircle(64, 57, 7); u8g2.setDrawColor(0); u8g2.drawCircle(64, 57, 4); u8g2.setDrawColor(1);
      u8g2.drawCircle(8, 32, 8, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
      u8g2.drawCircle(120, 32, 8, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_LOWER_LEFT);
      u8g2.setFont(u8g2_font_haxrcorp4089_tr); u8g2.drawStr(43, 12, "LISTENING");
      if (now - stateStartTime > 4500) currentState = HOME;
      break;
    }

    case CELEBRATION: {
      unsigned long elapsed = now - stateStartTime;
      int size = 15 + (int)(sin(elapsed * 0.012f) * 3);
      drawHeartEye(32, 25, size, false, 0); drawHeartEye(96, 25, size, false, 0);
      drawUltraMouth(64, 60, false);
      for (int i = 0; i < 6; i++) {
        int x = (i * 23 + (elapsed / 40) % 23) % 128;
        int y = (i * 13 + (elapsed / 25) % 64);
        u8g2.drawPixel(x, y);
      }
      if (elapsed > 4500) currentState = HOME;
      break;
    }


    case STOPWATCH_MODE: {
      uint64_t totalSeconds = getStopwatchElapsedMs(now) / 1000ULL;
      uint64_t hours = totalSeconds / 3600ULL;
      uint64_t minutes = (totalSeconds / 60ULL) % 60ULL;
      uint64_t seconds = totalSeconds % 60ULL;
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(36, 12, stopwatchRunning ? "STOPWATCH RUN" : "STOPWATCH PAUSED");
      char watchText[16];
      if (hours > 0) {
        snprintf(watchText, sizeof(watchText), "%02llu:%02llu:%02llu",
                 (unsigned long long)(hours % 100ULL),
                 (unsigned long long)minutes, (unsigned long long)seconds);
        u8g2.setFont(u8g2_font_logisoso20_tn);
        u8g2.drawStr(4, 50, watchText);
      } else {
        snprintf(watchText, sizeof(watchText), "%02llu:%02llu",
                 (unsigned long long)minutes, (unsigned long long)seconds);
        u8g2.setFont(u8g2_font_logisoso28_tn);
        u8g2.drawStr(20, 50, watchText);
      }
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(8, 63, "1=GO 2=RST 3=TM 4=EXIT");
      break;
    }

    // ── TIMER MODE ──────────────────────────────────────────
    case TIMER_MODE: {
      // Status label at the top
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);

      if (remainingSeconds <= 5 && remainingSeconds > 0 && isTimerRunning) {
        // Flash "ALMOST!" text during the 5-second warning window
        // Flashes at 250ms rate using millis parity trick
        if ((now / 250) % 2 == 0)
          u8g2.drawStr(30, 12, "!! ALMOST !!");
      } else {
        u8g2.drawStr(38, 12, isTimerRunning ? "TIMER RUNNING" : "TIMER PAUSED");
      }

      // Big countdown digits in the centre
      u8g2.setFont(u8g2_font_logisoso28_tn);
      char tBuf[10];
      sprintf(tBuf, "%02ld:%02ld", remainingSeconds / 60, remainingSeconds % 60);

      // Invert the display colour during the warning to create a visual flash
      if (remainingSeconds <= 5 && remainingSeconds > 0 && isTimerRunning) {
        // Draw a filled white background box then black text for inverted look
        if ((now / 250) % 2 == 0) {
          u8g2.setDrawColor(1);
          u8g2.drawRBox(10, 20, 108, 36, 4); // White block behind timer
          u8g2.setDrawColor(0);              // Black digits on white
        }
      }
      u8g2.drawStr(20, 50, tBuf);
      u8g2.setDrawColor(1); // Restore draw colour

      // Small hint line at the bottom
      u8g2.setFont(u8g2_font_haxrcorp4089_tr);
      u8g2.drawStr(3, 63, "1=GO 2=RST 3=TIME 4=SW 5=EXIT");
      break;
    }


    // ── COMPLIMENT ──────────────────────────────────────────
    case COMPLIMENT: {
      unsigned long elapsed = now - stateStartTime;
      u8g2.clearBuffer();
      String msg = String(compliments[selectedCompliment]);
      String words[3]; int wordCount = 0, lastIdx = 0; int spaceIdx = -1;
      while ((spaceIdx = msg.indexOf(' ', lastIdx)) != -1 && wordCount < 2) {
        words[wordCount++] = msg.substring(lastIdx, spaceIdx); lastIdx = spaceIdx + 1;
      }
      words[wordCount++] = msg.substring(lastIdx);
      if (wordCount == 1) {
        u8g2.setFont(u8g2_font_logisoso20_tf);
        int w = u8g2.getStrWidth(words[0].c_str());
        u8g2.drawStr(64-(w/2), 42, words[0].c_str());
      } else if (wordCount == 2) {
        u8g2.setFont(u8g2_font_helvB12_tf);
        u8g2.drawStr(64-(u8g2.getStrWidth(words[0].c_str())/2), 30, words[0].c_str());
        u8g2.drawStr(64-(u8g2.getStrWidth(words[1].c_str())/2), 52, words[1].c_str());
      } else {
        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.drawStr(64-(u8g2.getStrWidth(words[0].c_str())/2), 18, words[0].c_str());
        u8g2.drawStr(64-(u8g2.getStrWidth(words[1].c_str())/2), 38, words[1].c_str());
        u8g2.drawStr(64-(u8g2.getStrWidth(words[2].c_str())/2), 58, words[2].c_str());
      }
      if (elapsed >= 1000) currentState = HOME;
      break;
    }


    // ── PETTING (Rabbit mode) ────────────────────────────────
    case PETTING: {
      unsigned long elapsed = now - stateStartTime;
      int bounce = abs((int)(3 * sin(elapsed * 0.004)));
      drawRabbitEars(30, 17-bounce, bounce);
      drawRabbitEars(98, 17-bounce, bounce);
      drawPettingEye(30, 17-bounce);
      drawPettingEye(98, 17-bounce);
      drawRabbitMouth(64, 56, (elapsed % 500 < 250));
      if (elapsed % 150 < 40) { // Soft purring tick
        digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(120); digitalWrite(BUZZER_PIN, LOW);
      }
      break;
    }


    // ── BLINK ────────────────────────────────────────────────
    case BLINK: {
      unsigned long elapsed = now - stateStartTime;
      float gaze = 0.0f, eyeScale = 1.0f, closure = 0.0f;

      // A quick, readable two-second reaction: wake, glance, blink, smile.
      if (elapsed < 280) {
        eyeScale = 0.86f + 0.24f * easeInOut(elapsed / 280.0f);
      } else if (elapsed < 660) {
        eyeScale = 1.10f;
        gaze = -7.0f * easeInOut((elapsed - 280) / 380.0f);
      } else if (elapsed < 1060) {
        eyeScale = 1.10f;
        gaze = -7.0f + 14.0f * easeInOut((elapsed - 660) / 400.0f);
      } else if (elapsed < 1280) {
        eyeScale = 1.10f;
        gaze = 7.0f * (1.0f - easeInOut((elapsed - 1060) / 220.0f));
      } else if (elapsed < 1600) {
        eyeScale = 1.10f;
      } else {
        float settle = easeInOut(min((elapsed - 1600) / 400.0f, 1.0f));
        eyeScale = 1.10f - 0.08f * settle + 0.012f * sinf((elapsed - 1600) * 0.009f);
      }
      if (elapsed >= 1640 && elapsed < SINGLE_TAP_ANIMATION_MS) {
        float flourish = (elapsed - 1640) / 360.0f;
        float pulse = sinf(PI * flourish);
        gaze = 2.0f * pulse;
        eyeScale = 1.04f + 0.04f * pulse;
      }

      // Smooth close and reopen in the middle of the short reaction.
      if (elapsed >= 1280 && elapsed < 1440) closure = easeInOut((elapsed - 1280) / 160.0f);
      else if (elapsed >= 1440 && elapsed < 1480) closure = 1.0f;
      else if (elapsed >= 1480 && elapsed < 1640) closure = 1.0f - easeInOut((elapsed - 1480) / 160.0f);
      // A quick, softer second blink gives the smile a playful finish.
      if (elapsed >= 1700 && elapsed < 1760) closure = easeInOut((elapsed - 1700) / 60.0f);
      else if (elapsed >= 1760 && elapsed < 1780) closure = 1.0f;
      else if (elapsed >= 1780 && elapsed < 1840) closure = 1.0f - easeInOut((elapsed - 1780) / 60.0f);

      bool touchResponse = touching && currentState == BLINK;
      if (touchResponse) { gaze = 0.0f; eyeScale = 1.10f; closure = 0.0f; }
      bool surprised = touchResponse || elapsed < 280;
      int pX = (int)lroundf(gaze);
      bool happyBrow = surprised || (elapsed >= 1640 && elapsed < SINGLE_TAP_ANIMATION_MS);
      drawBlinkBrow(30, 25, happyBrow);
      drawBlinkBrow(98, 25, happyBrow);
      drawSoftBlinkEye(30, 25, pX, 0, closure, eyeScale);
      drawSoftBlinkEye(98, 25, pX, 0, closure, eyeScale);
      int smileBob = elapsed >= 1640 ? (int)lroundf(sinf((elapsed - 1640) * 0.017f)) : 0;
      drawUltraMouth(64, 60 + smileBob, surprised);
      if (elapsed >= 1640) {
        u8g2.drawLine(51, 49, 55, 51); u8g2.drawPixel(50, 47);
        u8g2.drawLine(73, 51, 77, 49); u8g2.drawPixel(78, 47);
        bool sparkleBright = ((elapsed / 100) % 2 == 0);
        drawLoveSparkle(9, 40, sparkleBright);
        drawLoveSparkle(118, 40, sparkleBright);
      }
      if (elapsed >= 1400 && elapsed < 1440 && !isAutoBlink) beepClick(2);

      if (elapsed >= SINGLE_TAP_ANIMATION_MS) {
        currentState   = isAutoBlink ? HOME : COMPLIMENT;
        if (!isAutoBlink) { selectedCompliment = random(0, 100); stateStartTime = millis(); }
      }
      break;
    }


    // ── HEART ────────────────────────────────────────────────
    case HEART: {
      unsigned long elapsed = now - stateStartTime;

      // --- Pupil eye-movement timeline (same as BLINK) ---
      int pX = 0; bool eClosed = false;
      if      (elapsed < 400)  pX = -6;
      else if (elapsed < 800)  pX = 6;
      else if (elapsed < 1000) pX = 0;
      else if (elapsed < 1400) eClosed = true;

      // --- Soft breathing plus a rounded double-beat pulse ---
      // Fractional scale is rounded at render time, so each pixel-size step
      // lands evenly instead of truncating toward the smaller shape.
      float breath = sinf(elapsed * (2.0f * PI / 1900.0f));
      float beatPhase = fmodf((float)elapsed, 920.0f) / 920.0f;
      float d1 = (beatPhase - 0.12f) / 0.055f;
      float d2 = (beatPhase - 0.31f) / 0.065f;
      float beat1 = expf(-0.5f * d1 * d1);
      float beat2 = expf(-0.5f * d2 * d2);
      float pulseSize = 16.0f + 1.6f * breath + 3.0f * beat1 + 1.5f * beat2;
      int heartSize = (int)lroundf(pulseSize);

      // --- Sky-rocket fireworks in the background ---
      updateAndDrawRockets();

      // --- Draw the two pulsing heart eyes ---
      int heartY = 25 - (int)lroundf((beat1 * 1.4f) + (breath * 0.5f));
      drawHeartEye(32, heartY, heartSize, eClosed, pX);  // Left
      drawHeartEye(96, heartY, heartSize, eClosed, pX);  // Right
      drawRomanticAccents(elapsed);

      // --- Heartbeat sound: two quick pulses like "lub-dub" ---
      // First beat at 0ms of each 700ms cycle
      // Second beat at 120ms of each 700ms cycle
      if (elapsed % 700 < 20 || (elapsed % 700 > 120 && elapsed % 700 < 140)) {
        digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(200); digitalWrite(BUZZER_PIN, LOW);
      }

      // Extended duration: 3.5 seconds so rockets have time to launch and burst
      if (elapsed > 4500) currentState = HOME;
      break;
    }


    // ── EXCITED ──────────────────────────────────────────────
    case EXCITED: {
      unsigned long elapsed = now - stateStartTime;
      int sX = random(-2, 3), sY = random(-2, 3); // Screen-shake offsets
      drawCrazyMouth(64+sX, 52+sY);
      int ePulse = (elapsed % 200 < 100) ? 2 : 0;
      drawCrazyEye(30+sX, 22+sY, 17+ePulse);
      drawCrazyEye(98+sX, 22+sY, 17-ePulse);
      for (int i = 0; i < 4; i++) { // Chaotic static lines
        int rx = random(0,128), ry = random(0,64);
        u8g2.drawLine(rx, ry, rx+random(-5,6), ry+random(-5,6));
      }
      digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(random(50,200)); digitalWrite(BUZZER_PIN, LOW);
      if (elapsed > 2500) currentState = HOME;
      break;
    }


    // ── SECRET GIFT ──────────────────────────────────────────
    case SECRET_GIFT: {
      u8g2.setFont(u8g2_font_logisoso24_tf);
      u8g2.drawStr(64-(u8g2.getStrWidth("ROMI")/2), 28, "ROMI");
      u8g2.setFont(u8g2_font_helvB14_tf);
      u8g2.drawStr(64-(u8g2.getStrWidth("LOVE YOU")/2), 60, "LOVE YOU");
      if ((now / 200) % 2 == 0) { // Soft pulse beep
        digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(150); digitalWrite(BUZZER_PIN, LOW);
      }
      if (now - stateStartTime > 5000) currentState = HOME; // Auto-exit after 5s
      break;
    }

  } // end switch

  u8g2.sendBuffer(); // Push buffer to physical display
  unsigned long frameElapsed = millis() - lastFrameAt;
  if (frameElapsed < FRAME_INTERVAL_MS) delay(FRAME_INTERVAL_MS - frameElapsed);
  lastFrameAt = millis();
}
