struct BleCluster {
  uint32_t fp;
  uint16_t id;
  uint16_t sightings;
  uint8_t addrCount;
  int8_t rssi;
  bool randomAddr;
  char recentAddr[4][18];
  uint8_t recentHead;
  uint32_t lastMs;
};

struct BleMacCluster {
  char addr[18];
  uint16_t id;
};

constexpr int kMaxBleClusters = 24;
constexpr int kBleMacRingSlots = 40;
constexpr int kBleIdVisibleRows = 7;

static BleCluster bleClusters[kMaxBleClusters];
static int bleClusterCount = 0;
static uint16_t nextBleClusterId = 1;
static BleMacCluster bleMacRing[kBleMacRingSlots];
static int bleMacRingFill = 0;
static int bleMacRingHead = 0;
static uint32_t bleIdEvictions = 0;

static uint32_t bleFingerprint(const BleEntry& e) {
  uint32_t fp = 2166136261u;
  const char* s = e.serviceUuids.c_str();
  for (; *s; ++s) fp = (fp ^ (uint8_t)*s) * 16777619u;
  const uint32_t mid = (uint32_t)e.manufacturerId;
  for (int i = 0; i < 4; ++i) fp = (fp ^ ((mid >> (i * 8)) & 0xFF)) * 16777619u;
  const char* m = e.manufacturerDataHex.c_str();
  for (int i = 0; i < 8 && m[i]; ++i) fp = (fp ^ (uint8_t)m[i]) * 16777619u;
  const uint32_t tx = (uint32_t)e.txPower;
  for (int i = 0; i < 2; ++i) fp = (fp ^ ((tx >> (i * 8)) & 0xFF)) * 16777619u;
  fp = (fp ^ e.addressType) * 16777619u;
  fp = (fp ^ (e.connectable ? 1u : 0u)) * 16777619u;
  fp = (fp ^ (e.scannable ? 1u : 0u)) * 16777619u;
  return fp;
}

static bool addrIsRandom(const String& addr) {
  if (addr.length() < 2) return false;
  int hi = 0;
  const char c = addr.charAt(0);
  if (c >= '0' && c <= '9') hi = c - '0';
  else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
  return (hi & 0x0C) != 0;
}

static int bleFindOrAddCluster(uint32_t fp) {
  for (int i = 0; i < bleClusterCount; ++i)
    if (bleClusters[i].fp == fp) return i;
  if (bleClusterCount >= kMaxBleClusters) {
    ++bleIdEvictions;
    int oldest = 0;
    for (int i = 1; i < bleClusterCount; ++i)
      if (bleClusters[i].lastMs < bleClusters[oldest].lastMs) oldest = i;
    bleClusters[oldest] = BleCluster();
    bleClusters[oldest].fp = fp;
    bleClusters[oldest].id = nextBleClusterId++;
    return oldest;
  }
  const int idx = bleClusterCount++;
  bleClusters[idx] = BleCluster();
  bleClusters[idx].fp = fp;
  bleClusters[idx].id = nextBleClusterId++;
  return idx;
}

static void bleMacRingPut(const char* addr, uint16_t id) {
  for (int i = 0; i < bleMacRingFill; ++i)
    if (strncmp(bleMacRing[i].addr, addr, sizeof(bleMacRing[i].addr)) == 0) {
      bleMacRing[i].id = id;
      return;
    }
  strncpy(bleMacRing[bleMacRingHead].addr, addr, sizeof(bleMacRing[bleMacRingHead].addr) - 1);
  bleMacRing[bleMacRingHead].addr[sizeof(bleMacRing[bleMacRingHead].addr) - 1] = 0;
  bleMacRing[bleMacRingHead].id = id;
  bleMacRingHead = (bleMacRingHead + 1) % kBleMacRingSlots;
  if (bleMacRingFill < kBleMacRingSlots) ++bleMacRingFill;
}

