# 04 — Protocols

## 1. HTTP/HTTPS downloads

- Methods: `GET`, `POST` (with `postdata`) (E4 schema).
- `Range: bytes=<start>-<end>` for segment requests; the original adds it
  conditionally (E12).
- `Accept-Ranges`, `Content-Range`, `Content-Disposition`, `Content-Length`
  parsing (static strings E15).
- Chunked transfer decoding and redirect handling (E15).
- Custom headers per download (`headers` table) and cookies (static strings
  `Cookie:`, `SetCookie`, `ReadCookie`).
- User-Agent: per-download `useragent`, else global `DefaultAgent` (E8).
- TLS: system/OpenSSL via libcurl (original used Schannel, E16).
- Proxy: HTTP proxy with `CONNECT` tunneling for TLS; SOCKS4/SOCKS5 with and
  without auth (`SocksVersion` setting; strings E15). No proxy mode default.

## 2. FTP

- Passive mode (`FTP_STATE_COMMAND_SENT_PASV`), `SIZE`/`REST`/`RETR` for
  resume (E21). Implemented 0.11.0 with libcurl: `ftp://`/`ftps://` URLs take a
  probe (`NOBODY` maps to `SIZE`), then one connection downloads the file;
  `REST` continues an interrupted transfer from the part file.
- A single connection is used (no parallel segments): FTP resume is
  offset-based, not range-based, so parallel segments are not possible.
- Transfer errors are reported from the curl error buffer; FTP has no HTTP
  status code, so failures are surfaced as "could not reach/size the file".
- The Chromium extension can only forward explicit `ftp://` links (modern
  Chromium no longer loads FTP pages itself); the app, CLI, and the manual
  dialog accept `ftp://` URLs directly.

## 3. Browser-extension WebSocket protocol (v2 scope, design fixed now)

The original runs a WebSocket server on `127.0.0.1` (E10). Observed:

- Port **10007** (`0x2717`, htons) (E10).
- Subprotocol: `neatextension.v1` (E11).
- Standard RFC6455 handshake (`Sec-WebSocket-Accept`) (E11).
- Client frames: JSON messages (static strings `text/x-json`, `application/json`).

SwiftDM protocol **[design]** — documented, versioned, our own, sharing only the
observable basics (127.0.0.1, WebSocket, JSON):

```
GET / HTTP/1.1
Upgrade: websocket
Sec-WebSocket-Protocol: swiftdm.v1
```

Message envelope:

```json
{ "v": 1, "cmd": "<command>", "id": "<request-id>", "data": { ... } }
```

Commands (v1 of our extension protocol):

| cmd | direction | data |
|---|---|---|
| `ping` | ext → app | `{}` → `{ "pong": true, "version": "0.x.y" }` |
| `addDownload` | ext → app | `{ "url", "pageUrl", "pageTitle", "cookies", "referer", "filename"?, "userAgent"?, "method"?, "postData"?, "postDataEncoding"?: "utf8"\|"base64", "headers"?: [{ "name": "x-…", "value" }], "group"? }` |
| `showPanel` | ext → app | `{ "visible": true|false }` |
| `getVersion` | ext → app | `{}` → `{ "version" }` |
| `subscribe` | ext → app | `{}` → `{ "subscribed": true, "version", "status": { "waiting", "active" } }`; the socket then receives `{ "v":1, "event":"status", "data": { "waiting", "active" } }` frames whenever the app's active-download count changes |

Server reply: `{ "v":1, "id":..., "ok": true|false, "error"?: "..." }`. Push
frames carry `event` and no `id`, so command sockets (which never subscribe) are
unaffected.

`postData` is accepted in UTF-8 or base64 form, capped at 256 KiB decoded, and
implies `POST`. Extra request headers must be named `x-*` (HTTP token
characters, 16 headers / 16 KiB total); raw `Cookie`, `Authorization`, or
`Referer` headers are rejected because cookies stay structured and domain-scoped
(C13 decision: the strict structured cookie model is kept — no raw-cookie mode).
`group` is an optional short token (`[A-Za-z0-9_-]{1,32}`, e.g. `pair-1f2e3d4c`)
attached to the video and audio halves of one adaptive handoff. The app stores
it on the item (Properties → *Pair*) and in `downloads.groupid`; nothing else
depends on it yet — muxing both halves is future work (`03-engine.md` §9).

Download URLs may use `http`, `https`, or `ftp`; referers stay HTTP(S) only, so
an FTP handoff never carries a referer or cookies. HLS playlists need no extra
protocol field: a `video/mp2t`/`mpegurl` URL (or a `.m3u8` path) is detected by
the engine and downloaded segment by segment (`03-engine.md` §9).

