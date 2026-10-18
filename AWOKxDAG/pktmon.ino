// AWOKxDAG — packet monitor (compiled as part of the sketch; see awok_common.h)
//
// Promiscuous capture of every 802.11 frame on the hopped channels, written to
// a pcap on SD with a live management/data/control breakdown. Reuses the pcap
// helpers and capture ring buffer from the handshake module (they never run at
// the same time).

constexpr char kPktmonPcapPath[] = "/awokxdag/pktmon.pcap";
constexpr uint32_t kPktmonHopIntervalMs = 300;
constexpr uint32_t kPktmonRedrawMs = 500;

// pktmonActive lives in the main sketch's global block (defined before the
// input tab, which reads it in the serial guard).
File pktmonFile;
bool pktmonFileOpen = false;
uint32_t pktMgmt = 0;
uint32_t pktData = 0;
uint32_t pktCtrl = 0;
uint32_t pktTotal = 0;
uint32_t pktWritten = 0;
int pktmonHopIndex = 0;
uint32_t lastPktmonHopMs = 0;
uint32_t lastPktmonDrawMs = 0;
uint32_t pktmonStartMs = 0;

static bool pktmonGraph = false;
static int pktGraphChIdx = 0;
static uint32_t lastPktRowMs = 0;
static uint32_t lastPktSliceTotal = 0;
static uint32_t pktPeakSlice = 1;
static uint32_t pktGraphBaseTotal = 0;
volatile int8_t pktSlicePeakRssi = -127;
constexpr int kPktGraphRows = 160;
constexpr int kPktGraphCenter = 120;
constexpr uint32_t kPktGraphRowMs = 80;
constexpr uint32_t kPktGraphRedrawMs = 100;
constexpr uint32_t kPktFloorSlice = 12;
static uint8_t pktRowLevel[kPktGraphRows] = {0};
static int pktRowHead = -1;
static uint8_t pktKernel[kPktGraphCenter + 1];
static uint16_t pktPalette[101];
static bool pktLutReady = false;

// Runs in the Wi-Fi task: tally by frame type and enqueue a copy for the loop.
void pktmonCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  // WIFI_PKT_MISC supplies receive metadata only, with no frame payload.
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA && type != WIFI_PKT_CTRL)
    return;
  const wifi_promiscuous_pkt_t* packet =
      static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const int length = packet->rx_ctrl.sig_len;
  if (length < 4) return;
  const int8_t rssi = packet->rx_ctrl.rssi;
  if (rssi > pktSlicePeakRssi) pktSlicePeakRssi = rssi;
  if (type == WIFI_PKT_MGMT) {
    ++pktMgmt;
  } else if (type == WIFI_PKT_DATA) {
    ++pktData;
  } else {
    ++pktCtrl;
  }
  ++pktTotal;

  const int next = (captureHead + 1) % kCaptureQueueSlots;
  if (next == captureTail) return;  // queue full: drop the payload, keep count
  CaptureFrame& slot = captureQueue[captureHead];
  const int copyLen = length > kCaptureSlotBytes ? kCaptureSlotBytes : length;
  memcpy(slot.data, packet->payload, copyLen);
  slot.len = static_cast<uint16_t>(copyLen);
  slot.origLen = static_cast<uint16_t>(length);
  const uint32_t nowUs = micros();
  slot.tsSec = nowUs / 1000000UL;
  slot.tsUsec = nowUs % 1000000UL;
  slot.isEapol = false;
  captureHead = next;
}

bool openPktmonPcap() {
  if (!ensureSdCard()) return false;
  SD.remove(kPktmonPcapPath);
  pktmonFile = SD.open(kPktmonPcapPath, FILE_WRITE);
  if (!pktmonFile) {
    sdReady = false;
    return false;
  }
  writePcapLe32(pktmonFile, 0xA1B2C3D4);
  writePcapLe16(pktmonFile, 2);
  writePcapLe16(pktmonFile, 4);
  writePcapLe32(pktmonFile, 0);
  writePcapLe32(pktmonFile, 0);
  writePcapLe32(pktmonFile, kCaptureSlotBytes);
  writePcapLe32(pktmonFile, 105);  // LINKTYPE_IEEE802_11
  pktmonFile.flush();
  pktmonFileOpen = pktmonFile.getWriteError() == 0;
  return pktmonFileOpen;
}

void writePktmonFrame(const CaptureFrame& frame) {
  if (!pktmonFileOpen) return;
  writePcapLe32(pktmonFile, frame.tsSec);
  writePcapLe32(pktmonFile, frame.tsUsec);
  writePcapLe32(pktmonFile, frame.len);
  writePcapLe32(pktmonFile, frame.origLen);
  pktmonFile.write(frame.data, frame.len);
  ++pktWritten;
}

