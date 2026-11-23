#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// b65-s3c-pf9b phase-1 diagnostic switch (WiFi/BLE lifecycle only).
#include "Pf9bDiagConfig.h"

// System-level BLE push peripheral (EEGO A4 rebuild, batch B3).
//
// Owns a NimBLE GATT server whose service table is copied byte-for-byte from the
// pico reference (components/push_sink/src/push_sink.c, draft §3.1): 128-bit
// service e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10 with DATA(..11, WWR), CTRL(..12,
// WWR) and STATUS(..13, READ|NOTIFY + CCCD). Advertising is system-level and
// persistent from boot: "PicoPush-%04X", token = Wi-Fi STA MAC [4]<<8|[5].
//
// This module deliberately does NOT touch the display or the refresh chain.
// The status-bar icon is ordinary frame content that reads g_bleConnected
// during the next normal full-frame render (no callback ever repaints).
//
// The actual .part streaming sink / CRC32 / commit is B6.5; B3 delivers the
// connection layer only and exposes handler hooks for that later batch.
namespace blepushgatt {

extern const char* kSvcUuid;     // e2f5a1c0-9b7d-4e3f-8a61-3c9d2b7e5f10
extern const char* kDataUuid;    // ...5f11 (DATA, WWR, no opcode)
extern const char* kCtrlUuid;    // ...5f12 (CTRL, WWR, opcode at offset 2)
extern const char* kStatusUuid;  // ...5f13 (STATUS, READ|NOTIFY, CCCD)

// Connection / receive flags — global singletons, §7 "one radio, one master".
// g_bleConnected: a phone push link is up. g_bleReceiving: a CTRL BEGIN opened a
// receive session (set on 0x01, cleared on 0x02/0x04 or link loss).
extern std::atomic<bool> g_bleConnected;
extern std::atomic<bool> g_bleReceiving;

struct Status {
  bool initialized = false;
  bool advertising = false;
  uint16_t mtu = 0;
  char advName[32] = {};
};

// Idempotent. Initialises NimBLE (if needed), raises ATT MTU to 247, disables
// bonding, registers the service table and starts persistent advertising.
// Returns true on success. Logs WWR characteristic flags and negotiated MTU.
bool begin();

// Housekeeping on the main loop: re-arm advertising after a disconnect and
// clear g_bleReceiving if a link dropped mid-session. Never repaints.
void loop();

const Status& status();

// token = (wifi_sta_mac[4] << 8) | wifi_sta_mac[5]; 0 when the MAC is unreadable.
uint16_t tokenFromStaMac();

// Handler hooks for the B6.5 push sink. B3 leaves them null (no-ops).
using DataHandler = void (*)(const uint8_t* payload, size_t len);
using CtrlHandler = void (*)(uint8_t opcode, const uint8_t* frame, size_t len);
void setDataHandler(DataHandler h);
void setCtrlHandler(CtrlHandler h);

// B5: dedicated hook for the 0x07 AI-definition result (CTRL write, opcode at
// offset 2). Kept separate from the B6.5 CtrlHandler so the two CTRL consumers
// cannot clobber each other's registration ("one variable per batch" +
// separable responsibility). Fired on the NimBLE host task with the whole
// CTRL frame; the sink must copy/publish quickly and defer display work.
using AiResultHandler = void (*)(const uint8_t* frame, size_t len);
void setAiResultHandler(AiResultHandler h);

// Plan B: dedicated hook for the 0x08 AI-definition SEGMENTED result (CTRL
// write, opcode at offset 2). Same host-task/short-copy discipline as 0x07,
// kept as a separate handler so the single-frame 0x07 path stays untouched.
// Fired once per 241B segmented frame; the sink feeds its reassembler.
using AiSegHandler = void (*)(const uint8_t* frame, size_t len);
void setAiSegHandler(AiSegHandler h);

// True while the push peripheral owns the single radio; the HID page-turner
// central must refuse to start in that window (§7 connection exclusivity).
bool radioOwned();

// B4′: push one already-encoded 0x1A scene frame up the STATUS(..13) NOTIFY
// characteristic. No-op (returns false) when no phone link is up or the client
// has not subscribed to the CCCD — the local dictionary lookup never waits on
// this. The caller owns the frame buffer; notify is fire-and-forget.
bool sendSceneFrame(const uint8_t* frame, size_t len);
bool canNotifyScene();

// Plan B: notify a pre-encoded STATUS frame (e.g. 0x18 CAPS / 0x13 ERR) up the
// same STATUS(..13) NOTIFY characteristic via an independent mbuf (no shared
// value buffer). Sent automatically on CCCD subscribe for 0x18; exposed so the
// sink can also emit an explicit failure status. No-op when no link/subscribe.
bool sendStatusFrame(const uint8_t* frame, size_t len);

// ---- b65-s3c-pf9c FUNCTIONAL: WiFi/BLE time-division (stop + safe resume) ---
// The pf9c private NimBLE patch makes nimble_port_stop() wait for the real host
// task exit before deinit(), so the pf9b-2 stop/resume flow can be restored
// without the PC=0x0 crash. Resume is called only AFTER the parent Web activity
// has fully recycled WiFi (disconnect/stop/deinit), never on WiFi-page exit.
enum class WifiBleState : uint8_t {
  ACTIVE = 0,        // BLE resident, normal operation
  STOPPED_FOR_WIFI,  // L2 teardown done; WiFi owns the radio
  RESUMING,          // resume in progress
  DEAD,              // stop/resume failed terminally; reboot required
};
WifiBleState wifiBleState();

// L2 teardown on entering the WiFi-settings page: stop advertising, then
// NimBLEDevice::deinit(false) (host resources released, NVS bonds preserved).
// Idempotent. Emits one [WFBLE] pre-heap snapshot and the post-deinit
// controller status. On host-exit timeout it enters the sticky DEAD state and
// returns false; the caller must NOT proceed to deinit or pretend BLE is online.
bool stopForWifi();

// Adaptive resume after WiFi has been fully recycled. Conservative scheme A:
// read esp_bt_controller_get_status(), move the controller to IDLE only if
// needed, then rebuild through the normal NimBLEDevice::init()/begin() path.
// Idempotent. Failure enters the sticky DEAD state and returns false.
bool resumeForWifi();

// Called once at startup (from begin()). If the previous boot ended in a PANIC,
// erase only NimBLE's transient cache keys in NVS namespace "nimble_bond"
// (cccd_sec / p_dev_rec / rpa_rec / csfc_sec / ead_sec) while KEEPING the
// pairing/bond keys (peer_sec / our_sec / local_irk). This clears possibly
// corrupted subscription/peer state without forcing the phone to re-pair.
void clearBleNvsCacheAfterPanic();

#if defined(PF9B_MEMX_DIAG) && PF9B_MEMX_DIAG
// ---- b65-s3c-pf9b PHASE 1 DIAGNOSTIC ONLY (macro-isolated) --------------------
// Tear the BLE stack down one level at a time and emit a [MEMX] free/maxalloc
// snapshot after each level: idle -> stop_adv -> disable_bd -> disable_ctrl.
// The ladder is intentionally one-way (the host/controller are not re-armed);
// it exists solely to measure how much internal RAM each level releases so the
// phase-2 stop level can be chosen. Absent from every functional build.
void memxStopLadder();
// True once the diagnostic ladder has torn the stack down; loop() then becomes
// a no-op so no callback resurrects advertising against a deinit'd controller.
bool memxStopped();
// Probe esp_wifi_init() explicitly and log its real return code + heap snapshot.
// Called before the Arduino WiFi wrapper first touches the radio.
void memxWifiInitProbe();
#endif

}  // namespace blepushgatt
