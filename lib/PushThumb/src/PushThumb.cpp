// S3c merged-order P2 — from-scratch cover redraw. Pure in-memory 1-bit cover
// renderer (no ESP/PSRAM/font-system dependency). See include/PushThumb.h.
#include "PushThumb.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace pushthumb {
namespace {

// Canvas: 1 bit per pixel, white=1 (background), black=0 (ink).
// Row stride equals the BMP file stride (4-byte aligned): (240+31)/32*4 = 32 B.
// fillWhite covers all stride*H bytes (padding included, white); setBlack indexes
// y*stride+x/8. BottomUp BMP output emits rows H-1..0.
struct Canvas {
  uint8_t* bits;
  int w, h, stride;
  void setWhite(int x, int y) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    bits[y * stride + (x >> 3)] |= static_cast<uint8_t>(0x80 >> (x & 7));
  }
  void setBlack(int x, int y) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    bits[y * stride + (x >> 3)] &= static_cast<uint8_t>(~(0x80 >> (x & 7)));
  }
  void drawInk(int x, int y, uint8_t cov) {  // cov 0..255; >=128 -> black
    if (cov >= 128) setBlack(x, y); else setWhite(x, y);
  }
  void clear() { std::memset(bits, 0xFF, static_cast<size_t>(stride) * h); }  // white background incl. padding
};

// ---- Small text: 8x8 ASCII glyphs (MSB = leftmost). All cover strings ASCII. ----
struct Glyph8 { char c; uint8_t rows[8]; };
static const Glyph8 kFont8[] = {
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
    {'b', {0x80, 0x80, 0x80, 0x7C, 0x82, 0x82, 0x7C, 0x00}},
    {'y', {0x00, 0x82, 0x82, 0x7E, 0x02, 0x02, 0x7C, 0x00}},
    {'t', {0x10, 0x10, 0x7C, 0x10, 0x10, 0x10, 0x0C, 0x00}},
    {'e', {0x00, 0x7C, 0x82, 0xFE, 0x80, 0x82, 0x7C, 0x00}},
    {'s', {0x00, 0x7E, 0x80, 0x7C, 0x02, 0x02, 0x7C, 0x00}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};
const Glyph8* glyph8(char c) {
  for (const auto& g : kFont8) if (g.c == c) return &g;
  return glyph8(' ');
}
constexpr int kUsableW = 208;   // card 240 - 2*16 margin
constexpr int kCenterX = 120;   // card midline

// Exact 8x8 width at integer scale (advance = 8*scale per char).
int textWidth8(const char* s, int scale) {
  if (!s) return 0;
  return static_cast<int>(std::strlen(s)) * 8 * scale;
}
// Largest integer scale in [1..3] whose width fits kUsableW; 0 = no fit.
int fitScale8(const char* s) {
  for (int scale = 3; scale >= 1; --scale)
    if (textWidth8(s, scale) <= kUsableW) return scale;
  return 0;
}
// Draw 8x8 text horizontally centered at kCenterX. Returns ink bbox in/out.
struct BBox { int x, y, w, h; };
BBox drawText8(Canvas& cv, const char* s, int yTop, int scale) {
  const int tw = textWidth8(s, scale);
  int x = kCenterX - tw / 2;
  int minX = cv.w, maxX = -1, minY = cv.h, maxY = -1;
  for (const char* p = s; *p; ++p) {
    const Glyph8* g = glyph8(*p);
    for (int r = 0; r < 8; ++r) {
      const uint8_t row = g->rows[r];
      for (int col = 0; col < 8; ++col) {
        if (!(row & (0x80 >> col))) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) {
            const int px = x + col * scale + sx, py = yTop + r * scale + sy;
            cv.setBlack(px, py);
            if (px < minX) minX = px;
            if (px > maxX) maxX = px;
            if (py < minY) minY = py;
            if (py > maxY) maxY = py;
          }
      }
    }
    x += 8 * scale;
  }
  if (maxX < 0) return {0, 0, 0, 0};
  return {minX, minY, maxX - minX + 1, maxY - minY + 1};
}

