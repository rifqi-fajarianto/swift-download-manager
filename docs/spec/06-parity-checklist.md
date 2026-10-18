# 06 — Parity checklist

Scope control across releases. "Replicated" = same observable behavior;
"Diverges" = intentional difference (documented); "Dropped" = not implemented.
Status as of 0.3.0 (Milestone 3, first GUI increment).

## v1 — core engine + GUI

| Feature | Status | Notes |
|---|---|---|
| Segmented HTTP/HTTPS downloads | Replicated | libcurl multi, Range segments |
| Pause / resume / restart | Replicated | JSON state + probe validation |
| Resume after app restart | Replicated via CLI | GUI list is in-session; history reload is v1.1 |
| Queue & status table | Replicated (in session) | columns per 01-product.md; no persisted queue yet |
| Categories + category folders | Replicated | tree filter + extension-based routing |
| Per-download bandwidth limit | Replicated | token bucket |
| Speed + remaining time display | Replicated | 250 ms engine progress → 1 Hz effective UI refresh |
| Retry with backoff | Replicated (approximation) | exact original values UNKNOWN (E23) |
| HTTP proxy + CONNECT tunnel | Replicated | via libcurl; settings dialog |
| SOCKS4/SOCKS5 + auth | Replicated | via libcurl; settings dialog |
| Custom headers / cookies passthrough | Replicated | CLI `--header`; GUI passthrough later |
| User-Agent setting + reset | Replicated | default UA string (E24) |
| Download complete dialog | Replicated | toggle in settings |
| Error dialog | Replicated | message format `Error : HTTP/1.1 ...` |
| Tray icon + close-to-tray | Replicated | QSystemTrayIcon; hidden when unavailable |
| Start at login setting | Replicated | XDG autostart (`~/.config/autostart/swiftdm.desktop`) |
| New URL dialog | Replicated | URL field + Download/Cancel; probes first, then a confirmation window (file name + size) before the transfer starts |
| Progress window opens when a transfer starts | Replicated | manual adds (after confirmation) and toolbar Resume; browser adds will skip the confirmation in v2 |
| Download window (progress + segments) | Replicated | Download/Options/Connections tabs, segment bar |
| Browsers dialog (info page) | Replicated (static info) | extensions ship in v2 |
| About dialog | Diverges | no update check, no license tier |
| Update check to vendor server | Dropped | intentionally not implemented |
| Windows registry storage | Diverges | INI in `~/.config/swiftdm` |
| `segments.bin` binary format | Diverges | `segments.json` |
| Hidden/dark custom theming | Diverges | Qt Fusion + dark palette, own icons |

## v1.1 — GUI completion (planned)

| Feature | Status |
|---|---|
| SQLite download history (`downloads` table) + restore list at startup | Planned |
| Properties dialog + per-download log file | Planned |
| Saved-passwords UI (Passwords tab; `auths` table) | Planned |
| Queue scheduling (manual start, limit simultaneous downloads) | Planned |
| "Get File Creation Date From Server" applied to file mtime | Planned (setting saved, not applied) |
| Browser-sent URL passthrough headers (cookies/referer from extension) | Planned with v2 |

## v2 — browser integration

| Feature | Status |
|---|---|
| WebSocket server (loopback, JSON, documented protocol) | Planned |
| Chrome (MV3) + Firefox extension sources in `ext/` | Planned |
| `addDownload`, `showPanel`, `ping` commands | Planned |

## v3 — extra protocols and media

| Feature | Status |
|---|---|
| FTP (passive, REST resume) | Planned |
| HLS/m3u8 (`NeatSocketHlsMaster` equivalent) | Planned |
| MKV/WebM muxing of HLS streams | Planned |
| IE/Opera/Edge-specific integration | Dropped | platform-specific |
| MKV "Basic/Pro" license gates | Dropped | we have no license tiers |

## Verification notes

- Automated verification is CLI/backend only (AGENTS.md): unit tests, a local
  range-capable HTTP test server, resume/checksum integration tests.
- GUI verification is manual by the user with a per-milestone checklist.
