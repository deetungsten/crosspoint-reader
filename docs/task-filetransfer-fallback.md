SMALL FOLLOW-UP FIX (regression from the just-completed home-menu rework): in the Kobo home layout (Kobo Library / Kobo Sync / Browse Files / Library / Settings) the File Transfer screen became unreachable — it was previously a Home row. The user needs it now (SD access for installing fonts).

Deliverables:
1. Add a "File Transfer" entry to the Settings screen (place it in the connectivity/network area where Wi-Fi/OPDS entries live; reuse STR_FILE_TRANSFER). Activating it must open the exact same File Transfer flow the old Home row opened (same network-mode selection entry point — inspect how HomeActivity launched it, including any silent-reboot path noted in src/main.cpp around "Rebooted on the way *into* File Transfer").
2. Also add a "Plugin Catalog" (or similarly named, matching existing strings) entry in Settings if the plugin catalog screen is currently unreachable in the Kobo layout (previous handoff noted it lost its home slot; verify first — if already reachable, skip and say so).
3. Gate: these fallback entries appear only when the Kobo home layout is active (CROSSPOINT_KOBO_SYNC=1 and credentials configured), so flag-off/old-layout Settings screens are unchanged.
4. Update USER_GUIDE.md + docs/kobo-sync.md home-layout description accordingly.

Verify: pio run -e default, -e picco, -e slim compile; ctest suite passes. No git commit. Minimal diff.
