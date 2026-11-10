# 08 — Media download from the browser

Specification for taking over a video/audio stream that is playing in the
browser and downloading it with SwiftDM. Written before the implementation
(rule 3 of `README.md`), so the design is fixed here and the code follows it.

Status: **0.26.0**.

> Clean-room note: this document describes SwiftDM's own design. It is derived
> from the general architecture of browser-integrated download managers (a
> browser extension observes the page and hands *metadata* to a local
> application, which re-issues the request itself). No protocol, byte format,
> code, asset or branding of any reference application is reproduced. The
> SwiftDM wire protocol stays `swiftdm.v1` (`04-protocols.md` §3).

---

## 1. The one architectural rule

**The browser never sends media bytes to SwiftDM.**

The extension sends only metadata: the media URL (or the manifest URL, or the
list of media segments), the page URL, a same-origin `Referer`, domain-scoped
cookies, the `User-Agent`, the chosen quality label and the element geometry.
SwiftDM then re-issues the HTTP request itself with those values replayed.

Consequences that shape the rest of this document:

1. A download survives closing the tab or quitting the browser.
2. Signed/short-lived CDN URLs work as long as they are still valid when the
   download starts, because the *same* URL plus the *same* session headers are
   replayed.
3. What the browser can see, the extension can only describe. When the page
   hides the real URL inside its own JavaScript (`MediaSource` + `blob:`), the
   extension has to reconstruct the stream from the segment requests it
   observed (§6). That reconstruction is best-effort by nature and says so in
   the UI.
4. DRM is not broken. If the only playable stream is Widevine/PlayReady
   encrypted, the plaintext bytes do not exist for anyone but the player; the
   extension marks the candidate `protected` and SwiftDM refuses it with a
   readable message instead of writing an unplayable file.

---

## 2. Data flow

```
 page (browser)                       extension                       SwiftDM (0.26.0)
 ─────────────                        ─────────                       ───────────────
 <video>/<audio> DOM   ──scan──►  content.js  ─┐
 XHR/fetch metadata    ──hook──►  page-hook.js ─┤
 segment requests      ───────►  core.js (webRequest + media-mse.js)
                                                │ merge + rank + dedupe per tab
                                                ▼
                                     popup list / overlay button
                                                │ user picks one
                                                ▼
                                     cmd "addMedia" over ws://127.0.0.1:10007
                                                │
                                                ▼
                                     BrowserIntegration (validate, rate-limit)
                                                │ mediaRequested(QJsonObject)
                                                ▼
                                     MediaProbeTask  ── HLS master / MPD / file probe
                                                │ variants + duration + size
                                                ▼
                                     ConfirmDownloadDialog (Kind, Quality, warning)
                                                │
                                                ▼
                                     DownloadEngine  ── file | HLS | DASH | MSE
                                                │ cookie/Referer/UA replay
                                                ▼
                                     Downloads list row (+ group token for pairs)
```

---

## 3. Discovery — how a playing video is found

Four independent signals run at once and are merged per tab.

### 3.1 DOM scan (`content.js` + `media-detect.js`)

Every frame walks `video, audio, object, embed` and keeps the best candidate
using this priority (lower wins):

| Rank | Candidate | Why |
|---|---|---|
| 1 | `src` (or `<source src>`, or `data` for plugins) equals the URL the app asked about, or the element was pinned by id | exact match is always right |
| 2 | `object-fit: cover` element | a decorative/background video |
| 3 | `document.activeElement` if it is a media element, else `document.elementFromPoint(mouse)` | the thing the user is looking at |
| 4 | `src` already classified as media by the network sniffer | already known to be a stream |
| 5 | **largest playing, visible, video-ish element** | the main player |
| 6 | first visible media element | fallback |
| 7 | first hidden `<video>` | a player that is momentarily `display:none` |

Filters applied before ranking:

* `clientWidth > 0 && clientHeight > 0` (zero-size elements are invisible).
* Not entirely left of / above the viewport.
* `visibility != hidden`, `display != none`, `opacity != 0`.
* `<object>`/`<embed>`: `type` must not start with `text/` or `image/`;
  `<embed>` must be `application/x-shockwave-flash`.
