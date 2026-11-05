#include <math.h>

struct WardrivePoint {
  float lat;
  float lon;
  uint8_t sec;
  int8_t rssi;
};

constexpr int kWardriveMapMaxPoints = 300;
constexpr uint32_t kWardriveMapRedrawMs = 700;
constexpr int kWdmMapTop = kHeaderHeight;
constexpr int kWdmMapBottom = 256;
constexpr int kWdmStatusY = 270;
constexpr int kWdmCx = kScreenWidth / 2;
constexpr int kWdmCy = (kWdmMapTop + kWdmMapBottom) / 2;
constexpr uint16_t kWdmWpa = 0x051F;

static const float kWdmSpanOptions[] = {0.0f, 100.0f, 250.0f, 500.0f, 1000.0f, 2500.0f};
constexpr int kWdmSpanCount = (int)(sizeof(kWdmSpanOptions) / sizeof(kWdmSpanOptions[0]));

static WardrivePoint wdmPoints[kWardriveMapMaxPoints];
static int wdmCount = 0;
static int wdmHead = 0;
static int wdmZoomIdx = 0;
static float wdmCentreLat = 0.0f;
static float wdmCentreLon = 0.0f;
static bool wdmHaveCentre = false;
static uint32_t lastWdmDrawMs = 0;

void wardriveMapCapture(float lat, float lon, uint8_t sec, int8_t rssi) {
  wdmPoints[wdmHead].lat = lat;
  wdmPoints[wdmHead].lon = lon;
  wdmPoints[wdmHead].sec = sec;
  wdmPoints[wdmHead].rssi = rssi;
  wdmHead = (wdmHead + 1) % kWardriveMapMaxPoints;
  if (wdmCount < kWardriveMapMaxPoints) ++wdmCount;
  wdmCentreLat = lat;
  wdmCentreLon = lon;
  wdmHaveCentre = true;
}

void wardriveMapClear() {
  wdmCount = 0;
  wdmHead = 0;
}

void wardriveMapZoomCycle() {
  wdmZoomIdx = (wdmZoomIdx + 1) % kWdmSpanCount;
}

static uint16_t wdmSecColor(uint8_t sec) {
  switch (sec) {
    case 0: return kBad;
    case 1: return kWarn;
    case 2: return kWdmWpa;
    default: return kGood;
  }
}

static float wdmNiceNumber(float v) {
  if (v <= 0.0f) return 1.0f;
  float mag = powf(10.0f, floorf(log10f(v)));
  float f = v / mag;
  float nf = f < 1.5f ? 1.0f : f < 3.5f ? 2.0f : f < 7.5f ? 5.0f : 10.0f;
  return nf * mag;
}

