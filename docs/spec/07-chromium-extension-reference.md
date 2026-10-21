# 07 — Chromium browser-extension reference

Implementation reference for the browser-integration workstream. It records
(a) how the shipped original extension works together with the original Windows
application, (b) what our development extension does today, (c) the differences,
and (d) candidate work items.

**Status: reference only — no item below is committed to the roadmap.** The
feature owner selects candidates in §6 before any implementation starts; a
selected item then follows the normal build/version rules in `AGENTS.md`.
Nothing in this document overrides `04-protocols.md`; our protocol stays
`swiftdm.v1` unless a decision recorded here says otherwise.

## 1. Artifacts and method

| Item | Value |
|---|---|
| Original extension | `binary-original/original-chromium-extension/1.9.91_0/` — *NeatDownloadManager Extension* v1.9.91 (Manifest V3), Edge store build; author "Javad Motallebi" |
| Files | `manifest.json`; `bg.js` (service worker, 19.7 KB minified); `ct.js` (content script, 17 KB minified); `img/`; `_metadata/` (store hashes) |
| Original application | `binary-original/original-windows-binary/NeatDM.exe`, SHA-256 `60b06db7dfeb6fffb1be82f8ad059d61bdb1b1a3889439b56eaac162e64c0f37` |
| Our extension | `ext/chromium/` (v0.2.0) — see `ext/chromium/README.md` |
| Our app side | `src/ui/browser_integration.cpp` (`swiftdm.v1`, JSON envelope) |
| Evidence | E27–E37 in `evidence-ledger.md` (extension source lines + REA IDs) |

The original extension files ship as readable minified JavaScript, so the
extension behavior is described from its own source, not from decompilation.
The application side was verified with REA (Ghidra) on the same binary digest as
E1–E26. **Neither side was executed for this analysis**; behavior marked
UNKNOWN is not reconstructed by guesswork. Line references such as `bg.js:15`
point to statement groups inside the minified file.

## 2. Original extension: package facts (E27)

| Aspect | Observed value |
|---|---|
| Manifest version / extension version | MV3 / 1.9.91 |
| Name / description | *NeatDownloadManager Extension* / "Sends Download Links to Neat Download Manager" |
| `permissions` | `webRequest`, `webNavigation`, `cookies`, `contextMenus`, `storage`, `downloads` |
| `host_permissions` | `<all_urls>` |
| `content_scripts` | `ct.js`, `matches` http/https, `all_frames: true`, `run_at: document_start` |
| Action | icon only, **no `default_popup`**; `chrome.action.onClicked` toggles the catcher (`bg.js:13`) |
| Background | `service_worker: bg.js` |
| `web_accessible_resources` | panel icons only (`icon16(.@2x)`, `close16(.@2x)`) |
| Distribution | `update_url` = Edge store; fixed `key` (stable extension ID) |
| Cookies permission | granted up front together with `<all_urls>` (no per-origin prompt) |

## 3. Original extension: behavior

### 3.1 Traffic observation (E28)

The worker observes requests for seven resource types
(`object`, `xmlhttprequest`, `media`, `other`, `main_frame`, `sub_frame`,
`image`) on `http://*/*`, `https://*/*`, and `ftp://*/*` (`bg.js:1`, `bg.js:12`):

- `onBeforeRequest` with `requestBody` — creates a per-request record, keeps POST
  bodies for later reconstruction, and hands `ftp://` URLs straight to the app
  when the path is non-empty (`bg.js:39`).
- `onBeforeSendHeaders` — stores request headers (used for POST rebuilding,
  `Content-Type`/`Content-Disposition`, and `x-*` custom headers) (`bg.js:38-39`).
- `onHeadersReceived` — the classifier (see below) (`bg.js:29-37`).
- `onCompleted` / `onErrorOccurred` — drops per-request records (`bg.js:24`).
- `onHistoryStateUpdated` — notifies the content script of SPA navigation so the
  page can be re-scanned (`bg.js:14`).

Records are dropped when the status is not 200/206, when a 0-byte body is
reported, and while a redirect (301/302/303/307/308) is in flight; redirects
continue the record into the next response (E36).

### 3.2 Classification (E36)

