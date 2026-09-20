# DeskBuddy Design

DeskBuddy is a local-first companion robot. Its primary interaction loop—display, touch gestures, timer, animations, and indoor sensor readings—works without network access.

## Modules

1. **Interaction** — GPIO 7 touch/button input is interpreted as taps and long presses.
2. **Display** — an SH1106 128×64 I2C OLED presents the clock, sensor dashboard, and animations.
3. **Sensors** — a DHT11 supplies indoor temperature and humidity, sampled every two seconds.
4. **Sound** — a passive buzzer provides gesture and timer feedback.
5. **Network** — Wi-Fi connects only for NTP and weather. Weather is fetched at boot and then refreshed every 30 minutes; Wi-Fi is turned off between refreshes.

The firmware is a state machine with HOME, animation, timer, petting, compliment, and secret-gift states. Long-running user interactions remain independent of periodic sensor and network updates.
