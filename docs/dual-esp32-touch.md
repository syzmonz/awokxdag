# Original Dual ESP32 Touch (v1 / v2 / v3)

Experimental 2.4 GHz-only profiles for the classic **ESP32 Dev Module**
(4 MB flash, `huge_app` partition, PSRAM disabled) with an ILI9341 panel and
XPT2046 touchscreen on the AWOK white USB port.

Build:

```bash
python3 scripts/build_firmware.py dual-esp32-touch-v1   # or -v2 / -v3
```

Select **ESP32 Dev Module** if you compile from Arduino IDE, and enable exactly
one of `AWOK_DUAL_ESP32_TOUCH_V1` / `_V2` / `_V3`. Flashing a C5 image onto
these boards, or the reverse, is rejected at compile time.

## Pin map

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 18 / 19 / 23 |
| ILI9341 CS / DC / Reset | 17 / 16 / 5 |
| Backlight | 32 (active high) |
| XPT2046 touch CS | 21 |
| SD card CS | 12 (v1) · 14 (v2, v3) |
| GPS UART2 RX / TX | 4 / 13 @ 115200 NMEA (selectable on the GPS screen) |
| Battery ADC | unset (`kBatteryAdc = -1`) |

These numbers match the ESP32 Marauder **TFT_DIY / v6** (Touch v1) and **v6.1**
(Touch v2/v3) white-port maps. Touch calibration in `board_pins.h` is converted
from Marauder's TFT_eSPI `{339,3470,237,3438,2}` baseline; verify all four
corners on the physical panel.

## Runtime limits

- 2.4 GHz channels only (1–13).
- 32-entry result tables; 128 Wardrive deduplication addresses.
- Combined Wi-Fi+BLE views use Wi-Fi only. BLE-only tools remain available.
- Link Mode pairs with C5 units: the classic board takes 2.4 GHz, the C5 takes
  5 GHz. See [link-mode.md](link-mode.md).

## Validation

1.3.0 compiled the Touch v1 profile. Physical validation of original Touch
revisions is still experimental; confirm SD, GPS, and touch corners on the
board in front of you before relying on a capture.
