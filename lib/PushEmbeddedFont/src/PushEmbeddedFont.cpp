#include "PushEmbeddedFont.h"

#include <EpdFontData.h>
#include <Logging.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <MinizConfig.h>  // renamed tinfl API + vendored miniz.h

#include "Pif1Codec.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

namespace pushembedfont {
namespace {

using pushembedfont::Pif1Reader;
using pushembedfont::Pif1View;

// PIF1 decode state: a single shared reader, allocated once. Glyph faults only
// ever run on the single render task (RenderLock serializes build + render), so
// the file-static scratch buffers are safe to reuse and stay OFF the 24KB task
// stack (they were the S3c stack-overflow cause). Tier-③ (SD) is NOT here — it
// lives in each face's FallbackContext so PUSH/FAST cannot clobber each other.
uint8_t* g_blockBuf = nullptr;            // 32 * 77 = 2464 bytes
Pif1View g_view;
Pif1Reader* g_reader = nullptr;
// One-shot CJK-unavailable notice flag, queried by the reader UI. The per-face
// fallback ctx writes through `unavailable`, so PUSH/FAST share this signal.
std::atomic<bool> g_unavailable{false};

// File-static decompressor (~8.3KB) — see S3c P0 stack-overflow fix.
static tinfl_decompressor g_inflateState;

size_t inflateBlock(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
  tinfl_init(&g_inflateState);
  size_t outLen = dstCap;
  const tinfl_status status = tinfl_decompress(
      &g_inflateState, src, &srcLen, dst, dst, &outLen,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE ? outLen : 0;
}

inline uint8_t cov2bit(uint8_t a) { return a < 64 ? 0 : a < 128 ? 1 : a < 192 ? 2 : 3; }
inline uint8_t cov1bit(uint8_t a) { return a < 128 ? 0 : 1; }

// Per-codepoint secondary source. `ctx` is the face's FallbackContext.
// Returns true when a glyph was injected into `font`'s face arena for `cp`.
bool secondary(void* ctx, uint32_t cp, uint8_t style, TtfEpdFont& font) {
  FallbackContext* c = static_cast<FallbackContext*>(ctx);

  // Tier ②: PIF1 Han bitmap.
  if (g_reader) {
    uint8_t rec[kPif1Record];
    if (g_reader->get(cp, rec)) {
      // S3c P1: getSizeInPixels() is an em ppem; PIF1 px is asc-desc height.
      // pif1PxFromEmPx() converts em->asc-desc (x1.448) before the Pico formulas.
      const int px = pif1PxFromEmPx(font.family().getSizeInPixels());
      const int w = scaledW(rec[2], px);
      const int h = scaledW(rec[3], px);
      if (w > 0 && h > 0 && w <= 72 && h <= 72) {
        static uint8_t cover[72 * 72];
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x) cover[y * w + x] = coverage(rec, w, h, x, y);
        const bool twoBit = true;
        static uint8_t packed[72 * 72 / 4 + 4];
        size_t n = 0;
        if (twoBit) {
          std::memset(packed, 0, sizeof(packed));
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
              const uint8_t v = cov2bit(cover[y * w + x]);
              packed[(y * w + x) >> 2] |= v << ((3 - ((y * w + x) & 3)) * 2);
            }
          n = (size_t(w) * h + 3) / 4;
        } else {
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) packed[n++] = cov1bit(cover[y * w + x]) ? 0xFF : 0x00;
        }
        const int adv = scaledAdvance(rec[4], px);
        const int left = scaledLeft(s8(rec[0]), px);
        const int top = -scaledTop(s8(rec[1]), px);
        const int32_t adv12 = adv << 4;
        if (font.injectGlyph(style, cp, uint8_t(w), uint8_t(h), packed, uint16_t(n), adv12,
                             int16_t(left), int16_t(top))) {
          if (c) ++c->stats.pif1Hits;
          return true;
        }
      }
    }
  }

  // Tier ③: SD full font (LXGW) for GB2312-outside codepoints. Per-face pointer.
  if (c && c->sdFont) {
    uint8_t w = 0, h = 0; uint16_t bytes = 0; int32_t adv = 0; int16_t left = 0, top = 0;
    const uint8_t* bmp = c->sdFont->rasterizedBitmap(cp, w, h, bytes, adv, left, top, style < 4 ? style : 0);
    if (bmp && bytes && w <= 72 && h <= 72 && font.injectGlyph(style, cp, w, h, bmp, bytes, adv, left, top)) {
      ++c->stats.sdHits;
      return true;
    }
  }

  // Tier ④: no source has this codepoint. Graceful degradation: leave it to
  // EpdFont's replacement glyph (transparent box) + one ASCII log line. Never
  // fail the page. Track CJK miss ratio so a wildly unsupported doc can show
  // the ASCII notice once.
  if (c) ++c->stats.misses;
  LOG_INF("PFNT", "glyph miss U+%04X (tiers 1-3 all failed; placeholder)", (unsigned)cp);
  if (cp >= 0x3400 && cp <= 0x9FFF) {
    if (c) {
      ++c->cjkSeen;
      ++c->cjkMissing;
      if (c->unavailable && c->cjkSeen >= 20 && c->cjkMissing * 5 > c->cjkSeen)
        static_cast<std::atomic<bool>*>(c->unavailable)->store(true, std::memory_order_release);
    }
  }
  return false;
}

