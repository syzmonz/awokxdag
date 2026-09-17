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
constexpr int kSettingsRow0 = 48;
constexpr int kSettingsRowH = 26;
constexpr int kSettingsPitch = 28;
constexpr int kSettingsRows = 8;
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

String settingsRowLabel(int row) {
  switch (row) {
    case 0:
      return String("Sleep  ") +
             backlightTimeoutLabel(deviceSettings.backlightTimeoutMs);
    case 1:
      return "Bright  " + String(deviceSettings.brightnessPercent) + "%";
    case 2:
      return "GPS  " + String(deviceSettings.gpsBaud);
    case 3:
      return settingFlag(kSettingSkipSplash) ? "Splash  Off" : "Splash  On";
    case 4:
      return settingFlag(kSettingConfirmAttacks) ? "Active  Confirm"
                                                 : "Active  Instant";
    case 5:
      return settingFlag(kSettingNmeaEcho) ? "NMEA  On" : "NMEA  Off";
    case 6:
      return "Batt  " + String(deviceSettings.batteryCapacityMah) + "mAh";
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

void drawSettings() {
  currentView = View::kSettings;
  display.fillScreen(kBackground);
  drawHeader("SETTINGS", lastSettingsWriteOk ? "stored on this device"
                                             : "save failed; RAM only");
  for (int row = 0; row < kSettingsRows; ++row) {
    drawSmallButton(20, kSettingsRow0 + row * kSettingsPitch, 200, kSettingsRowH,
                    settingsRowLabel(row), kAccent);
  }
  drawFooter("Back", "Defaults");
}

void handleSettingsTouch(int x, int y) {
  if (y >= kFooterTop) {
    if (x < kScreenWidth / 2) {
      drawStatus();
    } else {
      resetDeviceSettings();
      drawSettings();
    }
    return;
  }
  if (y < kSettingsRow0) return;
  const int row = (y - kSettingsRow0) / kSettingsPitch;
  if (row < 0 || row >= kSettingsRows) return;
  if ((y - kSettingsRow0) % kSettingsPitch >= kSettingsRowH) return;
  switch (row) {
    case 0:
      cycleBacklightTimeout();
      drawSettings();
      break;
    case 1:
      cycleBrightness();
      drawSettings();
      break;
    case 2:
      cycleGpsBaud();
      drawSettings();
      break;
    case 3:
      toggleSettingFlag(kSettingSkipSplash);
      drawSettings();
      break;
    case 4:
      toggleSettingFlag(kSettingConfirmAttacks);
      drawSettings();
      break;
    case 5:
      toggleSettingFlag(kSettingNmeaEcho);
      drawSettings();
      break;
    case 6:
      cycleBatteryCapacity();
      drawSettings();
      break;
    default:
      startScreenTest();
      break;
  }
}