The classifier maps MIME types to file extensions (≈100-entry table at
`bg.js:2-7`; e.g. `application/zip → ZIP`, `video/x-matroska → MKV`,
`application/vnd.apple.mpegurl → M3U8`), then combines it with the URL
extension, the `Content-Disposition` filename, `Content-Type`, size, and
resource type. Observed lists and thresholds:

| Constant | Observed values |
|---|---|
| Media extensions (`u`) | FLV SWF MP3 MP4 M4V F4F F4V M4A MPG MPEG MPEG4 MPE AVI WMV WMA WAV WAVE ASF RM RAM OGG OGV OGM OGA MOV MID MIDI 3GP 3GPP QT WEBM TS MKV AAC MP2T MPEGTS RMVB VTT SRT |
| "Page/asset" extensions (`fa`, never caught) | HTM HTML MHT MHTML SHTML SHTM XHT XHTM XHTML XML TXT CSS JS JSON GIF ICO JPEG JPG PNG WEBP BMP SVG TIF TIFF PDF PHP ASP ASPX EOT TTF WOF WOFF WOFF2 MSG CHN PEM BR OTF ACZ AZC CGI TPL OSD M3U8 DO |
| Video-ish extensions for background types (`ha`) | FLV AVI MPG MPE WMV QT MOV RM RAM WMA MID MIDI AAC MKV RMVB |
| TS variants that skip the "main-frame video" path (`C`) | F4F MPEGTS TS MP2T |
| Octet-stream-ish MIME (`ea`) | application/x-apple-diskimage, application/download, application/force-download, application/x-msdownload, binary/octet-stream |
| Always ignored URL parts | `manifest`, `favicon.ico`, `pem.msg`, `.wasm`, `.json` |
| Size gates | media candidate requires unknown size, > 200 KiB, or `attachment`; ASF skipped when ≤ 1 MiB; TXT/JS XHR responses ≤ 300 KiB are not probed for `m3u8`; `Server: DCLK-AdSvr` skipped |

Main-frame navigations to a media-extension URL that match the active tab are
closed when the tab was opened from another tab, and handed off with the
`media` type hint (`bg.js:36-37`); other requests that pass the classifier are
auto-handed to the application.

### 3.3 Media path: the in-page panel (E37)

Media candidates are **not** sent to the application automatically. They are
posted to the page's content script, which renders a draggable overlay panel
listing the items with quality/format labels (`ct.js:4-15`); the user picks an
item and only then is it handed to the app (`ct.js:24`, `bg.js:42-43`).

Page integration observed in `ct.js`:

- One long-lived `chrome.runtime.connect({name: "neat"})` port per frame; panel
  items are per `(tabId, frameId)` (`ct.js:16`).
- Keyboard bypass: `Delete` keydown/keyup is reported to the worker, which then
  ignores catches while the key is held (`ct.js:17,34`, `bg.js:12,15`).
- Media element anchoring: the panel is positioned near the clicked
  `VIDEO/AUDIO/OBJECT/EMBED`, with fallbacks to the element under the cursor or
  the largest visible element (`ct.js:25-28`).
- Site extractors: YouTube inline player-response JSON (`formats` /
  `adaptiveFormats`, itag→quality map, `signature`/`sig` requirement) and
  adaptive video+audio pairing into an "MKV File …" proposal (`ct.js:20-23,29-32`);
  Vimeo progressive JSON; Facebook `/videos/` link scanning through the embed
  page (`ct.js:18-19`).
- HLS: the worker parses `m3u8` playlists (`EXTINF`, `EXT-X-STREAM-INF`,
  `EXT-X-BYTERANGE`) into stream candidates and byte-range information
  (`bg.js:21-24`).
- Subtitles: VTT/SRT items get explicit labels and are forced to the `normal`
  path instead of the media panel path (`ct.js:2,24`, `bg.js:42`).

### 3.4 File path: automatic handoff and browser-copy cancellation (E29)

The extension cannot block requests (MV3 without `webRequestBlocking`), so the
handoff is two-step:

1. `onHeadersReceived` selects a URL as a download and the worker builds a
   message for the application (`bg.js:36`).
