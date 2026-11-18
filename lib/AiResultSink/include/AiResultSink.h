#pragma once

#include <cstddef>
#include <cstdint>

// B5 — 0x07 AI 释义单帧接收固件桥接（EEGO A4 rebuild）。
//
// On the NimBLE host task a phone's 0x07 CTRL write lands here via the
// dedicated BlePushGatt::AiResultHandler hook. That callback only publishes
// the raw 241B frame into the PushAiResultCodec double-slot ring (fast, no
// allocation, no display work). The main loop calls pump() which takes the
// latest complete frame, decodes it against the whitepaper §2.2 field table,
// runs the on-screen decision (append/reflow→jump-last-page; reflow fail→
// rollback) and keeps a hex trace for audit.
//
// Design constraints (B5 card / v1.6):
//   * Producer (BLE task) and consumer (main loop) never touch the same slot:
//     PushAiResultCodec's per-slot seqlock guarantees a non-torn snapshot.
//   * Decoding is authoritative-offset based; printable-segment scan is a
//     cross-check only (mismatch → hex log warning, never silent accept).
//   * Reflow failure rolls back to the last good text (no half definition).
//   * 0x07 receive is independent of the reader page lifetime (like B3
//     persistent advertising); pump() is a cheap no-op when no frame pending.
namespace aireceivesink {

// Plan B: reassembled full definition capacity (0x08 path). 8192B per the
// whitepaper; a reassembled definition larger than this OVERFLOWs.
constexpr size_t AI_FULL_DEF_MAX = 8192u;

// Plan B failure reason for a 0x08 segmented definition. APP 不监听释义回执
// (REQ2 定案)：错误必须走设备本地 UI，由 pollAiResult 读 lastError 立即提示，
// 不静默等满 15s 超时。
enum class SegError : uint8_t {
  NONE = 0,
  REJECT,    // 乱序 / 词变 / total 变
  OVERFLOW,  // 超 8192B
  DECODE,    // 0x08 帧解码失败（坏帧）
};

struct SegErrorInfo {
  SegError code = SegError::NONE;
  uint32_t generation = 0;  // 触发错误的帧代际（去重用）
};

// Idempotent. Creates the double-slot ring and registers the CTRL 0x07 hook.
// Plan B: also registers the CTRL 0x08 segmented hook.
// Safe to call once from setup after blepushgatt::begin().
void init();

// Main-loop consumer: take latest frame, decode, decide on-screen. Returns
// true when a new generation was consumed (caller may force a redraw); false
// when nothing pending or the frame was rejected/duplicate.
bool pump();

// Latest accepted definition for the dictionary UI (s11 behavior oracle).
// Copies into caller buffers; returns false if none yet. `generation` is the
// monotonic 0x07 frame number. `text` is the accumulated on-screen text after
// append/reflow; `word` is the headword of the latest frame.
bool latest(char* word, size_t wordCap, char* text, size_t textCap,
            uint32_t* generation);

// B11 consumption face: the latest *single-frame* definition (not the
// accumulated on-screen text returned by latest()). The inline definition card
// replaces its body with exactly one 0x07 answer, so it must not receive the
// append/reflow history. Copies into caller buffers; returns false if none yet.
bool latestFrame(char* word, size_t wordCap, char* def, size_t defCap,
                 uint32_t* generation);

// Plan B: consume and clear the latest 0x08 segmented error, if any. Returns
// true and fills `out` when a new error arrived since the last call; the reader
// must call this BEFORE latestFrame() so a REJECT/OVERFLOW surfaces an explicit
// prompt instead of silently waiting out the 15s timeout. Generation guards
// against double-reporting the same error.
bool takeSegError(SegErrorInfo* out);

// Plan B: drop any in-flight reassembly (call on disconnect / CCCD off so a
// half-collected definition never splices into the next query).
void resetSegmentation();

// Diagnostics counters (audit).
struct Stats {
  uint32_t received;   // 0x07 writes published
  uint32_t decoded;    // authoritative decode ok
  uint32_t rejected;   // bad frame / cross-check mismatch
  uint32_t reflowed;   // append+reflow committed (jump last page)
  uint32_t rollbacks;  // reflow failed → kept last good text
  uint32_t duplicate;  // old generation dropped
};
Stats stats();

}  // namespace aireceivesink
