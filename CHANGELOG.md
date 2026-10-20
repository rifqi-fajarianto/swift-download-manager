# Changelog

## 0.18.4 — 2026-10-20

Confirm before fully quitting SwiftDM.

- Toolbar and tray-menu Quit actions now show a confirmation dialog explaining
  that the app will close completely and active downloads will be paused for a
  later resume.
- Cancel leaves the application running; the confirmation defaults to Cancel
  to prevent accidental exits.

## 0.18.3 — 2026-10-20

Keep the main window hidden while confirming browser-sent downloads.

- Browser handoffs no longer restore or activate the main window before showing
  the Download Confirmation dialog.
- The confirmation dialog is a top-level window without the main window as its
  parent, so it remains visible when the main window is hidden to the tray or
  minimized.

## 0.18.2 — 2026-10-20

Speed limits can now be changed while a download is running.

- The Options tab applies speed-limiter toggle/value changes after a 500 ms
  debounce, without pausing or resuming the transfer. This works for segmented
  HTTP/HTTPS, sequential HTTP, FTP and each HLS resource.
- Speed-limit edits are persisted immediately to the history row and restored
  for a later resume. Save To, File Name, Connections and Referer remain
  disabled while the transfer is active; Apply continues to apply those fields
  while paused.
- A shared receive-rate gate paces data before writing, avoiding libcurl option
  changes on active handles. Pause/cancel can interrupt the pacing wait.
- Backend integration tests update the rate during segmented/sequential HTTP,
  FTP, and HLS transfers.

## 0.18.1 — 2026-10-19

Speed limiter with an explicit toggle, presets and an active-limit display.

- Settings and the download window's Options tab now have an "Enable speed
  limiter" toggle (new `BandWidthLimitEnabled` key and `bandwidthlimit_enabled`
  history column). Off means unlimited; the KB/s value stays visible and
  editable so it can be prepared and enabled later.
- Presets in both editors: 500 KB/s, 1 MB/s, 5 MB/s, 10 MB/s, 20 MB/s and
  50 MB/s, plus the custom KB/s field.
- New downloads start from the global toggle/value and can override both per
  download; restoring a row brings the per-download state back.
- The Download tab shows `Speed limit: <value>` in orange next to the
  Bandwidth value while the limiter is on.
- The limiter now also caps the sequential transfer paths (unknown-size HTTP,
  FTP, HLS) through libcurl's receive-rate cap.
- Upgrade compatibility: configs and history rows written earlier infer the
  toggle from a non-zero stored limit (the old "non-zero = limit on" rule).
- Backend tests cover the settings round-trip, the legacy-config inference,
  the history column migration and the transfer cap (on limits the sequential
  path; off stays unlimited even with a stored value).

## 0.18.0 — 2026-10-19

One confirmation window before every transfer.

- Manual "New URL" and browser handoff both probe the URL first without
  touching the list or the database, then show a Download Confirmation window
  with the full URL, the server-provided file name and size, a Category
  dropdown (extension-based default, e.g. Video, overridable) and a Location
  field (category folder by default, overridable with Browse).
- Buttons: **Download Now** inserts the row and starts the transfer;
  **Cancel** leaves nothing behind (no list row, no history entry); **Add to
  queue** is a visible placeholder until queue scheduling is implemented.
- The chosen folder is saved exactly (new `folderexact` history column), so a
  category/folder override survives a restart. Rows written before this
  release keep their base folder plus appended category folder.
- `DownloadRequest.apply_category_folder` lets the GUI pass a folder that
  already includes the category; CLI and legacy callers are unchanged.
- Backend tests cover the category list, the exact-folder override, and the
  history migration.

## 0.17.0 — 2026-10-19

The download list survives restarts.

- The main window restores the SQLite history at startup: completed, paused,
  and failed downloads appear again with their last known progress. Rows that
  were still `Downloading`/`Queued` when the app quit become Paused; nothing
  resumes automatically, the user presses Resume.
