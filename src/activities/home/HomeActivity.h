#pragma once
#include <array>
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "KoboSyncConfig.h"
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
  bool hasKobo = false;
  // Ordered menu rows, rebuilt in onEnter() after the visibility flags settle.
  // The Kobo layout is fixed; the other builds keep their conditional slot.
  static constexpr int MAX_MENU_ITEMS = 5;
  std::array<HomeMenuItem, MAX_MENU_ITEMS> menuOrder{};
  int menuItemCount = 0;
  // The home "library" slot (index 2) shows Plugins while the plugin system is
  // enabled, otherwise OPDS when a server is configured. NONE hides the slot.
  // Only the non-Kobo layout uses it.
  HomeMenuItem slotItem() const {
    if (hasPlugins) return HomeMenuItem::PLUGINS;
    return hasOpdsServers ? HomeMenuItem::OPDS_BROWSER : HomeMenuItem::NONE;
  }
  void buildMenuOrder();
  int menuItemToIndex(HomeMenuItem item) const;
  HomeMenuItem indexToMenuItem(int idx) const;
  bool hasContinueReading = false;
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

  void onRecentBookSelected(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();
#if CROSSPOINT_KOBO_SYNC
  void onKoboShelfOpen();
  void onKoboSyncOpen();
#endif

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary();
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
