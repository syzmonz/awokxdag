// AWOKxDAG — device settings (GPS baud, backlight). Stored in NVS separately
// from saved networks. Reached from Status → Settings.

const uint32_t kBacklightTimeoutOptions[] = {0, 15000, 30000, 60000, 120000,
                                             300000};
constexpr int kBacklightTimeoutOptionCount = static_cast<int>(
    sizeof(kBacklightTimeoutOptions) / sizeof(kBacklightTimeoutOptions[0]));
const uint8_t kBrightnessOptions[] = {20, 40, 60, 80, 100};
constexpr int kBrightnessOptionCount =
    static_cast<int>(sizeof(kBrightnessOptions) / sizeof(kBrightnessOptions[0]));
const uint16_t kBatteryCapacityOptions[] = {500, 1000, 1500, 2000, 2500,
                                            3000, 4000, 5000};
constexpr int kBatteryCapacityOptionCount = static_cast<int>(
    sizeof(kBatteryCapacityOptions) / sizeof(kBatteryCapacityOptions[0]));
constexpr uint16_t kBatteryCapacityDefaultMah = 2000;
const uint8_t kBatteryTuneOptions[] = {50, 55, 60, 65, 70, 75, 80, 85, 90, 95,
                                       100, 105, 110, 115, 120, 125, 130, 135,
                                       140, 145, 150};
constexpr int kBatteryTuneOptionCount = static_cast<int>(
    sizeof(kBatteryTuneOptions) / sizeof(kBatteryTuneOptions[0]));
constexpr uint8_t kBatteryTuneDefault = 100;
constexpr int kSettingsRow0 = 48;
constexpr int kSettingsRowH = 28;
constexpr int kSettingsPitch = 30;
constexpr int kSettingsPageCount = 2;
int settingsPage = 0;

enum SettingItem : uint8_t {
  kItemBright, kItemGps, kItemSplash, kItemActive, kItemNmea,
  kItemSleep, kItemScreenTest, kItemCapacity, kItemTune
};
const uint8_t kSettingsPage0[] = {kItemBright, kItemGps, kItemSplash,
                                 kItemActive, kItemNmea, kItemCapacity,
                                 kItemTune};
const uint8_t kSettingsPage1[] = {kItemSleep, kItemScreenTest};
constexpr int kSettingsPage0Count =
    static_cast<int>(sizeof(kSettingsPage0) / sizeof(kSettingsPage0[0]));
constexpr int kSettingsPage1Count =
    static_cast<int>(sizeof(kSettingsPage1) / sizeof(kSettingsPage1[0]));

const uint8_t* settingsPageItems(int page, int& count) {
  if (page == 1) {
    count = kSettingsPage1Count;
    return kSettingsPage1;
  }
  count = kSettingsPage0Count;
  return kSettingsPage0;
}
char attackConfirmLabel[24] = {};
uint32_t attackConfirmMs = 0;

bool gpsBaudIsKnown(unsigned long baud) {
  for (int i = 0; i < kGpsBaudOptionCount; ++i) {
    if (kGpsBaudOptions[i] == baud) return true;
  }
  return false;
}

bool settingFlag(uint8_t bit) { return (deviceSettings.flags & bit) != 0; }

void setSettingFlag(uint8_t bit, bool on) {
  if (on) deviceSettings.flags |= bit;
  else deviceSettings.flags = static_cast<uint8_t>(deviceSettings.flags & ~bit);
}

const char* backlightTimeoutLabel(uint32_t ms) {
  if (ms == 0) return "Off";
  if (ms == 15000) return "15s";
  if (ms == 30000) return "30s";
  if (ms == 60000) return "1m";
  if (ms == 120000) return "2m";
  if (ms == 300000) return "5m";
  return "?";
}