* A `<video>` whose `src` starts with `data:audio/` is not a video.
* Video-ish aspect ratio: `w * 1.35 > h && w < h * 3` — accepts 3:4 portrait up
  to 3:1 landscape, rejects banners and tracking pixels.
* Minimum 15×10 CSS px for a `<video>`.
* Occlusion: an element covered at its centre by an opaque, visible element is
  skipped (background tabs, modal overlays, cookie walls).

Rank 5 is what distinguishes this feature from "the biggest box on the page":
an element that is **currently playing** (`!paused && currentTime > 0 &&
readyState >= 2`) outranks a paused one of the same size, and the two are
compared by area only within the playing group first. A 1280×720 trailer that
is playing beats a 1920×1080 hero image that is not.

The scan also reads, per candidate: `currentSrc`/`src`, `currentTime`,
`duration`, `paused`, `readyState`, `videoWidth`/`videoHeight`,
`mediaKeys != null` (an EME/DRM player), and the element geometry
`[x0, y0, x1, y1, dpr]` for the overlay button.

Scanning is **on demand** (popup opens, overlay button needs a position) plus
debounced `MutationObserver`/`IntersectionObserver` events, throttled to at most
one full scan per second per frame. There is no periodic push to the app.

### 3.2 Network sniffing (`core.js`)

`webRequest` listeners are registered for
`main_frame, sub_frame, object, xmlhttprequest, media, other`. A response is a
media candidate when any of these holds:

* `Content-Type` maps to a media type (`video/*`, `audio/*`, `mpegurl`,
  `dash+xml`, `f4m`, `iso.segment`, …);
* `Content-Disposition: attachment`;
* the URL path ends with a media extension (`.mp4`, `.m4v`, `.webm`, `.mkv`,
  `.ts`, `.m4s`, `.mp3`, `.m4a`, `.flac`, `.m3u8`, `.mpd`, `.f4m`, …);
* the URL matches a streaming shape (`/playlist/`, `/hls/`, `.m3u8`, `.mpd`).

Kept per tab with a 60 s TTL and an 8-entry cap per kind: `hls`, `dash`,
`video`, `audio`, `segment`. Video-only and audio-only responses are paired
newest-first and handed off with a shared `group` token (existing behaviour,
0.11.0).

### 3.3 In-page hooks (`page-hook.js`, MAIN world)

Modern players build the media URL inside their own JavaScript. The hook runs in
the page's own world so it can see `XMLHttpRequest` and `fetch` calls the
isolated world cannot, and records **metadata only**:

| Hooked | Recorded |
|---|---|
| `XMLHttpRequest.prototype.open` | method, URL |
| `XMLHttpRequest.prototype.send` | nothing (never reads the body) |
| `XMLHttpRequest.prototype.setRequestHeader` | request header names only |
| `window.fetch` | URL, method, `content-type` request header |
| `Response.prototype.json` | nothing (no body copy) |

Each record is `{url, method, order, at}` plus, once the response headers are
known, `contentType` and `contentLength`. Records are forwarded to the content
script with a per-page nonce so a page cannot forge them.

The hook exists to (a) prove that a URL was fetched by the page's own code
(rank 4 in the DOM scan) and (b) supply the ordering for §6.

### 3.4 Site extractors — deliberately out of scope

