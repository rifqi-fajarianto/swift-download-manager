# Changelog

## 0.20.4 — 2026-10-29

The main window no longer drifts down when it is reopened.

- The saved position came from `geometry()` (the client area) while `move()`
  uses the window-frame position, so every restore added the title-bar offset
  and the window crept downward. The position is now read from `pos()` (the
  frame position); the size still comes from the client size that `resize()`
  expects.
- Maximizing or entering full screen no longer overwrites the saved normal
  geometry: the debounced save before the maximize keeps the last normal
  position/size, and the next start opens there. The maximize state itself is
  still not remembered.

## 0.20.3 — 2026-10-29

The main window remembers its size and position.

- Resizing or moving the main window now saves `WindowWidth`/`WindowHeight`
  and `WindowX`/`WindowY` in the `[interface]` section, and the next start
  restores them. The write is debounced (about half a second after the last
  resize/move) and flushed when the app quits, so a quit right after dragging
  the window keeps the new geometry.
- A maximized or full-screen window stores its normal geometry, so the next
  start opens as a normal window at the previous size/position. Restored values
  are clamped to the screen that holds the window; a saved position that no
  longer overlaps any screen (removed monitor, smaller resolution) is ignored
  so the window cannot open out of reach. Wayland compositors may ignore the
  restored position.
- Tests cover loading valid, partial, and invalid geometry keys plus the
  save/load round-trip.
- Version policy note: this UI behavior change ships as a PATCH bump
  (0.20.2 → 0.20.3) on the maintainer's request instead of the usual MINOR
  bump.

## 0.20.2 — 2026-10-28

- The Download Complete window (and the other notification windows) now takes
  clicks while the Settings dialog, the Download Properties window, the Browser
  Extensions window or About is open. Those dialogs used `exec()`, whose
  application-wide modal loop blocked every other top-level window of the app.
  They are now window-modal and opened non-blocking, so only the main window is
  blocked; Settings keeps raising its existing window instead of opening a
  second one.

## 0.20.1 — 2026-10-28

- The `Get file creation date from server` option no longer carries the stale
  "Not implemented in SwiftDM yet" tooltip; it now describes what 0.20.0
  applies (HTTP `Creation-Date`/`Last-Modified`, FTP `MDTM`).

## 0.20.0 — 2026-10-28

Per-download logs, the server file date, and a smaller Settings window.

- Every download row now keeps its own log at
  `<app data>/logs/<history id>-<filename>.log` (Linux
  `~/.local/share/swiftdm/logs`, Windows `%LOCALAPPDATA%\SwiftDM\logs`). The
  engine appends a timestamped line per event — probe result, segment plan or
  resume, the 403 single-connection fallback, pause, completion, failure with
  the host error — so a resumed download keeps the earlier attempts. The file
  is capped at 1 MiB (the oldest content is dropped past that), CLI runs write
  no log, and deleting the row deletes its log.
- Download Properties gains a `Log file` row with an `Open Log` button; the
  button stays disabled until the row owns a log file.
- `ServerCreationDate` ("Get File Creation Date From Server") is applied: the
  probe now records the server file time from the HTTP `Creation-Date` /
  `Last-Modified` headers or from `CURLINFO_FILETIME` (the FTP `MDTM` the probe
  already asks for), and a completed download gets that value as its
  modification time. Only the modification time is set; Windows keeps its own
  creation time.
- The empty `Passwords` tab is gone from the Settings window until the `auths`
  store exists; the planned tab is documented in `docs/spec/01-product.md` and
  the parity checklist. The window keeps four tabs (General, Interface,
  Download, Proxy/Socks).
- Tests cover the applied server date (HTTP and disabled cases), the log file
  (content and append across runs), the Properties log row, and the four-tab
  dialog.

## 0.19.19 — 2026-10-28

Allow browser downloads to follow cross-origin redirects without leaking request metadata.

- Probe and text fetch requests now retry a blocked cross-origin redirect at the resolved target after dropping caller `x-*` headers, `Referer`, and any POST body. 301/302/303 continue as GET; a cross-origin 307/308 that would resend a body still fails closed.
- `DownloadEngine` transfers the probe's resolved URL directly when the origin changed and strips the same metadata before transfer, so GitHub release downloads (`github.com` -> `release-assets.githubusercontent.com`) work from the Chromium extension instead of failing with `redirect blocked because the request contains custom headers or a referer`.
- Cookies, user agent, proxy and same-origin redirect headers keep their existing behavior. Transfers that still encounter a sensitive cross-origin redirect fail closed.
- Regression tests cover probe, `fetch_text`, segmented transfer and sequential transfer, plus same-origin headers.

## 0.19.18 — 2026-10-27

- The Windows GUI executable now uses the GUI subsystem, so launching it from
  Explorer, the installer shortcut, the tray autostart entry or a shell no
  longer opens a console window next to the application. CLI runs (`--version`,
  `--help`, `get`) attach to the parent console with `AttachConsole` and keep
  working, or allocate their own console when started without one. Output that
  is already redirected to a pipe or a file keeps its handle, so the CI version
  check and scripted use are unaffected. Windows shells do not wait for a
  GUI-subsystem process; scripts that must block on `get` use
  `Start-Process -Wait` or `start /wait`. Headless Windows builds keep the
  console subsystem.
