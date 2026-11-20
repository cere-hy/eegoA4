#pragma once

// S3cPF6: a load-time, read-only codepoint->advanceWidth map built from an
// sfnt's cmap (format 4 + format 12) and hmtx tables. It lets layout obtain a
// glyph's raw advance without any per-codepoint SD seek into the font's tail
// (LXGW keeps hmtx ~15 MB into the file, far past the 1 MB prefix cache).
//
// The map stores RAW advanceWidth in font units (u16), keyed by BMP codepoint
// (u16). Converting to the 12.4 value used by TtfEpdFont is done by the caller
// via FtFont::unitsToAdvance26_6() so it is byte-identical to FreeType's own
// linearHoriAdvance (verified: 400/400 codepoints, diff=0, on DejaVu + builtin).
//
// Design constraints:
//   * Built only during font LOAD, never on the render/draw path.
//   * PSRAM backed; BMP-only keeps LXGW under 100 KB (44,810 mapped cps * 2B
//     advances + 2B keys = ~87.5 KiB data). Supplementary cps fall through to
//     the existing fallback chain (rare in practice).
//   * On-disk cache (/.crosspoint/fontmap/<hash>.fmap) with a key of
//     path+mtime+size; a corrupt/foreign file is ignored and rebuilt.
//   * AdvanceTable.h and FallbackChain.h are untouched — a FontMap hit simply
//     feeds the same advanceTableInsert() path.

#include <cstddef>
#include <cstdint>

namespace epd {

using FontReadFn = unsigned long (*)(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);

struct FontMapStats {
  uint32_t codepoints = 0;   // BMP codepoints recorded
  uint32_t skippedSupp = 0;  // supplementary codepoints skipped
  bool fromCache = false;    // loaded from .fmap instead of scanning
  bool buildOk = false;
};

class FontMap {
 public:
  FontMap() = default;
  ~FontMap();
  FontMap(const FontMap&) = delete;
  FontMap& operator=(const FontMap&) = delete;
  FontMap(FontMap&& other) noexcept;
  FontMap& operator=(FontMap&& other) noexcept;

  // Build in RAM from a font byte-stream reader. `read` must serve arbitrary
  // absolute-offset ranges (the 1 MB prefix reader qualifies). Returns false
  // on an unparseable sfnt / missing cmap / missing hmtx; caller then degrades
  // to the ordinary fallback chain (no crash, no blank page).
  bool build(FontReadFn read, void* ctx, unsigned long fileSize, uint16_t& upemOut, FontMapStats& stats);

  // Serialize / load the compact cache. `save` writes the exact on-disk image
  // (magic + version + count + keys + advances); `load` validates magic,
  // version, count vs buffer length and monotonic keys, returning false for a
  // corrupt/foreign file so the caller rebuilds.
  bool save(void* outCtx, bool (*write)(void* ctx, const uint8_t* data, size_t len)) const;
  bool load(const uint8_t* data, size_t len);

  // Raw advanceWidth lookup (font units). Returns false when this codepoint is
  // not mapped (caller falls through to FreeType/fallback chain). `cp` must be
  // a BMP codepoint (<0x10000); supplementary cps always return false here.
  bool lookup(uint32_t cp, uint16_t& advanceWidthFontUnits) const;

  // Compact layout: 8192B BMP presence bitmap + 512B per-256-block prefix
  // index + 2B/advance (dense) + 16B header.  LXGW (42,220 cps) ~= 91 KiB,
  // under the 100 KiB/face ceiling. Only advances are stored per codepoint
  // (the key is implied by the bitmap), matching the 2B/cp budget.
  size_t byteSize() const { return kHeaderSize + kSegIndexBytes + kBitmapBytes + count_ * sizeof(uint16_t); }
  uint16_t upem() const { return upem_; }
  uint32_t count() const { return count_; }
  bool empty() const { return count_ == 0; }

 private:
  static constexpr uint32_t kMagic = 0x504D4646u;  // 'FFMP'
  static constexpr uint8_t kVersion = 2;            // v2: bitmap+prefix (v1 was key/adv pairs)
  static constexpr size_t kHeaderSize = 16;
  static constexpr size_t kBmpBits = 65536;
  static constexpr size_t kBitmapBytes = kBmpBits / 8;      // 8192
  static constexpr size_t kSegCount = kBmpBits / 256;        // 256
  static constexpr size_t kSegIndexBytes = kSegCount * sizeof(uint16_t);  // 512

  uint8_t* present_ = nullptr;    // BMP presence bitmap (kBitmapBytes)
  uint16_t* segBase_ = nullptr;   // prefix count per 256-codepoint block
  uint16_t* advances_ = nullptr;  // raw advanceWidth (font units), dense by cp
  uint32_t count_ = 0;
  uint16_t upem_ = 1000;

  void free();
};

}  // namespace epd
