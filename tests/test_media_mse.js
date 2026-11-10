// Headless checks for ext/shared/media-mse.js — the MSE (blob:) stream
// reconstruction logic. No browser, no DOM: the module is pure.

const path = require("node:path");

const mse = require(path.join(__dirname, "..", "ext", "shared", "media-mse.js"));

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

{
  check(mse.isSegmentMime("video/iso.segment"), "video/iso.segment is a segment type");
  check(mse.isSegmentMime("video/mp4; codecs=avc1"), "a video/mp4 with parameters is a segment type");
  check(!mse.isSegmentMime("application/vnd.apple.mpegurl"), "an HLS manifest is not a segment");
  check(!mse.isSegmentMime("application/dash+xml"), "a DASH manifest is not a segment");
  check(!mse.isSegmentMime("text/html"), "HTML is not a segment");

  check(mse.isSegmentUrl("https://cdn/a/seg-00001.m4s"), ".m4s is a segment URL");
  check(mse.isSegmentUrl("https://cdn/a/chunk-3"), "/chunk- is a segment URL");
  check(mse.isSegmentUrl("https://cdn/a/range/1024-2048"), "/range/<n> is a segment URL");
  check(!mse.isSegmentUrl("https://cdn/a/index.html"), "an HTML page is not a segment URL");

  check(mse.isInitUrl("https://cdn/a/init.mp4"), "/init.mp4 is an init URL");
  check(mse.isInitUrl("https://cdn/a/header"), "/header is an init URL");
  check(!mse.isInitUrl("https://cdn/a/seg-1.m4s"), "a media segment is not an init URL");

  check(mse.streamKindOf("video/mp4", "") === "video", "video/mp4 is video");
  check(mse.streamKindOf("audio/mp4", "") === "audio", "audio/mp4 is audio");
  check(mse.streamKindOf("", "https://cdn/a/track.m4a") === "audio", ".m4a is audio");
}

// A POST is not a segment; a manifest is not a segment; an init segment is
// classified as init.
{
  check(mse.classifySegment({ url: "https://cdn/a/seg-1.m4s", method: "POST" }).segment === false,
    "a POST is not a media segment");
  check(mse.classifySegment({ url: "https://cdn/a/x.m3u8", contentType: "application/vnd.apple.mpegurl" }).segment === false,
    "a playlist is not a media segment");
  const init = mse.classifySegment({ url: "https://cdn/a/init.mp4", contentType: "video/mp4" });
  check(init.segment === true && init.role === "init", "init.mp4 is an init segment");
  const media = mse.classifySegment({ url: "https://cdn/a/seg-2.m4s", contentType: "video/mp4" });
  check(media.segment === true && media.role === "media" && media.kind === "video",
    "seg-2.m4s is a media video segment");

  // A whole progressive file is a track, not a segment: same MIME, different
  // URL shape.
  check(mse.classifySegment({ url: "https://cdn/sd/video.mp4", contentType: "video/mp4" }).segment === false,
    "a progressive video.mp4 is a track, not a segment");
  check(mse.classifySegment({ url: "https://cdn/a/seg-1.m4s", contentType: "video/iso.segment" }).segment === true,
    "an explicit segment MIME is always a segment");
  check(mse.classifySegment({
    url: "https://cdn/media.bin",
    contentType: "video/mp4",
    contentRange: "bytes 0-1023/2048"
  }).segment === true, "a partial response is a segment");
}

// ---------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------

{
  const a = mse.groupKey("https://cdn.example/v/seg-1.m4s");
  const b = mse.groupKey("https://cdn.example/v/seg-2.m4s");
  const c = mse.groupKey("https://cdn.example/a/seg-1.m4s");
  check(a === b, "segments in one directory share a group");
  check(a !== c, "different directories are different groups");
  check(mse.groupKey("not a url") === "not a url", "an unparsable URL is its own group");
}

// ---------------------------------------------------------------------------
// SegmentLog
// ---------------------------------------------------------------------------

// Order: the init segment is always first, then media segments by counter.
{
  const log = new mse.SegmentLog();
  log.add({ url: "https://cdn/v/seg-2.m4s", contentType: "video/mp4", order: 2 });
  log.add({ url: "https://cdn/v/init.mp4", contentType: "video/mp4", order: 1 });
  log.add({ url: "https://cdn/v/seg-1.m4s", contentType: "video/mp4", order: 0 });

  const key = mse.groupKey("https://cdn/v/seg-1.m4s");
  const listed = log.list(key);
  check(listed.segments.length === 3, "three segments were recorded");
  check(listed.segments[0].role === "init", "the init segment is first");
  check(listed.segments[1].url.endsWith("seg-1.m4s"), "media segments keep their order");
  check(listed.segments[2].url.endsWith("seg-2.m4s"), "media segments keep their order (2)");
}