- The Windows executable and the Inno Setup installer now carry the SwiftDM
  application icon instead of the stock Windows one. `ui-assets/app-icon/main.ico`
  holds the icon at 16, 24, 32, 48, 64, 128 and 256 px, rendered from
  `main-icon.svg` by the new `render-ico` target; `src/app/swiftdm.rc.in` embeds
  it in the executable and `SetupIconFile` gives the installer, its shortcuts
  and the uninstaller the same icon.

## 0.19.16 — 2026-10-27

- The experimental Windows 10 port now lives on `main`; the `windows` branch was
  merged fast-forward at 0.19.15. No application behavior changed for Linux.
- Windows packages gain an Inno Setup installer
  (`swiftdm-<version>-win64-setup.exe`, target `windows-installer`). It installs
  per user into `%LOCALAPPDATA%\Programs\SwiftDM`, adds Start Menu (and optional
  desktop) shortcuts, and registers an uninstaller. The portable ZIP stays
  available. The target is skipped when `ISCC.exe` is absent.
- `.github/workflows/release.yml` builds the Windows port next to the Debian
  package: the `windows-package` job runs `ctest`, `cpack` and the installer on
  `windows-latest`, the release job waits for it and publishes `.deb`, ZIP,
  installer and one `SHA256SUMS` covering all three, with provenance
  attestation for each artifact.
- `workflow_dispatch` on the release workflow gains a `dry_run` input (default
  on): the full pipeline runs and uploads artifacts, but no release is created,
  edited, or attested.
- Windows packages are not code signed yet; SmartScreen warns on first run.
- Fixed the Windows package deploy step, which aborted the CI build with
  "ldd found no MinGW runtime DLLs". `tools/deploy_msys_runtime.sh` now walks the
  PE import table with `objdump` and copies each imported DLL found in the MSYS2
  prefix (following the dependencies of the copied DLLs), instead of matching
  `ldd` output against a fixed POSIX prefix path. The prefix is normalised with
  `cygpath`, and a failure now prints the imports and the prefix.

## 0.19.15 — 2026-10-27

- Progress updates reach the UI ten times per second instead of four:
  `kReportIntervalMs` is now 100 ms (was 250 ms) and the segment loop no longer
  sleeps past the next report. The Bandwidth column, the Remaining Time
  estimate and the progress window follow the same cadence, so the reported
  speed moves fluidly instead of stepping every few ticks.
- The speed sample window shrinks to 200 ms (was 500 ms) so the displayed value
  reacts sooner; the EMA smoothing (`0.6 * old + 0.4 * instant`) is unchanged.
  The window is now `kSpeedSampleMs` in `net/http_internal.h`.
- Tuning only: DB checkpoints (5 s), resume state (2 s) and the transfer paths
  are untouched; sequential/FTP/HLS downloads still report no speed value.

## 0.19.14 — 2026-10-27

Windows only; Linux behavior unchanged.

- `resize_file()` no longer zero-fills the part file on Windows. `_chsize_s()`
  writes every byte of the extension, so preallocating a multi-gigabyte part
  file kept the process busy writing zeros (measured 1.4 s for 512 MiB) before
  the first network byte arrived, which looked like a stalled download. The
  Windows branch now seeks to the new length and calls `SetEndOfFile()`, which
  leaves the added range unallocated (0.001 s for the same size). Preallocation
  itself is unchanged, so resume semantics stay the same on both platforms.
- `tests/test_file_io.cpp` grows a Windows-only regression: a 512 MiB resize
  must report the new length, end in a zero byte and issue less than a quarter
  of the size in process writes.

## 0.19.12 — 2026-10-25

Code layout only; no behavior change.

- `src/net/http.cpp` (1763 lines) is split into per-concern translation units:
  `http.cpp` (reason phrases, URL scheme checks, `fetch_text`), `http_request.cpp`
  (shared libcurl plumbing, header list, origin guard, common options, content
  range/Disposition parsing), `http_error.cpp` (error text and body summaries),
  `http_probe.cpp` (probe, FTP probe, interstitial resolution), `http_segment.cpp`
  (`SegmentDownloader`) and `http_stream.cpp` (sequential `stream_download`).
  Cross-unit helpers and constants live in `src/net/http_internal.h`
  (`swiftdm::detail`); `src/net/http.h` is unchanged.

## 0.19.11 — 2026-10-25

Small correctness, metadata and CI/release hardening (audit finding 8).

- The segment writer retries `pwrite()` on `EINTR` and finishes a short write,
  so a partial write is no longer reported as "disk write failed".
- The autostart entry quotes the executable path as a single `Exec` argument
  (reserved characters backslash-escaped, per the desktop entry spec); a path
  with spaces used to break the entry.
- `sanitize_filename()` prefixes a leading dot with `_` (no hidden
  server-named dotfile, no `..` name) and appends `_` to a `.desktop` suffix
  (a downloaded desktop entry could be launched from the download directory).
- The URL allowlist rejects userinfo (`https://user:pw@host/…`) on the CLI path
  too, matching the browser integration; proxy authentication keeps its own
  setting.
- Unused `fallback` parameter removed from the settings integer parser (the
  compiler warning is gone).
- The Debian homepage and the Metainfo homepage/bugtracker point at the actual
  repository.
