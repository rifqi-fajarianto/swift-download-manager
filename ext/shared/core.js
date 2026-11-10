// Background logic shared by the Chromium service worker and the Firefox event
// page. `api.js`, `cookie-permission.js` and `action-icon.js` are loaded first
// (importScripts in Chromium, background.scripts in Firefox), so `swiftdmApi`,
// `swiftdmCookies` and `swiftdmActionIcon` already exist when this file runs.

const SOCKET_URL = "ws://127.0.0.1:10007/";
const SUBPROTOCOL = "swiftdm.v1";
const MENU_LINK = "swiftdm-download-link";
const MENU_PAGE = "swiftdm-download-page";
const OBSERVED_TTL_MS = 30000;
const OBSERVED_MAX = 200;
const STREAM_TTL_MS = 60000;
const STREAM_MAX_PER_TAB = 8;
const MAX_POST_BYTES = 262144;
const STATUS_RECONNECT_MS = 15000;
// Media detection (docs/spec/08-media-download.md): one recorded MSE stream per
// tab, capped so the worker never accumulates browsing data.
const MEDIA_MAX_TABS = 16;
const MEDIA_MAX_SEGMENTS = 2000;
const MEDIA_MAX_BYTES = 512 * 1024;
const MEDIA_TTL_MS = 30 * 60 * 1000;
const MEDIA_SCAN_TIMEOUT_MS = 1500;
const autoIntercepting = new Set();

// Request bodies and custom headers are remembered briefly so an automatic
// handoff of a POST-initiated download can forward them. Entries expire so the
// worker never accumulates browsing data.
const observedRequests = new Map();

// Per-tab media-only responses (video/audio/HLS/DASH) seen while browsing.
// Adaptive pages serve their video and audio tracks as separate responses, so
// the popup can offer both at once (the app tags them with a shared group
// token).
const tabStreams = new Map();

// Per-tab MSE segment log (blob: players). `swiftdmMediaMse` is loaded before
// this file by the content script and by the worker bootstrap.
const tabSegments = new Map();

// State the app pushes: whether the in-page overlay button may be shown. It is
// persisted so a restarted worker keeps the last known value.
let appConfig = { mediaOverlay: true };

// The pairing token the app handed out on first contact. Kept in
// storage.local, sent as `?t=` on every socket URL.
let pairingToken = "";

let statusSocket = null;
let statusReconnectTimer = null;
let commandSocket = null;
let commandSocketReady = null;
const commandRequests = new Map();

// The toolbar mark follows the automatic-handoff switch: the colored mark while
// it is on, the grayscale one while it is off. The worker applies the stored
// value whenever it wakes; the popup also applies it immediately on a flip.
async function restoreActionIcon() {
  try {
    const { autoIntercept = true } = await swiftdmApi.storage.get(["autoIntercept"]);
    await swiftdmActionIcon.apply(autoIntercept !== false);
  } catch {
    // The toolbar icon is cosmetic; never fail a handoff because of it.
  }
}

void restoreActionIcon();
// The pairing token and the app-pushed config are read before the first socket
// so a restarted worker reconnects silently instead of prompting again.
void loadPairingToken();

swiftdmApi.storage.onChanged((changes, area) => {
  if (area !== "local" || !changes.autoIntercept) return;
  void swiftdmActionIcon.apply(changes.autoIntercept.newValue !== false);
});

swiftdmApi.runtime.onInstalled(async () => {
  const values = await swiftdmApi.storage.get(["autoIntercept", "bypassDownloads"]);
  const defaults = {};
  if (typeof values.autoIntercept !== "boolean") defaults.autoIntercept = true;
  // The separate bypass switch was removed in 0.4.0: it did exactly what
  // turning automatic interception off does. Carry an enabled bypass over as
  // "automatic interception off" so the user's choice is preserved.
  if (values.bypassDownloads === true) defaults.autoIntercept = false;
  if (Object.keys(defaults).length) await swiftdmApi.storage.set(defaults);
  if (values.bypassDownloads !== undefined) {
    await swiftdmApi.storage.remove("bypassDownloads");
  }
  await createContextMenus();
  ensureStatusSocket();
  void restoreActionIcon();
});

swiftdmApi.runtime.onStartup(() => ensureStatusSocket());

async function createContextMenus() {
  await swiftdmApi.menus.removeAll();
  await swiftdmApi.menus.create({
    id: MENU_LINK,
    title: "Download link with SwiftDM",
    contexts: ["link"],
    targetUrlPatterns: ["http://*/*", "https://*/*"]
  });
  await swiftdmApi.menus.create({
    id: MENU_PAGE,
    title: "Download page with SwiftDM",
    contexts: ["page"],
    documentUrlPatterns: ["http://*/*", "https://*/*"]
  });
}

