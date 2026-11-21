#pragma once

#include <cstddef>
#include <cstdint>

// S3cPF8c — CSS load gate and user-visible failure state.
//
// These values were moved out of Epub::parseCssFiles() so the boundary can be
// unit-tested on the host without pulling in the whole embedded Epub.cpp.
// Keep the two constants in sync with the firmware build; the host test
// (test/css_load_guard) asserts them directly.
namespace CssLoadGuard {

// Minimum free heap required before attempting to parse source stylesheets.
//
// Provenance: the original value was 64 KiB (65536), introduced by upstream
// commit e1074a84 (#952). It had no measurement behind it — a conservative
// guess made while fixing an unrelated 250KB-CSS crash. S3cPF8c host
// measurement (real CssParser.cpp + malloc tracker) put the true peak for a
// real book's three Calibre stylesheets at ~11 KB (14 KB with one large table;
// 22 KB for three large tables). The three pools grow on demand via
// ensureCapacity and the largest single allocation on the real-book path is
// 8192 B (well under the measured MaxAlloc 31732 and free heap 62-65 KB).
// 24 KiB therefore covers the real peak with margin and is far below the
// available heap, breaking the deadlock where 64 KB was never reachable.
constexpr uint32_t MIN_HEAP_FOR_CSS_PARSING = 24 * 1024;  // 24576

// Maximum uncompressed CSS file size we will parse. This is the real memory
// defence from upstream issue #947 (250KB CSS crash). S3cPF8c must NOT change
// it: lowering the heap gate must not reopen that crash vector.
constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB

// Why CSS styles were not loaded. Reported to the UI so the failure is no
// longer silent (previously only LOG_ERR, load() still returned true and the
// reader showed no sign embedded styles were inactive). None means either no
// CSS files, or a cache hit, or a fully successful parse.
enum class Failure : uint8_t {
  None = 0,
  LowHeap,       // skipped: free heap below MIN_HEAP_FOR_CSS_PARSING
  FileTooLarge,  // skipped: a stylesheet exceeded MAX_CSS_FILE_SIZE
  ParseError,    // a stylesheet failed to parse / read
};

// Pure predicates for the two skip conditions in Epub::parseCssFiles().
inline bool isHeapInsufficient(const uint32_t freeHeap) {
  return freeHeap < MIN_HEAP_FOR_CSS_PARSING;
}

inline bool isFileTooLarge(const size_t fileSize) {
  return fileSize > MAX_CSS_FILE_SIZE;
}

// Keep the first recorded failure (the dominant reason the user should act on)
// and ignore later ones. Pure so the accumulation logic is host-testable.
inline Failure mergeFailure(const Failure current, const Failure incoming) {
  return current == Failure::None ? incoming : current;
}

}  // namespace CssLoadGuard
