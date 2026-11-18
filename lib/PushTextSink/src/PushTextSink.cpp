// B6.5 S2 — text-push GATT glue. See include/PushTextSink.h.
#include "PushTextSink.h"

#if defined(FREEINK_DEVICE_EEGO_A4) && FREEINK_DEVICE_EEGO_A4

#include <Arduino.h>
#include <BlePushGatt.h>
#include <Logging.h>
#include <push_sink_core.h>

#include <cstdarg>
#include <cstdio>

namespace pushtextsink {
namespace {

// The one session core. Static storage — no heap. Touched ONLY on the NimBLE
// host task from the CTRL/DATA callbacks below (serialized on that task).
push_sink_core_t g_core;
bool g_ready = false;
uint16_t g_token = 0;

// PROGRESS throttle: at most one status notification per 400 ms.
uint32_t g_lastProgressMs = 0;
constexpr uint32_t kProgressGapMs = 400u;

void logf(const char* fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logPrintf("INF", "BLE", "PUSH %s", line);
}

// Drive the core to CONNECTED from whatever state it is in, so a BEGIN (or a
// RESUME_QUERY after reconnect) is always accepted and answered:
//   IDLE      -> ARM -> ADV_UP -> CONNECT  (first session)
//   ARMED     -> ADV_UP -> CONNECT
//   WINDOW    -> CONNECT
//   CONNECTED -> no-op on each step
//   RECEIVING / PROCESSING / COOLDOWN -> DISCONNECT -> WINDOW -> CONNECT
// The DISCONNECT step deliberately collapses a session left half-received by a
// link drop back to WINDOW, so the reconnect gets a clean handshake instead of
// an illegal-transition swallow. None of these steps emit a STATUS frame; the
// only STATUS after this sequence is the one produced by the CTRL event that
// follows.
void driveConnected(uint32_t now_ms) {
  if (g_core.sess.state == PUSH_STATE_IDLE) {
    // Always-on window: window_ms=0 -> never times out; no cooldown in S2.
    push_sink_core_arm(&g_core, g_token, 0u, 0u, now_ms);
  }
  push_sink_core_disconnect(&g_core);  // connected-ish -> WINDOW
  push_sink_core_adv_up(&g_core);      // ARMED -> WINDOW
  push_sink_core_connect(&g_core);     // WINDOW -> CONNECTED
}

// Take the core's latest STATUS frame (consume it) and emit it up STATUS(..13)
// through the independent-mbuf path. READY/RESUME/DONE/ERR go out immediately;
// PROGRESS is throttled to one per 400 ms. Clearing status_len after the read
// gives take semantics so a later no-status event cannot re-send a stale frame.
void emitStatus(uint32_t now_ms) {
  uint8_t frame[PUSH_CORE_STATUS_CAP];
  size_t n = push_sink_core_status(&g_core, frame, sizeof(frame));
  g_core.status_len = 0;  // take: consume whatever the core just produced
  if (n == 0) return;

  const uint8_t op = frame[0];
  if (op == PUSH_ST_PROGRESS) {
    if (g_lastProgressMs != 0 && (uint32_t)(now_ms - g_lastProgressMs) < kProgressGapMs) {
      return;  // throttled: a later PROGRESS/DONE supersedes this one
    }
    g_lastProgressMs = now_ms;
  } else {
    // READY/RESUME/DONE/ERR are never throttled. Reset the progress window so
    // the first DATA of the next session can report immediately.
    g_lastProgressMs = 0;
  }

  const bool ok = blepushgatt::sendStatusFrame(frame, n);
  logf("STATUS op=0x%02X len=%u notify=%s", (unsigned)op, (unsigned)n, ok ? "ok" : "FAIL");
}

// CTRL hook. op is the opcode byte at offset 2; frame is the whole CTRL frame.
// 0x07/0x08 are routed to their dedicated hooks by B3 and never reach here.
void onCtrl(uint8_t op, const uint8_t* frame, size_t len) {
  if (!g_ready || frame == nullptr) return;
  const uint32_t now = millis();

  if (op == PUSH_OP_BEGIN || op == PUSH_OP_RESUME_QUERY) {
    driveConnected(now);
    logf("CTRL op=0x%02X recv (driven to CONNECTED, state=%d)", (unsigned)op,
         (int)g_core.sess.state);
  } else {
    logf("CTRL op=0x%02X recv (state=%d)", (unsigned)op, (int)g_core.sess.state);
  }

  push_sink_core_on_ctrl(&g_core, frame, len);
  emitStatus(now);

  // S2 does not consume commit_ready/open_ready (S3 persistence / S4 auto-open).
  // They stay latched in the core for the later batches.
}

// DATA hook. Frame is raw seq(u16 LE)+len(u8)+payload, no opcode.
void onData(const uint8_t* frame, size_t len) {
  if (!g_ready || frame == nullptr) return;
  const uint32_t now = millis();
  push_sink_core_on_data(&g_core, frame, len);
  emitStatus(now);
}

}  // namespace

void init() {
  if (g_ready) return;
  push_sink_core_init(&g_core);
  g_token = blepushgatt::tokenFromStaMac();
  // Pre-wire the state machine to WINDOW so the very first BEGIN only needs a
  // CONNECT step; driveConnected() also covers a cold IDLE as a fallback.
  push_sink_core_arm(&g_core, g_token, 0u, 0u, millis());
  push_sink_core_adv_up(&g_core);
  g_lastProgressMs = 0;
  g_ready = true;

  blepushgatt::setCtrlHandler(&onCtrl);
  blepushgatt::setDataHandler(&onData);
  logf("sink init: token=0x%04X state=%d (ctrl+data hooks registered)",
       (unsigned)g_token, (int)g_core.sess.state);
}

}  // namespace pushtextsink

#else  // non-EEGO-A4: inert stubs

namespace pushtextsink {
void init() {}
}  // namespace pushtextsink

#endif
