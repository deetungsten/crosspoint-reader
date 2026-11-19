# Task: Pre-sleep Kobo autosync (timeout-triggered)

## Sandbox rules (read first — violating these kills the run)
- Your FIRST command must be `mkdir -p review-tmp/scratch`. All files you create (logs, reports, scratch) live under `review-tmp/` in the repo. `/tmp` does NOT exist for you; writes outside the repo are auto-rejected and the run dies silently with exit 0.
- Use paths RELATIVE to the working directory in every command. Never invoke `/abs/path/...`. On any permission rejection, switch to `review-tmp/scratch/` and continue — do not stop.
- Do NOT `git add`, `git commit`, or `git push`. The parent commits after verifying.

## Environment
- Repo: this directory (branch `feat/kobo-sync`). Working tree is CLEAN at commit 20d1e37d.
- Build: `~/.pio-venv/bin/pio run -e default` (esp32-s3 full build). Also must stay green: `~/.pio-venv/bin/pio run -e picco` and `~/.pio-venv/bin/pio run -e slim` (slim has CROSSPOINT_KOBO_SYNC=0 — all new code must compile out).
- Host tests: `~/.pio-venv/bin/pio test` — baseline right now is **943 PASS / 0 FAIL** (plus a few env-specific skips). Do not break them.
- No network access to the real calibre-web instance is available to you; unit-test logic, don't integration-test the server.

## Verified current state (do not re-derive)
- Kobo sync client lives in `lib/KoboSync/`: `KoboSyncClient` (feed paging, catalog, ReadingPosition get/put), `KoboSyncConfig` (credentials, `isConfigured()`), `KoboCatalogIndex`, `KoboFeedParser`, `KoboSyncPaths`.
- UI sync flow: `src/activities/settings/KoboSyncActionActivity.{h,cpp}` — `runSyncFlow()` does: ensure WiFi → sync feed (catalog delta) → auto-download batch (sequential, cancellable, resumable) → progress flush. It renders progress to the screen; it is UI-coupled.
- Pending reading progress: `src/activities/reader/KoboProgressQueue.{h,cpp}` — queue of unsent positions, flushed via `KoboSyncClient` when online.
- Reading-position apply-on-open: `src/activities/reader/KoboStatusSyncActivity`.
- Deep sleep entry: `src/main.cpp`, function `enterDeepSleep(bool fromTimeout)` (~line 458). Sequence: power lock → trusted time note → `deepSleepInProgress = true` → BLE stop → plugin sleep events → `APP_STATE.saveToFile()` → `activityManager.goToSleep(fromTimeout)` → **WiFi teardown (`WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`)** → display/tilt deep sleep → `powerManager.startDeepSleep(...)`.
- The main loop calls `enterDeepSleep(true)` on idle timeout and `enterDeepSleep(false)` on explicit power-button sleep.

## The feature (user's exact words)
"when before it times out and shutdown, to autosync once. Try for 10 seconds, if it syncs, keeps syncing until done. If it doesn't work, just time out and shutdown."

Interpretation / requirements:
1. **Trigger:** only when `fromTimeout == true`, Kobo is configured (`KoboSyncConfig::isConfigured()`), and WiFi is currently connected (`WiFi.isConnected()`). Manual sleep (`fromTimeout == false`) must NOT gain any delay. If WiFi is off/disconnected, skip instantly — do not bring the radio up just for this.
2. **Placement:** inside `enterDeepSleep()` BEFORE `activityManager.goToSleep(fromTimeout)` and before the WiFi teardown. Show a brief on-screen status line ("Syncing before sleep…") using the renderer, best-effort (the sleep screen paints over it anyway).
3. **10-second probe:** attempt a cheap reachability check against the configured endpoint (e.g. a sync-feed GET or HEAD with short socket timeouts, or a TCP connect to host:port) with a hard total budget of 10 seconds. If it fails or times out → skip sync entirely and continue into sleep exactly as today.
4. **If the probe succeeds:** run the full sync to completion — progress flush first (most important: last reading position), then catalog delta, then the auto-download batch — with NO artificial time limit ("keeps syncing until done"). Reuse the existing sync logic; refactor the headless parts of `KoboSyncActionActivity::runSyncFlow()` into a reusable entry point (e.g. `KoboSyncService::runFullSync(headless=true, progressCb)`) rather than duplicating protocol code. Keep the UI path behavior identical.
5. **Abort conditions during the long sync:** (a) user presses any button → stop at the next batch boundary and go to sleep; (b) WiFi drops → stop; (c) any per-item failure → same semantics as the existing auto-download batch (skip item, continue). Wrap everything so NO exception/error can prevent sleep from happening — worst case the device sleeps late, never "never".
6. **Build gating:** all new code inside `#if CROSSPOINT_KOBO_SYNC` (or the CMake/source-gating pattern the existing Kobo files use — check how `KoboSyncActionActivity` is gated and match it). `slim` build must contain zero new Kobo symbols.
7. **Watchdog safety:** the ESP32 task watchdog must not fire during the long sync. The existing sync flow already yields between items; match its pattern (call `esp_task_wdt_reset()`/`vTaskDelay(1)` the same way the UI flow does).

## Deliverables (in order)
1. `review-tmp/presleep-report.md` — create it EARLY (right after reading the code) and rewrite after every step; a run that ends without this file is a failed run.
2. Implementation per requirements above.
3. A host unit test where logic is extractable (e.g. the probe-budget decision, "should we sync" predicate) under `test/kobo_sync/`. UI/network code cannot be host-tested — say so in the report instead of forcing a fake test.
4. Verification, paste actual output into the report: `pio run -e default` SUCCESS with flash/RAM %, `pio run -e picco` SUCCESS, `pio run -e slim` SUCCESS + `grep -c Kobo` on the slim elf/map shows no new symbols, `pio test` ≥ baseline PASS.
5. Report must state: files changed, the new reusable entry point signature, where the hook sits in `enterDeepSleep`, probe design chosen, and end with the exact line `DONE: all-done` or `DONE: partial: <what's missing>`.

## Hard constraints
- Do not change behavior of the manual (button) sleep path.
- Do not change the UI sync flow's user-visible behavior (refactor only).
- Do not touch `platformio.ini` envs, partitions, or the version bump script.
- Keep the diff minimal and in the house style (look at neighboring Kobo files for naming/comment density).
