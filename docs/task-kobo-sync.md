# Task: Kobo (calibre-web) sync client — "KoboShelf" sync feature

## Goal

Add a sync feature to CrossPoint firmware that speaks the **Kobo device sync protocol as
implemented by calibre-web**, so an Xteink X4 / Boox Picco running CrossPoint syncs against
the user's calibre-web instance exactly like a real Kobo eReader does. The user already runs
several real Kobos pointed at calibre-web (via `api_endpoint` override); this firmware must be
a drop-in peer of those devices: same endpoint, same per-user auth token, same books/shelves
feed, same reading-position ("status") sync.

User-facing requirement (verbatim intent): "I want to be able to add an API endpoint address
and token, then sync just like a Kobo ereader — status and books and all."

## Authoritative protocol reference (fetch these first, save under `docs/reference/kobo/`)

The server is calibre-web's Kobo integration. Fetch and keep as reference:
- https://raw.githubusercontent.com/janeczku/calibre-web/master/cps/kobo.py  (all routes + payloads)
- https://raw.githubusercontent.com/janeczku/calibre-web/master/cps/kobo_auth.py  (auth model)
- https://raw.githubusercontent.com/janeczku/calibre-web/master/cps/SyncToken.py  (sync token format)
- https://github.com/janeczku/calibre-web/wiki/Kobo-Integration

Protocol summary (verify against the sources above — they are the truth):
- All routes live under `<base>/kobo/<auth_token>/v1/...`. The per-user **auth token is a path
  segment** generated in calibre-web's profile page ("Create/View Kobo Sync Key").
  The user will enter an endpoint (e.g. `http://192.168.1.50:8083`) and that token separately.
- calibre-web sets a **login cookie** on responses; capture `Set-Cookie` and replay it on
  subsequent requests (book download links hit non-Kobo endpoints that need the cookie).
- Key endpoints:
  - `POST /v1/auth/device` — body `{"DeviceId": "...", "UserKey": "..."}` → dummy
    `{"AccessToken","RefreshToken","TokenType","UserKey","TrackingId"}`. Send it once at
    "test connection" time to mimic device registration.
  - `GET /v1/library/sync` — incremental catalog feed. Request carries the previous sync token
    (see SyncToken.py; header names in kobo.py, e.g. `x-kobo-sync-token`); response is a JSON
    array of entries (`ItemType: "Book"`, `ContentKey.Id`, `ContentVersion`, `BookMetadata`
    with title/authors, `Links[]` incl. `FirstReaderLink`/download URL) and response headers
    carry the new sync token + `x-kobo-sync: continue` paging semantics.
  - `GET /v1/library/<bookId>/metadata` — fresh `DownloadLink.Url` per book.
  - `GET /v1/library/<bookId>/coverimage` — cover image.
  - Reading position ("status"): the `ReadingPosition` routes in kobo.py (PUT to upload
    `{"Progress": 0..1, "LocationType", "LocationUri", "LastTimeOpened", ...}`, GET to fetch)
    keyed by volume/user. calibre-web feeds stored positions back through the sync entries.
  - Shelves: calibre-web surfaces shelf membership through the same sync feed / metadata
    (mirror what a real Kobo would see; do not invent new endpoints).

## Existing code to mirror (read these before designing)

- `lib/KOReaderSync/` — `KOReaderSyncClient` + `KOReaderCredentialStore` (PersistableStore on SD,
  XOR+base64 obfuscation tied to device MAC, ArduinoJson). New `lib/KoboSync/` should follow this
  shape: `KoboSyncClient` + `KoboCredentialStore` (fields: endpoint base URL, auth token,
  device id (persist a generated UUID), last sync token, last sync time).
- `src/activities/reader/KOReaderSyncActivity.*` — sync UI + progress apply/upload conflict flow
  (`KOReaderSyncBehavior::SMART` ask/apply/upload pattern). Reuse this UX pattern for Kobo status.
- `src/activities/CatalogActivity.*` + `src/network/HttpDownloader.*` — OPDS browse + download
  flow (progress UI, cancel, headers). The Kobo shelf browser should reuse `HttpDownloader`
  (it already supports custom headers) and this activity's download UX.
