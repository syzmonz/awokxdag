// AWOKxDAG — Dual-Band RF Spectrogram & Waterfall Analyzer
// (compiled as part of the sketch; see awok_common.h)
//
// Continuous passive RF analyzer that measures channel duty cycle, airtime
// saturation, packet density, and noise floor across 2.4 GHz and 5 GHz bands.
// Generates a real-time thermal waterfall spectrogram on device, logs snapshots
// to SD (/awokxdag/spectrogram.csv), and streams $SPEC telemetry over BLE/Serial.

enum SpectrogramMode : uint8_t {
  kSpecMode24 = 0,
  kSpecModeAll = 1,
  kSpecModeLock = 2,
  kSpecMode5 = 3,
};

const uint8_t kSpec24Channels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
constexpr int kSpec24Count = sizeof(kSpec24Channels) / sizeof(kSpec24Channels[0]);

#ifndef AWOK_CLASSIC_ESP32
const uint8_t kSpec5Channels[] = {
    36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108, 112, 116,
    120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};
constexpr int kSpec5Count = sizeof(kSpec5Channels) / sizeof(kSpec5Channels[0]);
#else
const uint8_t kSpec5Channels[] = {0};
constexpr int kSpec5Count = 0;
#endif

constexpr int kMaxSpecBuckets = 38;

SpectrogramChannelStats specStats[kMaxSpecBuckets];
uint8_t specPeakHold[kMaxSpecBuckets] = {0};
uint8_t waterfallHistory[kWaterfallHistoryRows][kMaxSpecBuckets] = {{0}};
int waterfallHead = 0;

SpectrogramMode specMode = kSpecMode24;
uint8_t specLockedChannel = 6;
int specHopIndex = 0;
uint32_t lastSpecHopMs = 0;
uint32_t lastSpecDrawMs = 0;
uint32_t lastPeakDecayMs = 0;
uint32_t specStartMs = 0;
uint8_t specCurrentChannel = 1;

// Per-dwell volatile accumulators
volatile uint16_t dwellFrames = 0;
volatile uint32_t dwellBytes = 0;
volatile int8_t dwellPeakRssi = -127;
volatile int32_t dwellNoiseSum = 0;
volatile uint16_t dwellNoiseSamples = 0;
volatile uint16_t dwellMgmt = 0;
volatile uint16_t dwellCtrl = 0;
volatile uint16_t dwellData = 0;

int specChannelToIndex(uint8_t ch) {
  if (ch >= 1 && ch <= 13) return ch - 1;
#ifndef AWOK_CLASSIC_ESP32
  for (int i = 0; i < kSpec5Count; ++i) {
    if (kSpec5Channels[i] == ch) return kSpec24Count + i;
  }
#endif
  return -1;
}

uint8_t specIndexToChannel(int idx) {
  if (idx >= 0 && idx < kSpec24Count) return kSpec24Channels[idx];
#ifndef AWOK_CLASSIC_ESP32
  if (idx >= kSpec24Count && idx < kSpec24Count + kSpec5Count) {
    return kSpec5Channels[idx - kSpec24Count];
  }
#endif
  return 1;
}

int specBandBase() {
#ifndef AWOK_CLASSIC_ESP32
  return (specMode == kSpecMode5) ? kSpec24Count : 0;
#else
  return 0;
#endif
}

int specTotalChannels() {
#ifndef AWOK_CLASSIC_ESP32
  if (specMode == kSpecMode24) return kSpec24Count;
  if (specMode == kSpecMode5) return kSpec5Count;
  return kSpec24Count + kSpec5Count;
#else
  return kSpec24Count;
#endif
}

