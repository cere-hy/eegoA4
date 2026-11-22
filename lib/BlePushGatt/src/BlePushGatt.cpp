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
#include <push_ai_seg.h>  // plan B: 0x08/0x18/0x13 opcodes + caps encoder
#include <esp_bt.h>            // b65-s3c-pf9b-2: controller status/get + adaptive resume
#include <Arduino.h>           // b65-s3c-pf9b-2: millis() for resume timing; ESP.getFreeHeap for diag
#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
#include <esp_heap_caps.h>
#endif

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

// plan B: defined at namespace scope below (not in the anonymous namespace),
// so the forward declaration must live here too for onSubscribe() to link.
bool sendStatusFrame(const uint8_t* frame, size_t len);

namespace {

Status s_status;
DataHandler s_dataHandler = nullptr;
CtrlHandler s_ctrlHandler = nullptr;
AiResultHandler s_aiResultHandler = nullptr;  // B5: dedicated 0x07 consumer
AiSegHandler s_aiSegHandler = nullptr;        // plan B: dedicated 0x08 consumer
NimBLEServer* s_server = nullptr;
NimBLECharacteristic* s_data = nullptr;
NimBLECharacteristic* s_ctrl = nullptr;
NimBLECharacteristic* s_statusChr = nullptr;
std::atomic<bool> s_statusSubscribed{false};  // B4′: STATUS CCCD subscribed by peer
// b65-s3c-pf9b-2: functional WiFi/BLE time-division state. Sticky DEAD terminal.
std::atomic<uint8_t> s_wifiBleState{static_cast<uint8_t>(WifiBleState::ACTIVE)};
#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
std::atomic<bool> s_memxStopped{false};  // phase-1 one-way ladder latch
#endif

void log(const char* fmt, ...);
void startAdvertising();
#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
void memxSnap(const char* stage);
#endif

// CTRL opcodes (push_proto.h, frozen).
constexpr uint8_t kOpBegin = 0x01;
constexpr uint8_t kOpEnd = 0x02;
constexpr uint8_t kOpAbort = 0x04;
constexpr uint8_t kOpAiResult = 0x07;  // B5: 0x07 single-frame AI definition
constexpr uint8_t kOpAiResultSeg = 0x08;  // plan B: 0x08 segmented AI definition

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
    if (op == kOpAiResultSeg) {
      // Plan B: 0x08 is a segmented AI-definition frame (241B, whole frame
      // incl. 4B CTRL header). Hand the raw frame to the dedicated reassembler
      // sink; never leak to the 0x07 handler or B6.5 CtrlHandler.
      if (s_aiSegHandler) s_aiSegHandler(p, n);
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
    // Plan B REQ3: advertise CAP_AI_MULTI in the SAME connection event the peer
    // subscribes, so devCaps is set before any AI query can be tapped. Without
    // this bit the APK keeps sending 0x07 short frames (169B truncation).
    if (subscribed) {
      uint8_t caps[5];
      const size_t n = pas_caps_encode(PAS_CAP_AI_MULTI, caps, sizeof(caps));
      if (n && sendStatusFrame(caps, n)) {
        log("STATUS 0x18 caps=0x%x sent (CAP_AI_MULTI)", (unsigned)PAS_CAP_AI_MULTI);
      } else {
        log("WARN STATUS 0x18 caps send FAILED");
      }
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

#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
// Phase-1 measurement snapshot. Internal-RAM free + largest contiguous block,
// printed with the exact [MEMX] prefix the acceptance sheet greps for.
void memxSnap(const char* stage) {
  logPrintf("INF", "MEMX", "[MEMX] stage=%-14s free=%u maxalloc=%u", stage,
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}
#endif

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
#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
  // After the one-way measurement ladder the host/controller are gone; never
  // try to re-arm advertising against them.
  if (s_memxStopped.load(std::memory_order_acquire)) return;
#else
  // b65-s3c-pf9b-2: while WiFi owns the radio (or resume failed terminally),
  // never re-arm advertising against a deinit'd host.
  if (s_wifiBleState.load(std::memory_order_acquire) != static_cast<uint8_t>(WifiBleState::ACTIVE)) return;
#endif
  if (!s_status.initialized) return;
  if (!g_bleConnected.load(std::memory_order_acquire) && !s_status.advertising) {
    startAdvertising();
  }
}

#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
// ---- b65-s3c-pf9b phase-1 one-way teardown ladder (DIAGNOSTIC ONLY) -----------
// Levels are cumulative: idle -> stop advertising -> deinit NimBLE host (bond
// preserved) -> deinit BT controller. A snapshot is taken after each level so
// the acceptance sheet can see exactly how much internal RAM each releases and
// decide the minimum safe stop level for the phase-2 functional build.
void memxStopLadder() {
  if (s_memxStopped.load(std::memory_order_acquire)) return;  // idempotent

  // L0: BLE resident, WiFi not yet initialised by this screen.
  memxSnap("idle");

  // L1: stop advertising only — host and controller stay up.
  if (NimBLEDevice::isInitialized()) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv != nullptr) adv->stop();
  }
  s_status.advertising = false;
  delay(50);
  memxSnap("stop_adv");

  // L2: deinit the NimBLE host. clearAll=false keeps bonds in NVS (clause 10:
  // pairing must survive; this level is the fallback if controller deinit
  // loses pairing). On IDF 5.x deinit() stops/deinits nimble_port but leaves the
  // controller allocated, so it does NOT by itself release the ~46.5 KB block.
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(false);
  }
  s_status.initialized = false;
  s_status.advertising = false;
  s_server = nullptr;
  s_data = s_ctrl = s_statusChr = nullptr;
  delay(50);
  memxSnap("disable_bd");

