// B4′ — 0x1A send glue — see include/ScenePush.h.
#include "ScenePush.h"

#include <BlePushGatt.h>
#include <push_scene.h>
#include <Logging.h>

namespace scenepush {

namespace {
std::atomic<bool> s_registered{false};
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

  LOG_INF("SCENE", "0x1A emit: opcode=0x1A word='%s' wordLen=%u payloadCtx=%u wordOff=%u flags=0x%02X planFrames=%d",
          word, (unsigned)wlen, (unsigned)ctxLen, (unsigned)wordOff, (unsigned)flags, n);

  uint8_t frame[PSC_FRAME_MAX];
  int sent = 0;
  // seq starts at 0 on every scene: the peer treats seq==0 as the session
  // boundary (white-paper §2.1), so no cross-burst sequence state is kept.
  for (int seq = 0; seq < n; ++seq) {
    const size_t wrote = psc_frame_encode(
        static_cast<uint16_t>(seq), wordBuf, wlen, wordOff, flags,
        c, ctxLen, &plan, frame);
    if (wrote == 0) break;
    if (!blepushgatt::sendSceneFrame(frame, wrote)) break;  // peer vanished mid-burst
    ++sent;
  }
  LOG_INF("SCENE", "0x1A done: word='%s' frames=%d/%d result=%s",
          word, sent, n, sent == n ? "ok" : "partial/failed");
  return sent;
}

}  // namespace scenepush
