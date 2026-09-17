#pragma once
// No-op display for the headless orange bridge chip (AWOK_DUAL_C5_BRIDGE).
//
// The bridge runs the full firmware but has no screen -- it is driven entirely
// over BLE and reports through BLE notifications / serial. Every existing
// display.* call in the tools is standard Adafruit_GFX, so a GFX subclass whose
// drawPixel() (and begin()/present()) do nothing gives all of that code a valid
// sink at ~zero RAM cost (no 150 KB canvas). Matches the AwokTouchDisplay API
// the sketch expects (begin(freq), present(bool), setRotation).
#include <Adafruit_GFX.h>

class AwokHeadlessDisplay : public Adafruit_GFX {
 public:
  // Signature matches AwokTouchDisplay(dc, cs, rst); args are ignored.
  AwokHeadlessDisplay(int8_t = -1, int8_t = -1, int8_t = -1)
      : Adafruit_GFX(240, 320) {}

  bool begin(uint32_t = 0) { return true; }
  void present(bool = true) {}
  void setRotation(uint8_t r) { Adafruit_GFX::setRotation(r); }

  // The one pure-virtual GFX hook; everything else (print, fillRect, drawLine,
  // setCursor, setTextColor, ...) is provided by Adafruit_GFX on top of this.
  void drawPixel(int16_t, int16_t, uint16_t) override {}
  void fillScreen(uint16_t) override {}
  // Skip glyph rasterization entirely: print()/printf() would otherwise walk the
  // font bitmap calling drawPixel() for every pixel, burning CPU for no output.
  size_t write(uint8_t) override { return 1; }
};
