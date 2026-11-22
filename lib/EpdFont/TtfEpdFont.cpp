#include "TtfEpdFont.h"
#include "AdvanceTable.h"

#if CROSSPOINT_VECTOR_FONTS

#include <Logging.h>
#include <MemoryManager.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <esp_memory_utils.h>
#endif

namespace {

// S3c+ [EHP-PROF] cumulative glyph-rasterization profile. All state is
// file-static and touched only on the single render task (RenderLock serializes
// layout + render), so plain counters suffice — no allocation, no atomics in
// the hot path. millis() is 1ms-resolution, enough for a per-page total.
#if defined(ESP_PLATFORM)
namespace glyphprof {
uint32_t calls = 0;
uint32_t ms = 0;
inline uint32_t now() { return millis(); }
inline void begin() { /* timestamp captured by caller */ }
inline void add(const uint32_t t0) {
  ++calls;
  const uint32_t dt = now() - t0;
  ms += dt;  // includes wraparound-safe unsigned subtraction
}
}  // namespace glyphprof
#endif

#if defined(PF8E7_ADV_TRACE)
// S3cPF8e-7 DIAG ONLY. One line per getAdvance() call, both the first (cold,
// src=M/F/A) computation and every later hot-table hit (src=H). `raw` is the
// font-unit advanceWidth read back from the attached FontMap when available
// (0 for built-in/PIF1 faces with no FontMap), so a stale 26_4 value served
// from the hot table can be matched against the font's real upem.
inline void advTrace(const uint32_t cp, const char src, const uint32_t raw,
                     const uint16_t fin26_4, const uint16_t sizePt, const int fid) {
  LOG_INF("ADV", "cp=%04X src=%c raw=%u fin26_4=%d size=%u fid=%d",
          cp, src, raw, (int)fin26_4, sizePt, fid);
}
#endif

// S3c-final P0#3 guard: an advance-table buffer must live in WRITABLE RAM.
// Flash DROM is cache-mapped into the same 0x3Cxx.... window as PSRAM, so a
// raw address-range test cannot tell them apart — use the ESP memory utils.
// Returns true for nullptr/empty (nothing to validate yet) and on host builds.
inline bool writableRam(const void* p) {
  if (p == nullptr) return true;
#if defined(ESP_PLATFORM)
  return esp_ptr_internal(p) || esp_ptr_external_ram(p);
#else
  (void)p;
  return true;
#endif
}

// Cache growth policy: PsramAlloc ABORTS when fiFontMalloc fails, and vector
// doubling transiently needs old+new blocks, so every cache expansion must be
// an exact, heap-checked reserve. On no-PSRAM boards the block lands in
// internal DRAM and must leave working headroom for the rest of the system.
bool canGrow(const size_t bytes) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= bytes) return true;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + 12 * 1024;
}

// Exact reserve to `need` rounded up to `step`, capped at `ceil`. Returns
// false (leaving the vector untouched) when the heap cannot fund it.
template <typename V>
bool reserveChecked(V& v, const size_t need, const size_t step, const size_t ceil) {
  if (v.capacity() >= need) return true;
  size_t newCap = ((need + step - 1) / step) * step;
  if (newCap > ceil) newCap = ceil;
  if (newCap < need) newCap = need;  // need may legitimately exceed ceil rounding
  if (!canGrow(newCap * sizeof(typename V::value_type))) return false;
  v.reserve(newCap);
  return true;
}
}  // namespace

void TtfEpdFont::addResidentSource(const uint8_t style, const uint8_t* data, const uint32_t len) {
  if (style >= 4 || data == nullptr || len == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = false;
  s.data = data;
  s.len = len;
}

void TtfEpdFont::addStreamSource(const uint8_t style, const freeink::font::FtFont::ReadFn read, void* ctx,
                                 const unsigned long fileSize) {
  if (style >= 4 || read == nullptr || fileSize == 0) return;
  Source& s = sources_[style];
  s = Source{};
  s.present = true;
  s.streamed = true;
  s.read = read;
  s.ctx = ctx;
  s.fileSize = fileSize;
}

void TtfEpdFont::resolveFaces() {
  // Map each style face onto the best available source. Regular (0) anchors the
  // family; the others prefer an explicit file, then fall back to synthesizing
  // from a related source (wght axis / faux bold for weight, ital axis / oblique
  // for slant — all handled inside FtFont).
  const bool haveB = sources_[Bold].present;
  const bool haveI = sources_[Italic].present;
  const bool haveBI = sources_[BoldItalic].present;

  // regular
  faces_[Regular].srcIndex = Regular;
  faces_[Regular].weight = 400;
  faces_[Regular].wantItalic = false;

  // bold
  faces_[Bold].srcIndex = haveB ? Bold : Regular;
  faces_[Bold].weight = haveB ? 400 : 700;
  faces_[Bold].wantItalic = false;

  // italic
  faces_[Italic].srcIndex = haveI ? Italic : Regular;
  faces_[Italic].weight = 400;
  faces_[Italic].wantItalic = !haveI;  // oblique/axis only when using the roman source

  // bold-italic: dedicated file > bold-of-italic-file > italic-of-bold-file > roman
  Face& bi = faces_[BoldItalic];
  if (haveBI) {
    bi.srcIndex = BoldItalic;
    bi.weight = 400;
    bi.wantItalic = false;
  } else if (haveI) {
    bi.srcIndex = Italic;
    bi.weight = 700;  // wght axis / faux bold on the italic design
    bi.wantItalic = false;
  } else if (haveB) {
    bi.srcIndex = Bold;
    bi.weight = 400;
    bi.wantItalic = true;  // oblique on the bold design
  } else {
    bi.srcIndex = Regular;
    bi.weight = 700;
    bi.wantItalic = true;
  }
}

bool TtfEpdFont::load(const uint16_t pointSize, const bool twoBit, const size_t glyphCacheBytes,
                      const uint16_t maxGlyphs) {
  loaded_ = false;
  master_ = nullptr;
  for (uint8_t& lock : faceLocks_) lock = 0;
  if (!sources_[Regular].present) return false;
  pointSize_ = pointSize;
  // CrossPoint speaks point-size-at-150-DPI (matching the .cpfont converter's
  // FT_Set_Char_Size(size, size, 150, 150)); FreeInkFont speaks pixels. Convert
  // so vector fonts match the on-glyph size and metrics of the bitmap fonts:
  //   ppem = pointSize * 150 / 72, kept in 26.6 so the fractional part survives
  // (glyphs raster and advance at the exact ppem; the per-face sizePx is the
  // rounded form for the integer-pixel Font API).
  size26_6_ = (static_cast<uint32_t>(pointSize) * FONT_RASTER_DPI * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  resolveFaces();
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.owner = this;
    f.ft = &f.ftStorage_;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    // Regular face (style 0) owns the full novel cache budget split into 75% core / 25% tail;
    // styled variants (bold/italic) only hold occasional emphasis words, capped at 64KB/512 glyphs
    // with tail-only eviction.
    f.cap = (i == 0) ? glyphCacheBytes : std::min<size_t>(glyphCacheBytes, 64 * 1024);
    f.maxGlyphs = (i == 0) ? maxGlyphs : std::min<uint16_t>(maxGlyphs, 512);
    f.coreCap = (i == 0) ? (f.cap * 3) / 4 : 0;
    f.tailCap = f.cap - f.coreCap;
    f.maxCoreGlyphs = (i == 0) ? (f.maxGlyphs * 3) / 4 : 0;
    f.maxTailGlyphs = f.maxGlyphs - f.maxCoreGlyphs;
    f.inited = false;
    f.ready = false;
    f.initFailures = 0;
    // Flush the glyph/kern caches, not just the arena cursor: faultGlyph
    // consults the lookup tables before anything else, so entries rasterized
    // at a previous size would serve stale metrics — and their bitmaps get
    // overwritten as new-size glyphs refill the arena from offset 0.
    flushFace(f);
    // Advance values depend on size26_6_; a reload at a new point size must
    // drop cached advances or measure would disagree with draw.
    freeink::font::PsramVector<Face::AdvanceEntry>().swap(f.advances);
    ++advanceGeneration_;                // S3cPF8e-8: invalidate any surviving rows
    f.ligPairCount = 0;                  // re-resolved in initFace; stale pairs must not leak
    for (uint32_t& g : f.ligGid) g = 0;  // across a reload with new sources
  }
  initFace(faces_[0]);  // regular eagerly: validates the font + gives metrics
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);  // handlers + placeholder metrics (regular's)
  loaded_ = true;
  return true;
}

