struct FpEvt {
  uint32_t fp;
  uint8_t mac[6];
  int8_t rssi;
  bool randomized;
};

struct DeviceCluster {
  uint32_t fp;
  uint16_t id;
  uint16_t probes;
  uint8_t macCount;
  int8_t rssi;
  bool randomized;
  uint8_t recentMacs[4][6];
  uint8_t recentHead;
  uint32_t lastMs;
};

struct MacCluster {
  uint8_t mac[6];
  uint16_t id;
};

constexpr int kFpEvtSlots = 48;
constexpr int kMaxClusters = 24;
constexpr int kMacRingSlots = 40;
constexpr uint32_t kDeviceIdHopMs = 280;
constexpr uint32_t kDeviceIdRedrawMs = 600;
constexpr int kDeviceIdVisibleRows = 7;

static volatile FpEvt fpEvtQ[kFpEvtSlots];
static volatile int fpEvtHead = 0;
static volatile int fpEvtTail = 0;

static DeviceCluster clusters[kMaxClusters];
static int clusterCount = 0;
static uint16_t nextClusterId = 1;
static MacCluster macRing[kMacRingSlots];
static int macRingFill = 0;
static int macRingHead = 0;
static uint32_t totalProbes = 0;
static uint32_t fpEvictions = 0;
static bool deviceIdActive = false;
static int deviceIdHopIdx = 0;
static uint32_t lastDeviceIdHopMs = 0;
static uint32_t lastDeviceIdDrawMs = 0;

void deviceIdCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* packet = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = packet->payload;
  const int length = packet->rx_ctrl.sig_len;
  if (length < 28) return;
  if ((payload[0] & 0xF0) != 0x40) return;

  uint32_t fp = 2166136261u;
  int pos = 24;
  while (pos + 2 <= length) {
    const uint8_t tag = payload[pos];
    const uint8_t len = payload[pos + 1];
    if (pos + 2 + (int)len > length) break;
    fp = (fp ^ tag) * 16777619u;
    if (tag == 1 || tag == 45 || tag == 48 || tag == 50 || tag == 127 || tag == 191) {
      for (int k = 0; k < len; ++k) fp = (fp ^ payload[pos + 2 + k]) * 16777619u;
    } else if (tag == 221 && len >= 4) {
      for (int k = 0; k < 4; ++k) fp = (fp ^ payload[pos + 2 + k]) * 16777619u;
    }
    pos += 2 + len;
  }

  const int next = (fpEvtHead + 1) % kFpEvtSlots;
  if (next == fpEvtTail) return;
  fpEvtQ[fpEvtHead].fp = fp;
  for (int i = 0; i < 6; ++i) fpEvtQ[fpEvtHead].mac[i] = payload[10 + i];
  fpEvtQ[fpEvtHead].rssi = packet->rx_ctrl.rssi;
  fpEvtQ[fpEvtHead].randomized = (payload[10] & 0x02) != 0;
  fpEvtHead = next;
}

static bool macEq(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 6; ++i) if (a[i] != b[i]) return false;
  return true;
}

static int fpFindOrAddCluster(uint32_t fp) {
  for (int i = 0; i < clusterCount; ++i)
    if (clusters[i].fp == fp) return i;
  if (clusterCount >= kMaxClusters) {
    ++fpEvictions;
    int oldest = 0;
    for (int i = 1; i < clusterCount; ++i)
      if (clusters[i].lastMs < clusters[oldest].lastMs) oldest = i;
    clusters[oldest] = DeviceCluster();
    clusters[oldest].fp = fp;
    clusters[oldest].id = nextClusterId++;
    return oldest;
  }
  const int idx = clusterCount++;
  clusters[idx] = DeviceCluster();
  clusters[idx].fp = fp;
  clusters[idx].id = nextClusterId++;
  return idx;
}

static void macRingPut(const uint8_t* mac, uint16_t id) {
  for (int i = 0; i < macRingFill; ++i)
    if (macEq(macRing[i].mac, mac)) { macRing[i].id = id; return; }
  for (int j = 0; j < 6; ++j) macRing[macRingHead].mac[j] = mac[j];
  macRing[macRingHead].id = id;
  macRingHead = (macRingHead + 1) % kMacRingSlots;
  if (macRingFill < kMacRingSlots) ++macRingFill;
}

static void fpDrain() {
  while (fpEvtTail != fpEvtHead) {
    const int t = fpEvtTail;
    const uint32_t fp = fpEvtQ[t].fp;
    uint8_t mac[6];
    for (int j = 0; j < 6; ++j) mac[j] = fpEvtQ[t].mac[j];
    const int8_t rssi = fpEvtQ[t].rssi;
    const bool rnd = fpEvtQ[t].randomized;
    fpEvtTail = (t + 1) % kFpEvtSlots;

    const int c = fpFindOrAddCluster(fp);
    ++clusters[c].probes;
    ++totalProbes;
    clusters[c].rssi = rssi;
    clusters[c].randomized = rnd;
    clusters[c].lastMs = millis();

    bool known = false;
    for (int i = 0; i < 4; ++i)
      if (macEq(clusters[c].recentMacs[i], mac)) { known = true; break; }
    if (!known) {
      for (int j = 0; j < 6; ++j)
        clusters[c].recentMacs[clusters[c].recentHead][j] = mac[j];
      clusters[c].recentHead = (clusters[c].recentHead + 1) % 4;
      if (clusters[c].macCount < 255) ++clusters[c].macCount;
    }
    macRingPut(mac, clusters[c].id);
  }
}