2. The browser continues its own download; when `chrome.downloads.onCreated`
   fires, the worker cancels and erases the item **only when its `url` or
   `finalUrl` equals the URL that was just handed off** (`bg.js:15,36`).

Consequences observed in the source: unrelated browser downloads are untouched;
if the application is unreachable the message stays queued and, on socket error,
the content script shows a blocking alert telling the user to start the app,
hold `Delete`, or disable the extension (`bg.js:19`, `ct.js:33`). There is **no
browser-download fallback**.

### 3.5 User controls (E28)

| Control | Behavior |
|---|---|
| Toolbar icon | toggles the catcher; badge `Off` + title "Download catcher is Off" while disabled; default is on (`bg.js:12-13`) |
| Context menu | one item "Download by NeatDownloadManager" for `link` and `image`; handler validates http/https/ftp (`bg.js:12,20`) |
| Delete key | held = bypass catching, also disables browser-copy cancellation (`ct.js:17,34`, `bg.js:15`) |
| In-page panel | per-tab overlay, add/close/multi-item list, drag to move, auto-fade when idle (`ct.js:4-15`) |

### 3.6 Cookies (E30)

The `cookies` permission plus `<all_urls>` is requested at install. For a
handoff the worker calls `chrome.cookies.getAll({url})`, joins every cookie into
one raw string (`name=value; …`), and sends it as a literal `Cookie:` line
(`bg.js:17,20`). Cookie attributes (domain/path/secure/expiry) are not sent;
the application receives an already-formatted header. Automatic and manual
handoffs behave identically.

### 3.7 Connection lifecycle (E30)

- One persistent WebSocket per worker instance:
  `ws://127.0.0.1:10007/download` with subprotocol `neatextension.v1` (`bg.js:18`).
- Outbound messages are queued when the socket is closed or a message exceeds
  the 118,784-byte cap; the worker reopens the socket and flushes (`bg.js:16-18`).
- `onerror` while a message is pending triggers the content-script alert
  (`bg.js:19`).

### 3.8 Wire protocol (extension → app) (E30)

Plain text, one field per line, `\r\n`-separated (builder order as observed in
`bg.js:16-17`):

| Field | Meaning |
|---|---|
| `1:` | method, `GET`/`POST` (default `GET`) |
| `2:` | download URL |
| `3:` | companion URL — audio stream for an MKV merge proposal, or the TS container for HLS items |
| `6:` | type hint: `normal`, `media`, `hls` |
| `4:` | page title |
| `Origin: ` | origin of the page URL (raw header line, not the WebSocket handshake Origin) |
| `Referer: ` | page URL with the fragment stripped |
| `5:` | tab URL (fallback referer candidate) |
| `Cookie: ` | raw joined cookie string (§3.6) |
| `10:` | `Content-Type` for POST |
| `11:` | `Content-Disposition` |
| `9:` | display/quality label (e.g. "MP4 File 720p") |
| `x-*` | any request header whose name starts with `x-`, forwarded verbatim |
| `7:` | file size (bytes) |
| `8:` | MIME type |
| `__0NeatPostData9__:<body>` | POST body (raw, urlencoded, or reconstructed multipart with a random boundary); a POST without a body sends `Content-Length: 0` instead |

There is no request id and no per-message acknowledgement; the application
answers only with control messages (§3.9). POST bodies are reconstructed from
`requestBody` (raw bytes, `form-data` urlencoded, or multipart) at
`bg.js:25-26,38`.

### 3.9 App → extension messages (E31–E34)

Observed strings and senders in `NeatDM.exe`:

| Message | Meaning | App evidence |
|---|---|---|
| `waiting` | app reports it is idle/waiting; drives the extension's HEAD enrichment (§3.10) | `FUN_004e17d0`, ev_5d3eb5a3… |
| `nowaiting` | the active-download count reached zero | `FUN_005079e0`, ev_c153187a… |
| `ShowPanelChrome=0/1`, `ShowPanelFox=0/1`, `ShowPanelEdge=0/1` | per-browser visibility of the in-page media panel; stored in app settings (E9) | `FUN_004e17d0` / `FUN_004e2af0`, ev_5d3eb5a3… / ev_a9917db4… |
| `WinVersion1424` | app build notice | `FUN_004e17d0` |
| pong | reply to a client ping | `FUN_004e1c80`, ev_931b105b… |

