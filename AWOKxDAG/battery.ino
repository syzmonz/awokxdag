constexpr float kCurrentIdleMa = 150.0f;
constexpr float kCurrentScanMa = 300.0f;
constexpr float kCurrentTxMa = 480.0f;
constexpr float kBacklightFullMa = 60.0f;
constexpr float kBoostSagMax = 0.35f;
constexpr uint32_t kBatterySaveIntervalMs = 60000;
constexpr float kBatterySaveDeltaMah = 1.0f;
constexpr float kBatteryDefaultCapacityMah = 2000.0f;
constexpr char kBatteryNvsNamespace[] = "axd_batt";
constexpr uint32_t kBatteryNvsVersion = 2;

enum : uint8_t { kPowerIdle, kPowerScan, kPowerTx };

struct BatteryNvsRecord {
  uint32_t version;
  float consumedMah;
  float activeSec;
};

double batteryConsumedMah = 0.0;
double batteryActiveSec = 0.0;
uint32_t lastBatteryTickMs = 0;
uint32_t lastBatterySaveMs = 0;
float lastSavedConsumedMah = 0.0f;

float batteryCapacityMah() {
  float c = static_cast<float>(deviceSettings.batteryCapacityMah);
  return c > 0.0f ? c : kBatteryDefaultCapacityMah;
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
  return base + batteryBacklightMa();
}

float batterySagFactor(float consumedMah) {
  float soc = 1.0f - consumedMah / batteryCapacityMah();
  if (soc < 0.0f) soc = 0.0f;
  if (soc > 1.0f) soc = 1.0f;
  return 1.0f + kBoostSagMax * (1.0f - soc);
}

void saveBatteryEstimate() {
  Preferences p;
  if (!p.begin(kBatteryNvsNamespace, false)) return;
  BatteryNvsRecord rec{kBatteryNvsVersion,
                       static_cast<float>(batteryConsumedMah),
                       static_cast<float>(batteryActiveSec)};
  p.putBytes("batt", &rec, sizeof(rec));
  p.end();
  lastSavedConsumedMah = static_cast<float>(batteryConsumedMah);
  lastBatterySaveMs = millis();
}

void loadBatteryEstimate() {
  batteryConsumedMah = 0.0;
  batteryActiveSec = 0.0;
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
      float a = rec.activeSec;
      if (a < 0.0f) a = 0.0f;
      batteryConsumedMah = c;
      batteryActiveSec = a;
      lastSavedConsumedMah = c;
    }
  }
  p.end();
}

void resetBatteryEstimate() {
  batteryConsumedMah = 0.0;
  batteryActiveSec = 0.0;
  lastBatteryTickMs = millis();
  saveBatteryEstimate();
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
  batteryActiveSec += dt / 1000.0;

  if (now - lastBatterySaveMs >= kBatterySaveIntervalMs &&
      fabsf(static_cast<float>(batteryConsumedMah) - lastSavedConsumedMah) >=
          kBatterySaveDeltaMah) {
    saveBatteryEstimate();
  }
}

String batteryEstimateString(int& pctOut) {
  const float cap = batteryCapacityMah();
  float remaining = cap - static_cast<float>(batteryConsumedMah);
  if (remaining < 0.0f) remaining = 0.0f;
  int pct = static_cast<int>(remaining / cap * 100.0f + 0.5f);
  pct = (pct + 5) / 10 * 10;
  if (pct > 100) pct = 100;
  pctOut = pct;

  float avgMa = kCurrentIdleMa;
  if (batteryActiveSec > 30.0 && batteryConsumedMah > 0.0) {
    avgMa = static_cast<float>(batteryConsumedMah /
                               (batteryActiveSec / 3600.0));
  }
  if (avgMa < 1.0f) avgMa = 1.0f;
  int minsLeft = static_cast<int>(remaining / avgMa * 60.0f);
  if (minsLeft < 0) minsLeft = 0;
  minsLeft = (minsLeft + 7) / 15 * 15;
  if (minsLeft > 5999) minsLeft = 5999;

  char buf[48];
  if (minsLeft >= 60) {
    snprintf(buf, sizeof(buf), "~%dh%02dm (est, %d%%)", minsLeft / 60,
             minsLeft % 60, pct);
  } else {
    snprintf(buf, sizeof(buf), "~%dm (est, %d%%)", minsLeft, pct);
  }
  return String(buf);
}