- `src/activities/settings/OpdsServerListActivity.*` — pattern for a settings entry that manages
  a server list/credentials.
- `src/activities/reader/EpubReaderActivity.cpp` — where KOReader sync is launched on open/close
  (`launchKOReaderSync`, lines ~938–1456). Hook Kobo position fetch/upload at the same points.
- `lib/HttpHeader/HttpHeader.h`, `lib/JsonParser`, `lib/TrustedTime`, `lib/Serialization` —
  use these, don't reinvent.

## Deliverables (this task)

1. `lib/KoboSync/` — `KoboCredentialStore` + `KoboSyncClient`:
   - auth/device registration, cookie capture/replay, sync-token persistence + paging,
     catalog fetch (streaming/bounded JSON parse — ArduinoJson with a filter or incremental
     parse; the ESP32-C3 baseline has ~380 KB RAM, no PSRAM: never buffer the whole feed),
     metadata + download-link fetch, cover URL, ReadingPosition get/put.
   - Device identification headers a real Kobo sends where cheap (`x-kobo-device-id`,
     `x-kobo-device-model`, `x-kobo-product-name`, `x-kobo-platform-id`...), stable per device.
2. Settings UI: "Kobo Sync" screen — endpoint URL + token entry (reuse existing text-entry
   widgets), "Test connection", "Sync now", sync-behavior toggle (ask/smart like KOReader).
   Wire into the settings list and i18n system (see `docs/i18n.md` / translation workflow in
   `.agents/rules/generated-files-cache.md` — add strings the repo's way).
3. "Kobo Shelf" browse activity reachable from home/menu: list synced books (title/author,
   new-since-last-sync marker), download to the library folder via HttpDownloader (cookie +
   headers), record book-id ↔ local-file mapping in a small persisted index so progress sync
   can resolve which ContentKey.Id a local file corresponds to.
4. Reading status: on opening a mapped book, fetch remote ReadingPosition and offer apply if
   further along (SMART behavior like KOReaderSyncActivity); on close/periodic save, upload
   progress. Progress % from the existing EPUB position machinery.
5. Feature gating: the whole feature must cost nothing on builds that don't enable it
   (SCOPE.md / "flash size" rule). Put it behind a build flag (e.g. `CROSSPOINT_KOBO_SYNC`)
   enabled for `default` (X4), `x4c`, `x4pro`, and `picco` envs, or follow whatever opt-in
   mechanism SCOPE.md prescribes — check SCOPE.md and `.agents/rules/hardware-resources.md`
   first and follow it.
6. Tests where the repo's `test/` harness allows (sync-token round-trip, feed parsing from
   captured calibre-web JSON fixtures — you can generate fixtures by hand from kobo.py shapes).
7. Short user doc `docs/kobo-sync.md` (endpoint + token setup against calibre-web) and a
   summary of what was built + how to verify, in `docs/task-kobo-sync-result.md`.

## Hard constraints

- **No git commit / push / branch.** Leave all changes uncommitted for human review.
- Do not modify anything under `freeink-sdk/` (pinned submodule).
- Memory discipline: justify every allocation; no unbounded buffers; stream or chunk the sync
  feed and downloads. Check `.agents/skills/heap-discipline`.
- Follow `.agents/rules/coding-standards.md`, UI rules in `.agents/rules/ui-activities.md`.
- Verification: `~/.pio-venv/bin/pio run -e default` and `~/.pio-venv/bin/pio run -e picco`
  must compile cleanly and are VERIFIED WORKING (pioarduino fork 6.1.19; do NOT reinstall or
  upgrade platformio — stock PlatformIO 6.2.0 breaks the build with SCons.Tool.FortranCommon). Run the repo's
  host-side test suite (`test/` — check `.agents/rules/testing-debugging.md` for the command).
  Negative/partial results are acceptable deliverables — document what didn't work instead of
  faking it.
- Keep the diff reviewable: new code in `lib/KoboSync/` + `src/activities/settings/KoboSync*` +
  `src/activities/library/KoboShelf*` (names indicative; follow repo layout), minimal touch
  points into existing files.
