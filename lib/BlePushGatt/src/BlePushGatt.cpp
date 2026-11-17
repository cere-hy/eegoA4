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
NimBLEServer* s_server = nullptr;
NimBLECharacteristic* s_data = nullptr;
NimBLECharacteristic* s_ctrl = nullptr;
NimBLECharacteristic* s_statusChr = nullptr;

void log(const char* fmt, ...);
void startAdvertising();

// CTRL opcodes (push_proto.h, frozen).
constexpr uint8_t kOpBegin = 0x01;
constexpr uint8_t kOpEnd = 0x02;
constexpr uint8_t kOpAbort = 0x04;

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
    // CTRL frame layout: magic(2)=0x5043, version(1), opcode(1) at offset 2.
    const uint8_t op = (n >= 4) ? p[2] : 0;
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
bool radioOwned() { return s_status.initialized; }

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
bool radioOwned() { return false; }
}  // namespace blepushgatt

#endif
