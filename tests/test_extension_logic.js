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

function createEnvironment({ autoIntercept = true, webSocketMode = "ok",
                             cookieGranted = true } = {}) {
  const calls = {
    cancel: [],
    erase: [],
    download: [],
    sockets: [],
  };
  let onCreated = null;
  let onMessage = null;
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
      onMessage: { addListener(fn) { onMessage = fn; } },
    },
    contextMenus: {
      removeAll(callback) { callback?.(); },
      create() {},
      onClicked: { addListener() {} },
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
        async get(key) {
          if (key === "autoIntercept") return { autoIntercept };
          return {};
        },
        async set() {},
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
    Set,
    Promise,
  });
  vm.runInContext(source, context, { filename: "service-worker.js" });

  return {
    calls,
    onCreated: () => onCreated,
    onMessage: () => onMessage,
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

(async () => {
  await testSuccessfulHandoff();
  await testFallbackWhenAppIsDown();
  await testTransientConnectionRetry();
  await testNoCookiePermissionSendsEmptyArray();
  await testDisabledToggle();
  await testIgnoredItems();
  await testCrossOriginReferrerIsDropped();

  if (failures) {
    console.error(`${failures} failure(s)`);
    process.exit(1);
  }
  console.log("Extension logic tests passed");
})();
