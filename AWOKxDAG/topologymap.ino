bool topoGraphMode = false;

constexpr uint32_t kTopoFreshMs = 3000;
constexpr uint32_t kTopoHideMs = 15000;
constexpr int kTopoSlots = 12;

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

uint16_t topoHeat(int rssi) {
  static const uint8_t stops[8][3] = {
    {10, 14, 39}, {22, 37, 122}, {31, 111, 178}, {34, 193, 195},
    {62, 196, 109}, {220, 214, 60}, {240, 150, 45}, {236, 60, 60}};
  int v = rssi;
  if (v < -90) v = -90;
  if (v > -30) v = -30;
  int t = ((v + 90) * 7 * 256) / 60;
  int seg = t >> 8;
  if (seg > 6) seg = 6;
  int f = t & 0xFF;
  int r = stops[seg][0] + ((stops[seg + 1][0] - stops[seg][0]) * f) / 256;
  int g = stops[seg][1] + ((stops[seg + 1][1] - stops[seg][1]) * f) / 256;
  int b = stops[seg][2] + ((stops[seg + 1][2] - stops[seg][2]) * f) / 256;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
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

  display.fillRect(0, cyTop, kScreenWidth, cyBot - cyTop, kBackground);

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

    int slot = topoSlotFor(ap.bssid);
    float ang = -1.5707963f + (6.2831853f * slot) / kTopoSlots;

    int rssi = ap.rssi;
    if (rssi < -90) rssi = -90;
    if (rssi > -40) rssi = -40;
    int rr = 46 + (-40 - rssi);

    int ax = cx + (int)(cosf(ang) * rr);
    int ay = cy + (int)(sinf(ang) * rr);
    if (ay < cyTop + 12) ay = cyTop + 12;
    if (ay > cyBot - 14) ay = cyBot - 14;

    uint16_t heat = topoDim(topoHeat(ap.rssi), pct);
    display.drawLine(cx, cy, ax, ay, heat);

    int drawn = 0;
    for (int c = 0; c < topoClientCount && drawn < 4; ++c) {
      if (!topoClients[c].hasBssid) continue;
      if (memcmp(topoClients[c].bssid, ap.bssid, 6) != 0) continue;
      int cpct = topoFreshPct(now, topoClients[c].lastSeenMs);
      if (cpct <= 0) continue;
      float ca = ang + (drawn - 1.5f) * 0.5f;
      int cxx = ax + (int)(cosf(ca) * 14);
      int cyy = ay + (int)(sinf(ca) * 14);
      if (cyy < cyTop + 3) cyy = cyTop + 3;
      if (cyy > cyBot - 3) cyy = cyBot - 3;
      uint16_t chc = topoDim(topoHeat(topoClients[c].rssi), cpct);
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
  display.setCursor(cx - 6, cy + 10);
  display.print("C5");
}
