static bool cpArmed = false;
static uint8_t cpLastBssid[6] = {0, 0, 0, 0, 0, 0};
static uint32_t cpLastFireMs = 0;
static uint32_t cpFireCount = 0;
constexpr uint32_t kCpMinIntervalMs = 1500;

bool counterPineappleArmed() {
  return cpArmed;
}

uint32_t counterPineappleFires() {
  return cpFireCount;
}

void counterPineappleDisarm() {
  cpArmed = false;
}

void counterPineappleToggle() {
  if (cpArmed) {
    cpArmed = false;
    return;
  }
  if (!confirmActiveTest("Counter-pineapple")) return;
  cpArmed = true;
}

void counterPineappleOnTwin(const uint8_t* bssid, int channel, const char* ssid) {
  if (!cpArmed || !rogueWatchActive) return;
  if (channel < 1) return;
  const uint32_t now = millis();
  bool sameTarget = true;
  for (int i = 0; i < 6; ++i)
    if (cpLastBssid[i] != bssid[i]) { sameTarget = false; break; }
  if (sameTarget && now - cpLastFireMs < kCpMinIntervalMs) return;
  for (int i = 0; i < 6; ++i) cpLastBssid[i] = bssid[i];
  cpLastFireMs = now;

  static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t frame[26];
  esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
  buildDeauthFrame(frame, broadcast, bssid, 0xC0);
  for (int k = 0; k < 3; ++k)
    esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
  ++cpFireCount;

  if (ensureSdCard()) {
    File f = SD.open("/awokxdag/counter_log.csv", FILE_APPEND);
    if (f) {
      f.printf("%lu,%s,%s\n", (unsigned long)(now / 1000),
               macToString(bssid).c_str(), csvField(String(ssid)).c_str());
      f.close();
    }
  }
}
