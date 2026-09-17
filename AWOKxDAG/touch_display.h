#pragma once
// Off-screen back buffer for the ILI9341 Touch UI.
//
// Live views redraw on 0.5-1 s timers and each begins with a full-screen
// fillScreen(); drawing straight to the panel makes that clear-then-repaint
// visible as flicker. Render into a PSRAM back buffer instead and blit it to
// the panel once per frame (dirty-gated) so each refresh appears atomically.
//
// Every existing display.* call is standard Adafruit_GFX, so this subclass is
// a drop-in for the raw Adafruit_ILI9341 the sketch used before -- it just adds
// begin()/present(). When there is no PSRAM to hold the ~150 KB buffer (classic
// ESP32 Touch, PSRAM disabled) it falls back to drawing straight to the panel,
// exactly as before (flicker unchanged there; those profiles are experimental).
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <esp_heap_caps.h>

class AwokTouchDisplay : public Adafruit_GFX {
 public:
  AwokTouchDisplay(int8_t dc, int8_t cs, int8_t rst)
      : Adafruit_GFX(240, 320), tft_(&SPI, dc, cs, rst) {}

  bool begin(uint32_t freq) {
    tft_.begin(freq);
    tft_.setRotation(0);  // buffer is raw 240x320; blit is always at rotation 0
    if (!buffer_) {
      buffer_ = static_cast<uint16_t*>(heap_caps_malloc(
          size_t(WIDTH) * HEIGHT * sizeof(uint16_t),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    buffered_ = buffer_ != nullptr;
    if (buffered_) {
      fillScreen(0);
    } else {
      Serial.println("[display] no PSRAM back buffer; drawing direct-to-panel");
      tft_.fillScreen(0);
    }
    return true;
  }

  // Push the back buffer to the panel; no-op unless something was drawn.
  void present(bool = true) {
    if (!buffered_ || !dirty_) return;
    tft_.drawRGBBitmap(0, 0, buffer_, WIDTH, HEIGHT);
    dirty_ = false;
  }

  void setRotation(uint8_t r) {
    Adafruit_GFX::setRotation(r);  // tft stays at 0; the buffer is the canvas
  }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (!buffered_) {
      tft_.drawPixel(x, y, color);
      return;
    }
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    int16_t t;
    switch (rotation) {
      case 1: t = x; x = WIDTH - 1 - y; y = t; break;
      case 2: x = WIDTH - 1 - x; y = HEIGHT - 1 - y; break;
      case 3: t = x; x = y; y = HEIGHT - 1 - t; break;
    }
    buffer_[int32_t(y) * WIDTH + x] = color;
    dirty_ = true;
  }

  void fillScreen(uint16_t color) override {
    if (!buffered_) {
      tft_.fillScreen(color);
      return;
    }
    const uint32_t n = uint32_t(WIDTH) * HEIGHT;
    for (uint32_t i = 0; i < n; ++i) buffer_[i] = color;
    dirty_ = true;
  }

  // Panel-native fast fills when in passthrough; buffered mode loops drawPixel
  // (RAM writes, so per-pixel is fine and keeps rotation handling in one place).
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (!buffered_) {
      tft_.drawFastHLine(x, y, w, color);
      return;
    }
    for (int16_t i = 0; i < w; ++i) drawPixel(x + i, y, color);
  }

  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (!buffered_) {
      tft_.drawFastVLine(x, y, h, color);
      return;
    }
    for (int16_t i = 0; i < h; ++i) drawPixel(x, y + i, color);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                uint16_t color) override {
    if (!buffered_) {
      tft_.fillRect(x, y, w, h, color);
      return;
    }
    for (int16_t j = 0; j < h; ++j) drawFastHLine(x, y + j, w, color);
  }

 private:
  Adafruit_ILI9341 tft_;
  uint16_t* buffer_ = nullptr;
  bool buffered_ = false;
  bool dirty_ = true;
};
