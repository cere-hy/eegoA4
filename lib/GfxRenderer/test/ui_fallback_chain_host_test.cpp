// S3c+++ host guard for the UI string-level fallback chain. Compiles the SAME
// UiFallbackChain.h the device firmware (GfxRenderer::resolveTextFontId) uses,
// so the tier priority + cycle guard + terminal backstop cannot drift.
#include "UiFallbackChain.h"

#include <cstdio>
#include <map>
#include <vector>

using uifallbackchain::kMaxFallbackHops;
using uifallbackchain::needsFallback;
using uifallbackchain::resolveChain;

static int checks = 0, red = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++red; std::printf("RED line %d: %s\n", __LINE__, #cond); } } while (0)

// Miniature model of GfxRenderer's fallbackFontMap_ + fontMap coverage.
struct Chain {
  std::map<int, int> fb;      // primary -> next
  std::vector<uint32_t> has;  // font ids whose cmap covers the test codepoint

  int next(int cur, int& out) const {
    auto it = fb.find(cur);
    if (it == fb.end() || it->second == 0) return 0;
    out = it->second;
    return 1;
  }
  bool covers(int id, uint32_t) const {
    for (uint32_t x : has) if (int(x) == id) return true;
    return false;
  }
};

int main() {
  const uint32_t han = 0x4E00;

  // ---- needsFallback: ASCII never redirects; only non-ASCII + primary miss ----
  CHECK(needsFallback('A', /*primaryHas=*/false) == false);
  CHECK(needsFallback(0x7F, false) == false);
  CHECK(needsFallback(han, /*primaryHas=*/true) == false);
  CHECK(needsFallback(han, /*primaryHas=*/false) == true);

  // ---- no chain: stay on primary ----
  {
    Chain c;  // empty map
    CHECK(resolveChain(/*primary=*/10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 10);
  }

  // ---- priority: SD (②) wins over builtin (③) when both cover ----
  // chain: 10(ubuntu) -> 20(SD) -> 30(builtin); both SD+builtin cover han.
  {
    Chain c;
    c.fb[10] = 20; c.fb[20] = 30;
    c.has = {20, 30};
    CHECK(resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 20);
  }

  // ---- Latin-only SD font: SD does NOT cover, builtin covers -> builtin ----
  {
    Chain c;
    c.fb[10] = 20; c.fb[20] = 30;
    c.has = {30};  // SD (20) lacks Han
    CHECK(resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 30);
  }

  // ---- no SD font: primary -> builtin directly ----
  {
    Chain c;
    c.fb[10] = 30;
    c.has = {30};
    CHECK(resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 30);
  }

  // ---- rare glyph no cmap covers: terminal (builtin) is the backstop ----
  // This is the critical case: builtin.ttf cmap misses a rare char, but the
  // builtin variant's PIF1/placeholder secondary handles it. Landing anywhere
  // else (esp. primary) would show empty boxes.
  {
    Chain c;
    c.fb[10] = 20; c.fb[20] = 30;
    c.has = {};  // nobody's cmap covers it
    CHECK(resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 30);
  }

  // ---- cycle guard: primary -> a -> b -> a (cycle) must terminate, not hang ----
  // With no coverage, terminal is the last distinct hop before the cycle.
  {
    Chain c;
    c.fb[10] = 20; c.fb[20] = 30; c.fb[30] = 20;  // 20<->30 cycle
    c.has = {};
    int r = resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                         [&](int id, uint32_t cp) { return c.covers(id, cp); });
    // hop budget bounds it; result must be a registered fallback id, never hang.
    CHECK(r == 20 || r == 30);
    CHECK(kMaxFallbackHops >= 3);  // enough hops for a 3-tier chain
  }

  // ---- self-loop guard: fb[10]=10 breaks inside resolveChain (fb==cur) ----
  {
    Chain c;
    c.fb[10] = 10;
    c.has = {10};  // primary covers, but we force the chain path below
    // resolveChain sees next=10==cur and breaks immediately -> terminal stays primary.
    CHECK(resolveChain(10, han, [&](int cur, int& out) { return c.next(cur, out); },
                       [&](int id, uint32_t cp) { return c.covers(id, cp); }) == 10);
  }

  std::printf("ui_fallback_chain host: %d checks, %d RED\n", checks, red);
  return red == 0 ? 0 : 1;
}