static void pktBuildLut() {
  for (int d = 0; d <= kPktGraphCenter; ++d) {
    float sharp = expf(-(float)(d * d) / (2.0f * 4.5f * 4.5f));
    float tail = expf(-(float)d / 22.0f);
    float v = 0.90f * sharp + 0.18f * tail;
    if (v > 1.0f) v = 1.0f;
    pktKernel[d] = (uint8_t)(v * 255.0f);
  }
  static const uint8_t stops[6][3] = {
      {2, 4, 24}, {20, 30, 150}, {40, 40, 210},
      {110, 30, 190}, {230, 40, 50}, {255, 255, 255}};
  for (int i = 0; i <= 100; ++i) {
    float t = (float)i / 100.0f * 5.0f;
    int s = (int)t;
    if (s > 4) s = 4;
    float f = t - s;
    int r = stops[s][0] + (int)((stops[s + 1][0] - stops[s][0]) * f);
    int g = stops[s][1] + (int)((stops[s + 1][1] - stops[s][1]) * f);
    int b = stops[s][2] + (int)((stops[s + 1][2] - stops[s][2]) * f);
    pktPalette[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }
  pktLutReady = true;
}

static void pktResetGraph() {
  pktRowHead = -1;
  lastPktSliceTotal = pktTotal;
  pktGraphBaseTotal = pktTotal;
  lastPktRowMs = millis();
  pktSlicePeakRssi = -127;
  for (int i = 0; i < kPktGraphRows; ++i) pktRowLevel[i] = 0;
}

static void pktPushRow() {
  const uint32_t slice = pktTotal - lastPktSliceTotal;
  lastPktSliceTotal = pktTotal;
  if (slice > pktPeakSlice) pktPeakSlice = slice;
  const uint32_t den = pktPeakSlice > kPktFloorSlice ? pktPeakSlice : kPktFloorSlice;
  int lvl = (int)((slice * 100UL) / den);
  if (lvl < 0) lvl = 0;
  if (lvl > 100) lvl = 100;
  pktRowHead = (pktRowHead + 1) % kPktGraphRows;
  pktRowLevel[pktRowHead] = (uint8_t)lvl;
  pktSlicePeakRssi = -127;
}

void drawPacketMonGraph() {
  currentView = View::kPacketMon;
  if (!pktLutReady) pktBuildLut();
  display.fillScreen(kBackground);
  const uint8_t gch = kDeauthHopChannels[pktGraphChIdx];
  drawHeader("PACKET MONITOR", "live graph");
  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 44);
  display.printf("Ch:%d %sG   Pkt:%lu   Pk:%ddBm",
                 gch, bandLabel(gch),
                 static_cast<unsigned long>(pktTotal - pktGraphBaseTotal),
                 pktSlicePeakRssi > -127 ? pktSlicePeakRssi : 0);

  const int traceTop = 56, traceBot = 118, wfTop = 126, wfBot = kFooterTop;
  const int newest = pktRowHead >= 0 ? pktRowLevel[pktRowHead] : 0;
  display.fillRect(0, traceTop, kScreenWidth, traceBot - traceTop, pktPalette[0]);
  for (int x = 0; x < kScreenWidth; ++x) {
    const int d = x >= kPktGraphCenter ? x - kPktGraphCenter : kPktGraphCenter - x;
    const int mag = newest * pktKernel[d] / 255;
    const int h = mag * (traceBot - traceTop) / 100;
    if (h > 0) display.drawFastVLine(x, traceBot - h, h, pktPalette[mag]);
  }
  display.drawFastHLine(0, traceBot + 2, kScreenWidth, kMuted);

  const int rows = wfBot - wfTop;
  for (int row = 0; row < rows; ++row) {
    const int y = wfTop + row;
    const int lvl = pktRowHead >= 0
        ? pktRowLevel[((pktRowHead - row) % kPktGraphRows + kPktGraphRows) % kPktGraphRows]
        : 0;
    for (int x = 0; x < kScreenWidth; ++x) {
      const int d = x >= kPktGraphCenter ? x - kPktGraphCenter : kPktGraphCenter - x;
      const int mag = lvl * pktKernel[d] / 255;
      display.drawPixel(x, y, pktPalette[mag]);
    }
  }
  drawFourButtonFooter("Home", "Ch-", "Ch+", "Stats");
}

bool pktmonGraphActive() { return pktmonGraph; }

void pktmonToggleGraph() {
  pktmonGraph = !pktmonGraph;
  if (pktmonGraph) {
    pktGraphChIdx = pktmonHopIndex;
    esp_wifi_set_channel(kDeauthHopChannels[pktGraphChIdx], WIFI_SECOND_CHAN_NONE);
    pktResetGraph();
    lastPktmonDrawMs = 0;
    drawPacketMonGraph();
  } else {
    lastPktmonHopMs = millis();
    lastPktmonDrawMs = 0;
    drawPacketMon();
  }
}

