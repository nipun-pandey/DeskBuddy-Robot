# DeskBuddy Robot 🤖

**DeskBuddy Robot** is a minimal, clean ESP32-C3 desktop companion featuring animated expressive eyes, a compact digital clock, a love animation, a crazy funny face, and an on-demand Pomodoro timer and millisecond stopwatch.

---

## 1. Minimal 5-Gesture Mapping

DeskBuddy uses a simple, intuitive single-button / touch input scheme with only five tap gestures:

| Gesture | Name | Description |
| :--- | :--- | :--- |
| **1 Tap** | **Love Animation ❤️** | Heart-shaped pulsing eyes with cute floating hearts drifting upward. Returns to Home after 3 seconds. |
| **2 Taps** | **Timer & Stopwatch ⏱️** | Opens a minimal 2-option menu: Pomodoro Timer (50, 25, 10, 5, 1 min) or persistent Stopwatch with large readable OLED digits. |
| **3 Taps** | **Crazy Face 🤪** | Animated star eyes with alternating pupils, big open mouth, playful audio chirps, and screen shake. Returns to Home after 2.5 seconds. |
| **4 Taps** | **Clock Format 🕒** | Cycles through 4 formats: `12H`, `12H+sec` (default), `24H`, and `24H+sec`. Selection is automatically saved in flash storage (NVS) across reboots. |
| **5 Taps** | **Time Sync 🔄** | Connects to Wi-Fi in the background, syncs with NTP servers (IST UTC+05:30), updates the clock, and displays a subtle `OK` / `ERR` indicator. |

---

## 2. Minimal OLED Interface

The SH1106 128×64 OLED display presents an uncluttered, modern interface:

- **Home Screen:**
  - Expressive robot eyes with natural eye-blinking animation every few seconds and gentle idle gaze.
  - A clean horizontal divider.
  - Centered clock in your chosen persisted format.
  - Subtle unobtrusive sync indicator (`SYNC`, `OK`, or `ERR`).
- **Timer & Stopwatch Menu (2 Taps):**
  - Minimal 2-item list: `1. POMODORO TIMER` and `2. STOPWATCH`.
  - `1 Tap`: Cycle selection.
  - `2 Taps` or `Hold`: Open selected tool.
  - `3 Taps`: Return to Home.
- **Pomodoro Timer Screen:**
  - Extra-large countdown digits (`MM:SS`).
  - Small header with duration setting (`POMO 25M`) and status (`RUNNING` / `PAUSED` / `READY`).
  - `1 Tap`: Start / Pause / Resume.
  - `2 Taps`: Reset to full duration.
  - `3 Taps`: Cycle duration (`50`, `25`, `10`, `5`, `1` min).
  - `4 Taps`: Switch directly to Stopwatch.
  - `Hold (>0.8s)`: Return to Menu.
  - 5-second countdown warning beeps at seconds 5, 4, 3, 2, 1.
  - On finish: completion alarm and auto-reset back to chosen duration.
- **Stopwatch Screen:**
  - Extra-large elapsed digits (`MM:SS.t` with live tenths; switches to `HH:MM:SS` after 1 hour).
  - `1 Tap`: Start / Pause / Resume.
  - `2 Taps`: Reset to 00:00.
  - `3 Taps`: Switch directly to Pomodoro.
  - `Hold (>0.8s)`: Return to Menu.
  - Elapsed time is measured using monotonic timestamps (`millis()`) and saved in NVS across reboots.

---

## 3. Hardware Specification

| Component | Pin / Connection | Notes |
| :--- | :--- | :--- |
| **Microcontroller** | ESP32-C3 DevKitM-1 | 160 MHz 32-bit RISC-V, 4 MB Flash, 400 KB SRAM |
| **SH1106 OLED SDA** | GPIO 8 | I2C Data line (400 kHz fast mode) |
| **SH1106 OLED SCL** | GPIO 9 | I2C Clock line |
| **Passive Buzzer**   | GPIO 5 | Piezo buzzer for feedback tones and alarms |
| **Touch / Button**   | GPIO 7 | Capacitive touch sensor (active-HIGH, internal pulldown) |
| **Power**            | 3.3V & GND | Common ground across all modules; 3.3V logic levels |

---

## 4. Setting Time via Bluetooth (BLE)

DeskBuddy advertises as **`DeskBuddy`** using NimBLE:
1. Connect via any BLE utility (such as LightBlue or nRF Connect).
2. Service UUID: `a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001`.
3. Characteristic UUID: `a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001`.
4. Write the current Unix UTC epoch timestamp (in seconds or 13-digit milliseconds).
5. DeskBuddy sets its RTC, applies the IST offset (+05:30), beeps for confirmation, and marks the clock as synchronized.

---

## 5. Build and Upload

### PlatformIO
1. Copy `firmware/esp32/DeskBuddy-Robot/secrets.example.h` to `firmware/esp32/DeskBuddy-Robot/secrets.h` and add your Wi-Fi credentials.
2. Build and upload:
   ```sh
   pio run --target upload
   ```

---

## License

MIT License. See [LICENSE](LICENSE) for details.