// Duplicates are ignored.
{
  const log = new mse.SegmentLog();
  const record = { url: "https://cdn/v/seg-1.m4s", contentType: "video/mp4", order: 1 };
  check(log.add(record) === true, "the first record is kept");
  check(log.add(record) === false, "the same URL twice is ignored");
  check(log.list(mse.groupKey(record.url)).segments.length === 1, "one record after a duplicate");
}

// A video track and an audio track in different directories stay separate.
{
  const log = new mse.SegmentLog();
  log.add({ url: "https://cdn/video/seg-1.m4s", contentType: "video/mp4", order: 1 });
  log.add({ url: "https://cdn/audio/seg-1.m4s", contentType: "audio/mp4", order: 2 });
  log.add({ url: "https://cdn/audio/seg-2.m4s", contentType: "audio/mp4", order: 3 });
  const candidates = log.candidates();
  check(candidates.length === 2, "two streams are reported");
  check(candidates[0].kind === "audio" && candidates[0].segmentCount === 2,
    "the longer (audio) stream sorts first");
  check(candidates[1].kind === "video" && candidates[1].segmentCount === 1,
    "the video stream is reported separately");
}

// The cap drops the oldest media segments but keeps the init segment.
{
  const log = new mse.SegmentLog({ maxSegments: 4 });
  log.add({ url: "https://cdn/v/init.mp4", contentType: "video/mp4", order: 0 });
  for (let i = 1; i <= 8; i++) {
    log.add({ url: `https://cdn/v/seg-${i}.m4s`, contentType: "video/mp4", order: i });
  }
  const listed = log.list(mse.groupKey("https://cdn/v/seg-1.m4s"));
  check(listed.segments.length === 4, "the log honours maxSegments");
  check(listed.segments[0].role === "init", "the init segment survives the cap");
  check(listed.incomplete === true, "a trimmed log is marked incomplete");
  check(listed.dropped === 5, "the dropped count is reported");
}

// The byte cap also trims, and a zero-length segment does not consume budget.
{
  const log = new mse.SegmentLog({ maxBytes: 1000 });
  log.add({ url: "https://cdn/v/init.mp4", contentType: "video/mp4", contentLength: 100, order: 0 });
  for (let i = 1; i <= 5; i++) {
    log.add({ url: `https://cdn/v/seg-${i}.m4s`, contentType: "video/mp4", contentLength: 400, order: i });
  }
  const listed = log.list(mse.groupKey("https://cdn/v/seg-1.m4s"));
  check(listed.incomplete === true, "the byte cap marks the stream incomplete");
  const total = listed.segments.reduce((sum, item) => sum + item.bytes, 0);
  check(total <= 1000, "the byte cap is respected");
}

// Expiry removes a stream.
{
  let now = 1000;
  const log = new mse.SegmentLog({ ttlMs: 100, now: () => now });
  log.add({ url: "https://cdn/v/seg-1.m4s", contentType: "video/mp4", order: 1 });
  check(log.candidates().length === 1, "a fresh stream is listed");
  now += 1000;
  check(log.candidates().length === 0, "an expired stream is pruned");
}

// A stream with only an init segment is not a candidate: there is nothing to
// download yet.
{
  const log = new mse.SegmentLog();
  log.add({ url: "https://cdn/v/init.mp4", contentType: "video/mp4", order: 0 });
  check(log.candidates().length === 0, "an init-only stream is not offered");
}

// clear() resets both the streams and the counter.
{
  const log = new mse.SegmentLog();
  log.add({ url: "https://cdn/v/seg-1.m4s", contentType: "video/mp4", order: 5 });
  log.clear();
  check(log.candidates().length === 0, "clear() empties the log");
  log.add({ url: "https://cdn/v/seg-9.m4s", contentType: "video/mp4" });
  const listed = log.list(mse.groupKey("https://cdn/v/seg-9.m4s"));
  check(listed.segments.length === 1, "the log works again after clear()");
}

// A media segment without an explicit order gets one from the counter.
{
  const log = new mse.SegmentLog();
  log.add({ url: "https://cdn/v/a.m4s", contentType: "video/mp4" });
  log.add({ url: "https://cdn/v/b.m4s", contentType: "video/mp4" });
  const listed = log.list(mse.groupKey("https://cdn/v/a.m4s"));
  check(listed.segments[0].url.endsWith("a.m4s"), "insertion order is preserved");
  check(listed.segments[1].url.endsWith("b.m4s"), "insertion order is preserved (2)");
}

// ---------------------------------------------------------------------------

if (failures > 0) {
  console.error(`media_mse: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("media_mse: all checks passed");
