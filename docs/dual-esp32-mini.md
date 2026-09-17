# Original Dual ESP32 Mini (v1 / v2 / v3)

Original **Dual ESP32 Mini** profiles share the C5 Mini's native 128×128
ST7735 UI and five-button navigation, on classic ESP32 silicon (4 MB flash,
`huge_app`, PSRAM disabled, 2.4 GHz only).

Build:

```bash
python3 scripts/build_firmware.py dual-esp32-mini-v1   # or -v2 / -v3
```

Install **Adafruit ST7735 and ST7789 Library** in addition to the Touch
libraries. Select **ESP32 Dev Module** in Arduino IDE and exactly one of
`AWOK_DUAL_ESP32_MINI_V1` / `_V2` / `_V3`.

## Pin map

| Function | GPIO |
| --- | ---: |
| SPI SCK / MISO / MOSI | 18 / 19 / 23 |
| ST7735 CS / DC / Reset | 17 / 16 / 5 |
| Backlight | 32 (active low) |
| Buttons Left / Center / Up / Right / Down | 13 / 34 / 36 / 39 / 35 |
| SD card CS | 4 |
| GPS UART2 RX / TX | 21 / 22 @ 9600 NMEA (selectable on the GPS screen) |
| Battery ADC | unset (`kBatteryAdc = -1`) |

Pin numbers follow ESP32 Marauder `MARAUDER_MINI` (`L_BTN 13`, `C_BTN 34`,
`U_BTN 36`, `R_BTN 39`, `D_BTN 35`) and the FZEasyMarauderFlash Mini flashing
reference. GPIO **34–39 are input-only and have no internal pull-ups**; Center,
Up, Right, and Down rely on the board's external biasing. Left on GPIO13 uses
`INPUT_PULLUP`.

## Runtime limits

Same as the original Touch profiles: 2.4 GHz only, 32-entry tables, 128
Wardrive addresses, Wi-Fi-only combined views. Mini dual-radio admission is
off on classic ESP32 (no verified PSRAM on these display pins).

GPS defaults to **9600** baud on original Mini (C5 Mini stays at 115200). Cycle
baud from the GPS screen if the module is configured differently.

## Validation

1.3.0 compiled the Mini v3 profile. Treat original Mini revisions as
experimental until SD, buttons, GPS, and the ST7735 have been checked on that
hardware. C5 Mini (`dual-c5-mini`) is a separate, confirmed-on-hardware map.
