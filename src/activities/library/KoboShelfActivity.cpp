#include "KoboShelfActivity.h"

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KoboCredentialStore.h"
#include "KoboDownloadPlan.h"
#include "KoboSyncClient.h"
#include "activities/settings/KoboSyncSettingsActivity.h"
#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "components/icons/listIcons.h"
#include "network/HttpDownloader.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
void appendKoboHeaders(std::vector<HttpDownloader::Header>& headers) {
  if (!KOBO_STORE.getCookie().empty()) headers.push_back({"Cookie", KOBO_STORE.getCookie()});
  headers.push_back({"x-kobo-device-id", KOBO_STORE.getDeviceId()});
}
}  // namespace

void KoboShelfActivity::onEnter() {
  CatalogActivity::onEnter();
  app.on(ACTION_SETTINGS, &KoboShelfActivity::onSettingsEvent, this);
  // The settings screen may have changed credentials since boot.
  KOBO_STORE.loadFromFile();
  if (!KOBO_STORE.hasCredentials()) {
    fail(StrId::STR_KOBO_NO_CREDENTIALS);
    return;
  }
  // Opening the shelf never brings Wi-Fi up. startBrowse() only syncs when the
  // radio is already connected, and never prompts for Wi-Fi.
  startBrowse();
}

void KoboShelfActivity::onExit() {
  releaseRowStorage();
  catalog.close();
  // Release the radio and its driver heap in place. With the mode off the
  // shared catalog teardown skips its heap-defrag reboot.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  CatalogActivity::onExit();
}

void KoboShelfActivity::releaseForNetworkSync() {
  // The sleep sync opens its own catalog; do not leave the shelf's rows and
  // SD handles resident through TLS.
  releaseRowStorage();
  catalog.close();
}

int KoboShelfActivity::listCount() const { return state == State::BROWSING ? static_cast<int>(rowItems.size()) : 0; }

void KoboShelfActivity::startBrowse() {
  pendingContentId.clear();
  pendingPath.clear();
  pageOffset = 0;
  nextPageOffset = 0;
  hasMorePages = false;

  // Consume the one-shot handoff before any early return, so Retry does not
  // keep skipping the feed.
  const bool skipSync = resumeDownloads;
  resumeDownloads = false;
  // Auto-sync is best effort: use an existing connection, never bring one up.
  const bool autoSync = !skipSync && wifiConnected();

  if (autoSync) {
    state = State::LOADING;
    statusMessage = tr(STR_KOBO_SYNCING);
    requestUpdateAndWait();
  }

  if (!catalog.isOpen() && !catalog.open()) {
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }

  // "Sync Now" already fetched the feed; go straight to the downloads it left
  // pending instead of asking the server for the same page set again.
  bool syncOk = true;
  if (autoSync) {
    KoboSyncClient::SyncStats stats;
    const auto result = KoboSyncClient::syncCatalog(catalog, stats);
    syncOk = result == KoboSyncClient::OK;
    if (!syncOk) {
      // A failed background sync must not hide the catalog already on SD.
      LOG_ERR("KOBO", "Shelf sync failed: %d", static_cast<int>(result));
    }
  }

  // Skip the download batch when the feed could not be refreshed; it would
  // retry the same failing server once per missing book.
  showCatalog(syncOk);
}

void KoboShelfActivity::showCatalog(const bool allowAutoDownload) {
  if (!catalog.isOpen() && !catalog.open()) {
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }

  // A recreated catalog (missing/corrupt file) must discard the incremental
  // token even when no sync can run. Otherwise the next connected sync would
  // only fetch changes after rows that no longer exist.
  if (catalog.wasReset()) {
    KOBO_STORE.clearSyncState();
    KOBO_STORE.saveToFile();
    catalog.clearResetFlag();
  }

  loadRows();
  state = State::BROWSING;
  // The settings row is first; start on the first book when there is one.
  if (!books.empty()) nav.selected = 1;
  requestUpdate(true);
  if (allowAutoDownload) maybeStartAutoDownload();
}

