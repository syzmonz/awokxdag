// AWOKxDAG — Swarm Mesh Topology Map (compiled as part of the sketch; see awok_common.h)
//
// Passively listens for 802.11 client<->AP data associations and directed probe requests.
// Builds an in-memory cluster graph mapping active clients to their parent APs,
// detects open networks, tracks probe SSID leaks, and shares topology links across the ESP-NOW swarm.

struct TopoAp {
  uint8_t bssid[6] = {0};
  char ssid[33] = {0};
  uint8_t channel = 0;
  int8_t rssi = -127;
  bool isOpen = false;
  uint16_t clientCount = 0;
  uint32_t lastSeenMs = 0;
};

struct TopoClient {
  uint8_t mac[6] = {0};
  uint8_t bssid[6] = {0};
  bool hasBssid = false;
  uint8_t channel = 0;
  int8_t rssi = -127;
  uint16_t packetCount = 0;
  uint32_t lastSeenMs = 0;
};

struct TopoProbe {
  uint8_t clientMac[6] = {0};
  char ssid[33] = {0};
  int8_t rssi = -127;
  uint16_t count = 0;
  uint32_t lastSeenMs = 0;
};

constexpr int kMaxTopoAps = 32;
constexpr int kMaxTopoClients = 48;
constexpr int kMaxTopoProbes = 32;
constexpr int kTopoHitQueueSlots = AwokPins::kDualBand ? 32 : 16;
constexpr uint32_t kTopoHopIntervalMs = 250;
constexpr uint32_t kTopoRedrawMs = 1000;
constexpr uint32_t kTopoTelemIntervalMs = 2000;

static TopoAp topoAps[kMaxTopoAps];
static int topoApCount = 0;

static TopoClient topoClients[kMaxTopoClients];
static int topoClientCount = 0;

static TopoProbe topoProbes[kMaxTopoProbes];
static int topoProbeCount = 0;

static TopoHit topoHitQueue[kTopoHitQueueSlots];
static volatile int topoHitHead = 0;
static volatile int topoHitTail = 0;

static int topoHopIndex = 0;
static uint32_t lastTopoHopMs = 0;
static uint32_t lastTopoDrawMs = 0;
static uint32_t lastTopoTelemMs = 0;
static uint32_t topoStartMs = 0;
static uint32_t topoTotalFrames = 0;

int topoFindAp(const uint8_t* bssid) {
  for (int i = 0; i < topoApCount; ++i) {
    if (memcmp(topoAps[i].bssid, bssid, 6) == 0) return i;
  }
  return -1;
}