swiftdmApi.menus.onClicked((info, tab) => {
  if (info.menuItemId === MENU_LINK) {
    void sendDownload(info.linkUrl, info.pageUrl, tab?.title ?? "", {
      requestCookiePermission: true
    })
      .then(() => clearActionError())
      .catch(showActionError);
  } else if (info.menuItemId === MENU_PAGE) {
    void sendDownload(info.pageUrl, info.pageUrl, tab?.title ?? "", {
      requestCookiePermission: true
    })
      .then(() => clearActionError())
      .catch(showActionError);
  }
});

swiftdmApi.runtime.onMessage((message, sender, sendResponse) => {
  // The content script cannot know its own tab id, so the sender is the only
  // reliable source; the popup passes it explicitly because it *is* a tab.
  const tabId = typeof sender?.tab?.id === "number" ? sender.tab.id : Number(message?.tabId);
  if (message?.type === "download-current-tab") {
    void sendDownload(message.url, "", message.title ?? "", {
      useGrantedCookiePermission: message.cookiesGranted === true
    })
      .then(() => sendResponse({ ok: true }))
      .catch((error) => sendResponse({ ok: false, error: errorText(error) }));
    return true;
  }
  if (message?.type === "get-connection-status") {
    ensureStatusSocket();
    void sendCommand("ping", {}).then(
      (response) => sendResponse({ ok: true, version: response.version }),
      (error) => sendResponse({ ok: false, error: errorText(error) })
    );
    return true;
  }
  if (message?.type === "get-media-streams") {
    // The overlay and the popup both ask for whatever was observed for their
    // tab: the network sniffing list merged with an on-demand DOM scan and the
    // MSE segment log (docs/spec/08-media-download.md §3).
    void collectMedia(tabId, message.frameId).then(
      (media) => sendResponse({ ok: true, media }),
      () => sendResponse({ ok: true, media: { hls: [], dash: [], pairs: [], dom: [], mse: [] } })
    );
    return true;
  }
  if (message?.type === "download-media") {
    void sendMedia(message.candidate, tabId, sender?.tab ?? null)
      .then(() => sendResponse({ ok: true }))
      .catch((error) => sendResponse({ ok: false, error: errorText(error) }));
    return true;
  }
  if (message?.type === "media-segment") {
    // Relay from the page hook (metadata only). The worker decides whether the
    // record belongs to a media stream.
    rememberSegment(tabId, message.record, message.pageUrl ?? sender?.tab?.url ?? "");
    sendResponse({ ok: true });
    return false;
  }
  if (message?.type === "get-media-config") {
    sendResponse({ ok: true, config: appConfig, paired: Boolean(pairingToken) });
    return false;
  }
  if (message?.type === "install-page-hook") {
    // Tabs that were already open when the extension was installed or updated
    // have no content script yet. Injecting the hook directly keeps the MSE
    // ordering hint working there too.
    void installPageHook(Number(message.tabId)).then(
      (installed) => sendResponse({ ok: true, installed }),
      () => sendResponse({ ok: false, installed: false })
    );
    return true;
  }
  if (message?.type === "download-url") {
    // A paired handoff from the overlay: the content script knows neither the
    // page URL nor whether cookies were granted, so both are filled in here.
    const pageUrl = validHttpUrl(message.pageUrl)?.href ?? sender?.tab?.url ?? "";
    void sendDownload(message.url, pageUrl, message.title || sender?.tab?.title || "", {
      useGrantedCookiePermission: true,
      group: message.group
    })
      .then(() => sendResponse({ ok: true }))
      .catch((error) => sendResponse({ ok: false, error: errorText(error) }));
    return true;
  }
});

// ---------------------------------------------------------------------------
// Request observation (POST bodies and x-* headers for automatic handoffs)
// ---------------------------------------------------------------------------

const observedTypes = ["main_frame", "sub_frame", "object", "xmlhttprequest", "media", "other"];
const observedFilter = { urls: ["http://*/*", "https://*/*"], types: observedTypes };

if (swiftdmApi.webRequest.available()) {
  swiftdmApi.webRequest.listen("onBeforeRequest", (details) => {
    rememberRequest(details.url, {
      method: details.method,
      ...requestBodyPayload(details.requestBody)
    });
  }, observedFilter, ["requestBody"]);
  swiftdmApi.webRequest.listen("onBeforeSendHeaders", (details) => {
    const headers = [];
    for (const header of details.requestHeaders ?? []) {
      const name = String(header.name ?? "");
      if (name.toLowerCase().startsWith("x-") && headers.length < 16) {
        headers.push({ name: name.toLowerCase(), value: String(header.value ?? "") });
      }
    }
    rememberRequest(details.url, { headers });
  }, observedFilter, ["requestHeaders"]);
  swiftdmApi.webRequest.listen("onHeadersReceived", (details) => {
    const header = (details.responseHeaders ?? []).find(
      (entry) => String(entry.name ?? "").toLowerCase() === "content-type"
    );
    const mimeType = header ? String(header.value ?? "") : "";
    if (!mimeType) return;
    rememberRequest(details.url, { mimeType: mimeType.split(";")[0].trim().toLowerCase() });
    rememberStream(details.tabId, details.url, mimeType);
    // A segment response also feeds the MSE log, so a blob: player can be put
    // back together from what the page actually fetched.
    rememberSegment(details.tabId, {
      url: details.url,
      method: details.method,
      contentType: mimeType,
      contentLength: Number(
        (details.responseHeaders ?? []).find(
          (entry) => String(entry.name ?? "").toLowerCase() === "content-length"
        )?.value ?? 0
      ) || 0
    }, details.documentUrl ?? "");
  }, observedFilter, ["responseHeaders"]);
}

