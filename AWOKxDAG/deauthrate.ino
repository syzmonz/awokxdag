constexpr int kDeauthRateSamples = 108;
constexpr uint32_t kDeauthRateRedrawMs = 400;
constexpr int kDrTop = kHeaderHeight;
constexpr int kDrGraphTop = 150;
constexpr int kDrGraphBottom = 256;

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
  display.printf("src %d", deauthApSeqCount);

  const int gh = kDrGraphBottom - kDrGraphTop;
  uint16_t scale = drPeak;
  if ((uint16_t)drThreshold > scale) scale = (uint16_t)drThreshold;
  if (scale < 5) scale = 5;
  scale = (uint16_t)(scale + scale / 3);

  const int alarmY = kDrGraphBottom - (int)((long)drThreshold * gh / scale);
  for (int x = 0; x < kScreenWidth; x += 6)
    display.drawFastHLine(x, alarmY, 3, kWarn);

  for (int i = 0; i < drFill; ++i) {
    const int idx = (drHead - drFill + i + kDeauthRateSamples * 2) % kDeauthRateSamples;
    const int v = drRate[idx];
    int h = (int)((long)v * gh / scale);
    if (h > gh) h = gh;
    const int x = i + (kScreenWidth - kDeauthRateSamples) / 2;
    const uint16_t c = (drAlarmArmed && v >= drThreshold) ? kBad : kAccent;
    if (h > 0) display.drawFastVLine(x, kDrGraphBottom - h, h, c);
  }
  display.drawFastHLine(0, kDrGraphBottom, kScreenWidth, kMuted);

  if (alarm) display.drawRect(0, kDrTop, kScreenWidth, kDrGraphBottom - kDrTop, kBad);

  drawThreeButtonFooter(deauthForensicsActive ? "Back" : "Back",
                        drAlarmArmed ? "Watch*" : "Watch", "Clear");
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
