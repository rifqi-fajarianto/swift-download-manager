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

## 2. FTP (later phase)

- Passive mode (`FTP_STATE_COMMAND_SENT_PASV`), `SIZE`/`REST`/`RETR` for
  segmented resume (E21). v1: not implemented.

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
| `addDownload` | ext → app | `{ "url", "pageUrl", "pageTitle", "cookies", "referer", "postData"?, "filename"?, "mime"?, "userAgent"? }` |
| `showPanel` | ext → app | `{ "visible": true|false }` |
| `getVersion` | ext → app | `{}` → `{ "version" }` |

Server reply: `{ "v":1, "id":..., "ok": true|false, "error"?: "..." }`.

Port defaults to 10007. The current implementation binds IPv4 loopback only and
validates the Chromium extension origin and `swiftdm.v1` subprotocol during the
handshake. The Chromium development extension uses Manifest V3 and sends a
download only after an explicit context-menu or popup action. Cookie permission
is optional and requested for the target origin; scoped cookie attributes are
passed to libcurl so cookies follow domain/path/secure rules across redirects.
The server rejects unsafe URLs, malformed messages, oversized frames, and
cookies scoped to a different host. Firefox and automatic download interception
remain future work.

## 4. Log/error formats

- Error display: `Error : HTTP/1.1 403 Forbidden` style (E18 screenshot).
- Per-download log file with `.LogFile.txt` extension (E5/E14 notes).
