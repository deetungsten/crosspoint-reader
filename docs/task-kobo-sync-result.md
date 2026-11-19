# Kobo Sync implementation result

Status: implemented, host-tested, and compiled for the `default` (X4/X3,
ESP32-C3), `picco`, and flag-off `slim` targets. Not tested on physical
hardware in this session.

## State found at the start of this session

A previous session had left the feature almost complete but not linkable:
`KoboShelfActivity` declared `handleCustomInput()` and `restartOnExit()`
overrides that had no definitions, so `pio run -e default` failed at the link
step with two undefined references. The host test suite built and passed.
Those declarations were removed (the `CatalogActivity` implementations are the
intended behavior), and the build was green before further work started.

## What was built

### `lib/KoboSync/`

- `KoboCredentialStore` — persisted endpoint, per-user auth token (obfuscated
  with the device MAC, same scheme as the KOReader store), generated device id,
  login cookie jar, last sync token, last sync time, sync behavior, and
  sync-on-open preference. Pasting calibre-web's full `api_endpoint`
  (`.../kobo/<token>`) splits the token out automatically.
- `KoboSyncClient` — device registration (`POST /v1/auth/device`), incremental
  catalog sync with `x-kobo-synctoken` replay and `x-kobo-sync: continue`
  paging, metadata/download-link fetch, reading-state get/put
  (`/v1/library/<uuid>/state`), cover URL, and Kobo device-identification
  headers. `Set-Cookie` is captured and replayed on later requests (including
  downloads). A close-time position queued by the reader is flushed by
  `testConnection()` and after a successful catalog sync.
- `KoboFeedParser` — streaming SAX parser over the bounded
  `StreamingJsonParser`; it keeps one fixed `FeedEntry` and emits entries as
  objects close, so a first sync of thousands of books never buffers the feed.
  It handles calibre-web's `NewEntitlement` / `ChangedEntitlement` /
  `ChangedReadingState` / `NewTag` / `ChangedTag` wrappers.
- `KoboCatalog` — SD-backed fixed-record catalog (`catalog.bin`), a sorted
  offset map (`catalog.map`) for O(log n) lookup up to 256 books, and
  `links/<hash>` files that map a local EPUB path to its Kobo `ContentKey.Id`.
  The map rebuild uses a bounded 256-entry scratch array; larger catalogs are
  marked "linear lookup" in the map header and keep working without a
  library-sized allocation. An unsorted-tail marker in the map header makes a
  crash before `finishSync()` rebuild instead of binary-searching stale order.
- `KoboSyncToken` — base64 JSON token round-trip with the six calibre-web
  timestamps, plus verbatim replay of official-store dotted tokens.
- `KoboSyncTime` — ISO-8601 UTC parsing for feed timestamps.

### UI and integration

- `Settings → Network → Kobo Sync`:
  `src/activities/settings/KoboSyncSettingsActivity.*` — server URL, auth
  token, Test Connection, Sync Now, Ask/Smart behavior, Sync on Open, Browse
  Kobo Shelf, and Clear Credentials.
- `src/activities/settings/KoboSyncActionActivity.*` — owns the Wi-Fi
  connect/teardown and runs the blocking test/sync behind a painted status
  screen.
- `src/activities/library/KoboShelfActivity.*` — lists the synced books with
  title/author, `*` new marker, downloaded marker, and shelf name. The list is
  paged in 48-row pages with a **Next Page** row. Download uses
  `HttpDownloader` with the captured cookie and records the book-id ↔
  local-path mapping; filename collisions get a content-id suffix instead of
  overwriting. Selecting a downloaded row opens it.
- `src/activities/reader/KoboStatusSyncActivity.*` — fetches the reading state,
  compares whole-book percentages, applies the further position in Smart mode,
  uploads the local one when it is ahead, and shows the Ask Every Time choice
  through the themed option popup. Back/tap-outside dismissal returns to the
  reader instead of leaving a dead screen.
- Reader hooks in `src/activities/reader/EpubReaderActivity.cpp`:
  - `Sync Progress` uses the Kobo position when the book has a content-id link.
  - `Sync on Open` can run the fetch when a mapped book opens.
  - Closing a mapped book queues its position; the next Kobo network action
    uploads it without blocking the reader exit path on TLS.
- `src/main.cpp` loads the Kobo store at boot and generates the device id.

### Feature gating