The v1.9.91 worker filters by string: messages containing `Version` are ignored,
and only messages starting with `ShowPanelEdge` update panel state — it applies
the Edge value to `chrome.storage.local` and broadcasts it to content scripts
(`bg.js:19`). The Chrome/Fox variants are sent by the app but unused by this
extension build. On the app side, the handler recognizably processes the
default type `normal`, `Content-Length: `, `User-Agent:`, `Referer: `, and the
POST marker, and consults the `BandWidthLimit` setting (`FUN_00509d70`, E34).

### 3.10 Size/MIME enrichment

When the app reports `waiting`, a handoff has no known size, and the method is
`GET`, the worker performs `fetch(url, {method: "HEAD", credentials: "include"})`
and fills `8:`/`7:` from the response before sending (`bg.js:18`). This avoids a
second probe in the application.

## 4. Our current extension (v0.1.1) — summary

Documented in `ext/chromium/README.md` and `04-protocols.md`; listed here for
the comparison only.

| Aspect | Our behavior |
|---|---|
| Structure | service worker + popup; **no content script, no page UI** |
| Permissions | `activeTab`, `contextMenus`, `downloads`, `storage`, `webRequest`; http/https host permissions (needed for request observation); `cookies` optional, requested from the popup's "Send browser cookies" switch or a context-menu action (both are user gestures) |
| Manual actions | context menu for a link and for the page; popup "Download this page" with connection status, app status, retry, auto-intercept switch, "Send browser cookies" switch |
| Popup design | the popup follows `ext/extension-ui-template.html` (design reference): header with the app mark, the app version and a connection pill, card groups with switch rows for the two toggles, a gradient "Download this page" button, a detected-media group and a collapsible footer note. The template itself is not shipped; the styles live in `popup.css` and the behavior in `popup.js`, because the extension page CSP only allows extension scripts |
| Automatic handoff | `downloads.onCreated` cancel + `erase` for every HTTP(S) download that is not incognito / extension-originated / non-http; **browser download restored** if the app is unreachable (plus one short retry) |
| Bypass | the same switch: automatic interception off leaves new downloads in the browser while manual actions still work (the separate bypass toggle was dropped in extension 0.4.0 as a duplicate; a stored bypass migrates to "off") |
| Toolbar icon | colored `extension-icon.svg` while automatic handoff is on, grayscale `extension-diactivate-icon.svg` while it is off; PNGs under `icons/` are rendered from those SVGs (Chromium does not accept SVG action icons) |
| POST/body + custom headers | `webRequest` observation remembers POST bodies (UTF-8 or base64, ≤256 KiB) and `x-*` request headers (≤16, ≤16 KiB) for 30 s and attaches them to an intercepted download |
| Cookies | optional permission, granted from the popup switch or a context-menu action; structured entries (domain/path/secure/expires); partitioned (CHIPS) cookies omitted; app validates domain scoping; automatic handoffs reuse a grant but never prompt |
| Referer/UA | same-origin referer only; `userAgent` forwarded; page title/URL sent and carried on the item |
| Protocol | JSON `{v, cmd, id, data}` → `{v, id, ok, error?}`, subprotocol `swiftdm.v1`, one short-lived socket per command, 5 s timeout |
| Commands | `ping`, `getVersion`, `showPanel` (server side; unused by the extension), `addDownload`, `subscribe` (push status) |
| Status channel | one persistent `subscribe` socket; `status` events (`waiting`/`active`) shown in the popup and used to reconnect lazily |
| FTP, media detection, site extractors, in-page panel | not implemented |

## 5. Differences

