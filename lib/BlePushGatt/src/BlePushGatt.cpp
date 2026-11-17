// B3 connection layer — see include/BlePushGatt.h.
// Compiled only for the EEGO A4 peripheral build; every other board links the
// no-op stub at the bottom of this file.
#include "BlePushGatt.h"

#if defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4

#include <NimBLEDevice.h>
#include <NimBLEAdvertising.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <esp_wifi.h>
#include <esp_err.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace blepushgatt {

const char* kSvcUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10";
const char* kDataUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f11";
const char* kCtrlUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f12";
const char* kStatusUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f13";

std::atomic<bool> g_bleConnected{false};
std::atomic<bool> g_bleReceiving{false};

namespace {

Status s_status;
DataHandler s_dataHandler = nullptr;
CtrlHandler s_ctrlHandler = nullptr;
AiResultHandler s_aiResultHandler = nullptr;  // B5: dedicated 0x07 consumer
NimBLEServer* s_server = nullptr;
NimBLECharacteristic* s_data = nullptr;
NimBLECharacteristic* s_ctrl = nullptr;
NimBLECharacteristic* s_statusChr = nullptr;
std::atomic<bool> s_statusSubscribed{false};  // B4′: STATUS CCCD subscribed by peer

void log(const char* fmt, ...);
void startAdvertising();

// CTRL opcodes (push_proto.h, frozen).
constexpr uint8_t kOpBegin = 0x01;
constexpr uint8_t kOpEnd = 0x02;
constexpr uint8_t kOpAbort = 0x04;
constexpr uint8_t kOpAiResult = 0x07;  // B5: 0x07 single-frame AI definition

constexpr uint16_t kPreferredMtu = 247;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    g_bleConnected.store(true, std::memory_order_release);
    s_status.advertising = false;  // a live link needs no advertising
    s_status.mtu = NimBLEDevice::getMTU();
    log("connected peer=%s mtu=%u", info.getAddress().toString().c_str(), s_status.mtu);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    g_bleConnected.store(false, std::memory_order_release);
    g_bleReceiving.store(false, std::memory_order_release);  // never leave the flag stuck at 1
    s_statusSubscribed.store(false, std::memory_order_release);  // B4′: CCCD dies with the link
    log("disconnected reason=%d", reason);
    // Re-arm the always-on advertisement after the link drops.
    startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    s_status.mtu = mtu;
    log("mtu negotiated=%u", mtu);
  }
};

class CtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const std::string v = chr->getValue();
    const uint8_t* p = reinterpret_cast<const uint8_t*>(v.data());
    const size_t n = v.size();
    // CTRL frame layout: magic(2)=0x5043, opcode(1), version(1) at offset 2/3.
    const uint8_t op = (n >= 4) ? p[2] : 0;
    if (op == kOpAiResult) {
      // B5: 0x07 is the AI-definition result. Route to its dedicated consumer
      // only — never leak it to the B6.5 push-sink CtrlHandler (separable
      // responsibility, one variable per batch).
      if (s_aiResultHandler) s_aiResultHandler(p, n);
      return;
    }
    if (op == kOpBegin) {
      g_bleReceiving.store(true, std::memory_order_release);
    } else if (op == kOpEnd || op == kOpAbort) {
      g_bleReceiving.store(false, std::memory_order_release);
    }
    if (s_ctrlHandler) s_ctrlHandler(op, p, n);
  }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    const std::string v = chr->getValue();
    // DATA frame is raw seq(u16 LE)+len(u8)+payload — B6.5 owns the sink.
    if (s_dataHandler) {
      s_dataHandler(reinterpret_cast<const uint8_t*>(v.data()), v.size());
    }
  }
};

// B4′: track the STATUS CCCD so fire-and-forget scene notify never fires into a
// void (or throws when the peer has not subscribed). Mirrors the ACK/notify
// subscription gate in the pico reference.
class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
    const bool subscribed = (subValue & 0x0001u) != 0;
    s_statusSubscribed.store(subscribed, std::memory_order_release);
    log("STATUS cccd sub=%u", static_cast<unsigned>(subscribed));
  }
};

void log(const char* fmt, ...) {
  // Log through the same "BLE" origin the page turner uses; static buffer keeps
  // this translation unit free of Arduino String.
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logPrintf("INF", "BLE", "PUSH %s", line);
}

void startAdvertising() {
  if (s_server == nullptr) return;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr) return;
  adv->stop();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);  // BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP
  advData.setName(s_status.advName, true);
  adv->setAdvertisementData(advData);
  // 128-bit service UUID rides the scan response (adv data capped at 31B).
  NimBLEAdvertisementData scanRsp;
  scanRsp.addServiceUUID(kSvcUuid);
  adv->setScanResponseData(scanRsp);
  if (adv->start()) {
    s_status.advertising = true;
  } else {
    s_status.advertising = false;
    log("adv start failed");
  }
}

}  // namespace

uint16_t tokenFromStaMac() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return 0;
  return static_cast<uint16_t>((static_cast<uint16_t>(mac[4]) << 8) | mac[5]);
}

