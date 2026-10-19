// Headless tests for the Chromium service worker's download-handoff logic.
// Chrome APIs and WebSocket are mocked; no browser or GUI is started.

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(
  path.join(__dirname, "..", "ext", "chromium", "service-worker.js"),
  "utf8"
);

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function settle(predicate, timeout = 1000) {
  const deadline = Date.now() + timeout;
  while (!predicate() && Date.now() < deadline) await sleep(5);
  return predicate();
}

function createEnvironment({ autoIntercept = true, bypassDownloads = false,
                             webSocketMode = "ok", cookieGranted = true } = {}) {
  const calls = {
    cancel: [],
    erase: [],
    download: [],
    sockets: [],
    storageSets: [],
  };
  let onCreated = null;
  let onMessage = null;
  let onBeforeRequest = null;
  let onBeforeSendHeaders = null;
  let onHeadersReceived = null;
  let attempts = 0;

  class MockWebSocket {
    constructor(url, protocol) {
      this.url = url;
      this.protocol = protocol;
      this.listeners = new Map();
      this.sent = [];
      this.attempt = ++attempts;
      calls.sockets.push(this);
      setTimeout(() => {
        if (webSocketMode === "error" ||
            (webSocketMode === "fail-once" && this.attempt === 1)) {
          this.emit("error", {});
          return;
        }
        this.emit("open", {});
      }, 0);
    }
    addEventListener(name, handler) {
      if (!this.listeners.has(name)) this.listeners.set(name, []);
      this.listeners.get(name).push(handler);
    }
    emit(name, event) {
      for (const handler of this.listeners.get(name) ?? []) handler(event);
    }
    send(payload) {
      this.sent.push(JSON.parse(payload));
      if (webSocketMode === "silent") return;
      const request = JSON.parse(payload);
      setTimeout(() => this.emit("message", {
        data: JSON.stringify({
          v: 1,
          id: request.id,
          ok: true,
          data: { version: "0.9.0", pong: true },
        }),
      }), 0);
    }
    close() {
      this.emit("close", { code: 1000 });
    }
  }

  const chrome = {
    runtime: {
      onInstalled: { addListener() {} },
      onStartup: { addListener() {} },
      onMessage: { addListener(fn) { onMessage = fn; } },
    },
    webRequest: {
      onBeforeRequest: { addListener(fn) { onBeforeRequest = fn; } },
      onBeforeSendHeaders: { addListener(fn) { onBeforeSendHeaders = fn; } },
      onHeadersReceived: { addListener(fn) { onHeadersReceived = fn; } },
    },
    contextMenus: {
      removeAll(callback) { callback?.(); },
      create() {},
      onClicked: { addListener() {} },
    },
    tabs: {
      onRemoved: { addListener() {} },
    },
    downloads: {
      onCreated: { addListener(fn) { onCreated = fn; } },
      async cancel(id) { calls.cancel.push(id); },
      async erase(query) { calls.erase.push(query.id); },
      async download(options) {
        calls.download.push(options);
        return 2;
      },
      async pause() {},
      async resume() {},
    },
    storage: {
      local: {
        async get(keys) {
          const result = {};
          for (const key of Array.isArray(keys) ? keys : [keys]) {
            if (key === "autoIntercept") result.autoIntercept = autoIntercept;
            if (key === "bypassDownloads") result.bypassDownloads = bypassDownloads;
          }
          return result;
        },
        async set(values) { calls.storageSets.push(values); },
      },
    },
    permissions: {
      request: async () => cookieGranted,
      contains: async () => cookieGranted,
      onAdded: { addListener() {} },
    },
    cookies: {
      async getAll() {
        return [{
          domain: "example.com",
          path: "/",
          name: "sid",
          value: "secret",
          secure: true,
          session: false,
          expirationDate: 2000000000,
        }];
      },
    },
    action: {
      setBadgeBackgroundColor() {},
      setBadgeText() {},
      setTitle() {},
    },
  };

  const context = vm.createContext({
    chrome,
    WebSocket: MockWebSocket,
    crypto: require("node:crypto").webcrypto,
    navigator: { userAgent: "MockBrowser/1.0" },
    console,
    setTimeout,
    clearTimeout,
    URL,
    URLSearchParams,
    btoa: (binary) => Buffer.from(binary, "binary").toString("base64"),
    Set,
    Promise,
  });
  vm.runInContext(source, context, { filename: "service-worker.js" });

  return {
    calls,
    onCreated: () => onCreated,
    onMessage: () => onMessage,
    onBeforeRequest: () => onBeforeRequest,
    onBeforeSendHeaders: () => onBeforeSendHeaders,
    onHeadersReceived: () => onHeadersReceived,
    context,
  };
}