bool TtfEpdFont::loadVariantOf(TtfEpdFont& master, const uint16_t pointSize, const bool twoBit,
                               const size_t glyphCacheBytes, const uint16_t maxGlyphs) {
  loaded_ = false;
  TtfEpdFont* root = &master;
  while (root->master_) root = root->master_;
  if (!root->loaded_) return false;
  master_ = root;
  for (uint8_t& lock : faceLocks_) lock = 0;
  pointSize_ = pointSize;
  size26_6_ = (static_cast<uint32_t>(pointSize) * FONT_RASTER_DPI * 64u + 36u) / 72u;
  const uint16_t sizePx = static_cast<uint16_t>((size26_6_ + 32u) >> 6);
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    const Face& mf = root->faces_[i];
    f.owner = this;
    f.ft = mf.ft;
    f.srcIndex = mf.srcIndex;
    f.weight = mf.weight;
    f.wantItalic = mf.wantItalic;
    f.twoBit = twoBit;
    f.sizePx = sizePx;
    // Variants only use the tail buffer (no core retention)
    f.cap = (i == 0) ? glyphCacheBytes : std::min<size_t>(glyphCacheBytes, 64 * 1024);
    f.maxGlyphs = (i == 0) ? maxGlyphs : std::min<uint16_t>(maxGlyphs, 512);
    f.coreCap = 0;
    f.maxCoreGlyphs = 0;
    f.tailCap = f.cap;
    f.maxTailGlyphs = f.maxGlyphs;
    f.inited = false;
    f.ready = false;
    f.initFailures = 0;
    flushFace(f);
    freeink::font::PsramVector<Face::AdvanceEntry>().swap(f.advances);
    ++advanceGeneration_;                // S3cPF8e-8: invalidate advance rows on variant init
    f.ligPairCount = 0;
    for (uint32_t& g : f.ligGid) g = 0;
  }
  initFace(faces_[0]);
  if (!faces_[0].ready) return false;
  for (int i = 1; i < 4; ++i) setupFace(faces_[i]);
  loaded_ = true;
  return true;
}

void TtfEpdFont::initFace(Face& f) {
  if (master_) {
    const size_t idx = static_cast<size_t>(&f - faces_);
    Face& mf = master_->faces_[idx];
    if (f.inited && f.ready && mf.ready) return;
    if (f.initFailures >= 2) return;
    if (!mf.ready) master_->initFace(mf);
    f.ready = mf.ready;
    f.inited = mf.inited;
    if (!f.ready) {
      f.initFailures++;
      return;
    }
    f.initFailures = 0;
    f.ligPairCount = mf.ligPairCount;
    for (int i = 0; i < 5; ++i) {
      f.ligGid[i] = mf.ligGid[i];
      f.ligPairs[i] = mf.ligPairs[i];
    }
    setupFace(f);
    return;
  }
  if (f.inited && f.ready) return;
  if (f.initFailures >= 2) return;
  const Source& s = sources_[f.srcIndex];
  if (!s.present) {
    f.ready = false;
    f.inited = true;
    return;
  }
  const auto tryInit = [&]() {
    return s.streamed ? f.ft->initStream(s.read, s.ctx, s.fileSize, f.sizePx, f.weight, f.wantItalic)
                      : f.ft->init(s.data, s.len, f.sizePx, f.weight, f.wantItalic);
  };
  f.ready = tryInit();
  if (!f.ready) {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
    f.ready = tryInit();
  }
  if (!f.ready) {
    // Transient failure under heap pressure; leave f.inited false so a later fault can retry,
    // but bound retries so a persistent failure doesn't thrash ensureFree on every glyph.
    f.inited = false;
    f.initFailures++;
    return;
  }
  f.initFailures = 0;
  f.inited = true;
  if (f.ready) {
    // Full auto-hinting + stem darkening pair with the BW-aware quantizer in
    // faultGlyph: the 50% ink threshold needs stems snapped to full-coverage
    // pixels or strokes break up / vary per letter on BW page turns (see
    // platformio.ini's FREEINK_FONT_ENABLE_AUTOHINT note). Auto (both axes)
    // rather than Light (vertical only): Light leaves stem WIDTHS fractional,
    // so the ink threshold prints the same-width stem as 1px or 2px depending
    // on each glyph's horizontal phase — visibly uneven letter weights. Auto
    // equalizes stem widths across the face. Darkening nudges borderline-thin
    // strokes over the threshold.
    freeink::font::FtFont::RenderOptions ro;
    ro.hinting = freeink::font::FtFont::HintingMode::Auto;
    ro.stemDarkening = true;
    if (!f.ft->setRenderOptions(ro)) {
      LOG_ERR("TTF", "Auto hinting unavailable (FREEINK_FONT_ENABLE_AUTOHINT not compiled)");
    }
    // GPOS kerning: modern variable fonts (e.g. Merriweather VF) ship pair kerning
    // exclusively in GPOS. Memory-backed (resident) faces borrow a zero-copy view.
    // For streamed faces on PSRAM-equipped boards, allow FtFont to fetch and own
    // only the exact GPOS table (allocated via fiFontMalloc into PSRAM), keeping it
    // bounded (256 KB) so multi-megabyte CJK GPOS tables won't allocate.
    // On boards without PSRAM (C3), keep budget 0 to protect tight DRAM.
#if defined(BOARD_HAS_PSRAM)
    f.ft->setGposByteBudget(256 * 1024);
#else
    f.ft->setGposByteBudget(0);
#endif
    resolveLigatures(f);
  }
  // Caches are NOT pre-reserved: the byte arenas (f.coreBmp / f.tailBmp) and the glyph tables
  // grow on demand in faultGlyph and converge on the book's page needs (see the
  // header's memory note). f.cap is only the hard ceiling that triggers a flush.
  setupFace(f);
}