uint16_t specThermalColor(uint8_t val) {
  if (val > 100) val = 100;
  static const uint8_t stops[8][3] = {
      { 16,  24, 180},
      {  0,  48, 255},
      {  0, 170, 255},
      {  0, 240, 190},
      { 60, 255,  60},
      {240, 240,   0},
      {255, 140,   0},
      {255,  40,  40}};
  float t = (float)val / 100.0f * 7.0f;
  int i = (int)t;
  if (i > 6) i = 6;
  float f = t - (float)i;
  int r = stops[i][0] + (int)((stops[i + 1][0] - stops[i][0]) * f);
  int g = stops[i][1] + (int)((stops[i + 1][1] - stops[i][1]) * f);
  int b = stops[i][2] + (int)((stops[i + 1][2] - stops[i][2]) * f);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static float specSampleAt(const uint8_t* vals, int n, float fx, float spread) {
  if (n <= 1) return vals ? (float)vals[0] : 0.0f;
  float num = 0.0f, den = 0.0f;
  int lo = (int)(fx - spread) - 1;
  int hi = (int)(fx + spread) + 1;
  if (lo < 0) lo = 0;
  if (hi > n - 1) hi = n - 1;
  for (int i = lo; i <= hi; ++i) {
    float d = ((float)i - fx) / spread;
    if (d < 0.0f) d = -d;
    if (d >= 1.0f) continue;
    float w = 1.0f - d;
    num += w * (float)vals[i];
    den += w;
  }
  return (den > 0.0f) ? (num / den) : 0.0f;
}

void IRAM_ATTR spectrogramCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL)
    return;
  const wifi_promiscuous_pkt_t* packet =
      static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const int length = packet->rx_ctrl.sig_len;
  if (length < 4) return;

  dwellFrames = dwellFrames + 1;
  dwellBytes = dwellBytes + length;

  const int8_t rssi = packet->rx_ctrl.rssi;
  if (rssi > dwellPeakRssi) dwellPeakRssi = rssi;

  const int noise = packet->rx_ctrl.noise_floor;
  if (noise < 0 && noise > -128) {
    dwellNoiseSum = dwellNoiseSum + noise;
    dwellNoiseSamples = dwellNoiseSamples + 1;
  }

  if (type == WIFI_PKT_MGMT) dwellMgmt = dwellMgmt + 1;
  else if (type == WIFI_PKT_CTRL) dwellCtrl = dwellCtrl + 1;
  else dwellData = dwellData + 1;
}

void spectrogramRecordDwell() {
  const int idx = specChannelToIndex(specCurrentChannel);
  if (idx < 0 || idx >= kMaxSpecBuckets) return;

  const uint32_t dwellUs = (millis() - lastSpecHopMs) * 1000UL;
  // Estimate airtime based on preamble overhead + packet payload duration (~18 Mbps nominal)
  uint32_t airtimeUs = (dwellFrames * 120UL) + (dwellBytes * 8UL / 18UL);
  if (dwellUs > 0 && airtimeUs > dwellUs) airtimeUs = dwellUs;
  uint8_t duty = (dwellUs > 0) ? static_cast<uint8_t>((airtimeUs * 100UL) / dwellUs) : 0;
  if (dwellFrames > 0 && duty == 0) duty = 1;
  if (duty > 100) duty = 100;

  const int8_t avgNoise = (dwellNoiseSamples > 0)
                              ? static_cast<int8_t>(dwellNoiseSum / dwellNoiseSamples)
                              : -95;

  specStats[idx].channel = specCurrentChannel;
  specStats[idx].dutyPercent = duty;
  specStats[idx].packetCount = dwellFrames;
  specStats[idx].byteCount = dwellBytes;
  specStats[idx].peakRssi = (dwellFrames > 0) ? dwellPeakRssi : -127;
  specStats[idx].avgNoise = avgNoise;
  specStats[idx].mgmtCount = dwellMgmt;
  specStats[idx].ctrlCount = dwellCtrl;
  specStats[idx].dataCount = dwellData;
  specStats[idx].lastSeenMs = millis();

  if (duty > specPeakHold[idx]) {
    specPeakHold[idx] = duty;
  }

  // Stream telemetry line: $SPEC,ch,dutyPct,pkts,peakRssi,noise,mgmt,ctrl,data
  char telem[96];
  snprintf(telem, sizeof(telem), "$SPEC,%u,%u,%u,%d,%d,%u,%u,%u",
           specCurrentChannel,
           duty,
           dwellFrames,
           specStats[idx].peakRssi,
           avgNoise,
           dwellMgmt,
           dwellCtrl,
           dwellData);
  Serial.println(telem);
#ifdef AWOK_HEADLESS
  bridgeNotifyResult(kSourceSpectrogram, reinterpret_cast<const uint8_t*>(telem), strlen(telem));
#endif
}

