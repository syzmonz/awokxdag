// AWOKxDAG — GPS interface + wardriving (compiled as part of the sketch; see awok_common.h)

// ---- GPS interface ------------------------------------------------------

TinyGPSPlus gps;
HardwareSerial gpsSerial(AwokPins::kGpsUart);
bool gpsStarted = false;
bool gpsRawEcho = false;

// Baud options to cycle through on-device; the confirmed default is first.
const unsigned long kGpsBaudOptions[] = {115200, 9600, 38400, 57600, 4800};
constexpr int kGpsBaudOptionCount =
    static_cast<int>(sizeof(kGpsBaudOptions) / sizeof(kGpsBaudOptions[0]));
int gpsBaudIndex = 0;
unsigned long gpsCurrentBaud = AwokPins::kGpsBaud;
char gpsLineBuf[100];
int gpsLineLen = 0;
char gpsLastSentence[28] = "(none)";
// passedChecksum() at the last baud change, so the screen can show whether the
// *current* baud is producing valid sentences.
uint32_t gpsBaudBaselinePassed = 0;

void applyGpsBaud(unsigned long baud, bool persist) {
  int index = 0;
  for (int i = 0; i < kGpsBaudOptionCount; ++i) {
    if (kGpsBaudOptions[i] == baud) {
      index = i;
      break;
    }
  }
  gpsBaudIndex = index;
  gpsCurrentBaud = kGpsBaudOptions[gpsBaudIndex];
  deviceSettings.gpsBaud = gpsCurrentBaud;
  if (gpsStarted) gpsSerial.end();
  gpsSerial.begin(gpsCurrentBaud, SERIAL_8N1, AwokPins::kGpsRx,
                  AwokPins::kGpsTx);
  gpsStarted = true;
  gpsBaudBaselinePassed = gps.passedChecksum();
  Serial.printf("[gps] UART%d rx=%d tx=%d @ %lu baud\n", AwokPins::kGpsUart,
                AwokPins::kGpsRx, AwokPins::kGpsTx, gpsCurrentBaud);
  if (persist) saveDeviceSettings();
}

void initGps() {
  const unsigned long baud =
      gpsBaudIsKnown(deviceSettings.gpsBaud) ? deviceSettings.gpsBaud
                                             : AwokPins::kGpsBaud;
  applyGpsBaud(baud, false);
}

void cycleGpsBaud() {
  applyGpsBaud(kGpsBaudOptions[(gpsBaudIndex + 1) % kGpsBaudOptionCount], true);
}

void updateGps() {
  if (!gpsStarted) return;
  while (gpsSerial.available()) {
    const char c = static_cast<char>(gpsSerial.read());
    gps.encode(c);
    if (gpsRawEcho) Serial.write(c);
    if (c == '\n' || c == '\r') {
      if (gpsLineLen > 0) {
        const int n = min(gpsLineLen, static_cast<int>(sizeof(gpsLastSentence)) - 1);
        memcpy(gpsLastSentence, gpsLineBuf, n);
        gpsLastSentence[n] = 0;
        gpsLineLen = 0;
      }
    } else if (gpsLineLen < static_cast<int>(sizeof(gpsLineBuf)) - 1) {
      gpsLineBuf[gpsLineLen++] = c;
    }
  }
}

bool gpsHasFix() {
  return gps.location.isValid() && gps.location.age() < 5000;
}

int gpsSats() {
  return gps.satellites.isValid() ? gps.satellites.value() : 0;
}

uint32_t gpsCharsProcessed() { return gps.charsProcessed(); }

// Comma-prefixed "lat,lon,alt" for appending to a CSV row, or empty fields
// when there is no fix.
String gpsCsvFields() {
  if (!gpsHasFix()) return ",,,";
  String out = ",";
  out += String(gps.location.lat(), 6);
  out += ',';
  out += String(gps.location.lng(), 6);
  out += ',';
  out += String(gps.altitude.meters(), 1);
  return out;
}

// "yyyy-MM-dd HH:mm:ss" from GPS UTC, for WiGLE FirstSeen. Falls back to an
// uptime marker when the date/time is invalid or the GPS link has gone stale.
String gpsTimestamp() {
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 5000 && gps.time.age() < 5000) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buffer);
  }
  return String("uptime+") + String(millis());
}


// ---- GPS status + wardriving --------------------------------------------

const char* wigleAuth(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:
      return "[ESS]";
    case WIFI_AUTH_WEP:
      return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:
      return "[WPA-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
      return "[WPA3-SAE-CCMP][ESS]";
    case WIFI_AUTH_ENTERPRISE:
      return "[WPA2-EAP-CCMP][ESS]";
    case WIFI_AUTH_OWE:
      return "[OWE][ESS]";
    default:
      return "[ESS]";
  }
}

