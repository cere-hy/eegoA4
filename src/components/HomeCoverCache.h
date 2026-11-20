#pragma once

#include <Bitmap.h>
#include <FreeInkUICore.h>
#include <HalMemory.h>
#include <HalStorage.h>

#include <array>
#include <string>

class GfxRenderer;
struct RecentBook;

// Screen-lifetime thumbnail decoding and PSRAM region snapshots.
class HomeCoverCache {
 public:
  static constexpr size_t MAX_COVERS = 7;
  explicit HomeCoverCache(GfxRenderer& renderer) : renderer(renderer) {}
  void begin();
  void prepare();
  void invalidate();
  void invalidate(size_t index);
  // S3cPF5: `book` may be null; when it is the PUSH virtual book the cover is
  // painted as vector art into `rect` instead of loading a centre-cropped bmp.
  bool paint(freeink::ui::Rect rect, size_t index, const std::string& path, const RecentBook* book = nullptr);

 private:
  struct CachedCover {
    freeink::ui::Rect rect{};
    size_t offset = 0;
    size_t bytes = 0;
    bool valid = false;
  };
  HalMemory::PsramBuffer coverCache;
  std::array<CachedCover, MAX_COVERS> cachedCovers{};
  size_t coverCacheCapacity = 0;
  size_t coverCacheUsed = 0;
  int coverCacheOrientation = -1;

  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
  GfxRenderer& renderer;
};
