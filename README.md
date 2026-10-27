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
  - Header displays duration (`25M`), **live clock** (`04:15P`), and status (`RUN` / `PAUS` / `READY`).
  - **When Reset:**
    - `1 Tap`: Cycle duration (`50`, `25`, `10`, `5`, `1` min).
    - `2 Taps` or `Hold`: Start countdown.
    - `3 Taps`: Exit to Home screen.
  - **When Running / Paused:**
    - `1 Tap`: Start / Stop (Pause / Resume).
    - `2 Taps`: Reset to full duration.
    - `3 Taps`: Exit to Home screen.
  - **Every 1 minute completed:** Displays robotic blinking eyes animation for 2 seconds to encourage focus, then automatically returns to the countdown.
  - 5-second countdown warning beeps at seconds 5, 4, 3, 2, 1.
  - On finish: completion alarm and auto-reset back to chosen duration.
- **Stopwatch Screen:**
  - Extra-large elapsed digits (`MM:SS.t` with live tenths; switches to `HH:MM:SS` after 1 hour).
  - Header displays live clock and status.
  - **Streamlined 3-Tap Controls:**
    - `1 Tap`: Start / Stop (Pause / Resume).
    - `2 Taps`: Reset to `00:00.0`.
    - `3 Taps`: Exit to Home screen.
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

## 4. Wi-Fi & Time Configuration

DeskBuddy provides multiple flexible ways to set Wi-Fi credentials and synchronize time without needing to edit source code:

### A. USB Serial Commands (Instant)
Connect the ESP32 to your computer via USB, open a serial terminal at 115200 baud, and send:
- `WIFI:YourSSID,YourPassword` — Saves to NVS flash storage and connects immediately to sync NTP time.
- `TIME:<unix_epoch>` (e.g. `TIME:1793141137`) — Sets the clock immediately to the exact second.
- `SYNC` — Manually triggers Wi-Fi connection and NTP time sync.
- `STATUS` — Prints Wi-Fi SSID, connection state, and current RTC time.

### B. Bluetooth Low Energy (BLE)
DeskBuddy advertises as **`DeskBuddy`** using NimBLE:
1. Connect via any BLE utility (e.g., LightBlue or nRF Connect).
2. Service UUID: `a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001`
3. Characteristics:
   - **Time UUID** (`a7e10002-...`): Write current Unix UTC epoch timestamp.
   - **Wi-Fi UUID** (`a7e10003-...`): Write `YourSSID,YourPassword` to configure and connect.

### C. Touch Gesture (5 Taps)
- Tap **5 times** on the touch sensor / button from the Home screen to trigger background Wi-Fi and NTP synchronization.

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
