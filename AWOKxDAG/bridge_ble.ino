// bridge_ble.ino — BLE GATT server for the headless orange bridge chip.
//
// Compiled ONLY under AWOK_HEADLESS (C5 or classic ESP32 bridge profiles). The bridge
// runs the same full firmware as the white chip, but with no screen; the phone
// drives it directly over BLE. Each command carries a target byte:
//   kTargetBridge -> run the tool locally on this chip (linkDispatchCommand)
//   kTargetScreen -> relay over ESP-NOW to the white screen chip (channel sweep)
// Telemetry/results from the bridge's own tools notify the phone directly; those
// relayed back from the white chip arrive over ESP-NOW and are forwarded too.
#ifdef AWOK_HEADLESS

static const char* kBridgeSvcUuid     = "a0d10000-1234-4a3c-8b21-000000000001";
static const char* kBridgeCmdUuid     = "a0d10000-1234-4a3c-8b21-000000000002";  // write
static const char* kBridgeStatusUuid  = "a0d10000-1234-4a3c-8b21-000000000003";  // notify
static const char* kBridgeResultsUuid = "a0d10000-1234-4a3c-8b21-000000000004";  // notify

NimBLECharacteristic* g_bridgeStatus = nullptr;
NimBLECharacteristic* g_bridgeResults = nullptr;
volatile bool g_bridgePhoneConnected = false;

// Relay a command to the white chip over ESP-NOW. The screen chip parks on the
// rendezvous channel when idle and hops across the plan while a tool runs, so:
//   1) burst on the rendezvous channel first -- an idle screen chip (the common
//      case: starting a tool, or anything issued from Home/a results view) gets
//      it instantly with no channel hopping at all;
//   2) then a short channel sweep as the fallback for a tool that is actively
//      hopping (e.g. Stop mid-scan).
// A per-command sequence number lets the screen act on it exactly once despite
// the burst + sweep repeats. Cutting 3 sweeps -> 2 and the dwell 3 ms -> 2 ms,
// plus the rendezvous burst, roughly halves the relay's on-air time and radio
// churn while keeping delivery reliable.
static const uint8_t kBridgeRvBurst = 4;      // rendezvous-channel sends first
static const uint8_t kBridgeSweeps = 2;       // fallback full-plan passes
static const uint32_t kBridgeChanDwellMs = 2; // per-channel dwell in a sweep
static uint16_t g_bridgeSeq = 0;

static void bridgeRelayToScreen(uint8_t op, uint8_t arg) {
  if (!linkEnsureEspNow()) return;
  g_bridgeSeq++;
  LinkPacket p;
  p.type = kLinkMsgCommand;
  p.code = g_bridgeSeq;
  p.reserved = static_cast<uint16_t>(op) | (static_cast<uint16_t>(arg) << 8);
  memcpy(p.srcMac, linkSelfMac, 6);

  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);
  for (uint8_t i = 0; i < kBridgeRvBurst; ++i) {
    esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&p), sizeof(p));
    delay(2);
  }
  for (uint8_t pass = 0; pass < kBridgeSweeps; ++pass) {
    for (int i = 0; i < kLink24ChannelCount; ++i) {
      esp_wifi_set_channel(kLink24Channels[i], WIFI_SECOND_CHAN_NONE);
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&p), sizeof(p));
      delay(kBridgeChanDwellMs);
    }
#ifndef AWOK_CLASSIC_ESP32
    for (int i = 0; i < kLink5ChannelCount; ++i) {
      esp_wifi_set_channel(kLink5Channels[i], WIFI_SECOND_CHAN_NONE);
      esp_now_send(kLinkBroadcastAddr, reinterpret_cast<uint8_t*>(&p), sizeof(p));
      delay(kBridgeChanDwellMs);
    }
#endif
  }
  esp_wifi_set_channel(kLinkChannel, WIFI_SECOND_CHAN_NONE);  // home for telem
}

// Push one status/result blob to the phone with a source tag prefixed.
void bridgeNotifyStatus(uint8_t source, const uint8_t* body, size_t len) {
  if (!g_bridgeStatus || !g_bridgePhoneConnected) return;
  uint8_t blob[1 + 32];
  blob[0] = source;
  size_t n = len < sizeof(blob) - 1 ? len : sizeof(blob) - 1;
  memcpy(blob + 1, body, n);
  g_bridgeStatus->setValue(blob, n + 1);
  g_bridgeStatus->notify();
}

void bridgeNotifyResult(uint8_t source, const uint8_t* body, size_t len) {
  if (!g_bridgeResults || !g_bridgePhoneConnected) return;
  uint8_t blob[1 + 180];  // fits a full WiGLE CSV row for wardrive streaming
  blob[0] = source;
  size_t n = len < sizeof(blob) - 1 ? len : sizeof(blob) - 1;
  memcpy(blob + 1, body, n);
  g_bridgeResults->setValue(blob, n + 1);
  g_bridgeResults->notify();
}

// A command may kick off a multi-second blocking scan; running that (and the
// result notifications) inside the BLE host-task onWrite callback would stall
// the stack and drop most notifications. So onWrite only latches the command and
// loop() (the Arduino loopTask) services it -- the host task stays free to drain
// notifications, exactly like the screen chip's queue-then-process path.
volatile uint8_t g_bridgePendOp = 0, g_bridgePendArg = 0, g_bridgePendTarget = 0;
volatile bool g_bridgeCmdPending = false;

class BridgeCmdCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    const NimBLEAttValue v = c->getValue();
    if (v.length() == 0) return;
    g_bridgePendOp = v.data()[0];
    g_bridgePendArg = v.length() > 1 ? v.data()[1] : 0;
    g_bridgePendTarget = v.length() > 2
                             ? v.data()[2]
                             : static_cast<uint8_t>(kTargetBridge);
    g_bridgeCmdPending = true;  // serviced on the main loop
  }
};

// Called from loop(): run any latched command off the BLE host task.
void bridgeServiceCommand() {
  if (!g_bridgeCmdPending) return;
  g_bridgeCmdPending = false;
  const uint8_t op = g_bridgePendOp, arg = g_bridgePendArg, target = g_bridgePendTarget;
  Serial.printf("[bridge] cmd op=%u arg=%u target=%u\n", op, arg, target);
  if (target == kTargetScreen) {
    bridgeRelayToScreen(op, arg);
  } else {
    linkDispatchCommand(op, arg);  // run on this chip
  }
}

class BridgeServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    g_bridgePhoneConnected = true;
    s->updateConnParams(info.getConnHandle(), 12, 12, 0, 200);
    Serial.println("[bridge] phone connected");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    g_bridgePhoneConnected = false;
    Serial.printf("[bridge] phone disconnected (%d); re-advertising\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

void bridgeBleBegin() {
  if (!NimBLEDevice::isInitialized()) NimBLEDevice::init("AxD-Bridge");
  NimBLEDevice::setPower(3);
  NimBLEDevice::setMTU(247);  // fewer fragments -> faster result/CSV streaming
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new BridgeServerCallbacks());
  NimBLEService* svc = server->createService(kBridgeSvcUuid);
  NimBLECharacteristic* cmd = svc->createCharacteristic(
      kBridgeCmdUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new BridgeCmdCallbacks());
  g_bridgeStatus = svc->createCharacteristic(
      kBridgeStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_bridgeStatus->setValue("ready");
  g_bridgeResults = svc->createCharacteristic(
      kBridgeResultsUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setName("AxD-Bridge");
  adv->start();
  Serial.println("[bridge] BLE GATT server up ('AxD-Bridge')");
}

#endif  // AWOK_HEADLESS