Per-site manifest/JSON scraping (a player's private API) needs constant
maintenance and breaks silently when a site changes. 0.26.0 does not ship any.
The generic DOM + network + hook signals cover the ordinary case, and a site
that defeats all three is simply not offered.

---

## 4. What the user sees

### 4.1 Overlay button (`content.js`)

When a media element passes the scan, a small button is positioned over its
top-left corner: shadow DOM host, `all: initial`, `position: fixed`, pointer
events only on the button itself, hidden on `fullscreenchange` conflicts, and
removed when the element leaves the DOM. It appears on hover/focus of the
element (never always-on, so it cannot obscure player controls) and clicking it
sends `{type: "download-media"}` to the background.

The overlay can be turned off in SwiftDM (Settings → General → *Media overlay
button*); the app pushes `{event: "config", data: {mediaOverlay: false}}` to
every subscribed extension socket, and the content script hides the button
without a reload.

### 4.2 Popup list

The popup shows one row per candidate:

```
Direct video · 720p           1920×1080 · 84.2 MB
HLS · 1080p (3 variants)      master playlist
DASH · 1080p                  video + audio
MSE · 412 segments            video + audio · may be incomplete
Audio · 128k                  paired with the video above
```

A `DRM` badge marks a protected candidate and its send button is disabled with
the reason on the row. Clicking a row sends `addMedia`.

---

## 5. Protocol

`swiftdm.v1` (`04-protocols.md` §3) gains one command and two events. Envelope
and reply shape are unchanged.

### 5.1 `addMedia` (extension → app)

```json
{ "v": 1, "cmd": "addMedia", "id": "<uuid>", "data": {
    "url": "https://cdn.example/v.mpd",
    "kind": "file" | "hls" | "dash" | "mse",
    "pageUrl": "https://site.example/watch",
    "pageTitle": "…",
    "referer": "https://site.example/watch",
    "userAgent": "…",
    "cookies": [ { "domain": ".cdn.example", "path": "/", "name": "t",
                   "value": "…", "secure": true, "expires": 0 } ],
    "headers": [ { "name": "x-token", "value": "…" } ],
    "filename": "v.mp4",
    "quality": "1080p",
    "group": "pair-1f2e3d4c",
    "durationSec": 600,
    "positionSec": 42,
    "elementTag": "video",
    "protected": false,
    "candidates": [ { "url": "…", "kind": "hls", "label": "720p",
                      "bandwidth": 2500000 } ],
    "segments": [ { "url": "…", "role": "init", "order": 0,
                    "contentType": "video/mp4" } ]
} }
```

Reply: `{ "v": 1, "id": "…", "ok": true, "data": { "accepted": true } }`,
written **before** the confirmation dialog opens (queued delivery, exactly like
`addDownload`), so the extension's 5 s timeout never fires and a retry cannot
produce a duplicate prompt.

Validation (shared helper with `addDownload`, so both commands enforce the same
rules):

| Field | Rule |
|---|---|
| `url`, `candidates[].url`, `segments[].url` | scheme allowlist `http/https/ftp/ftps`, no userinfo, ≤ 8192 chars |
| `kind` | one of `file`, `hls`, `dash`, `mse` |
| `cookies` | ≤ 256 structured entries, domain-scoped to the download host, same rules as `addDownload` |
| `referer` | same-origin as the download URL, ≤ 8192 |
| `headers` | `x-*` only, ≤ 16 entries, ≤ 16 KiB total |
| `filename` | ≤ 255 chars, sanitised again by the engine |
| `quality` | ≤ 64 chars, no control characters |
| `durationSec`, `positionSec`, `candidates[].bandwidth` | finite, ≥ 0 |
| `candidates` | ≤ 8 |
| `segments` | ≤ 2000, total payload ≤ 512 KiB, `role` ∈ `init`/`media`, `order` ascending |
| `protected` | `true` → rejected with `"protected media cannot be downloaded"` |

### 5.2 `config` and `pair` (app → extension, push)

```json
{ "v": 1, "event": "config", "data": { "mediaOverlay": true } }
{ "v": 1, "event": "pair",   "data": { "token": "<43-char base64url>" } }
```

`config` is sent to every socket that sent `subscribe`, and is also included in
the `subscribe` reply so a freshly connected worker starts with the right
state. `pair` answers the pairing handshake (§7).

### 5.3 Handshake token

```
GET /?t=<token> HTTP/1.1
```

The token is opaque, generated by the app, and compared in constant time. A
request without a token, or with a wrong one, follows the pairing flow in §7.

---

## 6. Media Source Extensions (`blob:` players)

When a player feeds `<video>` from `MediaSource`, `video.src` is a `blob:` URL
and no single downloadable file exists. What *does* exist is the sequence of
segment requests the player made.

### 6.1 Collection (`media-mse.js`)

A response is part of an MSE stream when:

* its `Content-Type` is a media segment type (`video/iso.segment`,
  `video/mp4`, `video/webm`, `audio/mp4`, `audio/webm`), **or**
* its `Content-Range`/`Content-Length` and URL shape match a segment pattern
  (`/segment/`, `/chunk/`, `.m4s`, `.m4v`, `.ts`, `/init`), **or**
* the page hook recorded a fetch for it from the page's own code.

Segments are grouped by `(host, path-prefix)` and by init/media role. The first
response with an `ftyp`+`moov` prefix (or a URL containing `init`) is the
initialisation segment. Order comes from the page hook's monotonic counter,
which is the closest observable proxy for the player's append order.

Caps: 2000 URLs, 512 KiB total, 30 min TTL. Beyond that the oldest entries are
dropped and the candidate is marked incomplete.

### 6.2 Reconstruction (`run_mse`)

1. Fetch the init segment first, then the media segments in recorded order,
   appending to one part file — the same loop `run_hls` uses, with a
   `mse_progress` file for resume.
2. Verify continuity while writing: parse the `moof` boxes of each media
   segment and require `mfhd.sequence_number` to increase by one and
   `tfdt.baseMediaDecodeTime` not to jump backwards by more than one segment
   duration. A gap (the user seeked, the extension was installed mid-stream, or
   entries were dropped by the caps) **fails the download** with
   "the recorded stream has a gap; play the video from the start and try again".
   A broken file is never written.
3. Report progress by segment index (byte total is unknown), exactly like HLS.

The result is an `fMP4` file (or WebM when the segments are WebM, in which case
continuity is checked by cluster timestamps instead). It plays in ordinary
players; audio and video are separate files when the player fetched them
separately, and are tagged with the same `group` token.

### 6.3 What is honest to say in the UI

The confirmation dialog shows the segment count and a warning that the result
is assembled from recorded segments and may be incomplete. This is not a
hidden limitation; it is the nature of `blob:` playback.

---

## 7. Pairing (trust boundary)

Binding to `127.0.0.1` keeps the socket off the network, but any local process
can still connect and claim to be the extension. Origin checks are compatibility
checks, not authentication.

0.26.0 adds a trust-on-first-use token:

1. The extension connects without a token (first run, or after the app's token
   file was deleted).
2. The app shows a modal prompt naming the connecting origin:
   *Allow* / *Deny*.
3. **Allow** → the app writes `$XDG_CONFIG_HOME/swiftdm/browser-token`
   (mode 0600, 32 random bytes, base64url), sends `{event:"pair", data:{token}}`,
   and keeps the socket. **Deny** → the socket is closed with code 1008 and the
   extension reports "SwiftDM refused the connection".
4. The extension stores the token in `storage.local` and appends `?t=<token>` to
   every later socket URL (status and command channels).
5. A connection carrying a valid token is accepted without a prompt. A wrong
   token is rejected like a missing one (the extension then clears its stored
   token and retries the pairing flow once).

The token file is per-user and never leaves the machine. Deleting it revokes
every paired extension; the next connection prompts again.

---

## 8. Quality selection

`MediaProbeTask` resolves the manifest before the confirmation dialog opens and
returns a list of variants:

| Kind | Variants come from |
|---|---|
| `file` | one entry (the URL itself) |
| `hls` | every `#EXT-X-STREAM-INF` variant: `RESOLUTION`, `BANDWIDTH`, `CODECS`, `FRAME-RATE`; a media playlist yields one entry |
| `dash` | every `<Representation>` of the first video `AdaptationSet` and the first audio `AdaptationSet`: `width`/`height`, `bandwidth`, `codecs`, `mimeType` |
| `mse` | one entry (segment count and role summary) |

The dialog shows a *Quality* combo (best first, labelled
`1080p · 5.0 Mb/s · avc1.640028`), preselecting the extension's `quality` when
it matches a label and the highest bandwidth otherwise. Choosing a variant sets
`DownloadTask::Options::url` to the rendition's media URL (an HLS media
playlist, or the MPD for DASH) and `variant_id` to the DASH representation id;
the engine then downloads that rendition instead of the "highest bandwidth"
default.

