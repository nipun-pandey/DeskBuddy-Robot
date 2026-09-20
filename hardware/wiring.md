# Wiring Guide

The current firmware targets an ESP32-C3 and uses the pin map below.

| Component | ESP32-C3 pin | Notes |
| --- | --- | --- |
| SH1106 OLED SDA | GPIO 8 | I2C data |
| SH1106 OLED SCL | GPIO 9 | I2C clock |
| DHT11 data | GPIO 6 | Add the module's required pull-up if it does not include one |
| Passive buzzer signal | GPIO 5 | Use a transistor driver if the buzzer draws more current than a GPIO can safely supply |
| Touch sensor / button output | GPIO 7 | Firmware expects HIGH when touched/pressed |
| OLED, DHT11, touch module | 3.3 V | Keep logic at 3.3 V |
| All components | GND | A common ground is required |

The firmware initializes I2C with `Wire.begin(8, 9)`. Do not use the older GPIO 21/22 wiring map with this revision.
