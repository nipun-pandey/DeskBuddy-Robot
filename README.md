# DeskBuddy Robot 🤖

**DeskBuddy Robot** is a responsive, interactive ESP32-C3 desk companion featuring an SH1106 128×64 I2C OLED display, DHT11 temperature and humidity sensor, passive buzzer with audible feedback, capacitive touch sensor, background Wi-Fi for NTP and weather, Bluetooth Low Energy (BLE) time synchronization, animated emotional expressions, and a Pomodoro countdown timer.

---

## Features

- **Non-blocking Firmware Architecture:** Built with an asynchronous FreeRTOS background worker for Wi-Fi, NTP, and weather requests, keeping OLED rendering at a smooth 60 FPS without UI freezes.
- **Accurate Clock & NTP Timezone:** Automatic time synchronization with NTP configured for India Standard Time (**IST: UTC+05:30**). Supports 12H, 12H+sec (boot default), 24H, and 24H+sec formats.
- **Offline Resilient:** If booted without Wi-Fi, the clock initializes to an explicit unsynchronized state (`--:--`) rather than fabricating historical build dates. Once synchronized, the ESP32 RTC maintains time continuously even if Wi-Fi disconnects.
- **Bluetooth Low Energy (BLE) Time Setting:** Broadcasts as `DeskBuddy` using NimBLE. Allows setting the clock directly from any BLE utility or smartphone app. Automatically resumes advertising when a client disconnects.
- **Indoor Climate Dashboard:** Displays real-time temperature (°C) and relative humidity (%) sampled every 2 seconds with high/low climate alerts (`HOT!`, `COLD`, `HUM!`, `DRY!`).
- **Weather & Forecast:** Fetches current condition and a 3-hour forecast from OpenWeatherMap at boot and refreshes every 30 minutes in the background, sleeping the Wi-Fi radio between updates.
- **Rich Emotional Expressions:** Includes eased eye blinking, pulsing heart eyes with rising skyrocket fireworks, excited star eyes with vibration, petting rabbit mode with purring sound, compliments, sleepy, curious, sad, listening, and celebration confetti.
- **Audible Touch Feedback:** A short rising-pitch square wave confirms every tap immediately, providing satisfying haptic/audio confirmation.
- **Pomodoro Timer & Persistent Stopwatch:** 
  - Pomodoro timer with selectable durations (50, 25, 10, 5, 1 min), 5-second countdown warning beeps, alarm finish, and auto-reset.
  - Millisecond-accurate stopwatch that persists elapsed time across reboots in non-volatile storage (NVS).

---

## Hardware Specification

| Component | Pin / Connection | Description |
| :--- | :--- | :--- |
| **Microcontroller** | ESP32-C3 DevKitM-1 | 160 MHz 32-bit RISC-V, 400 KB SRAM, 4 MB Flash |
| **SH1106 OLED SDA** | GPIO 8 | I2C Data line (400 kHz fast mode) |
| **SH1106 OLED SCL** | GPIO 9 | I2C Clock line |
| **DHT11 Sensor Data**| GPIO 6 | Single-wire digital data (with pull-up resistor) |
| **Passive Buzzer**   | GPIO 5 | Piezo buzzer for tones and alarms |
| **Touch / Button**   | GPIO 7 | Capacitive touch sensor (active-HIGH, internal pulldown) |
| **Power**            | 3.3V & GND | Common ground across all modules; 3.3V logic levels |

> [!IMPORTANT]
> The ESP32-C3 supports 2.4 GHz Wi-Fi and Bluetooth 5 (BLE). It does not support Bluetooth Classic (BR/EDR). All peripheral logic levels must not exceed 3.3V.

---

## Touch Gestures & Controls

DeskBuddy features a debounced, multi-tap gesture state machine with clear settlement timeouts:

### General & Home Mode
| Gesture | Action |
| :--- | :--- |
| **1 Tap** | Eased blinking eye animation, followed by a random motivational compliment |
| **2 Taps** | Heart-shaped eyes with interactive skyrocket fireworks display |
| **3 Taps** | Excited reaction with shaking star eyes and playful sound |
| **4 Taps** | Enter Pomodoro countdown timer mode |
| **5 Taps** | Cycle clock formats (`12H`, `12H+sec`, `24H`, `24H+sec`) |
| **6 Taps** | Open Weather & 3-hour forecast dashboard |
| **7 Taps** | Cycle expressions (`SLEEPY`, `CURIOUS`, `SAD`, `LISTENING`, `CELEBRATION`) |
| **8 Taps** | Trigger manual background Wi-Fi and NTP time synchronization |
| **Long Press (>1.0s)** | Petting mode: transforms into rabbit face with bouncing ears and soft purr |
| **3 Taps + Hold** | Secret gift mode: displays `DESKBUDDY LOVE YOU!` with heart pulses |