// ---- Large "PUSH": vector strokes rasterised at target size (no integer zoom). ----
// Each glyph is a list of axis-aligned strokes in an 8x8 em box. A stroke is
// {x0,y0,x1,y1} centre-line endpoints; half-width kStrokeHW. Rasterisation
// samples 2x2 sub-pixels per device pixel and sets ink at cov>=0.5, so edges
// are computed for the TARGET size instead of replicating an 8x8 bitmap.
struct Stroke { float x0, y0, x1, y1; };
struct VecGlyph { char c; const Stroke* strokes; int n; };
constexpr float kStrokeHW = 0.62f;
static const Stroke kP[] = {
    {1.4f,0.5f,1.4f,7.5f}, {1.4f,0.5f,5.4f,0.5f},
    {5.4f,0.5f,5.4f,3.7f}, {1.4f,3.7f,5.4f,3.7f},
};
static const Stroke kU[] = {
    {1.0f,0.5f,1.0f,6.2f}, {7.0f,0.5f,7.0f,6.2f},
    {1.0f,6.6f,7.0f,6.6f}, {1.0f,6.2f,1.0f,6.6f}, {7.0f,6.2f,7.0f,6.6f},
};
static const Stroke kS[] = {
    {1.4f,0.5f,6.6f,0.5f}, {1.4f,0.5f,1.4f,3.8f}, {1.4f,3.8f,6.6f,3.8f},
    {6.6f,4.2f,6.6f,7.5f}, {1.4f,7.5f,6.6f,7.5f},
};
static const Stroke kH[] = {
    {1.4f,0.5f,1.4f,7.5f}, {6.6f,0.5f,6.6f,7.5f}, {1.4f,4.0f,6.6f,4.0f},
};
static const VecGlyph kVec[] = {
    {'P', kP, 4}, {'U', kU, 5}, {'S', kS, 5}, {'H', kH, 3},
};
const VecGlyph* vecGlyph(char c) {
  for (const auto& g : kVec) if (g.c == c) return &g;
  return nullptr;
}
float distToStroke(float px, float py, const Stroke& s) {
  const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 0 ? ((px - s.x0) * dx + (py - s.y0) * dy) / len2 : 0.0f;
  if (t < 0) t = 0; else if (t > 1) t = 1;
  const float cx = s.x0 + t * dx, cy = s.y0 + t * dy;
  const float ex = px - cx, ey = py - cy;
  return ex * ex + ey * ey;  // squared distance
}
// Rasterise one vector glyph into canvas at (x,y) size (cell px).
BBox drawVecGlyph(Canvas& cv, char ch, int x, int y, float cell) {
  const VecGlyph* g = vecGlyph(ch);
  if (!g) return {0, 0, 0, 0};
  const float hw2 = kStrokeHW * kStrokeHW;
  int minX = cv.w, maxX = -1, minY = cv.h, maxY = -1;
  for (int py = 0; py < static_cast<int>(cell + 0.5f); ++py) {
    for (int px = 0; px < static_cast<int>(cell + 0.5f); ++px) {
      int hits = 0;
      for (int sy = 0; sy < 2; ++sy)
        for (int sx = 0; sx < 2; ++sx) {
          const float gx = (px + (sx + 0.5f) / 2.0f) * 8.0f / cell;
          const float gy = (py + (sy + 0.5f) / 2.0f) * 8.0f / cell;
          bool inside = false;
          for (int i = 0; i < g->n; ++i)
            if (distToStroke(gx, gy, g->strokes[i]) <= hw2) { inside = true; break; }
          if (inside) ++hits;
        }
      cv.drawInk(x + px, y + py, static_cast<uint8_t>(hits * 255 / 4));
      if (hits >= 2) {
        if (x + px < minX) minX = x + px;
        if (x + px > maxX) maxX = x + px;
        if (y + py < minY) minY = y + py;
        if (y + py > maxY) maxY = y + py;
      }
    }
  }
  if (maxX < 0) return {0, 0, 0, 0};
  return {minX, minY, maxX - minX + 1, maxY - minY + 1};
}

