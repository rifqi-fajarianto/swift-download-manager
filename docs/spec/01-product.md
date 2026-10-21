# 01 — Product & UX specification

Derived from the Windows application (see `evidence-ledger.md`, E18/E19 for the
screenshots). This describes **what SwiftDM must do**, not how the original is
implemented.

## 1. Purpose

A desktop download manager for Linux that accelerates HTTP/HTTPS downloads by
splitting each file into multiple parallel connections, with pause/resume,
queueing, per-download speed limits, proxy support, and a download history.

## 2. Main window

Layout (from `main-window.png`):

- **Toolbar** (left to right): `New URL`, `Resume`, `Stop`, `Delete`,
  `Delete File` | (right) `Settings`, `Browsers`, `About`, `Quit`.
  (`Delete File` is a SwiftDM addition, documented below.)
- **Left panel**: category tree.
  - `All Downloads` with children: `Video`, `Audio`, `Compressed`, `Document`,
    `Application`, `Misc`.
  - `Complete`, `Incomplete` (views, shown under All Downloads).
- **Right panel**: downloads table with columns:
  `File Name`, `Size`, `Status`, `Bandwidth`, `Remaining Time`, `Last Try`.
  Status column shows e.g. `Complete`, `Paused ( 54% )`, `Error ( 0% )`.
- **Item context menu** (right-click on a row): `Open File`, `Open File
  Location`, `Properties`; it selects the row under the cursor first. The
  original's version of this menu also carries `Resume`, `Stop`, `Redownload`,
  `Show Window` and `Delete` (E25) — here those stay on the toolbar and in the
  per-download window.
- **History restore**: at startup the table is rebuilt from SQLite with every
  row (Complete, Paused, Error). Rows left `Downloading`/`Queued` by a previous
  quit are shown as `Paused` and never resume by themselves; the user presses
  Resume. Deleting a row removes it from the database permanently and deletes
  its temp workspace (the partial `.part` file and `segments.json`); see
  `02-data-model.md` §2.
- Window title: application name + version.

## 3. Core user flows

1. **New download (manual)**: `New URL` toolbar button opens a small dialog
   with a URL field and `Download`/`Cancel`. On accept, the app probes the URL
   (no list row, no database entry), then shows the *Download Confirmation*
   window: full URL, server-resolved file name and size, a `Category` dropdown
   (file-type default, user overridable) and `Location` (category folder by
   default, user overridable). `Download Now` adds the item and starts the
   transfer; `Cancel` leaves no trace; `Add to queue` is a placeholder until
   queue scheduling exists (0.18.0).
   - Fixed default size (640×250, minimum 520×230): the window never grows with
     the file name or the URL length (0.18.9). The file name line is elided in
     the middle and keeps the full name in its tooltip.
   - Rows: `File` (name), `Size` (content length, `Unknown size` when the
     server reports none), `URL` (read-only field), `Category` and `Location`
     (0.18.9: the size is no longer inline with the file name). Every
     description is vertically centered with the widget next to it (0.18.9).
   - All inputs and buttons (URL field, location field, `Browse...`, category
     dropdown, `Download Now`, `Add to queue`, `Cancel`) share one height, so
     the rows look even in every style (0.18.9).
2. **Download progress window**: double-click opens the per-download window
   (`download-window.png`):
   - Fixed default size (560×420, minimum 420×300): the window never grows with
     the URL length. The URL line is elided in the middle and keeps the full
     URL in its tooltip and link.
   - Tabs: `Download`, `Options`, `Connections`.
   - Download tab: URL, Status, File Size, Downloaded (bytes + %), Bandwidth,
     Remaining Time, Resumable (Yes/No), main progress bar, segment progress
     bar (segments shown as colored slices), segmented count label
     (`Segments : N`), `Resume`/`Cancel` buttons. When the per-download speed
     limiter is on, an orange `Speed limit: <value>` line sits next to the
     Bandwidth value (0.18.1).
   - Options tab: Save To, File Name, Connections, Bandwidth Limit with an
     enable toggle and presets (500 KB/s, 1 MB/s, 5 MB/s, 10 MB/s, 20 MB/s,
     50 MB/s, or a custom KB/s value), Referer; `Apply` (0.18.1). While a
     transfer is active, only the speed-limit editor remains enabled and its
     changes apply after 500 ms without pausing. Other fields are disabled
     until the transfer is paused; `Apply` saves them then (0.18.2).
3. **Item actions** (right-click a download row):
   - `Open File` — opens the downloaded file with the default handler. It is
     available for every completed row; when the file was deleted or moved it
     shows a warning ("The file has been deleted or moved") with `Open File
     Location` instead of failing silently. A completed row whose file is
     missing is labelled `Complete (file missing)` in amber with a tooltip; no
     dialog appears at startup, only when `Open File` is chosen for that row.
   - `Open File Location` — opens the containing folder in the file manager.
   - `Properties` — read-only *Download Properties* window (see below).
