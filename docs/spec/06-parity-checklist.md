# 06 — Parity checklist

Scope control across releases. "Replicated" = same observable behavior;
"Diverges" = intentional difference (documented); "Dropped" = not implemented.
Status as of 0.18.17.

## v1 — core engine + GUI

| Feature | Status | Notes |
|---|---|---|
| Segmented HTTP/HTTPS downloads | Replicated | libcurl multi, Range segments |
| Pause / resume / restart | Replicated | JSON state + probe validation; Pause/Resume toggle in the download window |
| Resume after app restart | Replicated | GUI list restores from SQLite at startup; unfinished rows stay Paused until the user resumes |
| Queue & status table | Replicated | columns per 01-product.md; rows persist across restarts (queue scheduling is v1.1) |
| Categories + category folders | Replicated | tree filter + extension-based routing |
| Per-download bandwidth limit | Replicated | enable toggle + presets; active limit shown in orange. 0.18.2: live changes apply across transfer paths with 500 ms UI debounce; other Options fields require pause. |
| Speed + remaining time display | Replicated | 250 ms engine progress → 1 Hz effective UI refresh |
| Retry with backoff | Replicated (approximation) | exact original values UNKNOWN (E23) |
| HTTP proxy + CONNECT tunnel | Replicated | via libcurl; settings dialog |
| SOCKS4/SOCKS5 + auth | Replicated | via libcurl; settings dialog |
| Custom headers / cookies passthrough | Replicated | CLI `--header`; Chromium extension forwards scoped cookies and same-origin referrer |
| URLs containing spaces or escaped filename bytes | Fixed (0.18.5) | Browser URLs remain fully encoded and the curl layer encodes unsafe bytes; Drive folder ZIP names containing spaces no longer fail URL parsing |
| File names longer than a path component | Fixed (0.18.18) | A name from a long URL path or `Content-Disposition` no longer fails with "File name too long": it is shortened to 240 bytes (extension kept, hash of the original name appended), and the Windows device names are sanitized |
| User-Agent setting + reset | Replicated | default UA string (E24) |
| Download complete dialog | Replicated | toggle in settings; independent top-level window (not attached to the main window) |
| Error dialog | Replicated | message format `Error : HTTP/1.1 ...`; independent top-level window |
| Tray icon + close-to-tray | Replicated | QSystemTrayIcon with the colored SwiftDM mark; hidden when unavailable. The General tab can switch the tray icon off and can turn the close-to-tray behavior off, in which case closing the main window ends the app (with a confirmation) (0.18.13) |
| Explicit Quit confirmation | Implemented | Toolbar and tray-menu Quit show a confirmation defaulting to Cancel; confirming closes the app, and active downloads are paused for resume on next launch (0.18.4) |
| Start at login setting | Replicated | XDG autostart (`~/.config/autostart/swiftdm.desktop`) runs `swiftdm --autostart`; the app starts in the tray without showing the main window (0.18.7), with a visible-window fallback when no tray is available |
| New URL dialog | Replicated | URL field + Download/Cancel; probes first, then one Download Confirmation window (file name + size + Category + Location) before the transfer starts (0.18.0) |
| Progress window opens when a transfer starts | Replicated | manual adds and browser handoff (both after confirmation), plus toolbar Resume |
| Download confirmation window | Implemented | shown for manual and browser-sent downloads; independent of main-window visibility; does not restore a hidden/minimized main window; Download Now / Add to queue (placeholder) / Cancel; Cancel creates no row and no database entry; fixed size (640×250, minimum 520×230) with the file name elided in the middle and the size on its own `Size` row; the row descriptions are vertically centered with their widgets and all inputs and buttons share one height (0.18.9). The General tab can switch the window off; the download then starts right away with the default category and folder (0.18.17) |
| Download window (progress + segments) | Replicated | Download/Options/Connections tabs, segment bar |
| Item context menu (Open File / Open File Location / Properties) | Replicated | right-click on a row; original shows it on the list's `NM_RCLICK` (E25) |
| Properties dialog (read-only) | Replicated (partial) | fields per E18; per-download log file and editable URL are v1.1 |
| Delete File (remove the downloaded file from disk) | Added (not in the original) | toolbar button + warning dialog; drops the list entry too |
| Extensions dialog | Implemented | Shows local integration server status and Chromium development setup |
| About dialog | Diverges | no update check, no license tier |
| Update check to vendor server | Dropped | intentionally not implemented |
| Windows registry storage | Diverges | INI in `~/.config/swiftdm` |
| `segments.bin` binary format | Diverges | `segments.json` |
| Hidden/dark custom theming | Diverges | Qt Fusion + own icons; the palette follows the Interface tab (light, dark, or follow the system) instead of a custom hidden theme (0.18.12) |
| Toolbar size / toolbar text | Implemented | Interface tab: `Normal` or `Small` toolbar, with or without the text under the icons; applied without a restart (0.18.12) |
| Tray icon style | Implemented | Interface tab: the colored browser-integration mark or the monochrome mark, tinted for the current theme (0.18.12) |
| Message-box icons | Diverges | the message boxes use the app's own filled glyphs (info, alert, question, completion) instead of the built-in platform icons; only the icon area changes, the title bar keeps the app icon (0.18.16) |

