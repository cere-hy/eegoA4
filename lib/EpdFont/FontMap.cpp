#include "FontMap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include <esp_heap_caps.h>
#endif

namespace epd {
namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
// On-disk cache uses little-endian fields (both ESP32 and host are LE).
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

template <typename T>
T* allocMap(size_t n) {
#if defined(ESP_PLATFORM) || defined(ARDUINO)
  auto* p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!p) p = static_cast<T*>(heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT));
  return p;
#else
  return static_cast<T*>(std::malloc(n * sizeof(T)));
#endif
}

struct TableDir {
  uint32_t cmap = 0, cmapLen = 0;
  uint32_t hmtx = 0, hmtxLen = 0;
  uint32_t head = 0, hhea = 0, maxp = 0;
};

bool readRange(FontReadFn read, void* ctx, unsigned long off, uint8_t* dst, unsigned long len, unsigned long fileSize) {
  if (off > fileSize || len > fileSize - off) return false;
  return read(ctx, off, dst, len) == len;
}

bool parseDir(FontReadFn read, void* ctx, unsigned long fileSize, TableDir& t) {
  uint8_t head[12];
  if (!readRange(read, ctx, 0, head, 12, fileSize)) return false;
  const uint32_t sfnt = be32(head);
  if (sfnt != 0x00010000u && sfnt != 0x4F54544Fu && sfnt != 0x74727565u) return false;  // 1.0 / OTTO / true
  const uint16_t n = be16(head + 4);
  if (n == 0 || n > 256) return false;
  const unsigned long dirLen = 12UL + static_cast<unsigned long>(n) * 16;
  uint8_t* dir = static_cast<uint8_t*>(std::malloc(dirLen));
  if (!dir) return false;
  const bool ok = readRange(read, ctx, 0, dir, dirLen, fileSize);
  if (ok) {
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* p = dir + 12 + 16 * i;
      const uint32_t tag = be32(p);
      const uint32_t off = be32(p + 8);
      const uint32_t len = be32(p + 12);
      if (off > fileSize || len > fileSize - off) continue;
      if (tag == 0x636D6170u) { t.cmap = off; t.cmapLen = len; }
      else if (tag == 0x686D7478u) { t.hmtx = off; t.hmtxLen = len; }
      else if (tag == 0x68656164u) t.head = off;
      else if (tag == 0x68686561u) t.hhea = off;
      else if (tag == 0x6D617870u) t.maxp = off;
    }
  }
  std::free(dir);
  return ok && t.cmap && t.hmtx && t.head && t.hhea && t.maxp;
}

// Collect (codepoint -> glyphId) for BMP codepoints from the best cmap subtable.
// Preference: (3,10) format 12, then (3,1) format 4. Only BMP cps (<=0xFFFF)
// are stored; supplementary cps are counted in skippedSupp.
struct CmapEntry { uint16_t cp; uint16_t gid; };