bool confirmActiveTest(const char* label) {
  if (!settingFlag(kSettingConfirmAttacks)) return true;
  if (attackConfirmLabel[0] && strcmp(attackConfirmLabel, label) == 0 &&
      millis() - attackConfirmMs < 4000) {
    attackConfirmLabel[0] = 0;
    return true;
  }
  strncpy(attackConfirmLabel, label, sizeof(attackConfirmLabel) - 1);
  attackConfirmLabel[sizeof(attackConfirmLabel) - 1] = 0;
  attackConfirmMs = millis();
  Serial.printf("[settings] tap again to start %s\n", label);
  return false;
}

void drawConfirmBanner() {
  if (!attackConfirmLabel[0]) return;
  if (millis() - attackConfirmMs >= 4000) {
    attackConfirmLabel[0] = 0;
    return;
  }
  display.setTextSize(1);
  display.setTextColor(kBad, kBackground);
  display.setCursor(6, 250);
  display.print("Tap again: ");
  display.print(attackConfirmLabel);
}

void deviceSettingsDefaults(DeviceSettingsRecord& out) {
  out = {};
  out.version = kDeviceSettingsVersion;
  out.gpsBaud = AwokPins::kGpsBaud;
  out.backlightTimeoutMs = 0;
  out.brightnessPercent = 100;
  out.batteryCapacityMah = kBatteryCapacityDefaultMah;
  out.batteryTunePercent = kBatteryTuneDefault;
}

void writeBacklightPercent(int percent) {
  const int pin = AwokPins::kBacklight;
  pinMode(pin, OUTPUT);
  // Full on/off stay on GPIO so LEDC is not attached at boot (it fragments
  // the large internal heap block Wi-Fi needs).
  if (percent <= 0) {
    digitalWrite(pin, AwokPins::kBacklightOn ? LOW : HIGH);
    return;
  }
  if (percent >= 100) {
    digitalWrite(pin, AwokPins::kBacklightOn ? HIGH : LOW);
    return;
  }
  const int duty = constrain(percent, 20, 99) * 255 / 100;
  analogWrite(pin, AwokPins::kBacklightOn ? duty : (255 - duty));
}

void setBacklightLit(bool on) {
  backlightDimmed = !on;
  writeBacklightPercent(on ? deviceSettings.brightnessPercent : 0);
}

void noteActivity() {
  lastActivityMs = millis();
  if (backlightDimmed) setBacklightLit(true);
}

void updateBacklightSleep() {
  if (currentView == View::kScreenTest) return;
  if (deviceSettings.backlightTimeoutMs == 0 || backlightDimmed) return;
  if (millis() - lastActivityMs >= deviceSettings.backlightTimeoutMs) {
    setBacklightLit(false);
  }
}

bool saveDeviceSettings() {
  Preferences preferences;
  if (!preferences.begin("awokxdag", false)) {
    lastSettingsWriteOk = false;
    return false;
  }
  deviceSettings.version = kDeviceSettingsVersion;
  const bool written =
      preferences.putBytes("settings", &deviceSettings, sizeof(deviceSettings)) ==
      sizeof(deviceSettings);
  preferences.end();
  lastSettingsWriteOk = written;
  if (written) {
    recordFirmwareAudit(
        "configuration", "settings", "success",
        "gps_baud=" + String(deviceSettings.gpsBaud) +
            "; backlight_ms=" + String(deviceSettings.backlightTimeoutMs) +
            "; brightness=" + String(deviceSettings.brightnessPercent) +
            "; flags=" + String(deviceSettings.flags));
  } else {
    recordFirmwareAudit("configuration", "settings", "failed",
                        "persistent storage write failed");
  }
  return written;
}