void spectrogramPushWaterfallRow() {
  waterfallHead = (waterfallHead + 1) % kWaterfallHistoryRows;
  int total = kSpec24Count;
#ifndef AWOK_CLASSIC_ESP32
  total = kSpec24Count + kSpec5Count;
#endif
  for (int i = 0; i < total; ++i) {
    waterfallHistory[waterfallHead][i] = specStats[i].dutyPercent;
  }
}

bool exportSpectrogramToSd() {
  if (!ensureSdCard()) return false;
  const String temporaryPath = String(kSpectrogramCsvPath) + ".tmp";
  SD.remove(temporaryPath.c_str());
  File file = SD.open(temporaryPath.c_str(), FILE_WRITE);
  if (!file) {
    sdReady = false;
    return false;
  }
  file.println(
      "uptime_ms,channel,band,duty_percent,packets,bytes,peak_rssi,avg_noise,"
      "mgmt_frames,ctrl_frames,data_frames,latitude,longitude,altitude_m");
  const uint32_t uptime = millis();
  const String location = gpsCsvFields();
  const int total = specTotalChannels();
  for (int i = 0; i < total; ++i) {
    const uint8_t ch = specIndexToChannel(i);
    const char* band = (ch <= 14) ? "2.4GHz" : "5GHz";
    file.print(uptime);
    file.print(',');
    file.print(ch);
    file.print(',');
    file.print(band);
    file.print(',');
    file.print(specStats[i].dutyPercent);
    file.print(',');
    file.print(specStats[i].packetCount);
    file.print(',');
    file.print(specStats[i].byteCount);
    file.print(',');
    file.print(specStats[i].peakRssi);
    file.print(',');
    file.print(specStats[i].avgNoise);
    file.print(',');
    file.print(specStats[i].mgmtCount);
    file.print(',');
    file.print(specStats[i].ctrlCount);
    file.print(',');
    file.print(specStats[i].dataCount);
    file.println(location);
  }
  file.flush();
  const bool ok = file.getWriteError() == 0;
  file.close();
  if (!ok) {
    SD.remove(temporaryPath.c_str());
    return false;
  }
  SD.remove(kSpectrogramCsvPath);
  if (!SD.rename(temporaryPath.c_str(), kSpectrogramCsvPath)) return false;
  Serial.printf("[spectrogram] wrote %d channel row(s) to %s\n", total, kSpectrogramCsvPath);
  return true;
}

