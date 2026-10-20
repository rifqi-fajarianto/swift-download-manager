# 03 — Download engine specification

Observations from RE (E12–E14 plus static strings); designs marked **[design]**
are our own choices where the original is UNKNOWN (E23).

## 1. Task lifecycle

```
 Queued -> Connecting -> Downloading -> (Complete | Paused | Error)
             ^                |  ^
             |                v  |
             +---- Retry <----+  +---- Resume (re-probe, restore segments)
```

- **Probe** **[design]**: send `GET` with `Range: bytes=0-` (the original
  conditionally adds a `Range` header, E12). From the response we derive:
  final URL (after redirects), `Content-Length`/`Content-Range` total size,
  `Accept-Ranges`, `Content-Disposition` filename, MIME type, ETag /
  Last-Modified (resume validation), and `ServerCreationDate` metadata if the
  setting is enabled (E8).
- **Not resumable**: server without range support → single connection,
  restart-from-zero on failure, `resumable = 0` in the UI.
- **Resume**: restore segment table from `segments.json`; re-probe to validate
  file identity (size and ETag/Last-Modified); if changed → restart (original
  keeps `urla`/`hittitle` for this; strings reference re-download paths).

## 2. Segmentation

- Number of segments = min(`connections` setting of the download, server
  support, and a minimum chunk size) **[design: minimum chunk 1 MiB;** original
  minimum UNKNOWN]**.
- The file is split into contiguous ranges `[start, end]` roughly equal;
  the segment state keeps per-range progress (`done` bytes).