bool wardriveMacSeen(const uint8_t* mac) {
  for (int i = 0; i < wardriveMacCount; ++i) {
    bool equal = true;
    for (int j = 0; j < 6; ++j) {
      if (wardriveMacs[i][j] != mac[j]) {
        equal = false;
        break;
      }
    }
    if (equal) return true;
  }
  return false;
}

void wardriveAddMac(const uint8_t* mac) {
  if (wardriveMacCount >= kMaxWardriveMacs) return;
  memcpy(wardriveMacs[wardriveMacCount++], mac, 6);
}

bool openWardriveCsv() {
  if (!ensureSdCard()) return false;
  if (!SD.exists(kWardriveCsvPath)) {
    File file = SD.open(kWardriveCsvPath, FILE_WRITE);
    if (!file) {
      sdReady = false;
      return false;
    }
    file.println(
        String("WigleWifi_1.4,appRelease=AxD,model=") + AwokPins::kChipLabel + ",release=" +
        kVersion +
        ",device=AxD,display=ILI9341,board=" + AwokPins::kBoardLabel + ",brand=AxD");
    file.println(
        "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,"
        "CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
    file.close();
  }
  return true;
}

// Shared tail of a WiGLE row (timestamp, channel, rssi, gps, accuracy, type).
void appendWigleTail(File& file, int channel, int rssi, const char* type) {
  file.print(gpsTimestamp());
  file.print(',');
  file.print(channel);
  file.print(',');
  file.print(rssi);
  file.print(',');
  file.print(String(gps.location.lat(), 6));
  file.print(',');
  file.print(String(gps.location.lng(), 6));
  file.print(',');
  file.print(String(gps.altitude.meters(), 1));
  file.print(',');
  file.print(String(gps.hdop.isValid() ? gps.hdop.hdop() * 5.0 : 0.0, 1));
  file.print(',');
  file.println(type);
}

// The WiGLE row tail as a String (same fields/order as appendWigleTail): so the
// full row can be both written to SD and streamed to the phone.
String wardriveWigleTail(int channel, int rssi, const char* type) {
  return gpsTimestamp() + "," + String(channel) + "," + String(rssi) + "," +
         String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) +
         "," + String(gps.altitude.meters(), 1) + "," +
         String(gps.hdop.isValid() ? gps.hdop.hdop() * 5.0 : 0.0, 1) + "," + type;
}

// Persist a wardrive row to SD when available, and (on the headless bridge) push
// it to the phone so the app can build/download the WiGLE CSV even with no SD.
static void wardriveEmitRow(const String& line) {
#ifdef AWOK_HEADLESS
  if (g_bridgePhoneConnected)
    bridgeNotifyResult(kSourceWardrive,
                       reinterpret_cast<const uint8_t*>(line.c_str()),
                       line.length());
#endif
  if (!wardriveCsvReady) return;
  File file = SD.open(kWardriveCsvPath, FILE_APPEND);
  if (!file) {
    wardriveCsvReady = false;
    sdReady = false;
    return;
  }
  file.println(line);
  file.close();
}

void appendWardriveRow(const String& bssid, const String& ssid,
                       wifi_auth_mode_t auth, int channel, int rssi) {
  wardriveEmitRow(bssid + "," + csvField(ssid) + "," + wigleAuth(auth) + "," +
                  wardriveWigleTail(channel, rssi, "WIFI"));
}

void appendWardriveBleRow(const String& address, const String& name, int rssi) {
  wardriveEmitRow(address + "," + csvField(name) + ",[BLE]," +
                  wardriveWigleTail(0, rssi, "BLE"));
}

// NimBLE scan callback (runs in the BLE task). Enqueues a POD BleHit for the
// wardrive loop; no SD or String heap churn happens here.
class WardriveBleCallbacks : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const int next = (bleHitHead + 1) % kBleHitQueueSlots;
    if (next == bleHitTail) return;  // queue full: drop
    BleHit& hit = bleHitQueue[bleHitHead];
    strncpy(hit.addr, device->getAddress().toString().c_str(),
            sizeof(hit.addr) - 1);
    hit.addr[sizeof(hit.addr) - 1] = 0;
    hit.rssi = device->getRSSI();
    if (device->haveName()) {
      strncpy(hit.name, device->getName().c_str(), sizeof(hit.name) - 1);
      hit.name[sizeof(hit.name) - 1] = 0;
    } else {
      hit.name[0] = 0;
    }
    bleHitHead = next;
  }
};

WardriveBleCallbacks wardriveBleCallbacks;

// Time-multiplex scheduler for wardrive; drawWardrive reads its phase.
RadioScheduler wardriveSched;

