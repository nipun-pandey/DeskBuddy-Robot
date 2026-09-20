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
  BLINK,        // Eyes look left→right→blink          (1 tap)
  HEART,        // Pulsing heart eyes + rocket fireworks (2 taps)
  EXCITED,      // Shaking star eyes + noise             (3 taps)
  TIMER_MODE,   // Pomodoro countdown                    (4 taps)
  PETTING,      // Rabbit mode                     (long press)
  COMPLIMENT,   // Bold motivational text after BLINK
  SECRET_GIFT   // Hidden love message              (10-tap secret)
};

State currentState  = HOME;
State previousState = HOME;


// ============================================================
//  CLOCK SETTINGS
//  clockFormat: 0=12H  1=12H+sec  2=24H  3=24H+sec (default)
//  Cycle with 5-tap.
// ============================================================
int clockFormat = 3;

const unsigned long AUTO_BLINK_INTERVAL = 60000; // Auto-blink every 60s
unsigned long lastAutoBlink = 0;
bool isAutoBlink = false; // true = silent auto-blink (no compliment after)


// ============================================================
//  TIMER SETTINGS
// ============================================================
bool isTimerRunning  = false; // Countdown actively ticking
bool wasTimerRunning = false; // Saved state before PETTING — restored on exit

// Selectable durations (minutes). Cycle with double-tap in TIMER_MODE.
// If you add/remove entries, update the % modulus in the tap handler.
int  timerOptions[]    = {50, 25, 10, 5, 1};
int  currentOptionIndex = 1;          // Default: 25 min
long remainingSeconds   = 25 * 60;    // Live countdown (seconds)
long timerFullSeconds   = 25 * 60;    // Full duration of the current selection
                                       // ← NEW: used to reset after completion

unsigned long lastTick = 0;           // Timestamp of last 1-second tick

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

const unsigned long LONG_PRESS_MS = 1000; // Hold > 1s → PETTING
const unsigned long TAP_GAP_MS    = 450;  // Gap after last tap before sequence fires


// ============================================================
//  WEATHER
// ============================================================
bool   wifiEverConnected  = false;
bool   hasWeatherData     = false;
String currentWeather     = "Clear";
int    currentTemp        = 0;
unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_INTERVAL = 1800000; // 30 min
bool weatherConnectionInProgress = false;
unsigned long weatherConnectionStarted = 0;
const unsigned long WEATHER_CONNECTION_TIMEOUT = 10000;

float indoorTemperature = NAN;
float indoorHumidity = NAN;
unsigned long lastSensorRead = 0;
const unsigned long SENSOR_INTERVAL = 2000;


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
  int   burstAge;    // Frames since burst started
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
//  tone() is not used because many ESP32 boards lack it —
//  we toggle the pin directly with microsecond delays instead.
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
  return success;
}

void updateSensors(unsigned long now) {
  if (now - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = now;

  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  if (!isnan(temperature)) indoorTemperature = temperature;
  if (!isnan(humidity)) indoorHumidity = humidity;
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
    for (int i = 0; i < 360; i += 90)
      u8g2.drawLine(x, y, x+(cos(i)*6), y+(sin(i)*6));
  }
}