- CI gained a sanitizer job (ASan+UBSan and TSan on the headless build). The
  test servers no longer read their listening descriptor after `stop()` has
  closed it, which was the only report.
- The release workflow keeps `contents: read` at workflow level (write only in
  the release job), publishes a `SHA256SUMS` file and a build provenance
  attestation for the `.deb`.
- `src/ui/main_window.cpp` is split into per-concern translation units
  (lifecycle, history restore, download lifecycle, row actions, dialogs,
  queues, shutdown) with no behavior or layout change; the record/state helpers
  moved to `src/ui/main_window_internal.h`.

## 0.19.10 — 2026-10-25

Claim the final file name atomically instead of checking then renaming.

- `move_file_unique()` picks the first free `name (N).ext` candidate and claims
  it in the same step (`link()`, or `O_EXCL` create for a cross-filesystem
  destination). A file another process creates between the name choice and the
  move is therefore never overwritten; the next candidate is used instead
  (audit finding 7).
- The cross-device fallback copies into a freshly created exclusive file and
  never uses an overwriting copy.
- `unique_path()` keeps its existing contract for callers that only need a name
  suggestion; every completion path now goes through the atomic claim.
- Regression test covers repeated completions of the same file name, a taken
  plain name and a gap in the `(N)` sequence, and verifies the existing files
  keep their content.

## 0.19.9 — 2026-10-25

Validate resumed transfers against the server's own range metadata.

- Range requests now send `If-Range` with the strong `ETag`, falling back to
  `Last-Modified` when no strong validator exists. A `200` answer for a segment
  that already had progress means the remote file changed: the download fails
  with "the remote file changed during the download; restart is required"
  instead of writing the new representation over the old partial data (audit
  finding 6).
- Every `206` answer must carry a well-formed `Content-Range` whose start, end
  and total match the requested segment and the expected file size. A missing,
  shifted, truncated or mismatched range fails the segment before any byte is
  written to the part file.
- Regression tests cover `If-Range` on a resumed segmented download, a changed
  validator on the server, and three malformed `Content-Range` shapes, and
  assert that no new bytes were stored in the failed cases.

## 0.19.8 — 2026-10-25

Block cross-origin redirects on requests that carry custom headers or a referer.

- `probe_url`, `fetch_text`, segmented transfers and sequential transfers now
  compare redirect origins before sending each redirected request whenever
  caller-provided `x-*` headers or a `Referer` are present. Same-origin
  redirects continue to work; a cross-origin redirect fails with a clear
  message, preventing tokens or page URLs from leaking (audit finding 5).
- Regression coverage checks both custom headers and referers on probe,
  sequential and segmented paths, confirms no sensitive metadata reached the
  redirect target, and preserves same-origin redirect behavior.

## 0.19.7 — 2026-10-25

Restrict the loopback browser endpoint to SwiftDM's extension and expire stalled
WebSocket handshakes.

- The WebSocket Origin must use the extension ID derived from the checked-in
  Chromium manifest key (`hagednkleljjijmjonpeiokknhngknbb`). Chromium and the
  accepted Edge extension schemes remain supported; unrelated extensions can
  no longer connect just by presenting a syntactically valid ID (audit finding
  4).
- Incomplete handshakes are closed after five seconds, freeing one of the
  server's bounded client slots.
- `test_browser_protocol` now checks wrong extension IDs and schemes are
  rejected, the repository ID is accepted, and a client that sends no handshake
  is disconnected at the timeout.

## 0.19.6 — 2026-10-24

Protect locally stored configuration, history and partial downloads.

- Settings are serialized to a same-directory temporary file and atomically
  renamed into place with mode `0600`; existing config files are tightened to
  that mode when read. The app config directory is owner-only (`0700`), and
  newline/carriage-return characters in serialized string values are replaced
  with spaces so they cannot inject additional INI keys.
- The SQLite history file is created and maintained at `0600`; its containing
  directory is `0700`, which also protects SQLite journal/WAL siblings. Existing
  history file and app directory permissions are tightened on open.
- The configured temp root and per-download workspaces use mode `0700`. Part
  files keep their prior `0644` mode inside the private workspace; final
  download directory/file permissions are unchanged (audit finding 3).
- Regression coverage checks permissions for new and existing config/database
  files, atomic replacement preservation, newline sanitization, and the
  workspace/part-file modes. Proxy password Secret Service integration and a
  UI plaintext warning remain deferred by maintainer choice.

## 0.19.5 — 2026-10-24

Follow a download-confirmation form only on the origin that served it.

- `resolve_interstitial()` now compares the origin (scheme, host, effective
  port) of the confirmation form's `action` with the page that served the
  notice. A mismatch fails the probe with
  "download confirmation page could not be resolved" and no second request is
  sent, so a page from any host can no longer aim the follow-up request at an
  address of its choosing, including hosts on the local network (audit
  finding 2).
- Google Drive's handoff between its own download hosts
  (`drive.google.com`, `drive.usercontent.google.com`,
  `drive.googleusercontent.com`) stays allowed, and the existing Drive
  virus-scan flow is unchanged.
- Regression test: `test_http` serves a Drive-shaped form whose action points at
  `localhost` while the page comes from `127.0.0.1`; the probe fails and the
  request counter for the form target stays at zero.

## 0.19.4 — 2026-10-24

Refuse transfer URLs outside `http`, `https`, `ftp` and `ftps`.

