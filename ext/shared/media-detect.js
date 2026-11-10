// Media detection for the SwiftDM browser extensions.
//
// Two layers, on purpose:
//
//   1. rankCandidates()  — a pure function over plain descriptor objects. It
//      holds the whole priority rule set, so it can be unit tested in Node
//      without a DOM (tests/test_media_detect.js).
//   2. MediaScanner      — the DOM adapter. It walks video/audio/object/embed,
//      applies the geometric filters that need a live document, and turns each
//      surviving element into a descriptor for rankCandidates().
//
// The heuristics follow docs/spec/08-media-download.md §3.1. No code, asset or
// constant table is taken from any other download manager; the ranking order is
// our own design decision.

const MEDIA_TAGS = ["video", "audio", "object", "embed"];

// Rank values: lower wins. Exported so the tests and the popup can name them.
const RANK = {
  EXACT: 1,     // src equals a URL the caller asked about, or a pinned element
  COVER: 2,     // object-fit: cover (decorative / background video)
  FOCUSED: 3,   // activeElement, or the element under the pointer
  KNOWN: 4,     // src already classified as media by the network sniffer
  PLAYING: 5,   // largest element that is actually playing
  LARGEST: 6,   // largest visible "video-ish" element
  FIRST: 7,     // first visible media element
  HIDDEN: 8,    // first hidden <video>
};

const MIN_VIDEO_WIDTH = 15;
const MIN_VIDEO_HEIGHT = 10;

// A "video-ish" box: between 1:1.35 portrait and 3:1 landscape. This rejects
// 1x1 tracking pixels, 970x90 banners and 4:1 hero strips.
function isVideoShaped(width, height) {
  return width > 0 && height > 0 && width * 1.35 > height && width < height * 3;
}

// True when the element reports that it is currently decoding a stream. Used
// only for ranking: a paused element is still a valid candidate.
function isPlaying(descriptor) {
  if (!descriptor) return false;
  const { paused, currentTime, readyState } = descriptor;
  return paused === false && Number(currentTime) > 0 && Number(readyState ?? 0) >= 2;
}

/**
 * Ranks media candidates and returns the best one.
 *
 * Each descriptor is a plain object:
 *   {
 *     tag: "video" | "audio" | "object" | "embed",
 *     src: string | null,          // currentSrc || src || data, or <source src>
 *     pluginType: string,          // <object>/<embed> type attribute
 *     visible: boolean,            // geometric filters already applied
 *     hidden: boolean,             // in the DOM but not visible
 *     cover: boolean,              // object-fit: cover
 *     pinned: boolean,             // caller asked about this element
 *     focused: boolean,            // activeElement / elementFromPoint
 *     known: boolean,              // src already classified as media
 *     occluded: boolean,           // covered by an opaque element
 *     width: number, height: number,
 *     paused: boolean, currentTime: number, readyState: number,
 *     protected: boolean,          // mediaKeys != null (EME/DRM)
 *   }
 *
 * @param {object[]} descriptors
 * @param {{urls?: (string|null)[], knownUrls?: Set<string>|string[]}} [hint]
 * @returns {object|null} the winning descriptor
 */
