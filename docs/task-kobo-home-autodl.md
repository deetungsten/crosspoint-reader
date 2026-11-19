FEATURE TASK (continuation; repo already has the Kobo sync feature committed on branch feat/kobo-sync): four parts.

## Part 0 — REMOVE the Test Connection feature (user decision: it is noise; sync works)

- Remove the "Test Connection" menu item from KoboSyncSettingsActivity and the TEST mode from KoboSyncActionActivity (keep the SYNC mode).
- Remove KoboSyncClient::testConnection() and its i18n strings (STR_KOBO_TEST_CONNECTION, STR_KOBO_CONNECTED, STR_KOBO_CONNECTION_FAILED if unused elsewhere).
- IMPORTANT: testConnection() also flushed queued close-time progress uploads. Verify flushPendingProgress() still runs on the remaining paths (after successful catalog sync / shelf visit) so close-time positions still upload; if it only ran in testConnection, wire it into the sync path.
- A previous agent may have left half-finished edits to testConnection()/request() in the working tree — they become moot when the function is deleted; clean up anything that no longer compiles.
- Update docs/kobo-sync.md to drop Test Connection steps.


## Part 1 — Kobo entry point on the Home screen

Add a "Kobo Sync" item to the home menu (follow how existing home menu entries are added — inspect HomeActivity / the home theme menu items / UITheme; match existing patterns and i18n usage). Tapping it opens the Kobo hub: prefer opening KoboShelfActivity directly (it already syncs on visit and has a menu); include a menu option there to reach KoboSyncSettingsActivity so settings remain reachable. Only present the home item when CROSSPOINT_KOBO_SYNC=1 AND credentials are configured (hide otherwise so non-Kobo users never see it).

## Part 2 — Auto-download all books on sync (Kobo-style full library on SD)

Requirement (user): "like a Kobo — when it syncs it downloads all the books to the SD card and I can choose the book easily."

Design:
- New setting in KoboSyncSettingsActivity: "Auto-download books" (default ON when credentials configured), stored in KoboCredentialStore (persisted, bump cfgVersion handling per repo cache-versioning rules if needed).
- After a catalog sync that delivered new/changed entries (Sync Now, shelf visit, or new home entry), if auto-download is ON: iterate catalog entries lacking a local file (links mapping), download each sequentially via the existing HttpDownloader path used by KoboShelfActivity (same folder, same filename collision rules, same link registration so progress sync keeps working). Show a progress screen (reuse/extend the existing download UI: "Downloading 12/82 — Title", cancel button; cancel stops the batch, remaining books resume on next sync visit).
- Skip entries whose file already exists on SD (re-check at download time, not just from the map).
- KEPUB is fine to download (it is EPUB-compatible for this reader).
- Memory discipline: one download at a time, no catalog-wide buffering (heap-discipline rules).
- KoboShelfActivity: entries already downloaded show a downloaded marker and open in the reader directly (if that already exists, keep it); the shelf remains the manual browse/download UI.

## Part 3 — Fix bugs observed in live serial logs

1. `[KOBO] Failed to open file for reading: /.crosspoint/kobo/links/19673727.txt` appears repeatedly — some code path reads a links file that does not exist; treat missing link files as "not downloaded" without logging an error (or check existence first). Find the caller and fix.
2. A sync reported `books=82` (catalog rows delivered) but a later shelf visit showed an apparently empty list (user reported "synced 84 books but nothing shows"). Verify catalog rows persist across activities and reboots (catalog.bin write path, header count, flush); reproduce via the host test harness if possible; fix if rows are lost. Note the shelf visit itself triggers a sync that returned books=0 — the shelf must list ALL catalog rows, not just newly delivered ones.
3. Serial shows repeated device restarts shortly after KoboSyncAction/KoboShelf exits. Determine whether these are user power cycles or firmware resets (check for silentRestart calls in Kobo code paths that should not restart; KoboStatusSyncActivity's restart is expected, the action/shelf screens' are not). If firmware-caused, fix; if inconclusive, document.

## Verification (all required)

- ~/.pio-venv/bin/pio run -e default, -e picco, -e slim compile (slim = flag off, no Kobo symbols).
- Host suite: cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test — all pass; add tests for any new pure logic (e.g. "needs download" decision).
- Update docs/kobo-sync.md (home entry, auto-download behavior, cancel/resume semantics).

## Constraints

No git commit. Minimal diffs, follow .agents/rules (ui-activities, heap-discipline, coding-standards, i18n workflow). Feature stays fully gated by CROSSPOINT_KOBO_SYNC.
