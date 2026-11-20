# Pre-sleep Kobo autosync — work report

Status: implementation, review fixes, and verification complete.

## Environment / current-state check

- Branch: `feat/kobo-sync`, HEAD `19cce91c` (the task text says clean at
  `20d1e37d`; `20d1e37d` is HEAD~1, and the only unrelated untracked file is
  `review-tmp-presleep.log`).
- `~/.pio-venv/bin/pio` exists (PlatformIO 6.1.19).
- The task's "verified current state" is partly stale; actual code differs:
  - `KoboSyncActionActivity` has no `runSyncFlow()`; it ran
    `KoboSyncClient::syncCatalog()` then handed off to `KoboShelfActivity`
    (`resumeDownloads=true`) for the UI auto-download batch.
  - There is no `KoboProgressQueue`; a single deferred reading position lives
    in `KoboCredentialStore` (`pendingContentId`, `pendingProgressBp`,
    `pendingStatus`) and is flushed by `KoboSyncClient::flushPendingProgress()`.
  - The auto-download batch is `KoboShelfActivity::runAutoDownloadBatch()` /
    `downloadAutoRecord()`, coupled to `CatalogActivity` UI progress.
- Build envs in `platformio.ini`: `default` is ESP32-C3 X3/X4 with
  `CROSSPOINT_KOBO_SYNC=1`; `picco` is ESP32-S3 with the feature on; `slim` is
  C3 with the feature off.
- Host tests are registered as the PlatformIO target `unit-tests`
  (`scripts/register_unit_tests_target.py`), which runs CMake/CTest under
  `test/`. `pio test` is not the repo's host runner; it errored with
  "Nothing to build" and is not used below.

## Files changed

New:
- `lib/KoboSync/KoboPreSleepSync.h` — pure policy, wrap-safe probe budget,
  endpoint host/port parser.
- `src/network/KoboSyncService.h` / `.cpp` — reusable headless sync service.
- `test/kobo_sync/KoboPreSleepSyncTest.cpp` — host tests for the pure logic.

Modified:
- `src/main.cpp` — pre-sleep hook inside `enterDeepSleep()`, gated by
  `CROSSPOINT_KOBO_SYNC`; paints the existing `STR_KOBO_SYNCING` line, probes,
  releases activity memory, then runs the full sync.
- `src/activities/Activity.{h}` and `ActivityManager.{h,cpp}` — gated
  `releaseForNetworkSync()` hook so the reader/shelf can free rebuildable
  memory before TLS.
- `src/activities/reader/EpubReaderActivity.{h,cpp}` — extracted
  `queueKoboProgress()`, added a gated `prepareForSleep()` override so the
  current session's position is queued before the pre-sleep sync, and a gated
  `releaseForNetworkSync()` that frees `epub`/`section`/image caches before TLS
  (same pattern as `launchKoboStatusSync`). The finished-book folder move is
  preserved through the gated `releaseEpub()` helper.
- `src/activities/reader/EpubReaderChapterSelectionActivity.{h,cpp}` and
  `EpubReaderBookmarksActivity.{h,cpp}` — gated `releaseForNetworkSync()`
  overrides drop their `shared_ptr<Epub>` and row buffers so a child screen
  does not keep the book resident through TLS.
- `src/activities/ActivityManager.cpp` — during `ReplaceForSleep`, skips the
  outgoing activity's `loop()` in Kobo builds so a released reader cannot
  navigate away from the sleep screen.
- `src/activities/library/KoboShelfActivity.{h,cpp}` — gated
  `releaseForNetworkSync()` frees rows and closes the shelf catalog before the
  sleep sync opens its own; uses the shared suffix helper.
- `src/activities/settings/KoboSyncActionActivity.cpp` — UI path now calls
  `KoboSyncService::runFullSync(false)`; error text and shelf handoff preserved.
- `lib/KoboSync/KoboSyncClient.{h,cpp}` — replaced the unused `const bool*`
  cancel parameter with one allocation-free `CancelPredicate` + context for
  `syncCatalog()`; `request()`/`collectBody()` follow the same single contract.
- `lib/KoboSync/KoboDownloadPlan.h` — moved the content-id filename suffix
  helper here; `KoboShelfActivity` and the service share it.