- Every URL that reaches libcurl is now checked by
  `is_supported_download_url()` (`src/net/http.cpp`), and every handle also
  pins `CURLOPT_PROTOCOLS`/`CURLOPT_REDIR_PROTOCOLS` (or the `_STR` variants)
  to the same list. Redirect targets inherit the restriction.
- HLS playlists are remote input: a `file://` segment, `EXT-X-MAP` init
  segment, or master variant now fails the download with "the HLS playlist
  references an unsupported URL scheme" before the part file is created,
  instead of copying a local file into the output (audit finding 1).
- `fetch_text()` and `stream_download()` reject an unsupported scheme even
  when a caller skips the probe, so the guard also covers the CLI path.
- Regression tests: `test_http` (scheme allowlist plus direct `fetch_text` /
  `stream_download` calls) and `test_media` (three `file://` HLS fixtures).

## 0.19.3 — 2026-10-24

Queue items no longer trigger the global completion actions.

- A completed queue item no longer opens the `Download Complete` window or the
  download folder, even while both options are enabled in Settings. A queue
  owns its completion notice through `Notify when an item completes` (tray
  message, status-bar fallback).
- The global `Show Download Completion Dialog` and `Open the Download Folder
  When a Download Completes` options now apply only to downloads in the main
  Downloads list.
- The all-complete notification and the shutdown countdown of a queue are
  unchanged.

## 0.19.2 — 2026-10-24

Ask which queue a download belongs to instead of silently using the first one.

- `Add to queue` in the Download Confirmation window opens a `Choose Queue`
  picker when more than one queue exists. The picker lists every queue with its
  item count, starts without a selection, and enables `Add to queue` only after
  the user picks one; `Cancel` returns to the confirmation window.
- With a single queue, `Add to queue` uses it directly. A download started from
  a queue view still targets that queue without a picker and keeps
  `Add to queue` as the default button.
- The inline `Queue` row and its dropdown are removed from the confirmation
  window, so the target is chosen in one place. The window returns to its
  650×330 size (minimum 560×320).
- `Download Now` continues to target the main Downloads list.
- This is a UX change shipped as a patch (0.19.2) at the maintainer's request;
  the normal policy for UX changes is MINOR +1.

## 0.19.1 — 2026-10-23

Switch the toolbar to queue mode when the `Queues` branch is selected and show
a queue overview there.

- The `Queues` branch in the sidebar is now selectable. Selecting it switches
  the toolbar to queue mode and shows a list of queues (name + item count) in
  the right panel; with no queues, the panel shows a `New Queue` prompt.
  Selecting a queue from the list opens its downloads.
- Queue mode toolbar order: `New Queue`, `Delete Queue`, `Queue Properties` |
  `Start Queue`, `Stop Queue` | `New URL`, `Delete`, `Delete File`. Queue
  actions that need a specific queue stay disabled while only the branch is
  selected, and `Move to Downloads` remains available from the item context
  menu.
- New queue icons from `ui-assets/`: `queue.svg` (branch/children),
  `queue-add.svg` (`New Queue`), `queue-delete.svg` (`Delete Queue`) and
  `queue-settings.svg` (`Queue Properties`).
- The queue overview title uses the active theme text color with a slightly
  larger bold font, and the title/empty-state colors refresh when the theme
  changes while the app runs, so they stay readable in light and dark mode.

## 0.19.0 — 2026-10-23

Add named download queues with independent scheduling, limits and completion
actions.

- The left panel now has a `Queues` branch. A queue is a named workspace:
  selecting it shows only its items, while `All Downloads`, the categories,
  `Complete` and `Incomplete` show only items outside queues. `New Queue`
  creates a queue from the toolbar or the sidebar context menu.
- `Queue Properties` controls the simultaneous-download limit (0 = Unlimited),
  the connection count per item (0 = the global setting), a total speed limit
  shared by the active items, autostart, per-item and all-complete
  notifications, and an optional shutdown with a cancellable delay (5-3600 s,
  default 60 s). These settings live in the `queues` table, not in the INI
  file.
- `Start Queue` resumes queued, paused and failed items in insertion order;
  the automatic scheduler starts only queued items and respects the
  simultaneous limit. Manual starts from the context menu or the progress
  window honor the same limit. Lowering the limit pauses the newest active
  items and returns them to `Queued`; a manual pause stays `Paused`. A queue
  speed limit is split across the active items (minimum 1 KB/s) and replaces
  the per-item limit while the item belongs to the queue. A queue connection
  count replaces the per-item value in the same way.
- Inactive items can be moved between `Downloads` and a queue from the context
  menu or the queue toolbar; completed items can move too and remain visible
  in the queue. Deleting a queue asks for confirmation, deletes its rows and
  temporary workspaces, and leaves completed files on disk.
- The Download Confirmation window has a `Queue` row (`None (Downloads)` plus
  the existing queues). `Add to queue` is enabled only when a queue is
  selected; with the confirmation dialog disabled, `New URL` from a queue view
  adds to that queue while the main view and browser handoff use the main
  Downloads list.
- At startup, unfinished rows in a queue return as `Queued`; a queue with
  autostart starts immediately. Queue completion can show a tray notification
  (status-bar fallback), and the shutdown option pauses every active download
  and queue, persists their state, and runs `systemctl poweroff` (falling back
  to `shutdown -h now`).