## v1.1 — GUI completion (planned)

| Feature | Status |
|---|---|
| SQLite download history (`downloads` table) + restore list at startup | Implemented | Rows (incl. `pageurl`/`pagetitle`, POST data, headers, `done`) are written/updated via `src/store/history.cpp`; `MainWindow::restore_history()` rebuilds Complete/Paused/Error rows at startup, and deletes remove rows from `downloads`+`headers`. Missing completed files are flagged in the list (0.17.0); the startup summary dialog was removed in 0.18.6, so the warning appears only on Open File |
| Per-download log file (Properties "Log File") + editable URL field | Planned |
| Saved-passwords UI (Passwords tab; `auths` table) | Planned |
| Queue scheduling (manual start, limit simultaneous downloads) | Planned |
| "Get File Creation Date From Server" applied to file mtime | Planned (setting saved, not applied) |
| Firefox extension | Planned |

## v2 — browser integration

| Feature | Status |
|---|---|
| WebSocket server (loopback, JSON, documented protocol) | Implemented | Well-formed Chromium extension origin and subprotocol validation; rejected handshakes are logged; `ping`, `getVersion`, `showPanel`, `addDownload`, `subscribe` (status push) |
| Chromium (MV3) extension source in `ext/chromium/` | Development preview | Popup (connection pill with the app version, auto-intercept switch, browser cookies switch, detected media, download button, retry) styled after `ext/extension-ui-template.html`, explicit link/page context menu, automatic download handoff, POST body and `x-*` header capture; cookie permission is optional and granted on a user gesture |
| Automatic browser-download interception | Implemented (preview) | Cancels the browser item, hands the URL (plus observed body/headers) to SwiftDM, and restarts the browser download if SwiftDM is unreachable |
| Media detection in the popup | Implemented (preview) | Detected HLS playlists and same-tab video/audio pairs are listed in the popup; a pair is handed off with a shared `group` token |
| Firefox extension | Planned |
| Browser-sent page metadata | Implemented | `pageUrl`/title are sent by the extension, carried on the item, shown in Properties, and persisted in the SQLite `downloads` row (`pageurl`/`pagetitle`); restored at startup since 0.17.0 |

Open gaps identified by the original-extension comparison
(`07-chromium-extension-reference.md` §6, decision log in §10): POST/body and
custom-header handoff (C1), an app→extension status channel (C2), the
interception-off switch (C3), the strict cookie model (C13), and persisted page
metadata (C12)
are implemented as of 0.10.0; FTP (C11), HLS (C7), and adaptive pairing (C8)
landed in 0.11.0, with MKV muxing left as a documented proposal
(`03-engine.md` §9). The in-page media panel and site extractors (C4–C6, C9)
are still uncommitted.

## v3 — extra protocols and media

