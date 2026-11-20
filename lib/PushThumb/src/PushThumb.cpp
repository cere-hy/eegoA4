// S3c merged-order P2 — from-scratch cover redraw. Pure in-memory 1-bit cover
// renderer (no ESP/PSRAM/font-system dependency). See include/PushThumb.h.
#include "PushThumb.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace pushthumb {
namespace {

// Canvas paints into either a packed 1-bit BMP row buffer (legacy
// writeCoverBmp) or an arbitrary rectangle through an ink-pixel callback
// (renderCoverInRect, S3cPF5). Local coordinates (0..w, 0..h) are translated
// to absolute pixels: BMP indexing for the buffer, (ox+lx, oy+ly) for ink.
// Ink pixels are black; setWhite is a no-op in callback mode because the
// caller paints the card's white background before invoking us.
struct Canvas {
  uint8_t* bits;      // BMP mode: row buffer; callback mode: nullptr
  int w, h, stride;   // local size + BMP row stride
  int ox, oy;         // absolute origin of local (0,0)
  InkFn ink;          // callback mode only
  void* inkCtx;

  void setPixel(int lx, int ly, bool black) {
    if (lx < 0 || lx >= w || ly < 0 || ly >= h) return;
    if (bits) {
      const int ax = ox + lx, ay = oy + ly;
      uint8_t* p = bits + static_cast<size_t>(ay) * stride + (ax >> 3);
      const uint8_t mask = static_cast<uint8_t>(0x80 >> (ax & 7));
      if (black) *p &= static_cast<uint8_t>(~mask); else *p |= mask;
    } else if (ink && black) {
      ink(inkCtx, ox + lx, oy + ly, true);
    }
  }
  void setBlack(int x, int y) { setPixel(x, y, true); }
  void drawInk(int x, int y, uint8_t cov) { setPixel(x, y, cov >= 128); }
  void clear() { if (bits) std::memset(bits, 0xFF, static_cast<size_t>(stride) * h); }
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

// Exact integer-scale 8x8 text. advance = cell (8*scale) per char, centred on
// the rect midline. `cell` is always a multiple of 8 so the legacy 240x400
// output (scale 1/2/3) is bit-identical.
int textWidth(const char* s, int cell) {
  if (!s) return 0;
  return static_cast<int>(std::strlen(s)) * cell;
}
// Largest integer scale in [1..3] whose width fits usableW; 0 = no fit.
int fitScale(const char* s, int usableW) {
  if (!s || !*s) return 1;
  const int len = static_cast<int>(std::strlen(s));
  for (int scale = 3; scale >= 1; --scale)
    if (len * 8 * scale <= usableW) return scale;
  return 0;
}
// Draw 8x8 text with integer zoom `scale`, horizontally centred at cx.
void drawText(Canvas& cv, const char* s, int yTop, int scale, int cx) {
  if (!s || !*s || scale < 1) return;
  const int cell = 8 * scale;
  const int tw = textWidth(s, cell);
  int gx = cx - tw / 2;
  for (const char* p = s; *p; ++p) {
    const Glyph8* g = glyph8(*p);
    for (int r = 0; r < 8; ++r) {
      const uint8_t row = g->rows[r];
      for (int col = 0; col < 8; ++col) {
        if (!(row & (0x80 >> col))) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) cv.setBlack(gx + col * scale + sx, yTop + r * scale + sy);
      }
    }
    gx += cell;
  }
}

// ---- Large "PUSH": vector strokes rasterised at target size (no integer zoom). ----
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
  return ex * ex + ey * ey;
}
// Rasterise one vector glyph; `cell` may be fractional (small-card scaling).
void drawVecGlyph(Canvas& cv, char ch, int x, int y, float cell) {
  const VecGlyph* g = vecGlyph(ch);
  if (!g || cell < 1.0f) return;
  const float hw2 = kStrokeHW * kStrokeHW;
  const int n = static_cast<int>(cell + 0.5f);
  for (int py = 0; py < n; ++py) {
    for (int px = 0; px < n; ++px) {
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
    }
  }
}