- Queue state is persisted in SQLite: a `queues` table plus an additive
  `downloads.queueid` column (0/NULL = main Downloads). Existing rows keep
  working. Tests: `queue_logic`, `queue_dialog`, and extended `history`,
  `confirm_dialog`, `download_window`.

- The system tray now shows the colored SwiftDM mark (the same artwork as the
  browser integration) instead of the monochrome variant. The mark keeps its
  gradients, so it stays readable on light and dark tray panels.
- Chromium extension 0.5.2: the popup follows the design in
  `ext/extension-ui-template.html` (kept as the design reference). The header
  shows the app mark, the app version reported by the running app and a pill
  with the connection state; the two settings became switch rows; the
  "Download this page" button, the detected-media list, the retry button and
  the collapsible footer note keep their previous behavior. The template is
  not shipped: the styles live in `popup.css` and the behavior in `popup.js`,
  as the extension page CSP allows only extension scripts. The popup follows
  the light and dark color schemes of the browser.

## 0.19.13 - 2026-10-27

Merge `main` (0.19.12) into the experimental Windows 10 port and re-apply the
port on top of the new code layout.

- Bring in the named queues, the 0.19.4–0.19.11 security hardening and the
  0.19.12 module split (`src/net/http*`, `src/ui/main_window_*`).
- `claim_path()` keeps the atomic no-overwrite rule on Windows: a hard link
  claims the destination first, and a volume/filesystem boundary falls back to
  creating the destination exclusively and copying into it. `core/file_io.*`
  gained the `FileExclusive` flag and `read_file()` for that path.
- `history.cpp` keeps the POSIX `0600` + `O_NOFOLLOW` open on Unix and uses
  `sqlite3_open16` on Windows so a non-ASCII user name keeps working.
- `settings.cpp` loads the INI through a wide-path stream on Windows and keeps
  the hardened descriptor path on POSIX; directory privacy goes through
  `path_from_utf8`/`path_to_utf8`.
- The UI units keep the Windows guards: `xdg-open` fallbacks are Unix-only and
  the shutdown action calls `shutdown /s /t 0` on Windows.
- The Winsock test servers keep `socket_t` descriptors and provide the new
  `If-Range`, invalid-range and cross-origin confirmation-form routes.
- Windows support is still experimental: the GUI has only been smoke-tested
  through the CLI, and manual Windows 10 checks (tray, dialogs, pause/resume,
  autostart) remain open. The executable uses the console subsystem, so
  launching the GUI from Explorer also opens a console window.

## 0.19.0 - 2026-10-23

Start the experimental Windows 10 port while keeping Debian 13/Linux as the
reference platform.

- Add a cross-platform file I/O layer with wide-path support on Windows,
  positioned writes, file sizing and flushing.
- Use Windows roaming/local application data directories and the per-user
  Startup registry key; keep XDG autostart and paths on Unix.