function downloadItem(overrides = {}) {
  return {
    id: 1,
    state: "in_progress",
    incognito: false,
    url: "https://example.com/file.bin",
    referrer: "https://example.com/page",
    byExtensionId: undefined,
    ...overrides,
  };
}

async function testSuccessfulHandoff() {
  const env = createEnvironment();
  const listener = env.onCreated();
  check(typeof listener === "function", "onCreated listener is registered");

  listener(downloadItem());
  await settle(() => env.calls.cancel.length > 0 && env.calls.erase.length > 0);

  check(env.calls.cancel[0] === 1, "browser download is canceled");
  check(env.calls.erase[0] === 1, "canceled download is removed from the shelf");
  check(env.calls.download.length === 0, "no browser fallback after SwiftDM accepts");
  check(env.calls.sockets.length === 1, "one WebSocket is opened");
  check(env.calls.sockets[0].url === "ws://127.0.0.1:10007/",
    "WebSocket targets the local SwiftDM port");
  check(env.calls.sockets[0].protocol === "swiftdm.v1",
    "WebSocket uses the swiftdm.v1 subprotocol");
  const sent = env.calls.sockets[0].sent[0];
  check(sent.cmd === "addDownload", "service worker sends addDownload");
  check(sent.data.url === "https://example.com/file.bin", "download URL is forwarded");
  check(sent.data.referer === "https://example.com/page", "same-origin referrer is forwarded");
  check(Array.isArray(sent.data.cookies) && sent.data.cookies[0].name === "sid",
    "scoped cookies are forwarded as structured entries");
}

async function testFallbackWhenAppIsDown() {
  const env = createEnvironment({ webSocketMode: "error" });
  env.onCreated()(downloadItem());

  await settle(() => env.calls.cancel.length > 0 && env.calls.download.length > 0);
  check(env.calls.download[0]?.url === "https://example.com/file.bin",
    "browser download is restarted when SwiftDM is unreachable");
  check(env.calls.sockets.length === 2,
    "handoff retries once before restoring the browser download");
}

async function testTransientConnectionRetry() {
  const env = createEnvironment({ webSocketMode: "fail-once" });
  env.onCreated()(downloadItem());

  await settle(() => env.calls.sockets.length === 2 && env.calls.erase.length > 0);
  check(env.calls.cancel.length === 1, "browser item is canceled only once on retry");
  check(env.calls.download.length === 0,
    "no browser fallback when the retry reaches SwiftDM");
}

async function testNoCookiePermissionSendsEmptyArray() {
  const env = createEnvironment({ cookieGranted: false });
  env.onCreated()(downloadItem());
  await settle(() => env.calls.sockets.length > 0 &&
                     env.calls.sockets[0].sent.length > 0);
  const sent = env.calls.sockets[0].sent[0];
  check(Array.isArray(sent.data.cookies) && sent.data.cookies.length === 0,
    "missing cookie permission sends an empty array, not a string");
  check(env.calls.erase.length > 0,
    "download without cookie permission still reaches SwiftDM");
}

async function testDisabledToggle() {
  const env = createEnvironment({ autoIntercept: false });
  env.onCreated()(downloadItem());
  await sleep(30);
  check(env.calls.cancel.length === 0, "disabled auto-intercept leaves the browser download alone");
  check(env.calls.sockets.length === 0, "disabled auto-intercept does not contact SwiftDM");
}

