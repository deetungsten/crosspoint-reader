#pragma once

#include "KoboSyncConfig.h"

#if CROSSPOINT_KOBO_SYNC

#include <cstddef>
#include <cstdint>
#include <functional>

#include "KoboSyncClient.h"

// Orchestrates the Kobo sync outside an activity, so the timeout sleep path can
// reuse the same protocol code as the UI. The probe is a cheap TCP reachability
// check; the full sync has no artificial deadline and is bounded only by the
// caller's cancel predicate.
class KoboSyncService {
 public:
  struct Result {
    KoboSyncClient::Error error = KoboSyncClient::OK;
    uint16_t books = 0;
    uint16_t downloaded = 0;
    uint16_t skipped = 0;
    bool cancelled = false;
    bool downloadsIncomplete = false;
    // Distinguishes a catalog-open OOM (the activity shows its generic memory
    // error) from a TLS/heap failure inside the sync (the Kobo error text).
    bool catalogOpenFailed = false;
  };

  using ProgressCallback = std::function<void(const char* message, size_t done, size_t total)>;
  using CancelPredicate = KoboSyncClient::CancelPredicate;

  // TCP connect to the configured endpoint with a hard total budget. Does not
  // bring Wi-Fi up; the caller must already be connected.
  static bool probeEndpoint(uint32_t budgetMs);

  // Progress flush first (last reading position), then the catalog delta, then
  // the auto-download batch when headless is true and the setting is on.
  // Per-item download failures follow the shelf's skip/stop classification.
  static Result runFullSync(bool headless, const ProgressCallback& progress = {}, CancelPredicate cancel = nullptr,
                            void* cancelContext = nullptr);
};

#endif  // CROSSPOINT_KOBO_SYNC