void pktmonGraphStep(int dir) {
  pktGraphChIdx = (pktGraphChIdx + dir + kDeauthHopChannelCount) % kDeauthHopChannelCount;
  esp_wifi_set_channel(kDeauthHopChannels[pktGraphChIdx], WIFI_SECOND_CHAN_NONE);
  pktResetGraph();
  drawPacketMonGraph();
}

void drawPacketMon() {
  currentView = View::kPacketMon;
  display.fillScreen(kBackground);
  drawHeader("PACKET MONITOR",
             pktmonFileOpen ? "capturing to pktmon.pcap" : "live counts only");
  display.setTextSize(2);
  display.setTextColor(kAccent, kBackground);
  display.setCursor(6, 52);
  display.printf("%lu pkts", static_cast<unsigned long>(pktTotal));

  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 90);
  display.printf("Mgmt: %lu", static_cast<unsigned long>(pktMgmt));
  display.setCursor(6, 104);
  display.printf("Data: %lu", static_cast<unsigned long>(pktData));
  display.setCursor(6, 118);
  display.printf("Ctrl: %lu", static_cast<unsigned long>(pktCtrl));

  const uint32_t elapsed = (millis() - pktmonStartMs) / 1000;
  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, 138);
  display.printf("Rate: %lu pkt/s",
                 static_cast<unsigned long>(elapsed ? pktTotal / elapsed : 0));
  display.setCursor(6, 152);
  display.printf("Channel %d %sG   %lus",
                 kDeauthHopChannels[pktmonHopIndex],
                 bandLabel(kDeauthHopChannels[pktmonHopIndex]),
                 static_cast<unsigned long>(elapsed));

  display.drawFastHLine(6, 172, 228, kPanel);
  display.setTextColor(pktmonFileOpen ? kAccent : kWarn, kBackground);
  display.setCursor(6, 182);
  display.printf("Written: %lu frame(s)",
                 static_cast<unsigned long>(pktWritten));
  display.setCursor(6, 196);
  display.print(pktmonFileOpen ? "SD: pktmon.pcap (open in Wireshark)"
                               : "SD unavailable; counts only");
  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, 216);
  display.print("Hops 2.4/5 GHz; drops payload if the");
  display.setCursor(6, 228);
  display.print("SD write cannot keep up (counts stay).");
  drawFooter("Home", "Graph");
}

void startPacketMon() {
  pktMgmt = 0;
  pktData = 0;
  pktCtrl = 0;
  pktTotal = 0;
  pktWritten = 0;
  captureHead = 0;
  captureTail = 0;
  pktmonHopIndex = 0;
  lastPktmonHopMs = millis();
  lastPktmonDrawMs = 0;
  pktmonStartMs = millis();
  pktmonGraph = false;
  pktSlicePeakRssi = -127;
  pktPeakSlice = 1;
  pktResetGraph();
  signalMonitorActive = false;

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&pktmonCallback);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(kDeauthHopChannels[0], WIFI_SECOND_CHAN_NONE);

  pktmonFileOpen = openPktmonPcap();
  pktmonActive = true;
  Serial.println("[pktmon] started");
  drawPacketMon();
}

void stopPacketMon() {
  pktmonActive = false;
  esp_wifi_set_promiscuous(false);
  if (pktmonFileOpen) {
    pktmonFile.flush();
    pktmonFile.close();
    pktmonFileOpen = false;
  }
  WiFi.mode(WIFI_MODE_STA);
  WiFi.disconnect(true, false);
  Serial.printf("[pktmon] stopped; %lu captured, %lu written\n",
                static_cast<unsigned long>(pktTotal),
                static_cast<unsigned long>(pktWritten));
}

void updatePacketMon() {
  if (!pktmonActive || currentView != View::kPacketMon) return;
  while (captureTail != captureHead) {
    writePktmonFrame(captureQueue[captureTail]);
    captureTail = (captureTail + 1) % kCaptureQueueSlots;
  }
  const uint32_t now = millis();
  if (pktmonGraph) {
    if (now - lastPktRowMs >= kPktGraphRowMs) {
      lastPktRowMs = now;
      pktPushRow();
    }
    if (now - lastPktmonDrawMs >= kPktGraphRedrawMs) {
      lastPktmonDrawMs = now;
      if (pktmonFileOpen) pktmonFile.flush();
      drawPacketMonGraph();
    }
    return;
  }
  if (now - lastPktmonHopMs >= kPktmonHopIntervalMs) {
    lastPktmonHopMs = now;
    pktmonHopIndex = (pktmonHopIndex + 1) % kDeauthHopChannelCount;
    esp_wifi_set_channel(kDeauthHopChannels[pktmonHopIndex],
                         WIFI_SECOND_CHAN_NONE);
  }
  if (now - lastPktmonDrawMs >= kPktmonRedrawMs) {
    lastPktmonDrawMs = now;
    if (pktmonFileOpen) pktmonFile.flush();
    drawPacketMon();
  }
}