void TtfEpdFont::resolveLigatures(Face& f) {
  // One GSUB pass per face init: resolve the standard Latin ligatures to glyph
  // IDs, then drop the table — nothing else queries GSUB, so a streamed face
  // never keeps its (budgeted) table copy resident.
  static constexpr uint32_t kComps[5][3] = {
      {'f', 'f', 0}, {'f', 'i', 0}, {'f', 'l', 0}, {'f', 'f', 'i'}, {'f', 'f', 'l'}};
  f.ft->setGsubByteBudget(48 * 1024);  // C3 DRAM discipline: oversized GSUB → no ligatures, not OOM
  for (int i = 0; i < 5; ++i) f.ligGid[i] = f.ft->ligatureGlyphId(kComps[i], kComps[i][2] ? 3 : 2);
  f.ft->releaseLigatureTable();

  // Pair table for EpdFont::applyLigatures(), keyed on the U+FB00–FB04
  // presentation codepoints. A ligature is usable if GSUB named a glyph or the
  // cmap maps the presentation codepoint directly. Entries are appended in
  // ascending key order (the ff/fi/fl keys sort below the FB00-chained ones).
  auto avail = [&f](const int i) { return f.ligGid[i] != 0 || f.ft->hasGlyph(0xFB00u + i); };
  f.ligPairCount = 0;
  auto add = [&f](const uint32_t left, const uint32_t right, const uint32_t out) {
    f.ligPairs[f.ligPairCount++] = EpdLigaturePair{(left << 16) | right, out};
  };
  if (avail(0)) add('f', 'f', 0xFB00);
  if (avail(1)) add('f', 'i', 0xFB01);
  if (avail(2)) add('f', 'l', 0xFB02);
  if (avail(0)) {  // ffi/ffl chain through the ff result as the new left
    if (avail(3)) add(0xFB00, 'i', 0xFB03);
    if (avail(4)) add(0xFB00, 'l', 0xFB04);
  }
}

void TtfEpdFont::setupFace(Face& f) {
  // Metrics from this face once live, else borrow the (always-live) regular
  // face's — same font/size, so a fine placeholder until this face is faulted.
  freeink::font::FtFont& src = f.ready ? *f.ft : *faces_[0].ft;
  // Fractional-ppem line metrics, rounded once here rather than per-call.
  freeink::font::FtFont::LineMetrics lm;
  int ascent, lineHeight;
  if (src.lineMetrics26_6(f.owner->size26_6_, lm)) {
    ascent = static_cast<int>((lm.ascender26_6 + 32) >> 6);
    lineHeight = static_cast<int>((lm.height26_6 + 32) >> 6);
  } else {
    ascent = src.ascent(f.sizePx);
    lineHeight = src.lineHeight(f.sizePx);
  }
  f.data = EpdFontData{};
  f.data.advanceY = static_cast<uint8_t>(lineHeight > 255 ? 255 : (lineHeight < 0 ? 0 : lineHeight));
  f.data.ascender = ascent;
  f.data.descender = lineHeight - ascent > 0 ? lineHeight - ascent : 0;
  f.data.is2Bit = f.twoBit;
  f.data.glyphMissHandler = &TtfEpdFont::missThunk;
  f.data.glyphMissCtx = &f;
  f.data.coverageHandler = &TtfEpdFont::coverageThunk;
  f.data.vectorBitmapHandler = &TtfEpdFont::bitmapThunk;
  // GSUB-derived ligature pairs (resolveLigatures). Empty until this face
  // inits — a lazy style renders its first pass ligature-free, then picks
  // them up once its glyphs fault the face in.
  f.data.ligaturePairs = f.ligPairCount ? f.ligPairs : nullptr;
  f.data.ligaturePairCount = f.ligPairCount;
  f.data.kernHandler = &TtfEpdFont::kernThunk;
}

void TtfEpdFont::flushFace(Face& f) {
  f.glyphs.clear();
  f.cps.clear();
  f.slot.clear();
  f.kernKeys.clear();
  f.kernVals.clear();
  f.coreUsed = 0;
  f.tailUsed = 0;
  f.coreCount = 0;
  f.coreFull = false;
  f.miniMetadataOnly = false;
  f.faultFailed = false;
  f.flushed = true;
}

void TtfEpdFont::trimTail(Face& f) {
  if (f.miniMetadataOnly || f.tailCap == 0) {
    flushFace(f);
    return;
  }
  size_t w = 0;
  for (size_t r = 0; r < f.cps.size(); ++r) {
    if (f.slot[r] < f.coreCount) {
      f.cps[w] = f.cps[r];
      f.slot[w] = f.slot[r];
      ++w;
    }
  }
  f.cps.resize(w);
  f.slot.resize(w);
  f.glyphs.resize(f.coreCount);
  f.tailUsed = 0;
  f.flushed = true;
}

void TtfEpdFont::clearCache() {
  // Retain resident glyph data across PrewarmScopes (mirrors SdCardFont::resetStyleMiniData):
  // build() faults only missing codepoints against f.cps, which lets the idle
  // prewarm of page N+1 serve the actual page turn with zero FreeType rasterizations.
  // Full cache release still happens on memory pressure via releaseResidentCaches().
  // If an allocation failure left incomplete glyphs (miniMetadataOnly), flush so next pass retries.
  for (Face& f : faces_) {
    if (f.inited && f.miniMetadataOnly) flushFace(f);
  }
}