// Media-only responses that the popup can offer. HLS and DASH are handed off as
// manifests; plain video/audio tracks are paired so both halves travel together.
function streamKind(mimeType, url) {
  const mime = String(mimeType ?? "").split(";")[0].trim().toLowerCase();
  const path = String(url ?? "").split(/[?#]/)[0].toLowerCase();
  if (mime.includes("mpegurl") || path.endsWith(".m3u8")) return "hls";
  if (mime.includes("dash+xml") || path.endsWith(".mpd")) return "dash";
  if (mime.startsWith("video/")) return "video";
  if (mime.startsWith("audio/")) return "audio";
  return "";
}

function rememberStream(tabId, url, mimeType) {
  if (typeof tabId !== "number" || tabId < 0 || !validHttpUrl(url)) return;
  const kind = streamKind(mimeType, url);
  if (!kind) return;
  if ((kind === "video" || kind === "audio") && !isMediaOnly(mimeType)) return;
  // A segment is not a downloadable track: it is part of the MSE log instead.
  if (swiftdmMediaMse && swiftdmMediaMse.classifySegment({ url, contentType: mimeType }).segment) {
    return;
  }
  const now = Date.now();
  const entries = (tabStreams.get(tabId) ?? []).filter(
    (entry) => now - entry.at <= STREAM_TTL_MS && entry.url !== url
  );
  entries.push({ url, kind, at: now });
  while (entries.length > STREAM_MAX_PER_TAB) entries.shift();
  tabStreams.set(tabId, entries);
}

// Container types that are really media tracks (guards against "video/html"
// style bogus types that would otherwise be offered as a track).
function isMediaOnly(mimeType) {
  const mime = String(mimeType ?? "").split(";")[0].trim().toLowerCase();
  return (mime.startsWith("video/") || mime.startsWith("audio/")) && !mime.includes("html");
}

function mediaForTab(tabId) {
  const now = Date.now();
  const entries = (tabStreams.get(tabId) ?? []).filter(
    (entry) => now - entry.at <= STREAM_TTL_MS
  );
  const hls = entries.filter((entry) => entry.kind === "hls").map((entry) => entry.url);
  const dash = entries.filter((entry) => entry.kind === "dash").map((entry) => entry.url);
  const video = entries.filter((entry) => entry.kind === "video");
  const audio = entries.filter((entry) => entry.kind === "audio");
  const pairs = [];
  // Pair the newest video track with the newest audio track first; deeper pairs
  // cover pages that expose multiple renditions in one tab.
  const count = Math.min(video.length, audio.length);
  for (let i = 0; i < count; i++) {
    const videoEntry = video[video.length - 1 - i];
    const audioEntry = audio[audio.length - 1 - i];
    pairs.push({
      video: videoEntry.url,
      audio: audioEntry.url,
      group: streamGroup(videoEntry.url)
    });
  }
  return { hls, dash, pairs };
}

// Short stable token shared by both halves of a pair. It is not derived from
// page content, just enough for the app to recognize the pair later.
function streamGroup(videoUrl) {
  let hash = 0;
  for (let i = 0; i < videoUrl.length; i++) {
    hash = (hash * 31 + videoUrl.charCodeAt(i)) >>> 0;
  }
  return `pair-${hash.toString(16).padStart(8, "0")}`;
}

swiftdmApi.tabs.onRemoved((tabId) => {
  tabStreams.delete(tabId);
  tabSegments.delete(tabId);
});

function rememberRequest(url, patch) {
  if (!url) return;
  const now = Date.now();
  for (const [key, value] of observedRequests) {
    if (now - value.at > OBSERVED_TTL_MS) observedRequests.delete(key);
  }
  if (!observedRequests.has(url) && observedRequests.size >= OBSERVED_MAX) {
    observedRequests.delete(observedRequests.keys().next().value);
  }
  observedRequests.set(url, { ...observedRequests.get(url), ...patch, at: now });
}

function requestBodyPayload(requestBody) {
  if (!requestBody) return {};
  if (requestBody.formData) {
    const params = new URLSearchParams();
    for (const [name, values] of Object.entries(requestBody.formData)) {
      for (const value of values ?? []) params.append(name, String(value));
    }
    const text = params.toString();
    if (!text || text.length > MAX_POST_BYTES) return {};
    return { postData: text, postDataEncoding: "utf8" };
  }
  const parts = requestBody.raw ?? [];
  let size = 0;
  for (const part of parts) {
    if (part.bytes) size += new Uint8Array(part.bytes).byteLength;
  }
  if (!size || size > MAX_POST_BYTES) return {};
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const part of parts) {
    if (!part.bytes) continue;
    const chunk = new Uint8Array(part.bytes);
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return { postData: bytesToBase64(bytes), postDataEncoding: "base64" };
}

function bytesToBase64(bytes) {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 8192) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
  }
  return btoa(binary);
}

