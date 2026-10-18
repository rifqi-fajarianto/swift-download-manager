# Changelog

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
