/* S3b P4 host test — no Arduino/PSRAM, just assert the BMP is well-formed. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "PushThumb.h"

using namespace pushthumb;

static uint8_t g_buf[256 * 1024];
static size_t g_len;
static size_t sink(void* ctx, const uint8_t* data, size_t len) {
  (void)ctx;
  if (g_len + len > sizeof(g_buf)) return 0;
  memcpy(g_buf + g_len, data, len);
  g_len += len;
  return len;
}
static uint32_t rd32(int off) {
  return (uint32_t)g_buf[off] | ((uint32_t)g_buf[off+1]<<8) |
         ((uint32_t)g_buf[off+2]<<16) | ((uint32_t)g_buf[off+3]<<24);
}
static int rd16(int off) { return g_buf[off] | (g_buf[off+1]<<8); }

static int checks = 0, red = 0;
#define CHECK(cond, msg) do { ++checks; if (!(cond)) { ++red; printf("RED: %s\n", msg); } } while (0)

/* ---- S3cPF5: ink-pixel collector for renderCoverInRect() ---------------- */
static uint8_t g_ink[600 * 600];  // absolute-coordinate ink grid
static int g_inkX, g_inkY;  // rect origin (x used to localise; y kept for symmetry)
static int iMinX, iMinY, iMaxX, iMaxY, iCount;
static void inkReset(int ox, int oy) {
  memset(g_ink, 0, sizeof(g_ink));
  g_inkX = ox; g_inkY = oy;
  iMinX = iMinY = 1 << 30; iMaxX = iMaxY = -1; iCount = 0;
}
static void inkCb(void* ctx, int x, int y, bool state) {
  (void)ctx;
  if (!state) return;
  if (x < 0 || x >= 600 || y < 0 || y >= 600) return;
  g_ink[y * 600 + x] = 1;
  ++iCount;
  if (x < iMinX) iMinX = x;
  if (x > iMaxX) iMaxX = x;
  if (y < iMinY) iMinY = y;
  if (y > iMaxY) iMaxY = y;
}

