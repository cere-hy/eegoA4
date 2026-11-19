#pragma once

// S3c-final P0#3 — ordered advance-table insertion, shared by the device font
// code and the host regression test. This is the SAME algorithm both sides run.
//
// ROOT CAUSE this fixes:
//   The original getAdvance() computed `pos = lower_bound(...)` BEFORE growing
//   the vector. Growing via reserve() reallocates and frees the old buffer,
//   leaving `pos` dangling; vector::insert(pos, ...) then ran
//   std::__copy_move_backward over freed/remapped memory. On ESP32-S3 that
//   stale write targeted 0x3c5afffc (Flash-DROM read-only cache window) and
//   raised "Dbus write to cache rejected". Both a 46KB and a 4KB document
//   reproduced it whenever the table crossed a growth boundary.
//
// THE RULE (enforced here, in one place):
//   1. Grow FIRST (reserve), so the buffer is stable.
//   2. THEN compute the insertion position from the stable buffer.
//   3. THEN insert. No iterator ever survives a reallocation.
//
// `canGrow(bytes)` is an allocation gate (returns false -> no insert, caller
// falls back to the slow path). It is injectable so the host test can simulate
// OOM / cap without touching the real allocator.

#include <algorithm>
#include <cstddef>

namespace epd {

// Grow `v` to hold one more element, rounded up to `step`, capped at `ceil`.
// Returns false (leaving `v` untouched) when `canGrow` vetoes the allocation.
template <typename Vec, typename CanGrow>
bool advanceTableReserve(Vec& v, const std::size_t need, const std::size_t step,
                         const std::size_t ceil, CanGrow canGrow) {
  if (v.capacity() >= need) return true;
  std::size_t newCap = ((need + step - 1) / step) * step;
  if (newCap > ceil) newCap = ceil;
  // Cap is HARD: at the ceiling, refuse further growth rather than exceed it.
  // (The old `newCap < need -> newCap = need` let a full table grow past cap.)
  if (newCap < need) return false;
  if (!canGrow(newCap * sizeof(typename Vec::value_type))) return false;
  v.reserve(newCap);
  return true;
}

// Insert `entry` into the cp-ordered table `v`. Entries expose a `.cp` key.
// `canGrow(bytes)` gates allocation; if it returns false, nothing is inserted
// and the function returns false so the caller can use its slow path.
// Returns true on success (new entry inserted OR duplicate value refreshed).
template <typename Vec, typename Entry, typename CanGrow>
bool advanceTableInsert(Vec& v, const Entry& entry, const std::size_t step,
                        const std::size_t ceil, CanGrow canGrow) {
  // 1) Grow FIRST. After this returns, the buffer will not reallocate during
  //    the insert below (capacity >= size+1).
  if (!advanceTableReserve(v, v.size() + 1, step, ceil, canGrow)) return false;
  // 2) NOW compute the position from the stable buffer.
  const auto pos = std::lower_bound(v.begin(), v.end(), entry.cp,
                                    [](const Entry& e, const uint32_t c) { return e.cp < c; });
  if (pos != v.end() && pos->cp == entry.cp) {
    const_cast<Entry&>(*pos) = entry;  // refresh duplicate; no reallocation
    return true;
  }
  // 3) Insert without any dangling iterator.
  v.insert(pos, entry);
  return true;
}

}  // namespace epd