- Deleting a row (Delete, Delete File, or cancelling the add confirmation)
  removes it from `downloads` and `headers`, so removed entries never come
  back.
- A completed download whose file is gone from disk is marked
  `Complete (file missing)` (amber, with a tooltip) and right-click **Open
  File** explains that the file was deleted or moved, with **Open File
  Location**. One summary dialog lists the missing files after startup.
  Missing files are resolved against the folder by exact name and then by
  `name (N).ext` (closest modification time to the last attempt), so downloads
  saved with a unique suffix are still found.
- Completed rows now store the actual saved basename, so a file saved as
  `name (1).ext` reopens correctly after a restart.
- Progress is checkpointed to SQLite about every five seconds and on exit;
  paused/error rows restore their byte count from `segments.json` when the DB
  checkpoint is older. `HistoryStore` gained `load_all`, `remove`,
  `update_progress`, and a `done` column (additive migration).
- Last Try sorting uses the stored epoch instead of the display string, so the
  order stays correct across months. The Download window's Resume and Apply
  actions now go through the main window, keeping the history row in sync.
- Backend tests cover the new store API, the legacy-schema migration, and the
  missing-file resolver.

## 0.16.0 — 2026-10-19

Installable build: CMake install rules and a `.deb` package.

- `install()` rules put `swiftdm` in `/usr/bin`, a launcher entry in
  `/usr/share/applications`, hicolor icons in `/usr/share/icons/hicolor`, the
  AppStream metadata in `/usr/share/metainfo`, the Apache-2.0 copyright file in
  `/usr/share/doc/swiftdm`, and the Chromium extension in
  `/usr/share/swiftdm/chromium`. `cmake --install build --prefix /usr/local`
  installs without a package.
- `cpack` builds `swiftdm_<version>_amd64.deb` with dependencies resolved by
  `dpkg-shlibdeps`, plus `Recommends: chromium`. Maintainer scripts refresh the
  desktop and icon caches on install/remove.
- Added the missing `LICENSE` (Apache-2.0), `packaging/swiftdm.desktop`,
  AppStream metadata, and the `render-desktop-icons` target that regenerates the
  hicolor PNGs from `ui-assets/app-icon/main-icon.svg`.
- Build/install steps for Debian 13 are documented in `packaging/README.md`
  and the top-level README.

## 0.15.0 — 2026-10-19

One switch for "let the browser keep this download".

- Extension 0.4.0: the separate **Bypass** switch is gone. It did exactly what
  turning "Automatically use SwiftDM for downloads" off already does, so the
  popup now has two switches instead of three (automatic handoff, browser
  cookies). The status line says plainly that new downloads stay in the browser
  while the switch is off, and manual right-click/popup actions still send
  links and pages to SwiftDM.
- A stored bypass setting from an older build is migrated to "automatic
  interception off" when the extension updates, and the obsolete
  `bypassDownloads` key is removed.
- Headless extension tests cover the migrated setting and the switch-off path;
  docs (04 §3, 06, 07 C3/D13, extension README) now describe the single switch.

## 0.13.1 — 2026-10-19

Signed-in downloads get a path that non-CLI users can follow.

- The sign-in-wall message now names the two ways out with the labels the UI
  uses: share the file as "Anyone with the link", or open it in the browser and
  use the SwiftDM popup with "Send browser cookies" enabled.
- Extension 0.3.0: the popup gains a **Send browser cookies** switch. Only a
  user gesture can ask for the optional cookie permission, so the popup now
  asks for the site it was opened on (Chromium prompts once) and shows the
  current state; turning the switch off drops the grant again. The popup's
  "Download this page" and detected-media actions report that switch, and the
  service worker only reads cookies after such a grant - automatic
  interception still never prompts. Before this, cookies could only be granted
  from the context-menu action, which left automatic interception without a
  session for private files.
