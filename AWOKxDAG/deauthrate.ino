constexpr int kDeauthRateSamples = 108;
constexpr uint32_t kDeauthRateRedrawMs = 400;
constexpr int kDrTop = kHeaderHeight;
constexpr int kDrGraphTop = 150;
constexpr int kDrGraphBottom = 256;
constexpr uint16_t kDrFillCold = 0x0A6B;
constexpr uint16_t kDrFillHot = 0x3041;
constexpr uint16_t kDrMarker = 0xFC00;

static uint8_t drRate[kDeauthRateSamples];
static int drFill = 0;
static int drHead = 0;
static uint32_t drLastSampleMs = 0;
static uint32_t drLastTotal = 0;
static uint16_t drCur = 0;
static uint16_t drPeak = 0;
static int drThreshold = 20;
static bool drAlarmArmed = true;
static uint32_t lastDrDrawMs = 0;

void startDeauthRate() {
  startDeauthForensics();
  currentView = View::kDeauthRate;
  drFill = 0;
  drHead = 0;
  drCur = 0;
  drPeak = 0;
  drLastTotal = deauthTotalFrames;
  drLastSampleMs = millis();
  lastDrDrawMs = 0;
  drawDeauthRate();
}

static void drReset() {
  drFill = 0;
  drHead = 0;
  drCur = 0;
  drPeak = 0;
  drLastTotal = deauthTotalFrames;
}

void drawDeauthRate() {
  currentView = View::kDeauthRate;
  display.fillScreen(kBackground);

  const bool alarm = drAlarmArmed && drCur >= (uint16_t)drThreshold;
  drawHeader("DEAUTH DET", deauthForensicsActive ? "watching" : "idle");

  display.setTextSize(3);
  display.setTextColor(alarm ? kBad : kAccent, kBackground);
  display.setCursor(10, 60);
  display.printf("%u", drCur);
  display.setTextSize(1);
  display.setTextColor(kMuted, kBackground);
  display.setCursor(10, 92);
  display.print("deauth/s");

  display.setTextColor(kMuted, kBackground);
  display.setCursor(132, 64);
  display.printf("peak %u", drPeak);
  display.setCursor(132, 78);
  display.printf("total %lu", (unsigned long)deauthTotalFrames);
  display.setCursor(132, 92);
  if (deauthEventCount > 0 && (bool)deauthEvents) {
    const uint8_t* s = deauthEvents[0].sourceMac;
    display.printf("src %02X:%02X:%02X", s[3], s[4], s[5]);
  } else {
    display.print("src --");
  }

  const int gh = kDrGraphBottom - kDrGraphTop;
  uint16_t scale = drPeak;
  if ((uint16_t)drThreshold > scale) scale = (uint16_t)drThreshold;
  if (scale < 5) scale = 5;
  scale = (uint16_t)(scale + scale / 3);

  const uint16_t fillCol = alarm ? kDrFillHot : kDrFillCold;
  if (drFill >= 2) {
    for (int x = 0; x < kScreenWidth; ++x) {
      const float p = (float)x / (float)(kScreenWidth - 1) * (float)(drFill - 1);
      int i0 = (int)p;
      int i1 = (i0 + 1 < drFill) ? i0 + 1 : i0;
      const float frac = p - (float)i0;
      const int idx0 = (drHead - drFill + i0 + kDeauthRateSamples * 2) % kDeauthRateSamples;
      const int idx1 = (drHead - drFill + i1 + kDeauthRateSamples * 2) % kDeauthRateSamples;
      const float v = (float)drRate[idx0] * (1.0f - frac) + (float)drRate[idx1] * frac;
      int h = (int)((long)(v + 0.5f) * gh / scale);
      if (h > gh) h = gh;
      if (h < 0) h = 0;
      const int top = kDrGraphBottom - h;
      if (h > 0) display.drawFastVLine(x, top, h, fillCol);
      const uint16_t lc = (drAlarmArmed && v >= (float)drThreshold) ? kBad : kAccent;
      display.drawFastVLine(x, top, h > 1 ? 2 : 1, lc);
    }
  }

  const int alarmY = kDrGraphBottom - (int)((long)drThreshold * gh / scale);
  for (int x = 0; x < kScreenWidth; x += 6)
    display.drawFastHLine(x, alarmY, 3, kWarn);
  display.setTextSize(1);
  display.setTextColor(kBad, kBackground);
  display.setCursor(2, (alarmY - 10 > kDrGraphTop) ? alarmY - 10 : alarmY + 2);
  display.print("alarm");

  display.drawFastHLine(0, kDrGraphBottom, kScreenWidth, kMuted);

  if (drFill > 0) {
    const int lastIdx = (drHead - 1 + kDeauthRateSamples) % kDeauthRateSamples;
    int hv = (int)((long)drRate[lastIdx] * gh / scale);
    if (hv > gh) hv = gh;
    display.fillCircle(kScreenWidth - 4, kDrGraphBottom - hv, 3, kDrMarker);
  }

  if (alarm) display.drawRect(0, kDrTop, kScreenWidth, kDrGraphBottom - kDrTop, kBad);

  drawThreeButtonFooter("Back", drAlarmArmed ? "Watch*" : "Watch", "Clear");
}

void updateDeauthRate() {
  if (currentView != View::kDeauthRate || !deauthForensicsActive) return;
  const uint32_t now = millis();

  DeauthHit hit;
  while (deauthQueue.pop(hit)) deauthProcessHit(hit);

  if (now - lastDeauthForensicsHopMs >= kDeauthForensicsHopMs) {
    lastDeauthForensicsHopMs = now;
    deauthForensicsHopIndex = (deauthForensicsHopIndex + 1) % kDeauthHopChannelCount;
    esp_wifi_set_channel(kDeauthHopChannels[deauthForensicsHopIndex], WIFI_SECOND_CHAN_NONE);
  }

  if (now - drLastSampleMs >= 1000) {
    drLastSampleMs = now;
    const uint32_t total = deauthTotalFrames;
    uint32_t delta = total - drLastTotal;
    drLastTotal = total;
    if (delta > 255) delta = 255;
    drCur = (uint16_t)delta;
    if (drCur > drPeak) drPeak = drCur;
    drRate[drHead] = (uint8_t)delta;
    drHead = (drHead + 1) % kDeauthRateSamples;
    if (drFill < kDeauthRateSamples) ++drFill;
  }

  if (now - lastDrDrawMs >= kDeauthRateRedrawMs) {
    lastDrDrawMs = now;
    drawDeauthRate();
  }
}

void deauthRateTouch(int x) {
  if (x < kScreenWidth / 3) {
    stopDeauthForensics();
    drawReconMenu();
  } else if (x < (kScreenWidth * 2) / 3) {
    drAlarmArmed = !drAlarmArmed;
    drawDeauthRate();
  } else {
    drReset();
    drawDeauthRate();
  }
}
