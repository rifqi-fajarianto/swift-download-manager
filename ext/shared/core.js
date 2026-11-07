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
const autoIntercepting = new Set();

// Request bodies and custom headers are remembered briefly so an automatic
// handoff of a POST-initiated download can forward them. Entries expire so the
// worker never accumulates browsing data.
const observedRequests = new Map();

// Per-tab media-only responses (video/audio/HLS) seen while browsing. Adaptive
// pages serve their video and audio tracks as separate responses, so the popup
// can offer both at once (the app tags them with a shared group token).
const tabStreams = new Map();

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

swiftdmApi.runtime.onMessage((message, _sender, sendResponse) => {
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
    // Synchronous: the popup asks for whatever was observed for its tab.
    sendResponse({ ok: true, media: mediaForTab(Number(message.tabId)) });
    return false;
  }
  if (message?.type === "download-url") {
    void sendDownload(message.url, message.pageUrl ?? "", message.title ?? "", {
      useGrantedCookiePermission: message.cookiesGranted === true,
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

const observedTypes = ["main_frame", "sub_frame", "object", "xmlhttprequest", "other"];
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
  }, observedFilter, ["responseHeaders"]);
}

// Media-only responses that the popup can offer (HLS requires app support,
// plain video/audio tracks are paired so both halves are handed off together).
function streamKind(mimeType, url) {
  const mime = String(mimeType ?? "").split(";")[0].trim().toLowerCase();
  const path = String(url ?? "").split(/[?#]/)[0].toLowerCase();
  if (mime.includes("mpegurl") || path.endsWith(".m3u8")) return "hls";
  if (mime.startsWith("video/")) return "video";
  if (mime.startsWith("audio/")) return "audio";
  return "";
}

function rememberStream(tabId, url, mimeType) {
  if (typeof tabId !== "number" || tabId < 0 || !validHttpUrl(url)) return;
  const kind = streamKind(mimeType, url);
  if (!kind) return;
  if ((kind === "video" || kind === "audio") && !isMediaOnly(mimeType)) return;
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
  return { hls, pairs };
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

swiftdmApi.tabs.onRemoved((tabId) => tabStreams.delete(tabId));

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
// Status channel (app -> extension push)
// ---------------------------------------------------------------------------

function ensureStatusSocket() {
  if (statusSocket &&
      (statusSocket.readyState === WebSocket.OPEN ||
       statusSocket.readyState === WebSocket.CONNECTING)) {
    return;
  }
  try {
    statusSocket = new WebSocket(SOCKET_URL, SUBPROTOCOL);
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
      }
    } catch { /* ignore malformed events */ }
  });
  socket.addEventListener("error", () => {
    try { socket.close(); } catch { /* already closed */ }
  });
  socket.addEventListener("close", () => {
    if (statusSocket === socket) statusSocket = null;
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
    socket = new WebSocket(SOCKET_URL, SUBPROTOCOL);
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