void drawSpectrogram() {
  currentView = View::kSpectrogram;
  display.fillScreen(kBackground);

  const char* modeLabel = (specMode == kSpecMode24) ? "2.4 GHz"
                         : (specMode == kSpecModeAll) ? "2.4+5 GHz"
                         : (specMode == kSpecMode5) ? "5 GHz" : "Locked";
  const int curIdx = specChannelToIndex(specCurrentChannel);
  const uint8_t curDuty = (curIdx >= 0) ? specStats[curIdx].dutyPercent : 0;
  const int8_t curPeak = (curIdx >= 0) ? specStats[curIdx].peakRssi : -127;
  const int8_t curNoise = (curIdx >= 0) ? specStats[curIdx].avgNoise : -95;

  drawHeader("SPECTROGRAM", String(modeLabel) + " \xC2\xB7 Ch " + String(specCurrentChannel) + " (" + String(curDuty) + "%)");

  const char* bandTag = (specMode == kSpecMode24) ? "2.4"
                       : (specMode == kSpecModeAll) ? "2+5"
                       : (specMode == kSpecMode5) ? "5G" : "LCK";
  display.drawRoundRect(180, 6, 54, 28, 5, kAccent);
  display.setTextSize(1);
  display.setTextColor(kAccent, kBackground);
  display.setCursor(180 + (54 - (int)strlen(bandTag) * 6) / 2, 16);
  display.print(bandTag);

#ifdef AWOK_MINI_DISPLAY
  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(2, 46);
  display.printf("Mode: %-7s Ch: %-2u", modeLabel, specCurrentChannel);
  display.setCursor(2, 58);
  display.printf("Duty: %-3u%%  Peak: %d", curDuty, curPeak);
  display.setCursor(2, 70);
  display.printf("Noise: %-3d  SNR: %d", curNoise, (curPeak > -127 ? curPeak - curNoise : 0));

  const int miniTotal = min(specTotalChannels(), 13);
  const int barBaseY = 110;
  for (int i = 0; i < miniTotal; ++i) {
    const int val = specStats[i].dutyPercent;
    const int h = max(1, val * 32 / 100);
    const int x = 4 + i * 9;
    display.fillRect(x, barBaseY - h, 7, h, specThermalColor(val));
  }
  drawFooter("Back", lastSpectrogramCsvOk ? "Saved" : "Save");
  return;
#endif

  const int total = specTotalChannels();
  const int base = specBandBase();

  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(5, 46);
  display.printf("Ch %-2u (%s)  Peak: %3d dBm  Noise: %3d dBm",
                 specCurrentChannel,
                 (specCurrentChannel <= 14) ? "2.4G" : "5G",
                 curPeak,
                 curNoise);

  const int bx0 = 8;
  const int bw = 224;
  const int sBaseY = 120;
  const int sMaxH = 62;
  const float spread = (specMode == kSpecMode24) ? 1.5f : 0.95f;
  const float span = (total > 1) ? (float)(total - 1) : 1.0f;

  static uint8_t wfCurve[101];
  static bool wfCurveInit = false;
  if (!wfCurveInit) {
    for (int i = 0; i <= 100; ++i) {
      wfCurve[i] = (uint8_t)(powf((float)i / 100.0f, 0.72f) * 100.0f + 0.5f);
    }
    wfCurveInit = true;
  }

  uint8_t dutyVals[kMaxSpecBuckets];
  uint8_t peakVals[kMaxSpecBuckets];
  for (int i = 0; i < total; ++i) {
    dutyVals[i] = specStats[base + i].dutyPercent;
    peakVals[i] = specPeakHold[base + i];
  }

  for (int px = 0; px < bw; ++px) {
    const float fx = (float)px * span / (float)(bw - 1);
    const float iv = specSampleAt(dutyVals, total, fx, spread);
    int h = (int)(iv * (float)sMaxH / 100.0f + 0.5f);
    if (h < 1 && iv > 0.5f) h = 1;
    if (h > sMaxH) h = sMaxH;
    const int x = bx0 + px;
    display.drawFastVLine(x, sBaseY - sMaxH, sMaxH, specThermalColor(0));
    if (h > 0) display.drawFastVLine(x, sBaseY - h, h, specThermalColor((uint8_t)(iv + 0.5f)));
    const float pv = specSampleAt(peakVals, total, fx, spread);
    int ph = (int)(pv * (float)sMaxH / 100.0f + 0.5f);
    if (ph > sMaxH) ph = sMaxH;
    if (pv > 0.5f) display.drawPixel(x, sBaseY - ph, ILI9341_WHITE);
  }

  display.drawFastHLine(bx0, sBaseY, bw, kMuted);

  const int markIdx = specChannelToIndex(specCurrentChannel) - base;
  if (markIdx >= 0 && markIdx < total) {
    const int mx = bx0 + (int)((float)markIdx * (float)(bw - 1) / span + 0.5f);
    display.drawFastVLine(mx, sBaseY - sMaxH - 2, sMaxH + 2, ILI9341_WHITE);
  }

  if (specMode == kSpecMode24) {
    const int labels[3] = {1, 6, 11};
    display.setTextColor(kMuted, kBackground);
    for (int li = 0; li < 3; ++li) {
      const int idx = labels[li] - 1;
      const int lx = bx0 + (int)((float)idx * (float)(bw - 1) / span + 0.5f);
      display.setCursor(lx - (labels[li] < 10 ? 2 : 5), sBaseY + 3);
      display.print(labels[li]);
    }
  }

  const int wTop = 138;
  const int wRowH = 5;
  int wRows = kWaterfallHistoryRows;
  const int wMaxRows = (kFooterTop - wTop) / wRowH;
  if (wRows > wMaxRows) wRows = wMaxRows;

  for (int r = 0; r < wRows; ++r) {
    const int rowIdx = (waterfallHead - r + kWaterfallHistoryRows) % kWaterfallHistoryRows;
    const uint8_t* rowVals = &waterfallHistory[rowIdx][base];
    const int y = wTop + r * wRowH;
    for (int px = 0; px < bw; ++px) {
      const float fx = (float)px * span / (float)(bw - 1);
      const float iv = specSampleAt(rowVals, total, fx, spread);
      int wv = (int)(iv + 0.5f);
      if (wv < 0) wv = 0;
      if (wv > 100) wv = 100;
      display.drawFastVLine(bx0 + px, y, wRowH, specThermalColor(wfCurve[wv]));
    }
  }

  drawFourButtonFooter("Back", "Ch-", "Ch+", "Hop");
}