async function testIgnoredItems() {
  const env = createEnvironment();
  const listener = env.onCreated();
  listener(downloadItem({ byExtensionId: "abcdefghijklmnopabcdefghijklmnop" }));
  listener(downloadItem({ id: 3, incognito: true }));
  listener(downloadItem({ id: 4, url: "blob:https://example.com/123" }));
  listener(downloadItem({ id: 5, state: "complete" }));
  await sleep(30);
  check(env.calls.cancel.length === 0, "extension/incognito/non-http/completed items are ignored");
}

async function testCrossOriginReferrerIsDropped() {
  const env = createEnvironment();
  env.onCreated()(downloadItem({ referrer: "https://other.example/page" }));
  await settle(() => env.calls.sockets.length > 0);
  await settle(() => env.calls.sockets[0].sent.length > 0);
  const sent = env.calls.sockets[0].sent[0];
  check(sent.data.referer === "", "cross-origin referrer is not forwarded");
}

async function testBypassKeepsBrowserDownloads() {
  const env = createEnvironment({ bypassDownloads: true });
  env.onCreated()(downloadItem());
  await sleep(30);
  check(env.calls.cancel.length === 0, "bypass leaves the browser download alone");
  check(env.calls.sockets.length === 0, "bypass does not contact SwiftDM");
}

async function testPostBodyAndHeadersAreForwarded() {
  const env = createEnvironment();
  const remember = env.onBeforeRequest();
  const rememberHeaders = env.onBeforeSendHeaders();
  check(typeof remember === "function" && typeof rememberHeaders === "function",
    "webRequest observation listeners are registered");

  remember({
    method: "POST",
    url: "https://example.com/submit",
    requestBody: { formData: { q: ["swiftdm"], page: ["1"] } },
  });
  rememberHeaders({
    url: "https://example.com/submit",
    requestHeaders: [
      { name: "X-Api-Key", value: "secret" },
      { name: "Cookie", value: "not-forwarded" },
      { name: "Referer", value: "https://example.com/page" },
    ],
  });
  env.onCreated()(downloadItem({ url: "https://example.com/submit" }));
  await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);

  const sent = env.calls.sockets[0].sent[0];
  check(sent.data.method === "POST", "observed POST downloads send method=POST");
  check(sent.data.postData === "q=swiftdm&page=1", "form bodies are reconstructed");
  check(sent.data.postDataEncoding === "utf8", "form bodies are sent as utf8");
  check(sent.data.headers?.length === 1 && sent.data.headers[0].name === "x-api-key",
    "only x-* request headers are forwarded");
}

async function testRawPostBodyIsBase64Encoded() {
  const env = createEnvironment();
  const remember = env.onBeforeRequest();
  const bytes = new TextEncoder().encode("a=1&b=2");
  remember({
    method: "POST",
    url: "https://example.com/raw",
    requestBody: { raw: [{ bytes: bytes.buffer }] },
  });
  env.onCreated()(downloadItem({ url: "https://example.com/raw" }));
  await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
  const sent = env.calls.sockets[0].sent[0];
  check(sent.data.postDataEncoding === "base64", "raw bodies are sent as base64");
  check(Buffer.from(sent.data.postData, "base64").toString() === "a=1&b=2",
    "raw bodies survive the round trip");
}

async function testDetectedMediaStreams() {
  const env = createEnvironment();
  const headers = env.onHeadersReceived();
  check(typeof headers === "function", "response header observation listener is registered");

  const media = (url, mime, tabId = 7) =>
    headers({ url, tabId, responseHeaders: [{ name: "Content-Type", value: mime }] });

  media("https://cdn.example/playlist.m3u8", "application/vnd.apple.mpegurl", 1);
  media("https://cdn.example/sd/video.mp4", "video/mp4; codecs=avc1", 2);
  media("https://cdn.example/sd/audio.mp4", "audio/mp4", 2);
  media("https://cdn.example/page.html", "text/html", 2);

  let response = null;
  env.onMessage()({ type: "get-media-streams", tabId: 1 }, {}, (value) => { response = value; });
  check(response?.ok === true && response.media.hls.length === 1,
    "HLS playlists are reported for the tab that requested them");
  check(response.media.pairs.length === 0, "separate tabs are not paired");

  response = null;
  env.onMessage()({ type: "get-media-streams", tabId: 2 }, {}, (value) => { response = value; });
  check(response?.media.pairs.length === 1, "video and audio tracks in one tab are paired");
  check(response.media.pairs[0].video.endsWith("video.mp4") &&
        response.media.pairs[0].audio.endsWith("audio.mp4"),
    "the pair contains the video and audio URLs");
  check(/^pair-[0-9a-f]{8}$/.test(response.media.pairs[0].group),
    "the pair carries a short group token");

  response = null;
  env.onMessage()({ type: "get-media-streams", tabId: 3 }, {}, (value) => { response = value; });
  check(response?.media.pairs.length === 0 && response.media.hls.length === 0,
    "pages without media streams report nothing");
}

