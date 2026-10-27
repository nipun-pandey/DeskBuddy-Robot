# DeskBuddy Robot Design

DeskBuddy Robot is designed as a local-first, highly responsive desktop companion. Its design decouples visual interaction and touch input from network and sensor latency.

## Architecture

```text
┌────────────────────────────────────────────────────────┐
│             UI & Interaction Loop (Core 0)             │
│  - 60 FPS SH1106 OLED Display Pipeline                 │
│  - Debounced Touch Gesture State Machine               │
│  - Non-blocking Microsecond Tone Audio Engine          │
│  - Pomodoro Timer & Millisecond Persistent Stopwatch   │
│  - 10+ Expressive Animations & Compliments             │
└──────────────────────────┬─────────────────────────────┘
                           │ Shared Safe State
┌──────────────────────────┴─────────────────────────────┐
│          Background Asynchronous Workers (RTOS)        │
│  - FreeRTOS Background Network Task (Wi-Fi/NTP/Weather)│
│  - NimBLE BLE Time Synchronization Service             │
│  - Non-blocking DHT11 Environmental Sampler            │
└────────────────────────────────────────────────────────┘
```

## Functional Modules

1. **Interaction & State Machine:**
   - Single GPIO (GPIO 7) input filtered through 25ms debouncing.
   - Evaluates multi-taps (1 to 8 taps) with a 500ms settlement window.
   - Disambiguates between normal taps, long presses (>1.0s), and the secret multi-tap hold gesture (3 taps + hold) without false releases.

2. **Display Pipeline:**
   - Full-frame buffer rendering (`U8G2_SH1106_128X64_NONAME_F_HW_I2C`) over 400 kHz I2C.
   - Paced frame limiter (`FRAME_INTERVAL_MS = 16`) providing steady, tear-free 60 FPS animation.
   - Zero dynamic heap allocation during frame rendering.

3. **Audio Subsystem:**
   - Square wave tone generation on GPIO 5 passive buzzer.
   - Rising-pitch audio feedback for sequential taps.
   - Multi-burst frequency sweep for Pomodoro completion alarm.

4. **Bluetooth Low Energy (BLE):**
   - Advertises service `a7e10001-8f5d-4b4a-9d42-4e8b7a0c0001`.
   - Accepts Unix epoch strings via characteristic `a7e10002-8f5d-4b4a-9d42-4e8b7a0c0001`.
   - Implements `NimBLEServerCallbacks` with automatic advertising restart on disconnection.

5. **Power & Memory Efficiency:**
   - Low memory footprint: <9% RAM utilization and <48% Flash utilization on ESP32-C3.
   - Wi-Fi radio powered off between periodic 30-minute sync windows.
