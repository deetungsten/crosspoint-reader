# Kobo Sync (calibre-web)

CrossPoint can sync books and reading positions with a calibre-web server using
the same protocol a Kobo eReader uses. The server sees CrossPoint as one more
Kobo device: same endpoint, same per-user token, same library feed, same reading
status.

This is an opt-in firmware feature. Builds that do not define
`CROSSPOINT_KOBO_SYNC=1` do not contain any of its code or UI.

## 1. Create a Kobo sync token in calibre-web

1. Open calibre-web in a browser and sign in as the user who owns the library.
2. Open the user's profile page and find **Kobo Sync Key**.
3. Select **Create/View Kobo Sync Key**. calibre-web shows an `api_endpoint`
   such as `http://192.168.1.50:8083/kobo/0123456789abcdef0123456789abcdef`.

Keep both parts:

- **Server URL**: the part before `/kobo/`, for example
  `http://192.168.1.50:8083`
- **Auth token**: the part after `/kobo/`, for example
  `0123456789abcdef0123456789abcdef`

CrossPoint also accepts the full `api_endpoint` in the Server URL field. It
splits the token out automatically, and replaces the stored token when the
pasted endpoint carries a different one.

The account needs download permission; calibre-web rejects the sync feed with
HTTP 403 otherwise.

## 2. Configure CrossPoint

1. Connect the reader to the same network as the calibre-web server.
2. Open **Settings → Network → Kobo Sync**.
3. Set **Kobo Server URL** and **Kobo Auth Token**.
4. Select **Test Connection**. This sends the same device-registration request
   a Kobo sends, so a wrong URL or token fails here.
5. Select **Sync Now** to pull the catalog feed.

A sync run follows the server's `x-kobo-sync: continue` paging for up to 20
pages of 100 entries each. If the library is larger than that, run **Sync Now**
again; the persisted sync token continues where the previous run stopped.

The reader generates and persists a device id on first use, so the server keeps
treating it as the same device across reboots.

## 3. Browse and download books

Open **Kobo Sync → Browse Kobo Shelf**, or use **Settings → Network → Kobo Sync**
again later. The shelf lists the books calibre-web exposes through the Kobo feed:

- `*` marks a book added since the previous sync.
- `Downloaded` marks a book already stored on the SD card.
- **Next Page** continues through the catalog (48 rows per page).
- Selecting a downloaded book opens it.
- Selecting a new book fetches a fresh download link, downloads the EPUB into
  the folder configured for OPDS downloads (SD root by default), and records
  the book-id ↔ local-file mapping.

If two books sanitize to the same filename, the second one gets a short
content-id suffix instead of overwriting the first. Downloads use the
calibre-web login cookie captured during the sync, so they work with the same
permissions as the web UI.

## 4. Reading positions

calibre-web stores reading positions as the Kobo reading state for a book
(`GET`/`PUT /v1/library/<uuid>/state`). CrossPoint maps its EPUB position to the
whole-book percentage in that state.

- **Sync Behavior → Smart Sync** applies the further-ahead position
  automatically, uploads when the local position is ahead, and does nothing when
  both agree within 0.5%.
- **Sync Behavior → Ask Every Time** shows the same Apply/Upload choice as the
  CrossPoint Sync screen.
- **Sync on Open** fetches the position when a mapped book is opened. It is off
  by default because it needs Wi-Fi each time.
- Closing a mapped book queues its position in `/.crosspoint/kobo.json` instead
  of blocking the reader exit path on TLS. The next **Test Connection**,
  **Sync Now**, or **Kobo Shelf** visit uploads it. The reader menu's
  **Sync Progress** entry runs the fetch/compare/upload immediately and brings
  Wi-Fi up on demand. Only the most recently closed book's position is queued.

## Limitations

- Only the calibre-web feed shape is parsed. The official Kobo store proxy mode
  is not a target.
- Shelves are read-only: calibre-web shelf membership is shown on the shelf
  rows, but CrossPoint does not create or edit shelves. Membership is additive:
  removing a book from a shelf or deleting a shelf is not reflected until the
  Kobo catalog is cleared and synced again.
- Covers are not downloaded or displayed yet; the cover route is implemented in
  the client for a later UI.
- The catalog's sorted index is rebuilt in a bounded 256-entry scratch buffer.
  Larger libraries still sync correctly but fall back to linear SD lookups,
  which makes incremental sync slower.
- The book-id mapping is keyed by the local file path. Moving or renaming a
  downloaded book outside CrossPoint breaks status sync for it until it is
  downloaded again.
- Plain HTTP is the expected setup for a LAN server. HTTPS uses the firmware's
  existing insecure-client path and is not certificate-pinned.
- There is no background or periodic sync; every sync is user-initiated.
