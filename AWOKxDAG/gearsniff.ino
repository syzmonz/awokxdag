enum GearType {
  kGearNone = 0,
  kGearPwnagotchi = 1,
  kGearMarauder = 2,
  kGearFlipper = 3
};

constexpr uint16_t kGearPwnColor = 0xF81F;
constexpr uint16_t kGearMarauderColor = 0x07FF;
constexpr uint16_t kGearFlipperColor = 0xFC00;
constexpr uint32_t kGearHopMs = 180;
constexpr uint32_t kGearRedrawMs = 500;
constexpr uint32_t kGearRetentionMs = 20000;
constexpr int kGearMaxDevices = 24;
constexpr int kGearBleHitSlots = AwokPins::kDualBand ? 32 : 16;
static const uint8_t kPwnagotchiMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};

struct GearDevice {
  char addr[18];
  uint8_t type;
  int8_t rssi;
  uint32_t lastMs;
};

struct GearBleHit {
  char addr[18];
  int8_t rssi;
  uint8_t type;
};

static GearDevice gearDevices[kGearMaxDevices];
static int gearDeviceCount = 0;
static GearBleHit gearBleHitQueue[kGearBleHitSlots];
static volatile int gearBleHitHead = 0;
static volatile int gearBleHitTail = 0;

static int gearCounts[4] = {0, 0, 0, 0};
static int gearTotalSeen = 0;
static int gearClosestRssi = -127;
static uint8_t gearClosestType = kGearNone;
static uint8_t gearFilter = kGearNone;
static volatile int gearPwnBeacons = 0;
static volatile int8_t gearPwnRssi = -127;
static int gearPwnLastCount = 0;
static int gearHopIdx = 0;
static uint32_t lastGearHopMs = 0;
static uint32_t lastGearDrawMs = 0;

void gearWifiCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* packet = static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = packet->payload;
  if (packet->rx_ctrl.sig_len < 22) return;
  if (payload[0] != 0x80) return;
  for (int i = 0; i < 6; ++i)
    if (payload[10 + i] != kPwnagotchiMac[i]) return;
  ++gearPwnBeacons;
  if (packet->rx_ctrl.rssi > gearPwnRssi) gearPwnRssi = packet->rx_ctrl.rssi;
}

class GearBleCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    uint8_t type = kGearNone;
    for (int i = 0; i < device->getServiceUUIDCount(); ++i) {
      if (*NetworkParse::flipper(device->getServiceUUID(i).toString().c_str())) {
        type = kGearFlipper;
        break;
      }
    }
    if (type == kGearNone && device->haveName()) {
      String n = device->getName().c_str();
      n.toLowerCase();
      if (n.indexOf("pwnagotchi") >= 0 || n.indexOf("pwngrid") >= 0)
        type = kGearPwnagotchi;
      else if (n.indexOf("marauder") >= 0)
        type = kGearMarauder;
      else if (n.indexOf("flipper") >= 0)
        type = kGearFlipper;
    }
    if (type == kGearNone) return;
    const int next = (gearBleHitHead + 1) % kGearBleHitSlots;
    if (next == gearBleHitTail) return;
    GearBleHit& hit = gearBleHitQueue[gearBleHitHead];
    strncpy(hit.addr, device->getAddress().toString().c_str(), sizeof(hit.addr) - 1);
    hit.addr[sizeof(hit.addr) - 1] = 0;
    hit.rssi = device->getRSSI();
    hit.type = type;
    gearBleHitHead = next;
  }
};

GearBleCallbacks gearBleCallbacks;

static void gearEnterWifi() {
  WiFi.disconnect(false, false);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&gearWifiCallback);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(kDeauthHopChannels[gearHopIdx], WIFI_SECOND_CHAN_NONE);
}
static void gearExitWifi() { esp_wifi_set_promiscuous(false); }

RadioScheduler gearSched;

