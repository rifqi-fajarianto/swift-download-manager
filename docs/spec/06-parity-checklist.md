# 06 — Parity checklist

Scope control across releases. "Replicated" = same observable behavior;
"Diverges" = intentional difference (documented); "Dropped" = not implemented.
Status as of 0.9.3.

## v1 — core engine + GUI

| Feature | Status | Notes |
|---|---|---|
| Segmented HTTP/HTTPS downloads | Replicated | libcurl multi, Range segments |
| Pause / resume / restart | Replicated | JSON state + probe validation; Pause/Resume toggle in the download window |
| Resume after app restart | Replicated via CLI | GUI list is in-session; history reload is v1.1 |
| Queue & status table | Replicated (in session) | columns per 01-product.md; no persisted queue yet |
| Categories + category folders | Replicated | tree filter + extension-based routing |
| Per-download bandwidth limit | Replicated | token bucket |
| Speed + remaining time display | Replicated | 250 ms engine progress → 1 Hz effective UI refresh |
| Retry with backoff | Replicated (approximation) | exact original values UNKNOWN (E23) |
| HTTP proxy + CONNECT tunnel | Replicated | via libcurl; settings dialog |
| SOCKS4/SOCKS5 + auth | Replicated | via libcurl; settings dialog |
| Custom headers / cookies passthrough | Replicated | CLI `--header`; Chromium extension forwards scoped cookies and same-origin referrer |
| User-Agent setting + reset | Replicated | default UA string (E24) |
| Download complete dialog | Replicated | toggle in settings; independent top-level window (not attached to the main window) |
| Error dialog | Replicated | message format `Error : HTTP/1.1 ...`; independent top-level window |
| Tray icon + close-to-tray | Replicated | QSystemTrayIcon; hidden when unavailable |
| Start at login setting | Replicated | XDG autostart (`~/.config/autostart/swiftdm.desktop`) |
| New URL dialog | Replicated | URL field + Download/Cancel; probes first, then a confirmation window (file name + size) before the transfer starts |
| Progress window opens when a transfer starts | Replicated | manual adds (after confirmation), toolbar Resume, and browser handoff (skips the confirmation) |
| Download window (progress + segments) | Replicated | Download/Options/Connections tabs, segment bar |
| Item context menu (Open File / Open File Location / Properties) | Replicated | right-click on a row; original shows it on the list's `NM_RCLICK` (E25) |
| Properties dialog (read-only) | Replicated (partial) | fields per E18; per-download log file and editable URL are v1.1 |
| Delete File (remove the downloaded file from disk) | Added (not in the original) | toolbar button + warning dialog; drops the list entry too |
| Extensions dialog | Implemented | Shows local integration server status and Chromium development setup |
| About dialog | Diverges | no update check, no license tier |
| Update check to vendor server | Dropped | intentionally not implemented |
| Windows registry storage | Diverges | INI in `~/.config/swiftdm` |
| `segments.bin` binary format | Diverges | `segments.json` |
| Hidden/dark custom theming | Diverges | Qt Fusion + dark palette, own icons |

## v1.1 — GUI completion (planned)

| Feature | Status |
|---|---|
| SQLite download history (`downloads` table) + restore list at startup | Planned |
| Per-download log file (Properties "Log File") + editable URL field | Planned |
| Saved-passwords UI (Passwords tab; `auths` table) | Planned |
| Queue scheduling (manual start, limit simultaneous downloads) | Planned |
| "Get File Creation Date From Server" applied to file mtime | Planned (setting saved, not applied) |
| Firefox extension | Planned |

## v2 — browser integration

| Feature | Status |
|---|---|
| WebSocket server (loopback, JSON, documented protocol) | Implemented | Well-formed Chromium extension origin and subprotocol validation; rejected handshakes are logged; `ping`, `getVersion`, `showPanel`, `addDownload` |
| Chromium (MV3) extension source in `ext/chromium/` | Development preview | Popup, explicit link/page context menu, and automatic download handoff; cookie permission is optional and per-origin |
| Automatic browser-download interception | Implemented (preview) | Cancels the browser item, hands the URL to SwiftDM, and restarts the browser download if SwiftDM is unreachable |
| Firefox extension | Planned |
| Browser-sent page metadata | Partial | `pageUrl` and title sent by the extension; download history does not yet persist these fields |

Open gaps identified by the original-extension comparison
(`07-chromium-extension-reference.md` §6, decision pending): POST/body and
custom-header handoff (C1), an app→extension status channel (C2), a quick
bypass (C3), an in-page media panel with media detection/HLS/site extractors
(C4–C9), size enrichment (C10), FTP handoff (C11), persisted page metadata
(C12), and store packaging (C14). None of these are committed to a milestone.

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
