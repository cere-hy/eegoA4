#pragma once

#include <cstddef>
#include <cstdint>

// B6.5 S2 — text-push GATT glue (EEGO A4 rebuild).
//
// Bridges the S1 pure core (PushTextCore, linked via LDF because this .cpp
// includes push_sink_core.h) onto the existing BlePushGatt handler hooks. It
// does NOT change B3, PushTextCore, the protocol text, or the 0x07/0x08
// judgement. It touches NO filesystem this batch: DATA only advances the
// in-memory seq/CRC ledger and emits PROGRESS; END emits DONE/ERR from the CRC
// result. .part persistence / cleaning / document generation / auto-open are
// all S3+ work.
//
// Core acceptance point: a 0x01 BEGIN must yield 0x10 READY — the only signal
// the APK waits for, i.e. the "no more STATUS timeout" milestone. Before
// BEGIN/RESUME_QUERY the glue drives the state machine to CONNECTED from
// whatever state it is in (including a RECEIVING left behind by a drop), so a
// reconnect always gets a fresh handshake. DATA frames are handed to the core
// untouched — the state machine is already RECEIVING by then.
//
// STATUS frames produced by the core are emitted through BlePushGatt's
// independent-mbuf sendStatusFrame() (notify(value,len,conn), never the
// no-arg notify()). READY/RESUME/DONE/ERR are sent immediately; PROGRESS is
// throttled to at most one per 400 ms to keep the notify queue bounded.
//
// Threading: the single push_sink_core_t is touched ONLY on the NimBLE host
// task, inside the CTRL/DATA callbacks (same task, serialized). No main-loop
// hook, no lock, no shared mutable state with the UI.
//
// UI: handlers are short (no display work, no popup). A BEGIN arriving while
// the user reads another book must not block or interrupt — it never does.
// An AI query during a push rides the separate 0x07/0x08 hooks and is untouched
// here (full mutual-exclusion prompt is S4).
namespace pushtextsink {

// Idempotent. Initialise the single static core context (token = STA MAC low
// two bytes, always-on window) and register BlePushGatt's CTRL/DATA handlers.
// Call once in setup(), after blepushgatt::begin(). Non-A4 builds are inert.
void init();

}  // namespace pushtextsink
