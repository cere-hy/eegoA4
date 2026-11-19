// S3c-final P0#3 — host regression for the ordered advance table.
// This test compiles the SAME AdvanceTable.h the device font uses, so it
// directly covers the insert/grow path that crashed on hardware (host tests
// before this only exercised SdCardFont::getAdvance, never TtfEpdFont's table).
//
// Asserts: growth across the 64-step boundaries, ordering, cap enforcement,
// OOM veto, duplicate refresh, and concurrent inserts — with a canary proving
// the OLD (compute-pos-before-reserve) pattern invalidates iterators.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "AdvanceTable.h"

struct Entry {
  uint32_t cp;
  uint16_t adv12_4;
};

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++fails; std::printf("  RED: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const auto alwaysGrow = [](size_t) { return true; };

int main() {
  const size_t kStep = 64;
  const size_t kCap = 4096;

  // (1) Insert 5000 shuffled codepoints; cap is 4096 so exactly 4096 land.
  {
    std::vector<Entry> v;
    std::vector<uint32_t> cps;
    for (uint32_t i = 0; i < 5000; ++i) cps.push_back(0x4E00u + i * 7u);
    std::rotate(cps.begin(), cps.begin() + 1234, cps.end());  // scramble order
    for (uint32_t cp : cps) epd::advanceTableInsert(v, Entry{cp, uint16_t(10 + cp % 50)}, kStep, kCap, alwaysGrow);
    CHECK(v.size() == kCap, "table capped at 4096 (no overflow past cap)");
    CHECK(v.capacity() <= kCap, "capacity never exceeds cap");
    bool sorted = true;
    for (size_t i = 1; i < v.size(); ++i) sorted &= v[i - 1].cp < v[i].cp;
    CHECK(sorted, "table strictly cp-ordered after many reallocations");
    // Spot-check every stored value matches the source formula.
    bool valuesOk = true;
    for (const auto& e : v) valuesOk &= (e.adv12_4 == uint16_t(10 + e.cp % 50));
    CHECK(valuesOk, "all advances correct after reallocation/insert");
  }

  // (2) Growth boundaries specifically: insert 1..300 (spans 64/128/192/256).
  {
    std::vector<Entry> v;
    std::vector<const Entry*> seen;
    for (uint32_t i = 1; i <= 300; ++i) {
      CHECK(epd::advanceTableInsert(v, Entry{i, uint16_t(i)}, kStep, kCap, alwaysGrow), "insert at boundary %u", i);
      seen.push_back(&v.back());
    }
    // Re-fetch by index; every earlier address may have moved — that is fine,
    // as long as the TABLE content is intact (no stale iterator was used to
    // decide placement).
    for (uint32_t i = 1; i <= 300; ++i) {
      auto it = std::lower_bound(v.begin(), v.end(), i, [](const Entry& e, uint32_t c) { return e.cp < c; });
      CHECK(it != v.end() && it->cp == i && it->adv12_4 == i, "boundary insert %u intact", i);
    }
    CHECK(v.size() == 300, "boundary: 300 entries");
  }

  // (3) OOM veto: canGrow=false must leave the table untouched.
  {
    std::vector<Entry> v;
    const auto veto = [](size_t) { return false; };
    CHECK(!epd::advanceTableInsert(v, Entry{1, 32}, kStep, kCap, veto), "OOM veto rejects first insert");
    CHECK(v.empty(), "OOM veto leaves table empty");
    // Now allow it.
    CHECK(epd::advanceTableInsert(v, Entry{1, 32}, kStep, kCap, alwaysGrow), "allowed insert succeeds");
    CHECK(epd::advanceTableInsert(v, Entry{2, 33}, kStep, kCap, veto),
          "veto with spare capacity: reserve not needed, insert proceeds");
    CHECK(v.size() == 2, "two entries after mixed veto");
  }

  // (4) Duplicate cp refreshes value, no size growth.
  {
    std::vector<Entry> v;
    epd::advanceTableInsert(v, Entry{100, 40}, kStep, kCap, alwaysGrow);
    epd::advanceTableInsert(v, Entry{100, 55}, kStep, kCap, alwaysGrow);
    CHECK(v.size() == 1, "duplicate does not grow table");
    CHECK(v[0].adv12_4 == 55, "duplicate refreshes advance");
  }

  // (5) Canary: the OLD pattern (lower_bound BEFORE reserve) leaves `pos`
  //     pointing into the freed buffer after growth. Demonstrates the crash
  //     class this test guards against.
  {
    std::vector<Entry> v;
    v.reserve(2);
    v.push_back(Entry{1, 1});
    v.push_back(Entry{3, 3});
    const Entry* stale = &*std::lower_bound(v.begin(), v.end(), 2u,
        [](const Entry& e, uint32_t c) { return e.cp < c; });  // points into old buffer
    const Entry* oldData = v.data();
    epd::advanceTableReserve(v, v.size() + 1, kStep, kCap, alwaysGrow);  // reallocates
    CHECK(v.data() != oldData, "reserve reallocated the buffer");
    CHECK(stale != &v[1], "pre-reserve iterator no longer aliases the new buffer (would be dangling)");
    // The shared insert helper computes pos AFTER reserve — safe by construction.
    CHECK(epd::advanceTableInsert(v, Entry{2, 2}, kStep, kCap, alwaysGrow), "safe insert after growth");
    CHECK(v.size() == 3 && v[1].cp == 2, "safe insert placed entry correctly");
  }

  // (6) Concurrent inserts from 4 threads (distinct cp ranges) — no crash,
  //     all entries present. NOTE: real TtfEpdFont serialises under RenderLock;
  //     this is a robustness check of the container algorithm itself.
  //     Each thread uses its OWN table (the device uses per-face per-task-safe
  //     single-writer tables); a shared table would need external locking.
  {
    std::atomic<bool> ok{true};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
      ts.emplace_back([&, t] {
        std::vector<Entry> v;
        for (uint32_t i = 0; i < 1000; ++i) {
          uint32_t cp = uint32_t(t) * 100000u + i;
          if (!epd::advanceTableInsert(v, Entry{cp, 16}, kStep, kCap, alwaysGrow)) ok.store(false);
        }
        bool sorted = true;
        for (size_t i = 1; i < v.size(); ++i) sorted &= v[i - 1].cp < v[i].cp;
        if (v.size() != 1000 || !sorted) ok.store(false);
      });
    }
    for (auto& th : ts) th.join();
    CHECK(ok.load(), "4 concurrent independent tables: all inserts ordered & complete");
  }

  std::printf("AdvanceTable host: %d checks, %d RED\n", checks, fails);
  return fails ? 1 : 0;
}
