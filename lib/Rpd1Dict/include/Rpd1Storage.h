#pragma once
#include <cstddef>

// On-device RPD1 dictionary facade over the project's thread-safe Storage
// backend. Fixed path /dict/dict.bin (independent of StarDict /dictionaries).
// Wraps the platform-independent rpd1_core parser; sparse index in PSRAM.
namespace rpd1store {

// Max definition bytes returned by a lookup (RPD1_ENTRY_MAX from the core).
static constexpr size_t kDefCap = 1280;

// Open /dict/dict.bin if present and valid. Safe to call repeatedly
// (re-entrant: a failed open backs off briefly to avoid re-scanning a bad file
// on every tap). Returns true when a dictionary is ready.
bool open();
// Close and clear all state (e.g. on SD remount).
void close();
bool ready();

// Look up a word. On hit writes UTF-8 ('\n'-separated) definition into out and
// returns true. Logs word + elapsed ms at the entry (per design spec).
bool lookup(const char* word, char* out, size_t cap);

}  // namespace rpd1store
