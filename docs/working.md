# Working Principle — DeskBuddy Robot

1. **System Startup:**
   - The ESP32-C3 boots up, sets up GPIOs, and initializes the SH1106 OLED display over high-speed 400 kHz I2C (`Wire.begin(8, 9)`).
   - The DHT11 sensor and buzzer outputs are configured.
   - The local timezone is configured for India Standard Time (`IST-5:30`, UTC+05:30).
   - Stored stopwatch elapsed time is retrieved from non-volatile storage (`Preferences`).
   - A playful "HELLO BUDDY" boot animation plays on the screen.
   - Bluetooth Low Energy (BLE) service is launched as `DeskBuddy`.

2. **Asynchronous Background Connectivity:**
   - A dedicated FreeRTOS background task (`networkWorkerTask`) is spawned on priority 1.
   - The main UI thread immediately begins rendering the HOME screen at 60 FPS without waiting for the network.
   - If Wi-Fi credentials are provided, the background task attempts connection without blocking the UI.
   - Upon connection, it queries NTP servers asynchronously (`pool.ntp.org`, `time.google.com`, `time.cloudflare.com`) and validates the received timestamp.
   - When configured, current weather conditions and 3-hour forecasts are retrieved from OpenWeatherMap using secure HTTPS.
   - The Wi-Fi radio is immediately powered down (`WiFi.mode(WIFI_OFF)`) once sync completes to eliminate idle power draw and RF noise.

3. **Offline & Resilient Operation:**
   - If no Wi-Fi network is available, the robot remains fully interactive. The clock displays an explicit unsynchronized state (`--:--`) until valid time is supplied via BLE or Wi-Fi.
   - Once synchronized, the internal RTC maintains accurate time continuously, even if subsequent network sync attempts fail.

4. **Periodic Maintenance:**
   - Every 30 minutes, the background task powers on Wi-Fi briefly to refresh the weather cache and verify NTP synchronization, then sleeps again.
   - Manual synchronization can be triggered anytime via an 8-tap gesture.
