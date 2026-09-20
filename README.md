# DeskBuddy AI Robot 🤖

DeskBuddy is an ESP32-C3 desk companion with an SH1106 128×64 OLED display, DHT11 sensor, passive buzzer, clock, weather snapshot, touch gestures, and a Pomodoro timer.

## Features

- Clock and date, synchronized with NTP when Wi-Fi is configured
- Indoor temperature and humidity dashboard
- Weather snapshot refreshed at boot and every 30 minutes
- Touch gestures for animated reactions, compliments, clock formats, and timer controls
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
| 1 tap | Blink animation and compliment |
| 2 taps | Heart animation |
| 3 taps | Excited animation |
| 4 taps | Pomodoro timer mode |
| 5 taps | Change clock format |
| Hold for 1 second | Petting animation |

In timer mode: one tap starts/pauses, two taps reset or choose the next duration, and three taps exit.

## Build and upload

1. Install [PlatformIO](https://platformio.org/).
2. Copy `firmware/esp32/main/secrets.example.h` to `firmware/esp32/main/secrets.h`.
3. Set your Wi-Fi and OpenWeather API credentials in `secrets.h`. This local file is ignored by Git.
4. Connect the board and run:

```sh
pio run --target upload
pio device monitor --baud 115200
```

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
