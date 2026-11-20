#pragma once

#include <Epub/blocks/TextBlock.h>
#include <HalStorage.h>  // HalFile (kept open for streamed TTFs)
#include <PushEmbeddedFont.h>  // S3c+ FallbackContext (per-face tier③ pointer)
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <VectorFontSupport.h>

#if CROSSPOINT_VECTOR_FONTS
#include <FontPsram.h>  // PsramVector for resident TTF bytes
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;
class TtfEpdFont;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  // Constructor and destructor are out-of-line (defined in the .cpp where
  // TtfEpdFont is a complete type) so the std::unique_ptr<TtfEpdFont> member
  // can be constructed/destroyed with only a forward declaration visible here.
  SdCardFontSystem();
  ~SdCardFontSystem();
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// S3c: ensure the PUSH embedded font (resident builtin.ttf + PIF1 Han bitmap
  /// + SD fallback) is loaded at the current reader point size. Safe to call
  /// repeatedly; rebuilds only when the size changes. Returns its renderer font
  /// id (non-zero on success), or 0 on load failure. Only PUSH uses this font.
  int ensurePushFont(GfxRenderer& renderer);
  /// Renderer font id of the loaded PUSH font (0 = not loaded yet).
  int pushFontId() const { return pushFontId_; }

  /// S3c+: Built-in (Fast) reuses the SAME resident embedded face as PUSH, so
  /// there is one handler chain and no second face. The fast id equals the push
  /// id; kept as a separate accessor for intent clarity at call sites.
  int fastFontId() const { return pushFontId_; }

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched script fallback for the corresponding UI font, so book
  // titles/list rows in scripts the built-ins lack (CJK, Greek, Cyrillic, ...)
  // render at the same size as the surrounding Latin UI text. No-op when no SD
  // family is loaded. Safe to call repeatedly (sizes already loaded are
  // reused).
  void setupUiFallbacks(GfxRenderer& renderer);

#if CROSSPOINT_VECTOR_FONTS
  // --- Vector (.ttf/.otf) font path (FreeInkFont via TtfEpdFont) -------------
  // Load/refresh the selected TTF family at the current reader size, register
  // it with the renderer, and track it so ensureSdCardFontReady() rebuilds its
  // glyph set per page. registryWasDirty forces a reload even if unchanged.
  void loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, bool registryWasDirty);
  // Unregister + free the active TTF font (and its UI-size fallbacks), if any.
  void unloadTtf(GfxRenderer& renderer);
  // Register the loaded TTF at each built-in UI size as a script fallback, so UI
  // text (book titles, list rows, menus, status bar) in scripts the built-in
  // fonts lack renders in the chosen TTF. Mirrors setupUiFallbacks for .cpfont.
  void setupTtfUiFallbacks(GfxRenderer& renderer);
  // Open one style source file (resident if small, streamed if large) into
  // ttfSources_[style]. Returns false on open/read failure.
  bool openTtfSource(uint8_t style, const std::string& path);
  // Register every present source with `font` (shared bytes / file handles).
  void addTtfSources(TtfEpdFont& font);
  // S3c-plus2: (re)wire the SD-vector reader face's fallback chain once both
  // `ttf_` and the embedded `pushTtf_` exist. Safe to call repeatedly; no-op
  // if either face is absent.
  void refreshSdVectorFallback();
  // Close/free all style sources.
  void freeTtfSources();
  // ReadFn for streamed sources: serves the PSRAM prefix cache first, SD after.
  static unsigned long prefixRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
#endif  // CROSSPOINT_VECTOR_FONTS

  // --- Sized variants of the reader font (CSS font-size, headings, drop caps) ---
  // Served to the renderer through FontVariantProvider. Variant ids derive from the
  // reader font id and point size, so ids cached in section files can be reloaded after
  // eviction or a reboot. Least recently used variants are evicted at the caps below.
  int resolveVariant(int fontId, float scale);
  bool loadVariant(int fontId);
  bool loadCpfontVariant(const SdCardFontFamilyInfo& family, uint8_t pointSize, int fontId);
  void unloadCpfonts(GfxRenderer& renderer);
  static int variantFontId(int readerFontId, uint8_t pointSize);

  struct CpfontVariant {
    int fontId;
    uint32_t lastUse;
  };
  static constexpr size_t MAX_CPFONT_VARIANTS = TextBlock::MAX_WORD_FONTS + 2;  // Inline, block, and drop-cap fonts.
  std::vector<CpfontVariant> cpfontVariants_;
  uint32_t variantClock_ = 0;
  GfxRenderer* renderer_ = nullptr;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};