  // b65-s3c-pf9b-2: L3 (esp_bt_controller_disable/deinit) REMOVED. Measured
  // +0 free/maxalloc with rc=0x103 (ESP_ERR_INVALID_STATE) — never effective.
  // L2 alone frees ~41 KB and lifts maxalloc 7156 -> 26612, enough for WiFi.
  s_memxStopped.store(true, std::memory_order_release);
}

bool memxStopped() { return s_memxStopped.load(std::memory_order_acquire); }

void memxWifiInitProbe() {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  const esp_err_t rc = esp_wifi_init(&cfg);
  logPrintf("INF", "MEMX", "[MEMX] wifi after_init   free=%u maxalloc=%u rc=0x%X",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
            static_cast<unsigned>(rc));
}
#endif

// ---- b65-s3c-pf9b-2 FUNCTIONAL: WiFi/BLE time-division -----------------------
// L2 teardown. clearAll=false keeps NVS bonds so the phone never needs to
// re-pair (measured: deinit(false) frees ~41 KB of host internal RAM and lifts
// maxalloc 7156 -> 26612, enough for WiFi; L3 controller deinit added nothing
// and returned 0x103, so it is deliberately omitted here).
bool stopForWifi() {
  const uint8_t st = s_wifiBleState.load(std::memory_order_acquire);
  if (st == static_cast<uint8_t>(WifiBleState::DEAD)) return false;  // sticky terminal
  if (st == static_cast<uint8_t>(WifiBleState::STOPPED_FOR_WIFI)) return true;  // idempotent

  // L1: stop advertising only.
  if (NimBLEDevice::isInitialized()) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv != nullptr) adv->stop();
  }
  s_status.advertising = false;

  // L2: deinit the NimBLE host, keeping bonds.
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(false);
  }
  s_status.initialized = false;
  s_server = nullptr;
  s_data = s_ctrl = s_statusChr = nullptr;
  s_statusSubscribed.store(false, std::memory_order_release);
  s_wifiBleState.store(static_cast<uint8_t>(WifiBleState::STOPPED_FOR_WIFI), std::memory_order_release);

  // Clause: real-device one-shot confirmation of the controller state that the
  // adaptive resume must handle (必答1). esp_bt_controller_get_status():
  // 0=IDLE, 1=INITED (initialized but disabled), 2=ENABLED.
  logPrintf("INF", "WFBLE", "[WFBLE] stop done; controller_status=%d",
            static_cast<int>(esp_bt_controller_get_status()));
  return true;
}