function observedExtrasFor(...urls) {
  const now = Date.now();
  for (const url of urls) {
    if (!url) continue;
    const observed = observedRequests.get(url);
    if (!observed || now - observed.at > OBSERVED_TTL_MS) continue;
    const extras = {};
    if (observed.method) extras.method = observed.method;
    if (observed.postData) {
      extras.postData = observed.postData;
      extras.postDataEncoding = observed.postDataEncoding ?? "utf8";
    }
    if (observed.headers?.length) extras.headers = observed.headers;
    return extras;
  }
  return {};
}

// ---------------------------------------------------------------------------
// Media detection (docs/spec/08-media-download.md)
// ---------------------------------------------------------------------------

// One MSE segment log per tab, bounded and expiring like the stream list.
function segmentLogFor(tabId) {
  if (typeof tabId !== "number" || tabId < 0) return null;
  const now = Date.now();
  for (const [key, entry] of tabSegments) {
    if (now - entry.updatedAt > MEDIA_TTL_MS) tabSegments.delete(key);
  }
  let log = tabSegments.get(tabId);
  if (!log) {
    if (tabSegments.size >= MEDIA_MAX_TABS) {
      tabSegments.delete(tabSegments.keys().next().value);
    }
    log = new swiftdmMediaMse.SegmentLog({
      maxSegments: MEDIA_MAX_SEGMENTS,
      maxBytes: MEDIA_MAX_BYTES,
      ttlMs: MEDIA_TTL_MS
    });
    tabSegments.set(tabId, log);
  }
  log.updatedAt = now;
  return log;
}

function rememberSegment(tabId, record, pageUrl) {
  if (!swiftdmMediaMse || !record?.url || !validHttpUrl(record.url)) return;
  const log = segmentLogFor(tabId);
  if (!log) return;
  // A record arriving through the page hook keeps its own ordering counter; the
  // webRequest path has none, so the log assigns the next one.
  log.add({
    url: record.url,
    method: record.method ?? "GET",
    contentType: record.contentType ?? "",
    contentLength: record.contentLength ?? 0,
    order: Number.isFinite(record.order) ? record.order : undefined,
    pageUrl: pageUrl ?? ""
  });
}

// Asks every frame of the tab for its best media element. The DOM scan runs in
// the content script; the worker only merges the answers. Each frame answers
// `{ok, candidate}`, so the candidates are unwrapped here.
async function scanTab(tabId, urls) {
  if (typeof tabId !== "number" || tabId < 0) return [];
  const frames = await swiftdmApi.tabs.sendMessage(tabId, { type: "scan-media", urls });
  const found = [];
  for (const frame of Array.isArray(frames) ? frames : []) {
    if (!frame) continue;
    const candidate = frame.candidate ?? (frame.url ? frame : null);
    if (candidate && typeof candidate === "object") found.push(candidate);
  }
  return found;
}

// Merges network-sniffed streams, the DOM candidates and the MSE logs into the
// single list the popup renders. Duplicates are removed by URL.
async function collectMedia(tabId, frameId) {
  const sniffed = mediaForTab(tabId);
  const dom = await scanTab(tabId, [...sniffed.hls, ...sniffed.dash, ...sniffed.pairs.map((p) => p.video)]);
  const log = tabSegments.get(tabId);
  const mse = log ? log.candidates().map((stream) => ({
    key: stream.key,
    kind: stream.kind,
    segmentCount: stream.segmentCount,
    initUrl: stream.initUrl,
    incomplete: stream.incomplete,
    dropped: stream.dropped,
    segments: stream.segments.map((item) => ({
      url: item.url,
      role: item.role,
      order: item.order,
      contentType: item.contentType
    }))
  })) : [];

  const seen = new Set();
  const dedupe = (url) => {
    if (!url || seen.has(url)) return false;
    seen.add(url);
    return true;
  };
  return {
    hls: sniffed.hls.filter(dedupe),
    dash: sniffed.dash.filter(dedupe),
    pairs: sniffed.pairs.filter((pair) => dedupe(pair.video) && dedupe(pair.audio)),
    dom: dom.filter((candidate) => candidate && candidate.url && !candidate.blob),
    mse: mse.filter((stream) => stream.segmentCount > 0),
    frameId: frameId ?? 0
  };
}