void loadDeviceSettings() {
  deviceSettingsDefaults(deviceSettings);
  lastSettingsWriteOk = true;
  Preferences preferences;
  if (!preferences.begin("awokxdag", true)) return;
  if (!preferences.isKey("settings")) {
    preferences.end();
    return;
  }
  DeviceSettingsRecord loaded = {};
  const bool ok =
      preferences.getBytesLength("settings") == sizeof(loaded) &&
      preferences.getBytes("settings", &loaded, sizeof(loaded)) == sizeof(loaded) &&
      loaded.version == kDeviceSettingsVersion;
  preferences.end();
  if (!ok) {
    Serial.println("[settings] invalid NVS record; using defaults");
    lastSettingsWriteOk = false;
    return;
  }
  if (!gpsBaudIsKnown(loaded.gpsBaud)) loaded.gpsBaud = AwokPins::kGpsBaud;
  bool timeoutOk = false;
  for (int i = 0; i < kBacklightTimeoutOptionCount; ++i) {
    if (kBacklightTimeoutOptions[i] == loaded.backlightTimeoutMs) timeoutOk = true;
  }
  if (!timeoutOk) loaded.backlightTimeoutMs = 0;
  if (loaded.brightnessPercent < 20 || loaded.brightnessPercent > 100) {
    loaded.brightnessPercent = 100;
  }
  bool capOk = false;
  for (int i = 0; i < kBatteryCapacityOptionCount; ++i) {
    if (kBatteryCapacityOptions[i] == loaded.batteryCapacityMah) capOk = true;
  }
  if (!capOk) loaded.batteryCapacityMah = kBatteryCapacityDefaultMah;
  bool tuneOk = false;
  for (int i = 0; i < kBatteryTuneOptionCount; ++i) {
    if (kBatteryTuneOptions[i] == loaded.batteryTunePercent) tuneOk = true;
  }
  if (!tuneOk) loaded.batteryTunePercent = kBatteryTuneDefault;
  deviceSettings = loaded;
  gpsRawEcho = settingFlag(kSettingNmeaEcho);
  Serial.printf("[settings] baud=%lu timeout=%lums brightness=%u%% flags=0x%02x\n",
                deviceSettings.gpsBaud,
                static_cast<unsigned long>(deviceSettings.backlightTimeoutMs),
                unsigned(deviceSettings.brightnessPercent),
                unsigned(deviceSettings.flags));
}

void resetDeviceSettings() {
  deviceSettingsDefaults(deviceSettings);
  gpsRawEcho = false;
  attackConfirmLabel[0] = 0;
  applyGpsBaud(deviceSettings.gpsBaud, false);
  setBacklightLit(true);
  saveDeviceSettings();
}

void toggleSettingFlag(uint8_t bit) {
  setSettingFlag(bit, !settingFlag(bit));
  if (bit == kSettingNmeaEcho) gpsRawEcho = settingFlag(kSettingNmeaEcho);
  saveDeviceSettings();
}

String settingsItemLabel(uint8_t item) {
  switch (item) {
    case kItemSleep:
      return String("Sleep  ") +
             backlightTimeoutLabel(deviceSettings.backlightTimeoutMs);
    case kItemBright:
      return "Bright  " + String(deviceSettings.brightnessPercent) + "%";
    case kItemGps:
      return "GPS  " + String(deviceSettings.gpsBaud);
    case kItemSplash:
      return settingFlag(kSettingSkipSplash) ? "Splash  Off" : "Splash  On";
    case kItemActive:
      return settingFlag(kSettingConfirmAttacks) ? "Active  Confirm"
                                                 : "Active  Instant";
    case kItemNmea:
      return settingFlag(kSettingNmeaEcho) ? "NMEA  On" : "NMEA  Off";
    case kItemCapacity:
      return "Batt  " + String(deviceSettings.batteryCapacityMah) + "mAh";
    case kItemTune:
      return "Batt Tune  " + String(deviceSettings.batteryTunePercent) + "%";
    default:
      return "Screen test";
  }
}

void cycleBacklightTimeout() {
  int index = 0;
  for (int i = 0; i < kBacklightTimeoutOptionCount; ++i) {
    if (kBacklightTimeoutOptions[i] == deviceSettings.backlightTimeoutMs) {
      index = i;
      break;
    }
  }
  deviceSettings.backlightTimeoutMs =
      kBacklightTimeoutOptions[(index + 1) % kBacklightTimeoutOptionCount];
  noteActivity();
  setBacklightLit(true);
  saveDeviceSettings();
}

