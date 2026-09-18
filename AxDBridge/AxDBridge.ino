#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <NimBLEDevice.h>
#include "link_protocol.h"

static const char* kSvcUuid     = "a0d10000-1234-4a3c-8b21-000000000001";
static const char* kCmdUuid     = "a0d10000-1234-4a3c-8b21-000000000002";
static const char* kStatusUuid  = "a0d10000-1234-4a3c-8b21-000000000003";
static const char* kResultsUuid = "a0d10000-1234-4a3c-8b21-000000000004";

static NimBLECharacteristic* g_status = nullptr;
static NimBLECharacteristic* g_results = nullptr;
static volatile bool g_connected = false;
static uint8_t g_selfMac[6] = {0};

static const uint8_t kCmdSweeps = 3;
static const uint32_t kCmdSweepGapMs = 90;
static volatile uint8_t g_op = 0, g_arg = 0, g_sweepsLeft = 0;
static volatile uint16_t g_seq = 0;
static uint32_t g_nextStepMs = 0;
static int g_sweepChan = -1;

static uint8_t g_chanPlan[kLink24ChannelCount + kLink5ChannelCount];
static int g_chanPlanCount = 0;

static uint32_t g_lastScreenMs = 0;
static uint32_t g_lastBridgeBeatMs = 0;

static void bridgeBuildChanPlan() {
  g_chanPlanCount = 0;
  for (int i = 0; i < kLink24ChannelCount; ++i) g_chanPlan[g_chanPlanCount++] = kLink24Channels[i];
  for (int i = 0; i < kLink5ChannelCount; ++i) g_chanPlan[g_chanPlanCount++] = kLink5Channels[i];
}

static void bridgeNotify(NimBLECharacteristic* ch, const uint8_t* body, size_t len) {
  if (!ch || !g_connected) return;
  ch->setValue(body, len);
  ch->notify();
}

static void bridgeSendCommandFrame(uint8_t opcode, uint8_t arg, uint16_t seq) {
  LinkPacket p;
  p.type = kLinkMsgCommand;
  p.code = seq;
  p.reserved = static_cast<uint16_t>(opcode) | (static_cast<uint16_t>(arg) << 8);
  memcpy(p.srcMac, g_selfMac, 6);
  esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&p), sizeof(p));
}

static void bridgeQueueCommand(uint8_t opcode, uint8_t arg) {
  g_op = opcode;
  g_arg = arg;
  g_seq++;
  g_sweepsLeft = kCmdSweeps;
  g_sweepChan = 0;
  g_nextStepMs = 0;
  uint8_t echo[4] = {kSourceBridge, 0xC0, static_cast<uint8_t>(g_seq & 0xFF), opcode};
  bridgeNotify(g_status, echo, sizeof(echo));
  Serial.printf("[bridge] queue cmd op=%u arg=%u seq=%u\n", opcode, arg, g_seq);
}

static void bridgeStepCommandSweep() {
  if (!g_sweepsLeft) return;
  if (static_cast<int32_t>(millis() - g_nextStepMs) < 0) return;
  if (g_sweepChan < 0 || g_sweepChan >= g_chanPlanCount) {
    esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
    g_sweepChan = 0;
    if (--g_sweepsLeft == 0) { g_nextStepMs = 0; return; }
    g_nextStepMs = millis() + kCmdSweepGapMs;
    return;
  }
  esp_wifi_set_channel(g_chanPlan[g_sweepChan], WIFI_SECOND_CHAN_NONE);
  bridgeSendCommandFrame(g_op, g_arg, g_seq);
  g_sweepChan++;
  g_nextStepMs = millis() + 4;
}

static void bridgeForwardScreenStatus(const LinkPacket& p) {
  uint8_t blob[26];
  blob[0] = kSourceScreen;
  memcpy(blob + 1, &p.networks, 4);
  memcpy(blob + 5, &p.bleCount, 4);
  blob[9] = p.channel;
  blob[10] = (p.flags & kLinkTelemGpsFix) ? 1 : 0;
  blob[11] = static_cast<uint8_t>((p.reserved >> 8) & 0xFF);
  memcpy(blob + 12, &p.sessionId, 4);
  memcpy(blob + 16, &p.masterMillis, 4);
  blob[20] = (p.flags & kLinkTelemFleetActive) ? 1 : 0;
  blob[21] = static_cast<uint8_t>(p.reserved & 0xFF);
  memcpy(blob + 22, &p.code, 2);
  blob[24] = ((p.flags & kLinkTelemFleetCoordinator) ? 0x01 : 0) |
             ((p.flags & kLinkTelemFleetListening) ? 0x02 : 0) |
             ((p.flags & kLinkTelemFleetRunning) ? 0x04 : 0);
  blob[25] = p.role;
  bridgeNotify(g_status, blob, sizeof(blob));
}

static void bridgeSendOwnStatus() {
  uint8_t blob[26] = {0};
  blob[0] = kSourceBridge;
  blob[9] = 0xFF;
  bridgeNotify(g_status, blob, sizeof(blob));
}

static void onEspNowRecv(const esp_now_recv_info_t*, const uint8_t* data, int len) {
  if (len < 6 || !g_connected) return;
  uint32_t magic;
  memcpy(&magic, data, 4);
  if (magic != kLinkMagic) return;
  const uint8_t type = data[5];

  if (type == kLinkMsgTelem && len == static_cast<int>(sizeof(LinkPacket))) {
    LinkPacket p;
    memcpy(&p, data, sizeof(p));
    g_lastScreenMs = millis();
    bridgeForwardScreenStatus(p);
  } else if (type == kLinkMsgWifiResult && len == static_cast<int>(sizeof(AxdWifiResult))) {
    AxdWifiResult r;
    memcpy(&r, data, sizeof(r));
    if (!g_results) return;
    uint8_t blob[11 + 32];
    blob[0] = r.index;
    blob[1] = r.count;
    blob[2] = static_cast<uint8_t>(r.rssi);
    blob[3] = r.channel;
    blob[4] = r.auth;
    memcpy(blob + 5, r.bssid, 6);
    size_t sl = strnlen(r.ssid, 32);
    memcpy(blob + 11, r.ssid, sl);
    bridgeNotify(g_results, blob, 11 + sl);
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

  bridgeBuildChanPlan();
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
  peer.channel = 0;
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
  bridgeStepCommandSweep();
  const uint32_t now = millis();
  if (g_connected && now - g_lastBridgeBeatMs >= 1000) {
    g_lastBridgeBeatMs = now;
    bridgeSendOwnStatus();
  }
  delay(5);
}
