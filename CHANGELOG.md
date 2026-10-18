# Changelog

## 0.9.0 — 2026-10-18

Chromium can now hand off browser downloads to SwiftDM automatically.

- Added an enabled-by-default extension setting to intercept HTTP(S) downloads.
  The extension cancels the browser item and asks SwiftDM to take it over; if
  SwiftDM is unavailable, the browser download is restarted automatically.
- Browser download handoff skips downloads started by the extension itself,
  non-HTTP(S) URLs, and incognito downloads.
- Users can turn automatic handoff off in the extension popup.
- Connection diagnostics now surface the real WebSocket error in the popup and
  the app logs rejected handshakes. The server accepts any well-formed Chromium
  extension origin instead of one hard-coded development ID, so reloading an
  unpacked extension from another path no longer breaks the connection.

## 0.8.0 — 2026-10-18

Initial Chromium browser integration development preview.

- Added a loopback WebSocket server for the `swiftdm.v1` protocol, with
  Chromium-origin validation and `ping`, `getVersion`, `showPanel` and
  `addDownload` commands.
- Added an unpacked Manifest V3 extension for Chromium-based browsers. Users can
  send the active page or a link explicitly through the popup/context menu.
- Cookie access is optional and requested per origin; scoped cookies are
  forwarded without turning them into unrestricted Cookie headers.
- Firefox support and automatic download interception remain planned.

## 0.7.0 — 2026-10-18

Toolbar usability polish.

- All toolbar buttons now have a consistent size.
- `Resume`, `Stop`, `Delete` and `Delete File` are disabled when no download
  row is selected. `Delete File` also remains disabled unless a selected,
  completed download's file exists on disk.
- The `New URL` button now uses the `link-plus` icon.

## 0.6.0 — 2026-10-18

Download-item actions: a right-click menu on the table and a `Delete File`
toolbar button.

- Right-clicking a download row opens a context menu with `Open File`,
  `Open File Location` and `Properties`. The original shows the same menu on the
  list's right-click notification (E25); unlike the original, our `Open File`
  item is only enabled when the completed file is still on disk.
- `Properties` opens a read-only *Download Properties* window: file name, file
  size (bytes + human), status, saved-to path (+ `Open Folder`), temp path, URL,
  category, downloaded bytes/percent, resumability, segment count, added-on and
  last-try timestamps.
- New toolbar button `Delete File`: after a warning dialog it deletes the file
  of the selected completed download(s) from disk; the list entries are dropped
  as well. The button is enabled only while a selected download's file exists.

## 0.5.1 — 2026-10-18

The download window closes itself when the transfer finishes, and the
notification windows are no longer attached to the main window.

- The progress window now closes automatically once a running download reaches
  `Complete` (the completion notification appears instead); a window opened on
  an already-complete item still stays open for inspection.
- `Download Complete` and `Download Error` are now independent top-level
  windows (no transient parent, non-modal): the window manager cannot group
  them with the main window, so moving one no longer moves the other. They open
  centered near the main window and stay usable alongside it.

## 0.5.0 — 2026-10-18

The download window now has a Pause/Resume toggle button.

- While a transfer runs, the primary button reads `Pause` and pauses the
  download (the list shows `Paused ( N% )`); when stopped it reads `Resume`
  and continues from the partial state.
- `Cancel` keeps its behavior: pause (if running) and close the window.

## 0.4.0 — 2026-10-18

Manual downloads now confirm before transferring, and the progress window opens
when a download starts.

- New URL flow: after `Download` the URL is probed (row shows `Connecting...`)
  and a confirmation window shows the resolved `File Name` and `Size` (plus the
  URL); `Download` starts the transfer, `Cancel` discards the entry. Probe
  failures keep the row in `Error` state (Resume retries) and show the error
  dialog.
- The download progress window now opens automatically when a download starts
  from the main window: after the confirmation and on toolbar `Resume`.
- Model: new `Probing` state; `Stop` ignores items that are still connecting.

## 0.3.1 — 2026-10-18