function rankCandidates(descriptors, hint = {}) {
  const wanted = new Set();
  for (const url of hint.urls ?? []) {
    if (url) wanted.add(url);
  }
  const known = hint.knownUrls instanceof Set
    ? hint.knownUrls
    : new Set(hint.knownUrls ?? []);

  let exact = null;
  let cover = null;
  let focused = null;
  let knownHit = null;
  let playing = null;
  let playingArea = 0;
  let largest = null;
  let largestArea = 0;
  let first = null;
  let hidden = null;

  for (const item of descriptors ?? []) {
    if (!item) continue;
    const tag = item.tag;
    if (!MEDIA_TAGS.includes(tag)) continue;
    const plugin = tag === "object" || tag === "embed";

    // Plugin elements must actually be media, not an embedded image or text.
    if (plugin) {
      const type = String(item.pluginType ?? "").toLowerCase();
      if (type.startsWith("text/") || type.startsWith("image/")) continue;
      if (tag === "embed" && type !== "application/x-shockwave-flash") continue;
    }

    const src = item.src || null;

    // Rank 1: exact URL match, or the element the caller pinned.
    if (!exact) {
      if ((src && wanted.has(src)) || item.pinned) exact = item;
    }

    // Rank 2: object-fit: cover is a background video, never the main player.
    if (item.cover) {
      if (!cover) cover = item;
      continue;
    }

    // A <video> fed by an audio data URI is not a video.
    if (src && tag === "video" && src.startsWith("data:audio/")) continue;

    // Rank 4: a URL the network sniffer already classified as media.
    if (src && known.has(src)) {
      if (!knownHit) knownHit = item;
      continue;
    }

    if (!item.visible) {
      if (tag === "video" && !hidden && item.hidden) hidden = item;
      continue;
    }
    if (item.occluded) {
      if (tag === "video" && !hidden) hidden = item;
      continue;
    }

    // Rank 3: focused element. Checked after the visibility filters so a
    // hidden activeElement cannot win.
    if (item.focused && !focused) focused = item;

    const width = Number(item.width) || 0;
    const height = Number(item.height) || 0;
    // A <video> that is too small to be a real player is skipped. An <audio>
    // element usually has no box at all, so it is never rejected on size: it
    // simply loses the area comparisons and can still be the "first visible"
    // candidate.
    if (tag === "video" && (width < MIN_VIDEO_WIDTH || height < MIN_VIDEO_HEIGHT)) {
      continue;
    }

    // Rank 5: the largest element that is actually playing. This is the rule
    // that picks the video in front of the user instead of the biggest box.
    const area = width * height;
    if (isVideoShaped(width, height) && isPlaying(item) && area > playingArea) {
      playingArea = area;
      playing = item;
    }

    // Rank 6: the largest visible video-ish element.
    if (isVideoShaped(width, height) && area > largestArea) {
      largestArea = area;
      largest = item;
    }

    // Rank 7: the first visible media element of any shape.
    if (!first) first = item;
  }

  return exact || cover || focused || knownHit || playing || largest || first || hidden || null;
}

/**
 * Summarises a ranked candidate for the popup list and the addMedia payload.
 * Pure, so the popup tests can call it without a DOM.
 */
function describeCandidate(candidate, extra = {}) {
  if (!candidate) return null;
  const protectedStream = candidate.protected === true || extra.protected === true;
  return {
    url: candidate.src ?? extra.url ?? "",
    kind: extra.kind ?? "file",
    quality: extra.quality ?? "",
    elementTag: candidate.tag ?? "",
    durationSec: Number(candidate.duration) > 0 ? Number(candidate.duration) : 0,
    positionSec: Number(candidate.currentTime) > 0 ? Number(candidate.currentTime) : 0,
    protected: protectedStream,
    playing: isPlaying(candidate),
  };
}

// Short display name for a URL: the last path component, ellipsised.
function shortName(url) {
  try {
    const parsed = new URL(String(url));
    const name = parsed.pathname.split("/").filter(Boolean).pop() ?? parsed.hostname;
    return name.length > 32 ? `${name.slice(0, 29)}…` : name;
  } catch {
    return String(url ?? "");
  }
}

/**
 * Turns the merged detection result into the rows the overlay menu renders.
 *
 * One row per downloadable thing, in the order the user is most likely to want
 * them: manifests first (SwiftDM resolves the renditions), then a paired
 * video+audio handoff, then the element that is playing right now, then the
 * reconstructed blob: streams. Each row keeps the payload the background needs,
 * so neither the overlay nor the popup assembles protocol fields itself.
 *
 * Pure: no DOM, no browser API. See docs/spec/08-media-download.md §4.2.
 *
 * @param {{hls?: string[], dash?: string[], pairs?: object[],
 *          dom?: object[], mse?: object[]}} detected
 * @returns {object[]}
 */
