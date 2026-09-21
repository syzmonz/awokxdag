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

int specTotalChannels() {
#ifndef AWOK_CLASSIC_ESP32
  return (specMode == kSpecMode24) ? kSpec24Count : (kSpec24Count + kSpec5Count);
#else
  return kSpec24Count;
#endif
}

uint16_t specThermalColor(uint8_t val) {
  if (val == 0) return 0x0842;           // dark navy (idle)
  if (val < 15) return ILI9341_BLUE;     // low
  if (val < 35) return ILI9341_CYAN;     // light traffic
  if (val < 55) return ILI9341_GREEN;    // moderate
  if (val < 75) return ILI9341_YELLOW;   // busy
  if (val < 90) return ILI9341_RED;      // heavy congestion
  return ILI9341_MAGENTA;                // saturated / flood
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
  const int total = specTotalChannels();
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
                         : (specMode == kSpecModeAll) ? "2.4+5 GHz" : "Locked";
  const int curIdx = specChannelToIndex(specCurrentChannel);
  const uint8_t curDuty = (curIdx >= 0) ? specStats[curIdx].dutyPercent : 0;
  const int8_t curPeak = (curIdx >= 0) ? specStats[curIdx].peakRssi : -127;
  const int8_t curNoise = (curIdx >= 0) ? specStats[curIdx].avgNoise : -95;

  drawHeader("SPECTROGRAM", String(modeLabel) + " · Ch " + String(specCurrentChannel) + " (" + String(curDuty) + "%)");

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

  // Touch 240x320 Layout
  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(5, 46);
  display.printf("Ch %-2u (%s)  Peak: %3d dBm  Noise: %3d dBm",
                 specCurrentChannel,
                 (specCurrentChannel <= 14) ? "2.4G" : "5G",
                 curPeak,
                 curNoise);

  // Upper Bar Chart (Instantaneous Spectrum + Peak Hold)
  constexpr int kChartBaseY = 120;
  constexpr int kChartMaxH = 64;
  const int totalCh = specTotalChannels();

  if (specMode == kSpecMode24) {
    // 13 channels: 16px pitch
    for (int i = 0; i < kSpec24Count; ++i) {
      const int ch = kSpec24Channels[i];
      const int duty = specStats[i].dutyPercent;
      const int barH = max(1, duty * kChartMaxH / 100);
      const int x = 12 + i * 16;

      // Active bar
      display.fillRect(x, kChartBaseY - barH, 12, barH, specThermalColor(duty));

      // Peak hold line
      const int peakH = max(1, static_cast<int>(specPeakHold[i]) * kChartMaxH / 100);
      display.drawFastHLine(x, kChartBaseY - peakH, 12, ILI9341_WHITE);

      // Active channel marker
      if (ch == specCurrentChannel) {
        display.drawRect(x - 1, kChartBaseY - kChartMaxH - 2, 14, kChartMaxH + 4, kAccent);
      }

      // Channel text
      display.setTextColor((ch == specCurrentChannel) ? kAccent : kMuted, kBackground);
      display.setCursor(x + (ch < 10 ? 3 : 0), kChartBaseY + 2);
      display.print(ch);
    }
  } else {
    // All channels (up to 38): 6px pitch
    for (int i = 0; i < totalCh; ++i) {
      const int duty = specStats[i].dutyPercent;
      const int barH = max(1, duty * kChartMaxH / 100);
      const int x = 6 + i * 6;

      display.fillRect(x, kChartBaseY - barH, 4, barH, specThermalColor(duty));

      const int peakH = max(1, static_cast<int>(specPeakHold[i]) * kChartMaxH / 100);
      display.drawFastHLine(x, kChartBaseY - peakH, 4, ILI9341_WHITE);

      if (specIndexToChannel(i) == specCurrentChannel) {
        display.drawFastVLine(x + 2, kChartBaseY - kChartMaxH - 2, 4, kAccent);
      }
    }
  }

  // Divider
  display.drawFastHLine(4, 134, 232, 0x3186);

  // Lower Waterfall Heat Map (26 rows, scrolling downward)
  constexpr int kWaterTopY = 138;
  constexpr int kWaterRowH = 5;
  constexpr int kWaterDisplayRows = 26;

  for (int r = 0; r < kWaterDisplayRows; ++r) {
    const int rowIdx = (waterfallHead - r + kWaterfallHistoryRows) % kWaterfallHistoryRows;
    const int y = kWaterTopY + r * kWaterRowH;

    if (specMode == kSpecMode24) {
      for (int i = 0; i < kSpec24Count; ++i) {
        const uint8_t val = waterfallHistory[rowIdx][i];
        const int x = 12 + i * 16;
        display.fillRect(x, y, 12, kWaterRowH - 1, specThermalColor(val));
      }
    } else {
      for (int i = 0; i < totalCh; ++i) {
        const uint8_t val = waterfallHistory[rowIdx][i];
        const int x = 6 + i * 6;
        display.fillRect(x, y, 4, kWaterRowH - 1, specThermalColor(val));
      }
    }
  }

  drawFooter("Back", lastSpectrogramCsvOk ? "Saved" : "Save");
}

void startSpectrogram() {
  stopActiveTools();
  if (!ensureWifiStation(true)) return;

  specHopIndex = 0;
  specCurrentChannel = (specMode == kSpecModeLock) ? specLockedChannel : specIndexToChannel(0);
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
  specCurrentChannel = (specMode == kSpecModeLock) ? specLockedChannel : specIndexToChannel(0);
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
      specCurrentChannel = specIndexToChannel(specHopIndex);
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
    for (int i = 0; i < total; ++i) {
      if (specPeakHold[i] > 2) specPeakHold[i] -= 2;
      else specPeakHold[i] = 0;
    }
  }

  // Redraw
  if (now - lastSpecDrawMs >= kSpectrogramRedrawMs) {
    lastSpecDrawMs = now;
    drawSpectrogram();
  }
}
