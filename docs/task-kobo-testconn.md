SMALL FOCUSED FIX: Kobo Test Connection fails against calibre-web instances with "Kobo proxy" mode enabled.

Evidence (live user instance):
- GET /kobo/<token>/v1/library/sync -> 200 with 84 entries (calibre-web native path works)
- POST /kobo/<token>/v1/auth/device -> 400 {"ResponseStatus":{"ErrorCode":"ArgumentNullException","Message":"Value cannot be null. Parameter name: s"}} — .NET-style body means calibre-web proxied the call to storeapi.kobo.com (config_kobo_proxy), and the real store rejects our registration body because it lacks fields real Kobos send (ClientKind, ProductName, PlatformId, ClientVersion...).

Current code: lib/KoboSync/KoboSyncClient.cpp testConnection() sends POST /v1/auth/device {DeviceId, UserKey} and returns error on non-2xx -> user sees "Connection failed" even though everything calibre-web needs works.

Fix in testConnection():
1. Send the richer registration body a real Kobo sends (DeviceId, ClientKind "Ereader", ProductName "CrossPoint", PlatformId "crosspoint-esp32", ClientVersion string) — best-effort: log the result, do NOT fail on non-2xx.
2. Then perform the authoritative check: GET /v1/library/sync (first page, minimal/empty sync token, discard body via a chunk sink that just counts bytes and stops after a small prefix is enough — or reuse the existing request helper with a bounded sink). 2xx -> OK; 401/403 -> AUTH_FAILED; other -> SERVER_ERROR/NETWORK_ERROR as today.
3. Keep cookie capture behavior (the sync GET may set the login cookie; persist it like other calls do).
4. Update the docstring on testConnection() explaining registration is best-effort because proxy-mode servers forward it to the real Kobo store.
5. Add/adjust a host test ONLY if test/kobo_sync has a pattern for client-level tests (it likely does not — parser/token tests only; then skip).

Verify: ~/.pio-venv/bin/pio run -e default and -e picco compile; ctest suite passes (cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test). No git commit. Minimal diff.