4. **Delete File** (toolbar): deletes the downloaded file of the selected
   completed download(s) from disk after a warning confirmation ("This cannot
   be undone"); the list entries are removed as well. SwiftDM addition, not in
   the original toolbar.
5. **Pause/Resume**: toolbar or download window; paused state keeps partial
   data and can resume after restart.
6. **Completed**: `download-complete-window.png` with `Open Folder`, `Open`,
   `Close`. Shown only when the completion-dialog setting is on.
7. **Error**: `download-error-window.png` shows `Error : <message>`, the URL, and
   offers `Close` and `Log File`. The message carries the host's own explanation
   when the response body has one (JSON `message`/`value`, HTML title or text),
   e.g. `Error : HTTP/1.1 403 Forbidden - hotlink_detected: Hotlinking was
   detected for this download, hotlinking is only allowed on premium accounts`.
   When the download came from a browser page, an additional `Open Page in
   Browser` button opens that page (hosts such as pixeldrain require a CAPTCHA
   there before any tool may download the file).
   Google Drive links are resolved before the confirmation step (0.13.0):
   viewer/file links are rewritten to Drive's direct download URL and the
   virus-scan notice for files above Google's scan limit is confirmed
   automatically, so the dialog shows the real file name and size. Links that
   never lead to a file — Drive folder listings, Docs editor pages, sign-in
   walls for private or removed files — fail with an explanation instead of
   saving a page.
8. **Properties** (`download-properties-window.png`): icon, file size bytes,
   status, Saved To path (+ Open Folder), Temp Path, URL (editable), Page URL
   (+ Show Page), Page Title, Browser (source that submitted it), Added On,
   Last Try timestamps, `Log File` button.
   *Implemented subset (0.6.0)*: file name, File Size, Status, Saved To
   (+ Open Folder), Temp Path, URL (read-only), Category, Downloaded,
   Resumable, Segments, Added On, Last Try. Editable URL, Page
   URL/Page Title/Browser and `Log File` depend on history / browser
   integration (v1.1 / v2).
9. **Settings** (`options-window.png`), tabs:
   - *General*: app-wide behavior options — Show Download Completion Dialog;
     Start Application on System Startup (0.18.10 split the tabs: the download
     options moved to the `Download` tab).
   - *Interface*: appearance options — toolbar size, toolbar text, theme,
     tray icon style (added in the 0.18.x settings batch; see the parity
     checklist for the current status).
   - *Download*: Max Connections per Download (default 8, "8 Connections
     Recommended"); when-start policy (all at once / one by one); Bandwidth
     Limit per Download with an enable toggle and presets (500 KB/s, 1 MB/s,
     5 MB/s, 10 MB/s, 20 MB/s, 50 MB/s, or custom KB/s; value editable while
     the toggle is off — 0.18.1); Default User-Agent (+Reset,
     + "use for both manual and browser-sent downloads"); Download Directory
     (+ category folders toggle); Temporary Directory; Get File Creation Date
     From Server.
   - *Proxy/Socks*: No Proxy/Socks | HTTP Proxy | Socks Proxy (V4/V5),
     Address/Port/User/Password, "https protocol" handling note.
   - *Passwords*: credential table (Target/User/Password; remembered auth per
     server), matching the `auths` DB table.
10. **Browser integration**: `Browsers` toolbar button shows supported browsers
    (Chrome, Firefox, Edge, Opera) and instructions; the app runs a local
    WebSocket server accepting download requests from browser extensions.
11. **Tray behavior**: closing the main window hides to tray; tray menu can
    restore the window (per strings "Quit NeatDownloadManager", "You can hide
    ... in Notification Area..."). The tray shows the colored SwiftDM mark
    (same artwork as the browser integration), so it stays recognizable on
    both light and dark panels. When started at login through the autostart
    entry (`swiftdm --autostart`), the main window stays hidden and only the
    tray icon appears; without a system tray the window is shown as a fallback
    (0.18.7). Toolbar and tray-menu `Quit` actions ask
    for confirmation before fully exiting; active downloads are paused and can
    be resumed on the next launch, while Cancel keeps the app running (0.18.4).
12. **About**: version, license tier, support contact, website; update check
    button (we replace this with "no update check").

## 4. Non-goals for v1 (see 06-parity-checklist.md)

- Browser extension + WebSocket server: v2.
- FTP, HLS/m3u8 + MKV muxing: v3.
- Any phone-home/update-check behavior: dropped intentionally.

## 5. Open questions (UNKNOWN, from E23)

- Exact status string set and % rounding in the table.
- Sort order and refresh cadence of the table.
- Tray icon menu item set.