| # | Aspect | Original 1.9.91 | Ours (v0.1.1) |
|---|---|---|---|
| D1 | Primary model | media catcher + automatic file handoff | explicit actions + automatic handoff of all browser downloads |
| D2 | Page integration | content script on all frames + in-page panel | none |
| D3 | Permissions | `webRequest`, `webNavigation`, full `cookies`, `<all_urls>` | `webRequest` + http(s) host permissions, optional cookies requested on a user gesture |
| D4 | Interception | `webRequest` observation + classifier; cancels only its own handed-off URL | `downloads.onCreated` cancel for every eligible download, then handoff |
| D5 | App unavailable | alert in the page, download lost | browser download restarted, retry once |
| D6 | Media (YouTube/Vimeo/FB/HLS/DASH/subtitles/MKV) | extensive extractors and pairing | none |
| D7 | Connection | one persistent socket + queue + control messages | per-command socket, request/response |
| D8 | Protocol | line-based text, `neatextension.v1`, no ids/acks | JSON envelope, `swiftdm.v1`, ids + ok/error |
| D9 | POST/body + `x-*` headers | supported (form/multipart reconstruction) | supported (UTF-8/base64 body, `x-*` headers, 256 KiB cap) |
| D10 | Cookies | raw joined `Cookie:` string, full permission | structured, validated, optional permission requested from the popup switch or a context-menu action |
| D11 | Referer/Origin | always sent (`Origin:` + `Referer:` lines) | same-origin referer only |
| D12 | Page metadata | title + tab URL sent | sent, not persisted (same as ours) |
| D13 | Bypass | hold `Delete` | automatic-interception switch off (persistent) |
| D14 | Size enrichment | worker HEAD fetch when app is idle | app-side probe |
| D15 | App → extension channel | `waiting`/`nowaiting`/`ShowPanel*` push | `subscribe` socket + `status` events (idle/active count) |
| D16 | FTP | observed and handed off | http/https only |
| D17 | Distribution | store build with stable ID | unpacked development build |
| D18 | Limits | 118,784-byte message cap | 1 MiB frame cap, 32 clients |
| D19 | Security posture | no origin pinning found in the app (bounded negative, E35) | origin + subprotocol validation, URL/cookie/referer/header validation |
| D20 | Incognito | not excluded by the extension | explicitly skipped |

## 6. Candidate work items (decision pending)

Each candidate lists what it would require. No order is implied. When one is
selected, record the decision here and implement it against this document and
`04-protocols.md`.

| ID | Candidate | Derived from | Would touch | Notes / open questions |
|---|---|---|---|---|
| C1 | POST/body handoff and `x-*` request headers | D9, §3.8 | extension `addDownload` payload; `docs/spec/04` message schema; app validation + engine POST path | Which headers are safe to forward? Size cap for bodies? Interaction with our cookie scoping |
| C2 | App → extension status channel | D15, §3.9 | our WebSocket server (push messages), extension worker, protocol version | Our protocol is request/response today; a push channel needs framing/versioning and a reconnect story |
| C3 | Quick bypass (hold `Delete`) | D13 | content script or a keyboard shortcut approach; extension manifest | Bypass only matters once interception is broad; content script costs a new permission surface |
| C4 | In-page media panel | D2, §3.3 | new content script + panel UI + message bridge | Largest addition; needs UX decisions (styling, per-tab state, incognito) |
| C5 | Media detection heuristics | §3.2 | extension classifier | Which lists/thresholds to adopt or simplify; false-positive policy |
| C6 | Site extractors (YouTube/Vimeo/Facebook) | §3.3 | content script | Site-specific maintenance; clean-room re-implementation required |
| C7 | HLS (`m3u8`) handling | §3.3, `03-engine.md` HLS note | extension parser + engine (v3 checklist) | Overlaps the planned HLS engine work |
| C8 | DASH/adaptive pairing + MKV mux proposal | §3.3 | engine mux (v3), extension pairing | Depends on C7 |
| C9 | Subtitle (VTT/SRT) handling | §3.3 | extension labels, app type hint | Small; depends on C4/C5 context |
| C10 | Size/MIME enrichment (HEAD by extension or keep app probe) | D14, §3.10 | extension worker, protocol `addDownload` fields | App-side probe already exists; only useful with C2 |
| C11 | FTP handoff | D16 | extension observation + engine FTP (v3 checklist) | Engine support is the real dependency |
| C12 | Persist `pageUrl`/`pageTitle` | D12 | download history store (v1.1 item) + list columns | Same gap as the original; needs the history schema first |
| C13 | Cookie model decision | D3, D10 | keep current model or add a "send raw cookies" mode | Current model is deliberately stricter; document any change in `04-protocols.md` |
| C14 | Store packaging + stable ID | D17 | manifest `key`, packaging scripts, install docs | `key` is already present; store publishing out of scope for now |

