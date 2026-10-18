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

- **Toolbar** (left to right): `New URL`, `Resume`, `Stop`, `Delete` | (right)
  `Settings`, `Browsers`, `About`, `Quit`.
- **Left panel**: category tree.
  - `All Downloads` with children: `Video`, `Audio`, `Compressed`, `Document`,
    `Application`, `Misc`.
  - `Complete`, `Incomplete` (views, shown under All Downloads).
- **Right panel**: downloads table with columns:
  `File Name`, `Size`, `Status`, `Bandwidth`, `Remaining Time`, `Last Try`.
  Status column shows e.g. `Complete`, `Paused ( 54% )`, `Error ( 0% )`.
- Window title: application name + version.

## 3. Core user flows

1. **New download (manual)**: `New URL` toolbar button opens a small dialog
   with a URL field and `Download`/`Cancel`. On accept, the app probes the URL,
   resolves the filename/size, and adds it to the table (status Downloading).
2. **Download progress window**: double-click opens the per-download window
   (`download-window.png`):
   - Tabs: `Download`, `Options`, `Connections`.
   - Download tab: URL, Status, File Size, Downloaded (bytes + %), Bandwidth,
     Remaining Time, Resumable (Yes/No), main progress bar, segment progress
     bar (segments shown as colored slices), segmented count label
     (`Segments : N`), `Resume`/`Cancel` buttons.
3. **Pause/Resume**: toolbar or download window; paused state keeps partial
   data and can resume after restart.
4. **Completed**: `download-complete-window.png` with `Open Folder`, `Open`,
   `Close`. Shown only when the completion-dialog setting is on.
5. **Error**: `download-error-window.png` shows `Error : HTTP/1.1 403 Forbidden`,
   the URL, and offers `Close` and `Log File`.
6. **Properties** (`download-properties-window.png`): icon, file size bytes,
   status, Saved To path (+ Open Folder), Temp Path, URL (editable), Page URL
   (+ Show Page), Page Title, Browser (source that submitted it), Added On,
   Last Try timestamps, `Log File` button.
7. **Settings** (`options-window.png`), tabs:
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
8. **Browser integration**: `Browsers` toolbar button shows supported browsers
   (Chrome, Firefox, Edge, Opera) and instructions; the app runs a local
   WebSocket server accepting download requests from browser extensions.
9. **Tray behavior**: closing the main window hides to tray; tray menu can
   restore the window (per strings "Quit NeatDownloadManager", "You can hide
   ... in Notification Area...").
10. **About**: version, license tier, support contact, website; update check
    button (we replace this with "no update check").

## 4. Non-goals for v1 (see 06-parity-checklist.md)

- Browser extension + WebSocket server: v2.
- FTP, HLS/m3u8 + MKV muxing: v3.
- Any phone-home/update-check behavior: dropped intentionally.

## 5. Open questions (UNKNOWN, from E23)

- Exact status string set and % rounding in the table.
- Sort order and refresh cadence of the table.
- Tray icon menu item set.