void KoboShelfActivity::loadRows() {
  books.clear();
  rowLabels.clear();
  rowItems.clear();

  books.reserve(MAX_SHELF_ROWS);
  rowLabels.reserve(MAX_SHELF_ROWS + 1);
  rowItems.reserve(MAX_SHELF_ROWS + 2);

  // A fixed menu row keeps Kobo settings reachable on button-only devices;
  // the header gear is touch-only.
  fui::ListItem settingsItem;
  settingsItem.label = tr(STR_KOBO_SETTINGS);
  settingsItem.actionValue = 0;
  rowItems.push_back(settingsItem);

  const uint16_t total = catalog.count();
  size_t scanned = pageOffset;
  while (scanned < total && books.size() < MAX_SHELF_ROWS) {
    KoboBookRecord record;
    const bool read = catalog.readAt(static_cast<uint16_t>(scanned), record);
    scanned++;
    if (!read || (record.flags & kobo_flags::REMOVED) != 0) continue;

    books.push_back(record);
    std::string label = record.title;
    if (label.empty()) label = record.contentId;
    if (record.author[0] != '\0') {
      label += '\n';
      label += record.author;
    }
    if ((record.flags & kobo_flags::NEW_SINCE_SYNC) != 0) {
      label.insert(0, "* ");
    }
    rowLabels.push_back(std::move(label));
  }

  // A page can be empty when the tail of the catalog is all removed rows.
  // Fall back to the first page rather than showing a dead end.
  if (books.empty() && pageOffset > 0) {
    pageOffset = 0;
    loadRows();
    return;
  }

  nextPageOffset = scanned;
  hasMorePages = scanned < total;

  // Keep the settings row reachable while still explaining an empty catalog.
  if (books.empty() && !hasMorePages) {
    fui::ListItem emptyItem;
    emptyItem.label = tr(STR_KOBO_NO_BOOKS);
    emptyItem.actionValue = 1;
    rowItems.push_back(emptyItem);
  }

  for (size_t i = 0; i < books.size(); i++) {
    fui::ListItem item;
    item.label = rowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(i + 1);
    const KoboBookRecord& record = books[i];
    if ((record.flags & kobo_flags::NEW_SINCE_SYNC) != 0) {
      item.value = tr(STR_KOBO_NEW_BOOK);
    } else if ((record.flags & kobo_flags::DOWNLOADED) != 0) {
      item.value = tr(STR_KOBO_DOWNLOADED);
    } else if (record.shelf[0] != '\0') {
      item.value = record.shelf;
    }
    rowItems.push_back(item);
  }

  if (hasMorePages) {
    fui::ListItem item;
    item.label = tr(STR_NEXT_PAGE);
    item.actionValue = static_cast<int16_t>(books.size() + 1);
    rowItems.push_back(item);
  }
}

