bool topoGraphMode = false;

constexpr uint32_t kTopoFreshMs = 3000;
constexpr uint32_t kTopoHideMs = 15000;
constexpr int kTopoMaxNodes = 8;

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

static int topoNodeX[kTopoMaxNodes];
static int topoNodeY[kTopoMaxNodes];
static uint8_t topoNodeBssid[kTopoMaxNodes][6];
static int topoNodeCount = 0;
static uint8_t topoSelBssid[6];
static bool topoHasSel = false;

int topoSinT(int i) { return kTopoSin[((i % 24) + 24) % 24]; }
int topoCosT(int i) { return kTopoSin[(((i + 6) % 24) + 24) % 24]; }

int topoChanIdx(int ch) {
  if (ch <= 14) {
    int i = 13 + ((ch - 1) * 10) / 13;
    if (i > 23) i = 23;
    if (i < 13) i = 13;
    return i;
  }
  int i = 1 + (ch - 36) / 12;
  if (i > 11) i = 11;
  if (i < 1) i = 1;
  return i;
}

uint16_t topoColor(int rssi, int minR, int span) {
  int lvl = (rssi - minR) * 100 / span;
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

void topoGraphReset() {
  topoHasSel = false;
}

bool topoGraphTap(int x, int y) {
  for (int i = 0; i < topoNodeCount; ++i) {
    int dx = x - topoNodeX[i];
    int dy = y - topoNodeY[i];
    if (dx * dx + dy * dy <= 121) {
      memcpy(topoSelBssid, topoNodeBssid[i], 6);
      topoHasSel = true;
      return true;
    }
  }
  topoHasSel = false;
  return false;
}

void drawTopoPopup(const TopoAp& ap) {
  const int bx = 6;
  const int by = 44;
  const int bw = 228;
  const int bh = 46;
  display.fillRect(bx, by, bw, bh, kPanel);
  display.drawRect(bx, by, bw, bh, kAccent);
  display.setTextColor(kAccent, kPanel);
  display.setCursor(bx + 4, by + 3);
  display.print(ap.ssid[0] ? clipped(ap.ssid, 36) : String("<hidden>"));
  display.setTextColor(kForeground, kPanel);
  display.setCursor(bx + 4, by + 14);
  display.print(macToString(ap.bssid));
  display.setCursor(bx + 4, by + 25);
  display.print("ch ");
  display.print(ap.channel);
  display.print("   ");
  display.print(ap.rssi);
  display.print(" dBm");
  display.setCursor(bx + 4, by + 36);
  display.setTextColor(ap.isOpen ? kWarn : kMuted, kPanel);
  display.print(ap.isOpen ? "OPEN" : "secured");
  display.setTextColor(kMuted, kPanel);
  display.print("   ");
  display.print(ap.clientCount);
  display.print(" cli");
}

void drawTopologyGraphBody() {
  const int cx = kScreenWidth / 2;
  const int cyTop = kHeaderHeight;
  const int cyBot = kFooterTop;
  const int cy = (cyTop + cyBot) / 2;
  const uint32_t now = millis();

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

  int minR = -127;
  int maxR = -127;
  bool anyR = false;
  for (int k = 0; k < n; ++k) {
    if (topoFreshPct(now, topoAps[order[k]].lastSeenMs) <= 0) continue;
    int r = topoAps[order[k]].rssi;
    if (!anyR) { minR = r; maxR = r; anyR = true; }
    else { if (r < minR) minR = r; if (r > maxR) maxR = r; }
  }
  int span = maxR - minR;
  if (span < 20) span = 20;

  int spoke[24];
  for (int i = 0; i < 24; ++i) spoke[i] = 0;

  int shown = 0;
  topoNodeCount = 0;

  for (int k = 0; k < n && shown < kTopoMaxNodes; ++k) {
    const TopoAp& ap = topoAps[order[k]];
    int pct = topoFreshPct(now, ap.lastSeenMs);
    if (pct <= 0) continue;

    int idxAP = topoChanIdx(ap.channel);
    int rssi = ap.rssi;
    if (rssi < -88) rssi = -88;
    if (rssi > -32) rssi = -32;
    int rr = 32 + (-32 - rssi) * 80 / 56;
    rr += spoke[idxAP] * 6;
    spoke[idxAP]++;

    int ax = cx + (topoCosT(idxAP) * rr) / 256;
    int ay = cy + (topoSinT(idxAP) * rr) / 256;
    if (ay < cyTop + 12) ay = cyTop + 12;
    if (ay > cyBot - 14) ay = cyBot - 14;

    uint16_t heat = topoDim(topoColor(ap.rssi, minR, span), pct);
    display.drawLine(cx, cy, ax, ay, heat);

    int drawn = 0;
    static const int coff[4] = {-2, -1, 1, 2};
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
      uint16_t chc = topoDim(topoColor(topoClients[c].rssi, minR, span), cpct);
      display.drawLine(ax, ay, cxx, cyy, chc);
      display.fillCircle(cxx, cyy, 2, chc);
      ++drawn;
    }

    int nodeR = 5 + (ap.clientCount > 4 ? 4 : ap.clientCount);
    display.fillCircle(ax, ay, nodeR, heat);
    if (ap.isOpen) display.drawCircle(ax, ay, nodeR + 2, topoDim(kWarn, pct));

    char hexLabel[5];
    snprintf(hexLabel, sizeof(hexLabel), "%02X%02X", ap.bssid[4], ap.bssid[5]);
    String label = ap.ssid[0] ? clipped(ap.ssid, 7) : String(hexLabel);
    int lw = label.length() * 6;
    int dxc = ax - cx;
    if (dxc < 0) dxc = -dxc;
    int lx;
    int ly;
    if (dxc <= 20) {
      lx = ax - lw / 2;
      ly = (ay < cy) ? (ay - nodeR - 10) : (ay + nodeR + 2);
    } else {
      ly = ay - 3;
      lx = (ax < cx) ? (ax - nodeR - lw - 1) : (ax + nodeR + 1);
    }
    if (ly < cyTop + 1) ly = ay + nodeR + 2;
    if (ly > cyBot - 9) ly = ay - nodeR - 10;
    if (lx < 1) lx = 1;
    if (lx + lw > kScreenWidth - 1) lx = kScreenWidth - 1 - lw;
    display.setTextColor(topoDim(ap.isOpen ? kWarn : kForeground, pct), kBackground);
    display.setCursor(lx, ly);
    display.print(label);

    topoNodeX[shown] = ax;
    topoNodeY[shown] = ay;
    memcpy(topoNodeBssid[shown], ap.bssid, 6);
    ++shown;
  }
  topoNodeCount = shown;

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

  if (topoHasSel) {
    int si = -1;
    for (int i = 0; i < topoApCount; ++i) {
      if (memcmp(topoAps[i].bssid, topoSelBssid, 6) == 0) { si = i; break; }
    }
    if (si < 0) topoHasSel = false;
    else drawTopoPopup(topoAps[si]);
  }
}