bool parseCmap(FontReadFn read, void* ctx, const TableDir& t, unsigned long fileSize,
               std::vector<CmapEntry>& out, uint32_t& skippedSupp) {
  uint8_t ch[4];
  if (!readRange(read, ctx, t.cmap, ch, 4, fileSize)) return false;
  const uint16_t nsub = be16(ch + 2);
  if (nsub == 0 || nsub > 64) return false;
  std::vector<uint8_t> cmap(t.cmapLen);
  if (!readRange(read, ctx, t.cmap, cmap.data(), t.cmapLen, fileSize)) return false;

  uint32_t chosenF12 = 0, chosenF4 = 0;
  for (uint16_t i = 0; i < nsub; ++i) {
    const uint8_t* p = cmap.data() + 4 + 8 * i;
    const uint16_t plat = be16(p), enc = be16(p + 2);
    const uint32_t off = be32(p + 4);
    if (off + 2 > cmap.size()) continue;
    const uint16_t fmt = be16(cmap.data() + off);
    if (plat == 3 && enc == 10 && fmt == 12) chosenF12 = off;
    else if (plat == 3 && enc == 1 && fmt == 4 && !chosenF4) chosenF4 = off;
    else if (plat == 0 && fmt == 4 && !chosenF4) chosenF4 = off;
  }

  out.reserve(8192);
  auto add = [&](uint32_t cp, uint32_t gid) {
    if (gid == 0 || gid > 0xFFFFu) return;
    if (cp > 0xFFFFu) { ++skippedSupp; return; }
    out.push_back({static_cast<uint16_t>(cp), static_cast<uint16_t>(gid)});
  };

  if (chosenF12) {
    const uint8_t* f = cmap.data() + chosenF12;
    const uint32_t ngrp = be32(f + 12);
    if (ngrp > 200000u || 16UL + static_cast<unsigned long>(ngrp) * 12 > cmap.size() - chosenF12) return false;
    for (uint32_t g = 0; g < ngrp; ++g) {
      const uint8_t* p = f + 16 + 12 * g;
      const uint32_t s = be32(p), e = be32(p + 4), sg = be32(p + 8);
      if (e < s || e - s > 0x10000u) continue;
      for (uint32_t c = s; c <= e; ++c) add(c, sg + (c - s));
    }
    return true;
  }
  if (chosenF4) {
    const uint8_t* f = cmap.data() + chosenF4;
    const uint16_t segX2 = be16(f + 6);
    const uint16_t nseg = segX2 / 2;
    if (nseg == 0 || 16UL + 8UL * nseg > cmap.size() - chosenF4) return false;
    const uint8_t* endC = f + 14;
    const uint8_t* startC = f + 16 + 2 * nseg;
    const uint8_t* idDelta = f + 16 + 4 * nseg;
    const uint8_t* idRange = f + 16 + 6 * nseg;
    for (uint16_t s = 0; s < nseg; ++s) {
      const uint16_t end = be16(endC + 2 * s);
      const uint16_t start = be16(startC + 2 * s);
      const uint16_t delta = be16(idDelta + 2 * s);
      const uint16_t ro = be16(idRange + 2 * s);
      if (end < start || static_cast<uint32_t>(end - start) > 4096u) continue;
      for (uint32_t c = start; c <= end; ++c) {
        uint32_t g;
        if (ro == 0) {
          g = (c + delta) & 0xFFFFu;
        } else {
          const uint32_t addr = static_cast<uint32_t>(idRange - cmap.data()) + 2 * s + ro + 2 * (c - start);
          if (addr + 2 > cmap.size()) continue;
          g = be16(cmap.data() + addr);
          if (g) g = (g + delta) & 0xFFFFu;
        }
        add(c, g);
      }
    }
    return true;
  }
  return false;
}

}  // namespace

FontMap::~FontMap() { free(); }
FontMap::FontMap(FontMap&& o) noexcept
    : present_(o.present_), segBase_(o.segBase_), advances_(o.advances_), count_(o.count_), upem_(o.upem_) {
  o.present_ = nullptr; o.segBase_ = nullptr; o.advances_ = nullptr; o.count_ = 0;
}
FontMap& FontMap::operator=(FontMap&& o) noexcept {
  if (this != &o) {
    free();
    present_ = o.present_; segBase_ = o.segBase_; advances_ = o.advances_; count_ = o.count_; upem_ = o.upem_;
    o.present_ = nullptr; o.segBase_ = nullptr; o.advances_ = nullptr; o.count_ = 0;
  }
  return *this;
}
void FontMap::free() {
  if (present_) std::free(present_);
  if (segBase_) std::free(segBase_);
  if (advances_) std::free(advances_);
  present_ = nullptr; segBase_ = nullptr; advances_ = nullptr; count_ = 0;
}

static inline int popcount8(uint8_t v) {
  int c = 0; while (v) { c += v & 1; v >>= 1; } return c;
}

