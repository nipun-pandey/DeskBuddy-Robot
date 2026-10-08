# DeskBuddy AI Robot 🤖

DeskBuddy is an ESP32-C3 desk companion with an SH1106 128×64 OLED display, DHT11 sensor, passive buzzer, clock, weather snapshot, touch gestures, and a Pomodoro timer.

## Features

- Clock and date, with hourly NTP refreshes while the home screen is idle
- If saved Wi-Fi is unavailable, scans nearby open Wi-Fi networks and tries NTP
- Uses the firmware build time as a fallback when no NTP network is reachable
- Indoor temperature and humidity dashboard
- Hot/cold and high/low humidity alerts on the home screen
- Current weather and a three-hour forecast when an OpenWeather key is configured
- BLE time setting from a phone or generic BLE utility
- Small heart badge on the home clock and a gesture to retry NTP sync
- Sleepy, curious, sad, listening, and celebration expressions
- Touch gestures for animated reactions, compliments, clock formats, and timer controls
- A short rising-pitch buzzer tone immediately confirms every tap, including each tap in a multi-tap gesture
- Home clock boots in 12-hour format with seconds
- Offline fallback: the robot continues to work when Wi-Fi is unavailable

## Hardware

| Part | Connection |
| --- | --- |
| ESP32-C3 | ESP32-C3 DevKitM-1-compatible board |
| SH1106 OLED SDA | GPIO 8 |
| SH1106 OLED SCL | GPIO 9 |
| DHT11 data | GPIO 6 |
| Passive buzzer | GPIO 5 |
| Touch sensor / button | GPIO 7 |
| All modules | 3.3 V logic and common GND |

Do not power motors or other high-current devices from the ESP32-C3 board. Confirm that the OLED and touch module support 3.3 V before connecting them.

## Gestures

| Input | Action |
| --- | --- |
| 1 tap | Two-second blink animation, then a one-second compliment |
| 2 taps | Heart animation |
| 3 taps | Stopwatch mode |
| 4 taps | Change clock format |
| 5 taps | Excited animation |
| Hold for 1 second | Petting animation |
| 6 taps | Show current weather and three-hour forecast |
| 7 taps | Cycle through the five new expressions |
| 8 taps | Retry clock sync over Wi-Fi |
| 9 taps, then hold within 5 seconds | Secret message |

In stopwatch mode: one tap starts/pauses, two taps reset to zero, three taps switch to timer mode, and four taps exit while saving elapsed time. In timer mode: one tap starts/pauses, two taps reset the selected duration, three taps cycle through timer durations, four taps switch back to the stopwatch, and five taps exit while preserving the remaining time. The stopwatch value is restored after reboot.

## Build and upload

1. Install [PlatformIO](https://platformio.org/).
2. Copy `firmware/esp32/main/secrets.example.h` to `firmware/esp32/main/secrets.h`.
3. Set your Wi-Fi and OpenWeather API credentials in `secrets.h`. This local file is ignored by Git.
4. Connect the board and run:

```sh
pio run --target upload
pio device monitor --baud 115200
```

At startup, ROMI tries the saved Wi-Fi network first. If it cannot sync the
clock there, it tries nearby open networks from strongest signal to weakest.
Open networks with captive portals or without Internet access cannot provide
time; in that case ROMI starts with the firmware build timestamp. BLE time
setting is available through a BLE utility or an app that can write the
documented timestamp characteristic.

To set time over BLE, connect to the device named `ROMI` and write the current
Unix UTC timestamp as decimal text to characteristic
`a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001` in service
`a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001`. Seconds and 13-digit milliseconds are
accepted. ROMI retries NTP hourly while the home screen is idle; use the
eight-tap gesture to retry immediately. The top-right heart is decorative.
Sensor alert limits are 16–30 °C and 25–70% humidity.

The default target is `esp32-c3-devkitm-1`. Change the board in `platformio.ini` if your ESP32-C3 board uses a different definition.

## Project layout

```text
firmware/esp32/main/  ESP32 firmware and local configuration example
hardware/             Wiring documentation
docs/                 Design and interaction notes
```

## Security

Never commit `secrets.h`. If credentials were ever committed to a public remote, rotate them and remove them from Git history before sharing the repository further.

## License

MIT. See [LICENSE](LICENSE).