static void gearMergeDevice(const char* addr, uint8_t type, int8_t rssi) {
  const uint32_t now = millis();
  for (int i = 0; i < gearDeviceCount; ++i) {
    if (gearDevices[i].type == type &&
        strncmp(gearDevices[i].addr, addr, sizeof(gearDevices[i].addr)) == 0) {
      gearDevices[i].rssi = rssi;
      gearDevices[i].lastMs = now;
      return;
    }
  }
  int slot;
  if (gearDeviceCount < kGearMaxDevices) {
    slot = gearDeviceCount++;
  } else {
    slot = 0;
    for (int i = 1; i < gearDeviceCount; ++i)
      if (gearDevices[i].lastMs < gearDevices[slot].lastMs) slot = i;
  }
  strncpy(gearDevices[slot].addr, addr, sizeof(gearDevices[slot].addr) - 1);
  gearDevices[slot].addr[sizeof(gearDevices[slot].addr) - 1] = 0;
  gearDevices[slot].type = type;
  gearDevices[slot].rssi = rssi;
  gearDevices[slot].lastMs = now;
}

static void gearPrune(uint32_t now) {
  for (int i = 0; i < gearDeviceCount;) {
    if (now - gearDevices[i].lastMs > kGearRetentionMs) {
      for (int j = i + 1; j < gearDeviceCount; ++j) gearDevices[j - 1] = gearDevices[j];
      --gearDeviceCount;
    } else {
      ++i;
    }
  }
}

static void gearRecompute() {
  for (int i = 0; i < 4; ++i) gearCounts[i] = 0;
  gearTotalSeen = 0;
  gearClosestRssi = -127;
  gearClosestType = kGearNone;
  for (int i = 0; i < gearDeviceCount; ++i) {
    const uint8_t t = gearDevices[i].type;
    if (t < 1 || t > 3) continue;
    ++gearCounts[t];
    ++gearTotalSeen;
    if (gearFilter != kGearNone && t != gearFilter) continue;
    if ((int)gearDevices[i].rssi > gearClosestRssi) {
      gearClosestRssi = gearDevices[i].rssi;
      gearClosestType = t;
    }
  }
}

static uint16_t gearColor(uint8_t type) {
  switch (type) {
    case kGearPwnagotchi: return kGearPwnColor;
    case kGearMarauder: return kGearMarauderColor;
    case kGearFlipper: return kGearFlipperColor;
    default: return kMuted;
  }
}

static const char* gearLabel(uint8_t type) {
  switch (type) {
    case kGearPwnagotchi: return "PWN";
    case kGearMarauder: return "MRD";
    case kGearFlipper: return "FLP";
    default: return "--";
  }
}

static void gearTile(int x, int y, const char* name, int count, uint16_t color) {
  const int w = 108, h = 48;
  display.drawRect(x, y, w, h, 0x18E3);
  display.setTextSize(1);
  display.setTextColor(color, kBackground);
  display.setCursor(x + 8, y + 8);
  display.print(name);
  display.setTextSize(2);
  display.setTextColor(count > 0 ? kForeground : kMuted, kBackground);
  display.setCursor(x + 8, y + 24);
  display.printf("%d", count);
}