// Conservative scheme A resume. Never bypasses NimBLEDevice::init(): begin()
// calls it internally. Before that, read the controller status (必答1) and,
// only if needed, move the controller back to IDLE so init()'s internal
// esp_bt_controller_init()/enable() won't fail with 0x103. Branch taken (必答3)
// and every deinit() return code (必答2) are logged under tag "WFBLE".
bool resumeForWifi() {
  const uint8_t st0 = s_wifiBleState.load(std::memory_order_acquire);
  if (st0 == static_cast<uint8_t>(WifiBleState::DEAD)) {
    logPrintf("INF", "WFBLE", "[WFBLE] resume skipped (DEAD terminal)");
    return false;
  }
  if (st0 == static_cast<uint8_t>(WifiBleState::ACTIVE)) {
    return true;  // idempotent: nothing was torn down
  }

  s_wifiBleState.store(static_cast<uint8_t>(WifiBleState::RESUMING), std::memory_order_release);
  const uint32_t t0 = static_cast<uint32_t>(millis());

  const esp_bt_controller_status_t cst = esp_bt_controller_get_status();
  logPrintf("INF", "WFBLE", "[WFBLE] resume start; controller_status=%d", static_cast<int>(cst));

  // Scheme A: adapt controller state only, never bypass NimBLEDevice::init().
  // begin() -> NimBLEDevice::init() -> esp_bt_controller_init()+enable() requires
  // the controller to be IDLE first; if it is still INITIALIZED/ENABLED, bring
  // it down here so init() can rebuild it cleanly.
  if (cst == ESP_BT_CONTROLLER_STATUS_ENABLED) {
    logPrintf("INF", "WFBLE", "[WFBLE] branch=ENABLED");
    const esp_err_t rcDis = esp_bt_controller_disable();
    logPrintf("INF", "WFBLE", "[WFBLE] controller_disable rc=0x%X", static_cast<unsigned>(rcDis));
    const esp_err_t rcDeinit = esp_bt_controller_deinit();
    logPrintf("INF", "WFBLE", "[WFBLE] controller_deinit rc=0x%X", static_cast<unsigned>(rcDeinit));
  } else if (cst == ESP_BT_CONTROLLER_STATUS_INITED) {
    logPrintf("INF", "WFBLE", "[WFBLE] branch=INITED");
    const esp_err_t rcDeinit = esp_bt_controller_deinit();
    logPrintf("INF", "WFBLE", "[WFBLE] controller_deinit rc=0x%X", static_cast<unsigned>(rcDeinit));
  } else if (cst == ESP_BT_CONTROLLER_STATUS_IDLE) {
    logPrintf("INF", "WFBLE", "[WFBLE] branch=IDLE (init directly)");
  } else {
    logPrintf("INF", "WFBLE", "[WFBLE] branch=OTHER(%d); attempting init as-is", static_cast<int>(cst));
  }

  // deinit(false) at stop time deliberately kept the C++ GATT objects (bonds
  // live in NVS, not in them), but their NimBLE handles are stale now. Reclaim
  // them before begin() rebuilds the server/service table — deinit(true) only
  // deletes those C++ objects; it never touches NVS bonds. m_initialized is
  // already false so the port stop/deinit part is skipped.
  NimBLEDevice::deinit(true);

  // Rebuild via the normal begin() path (which calls NimBLEDevice::init()).
  const bool ok = begin();
  const uint32_t elapsed = static_cast<uint32_t>(millis()) - t0;
  if (!ok) {
    s_wifiBleState.store(static_cast<uint8_t>(WifiBleState::DEAD), std::memory_order_release);
    logPrintf("INF", "WFBLE", "[WFBLE] resume FAILED in %ums -> DEAD", elapsed);
    return false;
  }
  s_wifiBleState.store(static_cast<uint8_t>(WifiBleState::ACTIVE), std::memory_order_release);
  logPrintf("INF", "WFBLE", "[WFBLE] resume OK in %ums; advertising=%d discoverable", elapsed,
            s_status.advertising ? 1 : 0);
  return true;
}

WifiBleState wifiBleState() {
  return static_cast<WifiBleState>(s_wifiBleState.load(std::memory_order_acquire));
}

const Status& status() { return s_status; }
void setDataHandler(DataHandler h) { s_dataHandler = h; }
void setCtrlHandler(CtrlHandler h) { s_ctrlHandler = h; }
void setAiResultHandler(AiResultHandler h) { s_aiResultHandler = h; }
void setAiSegHandler(AiSegHandler h) { s_aiSegHandler = h; }
bool radioOwned() { return s_status.initialized; }

