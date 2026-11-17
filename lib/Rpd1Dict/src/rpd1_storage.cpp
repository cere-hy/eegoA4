#include "Rpd1Storage.h"

extern "C" {
#include "rpd1_core.h"
}

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif

namespace rpd1store {

namespace {

constexpr const char* TAG = "RPD1";
constexpr const char* DICT_PATH = "/dict/dict.bin";

// File handle kept open across lookups (upstream keeps FILE* open too).
HalFile s_file;
bool s_fileOpen = false;
rpd1_dict_t s_dict;
rpd1_sparse_t* s_sparse = nullptr;  // PSRAM
rpd1_io_t s_io;

static uint32_t now_ms() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return (uint32_t)(esp_timer_get_time() / 1000);
#else
  return 0;
#endif
}

// --- rpd1_io_t implementation over HalFile (thread-safe via Storage lock) ---
bool io_read_at(uint32_t off, void* buf, size_t len, void*) {
  if (!s_fileOpen) return false;
  if (!s_file.seekSet((size_t)off)) return false;
  return s_file.read(buf, len) == (int)len;
}

bool io_file_size(uint64_t* out, void*) {
  if (!s_fileOpen) return false;
  *out = s_file.fileSize64();
  return true;
}

void closeFile() {
  if (s_fileOpen) {
    s_file.close();
    s_fileOpen = false;
  }
}

}  // namespace

bool ready() { return rpd1_ready(&s_dict); }

bool open() {
  if (ready()) return true;
  close();
  LOG_INF(TAG, "open %s", DICT_PATH);
  if (!Storage.exists(DICT_PATH)) {
    LOG_INF(TAG, "not present: %s (dictionary unavailable)", DICT_PATH);
    return false;
  }
  s_file = Storage.open(DICT_PATH, O_RDONLY);
  if (!s_file) {
    LOG_ERR(TAG, "open failed: %s", DICT_PATH);
    return false;
  }
  s_fileOpen = true;

  s_io.read_at = &io_read_at;
  s_io.file_size = &io_file_size;
  s_io.ctx = nullptr;

  s_sparse = (rpd1_sparse_t*)heap_caps_calloc(RPD1_SPARSE_MAX, sizeof(rpd1_sparse_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_sparse) {
    LOG_ERR(TAG, "no PSRAM for sparse index (%u slots)", (unsigned)RPD1_SPARSE_MAX);
    close();
    return false;
  }

  const uint32_t t0 = now_ms();
  const bool ok = rpd1_open(&s_dict, &s_io, s_sparse, RPD1_SPARSE_MAX);
  const uint32_t elapsed = now_ms() - t0;
  if (!ok) {
    LOG_ERR(TAG, "invalid/mismatched RPD1 file: %s (%ums) - dictionary unavailable", DICT_PATH, elapsed);
    close();
    return false;
  }
  LOG_INF(TAG, "ready: entries=%u block_log2=%u sparse=%u (%ums)",
          (unsigned)s_dict.entries, (unsigned)s_dict.block_log2,
          (unsigned)s_dict.sparse_count, elapsed);
  return true;
}

void close() {
  rpd1_close(&s_dict);
  if (s_sparse) {
    heap_caps_free(s_sparse);
    s_sparse = nullptr;
  }
  closeFile();
}

bool lookup(const char* word, char* out, size_t cap) {
  if (!ready() && !open()) return false;
  const uint32_t t0 = now_ms();
  const bool hit = rpd1_lookup(&s_dict, word, out, cap);
  const uint32_t elapsed = now_ms() - t0;
  LOG_INF(TAG, "lookup word='%s' %s %ums", word ? word : "", hit ? "HIT" : "MISS", elapsed);
  return hit;
}

}  // namespace rpd1store