void write32le(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
void write16le(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

}  // namespace

bool writeCoverBmp(WriteFn sink, void* ctx, const CoverInfo& info) {
  if (!sink) return false;
  constexpr int W = kThumbWidth, H = kThumbHeight;
  constexpr int rowBytes = (W + 31) / 32 * 4;  // 32 for W=240
  const size_t pixelsSize = static_cast<size_t>(rowBytes) * H;
  const size_t headerSize = 14 + 40 + 8;       // BITMAPFILEHEADER + INFOHEADER + 2 colors
  const size_t fileSize = headerSize + pixelsSize;

  auto pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pixelsSize]);
  if (!pixels) return false;

  Canvas cv{pixels.get(), W, H, rowBytes};
  cv.clear();

  // ---- Layout: evenly distributed, centred on the card midline. ----
  // Large PUSH: cell=48px tall, centred slightly above middle (top ~100).
  constexpr float kCell = 48.0f;
  const int cellI = static_cast<int>(kCell + 0.5f);
  const int pushW = 4 * cellI + 3 * 4;           // 4 glyphs + 3 gaps of 4px
  int pushX = kCenterX - pushW / 2;              // centred -> left ~18
  for (char ch : {'P', 'U', 'S', 'H'}) {
    drawVecGlyph(cv, ch, pushX, 100, kCell);
    pushX += cellI + 4;
  }

  // S3c final (甲方案): time line moved BELOW the byte-count line so the
  // home-card theme's centred title box (canvas y~177..222, PUSH) no longer
  // covers it. Large PUSH (y100..147) and byte count (y252..265) are UNCHANGED.
  // Time line target y290..296: gap to byte-count bottom (265) is 25px (>=20),
  // clear of the theme title box bottom (222), above the Continue Reading
  // button (y361..391), and within the y320 safety ceiling. Scale/fitScale8,
  // centring on x120 and stride are all untouched.

  // Byte count MUST carry a unit. "4458 bytes" is 10 chars -> scale 2 fits (160px).
  char line[48];
  std::snprintf(line, sizeof(line), "%zu bytes", info.byteCount);
  const int bsc = fitScale8(line);
  drawText8(cv, line, 252, bsc > 0 ? bsc : 1);

  // Time line: adaptive scale, centred; split into date / time if it will not fit.
  if (info.timeText && info.timeText[0]) {
    const int sc = fitScale8(info.timeText);
    if (sc > 0) {
      drawText8(cv, info.timeText, 290, sc);
    } else {
      // Fallback: split "YYYY-MM-DD HH:MM" into two centred lines at scale 1.
      // y286 / y300: first line >= 21px below byte-count bottom, second ends
      // y306 (< y320 ceiling), both above the Continue Reading button.
      const char* sp = std::strchr(info.timeText, ' ');
      if (sp) {
        const size_t n = static_cast<size_t>(sp - info.timeText);
        std::string date(info.timeText, n);
        std::string clock(sp + 1);
        drawText8(cv, date.c_str(), 286, 1);
        drawText8(cv, clock.c_str(), 300, 1);
      } else {
        drawText8(cv, info.timeText, 290, 1);
      }
    }
  }

  uint8_t header[headerSize];
  std::memset(header, 0, sizeof(header));
  write16le(header + 0, 0x4D42);            // 'BM'
  write32le(header + 2, static_cast<uint32_t>(fileSize));
  write32le(header + 10, static_cast<uint32_t>(headerSize));
  write32le(header + 14, 40);               // biSize
  write32le(header + 18, static_cast<uint32_t>(W));
  write32le(header + 22, static_cast<uint32_t>(H));  // positive = BottomUp
  write16le(header + 26, 1);                // biPlanes
  write16le(header + 28, 1);                // biBitCount
  write32le(header + 34, static_cast<uint32_t>(pixelsSize));
  write32le(header + 46, 2);                // biClrUsed
  header[54] = header[55] = header[56] = header[57] = 0;              // color0 black
  header[58] = header[59] = header[60] = header[61] = 0xFF;           // color1 white

  if (sink(ctx, header, headerSize) != headerSize) return false;
  for (int y = H - 1; y >= 0; --y) {
    const uint8_t* row = pixels.get() + static_cast<size_t>(y) * rowBytes;
    if (sink(ctx, row, rowBytes) != rowBytes) return false;
  }
  return true;
}

}  // namespace pushthumb
