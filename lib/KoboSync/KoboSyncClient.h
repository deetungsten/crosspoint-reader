#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstdint>
#include <string>

#include "KoboCatalog.h"
#include "KoboSyncTypes.h"

// HTTP client for calibre-web's Kobo integration.
//
// Routes (all under <endpoint>/kobo/<auth-token>):
//   GET  /v1/library/sync                incremental catalog feed
//   GET  /v1/library/<uuid>/metadata     fresh download link
//   GET  /v1/library/<uuid>/state        reading position
//   PUT  /v1/library/<uuid>/state        upload reading position
//   GET  /<uuid>/<w>/<h>/false/image.jpg cover image
class KoboSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    LOW_MEMORY,
    CANCELLED,
  };

  struct SyncStats {
    uint16_t books = 0;
    uint16_t readingStates = 0;
    uint16_t tags = 0;
    uint16_t pages = 0;
  };

  // Pull every page of the incremental feed into `catalog`. The new sync token
  // is persisted only after the whole paging sequence succeeds, so an
  // interrupted sync repeats the same page set next time.
  static Error syncCatalog(KoboCatalog& catalog, SyncStats& stats, const bool* cancelFlag = nullptr);

  static Error fetchMetadata(const std::string& contentId, kobo::Metadata& out);
  static Error fetchReadingState(const std::string& contentId, kobo::ReadingState& out);
  static Error putReadingState(const std::string& contentId, float progressPercent, kobo::ReadStatus status,
                               const std::string& location);
  // Uploads a position queued by the reader on close. Flushed after a
  // successful catalog sync so leaving a book never blocks on TLS.
  static Error flushPendingProgress();

  static std::string coverUrl(const std::string& contentId, int width, int height);

  static const char* errorString(Error error);

  static int lastHttpCode;
};

#endif  // CROSSPOINT_KOBO_SYNC
