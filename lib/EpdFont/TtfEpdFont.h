#pragma once

// CrossPoint <- FreeInkFont adapter (FreeType, 4-style, lazy, streaming).
//
// Renders one or more TrueType/OpenType files — including OpenType VARIABLE
// fonts — as a CrossPoint EpdFontFamily with regular / bold / italic /
// bold-italic via the FreeInkFont FreeType backend.
//
// STYLE SOURCES. A family can be built from up to four source files, one per
// style role (0=regular, 1=bold, 2=italic, 3=bold-italic); only regular is
// required. Each face resolves to the best available source:
//   * an explicit file for that style, if supplied (e.g. a separate
//     Family-Italic.ttf → true italic letterforms, or a static Family-Bold.ttf);
//   * else the regular source with the wght axis pushed to bold (variable
//     fonts) or a per-glyph outline embolden (static) for bold;
//   * else the regular source with an ital/slnt axis or an oblique shear for
//     italic. Bold-italic combines the two, preferring an italic source's bold.
// So a single variable file yields all four styles, and dropping in dedicated
// Bold/Italic files upgrades those styles to the real designs.
//
// Memory-lean:
//   * STREAMING (addStreamSource) — FreeType pulls bytes from SD on demand, so a
//     multi-MB variable/CJK file never sits in RAM. Use addResidentSource() only
//     for small fonts already held in RAM.
//   * LAZY per-style faces — only the regular face is built up front; bold /
//     italic / bold-italic (and their glyph caches) are created on first use, so
//     a book with no bold pays nothing for it.
//   * per-face glyph caches that GROW TO CONVERGE (no worst-case pre-reserve):
//     the byte arena and glyph tables start empty and grow only to the book's
//     actual page needs, then stop touching the allocator (clearCache keeps the
//     capacity across page turns). Bounded by a hard byte cap that flushes when
//     full, and shed entirely by releaseResidentCaches() on heap-critical
//     transitions — on par with the SD (.cpfont) font system's discipline.
//   * caches live in PSRAM when the board has it (FontPsram / FontAlloc), so
//     glyph arenas don't consume scarce internal SRAM.
//
// Lifetime: every configured source (resident bytes or streamed read source,
// e.g. an open SD file) is BORROWED and must outlive this object, which must
// outlive any GfxRenderer registration.

#include "VectorFontSupport.h"

#if CROSSPOINT_VECTOR_FONTS

#include <FontPsram.h>
#include <FtFont.h>

#include <cstddef>
#include <cstdint>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "EpdFontFamily.h"
#include "FontMap.h"

#include <memory>

class TtfEpdFont {
 public:
  // Style roles for source slots.
  enum Style : uint8_t { Regular = 0, Bold = 1, Italic = 2, BoldItalic = 3 };

  // Configure a source file for a style role BEFORE calling load(). Regular is
  // required; the rest are optional and upgrade their style to a real design.
  // Resident: bytes borrowed. Streamed: read source borrowed.
  void addResidentSource(uint8_t style, const uint8_t* data, uint32_t len);
  void addStreamSource(uint8_t style, freeink::font::FtFont::ReadFn read, void* ctx, unsigned long fileSize);

  // Build the family at the given reader point size from the configured sources.
  // Returns false if the regular source is missing or unparseable.
  bool load(uint16_t pointSize, bool twoBit = true, size_t glyphCacheBytes = 32 * 1024, uint16_t maxGlyphs = 768);

  // Initialize as a sized variant of an existing (master) font family. Borrows
  // the master font's sources and FreeType faces (no re-parsing of font files
  // and no duplicate FT_Face allocations in memory).
  bool loadVariantOf(TtfEpdFont& master, uint16_t pointSize, bool twoBit = true, size_t glyphCacheBytes = 64 * 1024,
                     uint16_t maxGlyphs = 512);

  EpdFontFamily family() const;