// Builds the addMedia payload from a candidate the overlay picked. Everything
// that identifies the request (cookies, referer, UA, x-* headers) travels with
// it, so the app can re-issue the request itself.
//
// `tab` is the sender's tab: the content script knows neither the page URL nor
// the tab id, and a blob: candidate can only be resolved to the recorded
// segments of that tab.
async function sendMedia(candidate, tabId, tab = null) {
  if (!candidate || typeof candidate !== "object") {
    throw new Error("No media was selected.");
  }
  if (candidate.protected === true) {
    throw new Error("Protected (DRM) media cannot be downloaded.");
  }
  const pageUrl = validHttpUrl(candidate.pageUrl)
    ? candidate.pageUrl
    : (validHttpUrl(tab?.url)?.href ?? "");
  const pageTitle = String(candidate.pageTitle || tab?.title || "").slice(0, 1024);

  let kind = ["file", "hls", "dash", "mse"].includes(candidate.kind) ? candidate.kind : "file";
  // A chooser row carries the recorded stream under `stream`; the raw candidate
  // shape carries `segments` directly. Both end up in the same payload.
  const stream = candidate.stream && typeof candidate.stream === "object"
    ? candidate.stream
    : null;
  let url = candidate.url || stream?.initUrl || candidate.initUrl || "";
  let segments = Array.isArray(candidate.segments)
    ? candidate.segments
    : (Array.isArray(stream?.segments) ? stream.segments : []);
  // A blob: player has no downloadable URL of its own; what it does have is the
  // ordered list of segments the page fetched (docs/spec/08-media-download.md
  // §6). The overlay sends the element's blob: URL, so the log fills it in.
  if (!validHttpUrl(url) || candidate.blob === true) {
    const recorded = mseStreamFor(tabId, candidate.streamKey ?? stream?.key);
    if (!recorded || recorded.segmentCount === 0) {
      throw new Error(
        "This player streams through blob: URLs. Play the video from the start so " +
        "the segments are recorded, then try again.");
    }
    kind = "mse";
    segments = recorded.segments;
    url = recorded.initUrl || recorded.segments[0]?.url || "";
  }
  if (!validHttpUrl(url)) {
    throw new Error("This stream has no downloadable address.");
  }
  if (kind === "mse" && segments.length === 0) {
    throw new Error(
      "This player streams through blob: URLs and no segments were recorded yet. " +
      "Play the video from the start, then try again.");
  }

  const commandSocketWarmup = getCommandSocket();
  commandSocketWarmup.catch(() => {});

  let granted = await swiftdmCookies.granted();
  let cookies = [];
  if (granted) {
    try {
      const siteCookies = await swiftdmApi.cookies.getAll({ url });
      cookies = siteCookies
        .filter((cookie) => !cookie.partitionKey)
        .map((cookie) => ({
          domain: cookie.domain,
          path: cookie.path,
          name: cookie.name,
          value: cookie.value,
          secure: cookie.secure,
          expires: cookie.session ? 0 : Math.floor(cookie.expirationDate ?? 0)
        }));
    } catch {
      cookies = [];
    }
  }

  const data = {
    url,
    kind,
    pageUrl,
    pageTitle,
    referer: pageUrl && sameOrigin(pageUrl, url) ? pageUrl : "",
    userAgent: navigator.userAgent,
    cookies,
    filename: String(candidate.filename ?? "").slice(0, 255),
    quality: String(candidate.quality ?? "").slice(0, 64),
    durationSec: Math.max(0, Number(candidate.durationSec) || 0),
    positionSec: Math.max(0, Number(candidate.positionSec) || 0),
    elementTag: String(candidate.elementTag ?? "").slice(0, 16),
    protected: false
  };
  // An empty group is not a group: the app validates the token with a 1..32
  // character pattern and rejects the whole request otherwise.
  const group = sanitizeGroup(candidate.group);
  if (group) data.group = group;
  if (Array.isArray(candidate.candidates) && candidate.candidates.length) {
    data.candidates = candidate.candidates.slice(0, 8).map((entry) => ({
      url: String(entry?.url ?? ""),
      kind: String(entry?.kind ?? kind),
      label: String(entry?.label ?? "").slice(0, 64),
      bandwidth: Math.max(0, Number(entry?.bandwidth) || 0)
    }));
  }
  if (segments.length) {
    data.segments = segments.slice(0, MEDIA_MAX_SEGMENTS).map((entry, index) => ({
      url: String(entry?.url ?? ""),
      role: entry?.role === "init" ? "init" : "media",
      order: Number.isFinite(entry?.order) ? entry.order : index,
      contentType: String(entry?.contentType ?? "").slice(0, 128)
    }));
  }
  const extras = observedExtrasFor(url);
  if (extras.headers?.length) data.headers = extras.headers;
  return sendCommand("addMedia", data, commandSocketWarmup);
}

// The recorded MSE stream for a tab: the one the candidate names, or the
// longest one when it names none.
function mseStreamFor(tabId, streamKey) {
  const log = tabSegments.get(tabId);
  if (!log) return null;
  const streams = log.candidates();
  if (!streams.length) return null;
  if (streamKey) {
    const named = streams.find((stream) => stream.key === streamKey);
    if (named) return named;
  }
  return streams[0];
}

