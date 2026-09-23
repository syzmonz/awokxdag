// AxD orange bridge chip: BLE GATT server <-> ESP-NOW to the white screen chip.
//
// The phone (Bluefy on iOS, Chrome on Android) connects to this chip over BLE
// and writes command opcodes; the bridge forwards each as an ESP-NOW command
// frame to the screen chip, which runs the tool. Telemetry the screen chip
// broadcasts (Wi-Fi/BLE counts, current tool) comes back over ESP-NOW and is
// pushed to the phone as a BLE notification. Headless: no display/SD/GPS. Wi-Fi
// is used only for ESP-NOW (fixed rendezvous channel), so BLE + ESP-NOW coexist
// comfortably on one C5 (validated: ~106 KB DMA free while connected).
//
// The GATT UUIDs match website/public/control.html. LinkPacket / opcodes are the
// shared link_protocol.h (same file the screen firmware uses), so the on-air
// format can't drift. Build with -I../AWOKxDAG so that header resolves.
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include "link_protocol.h"

static const char* kSvcUuid     = "a0d10000-1234-4a3c-8b21-000000000001";
static const char* kCmdUuid     = "a0d10000-1234-4a3c-8b21-000000000002";  // write
static const char* kStatusUuid  = "a0d10000-1234-4a3c-8b21-000000000003";  // notify
static const char* kResultsUuid = "a0d10000-1234-4a3c-8b21-000000000004";  // notify

static NimBLECharacteristic* g_status = nullptr;
static NimBLECharacteristic* g_results = nullptr;
static volatile bool g_connected = false;
static uint8_t g_selfMac[6] = {0};

// The screen chip hops across every channel while a tool runs, so a command
// sent only on the rendezvous channel is missed (that's why Stop failed inside
// a running tool). Instead the bridge SWEEPS the whole channel plan, sending on
// each, and repeats the sweep a few times -- so the command lands on whatever
// channel the screen is currently on. A per-command sequence number (p.code)
// lets the screen act on it exactly once despite the repeats. When idle the
// screen sits on ch1, so the sweep still delivers there too.
static const uint8_t kCmdSweeps = 3;      // full channel passes per command
static const uint32_t kCmdSweepGapMs = 120;
static const uint32_t kChanDwellMs = 3;   // per-channel dwell within a sweep
static volatile uint8_t g_op = 0, g_arg = 0, g_sweepsLeft = 0;
static volatile uint16_t g_seq = 0;
static uint32_t g_nextSweepMs = 0;

static void bridgeSendOnCurrentChannel(uint8_t opcode, uint8_t arg, uint16_t seq) {
  LinkPacket p;
  p.type = kLinkMsgCommand;
  p.code = seq;
  p.reserved = static_cast<uint16_t>(opcode) |
               (static_cast<uint16_t>(arg) << 8);
  memcpy(p.srcMac, g_selfMac, 6);
  esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&p), sizeof(p));
}

// One pass over the whole channel plan, sending the command on each channel,
// then home to the rendezvous channel so telemetry reception resumes.
static void bridgeSweepCommand(uint8_t opcode, uint8_t arg, uint16_t seq) {
  for (int i = 0; i < kLink24ChannelCount; ++i) {
    esp_wifi_set_channel(kLink24Channels[i], WIFI_SECOND_CHAN_NONE);
    bridgeSendOnCurrentChannel(opcode, arg, seq);
    delay(kChanDwellMs);
  }
  for (int i = 0; i < kLink5ChannelCount; ++i) {
    esp_wifi_set_channel(kLink5Channels[i], WIFI_SECOND_CHAN_NONE);
    bridgeSendOnCurrentChannel(opcode, arg, seq);
    delay(kChanDwellMs);
  }
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
}

static void bridgeQueueCommand(uint8_t opcode, uint8_t arg) {
  g_op = opcode;
  g_arg = arg;
  g_seq++;
  g_sweepsLeft = kCmdSweeps;
  g_nextSweepMs = 0;  // fire immediately in loop()
  Serial.printf("[bridge] queue cmd op=%u arg=%u seq=%u\n", opcode, arg, g_seq);
}