static void specResetDwell() {
  dwellFrames = 0;
  dwellBytes = 0;
  dwellPeakRssi = -127;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  dwellMgmt = 0;
  dwellCtrl = 0;
  dwellData = 0;
}

int spectrogramFullChannelCount() {
#ifndef AWOK_CLASSIC_ESP32
  return kSpec24Count + kSpec5Count;
#else
  return kSpec24Count;
#endif
}

void spectrogramLockToIndex(int idx) {
  const int total = spectrogramFullChannelCount();
  if (idx < 0) idx = 0;
  if (idx >= total) idx = total - 1;
  specMode = kSpecModeLock;
  specLockedChannel = specIndexToChannel(idx);
  specCurrentChannel = specLockedChannel;
  esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
  lastSpecHopMs = millis();
  specResetDwell();
  drawSpectrogram();
}

void spectrogramLockStep(int dir) {
  const int total = spectrogramFullChannelCount();
  int idx = specChannelToIndex(specLockedChannel);
  if (idx < 0) idx = 0;
  idx = (idx + dir + total) % total;
  spectrogramLockToIndex(idx);
}

void spectrogramToggleHop() {
  if (specMode == kSpecModeLock) {
#ifndef AWOK_CLASSIC_ESP32
    specMode = (specLockedChannel <= 14) ? kSpecMode24 : kSpecMode5;
#else
    specMode = kSpecMode24;
#endif
  }
  specHopIndex = 0;
  specCurrentChannel = specIndexToChannel(specBandBase());
  esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
  lastSpecHopMs = millis();
  specResetDwell();
  drawSpectrogram();
}

void spectrogramCycleBand() {
#ifndef AWOK_CLASSIC_ESP32
  if (specMode == kSpecMode24) specMode = kSpecModeAll;
  else if (specMode == kSpecModeAll) specMode = kSpecMode5;
  else specMode = kSpecMode24;
#else
  specMode = kSpecMode24;
#endif
  specHopIndex = 0;
  specCurrentChannel = specIndexToChannel(specBandBase());
  esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
  lastSpecHopMs = millis();
  specResetDwell();
  drawSpectrogram();
}

void startSpectrogram() {
  stopActiveTools();
  if (!ensureWifiStation(true)) return;

  specHopIndex = 0;
  specCurrentChannel = (specMode == kSpecModeLock) ? specLockedChannel : specIndexToChannel(specBandBase());
  lastSpecHopMs = millis();
  lastSpecDrawMs = 0;
  lastPeakDecayMs = millis();
  lastSpectrogramCsvOk = false;
  waterfallHead = 0;

  for (int i = 0; i < kMaxSpecBuckets; ++i) {
    specStats[i] = SpectrogramChannelStats();
    specStats[i].channel = specIndexToChannel(i);
    specPeakHold[i] = 0;
    for (int r = 0; r < kWaterfallHistoryRows; ++r) {
      waterfallHistory[r][i] = 0;
    }
  }

  dwellFrames = 0;
  dwellBytes = 0;
  dwellPeakRssi = -127;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  dwellMgmt = 0;
  dwellCtrl = 0;
  dwellData = 0;

  esp_wifi_set_promiscuous(false);
  esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous_rx_cb(spectrogramCallback);
  esp_wifi_set_promiscuous(true);

  spectrogramActive = true;
  specStartMs = millis();
  Serial.printf("[spectrogram] started on ch %u (mode %d)\n", specCurrentChannel, specMode);
  drawSpectrogram();
}

