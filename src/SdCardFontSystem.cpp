#include "SdCardFontSystem.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PushEmbeddedFont.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

#if CROSSPOINT_VECTOR_FONTS
// Stable, non-zero renderer font id for a vector family at a size (FNV-1a of
// name + size). 0 is the "not found" sentinel, so bump collisions to 1.
int computeTtfFontId(const char* familyName, uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (const char* p = familyName; p && *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x54544600u;  // "TTF\0" salt to avoid colliding with cpfont ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif  // CROSSPOINT_VECTOR_FONTS

}  // namespace

// Out-of-line ctor/dtor: TtfEpdFont is complete here, so unique_ptr<TtfEpdFont>
// can be constructed/destroyed. (Declared in the header where it is only
// forward-declared.)
SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so UI text
// in scripts the built-ins lack (CJK, Greek, Cyrillic, ...) matches the
// surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::refreshSdVectorFallback() {
#if CROSSPOINT_VECTOR_FONTS
  // Both faces must be live. The SD-vector face (ttf_) gets the chain; tier①
  // is the embedded builtin face (pushTtf_). Tier③ deliberately unset.
  if (ttf_ && pushTtf_) {
    pushembedfont::installSdVectorFallback(*ttf_, sdTtfFallback_, *pushTtf_);
  }
#endif
}

int SdCardFontSystem::ensurePushFont(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  // S3c+ tier③ points at the active SD (LXGW) face, PER-FACE via pushFallback_.
  // PUSH and FAST share this face + this ctx, so neither can clobber the other.
  pushembedfont::setSdFallback(pushFallback_, ttf_.get());
  const uint8_t pt = SETTINGS.fontPointSize;
  if (pushTtf_ && pushPt_ == pt && pushFontId_ != 0) return pushFontId_;

  // b65-s3c-pf3 D2: log every embedded-face (re)build with its cause so the
  // next push tells "first load" from "point-size change" without guessing.
  LOG_DBG("SDFS", "push face rebuilt: pt=%u reason=%s", static_cast<unsigned>(pt),
          pushTtf_ ? "字号变化" : "首次");

  if (pushTtf_) {
    renderer.unregisterTtfFont(pushFontId_);
    renderer.removeFont(pushFontId_);
    pushembedfont::setEmbeddedFace(nullptr);  // S3c+: drop global fallback ref before free
    pushTtf_.reset();
    pushFontId_ = 0;
  }
  auto f = makeUniqueNoThrow<TtfEpdFont>();
  if (!f) {
    LOG_ERR("SDFS", "OOM: TtfEpdFont for PUSH embedded font");
    return 0;
  }
  // Resident builtin.ttf: embed bytes are borrowed and outlive this object.
  f->addResidentSource(TtfEpdFont::Regular, pushembedfont::builtinTtfData(),
                       static_cast<uint32_t>(pushembedfont::builtinTtfLen()));
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 512 * 1024 : 48 * 1024;
  const uint16_t maxGlyphs = havePsram ? 2400 : 512;
  if (!f->load(pt, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
    LOG_ERR("SDFS", "PUSH embedded font load failed @%upt", pt);
    return 0;
  }
  f->build(" ");
  pushembedfont::installInto(*f, pushFallback_);  // install PIF1 + SD + placeholder secondary
  const int id = computeTtfFontId("\x01push_embedded", pt);
  renderer.insertFont(id, f->family());
  renderer.registerTtfFont(id, f.get());
  // Match theme: PUSH body uses the reader point size; UI-size variants of the
  // embedded font are not needed (PUSH text is body only).
  pushTtf_ = std::move(f);
  pushFontId_ = id;
  pushPt_ = pt;
  // S3c+ global fallback: point the stable holder at the live embedded face so
  // built-in Noto Sans can serve CJK misses from tiers ①-③. Updated on every
  // point-size rebuild; cleared on teardown below.
  pushembedfont::setEmbeddedFace(pushTtf_.get());
  // S3c-plus2: embedded face just became available — wire the SD-vector face's
  // fallback chain now (no-op if no SD vector font is loaded).
  refreshSdVectorFallback();
  LOG_INF("SDFS", "PUSH embedded font ready @%upt (id %d, cache %u KB)", pt, id, (unsigned)(cacheBytes / 1024));
  return id;
#else
  (void)renderer;
  return 0;  // no vector path: PUSH falls back to the reader's built-in font
#endif
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  registry_.discover();
  renderer_ = &renderer;
  cpfontVariants_.reserve(MAX_CPFONT_VARIANTS);
#if CROSSPOINT_VECTOR_FONTS
  ttfVariants_.reserve(MAX_TTF_VARIANTS);
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->setReleaseCallback(
        [](void* ctx) {
          auto* self = static_cast<SdCardFontSystem*>(ctx);
          if (self->renderer_) self->clearTtfVariants(*self->renderer_);
        },
        this);
  }
#endif
  FontVariantProvider provider;
  provider.ctx = this;
  provider.resolve = [](void* ctx, const int fontId, const float scale) {
    return static_cast<SdCardFontSystem*>(ctx)->resolveVariant(fontId, scale);
  };
  provider.load = [](void* ctx, const int fontId) { return static_cast<SdCardFontSystem*>(ctx)->loadVariant(fontId); };
  renderer.setFontVariantProvider(provider);

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // S3c+: Built-in (Fast) resolves to the shared PUSH/embedded face id. The
  // face is prepared at the end of begin(); until then fastFontId() is 0 and
  // getReaderFontId() falls through harmlessly.
  SETTINGS.fastFontIdResolver = [](void* ctx) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->fastFontId();
  };
  SETTINGS.fastFontResolverCtx = this;

  // S3c+++: build the three resident built-in UI variants and establish the
  // terminal fallback chain BEFORE any SD font load, so setupUiFallbacks /
  // setupTtfUiFallbacks relink against live builtin ids. With no SD font this
  // is the final chain: ubuntu → builtin (8/10/12pt) → placeholder.
  setupBuiltinUiFallbacks(renderer);

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
#if CROSSPOINT_VECTOR_FONTS
      if (family->vector) {
        // Vector (.ttf/.otf) families load through the FreeInkFont path; the
        // .cpfont manager below rejects them ("Invalid magic bytes") and would
        // wipe the user's selection on every boot. loadTtfFamily keeps the
        // selection on transient failures and registers UI fallbacks itself.
        loadTtfFamily(*family, renderer, /*registryWasDirty=*/false);
      } else