| Feature | Status |
|---|---|
| FTP (passive, REST resume) | Implemented | `ftp://`/`ftps://` probe (`SIZE`), single connection, `REST` resume, validated protocol handoff |
| HLS/m3u8 (`NeatSocketHlsMaster` equivalent) | Implemented | master/media playlists, highest-bandwidth variant, `EXT-X-MAP`, sequential segments concatenated to `.ts`, per-segment resume; encrypted playlists are refused |
| MKV/WebM muxing of HLS streams | Proposal | `03-engine.md` §9: paired tracks are stored with a `groupid`; ffmpeg `-c copy` remux (hand-written EBML fallback) is the planned v3 step |
| IE/Opera/Edge-specific integration | Dropped | platform-specific |
| MKV "Basic/Pro" license gates | Dropped | we have no license tiers |

## v3.1 — refusals that explain themselves (0.12.0)

Pixeldrain (and other hosts) intentionally refuse direct downloads: their FAQ
documents "hotlink protection" that detects download managers, answers `403`
(`hotlink_detected`, `file_rate_limited_captcha_required`,
`max_concurrent_downloads`) and requires a CAPTCHA on the file's page. SwiftDM
does not circumvent that gate:

| Behavior | Status |
|---|---|
| Show the host's reason | Implemented | 4xx/5xx bodies are summarized into the error text (`value: message`, HTML title/text) |
| Refused bodies in the partial file | Fixed | 3xx/4xx/5xx bodies are never written at the segment offset |
| Parallel `403` handling | Implemented | one retry of the remaining ranges on a single connection (1 s pause) before failing |
| `Open Page in Browser` in the error dialog | Implemented | offered when the download came from a browser page, so the user can clear the host's CAPTCHA and retry |
| CAPTCHA solving / hotlink circumvention | Dropped | out of scope: the host asks for a human step, the app points at the page |

## v3.2 — links that are pages, not files (0.13.0)

Google Drive answered different link shapes in different ways: sometimes the
file arrived, sometimes a 2 KB HTML page was stored under a name taken from the
URL (`download`, `view`, or a Drive folder id).

| Behavior | Status |
|---|---|
| Viewer/file links (`/file/d/<id>[/view]`, `/open?id=`, `/uc?id=`, `/drive/u/<n>/…`) | Implemented | normalised to `drive.usercontent.google.com/download?id=<id>&export=download`, keeping `authuser`/`resourcekey`/`confirm` (`uuid` dropped); ZIP paths with spaces are percent-encoded before libcurl |
| Virus-scan notice above Google's scan limit | Implemented | `confirm=t` submitted transparently, exactly one step; `uuid` dropped so the URL stays deterministic |
| Transfers follow the resolved URL | Implemented | `ProbeResult::download_url`; resume state keeps matching because the resolved URL is stable across runs and link shapes |
| Drive folder links | Implemented | refused before any request, with the reason |
| Sign-in walls (private or removed files, `accounts.google.com`) | Implemented | refused with the reason instead of saving a page; also covers the browser's own `confirm=t` URLs fetched without the account session; the message names the shared-link and browser-session ways out |
| Signed-in retry from the browser | Implemented | the extension popup's "Send browser cookies" switch asks for the optional cookie permission on a user gesture, so the browser session travels with the handoff; automatic interception reuses the grant and never prompts |
| Google Docs editor links | Implemented | refused with guidance (`File > Download` / export link) |
| Plain HTML pages from other hosts | Unchanged | still downloadable; only a page carrying the confirmation form is auto-submitted |
| CAPTCHA / account-only gates | Not bypassed | same policy as v3.1: the user clears them on the host's page |

## Verification notes

- Automated verification is CLI/backend only (AGENTS.md): unit tests, a local
  range-capable HTTP test server (including JSON/HTML refusal fixtures and a
  connection-limited route), resume/checksum integration tests.
- FTP and HLS are verified by the new `media` test: a local FTP server (control
  connection plus passive data, `SIZE`/`REST`/`EPSV`) checks the probe, a full
  download, and a `REST`-based resume from a pre-seeded part file; HLS fixtures
  served by the HTTP test server check master-variant selection, segment
  concatenation, mid-download pause plus resume, and the encrypted-playlist
  refusal. The extension-side pairing logic runs in the Node `vm` harness
  (`extension_logic`) with mocked `webRequest`/`tabs` APIs.
- GUI verification is manual by the user with a per-milestone checklist.
