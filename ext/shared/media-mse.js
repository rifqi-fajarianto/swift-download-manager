// Reconstruction of Media Source Extensions (MSE) streams.
//
// When a player feeds <video> from MediaSource, `video.src` is a `blob:` URL and
// no single downloadable file exists. What does exist is the sequence of
// segment requests the player made. This module holds the pure logic that turns
// those observed requests into an ordered, downloadable segment list:
//
//   • classifySegment()  — init or media, video or audio
//   • groupKey()         — which stream a segment belongs to
//   • SegmentLog         — the bounded, ordered per-stream record
//
// Nothing here touches the DOM or a browser API, so it runs unchanged in the
// service worker, the event page and the Node test harness.
// See docs/spec/08-media-download.md §6.

// Content types that are segment payloads rather than documents.
const SEGMENT_MIME_TYPES = [
  "video/iso.segment",
  "video/mp4",
  "video/webm",
  "video/quicktime",
  "audio/mp4",
  "audio/webm",
  "audio/aac",
  "audio/mpeg",
];

// URL shapes used by the common segmenters (DASH/HLS/CMAF).
const SEGMENT_URL_PATTERNS = [
  /\.m4s(?:[?#]|$)/i,
  /\.m4v(?:[?#]|$)/i,
  /\.m4a(?:[?#]|$)/i,
  /\.cmfv(?:[?#]|$)/i,
  /\.cmfa(?:[?#]|$)/i,
  /\.ts(?:[?#]|$)/i,
  /\/seg(?:ment)?[-_/]/i,
  /\/chunk[-_/]/i,
  /\/frag(?:ment)?[-_/]/i,
  /\/range\/\d+/i,
];

// The initialisation segment carries ftyp+moov. Players usually ask for it
// once, before any media segment, and often name it explicitly.
const INIT_URL_PATTERNS = [
  /\/init(?:\.mp4)?(?:[?#]|$)/i,
  /[?&]init=1(?:&|$)/i,
  /\/header(?:[?#]|$)/i,
];

const MAX_SEGMENTS = 2000;
const MAX_BYTES = 512 * 1024;
const DEFAULT_TTL_MS = 30 * 60 * 1000;

function normalizedMime(contentType) {
  return String(contentType ?? "").split(";")[0].trim().toLowerCase();
}

// True when the response looks like a media segment payload.
function isSegmentMime(contentType) {
  const mime = normalizedMime(contentType);
  if (!mime) return false;
  if (SEGMENT_MIME_TYPES.includes(mime)) return true;
  // Any video/* or audio/* payload that is not a manifest/document counts.
  if ((mime.startsWith("video/") || mime.startsWith("audio/")) &&
      !mime.includes("html") && !mime.includes("mpegurl") && !mime.includes("dash")) {
    return true;
  }
  return false;
}

function isSegmentUrl(url) {
  const text = String(url ?? "");
  if (!text) return false;
  return SEGMENT_URL_PATTERNS.some((pattern) => pattern.test(text));
}

function isInitUrl(url) {
  const text = String(url ?? "");
  if (!text) return false;
  return INIT_URL_PATTERNS.some((pattern) => pattern.test(text));
}

// Video or audio, from the MIME type first and the URL as a fallback.
function streamKindOf(contentType, url) {
  const mime = normalizedMime(contentType);
  if (mime.startsWith("video/")) return "video";
  if (mime.startsWith("audio/")) return "audio";
  const text = String(url ?? "").toLowerCase();
  if (/\.(m4a|cmfa|aac|mp3)(?:[?#]|$)/.test(text) || /[?&](?:type|kind)=audio/.test(text)) {
    return "audio";
  }
  return "video";
}

/**
 * Classifies one observed response.
 *
 * A plain `video/mp4` response is ambiguous: it is both what a progressive
 * track looks like and what a CMAF segment looks like. The URL shape decides,
 * because a whole file and a segment differ there:
 *
 *   • an explicit segment MIME (`video/iso.segment`) is always a segment;
 *   • a partial response (`Content-Range`) is a byte range of one resource and
 *     is treated as a segment;
 *   • otherwise the URL must look like a segment (`/seg/`, `.m4s`, `/chunk/`,
 *     `.ts`, …). A plain `video.mp4` stays a downloadable track.
 *
 * @param {{url: string, contentType?: string, contentLength?: number,
 *          contentRange?: string, method?: string}} record
 * @returns {{segment: boolean, role: "init"|"media"|"", kind: "video"|"audio"}}
 */
function classifySegment(record) {
  const url = String(record?.url ?? "");
  if (!url) return { segment: false, role: "", kind: "" };
  const method = String(record?.method ?? "GET").toUpperCase();
  if (method !== "GET") return { segment: false, role: "", kind: "" };

  const mime = normalizedMime(record?.contentType);
  const explicitSegmentMime = mime === "video/iso.segment" || mime === "audio/iso.segment";
  const ranged = Boolean(record?.contentRange);
  // An initialisation segment is always part of a segmented stream, whatever
  // its name or MIME type says.
  const init = isInitUrl(url) && isSegmentMime(mime);
  const byUrl = isSegmentUrl(url) && isSegmentMime(mime);
  if (!explicitSegmentMime && !ranged && !init && !byUrl) {
    return { segment: false, role: "", kind: "" };
  }

  const role = init ? "init" : "media";
  return { segment: true, role, kind: streamKindOf(record?.contentType, url) };
}

// Groups segments that belong to one stream: same host, same directory-ish
// path prefix. Players emit numbered segments under one prefix, so this keeps
// the video and audio tracks of one title apart without guessing from headers.
function groupKey(url) {
  try {
    const parsed = new URL(String(url));
    const segments = parsed.pathname.split("/").filter(Boolean);
    // Drop the last component (the segment file itself) and keep the parent
    // directory; fall back to the host when the path is flat.
    const prefix = segments.slice(0, Math.max(0, segments.length - 1)).join("/");
    return `${parsed.host}/${prefix}`;
  } catch {
    return String(url ?? "");
  }
}

/**
 * Ordered, bounded record of the segments seen for one tab.
 *
 * Ordering comes from a monotonic counter supplied by the caller (the page hook
 * sees the page's own fetch/XHR calls, which is the closest observable proxy
 * for the player's append order). Records that arrive without a counter keep
 * insertion order.
 */
class SegmentLog {
  constructor(options = {}) {
    this.maxSegments = options.maxSegments ?? MAX_SEGMENTS;
    this.maxBytes = options.maxBytes ?? MAX_BYTES;
    this.ttlMs = options.ttlMs ?? DEFAULT_TTL_MS;
    this.now = options.now ?? (() => Date.now());
    this.streams = new Map();  // group key -> { records, bytes, dropped, updatedAt }
    this.sequence = 0;
  }

  /**
   * Records one response. Returns true when it was kept.
   * `order` is optional; when absent the log assigns the next sequence number.
   */
  add(record) {
    const classified = classifySegment(record);
    if (!classified.segment) return false;
    const url = String(record.url);
    const key = groupKey(url);
    const entry = this.entry(key);
    if (entry.records.some((item) => item.url === url)) return false;

    const order = Number.isFinite(record.order) ? Number(record.order) : ++this.sequence;
    if (Number.isFinite(record.order)) this.sequence = Math.max(this.sequence, order);

    const bytes = Math.max(0, Number(record.contentLength) || 0);
    entry.records.push({
      url,
      role: classified.role,
      kind: classified.kind,
      order,
      contentType: normalizedMime(record.contentType),
      bytes,
      at: this.now(),
    });
    entry.bytes += bytes;
    entry.updatedAt = this.now();

    // Bounded: drop the oldest media segments first (never the init segment,
    // which is required to play the file at all), and mark the stream
    // incomplete so the UI can say so.
    while (entry.records.length > this.maxSegments || entry.bytes > this.maxBytes) {
      const index = entry.records.findIndex((item) => item.role !== "init");
      if (index < 0) break;
      entry.bytes -= entry.records[index].bytes;
      entry.records.splice(index, 1);
      entry.dropped += 1;
    }
    return true;
  }

  entry(key) {
    let entry = this.streams.get(key);
    if (!entry) {
      entry = { key, records: [], bytes: 0, dropped: 0, updatedAt: this.now() };
      this.streams.set(key, entry);
    }
    return entry;
  }

  // Removes expired streams and returns the live ones.
  prune() {
    const now = this.now();
    for (const [key, entry] of this.streams) {
      if (now - entry.updatedAt > this.ttlMs) this.streams.delete(key);
    }
    return this.streams;
  }

  clear() {
    this.streams.clear();
    this.sequence = 0;
  }

  /**
   * The segment list for one stream, ordered for playback.
   * The init segment is always first, then media segments by their counter.
   */
  list(key) {
    const entry = this.streams.get(key);
    if (!entry) return { segments: [], incomplete: false, dropped: 0, kind: "" };
    const ordered = [...entry.records].sort((a, b) => {
      if (a.role === "init" && b.role !== "init") return -1;
      if (b.role === "init" && a.role !== "init") return 1;
      return a.order - b.order;
    });
    const kind = ordered.find((item) => item.kind)?.kind ?? "";
    return {
      segments: ordered.map((item, index) => ({ ...item, index })),
      incomplete: entry.dropped > 0,
      dropped: entry.dropped,
      kind,
    };
  }

  // Every stream with at least one media segment, best (longest) first.
  candidates() {
    const out = [];
    for (const key of this.prune().keys()) {
      const listed = this.list(key);
      const media = listed.segments.filter((item) => item.role === "media");
      if (media.length === 0) continue;
      out.push({
        key,
        kind: listed.kind,
        segmentCount: media.length,
        initUrl: listed.segments.find((item) => item.role === "init")?.url ?? "",
        incomplete: listed.incomplete,
        dropped: listed.dropped,
        segments: listed.segments,
      });
    }
    out.sort((a, b) => b.segmentCount - a.segmentCount);
    return out;
  }
}

const mediaMse = {
  SEGMENT_MIME_TYPES,
  MAX_SEGMENTS,
  MAX_BYTES,
  DEFAULT_TTL_MS,
  isSegmentMime,
  isSegmentUrl,
  isInitUrl,
  streamKindOf,
  classifySegment,
  groupKey,
  SegmentLog,
};

if (typeof module !== "undefined" && module.exports) {
  module.exports = mediaMse;
} else if (typeof globalThis !== "undefined") {
  globalThis.swiftdmMediaMse = mediaMse;
}
