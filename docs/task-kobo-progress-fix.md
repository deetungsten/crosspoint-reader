# Task: Fix buggy reading-progress sync (Kobo/calibre-web)

## Sandbox rules (read first — violating these kills the run)
- Your FIRST command must be `mkdir -p review-tmp/scratch`. All files you create (logs, reports, scratch) live under `review-tmp/` in the repo. `/tmp` does NOT exist for you; writes outside the repo are auto-rejected and the run dies silently with exit 0.
- Use paths RELATIVE to the working directory in every command. Never invoke `/abs/path/...`. On any permission rejection, switch to `review-tmp/scratch/` and continue — do not stop.
- Do NOT `git add`, `git commit`, or `git push`. The parent commits after verifying.

## Environment
- Repo: this directory (branch `feat/kobo-sync`, clean at commit 92d7a4c2).
- Build: `~/.pio-venv/bin/pio run -e default`; also keep green: `-e picco`, `-e slim` (slim = CROSSPOINT_KOBO_SYNC off, must contain zero Kobo symbols).
- Host tests: `~/.pio-venv/bin/pio run -t unit-tests` — baseline **952/952 PASS**. Do not break them. (`pio test` is NOT the runner.)
- Live server available for protocol ground-truth: calibre-web at `http://192.168.1.10:8084` with Kobo sync key `80f8aacb9d50c4d22ca52804b15fab40`. You may GET/PUT against it to observe real behavior (it is the user's own server; do not delete books or tags; reading-state PUTs are fine).

## Symptom (from the user, verbatim)
"It sync the correct book I'm reading but progress is all buggy."
Book identity syncs correctly; the *progress values* (percent / position / status) are wrong, inconsistent, or jump around.

## The progress pipeline (map it before changing anything)
1. **Compute:** `src/activities/reader/EpubReaderActivity.cpp` — `getProgressBasisPoints()`, `bookPercentFor(position)`, `getProgressPercent()`. Basis points = 0–10000 of the whole book.
2. **Queue:** on book close, position is queued (search for the pending-progress queue used by `KoboSyncClient::flushPendingProgress`; files under `src/activities/reader/` and `lib/KoboSync/`).
3. **Upload:** `KoboSyncClient::putReadingState(contentId, progressPercent, status, location, ...)` → `PUT /v1/library/<uuid>/state`.
4. **Download/apply:** `KoboStatusSyncActivity` on book open — fetches remote state, compares "further along", applies position.
5. **Triggers:** UI sync (`KoboSyncService::runFullSync`), pre-sleep headless sync (`maybeRunPreSleepSync` in `src/main.cpp` — flushes progress BEFORE catalog sync), shelf visits.
6. **Server semantics:** calibre-web stores whatever the client PUTs per (user, book): PercentComplete (0–100 float), Status (Opened/Finished), LastLocation (opaque string), LastOpened timestamp. The reference copy of an older calibre-web `kobo.py` is in `docs/reference/kobo/kobo.py` — but the DEPLOYED server is authoritative; probe it with curl.

## Ranked hypotheses to check (audit all, fix every real one)
1. **Unit mismatch:** percent sent as 0–1 vs 0–100, or basis points (0–10000) leaked into a percent field somewhere; or apply-side divides/multiplies wrongly → progress shows as 0%, 100%, or 10000%-clamped values.
2. **Queue ordering / staleness:** multiple queued entries for the same book flushed oldest→newest vs newest→oldest; a stale queued entry re-uploading AFTER a newer one (especially: pre-sleep flush + UI flush racing, or queue not deduped per book keeping the MAX position). Check the queue file survives reboots with stale entries that later clobber fresh progress.
3. **Apply-side comparison bug:** "remote is further along" compares incompatible quantities (per-chapter progress vs whole-book percent; page number vs percent), or applies remote position even when LOCAL is further, or applies to wrong spine index → jumps to wrong spot.
4. **Status flip:** Finished (>=99.5%) logic or Opened/Finished sent in wrong field; LastLocation format the apply path can't parse → falls back to 0%.
5. **contentVersion / book identity edge:** correct book but state keyed under a different uuid variant (hyphen casing!) — calibre-web uuids are lowercase hex without braces; a casing/format mismatch makes GET return empty → looks like "progress lost".
6. **Timestamp/timezone:** LastOpened or sync-token timestamps in local time vs UTC causing the server to treat older state as newer.

For each hypothesis: state VERDICT (bug / not-a-bug) with evidence (code lines + live curl output) in the report. Fix every real bug with the minimal change.

## Live protocol probe (do this early, paste raw outputs into the report)
- `GET http://192.168.1.10:8084/kobo/<key>/...` — discover the deployed endpoint shapes from the firmware's own URL builders in `KoboSyncClient.cpp` (base path handling matters; the key is embedded in the base URL).
- Pick one real book uuid from the sync feed, GET its reading state, PUT a known percent (e.g. 37.5), GET again — confirm the value round-trips EXACTLY as the firmware formats it. If it doesn't, that IS the bug.

## Deliverables (in order)
1. `review-tmp/progress-fix-report.md` — create EARLY, rewrite after every step. A run ending without it is a failed run.
2. Verdicts on all 6 hypotheses with evidence.
3. Fixes for every confirmed bug, minimal diff, house style.
4. Host regression tests for every fix where logic is extractable (unit conversion, queue dedupe/ordering, comparison logic) under `test/kobo_sync/`.
5. Verification pasted into the report: default/picco/slim builds SUCCESS, slim zero Kobo symbols, `pio run -t unit-tests` ≥ 952 PASS with new tests added.
6. End the report with exactly `DONE: all-done` or `DONE: partial: <what's missing>`.

## Hard constraints
- Do not change the sync-feed/catalog/download behavior — progress path only.
- Do not change the pre-sleep sync trigger conditions (that was just shipped and verified).
- Keep the UI conflict flow (Ask/Smart modes) behavior; fix its inputs if they are the bug.
- Never print the sync key in the report beyond the one given above.