## 7. Open questions / unknowns

- Runtime behavior of either side was not observed (no app/browser execution);
  all statements are static-source or static-binary observations.
- The exact MIME→extension table (≈100 entries) and the full classifier branch
  logic are in the artifact (`bg.js:1-7,29-37`); this document summarizes them.
- The application's request-line handling for `GET /download` is not pinned:
  no path literal was found near the handshake function, so the `/download`
  path appears cosmetic (bounded negative).
- `WinVersion1424` semantics are unknown beyond "contains Version → ignored by
  the extension".
- App-side parsing of fields `3:` and `9:` is inferred from the extension's
  builder and the content-script message format; the app-side branch was not
  traced end to end.
- Whether the app authenticates clients beyond the handshake (origin pinning)
  is not evidenced; no origin string or check was found in the binary.
- The original content script's site extractors are version-specific to the
  sites as of 1.9.91; site behavior may have changed since.

## 8. Verification expectations for future work

- Extension logic is testable headless with the existing Node harness
  (`tests/test_extension_logic.js`) by mocking `chrome.*` APIs.
- App-side protocol changes extend `tests/test_browser_protocol.cpp`.
- UI behavior remains manually verified by the owner (AGENTS.md §2); the agent
  provides a checklist instead of driving the GUI.

## 9. References

- `04-protocols.md` §3 — our `swiftdm.v1` protocol and its rationale.
- `06-parity-checklist.md` §v2 — status of browser integration.
- `evidence-ledger.md` E27–E37 — evidence rows for this document.
- `ext/chromium/README.md` — our extension's install steps and limitations.

## 10. Decision log

The feature owner selected the candidates below for implementation (2026-10).
C3 was adjusted during selection: bypass is a persistent switch instead of
holding `Delete`; extension 0.4.0 folded it into the automatic-interception
switch, since "keep downloads in the browser" is exactly that switch off.
Status is updated as the work lands; version steps follow `AGENTS.md`.

| ID | Decision | Status |
|---|---|---|
| C1 | POST/body + `x-*` headers via `webRequest` observation; caps and validation recorded in `04-protocols.md` | Implemented (extension 0.2.0, app 0.10.0) |
| C2 | App→extension status channel: `subscribe` command plus `status` push events (`waiting`, `active`) | Implemented |
| C3 | Bypass as a persistent switch instead of hold-`Delete` (owner-adjusted); since extension 0.4.0 the automatic-interception switch covers it, and a stored bypass value migrates to "off" | Implemented |
| C13 | Keep the strict structured cookie model; no raw-cookie mode. Raw `Cookie`/`Authorization` headers stay rejected | Decided; enforced by protocol validation and tests |
| C12 | Persist `pageUrl`/`pageTitle` | Implemented: SQLite history rows (spec 02 schema) are written/updated per download, including page metadata; restoring the list at startup remains a v1.1 item |
| C11 | FTP handoff / engine support | Implemented: `ftp://`/`ftps://` are accepted by the protocol, the manual dialog, and the CLI; the engine probes with `SIZE` and resumes with `REST` on one connection. The extension only forwards explicit FTP links (modern Chromium no longer loads FTP pages) |
| C7 | HLS (`m3u8`) handling | Implemented: engine parses master/media playlists, picks the highest-bandwidth variant, writes `EXT-X-MAP`, downloads segments sequentially into one `.ts`, and resumes at the first unfinished segment; encrypted playlists are refused. The extension lists detected playlists in the popup |
| C8 | Adaptive pairing + MKV mux proposal | Implemented (pairing): the extension pairs the newest video-only and audio-only responses per tab and hands both off with a shared `group` token (validated by the protocol, shown in Properties, stored in `downloads.groupid`). MKV muxing itself is proposed only — no muxer dependency is added in 0.11.0 (`03-engine.md` §9) |
