// S3c-plus2 host guard for the SD-vector fallback chain. Compiles the SAME
// FallbackChain.h the device firmware uses, so tier order + recursion guards
// cannot drift (the class of bug behind the three host-green/device-crash cycles).
#include "FallbackChain.h"

#include <cstdio>
#include <cstdlib>

using pushembedfont::fallbackchain::Availability;
using pushembedfont::fallbackchain::Tier;
using pushembedfont::fallbackchain::choose;
using pushembedfont::fallbackchain::sdFontAllowed;

static int checks = 0, red = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++red; std::printf("RED line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
  // ---- tier order: ① builtin vector > ② PIF1 > ③ SD > ④ placeholder ----
  CHECK(choose({true, true, true}) == Tier::kBuiltinVector);
  CHECK(choose({true, true, false}) == Tier::kBuiltinVector);
  CHECK(choose({true, false, true}) == Tier::kBuiltinVector);
  CHECK(choose({false, true, true}) == Tier::kPif1);
  CHECK(choose({false, true, false}) == Tier::kPif1);
  CHECK(choose({false, false, true}) == Tier::kSdFont);
  CHECK(choose({false, false, false}) == Tier::kPlaceholder);

  // ---- recursion guard: tier③ self-reference must be disabled ----
  // SD-vector face: candidate sdFont == this face -> forbidden.
  CHECK(sdFontAllowed(true, /*isSelf=*/true) == false);
  CHECK(sdFontAllowed(false, /*isSelf=*/true) == false);
  // PUSH/FAST face: tier③ points at a DIFFERENT (SD) face -> allowed.
  CHECK(sdFontAllowed(true, /*isSelf=*/false) == true);
  CHECK(sdFontAllowed(false, /*isSelf=*/false) == false);

  // ---- SD-vector ctx invariant: builtin① on, PIF1② on, SD③ off ----
  Availability sdVector{/*builtinVector=*/true, /*pif1=*/true,
                        /*sdFont=*/sdFontAllowed(true, /*isSelf=*/true)};
  CHECK(sdVector.sdFont == false);
  CHECK(choose(sdVector) == Tier::kBuiltinVector);
  // Even if ① and ② both miss, SD-vector face must go straight to placeholder,
  // never to tier③ (which would recurse).
  Availability sdVectorMiss{false, false, sdFontAllowed(true, true)};
  CHECK(choose(sdVectorMiss) == Tier::kPlaceholder);

  // ---- PUSH/FAST ctx invariant: builtin① off (self is primary), PIF1② on, SD③ on ----
  Availability pushFast{/*builtinVector=*/false, /*pif1=*/true,
                        /*sdFont=*/sdFontAllowed(true, /*isSelf=*/false)};
  CHECK(pushFast.sdFont == true);
  CHECK(choose(pushFast) == Tier::kPif1);
  Availability pushFastRare{false, false, sdFontAllowed(true, false)};
  CHECK(choose(pushFastRare) == Tier::kSdFont);

  std::printf("fallback_chain host: %d checks, %d RED\n", checks, red);
  return red == 0 ? 0 : 1;
}