void drawWardriveMap() {
  currentView = View::kWardriveMap;
  display.fillScreen(kBackground);

  const bool fix = gpsHasFix();
  if (fix) {
    wdmCentreLat = (float)gps.location.lat();
    wdmCentreLon = (float)gps.location.lng();
    wdmHaveCentre = true;
  }

  drawHeader("WARDRIVE MAP", fix ? "fix 3D" : "fix --");

  for (int gx = 40; gx < kScreenWidth; gx += 40)
    display.drawFastVLine(gx, kWdmMapTop, kWdmMapBottom - kWdmMapTop, 0x1082);
  for (int gy = kWdmMapTop + 40; gy < kWdmMapBottom; gy += 40)
    display.drawFastHLine(0, gy, kScreenWidth, 0x1082);

  if (wdmCount == 0 || !wdmHaveCentre) {
    display.setTextColor(kMuted, kBackground);
    display.setTextSize(1);
    display.setCursor(56, kWdmCy - 12);
    display.print("No points yet");
    display.setCursor(40, kWdmCy + 4);
    display.print("Start Wardrive to log");
    drawThreeButtonFooter("Back", "Zoom", "Clear");
    return;
  }

  const float cLat = wdmCentreLat;
  const float cLon = wdmCentreLon;
  const float mPerDegLat = 111320.0f;
  const float mPerDegLon = 111320.0f * cosf(cLat * (float)M_PI / 180.0f);

  float spanM = kWdmSpanOptions[wdmZoomIdx];
  if (spanM <= 0.0f) {
    float maxAbs = 0.0f;
    for (int i = 0; i < wdmCount; ++i) {
      const float dxm = (wdmPoints[i].lon - cLon) * mPerDegLon;
      const float dym = (wdmPoints[i].lat - cLat) * mPerDegLat;
      const float ax = fabsf(dxm);
      const float ay = fabsf(dym);
      if (ax > maxAbs) maxAbs = ax;
      if (ay > maxAbs) maxAbs = ay;
    }
    spanM = maxAbs * 2.3f;
    if (spanM < 60.0f) spanM = 60.0f;
  }

  const int mapH = kWdmMapBottom - kWdmMapTop;
  const float ppm = (float)mapH / spanM;

  for (int i = 0; i < wdmCount; ++i) {
    const float dxm = (wdmPoints[i].lon - cLon) * mPerDegLon;
    const float dym = (wdmPoints[i].lat - cLat) * mPerDegLat;
    const int sx = kWdmCx + (int)(dxm * ppm + (dxm >= 0 ? 0.5f : -0.5f));
    const int sy = kWdmCy - (int)(dym * ppm + (dym >= 0 ? 0.5f : -0.5f));
    if (sx < 2 || sx > kScreenWidth - 2 || sy < kWdmMapTop + 2 || sy > kWdmMapBottom - 2)
      continue;
    const int8_t r = wdmPoints[i].rssi;
    const int rad = r > -55 ? 4 : r > -70 ? 3 : 2;
    const uint16_t c = wdmSecColor(wdmPoints[i].sec);
    display.fillCircle(sx, sy, rad, c);
    if (wdmPoints[i].sec == 0) display.drawCircle(sx, sy, rad + 2, kWarn);
  }

  display.drawFastVLine(kWdmCx, kWdmCy - 6, 13, kAccent);
  display.drawFastHLine(kWdmCx - 6, kWdmCy, 13, kAccent);
  display.drawCircle(kWdmCx, kWdmCy, 5, kAccent);

  float barM = wdmNiceNumber(spanM / 4.0f);
  int barPx = (int)(barM * ppm + 0.5f);
  if (barPx > 100) { barM = wdmNiceNumber(spanM / 8.0f); barPx = (int)(barM * ppm + 0.5f); }
  const int barX = kScreenWidth - 16 - barPx;
  const int barY = kWdmMapBottom - 10;
  display.drawFastHLine(barX, barY, barPx, kMuted);
  display.drawFastVLine(barX, barY - 3, 6, kMuted);
  display.drawFastVLine(barX + barPx, barY - 3, 6, kMuted);
  display.setTextColor(kMuted, kBackground);
  display.setTextSize(1);
  display.setCursor(barX, barY - 14);
  if (barM >= 1000.0f) display.printf("%.1f km", barM / 1000.0f);
  else display.printf("%d m", (int)barM);

  display.setTextSize(1);
  display.setTextColor(kForeground, kBackground);
  display.setCursor(6, kWdmStatusY);
  display.printf("Nets %lu", (unsigned long)wdmCount);
  display.setTextColor(fix ? kGood : kMuted, kBackground);
  display.setCursor(96, kWdmStatusY);
  display.print(fix ? "Fix 3D" : "Fix --");
  display.setTextColor(kMuted, kBackground);
  display.setCursor(168, kWdmStatusY);
  display.printf("Sats %u", (unsigned)gpsSats());

  drawThreeButtonFooter("Back", "Zoom", "Clear");
}

void updateWardriveMap() {
  if (currentView != View::kWardriveMap) return;
  const uint32_t now = millis();
  if (now - lastWdmDrawMs < kWardriveMapRedrawMs) return;
  lastWdmDrawMs = now;
  drawWardriveMap();
}

void startWardriveMap() {
  wdmZoomIdx = 0;
  lastWdmDrawMs = 0;
  drawWardriveMap();
}