void drawGps() {
  currentView = View::kGps;
  display.fillScreen(kBackground);
  drawHeader("GPS", gpsHasFix() ? "fix acquired" : "searching for satellites");
  display.setTextSize(2);
  display.setTextColor(gpsHasFix() ? kGood : kWarn, kBackground);
  display.setCursor(6, 54);
  display.print(gpsHasFix() ? "FIX" : "NO FIX");

  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 90);
  display.printf("Satellites: %d", gpsSats());
  if (gpsHasFix()) {
    display.setCursor(6, 104);
    display.printf("Lat: %.6f", gps.location.lat());
    display.setCursor(6, 116);
    display.printf("Lon: %.6f", gps.location.lng());
    display.setCursor(6, 128);
    display.printf("Alt: %.1f m  Spd: %.1f km/h", gps.altitude.meters(),
                   gps.speed.kmph());
    display.setCursor(6, 140);
    display.print("UTC: ");
    display.print(gpsTimestamp());
  }

  // Link diagnostics: distinguish "wrong baud/wiring" from "no fix yet".
  const uint32_t passed = gps.passedChecksum();
  const uint32_t failed = gps.failedChecksum();
  const uint32_t passedHere =
      passed >= gpsBaudBaselinePassed ? passed - gpsBaudBaselinePassed : passed;
  display.drawFastHLine(6, 154, 228, kPanel);
  display.setTextColor(kAccent, kBackground);
  display.setCursor(6, 160);
  display.print("LINK DIAGNOSTICS");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 174);
  display.printf("Baud %lu  chars %lu", gpsCurrentBaud,
                 static_cast<unsigned long>(gps.charsProcessed()));
  display.setCursor(6, 186);
  display.setTextColor(passedHere > 0 ? kGood : kBad, kBackground);
  display.printf("NMEA ok %lu (this baud %lu)  bad %lu",
                 static_cast<unsigned long>(passed),
                 static_cast<unsigned long>(passedHere),
                 static_cast<unsigned long>(failed));
  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, 200);
  display.print("Last: ");
  display.print(clipped(String(gpsLastSentence), 32));
  display.setTextColor(passedHere > 0 ? kMuted : kWarn, kBackground);
  display.setCursor(6, 214);
  if (passedHere == 0) {
    display.print("No valid NMEA: tap Baud to retry.");
  } else if (!gpsHasFix()) {
    display.print("Good data; need open-sky fix.");
  } else {
    display.print("Fix locked.");
  }
  drawFourButtonFooter("Home", "Baud", "Drive", "Link");
}

void drawWardrive() {
  currentView = View::kWardrive;
  display.fillScreen(kBackground);
  drawHeader("WARDRIVE",
             gpsHasFix() ? "logging to WiGLE CSV" : "waiting for GPS fix");
  display.setTextSize(2);
  display.setTextColor(gpsHasFix() ? kGood : kWarn, kBackground);
  display.setCursor(6, 54);
  display.print(gpsHasFix() ? "LOGGING" : "NO FIX");

  display.setTextSize(1);
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 92);
  if (radiosCoexist) {
    const bool bleNow = wardriveSched.phase == RadioPhase::kBle;
    display.printf("Wi-Fi: %lu   BLE: %lu   [%s]",
                   static_cast<unsigned long>(wardriveNetworks),
                   static_cast<unsigned long>(wardriveBleCount),
                   bleNow ? "BLE" : "WiFi");
  } else {
    display.printf("Wi-Fi: %lu   BLE: off",
                   static_cast<unsigned long>(wardriveNetworks));
  }
  display.setCursor(6, 106);
  display.printf("Scans: %lu   Sats: %d",
                 static_cast<unsigned long>(wardriveScans), gpsSats());
  const uint32_t elapsed = (millis() - wardriveStartMs) / 1000;
  display.setCursor(6, 120);
  display.printf("Elapsed: %lus", static_cast<unsigned long>(elapsed));
  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, 140);
  if (gpsHasFix()) {
    display.printf("At: %.5f, %.5f", gps.location.lat(), gps.location.lng());
  } else {
    display.print("Networks are only logged with a");
    display.setCursor(6, 152);
    display.print("valid fix; keep moving.");
  }
  display.setTextColor(wardriveCsvReady ? kAccent : kWarn, kBackground);
  display.setCursor(6, 176);
  display.print(wardriveCsvReady ? "SD: wardrive.csv (WiGLE)"
                                 : "SD unavailable; not logging");
  display.setTextColor(kMuted, kBackground);
  if (radiosCoexist) {
    display.setCursor(6, 196);
    display.print("Wi-Fi and BLE alternate windows");
    display.setCursor(6, 208);
    display.print("(one radio at a time on C5).");
  } else {
    display.setCursor(6, 196);
    display.print("BLE unavailable on this board;");
    display.setCursor(6, 208);
    display.print("logging Wi-Fi APs only.");
  }
  drawFooter("Back", "Home");
}