function buildMediaItems(detected = {}) {
  const items = [];
  for (const url of new Set(detected.hls ?? [])) {
    items.push({
      label: `HLS playlist · ${shortName(url)}`,
      hint: "Master playlist is resolved by SwiftDM",
      kind: "hls",
      urls: [url],
    });
  }
  for (const url of new Set(detected.dash ?? [])) {
    items.push({
      label: `DASH manifest · ${shortName(url)}`,
      hint: "Pick the video or the audio rendition in the next window",
      kind: "dash",
      urls: [url],
    });
  }
  for (const pair of detected.pairs ?? []) {
    items.push({
      label: `Video + audio · ${shortName(pair.video)}`,
      hint: "Sent to SwiftDM as a pair",
      kind: "file",
      urls: [pair.video, pair.audio],
      group: pair.group,
    });
  }
  for (const candidate of detected.dom ?? []) {
    const size = candidate.videoWidth
      ? `${candidate.videoWidth}×${candidate.videoHeight}`
      : candidate.elementTag || "media";
    items.push({
      label: `Playing now · ${size}`,
      hint: candidate.protected
        ? "Protected (DRM): cannot be downloaded"
        : "The media element in the page",
      kind: "file",
      urls: [candidate.url],
      candidate,
      disabled: candidate.protected === true,
    });
  }
  for (const stream of detected.mse ?? []) {
    const count = Number(stream.segmentCount ?? 0);
    items.push({
      label: `MSE stream · ${count} segment${count === 1 ? "" : "s"}`,
      hint: stream.incomplete
        ? "Assembled from recorded segments; may be incomplete"
        : "Assembled from the segments the page downloaded",
      kind: "mse",
      urls: [stream.initUrl || stream.segments?.[0]?.url || ""],
      stream,
      disabled: !stream.initUrl && !stream.segments?.length,
    });
  }
  return items;
}

/**
 * Builds the `addMedia` candidate for one row of buildMediaItems().
 *
 * The page URL and title come from the sender's tab (the worker is the only
 * place that has them), so the same row works from the overlay and from any
 * other surface.
 *
 * @param {object} item a row from buildMediaItems()
 * @param {{url?: string, title?: string}|null} [tab]
 * @returns {object|null}
 */
function mediaCandidate(item, tab = null) {
  if (!item || typeof item !== "object") return null;
  const page = {
    pageUrl: typeof tab?.url === "string" ? tab.url : "",
    pageTitle: String(tab?.title ?? "").slice(0, 1024),
  };
  if (item.kind === "mse" && item.stream) {
    return {
      url: item.stream.initUrl || item.stream.segments?.[0]?.url || "",
      kind: "mse",
      ...page,
      segments: (item.stream.segments ?? []).map((segment) => ({
        url: segment.url,
        role: segment.role,
        order: segment.order,
        contentType: segment.contentType,
      })),
      durationSec: 0,
      positionSec: 0,
      protected: false,
    };
  }
  if (item.candidate) {
    return { ...item.candidate, kind: item.kind, ...page, group: item.group ?? "" };
  }
  return {
    url: item.urls?.[0] ?? "",
    kind: item.kind,
    ...page,
    group: item.group ?? "",
    durationSec: 0,
    positionSec: 0,
    protected: false,
  };
}

// The DOM adapter is only used in a content script, so it is created lazily:
// requiring this file in Node must not touch `document`.
class MediaScanner {
  /**
   * @param {Document} doc
   * @param {{mouseX?: number, mouseY?: number,
   *          knownUrls?: Set<string>|string[]}} [options]
   */
  constructor(doc, options = {}) {
    this.doc = doc;
    this.mouseX = Number.isFinite(options.mouseX) ? options.mouseX : -1;
    this.mouseY = Number.isFinite(options.mouseY) ? options.mouseY : -1;
    this.knownUrls = options.knownUrls ?? new Set();
    this.occluded = new WeakMap();
  }

  style(element) {
    return this.doc.defaultView.getComputedStyle(element);
  }

  // Port of the occlusion rule in spec §3.1: an element covered at its centre
  // by an opaque, visible element is skipped.
  updateOcclusion(element) {
    if (typeof this.doc.elementsFromPoint !== "function") return false;
    const rect = element.getBoundingClientRect();
    const stack = this.doc.elementsFromPoint(
      (rect.left + rect.right) / 2,
      (rect.top + rect.bottom) / 2
    );
    let occluded = false;
    for (const node of stack ?? []) {
      if (node === element || node.isSameNode?.(element)) break;
      const style = this.style(node);
      if (!style) continue;
      const transparent = /^rgba?\(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*0\s*\)$/
        .test(String(style.backgroundColor ?? ""));
      if (style.visibility === "visible" && style.opacity !== "0" && !transparent) {
        occluded = true;
        break;
      }
    }
    this.occluded.set(element, occluded);
    return occluded;
  }

