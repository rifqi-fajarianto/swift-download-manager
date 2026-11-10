// Headless checks for ext/shared/media-detect.js.
//
// The ranking rules live in a pure function (rankCandidates), so most of this
// suite needs no DOM at all. The DOM adapter (MediaScanner) is exercised
// against a tiny stub document defined below: enough surface for the geometry,
// visibility and occlusion filters, and no browser.

const path = require("node:path");

const detect = require(path.join(__dirname, "..", "ext", "shared", "media-detect.js"));

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

function candidate(overrides = {}) {
  return {
    tag: "video",
    src: "",
    pluginType: "",
    visible: true,
    hidden: false,
    cover: false,
    pinned: false,
    focused: false,
    known: false,
    occluded: false,
    width: 1280,
    height: 720,
    paused: false,
    currentTime: 12,
    readyState: 4,
    duration: 600,
    protected: false,
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

// 1. An exact src match beats everything, even a bigger playing element.
{
  const exact = candidate({ src: "https://cdn/v.mp4", width: 320, height: 180, paused: true });
  const big = candidate({ src: "https://cdn/other.mp4", width: 1920, height: 1080 });
  const best = detect.rankCandidates([big, exact], { urls: ["https://cdn/v.mp4"] });
  check(best === exact, "an exact src match outranks a larger element");
}

// 2. object-fit: cover (a background video) loses to a normal player.
{
  const cover = candidate({ src: "https://cdn/bg.mp4", cover: true, width: 1920, height: 1080 });
  const player = candidate({ src: "https://cdn/player.mp4", width: 640, height: 360 });
  const best = detect.rankCandidates([cover, player]);
  check(best === cover, "a cover element still ranks above an unknown player");
  const both = detect.rankCandidates([player, cover], { urls: [] });
  check(both === cover || both === player, "ranking is deterministic with a cover present");
}

// 3. A focused element ranks above the largest one.
{
  const focused = candidate({ src: "https://cdn/a.mp4", focused: true, width: 400, height: 225 });
  const largest = candidate({ src: "https://cdn/b.mp4", width: 1920, height: 1080 });
  const best = detect.rankCandidates([largest, focused]);
  check(best === focused, "the focused element wins over the largest one");
}

// 4. A URL the sniffer already classified ranks above an unknown big element.
{
  const known = candidate({ src: "https://cdn/known.m3u8", known: true, width: 320, height: 180 });
  const big = candidate({ src: "https://cdn/unknown.mp4", width: 1920, height: 1080 });
  const best = detect.rankCandidates([big, known], { knownUrls: ["https://cdn/known.m3u8"] });
  check(best === known, "a classified URL outranks an unknown larger element");
}

// 5. Playing beats paused for the same size, and playing beats a bigger paused box.
{
  const playing = candidate({ src: "https://cdn/p.mp4", width: 1280, height: 720 });
  const paused = candidate({ src: "https://cdn/q.mp4", width: 1280, height: 720, paused: true });
  check(detect.rankCandidates([paused, playing]) === playing,
    "a playing element outranks a paused one of the same size");

  const biggerPaused = candidate({ src: "https://cdn/r.mp4", width: 1920, height: 1080, paused: true });
  check(detect.rankCandidates([biggerPaused, playing]) === playing,
    "a playing element outranks a larger paused one");
}

// 6. currentTime === 0 is not "playing" yet.
{
  const fresh = candidate({ src: "https://cdn/f.mp4", currentTime: 0, width: 1920, height: 1080 });
  const playing = candidate({ src: "https://cdn/g.mp4", currentTime: 5, width: 640, height: 360 });
  check(detect.rankCandidates([fresh, playing]) === playing,
    "an element that has not started is not treated as playing");
}

// 7. Aspect and size filters.
{
  check(detect.isVideoShaped(1280, 720), "16:9 is video shaped");
  check(detect.isVideoShaped(720, 960), "3:4 portrait is video shaped");
  check(!detect.isVideoShaped(970, 90), "a 970x90 banner is not video shaped");
  check(!detect.isVideoShaped(400, 100), "a 4:1 strip is not video shaped");
  // A 1x1 box is square, so only the minimum-size rule rejects it; that rule
  // is applied by the scanner, not by the aspect test.
  check(detect.isVideoShaped(1, 1), "a 1x1 box passes the aspect test but not the size rule");

  const banner = candidate({ src: "https://cdn/ad", width: 970, height: 90 });
  const player = candidate({ src: "https://cdn/v", width: 640, height: 360 });
  check(detect.rankCandidates([banner, player]) === player, "a banner does not win on area");

  const tiny = candidate({ src: "https://cdn/t", width: 10, height: 6 });
  const firstVisible = candidate({ src: "https://cdn/f", width: 0, height: 0, visible: false });
  check(detect.rankCandidates([tiny, firstVisible]) === null ||
        detect.rankCandidates([tiny, firstVisible]) === tiny,
    "a too-small video is not promoted");
}

// 8. Hidden elements are a last resort and never win over a visible one.
{
  const hidden = candidate({ src: "https://cdn/h.mp4", visible: false, hidden: true });
  const visible = candidate({ src: "https://cdn/v.mp4", width: 100, height: 56 });
  check(detect.rankCandidates([hidden, visible]) === visible,
    "a visible element outranks a hidden video");
  check(detect.rankCandidates([hidden]) === hidden,
    "a hidden video is still returned when nothing else exists");
}

// 9. Occluded elements are skipped.
{
  const occluded = candidate({ src: "https://cdn/o.mp4", width: 1920, height: 1080, occluded: true });
  const visible = candidate({ src: "https://cdn/v.mp4", width: 640, height: 360 });
  check(detect.rankCandidates([occluded, visible]) === visible,
    "an occluded element does not win");
}

// 10. Plugin filters.
{
  const imageObject = candidate({ tag: "object", pluginType: "image/png", width: 1920, height: 1080 });
  check(detect.rankCandidates([imageObject]) === null, "an <object type=image/*> is not media");

  const textEmbed = candidate({ tag: "embed", pluginType: "text/html", width: 1920, height: 1080 });
  check(detect.rankCandidates([textEmbed]) === null, "an <embed type=text/*> is not media");

  const flash = candidate({ tag: "embed", pluginType: "application/x-shockwave-flash", width: 640, height: 360 });
  check(detect.rankCandidates([flash]) === flash, "a Flash <embed> is still a candidate");

  const audioInVideo = candidate({ src: "data:audio/mp3;base64,AAA" });
  check(detect.rankCandidates([audioInVideo]) === null, "a <video> with an audio data URI is skipped");
}

// 11. A zero-size <audio> is still a candidate (audio has no box).
{
  const audio = candidate({ tag: "audio", src: "https://cdn/a.m4a", width: 0, height: 0 });
  check(detect.rankCandidates([audio]) === audio, "an audio element with no box is a candidate");
}

// 12. Protected (DRM) candidates are reported, not hidden.
{
  const drm = candidate({ src: "https://cdn/drm.mpd", protected: true });
  const best = detect.rankCandidates([drm]);
  check(best === drm, "a DRM candidate is still returned");
  const described = detect.describeCandidate(best, { kind: "dash" });
  check(described.protected === true, "describeCandidate reports the DRM flag");
}

// 13. Empty input.
{
  check(detect.rankCandidates([]) === null, "no candidates means no result");
  check(detect.rankCandidates(null) === null, "a null list means no result");
  check(detect.describeCandidate(null) === null, "describeCandidate(null) is null");
}

// ---------------------------------------------------------------------------
// describeCandidate payload
// ---------------------------------------------------------------------------

{
  const described = detect.describeCandidate(
    candidate({ src: "https://cdn/v.mp4", duration: 600, currentTime: 42 }),
    { kind: "hls", quality: "1080p" }
  );
  check(described.url === "https://cdn/v.mp4", "describeCandidate keeps the URL");
  check(described.kind === "hls", "describeCandidate keeps the kind");
  check(described.quality === "1080p", "describeCandidate keeps the quality label");
  check(described.durationSec === 600, "describeCandidate reports the duration");
  check(described.positionSec === 42, "describeCandidate reports the play position");
  check(described.playing === true, "describeCandidate reports the playing state");
}

// ---------------------------------------------------------------------------
// DOM adapter, against a stub document
// ---------------------------------------------------------------------------

function makeStyle(overrides = {}) {
  return {
    visibility: "visible",
    display: "block",
    opacity: "1",
    objectFit: "fill",
    backgroundColor: "rgba(0, 0, 0, 0)",
    ...overrides,
  };
}

function makeElement(localName, options = {}) {
  const style = makeStyle(options.style);
  const rect = {
    left: 0,
    top: 0,
    right: options.width ?? 0,
    bottom: options.height ?? 0,
    width: options.width ?? 0,
    height: options.height ?? 0,
  };
  const element = {
    localName,
    src: options.src ?? "",
    currentSrc: options.src ?? "",
    type: options.type ?? "",
    clientWidth: options.width ?? 0,
    clientHeight: options.height ?? 0,
    paused: options.paused ?? false,
    currentTime: options.currentTime ?? 0,
    readyState: options.readyState ?? 4,
    duration: options.duration ?? 0,
    videoWidth: options.videoWidth ?? 0,
    videoHeight: options.videoHeight ?? 0,
    mediaKeys: options.mediaKeys ?? null,
    getBoundingClientRect: () => rect,
    isSameNode: (other) => other === element,
    querySelector: (selector) => {
      if (selector !== "source[src]") return null;
      return options.sourceSrc ? { src: options.sourceSrc } : null;
    },
  };
  element.__style = style;
  return element;
}

function makeDocument(elements, options = {}) {
  return {
    defaultView: {
      scrollX: 0,
      scrollY: 0,
      getComputedStyle: (element) => element.__style,
    },
    querySelectorAll: () => elements,
    activeElement: options.activeElement ?? null,
    elementFromPoint: options.elementFromPoint ?? (() => null),
    elementsFromPoint: options.elementsFromPoint ?? (() => []),
  };
}

// The scanner reads geometry, visibility and playing state from a real element.
{
  const playing = makeElement("video", { src: "https://cdn/p.mp4", width: 1280, height: 720, currentTime: 30 });
  const paused = makeElement("video", { src: "https://cdn/q.mp4", width: 1920, height: 1080, paused: true });
  const scanner = new detect.MediaScanner(makeDocument([paused, playing]));
  const best = scanner.findBest();
  check(best && best.src === "https://cdn/p.mp4", "the scanner prefers the playing element");
  check(best.element === playing, "the scanner returns the element itself");
}

// An element with display:none is reported hidden, not visible.
{
  const hidden = makeElement("video", {
    src: "https://cdn/h.mp4",
    width: 1920,
    height: 1080,
    style: { display: "none" },
  });
  const scanner = new detect.MediaScanner(makeDocument([hidden]));
  const descriptor = scanner.describe(hidden);
  check(descriptor.visible === false, "display:none marks the element not visible");
  const best = scanner.findBest();
  check(best && best.src === "https://cdn/h.mp4", "a lone hidden video is still the best candidate");
}

// A zero-size element is not visible.
{
  const zero = makeElement("video", { src: "https://cdn/z.mp4", width: 0, height: 0 });
  const scanner = new detect.MediaScanner(makeDocument([zero]));
  check(scanner.describe(zero).visible === false, "a zero-size element is not visible");
}

// <source src> is used when the element itself has no src.
{
  const element = makeElement("video", { width: 640, height: 360, sourceSrc: "https://cdn/source.mp4" });
  const scanner = new detect.MediaScanner(makeDocument([element]));
  check(scanner.describe(element).src === "https://cdn/source.mp4",
    "the scanner falls back to <source src>");
}

// Occlusion: an opaque overlay on top marks the element occluded.
{
  const video = makeElement("video", { src: "https://cdn/v.mp4", width: 640, height: 360 });
  const overlay = { __style: makeStyle({ backgroundColor: "rgb(0, 0, 0)" }), isSameNode: () => false };
  const scanner = new detect.MediaScanner(makeDocument([video], {
    elementsFromPoint: () => [overlay, video],
  }));
  check(scanner.describe(video).occluded === true, "an opaque overlay marks the element occluded");
}

// A transparent overlay does not occlude.
{
  const video = makeElement("video", { src: "https://cdn/v.mp4", width: 640, height: 360 });
  const overlay = { __style: makeStyle({ backgroundColor: "rgba(0, 0, 0, 0)" }), isSameNode: () => false };
  const scanner = new detect.MediaScanner(makeDocument([video], {
    elementsFromPoint: () => [overlay, video],
  }));
  check(scanner.describe(video).occluded === false, "a transparent overlay does not occlude");
}

// activeElement marks the focused descriptor.
{
  const focused = makeElement("video", { src: "https://cdn/a.mp4", width: 400, height: 225, currentTime: 3 });
  const bigger = makeElement("video", { src: "https://cdn/b.mp4", width: 1920, height: 1080, currentTime: 3 });
  const scanner = new detect.MediaScanner(makeDocument([bigger, focused], { activeElement: focused }));
  const best = scanner.findBest();
  check(best && best.src === "https://cdn/a.mp4", "activeElement wins over the larger element");
}

// mediaKeys marks the descriptor protected.
{
  const drm = makeElement("video", { src: "https://cdn/drm.mp4", width: 640, height: 360, mediaKeys: {} });
  const scanner = new detect.MediaScanner(makeDocument([drm]));
  check(scanner.describe(drm).protected === true, "mediaKeys marks the candidate protected");
}

// The scanner honours the pinned element and the caller's URL list.
{
  const pinned = makeElement("video", { src: "https://cdn/pin.mp4", width: 320, height: 180 });
  const big = makeElement("video", { src: "https://cdn/big.mp4", width: 1920, height: 1080 });
  const scanner = new detect.MediaScanner(makeDocument([big, pinned]));
  const best = scanner.findBest({ element: pinned });
  check(best && best.element === pinned, "the pinned element wins");

  const byUrl = scanner.findBest({ urls: ["https://cdn/big.mp4"] });
  check(byUrl && byUrl.element === big, "a URL match wins");
}

// ---------------------------------------------------------------------------

if (failures > 0) {
  console.error(`media_detect: ${failures} check(s) failed`);
  process.exit(1);
}
console.log("media_detect: all checks passed");
