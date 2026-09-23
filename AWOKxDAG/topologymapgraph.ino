bool topoGraphMode = false;

constexpr uint32_t kTopoFreshMs = 3000;
constexpr uint32_t kTopoHideMs = 15000;
constexpr int kTopoSlots = 12;
constexpr int kTopoStartIdx = 18;

static const uint16_t kTopoPal[101] = {
  0x0049, 0x088B, 0x08AC, 0x08CD, 0x08EE, 0x090F, 0x0930, 0x0951, 0x0992, 0x09B2,
  0x09F3, 0x0A13, 0x1234, 0x1274, 0x1295, 0x12B5, 0x12F5, 0x1316, 0x1336, 0x1356,
  0x1396, 0x13B5, 0x13D5, 0x13F5, 0x1415, 0x1435, 0x1454, 0x1494, 0x14B4, 0x14D4,
  0x14F4, 0x1513, 0x1513, 0x1532, 0x1D32, 0x1D51, 0x1D50, 0x1D70, 0x1D6F, 0x1D8F,
  0x258E, 0x25AE, 0x25AD, 0x25CC, 0x25CC, 0x25EB, 0x2DEB, 0x2DEA, 0x35EA, 0x3E0A,
  0x4609, 0x4E09, 0x5608, 0x5608, 0x5E08, 0x6627, 0x6E27, 0x7627, 0x7E26, 0x7E26,
  0x8626, 0x8E25, 0x9645, 0x9E25, 0x9E24, 0xA624, 0xA604, 0xAE04, 0xADE4, 0xB5E4,
  0xB5E4, 0xBDC4, 0xC5C4, 0xC5A4, 0xCDA4, 0xCD84, 0xD584, 0xD584, 0xDD63, 0xDD63,
  0xE543, 0xE543, 0xED03, 0xECE3, 0xECA4, 0xEC84, 0xEC44, 0xEC24, 0xF404, 0xF3C4,
  0xF3A4, 0xF364, 0xF344, 0xF304, 0xF2E4, 0xFAC4, 0xFA84, 0xFA64, 0xFA24, 0xFA04,
  0xF9E5
};

static const int16_t kTopoSin[24] = {
  0, 66, 128, 181, 222, 247, 256, 247, 222, 181, 128, 66,
  0, -66, -128, -181, -222, -247, -256, -247, -222, -181, -128, -66
};

static uint8_t topoSlotBssid[16][6];
static int topoSlotUsed = 0;

int topoSlotFor(const uint8_t* bssid) {
  for (int i = 0; i < topoSlotUsed; ++i) {
    if (memcmp(topoSlotBssid[i], bssid, 6) == 0) return i % kTopoSlots;
  }
  if (topoSlotUsed < 16) {
    memcpy(topoSlotBssid[topoSlotUsed], bssid, 6);
    return (topoSlotUsed++) % kTopoSlots;
  }
  return bssid[5] % kTopoSlots;
}

int topoSinT(int i) { return kTopoSin[((i % 24) + 24) % 24]; }
int topoCosT(int i) { return kTopoSin[(((i + 6) % 24) + 24) % 24]; }

uint16_t topoColor(int rssi) {
  int v = rssi;
  if (v < -90) v = -90;
  if (v > -30) v = -30;
  int lvl = (v + 90) * 100 / 60;
  if (lvl < 0) lvl = 0;
  if (lvl > 100) lvl = 100;
  return kTopoPal[lvl];
}