async function testPairedHandoffSharesTheGroupToken() {
  const env = createEnvironment();
  const headers = env.onHeadersReceived();
  headers({ url: "https://cdn.example/hd/video.mp4", tabId: 4,
    responseHeaders: [{ name: "Content-Type", value: "video/mp4" }] });
  headers({ url: "https://cdn.example/hd/audio.mp4", tabId: 4,
    responseHeaders: [{ name: "Content-Type", value: "audio/mp4" }] });

  let media = null;
  env.onMessage()({ type: "get-media-streams", tabId: 4 }, {}, (value) => { media = value; });
  const pair = media.media.pairs[0];

  for (const url of [pair.video, pair.audio]) {
    let response = null;
    env.onMessage()({ type: "download-url", url, group: pair.group ?? "",
      pageUrl: "https://cdn.example/watch", title: "Episode", cookiesGranted: false },
      {}, (value) => { response = value; });
    await settle(() => response !== null && env.calls.sockets.length > 0 &&
                        env.calls.sockets[env.calls.sockets.length - 1].sent.length > 0);
    check(response?.ok === true, "detected media handoff is accepted");
    const sent = env.calls.sockets[env.calls.sockets.length - 1].sent[0];
    check(sent.cmd === "addDownload" && sent.data.url === url,
      "the handoff uses addDownload with the detected URL");
    check(sent.data.group === pair.group, "both halves share the pair token");
  }
}

async function testPairTokenIsSanitized() {
  const env = createEnvironment();
  let response = null;
  env.onMessage()({ type: "download-url", url: "https://cdn.example/v.mp4",
    group: "pair token/../etc", pageUrl: "", title: "", cookiesGranted: false },
    {}, (value) => { response = value; });
  await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
  const sent = env.calls.sockets[0].sent[0];
  check(sent.data.group === "pairtokenetc", "group tokens keep only safe characters");
  check(response?.ok === true, "sanitized handoff still succeeds");
}

async function testStatusChannel() {
  const env = createEnvironment();
  let response = null;
  env.onMessage()({ type: "get-connection-status" }, {}, (value) => { response = value; });
  await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);

  const statusSocket = env.calls.sockets[0];
  check(statusSocket.sent[0].cmd === "subscribe", "the status socket subscribes for push events");
  statusSocket.emit("message", {
    data: JSON.stringify({ v: 1, event: "status", data: { waiting: false, active: 2 } }),
  });
  await settle(() => env.calls.storageSets.some((values) => values.appStatus));
  const stored = env.calls.storageSets.find((values) => values.appStatus)?.appStatus;
  check(stored?.waiting === false && stored?.active === 2,
    "status events are stored for the popup");

  await settle(() => response !== null);
  check(response?.ok === true && response.version === "0.9.0",
    "the ping command still answers while subscribed");
}

(async () => {
  await testSuccessfulHandoff();
  await testFallbackWhenAppIsDown();
  await testTransientConnectionRetry();
  await testNoCookiePermissionSendsEmptyArray();
  await testDisabledToggle();
  await testIgnoredItems();
  await testCrossOriginReferrerIsDropped();
  await testBypassKeepsBrowserDownloads();
  await testPostBodyAndHeadersAreForwarded();
  await testRawPostBodyIsBase64Encoded();
  await testDetectedMediaStreams();
  await testPairedHandoffSharesTheGroupToken();
  await testPairTokenIsSanitized();
  await testStatusChannel();

  if (failures) {
    console.error(`${failures} failure(s)`);
    process.exit(1);
  }
  console.log("Extension logic tests passed");
})();
