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

  printf("PushThumb host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