int main(void) {
  CoverInfo info = {"2026-11-19 12:34", 2834};
  g_len = 0;
  CHECK(writeCoverBmp(sink, NULL, info), "writeCoverBmp returns true");
  const int rowBytes = (pushthumb::kThumbWidth + 31) / 32 * 4;
  const size_t expected = 14 + 40 + 8 + (size_t)rowBytes * pushthumb::kThumbHeight;
  CHECK(g_len == expected, "file size matches 240x400 1-bit BMP");
  CHECK(rd16(0) == 0x4D42, "BM magic");
  CHECK(rd32(2) == (uint32_t)g_len, "bfSize field");
  CHECK(rd32(10) == 14+40+8, "pixel offset");
  CHECK(rd32(18) == pushthumb::kThumbWidth, "width=240");
  CHECK(rd32(22) == pushthumb::kThumbHeight, "height=400 (BottomUp positive)");
  CHECK(rd16(26) == 1, "planes=1");
  CHECK(rd16(28) == 1, "1-bit");
  CHECK(g_buf[54]==0 && g_buf[58]==0xFF, "palette color0=black color1=white");

  /* Background must be mostly white: sample a corner pixel row region. */
  const uint8_t* firstRow = g_buf + (14+40+8) + (size_t)rowBytes * (pushthumb::kThumbHeight - 1);
  int whiteBits = 0;
  for (int i = 0; i < rowBytes; ++i) {
    uint8_t b = firstRow[i];
    for (int k = 0; k < 8; ++k) whiteBits += (b >> (7-k)) & 1;
  }
  CHECK(whiteBits > rowBytes * 8 - 8, "top row is white background");

  /* S3c P3: every row's 2-byte padding (240px=30B -> stride 32B) must be white
     (0xFF). The old bug left these at 0x00 (black), producing horizontal stripes.
     Also assert rows are not shifted: padding columns must be identical on all
     rows, and a solid-white row region has no unexpected black bits. */
  const int pixOff = 14 + 40 + 8;
  const uint8_t* rows = g_buf + pixOff;
  int padBlack = 0;
  for (int r = 0; r < pushthumb::kThumbHeight; ++r) {
    const uint8_t* row = rows + (size_t)r * rowBytes;
    if (row[30] != 0xFF || row[31] != 0xFF) ++padBlack;
  }
  CHECK(padBlack == 0, "S3c: all 400 rows have white padding (no black stripes)");

  /* Stride proof: rendered ink columns stay within the 240 visible bytes; the
     padding bytes are never written black even on the rows carrying text. */
  int textRowPadBlack = 0;
  for (int r = 96; r < 320; ++r) {  // band containing PUSH + time + bytes text
    const uint8_t* row = rows + (size_t)r * rowBytes;
    if (row[30] != 0xFF || row[31] != 0xFF) ++textRowPadBlack;
  }
  CHECK(textRowPadBlack == 0, "S3c: text rows keep white padding (no per-row shift)");

  /* Different byte counts must change pixels (cover reflects content). */
  uint8_t snapshot[256*1024]; size_t snapLen;
  memcpy(snapshot, g_buf, g_len); snapLen = g_len;
  g_len = 0;
  CoverInfo info2 = {"2026-11-19 12:34", 5720};
  CHECK(writeCoverBmp(sink, NULL, info2), "second render ok");
  CHECK(g_len == snapLen, "same size");
  int diff = 0;
  for (size_t i = 0; i < g_len; ++i) if (g_buf[i] != snapshot[i]) ++diff;
  CHECK(diff > 0, "byte count changes cover pixels");

  /* ASCII-only source strings used are all in glyph set (they render w/o crash). */
  CoverInfo z = {"2026-11-19 00:00", 0};
  CHECK(writeCoverBmp(sink, NULL, z), "zero bytes ok");
  CoverInfo nt = {NULL, 999999};
  CHECK(writeCoverBmp(sink, NULL, nt), "null time string ok");

  /* ---- S3c merged-order P2: from-scratch cover redraw ---- */
  g_len = 0;
  CoverInfo spec = {"2026-11-19 08:58", 4458};
  CHECK(writeCoverBmp(sink, NULL, spec), "P2 render ok");
  const int PO = 14 + 40 + 8;  // pixel offset
  const uint8_t* prows = g_buf + PO;
  auto isInk = [&](int x, int cy) {  // canvas top-down y -> BottomUp file row
    const uint8_t b = prows[(pushthumb::kThumbHeight - 1 - cy) * rowBytes + (x >> 3)];
    return !((b >> (7 - (x & 7))) & 1);
  };
  // White coverage > 90%.
  long white = 0;
  for (int cy = 0; cy < pushthumb::kThumbHeight; ++cy)
    for (int x = 0; x < pushthumb::kThumbWidth; ++x)
      if (!isInk(x, cy)) ++white;
  CHECK(white * 100 > (long)pushthumb::kThumbWidth * pushthumb::kThumbHeight * 90,
        "P2: white background coverage > 90%");

  // Find ink row segments (canvas top-down), merge gaps <= 3px.
  int segY0[8], segY1[8], nseg = 0;
  for (int cy = 0; cy < pushthumb::kThumbHeight; ++cy) {
    bool any = false;
    for (int x = 0; x < pushthumb::kThumbWidth; ++x) if (isInk(x, cy)) { any = true; break; }
    if (!any) continue;
    if (nseg && cy - segY1[nseg-1] <= 3) segY1[nseg-1] = cy;
    else if (nseg < 8) { segY0[nseg] = segY1[nseg] = cy; ++nseg; }
  }
  CHECK(nseg == 3, "P2: exactly 3 ink bands (PUSH / bytes / time; no mid duplicate)");
  if (nseg == 3) {
    // Top-down order after 甲方案: [0] PUSH y100..147, [1] bytes y252..265,
    // [2] time y290..296. All three horizontally centred on x120.
    for (int s = 0; s < nseg; ++s) {
      int bx0 = pushthumb::kThumbWidth, bx1 = -1;
      for (int cy = segY0[s]; cy <= segY1[s]; ++cy)
        for (int x = 0; x < pushthumb::kThumbWidth; ++x)
          if (isInk(x, cy)) { if (x < bx0) bx0 = x; if (x > bx1) bx1 = x; }
      CHECK(bx0 >= 8, "P2: band left margin >= 8");
      CHECK(bx1 <= 232, "P2: band right margin <= 232 (right edge)");
      const int cx2 = bx0 + bx1;  // 2*centre; expect ~240
      CHECK(cx2 >= 232 && cx2 <= 248, "P2: band horizontally centred on 120 (+/-4)");
    }
    // Large PUSH band ~48px tall, near top (~y100) — UNCHANGED.
    CHECK((segY1[0] - segY0[0] + 1) >= 40, "P2: large PUSH band ~48px tall (vector raster)");
    CHECK(segY0[0] >= 96 && segY1[0] <= 150, "P2-final: large PUSH stays y100..147");
    // Byte-count band UNCHANGED at y252..265.
    CHECK(segY0[1] >= 248 && segY1[1] <= 268, "P2-final: byte-count band stays y252..265");
    // Time band MOVED below bytes: y290..300 (no longer y200..206 under title box).
    CHECK(segY0[2] >= 286 && segY0[2] <= 302, "P2-final: time band moved to y290..300");
    CHECK(segY1[2] <= 320, "P2-final: time band within y320 ceiling (clear of button)");
    CHECK(segY0[2] - segY1[1] >= 20, "P2-final: time band >=20px below byte-count band");
    CHECK(segY0[2] > 222, "P2-final: time band clear of theme title box (bottom 222)");
    // Time band fully inside (not clipped at x=0/239 as the old bug did).
    int tbx0 = pushthumb::kThumbWidth, tbx1 = -1;
    for (int cy = segY0[2]; cy <= segY1[2]; ++cy)
      for (int x = 0; x < pushthumb::kThumbWidth; ++x)
        if (isInk(x, cy)) { if (x < tbx0) tbx0 = x; if (x > tbx1) tbx1 = x; }
    CHECK(tbx0 > 8 && tbx1 < 232, "P2: time line fully visible, no edge clipping");
  }
  // Byte count carries a unit: rendering a much larger byte count must change
  // the pixels (the numeric part is present), and " bytes" suffix glyph set exists.
  {
    uint8_t snap[256*1024]; size_t sl;
    memcpy(snap, g_buf, g_len); sl = g_len;
    g_len = 0;
    writeCoverBmp(sink, NULL, (CoverInfo){"2026-11-19 08:58", 12345});
    int diff = 0; for (size_t i = 0; i < sl; ++i) if (g_buf[i] != snap[i]) ++diff;
    CHECK(diff > 0, "P2: byte count value reflected in cover pixels");
    CHECK(sl == g_len, "P2: byte count change keeps BMP size");
  }

  /* ---- S3cPF5: rect-driven vector cover (no BMP, per-theme rects) ---- */
  {
    CoverInfo pf5 = {"2026-11-19 08:58", 4458};

    // (a) Classic 240x400 via rect renderer must equal the legacy BMP ink.
    // Re-render 4458 into g_buf (an earlier check overwrote it with 12345).
    g_len = 0;
    CHECK(writeCoverBmp(sink, NULL, pf5), "PF5: re-render legacy 4458");
    auto legacyInk = [&](int cx, int cy) {
      const uint8_t b = prows[(pushthumb::kThumbHeight - 1 - cy) * rowBytes + (cx >> 3)];
      return !((b >> (7 - (cx & 7))) & 1);
    };
    inkReset(0, 0);
    bool drewTime = renderCoverInRect(nullptr, inkCb, 0, 0, 240, 400, pf5);
    CHECK(drewTime, "PF5: classic 240x400 draws time line");
    int mismatch = 0, extra = 0;
    for (int cy = 0; cy < 400; ++cy)
      for (int cx = 0; cx < 240; ++cx) {
        const bool a = legacyInk(cx, cy) != 0;
        const bool b = g_ink[cy * 600 + cx] != 0;
        if (a != b) ++mismatch;
        if (!a && b) ++extra;
      }
    CHECK(mismatch == 0, "PF5: rect renderer is byte-identical to legacy 240x400 BMP");
    (void)extra;

    // (b) Per-theme rects: all ink must stay inside the card, be centred, and
    //     never clipped at an edge. Origins are non-zero to test translation.
    struct Rect { int w, h; const char* name; };
    const Rect rects[] = {
        {240, 226, "Lyra"}, {214, 226, "Lyra3Covers"},
        {240, 300, "RoundedRaff"}, {113, 170, "grid-small"},
        {150, 150, "min-card"},
    };
    for (const Rect& r : rects) {
      const int ox = 37, oy = 53;  // arbitrary card placement
      inkReset(ox, oy);
      renderCoverInRect(nullptr, inkCb, ox, oy, r.w, r.h, pf5);
      CHECK(iCount > 0, "PF5: ink present on card");
      CHECK(iMinX >= ox && iMaxX < ox + r.w, "PF5: ink inside card horizontally");
      CHECK(iMinY >= oy && iMaxY < oy + r.h, "PF5: ink inside card vertically (no clip)");
      const int lx0 = iMinX - ox, lx1 = iMaxX - ox;
      const int cx2 = lx0 + lx1;  // 2*local-centre
      CHECK(cx2 >= r.w - 8 && cx2 <= r.w + 8, "PF5: art centred on card midline (+/-4px)");
      // Large word must not be cut off at left/right edges.
      CHECK(lx0 >= 2 && lx1 <= r.w - 3, "PF5: no horizontal word clipping");
    }

    // (c) Small-card legibility policy: time line omitted on very small cards
    //     (w<120 && h<150) so nothing is clipped; byte count always present.
    inkReset(0, 0);
    bool tinyTime = renderCoverInRect(nullptr, inkCb, 0, 0, 100, 140, pf5);
    CHECK(!tinyTime, "PF5: time line omitted on tiny 100x140 card");
    CHECK(iCount > 0, "PF5: tiny card still shows large PUSH + bytes");

    // (d) Minimum card 150x150: large PUSH glyph height must be >= 14px.
    inkReset(0, 0);
    renderCoverInRect(nullptr, inkCb, 0, 0, 150, 150, pf5);
    // Large-word band is the top ink segment; measure rows above byte band.
    int topMinY = iMinY, topMaxY = iMinY;
    for (int cy = iMinY; cy <= iMaxY; ++cy) {
      bool any = false;
      for (int cx = 0; cx < 150; ++cx) if (g_ink[cy * 600 + cx]) { any = true; break; }
      if (any) topMaxY = cy;
      // stop at first blank gap after the word
      if (!any && cy > topMinY + 4) break;
    }
    CHECK((topMaxY - topMinY + 1) >= 14, "PF5: 150x150 large PUSH height >= 14px (readable)");
  }

  printf("PushThumb host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