void stopSpectrogram() {
  spectrogramActive = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  lastSpectrogramCsvOk = exportSpectrogramToSd();
  Serial.println("[spectrogram] stopped");
}

void cycleSpectrogramMode() {
  if (specMode == kSpecMode24) {
#ifndef AWOK_CLASSIC_ESP32
    specMode = kSpecModeAll;
#else
    specMode = kSpecModeLock;
    specLockedChannel = specCurrentChannel;
#endif
  } else if (specMode == kSpecModeAll) {
    specMode = kSpecModeLock;
    specLockedChannel = specCurrentChannel;
  } else {
    specMode = kSpecMode24;
  }
  specHopIndex = 0;
  specCurrentChannel = (specMode == kSpecModeLock) ? specLockedChannel : specIndexToChannel(specBandBase());
  esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
  lastSpecHopMs = millis();
  dwellFrames = 0;
  dwellBytes = 0;
  dwellPeakRssi = -127;
  dwellNoiseSum = 0;
  dwellNoiseSamples = 0;
  dwellMgmt = 0;
  dwellCtrl = 0;
  dwellData = 0;
  drawSpectrogram();
}

void handleSpectrogramBarTouch(int touchedIdx) {
  if (touchedIdx >= 0 && touchedIdx < kSpec24Count) {
    specMode = kSpecModeLock;
    specLockedChannel = kSpec24Channels[touchedIdx];
    specCurrentChannel = specLockedChannel;
    esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
    lastSpecHopMs = millis();
    dwellFrames = 0;
    dwellBytes = 0;
    dwellPeakRssi = -127;
    dwellNoiseSum = 0;
    dwellNoiseSamples = 0;
    dwellMgmt = 0;
    dwellCtrl = 0;
    dwellData = 0;
    drawSpectrogram();
  }
}

void updateSpectrogram() {
  if (!spectrogramActive || currentView != View::kSpectrogram) return;

  const uint32_t now = millis();

  // Channel dwell completed
  if (now - lastSpecHopMs >= kSpectrogramDwellMs) {
    spectrogramRecordDwell();

    if (specMode == kSpecModeLock) {
      spectrogramPushWaterfallRow();
      lastSpecHopMs = now;
      dwellFrames = 0;
      dwellBytes = 0;
      dwellPeakRssi = -127;
      dwellNoiseSum = 0;
      dwellNoiseSamples = 0;
      dwellMgmt = 0;
      dwellCtrl = 0;
      dwellData = 0;
    } else {
      const int total = specTotalChannels();
      specHopIndex = (specHopIndex + 1) % total;
      if (specHopIndex == 0) {
        spectrogramPushWaterfallRow();
      }
      specCurrentChannel = specIndexToChannel(specBandBase() + specHopIndex);
      esp_wifi_set_channel(specCurrentChannel, WIFI_SECOND_CHAN_NONE);
      lastSpecHopMs = now;
      dwellFrames = 0;
      dwellBytes = 0;
      dwellPeakRssi = -127;
      dwellNoiseSum = 0;
      dwellNoiseSamples = 0;
      dwellMgmt = 0;
      dwellCtrl = 0;
      dwellData = 0;
    }
  }

  // Peak decay every 400ms
  if (now - lastPeakDecayMs >= 400) {
    lastPeakDecayMs = now;
    const int total = specTotalChannels();
    const int base = specBandBase();
    for (int i = 0; i < total; ++i) {
      if (specPeakHold[base + i] > 2) specPeakHold[base + i] -= 2;
      else specPeakHold[base + i] = 0;
    }
  }

  // Redraw
  if (now - lastSpecDrawMs >= kSpectrogramRedrawMs) {
    lastSpecDrawMs = now;
    drawSpectrogram();
  }
}