bool canNotifyScene() {
  return s_statusChr != nullptr && s_status.initialized &&
         g_bleConnected.load(std::memory_order_acquire) &&
         s_statusSubscribed.load(std::memory_order_acquire);
}

bool sendSceneFrame(const uint8_t* frame, size_t len) {
  // diag-pace3 (root-cause fix): a 0x1A frame is 241 B. NimBLE 2.5.1 silently
  // truncates an ATT notify to (MTU-3) in ble_att_truncate_to_mtu() and still
  // returns success — so "notify() returned true" used to masquerade as "frame
  // delivered whole". Self-check BEFORE sending: if the frame cannot fit the
  // negotiated MTU, refuse (return false) and shout, never claim success.
  constexpr size_t kSceneFrameMax = 241;
  if (!canNotifyScene() || frame == nullptr || len == 0 || len > kSceneFrameMax) return false;

  const uint16_t mtu = s_status.mtu;
  const size_t capacity = (mtu > 3) ? (static_cast<size_t>(mtu) - 3u) : 0u;
  const size_t wire = (len < capacity) ? len : capacity;  // = min(len, mtu-3)

  // (5) truncation warning + MTU self-check: refuse instead of pretending success.
  if (len > capacity) {
    log("WARN scene notify MTU TOO SMALL len=%u cap=%u mtu=%u -> REFUSED",
        (unsigned)len, (unsigned)capacity, (unsigned)mtu);
    return false;
  }

  // ROOT-CAUSE FIX (diag-pace3): the old path did setValue(frame) then a no-arg
  // notify(). No-arg notify() ends in ble_gatts_chr_updated(), a *marker* on the
  // characteristic's shared value buffer: two frames within one connection
  // event overwrite that buffer and the markers coalesce -> only one frame on
  // air (exactly the 1ms back-to-back / 100% double-frame failure seen in the
  // field). The value+len+connHandle overload instead calls
  // ble_hs_mbuf_from_flat() per frame -> ble_gattc_notify_custom(): a *separate*
  // mbuf queued per frame, no shared buffer, no coalescing. Protocol/B3/0x07
  // untouched — this is a send-mechanism bug fix, not a protocol change.
  uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
  if (s_server != nullptr) {
    const auto peers = s_server->getPeerDevices();
    if (!peers.empty()) conn = peers.front();
  }
  const bool ok = s_statusChr->notify(frame, len, conn);

  // (6) per-frame observability: every notify prints its own result + conn +
  // wire bytes. A failed enqueue or a truncated send is never silent again.
  log("scene notify conn=%u len=%u wire=%u cap=%u mtu=%u result=%s",
      (unsigned)conn, (unsigned)len, (unsigned)wire, (unsigned)capacity, (unsigned)mtu,
      ok ? "queued" : "FAILED");
  if (!ok) {
    log("WARN scene notify FAILED len=%u mtu=%u conn=%u", (unsigned)len, (unsigned)mtu, (unsigned)conn);
  }
  return ok;
}

bool sendStatusFrame(const uint8_t* frame, size_t len) {
  // Plan B: 0x18 CAPS / 0x13 ERR are tiny STATUS notifies. Same independent-mbuf
  // path as sendSceneFrame (no setValue, no shared-buffer marker semantics).
  if (!canNotifyScene() || frame == nullptr || len == 0 || len > PAS_ST_MAX) return false;
  uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
  if (s_server != nullptr) {
    const auto peers = s_server->getPeerDevices();
    if (!peers.empty()) conn = peers.front();
  }
  const bool ok = s_statusChr->notify(frame, len, conn);
  log("status notify op=0x%02X len=%u conn=%u result=%s",
      (unsigned)frame[0], (unsigned)len, (unsigned)conn, ok ? "queued" : "FAILED");
  return ok;
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
void setAiSegHandler(AiSegHandler) {}
bool radioOwned() { return false; }
bool sendSceneFrame(const uint8_t*, size_t) { return false; }
bool canNotifyScene() { return false; }
bool sendStatusFrame(const uint8_t*, size_t) { return false; }
WifiBleState wifiBleState() { return WifiBleState::ACTIVE; }
bool stopForWifi() { return true; }
bool resumeForWifi() { return true; }
#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
void memxStopLadder() {}
bool memxStopped() { return false; }
void memxWifiInitProbe() {}
#endif
}  // namespace blepushgatt

#endif