// ============================================================
//  FUNCTION: setManualTime()
//  Fallback clock when NTP fails.  Update these values if needed.
// ============================================================
void setManualTime() {
  struct tm tm;
  tm.tm_year = 2024 - 1900; tm.tm_mon = 3; tm.tm_mday = 14;
  tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;
  time_t t = mktime(&tm);
  struct timeval tv = { .tv_sec = t };
  settimeofday(&tv, NULL);
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

  // Launch a new rocket on schedule (if a slot is free)
  if (now - lastRocketLaunch > ROCKET_INTERVAL) {
    launchRocket();
    lastRocketLaunch = now;
  }

  for (int i = 0; i < MAX_ROCKETS; i++) {
    if (!rockets[i].active) continue;

    if (!rockets[i].burst) {
      // --- RISING PHASE ---
      rockets[i].y += rockets[i].vy; // Move upward (vy is negative)

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
      rockets[i].burstAge++;

      // Draw and move each of the 12 burst particles
      for (int p = 0; p < 12; p++) {
        rockets[i].burstX[p] += rockets[i].burstVx[p];
        rockets[i].burstY[p] += rockets[i].burstVy[p];
        rockets[i].burstVy[p] += 0.12f; // Gravity pulls particles down

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
    u8g2.drawRBox(x - 6 + pupilOffset, y - 10, 4, 12, 1); // Left pupil slit
    u8g2.drawRBox(x + 2 + pupilOffset, y - 10, 4, 12, 1); // Right pupil slit
    u8g2.setDrawColor(1);
  }
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

  pinMode(TOUCH_PIN,  INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();
  dht.begin();

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

  // --- WiFi connection ---
  // No credentials means offline mode: skip connection attempts and start immediately.
  bool networkConfigured = strlen(ssid) > 0 && strlen(weatherKey) > 0;
  bool forceSkip = !networkConfigured, touchWasHigh = false;
  unsigned long startAttempt = millis();

  if (networkConfigured) {
    WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(200);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(ssid, password);
  }

  while (networkConfigured && WiFi.status() != WL_CONNECTED) {
    bool touchNow = digitalRead(TOUCH_PIN);
    if (touchNow && !touchWasHigh) { forceSkip = true; break; }
    touchWasHigh = touchNow;
    if (millis() - startAttempt > 10000) { WiFi.begin(ssid, password); startAttempt = millis(); }
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(25, 18, "CONNECTING...");
    int pw = (int)(sin(millis()/300.0)*38)+50;
    u8g2.drawRBox(64-(pw/2), 58, pw, 5, 2); u8g2.sendBuffer(); delay(100);
  }

  // --- NTP sync → then WiFi off ---
  if (!forceSkip && WiFi.status() == WL_CONNECTED) {
    configTime(19800, 0, "pool.ntp.org", "time.google.com"); // IST = UTC+5:30
    struct tm timeinfo; int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 20) { delay(500); retry++; }
    if (getLocalTime(&timeinfo)) { beepClick(50); } // Success beep
    else { setManualTime(); }
    wifiEverConnected = true;
    updateWeather();
    lastWeatherUpdate = millis();
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  } else {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); setManualTime();
  }

  tapCount = 0; lastTouchState = HIGH; lastAutoBlink = millis();
}


// ============================================================
//  drawHome()
//  Renders the main clock + sensor dashboard screen.
// ============================================================
void drawHome() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;

  // --- Top date pill ---
  u8g2.setDrawColor(1); u8g2.drawRBox(15, 2, 98, 13, 3);
  u8g2.setDrawColor(0); u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  const char* days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  char dateBuf[45];
  sprintf(dateBuf, "%s | %02d/%02d/%d", days[ti.tm_wday], ti.tm_mday, ti.tm_mon+1, ti.tm_year+1900);
  u8g2.drawStr(22, 12, dateBuf);
  u8g2.setDrawColor(1);

  // --- Clock (4 formats cycled by 5-tap) ---
  char timeStr[15];
  int h12 = ti.tm_hour % 12; if (h12 == 0) h12 = 12;
  bool isPM = (ti.tm_hour >= 12);

  if (clockFormat == 0) {
    u8g2.setFont(u8g2_font_logisoso28_tn);
    sprintf(timeStr, "%02d:%02d", h12, ti.tm_min);
    u8g2.drawStr(20, 45, timeStr);
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(105, 45, isPM ? "PM" : "AM");
  } else if (clockFormat == 1) {
    u8g2.setFont(u8g2_font_logisoso20_tn);
    sprintf(timeStr, "%02d:%02d:%02d", h12, ti.tm_min, ti.tm_sec);
    u8g2.drawStr(10, 45, timeStr);
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(112, 45, isPM ? "P" : "A");
  } else if (clockFormat == 2) {
    u8g2.setFont(u8g2_font_logisoso28_tn);
    sprintf(timeStr, "%02d:%02d", ti.tm_hour, ti.tm_min);
    u8g2.drawStr(25, 45, timeStr);
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(105, 45, "24H");
  } else {
    u8g2.setFont(u8g2_font_logisoso20_tn);
    sprintf(timeStr, "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    u8g2.drawStr(15, 45, timeStr);
  }

  // --- Bottom sensor row ---
  u8g2.drawLine(0, 52, 128, 52);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(4, 63);
  if (isnan(indoorTemperature)) u8g2.print("--.-C");
  else { u8g2.print(indoorTemperature, 1); u8g2.print((char)176); u8g2.print("C"); }
  u8g2.setCursor(44, 63);
  if (isnan(indoorHumidity)) u8g2.print("--%");
  else { u8g2.print((int)indoorHumidity); u8g2.print("%"); }
  if (hasWeatherData) {
    u8g2.drawLine(64, 52, 64, 64);
    drawWeatherIcon(74, 58);
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.setCursor(88, 63); u8g2.print((float)currentTemp, 1); u8g2.print((char)176); u8g2.print("C");
  }
}


// ============================================================
//  loop()
// ============================================================
void loop() {
  u8g2.clearBuffer();
  unsigned long now = millis();
  bool touching = digitalRead(TOUCH_PIN);


  // ── 1. BACKGROUND SENSOR AND WEATHER UPDATES ─────────────
  updateSensors(now);
  if (!weatherConnectionInProgress && now - lastWeatherUpdate >= WEATHER_INTERVAL) {
    startWeatherRefresh(now);
  }
  if (weatherConnectionInProgress) {
    if (WiFi.status() == WL_CONNECTED) {
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


  // ── 2. AUTO-BLINK (idle animation every 60s on HOME) ─────
  if (currentState == HOME && (now - lastAutoBlink >= AUTO_BLINK_INTERVAL)) {
    isAutoBlink = true; currentState = BLINK; stateStartTime = now; lastAutoBlink = now;
  }


  // ── 3. TOUCH SENSING ─────────────────────────────────────
  // A) Rising edge: finger just touched
  if (touching && !lastTouchState) lastTouchTime = now;

  // B) Long press → PETTING (or SECRET_GIFT if 10 taps precede it)
  if (touching && (now - lastTouchTime > LONG_PRESS_MS)) {
    if (tapCount == 10) {
      currentState = SECRET_GIFT; stateStartTime = now; tapCount = 0;
    } else if (currentState != PETTING && currentState != SECRET_GIFT) {
      previousState   = currentState;
      wasTimerRunning = isTimerRunning; // Save timer state before interrupting
      currentState    = PETTING;
      stateStartTime  = now;
    }
  }

  // C) Falling edge: finger lifted
  if (!touching && lastTouchState) {
    if (currentState == PETTING) {
      currentState   = previousState;      // Return to prior state
      isTimerRunning = wasTimerRunning;    // Restore timer
      if (isTimerRunning) lastTick = now;  // Resync tick to avoid phantom decrement
    } else if (currentState == SECRET_GIFT) {
      currentState = HOME;
    } else {
      tapCount++; // Count this tap — action fires after TAP_GAP_MS silence
    }
    lastTouchTime = now;
  }
  lastTouchState = touching;


  // ── 4. TIMER COUNTDOWN ───────────────────────────────────
  // Ticks even during PETTING so the background timer keeps running.
  if ((currentState == TIMER_MODE ||
       (currentState == PETTING && wasTimerRunning)) && isTimerRunning) {

    if (now - lastTick >= 1000) {
      lastTick = now;

      if (remainingSeconds > 0) {
        remainingSeconds--;

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
  if (tapCount > 0 && (now - lastTouchTime > TAP_GAP_MS)) {
    isAutoBlink = false;

    if (currentState == TIMER_MODE) {
      // --- Timer tap controls ---

      if (tapCount == 1) {
        // Pause / resume
        isTimerRunning = !isTimerRunning;
        lastTick = now; // Resync to avoid phantom decrement on resume
        // Beep feedback: low for pause, high for resume
        beepChirp(isTimerRunning ? 600 : 1400, 80);
      }
      else if (tapCount == 2) {
        // Reset or switch duration
        isTimerRunning = false;
        if (remainingSeconds < timerFullSeconds) {
          // Time has elapsed → reset to start of current duration
          remainingSeconds = timerFullSeconds;
        } else {
          // Already at start → advance to next duration option
          currentOptionIndex = (currentOptionIndex + 1) % 5;
          timerFullSeconds   = (long)timerOptions[currentOptionIndex] * 60;
          remainingSeconds   = timerFullSeconds;
        }
        beepChirp(1000, 100); // Mid-tone confirmation
      }
      else if (tapCount == 3) {
        // Exit timer → HOME
        currentState     = HOME;
        isTimerRunning   = false;
        remainingSeconds = timerFullSeconds; // Clean reset for next entry
        beepChirp(500, 60); delay(40); beepChirp(500, 60); // Double-beep exit
      }

    } else {
      // --- Normal tap actions ---

      if (tapCount == 1) {
        beepTapFeedback(1);          // Low chirp
        currentState   = BLINK;
        stateStartTime = now;
      }
      else if (tapCount == 2) {
        beepTapFeedback(2);          // Slightly higher chirp
        initRockets();               // Reset rocket system for HEART state
        currentState   = HEART;
        stateStartTime = now;
      }
      else if (tapCount == 3) {
        beepTapFeedback(3);
        currentState   = EXCITED;
        stateStartTime = now;
      }
      else if (tapCount == 4) {
        beepTapFeedback(4);
        currentState   = TIMER_MODE;
        isTimerRunning = false; // Start paused so user sets duration first
        lastTick       = now;
      }
      else if (tapCount == 5) {
        beepTapFeedback(5);          // Highest chirp
        clockFormat = (clockFormat + 1) % 4;
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
        u8g2.drawStr(35, 12, isTimerRunning ? "FOCUSED..." : "PAUSED / SET");
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
      u8g2.drawStr(18, 63, "1=PAUSE 2=RST 3=EXIT");
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
      if (elapsed > 3000) currentState = HOME;
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
      int pX = 0; bool eClosed = false;
      // Timeline: look-left → look-right → centre → close → open
      if      (elapsed < 400)  pX = -8;
      else if (elapsed < 800)  pX = 8;
      else if (elapsed < 1000) pX = 0;
      else if (elapsed < 1350) eClosed = true;

      drawMovingEye(30, 17, eClosed, pX);
      drawMovingEye(98, 17, eClosed, pX);
      drawUltraMouth(64, 58, eClosed);

      // Tiny click beep at eye-close moment (manual taps only)
      if (elapsed >= 1000 && elapsed < 1020 && !isAutoBlink) beepClick(2);

      if (elapsed > 1500) {
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

      // --- Heart PULSE (breathing size effect) ---
      // Size oscillates between 14 and 19 using a sine wave
      // Period ~600ms — feels like a heartbeat at ~100 BPM
      float pulse = sin(elapsed * 0.010f); // 0.010 ≈ one cycle per 628ms
      int heartSize = 16 + (int)(pulse * 3.0f); // Range: 13–19 pixels

      // --- Sky-rocket fireworks in the background ---
      updateAndDrawRockets();

      // --- Draw the two pulsing heart eyes ---
      drawHeartEye(32, 25, heartSize, eClosed, pX);  // Left
      drawHeartEye(96, 25, heartSize, eClosed, pX);  // Right

      // --- Heartbeat sound: two quick pulses like "lub-dub" ---
      // First beat at 0ms of each 700ms cycle
      // Second beat at 120ms of each 700ms cycle
      if (elapsed % 700 < 20 || (elapsed % 700 > 120 && elapsed % 700 < 140)) {
        digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(200); digitalWrite(BUZZER_PIN, LOW);
      }

      // Extended duration: 3.5 seconds so rockets have time to launch and burst
      if (elapsed > 3500) currentState = HOME;
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
}
