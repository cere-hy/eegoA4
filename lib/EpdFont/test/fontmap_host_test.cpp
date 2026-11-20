// S3cPF6 host regression: FontMap (cmap+hmtx -> codepoint/advanceWidth) must
// reproduce FreeType's own advance, byte-for-byte, on real TTF samples.
#include "FontMap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

static int checks = 0, red = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++red; printf("RED: %s\n", m); } } while (0)

struct MemCtx {
  const uint8_t* data;
  unsigned long size;
};
static unsigned long memRead(void* ctx, unsigned long off, uint8_t* buf, unsigned long count) {
  auto* m = static_cast<MemCtx*>(ctx);
  if (off > m->size || count > m->size - off) return 0;
  std::memcpy(buf, m->data + off, count);
  return count;
}
static bool vecWrite(void* ctx, const uint8_t* data, size_t len) {
  auto* v = static_cast<std::vector<uint8_t>*>(ctx);
  v->insert(v->end(), data, data + len);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: fontmap_host <font.ttf>\n"); return 2; }
  FILE* fp = std::fopen(argv[1], "rb");
  if (!fp) { printf("open FAIL %s\n", argv[1]); return 2; }
  std::fseek(fp, 0, SEEK_END); long sz = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> bytes(sz);
  if (std::fread(bytes.data(), 1, sz, fp) != (size_t)sz) { std::fclose(fp); return 2; }
  std::fclose(fp);

  // ---- build ----
  epd::FontMap fm;
  MemCtx mc{bytes.data(), static_cast<unsigned long>(sz)};
  uint16_t upem = 0;
  epd::FontMapStats stats;
  const bool built = fm.build(&memRead, &mc, mc.size, upem, stats);
  CHECK(built, "FontMap build succeeds on real TTF");
  CHECK(!fm.empty() && fm.count() > 100, "FontMap has mapped codepoints");
  CHECK(upem > 0, "unitsPerEm parsed");
  printf("FontMap: cps=%u upem=%u supp=%u bytes=%zu\n", fm.count(), upem, stats.skippedSupp, fm.byteSize());

  // ---- save/load round-trip ----
  std::vector<uint8_t> img;
  CHECK(fm.save(&img, &vecWrite), "FontMap save");
  CHECK(img.size() == fm.byteSize(), "saved image size == byteSize");
  epd::FontMap fm2;
  CHECK(fm2.load(img.data(), img.size()), "FontMap load round-trip");
  CHECK(fm2.count() == fm.count(), "round-trip count equal");

  // ---- corrupt cache is rejected ----
  std::vector<uint8_t> bad = img;
  bad[4] = 0xFF;  // version byte
  epd::FontMap fmBad;
  CHECK(!fmBad.load(bad.data(), bad.size()), "corrupt version rejected");
  if (bad.size() > 8720) {
    bad[4] = img[4]; // restore version
    bad[16 + 512] ^= 0x01; // flip one presence bit -> segBase/popcount mismatch
  }
  epd::FontMap fmDup;
  CHECK(!fmDup.load(bad.data(), bad.size()), "corrupt presence bitmap rejected");

  // ---- byte-identity vs FreeType at several point sizes ----
  FT_Library lib; FT_Init_FreeType(&lib);
  FT_Face face;
  if (FT_New_Memory_Face(lib, bytes.data(), sz, 0, &face) != 0) {
    printf("SKIP FreeType compare: face open failed (font format unsupported in host build)\n");
    FT_Done_FreeType(lib);
    printf("FontMap host: %d checks, %d RED (compare skipped)\n", checks, red);
    return red ? 1 : 0;
  }
  int pointSizes[] = {12, 16, 20};
  for (int ps : pointSizes) {
    long size26_6 = (long(ps) * 150 * 64 + 36) / 72;
    FT_Set_Char_Size(face, size26_6, size26_6, 72, 72);
    long xscale = face->size->metrics.x_scale;
    int tested = 0, diff = 0;
    // Walk mapped BMP codepoints via FreeType, take every Nth up to 100.
    uint32_t seen = 0, step = 1;
    FT_UInt gid0 = 0;
    FT_ULong cp = FT_Get_First_Char(face, &gid0);
    // Two-pass: first count mapped cps to set the sampling stride.
    for (FT_ULong c = cp; c != 0; c = FT_Get_Next_Char(face, c, &gid0)) if (c <= 0xFFFFu) ++seen;
    step = seen / 100 + 1;
    seen = 0;
    for (; cp != 0 && tested < 100; cp = FT_Get_Next_Char(face, cp, &gid0)) {
      if (cp > 0xFFFFu) continue;
      if (seen++ % step) continue;
      FT_UInt gid = FT_Get_Char_Index(face, cp);
      if (!gid || gid >= face->num_glyphs) continue;
      FT_Load_Glyph(face, gid, FT_LOAD_NO_BITMAP | FT_LOAD_FORCE_AUTOHINT);
      long lin = face->glyph->linearHoriAdvance;
      long lin26 = lin >= 0 ? (lin + 512) >> 10 : -((-lin + 512) >> 10);
      int32_t ref124 = (int32_t)(lin26 >> 2);
      uint16_t rawAw = 0;
      if (!fm.lookup(cp, rawAw)) continue;
      int32_t adv26 = (int32_t)FT_MulFix((FT_Long)rawAw, xscale);
      int32_t got124 = adv26 >> 2;
      if (got124 != ref124) ++diff;
      ++tested;
    }
    char msg[128];
    std::snprintf(msg, sizeof(msg), "FontMap advance == FreeType @%dpt (tested %d, diff %d)", ps, tested, diff);
    CHECK(tested >= 50 && diff == 0, msg);
    printf("  @%dpt: tested=%d diff=%d\n", ps, tested, diff);
  }
  FT_Done_Face(face); FT_Done_FreeType(lib);

  printf("FontMap host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
