// B5 — 0x07 AI definition receive glue — see include/AiResultSink.h.
#include "AiResultSink.h"

#include <BlePushGatt.h>
#include <Logging.h>
#include <push_ai_result.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace aireceivesink {

namespace {

par_dslot_t* g_ring = nullptr;                 // double-slot SPSC ring
par_screen_t g_screen;                          // on-screen text + generation
std::atomic<bool> g_inited{false};
Stats g_stats{};
std::atomic<uint32_t> g_received{0};

// Latest accepted frame headword + generation, guarded by main-loop-only
// access in pump()/latest() (both run on the loop task; no cross-thread read).
char g_headword[PAR_WORD_SLOT] = {};
// B11: latest single-frame def (mirrors g_headword) — kept separate from the
// accumulating g_screen.text so the inline card can show exactly one answer.
char g_lastDef[PAR_DEF_SLOT] = {};

void logf(const char* fmt, ...) {
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  logPrintf("INF", "BLE", "AIR %s", line);
}

// Called on the NimBLE host task. Must be fast: publish raw frame only.
void onCtrlResult(const uint8_t* frame, size_t len) {
  g_received.fetch_add(1u, std::memory_order_relaxed);
  if (g_ring != nullptr && frame != nullptr && len > 0) {
    par_dslot_publish(g_ring, frame, len);
  }
}

// Printable-segment cross-check (whitepaper §2.2 "扫可打印段"兜底/交叉校验):
// longest printable run ≈ definition, first ASCII run ≈ word. We only compare
// lengths as a cheap sanity signal; authoritative decode always wins, a
// mismatch is logged (hex留证), never silently trusted.
bool printable_crosscheck_ok(const uint8_t* frame, size_t len, size_t off,
                             const par_result_t& r) {
  const uint8_t* p = frame + off;
  const size_t avail = len - off;
  if (avail < PAR_FIELD_LEN) return false;
  size_t best = 0, run = 0;
  for (size_t i = 0; i < PAR_FIELD_LEN; ++i) {
    const uint8_t c = p[i];
    if (c >= 0x20u && c < 0x7Fu) { if (++run > best) best = run; }
    else run = 0;
  }
  // In the frozen layout the longest ASCII run should not wildly exceed the
  // declared def length + word length; a gross mismatch hints a shifted frame.
  const size_t declared = r.word_len + r.def_len;
  if (declared == 0) return false;
  return best <= declared + 8u;
}

}  // namespace

void init() {
  if (g_inited.exchange(true, std::memory_order_acq_rel)) return;
  std::memset(&g_screen, 0, sizeof(g_screen));
  par_screen_init(&g_screen);
  g_ring = par_dslot_create();
  blepushgatt::setAiResultHandler(&onCtrlResult);
  logf("init: 0x07 receive hook armed, ring=%s", g_ring ? "ok" : "NULL");
}

bool pump() {
  if (!g_inited.load(std::memory_order_acquire) || g_ring == nullptr) return false;

  uint8_t frame[PAR_FRAME_LEN];
  size_t len = 0;
  uint32_t gen = 0;
  const uint32_t got = par_dslot_take_latest(g_ring, frame, &len, &gen);
  if (got == 0) return false;                    // no frame pending
  g_stats.received = g_received.load(std::memory_order_relaxed);

  if (gen == g_screen.last_gen) {                // duplicate of last shown
    g_stats.duplicate++;
    return false;
  }

  const size_t off = par_frame_offset(frame, len);
  par_result_t r;
  if (off == (size_t)-1 || !par_decode(frame, len, off, &r)) {
    g_stats.rejected++;
    char hex[PAR_FRAME_LEN * 2u + 16u];
    par_hexdump(frame, len, hex, sizeof(hex));
    logf("REJECT gen=%u len=%u\n%s", gen, (unsigned)len, hex);  // hex留证
    return false;
  }

  if (!printable_crosscheck_ok(frame, len, off, r)) {
    g_stats.rejected++;
    // Cross-check mismatch: authoritative decode still stands, but leave a
    // hex trace + warning per whitepaper §2.2 (never silent accept).
    char hex[PAR_FRAME_LEN * 2u + 16u];
    par_hexdump(frame, len, hex, sizeof(hex));
    logf("WARN crosscheck gen=%u word=%s def_len=%u\n%s",
         gen, r.word, (unsigned)r.def_len, hex);
  }

  // Judgement-line trio (whitepaper B5: 判读行三连 + 241B hex per frame)
  {
    char hex[PAR_FRAME_LEN * 2u + 16u];
    par_hexdump(frame, len, hex, sizeof(hex));
    logf("0x07 gen=%u off=%u wlen=%u dlen=%u word=%s",
         gen, (unsigned)off, (unsigned)r.word_len, (unsigned)r.def_len, r.word);
    logf("0x07 def=%s", r.def);
    logf("0x07 hex241\n%s", hex);
  }

  g_stats.decoded++;
  const bool committed = par_screen_apply(&g_screen, gen, &r) != 0;
  if (committed) {
    g_stats.reflowed++;                          // append/reflow ok → UI jumps last page
    std::snprintf(g_headword, sizeof(g_headword), "%s", r.word);
    std::snprintf(g_lastDef, sizeof(g_lastDef), "%s", r.def);  // B11 single-frame mirror
  } else {
    g_stats.rollbacks++;                          // reflow failed → kept last good text
  }
  return committed;
}

bool latest(char* word, size_t wordCap, char* text, size_t textCap,
            uint32_t* generation) {
  if (generation != nullptr) *generation = g_screen.last_gen;
  if (word != nullptr && wordCap > 0) {
    std::snprintf(word, wordCap, "%s", g_headword);
  }
  if (text != nullptr && textCap > 0) {
    const size_t n = g_screen.text_len < textCap - 1u ? g_screen.text_len : textCap - 1u;
    std::memcpy(text, g_screen.text, n);
    text[n] = '\0';
  }
  return g_screen.last_gen != 0;
}

bool latestFrame(char* word, size_t wordCap, char* def, size_t defCap,
                 uint32_t* generation) {
  if (generation != nullptr) *generation = g_screen.last_gen;
  if (word != nullptr && wordCap > 0) {
    std::snprintf(word, wordCap, "%s", g_headword);
  }
  if (def != nullptr && defCap > 0) {
    const size_t n = std::strlen(g_lastDef) < defCap - 1u ? std::strlen(g_lastDef) : defCap - 1u;
    std::memcpy(def, g_lastDef, n);
    def[n] = '\0';
  }
  return g_screen.last_gen != 0;
}

Stats stats() {
  Stats s = g_stats;
  s.received = g_received.load(std::memory_order_relaxed);
  return s;
}

}  // namespace aireceivesink