function sameOrigin(a, b) {
  try {
    const left = new URL(a);
    const right = new URL(b);
    return left.protocol === right.protocol && left.host === right.host;
  } catch {
    return false;
  }
}

// Injects the MAIN-world hook into every frame of a tab. The content script
// normally loads it with a <script src>, but that fails on pages whose CSP
// forbids extension resources; scripting.executeScript is not subject to the
// page CSP.
async function installPageHook(tabId) {
  if (typeof tabId !== "number" || tabId < 0) return false;
  if (!swiftdmApi.scripting.available()) return false;
  try {
    await swiftdmApi.scripting.executeInPage(tabId, ["page-hook.js"]);
    return true;
  } catch {
    // chrome:// pages, the Web Store, PDF viewer: nothing to hook.
    return false;
  }
}

// ---------------------------------------------------------------------------
// Automatic handoff
// ---------------------------------------------------------------------------

swiftdmApi.downloads.onCreated((item) => {
  if (item.state !== "in_progress" || item.incognito || !validHttpUrl(item.url)) return;
  // Downloads started by this extension (the fallback path below) are already
  // handled; intercepting them again would loop forever.
  if (item.byExtensionId) return;
  if (autoIntercepting.has(item.id)) return;
  autoIntercepting.add(item.id);
  void interceptDownload(item).finally(() => autoIntercepting.delete(item.id));
});

async function interceptDownload(item) {
  const handoffStartedAt = Date.now();
  const { autoIntercept = true } = await swiftdmApi.storage.get(["autoIntercept"]);
  if (!autoIntercept) return;

  // Cancel immediately rather than pause: a paused item can still finish for
  // tiny files, which is exactly the case where the browser would "win" before
  // SwiftDM could take over. If SwiftDM is unreachable, the fallback below asks
  // the browser to download the URL again.
  try {
    await swiftdmApi.downloads.cancel(item.id);
    try { await swiftdmApi.downloads.erase({ id: item.id }); } catch { /* already gone */ }
  } catch {
    // The download already completed (for example a tiny data response); keep
    // the browser's copy instead of downloading it twice.
    return;
  }

  // Chromium reports the post-redirect address as `finalUrl`; Firefox's
  // DownloadItem has no such field, so the original URL is the fallback there.
  const observed = observedExtrasFor(item.finalUrl ?? "", item.url);
  try {
    await sendDownloadWithRetry(item.url, item.referrer ?? "", "", {
      useGrantedCookiePermission: true,
      ...observed
    });
    console.info("SwiftDM handoff accepted", {
      elapsedMs: Date.now() - handoffStartedAt
    });
    await clearActionError();
  } catch (error) {
    await showActionError(new Error(
      `SwiftDM unavailable, browser download restarted: ${errorText(error)}`
    ));
    try {
      await swiftdmApi.downloads.download({ url: item.url, conflictAction: "uniquify" });
    } catch (fallbackError) {
      await showActionError(new Error(
        `SwiftDM unavailable and the browser fallback failed: ${errorText(fallbackError)}`
      ));
    }
  }
}

async function sendDownloadWithRetry(...args) {
  try {
    return await sendDownload(...args);
  } catch {
    // The app may still be starting, or the worker may have raced the port
    // becoming available. One short retry avoids needlessly restoring the
    // browser download.
    await new Promise((resolve) => setTimeout(resolve, 250));
    return sendDownload(...args);
  }
}

async function sendDownload(rawUrl, pageUrl, pageTitle, options = {}) {
  const url = validHttpUrl(rawUrl);
  if (!url) throw new Error("Only HTTP and HTTPS URLs can be sent to SwiftDM.");

  // Establish the local command channel while the browser's cookie API is being
  // queried. On a cold handoff this overlaps the WebSocket handshake with the
  // browser metadata work instead of adding those latencies in series.
  const commandSocketWarmup = getCommandSocket();
  commandSocketWarmup.catch(() => {});

  let granted = false;
  if (options.requestCookiePermission) {
    // Context-menu actions are user gestures, so they may prompt. Refusal or an
    // unavailable prompt just means the handoff continues without cookies.
    granted = await swiftdmCookies.request(url.href);
  } else if (options.useGrantedCookiePermission) {
    // Background events cannot prompt; the popup asks once and later automatic
    // handoffs reuse that grant.
    granted = await swiftdmCookies.granted();
  }
  let cookies = [];
  if (granted) {
    try {
      const siteCookies = await swiftdmApi.cookies.getAll({ url: url.href });
      cookies = siteCookies
        .filter((cookie) => !cookie.partitionKey)
        .map((cookie) => ({
          domain: cookie.domain,
          path: cookie.path,
          name: cookie.name,
          value: cookie.value,
          secure: cookie.secure,
          expires: cookie.session ? 0 : Math.floor(cookie.expirationDate ?? 0)
        }));
    } catch {
      cookies = [];
    }
  }

  const source = validHttpUrl(pageUrl);
  const data = {
    url: url.href,
    pageUrl: source?.href ?? "",
    pageTitle: String(pageTitle).slice(0, 1024),
    referer: source?.origin === url.origin ? source.href : "",
    cookies,
    userAgent: navigator.userAgent
  };
  if (options.postData) {
    data.method = "POST";
    data.postData = options.postData;
    data.postDataEncoding = options.postDataEncoding ?? "utf8";
  } else if (options.method === "POST") {
    data.method = "POST";
  }
  if (options.headers?.length) data.headers = options.headers;
  // Adaptive pairs share a short token so the app can tell both halves apart
  // without inventing a second protocol command.
  const group = sanitizeGroup(options.group);
  if (group) data.group = group;
  return sendCommand("addDownload", data, commandSocketWarmup);
}