When the manifest cannot be read (offline, 403, malformed), the dialog still
opens with the raw URL and no quality combo, so the user can cancel cleanly
instead of being stuck.

---

## 9. DASH

Supported subset, chosen so that a failure is always a readable message rather
than a broken file:

* `MPD` → `Period` (first) → `AdaptationSet` (one video, one audio) →
  `Representation`.
* Addressing: `SegmentTemplate` with `$Number$`, `$Time$`,
  `$RepresentationID$`, `$Bandwidth$` and `SegmentTimeline` (`r` repeats);
  `SegmentList` with `SegmentURL@media`; `SegmentBase` (fetched as one file;
  its `sidx` is readable but the engine does not need to split it).
* `BaseURL` inheritance at MPD/Period/AdaptationSet/Representation level.
* Init segment from `initialization` (template/list) or `Initialization@sourceURL`.
* Unsupported: multi-period timelines, `xlink:href` expansion, DRM
  `ContentProtection` (reported as protected), live/dynamic MPDs (reported as
  "live streams are not supported").

Download: the chosen representation is fetched as one file, its init segment
first, then every media segment appended in order. The quality combo lists the
video and the audio representations of the first `AdaptationSet` each, so a
demuxed stream is fetched as two separate files by downloading the video
rendition and then the audio rendition. Muxing them into one container is still
out of scope (`03-engine.md` §9), so the app does not pretend a video-only
representation is playable on its own.

