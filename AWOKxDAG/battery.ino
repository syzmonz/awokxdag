constexpr float kBatteryCapacityMah = 1000.0f;
constexpr float kCurrentIdleMa = 150.0f;
constexpr float kCurrentScanMa = 300.0f;
constexpr float kCurrentTxMa = 480.0f;
constexpr float kBacklightFullMa = 60.0f;
constexpr float kBoostSagMax = 0.35f;
constexpr float kCurrentAvgTauMs = 30000.0f;
constexpr uint32_t kBatterySaveIntervalMs = 60000;
constexpr float kBatterySaveDeltaMah = 1.0f;
constexpr char kBatteryNvsNamespace[] = "axd_batt";
constexpr uint32_t kBatteryNvsVersion = 1;

enum class PowerMode : uint8_t { kIdle, kScan, kTx };

struct BatteryNvsRecord {
  uint32_t version;
  float consumedMah;
};

double batteryConsumedMah = 0.0;
float batteryAvgCurrentMa = kCurrentIdleMa;
uint32_t lastBatteryTickMs = 0;
uint32_t lastBatterySaveMs = 0;
float lastSavedConsumedMah = 0.0f;

PowerMode batteryModeForView(View v) {
  switch (v) {
    case View::kDeauthAttack:
    case View::kBeaconFlood:
    case View::kEvilPortal:
    case View::kProbeLure:
      return PowerMode::kTx;
    case View::kHome:
    case View::kSaved:
    case View::kStatus:
    case View::kSettings:
    case View::kScreenTest:
    case View::kFiles:
    case View::kNetworkMenu:
    case View::kNetworkSetup:
    case View::kNetworkEdit:
      return PowerMode::kIdle;
    default:
      return PowerMode::kScan;
  }
}

float batteryBaseCurrentMa(PowerMode m) {
  switch (m) {
    case PowerMode::kTx: return kCurrentTxMa;
    case PowerMode::kScan: return kCurrentScanMa;
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

float batteryPresentCurrentMa(PowerMode m) {
  float base = batteryBaseCurrentMa(m) - kBacklightFullMa;
  if (base < 0.0f) base = 0.0f;
  return base + batteryBacklightMa();
}

float batterySagFactor(float consumedMah) {
  float soc = 1.0f - consumedMah / kBatteryCapacityMah;
  if (soc < 0.0f) soc = 0.0f;
  if (soc > 1.0f) soc = 1.0f;
  return 1.0f + kBoostSagMax * (1.0f - soc);
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
  batteryAvgCurrentMa = kCurrentIdleMa;
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
      if (c > kBatteryCapacityMah) c = kBatteryCapacityMah;
      batteryConsumedMah = c;
      lastSavedConsumedMah = c;
    }
  }
  p.end();
}

void resetBatteryEstimate() {
  batteryConsumedMah = 0.0;
  batteryAvgCurrentMa = kCurrentIdleMa;
  lastBatteryTickMs = millis();
  saveBatteryEstimate();
}

void updateBatteryEstimate() {
  if (kBatteryCapacityMah <= 0.0f) return;
  const uint32_t now = millis();
  const uint32_t dt = now - lastBatteryTickMs;
  lastBatteryTickMs = now;
  if (dt == 0 || dt > 10000) return;

  const float ma = batteryPresentCurrentMa(batteryModeForView(currentView));
  const float sag = batterySagFactor(static_cast<float>(batteryConsumedMah));
  const float drain = ma * sag;
  batteryConsumedMah += drain * (dt / 3600000.0);
  if (batteryConsumedMah > kBatteryCapacityMah) {
    batteryConsumedMah = kBatteryCapacityMah;
  }

  const float alpha = dt / (kCurrentAvgTauMs + dt);
  batteryAvgCurrentMa += alpha * (drain - batteryAvgCurrentMa);

  if (now - lastBatterySaveMs >= kBatterySaveIntervalMs &&
      fabsf(static_cast<float>(batteryConsumedMah) - lastSavedConsumedMah) >=
          kBatterySaveDeltaMah) {
    saveBatteryEstimate();
  }
}

String batteryEstimateString(int& pctOut) {
  if (kBatteryCapacityMah <= 0.0f) {
    pctOut = -1;
    return String();
  }
  float remaining = kBatteryCapacityMah - static_cast<float>(batteryConsumedMah);
  if (remaining < 0.0f) remaining = 0.0f;
  int pct = static_cast<int>(remaining / kBatteryCapacityMah * 100.0f + 0.5f);
  pct = (pct + 5) / 10 * 10;
  if (pct > 100) pct = 100;
  pctOut = pct;

  const float ma = batteryAvgCurrentMa > 1.0f ? batteryAvgCurrentMa : 1.0f;
  int minsLeft = static_cast<int>(remaining / ma * 60.0f);
  if (minsLeft < 0) minsLeft = 0;
  minsLeft = (minsLeft + 7) / 15 * 15;

  char buf[24];
  if (minsLeft >= 60) {
    snprintf(buf, sizeof(buf), "~%dh%02dm (est, %d%%)", minsLeft / 60,
             minsLeft % 60, pct);
  } else {
    snprintf(buf, sizeof(buf), "~%dm (est, %d%%)", minsLeft, pct);
  }
  return String(buf);
}