- Support paths longer than `MAX_PATH`: the platform layer prefixes absolute
  paths with the extended-length `\\?\` form and strips it again on the way
  out, so neither the history database nor the UI sees the prefix. Workspace
  cleanup uses a Win32 tree walk because the MinGW recursive iterator spins
  forever on a name beyond `MAX_PATH`. File names are still limited to 240
  bytes per component.
- Read the command line as UTF-16 and convert it to UTF-8, so `--name` and
  `--dir` accept non-ASCII text on Windows.
- Port the local HTTP/FTP/HLS test servers to Winsock. The `http` and `media`
  tests now run on Windows, and the Windows CI job executes the same test
  suite as Linux.
- Deploy the Qt runtime with `windeployqt` and the MinGW/libcurl/SQLite
  dependency tree with `tools/deploy_msys_runtime.sh`, so the ZIP package runs
  without MSYS2 installed. Ship the Chromium extension together with
  `LICENSE` and `README.md` in the package.
- Ship the Mozilla CA bundle as `ca-bundle.crt` and point libcurl at it with
  `CURLOPT_CAINFO`. The MSYS2 build of libcurl defaults to the POSIX path
  `/ucrt64/etc/ssl/certs/ca-bundle.crt`, which does not exist outside MSYS2,
  so every HTTPS download failed with `CURLE_SSL_CACERT_BADFILE`
  ("problem with the SSL CA cert"). The lookup order is the `SWIFTDM_CA_BUNDLE`
  environment variable, then `ca-bundle.crt` next to the executable, then
  libcurl's built-in default — so Linux keeps its system bundle and the
  Debian package is unchanged. Verified by the `ca_bundle` test and by HTTPS
  downloads from the Windows package.
- Separate Linux desktop metadata and Debian packaging from the Windows ZIP
  configuration. Qt 6.5–6.12 is required; 6.12 is the last line that supports
  Windows 10.
- Windows support is still experimental: the GUI has only been smoke-tested
  through the CLI, and manual Windows 10 checks (tray, dialogs, pause/resume,
  autostart) remain open. The executable uses the console subsystem, so
  launching the GUI from Explorer also opens a console window.

- The extension reuses its command WebSocket between handoffs and opens the
  command channel while browser cookie metadata is being gathered. The status
  subscription remains a separate socket.

## 0.18.22 — 2026-10-22

Redesign the Download Properties, Download progress and Download Confirmation
windows around labeled sections.

- The read-only values are grouped into `General`, `Location`, `Source` and
  `Time`, using the same section spacing as the settings window (0.18.20).
- `General` holds the file size, status, category, downloaded amount,
  resumable flag and segment count. `Location` holds the saved folder with
  `Open Folder` and the temp path. `Source` holds the URL and the optional
  page URL, page title and pair token. `Time` holds the added and last-try
  timestamps.
- Each section has a bold title; labels use sentence case and left alignment.
  Short values are selectable labels, while paths and URLs stay read-only
  fields so they can be scrolled and copied.
- The header keeps the properties icon and file name. Long names are elided in
  the middle and keep the full name in their tooltip.
- The dialog opens at 650×690; its minimum size follows the section layout.
  The conditional source rows grow the window instead of clipping.
- Fields, values, the status color and `Open Folder` keep their previous
  behavior. No data or engine behavior changed.
- Regression test `properties_dialog` covers the section order, field
  placement, conditional rows, header elision and the OK button.

The progress window now shares the same section language and opens at 650×500
(minimum 520×420).

- The Download tab groups the read-only values under `Details` (Status, File
  size, Downloaded, Bandwidth, Remaining time, Resumable) and the progress
  widgets under `Progress` (bar with the percentage inside, segment bar and
  `Segments: N`). `Resume`/`Cancel` are right-aligned.
- The Options tab groups the fields under `Destination` (Save to, File name)
  and `Transfer` (Connections, Bandwidth limit, Referer, note). The shared
  speed-limit editor puts the toggle and preset on the first row and the
  custom KB/s value below it. `Apply` stays at the bottom right.
- The Connections tab draws the segment table inside one frame, without the
  table's own border or grid lines.
- The section headings are bold and use the shared spacing of the settings and
  properties windows; no field or engine behavior changed.
- Regression test `download_window` covers the tab order, section order, field
  placement, default/minimum size and the framed table.

The confirmation window now uses the same sections and opens at 650×330
(minimum 560×320).

- `File` holds the name and size; `Save` holds the URL, category and location.
  Both titles are bold and use the shared section spacing.
- The file name is still elided in the middle with the full name in its
  tooltip, and the URL stays a read-only field.
- The buttons move to one row: `Cancel` on the left, `Add to queue` and
  `Download Now` on the right; `Download Now` remains the default button. All
  inputs and buttons keep their common height.
- `confirm_dialog` now also covers the section order, field placement, button
  order and the hidden queue hint.
- This UX change uses PATCH 0.18.22 by the maintainer's decision rather than
  the usual MINOR bump.

## 0.18.21 — 2026-10-22

Make the toolbar Resume and Stop buttons reflect what they can act on.

- Resume is enabled only while at least one selected download is paused, failed
  or queued and its worker is not running. Stop is enabled only while at least
  one selected download is still transferring. Complete or probing rows leave
  both buttons disabled. A mixed selection enables each button that has at
  least one matching row, so a downloading + paused selection can use both
  actions.
- `on_resume()` and `on_stop()` use the same per-row rules as the button
  states, so the toolbar cannot offer an action that does nothing.
- The toolbar has a divider after `New URL` and after `Stop`.
- Regression test `action_state` covers the status matrix and mixed selections.
- This UX change uses PATCH 0.18.21 by the maintainer's decision rather than
  the usual MINOR bump.

## 0.18.20 — 2026-10-22

Group the settings window into labeled sections so the options are easier to
scan.

- Every settings tab now uses the same section layout: `General` (Downloads,
  Startup, System tray), `Interface` (Toolbar, Appearance, System tray),
  `Download` (Connections, Bandwidth, User-Agent, Files and folders),
  `Proxy/Socks` (Proxy type, Server, Authentication) and `Passwords` (Saved
  passwords).
- Labels use sentence case and consistent punctuation; the leftover spaces in
  the connection-policy and category-folder labels are gone. The "8 connections
  recommended" hint follows the active theme's link color instead of a fixed
  dark-theme blue.
- The dialog opens slightly larger (720×580) and its minimum size follows the
  section layout, so a short window cannot squeeze the directory fields down to
  an invisible row.
- No setting key, default, stored value or behavior changed. This layout-only
  UX change is versioned as 0.18.20 by the maintainer's decision rather than
  the usual MINOR bump.
- The headless settings test now checks the section titles and order, that
  every interactive control sits inside a section, and that the dialog leaves
  its minimum size to the layout so the rows cannot be squeezed.

## 0.18.19 — 2026-10-22

Reduce the wait before browser confirmation and the first transfer progress.

- The confirmation dialog opens immediately while the server probe runs in a
  worker. It initially shows the browser/URL fallback name and `Checking…`;
  Download Now stays disabled until the resolved metadata is ready. The file
  name, size and default category are then updated without replacing any
  category or location the user already selected. Cancel still creates no row
  or history record.
- The successful confirmation probe is passed through the GUI task into the
  engine. The initial transfer no longer repeats the same HEAD/range probe;
  resume continues to perform a fresh probe to validate saved state. Resolved
  URLs (including Drive interstitial results), validators and range metadata
  are retained.
- The Chromium service worker reuses a command WebSocket for successive
  requests and correlates replies by request id; failed/closed sockets reject
  pending requests and are recreated on retry or the next handoff. Handoff and
  application latency milestones are logged for diagnosis.
- Regression tests cover engine probe reuse, pending confirmation metadata and
  command-socket reuse. This behavior/UX change is versioned as 0.18.19 by the
  maintainer's decision rather than the usual MINOR bump.
- The extension was bumped independently to 0.5.2 for the command-channel
  reuse and handoff warm-up.

## 0.18.18 — 2026-10-21

Shorten file names that are too long for the filesystem.

- A download whose name comes from a very long URL path (a token in the path,
  for example) or from a long `Content-Disposition` header no longer fails
  with "cannot create '...': File name too long". The name is shortened to 240
  bytes: the beginning of the name and the extension are kept, and a hash of
  the original name is appended so two different long names never end up as
  the same file. The shortened name stays stable across runs, so a paused
  download resumes with the same temporary file.
- The temporary file in the workspace (`<name>.part`) and the de-duplicated
  name of an existing file (`name (1).ext`) obey the same limit, so both
  always fit in a single path component.
- The names of Windows device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`,
  `LPT1-9`) are sanitized, because Windows rejects them even with an
  extension. This prepares the app for the Windows release.
