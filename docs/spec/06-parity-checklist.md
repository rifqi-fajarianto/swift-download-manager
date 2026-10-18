# 06 — Parity checklist

Scope control across releases. "Replicated" = same observable behavior;
"Diverges" = intentional difference (documented); "Dropped" = not implemented.

## v1 — core engine + GUI (this milestone)

| Feature | Status | Notes |
|---|---|---|
| Segmented HTTP/HTTPS downloads | Replicated | libcurl multi, Range segments |
| Pause / resume / restart | Replicated | JSON state + probe validation |
| Resume after app restart | Replicated | state file + DB |
| Queue & status table | Replicated | columns per 01-product.md |
| Categories + category folders | Replicated | extension-based routing |
| Per-download bandwidth limit | Replicated | token bucket |
| Speed + remaining time display | Replicated | 1 Hz refresh |
| Retry with backoff | Replicated (approximation) | exact original values UNKNOWN |
| HTTP proxy + CONNECT tunnel | Replicated | via libcurl |
| SOCKS4/SOCKS5 + auth | Replicated | via libcurl |
| Basic/Digest auth + saved passwords | Replicated | Digest via libcurl; NTLM optional later |
| Custom headers / cookies passthrough | Replicated | headers table |
| User-Agent setting + reset | Replicated | default UA string |
| Download complete dialog | Replicated | toggle in settings |
| Properties dialog + log file | Replicated | per-download log |
| Error dialog | Replicated | message format `Error : HTTP/1.1 ...` |
| Tray icon + close-to-tray | Replicated | StatusNotifier/AppIndicator |
| Start at login setting | Replicated | XDG autostart |
| New URL dialog | Replicated | |
| Browsers dialog (info page) | Replicated (static info) | extensions ship in v2 |
| About dialog | Diverges | no update check, no license tier |
| Update check to vendor server | Dropped | intentionally not implemented |
| Windows registry storage | Diverges | INI in `~/.config/swiftdm` |
| `segments.bin` binary format | Diverges | `segments.json` |
| Hidden/dark custom theming | Diverges | Qt Fusion + dark palette, own icons |

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