int topoFindClient(const uint8_t* mac) {
  for (int i = 0; i < topoClientCount; ++i) {
    if (memcmp(topoClients[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

int topoFindProbe(const uint8_t* mac, const char* ssid) {
  for (int i = 0; i < topoProbeCount; ++i) {
    if (memcmp(topoProbes[i].clientMac, mac, 6) == 0 &&
        strncmp(topoProbes[i].ssid, ssid, 32) == 0) {
      return i;
    }
  }
  return -1;
}

void topoPromiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t* packet =
      static_cast<const wifi_promiscuous_pkt_t*>(buf);
  const uint8_t* payload = packet->payload;
  const int length = packet->rx_ctrl.sig_len;
  if (length < 24) return;

  const uint8_t frameControl = payload[0];
  const uint8_t frameType = (frameControl >> 2) & 0x03;
  TopoHit hit = {};
  hit.rssi = packet->rx_ctrl.rssi;
  hit.channel = packet->rx_ctrl.channel;

  if (frameType == 0x00) {  // Management frame
    const uint8_t subtype = frameControl & 0xF0;
    if (subtype == 0x40) {  // Probe request
      const uint8_t* mac = payload + 10;
      if (mac[0] & 0x01) return;  // ignore multicast/broadcast
      hit.type = 1;
      memcpy(hit.clientMac, mac, 6);
      if (length >= 26) {
        const uint8_t elementId = payload[24];
        const uint8_t elementLen = payload[25];
        if (elementId == 0 && elementLen > 0 && elementLen <= 32 &&
            length >= 26 + elementLen) {
          memcpy(hit.ssid, payload + 26, elementLen);
          hit.ssid[elementLen] = '\0';
        }
      }
    } else if (subtype == 0x80 || subtype == 0x50) {  // Beacon or Probe Response
      hit.type = 2;
      memcpy(hit.bssid, payload + 16, 6);
      if (length >= 36) {
        uint16_t cap = payload[34] | (payload[35] << 8);
        hit.isOpen = (cap & 0x0010) == 0;
        int offset = 36;
        while (offset + 2 <= length) {
          uint8_t tag = payload[offset];
          uint8_t tagLen = payload[offset + 1];
          if (offset + 2 + tagLen > length) break;
          if (tag == 0 && tagLen > 0 && tagLen <= 32) {
            memcpy(hit.ssid, payload + offset + 2, tagLen);
            hit.ssid[tagLen] = '\0';
            break;
          }
          offset += 2 + tagLen;
        }
      }
    } else {
      return;
    }
  } else if (frameType == 0x02) {  // Data frame
    const uint8_t frameControl1 = payload[1];
    const bool toDs = frameControl1 & 0x01;
    const bool fromDs = frameControl1 & 0x02;
    if (toDs && !fromDs) {  // Station -> AP
      const uint8_t* staMac = payload + 10;
      const uint8_t* bssid = payload + 4;
      if ((staMac[0] & 0x01) || (bssid[0] & 0x01)) return;
      hit.type = 0;
      memcpy(hit.clientMac, staMac, 6);
      memcpy(hit.bssid, bssid, 6);
    } else {
      return;
    }
  } else {
    return;
  }

  const int next = (topoHitHead + 1) % kTopoHitQueueSlots;
  if (next == topoHitTail) return;  // Drop when queue full
  topoHitQueue[topoHitHead] = hit;
  topoHitHead = next;
}

void topoBroadcastLink(uint8_t linkType, int8_t rssi, uint8_t channel,
                       const uint8_t* clientMac, const uint8_t* targetMac,
                       const char* targetName) {
  if (!linkEspNowReady) return;
  FleetTopologyLink linkMsg;
  linkMsg.linkType = linkType;
  linkMsg.rssi = rssi;
  linkMsg.channel = channel;
  memcpy(linkMsg.clientMac, clientMac, 6);
  if (targetMac) memcpy(linkMsg.targetMac, targetMac, 6);
  if (targetName) strncpy(linkMsg.targetName, targetName, sizeof(linkMsg.targetName) - 1);
  esp_now_send(kLinkBroadcastAddr, reinterpret_cast<const uint8_t*>(&linkMsg), sizeof(linkMsg));
}

void topoMergeHit(const TopoHit& hit) {
  ++topoTotalFrames;
  const uint32_t now = millis();

  if (hit.type == 2) {  // Beacon / Probe response
    int idx = topoFindAp(hit.bssid);
    if (idx < 0) {
      if (topoApCount >= kMaxTopoAps) return;
      idx = topoApCount++;
      topoAps[idx] = TopoAp();
      memcpy(topoAps[idx].bssid, hit.bssid, 6);
    }
    topoAps[idx].channel = hit.channel;
    topoAps[idx].rssi = hit.rssi;
    topoAps[idx].isOpen = hit.isOpen;
    topoAps[idx].lastSeenMs = now;
    if (hit.ssid[0] != '\0') {
      strncpy(topoAps[idx].ssid, hit.ssid, sizeof(topoAps[idx].ssid) - 1);
    }
    return;
  }

  if (hit.type == 0) {  // Data frame: Station -> AP
    // Ensure AP entry exists
    int apIdx = topoFindAp(hit.bssid);
    if (apIdx < 0) {
      if (topoApCount < kMaxTopoAps) {
        apIdx = topoApCount++;
        topoAps[apIdx] = TopoAp();
        memcpy(topoAps[apIdx].bssid, hit.bssid, 6);
        topoAps[apIdx].channel = hit.channel;
        topoAps[apIdx].rssi = hit.rssi;
        topoAps[apIdx].lastSeenMs = now;
      }
    } else {
      topoAps[apIdx].rssi = hit.rssi;
      topoAps[apIdx].lastSeenMs = now;
    }

    // Upsert client
    int cliIdx = topoFindClient(hit.clientMac);
    bool isNewClient = false;
    if (cliIdx < 0) {
      if (topoClientCount >= kMaxTopoClients) return;
      cliIdx = topoClientCount++;
      topoClients[cliIdx] = TopoClient();
      memcpy(topoClients[cliIdx].mac, hit.clientMac, 6);
      isNewClient = true;
    }
    TopoClient& cli = topoClients[cliIdx];
    cli.channel = hit.channel;
    cli.rssi = hit.rssi;
    cli.lastSeenMs = now;
    ++cli.packetCount;

    if (!cli.hasBssid || memcmp(cli.bssid, hit.bssid, 6) != 0) {
      memcpy(cli.bssid, hit.bssid, 6);
      cli.hasBssid = true;
      if (apIdx >= 0) ++topoAps[apIdx].clientCount;
      // Share topology link across the swarm
      topoBroadcastLink(0, hit.rssi, hit.channel, hit.clientMac, hit.bssid, nullptr);
    } else if (isNewClient) {
      if (apIdx >= 0) ++topoAps[apIdx].clientCount;
      topoBroadcastLink(0, hit.rssi, hit.channel, hit.clientMac, hit.bssid, nullptr);
    }
    return;
  }

  if (hit.type == 1) {  // Probe request
    int cliIdx = topoFindClient(hit.clientMac);
    if (cliIdx < 0 && topoClientCount < kMaxTopoClients) {
      cliIdx = topoClientCount++;
      topoClients[cliIdx] = TopoClient();
      memcpy(topoClients[cliIdx].mac, hit.clientMac, 6);
      topoClients[cliIdx].channel = hit.channel;
      topoClients[cliIdx].rssi = hit.rssi;
      topoClients[cliIdx].lastSeenMs = now;
    } else if (cliIdx >= 0) {
      topoClients[cliIdx].rssi = hit.rssi;
      topoClients[cliIdx].lastSeenMs = now;
    }

    if (hit.ssid[0] != '\0') {
      int prbIdx = topoFindProbe(hit.clientMac, hit.ssid);
      if (prbIdx < 0) {
        if (topoProbeCount >= kMaxTopoProbes) return;
        prbIdx = topoProbeCount++;
        topoProbes[prbIdx] = TopoProbe();
        memcpy(topoProbes[prbIdx].clientMac, hit.clientMac, 6);
        strncpy(topoProbes[prbIdx].ssid, hit.ssid, sizeof(topoProbes[prbIdx].ssid) - 1);
        topoProbes[prbIdx].rssi = hit.rssi;
        topoProbes[prbIdx].count = 1;
        topoProbes[prbIdx].lastSeenMs = now;
        topoBroadcastLink(1, hit.rssi, hit.channel, hit.clientMac, nullptr, hit.ssid);
      } else {
        topoProbes[prbIdx].rssi = hit.rssi;
        ++topoProbes[prbIdx].count;
        topoProbes[prbIdx].lastSeenMs = now;
      }
    }
  }
}

void topologyOnLinkFrame(const uint8_t* data) {
  FleetTopologyLink r;
  memcpy(&r, data, sizeof(r));
  const uint32_t now = millis();

  if (r.linkType == 0) {  // Client -> AP
    int apIdx = topoFindAp(r.targetMac);
    if (apIdx < 0 && topoApCount < kMaxTopoAps) {
      apIdx = topoApCount++;
      topoAps[apIdx] = TopoAp();
      memcpy(topoAps[apIdx].bssid, r.targetMac, 6);
      topoAps[apIdx].channel = r.channel;
      topoAps[apIdx].rssi = r.rssi;
      topoAps[apIdx].lastSeenMs = now;
    }
    int cliIdx = topoFindClient(r.clientMac);
    if (cliIdx < 0 && topoClientCount < kMaxTopoClients) {
      cliIdx = topoClientCount++;
      topoClients[cliIdx] = TopoClient();
      memcpy(topoClients[cliIdx].mac, r.clientMac, 6);
      memcpy(topoClients[cliIdx].bssid, r.targetMac, 6);
      topoClients[cliIdx].hasBssid = true;
      topoClients[cliIdx].channel = r.channel;
      topoClients[cliIdx].rssi = r.rssi;
      topoClients[cliIdx].packetCount = 1;
      topoClients[cliIdx].lastSeenMs = now;
      if (apIdx >= 0) ++topoAps[apIdx].clientCount;
    } else if (cliIdx >= 0) {
      memcpy(topoClients[cliIdx].bssid, r.targetMac, 6);
      topoClients[cliIdx].hasBssid = true;
      topoClients[cliIdx].rssi = r.rssi;
      topoClients[cliIdx].lastSeenMs = now;
    }
  } else if (r.linkType == 1 && r.targetName[0] != '\0') {  // Client -> Probe
    int prbIdx = topoFindProbe(r.clientMac, r.targetName);
    if (prbIdx < 0 && topoProbeCount < kMaxTopoProbes) {
      prbIdx = topoProbeCount++;
      topoProbes[prbIdx] = TopoProbe();
      memcpy(topoProbes[prbIdx].clientMac, r.clientMac, 6);
      strncpy(topoProbes[prbIdx].ssid, r.targetName, sizeof(topoProbes[prbIdx].ssid) - 1);
      topoProbes[prbIdx].rssi = r.rssi;
      topoProbes[prbIdx].count = 1;
      topoProbes[prbIdx].lastSeenMs = now;
    } else if (prbIdx >= 0) {
      topoProbes[prbIdx].rssi = r.rssi;
      ++topoProbes[prbIdx].count;
      topoProbes[prbIdx].lastSeenMs = now;
    }
  }
}

bool exportTopologyToSd() {
  if (!ensureSdCard()) return false;
  const String temporaryPath = String(kTopologyCsvPath) + ".tmp";
  SD.remove(temporaryPath.c_str());
  File file = SD.open(temporaryPath.c_str(), FILE_WRITE);
  if (!file) {
    sdReady = false;
    return false;
  }
  file.println("uptime_ms,type,client_mac,target_mac,target_name,channel,rssi,packets,latitude,longitude,altitude_m");
  const uint32_t uptime = millis();
  const String location = gpsCsvFields();

  // Write AP nodes
  for (int i = 0; i < topoApCount; ++i) {
    file.printf("%lu,AP,,%s,%s,%u,%d,%u%s\n",
                static_cast<unsigned long>(uptime),
                macToString(topoAps[i].bssid).c_str(),
                csvField(topoAps[i].ssid[0] ? topoAps[i].ssid : "<hidden>").c_str(),
                topoAps[i].channel,
                topoAps[i].rssi,
                topoAps[i].clientCount,
                location.c_str());
  }

  // Write Client nodes
  for (int i = 0; i < topoClientCount; ++i) {
    file.printf("%lu,CLIENT,%s,%s,,%u,%d,%u%s\n",
                static_cast<unsigned long>(uptime),
                macToString(topoClients[i].mac).c_str(),
                topoClients[i].hasBssid ? macToString(topoClients[i].bssid).c_str() : "",
                topoClients[i].channel,
                topoClients[i].rssi,
                topoClients[i].packetCount,
                location.c_str());
  }

  // Write Probe nodes
  for (int i = 0; i < topoProbeCount; ++i) {
    file.printf("%lu,PROBE,%s,,%s,,%d,%u%s\n",
                static_cast<unsigned long>(uptime),
                macToString(topoProbes[i].clientMac).c_str(),
                csvField(topoProbes[i].ssid).c_str(),
                topoProbes[i].rssi,
                topoProbes[i].count,
                location.c_str());
  }

  file.flush();
  const bool ok = (file.getWriteError() == 0);
  file.close();
  if (!ok) {
    SD.remove(temporaryPath.c_str());
    return false;
  }
  SD.remove(kTopologyCsvPath);
  if (!SD.rename(temporaryPath.c_str(), kTopologyCsvPath)) return false;
  Serial.printf("[topo] exported topology to %s\n", kTopologyCsvPath);
  return true;
}

void topologyEmitTelemetry() {
  // Emit stream rows for active entities
  for (int i = 0; i < topoApCount; ++i) {
    char rowBuf[128];
    int len = snprintf(rowBuf, sizeof(rowBuf), "$TOPO,AP,%s,%s,%u,%d,%d",
                       macToString(topoAps[i].bssid).c_str(),
                       topoAps[i].ssid[0] ? topoAps[i].ssid : "<hidden>",
                       topoAps[i].channel,
                       topoAps[i].rssi,
                       topoAps[i].isOpen ? 1 : 0);
    if (len > 0) {
      Serial.println(rowBuf);
#ifdef AWOK_HEADLESS
      if (g_bridgePhoneConnected) {
        bridgeNotifyResult(kSourceTopo, reinterpret_cast<const uint8_t*>(rowBuf), len);
      }
#endif
    }
  }

  for (int i = 0; i < topoClientCount; ++i) {
    if (!topoClients[i].hasBssid) continue;
    char rowBuf[128];
    int len = snprintf(rowBuf, sizeof(rowBuf), "$TOPO,CLI,%s,%s,%d,%u",
                       macToString(topoClients[i].mac).c_str(),
                       macToString(topoClients[i].bssid).c_str(),
                       topoClients[i].rssi,
                       topoClients[i].packetCount);
    if (len > 0) {
      Serial.println(rowBuf);
#ifdef AWOK_HEADLESS
      if (g_bridgePhoneConnected) {
        bridgeNotifyResult(kSourceTopo, reinterpret_cast<const uint8_t*>(rowBuf), len);
      }
#endif
    }
  }

  for (int i = 0; i < topoProbeCount; ++i) {
    char rowBuf[128];
    int len = snprintf(rowBuf, sizeof(rowBuf), "$TOPO,PRB,%s,%s,%d,%u",
                       macToString(topoProbes[i].clientMac).c_str(),
                       topoProbes[i].ssid,
                       topoProbes[i].rssi,
                       topoProbes[i].count);
    if (len > 0) {
      Serial.println(rowBuf);
#ifdef AWOK_HEADLESS
      if (g_bridgePhoneConnected) {
        bridgeNotifyResult(kSourceTopo, reinterpret_cast<const uint8_t*>(rowBuf), len);
      }
#endif
    }
  }
}

void drawTopologyMap() {
  currentView = View::kTopologyMap;
  display.fillScreen(kBackground);

  String headerSubtitle = String(topoApCount) + " AP | " + String(topoClientCount) + " Cli";
  if (topoProbeCount > 0) headerSubtitle += " | " + String(topoProbeCount) + " Prb";
  drawHeader("TOPOLOGY MAP", headerSubtitle);

  display.setTextSize(1);

#ifdef AWOK_MINI_DISPLAY
  int y = 30;
  int linesRendered = 0;
  constexpr int kMiniMaxLines = 6;

  for (int a = 0; a < topoApCount && linesRendered < kMiniMaxLines; ++a) {
    display.setTextColor(topoAps[a].isOpen ? kWarn : kAccent, kBackground);
    display.setCursor(4, y);
    display.printf("[%d] %s (%d)", topoAps[a].channel,
                   clipped(topoAps[a].ssid[0] ? topoAps[a].ssid : macToString(topoAps[a].bssid), 11).c_str(),
                   topoAps[a].clientCount);
    y += 12;
    ++linesRendered;

    for (int c = 0; c < topoClientCount && linesRendered < kMiniMaxLines; ++c) {
      if (!topoClients[c].hasBssid || memcmp(topoClients[c].bssid, topoAps[a].bssid, 6) != 0) continue;
      display.setTextColor(kForeground, kBackground);
      display.setCursor(12, y);
      display.printf("> %02X:%02X %ddB", topoClients[c].mac[4], topoClients[c].mac[5], topoClients[c].rssi);
      y += 12;
      ++linesRendered;
    }
  }

  if (topoApCount == 0 && topoClientCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(10, 60);
    display.print("Sniffing links...");
  }
#else
  int y = 48;
  int linesRendered = 0;
  constexpr int kTouchMaxLines = 11;

  for (int a = 0; a < topoApCount && linesRendered < kTouchMaxLines; ++a) {
    display.setTextColor(topoAps[a].isOpen ? kWarn : kAccent, kBackground);
    display.setCursor(6, y);
    display.printf("[Ch %2d] %s", topoAps[a].channel,
                   clipped(topoAps[a].ssid[0] ? topoAps[a].ssid : "<hidden>", 20).c_str());
    if (topoAps[a].isOpen) {
      display.setTextColor(kWarn, kBackground);
      display.print(" [OPEN]");
    }
    display.setTextColor(kMuted, kBackground);
    display.printf(" (%d cli, %d dBm)", topoAps[a].clientCount, topoAps[a].rssi);
    y += 18;
    ++linesRendered;

    for (int c = 0; c < topoClientCount && linesRendered < kTouchMaxLines; ++c) {
      if (!topoClients[c].hasBssid || memcmp(topoClients[c].bssid, topoAps[a].bssid, 6) != 0) continue;
      display.setTextColor(kForeground, kBackground);
      display.setCursor(18, y);
      display.printf("-> %s  %d dBm  (%d pkts)",
                     macToString(topoClients[c].mac).c_str(),
                     topoClients[c].rssi,
                     topoClients[c].packetCount);
      y += 18;
      ++linesRendered;
    }
  }

  // If room left, show probe leaks
  for (int p = 0; p < topoProbeCount && linesRendered < kTouchMaxLines; ++p) {
    display.setTextColor(kAccent, kBackground);
    display.setCursor(6, y);
    display.printf("[PRB] %02X:%02X -> \"%s\" (%d dBm)",
                   topoProbes[p].clientMac[4], topoProbes[p].clientMac[5],
                   clipped(topoProbes[p].ssid, 14).c_str(),
                   topoProbes[p].rssi);
    y += 18;
    ++linesRendered;
  }

  if (topoApCount == 0 && topoClientCount == 0) {
    display.setTextColor(kMuted, kBackground);
    display.setCursor(20, 140);
    display.print("Sniffing 802.11 client-AP associations & probes...");
  }
#endif

  drawFooter("Back", lastTopologyCsvOk ? "Saved" : "Save");
}

void startTopologyMap() {
  topoApCount = 0;
  topoClientCount = 0;
  topoProbeCount = 0;
  topoHitHead = 0;
  topoHitTail = 0;
  topoHopIndex = 0;
  topoTotalFrames = 0;
  lastTopoHopMs = millis();
  lastTopoDrawMs = 0;
  lastTopoTelemMs = 0;
  topoStartMs = millis();
  lastTopologyCsvOk = false;
  signalMonitorActive = false;

  // Pre-seed known APs from wifiEntries if previously scanned
  if (wifiCount > 0) {
    for (int i = 0; i < wifiCount && topoApCount < kMaxTopoAps; ++i) {
      uint8_t mac[6];
      if (parseBssid(wifiEntries[i].bssid, mac)) {
        int idx = topoApCount++;
        topoAps[idx] = TopoAp();
        memcpy(topoAps[idx].bssid, mac, 6);
        strncpy(topoAps[idx].ssid, wifiEntries[i].ssid.c_str(), sizeof(topoAps[idx].ssid) - 1);
        topoAps[idx].channel = wifiEntries[i].channel;
        topoAps[idx].rssi = wifiEntries[i].rssi;
        topoAps[idx].isOpen = (wifiEntries[i].auth == WIFI_AUTH_OPEN);
        topoAps[idx].lastSeenMs = millis();
      }
    }
  }

  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_promiscuous(false);

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(&topoPromiscuousCallback);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(kDeauthHopChannels[0], WIFI_SECOND_CHAN_NONE);

  topologyActive = true;
  Serial.println("[topo] swarm mesh topology map started");
  drawTopologyMap();
}

void stopTopologyMap() {
  topologyActive = false;
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_MODE_STA);
  WiFi.disconnect(true, false);
  lastTopologyCsvOk = exportTopologyToSd();
  Serial.printf("[topo] stopped; %d APs, %d clients, %d probes, %lu frames\n",
                topoApCount, topoClientCount, topoProbeCount,
                static_cast<unsigned long>(topoTotalFrames));
}

void updateTopologyMap() {
  if (!topologyActive) return;

  // Drain promiscuous hit queue
  while (topoHitTail != topoHitHead) {
    topoMergeHit(topoHitQueue[topoHitTail]);
    topoHitTail = (topoHitTail + 1) % kTopoHitQueueSlots;
  }

  const uint32_t now = millis();

  // Channel hopping
  if (now - lastTopoHopMs >= kTopoHopIntervalMs) {
    lastTopoHopMs = now;
    topoHopIndex = (topoHopIndex + 1) % kDeauthHopChannelCount;
    esp_wifi_set_channel(kDeauthHopChannels[topoHopIndex], WIFI_SECOND_CHAN_NONE);
  }

  // Telemetry stream
  if (now - lastTopoTelemMs >= kTopoTelemIntervalMs) {
    lastTopoTelemMs = now;
    topologyEmitTelemetry();
  }

  // Screen redraw
  if (currentView == View::kTopologyMap && now - lastTopoDrawMs >= kTopoRedrawMs) {
    lastTopoDrawMs = now;
    drawTopologyMap();
  }
}
