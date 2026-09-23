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
bool gpsClockSynced = false;
uint32_t gpsLastClockSyncMs = 0;

// Store the zone name, not its table index (indices can change on data updates).
int gpsLocalZone = -1;
bool gpsZoneLookedUp = false;
uint32_t gpsLastZoneLookupMs = 0;
int gpsAppliedOffset = 32767;
char gpsPosixZone[24] = "UTC0";

const char* gpsClockTimezone() { return gpsPosixZone; }

static void updateGpsTimezone() {
  const uint32_t nowMs = millis();
  if (gps.location.isValid() && gps.location.age() < 5000 &&
      (!gpsZoneLookedUp || nowMs - gpsLastZoneLookupMs >= 30000)) {
    gpsZoneLookedUp = true;
    gpsLastZoneLookupMs = nowMs;
    const int zone = AwokTime::zoneAt(gps.location.lat(), gps.location.lng());
    if (zone >= 0 && zone != gpsLocalZone) {
      gpsLocalZone = zone;
      Preferences preferences;
      if (preferences.begin("awok-tz", false)) {
        preferences.putString("zone", AwokTime::kZones[zone].name);
        preferences.end();
      }
      Serial.printf("[gps] local timezone: %s\n", AwokTime::kZones[zone].name);
    }
  }
  int minutes = 0;
  bool dst = false;
  if (!AwokTime::offsetAt(gpsLocalZone, int64_t(time(nullptr)), minutes, dst) ||
      minutes == gpsAppliedOffset) return;
  // libc/FAT uses the current local offset, while epoch seconds stay absolute
  // for TLS/NTP. Re-evaluated each loop, including at DST transitions without GPS.
  const int absolute = abs(minutes);
  snprintf(gpsPosixZone, sizeof(gpsPosixZone), "LOC%s%d:%02d",
           minutes >= 0 ? "-" : "+", absolute / 60, absolute % 60);
  setenv("TZ", gpsPosixZone, 1);
  tzset();
  gpsAppliedOffset = minutes;
}

// Convert a validated Gregorian UTC date to Unix seconds without mktime(),
// whose result depends on the process timezone. All supported GPS years are
// positive, so this era decomposition is identical on both ESP32 toolchains.
static time_t gpsUtcEpoch(int year, unsigned month, unsigned day,
                          unsigned hour, unsigned minute, unsigned second) {
  year -= month <= 2;
  const int era = year / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned shiftedMonth =
      month > 2 ? month - 3 : month + 9;
  const unsigned dayOfYear = (153 * shiftedMonth + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 -
                            yearOfEra / 100 + dayOfYear;
  const int64_t days = static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
  return static_cast<time_t>(days * 86400 + hour * 3600 + minute * 60 + second);
}

static bool gpsDateTimeFreshAndSane() {
  if (!gps.date.isValid() || !gps.time.isValid() ||
      gps.date.age() >= 5000 || gps.time.age() >= 5000) return false;
  const int year = gps.date.year();
  const unsigned month = gps.date.month();
  const unsigned day = gps.date.day();
  const unsigned hour = gps.time.hour();
  const unsigned minute = gps.time.minute();
  const unsigned second = gps.time.second();
  if (year < 2020 || year > 2099 || month < 1 || month > 12 ||
      hour > 23 || minute > 59 || second > 59) return false;
  static const uint8_t daysPerMonth[] =
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  unsigned maximumDay = daysPerMonth[month - 1];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximumDay;
  return day >= 1 && day <= maximumDay;
}

// GPS NMEA date/time is UTC. Use it to seed and discipline the ESP system
// clock, which in turn supplies TLS validation and FAT file timestamps. GPS is
// authoritative whenever it is fresh; network time remains a fallback for
// devices that have no fix when an HTTPS upload starts.
static void syncSystemClockFromGps() {
  if (!gpsDateTimeFreshAndSane()) return;
  const int year = gps.date.year();
  const unsigned month = gps.date.month();
  const unsigned day = gps.date.day();
  const unsigned hour = gps.time.hour();
  const unsigned minute = gps.time.minute();
  const unsigned second = gps.time.second();
  const uint32_t nowMs = millis();
  if (gpsClockSynced && nowMs - gpsLastClockSyncMs < 60000) return;

  const time_t epoch = gpsUtcEpoch(year, month, day, hour, minute, second);
  if (epoch <= 1577836800) return;  // reject 2020-01-01 and older

  const time_t oldEpoch = time(nullptr);
  struct timeval tv = {};
  tv.tv_sec = epoch;
  tv.tv_usec = static_cast<suseconds_t>(gps.time.centisecond()) * 10000;
  if (settimeofday(&tv, nullptr) != 0) {
    Serial.printf("[gps] system clock sync failed: errno=%d\n", errno);
    return;
  }
  gpsClockSynced = true;
  gpsLastClockSyncMs = nowMs;
  long long correction = static_cast<long long>(epoch) -
                         static_cast<long long>(oldEpoch);
  if (correction < 0) correction = -correction;
  if (oldEpoch < 1577836800 || correction > 2) {
    Serial.println("[gps] clock synchronized from GPS");
  }
}

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
  Preferences preferences;
  if (preferences.begin("awok-tz", true)) {
    const String saved = preferences.getString("zone", "");
    gpsLocalZone = AwokTime::zoneByName(saved.c_str());
    preferences.end();
  }
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
  syncSystemClockFromGps();
  updateGpsTimezone();
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

// Local calendar time for every log and WiGLE FirstSeen. GPS supplies absolute
// time; the latest valid position chooses timezone and DST rules. Keep the last
// zone during fix loss. Until both clock and zone are known, label uptime rather
// than silently writing UTC as though it were local time.
static time_t gpsTimestampEpoch() {
  if (gpsDateTimeFreshAndSane())
    return gpsUtcEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                       gps.time.hour(), gps.time.minute(), gps.time.second());
  return time(nullptr);
}