function sanitizeGroup(value) {
  return String(value ?? "").replace(/[^A-Za-z0-9_-]/g, "").slice(0, 32);
}

// ---------------------------------------------------------------------------
// Pairing token (docs/spec/08-media-download.md §7)
// ---------------------------------------------------------------------------

// The socket URL carries the pairing token when the app has handed one out.
// The app answers a token-less connection with a pairing prompt, so the very
// first connection works and later ones are silent.
function socketUrl() {
  return pairingToken ? `${SOCKET_URL}?t=${encodeURIComponent(pairingToken)}` : SOCKET_URL;
}

async function loadPairingToken() {
  try {
    const stored = await swiftdmApi.storage.get(["pairingToken", "appConfig"]);
    if (typeof stored.pairingToken === "string") pairingToken = stored.pairingToken;
    if (stored.appConfig && typeof stored.appConfig === "object") {
      appConfig = { mediaOverlay: stored.appConfig.mediaOverlay !== false };
    }
  } catch {
    // A missing storage entry simply means "not paired yet".
  }
}

async function storePairingToken(token) {
  if (typeof token !== "string" || !token) return;
  pairingToken = token;
  try {
    await swiftdmApi.storage.set({ pairingToken: token });
  } catch {
    // The token still works for this worker lifetime.
  }
}

async function forgetPairingToken() {
  pairingToken = "";
  try {
    await swiftdmApi.storage.remove(["pairingToken"]);
  } catch {
    // ignored: the next connection simply prompts again
  }
}

// Applies a `config` push: persist it and tell the content scripts so the
// overlay disappears without reloading the page.
async function applyConfig(config) {
  if (!config || typeof config !== "object") return;
  appConfig = { mediaOverlay: config.mediaOverlay !== false };
  try {
    await swiftdmApi.storage.set({ appConfig });
  } catch {
    // ignored: the value still applies for this worker lifetime
  }
  await broadcastToTabs({ type: "media-config", mediaOverlay: appConfig.mediaOverlay });
}

async function broadcastToTabs(message) {
  let tabs = [];
  try {
    tabs = await swiftdmApi.tabs.query({});
  } catch {
    return;
  }
  for (const tab of tabs) {
    if (typeof tab?.id !== "number") continue;
    try {
      await swiftdmApi.tabs.sendMessage(tab.id, message);
    } catch {
      // No content script in that tab (chrome:// page, PDF viewer, discarded
      // tab). Not an error.
    }
  }
}

// ---------------------------------------------------------------------------
// Status channel (app -> extension push)
// ---------------------------------------------------------------------------

function ensureStatusSocket() {
  if (statusSocket &&
      (statusSocket.readyState === WebSocket.OPEN ||
       statusSocket.readyState === WebSocket.CONNECTING)) {
    return;
  }
  try {
    statusSocket = new WebSocket(socketUrl(), SUBPROTOCOL);
  } catch {
    statusSocket = null;
    scheduleStatusReconnect();
    return;
  }
  const socket = statusSocket;
  socket.addEventListener("open", () => {
    try {
      socket.send(JSON.stringify({ v: 1, cmd: "subscribe", id: crypto.randomUUID(), data: {} }));
    } catch { /* socket closed before the subscribe was written */ }
  });
  socket.addEventListener("message", (event) => {
    try {
      const message = JSON.parse(event.data);
      if (message.event === "status" && message.data) {
        void swiftdmApi.storage.set({ appStatus: message.data });
      } else if (message.event === "config") {
        void applyConfig(message.data);
      } else if (message.event === "pair" && message.data?.token) {
        void storePairingToken(message.data.token);
      }
    } catch { /* ignore malformed events */ }
  });
  socket.addEventListener("error", () => {
    try { socket.close(); } catch { /* already closed */ }
  });
  socket.addEventListener("close", (event) => {
    if (statusSocket === socket) statusSocket = null;
    // 1008 = the app refused this connection (a stale pairing token). Drop the
    // token so the next attempt goes through the pairing prompt again.
    if (event?.code === 1008) void forgetPairingToken();
    scheduleStatusReconnect();
  });
}

function scheduleStatusReconnect() {
  if (statusReconnectTimer) return;
  statusReconnectTimer = setTimeout(() => {
    statusReconnectTimer = null;
    ensureStatusSocket();
  }, STATUS_RECONNECT_MS);
}