void cycleBrightness() {
  int index = kBrightnessOptionCount - 1;
  for (int i = 0; i < kBrightnessOptionCount; ++i) {
    if (kBrightnessOptions[i] == deviceSettings.brightnessPercent) {
      index = i;
      break;
    }
  }
  deviceSettings.brightnessPercent =
      kBrightnessOptions[(index + 1) % kBrightnessOptionCount];
  noteActivity();
  setBacklightLit(true);
  saveDeviceSettings();
}

void cycleBatteryCapacity() {
  int index = 0;
  for (int i = 0; i < kBatteryCapacityOptionCount; ++i) {
    if (kBatteryCapacityOptions[i] == deviceSettings.batteryCapacityMah) {
      index = i;
      break;
    }
  }
  deviceSettings.batteryCapacityMah =
      kBatteryCapacityOptions[(index + 1) % kBatteryCapacityOptionCount];
  resetBatteryEstimate();
  saveDeviceSettings();
}

void cycleBatteryTune() {
  int index = 0;
  for (int i = 0; i < kBatteryTuneOptionCount; ++i) {
    if (kBatteryTuneOptions[i] == deviceSettings.batteryTunePercent) {
      index = i;
      break;
    }
  }
  deviceSettings.batteryTunePercent =
      kBatteryTuneOptions[(index + 1) % kBatteryTuneOptionCount];
  saveDeviceSettings();
}

void handleSettingsItem(uint8_t item) {
  switch (item) {
    case kItemSleep: cycleBacklightTimeout(); drawSettings(); break;
    case kItemBright: cycleBrightness(); drawSettings(); break;
    case kItemGps: cycleGpsBaud(); drawSettings(); break;
    case kItemSplash: toggleSettingFlag(kSettingSkipSplash); drawSettings(); break;
    case kItemActive: toggleSettingFlag(kSettingConfirmAttacks); drawSettings(); break;
    case kItemNmea: toggleSettingFlag(kSettingNmeaEcho); drawSettings(); break;
    case kItemCapacity: cycleBatteryCapacity(); drawSettings(); break;
    case kItemTune: cycleBatteryTune(); drawSettings(); break;
    default: startScreenTest(); break;
  }
}

void drawSettings() {
  currentView = View::kSettings;
  display.fillScreen(kBackground);
  String detail = String(settingsPage + 1) + "/" + String(kSettingsPageCount) +
                  (lastSettingsWriteOk ? "  stored" : "  RAM only");
  drawHeader("SETTINGS", detail);
  int count = 0;
  const uint8_t* items = settingsPageItems(settingsPage, count);
  for (int row = 0; row < count; ++row) {
    drawSmallButton(20, kSettingsRow0 + row * kSettingsPitch, 200, kSettingsRowH,
                    settingsItemLabel(items[row]), kAccent);
  }
  drawFourButtonFooter("Back", "<", ">", "Defaults");
}

void handleSettingsTouch(int x, int y) {
  if (y >= kFooterTop) {
    if (x < 60) {
      drawStatus();
    } else if (x < 120) {
      settingsPage = (settingsPage + kSettingsPageCount - 1) % kSettingsPageCount;
      drawSettings();
    } else if (x < 180) {
      settingsPage = (settingsPage + 1) % kSettingsPageCount;
      drawSettings();
    } else {
      resetDeviceSettings();
      settingsPage = 0;
      drawSettings();
    }
    return;
  }
  if (y < kSettingsRow0) return;
  const int row = (y - kSettingsRow0) / kSettingsPitch;
  if ((y - kSettingsRow0) % kSettingsPitch >= kSettingsRowH) return;
  int count = 0;
  const uint8_t* items = settingsPageItems(settingsPage, count);
  if (row < 0 || row >= count) return;
  handleSettingsItem(items[row]);
}
