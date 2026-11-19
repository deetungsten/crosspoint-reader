SMALL FOCUSED TASK: add a web-server API for Kobo sync credentials, mirroring the existing Wi-Fi credential API exactly.

Context: lib/KoboSync/KoboCredentialStore (singleton PersistableStore, file /.crosspoint/kobo.json) holds endpoint + authToken (setters/getters exist; token stored XOR-obfuscated with device MAC — the store handles that). The device web server (src/network/CrossPointWebServer.cpp, docs/webserver-endpoints.md) already exposes POST /api/wifi and /api/opds that accept plaintext credentials over the LAN and persist them via the on-device store. There is no Kobo equivalent, so a user must type a 32-char token on the e-ink keyboard.

Deliverables:
1. `POST /api/kobo` — JSON body {"endpoint": "...", "token": "..."} (either field optional; empty/absent = leave unchanged). Validates lengths against KoboCredentialStore bounds, sets them, saveToFile(), returns same success/error JSON style as POST /api/wifi. Also accept {"apiEndpoint": "http://host:port/kobo/<token>"} full-form input: reuse whatever split logic KoboSyncSettingsActivity/KoboCredentialStore already has for pasting the combined api_endpoint.
2. `GET /api/kobo` — return {"configured": bool, "endpoint": "..."} — NEVER return the token (mirror how GET /api/wifi handles secrets).
3. Protection: ensure /.crosspoint/kobo.json is in the web server's protected-path list (like wifi.json/opds.json/koreader.json) so it can never be downloaded/uploaded directly. Check whether it already is; add + update docs/webserver-endpoints.md protected list if missing.
4. Gate all of it under CROSSPOINT_KOBO_SYNC (flag off = routes not registered).
5. Update docs/webserver-endpoints.md with the two endpoints (follow the /api/wifi doc style).
6. Verify: ~/.pio-venv/bin/pio run -e default and -e slim must compile; run host tests (cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test) — all must pass. Add a host test only if the existing harness has a pattern for web-server handlers (check test/ for wifi/opds API tests first; if none, skip tests).

Constraints: no git commit; minimal diff; follow existing handler style in CrossPointWebServer.cpp (auth middleware, JSON helpers, response codes).
