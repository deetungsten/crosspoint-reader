SMALL UX FIX: Kobo Shelf (Kobo Library) currently syncs on every open, which is hostile when there is no Wi-Fi (user out and about wanting to read an already-downloaded book).

Required behavior:
1. Opening Kobo Library must ALWAYS be instant and offline-capable: list catalog rows, downloaded books open in the reader, zero network dependency.
2. Auto-sync on open (existing syncOnOpen setting) becomes best-effort: only attempted when WiFi.status()==WL_CONNECTED already; if not connected, skip silently — no Wi-Fi selection prompt, no error screen, no delay. A small "offline" hint in the header/footer is fine if the layout supports it without churn.
3. Manual "Sync Now" (menu/home entry) keeps current behavior including the Wi-Fi prompt when disconnected — explicit user intent.
4. Auto-download batch likewise only runs when connected (should already; verify).
5. Check the same open-path for the home "Kobo Sync" row: if it force-prompts Wi-Fi when disconnected, that is intended (explicit action) — leave it.

Verify: pio run -e default, -e picco, -e slim compile; ctest passes. No git commit. Minimal diff. Note behavior in docs/kobo-sync.md.