void TtfEpdFont::releaseResidentCaches() {
  if (evictionLocked_) return;  // mid-fault: this font's faces are live
  for (int i = 0; i < 4; ++i) {
    Face& f = faces_[i];
    f.initFailures = 0;
    // Actually RELEASE the caches (swap-with-empty frees capacity; clear() alone
    // would not).
    flushFace(f);
    freeink::font::PsramVector<uint8_t>().swap(f.coreBmp);
    freeink::font::PsramVector<uint8_t>().swap(f.tailBmp);
    freeink::font::PsramVector<EpdGlyph>().swap(f.glyphs);
    freeink::font::PsramVector<uint32_t>().swap(f.cps);
    freeink::font::PsramVector<uint16_t>().swap(f.slot);
    freeink::font::PsramVector<Face::AdvanceEntry>().swap(f.advances);
    ++advanceGeneration_;                // S3cPF8e-8: invalidate advance rows on cache release
    freeink::font::PsramVector<uint64_t>().swap(f.kernKeys);
    freeink::font::PsramVector<int8_t>().swap(f.kernVals);
    // Keep the regular face's FreeType face live so coverage()/metrics still
    // answer without a reload (mirrors SD keeping its interval table resident).
    // Shed the lazy bold/italic/bold-italic faces entirely; they re-init on the
    // next glyph fault for that style.
    if (i == 0) continue;
    if (master_) {
      f.inited = false;
      f.ready = false;
      continue;
    }
    if (faceLocks_[i] > 0) continue;
    if (f.inited) {
      f.ft->deinit();
      f.inited = false;
      f.ready = false;
    }
  }
}

const EpdGlyph* TtfEpdFont::faultGlyph(Face& f, const uint32_t cp, const bool allowFlush) {
  if (!f.inited || (master_ && !master_->faces_[&f - faces_].ready)) initFace(f);
  if (!f.ready) {
    f.faultFailed = true;
    return nullptr;
  }
  {
    const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
    if (it != f.cps.end() && *it == cp) return &f.glyphs[f.slot[static_cast<size_t>(it - f.cps.begin())]];
  }

  // Resolve to a glyph ID: cmap first, then the GSUB result for the ligature
  // presentation codepoints (whose glyphs commonly have no cmap entry at all).
  freeink::font::FtFont::GlyphId gid = f.ft->glyphId(cp);
  if (gid == 0 && cp >= 0xFB00u && cp <= 0xFB04u) gid = f.ligGid[cp - 0xFB00u];
  if (gid == 0 && cp != REPLACEMENT_GLYPH) return nullptr;

  const size_t faceIdx = static_cast<size_t>(&f - faces_);
  FaceLock lock(master_ ? master_ : this, faceIdx);

  // The metrics pass costs a second glyph load per miss, but retains the
  // fractional advance; GlyphBitmap.advance is whole pixels. Truncate to 12.4
  // below so only the renderer rounds advance + kerning to whole pixels.
  const uint32_t size26_6 = f.owner->size26_6_;
  freeink::font::FtFont::GlyphMetrics gm;
  const bool haveMetrics = f.ft->metricsGlyph26_6(gid, size26_6, gm);
#if defined(ESP_PLATFORM)
  const uint32_t profT0 = glyphprof::now();
#endif
  const freeink::font::GlyphBitmap* g = f.ft->rasterizeGlyph26_6(gid, size26_6);
#if defined(ESP_PLATFORM)
  glyphprof::add(profT0);
#endif
  uint32_t px = (g && g->pixels) ? static_cast<uint32_t>(g->width) * g->height : 0;
  size_t bytes = px ? (f.twoBit ? (px + 3) / 4 : (px + 7) / 8) : 0;
  if (bytes > f.cap) {
    bytes = 0;
    px = 0;
    f.miniMetadataOnly = true;
  }
  // Determine whether this glyph routes to coreBmp or tailBmp
  if (!f.coreFull) {
    if (f.coreCount >= f.maxCoreGlyphs || f.coreUsed + bytes > f.coreCap) {
      f.coreFull = true;
    }
  }

  // Route to tail buffer when core is full, or when this face is tail-only (coreCap == 0)
  if (f.coreFull || f.coreCap == 0) {
    // Route to tail buffer: trim tail on overflow (or flush if face has no core)
    const size_t tailGlyphs = f.glyphs.size() - f.coreCount;
    if (tailGlyphs >= f.maxTailGlyphs || f.tailUsed + bytes > f.tailCap) {
      if (!allowFlush) {
        f.flushed = true;
        return nullptr;
      }
      trimTail(f);
      if (bytes > f.tailCap) {
        bytes = 0;
        px = 0;
        f.miniMetadataOnly = true;
      }
      // If trimTail fell back to flushFace, core was cleared (coreFull = false, coreCount = 0).
      // Re-evaluate coreFull for the freshly flushed face so isTail routes to core.
      if (!f.coreFull && (f.coreCount >= f.maxCoreGlyphs || f.coreUsed + bytes > f.coreCap)) {
        f.coreFull = true;
      }
    }
  }

  const bool isTail = f.coreFull || f.coreCap == 0;

  // All growth below is exact and heap-checked (see reserveChecked). On a
  // failed grow, ask the memory manager to evict rebuildable caches (render
  // glyph cache, other fonts' arenas — evictionLocked_ keeps THIS font's
  // faces alive) and retry once. After that: a failed table grow is an
  // uncached miss (caller skips the glyph this pass); a failed arena grow
  // degrades to an advance-only glyph so layout survives.
  static constexpr size_t kTableStep = 64;
  const auto growTables = [&]() {
    return reserveChecked(f.glyphs, f.glyphs.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.cps, f.cps.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.slot, f.slot.size() + 1, kTableStep, f.maxGlyphs);
  };
  const auto evictOthers = [&]() {
    evictionLocked_ = true;
    freeink::MemoryManager::instance().ensureFree(16 * 1024);
    evictionLocked_ = false;
  };
  if (!growTables()) {
    evictOthers();
    if (!growTables()) {
      f.faultFailed = true;
      return nullptr;
    }
  }

  auto& bmpVec = isTail ? f.tailBmp : f.coreBmp;
  size_t& bmpUsed = isTail ? f.tailUsed : f.coreUsed;
  const size_t bmpCap = isTail ? f.tailCap : f.coreCap;

  if (px && !reserveChecked(bmpVec, bmpUsed + bytes, 4096, bmpCap)) {
    evictOthers();
    if (!reserveChecked(bmpVec, bmpUsed + bytes, 4096, bmpCap)) {
      bytes = 0;
      px = 0;
      f.miniMetadataOnly = true;
    }
  }

  if (!g || !haveMetrics) f.miniMetadataOnly = true;
  EpdGlyph eg{};
  const int32_t adv12_4 = haveMetrics ? fp4::truncate26_6_to_4(gm.advance26_6) : (g ? g->advance << 4 : 0);
  eg.advanceX = static_cast<uint16_t>(adv12_4 < 0 ? 0 : adv12_4);
  if (px && g && g->pixels) {
    // Grow the byte arena on demand toward the book's page high-water mark.
    // flush/trim above guarantees bmpUsed + bytes <= bmpCap, so this never exceeds
    // the ceiling; capacity is retained across page turns (clearCache keeps it).
    if (bmpVec.size() < bmpUsed + bytes) bmpVec.resize(bmpUsed + bytes, 0);
    uint8_t* dst = bmpVec.data() + bmpUsed;
    for (size_t i = 0; i < bytes; ++i) dst[i] = 0;
    for (uint32_t i = 0; i < px; ++i) {
      const uint8_t a = g->pixels[i];
      if (f.twoBit) {
        // Same 25/50/75% level thresholds as the .cpfont converter
        // (fontconvert_sdcard.py's 4-bit >=4/8/12 downsample), so a TTF face
        // prints at the same stroke weight as its .cpfont rendition. The BW
        // page-turn pass inks ANY nonzero level, so the 25% start is what
        // gives CJK strokes their full weight; starting at 50% rendered
        // visibly thinner than cpfont.
        const uint8_t v = a < 64 ? 0 : a < 128 ? 1 : a < 192 ? 2 : 3;
        dst[i >> 2] |= static_cast<uint8_t>(v << ((3 - (i & 3)) * 2));
      } else if (a >= 128) {
        dst[i >> 3] |= static_cast<uint8_t>(1u << (7 - (i & 7)));
      }
    }
    eg.width = static_cast<uint8_t>(g->width);
    eg.height = static_cast<uint8_t>(g->height);
    eg.left = g->xoff;
    // FreeInkFont's yoff is negative-above (top offset from baseline); CrossPoint's
    // EpdGlyph.top is positive-above (renderer computes screen Y = cursorY - top,
    // matching the .cpfont converter's top = FreeType bitmap_top). Flip the sign.
    eg.top = static_cast<int16_t>(-g->yoff);
    eg.dataOffset = static_cast<uint32_t>(bmpUsed);
    eg.dataLength = static_cast<uint16_t>(bytes);
    bmpUsed += bytes;
  } else {
    eg.dataOffset = static_cast<uint32_t>(bmpUsed);
    eg.dataLength = 0;
  }

  f.glyphs.push_back(eg);
  if (!isTail) {
    f.coreCount++;
  }
  const uint16_t newIdx = static_cast<uint16_t>(f.glyphs.size() - 1);
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.cps.begin());
  f.cps.insert(it, cp);
  f.slot.insert(f.slot.begin() + pos, newIdx);
  return &f.glyphs[newIdx];
}

