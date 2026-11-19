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
  CHECK(nseg == 3, "P2: exactly 3 ink bands (PUSH / time / bytes; no mid duplicate)");
  if (nseg == 3) {
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
    // Large PUSH band ~48px tall, near top (~y100).
    CHECK((segY1[0] - segY0[0] + 1) >= 40, "P2: large PUSH band ~48px tall (vector raster)");
    // Time band must be fully inside (not clipped at x=0/239 as the old bug did).
    int tbx0 = pushthumb::kThumbWidth, tbx1 = -1;
    for (int cy = segY0[1]; cy <= segY1[1]; ++cy)
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

  printf("PushThumb host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
