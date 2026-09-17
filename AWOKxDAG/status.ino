// AWOKxDAG — system / status screen (compiled as part of the sketch; see
// awok_common.h)

uint32_t lastStatusDrawMs = 0;

String uptimeString() {
  const uint32_t s = millis() / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", static_cast<unsigned>(s / 3600),
           static_cast<unsigned>((s % 3600) / 60),
           static_cast<unsigned>(s % 60));
  return String(buf);
}

void drawStatus() {
  currentView = View::kStatus;
  display.fillScreen(kBackground);
  drawHeader("STATUS", "device health");
  display.setTextSize(1);

  int y = 50;
  const int step = 16;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("Uptime: ");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.print(uptimeString());
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("Power: ");
  if (AwokPins::kBatteryAdc >= 0) {
    const int mv = analogReadMilliVolts(AwokPins::kBatteryAdc);
    display.setTextColor(ILI9341_WHITE, kBackground);
    display.printf("%.2f V", mv * AwokPins::kBatteryDivider / 1000.0f);
  } else {
    int pct = -1;
    const String est = batteryEstimateString(pct);
    if (est.length() == 0) {
      display.setTextColor(ILI9341_WHITE, kBackground);
      display.print("ON");
    } else {
      const uint16_t col = pct <= 10 ? kBad : (pct <= 20 ? kWarn : ILI9341_WHITE);
      display.setTextColor(col, kBackground);
      display.print(est);
    }
  }
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("Free heap: ");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.printf("%u KB", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("Chip temp: ");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.printf("%.1f C", temperatureRead());
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("SD card: ");
  display.setTextColor(sdReady ? ILI9341_WHITE : kWarn, kBackground);
  if (sdReady) {
    const uint32_t totalMb = static_cast<uint32_t>(SD.totalBytes() / 1048576ULL);
    const uint32_t usedMb = static_cast<uint32_t>(SD.usedBytes() / 1048576ULL);
    display.printf("%lu / %lu MB used", static_cast<unsigned long>(usedMb),
                   static_cast<unsigned long>(totalMb));
  } else {
    display.print("not mounted");
  }
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("GPS: ");
  display.setTextColor(gpsHasFix() ? kGood : kMuted, kBackground);
  if (gpsHasFix()) {
    display.printf("fix, %d sat", gpsSats());
  } else {
    display.printf("no fix (%d sat)", gpsSats());
  }
  y += step;

  if (gpsHasFix()) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(6, y);
    display.printf("  %.5f, %.5f", gps.location.lat(), gps.location.lng());
    y += step;
  }

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print("Wi-Fi MAC: ");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.print(WiFi.macAddress());
  y += step;

  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, y);
  display.print(String("Firmware: AxD (") + AwokPins::kChipLabel + ")");

  drawThreeButtonFooter("Home", "Settings", "Files");
}

void updateStatus() {
  if (currentView != View::kStatus) return;
  if (millis() - lastStatusDrawMs < 1000) return;
  lastStatusDrawMs = millis();
  drawStatus();
}

void handleStatusTouch(int x, int y) {
  if (AwokPins::kBatteryAdc < 0 && y >= 62 && y < 78) {
    resetBatteryEstimate();
    drawStatus();
    return;
  }
  if (y < kFooterTop) return;
  if (x < 80) {
    drawHome();
  } else if (x < 160) {
    drawSettings();
  } else {
    openFilesManager();
  }
}