const uint8_t* TtfEpdFont::rasterizedBitmap(const uint32_t cp, uint8_t& w, uint8_t& h, uint16_t& bytes,
                                            int32_t& adv12_4, int16_t& left, int16_t& top, const uint8_t style) {
  if (style >= 4 || !loaded_) return nullptr;
  Face& f = faces_[style];
  // faultGlyph uses only this face's FreeType cmap (it does NOT call secondary_),
  // so this is safe to call from a secondary handler without recursion.
  const EpdGlyph* g = faultGlyph(f, cp, /*allowFlush=*/true);
  if (!g || g->dataLength == 0) return nullptr;
  const uint8_t* bmp = bitmapThunk(&f, g);
  if (!bmp) return nullptr;
  w = g->width;
  h = g->height;
  bytes = g->dataLength;
  adv12_4 = g->advanceX;
  left = g->left;
  top = g->top;
  return bmp;
}

int8_t TtfEpdFont::faultKern(Face& f, const uint32_t leftCp, const uint32_t rightCp) {
  if (leftCp == 0 || rightCp == 0) return 0;
  if (!f.inited || (master_ && !master_->faces_[&f - faces_].ready)) initFace(f);
  if (!f.ready) return 0;
  const uint64_t key = (uint64_t(leftCp) << 32) | rightCp;
  {
    const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
    if (it != f.kernKeys.end() && *it == key) return f.kernVals[static_cast<size_t>(it - f.kernKeys.begin())];
  }

  // 26.6 → 4.4 (round-half-away), clamped to the int8 contract (±8px, far
  // beyond any real kern at reader sizes).
  const int32_t k26 = f.ft->kerning26_6(leftCp, rightCp, f.owner->size26_6_);
  int32_t k4 = (k26 + (k26 < 0 ? -2 : 2)) / 4;
  if (k4 < -128) k4 = -128;
  if (k4 > 127) k4 = 127;

  // Text uses few distinct pairs; the cap only guards against pathological
  // content churning the cache without bound.
  static constexpr size_t kKernCap = 512;
  if (f.kernKeys.size() >= kKernCap) {
    f.kernKeys.clear();
    f.kernVals.clear();
  }
  // One-time exact reserve: with capacity pinned at the cap, the sorted
  // inserts below never touch the allocator. Uncached result if the heap
  // cannot fund the cache at all.
  if (!reserveChecked(f.kernKeys, kKernCap, kKernCap, kKernCap) ||
      !reserveChecked(f.kernVals, kKernCap, kKernCap, kKernCap)) {
    return static_cast<int8_t>(k4);
  }
  const auto it = std::lower_bound(f.kernKeys.begin(), f.kernKeys.end(), key);
  const size_t pos = static_cast<size_t>(it - f.kernKeys.begin());
  f.kernKeys.insert(it, key);
  f.kernVals.insert(f.kernVals.begin() + pos, static_cast<int8_t>(k4));
  return static_cast<int8_t>(k4);
}

const EpdGlyph* TtfEpdFont::missThunk(void* ctx, const uint32_t codepoint) {
  Face* f = static_cast<Face*>(ctx);
  const EpdGlyph* g = f->owner->faultGlyph(*f, codepoint);
  if (g != nullptr || f->owner->secondary_ == nullptr) return g;
  // S3c: cmap had no glyph. Let the secondary source (PIF1 bitmap / SD fallback)
  // rasterize this codepoint into the same face arena, then re-resolve it so the
  // normal vectorBitmapHandler path serves the bitmap. One attempt only: a
  // secondary miss leaves the replacement glyph to EpdFont::getGlyph().
  const size_t style = static_cast<size_t>(f - f->owner->faces_);
  f->owner->secondary_(f->owner->secondaryCtx_, codepoint, static_cast<uint8_t>(style), *f->owner);
  return f->owner->findGlyph(*f, codepoint);
}

