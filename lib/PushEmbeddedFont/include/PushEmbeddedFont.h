#pragma once
// S3c — on-device glue that turns the embedded builtin.ttf + ui-hanzi.bin into
// a four-tier per-codepoint fallback for the PUSH reader font:
//   ① builtin.ttf vector (FreeType, resident PSRAM)  — served by TtfEpdFont
//   ② ui-hanzi.bin PIF1 Han bitmap                   — this module (secondary)
//   ③ SD full font (LXGW) for out-of-GB2312 glyphs   — this module (secondary)
//   ④ placeholder box + ASCII miss log              — this module
// All injected glyphs land in the SAME TtfEpdFont face arena, so the existing
// vectorBitmapHandler draw path serves them unchanged and layout is per-glyph.
//
// S3c+ — the embedded font face is ALSO the "Built-in (Fast)" reader face:
// the FAST reader id maps to the SAME resident TtfEpdFont as PUSH, so PUSH and
// FAST share ONE handler context (no duplicate registration, no cross-over).
// The secondary/advance handlers are now ctx-driven (`FallbackContext`) so the
// tier-③ pointer is per-face, never a process-global that one face can clobber.
//
// The same fallback chain is also offered to GLOBAL bitmap fonts (built-in
// Noto Sans, SD .cpfont) via a plain glyph-miss callback installed in their
// EpdFontData — see `installGlobalFallback()`.
#include <cstddef>
#include <cstdint>

class TtfEpdFont;
#include <EpdFontData.h>  // EpdFontData is an anonymous-struct typedef; include it

namespace pushembedfont {

// Embed symbols (board_build.embed_files in platformio.ini). PlatformIO's
// objcopy uses the FULL relative path in the symbol name (dots/dashes -> '_').
extern const uint8_t builtin_ttf_start[] asm("_binary_assets_fonts_builtin_ttf_start");
extern const uint8_t builtin_ttf_end[] asm("_binary_assets_fonts_builtin_ttf_end");
extern const uint8_t uihanzi_bin_start[] asm("_binary_assets_fonts_ui_hanzi_bin_start");
extern const uint8_t uihanzi_bin_end[] asm("_binary_assets_fonts_ui_hanzi_bin_end");

inline size_t builtinTtfLen() { return size_t(builtin_ttf_end - builtin_ttf_start); }
inline const uint8_t* builtinTtfData() { return builtin_ttf_start; }

// Diagnostics counters (reset on each install).
struct Stats {
  uint32_t pif1Hits;
  uint32_t sdHits;
  uint32_t misses;
};

// Per-face fallback state. Lives as long as the face it is installed into; the
// secondary/advance handlers receive it through their `void* ctx` argument.
// S3c+ engineering rule: tier-③ is per-face so PUSH/FAST and any other face
// cannot overwrite each other's SD fallback pointer.
// S3cPF5 — "confirmed absent" codepoint set. A codepoint that reached tier ④
// (no source has it) is recorded here; later occurrences short-circuit to the
// placeholder WITHOUT re-running all three rasterising tiers or re-logging.
// This is the root-cause fix for built-in mode's super-linear cost: before this
// set, an emoji/special glyph re-ran builtin→PIF1→SD rasterisation and emitted
// a 115200-baud log line on EVERY occurrence and on EVERY page turn.
// Bounded, RAM-resident, per-face (lives in FallbackContext, never on a task
// stack). Full set simply stops caching new entries — no correctness loss.
static constexpr size_t kMissingCacheCap = 256;
struct FallbackContext {
  TtfEpdFont* sdFont = nullptr;      // tier ③ (SD font). nullptr disables tier ③.
  TtfEpdFont* builtinFace = nullptr; // tier ① (builtin.ttf vector) for SD-vector faces;
                                     // nullptr for PUSH/FAST (themselves primary).
  Stats stats{};                     // diagnostics (per-face)
  uint32_t cjkSeen = 0;
  uint32_t cjkMissing = 0;
  void* unavailable = nullptr;       // std::atomic<bool>* when provided

  uint32_t missingCps[kMissingCacheCap]{};  // confirmed-absent codepoints
  uint16_t missingCount = 0;

  // True if `cp` was previously confirmed absent by tier ④.
  bool isMissing(uint32_t cp) const {
    for (uint16_t i = 0; i < missingCount; ++i)
      if (missingCps[i] == cp) return true;
    return false;
  }
  // Record `cp` as confirmed absent. Returns true only for a NEW entry (used
  // to emit the miss log exactly once per codepoint). Full cache => false.
  bool markMissing(uint32_t cp) {
    if (isMissing(cp)) return false;
    if (missingCount >= kMissingCacheCap) return false;
    missingCps[missingCount++] = cp;
    return true;
  }
  // Clear on face rebuild / SD-tier change: a newly available SD font can
  // supply glyphs that were previously absent.
  void resetMissing() { missingCount = 0; }
};

// Install the SAME secondary/advance handler chain into an SD vector face
// (DejaVuSans / LXGW / any .ttf). `builtinFace` supplies tier ① (builtin.ttf
// vector glyphs), PIF1 supplies tier ②, `sdFont` (tier ③) is intentionally
// nullptr — the face IS the user's chosen SD font, so self-referencing is a
// no-op and recursing is forbidden. `ctx` must outlive `font`. Idempotent.
void installSdVectorFallback(TtfEpdFont& font, FallbackContext& ctx, TtfEpdFont& builtinFace);

// Set the SD fallback font (tier ③) for a context. `font` must outlive every
// face whose handler carries this ctx. Pass nullptr to disable tier ③ (used on
// teardown and when the user deleted the SD font file).
void setSdFallback(FallbackContext& ctx, TtfEpdFont* font);

// Install the secondary/advance handlers into an embedded TtfEpdFont (call
// after load()). `ctx` must outlive `pushFont`. Idempotent.
void installInto(TtfEpdFont& pushFont, FallbackContext& ctx);

// S3c+ global bitmap-font字级回退: wire a global (built-in Noto Sans) font's
// EpdFontData so a missing codepoint is served per-glyph from tiers ①-③ of the
// embedded PUSH/FAST face. `data` must be a MUTABLE copy the caller owns. The
// handlers use a stable holder updated by setEmbeddedFace(), so point-size
// rebuilds of the embedded face need no re-wiring.
void installGlobalFallback(EpdFontData& data);
// Point the global fallback holder at the current embedded face (call after
// ensurePushFont builds/rebuilds it; pass nullptr on teardown).
void setEmbeddedFace(TtfEpdFont* face);

// True once when tier ④ had to placeholder a large share of CJK (ASCII notice).
bool takePushFontUnavailable();

Stats stats();

}  // namespace pushembedfont