static void bleRecluster() {
  for (int i = 0; i < bleClusterCount; ++i) bleClusters[i].lastMs = 0;
  for (int i = 0; i < bleCount; ++i) {
    const BleEntry& e = bleEntries[i];
    const uint32_t fp = bleFingerprint(e);
    const int c = bleFindOrAddCluster(fp);
    ++bleClusters[c].sightings;
    bleClusters[c].rssi = (int8_t)e.rssi;
    bleClusters[c].randomAddr = addrIsRandom(e.address);
    bleClusters[c].lastMs = millis();

    const char* addr = e.address.c_str();
    bool known = false;
    for (int k = 0; k < 4; ++k)
      if (strncmp(bleClusters[c].recentAddr[k], addr, 18) == 0) { known = true; break; }
    if (!known) {
      strncpy(bleClusters[c].recentAddr[bleClusters[c].recentHead], addr, 17);
      bleClusters[c].recentAddr[bleClusters[c].recentHead][17] = 0;
      bleClusters[c].recentHead = (bleClusters[c].recentHead + 1) % 4;
      if (bleClusters[c].addrCount < 255) ++bleClusters[c].addrCount;
    }
    bleMacRingPut(addr, bleClusters[c].id);
  }
}

uint16_t bleClusterFor(const char* addr) {
  for (int i = 0; i < bleMacRingFill; ++i)
    if (strncmp(bleMacRing[i].addr, addr, 18) == 0) return bleMacRing[i].id;
  return 0;
}

void drawBleId() {
  currentView = View::kBleId;
  display.fillScreen(kBackground);
  drawHeader("BLE ID", "advertiser clusters");

  int confirmed = 0;
  for (int i = 0; i < bleClusterCount; ++i)
    if (bleClusters[i].addrCount >= 2) ++confirmed;

  display.setTextSize(1);
  display.setTextColor(kGood, kBackground);
  display.setCursor(8, 48);
  display.printf("%d conf", confirmed);
  display.setTextColor(kMuted, kBackground);
  display.setCursor(80, 48);
  display.printf("%d seen", bleClusterCount);
  if (bleIdEvictions > 0) {
    display.setTextColor(kWarn, kBackground);
    display.setCursor(168, 48);
    display.printf("ev%lu", (unsigned long)bleIdEvictions);
  }
  display.drawFastHLine(0, 62, kScreenWidth, 0x18E3);

  int order[kMaxBleClusters];
  for (int i = 0; i < bleClusterCount; ++i) order[i] = i;
  for (int i = 0; i < bleClusterCount; ++i)
    for (int j = i + 1; j < bleClusterCount; ++j)
      if (bleClusters[order[j]].addrCount > bleClusters[order[i]].addrCount) {
        const int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
      }

  int y = 70;
  const int shown = bleClusterCount < kBleIdVisibleRows ? bleClusterCount : kBleIdVisibleRows;
  for (int r = 0; r < shown; ++r) {
    const BleCluster& c = bleClusters[order[r]];
    display.setTextColor(kForeground, kBackground);
    display.setCursor(8, y);
    display.printf("#%u", c.id);
    display.setTextColor(c.addrCount > 1 ? kWarn : kMuted, kBackground);
    display.setCursor(56, y);
    display.printf("x%u addr", c.addrCount);
    display.setTextColor(kMuted, kBackground);
    display.setCursor(152, y);
    display.printf("%d", c.rssi);
    if (c.randomAddr) {
      display.setTextColor(kWarn, kBackground);
      display.setCursor(214, y);
      display.print("R");
    }
    y += 26;
  }

  if (bleClusterCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(52, 150);
    display.print("Scan for BLE gear");
  }

  drawThreeButtonFooter("Back", "Scan", "Clear");
}

void startBleId() {
  if (!stopActiveTools()) return;
  bleClusterCount = 0;
  nextBleClusterId = 1;
  bleMacRingFill = 0;
  bleMacRingHead = 0;
  bleIdEvictions = 0;
  currentView = View::kBleId;
  drawBleId();
}

void updateBleId() {
  if (currentView != View::kBleId) return;
}

void bleIdTouch(int x) {
  if (x < kScreenWidth / 3) {
    drawReconMenu();
  } else if (x < (kScreenWidth * 2) / 3) {
    scanBle();
    bleRecluster();
    drawBleId();
  } else {
    bleClusterCount = 0;
    nextBleClusterId = 1;
    bleMacRingFill = 0;
    bleMacRingHead = 0;
    bleIdEvictions = 0;
    drawBleId();
  }
}
