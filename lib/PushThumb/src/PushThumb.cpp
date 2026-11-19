// S3b P4 — see include/PushThumb.h. Pure in-memory 1-bit cover renderer.
#include "PushThumb.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace pushthumb {
namespace {

// Minimal 8x8 ASCII glyphs (MSB = leftmost pixel). Only the characters the
// cover can contain: P U S H, digits, ':', '-', ' ', '/', '.'. All ASCII.
struct Glyph { char c; uint8_t rows[8]; };
static const Glyph kFont[] = {
    {'P', {0x7C, 0x82, 0x82, 0x7C, 0x80, 0x80, 0x80, 0x00}},
    {'U', {0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C, 0x00}},
    {'S', {0x7E, 0x80, 0x80, 0x7E, 0x02, 0x02, 0x7E, 0x00}},
    {'H', {0x82, 0x82, 0x82, 0xFE, 0x82, 0x82, 0x82, 0x00}},
    {'0', {0x7C, 0x82, 0x86, 0x8A, 0x92, 0xA2, 0x7C, 0x00}},
    {'1', {0x10, 0x30, 0x10, 0x10, 0x10, 0x10, 0x38, 0x00}},
    {'2', {0x7C, 0x02, 0x02, 0x7C, 0x80, 0x80, 0x7E, 0x00}},
    {'3', {0x7C, 0x02, 0x02, 0x3C, 0x02, 0x02, 0x7C, 0x00}},
    {'4', {0x82, 0x82, 0x82, 0xFE, 0x02, 0x02, 0x02, 0x00}},
    {'5', {0xFE, 0x80, 0x80, 0x7C, 0x02, 0x02, 0x7C, 0x00}},
    {'6', {0x3C, 0x80, 0x80, 0x7C, 0x82, 0x82, 0x7C, 0x00}},
    {'7', {0xFE, 0x02, 0x04, 0x08, 0x10, 0x10, 0x10, 0x00}},
    {'8', {0x7C, 0x82, 0x82, 0x7C, 0x82, 0x82, 0x7C, 0x00}},
    {'9', {0x7C, 0x82, 0x82, 0x7E, 0x02, 0x02, 0x7C, 0x00}},
    {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}},
    {'/', {0x02, 0x04, 0x04, 0x08, 0x10, 0x10, 0x20, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

const Glyph* glyphFor(char c) {
  for (const auto& g : kFont) if (g.c == c) return &g;
  return glyphFor(' ');
}

// Canvas: 1 bit per pixel, white=1 (background), black=0 (ink).
struct Canvas {
  uint8_t* bits;
  int w, h;
  void fillWhite() { std::memset(bits, 0xFF, static_cast<size_t>((w + 7) / 8) * h); }
  void setBlack(int x, int y) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    bits[y * ((w + 7) / 8) + (x >> 3)] &= static_cast<uint8_t>(~(0x80 >> (x & 7)));
  }
  // Draw an 8x8 glyph scaled by an integer factor, top-left at (x,y).
  void drawGlyph(char c, int x, int y, int scale) {
    const Glyph* g = glyphFor(c);
    for (int r = 0; r < 8; ++r) {
      const uint8_t row = g->rows[r];
      for (int col = 0; col < 8; ++col) {
        if (row & (0x80 >> col)) {
          for (int sy = 0; sy < scale; ++sy)
            for (int sx = 0; sx < scale; ++sx) setBlack(x + col * scale + sx, y + r * scale + sy);
        }
      }
    }
  }
  int textWidth(const char* s, int scale) const { return static_cast<int>(strlen(s)) * 8 * scale; }
  void drawCentered(const char* s, int y, int scale) {
    int cx = (w - textWidth(s, scale)) / 2;
    for (const char* p = s; *p; ++p) { drawGlyph(*p, cx, y, scale); cx += 8 * scale; }
  }
};

void write32le(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
void write16le(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

}  // namespace

bool writeCoverBmp(WriteFn sink, void* ctx, const CoverInfo& info) {
  if (!sink) return false;
  constexpr int W = kThumbWidth, H = kThumbHeight;
  constexpr int rowBytes = (W + 31) / 32 * 4;  // 52
  const size_t pixelsSize = static_cast<size_t>(rowBytes) * H;
  const size_t headerSize = 14 + 40 + 8;       // BITMAPFILEHEADER + INFOHEADER + 2 colors
  const size_t fileSize = headerSize + pixelsSize;

  auto pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pixelsSize]);
  if (!pixels) return false;
  std::memset(pixels.get(), 0, pixelsSize);

  Canvas cv{pixels.get(), W, H};
  cv.fillWhite();

  // Large centered title.
  cv.drawCentered("PUSH", 96, 4);  // 4x -> 32px tall, 128px wide

  // Two small lines: time and byte count.
  char line[48];
  if (info.timeText) {
    std::snprintf(line, sizeof(line), "%s", info.timeText);
    cv.drawCentered(line, 244, 2);  // 16px tall
  }
  std::snprintf(line, sizeof(line), "%zu bytes", info.byteCount);
  cv.drawCentered(line, 288, 2);

  uint8_t header[headerSize];
  std::memset(header, 0, sizeof(header));
  write16le(header + 0, 0x4D42);            // 'BM'
  write32le(header + 2, static_cast<uint32_t>(fileSize));
  write32le(header + 10, static_cast<uint32_t>(headerSize));
  write32le(header + 14, 40);               // biSize
  // biWidth @18, biHeight @22 as int32 LE (positive = BottomUp)
  write32le(header + 18, static_cast<uint32_t>(W));
  write32le(header + 22, static_cast<uint32_t>(H));
  write16le(header + 26, 1);                // biPlanes
  write16le(header + 28, 1);                // biBitCount
  write32le(header + 34, static_cast<uint32_t>(pixelsSize));
  write32le(header + 46, 2);                // biClrUsed
  // color0 = black (all zero), color1 = white
  header[54] = header[55] = header[56] = 0; header[57] = 0;
  header[58] = header[59] = header[60] = 0xFF; header[61] = 0xFF;

  if (sink(ctx, header, headerSize) != headerSize) return false;
  // BottomUp: emit rows from bottom to top of the canvas.
  for (int y = H - 1; y >= 0; --y) {
    const uint8_t* row = pixels.get() + static_cast<size_t>(y) * rowBytes;
    if (sink(ctx, row, rowBytes) != rowBytes) return false;
  }
  return true;
}

}  // namespace pushthumb