// Screen chip -> bridge: relay telemetry to the phone as a status notification.
// Blob layout matches control.html parseStatus(): [wifi u32][ble u32][tool u8].
static void onEspNowRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
  if (len < 6 || !g_connected) return;
  uint32_t magic;
  memcpy(&magic, data, 4);
  if (magic != kLinkMagic) return;
  const uint8_t type = data[5];  // magic(4) version(1) type(1) — shared prefix

  if (type == kLinkMsgTelem && len == static_cast<int>(sizeof(LinkPacket))) {
    LinkPacket p;
    memcpy(&p, data, sizeof(p));
    if (!g_status) return;
    uint8_t blob[25] = {0};
    blob[0] = 1;  // kSourceScreen
    memcpy(blob + 1, &p.networks, 4);
    memcpy(blob + 5, &p.bleCount, 4);
    blob[9] = p.channel;
    if (p.flags & kLinkTelemStatus) {
      blob[10] = (p.flags & kLinkTelemGpsFix) ? 1 : 0;
      blob[11] = static_cast<uint8_t>(p.reserved >> 8);
      memcpy(blob + 12, &p.sessionId, 4);    // latitude float bits
      memcpy(blob + 16, &p.masterMillis, 4); // longitude float bits
      blob[20] = (p.flags & kLinkTelemFleetActive) ? 1 : 0;
      blob[21] = static_cast<uint8_t>(p.reserved & 0xFF);
      memcpy(blob + 22, &p.code, 2);
      blob[24] = ((p.flags & kLinkTelemFleetCoordinator) ? 0x01 : 0) |
                 ((p.flags & kLinkTelemFleetListening) ? 0x02 : 0) |
                 ((p.flags & kLinkTelemFleetRunning) ? 0x04 : 0);
    }
    g_status->setValue(blob, sizeof(blob));
    g_status->notify();
  } else if (type == kLinkMsgWifiResult &&
             len == static_cast<int>(sizeof(AxdWifiResult))) {
    AxdWifiResult r;
    memcpy(&r, data, sizeof(r));
    if (!g_results) return;
    // Compact blob for the phone: source,index,count,rssi,channel,auth,bssid[6],ssid.
    uint8_t blob[1 + 11 + 32];
    blob[0] = 1;  // kSourceScreen
    blob[1] = r.index;
    blob[2] = r.count;
    blob[3] = static_cast<uint8_t>(r.rssi);
    blob[4] = r.channel;
    blob[5] = r.auth;
    memcpy(blob + 6, r.bssid, 6);
    size_t sl = strnlen(r.ssid, 32);
    memcpy(blob + 12, r.ssid, sl);
    g_results->setValue(blob, 1 + 11 + sl);
    g_results->notify();
  } else if (type == kLinkMsgFleetHuntResult &&
             len == static_cast<int>(sizeof(FleetHuntResult))) {
    FleetHuntResult r;
    memcpy(&r, data, sizeof(r));
    if (!g_results) return;
    char bssidStr[20];
    snprintf(bssidStr, sizeof(bssidStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5]);
    char rowBuf[128];
    int rlen = snprintf(rowBuf, sizeof(rowBuf), "$HUNT,%s,%s,%.6f,%.6f,%.1f,%.1f,%.1f,%d,%u",
                        bssidStr, r.ssid,
                        r.lat, r.lon,
                        r.distanceM, r.bearingDeg,
                        r.confidenceM,
                        r.rssi,
                        r.points);
    if (rlen > 0) {
      uint8_t blob[1 + 128];
      blob[0] = 3;  // kSourceHunt
      memcpy(blob + 1, rowBuf, rlen);
      g_results->setValue(blob, 1 + rlen);
      g_results->notify();
    }
  } else if (type == kLinkMsgFleetTopology &&
             len == static_cast<int>(sizeof(FleetTopologyLink))) {
    FleetTopologyLink r;
    memcpy(&r, data, sizeof(r));
    if (!g_results) return;
    char clientStr[20];
    snprintf(clientStr, sizeof(clientStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             r.clientMac[0], r.clientMac[1], r.clientMac[2], r.clientMac[3], r.clientMac[4], r.clientMac[5]);
    char rowBuf[128];
    int rlen = 0;
    if (r.linkType == 0) {
      char targetStr[20];
      snprintf(targetStr, sizeof(targetStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               r.targetMac[0], r.targetMac[1], r.targetMac[2], r.targetMac[3], r.targetMac[4], r.targetMac[5]);
      rlen = snprintf(rowBuf, sizeof(rowBuf), "$TOPO,CLI,%s,%s,%d,1",
                      clientStr, targetStr, r.rssi);
    } else {
      rlen = snprintf(rowBuf, sizeof(rowBuf), "$TOPO,PRB,%s,%s,%d,1",
                      clientStr, r.targetName, r.rssi);
    }
    if (rlen > 0) {
      uint8_t blob[1 + 128];
      blob[0] = 4;  // kSourceTopo
      memcpy(blob + 1, rowBuf, rlen);
      g_results->setValue(blob, 1 + rlen);
      g_results->notify();
    }
  }
}

class CmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue v = c->getValue();
    if (v.length() == 0) return;
    const uint8_t op = v.data()[0];
    const uint8_t arg = v.length() > 1 ? v.data()[1] : 0;
    bridgeQueueCommand(op, arg);
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    g_connected = true;
    // Ask for a short connection interval (~15 ms) so the burst of Wi-Fi result
    // notifications drains without overflowing the ATT tx queue (dropping APs).
    s->updateConnParams(info.getConnHandle(), 12, 12, 0, 200);
    Serial.println("[bridge] phone connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    g_connected = false;
    Serial.printf("[bridge] phone disconnected (%d); re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== AxD bridge (BLE <-> ESP-NOW) ===");

  // Wi-Fi only for ESP-NOW: pin to the rendezvous channel, no scanning.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, g_selfMac);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[bridge] esp_now_init failed");
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_set_pmk(kLinkPmk);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kLinkBroadcastAddr, 6);
  peer.channel = 0;  // 0 = send on whatever channel we're currently set to
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(kLinkBroadcastAddr)) esp_now_add_peer(&peer);

  NimBLEDevice::init("AxD-Bridge");
  NimBLEDevice::setPower(3);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* svc = server->createService(kSvcUuid);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      kCmdUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new CmdCallbacks());
  g_status = svc->createCharacteristic(
      kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_status->setValue("ready");
  g_results = svc->createCharacteristic(
      kResultsUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setName("AxD-Bridge");
  adv->start();
  Serial.printf("[bridge] up: BLE 'AxD-Bridge' + ESP-NOW on ch %u\n", kLinkChannel);
}

void loop() {
  if (g_sweepsLeft && static_cast<int32_t>(millis() - g_nextSweepMs) >= 0) {
    bridgeSweepCommand(g_op, g_arg, g_seq);
    g_sweepsLeft--;
    g_nextSweepMs = millis() + kCmdSweepGapMs;
  }
  static uint32_t lastSelfStatusMs = 0;
  if (g_connected && g_status && millis() - lastSelfStatusMs >= 1000) {
    lastSelfStatusMs = millis();
    uint8_t blob[25] = {0};
    blob[0] = 0;  // kSourceBridge
    blob[9] = g_sweepsLeft ? g_op : 0;
    g_status->setValue(blob, sizeof(blob));
    g_status->notify();
  }
  delay(10);
}