// ---------------------------------------------------------------------------
// Command channel (extension -> app request/response)
// ---------------------------------------------------------------------------

async function getCommandSocket() {
  if (commandSocket && commandSocket.readyState === WebSocket.OPEN) return commandSocket;
  if (commandSocket && commandSocket.readyState === WebSocket.CONNECTING && commandSocketReady) {
    return commandSocketReady;
  }

  let socket;
  try {
    socket = new WebSocket(socketUrl(), SUBPROTOCOL);
  } catch (error) {
    return Promise.reject(new Error(`Cannot create the SwiftDM connection: ${errorText(error)}`));
  }
  commandSocket = socket;
  commandSocketReady = new Promise((resolve, reject) => {
    let opened = false;
    socket.addEventListener("open", () => {
      opened = true;
      resolve(socket);
    }, { once: true });
    socket.addEventListener("message", (event) => {
      try {
        const response = JSON.parse(event.data);
        if (response.event === "config") {
          void applyConfig(response.data);
          return;
        }
        if (response.event === "pair" && response.data?.token) {
          void storePairingToken(response.data.token);
          return;
        }
        if (typeof response.id !== "string") return;
        if (!response.ok) {
          finishCommandRequest(response.id, new Error(response.error ?? "SwiftDM rejected the request."));
        } else {
          finishCommandRequest(response.id, null, response.data ?? {});
        }
      } catch {
        dropCommandSocket(socket, new Error("SwiftDM sent an invalid response."));
      }
    });
    socket.addEventListener("error", () => {
      const error = new Error(
        "Cannot connect to SwiftDM. Make sure SwiftDM is running and check its Extensions dialog."
      );
      if (!opened) reject(error);
      dropCommandSocket(socket, error);
    });
    socket.addEventListener("close", (event) => {
      const detail = event.code && event.code !== 1005 ? ` (close ${event.code})` : "";
      const error = new Error(`SwiftDM closed the connection${detail}.`);
      if (event?.code === 1008) void forgetPairingToken();
      if (!opened) reject(error);
      dropCommandSocket(socket, error);
    });
  }).finally(() => {
    if (commandSocket === socket) commandSocketReady = null;
  });
  return commandSocketReady;
}

function sendCommand(cmd, data, socketPromise = null) {
  const id = crypto.randomUUID();
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      finishCommandRequest(id, new Error(
        "SwiftDM did not respond on 127.0.0.1:10007. Check that SwiftDM is running and the port is free."
      ));
    }, 5000);
    commandRequests.set(id, { resolve, reject, timeout });

    (socketPromise ?? getCommandSocket()).then((socket) => {
      if (!commandRequests.has(id)) return;
      try {
        socket.send(JSON.stringify({ v: 1, cmd, id, data }));
      } catch (error) {
        finishCommandRequest(id, new Error(`Cannot send the request to SwiftDM: ${errorText(error)}`));
        dropCommandSocket(socket, new Error("SwiftDM command connection closed."));
      }
    }).catch((error) => {
      finishCommandRequest(id, error);
    });
  });
}

function finishCommandRequest(id, error, response) {
  const pending = commandRequests.get(id);
  if (!pending) return;
  commandRequests.delete(id);
  clearTimeout(pending.timeout);
  if (error) pending.reject(error);
  else pending.resolve(response ?? {});
}

function dropCommandSocket(socket, error) {
  if (commandSocket === socket) {
    commandSocket = null;
    commandSocketReady = null;
  }
  for (const [id] of commandRequests) finishCommandRequest(id, error);
  try {
    if (socket.readyState !== WebSocket.CLOSED) socket.close();
  } catch { /* already closed */ }
}

// ---------------------------------------------------------------------------
// Toolbar feedback
// ---------------------------------------------------------------------------

// The badge is the only feedback surface shared by both browsers (Chromium
// needs the "action" permission for it, Firefox shows it on the toolbar button).
async function showActionError(error) {
  const message = errorText(error);
  console.error("SwiftDM integration:", message);
  try {
    await swiftdmApi.action.setBadgeBackgroundColor({ color: "#c43c3c" });
    await swiftdmApi.action.setBadgeText({ text: "!" });
    await swiftdmApi.action.setTitle({ title: `SwiftDM: ${message}` });
  } catch {
    // The badge is cosmetic; the popup still reports the failure.
  }
}

async function clearActionError() {
  try {
    await swiftdmApi.action.setBadgeText({ text: "" });
    await swiftdmApi.action.setTitle({ title: "SwiftDM" });
  } catch {
    // ignored: see showActionError
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function validHttpUrl(value) {
  try {
    const url = new URL(value);
    if ((url.protocol !== "http:" && url.protocol !== "https:") || url.username || url.password) {
      return null;
    }
    return url;
  } catch {
    return null;
  }
}

function errorText(error) {
  return error instanceof Error ? error.message : String(error);
}