const EpdGlyph* TtfEpdFont::findGlyph(Face& f, const uint32_t cp) const {
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  if (it != f.cps.end() && *it == cp) {
    return &f.glyphs[f.slot[static_cast<size_t>(it - f.cps.begin())]];
  }
  return nullptr;
}

// S3cPF8e-8 / PF9a: the single conversion口径 for every rawAw source. The FtFont
// is SHARED across all variants of this family, so its FT_Face may currently be
// sized for a sibling variant. Align it to THIS variant's size26_6_ before
// reading x_scale — otherwise the multiply uses the wrong ppem.
// PF9a: use the public lineMetrics26_6(target) instead of a private forwarder —
// it passes the EXTERNAL target into ensureSize26_6 (not size26_6_ itself), so
// it truly re-sizes the face when the target differs and is a cached no-op when
// equal. Keeps the main repo self-contained (no submodule patch required).
bool TtfEpdFont::advance12_4FromRaw(Face& f, uint16_t rawAw, int32_t& adv12_4) {
  freeink::font::FtFont::LineMetrics alignSink;
  if (!f.ft->lineMetrics26_6(size26_6_, alignSink)) return false;
  int32_t adv26 = 0;
  if (!f.ft->unitsToAdvance26_6(rawAw, adv26)) return false;
  adv12_4 = fp4::truncate26_6_to_4(adv26);
  return true;
}

// S3c final — advance-only query for layout measurement. NEVER rasterizes.
// The advance returned is byte-identical to what faultGlyph() would cache
// (same gm.advance26_6 -> truncate26_6_to_4, or the same secondary source), so
// measure and draw cannot disagree. FreeType loads happen at most ONCE per
// codepoint per face and the 12.4 result lives in `f.advances`, which persists
// across page turns (unlike the rasterized glyph arena).
bool TtfEpdFont::getAdvance(const uint32_t cp, const uint8_t style, uint16_t& adv12_4) {
#if defined(PF8E7_ADV_TRACE)
  char advSrc = 'N';       // H=hot table, M=FontMap, F=FreeType, A=handler, N=none
  uint32_t advRaw = 0;     // font-unit advanceWidth from FontMap (0 if no map)
  auto emitAdv = [&](const uint16_t fin) {
    advTrace(cp, advSrc, advRaw, fin, pointSize_, diagFontId_);
  };
  // A cold call succeeds through the hot table only when a prior call already
  // cached this cp; stamp `raw` from the map so a stale 26_4 can be diagnosed.
  auto lookupRaw = [&]() {
    if (fontMap_) { uint16_t r = 0; if (fontMap_->lookup(cp, r)) advRaw = r; }
  };
#endif
  if (style >= 4 || !loaded_) return false;
  Face& f = faces_[style];
  if (!f.inited || (master_ && !master_->faces_[style].ready)) initFace(f);
  if (!f.ready) return false;

  // 1) Served from the persistent advance table — no glyph load / SD seek.
  // S3cPF8e-8: a kAdvRaw row holds size-independent rawAw; convert it through
  // the SAME helper as the FontMap path so a shared face left at a sibling
  // size can never yield a wrong-x_scale advance. A kAdvBaked row already holds
  // the correctly-aligned 12.4 (F/A sources have no font units) and is served
  // verbatim — no multiply, hence no exposure to the face's current size.
  // A generation mismatch means an older font map — treat as a miss.
  {
    const auto it = std::lower_bound(f.advances.begin(), f.advances.end(), cp,
                                     [](const Face::AdvanceEntry& e, uint32_t c) { return e.cp < c; });
    if (it != f.advances.end() && it->cp == cp && it->generation == advanceGeneration_) {
      if (it->kind == kAdvBaked) {
        adv12_4 = it->value;
#if defined(PF8E7_ADV_TRACE)
        advSrc = 'H'; lookupRaw(); emitAdv(adv12_4);
#endif
        return true;
      }
      int32_t fin = 0;
      // Single-threaded layout on this core; FaceLock guards glyph-fault
      // re-entry (face deinit), not concurrent measure, so the align-in-helper
      // is safe here ahead of the lock below.
      if (advance12_4FromRaw(f, it->value, fin)) {
        adv12_4 = static_cast<uint16_t>(fin < 0 ? 0 : fin);
#if defined(PF8E7_ADV_TRACE)
        advSrc = 'H'; advRaw = it->value; emitAdv(adv12_4);
#endif
        return true;
      }
    }
    // generation mismatch / conversion failure falls through to M/F paths.
  }

  // Hard cap: if the table is full, defer to the caller's ordinary (slower)
  // path rather than grow unbounded. Bounded memory, no correctness loss.
  const size_t cap = (style == 0) ? 4096u : 512u;
  if (f.advances.size() >= cap) {
#if defined(PF8E7_ADV_TRACE)
    emitAdv(0);
#endif
    return false;
  }

  int32_t adv = 0;
  bool have = false;
  uint16_t storeVal = 0;               // S3cPF8e-8: value written to the hot table
  uint8_t storeKind = kAdvBaked;       // ...and its kind (default: baked 12.4)
  const size_t faceIdx = static_cast<size_t>(&f - faces_);
  FaceLock lock(master_ ? master_ : this, faceIdx);

  // S3cPF6/S3cPF8e-8: consult the load-time advance map BEFORE any FreeType
  // glyph load. A hit needs only one x_scale multiply and is byte-identical to
  // metricsGlyph26_6().advance26_6 — PROVIDED the shared face is first aligned
  // to this variant's size. Route through advance12_4FromRaw() for that align,
  // and cache the size-independent rawAw (not the 12.4) so sibling variants
  // never inherit a wrong-x_scale row.
  if (fontMap_) {
    uint16_t rawAw = 0;
    if (fontMap_->lookup(cp, rawAw)) {
#if defined(PF8E7_ADV_TRACE)
      advRaw = rawAw;
#endif
      int32_t fin = 0;
      if (advance12_4FromRaw(f, rawAw, fin)) {
        adv = fin;
        have = true;
        storeVal = rawAw;
        storeKind = kAdvRaw;
#if defined(PF8E7_ADV_TRACE)
        advSrc = 'M';
#endif
      }
    }
  }

  const freeink::font::FtFont::GlyphId gid = have ? 0 : f.ft->glyphId(cp);
  if (!have && (gid != 0 || (cp >= 0xFB00u && cp <= 0xFB04u && f.ligGid[cp - 0xFB00u] != 0))) {
    // 2) Vector glyph: ONE metrics load (FT_Load_Glyph, no FT_Render_Glyph).
    const freeink::font::FtFont::GlyphId useGid =
        gid ? gid : f.ligGid[cp - 0xFB00u];
    freeink::font::FtFont::GlyphMetrics gm;
    if (f.ft->metricsGlyph26_6(useGid, size26_6_, gm)) {
      adv = fp4::truncate26_6_to_4(gm.advance26_6);
      have = true;
      // metricsGlyph26_6 aligned the face to size26_6_; this 12.4 is final.
      storeVal = static_cast<uint16_t>(adv < 0 ? 0 : adv);
      storeKind = kAdvBaked;
#if defined(PF8E7_ADV_TRACE)
      advSrc = 'F'; lookupRaw();
#endif
    }
  }
  if (!have && advanceHandler_ != nullptr) {
    // 3) cmap miss: ask PIF1 (record[4]) / SD fallback for advance only.
    uint16_t secondaryAdv = 0;
    if (advanceHandler_(advanceCtx_, cp, static_cast<uint8_t>(style), *this, secondaryAdv)) {
      adv = secondaryAdv;
      have = true;
      storeVal = secondaryAdv;   // handler gives 12.4 directly (no font units)
      storeKind = kAdvBaked;
#if defined(PF8E7_ADV_TRACE)
      advSrc = 'A';
#endif
    }
  }
  if (!have) {
#if defined(PF8E7_ADV_TRACE)
    emitAdv(0);
#endif
    return false;
  }

  // S3c-final P0#3 ROOT-CAUSE FIX:
  // The previous code computed `pos = lower_bound(...)` BEFORE reserveChecked().
  // reserveChecked() may call vector::reserve(), which REALLOCATES and frees the
  // old buffer; the already-computed `pos` then dangles into freed/remapped
  // memory. vector::insert(pos, ...) invoked __copy_move_backward on that stale
  // iterator, writing to 0x3c5afffc — a Flash-DROM (read-only cache) address —
  // causing "Dbus write to cache rejected" on both large and small documents.
  // Fix: grow FIRST (shared AdvanceTable.h, same code the host test runs), then
  // validate the buffer is writable, then insert from the STABLE buffer.
  static constexpr size_t kAdvanceStep = 64;
  if (!epd::advanceTableReserve(f.advances, f.advances.size() + 1, kAdvanceStep, cap, canGrow)) {
#if defined(PF8E7_ADV_TRACE)
    emitAdv(0);
#endif
    return false;
  }
  // Defence in depth: never write if the (possibly reallocated) buffer is not
  // in writable RAM. Falling back to the ordinary slow path is always safe.
  if (!writableRam(f.advances.data())) {
#if defined(PF8E7_ADV_TRACE)
    emitAdv(0);
#endif
    return false;
  }
  const Face::AdvanceEntry entry{cp, storeVal, advanceGeneration_, storeKind};
  if (!epd::advanceTableInsert(f.advances, entry, kAdvanceStep, cap, canGrow)) {
#if defined(PF8E7_ADV_TRACE)
    emitAdv(0);
#endif
    return false;
  }
  adv12_4 = static_cast<uint16_t>(adv < 0 ? 0 : adv);
#if defined(PF8E7_ADV_TRACE)
  emitAdv(adv12_4);
#endif
  return true;
}