String gpsTimestamp() {
  struct tm local = {};
  int minutes = 0;
  bool dst = false;
  if (AwokTime::localTime(gpsLocalZone, gpsTimestampEpoch(), local, minutes, dst)) {
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local);
    return String(buffer);
  }
  return String("uptime+") + String(millis());
}

String gpsTimezoneLabel() {
  int minutes = 0;
  bool dst = false;
  if (!AwokTime::offsetAt(gpsLocalZone, int64_t(gpsTimestampEpoch()), minutes, dst))
    return "Local time: waiting for GPS";
  char offset[32];
  snprintf(offset, sizeof(offset), "GMT%c%02d:%02d %s", minutes < 0 ? '-' : '+',
           abs(minutes) / 60, abs(minutes) % 60, dst ? "DST" : "standard");
  return String(offset);
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

static inline uint32_t macHash1(const uint8_t* mac) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; ++i) {
    h = (h ^ mac[i]) * 16777619u;
  }
  return h;
}

static inline uint32_t macHash2(const uint8_t* mac) {
  uint32_t h = 0x811c9dc5u;
  for (int i = 0; i < 6; ++i) {
    h = (h * 33) ^ mac[i];
  }
  return h;
}

constexpr size_t kWardriveBloomBits = kWardriveBloomFilterBytes * 8;

bool wardriveMacSeen(const uint8_t* mac) {
  const uint32_t h1 = macHash1(mac);
  const uint32_t h2 = macHash2(mac);
  const uint32_t b1 = h1 % kWardriveBloomBits;
  const uint32_t b2 = (h1 + h2) % kWardriveBloomBits;
  const uint32_t b3 = (h1 + 2 * h2) % kWardriveBloomBits;
  const uint32_t b4 = (h1 + 3 * h2) % kWardriveBloomBits;
  return (wardriveBloom[b1 >> 3] & (1 << (b1 & 7))) &&
         (wardriveBloom[b2 >> 3] & (1 << (b2 & 7))) &&
         (wardriveBloom[b3 >> 3] & (1 << (b3 & 7))) &&
         (wardriveBloom[b4 >> 3] & (1 << (b4 & 7)));
}

