#pragma once

#include <cstddef>
#include <cstdint>

// S3b P4 — deterministic, dependency-free cover thumbnail for the pushed PUSH
// document. Produces the SAME 1-bit BMP format/size the book shelf already uses
// (240x400 = height*0.6 x height, BottomUp, color0=black/color1=white), so it
// drops straight into <cachePath>/thumb_400.bmp with no renderer/UI dependency.
//
// The cover is pure ASCII art drawn from an embedded 8x8 font: white background,
// large centered "PUSH", then two small lines: push time and byte count. Ink
// screen gray-3 shows 1-bit art cleanly; no decorative shapes.
//
// All rendering is in-memory; the caller supplies a sink that receives the
// finished BMP bytes (a HalFile::write adapter on device, a buffer in host
// tests). No SD/FS/font-system coupling here — those live in the caller.
//
// S3cPF5 — the drawing core is factored into renderCoverInRect(), which paints
// into an arbitrary (x,y,w,h) rectangle through an ink-pixel callback. The
// legacy writeCoverBmp() is now a thin 240x400 adapter over the SAME core, so
// classic-theme pixels are byte-identical while every other theme can paint
// its own card rect directly (no bmp load, no centre-crop).
namespace pushthumb {

constexpr int kThumbWidth = 240;   // height(400) * 0.6, matches Epub.cpp
constexpr int kThumbHeight = 400;

// Reference card the layout constants were tuned against. At (240,400) the
// rect renderer reproduces the original BMP pixel-for-pixel.
constexpr int kRefWidth = 240;
constexpr int kRefHeight = 400;

// Byte sink: return number of bytes consumed (== len on success).
using WriteFn = size_t (*)(void* ctx, const uint8_t* data, size_t len);

struct CoverInfo {
  const char* timeText;  // "YYYY-MM-DD HH:MM" (ASCII, 16 chars)
  size_t byteCount;      // cleaned document size in bytes
};

// Renders the cover and streams the complete BMP through sink. Returns true on
// success. Never allocates more than one row buffer (handled internally by the
// caller's sink contract, so this is pure stack/math).
bool writeCoverBmp(WriteFn sink, void* ctx, const CoverInfo& info);

// ---- S3cPF5: rect-driven vector cover -------------------------------------
// Called once per ink pixel, in absolute screen coordinates. `state=true`
// means ink (black); the caller is responsible for painting the rect's white
// background and clipping. Coordinates are always clipped to [x,x+w)x[y,y+h)
// before the callback fires, so no element can be drawn outside the card.
using InkFn = void (*)(void* ctx, int x, int y, bool state);

// Paint the PUSH cover into an arbitrary rectangle. Pure math + callbacks: no
// BMP load, no font-system/PSRAM dependency. Layout scales with the rect:
//   * large PUSH  — top band, glyph cell scaled by min(w/h vs reference)
//   * "NNNN bytes" — middle band
//   * time line   — lower band; split into date/clock or OMITTED when the rect
//                   is too small to show it legibly (see renderCoverInRect).
// Everything is horizontally centred in the rect. Returns true if a time line
// was drawn, false if it was omitted for legibility.
bool renderCoverInRect(void* ctx, InkFn ink, int x, int y, int w, int h, const CoverInfo& info);

// Geometry query used by device-side host tests: minimum rect (px) at which
// the time line is still rendered. Below this height/width the time line is
// omitted so the large word and byte count stay readable and unclipped.
constexpr int kMinTimeLineWidth = 120;   // usable width for a 16-char time line @scale1
constexpr int kMinTimeLineHeight = 150;  // vertical room for three centred bands

}  // namespace pushthumb
