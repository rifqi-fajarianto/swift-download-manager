# Changelog

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