  // Per-scope reset (mirrors SdCardFont::clearCache): retains resident glyph
  // data across PrewarmScopes so build() faults only missing glyphs against
  // f.cps and serves page turns from idle-prewarmed glyphs with zero FreeType
  // rasterizations. Full release still happens via releaseResidentCaches().
  void clearCache();

  // Heap-critical teardown (mirrors SdCardFont::releaseResidentCaches): free
  // every rebuildable cache — the byte arenas, glyph tables, and the lazy bold/
  // italic/bold-italic FreeType faces — keeping only the regular face live so
  // coverage() still answers without a reload. Everything faults back in on
  // demand. Driven by FontCacheManager::releaseSdFontCaches() before heap-hungry
  // transitions (WiFi + web server, image decode, dictionary, sleep).
  void releaseResidentCaches();

  // Batch pre-warm of a face for a render scope (defaults to regular; styles fault lazily).
  // Retains existing glyph coverage and accumulates across scopes until capacity is
  // reached, topping up any earlier codepoints of the page if a flush occurs mid-pass.
  bool build(const char* utf8, uint8_t style = 0);
  bool addCoverage(const char* utf8, uint8_t style = 0);

  // --- Secondary glyph source (S3c PUSH embedded font) -----------------------
  // When a face's cmap has no glyph for a codepoint, faultGlyph returns null and
  // missThunk calls the registered secondary handler. The handler may rasterize
  // the codepoint from another source (e.g. the PIF1 Han bitmap) and inject the
  // result into this same face via injectGlyph(), so vectorBitmapHandler serves
  // it on the normal draw path with no renderer changes. Registered per TtfEpdFont
  // (all styles share it); nullptr by default so every other font is untouched.
  using SecondaryHandler = bool (*)(void* ctx, uint32_t codepoint, uint8_t style, TtfEpdFont& font);
  void setSecondaryHandler(SecondaryHandler handler, void* ctx) {
    secondary_ = handler;
    secondaryCtx_ = ctx;
  }

  // --- Advance-only query (S3c final: measure must not rasterize) -------------
  // Layout measurement needs only advanceX, but EpdFont::getGlyphMetrics() faulted
  // the full glyph (metrics load + rasterize load + render + bitmap packing). On
  // a long CJK document that is thousands of wasted rasterizations. getAdvance()
  // returns the SAME 12.4 fixed-point adv faultGlyph would store (so measure and
  // draw agree exactly) while rasterizing nothing:
  //   * cached in an ordered per-face advance table -> pure binary lookup;
  //   * else glyphId + metricsGlyph26_6 (ONE FT_Load_Glyph, no render) -> cache;
  //   * else (cmap miss) ask the registered AdvanceHandler (PIF1 record[4] /
  //     SD fallback advance) -> cache.
  // Draw still goes through the untouched glyph cache + missThunk/faultGlyph, so
  // no glyph is forced into existence by measuring and none is rasterized twice.
  // Returns false only when no source can supply an advance (caller falls back).
  using AdvanceHandler = bool (*)(void* ctx, uint32_t codepoint, uint8_t style, TtfEpdFont& font, uint16_t& adv12_4);
  void setAdvanceHandler(AdvanceHandler handler, void* ctx) {
    advanceHandler_ = handler;
    advanceCtx_ = ctx;
  }
  bool getAdvance(uint32_t codepoint, uint8_t style, uint16_t& adv12_4);

  static constexpr uint8_t kAdvRaw = 0;    // AdvanceEntry.value holds font units
  static constexpr uint8_t kAdvBaked = 1;  // AdvanceEntry.value holds 12.4

