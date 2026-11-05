enum GearType {
  kGearNone = 0,
  kGearPwnagotchi = 1,
  kGearMarauder = 2,
  kGearFlipper = 3
};

constexpr uint16_t kGearPwnColor = 0xF81F;
constexpr uint16_t kGearMarauderColor = 0x07FF;
constexpr uint16_t kGearFlipperColor = 0xFC00;

static int gearCounts[4] = {0, 0, 0, 0};
static int gearTotalSeen = 0;
static int gearClosestRssi = -127;
static uint8_t gearClosestType = kGearNone;
static uint8_t gearFilter = kGearNone;

static uint8_t gearClassify(const BleEntry& e) {
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
  for (int i = 0; i < bleCount; ++i) {
    const uint8_t t = gearClassify(bleEntries[i]);
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
  char sub[16];
  snprintf(sub, sizeof(sub), "filter %s", gearLabel(gearFilter));
  drawHeader("GEAR SNIFF", gearFilter == kGearNone ? "all gear" : sub);

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
    scanBle();
    gearRecompute();
    drawGearSniff();
  } else {
    gearFilter = (uint8_t)((gearFilter + 1) % 4);
    gearRecompute();
    drawGearSniff();
  }
}