- Shared permission handling lives in `ext/chromium/cookie-permission.js`
  (loaded by the popup and imported by the service worker); the headless
  extension tests cover the prompt target, the refusal path, and cookie-carrying
  popup handoffs.

## 0.13.0 — 2026-10-19

Google Drive links end up in the file that was asked for, not in a page.

- Viewer and file links (`drive.google.com/file/d/<id>/view`, `/open?id=`,
  `/uc?id=`, `/drive/u/<n>/…`) are normalised to Drive's direct download URL
  (`drive.usercontent.google.com/download?id=<id>&export=download`), keeping the
  link's `authuser`/`resourcekey`/`confirm` handoff parameters.
- Drive's virus-scan notice ("Google Drive can't scan this file for viruses",
  shown for files above Google's scan limit) is confirmed transparently, so
  those files download with their real name and size instead of a 2 KB HTML
  page saved as `download`, `view`, or the Drive id.
- Transfers now use the URL the probe resolved (`ProbeResult::download_url`).
  The resolved URL is deterministic, so pause/resume keeps matching the partial
  file, also when the same file is retried through a different link shape.
- Links that never lead to a file fail with an explanation instead of saving a
  page: Drive folder links, Google Docs editor pages, and sign-in walls
  (private or removed files, including the browser's own `confirm=t` URLs
  fetched without the account session). The message names both ways out:
  share as "Anyone with the link", or hand the download over from the browser.
- Ordinary HTML pages from other hosts remain downloadable; a page is only
  auto-submitted when it carries a download-confirmation form.