bool begin() {
  if (s_status.initialized) return true;

  if (!NimBLEDevice::isInitialized()) {
    if (!NimBLEDevice::init("CrossPoint-Push")) {
      log("NimBLE init failed");
      return false;
    }
  }
  // No bonding / no encryption for the push link (plan §155).
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setMTU(kPreferredMtu);

  const uint16_t token = tokenFromStaMac();
  snprintf(s_status.advName, sizeof(s_status.advName), "PicoPush-%04X", token);

  s_server = NimBLEDevice::createServer();
  if (s_server == nullptr) {
    log("createServer failed");
    return false;
  }
  s_server->setCallbacks(new ServerCallbacks(), true);

  NimBLEService* svc = s_server->createService(kSvcUuid);
  // WWR (Write Request) — NOT WRITE_NR. Mirrors push_sink.c s_svcs[].flags.
  s_data = svc->createCharacteristic(kDataUuid, NIMBLE_PROPERTY::WRITE);
  s_ctrl = svc->createCharacteristic(kCtrlUuid, NIMBLE_PROPERTY::WRITE);
  // READ|NOTIFY — NimBLE auto-creates the CCCD (0x2902).
  s_statusChr = svc->createCharacteristic(
      kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  s_data->setCallbacks(new DataCallbacks());
  s_ctrl->setCallbacks(new CtrlCallbacks());
  s_statusChr->setCallbacks(new StatusCallbacks());  // B4′: CCCD subscription gate

  s_status.initialized = true;
  s_status.mtu = NimBLEDevice::getMTU();
  log("svc=%s", kSvcUuid);
  log("DATA=%s flags=WRITE(WWR)", kDataUuid);
  log("CTRL=%s flags=WRITE(WWR)", kCtrlUuid);
  log("STATUS=%s flags=READ|NOTIFY(CCCD)", kStatusUuid);
  log("adv-name=%s preferred-mtu=%u", s_status.advName, kPreferredMtu);

  startAdvertising();
  return true;
}

void loop() {
  if (!s_status.initialized) return;
  if (!g_bleConnected.load(std::memory_order_acquire) && !s_status.advertising) {
    startAdvertising();
  }
}

const Status& status() { return s_status; }
void setDataHandler(DataHandler h) { s_dataHandler = h; }
void setCtrlHandler(CtrlHandler h) { s_ctrlHandler = h; }
void setAiResultHandler(AiResultHandler h) { s_aiResultHandler = h; }
bool radioOwned() { return s_status.initialized; }

bool canNotifyScene() {
  return s_statusChr != nullptr && s_status.initialized &&
         g_bleConnected.load(std::memory_order_acquire) &&
         s_statusSubscribed.load(std::memory_order_acquire);
}

bool sendSceneFrame(const uint8_t* frame, size_t len) {
  // diag-mtu: a 0x1A frame is 241 B. NimBLE 2.5.1 silently truncates an ATT
  // notify to (MTU-3) in ble_att_truncate_to_mtu() and still returns success —
  // so "notify() returned true" used to masquerade as "frame delivered whole".
  // Self-check BEFORE sending: if the frame cannot fit the negotiated MTU,
  // refuse (return false) and shout, never claim success. This is a bug fix,
  // not a protocol change: frame/flags/notify characteristic are untouched.
  constexpr size_t kSceneFrameMax = 241;
  if (!canNotifyScene() || frame == nullptr || len == 0 || len > kSceneFrameMax) return false;

  const uint16_t mtu = s_status.mtu;
  const size_t capacity = (mtu > 3) ? (static_cast<size_t>(mtu) - 3u) : 0u;
  const size_t wire = (len < capacity) ? len : capacity;  // = min(len, mtu-3)

  // (4) wire bytes actually handed to ATT vs min(len, mtu-3).
  log("scene notify len=%u wire=%u cap=%u mtu=%u",
      (unsigned)len, (unsigned)wire, (unsigned)capacity, (unsigned)mtu);

  // (5) truncation warning + MTU self-check: refuse instead of pretending success.
  if (len > capacity) {
    log("WARN scene notify MTU TOO SMALL len=%u cap=%u mtu=%u -> REFUSED",
        (unsigned)len, (unsigned)capacity, (unsigned)mtu);
    return false;
  }

  s_statusChr->setValue(const_cast<uint8_t*>(frame), len);
  // (6) failure warning: notify() returning false is now visible on the console.
  if (!s_statusChr->notify()) {
    log("WARN scene notify failed len=%u mtu=%u", (unsigned)len, (unsigned)mtu);
    return false;
  }
  return true;
}

}  // namespace blepushgatt

#else  // non-EEGO-A4 or peripheral role disabled: inert stubs

namespace blepushgatt {
const char* kSvcUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10";
const char* kDataUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f11";
const char* kCtrlUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f12";
const char* kStatusUuid = "e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f13";
std::atomic<bool> g_bleConnected{false};
std::atomic<bool> g_bleReceiving{false};
bool begin() { return false; }
void loop() {}
Status s_inert;
const Status& status() { return s_inert; }
uint16_t tokenFromStaMac() { return 0; }
void setDataHandler(DataHandler) {}
void setCtrlHandler(CtrlHandler) {}
void setAiResultHandler(AiResultHandler) {}
bool radioOwned() { return false; }
bool sendSceneFrame(const uint8_t*, size_t) { return false; }
bool canNotifyScene() { return false; }
}  // namespace blepushgatt

#endif
