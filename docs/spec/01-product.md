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
- Window title: application name + version.

## 3. Core user flows

1. **New download (manual)**: `New URL` toolbar button opens a small dialog
   with a URL field and `Download`/`Cancel`. On accept, the app probes the URL,
   resolves the filename/size, and adds it to the table (status Downloading).
2. **Download progress window**: double-click opens the per-download window
   (`download-window.png`):
   - Fixed default size (560×420, minimum 420×300): the window never grows with
     the URL length. The URL line is elided in the middle and keeps the full
     URL in its tooltip and link.
   - Tabs: `Download`, `Options`, `Connections`.
   - Download tab: URL, Status, File Size, Downloaded (bytes + %), Bandwidth,
     Remaining Time, Resumable (Yes/No), main progress bar, segment progress
     bar (segments shown as colored slices), segmented count label
     (`Segments : N`), `Resume`/`Cancel` buttons.
3. **Item actions** (right-click a download row):
   - `Open File` — opens the downloaded file with the default handler; enabled
     only when the download is complete and the file still exists on disk.
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
   - *General*: Max Connections per Download (default 8, "8 Connections
     Recommended"); when-start policy (all at once / one by one); Bandwidth
     Limit per Download (KB/s, 0/blank = no limit); Default User-Agent (+Reset,
     + "use for both manual and browser-sent downloads"); Download Directory
     (+ category folders toggle); Temporary Directory; Get File Creation Date
     From Server; Show Download Completion Dialog; Start Application on System
     Startup.
   - *Proxy/Socks*: No Proxy/Socks | HTTP Proxy | Socks Proxy (V4/V5),
     Address/Port/User/Password, "https protocol" handling note.
   - *Passwords*: credential table (Target/User/Password; remembered auth per
     server), matching the `auths` DB table.
10. **Browser integration**: `Browsers` toolbar button shows supported browsers
    (Chrome, Firefox, Edge, Opera) and instructions; the app runs a local
    WebSocket server accepting download requests from browser extensions.
11. **Tray behavior**: closing the main window hides to tray; tray menu can
    restore the window (per strings "Quit NeatDownloadManager", "You can hide
    ... in Notification Area...").
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