uint16_t deviceClusterFor(const uint8_t* mac) {
  for (int i = 0; i < macRingFill; ++i)
    if (macEq(macRing[i].mac, mac)) return macRing[i].id;
  return 0;
}

void drawDeviceId() {
  currentView = View::kDeviceId;
  display.fillScreen(kBackground);
  drawHeader("DEVICE ID", deviceIdActive ? "probes -> devices" : "idle");

  int confirmed = 0;
  for (int i = 0; i < clusterCount; ++i)
    if (clusters[i].macCount >= 2) ++confirmed;

  display.setTextSize(1);
  display.setTextColor(kGood, kBackground);
  display.setCursor(8, 48);
  display.printf("%d tracked", confirmed);
  display.setTextColor(kMuted, kBackground);
  display.setCursor(100, 48);
  display.printf("%d seen", clusterCount);
  display.setCursor(166, 48);
  display.printf("%lupr", (unsigned long)totalProbes);
  if (fpEvictions > 0) {
    display.setTextColor(kWarn, kBackground);
    display.setCursor(212, 48);
    display.printf("ev%lu", (unsigned long)fpEvictions);
  }

  display.drawFastHLine(0, 62, kScreenWidth, 0x18E3);

  int order[kMaxClusters];
  for (int i = 0; i < clusterCount; ++i) order[i] = i;
  for (int i = 0; i < clusterCount; ++i)
    for (int j = i + 1; j < clusterCount; ++j)
      if (clusters[order[j]].probes > clusters[order[i]].probes) {
        const int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
      }

  int y = 70;
  const int shown = clusterCount < kDeviceIdVisibleRows ? clusterCount : kDeviceIdVisibleRows;
  for (int r = 0; r < shown; ++r) {
    const DeviceCluster& c = clusters[order[r]];
    display.setTextColor(kForeground, kBackground);
    display.setCursor(8, y);
    display.printf("#%u", c.id);
    display.setTextColor(c.macCount > 1 ? kWarn : kMuted, kBackground);
    display.setCursor(56, y);
    display.printf("x%u MAC", c.macCount);
    display.setTextColor(kMuted, kBackground);
    display.setCursor(136, y);
    display.printf("P%u", c.probes);
    display.setTextColor(kMuted, kBackground);
    display.setCursor(188, y);
    display.printf("%d", c.rssi);
    if (c.randomized) {
      display.setTextColor(kWarn, kBackground);
      display.setCursor(224, y);
      display.print("R");
    }
    y += 26;
  }

  if (clusterCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(18, 140);
    display.print("Listening for probe requests");
    display.setCursor(18, 158);
    display.print("Phones probe with random MACs;");
    display.setCursor(18, 172);
    display.print("same IE fingerprint = 1 device.");
    display.setTextColor(kGood, kBackground);
    display.setCursor(18, 192);
    display.print("x2+ MAC on one ID = tracked");
  } else {
    const int legendY = 70 + shown * 26 + 2;
    if (legendY <= 250) {
      display.setTextColor(kMuted, kBackground);
      display.setCursor(8, legendY);
      display.print("xN=MACs merged  P=probes  R=random");
    }
  }

  drawFooter("Back", "Clear");
}

void startDeviceId() {
  if (!stopActiveTools()) return;
  clusterCount = 0;
  nextClusterId = 1;
  macRingFill = 0;
  macRingHead = 0;
  totalProbes = 0;
  fpEvictions = 0;
  fpEvtHead = 0;
  fpEvtTail = 0;
  deviceIdHopIdx = 0;
  lastDeviceIdHopMs = millis();
  lastDeviceIdDrawMs = 0;
  signalMonitorActive = false;

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&deviceIdCallback);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(kDeauthHopChannels[0], WIFI_SECOND_CHAN_NONE);
  deviceIdActive = true;
  currentView = View::kDeviceId;
  drawDeviceId();
}

void stopDeviceId() {
  if (!deviceIdActive) return;
  deviceIdActive = false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
}

void updateDeviceId() {
  if (currentView != View::kDeviceId || !deviceIdActive) return;
  const uint32_t now = millis();
  fpDrain();
  if (now - lastDeviceIdHopMs >= kDeviceIdHopMs) {
    lastDeviceIdHopMs = now;
    deviceIdHopIdx = (deviceIdHopIdx + 1) % kDeauthHopChannelCount;
    esp_wifi_set_channel(kDeauthHopChannels[deviceIdHopIdx], WIFI_SECOND_CHAN_NONE);
  }
  if (now - lastDeviceIdDrawMs >= kDeviceIdRedrawMs) {
    lastDeviceIdDrawMs = now;
    drawDeviceId();
  }
}

void deviceIdTouch(int x) {
  if (x < kScreenWidth / 2) {
    stopDeviceId();
    drawReconMenu();
  } else {
    clusterCount = 0;
    nextClusterId = 1;
    macRingFill = 0;
    macRingHead = 0;
    totalProbes = 0;
    fpEvictions = 0;
    drawDeviceId();
  }
}
