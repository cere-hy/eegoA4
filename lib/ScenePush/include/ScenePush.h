#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// B4′ — 0x1A send glue (EEGO A4 rebuild).
//
// Turns a dictionary long-press (word + surrounding sentence + byte offset)
// into a burst of white-paper §2.1 scene frames and pushes them up the B3
// STATUS(..13) NOTIFY characteristic. This is the firmware half of B4: the
// "send capability goes on-device" single variable.
//
// Design constraints (v1.3 / B4 card):
//   * The dictionary lookup path NEVER waits on BLE. emit() is fire-and-forget:
//     it only runs when a phone is connected AND subscribed; otherwise it
//     returns 0 immediately. Local lookup (~16ms) and the 0x1A notify are two
//     non-blocking paths, not one serial chain.
//   * Emitter registration follows the reader activity lifetime — entering the
//     reader registers, exiting unregisters. The persistent B3 advertising is
//     untouched: unregister stops 0x1A emission, NOT the radio.
//   * Field layout comes solely from PushSceneCodec (B4a, white-paper §2.1).
//     No layout is duplicated here.
namespace scenepush {

// Reader-lifetime gate. Idempotent. Does NOT start/stop advertising.
void registerEmitter();
void unregisterEmitter();
bool isRegistered();

// Push one long-press scene. No-op (returns 0) when not registered, no peer,
// or invalid input. `word` is UTF-8 (already the selected token); `ctx` is the
// visible sentence/line text; `wordOff` is the byte offset of `word` within
// `ctx` (UTF-8, so Chinese = 3 B/char). Returns the number of frames sent.
//
// Flags are derived here: ctx truncation / sentence fallback are supplied by
// the caller when the sentence boundary was not found and a page-line fallback
// was used (white-paper §2.1 PSC_FLAG_*).
int emitScene(const char* word, size_t wordLen,
              const char* ctx, size_t ctxLen,
              uint16_t wordOff, uint8_t flags);

}  // namespace scenepush
