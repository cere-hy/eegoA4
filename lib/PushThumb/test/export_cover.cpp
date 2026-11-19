/* Export the exact device cover BMP for pixel-level verification. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "PushThumb.h"
using namespace pushthumb;
static FILE* g_f;
static size_t sink(void*, const uint8_t* d, size_t n) { return fwrite(d, 1, n, g_f); }
int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "thumb_400.bmp";
  const char* time = argc > 2 ? argv[2] : "2026-11-19 08:58";
  size_t bytes = argc > 3 ? (size_t)strtoull(argv[3], nullptr, 10) : 4458;
  g_f = fopen(path, "wb");
  if (!g_f) return 2;
  CoverInfo info{time, bytes};
  bool ok = writeCoverBmp(sink, nullptr, info);
  fclose(g_f);
  printf("export %s ok=%d time=%s bytes=%zu\n", path, ok, time, bytes);
  return ok ? 0 : 1;
}
