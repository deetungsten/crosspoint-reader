#include "KoboSyncService.h"

#if CROSSPOINT_KOBO_SYNC

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "KoboCatalog.h"
#include "KoboCredentialStore.h"
#include "KoboDownloadPlan.h"
#include "KoboPreSleepSync.h"
#include "network/HttpDownloader.h"
#include "util/OpdsFilename.h"
#include "util/TaskWatchdog.h"

namespace {

std::string downloadDestination(const kobo::Metadata& metadata) {
  const char* folder = SETTINGS.opdsDownloadFolder;
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("KOBO", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  std::string path = haveFolder ? std::string(folder) + '/' : "/";
  path +=
      opdsBookFilename(metadata.author, metadata.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  return path;
}

void appendKoboHeaders(std::vector<HttpDownloader::Header>& headers) {
  if (!KOBO_STORE.getCookie().empty()) headers.push_back({"Cookie", KOBO_STORE.getCookie()});
  headers.push_back({"x-kobo-device-id", KOBO_STORE.getDeviceId()});
}

bool heapAllowsTls() {
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP &&
         ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
}

struct CancelPoll {
  KoboSyncService::CancelPredicate user = nullptr;
  void* context = nullptr;
};

// The abort callback fires during feed paging; feeding the watchdog there keeps
// a long page fetch from looking like a hung task.
bool pollCancel(void* raw) {
  resetTaskWatchdogIfSubscribed();
  auto* poll = static_cast<CancelPoll*>(raw);
  return poll->user != nullptr && poll->user(poll->context);
}

enum class DownloadOutcome { Downloaded, Skipped, Stop, Cancelled };

// Mirrors KoboShelfActivity::downloadAutoRecord(): permanent per-book failures
// are skipped, systemic failures stop the batch, and a missing local path link
// is still recorded when the file landed.
DownloadOutcome downloadRecord(KoboCatalog& catalog, const KoboBookRecord& record,
                               const KoboSyncService::ProgressCallback& progress, const size_t done,
                               const size_t total, const KoboSyncService::CancelPredicate cancel,
                               void* cancelContext) {
  kobo::Metadata metadata;
  const auto prepared = KoboSyncClient::fetchMetadata(record.contentId, metadata, cancel, cancelContext);
  if (prepared != KoboSyncClient::OK) {
    LOG_ERR("KOBO", "Pre-sleep metadata failed for %s: %d", record.contentId, static_cast<int>(prepared));
    return prepared == KoboSyncClient::NOT_FOUND || prepared == KoboSyncClient::JSON_ERROR ? DownloadOutcome::Skipped
                                                                                          : DownloadOutcome::Stop;
  }
  if (metadata.title[0] == '\0') std::memcpy(metadata.title, record.title, sizeof(metadata.title));
  if (metadata.author[0] == '\0') std::memcpy(metadata.author, record.author, sizeof(metadata.author));
  if (metadata.downloadUrl[0] == '\0') return DownloadOutcome::Skipped;

  std::string destination = downloadDestination(metadata);
  if (Storage.exists(destination.c_str())) {
    std::string linkedId;
    if (KoboCatalog::contentIdForPath(destination, linkedId) && linkedId == record.contentId) {
      catalog.setLocalPath(record.contentId, destination.c_str());
      return DownloadOutcome::Skipped;
    }
    destination = kobo::withContentSuffix(std::move(destination), record.contentId);
  }
  if (Storage.exists(destination.c_str())) {
    std::string linkedId;
    if (KoboCatalog::contentIdForPath(destination, linkedId) && linkedId == record.contentId) {
      catalog.setLocalPath(record.contentId, destination.c_str());
      return DownloadOutcome::Skipped;
    }
  }

  if (progress) progress(metadata.title, done, total);
  if (cancel != nullptr && cancel(cancelContext)) return DownloadOutcome::Cancelled;
  if (!heapAllowsTls()) {
    LOG_ERR("KOBO", "Low heap for pre-sleep download (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return DownloadOutcome::Stop;
  }

  std::vector<HttpDownloader::Header> headers;
  appendKoboHeaders(headers);
  // Release the catalog's SD handles while TLS owns its record buffers, as the
  // shelf does before each transfer.
  catalog.close();
  bool downloadCancelled = false;
  const auto downloaded = HttpDownloader::downloadToFile(
      metadata.downloadUrl, destination,
      [&](size_t, size_t) {
        resetTaskWatchdogIfSubscribed();
        if (cancel != nullptr && cancel(cancelContext)) downloadCancelled = true;
      },
      &downloadCancelled, "", "", headers);
  if (!catalog.isOpen() && !catalog.open()) {
    LOG_ERR("KOBO", "Cannot reopen catalog after pre-sleep download");
    return DownloadOutcome::Stop;
  }

  if (downloaded != HttpDownloader::OK) {
    LOG_ERR("KOBO", "Pre-sleep download failed for %s: %d", record.contentId, static_cast<int>(downloaded));
    if (downloaded == HttpDownloader::ABORTED) return DownloadOutcome::Cancelled;
    if (downloaded == HttpDownloader::HTTP_ERROR) {
      const int status = HttpDownloader::lastStatus();
      if (heapAllowsTls() && (status == 404 || status == 410)) return DownloadOutcome::Skipped;
    }
    return DownloadOutcome::Stop;
  }

  if (!KoboCatalog::setPathLink(destination, record.contentId)) {
    LOG_ERR("KOBO", "Could not record path link for %s", record.contentId);
  }
  catalog.setLocalPath(record.contentId, destination.c_str());
  return DownloadOutcome::Downloaded;
}

void runDownloads(KoboCatalog& catalog, const KoboSyncService::ProgressCallback& progress,
                  const KoboSyncService::CancelPredicate cancel, void* cancelContext,
                  KoboSyncService::Result& result) {
  uint16_t total = 0;
  for (uint16_t i = 0; i < catalog.count(); i++) {
    KoboBookRecord record;
    if (!catalog.readAt(i, record)) continue;
    const bool hasPath = record.localPath[0] != '\0';
    if (kobo::needsDownload((record.flags & kobo_flags::REMOVED) != 0, hasPath,
                            hasPath && Storage.exists(record.localPath))) {
      total++;
    }
  }
  if (total == 0) return;

  size_t done = 0;
  for (uint16_t i = 0; i < catalog.count(); i++) {
    if (cancel != nullptr && cancel(cancelContext)) {
      result.cancelled = true;
      return;
    }
    if (!catalog.isOpen() && !catalog.open()) {
      result.downloadsIncomplete = true;
      return;
    }
    KoboBookRecord record;
    if (!catalog.readAt(i, record)) continue;
    const bool hasPath = record.localPath[0] != '\0';
    if (!kobo::needsDownload((record.flags & kobo_flags::REMOVED) != 0, hasPath,
                             hasPath && Storage.exists(record.localPath))) {
      continue;
    }

    const auto outcome = downloadRecord(catalog, record, progress, done, total, cancel, cancelContext);
    resetTaskWatchdogIfSubscribed();
    if (outcome == DownloadOutcome::Cancelled) {
      result.cancelled = true;
      return;
    }
    if (outcome == DownloadOutcome::Stop) {
      result.downloadsIncomplete = true;
      return;
    }
    if (outcome == DownloadOutcome::Skipped) {
      result.skipped++;
      done++;
      continue;
    }
    done++;
    result.downloaded++;
  }
}

}  // namespace

bool KoboSyncService::probeEndpoint(const uint32_t budgetMs) {
  const kobo::EndpointAddress address = kobo::parseEndpointAddress(KOBO_STORE.getBaseUrl());
  if (!address.valid) {
    LOG_ERR("KOBO", "Pre-sleep probe: invalid endpoint");
    return false;
  }

  const uint32_t start = millis();
  const uint32_t remaining = kobo::remainingProbeBudgetMs(start, millis(), budgetMs);
  if (remaining == 0) return false;

  WiFiClient client;
  const int connected = client.connect(address.host.c_str(), address.port, static_cast<int32_t>(remaining));
  client.stop();
  const bool reachable = connected == 1 && !kobo::probeBudgetExpired(start, millis(), budgetMs);
  LOG_DBG("KOBO", "Pre-sleep probe %s:%u %s", address.host.c_str(), static_cast<unsigned>(address.port),
          reachable ? "ok" : "failed");
  return reachable;
}

KoboSyncService::Result KoboSyncService::runFullSync(const bool headless, const ProgressCallback& progress,
                                                     const CancelPredicate cancel, void* cancelContext) {
  Result result;
  if (!KOBO_STORE.hasCredentials()) {
    result.error = KoboSyncClient::NO_CREDENTIALS;
    return result;
  }

  // The last reading position matters more than the catalog, so upload it
  // before the feed on the sleep path. A failure is not fatal: syncCatalog
  // retries the queue at the end, and a deleted book must not block the rest of
  // the sync. The UI path keeps its existing post-feed flush.
  CancelPoll poll{cancel, cancelContext};
  if (headless) {
    const auto flushed = KoboSyncClient::flushPendingProgress(pollCancel, &poll);
    if (flushed != KoboSyncClient::OK) {
      LOG_ERR("KOBO", "Pre-sleep pending position failed: %d", static_cast<int>(flushed));
    }
  }

  if (cancel != nullptr && cancel(cancelContext)) {
    result.error = KoboSyncClient::CANCELLED;
    result.cancelled = true;
    return result;
  }

  KoboCatalog catalog;
  if (!catalog.open()) {
    result.error = KoboSyncClient::LOW_MEMORY;
    result.catalogOpenFailed = true;
    return result;
  }

  KoboSyncClient::SyncStats stats;
  result.error = KoboSyncClient::syncCatalog(catalog, stats, pollCancel, &poll);
  result.books = stats.books;
  if (result.error == KoboSyncClient::CANCELLED) result.cancelled = true;
  if (result.error != KoboSyncClient::OK) {
    catalog.close();
    return result;
  }

  if (headless && KOBO_STORE.getAutoDownload()) runDownloads(catalog, progress, pollCancel, &poll, result);

  catalog.close();
  return result;
}

#endif  // CROSSPOINT_KOBO_SYNC