#if CROSSPOINT_VECTOR_FONTS
  // One style source file. SMALL files are read fully into `bytes` (resident,
  // PSRAM when present); LARGE files stream from `file` (kept open) so a multi-MB
  // file never sits in RAM. All faces (reader + UI sizes) share these sources.
  struct TtfSource {
    // Resident form: the whole file. Streamed form: a PSRAM prefix cache of the
    // file head (cmap/loca/hmtx) — empty when PSRAM couldn't fund it.
    freeink::font::PsramVector<uint8_t> bytes;
    HalFile file;  // open handle (streamed form)
    bool streamed = false;
    unsigned long size = 0;
    bool present = false;
  };

  // Active TTF font (at most one reader-size vector family loaded at a time).
  std::unique_ptr<TtfEpdFont> ttf_;
  // Up to 4 style sources: 0=regular (required), 1=bold, 2=italic, 3=bold-italic.
  TtfSource ttfSources_[4];
  std::string ttfFamily_;     // loaded vector family name ("" = none)
  int ttfFontId_ = 0;         // renderer font id for ttf_ (0 = none)
  uint8_t ttfPointSize_ = 0;  // size ttf_ was built at
  // UI-size TTF fallbacks (share ttfSources_); parallel to their renderer font ids.
  std::vector<std::unique_ptr<TtfEpdFont>> ttfUi_;
  std::vector<int> ttfUiIds_;

  // S3c PUSH embedded font: resident builtin.ttf (embed bytes, no SD), with
  // PIF1 Han bitmap + SD fallback installed as its secondary glyph source.
  // S3c+: this SAME face is the Built-in (Fast) reader font.
  std::unique_ptr<TtfEpdFont> pushTtf_;
  int pushFontId_ = 0;
  uint8_t pushPt_ = 0;
  // S3c+ per-face fallback state: tier③ points at the active SD (LXGW) face.
  // Lives as long as this system, so handler ctx never dangles before unload.
  pushembedfont::FallbackContext pushFallback_;
  // S3c-plus2: independent fallback ctx for the SD-VECTOR reader face (ttf_).
  // tier① = embedded builtin face, tier② = PIF1, tier③ = nullptr (self).
  // Kept separate from pushFallback_ so PUSH/FAST config is never overwritten.
  pushembedfont::FallbackContext sdTtfFallback_;

  // S3c+++: three RESIDENT built-in UI variants (8/10/12pt) built from
  // builtin.ttf with the same PIF1/placeholder secondary chain as PUSH. They
  // are the last-resort tier of the UI string-level fallback chain, so UI Han
  // never shows empty boxes when no SD font (or a Latin-only SD font) is
  // installed. Built once, kept for the process lifetime (UI sizes are fixed).
  std::unique_ptr<TtfEpdFont> builtinUi_[3];
  pushembedfont::FallbackContext builtinUiCtx_[3];
  int builtinUiIds_[3] = {0, 0, 0};
  // Per-size SD UI fallback id currently registered (0 = none). Lets the chain
  // be relinked deterministically after SD font load/unload without reading
  // GfxRenderer's private map. Parallel to kUiFontSizes.
  int sdUiIds_[3] = {0, 0, 0};
  // Build the three resident variants once, then (re)link every UI fallback
  // chain: ubuntu → SD (if any) → builtin variant → end. Idempotent.
  void setupBuiltinUiFallbacks(GfxRenderer& renderer);
  void relinkUiFallbacks(GfxRenderer& renderer);

  // Reader-font faces at other sizes (share ttfSources_).
  struct TtfVariant {
    int fontId;
    uint32_t lastUse;
    std::unique_ptr<TtfEpdFont> font;
  };
  static constexpr size_t MAX_TTF_VARIANTS = TextBlock::MAX_WORD_FONTS + 2;
  std::vector<TtfVariant> ttfVariants_;
  bool loadTtfVariant(uint8_t pointSize, int fontId);
  void clearTtfVariants(GfxRenderer& renderer);
#endif  // CROSSPOINT_VECTOR_FONTS
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