#endif
          if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  // S3c: prepare the PUSH embedded font once at startup so the home-screen
  // PUSH button works immediately; rebuilt lazily on point-size change via
  // ensurePushFont() from the reader/TextSettings paths.
  ensurePushFont(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  // b65-s3c-cn-pushfont2 项C: SETTINGS.loadFromFile() runs AFTER begin() at
  // boot, so the chain built in begin() used the struct-default fontFamily,
  // not the persisted one. Re-link once here (idempotent clear+set) so the
  // FAST/normal UI-fallback order always reflects the loaded setting before
  // any early return below. Later loadTtfFamily/setupUiFallbacks re-link again.
  relinkUiFallbacks(renderer);

  const char* wantedFamily = SETTINGS.sdFontFamilyName;

#if CROSSPOINT_VECTOR_FONTS
  // Vector (.ttf/.otf) family selected: route through the FreeInkFont path and
  // drop any pre-rasterized (.cpfont) font that was loaded.
  if (wantedFamily[0] != '\0') {
    const auto* wantedFam = registry_.findFamily(wantedFamily);
    if (wantedFam && wantedFam->vector) {
      if (!manager_.currentFamilyName().empty()) unloadCpfonts(renderer);
      loadTtfFamily(*wantedFam, renderer, registryWasDirty);
      return;
    }
  }
  // Not on a vector family — ensure any previously-loaded TTF font is released
  // before the pre-rasterized/built-in path below takes over.
  if (!ttfFamily_.empty()) unloadTtf(renderer);
#endif

  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      unloadCpfonts(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      unloadCpfonts(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    unloadCpfonts(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupBuiltinUiFallbacks(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  // Build each built-in UI size ONCE from the resident builtin.ttf. These faces
  // are the terminal fallback of the UI string-level chain: their primary is
  // builtin.ttf (1126 common Han glyphs), and installInto() adds the mature
  // secondary chain (PIF1 bitmap → SD tier③ → placeholder) for any codepoint
  // the cmap lacks. Identical construction to the PUSH/FAST face, so no new
  // fallback logic — only new sized instances. A 16 KB / 384-glyph cache per
  // variant is ample for short UI strings (titles/rows), ~48 KB total.
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    if (builtinUi_[i]) continue;  // resident: idempotent
    const auto& ui = kUiFontSizes[i];
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for builtin UI fallback @%upt", ui.pointSize);
      continue;
    }
    f->addResidentSource(TtfEpdFont::Regular, pushembedfont::builtinTtfData(),
                         static_cast<uint32_t>(pushembedfont::builtinTtfLen()));
    if (!f->load(ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384)) {
      LOG_ERR("SDFS", "builtin UI fallback load failed @%upt", ui.pointSize);
      continue;
    }
    f->build(" ");
    // Own ctx per variant: builtinFace=nullptr (this face IS the builtin
    // primary), sdFont=nullptr (tier③ off — the string-level chain already
    // tried the user's SD font in tier② before reaching here).
    pushembedfont::installInto(*f, builtinUiCtx_[i]);
    const int id = computeTtfFontId((std::string("\x01builtin_ui") + char(ui.pointSize)).c_str(), ui.pointSize);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    builtinUiIds_[i] = id;
    builtinUi_[i] = std::move(f);
    LOG_DBG("SDFS", "builtin UI fallback @%upt ready (id %d)", ui.pointSize, id);
  }
  relinkUiFallbacks(renderer);
#endif  // CROSSPOINT_VECTOR_FONTS
}

void SdCardFontSystem::relinkUiFallbacks(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  // Chain topology (device + host share UiFallbackChain.h decision logic):
  //   Normal (SD user font is the active reader font):
  //     ubuntu (primary) → sdUiIds_[i] (if any) → builtinUiIds_[i] → end
  //     so UI Han renders in the user's chosen font (LXGW etc.) — matches body.
  //   Built-in (Fast) active reader font:
  //     ubuntu (primary) → builtinUiIds_[i] (main) → sdUiIds_[i] (secondary) → end
  //     so UI Han renders in the builtin black face — matches the FAST body —
  //     while the SD font stays as a secondary tier for codepoints absent from
  //     builtin.ttf (rare glyphs still resolve via SD, never a tofu box).
  // b65-s3c-cn-pushfont2 项C: "显示什么渲染什么" — FAST 下 UI 主 fallback
  // 改为内置 variant, SD 降为内置 variant 的二级 fallback。两种顺序的链末
  // 都仍是 builtin 体系/builtin variant 的 PIF1 + tier④ 占位符, 故极生僻字
  // 兜底不变。resolveChain / resolveTextFontId 本体不动。
  const bool fastActive = (SETTINGS.fontFamily == CrossPointSettings::BUILTIN_FAST);
  renderer.clearFallbackFonts();  // one-shot: 3-entry map, clears all stale links
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const int primary = kUiFontSizes[i].fontId;
    const int builtin = builtinUiIds_[i];
    const int sd = sdUiIds_[i];
    if (fastActive) {
      // ubuntu → builtin(main) → sd(secondary, rare-glyph tier) → end
      if (builtin != 0) renderer.setFallbackFont(primary, builtin);
      if (sd != 0) {
        if (builtin != 0) {
          renderer.setFallbackFont(builtin, sd);  // builtin(main) → sd(secondary)
        } else {
          renderer.setFallbackFont(primary, sd);  // builtin absent: sd is the only tier
        }
      }
    } else {
      // ubuntu → sd(if any) → builtin → end (unchanged user-font-first chain)
      if (sd != 0) renderer.setFallbackFont(primary, sd);
      if (builtin != 0) {
        if (sd != 0) {
          renderer.setFallbackFont(sd, builtin);       // tier② → tier③
        } else {
          renderer.setFallbackFont(primary, builtin);  // no SD: builtin is tier②
        }
      }
    }
  }
#endif  // CROSSPOINT_VECTOR_FONTS
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack,
  // matching the non-Latin interval presets the SD-font converter ships
  // (docs/sd-card-fonts.md).
  static constexpr uint32_t kFallbackProbes[] = {
      0x4E00,  // CJK Unified Ideographs (Han)
      0x3042,  // Hiragana
      0x30A2,  // Katakana
      0xAC00,  // Hangul Syllables
      0x03B1,  // Greek
      0x0430,  // Cyrillic
      0x05D0,  // Hebrew
      0x0627,  // Arabic
      0x0531,  // Armenian
      0x10D0,  // Georgian
      0x1200,  // Ethiopic
      0x13A0,  // Cherokee
      0x2D30,  // Tifinagh
      0x0259,  // IPA Extensions (ə)
  };
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    // Use the persisted CSS variant id so UI and reader text share one prewarm set.
    const int id = variantFontId(manager_.getFontId(familyName), ui.pointSize);
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize, id);
    sdUiIds_[i] = sdFontId;  // S3c+++: remember for chain relink
    if (sdFontId == 0) {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
  relinkUiFallbacks(renderer);  // S3c+++: ubuntu → SD → builtin → end
}

int SdCardFontSystem::variantFontId(const int readerFontId, const uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>(static_cast<uint32_t>(readerFontId) >> shift);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x56415200u;  // "VAR\0" salt keeps variants apart from reader and UI ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

void SdCardFontSystem::unloadCpfonts(GfxRenderer& renderer) {
  cpfontVariants_.clear();  // the manager owns and frees the fonts themselves
  manager_.unloadAll(renderer);
  // S3c+++: cpfont UI fallbacks are gone; drop stale ids and relink back to
  // the resident built-in variants. Idempotent if a reload follows immediately.
  for (int& id : sdUiIds_) id = 0;
  relinkUiFallbacks(renderer);
}

int SdCardFontSystem::resolveVariant(const int fontId, const float scale) {
  if (const int builtin = builtinReaderFontVariant(fontId, scale); builtin != 0) {
    return renderer_->getFontMap().count(builtin) ? builtin : 0;
  }
#if CROSSPOINT_VECTOR_FONTS
  if (ttf_ && fontId == ttfFontId_) {
    // Vector faces render at any size.
    const int pointSize = std::clamp(static_cast<int>(ttfPointSize_ * scale + 0.5f), 6, 72);
    if (pointSize == ttfPointSize_) return fontId;
    const int id = variantFontId(fontId, static_cast<uint8_t>(pointSize));
    return loadTtfVariant(static_cast<uint8_t>(pointSize), id) ? id : 0;
  }
#endif
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty() || fontId != manager_.getFontId(familyName)) return 0;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;

  // Pick among the sizes installed for the family: shrinking takes the largest size at or
  // below the target so small print never rounds back up to body size; growing takes the
  // nearest.
  const uint8_t base = manager_.currentPointSize();
  const float target = base * scale;
  uint8_t chosen = base;
  if (scale < 1.0f) {
    uint8_t atOrBelow = 0;
    uint8_t smallest = base;
    for (const auto& file : family->files) {
      if (file.pointSize <= target && file.pointSize > atOrBelow) atOrBelow = file.pointSize;
      smallest = std::min(smallest, file.pointSize);
    }
    chosen = atOrBelow != 0 ? atOrBelow : smallest;
  } else {
    for (const auto& file : family->files) {
      if (std::fabs(file.pointSize - target) < std::fabs(chosen - target)) chosen = file.pointSize;
    }
  }
  if (chosen == base) return fontId;
  const int id = variantFontId(fontId, chosen);
  return loadCpfontVariant(*family, chosen, id) ? id : 0;
}

bool SdCardFontSystem::loadVariant(const int fontId) {
#if CROSSPOINT_VECTOR_FONTS
  if (ttf_) {
    for (int pointSize = 6; pointSize <= 72; ++pointSize) {
      if (variantFontId(ttfFontId_, static_cast<uint8_t>(pointSize)) == fontId) {
        return loadTtfVariant(static_cast<uint8_t>(pointSize), fontId);
      }
    }
    return false;
  }
#endif
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return false;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return false;
  const int readerFontId = manager_.getFontId(familyName);
  for (const auto& file : family->files) {
    if (variantFontId(readerFontId, file.pointSize) == fontId) {
      return loadCpfontVariant(*family, file.pointSize, fontId);
    }
  }
  return false;
}

bool SdCardFontSystem::loadCpfontVariant(const SdCardFontFamilyInfo& family, const uint8_t pointSize,
                                         const int fontId) {
  for (auto& variant : cpfontVariants_) {
    if (variant.fontId == fontId) {
      variant.lastUse = ++variantClock_;
      return true;
    }
  }
  // UI fallback sizes stay resident and must not enter the variant eviction list.
  if (manager_.getFontId(family.name, pointSize) == fontId) return true;
  // Each size keeps its interval tables resident (small for Latin, tens of KB for broad
  // CJK subsets), so skip it on a tight heap: the text then stays at the reader size.
  static constexpr size_t MIN_VARIANT_FREE_BLOCK = 48 * 1024;
  if (ESP.getMaxAllocHeap() < MIN_VARIANT_FREE_BLOCK) {
    LOG_DBG("SDFS", "Skipping %upt variant: largest free block %u", pointSize,
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  if (cpfontVariants_.size() >= MAX_CPFONT_VARIANTS) {
    const auto oldest = std::min_element(cpfontVariants_.begin(), cpfontVariants_.end(),
                                         [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; });
    manager_.unloadFont(*renderer_, oldest->fontId);
    cpfontVariants_.erase(oldest);
  }
  if (!manager_.loadFamilyVariant(family, *renderer_, pointSize, fontId)) return false;
  cpfontVariants_.push_back({fontId, ++variantClock_});
  LOG_DBG("SDFS", "Loaded %s %upt variant (heap free %u)", family.name.c_str(), pointSize,
          static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
#if CROSSPOINT_VECTOR_FONTS
  // A loaded vector (.ttf) family answers first — it isn't in the .cpfont manager.
  if (ttfFontId_ != 0 && familyName && ttfFamily_ == familyName) return ttfFontId_;
#endif
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

#if CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::freeTtfSources() {
  for (auto& s : ttfSources_) {
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // actually release
    if (s.file) s.file.close();
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
}

bool SdCardFontSystem::loadTtfVariant(const uint8_t pointSize, const int fontId) {
  if (!ttf_) return false;
  for (auto& variant : ttfVariants_) {
    if (variant.fontId == fontId) {
      variant.lastUse = ++variantClock_;
      return true;
    }
  }
  if (ttfVariants_.size() >= MAX_TTF_VARIANTS) {
    const auto oldest = std::min_element(ttfVariants_.begin(), ttfVariants_.end(),
                                         [](const auto& a, const auto& b) { return a.lastUse < b.lastUse; });
    renderer_->unregisterTtfFont(oldest->fontId);
    renderer_->removeFont(oldest->fontId);
    ttfVariants_.erase(oldest);
  }
  auto font = makeUniqueNoThrow<TtfEpdFont>();
  if (!font) {
    LOG_ERR("SDFS", "OOM: TtfEpdFont variant @%upt", pointSize);
    return false;
  }
  // Variants carry headings and inline runs, sharing ttf_'s FreeType faces.
  if (!font->loadVariantOf(*ttf_, pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/64 * 1024, /*maxGlyphs=*/512))
    return false;
  font->build(" ");
  renderer_->insertFont(fontId, font->family());
  renderer_->registerTtfFont(fontId, font.get());
  ttfVariants_.push_back({fontId, ++variantClock_, std::move(font)});
  return true;
}

void SdCardFontSystem::clearTtfVariants(GfxRenderer& renderer) {
  for (const auto& variant : ttfVariants_) {
    renderer.unregisterTtfFont(variant.fontId);
    renderer.removeFont(variant.fontId);
  }
  ttfVariants_.clear();
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  clearTtfVariants(renderer);  // they borrow ttfSources_
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  // UI-size fallbacks first (they borrow ttfSources_).
  for (const int id : ttfUiIds_) {
    renderer.unregisterTtfFont(id);
    renderer.removeFont(id);
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
  // S3c+++: drop stale SD UI ids, then relink the chain back to the resident
  // built-in variants (ubuntu → builtin → placeholder) so UI Han keeps working
  // with the SD vector font unloaded.
  for (int& id : sdUiIds_) id = 0;
  relinkUiFallbacks(renderer);
  if (ttfFontId_ != 0) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);  // drop from the renderer's fontMap
  }
  ttf_.reset();  // frees the FT faces first (they read ttfSources_)
  // S3c+ clear the per-face tier③ pointer before LXGW's face dangles. PUSH/FAST
  // then gracefully degrade to tier④ placeholder (no crash) until a new SD
  // face is loaded. See裁定必答4.
  pushembedfont::setSdFallback(pushFallback_, nullptr);
  freeTtfSources();
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

bool SdCardFontSystem::openTtfSource(const uint8_t style, const std::string& path) {
  if (style >= 4) return false;
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    LOG_ERR("SDFS", "Failed to open TTF: %s", path.c_str());
    return false;
  }
  const size_t len = f.size();
  if (len == 0) {
    LOG_ERR("SDFS", "Empty TTF: %s", path.c_str());
    f.close();
    return false;
  }
  TtfSource& s = ttfSources_[style];
  s.file = std::move(f);  // kept open; prefixRead() reads it on demand
  s.streamed = true;
  // Cache the file's head in PSRAM: an sfnt's per-glyph-fault tables (cmap,
  // loca, hmtx) sit before the multi-MB glyf table, so serving the first
  // 1 MB from RAM turns each glyph fault's 4-6 scattered SD seeks into one
  // glyf read. Gated per source so a small-PSRAM board takes what fits.
  static constexpr size_t kStreamPrefix = 1024 * 1024;
  const size_t prefix = len < kStreamPrefix ? len : kStreamPrefix;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > prefix + 256 * 1024) {
    s.bytes.resize(prefix);
    if (s.file.seek(0) && static_cast<size_t>(s.file.read(s.bytes.data(), prefix)) == prefix) {
      LOG_DBG("SDFS", "Cached %u KB TTF prefix in PSRAM", static_cast<unsigned>(prefix / 1024));
    } else {
      s.bytes.clear();  // fall back to pure streaming
    }
  }
  LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

// Streamed-source read: serve from the PSRAM prefix cache when the range is
// there, hit SD only for the tail (glyf outlines). A read straddling the
// boundary splits across both.
unsigned long SdCardFontSystem::prefixRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                           const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const unsigned long cached = s->bytes.size();
  if (offset < cached) {
    const unsigned long fromCache = (offset + count <= cached) ? count : cached - offset;
    if (count == 0) return 0;  // seek probe
    memcpy(buffer, s->bytes.data() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache +
           SdCardFontRegistry::halFileRead(&s->file, offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return SdCardFontRegistry::halFileRead(&s->file, offset, buffer, count);
}

void SdCardFontSystem::addTtfSources(TtfEpdFont& font) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = ttfSources_[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::prefixRead, &s, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

// ---- S3cPF6: load-time advance map (codepoint -> raw hmtx advanceWidth) -----
namespace {
uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) { h ^= static_cast<uint8_t>(*s); h *= 16777619u; }
  return h;
}
}  // namespace

std::shared_ptr<epd::FontMap> SdCardFontSystem::ensureFontMap(TtfSource& src, const std::string& keyTag) {
  if (!src.present || src.size == 0) return nullptr;
  const uint32_t mtime = src.file ? src.file.modificationTime() : 0;
  char fname[96];
  std::snprintf(fname, sizeof(fname), "/.crosspoint/fontmap/%08x_%08x_%08x.fmap",
                fnv1a(keyTag.c_str()), mtime, static_cast<unsigned>(src.size));
  const std::string cacheDir = "/.crosspoint/fontmap";
  const std::string cachePath = fname;

  // 1) Try the on-disk cache first (~15 ms read-back, no table scan).
  do {
    if (!Storage.exists(cachePath.c_str())) break;
    HalFile cf;
    if (!Storage.openFileForRead("FMAP", cachePath, cf)) break;
    const size_t len = cf.fileSize();
    if (len < 16 || len > 512 * 1024) break;
    std::vector<uint8_t> buf(len);
    const bool ok = cf.seek(0) && static_cast<size_t>(cf.read(buf.data(), len)) == len;
    cf.close();
    if (!ok) break;
    auto map = std::make_shared<epd::FontMap>();
    if (map && map->load(buf.data(), buf.size()) && !map->empty()) {
      LOG_INF("FMAP", "hit %s cps=%u", fname, map->count());
      return map;
    }
    LOG_INF("FMAP", "corrupt cache %s — rebuilding", fname);
  } while (false);

  // 2) Build from the font's cmap+hmtx, reading via the prefix reader (which
  //    serves the 1 MB head from PSRAM and reaches hmtx at 15 MB via SD).
  auto map = std::make_shared<epd::FontMap>();
  uint16_t upem = 0;
  epd::FontMapStats stats;
  const bool built = map->build(&SdCardFontSystem::prefixRead, &src, src.size, upem, stats);
  if (!built || map->empty()) {
    LOG_INF("FMAP", "build FAILED for %s — using ordinary fallback chain", keyTag.c_str());
    return nullptr;  // graceful degradation, no crash / blank page
  }
  LOG_INF("FMAP", "built %s cps=%u supp=%u upem=%u psram=%uB",
          keyTag.c_str(), stats.codepoints, stats.skippedSupp, upem, (unsigned)map->byteSize());

  // 3) Persist the compact image (best effort; a write failure is non-fatal).
  do {
    if (!Storage.ensureDirectoryExists(cacheDir.c_str())) break;
    Storage.remove(cachePath.c_str());
    HalFile cf;
    if (!Storage.openFileForWrite("FMAP", cachePath, cf)) break;
    struct WriteCtx { HalFile* f; } wc{&cf};
    const bool saved = map->save(&wc, [](void* ctx, const uint8_t* data, size_t len) -> bool {
      auto* w = static_cast<WriteCtx*>(ctx);
      return w->f->write(data, len) == len;
    });
    cf.flush();
    cf.close();
    if (!saved) Storage.remove(cachePath.c_str());
  } while (false);

  return map;
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (!ttf_ || ttfFamily_.empty()) return;

  // Small caches: UI strings (titles/rows) are short. Each UI family is a sized
  // variant of ttf_, sharing its FreeType faces without parsing font files or
  // allocating separate FT_Face objects in DRAM.
  static constexpr size_t kUiFallbackMinInternalHeap = 24 * 1024;
  // S3c+++: reset per-size SD ids first — a size skipped this pass must not
  // inherit a stale id from a previous family/size.
  for (int& id : sdUiIds_) id = 0;
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) == 0) {
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (internalFree < kUiFallbackMinInternalHeap) {
        LOG_DBG("SDFS", "Skipping TTF UI fallback @%upt (%u KB internal free)", ui.pointSize,
                static_cast<unsigned>(internalFree / 1024));
        continue;
      }
    }
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for UI fallback @%upt", ui.pointSize);
      continue;  // built-in bitmap UI fonts keep covering this size
    }
    const bool ok =
        f->loadVariantOf(*ttf_, ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384);
    if (!ok) continue;
    LOG_DBG("SDFS", "TTF UI fallback @%upt loaded (heap free %u)", ui.pointSize, (unsigned)ESP.getFreeHeap());
    // Distinct id from the reader-size font: a UI size can equal the reader size
    // (e.g. both 12pt), which would collide on computeTtfFontId and be dropped
    // as a duplicate. Salt the UI family name to separate the id spaces.
    const int id = computeTtfFontId((ttfFamily_ + "\x01ui").c_str(), ui.pointSize);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    sdUiIds_[i] = id;  // S3c+++: remember for chain relink
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
  relinkUiFallbacks(renderer);  // S3c+++: ubuntu → SD → builtin → end
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty) {
  // Keep sizes inherited from other families within the selectable vector range.
  snapFontPointSizeTo(
      snapToNearestPointSize(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES), SETTINGS.fontPointSize));
  const uint8_t size = SETTINGS.fontPointSize;

  // Already loaded, same family + size, and disk unchanged → nothing to do.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name && ttfPointSize_ == size) return;

  // Reader-face glyph-cache budget (used by both the resize fast path and the
  // full load below): the default 32 KB holds ~90 CJK glyphs, but a CJK page
  // uses 300+, so the cache flush-cycles mid-page and every page turn
  // re-rasterizes the whole page through streamed SD reads (multi-second
  // turns). The arenas are PSRAM-backed (FontPsram); 1 MB / 4096 glyphs holds
  // a whole Japanese novel's working set (~3000 unique kanji+kana at ~350 B
  // each), so the flush-everything ceiling is never hit and warm page turns
  // are pure cache hits. Without PSRAM keep the internal-DRAM-safe default.
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 1024 * 1024 : 32 * 1024;
  const uint16_t maxGlyphs = havePsram ? 4096 : 768;

  // Same family, only the reader size changed (size preview): the open style
  // sources and the size-independent UI fallbacks don't need rebuilding — just
  // re-drive the reader face at the new size, reusing the already-open files
  // instead of reopening all four and rebuilding every UI fallback.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name) {
    clearTtfVariants(renderer);  // their ids derive from the old reader size
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
    if (ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      ttf_->build(" ");
      ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
      renderer.insertFont(ttfFontId_, ttf_->family());
      renderer.registerTtfFont(ttfFontId_, ttf_.get());
      ttfPointSize_ = size;
      // S3c-plus2: reader face rebuilt at new size — rewire its fallback ctx.
      refreshSdVectorFallback();
      return;
    }
    // Resize failed: fall through to a clean full reload.
  }

  unloadTtf(renderer);

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.clearSdFontFamily();
    return;
  }

  // Open each style source the family ships (0=regular, 1=bold, 2=italic,
  // 3=bold-italic). A single-file family (loose .ttf, or a folder with one file)
  // supplies only regular; TtfEpdFont then derives bold/italic from the wght axis
  // or an oblique shear. Extra files upgrade those styles to the real designs.
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (ttfSources_[role].present) continue;  // registry already deduped by role
    if (!family.filePath(file, path, sizeof(path))) {
      LOG_ERR("SDFS", "Vector font path too long in family %s", family.name.c_str());
      continue;
    }
    openTtfSource(role, path);
  }
  if (!ttfSources_[0].present) {
    // Possibly a transient SD read failure: keep the user's selection so the
    // next ensureLoaded() retries; this session falls back to the built-in.
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    freeTtfSources();
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    // Transient OOM: keep the user's selection (unlike a parse failure) so the
    // next ensureLoaded() can retry once heap pressure passes.
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    freeTtfSources();
    return;
  }
  addTtfSources(*ttf_);
  // S3cPF6: attach the load-time advance map for the regular face's source.
  // Built/loaded ONCE per file (cache key = path-tag + mtime + size); a hit
  // lets getAdvance skip the per-codepoint SD seek into hmtx. Failure returns
  // nullptr and the font simply uses the ordinary FreeType/fallback path.
  auto fontMap = ensureFontMap(ttfSources_[0], family.name);
  ttf_->setFontMap(std::move(fontMap));
  const bool ok = ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs);
  if (!ok) {
    // init failure is ambiguous (corrupt font vs. transient OOM inside
    // FreeType): keep the selection and retry next ensureLoaded() rather than
    // silently reverting the user to the built-in font. A genuinely broken
    // font costs one failed load per reader entry, visible in the log.
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    freeTtfSources();
    return;
  }
  // Seed the regular face's glyph cache; other styles + glyphs fault on demand.
  ttf_->build(" ");

  ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
  renderer.insertFont(ttfFontId_, ttf_->family());
  renderer.registerTtfFont(ttfFontId_, ttf_.get());
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  // S3c+: keep PUSH/FAST tier③ pointing at the freshly (re)loaded SD face.
  // This also re-arms tier③ after a delete-then-reinstall or a point-size
  // change, so生僻字 keep resolving via SD and never degrade permanently.
  pushembedfont::setSdFallback(pushFallback_, ttf_.get());
  // S3c-plus2: wire this SD vector face's own fallback chain (tier① embedded
  // builtin + tier② PIF1; tier③ unset). No-op until pushTtf_ exists at startup;
  // ensurePushFont() re-calls this once the embedded face is ready.
  refreshSdVectorFallback();
  LOG_DBG("SDFS", "Reader TTF face loaded (heap free %u, max block %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  setupTtfUiFallbacks(renderer);  // CJK/script UI fallback at the built-in UI sizes
  LOG_DBG("SDFS", "Loaded TTF font: %s @ %upt (id %d)", family.name.c_str(), size, ttfFontId_);
}

#endif  // CROSSPOINT_VECTOR_FONTS