void write32le(uint8_t* p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
void write16le(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }

// ---- Shared layout core -----------------------------------------------------
// Paint all three elements into `cv` (local coords). Layout scales with the
// canvas size relative to the 240x400 reference: at 240x400 every constant is
// identical to the original, so the legacy BMP is reproduced pixel-for-pixel.
// Returns true when a time line was drawn.
bool drawCoverCore(Canvas& cv, const CoverInfo& info) {
  const int w = cv.w, h = cv.h;
  const int cx = w / 2;
  const float sx = static_cast<float>(w) / static_cast<float>(kRefWidth);
  const float sy = static_cast<float>(h) / static_cast<float>(kRefHeight);
  const float s = sx < sy ? sx : sy;                 // uniform glyph scale
  const int margin = static_cast<int>(16.0f * s + 0.5f);
  int usableW = w - 2 * margin;
  if (usableW < 8) usableW = 8;

  // Large PUSH — top band, original cell=48/gap=4/top=100 at reference.
  const float cellF = 48.0f * s;
  const int cellI = static_cast<int>(cellF + 0.5f);
  const int gap = static_cast<int>(4.0f * s + 0.5f);
  const int pushW = 4 * cellI + 3 * gap;
  int pushX = cx - pushW / 2;
  const int pushTop = static_cast<int>(100.0f * sy + 0.5f);
  for (char ch : {'P', 'U', 'S', 'H'}) {
    drawVecGlyph(cv, ch, pushX, pushTop, cellF);
    pushX += cellI + gap;
  }

  // Byte count carries a unit. Original y=252.
  char line[48];
  std::snprintf(line, sizeof(line), "%zu bytes", info.byteCount);
  const int bsc = fitScale(line, usableW);
  const int bytesY = static_cast<int>(252.0f * sy + 0.5f);
  drawText(cv, line, bytesY, bsc > 0 ? bsc : 1, cx);

  // Time line — original y=290; split to two centred lines if one won't fit.
  // Omitted entirely on very small cards so the large word + byte count stay
  // legible and nothing is clipped.
  if (!info.timeText || !info.timeText[0]) return false;
  if (w < kMinTimeLineWidth && h < kMinTimeLineHeight) return false;
  const int timeY = static_cast<int>(290.0f * sy + 0.5f);
  const int sc = fitScale(info.timeText, usableW);
  if (sc > 0) {
    drawText(cv, info.timeText, timeY, sc, cx);
    return true;
  }
  const char* sp = std::strchr(info.timeText, ' ');
  if (sp) {
    const size_t n = static_cast<size_t>(sp - info.timeText);
    std::string date(info.timeText, n);
    std::string clock(sp + 1);
    const int dsc = fitScale(date.c_str(), usableW);
    const int csc = fitScale(clock.c_str(), usableW);
    if (dsc > 0 && csc > 0) {
      const int lh = 8 * dsc;
      const int base = timeY - lh / 2;
      drawText(cv, date.c_str(), base, dsc, cx);
      drawText(cv, clock.c_str(), base + lh + static_cast<int>(4 * s + 0.5f), csc, cx);
      return true;
    }
  }
  return false;  // too small: omit time line rather than clip it
}

}  // namespace

// ---- Public: legacy 240x400 BMP (thin adapter over the shared core) --------
bool writeCoverBmp(WriteFn sink, void* ctx, const CoverInfo& info) {
  if (!sink) return false;
  constexpr int W = kThumbWidth, H = kThumbHeight;
  constexpr int rowBytes = (W + 31) / 32 * 4;  // 32 for W=240
  const size_t pixelsSize = static_cast<size_t>(rowBytes) * H;
  const size_t headerSize = 14 + 40 + 8;       // BITMAPFILEHEADER + INFOHEADER + 2 colors
  const size_t fileSize = headerSize + pixelsSize;

  auto pixels = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pixelsSize]);
  if (!pixels) return false;

  Canvas cv{pixels.get(), W, H, rowBytes, 0, 0, nullptr, nullptr};
  cv.clear();
  drawCoverCore(cv, info);

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

// ---- Public: S3cPF5 rect-driven vector cover -------------------------------
bool renderCoverInRect(void* ctx, InkFn ink, int x, int y, int w, int h, const CoverInfo& info) {
  if (!ink || w <= 0 || h <= 0) return false;
  // Callback canvas has no BMP backing; stride is unused. setWhite is a no-op
  // (caller paints white), and every ink pixel is clipped to the rect.
  Canvas cv{nullptr, w, h, (w + 7) / 8, x, y, ink, ctx};
  return drawCoverCore(cv, info);
}

}  // namespace pushthumb
