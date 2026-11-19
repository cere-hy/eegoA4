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
// Only installed on the PUSH dedicated font. Other books/fonts are untouched.
#include <cstddef>
#include <cstdint>

class TtfEpdFont;

namespace pushembedfont {

// Embed symbols (board_build.embed_files in platformio.ini). PlatformIO's
// objcopy uses the FULL relative path in the symbol name (dots/dashes -> '_').
extern const uint8_t builtin_ttf_start[] asm("_binary_assets_fonts_builtin_ttf_start");
extern const uint8_t builtin_ttf_end[] asm("_binary_assets_fonts_builtin_ttf_end");
extern const uint8_t uihanzi_bin_start[] asm("_binary_assets_fonts_ui_hanzi_bin_start");
extern const uint8_t uihanzi_bin_end[] asm("_binary_assets_fonts_ui_hanzi_bin_end");

inline size_t builtinTtfLen() { return size_t(builtin_ttf_end - builtin_ttf_start); }
inline const uint8_t* builtinTtfData() { return builtin_ttf_start; }

// Set the SD fallback font used for tier ③. `font` must outlive the PUSH font.
// Pass nullptr to disable tier ③.
void setSdFallback(TtfEpdFont* font);

// Install the secondary handler into a PUSH TtfEpdFont (call after load()).
void installInto(TtfEpdFont& pushFont);

// True once when tier ④ had to placeholder a large share of CJK (ASCII notice).
bool takePushFontUnavailable();

// Diagnostics counters (reset on each install).
struct Stats {
  uint32_t pif1Hits;
  uint32_t sdHits;
  uint32_t misses;
};
Stats stats();

}  // namespace pushembedfont
