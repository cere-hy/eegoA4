#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CoverGridHomeUi.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  // The home "library" slot (index 2) shows Plugins while the plugin system is
  // enabled, otherwise OPDS when a server is configured. NONE hides the slot.
  // NetworkModeSelectionActivity lists OPDS in exactly the other cases.
  HomeMenuItem slotItem() const {
    if (hasPlugins) return HomeMenuItem::PLUGINS;
    return hasOpdsServers ? HomeMenuItem::OPDS_BROWSER : HomeMenuItem::NONE;
  }
  bool hasContinueReading = false;
  bool pushNoticePending = false;  // S3: show "No push content" popup once on next render
  bool wifiAbnormalNoticePending = false;  // pf9d: stale WIFI boot_mode hint, shown once on next render
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Output polarity the snapshot was taken in (night mode stores images pre-inverted).
  bool coverBufferInverted = false;
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, HomeMenuItem slot) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (slot != HomeMenuItem::NONE && item == slot) return i;
    if (item == HomeMenuItem::PLUGINS) return 0;
    if (slot != HomeMenuItem::NONE) ++i;
    // b65-s3c-pf9k: FILE_TRANSFER row removed from Home. OPDS remains reachable
    // via its slot above; Settings/PUSH each shift down one index.
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    ++i;
    if (item == HomeMenuItem::PUSH) return i;  // S3: after Settings
    ++i;
    if (item == HomeMenuItem::WIFI_TRANSFER) return i;  // pf9d: last menu row
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, HomeMenuItem slot) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (slot != HomeMenuItem::NONE && idx == i++) return slot;
    // b65-s3c-pf9k: FILE_TRANSFER removed; Settings/PUSH shift down one index.
    if (idx == i++) return HomeMenuItem::SETTINGS_MENU;
    if (idx == i++) return HomeMenuItem::PUSH;  // S3: after Settings
    if (idx == i) return HomeMenuItem::WIFI_TRANSFER;  // pf9d: last menu row
    return HomeMenuItem::NONE;
  }
  void onRecentBookSelected(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();
  void onPushOpen();
  void onWifiTransferOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
  // b65-s3c-pf4 P1/P2: guarantee the pushed document is a home tile on EVERY
  // theme (not just COVER_GRID). Normalizes PUSH's cover path to the single
  // generated thumb_400.bmp (no [HEIGHT] token) so Lyra/Lyra3Covers/
  // RoundedRaff cannot request a non-existent thumb_226/300.bmp, then appends
  // a virtual PUSH book into the visible slot range when absent.
  void ensurePushHomeTile(int displaySlots);
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