void drawGearSniff() {
  currentView = View::kGearSniff;
  display.fillScreen(kBackground);
  char sub[20];
  if (gearFilter != kGearNone) {
    snprintf(sub, sizeof(sub), "only %s", gearLabel(gearFilter));
  } else if (gearActive) {
    snprintf(sub, sizeof(sub), "scan %s",
             gearSched.phase == RadioPhase::kBle ? "BLE" : "WiFi");
  } else {
    strncpy(sub, "idle", sizeof(sub));
    sub[sizeof(sub) - 1] = 0;
  }
  drawHeader("GEAR SNIFF", sub);

  gearTile(8, 52, "PWNAGOTCHI", gearCounts[kGearPwnagotchi], kGearPwnColor);
  gearTile(124, 52, "MARAUDER", gearCounts[kGearMarauder], kGearMarauderColor);
  gearTile(8, 108, "FLIPPER", gearCounts[kGearFlipper], kGearFlipperColor);
  gearTile(124, 108, "TRACKED", gearDeviceCount, kMuted);

  display.setTextSize(1);
  display.setTextColor(kMuted, kBackground);
  display.setCursor(8, 176);
  display.print("closest");
  if (gearClosestType != kGearNone) {
    display.setTextColor(gearColor(gearClosestType), kBackground);
    display.setCursor(160, 176);
    display.printf("%s %d", gearLabel(gearClosestType), gearClosestRssi);
  } else {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(196, 176);
    display.print("--");
  }
  display.setTextColor(kMuted, kBackground);
  display.setCursor(8, 194);
  display.print("total seen");
  display.setTextColor(kForeground, kBackground);
  display.setCursor(196, 194);
  display.printf("%d", gearTotalSeen);

  if (!radiosCoexist) {
    display.setTextColor(kWarn, kBackground);
    display.setCursor(8, 212);
    display.print("WiFi only: Flipper needs BLE");
  }

  drawThreeButtonFooter("Back", "Clear", "Type");
}

void startGearSniff() {
  if (!stopActiveTools()) return;
  currentView = View::kGearSniff;
  gearFilter = kGearNone;
  gearDeviceCount = 0;
  gearPwnBeacons = 0;
  gearPwnRssi = -127;
  gearPwnLastCount = 0;
  gearHopIdx = 0;
  lastGearHopMs = millis();
  lastGearDrawMs = 0;
  gearBleHitHead = 0;
  gearBleHitTail = 0;

  gearSched = RadioScheduler();
  gearSched.enterWifi = gearEnterWifi;
  gearSched.exitWifi = gearExitWifi;
  gearSched.bleCallbacks = &gearBleCallbacks;
  gearSched.bleActiveScan = true;
  gearSched.wifiWindowMs = 3000;
  gearSched.bleWindowMs = 3000;
  if (!radioSchedulerBegin(gearSched)) return;

  gearActive = true;
  gearRecompute();
  drawGearSniff();
}

void stopGearSniff() {
  if (!gearActive) return;
  gearActive = false;
  radioSchedulerEnd(gearSched);
}

void updateGearSniff() {
  if (!gearActive || currentView != View::kGearSniff) return;
  radioSchedulerTick(gearSched);
  const uint32_t now = millis();

  if (gearPwnBeacons != gearPwnLastCount) {
    gearPwnLastCount = gearPwnBeacons;
    gearMergeDevice("PWN", kGearPwnagotchi, gearPwnRssi);
  }

  while (gearBleHitTail != gearBleHitHead) {
    const GearBleHit& h = gearBleHitQueue[gearBleHitTail];
    gearMergeDevice(h.addr, h.type, h.rssi);
    gearBleHitTail = (gearBleHitTail + 1) % kGearBleHitSlots;
  }

  gearPrune(now);

  if (gearSched.phase == RadioPhase::kWifi && now - lastGearHopMs >= kGearHopMs) {
    lastGearHopMs = now;
    gearHopIdx = (gearHopIdx + 1) % kDeauthHopChannelCount;
    esp_wifi_set_channel(kDeauthHopChannels[gearHopIdx], WIFI_SECOND_CHAN_NONE);
  }

  if (now - lastGearDrawMs >= kGearRedrawMs) {
    lastGearDrawMs = now;
    gearRecompute();
    drawGearSniff();
  }
}

void gearSniffTouch(int x) {
  if (x < kScreenWidth / 3) {
    stopGearSniff();
    drawReconMenu();
  } else if (x < (kScreenWidth * 2) / 3) {
    gearDeviceCount = 0;
    gearPwnBeacons = 0;
    gearPwnLastCount = 0;
    gearPwnRssi = -127;
    gearRecompute();
    drawGearSniff();
  } else {
    gearFilter = (uint8_t)((gearFilter + 1) % 4);
    gearRecompute();
    drawGearSniff();
  }
}
