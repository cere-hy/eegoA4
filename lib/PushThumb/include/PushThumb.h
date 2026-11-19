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
namespace pushthumb {

constexpr int kThumbWidth = 240;   // height(400) * 0.6, matches Epub.cpp
constexpr int kThumbHeight = 400;

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

}  // namespace pushthumb
