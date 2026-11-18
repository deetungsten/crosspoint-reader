#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>
#include <vector>

#include "KoboCatalog.h"
#include "activities/CatalogActivity.h"

// Browse the books calibre-web exposes through the Kobo sync feed, download
// them into the library folder, and record the book-id <-> local-file mapping
// used by the reading-position sync.
class KoboShelfActivity final : public CatalogActivity {
 public:
  explicit KoboShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : CatalogActivity("KoboShelf", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;

 private:
  // One page of the shelf. Fixed cap keeps the row strings and records bounded
  // on the C3; the catalog itself stays on SD and is re-read on every visit.
  static constexpr size_t MAX_SHELF_ROWS = 48;

  KoboCatalog catalog;
  std::vector<KoboBookRecord> books;
  std::vector<std::string> rowLabels;
  std::vector<freeink::ui::ListItem> rowItems;

  // Pending transfer, kept across the blocking download so the mapping can be
  // written once the file is safely on SD.
  std::string pendingContentId;
  std::string pendingPath;
  size_t pageOffset = 0;
  size_t nextPageOffset = 0;
  bool hasMorePages = false;
  // Opening a downloaded book replaces this activity; skip the network
  // activity teardown (which reboots) so the reader can start normally.
  bool navigatingToReader = false;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void startBrowse() override;
  void downloadFinished(bool cancelled) override;
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}

  void loadRows();
  void downloadBook(size_t index);
  std::string downloadDestination(const kobo::Metadata& metadata) const;
};

#endif  // CROSSPOINT_KOBO_SYNC