void wardriveAddMac(const uint8_t* mac) {
  const uint32_t h1 = macHash1(mac);
  const uint32_t h2 = macHash2(mac);
  const uint32_t b1 = h1 % kWardriveBloomBits;
  const uint32_t b2 = (h1 + h2) % kWardriveBloomBits;
  const uint32_t b3 = (h1 + 2 * h2) % kWardriveBloomBits;
  const uint32_t b4 = (h1 + 3 * h2) % kWardriveBloomBits;
  wardriveBloom[b1 >> 3] |= (1 << (b1 & 7));
  wardriveBloom[b2 >> 3] |= (1 << (b2 & 7));
  wardriveBloom[b3 >> 3] |= (1 << (b3 & 7));
  wardriveBloom[b4 >> 3] |= (1 << (b4 & 7));
  ++wardriveMacCount;
}

void wardriveResetDedup() {
  memset(wardriveBloom, 0, sizeof(wardriveBloom));
  wardriveMacCount = 0;
}

// Open a FRESH CSV for this wardrive run: /awokxdag/wardrive-NNNN.csv, using the
// first index not already on the card, so every Start (solo, link, or fleet)
// writes a new file instead of appending to one growing log.
bool openWardriveCsv() {
  closeWardriveCsv();
  g_wardriveCsvPath = "";
  if (!ensureSdCard()) return false;
  for (int n = 1; n <= 9999; ++n) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s/wardrive-%04d.csv", kSdDirectory, n);
    if (!SD.exists(buf)) { g_wardriveCsvPath = String(buf); break; }
  }
  if (g_wardriveCsvPath.length() == 0) {
    Serial.println("[wardrive] no free session filename (0001-9999)");
    return false;
  }
  g_wardriveFile = SD.open(g_wardriveCsvPath.c_str(), FILE_WRITE);
  if (!g_wardriveFile) {
    sdReady = false;
    wardriveCsvReady = false;
    return false;
  }
  // Build both header lines without temporary heap Strings. A failed/short
  // header write must never leave a ready session that appends headerless rows.
  char header[512];
  const int headerLength = snprintf(
      header, sizeof(header),
      "WigleWifi-1.6,appRelease=AxD,model=%s,release=%s,device=AxD,"
      "display=ILI9341,board=%s,brand=AxD\r\n"
      "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,"
      "CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\r\n",
      AwokPins::kChipLabel, kVersion, AwokPins::kBoardLabel);
  const bool headerFits = headerLength > 0 &&
                         static_cast<size_t>(headerLength) < sizeof(header);
  const size_t written = headerFits
      ? g_wardriveFile.write(reinterpret_cast<const uint8_t*>(header), headerLength)
      : 0;
  g_wardriveFile.flush();
  if (!headerFits || written != static_cast<size_t>(headerLength) ||
      g_wardriveFile.size() != static_cast<size_t>(headerLength)) {
    Serial.println("[wardrive] CSV header write failed; SD logging disabled");
    closeWardriveCsv();
    SD.remove(g_wardriveCsvPath.c_str());  // only this newly created, incomplete session
    g_wardriveCsvPath = "";
    sdReady = false;
    return false;
  }
  wardriveCsvReady = true;
  Serial.printf("[wardrive] logging to %s\n", g_wardriveCsvPath.c_str());
  return true;
}

void closeWardriveCsv() {
  if (g_wardriveFile) {
    g_wardriveFile.flush();
    g_wardriveFile.close();
  }
  wardriveCsvReady = false;
}

void flushWardriveCsv() {
  if (wardriveCsvReady && g_wardriveFile) {
    g_wardriveFile.flush();
  }
}

// Basename of the current run's CSV for on-screen display (empty before start).
String wardriveCsvName() {
  if (g_wardriveCsvPath.length() == 0) return String("wardrive.csv");
  return g_wardriveCsvPath.substring(g_wardriveCsvPath.lastIndexOf('/') + 1);
}