Verified by build + CLI/backend suite (`test_http` gained a virus-scan notice
route, a notice that never resolves, a sign-in page, a plain page, and unit
tests for the link/notice parsing) plus live checks against Google Drive links
(public small/large files in every link shape, a folder link, and private-file
URLs taken from the app's own history).

## 0.12.0 — 2026-10-19

Make server refusals understandable (and survivable) instead of a bare
`HTTP/1.1 403 Forbidden`.

- Failure messages now quote the host. The first 1024 bytes of a 4xx/5xx body
  are summarized and appended after ` - ` (300 bytes max): JSON
  `value`/`message` fields (pixeldrain's `hotlink_detected`,
  `file_rate_limited_captcha_required`, `max_concurrent_downloads`, ...), an
  HTML `<title>` or the visible text with scripts/styles dropped, otherwise a
  single collapsed line. Probe, segmented and streamed failures all use it, and
  the same text is stored per download.
- Fixed: 3xx/4xx/5xx response bodies were written into the partial file at the
  segment offset; refused bodies are now kept for the error message instead.
- Downloads that are refused while running in parallel now retry once on a
  single connection (1 s pause) before failing, so hosts with a per-IP
  connection limit still complete.
- The error notification offers `Open Page in Browser` when the download came
  from a browser page, which is what hosts that gate downloads behind a CAPTCHA
  (pixeldrain's hotlink protection) need before a retry can work.

Verified by build + CLI/backend suite: `test_http` gained JSON/HTML refusal
fixtures, a connection-limited route and a single-connection fallback test.

## 0.11.1 — 2026-10-19

Fix the download progress window growing with the URL length.

- The per-download window had no fixed default size: the rich-text URL label
  reported its full text width as the layout minimum, so a long URL forced the
  window wider. It now uses a fixed default size (560×420, minimum 420×300)
  regardless of the URL, file name, or active tab.
- The URL line elides in the middle to fit the window and keeps the complete
  URL in its tooltip (and in the link target). Resizing the window re-elides.
- The confirm-download prompt and the download-error notification had the same
  problem: an unbroken URL cannot wrap, so it widened those windows too. The
  confirm prompt elides the URL (and a very long file name) with the full text
  in the tooltip; the error notification shows an elided URL with the complete
  URL behind **Show Details…**.

## 0.11.0 — 2026-10-19

Extra protocols and adaptive media: FTP, HLS, and paired video/audio handoff.

- FTP downloads: the app, CLI, and manual dialog accept `ftp://`/`ftps://`
  URLs. The engine probes the file with `SIZE` and downloads it over a single
  connection, resuming an interrupted transfer with `REST` (C11).
- HLS (`m3u8`): the engine parses master and media playlists, picks the
  highest-bandwidth variant, writes the `EXT-X-MAP` init segment when present,
  downloads every segment sequentially into one `.ts` file, and resumes at the
  first unfinished segment. Encrypted playlists are refused with a clear error
  instead of writing unusable data (C7).
- Adaptive pairing (C8): the extension remembers media-only responses per tab,
  pairs the newest video and audio tracks, and lists both — plus any detected
  HLS playlist — in the popup. Paired handoffs carry a shared `group` token;
  the app validates it, shows it in Properties, and stores it in
  `downloads.groupid`. Muxing the pair into MKV is documented as a v3 proposal
  (no muxer dependency is added yet).
- Tests: protocol coverage for POST/header/pair-token validation, subscribe,
  and status events; extension-logic coverage for bypass, POST/header
  forwarding, media detection/pairing, and the status channel; new `history`
  test for the SQLite store; new `media` test with a local FTP server
  (`SIZE`/`REST`/`EPSV`) and HLS fixtures (playlists, segments, resume).

## 0.10.0 — 2026-10-19

Browser integration: POST bodies, custom request headers, status push, and a
bypass option.

- `addDownload` accepts `method`, `postData` (UTF-8 or base64, ≤256 KiB), and
  `x-*` request headers (≤16 headers / 16 KiB). The engine posts the body and
  forwards the validated headers. Raw `Cookie`/`Authorization`-style headers
  stay rejected: cookies remain structured and domain-scoped (C13 decision).
- New `subscribe` command: subscribed extension sockets receive `status` events
  (`waiting`, `active` count) whenever the active-download count changes — the
  local equivalent of the original application's waiting/nowaiting push.
- Chromium extension 0.2.0: `webRequest` observation remembers POST bodies and
  `x-*` headers for 30 seconds so intercepted downloads carry them; a **Bypass**
  popup option keeps new downloads in the browser; the popup shows the live app
  status over the subscription socket.
- Browser page URL/title are carried on the download item and written to the
  SQLite history (`downloads.pageurl`/`pagetitle`), together with method, POST
  body, and forwarded headers. The history list is not restored at startup yet.
## 0.9.3 — 2026-10-18

Fix the `Open Folder` and `Open` buttons in the Download Complete notification.

- The actions were handled through `QMessageBox::buttonClicked` with the dialog
  as the receiver. The dialog closes and deletes itself on a button click, so
  the action could be skipped. Each button now connects directly to its action.
- Check that the folder/file still exists and report a failure instead of
  silently doing nothing.

## 0.9.2 — 2026-10-18

Fix the application quitting when a download notification is dismissed.

- Disable Qt's `quitOnLastWindowClosed` for the tray application. With the main
  window hidden in the tray and the progress window closed automatically, the
  `Download Complete` / `Download Error` dialog was the last visible window;
  clicking any of its buttons terminated the process and removed the tray icon.
  Quitting is now explicit (tray `Quit`, or closing the main window when no tray
  icon is available).
- Close notification dialogs after the button signal finishes, avoiding a race
  with `QMessageBox`'s internal close/delete handling.

## 0.9.1 — 2026-10-18

Chromium handoff connection fixes.

- Accept Edge's `extensions://` and `extension://` extension-origin spellings in
  addition to `chrome-extension://`, so WebSocket handshakes from Edge are not
  rejected.
- Show the last rejected handshake reason in the Extensions dialog and the status
  bar, and include it in the extension's connection error.
- Retry automatic handoff once after a short delay before restoring the browser
  download, covering app/service-worker startup races.
- Fix handoff without cookie permission: the extension now sends an empty cookie
  array instead of an empty string, which the app had rejected as an invalid
  download request. The server also tolerates the legacy empty string.

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
