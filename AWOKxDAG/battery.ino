constexpr float kCurrentIdleMa = 150.0f;
constexpr float kCurrentScanMa = 300.0f;
constexpr float kCurrentTxMa = 480.0f;
constexpr float kBacklightFullMa = 60.0f;
constexpr float kBoostSagMax = 0.35f;
constexpr uint32_t kBatterySaveIntervalMs = 60000;
constexpr float kBatterySaveDeltaMah = 1.0f;
constexpr float kBatteryDefaultCapacityMah = 2000.0f;
constexpr int kBatteryLowPercent = 10;
constexpr uint32_t kBatteryBannerMs = 5000;
constexpr char kBatteryNvsNamespace[] = "axd_batt";
constexpr uint32_t kBatteryNvsVersion = 3;

enum : uint8_t { kPowerIdle, kPowerScan, kPowerTx };

struct BatteryNvsRecord {
  uint32_t version;
  float consumedMah;
};

double batteryConsumedMah = 0.0;
uint32_t lastBatteryTickMs = 0;
uint32_t lastBatterySaveMs = 0;
float lastSavedConsumedMah = 0.0f;
bool batteryLowLatched = false;
bool batteryBannerActive = false;
uint32_t batteryBannerMs = 0;
bool batteryLimitEnforced = false;

float batteryCapacityMah() {
  float c = static_cast<float>(deviceSettings.batteryCapacityMah);
  return c > 0.0f ? c : kBatteryDefaultCapacityMah;
}

float batteryTuneScale() {
  int t = deviceSettings.batteryTunePercent;
  if (t < 50 || t > 150) t = 100;
  return t / 100.0f;
}

uint8_t batteryModeForView(View v) {
  switch (v) {
    case View::kDeauthAttack:
    case View::kBeaconFlood:
    case View::kEvilPortal:
    case View::kProbeLure:
      return kPowerTx;
    case View::kHome:
    case View::kSaved:
    case View::kStatus:
    case View::kSettings:
    case View::kScreenTest:
    case View::kFiles:
    case View::kNetworkMenu:
    case View::kNetworkSetup:
    case View::kNetworkEdit:
      return kPowerIdle;
    default:
      return kPowerScan;
  }
}

float batteryBaseCurrentMa(uint8_t m) {
  switch (m) {
    case kPowerTx: return kCurrentTxMa;
    case kPowerScan: return kCurrentScanMa;
    default: return kCurrentIdleMa;
  }
}

float batteryBacklightMa() {
  if (backlightDimmed) return 0.0f;
  int b = deviceSettings.brightnessPercent;
  if (b < 0) b = 0;
  if (b > 100) b = 100;
  return kBacklightFullMa * (b / 100.0f);
}

float batteryPresentCurrentMa(uint8_t m) {
  float base = batteryBaseCurrentMa(m) - kBacklightFullMa;
  if (base < 0.0f) base = 0.0f;
  return (base + batteryBacklightMa()) * batteryTuneScale();
}

float batterySagFactor(float consumedMah) {
  float soc = 1.0f - consumedMah / batteryCapacityMah();
  if (soc < 0.0f) soc = 0.0f;
  if (soc > 1.0f) soc = 1.0f;
  return 1.0f + kBoostSagMax * (1.0f - soc);
}

int batteryPercentNow() {
  const float cap = batteryCapacityMah();
  float remaining = cap - static_cast<float>(batteryConsumedMah);
  if (remaining < 0.0f) remaining = 0.0f;
  int pct = static_cast<int>(remaining / cap * 100.0f + 0.5f);
  pct = (pct + 2) / 5 * 5;
  if (pct > 100) pct = 100;
  return pct;
}

void saveBatteryEstimate() {
  Preferences p;
  if (!p.begin(kBatteryNvsNamespace, false)) return;
  BatteryNvsRecord rec{kBatteryNvsVersion,
                       static_cast<float>(batteryConsumedMah)};
  p.putBytes("batt", &rec, sizeof(rec));
  p.end();
  lastSavedConsumedMah = static_cast<float>(batteryConsumedMah);
  lastBatterySaveMs = millis();
}

void loadBatteryEstimate() {
  batteryConsumedMah = 0.0;
  lastBatteryTickMs = millis();
  lastBatterySaveMs = millis();
  lastSavedConsumedMah = 0.0f;
  Preferences p;
  if (!p.begin(kBatteryNvsNamespace, true)) return;
  if (p.isKey("batt")) {
    BatteryNvsRecord rec{};
    if (p.getBytesLength("batt") == sizeof(rec) &&
        p.getBytes("batt", &rec, sizeof(rec)) == sizeof(rec) &&
        rec.version == kBatteryNvsVersion) {
      float c = rec.consumedMah;
      if (c < 0.0f) c = 0.0f;
      if (c > batteryCapacityMah()) c = batteryCapacityMah();
      batteryConsumedMah = c;
      lastSavedConsumedMah = c;
    }
  }
  p.end();
}

void resetBatteryEstimate() {
  batteryConsumedMah = 0.0;
  lastBatteryTickMs = millis();
  batteryLowLatched = false;
  batteryBannerActive = false;
  batteryLimitEnforced = false;
  saveBatteryEstimate();
}

void batteryEnforceLimit() {
  if (batteryPercentNow() > kBatteryLowPercent) {
    batteryLimitEnforced = false;
    return;
  }
  if (batteryLimitEnforced) return;
  batteryLimitEnforced = true;
  bool stopped = false;
  if (deauthAttackActive) { stopDeauthAttack(); stopped = true; }
  if (beaconFloodActive) { stopBeaconFlood(); stopped = true; }
  if (evilPortalActive) { stopEvilPortal(); stopped = true; }
  if (probeLureActive) { stopProbeLure(); stopped = true; }
  if (authFloodActive) { stopAuthFlood(); stopped = true; }
  if (stopped) drawStatus();
}

void updateBatteryEstimate() {
  const uint32_t now = millis();
  const uint32_t dt = now - lastBatteryTickMs;
  lastBatteryTickMs = now;
  if (dt == 0 || dt > 10000) return;

  const float ma = batteryPresentCurrentMa(batteryModeForView(currentView));
  const float sag = batterySagFactor(static_cast<float>(batteryConsumedMah));
  const float drain = ma * sag;
  batteryConsumedMah += drain * (dt / 3600000.0);
  const float cap = batteryCapacityMah();
  if (batteryConsumedMah > cap) batteryConsumedMah = cap;

  if (batteryPercentNow() <= kBatteryLowPercent) {
    if (!batteryLowLatched) {
      batteryLowLatched = true;
      batteryBannerActive = true;
      batteryBannerMs = now;
    }
  } else {
    batteryLowLatched = false;
  }
  batteryEnforceLimit();

  if (now - lastBatterySaveMs >= kBatterySaveIntervalMs &&
      fabsf(static_cast<float>(batteryConsumedMah) - lastSavedConsumedMah) >=
          kBatterySaveDeltaMah) {
    saveBatteryEstimate();
  }
}

void updateBatteryBanner() {
  if (!batteryBannerActive) return;
  if (millis() - batteryBannerMs >= kBatteryBannerMs) {
    batteryBannerActive = false;
    return;
  }
  display.fillRect(0, 150, kScreenWidth, 22, kBad);
  display.setTextSize(2);
  display.setTextColor(ILI9341_WHITE, kBad);
  display.setCursor(54, 153);
  display.print("LOW BATTERY");
  display.present(false);
}

String batteryEstimateString(int& pctOut) {
  int pct = batteryPercentNow();
  pctOut = pct;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  return String(buf);
}