int wigleFrequencyMhz(int channel) {
  if (channel == 14) return 2484;
  if (channel >= 1 && channel <= 13) return 2407 + channel * 5;
  if (channel >= 32 && channel <= 177) return 5000 + channel * 5;
  return 0;  // BLE and unknown/non-Wi-Fi channels
}

// Shared tail of a WiGLE row (timestamp, channel/frequency, RSSI, GPS, type).
void appendWigleTail(File& file, int channel, int rssi, const char* type) {
  file.print(gpsTimestamp());
  file.print(',');
  file.print(channel);
  file.print(',');
  file.print(wigleFrequencyMhz(channel));
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
  file.print(",,0,");
  file.println(type);
}

// The WiGLE row tail as a String (same fields/order as appendWigleTail): so the
// full row can be both written to SD and streamed to the phone.
String wardriveWigleTail(int channel, int rssi, const char* type) {
  return gpsTimestamp() + "," + String(channel) + "," +
         String(wigleFrequencyMhz(channel)) + "," + String(rssi) + "," +
         String(gps.location.lat(), 6) + "," + String(gps.location.lng(), 6) +
         "," + String(gps.altitude.meters(), 1) + "," +
         String(gps.hdop.isValid() ? gps.hdop.hdop() * 5.0 : 0.0, 1) +
         ",,0," + type;
}

// Persist a wardrive row to SD via persistent open file handle, and (on the
// headless bridge) push it to the phone so the app can build/download the WiGLE
// CSV even with no SD.
static void wardriveEmitRow(const char* line, size_t len) {
#ifdef AWOK_HEADLESS
  if (g_bridgePhoneConnected)
    bridgeNotifyResult(kSourceWardrive,
                       reinterpret_cast<const uint8_t*>(line),
                       len);
#endif
  if (!wardriveCsvReady || !g_wardriveFile) return;
  g_wardriveFile.println(line);
}

static void wardriveEmitRow(const String& line) {
  wardriveEmitRow(line.c_str(), line.length());
}

// Coordinator: merge a WiGLE row received from a fleet member. Dedups by id
// (Wi-Fi BSSID / BLE address), builds the line from the member's own GPS carried
// in the frame, and emits it through the same SD+phone sink as local rows. The
// FirstSeen timestamp uses the coordinator's clock (rows stream in near real
// time and the fleet is co-located, so it is within a second of the sighting).
void wardriveEmitPeerRow(const FleetWardriveRow& r) {
  if (wardriveMacSeen(r.id)) return;
  wardriveAddMac(r.id);
  char line[256];
  const char* authStr = r.isBle ? "[BLE]" : wigleAuth((wifi_auth_mode_t)r.auth);
  const char* typeStr = r.isBle ? "BLE" : "WIFI";
  String escapedName = csvField(String(r.name));
  String ts = gpsTimestamp();
  int n = snprintf(line, sizeof(line),
                   "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%s,%u,%d,%d,%.6f,%.6f,%d.0,0.0,,0,%s",
                   r.id[0], r.id[1], r.id[2], r.id[3], r.id[4], r.id[5],
                   escapedName.c_str(), authStr, ts.c_str(),
                   (unsigned)r.channel, wigleFrequencyMhz(r.channel), (int)r.rssi,
                   r.lat, r.lon, (int)r.alt, typeStr);
  if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
    wardriveEmitRow(line, static_cast<size_t>(n));
  }
  if (r.isBle) ++wardriveBleCount; else ++wardriveNetworks;
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
  }
  display.setCursor(6, 140);
  display.print("Local: " + gpsTimestamp());
  display.setCursor(6, 152);
  display.print(gpsTimezoneLabel());
  display.setCursor(6, 164);
  if (gpsLocalZone >= 0) display.print(clipped(String(AwokTime::kZones[gpsLocalZone].name), 37));

  // Link diagnostics: distinguish "wrong baud/wiring" from "no fix yet".
  const uint32_t passed = gps.passedChecksum();
  const uint32_t failed = gps.failedChecksum();
  const uint32_t passedHere =
      passed >= gpsBaudBaselinePassed ? passed - gpsBaudBaselinePassed : passed;
  display.drawFastHLine(6, 180, 228, kPanel);
  display.setTextColor(kAccent, kBackground);
  display.setCursor(6, 186);
  display.print("LINK DIAGNOSTICS");
  display.setTextColor(ILI9341_WHITE, kBackground);
  display.setCursor(6, 200);
  display.printf("Baud %lu  chars %lu", gpsCurrentBaud,
                 static_cast<unsigned long>(gps.charsProcessed()));
  display.setCursor(6, 212);
  display.setTextColor(passedHere > 0 ? kGood : kBad, kBackground);
  display.printf("NMEA ok %lu (this baud %lu)  bad %lu",
                 static_cast<unsigned long>(passed),
                 static_cast<unsigned long>(passedHere),
                 static_cast<unsigned long>(failed));
  display.setTextColor(kMuted, kBackground);
  display.setCursor(6, 226);
  display.print("Last: ");
  display.print(clipped(String(gpsLastSentence), 32));
  display.setTextColor(passedHere > 0 ? kMuted : kWarn, kBackground);
  display.setCursor(6, 240);
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
  if (wardriveCsvReady) display.print("SD: " + wardriveCsvName());
  else display.print("SD unavailable; not logging");
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
  display.setCursor(6, 228);
  display.print("Local: " + gpsTimestamp());
  display.setCursor(6, 240);
  display.print(gpsTimezoneLabel());
  drawFooter("Back", "Home");
}