  // S3cPF6: attach a load-time codepoint->advanceWidth map (shared across all
  // faces/variants of this family). When getAdvance misses its persistent hot
  // table it consults this map BEFORE FreeType, avoiding a per-codepoint SD
  // seek into hmtx (which can live ~15 MB into LXGW). A hit is converted to
  // 12.4 via FtFont's x_scale and inserted through the SAME advanceTableInsert
  // path — AdvanceTable.h / FallbackChain.h are untouched. Set to nullptr (the
  // default) for built-in / PUSH embedded faces, which never read SD.
  // S3cPF8e-8: changing the codepoint->rawAw map invalidates any advance rows
  // converted under the previous map. Bump generation so stale H hits miss.
  void setFontMap(std::shared_ptr<epd::FontMap> map) {
    fontMap_ = std::move(map);
    ++advanceGeneration_;
  }
  const epd::FontMap* fontMap() const { return fontMap_.get(); }

  // S3cPF8e-8: hard-invalidate the persistent advance table. Called at every
  // font-identity / point-size / backing-map change point. clearCache() does
  // NOT call this — page turns must retain the table (acceptance item 7).
  void invalidateAdvances() { ++advanceGeneration_; }

#if defined(PF8E7_ADV_TRACE)
  // S3cPF8e-7 DIAG ONLY: renderer fontId stamped by SdCardFontSystem so the
  // [ADV] trace can report which font face served each advance. Compiled out of
  // every functional build (the macro is injected only into the DIAG firmware).
  void setDiagFontId(int id) { diagFontId_ = id; }
  int diagFontId() const { return diagFontId_; }
#endif

  // S3c-plus2: PRIMARY-ONLY advance probe for a fallback handler. Returns the
  // builtin/own-face vector advance (ordered table or ONE cmap metrics load),
  // NEVER calls advanceHandler_ and NEVER rasterizes or writes any table. This
  // is what lets the SD-vector fallback's tier ① consult the embedded builtin
  // face without re-entering that face's own secondary/advance chain (which
  // would recurse back into this SD face -> stack overflow).
  bool primaryAdvanceOnly(uint32_t codepoint, uint8_t style, uint16_t& adv12_4);

  // Inject an already-2-bit rasterized glyph into `style`'s arena. `bmp2bit` is
  // byte-packed exactly like EpdFontData (2 bits/px when twoBit, MSB-first;
  // 1 bit/px otherwise), `bytes` its length. Returns the cached EpdGlyph or null
  // if the arena/table could not grow. Used only by the secondary handler.
  const EpdGlyph* injectGlyph(uint8_t style, uint32_t cp, uint8_t w, uint8_t h, const uint8_t* bmp2bit,
                              uint16_t bytes, int32_t adv12_4, int16_t left, int16_t top);

  // Rasterize a codepoint with THIS font's own FreeType face (no secondary) and
  // return a pointer to its packed bitmap in this font's arena, plus metrics.
  // Valid until the next fault/flush on this font. Used by the PUSH secondary
  // handler to copy an SD-fallback glyph into the PUSH font's arena. Returns
  // nullptr when this font has no glyph for `cp`.
  const uint8_t* rasterizedBitmap(uint32_t cp, uint8_t& w, uint8_t& h, uint16_t& bytes, int32_t& adv12_4,
                                  int16_t& left, int16_t& top, uint8_t style = 0);

  // S3c+ global bitmap-font fallback bridge. `glyphForGlobal()` faults `cp`
  // through this face (primary + its secondary chain) and returns the EpdGlyph
  // in this face's arena (regular style), valid until the next fault/flush on
  // this face — exactly the EpdFontData::glyphMissHandler lifetime contract.
  // `bitmapForGlobal()` resolves that EpdGlyph back to its packed bitmap via
  // bitmapThunk, for the paired vectorBitmapHandler. Returns nullptr on miss.
  const EpdGlyph* glyphForGlobal(uint32_t cp);
  const uint8_t* bitmapForGlobal(const EpdGlyph* glyph);

 private:
  // A borrowed source file (one per style role that the caller supplies).
  struct Source {
    bool present = false;
    bool streamed = false;
    const uint8_t* data = nullptr;  // resident form
    uint32_t len = 0;
    freeink::font::FtFont::ReadFn read = nullptr;  // streamed form
    void* ctx = nullptr;
    unsigned long fileSize = 0;
  };