- The name shown in the confirmation dialog, in the list and in the Options
  tab is the name that is really written to disk.

## 0.18.16 — 2026-10-21

Use the app's own icons in the message boxes.

- The message boxes of the app no longer use the built-in icons of the
  platform. The Browser Extensions window shows the filled information icon,
  the Quit confirmation the filled question icon, the Download Complete
  window the filled check icon and every warning (Delete File, Download
  Error, a missing file or folder, and the warnings of the Settings and the
  Download Confirmation windows) the filled alert icon.
- The glyphs are tinted with the palette of the app: blue for information and
  question, red for warnings and green for the completion notice. In the light
  theme the darker variants are used, so they stay readable on the dialog
  background.
- The icons come from `ui-assets/info-filled.svg`, `allert-filled.svg`,
  `question-filled.svg` and `complete-filled.svg`.

## 0.18.17 — 2026-10-21

Add a switch for the download confirmation dialog.

- The `General` tab has a new option: "Show Download Confirmation Dialog".
  While it is on, every transfer asks for confirmation with the file name, the
  size, the category and the destination folder, exactly as before.
- With the option off, the confirmation is skipped: the download starts right
  away with the category from the file extension and the default folder (the
  download root, plus the category folder when the category folders are
  enabled). The progress window still opens, so an automatic download behaves
  like a confirmed one apart from the question.
- A note under the option explains this; it is shown while the option is off.
- The setting is stored in the `[general]` section as
  `ShowDownloadConfirmationDialog`. A configuration file without the key keeps
  asking for the confirmation, so an upgrade does not change the behavior.

## 0.18.15 — 2026-10-21

Render the launcher icons at build time.

- The hicolor launcher icons are now rendered during the build from
  `ui-assets/app-icon/main-icon.svg` into `build/generated/icons/hicolor/`, and
  the install and `.deb` rules take them from there. A changed app icon
  therefore reaches the installed launcher icons with a plain
  `cmake --build build`; no manual step is needed anymore.
- The checked-in PNGs under `packaging/icons/` were removed, so the SVG in
  `ui-assets/app-icon/` is the only source of the app icon.
- The app icon itself was updated: the window and launcher now show the new
  mark.

## 0.18.14 — 2026-10-21

Use the power glyph for the toolbar Quit button.

- The Quit button in the main window toolbar now uses the power glyph from
  `ui-assets/quit.svg`, so it matches the style of the other toolbar icons.
- The hand-drawn logout arrow that was used before was removed; the Quit
  action itself is unchanged.

## 0.18.11 — 2026-10-21

Store the new settings sections and keys.

- The configuration file now has one section per settings tab: `[general]`,
  `[download]`, `[interface]` and `[proxy]`.
- The download options moved from `[general]` to `[download]`. Configs written
  by older releases keep working: the loader reads `[general]` first and
  `[download]` overrides it, so the section order in the file does not matter.
- New keys in `[general]`: `ShowTrayIcon`, `CloseToTray` and
  `OpenFolderAfterCompletion`.
- New section `[interface]`: `Theme` (`system`, `light`, `dark`), `ToolbarSize`
  (`normal`, `small`), `ShowToolbarText` and `TrayIconStyle` (`colorful`,
  `simple`). Unknown values keep the default instead of being stored.
- The keys are documented in `docs/spec/05-settings.md`, including the
  compatibility rules for older configs.
- The settings store tests cover the new keys, the section fallback and the
  validation of the interface values.

## 0.18.13 — 2026-10-21

Add the app behavior options to the General tab.

- The `General` tab has three new options: "Show System Tray Icon",
  "Minimize to Tray When Closing the Window" and "Open the Download Folder When
  a Download Completes".
- The tray icon can be switched off; the icon disappears right away and the
  app is closed with the main window instead of hiding into the tray. Without a
  tray icon the app also starts with the main window visible, even when it was
  started at login.
- With "Minimize to Tray" switched off, closing the main window ends the app.
  The same confirmation as the Quit action is asked first, and cancelling
  keeps the window open.
- "Open the Download Folder" opens the folder of a finished download as soon
  as the transfer is done.
- The settings dialog test now covers the three new options.

