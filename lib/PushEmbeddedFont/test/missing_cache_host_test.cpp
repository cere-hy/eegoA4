// S3cPF5 host guard for the confirmed-absent codepoint cache. Compiles the
// SAME PushEmbeddedFont.h the firmware uses, so the de-dup / short-circuit
// contract cannot drift (root cause of built-in mode's super-linear cost).
#include "PushEmbeddedFont.h"

#include <cstdio>

using pushembedfont::FallbackContext;
using pushembedfont::kMissingCacheCap;

static int checks = 0, red = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++red; std::printf("RED line %d: %s\n", __LINE__, #c); } } while (0)

int main() {
  FallbackContext ctx;

  // A fresh context has no confirmed absences.
  CHECK(!ctx.isMissing(0x1F600u));
  CHECK(!ctx.isMissing(0x4E00u));

  // First tier-④ encounter: new -> logged, recorded; second: not new, cached.
  CHECK(ctx.markMissing(0x1F600u) == true);
  CHECK(ctx.isMissing(0x1F600u) == true);
  CHECK(ctx.markMissing(0x1F600u) == false);  // no second log / no chain re-run
  CHECK(ctx.missingCount == 1);

  // Distinct codepoints coexist; unrelated lookups unaffected.
  CHECK(ctx.markMissing(0x1F4A9u) == true);
  CHECK(ctx.isMissing(0x4E00u) == false);
  CHECK(ctx.missingCount == 2);

  // Fill to capacity: never overflows, extra entries are simply not cached.
  for (uint32_t cp = 0x10000u; cp < 0x10000u + kMissingCacheCap + 32u; ++cp) ctx.markMissing(cp);
  CHECK(ctx.missingCount <= kMissingCacheCap);
  CHECK(ctx.isMissing(0x10000u) == true);

  // Rebuild / SD-tier change clears the set so newly available glyphs are
  // re-evaluated instead of being stuck as absent.
  ctx.resetMissing();
  CHECK(ctx.missingCount == 0);
  CHECK(ctx.isMissing(0x1F600u) == false);
  CHECK(ctx.markMissing(0x1F600u) == true);

  std::printf("MissingCache host: %d checks, %d RED\n", checks, red);
  return red ? 1 : 0;
}