// Wi-Fi window hooks for the dual-radio scheduler: kick an async passive AP
// scan, and tear the scan down before Wi-Fi is released for the BLE window.
static void wardriveEnterWifi() {
  WiFi.disconnect(false, false);
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  linkBroadcastStatus();
  WiFi.scanNetworks(true, true, false, 120);
}
static void wardriveExitWifi() { WiFi.scanDelete(); }

void startWardrive() {
  stopActiveTools();  // release the previous tool before bringing up both radios
  wardriveNetworks = 0;
  wardriveBleCount = 0;
  wardriveScans = 0;
  wardriveResetDedup();
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
  // Long Wi-Fi dwell (many scan passes) between short BLE windows gives Wi-Fi
  // most airtime. Both controllers stay initialized throughout the session.
  wardriveSched.wifiWindowMs = 30000;
  wardriveSched.bleWindowMs = 8000;
  if (!radioSchedulerBegin(wardriveSched)) {
    closeWardriveCsv();
    return;
  }

  Serial.println(radiosCoexist
                     ? "[wardrive] started (Wi-Fi + BLE, time-shared)"
                     : "[wardrive] started (Wi-Fi only)");
  wardriveActive = true;
  drawWardrive();
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  linkBroadcastStatus();
}

void stopWardrive() {
  if (!wardriveActive && !wardriveSched.active) return;
  wardriveActive = false;
  Serial.println("[wardrive] stopping; closing CSV before radio shutdown");
  // Only loopTask writes the CSV; radio callbacks enqueue observations. Save
  // accepted rows before touching the host/controller shutdown path.
  closeWardriveCsv();
  Serial.println("[wardrive] CSV closed; stopping radios");
  radioSchedulerEnd(wardriveSched);
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  linkBroadcastStatus();
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
      // Between scan passes, briefly home to the rendezvous channel so the
      // screen chip delivers live AP & BLE counts to the bridge/phone.
      esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
      linkBroadcastStatus();
      WiFi.scanNetworks(true, true, false, 120);
    } else {
      // not started / failed: kick off a scan
      WiFi.scanNetworks(true, true, false, 120);
    }
  }
  static uint32_t lastWardriveFlushMs = 0;
  if (millis() - lastWardriveFlushMs >= 2000) {
    lastWardriveFlushMs = millis();
    flushWardriveCsv();
  }
  if (currentView == View::kWardrive &&
      millis() - lastWardriveDrawMs >= kWardriveRedrawMs) {
    lastWardriveDrawMs = millis();
    drawWardrive();
  }
}