- `docs/kobo-sync.md` — documents the timeout pre-sleep sync, the current
  book's position being queued on sleep, and the automatic-sync exception.
- `test/kobo_sync/CMakeLists.txt`, `test/kobo_sync/KoboDownloadPlanTest.cpp` —
  register the new suite and cover the suffix helper.

## New reusable entry point

```cpp
class KoboSyncService {
 public:
  struct Result {
    KoboSyncClient::Error error = KoboSyncClient::OK;
    uint16_t books = 0;
    uint16_t downloaded = 0;
    uint16_t skipped = 0;
    bool cancelled = false;
    bool downloadsIncomplete = false;
    bool catalogOpenFailed = false;
  };
  using ProgressCallback = std::function<void(const char* message, size_t done, size_t total)>;
  using CancelPredicate = KoboSyncClient::CancelPredicate;

  static bool probeEndpoint(uint32_t budgetMs);

  static Result runFullSync(bool headless, const ProgressCallback& progress = {},
                            CancelPredicate cancel = nullptr, void* cancelContext = nullptr);
};
```

`headless=true` flushes the pending position first, then runs the catalog
delta, then the auto-download batch. `headless=false` keeps the UI path's
existing post-feed flush and only does the catalog delta; the activity still
hands off to the shelf for the visible download screen.

## Hook location in enterDeepSleep

`src/main.cpp`, after `APP_STATE.saveToFile()` and before
`activityManager.goToSleep(fromTimeout)` and the Wi-Fi teardown:

```cpp
APP_STATE.saveToFile();

#if CROSSPOINT_KOBO_SYNC
  maybeRunPreSleepSync(fromTimeout);
#endif

  activityManager.goToSleep(fromTimeout);
```

`maybeRunPreSleepSync()` returns immediately unless
`kobo::shouldAttemptPreSleepSync(fromTimeout, KOBO_STORE.hasCredentials(),
WiFi.status() == WL_CONNECTED)` is true. Manual sleep (`fromTimeout == false`)
therefore gains no probe or sync delay. Quick-resume sleep modes skip the
status paint so their retained frame stays the reader page.

## Probe design

- Parse `KOBO_STORE.getBaseUrl()` into host/port with
  `kobo::parseEndpointAddress()` (http 80 / https 443 defaults, optional port,
  userinfo, bracketed IPv6).
- One TCP `WiFiClient::connect(host, port, remainingBudget)` with
  `PRE_SLEEP_PROBE_BUDGET_MS = 10000`. The remaining budget is wrap-safe; the
  post-connect check rejects a connect that finished after the deadline.
- No radio bring-up: the caller already checked `WiFi.status()`.
- DNS is bounded by lwIP's resolver, not by the 10 s constant; if it hangs, the
  device still sleeps after that bounded resolver timeout. This is the one
  known deviation from a strict wall-clock 10 s.

## Cancellation / watchdog / memory

- `preSleepSyncCancelled()` returns `gpio.rawInputActive() ||
  WiFi.status() != WL_CONNECTED`.
- The predicate is checked during feed paging (via the new
  `KoboSyncClient::CancelPredicate`) and during downloads. The download
  progress callback sets a `downloadCancelled` flag passed to
  `HttpDownloader::downloadToFile`, so a button/Wi-Fi drop aborts the current
  transfer; otherwise items finish at their boundary.
- `resetTaskWatchdogIfSubscribed()` is called from the cancel poll and download
  progress callback. The Arduino loop task is not WDT-subscribed
  (`loopTaskWDTEnabled = false` in the core), so these are safe no-ops there.
- `activityManager.releaseForNetworkSync()` runs only after the probe succeeds.
  It frees the reader's `epub`/`section`/image caches and the shelf's rows and
  catalog handles, matching the existing `launchKoboStatusSync` TLS-heap
  preparation.
- Per-item download failures mirror `KoboShelfActivity::downloadAutoRecord()`:
  permanent failures (404/410, missing/JSON metadata) skip; systemic failures
  stop the batch.

## Review round

Four read-only reviews were run against the candidate (correctness,
architecture, embedded, i18n/docs), followed by one targeted follow-up round.
Accepted fixes:

- UI "Sync Now" low-memory text: `Result::catalogOpenFailed` now distinguishes
  catalog-open OOM (`STR_MEMORY_ERROR`) from a TLS/heap `LOW_MEMORY`
  (`STR_KOBO_ERR_MEMORY`), preserving the old text.
- Removed the unused `const bool*` cancellation path from
  `KoboSyncClient::syncCatalog()`; one predicate contract remains, and it is
  threaded through `collectBody`, `fetchMetadata`, `putReadingState`, and
  `flushPendingProgress`.
- Wired the `ProgressCallback` into the pre-sleep path for per-item logging.
- Added `releaseForNetworkSync()` for the reader, shelf, chapter list, and
  bookmarks screen (embedded review: TLS heap floor, duplicate catalog handles,
  and child-held `shared_ptr<Epub>`).
- The download progress callback now feeds a cancel flag into the transfer.
- `ReplaceForSleep` skips the outgoing activity's `loop()` in Kobo builds, so a
  released reader cannot `finish()`/pop instead of sleeping.
- `releaseEpub()` preserves the finished-book folder move that the destructor
  previously performed.
- Quick-resume sleep modes skip the status paint, so the retained frame is the
  reader page rather than "Syncing Kobo library".
- Updated `docs/kobo-sync.md` for the new automatic path and the pre-feed
  position upload.

Rejected / documented, not fixed:

- The headless download batch still duplicates the shelf's per-book
  orchestration. The shelf's copy is tied to row storage, its progress screen,
  and cancel-to-home; extracting a shared batch would add an abstraction larger
  than the ~60 duplicated lines. The protocol client and the filename-collision
  helper are shared. `headless` is kept as the task's requested flag name.
- DNS is not bounded by the 10 s constant (see Probe design).
- The download connect/TLS handshake cannot observe a button press until the
  first body chunk; the socket timeout bounds it (60 s), and the first chunk
  arms the cancel flag.
- No low-battery guard was added: the path never brings the radio up, and the
  requirement explicitly asks for the sync; the adjacent 20 % guard exists for
  the plugin path that does bring Wi-Fi up.

## Verification (actual output)

All logs are under `review-tmp/scratch/`.

`pio run -e default` — SUCCESS:
```
RAM:   [==        ]  20.4% (used 66848 bytes from 327680 bytes)
Flash: [========= ]  86.5% (used 5671067 bytes from 6553600 bytes)
```

`pio run -e picco` — SUCCESS:
```
RAM:   [===       ]  34.8% (used 114012 bytes from 327680 bytes)
Flash: [====      ]  36.5% (used 5519382 bytes from 15138816 bytes)
```

`pio run -e slim` — SUCCESS:
```
RAM:   [==        ]  20.3% (used 66648 bytes from 327680 bytes)
Flash: [========  ]  83.4% (used 5466233 bytes from 6553600 bytes)
```

Slim symbol check:
```
slim Kobo symbol count: 0
nm -C .pio/build/slim/firmware.elf | grep -E "KoboSyncService|parseEndpointAddress|withContentSuffix|shouldAttemptPreSleepSync|probeEndpoint|preSleepSync|queueKoboProgress|releaseForNetworkSync"
# (no output)
default Kobo symbol count: 208
```

Host tests (`pio run -t unit-tests`) — 952/952 passed (baseline 943 plus 9
new tests):
```
100% tests passed out of 952
Total Test time (real) =   4.05 sec
```

`pio test` was tried as the task requested and is not the repo's host runner:
```
Error: Nothing to build. Please put your test suites to the '.../test' folder
```

Formatting: `./bin/clang-format-fix -g` could not run because neither
`clang-format-21` nor `clang-format` is installed in this environment. I
checked `git diff --check` (clean) and manually kept all changed lines within
the repo's 120-column limit.

## Hardware verification (human)

No hardware was available to me. Suggested checks on an X3/X4 with Kobo
configured and Wi-Fi already connected:

1. Set a short auto-sleep timeout, leave the device idle on the reader or shelf.
   Expect a brief "Syncing Kobo library" line, then the normal sleep screen.
2. Pull the server/AP before the timeout. Expect the device to sleep at the
   normal time after the probe fails.
3. Start a sync, then press any button during a download. Expect the transfer
   to stop and the device to sleep.
4. Repeat with a manual power-button sleep. Expect no added delay and no
   "Syncing" line.

DONE: all-done
