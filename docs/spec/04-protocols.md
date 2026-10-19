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
intercepted POST download is forwarded with them. A popup option turns
interception off (bypass) while manual actions keep working. A persistent
subscription socket receives app-status pushes for the popup. Cookie permission
is optional and requested for the target origin; scoped cookie attributes are
passed to libcurl so cookies follow domain/path/secure rules across redirects.
The server rejects unsafe URLs, malformed messages, oversized frames, bodies or
headers over the caps above, and cookies scoped to a different host. Firefox
support and a production extension-store ID pin remain future work. The original
`neatextension.v1` protocol and the behavioral comparison with the shipped
NeatDownloadManager extension are documented in
`07-chromium-extension-reference.md`; the adopted/remaining candidates are
tracked in its decision log.

## 4. Log/error formats

- Error display: `Error : HTTP/1.1 403 Forbidden` style (E18 screenshot).
- Per-download log file with `.LogFile.txt` extension (E5/E14 notes).
