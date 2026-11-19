UI TASK: restructure the Home screen menu (user's personal layout request).

Target home menu order, exactly:
1. Kobo Library   — opens KoboShelfActivity (browse/downloaded books; it syncs on visit)
2. Kobo Sync      — runs the catalog sync + auto-download batch (KoboSyncActionActivity SYNC mode); on success returns to shelf or home per existing handoff behavior
3. Browse Files   — existing FileBrowserActivity entry (keep current behavior)
4. Library        — existing LibraryListActivity entry (keep current behavior)
5. Settings       — existing Settings entry (keep current behavior)

REMOVE the OPDS Browser home entry entirely (the OPDS server list remains reachable wherever it lives under Settings — verify it still is; if OPDS catalog browsing becomes unreachable from anywhere, add an "OPDS Browser" row inside the existing OPDS settings screen instead).

Notes:
- The previous agent noted home menu ordering is hand-maintained index arithmetic over a HomeMenuItem enum — clean this up properly for the new fixed list (keep it simple; no over-engineering).
- Kobo Library / Kobo Sync rows: show only when CROSSPOINT_KOBO_SYNC=1 and credentials are configured (same visibility rule as the current single Kobo home entry). Non-Kobo builds keep their existing menu minus OPDS Browser... actually KEEP removing OPDS Browser only when CROSSPOINT_KOBO_SYNC=1 and configured, so flag-off builds are byte-identical in behavior to today (feature-gating rule: flag-off must cost nothing and change nothing).
- Button-only navigation must stay correct with the new item count (Up/Down wrap, Confirm on each row).
- i18n: add STR_KOBO_LIBRARY / reuse STR_KOBO_SHELF and STR_KOBO_SYNC strings as appropriate via the repo translation workflow.
- Update docs (USER_GUIDE.md home section + docs/kobo-sync.md) to match.

Verify: pio run -e default, -e picco, -e slim compile; ctest suite passes. No git commit. Minimal diff.
