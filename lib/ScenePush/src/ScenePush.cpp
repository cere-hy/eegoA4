// B4′ — 0x1A send glue — see include/ScenePush.h.
#include "ScenePush.h"

#include <BlePushGatt.h>
#include <push_scene.h>

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
  // Non-blocking gate: no registration, no subscribed peer, or bad input → 0.
  // The local dictionary lookup never waits here.
  if (!s_registered.load(std::memory_order_acquire)) return 0;
  if (!blepushgatt::canNotifyScene()) return 0;
  if (word == nullptr || wordLen == 0 || ctx == nullptr) return 0;

  const uint8_t* w = reinterpret_cast<const uint8_t*>(word);
  const uint8_t* c = reinterpret_cast<const uint8_t*>(ctx);
  if (ctxLen > PSC_CTX_MAX) ctxLen = PSC_CTX_MAX;

  // UTF-8-safe clamp so a long word never splits a multibyte char.
  uint8_t wordBuf[PSC_WORD_MAX];
  const size_t wlen = psc_word_clamp(w, wordLen, wordBuf);
  if (wlen == 0) return 0;

  psc_plan_t plan;
  const int n = psc_plan(c, ctxLen, wlen, &plan);
  if (n <= 0) return 0;

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
  return sent;
}

}  // namespace scenepush