  // Builds the descriptor for one element, applying every geometric filter
  // that needs a live document.
  describe(element) {
    const tag = String(element.localName ?? "").toLowerCase();
    if (!MEDIA_TAGS.includes(tag)) return null;
    const style = this.style(element);
    if (!style) return null;

    let src = element.currentSrc || element.src || element.data || "";
    if (!src && (tag === "video" || tag === "audio")) {
      const source = element.querySelector?.("source[src]");
      if (source) src = source.src ?? "";
    }

    const width = Number(element.clientWidth) || 0;
    const height = Number(element.clientHeight) || 0;
    const rect = element.getBoundingClientRect();
    const scrollX = Number(this.doc.defaultView?.scrollX) || 0;
    const scrollY = Number(this.doc.defaultView?.scrollY) || 0;
    const offscreen = rect.right + scrollX <= 0 || rect.bottom + scrollY <= 0;

    // An <audio> element usually has no box at all, so the box checks would
    // reject every audio track. Only the style filters apply to it.
    const boxless = tag === "audio";
    const visible = (boxless || (width > 0 && height > 0)) && !offscreen &&
      style.visibility !== "hidden" && style.display !== "none" &&
      style.opacity !== "0";

    return {
      element,
      tag,
      src,
      pluginType: String(element.type ?? ""),
      visible,
      hidden: !visible,
      cover: style.objectFit === "cover",
      pinned: false,
      focused: false,
      known: Boolean(src) && toSet(this.knownUrls).has(src),
      occluded: visible ? this.updateOcclusion(element) : false,
      width,
      height,
      paused: element.paused,
      currentTime: Number(element.currentTime) || 0,
      readyState: Number(element.readyState) || 0,
      duration: Number(element.duration) || 0,
      videoWidth: Number(element.videoWidth) || 0,
      videoHeight: Number(element.videoHeight) || 0,
      // An EME player exposes mediaKeys; the stream is DRM protected.
      protected: element.mediaKeys !== undefined && element.mediaKeys !== null,
    };
  }

  /**
   * Scans the document and returns the best candidate descriptor, or null.
   * @param {{urls?: (string|null)[], element?: Element}} [hint]
   */
  findBest(hint = {}) {
    const elements = this.doc.querySelectorAll(MEDIA_TAGS.join(","));
    const descriptors = [];
    for (const element of elements ?? []) {
      const descriptor = this.describe(element);
      if (!descriptor) continue;
      if (hint.element && element === hint.element) descriptor.pinned = true;
      descriptors.push(descriptor);
    }

    // The focused element, or the element under the pointer, ranks 3.
    const active = this.doc.activeElement;
    let focusTarget = null;
    if (active && MEDIA_TAGS.includes(String(active.localName ?? "").toLowerCase())) {
      focusTarget = active;
    } else if (this.mouseX >= 0 && this.mouseY >= 0 &&
               typeof this.doc.elementFromPoint === "function") {
      const under = this.doc.elementFromPoint(this.mouseX, this.mouseY);
      if (under && MEDIA_TAGS.includes(String(under.localName ?? "").toLowerCase())) {
        focusTarget = under;
      }
    }
    if (focusTarget) {
      for (const descriptor of descriptors) {
        if (descriptor.element === focusTarget) descriptor.focused = true;
      }
    }

    return rankCandidates(descriptors, { urls: hint.urls, knownUrls: this.knownUrls });
  }
}

function toSet(value) {
  return value instanceof Set ? value : new Set(value ?? []);
}

const mediaDetect = {
  MEDIA_TAGS,
  RANK,
  MIN_VIDEO_WIDTH,
  MIN_VIDEO_HEIGHT,
  isVideoShaped,
  isPlaying,
  rankCandidates,
  describeCandidate,
  shortName,
  buildMediaItems,
  mediaCandidate,
  MediaScanner,
};

// Works both as a classic content-script file (defines a global) and as a
// CommonJS module for the Node test harness.
if (typeof module !== "undefined" && module.exports) {
  module.exports = mediaDetect;
} else if (typeof globalThis !== "undefined") {
  globalThis.swiftdmMediaDetect = mediaDetect;
}
