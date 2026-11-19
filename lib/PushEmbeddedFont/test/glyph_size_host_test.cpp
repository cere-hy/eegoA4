// S3c P1 — host regression for PIF1 bitmap vs vector glyph size/baseline.
// Verifies the em->asc-desc px conversion aligns PIF1-supplement CJK glyphs
// with builtin.ttf vector glyphs in ink height, advance and baseline top.
// Reads stbtt from the FreeInkFont vendored copy and both embedded assets.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "Pif1Codec.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

using pushembedfont::kPif1AscDescPerEm;
using pushembedfont::pif1PxFromEmPx;

static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++fails; std::printf("  RED: %s\n", msg); } } while (0)

static std::vector<unsigned char> readFile(const char* p) {
  FILE* f = std::fopen(p, "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::rewind(f);
  std::vector<unsigned char> b(n); std::fread(b.data(), 1, n, f); std::fclose(f);
  return b;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::printf("usage: glyph_size_host_test <builtin.ttf> <ui-hanzi.bin>\n"); return 2; }
  auto ttf = readFile(argv[1]);
  auto bin = readFile(argv[2]);
  CHECK(!ttf.empty(), "builtin.ttf loaded");
  CHECK(!bin.empty(), "ui-hanzi.bin loaded");

  // Real PIF1 records for CJK supplement chars (decoded from bin) at base-24,
  // fields: x0 y0 w h advance. These are chars NOT in builtin.ttf -> tier 2.
  struct Rec { unsigned cp; int x0, y0, w, h, adv; };
  const Rec cjk[] = {
    {0x4E07, 0,-13,16,15,17},  // 万
    {0x4EC1, 0,-14,16,16,17},  // 仁
    {0x5242, 0,-15,16,17,17},  // 剂
  };

  stbtt_fontinfo fi;
  CHECK(stbtt_InitFont(&fi, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(),0)) != 0, "stbtt init builtin.ttf");
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
  // hhea-derived ratio must match the shared constant (guards against font swap).
  CHECK(std::fabs((float)(asc - desc) / 1000.0f - kPif1AscDescPerEm) < 0.01f,
        "hhea (asc-desc)/upem matches kPif1AscDescPerEm=1.448");

  const float ppem = 14.0f * 150.0f / 72.0f;      // pt=14 -> em ppem
  const float svec = stbtt_ScaleForMappingEmToPixels(&fi, ppem);
  const int px = pif1PxFromEmPx(ppem);             // converter under test
  CHECK(px == 42, "pif1PxFromEmPx(29.17)=42 (x1.448)");

  // Vector CJK baseline: use 国/中/大/字 which ARE in builtin.ttf.
  const unsigned vecCJK[] = {0x56FD, 0x4E2D, 0x5927, 0x5B57};
  int vhSum=0, vadvSum=0, vtopSum=0, vn=0;
  for (unsigned cp : vecCJK) {
    int gi = stbtt_FindGlyphIndex(&fi, cp);
    if (!gi) continue;
    int x0,y0,x1,y1,advu,lsb;
    stbtt_GetGlyphBitmapBox(&fi,gi,svec,svec,&x0,&y0,&x1,&y1);
    stbtt_GetGlyphHMetrics(&fi,gi,&advu,&lsb);
    vhSum += (y1-y0); vadvSum += (int)(advu*svec+0.5f); vtopSum += (-y0); ++vn;
  }
  CHECK(vn == 4, "4 vector CJK refs present");
  float vh = float(vhSum)/vn, vadv = float(vadvSum)/vn, vtop = float(vtopSum)/vn;

  // PIF1 scaling = device Pif1Codec formulas.
  auto floorDiv = [](int a,int b){int q=a/b,r=a%b; if(r&&((r<0)!=(b<0)))--q; return q;};
  auto sW = [&](int w24){ return (w24*px+23)/24; };
  auto sAdv = [&](int a24){ return (a24*px+12)/24; };
  auto sTop = [&](int y0){ return floorDiv(y0*px,24); };

  std::printf("\n  pt=14  em ppem=%.3f  PIF1 px=%d  vector CJK h=%.1f adv=%.1f top=%.1f\n", ppem, px, vh, vadv, vtop);
  std::printf("  %-6s %-6s %-6s %-6s %-6s\n", "char", "hB/hV", "adv", "top", "dTop");
  for (const Rec& r : cjk) {
    int bh = sW(r.h), bw = sW(r.w), badv = sAdv(r.adv);
    int btop = -sTop(r.y0);  // device stores top=-scaledTop(y0); renderer draws at cursorY-top
    float k = float(bh)/vh;
    int dTop = (int)std::lround(vtop - btop);
    std::printf("  U+%04X  %.3f  %2d/%-2d  %2d  %+d\n", r.cp, k, badv, (int)vadv, btop, dTop);
    CHECK(k >= 0.88f && k <= 1.12f, "PIF1 ink height k within ±12% of vector");
    CHECK(std::abs(badv - (int)std::lround(vadv)) <= 2, "PIF1 advance within ±2px of vector");
    CHECK(std::abs(dTop) <= 3, "PIF1 baseline top within ±3px of vector");
    (void)bw;
  }

  // Old (buggy) behaviour: feeding em ppem straight in would give k≈0.70 -> guard the fix.
  int pxOld = (int)(ppem + 0.5f);
  int bhOld = (cjk[0].h * pxOld + 23)/24;
  CHECK(float(bhOld)/vh < 0.85f, "old em-as-px path reproduces the mismatch (k<0.85)");

  std::printf("\n  %d checks, %d RED\n", checks, fails);
  return fails ? 1 : 0;
}
