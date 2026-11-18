// B5 — 0x07 AI definition receive glue — see include/AiResultSink.h.
#include "AiResultSink.h"

#include <BlePushGatt.h>
#include <Logging.h>
#include <push_ai_result.h>
#include <push_ai_seg.h>   // plan B: 0x08 frame decode + caps/err
#include <ai_segasm.h>     // plan B: 0x08 reassembler

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
// Plan B: widened from PAR_DEF_SLOT(170) to AI_FULL_DEF_MAX so a reassembled
// 0x08 definition (up to 8192B) fits. 0x07 single-frame path still writes <=169B.
char g_lastDef[AI_FULL_DEF_MAX] = {};

// Plan B: 0x08 reassembly state. Static (not stack) so the 8.3KB buffer never
// touches the 8KB task stack. Touched only on the NimBLE host task in
// onCtrlSeg(); reset on disconnect via resetSegmentation() from the loop task
// (guarded by a flag exchange — see below).
ai_segasm_t g_segasm;
std::atomic<bool> g_segResetPending{false};

// 0x08 DONE result handoff (host task → loop task), independent of the 0x07 ring
// because a reassembled def (up to 8192B) cannot fit a 241B ring slot.
// seqlock: odd = host task mid-write, even = stable. latestFrame() reads
// g_lastDef only when the seq is stable and unchanged across the read.
std::atomic<uint32_t> g_segSeq{0};
std::atomic<uint32_t> g_segReadyGen{0};  // 0 = no 0x08 result yet; else its generation
uint32_t g_segReadyLen = 0;              // guarded by g_segSeq

// Error state packed into one atomic u64 so host-task writes / loop-task reads
// need no lock: low 32 = monotonic generation (0 = none), high 32 = reason code.
std::atomic<uint64_t> g_segErrorPacked{0};
uint32_t g_segError_gen = 0;  // monotonic, host task only
void publishSegError(SegError code) {
  const uint64_t gen = (uint64_t)(++g_segError_gen);
  g_segErrorPacked.store(((uint64_t)((uint32_t)code) << 32) | gen,
                         std::memory_order_release);
}

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