bool FontMap::build(FontReadFn read, void* ctx, unsigned long fileSize, uint16_t& upemOut, FontMapStats& stats) {
  free();
  TableDir t;
  if (!parseDir(read, ctx, fileSize, t)) return false;

  uint8_t h[54];
  if (!readRange(read, ctx, t.head, h, 54, fileSize)) return false;
  const uint16_t upem = be16(h + 18);
  if (upem == 0) return false;
  uint8_t hh[36];
  if (!readRange(read, ctx, t.hhea, hh, 36, fileSize)) return false;
  const uint16_t numHMetrics = be16(hh + 34);
  if (numHMetrics == 0) return false;

  std::vector<CmapEntry> pairs;
  uint32_t skipped = 0;
  if (!parseCmap(read, ctx, t, fileSize, pairs, skipped)) return false;
  if (pairs.empty()) return false;

  // Sort by codepoint, de-duplicate (last gid wins), drop gid >= numHMetrics.
  std::sort(pairs.begin(), pairs.end(), [](const CmapEntry& a, const CmapEntry& b) { return a.cp < b.cp; });
  std::vector<CmapEntry> uniq;
  uniq.reserve(pairs.size());
  for (const auto& e : pairs) {
    if (e.gid >= numHMetrics) continue;
    if (!uniq.empty() && uniq.back().cp == e.cp) uniq.back().gid = e.gid;
    else uniq.push_back(e);
  }
  if (uniq.empty()) return false;

  // Read the advances for all referenced gids in ONE pass over hmtx's prefix
  // (numberOfHMetrics entries, 4 bytes each: advanceWidth u16 + lsb i16).
  const unsigned long awBytes = 4UL * numHMetrics;
  if (awBytes > fileSize) return false;
  std::vector<uint16_t> aw(numHMetrics);
  std::vector<uint8_t> hmtxBuf;
  hmtxBuf.resize(awBytes);
  if (!readRange(read, ctx, t.hmtx, hmtxBuf.data(), awBytes, fileSize)) return false;
  for (uint32_t g = 0; g < numHMetrics; ++g) aw[g] = be16(hmtxBuf.data() + 4 * g);

  if (uniq.size() > kBmpBits) { free(); return false; }
  present_ = allocMap<uint8_t>(kBitmapBytes);
  segBase_ = allocMap<uint16_t>(kSegCount);
  advances_ = allocMap<uint16_t>(uniq.size());
  if (!present_ || !segBase_ || !advances_) { free(); return false; }
  std::memset(present_, 0, kBitmapBytes);
  std::memset(segBase_, 0, kSegIndexBytes);
  uint32_t idx = 0;
  uint16_t segCounts[kSegCount] = {0};
  for (size_t i = 0; i < uniq.size(); ++i) {
    const uint16_t cp = uniq[i].cp;
    present_[cp >> 3] |= static_cast<uint8_t>(1u << (cp & 7));
    advances_[idx++] = aw[uniq[i].gid];
    ++segCounts[cp >> 8];
  }
  uint16_t running = 0;
  for (size_t s = 0; s < kSegCount; ++s) { segBase_[s] = running; running += segCounts[s]; }
  count_ = idx;
  upem_ = upem;
  upemOut = upem;
  stats.codepoints = count_;
  stats.skippedSupp = skipped;
  stats.buildOk = true;
  return true;
}