GUI polish: the SVG set in `ui-assets/` is now used throughout.

- Category tree icons: All Downloads (`folder`), Video, Audio (`music`),
  Compressed, Document, Application, Misc (`other`), Complete, Incomplete
  (`alert`) — all tinted to the toolbar accent color.
- Settings toolbar button now uses `ui-assets/settings.svg`; the project-drawn
  gear was removed.
- Download window shows the URL with the `link.svg` icon.
- All 22 bundled SVGs are embedded via `qt_add_resources` and rasterized by
  the headless `icons` test.

## 0.3.0 — 2026-10-18

Major feature: Qt6 Widgets GUI (Milestone 3, first increment).

- Main window: toolbar (New URL / Resume / Stop / Delete / Settings / Browsers /
  About / Quit), category tree (All Downloads + Video/Audio/Compressed/Document/
  Application/Misc, Complete, Incomplete), downloads table (`File Name`, `Size`,
  `Status`, `Bandwidth`, `Remaining Time`, `Last Try`), status bar summary.
- Downloads run on a worker thread per item (probe + engine); probe updates the
  resolved file name, size and resumability live; Stop pauses, Resume continues,
  Delete removes the row and its partial workspace.
- Download window (double-click a row): Download tab (status, size, downloaded,
  bandwidth, remaining time, resumable, progress bar, per-segment bar,
  Resume/Cancel), Options tab (save-to, filename, connections, bandwidth limit,
  referer — applied on resume), Connections tab (per-segment table).
- New URL dialog; Settings dialog (General / Proxy-Socks / Passwords
  placeholder); About, Browsers info, completion and error dialogs.
- Tray icon with close-to-tray; "Start Application on System Startup" writes an
  XDG autostart entry (`~/.config/autostart/swiftdm.desktop`).
- Dark Fusion theme; toolbar icons: the four user-provided SVGs in `ui-assets/`
  plus project-drawn glyphs in `src/ui/icons/`, tinted at load time.
- Engine: `ProgressInfo` carries a segment snapshot for the GUI; `DownloadResult`
  carries the workspace directory (cleanup of removed rows).
- CLI unchanged; running `swiftdm` without arguments (or with `--gui`) starts
  the GUI.
- Tests: new headless `icons` suite (offscreen rasterization of every icon);
  engine/CLI suites unchanged and passing.
- Known gaps tracked for v1.1 (see docs/spec/06-parity-checklist.md): SQLite
  download history restored at startup, properties dialog + per-download log,
  saved-passwords UI, queue scheduling, server-date → file mtime.

## 0.2.0 — 2026-10-18

Major feature: core download engine and headless CLI (Milestone 2).

- Engine: segmented HTTP/HTTPS downloads via libcurl multi, positioned writes,
  per-download bandwidth limit, ConnectionsAtOnce mode, retry with backoff,
  redirects, proxy/SOCKS support, custom headers, POST streaming fallback.
- Probe: HEAD with `Range: 0-0` fallback; size, range support, ETag,
  Last-Modified, Content-Disposition filename parsing.
- Resume: `segments.json` state with atomic writes and `.bak` fallback;
  validation against size/ETag/Last-Modified on resume; workspace cleanup.
- Storage: INI settings (`~/.config/swiftdm/swiftdm.ini`) mirroring the
  original registry keys; category routing (Video/Audio/Compressed/Document/
  Application/Misc); unique destination paths with " (n)" suffixes.
- CLI: `swiftdm get <url>` with `--dir/--name/--connections/--limit/--no-resume/
  --referer/--header/--ua/--post/--config/--pause-after/--quiet/--log-level`.
- Tests: segment planner, state file roundtrip/backup/corruption, settings
  parsing, and HTTP integration tests against an in-process test server
  (segmented download, resume after pause, redirect, 403, chunked unknown size).

## 0.1.0 — 2026-10-18

Initial skeleton: CMake project with generated version header, CLI stub
(`--version`/`--help`), logging utility, unit test harness, clean-room spec
documents (`docs/spec/`) and reverse-engineering evidence ledger.