// Plan B: called on the NimBLE host task for each 0x08 segmented frame (241B,
// whole frame incl 4B CTRL header). Decode → feed reassembler → on DONE write
// the full text into the SAME single-frame answer slot (g_lastDef/g_headword)
// so latestFrame() needs no change. REJECT/OVERFLOW/DECODE publish a SegError
// for the loop task to surface locally (REQ2: APK does not listen for a reply).
void onCtrlSeg(const uint8_t* frame, size_t len) {
  if (frame == nullptr || len != PAS_FRAME_LEN) {
    publishSegError(SegError::DECODE);
    logf("0x08 REJECT decode: bad len=%u", (unsigned)len);
    return;
  }
  pas_seg_t seg;
  if (!pas_seg_decode(frame, len, &seg)) {
    publishSegError(SegError::DECODE);
    logf("0x08 REJECT decode: bad header/fields");
    return;
  }
  const char* text = nullptr;
  size_t textLen = 0;
  const ai_segasm_status_t st = ai_segasm_feed(&g_segasm, &seg, &text, &textLen);
  logf("0x08 seg word='%s' idx=%u/%u dlen=%u -> %s (joined=%u)",
       seg.word, (unsigned)seg.seg_idx, (unsigned)seg.seg_total, (unsigned)seg.dlen,
       st == AI_SEGASM_PENDING ? "PENDING" :
       st == AI_SEGASM_DONE ? "DONE" :
       st == AI_SEGASM_REJECT ? "REJECT" : "OVERFLOW",
       (unsigned)g_segasm.len);
  if (st == AI_SEGASM_DONE) {
    if (text != nullptr && textLen < AI_FULL_DEF_MAX) {
      const uint32_t s = g_segSeq.fetch_add(1u, std::memory_order_acq_rel) + 1u;  // odd = writing
      (void)s;
      std::memcpy(g_lastDef, text, textLen);
      g_lastDef[textLen] = 0;
      g_segReadyLen = (uint32_t)textLen;
      std::snprintf(g_headword, sizeof(g_headword), "%s", g_segasm.word);
      // Distinct generation space: 0x08 results get g_segReadyGen = a fresh
      // value from the same monotonic source as g_received, so pollAiResult's
      // `generation > aiWaitBaseGen_` sees it. Publish gen LAST (release) so
      // the loop task only observes a fully-written buffer.
      const uint32_t gen = g_received.fetch_add(1u, std::memory_order_relaxed) + 1u;
      g_segSeq.fetch_add(1u, std::memory_order_release);  // even = stable
      g_segReadyGen.store(gen, std::memory_order_release);
      logf("0x08 DONE gen=%u len=%u -> full-def slot", gen, (unsigned)textLen);
    }
  } else if (st == AI_SEGASM_REJECT) {
    publishSegError(SegError::REJECT);
  } else if (st == AI_SEGASM_OVERFLOW) {
    publishSegError(SegError::OVERFLOW);
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
  blepushgatt::setAiSegHandler(&onCtrlSeg);  // plan B: 0x08 segmented path
  ai_segasm_reset(&g_segasm);
  logf("init: 0x07+0x08 receive hooks armed, ring=%s segmax=%u",
       g_ring ? "ok" : "NULL", (unsigned)AI_SEGASM_MAX);
}

bool pump() {
  if (!g_inited.load(std::memory_order_acquire) || g_ring == nullptr) return false;

  // Plan B: reset reassembly on link loss (true→false edge) or explicit request.
  // Done here on the loop task, never concurrently with host-task feed().
  static bool lastConnected = false;
  const bool connected = blepushgatt::g_bleConnected.load(std::memory_order_acquire);
  if (lastConnected && !connected) g_segResetPending.store(true, std::memory_order_release);
  lastConnected = connected;
  if (g_segResetPending.exchange(false, std::memory_order_acq_rel)) {
    ai_segasm_reset(&g_segasm);
    g_segReadyGen.store(0, std::memory_order_release);
    g_segErrorPacked.store(0, std::memory_order_release);
  }

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
  const uint32_t segGen = g_segReadyGen.load(std::memory_order_acquire);
  const bool useSeg = segGen > g_screen.last_gen;  // newer 0x08 full def wins

  if (generation != nullptr) *generation = useSeg ? segGen : g_screen.last_gen;
  if (word != nullptr && wordCap > 0) {
    std::snprintf(word, wordCap, "%s", g_headword);
  }
  if (def != nullptr && defCap > 0 && useSeg) {
    // Standard seqlock read: wait for a stable (even) seq, copy, re-check.
    uint32_t s0, s1;
    do {
      do { s0 = g_segSeq.load(std::memory_order_acquire); } while (s0 & 1u);
      const uint32_t n = g_segReadyLen < (defCap - 1u)
                             ? g_segReadyLen : (uint32_t)(defCap - 1u);
      std::memcpy(def, g_lastDef, n);
      def[n] = '\0';
      s1 = g_segSeq.load(std::memory_order_acquire);
    } while (s0 != s1);
  } else if (def != nullptr && defCap > 0) {
    const size_t n = std::strlen(g_lastDef) < defCap - 1u ? std::strlen(g_lastDef) : defCap - 1u;
    std::memcpy(def, g_lastDef, n);
    def[n] = '\0';
  }
  return (useSeg ? segGen : g_screen.last_gen) != 0;
}

bool takeSegError(SegErrorInfo* out) {
  // Single loop-task consumer. Atomic u64 snapshot: low 32 = gen (0 = none),
  // high 32 = reason code. Monotonic gen prevents double-reporting.
  static uint32_t lastSeen = 0;
  const uint64_t packed = g_segErrorPacked.load(std::memory_order_acquire);
  const uint32_t gen = (uint32_t)(packed & 0xFFFFFFFFu);
  if (gen == 0 || gen == lastSeen) return false;
  if (out != nullptr) {
    out->code = (SegError)(uint32_t)(packed >> 32);
    out->generation = gen;
  }
  lastSeen = gen;
  return true;
}

void resetSegmentation() {
  // Called from the loop task (e.g. on disconnect). Defer the actual memset to
  // pump() so it can never race the host task's ai_segasm_feed().
  g_segResetPending.store(true, std::memory_order_release);
}

Stats stats() {
  Stats s = g_stats;
  s.received = g_received.load(std::memory_order_relaxed);
  return s;
}

}  // namespace aireceivesink
