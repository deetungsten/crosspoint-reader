#include "KoboShelfActivity.h"

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KoboCredentialStore.h"
#include "KoboSyncClient.h"
#include "MappedInputManager.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
void appendKoboHeaders(std::vector<HttpDownloader::Header>& headers) {
  if (!KOBO_STORE.getCookie().empty()) headers.push_back({"Cookie", KOBO_STORE.getCookie()});
  headers.push_back({"x-kobo-device-id", KOBO_STORE.getDeviceId()});
}

std::string withContentSuffix(std::string path, const std::string& contentId) {
  const std::string suffix = " [" + contentId.substr(0, 8) + "]";
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return path + suffix;
  return path.substr(0, dot) + suffix + path.substr(dot);
}
}  // namespace

void KoboShelfActivity::onEnter() {
  CatalogActivity::onEnter();
  // The settings screen may have changed credentials since boot.
  KOBO_STORE.loadFromFile();
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  if (!KOBO_STORE.hasCredentials()) {
    fail(StrId::STR_KOBO_NO_CREDENTIALS);
    return;
  }
  checkAndConnectWifi();
}

void KoboShelfActivity::onExit() {
  books.clear();
  rowLabels.clear();
  rowItems.clear();
  catalog.close();
  if (navigatingToReader) {
    Activity::onExit();
    return;
  }
  CatalogActivity::onExit();
}

int KoboShelfActivity::listCount() const { return state == State::BROWSING ? static_cast<int>(rowItems.size()) : 0; }

void KoboShelfActivity::startBrowse() {
  pendingContentId.clear();
  pendingPath.clear();
  pageOffset = 0;
  nextPageOffset = 0;
  hasMorePages = false;
  state = State::LOADING;
  statusMessage = tr(STR_KOBO_SYNCING);
  requestUpdateAndWait();

  if (!catalog.isOpen() && !catalog.open()) {
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }

  KoboSyncClient::SyncStats stats;
  const auto result = KoboSyncClient::syncCatalog(catalog, stats);
  if (result != KoboSyncClient::OK) {
    LOG_ERR("KOBO", "Shelf sync failed: %d", static_cast<int>(result));
    errorMessage = KoboSyncClient::errorString(result);
    state = State::ERROR;
    requestUpdate(true);
    return;
  }

  loadRows();
  state = State::BROWSING;
  requestUpdate(true);
}

void KoboShelfActivity::loadRows() {
  books.clear();
  rowLabels.clear();
  rowItems.clear();

  books.reserve(MAX_SHELF_ROWS);
  rowLabels.reserve(MAX_SHELF_ROWS + 1);
  rowItems.reserve(MAX_SHELF_ROWS + 1);

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

  for (size_t i = 0; i < books.size(); i++) {
    fui::ListItem item;
    item.label = rowLabels[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
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
    item.actionValue = static_cast<int16_t>(books.size());
    rowItems.push_back(item);
  }
}

void KoboShelfActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_KOBO_SHELF));
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

void KoboShelfActivity::downloadBook(const size_t index) {
  if (index >= books.size()) return;
  pendingContentId.clear();
  pendingPath.clear();
  const KoboBookRecord record = books[index];

  kobo::Metadata metadata;
  auto result = KoboSyncClient::fetchMetadata(record.contentId, metadata);
  if (result != KoboSyncClient::OK) {
    errorMessage = KoboSyncClient::errorString(result);
    state = State::ERROR;
    requestUpdate(true);
    return;
  }
  if (metadata.title[0] == '\0') {
    std::memcpy(metadata.title, record.title, sizeof(metadata.title));
  }
  if (metadata.author[0] == '\0') {
    std::memcpy(metadata.author, record.author, sizeof(metadata.author));
  }

  const std::string url = metadata.downloadUrl;
  if (url.empty()) {
    fail(StrId::STR_DOWNLOAD_FAILED);
    return;
  }
  std::string destination = downloadDestination(metadata);
  // Two books can sanitize to the same "Author - Title.epub". Keep the
  // existing file (and its link) only when it is this same content id.
  if (Storage.exists(destination.c_str())) {
    std::string linkedId;
    if (!KoboCatalog::contentIdForPath(destination, linkedId) || linkedId != record.contentId) {
      destination = withContentSuffix(destination, record.contentId);
    }
  }
  pendingContentId = record.contentId;
  pendingPath = destination;

  // Release the row buffer and the SD index while TLS owns its record buffers.
  books.clear();
  rowLabels.clear();
  rowItems.clear();
  catalog.close();

  beginDownload(metadata.title);
  std::vector<HttpDownloader::Header> headers;
  appendKoboHeaders(headers);
  const auto downloadResult = downloadFile(url, destination, "", "", headers);
  finishDownload(downloadResult);
}

void KoboShelfActivity::activateIndex(const int index) {
  if (index < 0) return;
  if (static_cast<size_t>(index) == books.size()) {
    if (!hasMorePages) return;
    app.clearTapFlash();
    pageOffset = nextPageOffset;
    loadRows();
    requestUpdate(true);
    return;
  }
  if (static_cast<size_t>(index) > books.size()) return;
  app.clearTapFlash();

  const KoboBookRecord& record = books[static_cast<size_t>(index)];
  if ((record.flags & kobo_flags::DOWNLOADED) != 0 && record.localPath[0] != '\0' && Storage.exists(record.localPath)) {
    navigatingToReader = true;
    WiFi.disconnect(false);
    activityManager.goToReader(record.localPath);
    return;
  }
  downloadBook(static_cast<size_t>(index));
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