## 0.18.12 — 2026-10-21

Add the Interface tab with the appearance options.

- New tab `Interface` with four options: toolbar size (`Normal` / `Small`),
  "Show Toolbar Text", app theme (`Light` / `Dark` / `Follow System`) and tray
  icon style (`Simple` / `Colorful`).
- The changes take effect immediately when the settings dialog is confirmed;
  no restart is needed.
- The light theme is a real light palette now, and the "follow system" option
  uses the color scheme of the desktop. When the desktop switches its color
  scheme while the app runs, a "follow system" theme follows along.
- The colors that were hardcoded for the dark theme follow the theme as well:
  the toolbar icons of the neutral group, the status colors of the download
  table and the segment bar of the download window.
- The tray icon follows the chosen style: the colored browser-integration mark
  or the monochrome mark, tinted so it stays visible on light and dark panels.
- The toolbar keeps its text under the icons, or shows the icons only; the
  button sizes follow the toolbar size.
- New headless test `theme` covers the setting value, the two palettes and
  the "follow system" resolution; the icon test now checks both tray icon
  styles and the settings dialog test covers the Interface tab.

## 0.18.10 — 2026-10-21

Restructure the settings window into five tabs.

- The settings window now has the tabs `General`, `Interface`, `Download`,
  `Proxy/Socks` and `Passwords` (previously `General`, `Proxy/Socks` and
  `Passwords`).
- `Download` holds the download options that used to sit in `General`: max
  connections per download, the "when the first connection starts downloading"
  policy, the bandwidth limit, the default user agent, the download directory
  (with the category folders toggle), the temporary directory and "Get File
  Creation Date From Server".
- `General` keeps the app-wide behavior options: "Show Download Completion
  Dialog" and "Start Application on System Startup".
- `Interface` is a placeholder for now; the appearance options (toolbar size,
  toolbar text, theme, tray icon style) arrive in a later release of this
  batch.
- The window was slightly enlarged (640×540) because the `Download` tab needs
  more room.
- The settings window structure is covered by a new headless test
  (`settings_dialog`), so the tab order and the placement of every control are
  checked automatically.

> This release is part of a batch that is versioned as patch releases of the
> 0.18.x series (0.18.10 → 0.18.13) instead of a MINOR bump, by maintainer
> decision.

## 0.18.9 — 2026-10-21

Keep the download confirmation window at a fixed size.

- The confirmation window now has a default size (640×250) and a minimum size
  (520×230). It no longer grows with the file name or the URL length, so it
  cannot end up wider than the screen.
- The file name is elided in the middle and keeps the full name in its
  tooltip.
- The file size moved to its own `Size` row instead of being shown inline
  with the file name.
- Every description (`File :`, `Size :`, `URL :`, `Category :`, `Location :`)
  is now vertically centered with the widget next to it, so the rows line up
  regardless of the field height.
- All inputs and buttons share one height: the style gives buttons more
  padding than line edits, which made the `Browse...` button and the bottom
  buttons differ in size.

## 0.18.8 — 2026-10-20

Stop pulling a browser and printer stack when installing the `.deb`.

- Removed the `Recommends` on Chromium from the Debian package. Installing
  SwiftDM no longer installs Chromium, `chromium-common`, `libminizip1t64`,
  `libxnvctrl0`, `system-config-printer`, `gir1.2-handy-1`,
  `gir1.2-packagekitglib-1.0`, or `avahi-utils`.
- The browser extension remains optional and browser-agnostic: load it from
  `/usr/share/swiftdm/chromium` in Chrome, Chromium, Edge, Brave, or another
  Chromium-based browser.
- Runtime library dependencies (Qt6, libcurl, SQLite) and the explicit SVG
  plugin dependencies are unchanged.

## 0.18.7 — 2026-10-20

Start silently when SwiftDM is launched at login.

- The XDG autostart entry now runs `swiftdm --autostart`, which starts the app
  in the system tray without showing the main window.
- Existing autostart entries are refreshed when the GUI starts with the setting
  enabled, so entries written by older releases gain the new flag.
- If the desktop has no system tray, the main window is shown as a fallback so
  the running application remains reachable.
- The autostart entry points at the current executable, keeping the silent
  start working after the binary moves.

## 0.18.6 — 2026-10-20

Missing downloaded files stay quiet at startup.

- Starting SwiftDM no longer opens the "Downloaded Files Missing" summary
  dialog. Completed rows whose file is gone still show the amber
  `Complete (file missing)` status and the path tooltip in the download list.
- The `File Not Found` dialog appears only when the user chooses **Open File**
  for a missing download, and still offers **Open File Location**.
- Missing files are still counted in the startup log line.

## 0.18.5 — 2026-10-20

Fix browser and manual downloads whose URL paths contain spaces.

- Browser handoffs preserve fully encoded URLs instead of decoding `%20` back
  into a literal space before libcurl receives the URL.
- Manual URLs are validated and normalized; the network layer also encodes
  unsafe URL bytes and returns a readable message for malformed URLs.
- URL-derived fallback filenames decode percent escapes, so encoded names such
  as `Test%20SwiftDM.zip` are saved as `Test SwiftDM.zip`.
- Regression coverage includes a raw-space URL through probe and engine, URL
  encoding edge cases, and Chromium handoff of a Drive ZIP with spaces.

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