bool FontMap::save(void* outCtx, bool (*write)(void* ctx, const uint8_t* data, size_t len)) const {
  if (!present_ || !segBase_ || !advances_ || count_ == 0) return false;
  uint8_t hdr[kHeaderSize];
  std::memset(hdr, 0, sizeof(hdr));
  hdr[0] = static_cast<uint8_t>(kMagic);
  hdr[1] = static_cast<uint8_t>(kMagic >> 8);
  hdr[2] = static_cast<uint8_t>(kMagic >> 16);
  hdr[3] = static_cast<uint8_t>(kMagic >> 24);
  hdr[4] = kVersion;
  hdr[6] = static_cast<uint8_t>(count_);
  hdr[7] = static_cast<uint8_t>(count_ >> 8);
  hdr[8] = static_cast<uint8_t>(upem_);
  hdr[9] = static_cast<uint8_t>(upem_ >> 8);
  hdr[10] = static_cast<uint8_t>(count_ >> 16);
  hdr[11] = static_cast<uint8_t>(count_ >> 24);
  if (!write(outCtx, hdr, kHeaderSize)) return false;
  for (size_t s = 0; s < kSegCount; ++s) {
    uint8_t b[2] = {static_cast<uint8_t>(segBase_[s]), static_cast<uint8_t>(segBase_[s] >> 8)};
    if (!write(outCtx, b, 2)) return false;
  }
  if (!write(outCtx, present_, kBitmapBytes)) return false;
  for (uint32_t i = 0; i < count_; ++i) {
    uint8_t b[2] = {static_cast<uint8_t>(advances_[i]), static_cast<uint8_t>(advances_[i] >> 8)};
    if (!write(outCtx, b, 2)) return false;
  }
  return true;
}

bool FontMap::load(const uint8_t* data, size_t len) {
  free();
  const size_t fixed = kHeaderSize + kSegIndexBytes + kBitmapBytes;
  if (len < fixed) return false;
  if (le32(data) != kMagic || data[4] != kVersion) return false;
  const uint32_t count = static_cast<uint32_t>(le16(data + 6)) | (static_cast<uint32_t>(data[10]) << 16) |
                         (static_cast<uint32_t>(data[11]) << 24);
  const uint32_t upem = le16(data + 8);
  if (count == 0 || count > kBmpBits || upem == 0) return false;
  if (len < fixed + static_cast<size_t>(count) * 2) return false;
  // Validate segBase monotonic + present-popcount consistency (corrupt guard).
  const uint8_t* pbase = data + kHeaderSize;
  const uint8_t* pbits = data + kHeaderSize + kSegIndexBytes;
  uint32_t running = 0;
  for (size_t s = 0; s < kSegCount; ++s) {
    const uint16_t base = static_cast<uint16_t>(le16(pbase + 2 * s));
    if (base != running) return false;
    uint32_t segPop = 0;
    for (size_t b = s * 32; b < (s + 1) * 32; ++b) segPop += popcount8(pbits[b]);
    running += segPop;
  }
  if (running != count) return false;
  present_ = allocMap<uint8_t>(kBitmapBytes);
  segBase_ = allocMap<uint16_t>(kSegCount);
  advances_ = allocMap<uint16_t>(count);
  if (!present_ || !segBase_ || !advances_) { free(); return false; }
  std::memcpy(present_, pbits, kBitmapBytes);
  std::memcpy(segBase_, pbase, kSegIndexBytes);
  const uint8_t* adv = data + fixed;
  for (uint32_t i = 0; i < count; ++i) advances_[i] = le16(adv + 2 * i);
  count_ = count;
  upem_ = static_cast<uint16_t>(upem);
  return true;
}

bool FontMap::lookup(const uint32_t cp, uint16_t& advanceWidthFontUnits) const {
  if (cp > 0xFFFFu || count_ == 0 || !present_) return false;
  const uint16_t key = static_cast<uint16_t>(cp);
  const uint8_t byte = present_[key >> 3];
  const uint8_t bit = static_cast<uint8_t>(1u << (key & 7));
  if (!(byte & bit)) return false;
  // Dense index = segBase + popcount(bits before cp within this 256-block).
  const size_t seg = key >> 8;
  const size_t byteInSeg = (key >> 3) - seg * 32;
  uint32_t idx = segBase_[seg];
  for (size_t b = 0; b < byteInSeg; ++b) idx += popcount8(present_[seg * 32 + b]);
  idx += popcount8(static_cast<uint8_t>(byte & (bit - 1)));
  advanceWidthFontUnits = advances_[idx];
  return true;
}

}  // namespace epd