  struct Face {
    TtfEpdFont* owner = nullptr;
    freeink::font::FtFont ftStorage_;
    freeink::font::FtFont* ft = &ftStorage_;
    freeink::font::PsramVector<uint8_t> coreBmp;
    size_t coreUsed = 0;
    size_t coreCap = 0;
    uint16_t maxCoreGlyphs = 0;
    uint16_t coreCount = 0;
    bool coreFull = false;

    freeink::font::PsramVector<uint8_t> tailBmp;
    size_t tailUsed = 0;
    size_t tailCap = 0;
    uint16_t maxTailGlyphs = 0;

    size_t cap = 0;
    uint16_t maxGlyphs = 0;
    bool twoBit = true;
    uint16_t sizePx = 0;
    uint8_t srcIndex = 0;           // which Source this face initializes from
    int weight = 400;               // design weight requested (wght axis / faux bold)
    bool wantItalic = false;        // request italic from the source (axis or oblique)
    bool inited = false;            // init attempted (lazy)
    bool ready = false;             // FreeType face live
    bool miniMetadataOnly = false;  // bitmap allocation failed: advance-only glyph inserted
    bool flushed = false;           // flushFace() or trimTail() ran during the current pass
    bool faultFailed = false;       // face init or table grow failed (no corrupt glyph inserted)
    uint8_t initFailures = 0;       // consecutive failed initFace attempts
    // GSUB 'liga' resolution (fi fl ff ffi ffl), resolved once at initFace():
    // ligGid[i] is the raw glyph ID for U+FB00+i (0 = none), and ligPairs is
    // the EpdFont pair table (sorted by key) that routes applyLigatures() to
    // those presentation codepoints; faultGlyph() then rasterizes them by
    // glyph ID even when the face's cmap has no entry for them.
    uint32_t ligGid[5] = {0, 0, 0, 0, 0};
    EpdLigaturePair ligPairs[5] = {};
    uint8_t ligPairCount = 0;
    freeink::font::PsramVector<EpdGlyph> glyphs;
    freeink::font::PsramVector<uint32_t> cps;
    freeink::font::PsramVector<uint16_t> slot;
    // S3cPF8e-8: value is size-independent rawAw when kind==kAdvRaw (FontMap
    // source, the shared-face x_scale-mismatch case), read via
    // advance12_4FromRaw() at THIS variant's current size. F/A sources have no
    // font-unit advance (builtin/PIF1 are not TTF), so they store an already
    // aligned 12.4 with kind==kAdvBaked and are served verbatim — a baked 12.4
    // cannot suffer x_scale mismatch because no multiply happens on read.
    // generation guards identity changes that don't clear the table.
    struct AdvanceEntry {
      uint32_t cp;
      uint16_t value;       // rawAw if kind==kAdvRaw, else baked 12.4
      uint16_t generation;
      uint8_t kind;
    };
    freeink::font::PsramVector<AdvanceEntry> advances;
    // Kern pair cache ((left<<32)|right → 4.4 value), sorted by key. Kerning
    // resolves through FtFont (legacy 'kern' table, then the GPOS 'kern'
    // feature) once per pair, then serves from here — getKerning runs for
    // every adjacent glyph pair on every draw/measure pass.
    freeink::font::PsramVector<uint64_t> kernKeys;
    freeink::font::PsramVector<int8_t> kernVals;
    EpdFontData data{};
    EpdFont font{&data};
  };

  // S3cPF8e-8 / PF9a: single conversion口径 for every rawAw source (hot table H
  // and FontMap M). Declared after Face so the nested type is complete. Aligns
  // the SHARED FtFont face to THIS variant's size26_6_ FIRST via the public
  // lineMetrics26_6(target), then reads x_scale. Non-const: it mutates the
  // shared FtFont's size state. No submodule patch needed (PF9a self-contained).
  bool advance12_4FromRaw(Face& f, uint16_t rawAw, int32_t& adv12_4);

