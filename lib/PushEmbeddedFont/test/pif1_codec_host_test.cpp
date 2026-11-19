/* S3c host test — PIF1 decode + coverage interpolation, no ESP/PSRAM/miniz. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <zlib.h>

#include "Pif1Codec.h"

using namespace pushembedfont;

static int checks = 0, red = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++red; printf("RED: %s\n", m); } } while (0)

static std::string g_bin;
static size_t inflateZlib(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap) {
  uLongf n = static_cast<uLongf>(dstCap);
  if (uncompress(dst, &n, src, static_cast<uLong>(srcLen)) != Z_OK) return 0;
  return static_cast<size_t>(n);
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "assets/fonts/ui-hanzi.bin";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("RED: cannot open %s\n", path); return 2; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  g_bin.resize(static_cast<size_t>(sz));
  if (fread(&g_bin[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) { fclose(f); return 2; }
  fclose(f);

  Pif1View view{reinterpret_cast<const uint8_t*>(g_bin.data()), g_bin.size()};
  CHECK(view.valid(), "PIF1 header valid");
  CHECK(view.count() >= 5650, "count >= 5650 after punctuation augment");
  CHECK(view.has(0x4E07), "has U+4E07 (wan, bitmap-only)");
  CHECK(view.has(0x300A), "has U+300A (book title open)");
  CHECK(view.has(0x300B), "has U+300B (book title close)");
  CHECK(view.has(0x3010), "has U+3010 (lenticular open)");
  CHECK(!view.has(0x4E00 + 0x100000), "absent cp rejected");

  uint8_t blockBuf[kPif1Block * kPif1Record];
  Pif1Reader reader(view, blockBuf, inflateZlib);

  uint8_t rec[kPif1Record];
  CHECK(reader.get(0x4E07, rec), "decode record U+4E07");
  CHECK(rec[2] > 0 && rec[2] <= 24 && rec[3] > 0 && rec[3] <= 24, "record dims sane");
  CHECK(reader.get(0x300A, rec), "decode record U+300A");
  CHECK(rec[2] > 0 && rec[3] > 0, "punct record dims sane");
  CHECK(reader.get(0x4EC1, rec), "decode record U+4EC1 (block2 cache miss)");
  CHECK(reader.get(0x5242, rec), "decode record U+5242 (block10 cache miss)");
  CHECK(reader.get(0x4E07, rec), "decode record U+4E07 again (block0 re-cache)");

  // S3c final — advance-only query for layout: must equal the draw-time injected
  // advance byte-for-byte (scaledAdvance(rec[4], px) << 4), for every tier-2 char.
  {
    const int pxTest = 42;  // pt14 -> em ppem 29.17 -> asc-desc px 42
    const unsigned advChars[] = {0x4E07, 0x4EC1, 0x5242, 0x300A};
    for (unsigned cp : advChars) {
      uint8_t r[kPif1Record];
      CHECK(reader.get(cp, r), "adv-only: record readable");
      uint16_t aOnly = 0;
      CHECK(pif1AdvanceOnly(reader, cp, pxTest, aOnly), "adv-only: query succeeds");
      CHECK(aOnly == static_cast<uint16_t>(scaledAdvance(r[4], pxTest) << 4),
            "adv-only: matches draw-time injected advance (record[4])");
    }
    uint16_t dummy = 0;
    CHECK(!pif1AdvanceOnly(reader, 0x4E00, pxTest, dummy), "adv-only: absent cp fails");
  }

  // Coverage interpolation at 24px must reproduce the source bitmap exactly.
  uint8_t rec24[kPif1Record];
  CHECK(reader.get(0x4E07, rec24), "re-read U+4E07 for coverage");
  int w24 = rec24[2], h24 = rec24[3];
  int mismatch = 0;
  for (int y = 0; y < h24; ++y)
    for (int x = 0; x < w24; ++x) {
      uint8_t cov = coverage(rec24, w24, h24, x, y);
      unsigned b = pifBit(rec24, x, y);
      if ((cov >= 128) != bool(b)) ++mismatch;
    }
  CHECK(mismatch == 0, "24px coverage threshold reproduces source bitmap");

  // Coverage at smaller size stays in range and produces some ink.
  int w16 = scaledW(rec24[2], 16), h16 = scaledW(rec24[3], 16);
  int ink = 0;
  for (int y = 0; y < h16; ++y) {
    for (int x = 0; x < w16; ++x) {
      if (coverage(rec24, w16, h16, x, y) >= 128) ++ink;
    }
  }
  CHECK(ink > 0, "16px downscaled glyph has ink");
  CHECK(ink > 0, "16px glyph has ink");

  // Scaling formulas match Pico semantics.
  CHECK(scaledAdvance(24, 24) == 24, "advance 24px identity");
  CHECK(scaledAdvance(24, 12) == 12, "advance 12px half");
  CHECK(scaledLeft(-13, 14) == -8, "floor negative x0 @14px (-7.58 -> -8)");
  CHECK(scaledW(24, 14) == 14, "ceil width @14px");

  // S3c merged-order P1 — edge-preserving coverage.
  // (a) Edge protection must NOT change the exact 24px source reproduction:
  //     at 1:1 the sample sits on a pixel centre, so mx-mn never crosses the
  //     boundary threshold and the bilinear path yields the source bit.
  {
    int bad = 0;
    for (int y = 0; y < h24; ++y)
      for (int x = 0; x < w24; ++x) {
        uint8_t cov = coverage(rec24, w24, h24, x, y);
        if ((cov >= 128) != bool(pifBit(rec24, x, y))) ++bad;
      }
    CHECK(bad == 0, "edge coverage still reproduces source bitmap at 24px");
  }
  // (b) At device px=42, edge coverage returns only 0 or 255 (no gray smear):
  //     every sample is either interior (all taps agree -> exact bilinear,
  //     which for a 1-bit source is 0/255) or a boundary (nearest -> 0/255).
  //     A plain bilinear interpolator would produce many 1..254 mid values.
  {
    const int px = 42;
    int w = scaledW(rec24[2], px), h = scaledW(rec24[3], px), mid = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint8_t cov = coverage(rec24, w, h, x, y);
        if (cov != 0 && cov != 255) ++mid;
      }
    CHECK(mid == 0, "edge coverage at px=42 has zero mid-gray (no ink-blob smear)");
  }

  printf("Pif1Codec host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
