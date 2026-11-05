enum GearType {
  kGearNone = 0,
  kGearPwnagotchi = 1,
  kGearMarauder = 2,
  kGearFlipper = 3
};

constexpr uint16_t kGearPwnColor = 0xF81F;
constexpr uint16_t kGearMarauderColor = 0x07FF;
constexpr uint16_t kGearFlipperColor = 0xFC00;
constexpr uint32_t kGearSweepDwellMs = 180;
static const uint8_t kPwnagotchiMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};

static int gearCounts[4] = {0, 0, 0, 0};
static int gearTotalSeen = 0;
static int gearClosestRssi = -127;
static uint8_t gearClosestType = kGearNone;
static uint8_t gearFilter = kGearNone;
static volatile int gearPwnBeacons = 0;
static volatile int8_t gearPwnRssi = -127;

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

static void gearWifiSweep() {
  gearPwnBeacons = 0;
  gearPwnRssi = -127;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_promiscuous(false);
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&gearWifiCallback);
  esp_wifi_set_promiscuous(true);
  for (int c = 0; c < kDeauthHopChannelCount; ++c) {
    esp_wifi_set_channel(kDeauthHopChannels[c], WIFI_SECOND_CHAN_NONE);
    delay(kGearSweepDwellMs);
  }
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
}

static uint8_t gearClassifyBle(const BleEntry& e) {
  if (e.flipperLike) return kGearFlipper;
  String n = e.name;
  n.toLowerCase();
  if (n.indexOf("pwnagotchi") >= 0 || n.indexOf("pwngrid") >= 0) return kGearPwnagotchi;
  if (n.indexOf("marauder") >= 0) return kGearMarauder;
  return kGearNone;
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

static void gearRecompute() {
  for (int i = 0; i < 4; ++i) gearCounts[i] = 0;
  gearTotalSeen = 0;
  gearClosestRssi = -127;
  gearClosestType = kGearNone;

  gearCounts[kGearPwnagotchi] = gearPwnBeacons;
  gearTotalSeen += gearPwnBeacons;
  if (gearPwnBeacons > 0 &&
      (gearFilter == kGearNone || gearFilter == kGearPwnagotchi) &&
      (int)gearPwnRssi > gearClosestRssi) {
    gearClosestRssi = (int)gearPwnRssi;
    gearClosestType = kGearPwnagotchi;
  }

  for (int i = 0; i < bleCount; ++i) {
    const uint8_t t = gearClassifyBle(bleEntries[i]);
    if (t == kGearNone) continue;
    ++gearCounts[t];
    ++gearTotalSeen;
    if (gearFilter != kGearNone && t != gearFilter) continue;
    if ((int)bleEntries[i].rssi > gearClosestRssi) {
      gearClosestRssi = (int)bleEntries[i].rssi;
      gearClosestType = t;
    }
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
  char sub[18];
  snprintf(sub, sizeof(sub), "filter %s", gearLabel(gearFilter));
  drawHeader("GEAR SNIFF", gearFilter == kGearNone ? "wifi + ble" : sub);

  gearTile(8, 52, "PWNAGOTCHI", gearCounts[kGearPwnagotchi], kGearPwnColor);
  gearTile(124, 52, "MARAUDER", gearCounts[kGearMarauder], kGearMarauderColor);
  gearTile(8, 108, "FLIPPER", gearCounts[kGearFlipper], kGearFlipperColor);
  gearTile(124, 108, "UNKNOWN", 0, kMuted);

  display.setTextSize(1);
  display.setTextColor(kMuted, kBackground);
  display.setCursor(8, 176);
  display.print("closest");
  if (gearClosestType != kGearNone) {
    display.setTextColor(gearColor(gearClosestType), kBackground);
    display.setCursor(168, 176);
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

  drawThreeButtonFooter("Back", "Scan", "Type");
}

void startGearSniff() {
  if (!stopActiveTools()) return;
  currentView = View::kGearSniff;
  gearFilter = kGearNone;
  gearRecompute();
  drawGearSniff();
}

void updateGearSniff() {
  if (currentView != View::kGearSniff) return;
}

void gearSniffTouch(int x) {
  if (x < kScreenWidth / 3) {
    drawReconMenu();
  } else if (x < (kScreenWidth * 2) / 3) {
    gearWifiSweep();
    scanBle();
    gearRecompute();
    drawGearSniff();
  } else {
    gearFilter = (uint8_t)((gearFilter + 1) % 4);
    gearRecompute();
    drawGearSniff();
  }
}