  const EpdGlyph* findGlyph(Face& f, uint32_t cp) const;

  static const EpdGlyph* missThunk(void* ctx, uint32_t codepoint);
  static const uint8_t* bitmapThunk(void* ctx, const EpdGlyph* glyph);
  static bool coverageThunk(void* ctx, uint32_t codepoint);
  static int8_t kernThunk(void* ctx, uint32_t leftCp, uint32_t rightCp);

  void resolveFaces();             // map the 4 faces onto the configured sources
  void initFace(Face& f);          // lazy: create the FT face on first use
  void setupFace(Face& f);         // wire data handlers + metrics
  void resolveLigatures(Face& f);  // query GSUB once, build ligGid/ligPairs
  const EpdGlyph* faultGlyph(Face& f, uint32_t codepoint, bool allowFlush = true);
  int8_t faultKern(Face& f, uint32_t leftCp, uint32_t rightCp);
  static void flushFace(Face& f);
  static void trimTail(Face& f);

  struct FaceLock {
    FaceLock(TtfEpdFont* font, const size_t idx) : font_(font), idx_(idx) {
      while (font_ && font_->master_) font_ = font_->master_;
      if (font_ && idx_ < 4) font_->faceLocks_[idx_]++;
    }
    ~FaceLock() {
      if (font_ && idx_ < 4 && font_->faceLocks_[idx_] > 0) font_->faceLocks_[idx_]--;
    }
    FaceLock(const FaceLock&) = delete;
    FaceLock& operator=(const FaceLock&) = delete;

   private:
    TtfEpdFont* font_;
    size_t idx_;
  };

  Source sources_[4];                    // indexed by Style role
  Face faces_[4];                        // 0=regular 1=bold 2=italic 3=bold-italic
  TtfEpdFont* master_ = nullptr;         // if non-null, this is a variant borrowing master's FT faces
  uint8_t faceLocks_[4] = {0, 0, 0, 0};  // face in use by glyph fault (prevents FT_Face deinit)
  uint32_t size26_6_ = 0;                // exact 26.6 ppem (pt @150DPI), no whole-pixel rounding
  uint16_t pointSize_ = 0;
  // S3cPF8e-8: bumped on font-identity / size / backing-map changes. Stored in
  // each AdvanceEntry; a mismatch on lookup means the row belongs to an older
  // generation and is ignored. Second line of defence behind the table clears.
  uint16_t advanceGeneration_ = 1;       // never 0, so a zeroed row can't match
  bool loaded_ = false;
  // Set while a glyph fault runs MemoryManager::ensureFree(): the eviction
  // sink calls releaseResidentCaches() on every TTF font, and tearing down
  // the very faces mid-fault would be use-after-free.
  bool evictionLocked_ = false;

  // S3c secondary glyph source (nullptr for every font except PUSH embedded).
  SecondaryHandler secondary_ = nullptr;
  void* secondaryCtx_ = nullptr;
  // S3c final advance-only source (nullptr for every font except PUSH embedded).
  AdvanceHandler advanceHandler_ = nullptr;
  void* advanceCtx_ = nullptr;
  // S3cPF6 load-time advance map (nullptr = not attached / built-in faces).
  std::shared_ptr<epd::FontMap> fontMap_;
#if defined(PF8E7_ADV_TRACE)
  int diagFontId_ = -1;  // DIAG only; see setDiagFontId above.
#endif
};

// S3c+ [EHP-PROF] — lightweight cumulative glyph-rasterization profiler. Counts
// every FreeType rasterizeGlyph26_6() call and its wall time so the reader can
// report rasterize cost separately from HTML parse / page layout. Call
// epdFontProfileDump() at a page/section boundary; it logs one ASCII line and
// resets the counters. No-op (still linkable) on host builds.
void epdFontProfileDump(const char* tag);

#endif  // CROSSPOINT_VECTOR_FONTS