---

## 10. Security

| Risk | Control |
|---|---|
| Off-host access | bind `127.0.0.1` only (unchanged) |
| Local process impersonating the extension | pairing token (§7) |
| `file://` or `data:` URL smuggled in a manifest | every manifest/segment URL passes `is_supported_download_url()` before the part file exists |
| Path chosen by the peer | the peer sends a *suggested* name; the engine sanitises and length-limits it |
| Credential leak across hosts | cookies stay domain-scoped, `Referer` must be same-origin, `x-*` headers dropped on cross-origin redirect |
| Message flood | ≤ 30 `addMedia` per minute per socket, ≤ 8 candidates, ≤ 2000 segments |
| Page forging hook messages | per-page nonce on `postMessage`, and hook data is only a *hint* (rank 4), never a download command |
| Running page JavaScript | not done at all (no embedded script engine) |

---

## 11. Tests

| Test | Covers |
|---|---|
| `media_detect` (Node) | ranking order, aspect/size/occlusion filters, playing beats paused, DRM flag |
| `media_mse` (Node) | segment classification, ordering, gap detection, caps/TTL |
| `extension_logic` (Node) | `download-media`/`scan-media`/`media-segment`, merge+dedupe, `config` push, token in the socket URL |
| `extension_popup` (Node) | new elements exist and are driven; no inline script |
| `extension_package` (Node) | new files ship in both archives |
| `xml_lite` (C++) | attributes, entities, nesting, malformed input |
| `mp4_boxes` (C++) | `moof`/`mdat` walk, `mfhd`, `tfdt`, `sidx` |
| `dash` (C++) | template/timeline/list/base/inherited-template parsing |
| `engine_dash_mse` (C++) | DASH and MSE downloads from a local HTTP server; a sequence gap fails and removes the part file |
| `browser_token` (C++) | generate, 0600 mode, verify, reject a wrong token |
| `browser_protocol` (C++) | `addMedia` accept/reject matrix, `protected`, oversized `segments`, `config` push, pairing flow |
| `media` (C++) | HLS variant selection, MSE init+segments, gap failure, resume |
| `confirm_dialog` (C++) | Media section and Quality combo exist |

---

## 12. Out of scope for 0.26.0

* Muxing paired video/audio into one container (`03-engine.md` §9).
* Site-specific extractors (YouTube/Vimeo/Facebook/Instagram and similar).
* Adobe HDS (`.f4m`), live streams, DRM decryption.
* Downloading from a page whose segments are encrypted by the player itself.
* Capturing media that finished before the extension was installed.
