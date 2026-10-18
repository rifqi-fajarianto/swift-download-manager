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

## 4. Rate limiting

- `bandwidthlimit` (KB/s, per download; 0 = unlimited) (E8).
- **[design]**: global token bucket per download; segments share it; sleep
  granularity 50 ms. Global limit is not part of the original settings set.

## 5. Retry and error handling

- Engine-level retry with a remaining-retry counter (E13). **[design]**:
  max 5 retries per segment with exponential backoff (1s, 2s, 4s, 8s, 16s)
  capped at 60s; treat 5xx/connection resets as retryable, 4xx (except 408/429)
  as terminal.
- Errors surface as `Error : <protocol> <code> <reason>` in the error dialog
  (E18 screenshot) and are stored in `errortext`.
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