void KoboShelfActivity::buildScreen(UiScreen& screen) {
  // Settings stay reachable from the shelf, which is now the home entry point.
  // The gear is a touch-only affordance; button devices use the fixed row.
  const bool touch = mappedInput.hasTouch();
  catalogScreenHeader(screen, renderer, tr(STR_KOBO_SHELF),
                      touch ? fui::bitmapFromIcon(icon_settings_32) : fui::BitmapRef{},
                      touch ? ACTION_SETTINGS : fui::NO_ACTION);
  if (state != State::BROWSING) {
    buildStatusScreen(screen);
    return;
  }

  if (rowItems.empty()) {
    screen.centeredText(tr(STR_KOBO_NO_BOOKS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

std::string KoboShelfActivity::downloadDestination(const kobo::Metadata& metadata) const {
  const char* folder = SETTINGS.opdsDownloadFolder;
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("KOBO", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  std::string path;
  if (haveFolder) {
    path = folder;
    path += '/';
  } else {
    path = "/";
  }
  path +=
      opdsBookFilename(metadata.author, metadata.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

KoboSyncClient::Error KoboShelfActivity::prepareDownload(const KoboBookRecord& record, kobo::Metadata& metadata,
                                                         std::string& destination, bool& alreadyDownloaded) {
  alreadyDownloaded = false;
  const auto result = KoboSyncClient::fetchMetadata(record.contentId, metadata);
  if (result != KoboSyncClient::OK) return result;
  if (metadata.title[0] == '\0') std::memcpy(metadata.title, record.title, sizeof(metadata.title));
  if (metadata.author[0] == '\0') std::memcpy(metadata.author, record.author, sizeof(metadata.author));
  if (metadata.downloadUrl[0] == '\0') return KoboSyncClient::NOT_FOUND;

  destination = downloadDestination(metadata);
  // Two books can sanitize to the same "Author - Title.epub". Keep the
  // existing file (and its link) only when it is this same content id.
  if (Storage.exists(destination.c_str())) {
    std::string linkedId;
    if (KoboCatalog::contentIdForPath(destination, linkedId) && linkedId == record.contentId) {
      alreadyDownloaded = true;
      return KoboSyncClient::OK;
    }
    destination = kobo::withContentSuffix(destination, record.contentId);
  }
  if (Storage.exists(destination.c_str())) {
    std::string linkedId;
    if (KoboCatalog::contentIdForPath(destination, linkedId) && linkedId == record.contentId) {
      alreadyDownloaded = true;
    }
  }
  return KoboSyncClient::OK;
}

HttpDownloader::DownloadError KoboShelfActivity::startBookDownload(const KoboBookRecord& record,
                                                                   const kobo::Metadata& metadata,
                                                                   const std::string& destination,
                                                                   const char* status) {
  pendingContentId = record.contentId;
  pendingPath = destination;
  // Release the row buffers and the SD index while TLS owns its record buffers.
  releaseRowStorage();
  catalog.close();

  beginDownload(status != nullptr ? status : metadata.title);
  std::vector<HttpDownloader::Header> headers;
  appendKoboHeaders(headers);
  return downloadFile(metadata.downloadUrl, destination, "", "", headers);
}

void KoboShelfActivity::downloadBook(const size_t index) {
  if (index >= books.size()) return;
  pendingContentId.clear();
  pendingPath.clear();
  const KoboBookRecord record = books[index];

  kobo::Metadata metadata;
  std::string destination;
  bool alreadyDownloaded = false;
  const auto prepared = prepareDownload(record, metadata, destination, alreadyDownloaded);
  if (prepared != KoboSyncClient::OK) {
    errorMessage = KoboSyncClient::errorString(prepared);
    state = State::ERROR;
    requestUpdate(true);
    return;
  }
  if (alreadyDownloaded) {
    catalog.setLocalPath(record.contentId, destination.c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    activityManager.goToReader(destination);
    return;
  }
  // Let the list render finish before startBookDownload frees its rows.
  requestUpdateAndWait();
  finishDownload(startBookDownload(record, metadata, destination, nullptr));
}

void KoboShelfActivity::releaseRowStorage() {
  std::vector<KoboBookRecord>().swap(books);
  std::vector<std::string>().swap(rowLabels);
  std::vector<freeink::ui::ListItem>().swap(rowItems);
}

void KoboShelfActivity::activateIndex(const int index) {
  if (index < 0) return;
  if (index == 0) {
    openSettings();
    return;
  }
  const int bookIndex = index - 1;
  if (bookIndex == static_cast<int>(books.size())) {
    if (!hasMorePages) return;
    app.clearTapFlash();
    pageOffset = nextPageOffset;
    loadRows();
    requestUpdate(true);
    return;
  }
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  app.clearTapFlash();

  const KoboBookRecord& record = books[static_cast<size_t>(bookIndex)];
  if ((record.flags & kobo_flags::DOWNLOADED) != 0 && record.localPath[0] != '\0' && Storage.exists(record.localPath)) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    activityManager.goToReader(record.localPath);
    return;
  }
  downloadBook(static_cast<size_t>(bookIndex));
}

void KoboShelfActivity::onSettingsEvent(const freeink::ui::ActionEvent&, void* user) {
  static_cast<KoboShelfActivity*>(user)->openSettings();
}

void KoboShelfActivity::openSettings() {
  if (state != State::BROWSING || autoDownloadBatch) return;
  app.clearTapFlash();
  const std::string endpoint = KOBO_STORE.getEndpoint();
  const std::string token = KOBO_STORE.getAuthToken();
  // Let the list render finish, then drop the rows and open handles before
  // handing over so the next browse opens the current file.
  requestUpdateAndWait();
  state = State::LOADING;
  statusMessage = tr(STR_KOBO_SYNCING);
  releaseRowStorage();
  catalog.close();

  auto settings = makeUniqueNoThrow<KoboSyncSettingsActivity>(renderer, mappedInput);
  if (!settings) {
    LOG_ERR("KOBO", "OOM: Kobo settings");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  startActivityForResult(std::move(settings), [this, endpoint, token](const ActivityResult&) {
    KOBO_STORE.loadFromFile();
    if (!KOBO_STORE.hasCredentials()) {
      fail(StrId::STR_KOBO_NO_CREDENTIALS);
      return;
    }
    // Returning to the shelf must never prompt for Wi-Fi. If a child screen
    // left the radio off, browse the catalog already on SD; showCatalog keeps
    // the current page, unlike startBrowse which resets it.
    if (!wifiConnected()) {
      showCatalog();
      return;
    }
    // A settings-only change (for example the auto-download toggle) does not
    // need another feed fetch.
    if (endpoint != KOBO_STORE.getEndpoint() || token != KOBO_STORE.getAuthToken()) {
      startBrowse();
      return;
    }
    if (!catalog.isOpen() && !catalog.open()) {
      fail(StrId::STR_MEMORY_ERROR);
      return;
    }
    // A credential edit that ended where it started still cleared the catalog.
    // Re-sync instead of showing an empty shelf.
    if (catalog.wasReset()) {
      startBrowse();
      return;
    }
    showCatalog();
  });
}

bool KoboShelfActivity::needsAutoDownload(const KoboBookRecord& record) const {
  const bool hasPath = record.localPath[0] != '\0';
  return kobo::needsDownload((record.flags & kobo_flags::REMOVED) != 0, hasPath,
                             hasPath && Storage.exists(record.localPath));
}

void KoboShelfActivity::maybeStartAutoDownload() {
  if (!KOBO_STORE.getAutoDownload() || autoDownloadBatch || !catalog.isOpen() || state != State::BROWSING) return;
  // Leave rows pending for the next visit instead of failing every transfer.
  if (!wifiConnected()) return;

  autoDownloadTotal = 0;
  for (uint16_t i = 0; i < catalog.count(); i++) {
    KoboBookRecord record;
    if (catalog.readAt(i, record) && needsAutoDownload(record)) autoDownloadTotal++;
  }
  if (autoDownloadTotal == 0) return;

  // Let the shelf list finish painting before the batch frees its row buffers.
  requestUpdateAndWait();
  autoDownloadBatch = true;
  autoDownloadScan = 0;
  autoDownloadDone = 0;
  runAutoDownloadBatch();
}

void KoboShelfActivity::runAutoDownloadBatch() {
  // The list must not render with released rows; switch to the download
  // status screen before freeing them.
  state = State::DOWNLOADING;
  statusMessage = tr(STR_DOWNLOADING);
  downloadProgress = downloadTotal = 0;
  releaseRowStorage();
  while (autoDownloadScan < catalog.count()) {
    KoboBookRecord record;
    const size_t index = autoDownloadScan++;
    if (!catalog.readAt(static_cast<uint16_t>(index), record) || !needsAutoDownload(record)) continue;

    const auto result = downloadAutoRecord(record);
    if (result == AutoDownloadResult::Continue) continue;
    if (result == AutoDownloadResult::Skip) {
      autoDownloadDone++;
      continue;
    }
    if (result == AutoDownloadResult::CancelledHome) {
      autoDownloadBatch = false;
      onGoHome();
      return;
    }
    if (result == AutoDownloadResult::Failed) {
      autoDownloadBatch = false;
      fail(StrId::STR_DOWNLOAD_FAILED);
      return;
    }
    finishAutoDownload();
    return;
  }
  finishAutoDownload();
}

KoboShelfActivity::AutoDownloadResult KoboShelfActivity::downloadAutoRecord(const KoboBookRecord& record) {
  kobo::Metadata metadata;
  std::string destination;
  bool alreadyDownloaded = false;
  const auto prepared = prepareDownload(record, metadata, destination, alreadyDownloaded);
  if (prepared != KoboSyncClient::OK) {
    LOG_ERR("KOBO", "Auto-download metadata failed for %s: %d", record.contentId, static_cast<int>(prepared));
    // A missing or unsupported book is permanent; do not let it block the
    // rest of the library on every visit.
    return prepared == KoboSyncClient::NOT_FOUND || prepared == KoboSyncClient::JSON_ERROR
               ? AutoDownloadResult::Skip
               : AutoDownloadResult::Failed;
  }
  if (alreadyDownloaded) {
    catalog.setLocalPath(record.contentId, destination.c_str());
    autoDownloadDone++;
    return AutoDownloadResult::Continue;
  }

  char progress[160];
  snprintf(progress, sizeof(progress), tr(STR_KOBO_DOWNLOAD_PROGRESS), static_cast<unsigned>(autoDownloadDone + 1),
           static_cast<unsigned>(autoDownloadTotal), metadata.title);
  const auto downloadResult = startBookDownload(record, metadata, destination, progress);

  pendingContentId.clear();
  pendingPath.clear();
  if (downloadResult == HttpDownloader::ABORTED) {
    return downloadCancelGoesHome() ? AutoDownloadResult::CancelledHome : AutoDownloadResult::Cancelled;
  }

  if (downloadResult == HttpDownloader::OK && !KoboCatalog::setPathLink(destination, record.contentId)) {
    LOG_ERR("KOBO", "Could not record path link for %s", record.contentId);
  }
  // The transfer closed the catalog; reopen it for the next row and to record
  // this row's local path.
  if (!catalog.isOpen() && !catalog.open()) {
    LOG_ERR("KOBO", "Cannot reopen catalog after download");
    return AutoDownloadResult::Failed;
  }
  if (downloadResult != HttpDownloader::OK) {
    LOG_ERR("KOBO", "Auto-download failed for %s: %d", record.contentId, static_cast<int>(downloadResult));
    if (downloadResult == HttpDownloader::UNAUTHORIZED || downloadResult == HttpDownloader::FILE_ERROR) {
      return AutoDownloadResult::Failed;
    }
    if (downloadResult == HttpDownloader::HTTP_ERROR) {
      const int status = HttpDownloader::lastStatus();
      const bool heapOk = ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP &&
                          ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
      // A deleted book is per-row; a low-heap or 5xx failure is systemic.
      if (heapOk && (status == 404 || status == 410)) return AutoDownloadResult::Skip;
    }
    return AutoDownloadResult::Failed;
  }

  catalog.setLocalPath(record.contentId, destination.c_str());
  autoDownloadDone++;
  return AutoDownloadResult::Continue;
}

void KoboShelfActivity::finishAutoDownload() {
  autoDownloadBatch = false;
  autoDownloadScan = 0;
  autoDownloadDone = 0;
  autoDownloadTotal = 0;
  if (!catalog.isOpen() && !catalog.open()) {
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  loadRows();
  state = State::BROWSING;
  requestUpdate(true);
}

void KoboShelfActivity::downloadFinished(const bool cancelled) {
  if (!cancelled && !pendingContentId.empty() && !pendingPath.empty()) {
    // The path link is what the reader uses to resolve a content id; write it
    // even if the catalog itself cannot be reopened.
    if (!KoboCatalog::setPathLink(pendingPath, pendingContentId)) {
      LOG_ERR("KOBO", "Could not record path link for %s", pendingContentId.c_str());
    }
    if (catalog.isOpen() || catalog.open()) {
      catalog.setLocalPath(pendingContentId.c_str(), pendingPath.c_str());
    }
  }
  pendingContentId.clear();
  pendingPath.clear();

  // Reload the catalog after the transfer; a cancel still returns to the list
  // so the user can retry.
  if (catalog.isOpen() || catalog.open()) loadRows();
  state = State::BROWSING;
  requestUpdate(true);
}

#endif  // CROSSPOINT_KOBO_SYNC