- **ConnectionsAtOnce = 1** adds one connection when the previous one either
  finishes or proves too slow (original setting "Create Additional Connections
  One by One"); **0** starts all connections immediately (E8).
- Segment progress is persisted periodically and on pause/stop so a crash
  loses at most a few seconds of accounting; `.bak` fallback mirrors E14.

## 3. I/O and files

- Each segment writes into the destination temp file at its own offset (sparse
  writes), OR into per-segment part files that are merged at the end; the
  original does per-segment temp files with `.seg.x` suffix and merges (E14).
  **[design]**: single file with positioned writes (`pwrite`), merged
  implicitly; simpler and crash-safe with the JSON state. A `.part` suffix is
  used while incomplete; extension is dropped on completion.
- Temp directory: global default from settings, per-download `temppath`
  override (E8, E22).
- Destination: `folderpath` + `filename`, with per-category subfolders when
  `CategoryFolders` is enabled (E8/E19).
- Temp workspace lifecycle: one directory per download
  (`download-<hash>/` with `<filename>.part` and `segments.json`) is kept for
  resume while the item exists (pause, failure, quit) and removed on
  completion or when the user deletes the list row; layout and cleanup paths
  are in `02-data-model.md` §2.

## 4. Rate limiting

- `bandwidthlimit` (KB/s, per download) with the per-download
  `bandwidthlimit_enabled` toggle; the settings dialog has the global
  `BandWidthLimit` + `BandWidthLimitEnabled` pair (E8). 0.18.1: the toggle is
  authoritative — off means unlimited and the stored value is kept editable in
  the Options tab (and in settings) for later. **[design: 0.18.1]** new
  downloads are seeded from the global pair, then overridden per download.
- Preset values offered in both editors: 500 KB/s, 1 MB/s, 5 MB/s, 10 MB/s,
  20 MB/s, 50 MB/s (plus a custom KB/s field).
- While a transfer runs, the Download tab shows `Speed limit: <value>`
  in orange next to the Bandwidth value when the limiter is on.
- **[design]**: per-download receive-rate gate; segments share it. The global
  limit is not part of the original settings set.
  `stream_download` (unknown size, FTP, HLS) uses libcurl's receive-rate cap so
  the limiter applies to those paths as well.
- Configs and history rows written before 0.18.1 load with the toggle inferred
  from a non-zero stored limit (the old rule was "non-zero = limit on").
- **[design: 0.18.2]** The active transfer shares an atomic per-download byte
  rate with the UI. Receive callbacks pace data before writing it, so a change
  (including zero = unlimited) takes effect without restarting the transfer
  across segmented HTTP/HTTPS, sequential HTTP, FTP and each HLS resource.
  Pause/cancel interrupts a pacing wait. The history value is updated when the
  debounced UI edit is applied. Other Options fields still take effect on
  resume only.

## 5. Retry and error handling

- Engine-level retry with a remaining-retry counter (E13). **[design]**:
  max 5 retries per segment with exponential backoff (1s, 2s, 4s, 8s, 16s)
  capped at 60s; treat 5xx/connection resets as retryable, 4xx (except 408/429)
  as terminal.
- Errors surface as `Error : <protocol> <code> <reason>` in the error dialog
  (E18 screenshot) and are stored in `errortext`. **[design: 0.12.0]** the
  message also quotes the host: the first `1024` bytes of a 4xx/5xx body are
  summarized (JSON `value`/`message`, HTML `<title>`/text, otherwise one
  collapsed line) and appended after ` - ` (max `300` bytes). This is what
  makes hosts that explain themselves — pixeldrain's `hotlink_detected` /
  `file_rate_limited_captcha_required`, connection limits — understandable in
  the UI. Refused bodies are never written into the partial file (previously a
  4xx/3xx body could land at the segment offset).
- **[design: 0.12.0]** when parallel ranges are refused with `403` and the plan
  has more than one segment, the remaining ranges are retried once on a single
  connection (`ConnectionsAtOnce` semantics) after a 1 s pause. Hosts with a
  per-IP connection limit (pixeldrain's free tier answers
  `403 max_concurrent_downloads`) therefore still complete instead of failing.
  CAPTCHA-gated refusals are not bypassed: the user must clear them on the
  host's page (the error dialog offers `Open Page in Browser`).
- **[design: 0.13.0]** the probe resolves known download interstitials first
  (04-protocols.md §5): `ProbeResult::download_url` then names the URL the
  transfers must use, while filename, size, validators, segmentation and resume
  state all describe the real file.
- HTTP specifics supported: chunked transfer decoding, redirect following
  (limit **[design: 15]**), HTTP proxy CONNECT tunneling, SOCKS4/SOCKS5
  (E15), cookies, custom headers per download (`headers` table), Basic/Digest
  auth (NTLM via curl if available; otherwise not offered for v1).

## 6. Speed accounting

- Bandwidth column shows current aggregate speed (bytes/s, human units);
  Remaining Time = remaining bytes / current speed, falling back to `--` when
  speed is 0. Table refresh interval **[design: 1 s]**.

## 7. Scheduling and limits **[design]**

- Queue: sequential; downloads not complete count against the "simultaneous
  downloads" concept only when active. v1: unlimited simultaneous active
  downloads (original behavior UNKNOWN), but a setting is reserved.

## 8. Threading **[design]**

- libcurl multi (epoll backend) driven by one I/O thread; worker-side disk
  writes via positioned writes; UI communicates via queued signals (Qt).

## 9. FTP, HLS, and adaptive media (0.11.0)

### FTP

`ftp://` and `ftps://` URLs bypass the segmented HTTP path:

1. **Probe** — libcurl `NOBODY` maps to `SIZE`, so `probe_url` reports the file
   size (an error is reported verbatim when the server refuses `SIZE`). FTP has
   no HTTP status code; `accept_ranges` mirrors "size known" because resume uses
   `REST` rather than byte ranges.
2. **Transfer** — one connection writes the part file from offset 0, or, when a
   part file exists, seeks to its size and sets `CURLOPT_RESUME_FROM_LARGE`
   (libcurl then issues `REST <offset>` before `RETR`). `connections` is
   ignored on purpose.
3. **Completion** — the part file is moved to the final name and the workspace
   is removed, exactly like the HTTP path. A paused/failed transfer keeps the
   part file so the next attempt resumes.

### HLS (m3u8)

Urls whose MIME type contains `mpegurl` or whose path ends in `.m3u8` are
treated as HLS by `engine/hls.cpp`:

- **Playlist parsing** — `#EXTM3U`, `#EXT-X-STREAM-INF` (bandwidth + URI),
  `#EXTINF` segment URIs, `#EXT-X-MAP` init segment, `#EXT-X-KEY` (any method
  other than `NONE` marks the playlist encrypted). URIs are resolved against the
  post-redirect playlist URL (absolute, protocol-relative, root-relative, or
  relative).
- **Variant selection** — a master playlist picks the highest `BANDWIDTH`
  variant (one level; nested masters are rejected).
- **Download** — the `EXT-X-MAP` init segment (if any, fMP4) is written first,
  then every media segment sequentially into the part file, which becomes one
  `.ts` file. Progress follows the segment index; byte progress is reported when
  known.
- **Resume** — the workspace keeps `hls_progress` (index of the next segment)
  and the concatenated part file; a resumed run appends from that index. Segments
  already written are never re-fetched.
- **Unsupported** — encrypted playlists fail with "encrypted HLS playlists are
  not supported yet" instead of writing undecryptable data.

### MKV mux (planned)

Muxing the paired video/audio tracks (or an HLS variant with separate audio) into
one Matroska file stays out of scope for 0.11.0: the project has no muxer
dependency, and writing one is a release of its own. The planned shape:

1. The extension hands both tracks off with a shared `group` token; the app
   stores it (`downloads.groupid`) so the pair survives a restart.
2. A future engine step (v3) reads both completed files plus `groupid`, and
   remuxes them into MKV. The realistic implementation is a shell-out to
   `ffmpeg -c copy` when it is installed, with a hand-written EBML writer as the
   fallback (both tracks are usually already Matroska/WebM-friendly codecs, so a
   copy remux is enough and re-encoding is not required).
3. Until then the app documents the pair in Properties and leaves both files
   side by side — no silent data loss and no half-muxed output.
