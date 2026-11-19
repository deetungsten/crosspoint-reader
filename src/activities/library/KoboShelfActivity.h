#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <string>
#include <vector>

#include "KoboCatalog.h"
#include "KoboSyncClient.h"
#include "activities/CatalogActivity.h"

// Browse the books calibre-web exposes through the Kobo sync feed, download
// them into the library folder, and record the book-id <-> local-file mapping
// used by the reading-position sync.
class KoboShelfActivity final : public CatalogActivity {
 public:
  // resumeDownloads skips the catalog sync once and starts the pending
  // auto-download batch, so "Sync Now" can hand off to this screen without
  // fetching the same feed twice.
  explicit KoboShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool resumeDownloads = false)
      : CatalogActivity("KoboShelf", renderer, mappedInput), resumeDownloads(resumeDownloads) {}

  void onEnter() override;
  void onExit() override;

 private:
  // One page of the shelf. Fixed cap keeps the row strings and records bounded
  // on the C3; the catalog itself stays on SD and is re-read on every visit.
  static constexpr size_t MAX_SHELF_ROWS = 48;
  static constexpr freeink::ui::ActionId ACTION_SETTINGS = ACTION_USER + 2;

  enum class AutoDownloadResult { Continue, Skip, Cancelled, CancelledHome, Failed };

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
  bool resumeDownloads = false;
  bool autoDownloadBatch = false;
  size_t autoDownloadScan = 0;
  size_t autoDownloadDone = 0;
  size_t autoDownloadTotal = 0;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void startBrowse() override;
  void downloadFinished(bool cancelled) override;
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}
  bool restartsOnExit() const override { return false; }

  static void onSettingsEvent(const freeink::ui::ActionEvent& event, void* user);
  void openSettings();
  void loadRows();
  void showCatalog(bool allowAutoDownload = true);
  void releaseRowStorage();
  void downloadBook(size_t index);
  std::string downloadDestination(const kobo::Metadata& metadata) const;
  KoboSyncClient::Error prepareDownload(const KoboBookRecord& record, kobo::Metadata& metadata,
                                        std::string& destination, bool& alreadyDownloaded);
  HttpDownloader::DownloadError startBookDownload(const KoboBookRecord& record, const kobo::Metadata& metadata,
                                                  const std::string& destination, const char* status);
  bool needsAutoDownload(const KoboBookRecord& record) const;
  void maybeStartAutoDownload();
  void runAutoDownloadBatch();
  AutoDownloadResult downloadAutoRecord(const KoboBookRecord& record);
  void finishAutoDownload();
};

#endif  // CROSSPOINT_KOBO_SYNC