// S3c-plus2: primary-only advance probe (see header). Deliberately does NOT
// consult advanceHandler_ / secondary_ and does NOT rasterize or write a table:
// it is safe to call from another face's fallback handler without any risk of
// re-entering this face's own handler chain.
bool TtfEpdFont::primaryAdvanceOnly(const uint32_t cp, const uint8_t style, uint16_t& adv12_4) {
  if (style >= 4 || !loaded_) return false;
  Face& f = faces_[style];
  if (!f.inited || (master_ && !master_->faces_[style].ready)) initFace(f);
  if (!f.ready) return false;
  // Own persistent table first (zero FreeType work).
  // S3cPF8e-8: honour the same kind/generation semantics as getAdvance's H hit.
  {
    const auto it = std::lower_bound(f.advances.begin(), f.advances.end(), cp,
                                     [](const Face::AdvanceEntry& e, uint32_t c) { return e.cp < c; });
    if (it != f.advances.end() && it->cp == cp && it->generation == advanceGeneration_) {
      if (it->kind == kAdvBaked) {
        adv12_4 = it->value;
        return true;
      }
      int32_t fin = 0;
      if (advance12_4FromRaw(f, it->value, fin)) {
        adv12_4 = static_cast<uint16_t>(fin < 0 ? 0 : fin);
        return true;
      }
    }
  }
  // Own cmap: ONE metrics load, no render, no handler call.
  const size_t faceIdx = static_cast<size_t>(&f - faces_);
  FaceLock lock(master_ ? master_ : this, faceIdx);
  const freeink::font::FtFont::GlyphId gid = f.ft->glyphId(cp);
  if (gid == 0) return false;  // primary cmap miss — let caller try next tier
  freeink::font::FtFont::GlyphMetrics gm;
  if (!f.ft->metricsGlyph26_6(gid, size26_6_, gm)) return false;
  const int32_t adv = fp4::truncate26_6_to_4(gm.advance26_6);
  adv12_4 = static_cast<uint16_t>(adv < 0 ? 0 : adv);
  return true;
}

