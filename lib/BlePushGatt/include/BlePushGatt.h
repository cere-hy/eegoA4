#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

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

// True while the push peripheral owns the single radio; the HID page-turner
// central must refuse to start in that window (§7 connection exclusivity).
bool radioOwned();

// B4′: push one already-encoded 0x1A scene frame up the STATUS(..13) NOTIFY
// characteristic. No-op (returns false) when no phone link is up or the client
// has not subscribed to the CCCD — the local dictionary lookup never waits on
// this. The caller owns the frame buffer; notify is fire-and-forget.
bool sendSceneFrame(const uint8_t* frame, size_t len);
bool canNotifyScene();

}  // namespace blepushgatt