uint16_t topoDim(uint16_t c, int pct) {
  int r = (c >> 11) & 0x1F;
  int g = (c >> 5) & 0x3F;
  int b = c & 0x1F;
  r = r * pct / 100;
  g = g * pct / 100;
  b = b * pct / 100;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

int topoFreshPct(uint32_t now, uint32_t lastSeen) {
  uint32_t age = now - lastSeen;
  if (age >= kTopoHideMs) return 0;
  if (age <= kTopoFreshMs) return 100;
  return 100 - (int)((age - kTopoFreshMs) * 70 / (kTopoHideMs - kTopoFreshMs));
}

void drawTopologyGraphBody() {
  const int cx = kScreenWidth / 2;
  const int cyTop = kHeaderHeight;
  const int cyBot = kFooterTop;
  const int cy = (cyTop + cyBot) / 2;
  const uint32_t now = millis();
  static const int coff[4] = {-2, -1, 1, 2};

  display.fillRect(0, cyTop, kScreenWidth, cyBot - cyTop, kBackground);
  display.drawCircle(cx, cy, 48, topoDim(kMuted, 22));
  display.drawCircle(cx, cy, 95, topoDim(kMuted, 22));

  int order[kMaxTopoAps];
  int n = topoApCount;
  for (int i = 0; i < n; ++i) order[i] = i;
  for (int i = 0; i < n - 1; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (topoAps[order[j]].rssi > topoAps[order[i]].rssi) {
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  }

  int shown = 0;
  const int kMaxNodes = 8;

  for (int k = 0; k < n && shown < kMaxNodes; ++k) {
    const TopoAp& ap = topoAps[order[k]];
    int pct = topoFreshPct(now, ap.lastSeenMs);
    if (pct <= 0) continue;

    int idxAP = (kTopoStartIdx + topoSlotFor(ap.bssid) * 2) % 24;
    int rssi = ap.rssi;
    if (rssi < -90) rssi = -90;
    if (rssi > -40) rssi = -40;
    int rr = 46 + (-40 - rssi);

    int ax = cx + (topoCosT(idxAP) * rr) / 256;
    int ay = cy + (topoSinT(idxAP) * rr) / 256;
    if (ay < cyTop + 12) ay = cyTop + 12;
    if (ay > cyBot - 14) ay = cyBot - 14;

    uint16_t heat = topoDim(topoColor(ap.rssi), pct);
    display.drawLine(cx, cy, ax, ay, heat);

    int drawn = 0;
    for (int c = 0; c < topoClientCount && drawn < 4; ++c) {
      if (!topoClients[c].hasBssid) continue;
      if (memcmp(topoClients[c].bssid, ap.bssid, 6) != 0) continue;
      int cpct = topoFreshPct(now, topoClients[c].lastSeenMs);
      if (cpct <= 0) continue;
      int cidx = idxAP + coff[drawn];
      int cxx = ax + (topoCosT(cidx) * 14) / 256;
      int cyy = ay + (topoSinT(cidx) * 14) / 256;
      if (cyy < cyTop + 3) cyy = cyTop + 3;
      if (cyy > cyBot - 3) cyy = cyBot - 3;
      uint16_t chc = topoDim(topoColor(topoClients[c].rssi), cpct);
      display.drawLine(ax, ay, cxx, cyy, chc);
      display.fillCircle(cxx, cyy, 2, chc);
      ++drawn;
    }

    int nodeR = 5 + (ap.clientCount > 4 ? 4 : ap.clientCount);
    display.fillCircle(ax, ay, nodeR, heat);
    if (ap.isOpen) display.drawCircle(ax, ay, nodeR + 2, topoDim(kWarn, pct));

    String label = ap.ssid[0] ? clipped(ap.ssid, 7)
                              : String(ap.bssid[4], HEX) + String(ap.bssid[5], HEX);
    int lw = label.length() * 6;
    int lx = ax - lw / 2;
    if (lx < 1) lx = 1;
    if (lx + lw > kScreenWidth - 1) lx = kScreenWidth - 1 - lw;
    int ly = ay + nodeR + 2;
    if (ly > cyBot - 9) ly = ay - nodeR - 10;
    display.setTextColor(topoDim(ap.isOpen ? kWarn : kForeground, pct), kBackground);
    display.setCursor(lx, ly);
    display.print(label);
    ++shown;
  }

  if (shown == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(28, cy - 4);
    display.print("Sniffing links...");
  }

  display.fillCircle(cx, cy, 5, kAccent);
  display.drawCircle(cx, cy, 8, kAccent);
  display.setTextColor(kAccent, kBackground);
  display.setCursor(cx - 9, cy + 10);
  display.print("YOU");
}