// Advance-only query. Mirrors secondary()'s tier order but touches NO bitmap.
bool advanceOnly(void* ctx, uint32_t cp, uint8_t style, TtfEpdFont& font, uint16_t& adv12_4) {
  FallbackContext* c = static_cast<FallbackContext*>(ctx);
  // Tier ②: PIF1 Han bitmap — advance straight from record[4].
  if (g_reader) {
    const int px = pif1PxFromEmPx(font.family().getSizeInPixels());
    if (pif1AdvanceOnly(*g_reader, cp, px, adv12_4)) return true;
  }
  // Tier ③: SD full font (LXGW) — per-face pointer, its own advance path.
  if (c && c->sdFont && c->sdFont->getAdvance(cp, style < 4 ? style : 0, adv12_4)) return true;
  // Tier ④: no source has an advance for this codepoint.
  return false;
}

}  // namespace

// S3c+ global bitmap-font fallback thunks. ctx is a STABLE holder whose `face`
// is updated whenever the embedded PUSH/FAST face is (re)built on point-size
// change — so global font objects wire their handlers exactly once at startup.
struct EmbeddedFaceHolder {
  TtfEpdFont* face = nullptr;
};
static EmbeddedFaceHolder g_embeddedHolder{};

static const EpdGlyph* globalGlyphMiss(void* ctx, uint32_t cp) {
  TtfEpdFont* embedded = static_cast<EmbeddedFaceHolder*>(ctx)->face;
  return embedded ? embedded->glyphForGlobal(cp) : nullptr;
}
static const uint8_t* globalBitmap(void* ctx, const EpdGlyph* glyph) {
  TtfEpdFont* embedded = static_cast<EmbeddedFaceHolder*>(ctx)->face;
  return embedded ? embedded->bitmapForGlobal(glyph) : nullptr;
}

void setEmbeddedFace(TtfEpdFont* face) { g_embeddedHolder.face = face; }

void installGlobalFallback(EpdFontData& data) {
  // Only wire the miss path; leave all baked metrics/kerning tables untouched.
  // glyphMissCtx is the stable holder, not the face itself, so face rebuilds
  // need no re-wiring of global font objects.
  data.glyphMissHandler = &globalGlyphMiss;
  data.glyphMissCtx = &g_embeddedHolder;
  data.vectorBitmapHandler = &globalBitmap;
  data.coverageHandler = nullptr;  // global Noto Sans intervals are complete;
                                   // no RAM-only coverage index to consult.
}

void setSdFallback(FallbackContext& ctx, TtfEpdFont* font) { ctx.sdFont = font; }

void installInto(TtfEpdFont& pushFont, FallbackContext& ctx) {
  if (!g_reader) {
    const size_t viewSize = uihanzi_bin_end - uihanzi_bin_start;
    g_view = Pif1View{uihanzi_bin_start, viewSize};
    g_blockBuf = static_cast<uint8_t*>(heap_caps_malloc(kPif1Block * kPif1Record, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!g_blockBuf) {
      g_blockBuf = static_cast<uint8_t*>(malloc(kPif1Block * kPif1Record));
    }
    if (g_blockBuf && g_view.valid()) {
      g_reader = new (std::nothrow) Pif1Reader(g_view, g_blockBuf, inflateBlock);
    }
    if (!g_reader) LOG_ERR("PFNT", "PIF1 init failed: bin=%d buf=%d", g_view.valid() ? 1 : 0, g_blockBuf ? 1 : 0);
  }
  ctx.stats = Stats{};
  ctx.cjkSeen = ctx.cjkMissing = 0;
  ctx.unavailable = &g_unavailable;
  g_unavailable.store(false, std::memory_order_release);
  pushFont.setSecondaryHandler(&secondary, &ctx);
  pushFont.setAdvanceHandler(&advanceOnly, &ctx);
  LOG_INF("PFNT", "installed: pif1=%d sd=%d binChars=%u advOnly=1", g_reader ? 1 : 0, ctx.sdFont ? 1 : 0,
          g_view.valid() ? g_view.count() : 0u);
}

// Global bitmap-font fallback is installed by SdCardFontSystem using the public
// bridge methods on TtfEpdFont (glyphForGlobal / bitmapThunk) — no copies of the
// embedded glyph cross module boundaries here. See TtfEpdFont::installGlobalFallback.

bool takePushFontUnavailable() { return g_unavailable.exchange(false, std::memory_order_acq_rel); }

Stats stats() {
  // Diagnostics reflect the last installed face's ctx; SdCardFontSystem owns
  // it, so expose a neutral snapshot here (host tests use the algorithm header).
  return Stats{};
}

}  // namespace pushembedfont
