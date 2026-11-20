#pragma once
// S3c-plus2: shared fallback-tier decision logic. Compiled by BOTH the device
// firmware (via PushEmbeddedFont.cpp) and the host test, so the tier order and
// — critically — the self-reference / recursion guards cannot drift apart
// (the same class of bug that made three successive host-green, device-crash
// cycles in the advance table). Pure integer logic, no allocation, no platform
// headers.

#include <cstdint>

namespace pushembedfont {
namespace fallbackchain {

// Which source supplies the next glyph / advance for a codepoint.
enum class Tier : uint8_t {
  kBuiltinVector = 1,  // ① builtin.ttf vector face (SD-vector path only)
  kPif1 = 2,           // ② PIF1 Han bitmap
  kSdFont = 3,         // ③ another SD font (DISABLED for the SD-vector face itself)
  kPlaceholder = 4,    // ④ replacement glyph
  kNone = 0,
};

// Availability of each tier for a given face's context.
struct Availability {
  bool builtinVector;  // ctx.builtinFace != nullptr
  bool pif1;           // PIF1 reader decoded this codepoint
  bool sdFont;         // ctx.sdFont != nullptr AND != this face (self-ref guard)
};

// Deterministic first-available tier. The device handler asks these tiers in
// exactly this order; the host test asserts the same table, so a future edit
// that reintroduces tier③ on the SD-vector face (recursion) is caught here.
inline Tier choose(Availability a) {
  if (a.builtinVector) return Tier::kBuiltinVector;
  if (a.pif1) return Tier::kPif1;
  if (a.sdFont) return Tier::kSdFont;
  return Tier::kPlaceholder;
}

// Guard: an SD-vector face must NEVER allow its own face to serve as tier③.
// `isSelf` is true when the candidate tier③ font pointer == the requesting
// face. Returns the safe sdFont flag to feed into Availability.
inline bool sdFontAllowed(bool hasSdFont, bool isSelf) { return hasSdFont && !isSelf; }

}  // namespace fallbackchain
}  // namespace pushembedfont