### Pomodoro Timer Mode
| Gesture | Action |
| :--- | :--- |
| **1 Tap** | Start / Pause countdown |
| **2 Taps** | Reset countdown to selected duration |
| **3 Taps** | Cycle duration (`50`, `25`, `10`, `5`, `1` minute) |
| **4 Taps** | Switch to Stopwatch mode |
| **5 Taps** | Exit Timer mode back to Home |

### Stopwatch Mode
| Gesture | Action |
| :--- | :--- |
| **1 Tap** | Start / Pause stopwatch |
| **2 Taps** | Reset stopwatch to 00:00 |
| **3 Taps** | Switch to Pomodoro Timer mode |
| **4 Taps** | Exit Stopwatch mode back to Home (elapsed time saved in NVS) |

---

## Setting Time via Bluetooth (BLE)

You can synchronize DeskBuddy's clock instantly without Wi-Fi using any BLE scanner application (such as nRF Connect, LightBlue, or a custom script):

1. Scan for nearby Bluetooth devices and connect to **`DeskBuddy`**.
2. Locate the service UUID: `a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001`.
3. Locate the characteristic UUID: `a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001`.
4. Write the current Unix UTC epoch timestamp (either in 10-digit seconds e.g. `1729000000` or 13-digit milliseconds) as a UTF-8 text string or raw bytes.
5. DeskBuddy immediately sets its internal RTC, applies the IST offset (+05:30), beeps for confirmation, and marks the clock as synchronized.
6. When disconnected, DeskBuddy automatically resumes advertising.

---

## Build and Upload

### Method 1: PlatformIO (Recommended)
1. Install [PlatformIO Core or IDE](https://platformio.org/).
2. Copy `firmware/esp32/DeskBuddy-Robot/secrets.example.h` to `firmware/esp32/DeskBuddy-Robot/secrets.h`:
   ```sh
   cp firmware/esp32/DeskBuddy-Robot/secrets.example.h firmware/esp32/DeskBuddy-Robot/secrets.h
   ```
3. Edit `secrets.h` with your Wi-Fi SSID, password, and OpenWeatherMap API key (this file is ignored by Git).
4. Build and upload firmware to your connected board:
   ```sh
   pio run --target upload
   pio device monitor --baud 115200
   ```

### Method 2: Arduino IDE
1. Install the **esp32** board support package in Arduino IDE.
2. Select board **ESP32C3 Dev Module** (or `ESP32-C3 DevKitM-1`).
3. Install required libraries via Library Manager:
   - `U8g2` by oliver
   - `DHT sensor library` by Adafruit
   - `Adafruit Unified Sensor` by Adafruit
   - `ArduinoJson` by Benoît Blanchon (v6.x)
   - `NimBLE-Arduino` by h2zero (v1.4.x)
4. Open `firmware/esp32/DeskBuddy-Robot/DeskBuddy-Robot.ino`.
5. Compile and flash.

---

## Project Structure

```text
DeskBuddy-Robot/
├── firmware/
│   └── esp32/
│       └── DeskBuddy-Robot/
│           ├── DeskBuddy-Robot.ino   # Main application firmware
│           └── secrets.example.h     # Wi-Fi and API credentials template
├── hardware/
│   └── wiring.md                     # Detailed pinout & wiring notes
├── docs/
│   ├── design.md                     # System architecture & state machine design
│   └── working.md                    # Operational principles & power management
├── images/
│   └── robot.jpg                     # Hardware photo
├── platformio.ini                    # PlatformIO build configuration
└── README.md                         # Project documentation
```

---

## Troubleshooting

- **OLED is blank:** Check 3.3V power, GND, and I2C wiring (SDA = GPIO 8, SCL = GPIO 9). Verify the display module uses the SH1106 controller.
- **Clock shows `DESKBUDDY (NO SYNC)`:** The device has not yet synchronized with NTP or BLE. Check Wi-Fi credentials in `secrets.h` or connect via BLE to set the time.
- **Sensor shows `--.-C`:** Ensure DHT11 data pin is connected to GPIO 6 and has a 4.7k–10kΩ pull-up resistor to 3.3V if the sensor module does not integrate one.
- **Wi-Fi connection fails:** ESP32-C3 only supports 2.4 GHz Wi-Fi networks (802.11 b/g/n). 5 GHz networks are not supported.

---

## Security

Never commit `secrets.h`. The `.gitignore` file is configured to exclude `secrets.h` automatically.

---

## License

MIT License. See [LICENSE](LICENSE) for details.