const EpdGlyph* TtfEpdFont::injectGlyph(const uint8_t style, const uint32_t cp, const uint8_t w, const uint8_t h,
                                        const uint8_t* bmp2bit, const uint16_t bytes, const int32_t adv12_4,
                                        const int16_t left, const int16_t top) {
  if (style >= 4 || !loaded_) return nullptr;
  Face& f = faces_[style];
  // Same face-initialization contract as faultGlyph so the arena is live.
  if (!f.inited || (master_ && !master_->faces_[style].ready)) {
    // Secondary fonts are never variants; still, guard like faultGlyph does.
    if (!f.inited) return nullptr;
  }
  if (!f.ready) return nullptr;
  // Already present (e.g. two secondary tiers raced) → return cached.
  if (findGlyph(f, cp)) return findGlyph(f, cp);

  // Route exactly like faultGlyph: core until coreCount/coreCap exhausted, then tail.
  if (!f.coreFull && (f.coreCount >= f.maxCoreGlyphs || f.coreUsed + bytes > f.coreCap)) f.coreFull = true;
  const bool isTail = f.coreFull || f.coreCap == 0;
  if (isTail && (f.glyphs.size() - f.coreCount >= f.maxTailGlyphs || f.tailUsed + bytes > f.tailCap)) {
    return nullptr;  // secondary glyphs are a sparse supplement; never flush the page cache
  }
  if (!isTail && f.coreUsed + bytes > f.coreCap) return nullptr;

  static constexpr size_t kTableStep = 64;
  const auto growTables = [&]() {
    return reserveChecked(f.glyphs, f.glyphs.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.cps, f.cps.size() + 1, kTableStep, f.maxGlyphs) &&
           reserveChecked(f.slot, f.slot.size() + 1, kTableStep, f.maxGlyphs);
  };
  if (!growTables()) return nullptr;

  auto& bmpVec = isTail ? f.tailBmp : f.coreBmp;
  size_t& bmpUsed = isTail ? f.tailUsed : f.coreUsed;
  const size_t bmpCap = isTail ? f.tailCap : f.coreCap;
  if (bytes > bmpCap || !reserveChecked(bmpVec, bmpUsed + bytes, 4096, bmpCap)) return nullptr;
  if (bmpVec.size() < bmpUsed + bytes) bmpVec.resize(bmpUsed + bytes, 0);
  if (bytes) std::memcpy(bmpVec.data() + bmpUsed, bmp2bit, bytes);

  EpdGlyph eg{};
  eg.width = w;
  eg.height = h;
  eg.advanceX = static_cast<uint16_t>(adv12_4 < 0 ? 0 : adv12_4);
  eg.left = left;
  eg.top = top;
  eg.dataOffset = static_cast<uint32_t>(bmpUsed);
  eg.dataLength = bytes;
  bmpUsed += bytes;
  f.glyphs.push_back(eg);
  if (!isTail) f.coreCount++;
  const uint16_t newIdx = static_cast<uint16_t>(f.glyphs.size() - 1);
  const auto it = std::lower_bound(f.cps.begin(), f.cps.end(), cp);
  const size_t pos = static_cast<size_t>(it - f.cps.begin());
  f.cps.insert(it, cp);
  f.slot.insert(f.slot.begin() + pos, newIdx);
  return &f.glyphs[newIdx];
}
// S3c+ global bitmap-font fallback bridge: fault through the regular face,
// including its secondary (PIF1/SD) chain, and return the arena EpdGlyph.
const EpdGlyph* TtfEpdFont::glyphForGlobal(const uint32_t cp) {
  if (!loaded_) return nullptr;
  Face& f = faces_[0];
  const EpdGlyph* g = faultGlyph(f, cp);
  if (g != nullptr || secondary_ == nullptr) return g;
  // cmap miss — mirror missThunk(): let the secondary source rasterize into
  // this same arena, then re-resolve. Lifetime matches glyphMissHandler's
  // "valid until next fault/flush" contract, so the global font must consume
  // it (measure or draw) before requesting another missed glyph.
  secondary_(secondaryCtx_, cp, 0, *this);
  return findGlyph(f, cp);
}

const uint8_t* TtfEpdFont::bitmapForGlobal(const EpdGlyph* glyph) {
  return bitmapThunk(&faces_[0], glyph);
}

int8_t TtfEpdFont::kernThunk(void* ctx, const uint32_t leftCp, const uint32_t rightCp) {
  Face* f = static_cast<Face*>(ctx);
  return f->owner->faultKern(*f, leftCp, rightCp);
}
const uint8_t* TtfEpdFont::bitmapThunk(void* ctx, const EpdGlyph* glyph) {
  if (glyph == nullptr || glyph->dataLength == 0) return nullptr;
  Face* f = static_cast<Face*>(ctx);
  const EpdGlyph* base = f->glyphs.data();
  const size_t total = f->glyphs.size();
  if (base == nullptr || total == 0) return nullptr;

  const auto glyphAddr = reinterpret_cast<uintptr_t>(glyph);
  const auto baseAddr = reinterpret_cast<uintptr_t>(base);
  const auto endAddr = baseAddr + total * sizeof(EpdGlyph);
  if (glyphAddr < baseAddr || glyphAddr >= endAddr) return nullptr;

  if (glyph >= base + f->coreCount) {
    if (glyph->dataOffset + glyph->dataLength > f->tailBmp.size()) return nullptr;
    return f->tailBmp.data() + glyph->dataOffset;
  }
  if (glyph->dataOffset + glyph->dataLength > f->coreBmp.size()) return nullptr;
  return f->coreBmp.data() + glyph->dataOffset;
}
bool TtfEpdFont::coverageThunk(void* ctx, const uint32_t codepoint) {
  // All styles share one file → coverage comes from the always-live regular face.
  Face& reg = static_cast<Face*>(ctx)->owner->faces_[0];
  if (reg.ft->hasGlyph(codepoint)) return true;
  // Ligature presentation codepoints resolvable through GSUB despite no cmap
  // entry (some EPUBs carry literal U+FB01/U+FB02 in their text).
  return codepoint >= 0xFB00u && codepoint <= 0xFB04u && reg.ligGid[codepoint - 0xFB00u] != 0;
}

EpdFontFamily TtfEpdFont::family() const {
  return EpdFontFamily(pointSize_, &faces_[0].font, &faces_[1].font, &faces_[2].font, &faces_[3].font);
}

bool TtfEpdFont::build(const char* utf8, const uint8_t style) {
  if (!loaded_ || utf8 == nullptr || style >= 4) return false;
  Face& f = faces_[style];
  if (f.miniMetadataOnly) flushFace(f);
  f.flushed = false;
  f.faultFailed = false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != '\0') faultGlyph(f, utf8NextCodepoint(&p), /*allowFlush=*/true);
  // If faultGlyph() flushed the face mid-pass on capacity exhaustion, a second
  // pass faults back the earlier codepoints of this page without flushing again.
  if (f.flushed && !f.miniMetadataOnly && !f.faultFailed) {
    if (addCoverage(utf8, style)) return true;
    if (f.miniMetadataOnly || f.faultFailed) return false;
    flushFace(f);
    return addCoverage(utf8, style);
  }
  return !f.miniMetadataOnly && !f.faultFailed;
}
bool TtfEpdFont::addCoverage(const char* utf8, const uint8_t style) {
  if (!loaded_ || utf8 == nullptr || style >= 4) return false;
  Face& f = faces_[style];
  f.flushed = false;
  f.faultFailed = false;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p != '\0') {
    faultGlyph(f, utf8NextCodepoint(&p), /*allowFlush=*/false);
    if (f.flushed || f.miniMetadataOnly || f.faultFailed) return false;
  }
  return true;
}

void epdFontProfileDump(const char* tag) {
#if defined(ESP_PLATFORM)
  // [EHP-PROF] ASCII line: rasterize cost separated from parse/layout.
  LOG_INF("EHP-PROF", "%s rasterize calls=%u total_ms=%u", tag ? tag : "-", glyphprof::calls, glyphprof::ms);
  glyphprof::calls = 0;
  glyphprof::ms = 0;
#else
  (void)tag;
#endif
}

#endif  // CROSSPOINT_VECTOR_FONTS
