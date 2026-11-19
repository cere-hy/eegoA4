/* S3c P0 host regression — stack-budget guard for the ActivityManager render
 * task. The original host tests only exercised Pif1Codec.h (pure logic with an
 * injected zlib callback) and never compiled the device glue that actually
 * allocates the decompressor/scratch buffers, so an 18.6KB stack spike during
 * section-build glyph faults slipped through to real hardware (canary panic).
 * This test turns the root-cause facts into assertions so a regression is RED
 * on the host instead of on the device. */
#include <cstdio>
#include <cstdint>
#include <string>

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_DEFLATE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../../miniz/third_party/miniz.h"

static int checks = 0, red = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { printf("RED: %s\n", m); } } while (0)

static std::string slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return std::string();
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  std::string s(static_cast<size_t>(n), '\0');
  if (n > 0 && fread(&s[0], 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) s.clear();
  fclose(f);
  return s;
}

static bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

// Count occurrences only on non-comment lines (strip leading // comments).
static bool hasCall(const std::string& s, const char* needle) {
  size_t pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    const size_t ls = s.rfind('\n', pos) + 1;
    const std::string line = s.substr(ls, s.find('\n', pos) - ls);
    const size_t c = line.find("//");
    if (c == std::string::npos || pos - ls < c) return true;  // call before any //
    pos += std::string(needle).size();
  }
  return false;
}

int main(int argc, char** argv) {
  const char* glue = argc > 1 ? argv[1] : "lib/PushEmbeddedFont/src/PushEmbeddedFont.cpp";
  const char* actmgr = argc > 2 ? argv[2] : "src/activities/ActivityManager.cpp";

  // 1) Root-cause size: the vendored tinfl_decompressor is ~8.3KB. Any device
  //    call that declares it as a local stacks this much in one frame.
  const size_t decSize = sizeof(tinfl_decompressor);
  printf("sizeof(tinfl_decompressor) = %zu bytes\n", decSize);
  CHECK(decSize >= 8000 && decSize <= 9000,
        "tinfl_decompressor is the expected ~8.3KB (center of the 8.4KB backtrace frame)");

  // 2) Device glue must NOT use the stack-allocating mem_to_mem wrapper, and
  //    must keep the decompressor as a file-static singleton.
  const std::string g = slurp(glue);
  CHECK(!g.empty(), "device glue source readable");
  CHECK(!hasCall(g, "crosspoint_tinfl_decompress_mem_to_mem"),
        "inflateBlock no longer calls stack-allocating mem_to_mem wrapper");
  CHECK(contains(g, "static tinfl_decompressor g_inflateState"),
        "decompressor held as file-static singleton (off the stack)");
  CHECK(contains(g, "tinfl_init(&g_inflateState)"),
        "static decompressor is re-initialized per block");

  // 3) The 72x72 scratch arrays must be static, never stack locals.
  CHECK(contains(g, "static uint8_t cover[72 * 72]"),
        "cover[] is static (not a 5184B stack local)");
  CHECK(contains(g, "static uint8_t packed[72 * 72 / 4 + 4]"),
        "packed[] is static (not a 1308B stack local)");

  // 4) Render task stack headroom raised for the vector-font path.
  const std::string a = slurp(actmgr);
  CHECK(!a.empty(), "ActivityManager source readable");
  CHECK(contains(a, "kRenderTaskStackBytes = 24576"),
        "ActivityManager render stack >= 24KB on vector-font boards");
  CHECK(contains(a, "uxTaskGetStackHighWaterMark"),
        "post-render stack high-water logging present for real-hardware confirmation");

  printf("stack budget: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
