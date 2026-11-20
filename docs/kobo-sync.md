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
2. Open **Settings → Connectivity → Kobo Sync**.
3. Set **Kobo Server URL** and **Kobo Auth Token**.
4. Select **Sync Now** to pull the catalog feed. A wrong URL or token shows a
   sync error; a successful sync reports how many catalog rows arrived.

A sync run follows the server's `x-kobo-sync: continue` paging for up to 20
pages of 100 entries each. If the library is larger than that, run **Sync Now**
again; the persisted sync token continues where the previous run stopped.

After credentials are configured, Home switches to a fixed five-row layout:
**Kobo Library**, **Kobo Sync**, **Browse Files**, **Library**, and **Settings**.
**Kobo Library** opens the shelf directly; **Kobo Sync** runs the catalog sync
and hands off to the shelf when new rows need downloading. The shelf's settings
button or the **Kobo Settings** row opens the Kobo Sync settings screen.

This layout does not show the Plugins/OPDS row or **File Transfer**. While it
is active, **File Transfer** (web upload, Calibre Wireless, hotspot, USB Drive
where supported) is available from **Settings → Connectivity → File Transfer**,
and the on-device **Plugin Catalog** from **Settings → Connectivity → Plugin
Catalog** while plugins are enabled. OPDS browsing remains available from
**Settings → Connectivity → OPDS Servers → OPDS Browser** once a server is saved.
Clearing the credentials restores the default Home layout, but it also clears
the synced catalog.

The reader generates and persists a device id on first use, so the server keeps
treating it as the same device across reboots.

## 3. Browse and download books

Open **Kobo Library** on Home, or **Settings → Connectivity → Kobo Sync →
Browse Kobo Shelf**. Opening the shelf never brings Wi-Fi up by itself. If
Wi-Fi is already connected, the shelf refreshes the catalog before showing the
list; if it is not, it goes straight to the catalog already on the SD card,
with no Wi-Fi picker, network error screen, or network delay. If a refresh
fails, the shelf still shows the SD catalog instead of an error screen.
Downloaded books open from the SD card. If **Sync on Open** is enabled, opening
a mapped book only fetches its reading position when Wi-Fi is already connected;
when disconnected, the book opens locally without a prompt. Unlike the shelf,
**Sync Now** and Home's **Kobo Sync** still prompt for Wi-Fi when disconnected.

The shelf lists every book in the catalog, not only the rows delivered by the
latest sync:

- `*` marks a book added since the previous sync.
- `Downloaded` marks a book already stored on the SD card.
- **Next Page** continues through the catalog (48 rows per page).
- Selecting a downloaded book opens it.
- Selecting a new book fetches a fresh download link, downloads the EPUB into
  the folder configured for OPDS downloads (SD root by default), and records
  the book-id ↔ local-file mapping.
- The **Kobo Settings** row at the top, or the header settings button on touch
  devices, opens the Kobo Sync settings screen.

If two books sanitize to the same filename, the second one gets a short
content-id suffix instead of overwriting the first. Downloads use the
calibre-web login cookie captured during the sync, so they work with the same
permissions as the web UI.

### Auto-download

**Auto-download books** is on by default; toggle it in **Settings → Connectivity
→ Kobo Sync**. After a sync that delivered new or changed rows, and on every
shelf visit where Wi-Fi is already connected and the catalog refresh succeeds,
CrossPoint downloads catalog books that do not have a file on the SD card yet:

- **Sync Now** (or Home's **Kobo Sync**) opens the shelf to run the batch when
  it delivered new or changed rows and Auto-download is on; the sync result
  screen stays behind it.
- One book is downloaded at a time; the progress screen shows
  `Downloading...`, `12/82 — Title`, and a Cancel button.
- Files already on SD are skipped. A stale catalog mapping (the file was
  deleted) is detected by checking the SD card again before each transfer.
- Cancel stops the batch. Books already downloaded stay; the remaining books
  are retried on the next connected shelf visit.
- A sync that delivered no new rows still resumes pending downloads when the
  shelf is opened with Wi-Fi connected.
- KEPUB downloads are accepted; the reader opens them through the EPUB path.

Auto-download does not run in the background or on a timer. It uses the same
folder, filename-collision rule, and path link as a manual shelf download, so
reading positions continue to work for automatically downloaded books. One
exception is the timeout sleep path: when Kobo is configured and Wi-Fi is
already connected, the reader probes the server for up to 10 seconds and, if it
answers, runs the full sync (position upload, catalog delta, then the
auto-download batch) before shutting down. There is no time limit once the
probe succeeds; a button press or a dropped connection stops it at the next
item. A failed probe sleeps exactly as before, and a manual sleep never delays.

## 4. Reading positions

calibre-web stores reading positions as the Kobo reading state for a book
(`GET`/`PUT /v1/library/<uuid>/state`). CrossPoint maps its EPUB position to the
whole-book percentage in that state.

- **Sync Behavior → Smart Sync** applies the further-ahead position
  automatically, uploads when the local position is ahead, and does nothing when
  both agree within 0.5%.
- **Sync Behavior → Ask Every Time** shows the same Apply/Upload choice as the
  CrossPoint Sync screen.
- **Sync on Open** fetches the position when a mapped book is opened, but only
  when Wi-Fi is already connected. It is skipped when Wi-Fi is disconnected;
  use **Sync Progress** for an on-demand fetch that can prompt. It is off by
  default because it needs Wi-Fi each time.
- Closing a mapped book queues its position in `/.crosspoint/kobo.json` instead
  of blocking the reader exit path on TLS. The same position is queued when the
  reader goes to sleep, so the timeout pre-sleep sync uploads the current
  session. **Kobo Sync**, **Sync Now**, and a **Kobo Library** visit upload it
  after a successful catalog sync. The timeout pre-sleep sync uploads it before
  the catalog sync, so a failed catalog refresh still gets the position
  through. The reader menu's **Sync Progress** entry runs the
  fetch/compare/upload immediately and brings Wi-Fi up on demand. Only one
  book's position is queued at a time; a newer position replaces the previous
  one.

## Limitations

- Only the calibre-web feed shape is parsed. The official Kobo store proxy mode
  is not a target.
- Shelves are read-only: calibre-web shelf membership is shown on the shelf
  rows, but CrossPoint does not create or edit shelves. Membership is additive:
  removing a book from a shelf or deleting a shelf is not reflected until the
  Kobo catalog is cleared and synced again.
- Kobo titles and authors are stored with typographic punctuation folded to
  ASCII (`’` → `'`, `“`/`”` → `"`, `–`/`—` → `-`, `…` → `...`), so shelf rows
  do not depend on a particular UI-font glyph set. Rows already in the catalog
  pick this up when a later sync resends them or the catalog is rebuilt.
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
- There is no background or periodic sync. The automatic syncs are the shelf's
  catalog refresh when it opens, **Sync on Open** when a mapped book opens, and
  the timeout sleep path described above; the first two require an existing
  connection and do not start one, and the sleep path also requires an existing
  connection. Every other sync and auto-download batch follows an explicit
  action.
- Leaving the shelf turns Wi-Fi off in place; **Sync Now** does so when it
  brought the radio up. Neither screen reboots the device.