Port defaults to 10007. The current implementation binds IPv4 loopback only and
accepts a well-formed extension origin (`chrome-extension://`, `extensions://`,
or `extension://` plus a 32-character a-p ID) together with the `swiftdm.v1`
subprotocol during the handshake; the exact development extension ID is not
pinned because reloading an unpacked extension from another path can change it.
Ordinary web origins are rejected, the rejection is logged, and the reason is
shown in the Extensions dialog. The Chromium Manifest V3 extension sends
downloads either from an explicit context-menu/popup action or by intercepting
HTTP(S) downloads. `webRequest` observation (host permissions for http/https)
remembers POST bodies, `x-*` request headers, and response content types for a
short window so an
intercepted POST download is forwarded with them. A popup switch turns
automatic interception off while manual actions keep working. A persistent
subscription socket receives app-status pushes for the popup. Cookie permission
is optional, and only a user gesture may ask for it: the popup's "Send browser
cookies" switch requests it for the current site, the context-menu action for
its target origin, and later automatic handoffs reuse the grant. Scoped cookie
attributes are passed to libcurl so cookies follow domain/path/secure rules
across redirects. `addDownload` does not start a transfer directly: the app
probes the URL and shows the independent Download Confirmation window before
the transfer starts. The main window stays hidden or minimized if it was not
visible when the browser handoff arrived (0.18.3).
The server rejects unsafe URLs, malformed messages, oversized frames, bodies or
headers over the caps above, and cookies scoped to a different host. Firefox
support and a production extension-store ID pin remain future work. The original
`neatextension.v1` protocol and the behavioral comparison with the shipped
NeatDownloadManager extension are documented in
`07-chromium-extension-reference.md`; the adopted/remaining candidates are
tracked in its decision log.

## 4. Log/error formats

- Error display: `Error : HTTP/1.1 403 Forbidden` style (E18 screenshot).
- Error messages quote the host: when a probe or transfer fails with 4xx/5xx,
  the first `1024` bytes of the response body are summarized (JSON
  `value`/`message`, HTML `<title>`/text with scripts and styles dropped,
  anything else collapsed to one line) and appended after ` - `, capped at
  `300` bytes on a UTF-8 boundary. Refused bodies are never written into the
  partial file.
- Per-download log file with `.LogFile.txt` extension (E5/E14 notes).

## 5. Download interstitials (0.13.0)

Some hosts answer a file link with an HTML page instead of the file. Google
Drive is the case this section covers (verified against the live service):

- `drive.google.com/file/d/<id>/view` (also `/open?id=`, `/uc?id=`,
  `/download?id=`, `/drive/u/<n>/…`) serves the Drive web app; Drive's own
  download URL is
  `https://drive.usercontent.google.com/download?id=<id>&export=download`, so
  viewer links are rewritten to it before probing, keeping the link's handoff
  parameters: `authuser` (account), `resourcekey` (security-updated links need
  the key) and `confirm` (a browser's own download URL carries it, which then
  resolves in one step). The session `uuid` is dropped.
- Files above Google's virus-scan limit answer that URL with `200 text/html` —
  "Google Drive can't scan this file for viruses." — whose form carries
  `id`/`export`/`confirm=t`/`uuid` hidden fields. The probe submits that form
  transparently (exactly one step, so a looping site cannot recurse) and probes
  the resulting URL, so name, size and range support describe the real file.
  `uuid` is dropped: Drive ignores its value (verified with an arbitrary token)
  and leaving it out keeps the resolved URL deterministic, which is what keeps a
  resumed partial file matching. Before this step the 2.4 KB page was stored as
  the download under a URL-derived name (`download`, `view`, or the Drive id for
  folder links).
- Links that never lead to a file fail with a reason instead of a saved page:
  folder listings (`/drive/folders/<id>`; no request is made), Google Docs
  editor pages, and sign-in walls (`accounts.google.com`, where a private or
  removed file redirects — Drive answers the browser's own `confirm=t` URL the
  same way when it is fetched without the account's session). The message names
  both ways out: share the file as "Anyone with the link", or open it in the
  browser and hand it over with the extension popup's "Send browser cookies"
  switch on (the popup asks for the cookie permission on that user gesture, and
  later handoffs reuse the grant).
- Other hosts are unaffected: a page is only treated as a confirmation notice
  when it carries the form above, and the body is read with a `256 KiB` cap.
  Only Drive/Docs pages are refused without such a form, because Drive serves
  every file with `Content-Disposition` — an HTML response without it is never
  the file itself. A plain HTML document from an ordinary host is still
  downloadable as a file.
- The resolved URL is used for the transfer (`ProbeResult::download_url`,
  03-engine.md §5); the probe result otherwise flows through the pipeline
  unchanged.