// Wi-Fi window hooks for the dual-radio scheduler: kick an async passive AP
// scan, and tear the scan down before Wi-Fi is released for the BLE window.
static void wardriveEnterWifi() {
  WiFi.disconnect(false, false);
  WiFi.scanNetworks(true, true, false, 120);
}
static void wardriveExitWifi() { WiFi.scanDelete(); }

void startWardrive() {
  wardriveNetworks = 0;
  wardriveBleCount = 0;
  wardriveScans = 0;
  wardriveMacCount = 0;
  bleHitHead = 0;
  bleHitTail = 0;
  wardriveStartMs = millis();
  lastWardriveDrawMs = 0;
  signalMonitorActive = false;

  wardriveCsvReady = openWardriveCsv();

  wardriveSched = RadioScheduler();
  wardriveSched.enterWifi = wardriveEnterWifi;
  wardriveSched.exitWifi = wardriveExitWifi;
  wardriveSched.bleCallbacks = &wardriveBleCallbacks;  // passive scan
  // Wi-Fi window ends when the scan completes; cap high so a slow dual-band
  // sweep is never cut off mid-scan (which would log zero APs).
  // Long Wi-Fi dwell (many scan passes) between short BLE windows: minimizes the
  // number of BLE controller init/deinit cycles (each leaks ~0.4 KB DMA in the
  // closed C5 blob) so BLE survives most of a session, while Wi-Fi -- the
  // primary wardrive radio -- gets the majority of airtime.
  wardriveSched.wifiWindowMs = 30000;
  wardriveSched.bleWindowMs = 8000;
  if (!radioSchedulerBegin(wardriveSched)) return;

  Serial.println(radiosCoexist
                     ? "[wardrive] started (Wi-Fi + BLE, time-shared)"
                     : "[wardrive] started (Wi-Fi only)");
  wardriveActive = true;
  drawWardrive();
}

void stopWardrive() {
  wardriveActive = false;
  radioSchedulerEnd(wardriveSched);
  Serial.printf("[wardrive] stopped; %lu Wi-Fi, %lu BLE\n",
                static_cast<unsigned long>(wardriveNetworks),
                static_cast<unsigned long>(wardriveBleCount));
}

void updateWardrive() {
  if (!wardriveActive) return;
  radioSchedulerTick(wardriveSched);

  // Drain BLE advertisements (the queue only fills during a BLE window): log
  // each new address once, when a fix is present.
  while (bleHitTail != bleHitHead) {
    const BleHit& hit = bleHitQueue[bleHitTail];
    uint8_t mac[6];
    if (gpsHasFix() && parseBssid(String(hit.addr), mac) &&
        !wardriveMacSeen(mac)) {
      wardriveAddMac(mac);
      appendWardriveBleRow(String(hit.addr), String(hit.name), hit.rssi);
      ++wardriveBleCount;
    }
    bleHitTail = (bleHitTail + 1) % kBleHitQueueSlots;
  }

  // Service the Wi-Fi AP scan only during the Wi-Fi window and only until it
  // completes; the scheduler then ends the window (a dual-band scan can take
  // longer than a fixed slice, so ending on completion is what actually lets
  // APs get logged). Wi-Fi is torn down during BLE windows, so scanComplete()
  // must not be polled then.
  if (wardriveSched.phase == RadioPhase::kWifi && !wardriveSched.wifiScanDone) {
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
      // scan in progress: nothing to do this pass
    } else if (result >= 0) {
      for (int i = 0; i < result; ++i) {
        uint8_t* bssid = WiFi.BSSID(i);
        if (!bssid) continue;
        if (gpsHasFix() && !wardriveMacSeen(bssid)) {
          wardriveAddMac(bssid);
          appendWardriveRow(WiFi.BSSIDstr(i), WiFi.SSID(i),
                            WiFi.encryptionType(i), WiFi.channel(i),
                            WiFi.RSSI(i));
          ++wardriveNetworks;
        }
      }
      WiFi.scanDelete();
      ++wardriveScans;
      // Keep scanning for the whole Wi-Fi window (multiple passes) instead of
      // ending after one scan. Every Wi-Fi<->BLE switch triggers a BLE
      // controller init/deinit, and the closed C5 controller leaks ~0.4 KB DMA
      // per such cycle -- so switching less often is what keeps BLE alive across
      // a long wardrive (and a longer dwell captures more APs while driving).
      // The scheduler ends this window at wifiWindowMs.
      WiFi.scanNetworks(true, true, false, 120);
    } else {
      // not started / failed: kick off a scan
      WiFi.scanNetworks(true, true, false, 120);
    }
  }
  if (currentView == View::kWardrive &&
      millis() - lastWardriveDrawMs >= kWardriveRedrawMs) {
    lastWardriveDrawMs = millis();
    drawWardrive();
  }
}
