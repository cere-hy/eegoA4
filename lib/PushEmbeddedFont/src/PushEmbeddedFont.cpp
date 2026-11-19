#include "PushEmbeddedFont.h"

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

// PSRAM-held state, installed once for the PUSH font.
uint8_t* g_blockBuf = nullptr;            // 32 * 77 = 2464 bytes
Pif1View g_view;
Pif1Reader* g_reader = nullptr;
TtfEpdFont* g_sdFont = nullptr;           // tier ③ (out-of-GB2312), may be null
Stats g_stats{};
std::atomic<bool> g_unavailable{false};
uint32_t g_cjkSeen = 0;
uint32_t g_cjkMissing = 0;

// P0 stack-overflow fix (S3c): tinfl_decompressor is ~8.3KB. The vendored
// crosspoint_tinfl_decompress_mem_to_mem() declares it as a LOCAL (miniz.c:2971),
// which put an 8.3KB frame on the ActivityManager render task and tripped the
// canary during section-build glyph faults (backtrace ...mem_to_mem -> tinfl).
// Use ONE file-static decompressor instead: PUSH glyph faults only ever run on
// the single render task, so reuse is safe and the state moves off the stack.
static tinfl_decompressor g_inflateState;

size_t inflateBlock(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
  tinfl_init(&g_inflateState);
  size_t outLen = dstCap;
  const tinfl_status status = tinfl_decompress(
      &g_inflateState, src, &srcLen, dst, dst, &outLen,
      TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return status == TINFL_STATUS_DONE ? outLen : 0;
}

// 25/50/75% coverage → 2-bit, identical to TtfEpdFont::faultGlyph quantizer.
inline uint8_t cov2bit(uint8_t a) { return a < 64 ? 0 : a < 128 ? 1 : a < 192 ? 2 : 3; }
inline uint8_t cov1bit(uint8_t a) { return a < 128 ? 0 : 1; }

// Per-codepoint secondary source. Returns true when a glyph was injected into
// `font`'s face arena for `cp`.
bool secondary(void* /*ctx*/, uint32_t cp, uint8_t style, TtfEpdFont& font) {
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
        // P0 stack-overflow fix (S3c): cover[]/packed[] were stack locals
        // (5184B + 1308B) on the same render-task frame, together with the
        // 8.3KB tinfl state they blew the 16KB canary. Make them file-static
        // scratch buffers: PUSH glyph faults are single-task and each call
        // fully overwrites the region it reads back, so reuse is safe.
        static uint8_t cover[72 * 72];
        for (int y = 0; y < h; ++y)
          for (int x = 0; x < w; ++x) cover[y * w + x] = coverage(rec, w, h, x, y);
        const bool twoBit = true;  // PUSH font is always loaded twoBit
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
        // Pico draws at baseline + y0; CrossPoint top = positive-above = -y0.
        const int top = -scaledTop(s8(rec[1]), px);
        // advance is whole pixels → 12.4 fixed point.
        const int32_t adv12 = adv << 4;
        if (font.injectGlyph(style, cp, uint8_t(w), uint8_t(h), packed, uint16_t(n), adv12,
                             int16_t(left), int16_t(top))) {
          ++g_stats.pif1Hits;
          return true;
        }
      }
    }
  }

  // Tier ③: SD full font (LXGW) for GB2312-outside codepoints.
  if (g_sdFont) {
    uint8_t w = 0, h = 0; uint16_t bytes = 0; int32_t adv = 0; int16_t left = 0, top = 0;
    const uint8_t* bmp = g_sdFont->rasterizedBitmap(cp, w, h, bytes, adv, left, top, style < 4 ? style : 0);
    if (bmp && bytes && w <= 72 && h <= 72 && font.injectGlyph(style, cp, w, h, bmp, bytes, adv, left, top)) {
      ++g_stats.sdHits;
      return true;
    }
  }

  // Tier ④: no source has this codepoint. Leave it to EpdFont's replacement glyph
  // (transparent single box), log the codepoint, and track the ratio so a wildly
  // unsupported document can surface the ASCII notice once.
  ++g_stats.misses;
  LOG_INF("PFNT", "glyph miss U+%04X (tiers 1-3 all failed)", (unsigned)cp);
  if (cp >= 0x3400 && cp <= 0x9FFF) {
    ++g_cjkSeen;
    ++g_cjkMissing;
    if (g_cjkSeen >= 20 && g_cjkMissing * 5 > g_cjkSeen) g_unavailable.store(true, std::memory_order_release);
  }
  return false;
}

}  // namespace

void setSdFallback(TtfEpdFont* font) { g_sdFont = font; }

void installInto(TtfEpdFont& pushFont) {
  // Idempotent: allocate the reader once, install handler every time (font may
  // be reloaded on a point-size change).
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
  g_stats = Stats{};
  g_cjkSeen = g_cjkMissing = 0;
  g_unavailable.store(false, std::memory_order_release);
  pushFont.setSecondaryHandler(&secondary, nullptr);
  LOG_INF("PFNT", "installed: pif1=%d sd=%d binChars=%u", g_reader ? 1 : 0, g_sdFont ? 1 : 0,
          g_view.valid() ? g_view.count() : 0u);
}

bool takePushFontUnavailable() { return g_unavailable.exchange(false, std::memory_order_acq_rel); }

Stats stats() { return g_stats; }

}  // namespace pushembedfont
