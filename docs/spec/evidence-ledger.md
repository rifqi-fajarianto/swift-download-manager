# Evidence ledger

Every claim in the spec documents maps to a row here. REA Evidence IDs refer to
the analysis session on `NeatDM.exe` (SHA-256
`60b06db7dfeb6fffb1be82f8ad059d61bdb1b1a3889439b56eaac162e64c0f37`, Ghidra
12.1.4 provider). "static" rows are file-offset observations from local static
tooling (strings/PE parsing), not REA output.

| # | Claim | Evidence |
|---|---|---|
| E1 | Binary is native PE32 (i386), MSVC 2022 linker 14.31, static CRT, non-.NET | `inspect_managed_artifact` (E1a: cli-directory-absent); static PE headers |
| E2 | UI classes: `NeatMainWindow`, `NeatDownloadWindow(MKV)`, `NeatSettingWindow`, `NeatPropertiesWindow`, `NeatAuthWindow`, `NeatUrlWindow`, `NeatCompleteWindow`, `NeatErrorWindow`, custom controls | static RTTI strings `. ?AV...` |
| E3 | Engine classes: `NeatDownloadEngine`, `NeatSegmentManager`, `NeatSegment`, sockets `NeatSocket/Http/Ftp/HlsMaster`, `NeatWebSocket*` | static RTTI strings |
| E4 | SQLite schema `downloads`/`headers`/`auths` | static, file offset 0x17c797; REA `search_strings` "CREATE TABLE \"downloads\"" |
| E5 | DB file name `NeatDB.db` | static wide strings |
| E6 | Settings root `HKCU\Software\NeatDM` (RegOpenKeyExW/RegCreateKeyExW) | REA ev_b7b3b5a84e894b227ee4c8fe6a86b8e4e75dd4115606baa8fa1a40fd1c215361 (FUN_004eb140) |
| E7 | Setting types: DWORD getter returns -1 when missing (FUN_004eb5d0); string getter (FUN_004eb810) | REA ev_1930cfe6835cccfd0af7dd06db4d531a8dc2d7c4dabcf820656b957867388ca8, ev_8c444bf56cae005e938776830ea51b6a46037f41cec24e2261f3dc28796dc8c2 |
| E8 | Settings keys + defaults seen in dialog init: `MaxConnections`(d), `ConnectionsAtOnce`(0), `BandWidthLimit`(0), `UseUAgent`(2), `CategoryFolders`(2), `ServerCreationDate`(2), `CompletionDialog`(0), `AppAutoStart`(0), `DownloadDirectory`(s), `TempDirectory`(s), `DefaultAgent`(s), `LastDownloadID`, `SocksVersion` | REA ev_a169ffe8bace7dae8a4791bacc84be707288a6755a1202536c192180bd1bd671 (FUN_00510530), plus string hits ev_b37190b6..., ev_da1fd429..., ev_04f16748... |
| E9 | Browser panel keys: `ChromePanel`, `EdgePanel`, `FoxPanel`, `IEUser` (ShowPanel* wide strings) | static wide strings |
| E10 | WebSocket server: thread logs "Starting WebSocketServer...", binds `127.0.0.1`, port constant `0x2717` = **10007**, htons via ws2_32 ordinal | REA ev_4e392467db9d75eb6458ee03995957600e5ef5da022ead5064757694d5efe03e (FUN_004e25c0); string ev_0538e82e...; xref ev_e86be960... |
| E11 | WebSocket handshake response: `Upgrade: websocket`, `Connection: Upgrade`, `Sec-WebSocket-Accept: <key>`, `Sec-WebSocket-Protocol: neatextension.v1` | REA ev_974980d174785bfdf13388c8f4583f90d4d34b026f86607f8ad18789e5d24c79 (FUN_004e0eb0); strings ev_d4113e28..., ev_f0455541... |
| E12 | HTTP request builder appends `Range: bytes=` conditionally, custom headers, cookie/extra headers | REA ev_69257d35c736781c4b1fc6c98404d1c27fb31b20d4d803940fba84717badd49d (FUN_004d69f0); string ev_bcb2a837... |
| E13 | Engine retry: counter at engine+0x5b4, log "RemainedRetryCount = N and Engine is going to try again." | REA ev_50841d72aa9fd7c780f2c3a0e0759780f33689ffa06ce52649233ef0f156b44c (FUN_004bfd60); string ev_55747112... |
| E14 | Segment state persisted to `segments.bin`, backup `segments.bak`; per-segment temp files with `seg.x` suffix; loader logs " Segments were loaded from segments.bin file." | REA ev_782aba2e..., xrefs ev_1b9da527..., ev_86b686c4..., ev_0b21ba9a...; loaders ev_fe1bd4c9... (FUN_004cf3c0), ev_9cee90f4... (FUN_004cf120); path builder ev_35b07a00... (FUN_004ca400), seg-manager ctor ev_163bed45... (FUN_004ca590) |
| E15 | Chunked transfer support ("TRANSFER_TYPE_CHUNKED"), redirects ("Redirect Location Detected By Socket"), proxy CONNECT tunnel, SOCKS4/5 errors, Basic/Digest/NTLM classes `NeatAuthBasic/Digest/NTLM` | static strings; RTTI E3 |
| E16 | TLS via Schannel (Secur32/bcrypt imports, "SSLHandShake" strings); hashing via bcrypt | static imports; static strings |
| E17 | HLS: m3u8 MIME types, TS segment handling, MKV/WebM mux (`NeatMKVMuxer`, `NeatWebmReader`), default names `Video.webm`/`Audio.webm` | static strings; RTTI E3 |
| E18 | UI/UX: main window toolbar (New URL, Resume, Stop, Delete, Settings, Browsers, About, Quit), category tree, columns (File Name, Size, Status, Bandwidth, Remaining Time, Last Try), download window tabs (Download/Options/Connections), settings tabs (General/Proxy-Socks/Passwords), properties/complete/error dialogs | screenshots `binary-original/*.png` (observation) |
| E19 | Categories: Video, Audio, Compressed, Document, Application, Misc; views All Downloads, Complete, Incomplete | screenshots E18; static strings |
| E20 | Update check URL `http://www.neatdownloadmanager.com/WinVersion.txt` — **not replicated** | static strings |
| E21 | FTP support (`NeatSocketFtp`, `FTP_STATE_*` strings) | static strings; RTTI E3 |
| E22 | Temp path default `%APPDATA%\Roaming\NeatDM\` (per Properties dialog) | screenshot E18 |
| E23 | Min chunk sizing, exact segment split formula, exact retry counts/timeouts, exact bandwidth-limiting mechanism | UNKNOWN — further RE optional; SwiftDM uses documented design values (spec 03-engine.md) |
| E24 | Default User-Agent string `Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/107.0.0.0 Safari/537.36` | REA `search_strings` "Mozilla/5.0" @ `0x565138` |
| E25 | Main-window item context menu: `Resume`, `Stop`, `Redownload`, `Show Window`, separator, `Open Folder`, `Open`, `Delete`, separator, `Properties`; built in the main-window constructor, shown on the list's `NM_RCLICK` (-5) at the click point; `NM_DBLCLK` (-3) opens the per-download window | REA `procedure_pseudo_code` FUN_00504490 (menu build + `AppendMenuW` sites), FUN_00505240 (NM_RCLICK dispatch; `TrackPopupMenu` call 0x50535b), xrefs 0x504bcd / 0x504c20 / 0x504c5c / 0x504c91 |
| E26 | Tray icon: `NOTIFYICONDATAW` with callback message `0x40f`; tray menu holds `NeatDownloadManager Main Window` plus dynamically added download entries; left/right button up (`0x202`/`0x205`) shows the menu when it has more than 2 items, left double-click (`0x203`) restores the main window | REA `procedure_pseudo_code` FUN_00504490 (`Shell_NotifyIconW`, tray menu at object+0xc4), FUN_00506960 (callback handler) |
| E27 | Original Chromium extension package: MV3 v1.9.91, Edge-store `update_url`, fixed `key`, permissions `webRequest`/`webNavigation`/`cookies`/`contextMenus`/`storage`/`downloads`, `<all_urls>`, content script in all frames at `document_start`, action without popup | `binary-original/original-chromium-extension/1.9.91_0/manifest.json` (read-only bundle) |
| E28 | Original worker wiring: seven observed webRequest resource types on http/https/ftp; one link+image context menu; toolbar click toggles the catcher (badge `Off`, on by default); storage key `ShowMediaPanel` | `bg.js:1,12-14` |
| E29 | Original browser-copy cancellation: `downloads.onCreated` cancels+erases only when the item's `url`/`finalUrl` equals the URL just handed off; bypass/catcher-off leaves downloads alone; no browser fallback when the app is unreachable | `bg.js:15,36`; failure alert `ct.js:33` |
| E30 | Original wire format: numeric fields `1:`–`11:`, raw `Origin:`/`Referer:`/`Cookie:`/`x-*` lines, `__0NeatPostData9__:<body>`, `Content-Length: 0` for an empty POST, 118,784-byte cap, socket `ws://127.0.0.1:10007/download`, subprotocol `neatextension.v1` | `bg.js:16-19` |
| E31 | App sends `waiting`, `WinVersion1424`, and `ShowPanelChrome/Fox/Edge=0/1` on client setup; per-browser panel flags live in app settings | REA ev_5d3eb5a31b4a0a63df4d6dd5382cd9a5796796eb47421f81217ccea25b68779a (FUN_004e17d0); string ev_245728890bae2f1607d3cbf9a8f48bd83420480b845f5067b5943f14f01e9cb2 (`ShowPanelEdge`), ev_f57d9cc2eaea92a898377c3b80a33a79d4babc5eb36bb2e70bd8f1fb8034fa35 (`ShowPanelChrome`) |
| E32 | App sends `nowaiting` when its active-download count reaches zero | REA ev_c153187a2bc76a973b4157e52d755bef0dcd63e34e1277e0b4dfc07e9dbdd496 (FUN_005079e0) |
| E33 | App answers extension pings with pong | REA ev_931b105b810ba6e29ac21e058eda94c17e8eb4ad0539629c2f7386d3ff2a6edd (FUN_004e1c80) |
| E34 | App parses the extension text message; recognised literals include `normal`, `BandWidthLimit`, `Content-Length: `, `User-Agent:`, `Referer: `, `__0NeatPostData9__:` | REA ev_b866878c76d45c39fdf4922e9bec223b5754e12aab90fb5c4b4e19ab13ffb4c1 (FUN_00509d70); string ev_00ff4ccb76293c1140f9ffdc85e47ca54ecc70727469f6d2b66425e0b870cd69 |
| E35 | No origin pinning found in the app: no `chrome-extension://` or `Origin:` string, and the handshake builder (FUN_004e0eb0) emits the RFC6455 response without an origin check | REA ev_cd46393fdf47aed81eeffc848999cdd540ba1010a129f4cc67ed91f351a21dee (absent), ev_d68e42911f7306f1c7b95b5bb34aceb186ba8f76fdbc75539f1d845e1ea620a4 (absent), ev_377f49673eca95ad20452100cff86dbfba24514f2d4edd00ae7bf8fea26f8476 (FUN_004e0eb0); bounded negative — a string search cannot rule out a computed check |
| E36 | Original classifier constants: media/web extension lists, TS/video lists, octet-stream MIME list, exclusions (`manifest`, `favicon.ico`, `pem.msg`, `.wasm`, `.json`), size gates (200 KiB media, 1 MiB ASF, 300 KiB TXT/JS XHR), MIME→extension table (~100 entries) | `bg.js:1-7,29-37` |
| E37 | Original content script: all-frame content script; long-lived `runtime.connect({name:"neat"})` port; draggable in-page media panel; YouTube/Vimeo/Facebook extractors; HLS list building in the worker; VTT/SRT labels; Delete-key bypass | `ct.js:1-35`; `bg.js:20-28,41-43` |

## Notes

- Evidence for E4–E5, E9, E15–E17, E19–E21 comes from strings/PE parsing of the
  shipped binary; string presence is an observation about the artifact, not
  proof of a specific runtime path.
- E27–E37 are static observations of the shipped Chromium extension bundle
  (`binary-original/`, read-only) plus a fresh REA session on the same
  `NeatDM.exe` digest as E1–E26. Neither the application nor a browser was
  executed for these rows; `bg.js`/`ct.js` are minified, so line numbers point
  to statement groups.
- "UNKNOWN" rows must not be guessed in implementation; they get a documented
  design decision in the spec instead.