`CROSSPOINT_KOBO_SYNC` defaults to `0` in `lib/KoboSync/KoboSyncConfig.h`.
`platformio.ini` enables it for `default`, `gh_release`, `gh_release_rc`,
`x4c*`, `x4pro*`, and `picco*`. All new UI sources and headers are wrapped in
the flag, so other targets compile empty translation units. A `slim` build with
the flag off was checked: it links successfully and its ELF contains no
`KoboSync` / `KoboCatalog` / `KoboShelf` / `KoboStatus` / `KoboCredential`
symbols.

## Verification

Commands run from the repository root:

```bash
cmake -S test -B build/test
cmake --build build/test -j 8
ctest --test-dir build/test --output-on-failure
~/.pio-venv/bin/pio run -e default
~/.pio-venv/bin/pio run -e picco
~/.pio-venv/bin/pio run -e slim
```

Recorded outcomes (final run):

| Check | Result |
| --- | --- |
| `KoboSyncTest` (10 tests: token round-trip/malformed/store-token and feed parser whole/1-byte/17-byte chunks) | PASS |
| full `ctest` suite (936 tests) | PASS |
| `pio run -e default` | SUCCESS |
| `pio run -e picco` | SUCCESS |
| `pio run -e slim` (flag off) | SUCCESS |

Resource use on the default C3 build: RAM 66,856 / 327,680 bytes (20.4%),
flash 5,653,715 / 6,553,600 bytes (86.3%). The `picco` build uses RAM
114,012 / 327,680 bytes (34.8%) and flash 5,504,314 / 15,138,816 bytes (36.4%).
The slim flag-off build uses 5,465,411 bytes of flash; the difference includes
the feature plus the default env's debug log level, so it is not a pure
feature-size measurement.

## What works

- Endpoint + token setup, including pasting the full calibre-web
  `api_endpoint`.
- Device registration / "Test Connection" and cookie capture/replay.
- Incremental catalog sync with sync-token persistence and `x-kobo-sync:
  continue` paging, parsed as a stream.
- Shelf browsing with new/downloaded markers, shelf names, paging, downloads
  through `HttpDownloader` with the login cookie, and path ↔ content-id
  mapping.
- Reading-state fetch/apply/upload with SMART and ASK flows, manual
  **Sync Progress**, optional **Sync on Open**, and deferred close-time upload.
- Build gating: the feature is compiled out of flag-off targets.

## What does not work / limitations

- No physical-device test was possible in this session. Wi-Fi/TLS behavior,
  calibre-web cookie handling, and the reader progress mapping still need a
  real X4/Picco check against a live calibre-web instance.
- Only the calibre-web feed shape is parsed; the official Kobo store proxy
  entries are not a target.
- Shelves are read-only and best-effort: a tag for a book that has not yet
  entered the catalog is dropped (calibre-web normally sends books before
  tags), and membership is additive. Removing a book from a shelf or deleting
  a shelf (`DeletedTag`) is not reflected until the catalog is cleared and
  synced again.
- Covers are not displayed yet; `KoboSyncClient::coverUrl()` is ready for a
  later UI.
- The catalog map is sorted in a bounded 256-entry buffer. Larger libraries
  keep working through linear SD lookups, which makes incremental sync slower
  but does not lose data.
- Only the most recently closed book's position is queued in
  `/.crosspoint/kobo.json`; closing several books before the next network
  action keeps only the last one.
- There is no background/periodic sync. Each sync run follows at most 20 feed
  pages (about 2,000 entries); a larger first sync needs another **Sync Now**.
- HTTPS uses the firmware's insecure client path; a LAN calibre-web over plain
  HTTP is the intended setup.
- The host harness does not provide SD-card or HTTP stubs, so catalog storage,
  HTTP paging, and UI flows are not covered by automated tests. Token and feed
  parsing are covered with a captured-shape fixture.
- `KoboSyncTest` covers 10 cases; there is no automated test for the settings,
  shelf, or status activities.

## Scope note

`SCOPE.md` currently lists new external network connectors as temporarily
closed. This task explicitly requested the connector, so it is delivered
opt-in behind `CROSSPOINT_KOBO_SYNC` to keep non-enabled builds unchanged. The
human owner should decide whether it belongs in the shared firmware or a
downstream fork before merging.

No git commit, push, or branch was created; all changes are left uncommitted
for review.
