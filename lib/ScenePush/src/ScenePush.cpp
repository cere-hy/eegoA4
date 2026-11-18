// B4′ — 0x1A send glue — see include/ScenePush.h.
#include "ScenePush.h"

#include <BlePushGatt.h>
#include <push_scene.h>
#include <Logging.h>

#include <Arduino.h>  // millis() for the non-blocking inter-frame gap

namespace scenepush {

namespace {
std::atomic<bool> s_registered{false};

// next-diag build fingerprint. Every 0x1A emit prints this, so a flashed unit
// is identifiable from the serial log at a glance. The previous diag-pace
// build had NO such marker, which is exactly why "pacer missing" could not be
// distinguished from "old firmware still flashed" in the field report.
#define DIAG_BUILD_TAG "DIAGPACE2"

// Compile-time diagnostic switch: clamp ctx to the seq0 frame capacity so
// EVERY query collapses to ONE frame. If word queries then succeed 100%, the
// multi-frame notify burst is proved to be the failure variable. Temporary —
// removed when the diagnostic build is retired (验完即弃).
#define DIAG_FORCE_SINGLE_FRAME 1

// diag-pace: a multi-frame 0x1A burst is staged here at emit time. Only seq0
// goes out inline; the rest are dribbled out by loop() with a >=40 ms wall-clock
// gap. This is the fix candidate for "first send fails, resend works" (two
// back-to-back setValue()+notify() calls with zero spacing).
//
// Why NOT a literal delay(40) inside emitScene's for-loop: emitScene runs on
// the main loop (called from the card's AI button handler inside the activity
// loop). A blocking delay(40) per frame would freeze UI refresh and input
// sampling for up to (N-1)*40 ms. Instead the gap is expressed as a deadline
// checked by loop() once per iteration: unexpired -> return immediately (the
// main loop keeps rendering and sampling input), expired -> send exactly one
// frame and re-arm. No task is ever blocked; the 40 ms is elapsed wall time,
// not busy-wait.
constexpr unsigned long kInterFrameGapMs = 40;
uint8_t s_burst[PSC_MAX_FRAMES][PSC_FRAME_MAX];  // staged frames (seq0..N-1)
uint8_t s_burstLen[PSC_MAX_FRAMES] = {};
uint8_t s_burstCount = 0;      // frames in the active burst, 0 = idle
uint8_t s_burstNext = 0;       // index of the next frame to emit (starts at 1)
unsigned long s_burstDue = 0;  // millis() deadline for the next frame
}  // namespace

void registerEmitter() {
  // Idempotent; entering another reader page keeps it registered.
  s_registered.store(true, std::memory_order_release);
}

void unregisterEmitter() {
  s_registered.store(false, std::memory_order_release);
}

bool isRegistered() { return s_registered.load(std::memory_order_acquire); }

int emitScene(const char* word, size_t wordLen,
              const char* ctx, size_t ctxLen,
              uint16_t wordOff, uint8_t flags) {
  // Design-spec log: opcode / payload lengths / frames / word / wordOffset /
  // send result. Gate failures are logged with result=no-op so the AI button
  // path is fully observable on the serial console.
  const auto gated = [&](const char* why) {
    LOG_INF("SCENE", "0x1A emit gated (%s): word='%s' wordLen=%u ctxLen=%u wordOff=%u frames=0 result=no-op",
            why, word ? word : "", (unsigned)wordLen, (unsigned)ctxLen, (unsigned)wordOff);
    return 0;
  };
  // Non-blocking gate: no registration, no subscribed peer, or bad input → 0.
  // The local dictionary lookup never waits here.
  if (!s_registered.load(std::memory_order_acquire)) return gated("not-registered");
  if (!blepushgatt::canNotifyScene()) return gated("no-subscriber");
  if (word == nullptr || wordLen == 0 || ctx == nullptr) return gated("bad-input");

  const uint8_t* w = reinterpret_cast<const uint8_t*>(word);
  const uint8_t* c = reinterpret_cast<const uint8_t*>(ctx);
  if (ctxLen > PSC_CTX_MAX) ctxLen = PSC_CTX_MAX;

#if DIAG_FORCE_SINGLE_FRAME
  // diag next: force EVERY scene into exactly one frame so the multi-frame
  // notify burst is taken out of the equation. Frame 0's ctx capacity is
  // PSC_FRAME_MAX - (11 + word_len) (see psc_plan). Clamp ctxLen to that and
  // set wordOff=0 + PSC_FLAG_EXCERPT so the anchor word is guaranteed to be
  // the first thing in ctx (a word deeper than the cap would otherwise be
  // lost). Snap to a UTF-8 boundary; if the single multibyte char at the
  // boundary would be dropped, keep the whole char by clamping to it.
  {
    const size_t wlenProbe = (wordLen < PSC_WORD_MAX) ? wordLen : PSC_WORD_MAX;
    size_t cap = PSC_FRAME_MAX - (11u + wlenProbe);
    if (ctxLen > cap) {
      // Walk back off any UTF-8 continuation byte at the cut point.
      while (cap > 0 && (c[cap] & 0xC0u) == 0x80u) --cap;
      if (cap == 0) cap = 1;  // never produce an empty ctx via over-clamp
      ctxLen = cap;
      wordOff = 0;
      flags = static_cast<uint8_t>(flags | PSC_FLAG_EXCERPT | PSC_FLAG_CTX_TRUNCATED);
      LOG_INF("SCENE", "0x1A DIAG force-single: ctxLen clamped to %u (cap), wordOff=0 flags=0x%02X",
              (unsigned)ctxLen, (unsigned)flags);
    }
  }
#endif

  // UTF-8-safe clamp so a long word never splits a multibyte char.
  uint8_t wordBuf[PSC_WORD_MAX];
  const size_t wlen = psc_word_clamp(w, wordLen, wordBuf);
  if (wlen == 0) return 0;

  psc_plan_t plan;
  const int n = psc_plan(c, ctxLen, wlen, &plan);
  if (n <= 0) {
    LOG_INF("SCENE", "0x1A emit gated (plan-fail): word='%s' wordLen=%u ctxLen=%u frames=0 result=no-op",
            word, (unsigned)wlen, (unsigned)ctxLen);
    return 0;
  }

  // Build fingerprint: presence of "build=DIAGPACE2" is the field proof the
  // next diag image is actually flashed. The prior diag-pace image had no such
  // marker, so its missing "seq0 sent; staged" log was ambiguous.
  LOG_INF("SCENE", "0x1A emit build=%s: word='%s' wordLen=%u payloadCtx=%u wordOff=%u flags=0x%02X planFrames=%d",
          DIAG_BUILD_TAG, word, (unsigned)wlen, (unsigned)ctxLen, (unsigned)wordOff, (unsigned)flags, n);

  // diag-pace: encode every frame into the staging buffer up front (pure CPU,
  // no BLE), then send seq0 inline. Remaining frames are emitted by loop().
  // seq starts at 0 on every burst: the peer treats seq==0 as the session
  // boundary (white-paper §2.1), so no cross-burst sequence state is kept.
  int encoded = 0;
  for (int seq = 0; seq < n; ++seq) {
    const size_t wrote = psc_frame_encode(
        static_cast<uint16_t>(seq), wordBuf, wlen, wordOff, flags,
        c, ctxLen, &plan, s_burst[seq]);
    if (wrote == 0) break;
    s_burstLen[seq] = static_cast<uint8_t>(wrote);
    ++encoded;
  }
  if (encoded == 0) {
    LOG_INF("SCENE", "0x1A done: word='%s' frames=0/%d result=encode-failed", word, n);
    return 0;
  }

  // Cancel any prior burst still in flight (a new scene is a new session).
  s_burstCount = 0;
  s_burstNext = 0;
  s_burstDue = 0;

  // seq0 inline: keeps the existing synchronous success/failure semantics that
  // beginAiQuery() relies on (frames<=0 => no answer expected, don't wait).
  if (!blepushgatt::sendSceneFrame(s_burst[0], s_burstLen[0])) {
    LOG_INF("SCENE", "0x1A done: word='%s' frames=0/%d result=seq0-failed", word, encoded);
    return 0;
  }

  if (encoded > 1) {
    s_burstCount = static_cast<uint8_t>(encoded);
    s_burstNext = 1;
    s_burstDue = millis() + kInterFrameGapMs;
    LOG_INF("SCENE", "0x1A seq0 sent %s @%lums; staged %d more frame(s), paced >=%lums",
            DIAG_BUILD_TAG, s_burstDue - kInterFrameGapMs, encoded - 1, kInterFrameGapMs);
  } else {
    LOG_INF("SCENE", "0x1A done: word='%s' frames=1/1 result=ok (single frame)", word);
  }
  return encoded;
}

void loop() {
  // Non-blocking pacer. One frame at most per main-loop iteration. Nothing to
  // do (or gap not elapsed) returns instantly — no delay(), no busy-wait.
  if (s_burstCount == 0 || s_burstNext >= s_burstCount) return;
  if (static_cast<long>(millis() - s_burstDue) < 0) return;  // deadline not reached

  const uint8_t seq = s_burstNext;
  if (!blepushgatt::sendSceneFrame(s_burst[seq], s_burstLen[seq])) {
    LOG_INF("SCENE", "0x1A burst aborted at seq=%u/%u (peer vanished) result=partial",
            (unsigned)seq, (unsigned)s_burstCount);
    s_burstCount = 0;  // drop the rest; peer gone anyway
    return;
  }
  ++s_burstNext;
  const unsigned long sentAt = millis();
  // Field evidence of the real gap: previous frame's deadline vs this send.
  // With force-single ON this path never runs (1 frame); flip the macro to 0
  // to exercise it and read gap_ms directly off the serial log.
  LOG_INF("SCENE", "0x1A burst %s seq=%u/%u sent@%lums gap_ms=%lu (diag-pace)",
          DIAG_BUILD_TAG, (unsigned)s_burstNext, (unsigned)s_burstCount,
          sentAt, sentAt - (s_burstDue - kInterFrameGapMs));
  s_burstDue = sentAt + kInterFrameGapMs;
  if (s_burstNext >= s_burstCount) {
    LOG_INF("SCENE", "0x1A burst done %s: frames=%u/%u result=ok (diag-pace)",
            DIAG_BUILD_TAG, (unsigned)s_burstNext, (unsigned)s_burstCount);
    s_burstCount = 0;
  }
}

}  // namespace scenepush
