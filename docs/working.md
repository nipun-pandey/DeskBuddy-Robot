# Working Principle

1. The ESP32-C3 initializes the OLED, DHT11, touch input, and buzzer.
2. If local Wi-Fi credentials are configured, it synchronizes the clock through NTP and fetches a weather snapshot. Wi-Fi is then disabled to reduce power use.
3. The home screen shows time/date, cached indoor temperature/humidity, and cached weather when available.
4. Touch gestures select animations, motivational messages, clock layouts, and Pomodoro controls.
5. Every 30 minutes, the firmware reconnects briefly to refresh weather, then disables Wi-Fi again. If the network is unavailable, it leaves the prior display data intact and retries at the next interval.

The robot remains usable without secrets or network access; only accurate network time and weather are unavailable.
