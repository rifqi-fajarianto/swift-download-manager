// Headless tests for the shared background logic (ext/shared/core.js) as it runs
// in both extension flavours: the Chromium service worker and the Firefox event
// page. Browser APIs and WebSocket are mocked; no browser or GUI is started.
//
// The Chromium flavour loads the files through importScripts (as the real
// service worker does); the Firefox flavour runs them as background.scripts in
// one shared global scope. Both use the same files, so one suite covers both.

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.join(__dirname, "..");
const sharedDir = path.join(root, "ext", "shared");
const chromiumDir = path.join(root, "ext", "chromium");
const firefoxDir = path.join(root, "ext", "firefox");

// Loaded before core.js, in this order, in both browsers. The media helpers are
// part of the worker bootstrap too (they hold the pure detection/segment logic).
const bootFiles = ["api.js", "cookie-permission.js", "action-icon.js",
                   "media-detect.js", "media-mse.js"];

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

// The Firefox flavour is the same code with the promise-based `browser`
// namespace; the Chromium flavour uses callback-style `chrome` calls.
function createEnvironment(options = {}) {
  const {
    autoIntercept = true,
    legacyBypass = false,
    webSocketMode = "ok",
    cookieGranted = true,
    firefox = false,
  } = options;
  const calls = {
    cancel: [],
    erase: [],
    download: [],
    sockets: [],
    storageSets: [],
    storageRemovals: [],
    actionIcons: [],
    actionTitles: [],
    permissionRequests: [],
    permissionChecks: [],
    permissionRemoves: [],
    menuCreates: [],
    tabMessages: [],
    scriptInjections: [],
  };
  // The DOM side of media detection is mocked: the popup asks the worker for a
  // merged list and the worker asks each frame for its best element.
  let domCandidates = [];
  let onInstalled = null;
  let onStorageChanged = null;
  let onCreated = null;
  let onMessage = null;
  let onBeforeRequest = null;
  let onBeforeSendHeaders = null;
  let onHeadersReceived = null;
  let onMenuClicked = null;
  let onTabRemoved = null;
  let attempts = 0;

  class MockWebSocket {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;

    constructor(url, protocol) {
      this.url = url;
      this.protocol = protocol;
      this.listeners = new Map();
      this.sent = [];
      this.attempt = ++attempts;
      this.readyState = 0;
      calls.sockets.push(this);
      setTimeout(() => {
        if (webSocketMode === "error" ||
            (webSocketMode === "fail-once" && this.attempt === 1)) {
          this.readyState = 3;
          this.emit("error", {});
          return;
        }
        this.readyState = 1;
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
      this.readyState = 3;
      this.emit("close", { code: 1000 });
    }
  }

  // Callback-style APIs (Chromium) answer through the last argument; the
  // promise-style variants (Firefox) resolve. Each helper below models both.
  function callbackApi(fn) {
    return function (...args) {
      const callback = typeof args.at(-1) === "function" ? args.pop() : null;
      const value = fn(...args);
      if (callback) {
        Promise.resolve(value).then((result) => setTimeout(() => callback(result), 0));
        return undefined;
      }
      return Promise.resolve(value);
    };
  }

  const chrome = {
    runtime: {
      onInstalled: { addListener(fn) { onInstalled = fn; } },
      onStartup: { addListener() {} },
      onMessage: { addListener(fn) { onMessage = fn; } },
      getManifest: () => ({ version: "0.6.0" }),
    },
    webRequest: {
      onBeforeRequest: { addListener(fn) { onBeforeRequest = fn; } },
      onBeforeSendHeaders: { addListener(fn) { onBeforeSendHeaders = fn; } },
      onHeadersReceived: { addListener(fn) { onHeadersReceived = fn; } },
    },
    contextMenus: {
      removeAll(callback) { callback?.(); },
      create(properties) { calls.menuCreates.push(properties); },
      onClicked: { addListener(fn) { onMenuClicked = fn; } },
    },
    tabs: {
      onRemoved: { addListener(fn) { onTabRemoved = fn; } },
      query: callbackApi(() => [{ id: 1, url: "https://example.com/watch", title: "Video" }]),
      sendMessage: callbackApi((tabId, message) => {
        calls.tabMessages.push({ tabId, message });
        if (message?.type === "scan-media") {
          return domCandidates.map((candidate) => ({ ok: true, candidate }));
        }
        return { ok: true };
      }),
    },
    scripting: {
      executeScript: callbackApi((details) => {
        calls.scriptInjections.push(details);
        return [{ result: true }];
      }),
    },
    downloads: {
      onCreated: { addListener(fn) { onCreated = fn; } },
      cancel: callbackApi((id) => { calls.cancel.push(id); }),
      erase: callbackApi((query) => { calls.erase.push(query.id); }),
      download: callbackApi((options) => {
        calls.download.push(options);
        return 2;
      }),
    },
    storage: {
      onChanged: { addListener(fn) { onStorageChanged = fn; } },
      local: {
        get: callbackApi((keys) => {
          const result = {};
          for (const key of Array.isArray(keys) ? keys : [keys]) {
            if (key === "autoIntercept") result.autoIntercept = autoIntercept;
            if (key === "bypassDownloads" && legacyBypass) result.bypassDownloads = true;
          }
          return result;
        }),
        set: callbackApi((values) => { calls.storageSets.push(values); }),
        remove: callbackApi((key) => { calls.storageRemovals.push(key); }),
      },
    },
    permissions: {
      request: callbackApi((wanted) => {
        calls.permissionRequests.push(wanted);
        return cookieGranted;
      }),
      contains: callbackApi((wanted) => {
        calls.permissionChecks.push(wanted);
        return cookieGranted;
      }),
      remove: callbackApi((wanted) => {
        calls.permissionRemoves.push(wanted);
        return true;
      }),
      onAdded: { addListener() {} },
    },
    cookies: {
      getAll: callbackApi(() => [{
        domain: "example.com",
        path: "/",
        name: "sid",
        value: "secret",
        secure: true,
        session: false,
        expirationDate: 2000000000,
      }]),
    },
    action: {
      setBadgeBackgroundColor() {},
      setBadgeText() {},
      setTitle(options) { calls.actionTitles.push(options?.title ?? null); },
      setIcon(options) { calls.actionIcons.push(options?.path ?? null); },
    },
  };

  const sandbox = {
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
  };
  // Firefox: the promise namespace exists next to the callback one.
  if (firefox) sandbox.browser = chrome;
  sandbox.chrome = chrome;

  const context = vm.createContext(sandbox);
  const read = (file) => fs.readFileSync(path.join(sharedDir, file), "utf8");

  if (firefox) {
    // background.scripts share one global scope: run them in order.
    for (const file of [...bootFiles, "core.js"]) {
      vm.runInContext(read(file), context, { filename: file });
    }
  } else {
    // The service worker imports its shared files the same way a real service
    // worker does, so the test exercises the shipping file.
    sandbox.importScripts = (...files) => {
      for (const file of files) {
        vm.runInContext(read(file), context, { filename: file });
      }
    };
    vm.runInContext(
      fs.readFileSync(path.join(chromiumDir, "background.js"), "utf8"),
      context,
      { filename: "background.js" }
    );
  }

  return {
    calls,
    onInstalled: () => onInstalled,
    onStorageChanged: () => onStorageChanged,
    onCreated: () => onCreated,
    onMessage: () => onMessage,
    onBeforeRequest: () => onBeforeRequest,
    onBeforeSendHeaders: () => onBeforeSendHeaders,
    onHeadersReceived: () => onHeadersReceived,
    onMenuClicked: () => onMenuClicked,
    onTabRemoved: () => onTabRemoved,
    setDomCandidates: (candidates) => { domCandidates = candidates; },
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

// Every scenario runs against both extension flavours.
const flavours = [
  { name: "chromium", options: { firefox: false } },
  { name: "firefox", options: { firefox: true } },
];

function forEachFlavour(scenario) {
  return Promise.all(flavours.map(async (flavour) => {
    const label = `[${flavour.name}]`;
    const wrapped = {
      check: (condition, description) => check(condition, `${label} ${description}`),
      createEnvironment: (options = {}) => createEnvironment({ ...flavour.options, ...options }),
    };
    await scenario(wrapped);
  }));
}

async function testSuccessfulHandoff() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const listener = env.onCreated();
    check(typeof listener === "function", "onCreated listener is registered");

    listener(downloadItem());
    // Wait for the handoff command itself, not only for the cancel calls: the
    // WebSocket round trip is asynchronous and can lag behind under load.
    await settle(() => env.calls.cancel.length > 0 && env.calls.erase.length > 0 &&
      env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);

    check(env.calls.cancel[0] === 1, "browser download is canceled");
    check(env.calls.erase[0] === 1, "canceled download is removed from the shelf");
    check(env.calls.download.length === 0, "no browser fallback after SwiftDM accepts");
    check(env.calls.sockets.length === 1, "one persistent command WebSocket is opened");
    const commandSocket = env.calls.sockets.find((socket) =>
      socket.sent.some((message) => message.cmd === "addDownload")
    );
    check(commandSocket?.url === "ws://127.0.0.1:10007/",
      "WebSocket targets the local SwiftDM port");
    check(commandSocket?.protocol === "swiftdm.v1",
      "WebSocket uses the swiftdm.v1 subprotocol");
    const sent = commandSocket?.sent[0];
    check(sent.cmd === "addDownload", "the background sends addDownload");
    check(sent.data.url === "https://example.com/file.bin", "download URL is forwarded");
    check(sent.data.referer === "https://example.com/page", "same-origin referrer is forwarded");
    check(Array.isArray(sent.data.cookies) && sent.data.cookies[0].name === "sid",
      "scoped cookies are forwarded as structured entries");
  });
}

async function testFirefoxItemWithoutFinalUrl() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    // Firefox's DownloadItem has no finalUrl field at all.
    const env = createEnvironment();
    env.onCreated()(downloadItem({ url: "https://example.com/no-final.bin" }));
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(sent.data.url === "https://example.com/no-final.bin",
      "an item without finalUrl still hands off its URL");
  });
}

async function testSpaceInUrlIsEncodedForHandoff() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    env.onCreated()(downloadItem({
      url: "https://takeout-download-drive.usercontent.google.com/download/Test SwiftDM-20261020T072809Z-1-001.zip?j=test&i=0",
    }));
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(sent.data.url.includes("Test%20SwiftDM"), "space in download path is percent-encoded");
    check(!sent.data.url.includes(" "), "handoff URL contains no raw spaces");
  });
}

async function testCommandSocketIsReused() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const listener = env.onCreated();
    listener(downloadItem({ id: 21 }));
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    listener(downloadItem({ id: 22, url: "https://example.com/second.bin" }));
    await settle(() => env.calls.sockets[0].sent.length === 2);
    check(env.calls.sockets.length === 1,
      "sequential handoffs reuse the open command socket");
    const commandSocket = env.calls.sockets.find((socket) =>
      socket.sent.some((message) => message.cmd === "addDownload")
    );
    check(commandSocket?.sent.filter((message) => message.cmd === "addDownload").length === 2,
      "both handoffs are sent as addDownload commands");
  });
}

async function testFallbackWhenAppIsDown() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ webSocketMode: "error" });
    env.onCreated()(downloadItem());

    await settle(() => env.calls.cancel.length > 0 && env.calls.download.length > 0);
    check(env.calls.download[0]?.url === "https://example.com/file.bin",
      "browser download is restarted when SwiftDM is unreachable");
    check(env.calls.sockets.length === 2,
      "handoff retries once before restoring the browser download");
  });
}

async function testTransientConnectionRetry() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ webSocketMode: "fail-once" });
    env.onCreated()(downloadItem());

    await settle(() => env.calls.sockets.length === 2 && env.calls.erase.length > 0);
    check(env.calls.cancel.length === 1, "browser item is canceled only once on retry");
    check(env.calls.download.length === 0,
      "no browser fallback when the retry reaches SwiftDM");
  });
}

async function testNoCookiePermissionSendsEmptyArray() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ cookieGranted: false });
    env.onCreated()(downloadItem());
    await settle(() => env.calls.sockets.length > 0 &&
                       env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(Array.isArray(sent.data.cookies) && sent.data.cookies.length === 0,
      "missing cookie permission sends an empty array, not a string");
    check(env.calls.erase.length > 0,
      "download without cookie permission still reaches SwiftDM");
  });
}

async function testPopupAsksForTheCookiePermission() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ cookieGranted: false });
    const origin = await vm.runInContext(
      "swiftdmCookies.siteOrigin('https://drive.google.com/file/d/abc/view')",
      env.context
    );
    check(origin === "https://drive.google.com/*",
      "the cookie prompt targets the page origin");
    const noOrigin = await vm.runInContext(
      "swiftdmCookies.siteOrigin('about:addons')",
      env.context
    );
    check(noOrigin === "", "non-HTTP pages have no cookie origin to ask about");

    const granted = await vm.runInContext(
      "swiftdmCookies.request('https://drive.google.com/file/d/abc/view')",
      env.context
    );
    check(granted === false, "a refused prompt reports false");
    check(env.calls.permissionRequests.length === 2,
      "a refused site-scoped prompt falls back to the plain permission");
    check(env.calls.permissionRequests[0].permissions[0] === "cookies" &&
          env.calls.permissionRequests[0].origins[0] === "https://drive.google.com/*",
      "the popup first requests the optional cookie permission for its page");
    check(env.calls.permissionRequests[1].permissions[0] === "cookies" &&
          !env.calls.permissionRequests[1].origins,
      "the fallback asks for the cookie API permission only");
  });
}

async function testPopupHandoffCarriesTheSession() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const listener = env.onMessage();
    const response = await new Promise((resolve) => {
      const handedOff = listener(
        {
          type: "download-current-tab",
          url: "https://drive.google.com/file/d/abc/view",
          title: "Drive",
          cookiesGranted: true,
        },
        {},
        resolve
      );
      check(handedOff === true, "the popup handoff answers asynchronously");
    });
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(response?.ok === true, "the popup handoff is accepted");
    check(sent.cmd === "addDownload" &&
          Array.isArray(sent.data.cookies) &&
          sent.data.cookies.length === 1 &&
          sent.data.cookies[0].name === "sid",
      "cookies travel with a popup handoff when the toggle is on");
    check(env.calls.permissionChecks.length === 1,
      "an enabled toggle only checks the granted cookie permission");
  });
}

async function testPopupHandoffWithoutCookies() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const listener = env.onMessage();
    await new Promise((resolve) => {
      listener(
        {
          type: "download-current-tab",
          url: "https://drive.google.com/file/d/abc/view",
          title: "Drive",
          cookiesGranted: false,
        },
        {},
        resolve
      );
    });
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(Array.isArray(sent.data.cookies) && sent.data.cookies.length === 0,
      "the popup toggle off sends no cookies");
    check(env.calls.permissionChecks.length === 0 &&
          env.calls.permissionRequests.length === 0,
      "the background never prompts for the cookie permission");
  });
}

async function testToolbarIconFollowsTheSwitch() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ autoIntercept: true });
    await settle(() => env.calls.actionIcons.length > 0);
    check(env.calls.actionIcons[0]?.[16] === "icons/icon16.png",
      "the active toolbar icon is applied on start");

    env.onStorageChanged()({ autoIntercept: { newValue: false } }, "local");
    await settle(() => env.calls.actionIcons.at(-1)?.[16] === "icons/icon-off16.png");
    check(env.calls.actionIcons.at(-1)?.[16] === "icons/icon-off16.png",
      "turning automatic handoff off swaps to the deactivated icon");
    env.onStorageChanged()({ autoIntercept: { newValue: true } }, "local");
    await settle(() => env.calls.actionIcons.at(-1)?.[16] === "icons/icon16.png");
    check(env.calls.actionIcons.at(-1)?.[16] === "icons/icon16.png",
      "turning it back on restores the active icon");

    const before = env.calls.actionIcons.length;
    env.onStorageChanged()({ appStatus: { newValue: {} } }, "local");
    await sleep(10);
    check(env.calls.actionIcons.length === before,
      "unrelated storage changes leave the toolbar icon alone");
  });
}

async function testDisabledToggle() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ autoIntercept: false });
    env.onCreated()(downloadItem());
    await sleep(30);
    check(env.calls.cancel.length === 0,
      "disabled auto-intercept leaves the browser download alone");
    check(env.calls.sockets.length === 0, "disabled auto-intercept does not contact SwiftDM");
  });
}

async function testIgnoredItems() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const listener = env.onCreated();
    listener(downloadItem({ byExtensionId: "abcdefghijklmnopabcdefghijklmnop" }));
    listener(downloadItem({ id: 3, incognito: true }));
    listener(downloadItem({ id: 4, url: "blob:https://example.com/123" }));
    listener(downloadItem({ id: 5, state: "complete" }));
    await sleep(30);
    check(env.calls.cancel.length === 0,
      "extension/incognito/non-http/completed items are ignored");
  });
}

async function testCrossOriginReferrerIsDropped() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    env.onCreated()(downloadItem({ referrer: "https://other.example/page" }));
    await settle(() => env.calls.sockets.length > 0);
    await settle(() => env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(sent.data.referer === "", "cross-origin referrer is not forwarded");
  });
}

async function testLegacyBypassMigratesToAutoInterceptOff() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment({ legacyBypass: true });
    env.onInstalled()();
    await settle(() => env.calls.storageSets.length > 0 && env.calls.storageRemovals.length > 0);
    check(env.calls.storageSets[0].autoIntercept === false,
      "a stored bypass carries over as automatic interception off");
    check(env.calls.storageRemovals[0] === "bypassDownloads",
      "the obsolete bypass key is removed");
  });
}

async function testContextMenusAreRecreated() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    env.onInstalled()();
    await settle(() => env.calls.menuCreates.length === 2);
    check(env.calls.menuCreates.length === 2, "both context menus are created on install");
    check(env.calls.menuCreates[0]?.contexts?.[0] === "link", "the link menu targets links");
    check(env.calls.menuCreates[1]?.contexts?.[0] === "page", "the page menu targets pages");
  });
}

async function testPostBodyAndHeadersAreForwarded() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
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
  });
}

async function testRawPostBodyIsBase64Encoded() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
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
  });
}

async function testDetectedMediaStreams() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const headers = env.onHeadersReceived();
    check(typeof headers === "function",
      "response header observation listener is registered");

    const media = (url, mime, tabId = 7) =>
      headers({ url, tabId, responseHeaders: [{ name: "Content-Type", value: mime }] });

    media("https://cdn.example/playlist.m3u8", "application/vnd.apple.mpegurl", 1);
    media("https://cdn.example/sd/video.mp4", "video/mp4; codecs=avc1", 2);
    media("https://cdn.example/sd/audio.mp4", "audio/mp4", 2);
    media("https://cdn.example/page.html", "text/html", 2);

    // The handler answers asynchronously (it also asks the content script for a
    // DOM scan), so each request waits for its response.
    const ask = async (tabId) => {
      let value = null;
      env.onMessage()({ type: "get-media-streams", tabId }, {}, (answer) => { value = answer; });
      await settle(() => value !== null);
      return value;
    };

    let response = await ask(1);
    check(response?.ok === true && response.media.hls.length === 1,
      "HLS playlists are reported for the tab that requested them");
    check(response.media.pairs.length === 0, "separate tabs are not paired");

    response = await ask(2);
    check(response?.media.pairs.length === 1, "video and audio tracks in one tab are paired");
    check(response.media.pairs[0].video.endsWith("video.mp4") &&
          response.media.pairs[0].audio.endsWith("audio.mp4"),
      "the pair contains the video and audio URLs");
    check(/^pair-[0-9a-f]{8}$/.test(response.media.pairs[0].group),
      "the pair carries a short group token");

    response = await ask(3);
    check(response?.media.pairs.length === 0 && response.media.hls.length === 0,
      "pages without media streams report nothing");

    // A closed tab drops its observations.
    env.onTabRemoved()(2);
    response = await ask(2);
    check(response?.media.pairs.length === 0,
      "closing a tab forgets the media observed in it");
  });
}

async function testPairedHandoffSharesTheGroupToken() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const headers = env.onHeadersReceived();
    headers({ url: "https://cdn.example/hd/video.mp4", tabId: 4,
      responseHeaders: [{ name: "Content-Type", value: "video/mp4" }] });
    headers({ url: "https://cdn.example/hd/audio.mp4", tabId: 4,
      responseHeaders: [{ name: "Content-Type", value: "audio/mp4" }] });

    let media = null;
    env.onMessage()({ type: "get-media-streams", tabId: 4 }, {}, (value) => { media = value; });
    await settle(() => media !== null);
    const pair = media.media.pairs[0];

    for (const url of [pair.video, pair.audio]) {
      let response = null;
      env.onMessage()({ type: "download-url", url, group: pair.group ?? "",
        pageUrl: "https://cdn.example/watch", title: "Episode", cookiesGranted: false },
        {}, (value) => { response = value; });
      await settle(() => response !== null && env.calls.sockets.length > 0 &&
                          env.calls.sockets[env.calls.sockets.length - 1].sent.length > 0);
      check(response?.ok === true, "detected media handoff is accepted");
      const commandSocket = env.calls.sockets.find((socket) =>
        socket.sent.some((message) => message.cmd === "addDownload" && message.data.url === url)
      );
      const sent = commandSocket?.sent.find((message) => message.data.url === url);
      check(sent.cmd === "addDownload" && sent.data.url === url,
        "the handoff uses addDownload with the detected URL");
      check(sent.data.group === pair.group, "both halves share the pair token");
    }
  });
}

async function testPairTokenIsSanitized() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    let response = null;
    env.onMessage()({ type: "download-url", url: "https://cdn.example/v.mp4",
      group: "pair token/../etc", pageUrl: "", title: "", cookiesGranted: false },
      {}, (value) => { response = value; });
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);
    const sent = env.calls.sockets[0].sent[0];
    check(sent.data.group === "pairtokenetc", "group tokens keep only safe characters");
    // The mock answers on a timer; waiting for the socket alone races the reply
    // on platforms with coarse timer granularity.
    await settle(() => response !== null);
    check(response?.ok === true, "sanitized handoff still succeeds");
  });
}

async function testStatusChannel() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    let response = null;
    env.onMessage()({ type: "get-connection-status" }, {}, (value) => { response = value; });
    await settle(() => env.calls.sockets.length > 0 && env.calls.sockets[0].sent.length > 0);

    const statusSocket = env.calls.sockets[0];
    check(statusSocket.sent[0].cmd === "subscribe",
      "the status socket subscribes for push events");
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
  });
}

async function testMediaDomScanIsMerged() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const headers = env.onHeadersReceived();
    headers({ url: "https://cdn.example/watch.m3u8", tabId: 9, responseHeaders: [
      { name: "Content-Type", value: "application/vnd.apple.mpegurl" }
    ] });

    // The content script reports the media element it found.
    env.setDomCandidates([{
      url: "https://cdn.example/direct.mp4",
      kind: "file",
      elementTag: "video",
      durationSec: 120,
      positionSec: 8,
      protected: false,
      videoWidth: 1920,
      videoHeight: 1080
    }]);

    let media = null;
    env.onMessage()({ type: "get-media-streams", tabId: 9 }, {}, (value) => { media = value; });
    await settle(() => media !== null);

    check(media?.ok === true, "the merged media list answers");
    check(media.media.hls.length === 1, "the sniffed HLS playlist is still listed");
    check(media.media.dom.length === 1, "the DOM candidate is merged in");
    check(media.media.dom[0].url === "https://cdn.example/direct.mp4",
      "the DOM candidate keeps its URL");
    check(media.media.dom[0].videoHeight === 1080, "the DOM candidate keeps its size");

    check(env.calls.tabMessages.some((entry) => entry.message?.type === "scan-media"),
      "the worker asks the content script for a scan");
  });
}

async function testMseStreamIsOffered() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    const headers = env.onHeadersReceived();
    // A blob: player fetches an init segment and numbered media segments.
    headers({ url: "https://cdn.example/mse/init.mp4", tabId: 11,
      responseHeaders: [{ name: "Content-Type", value: "video/iso.segment" }] });
    for (let i = 1; i <= 3; i++) {
      headers({ url: `https://cdn.example/mse/seg-${i}.m4s`, tabId: 11,
        responseHeaders: [{ name: "Content-Type", value: "video/iso.segment" }] });
    }

    let media = null;
    env.onMessage()({ type: "get-media-streams", tabId: 11 }, {}, (value) => { media = value; });
    await settle(() => media !== null);

    check(media.media.mse.length === 1, "one MSE stream is reported");
    check(media.media.mse[0].segmentCount === 3, "the media segment count is reported");
    check(media.media.mse[0].initUrl.endsWith("init.mp4"), "the init segment is reported");
    check(media.media.mse[0].segments[0].role === "init", "the init segment sorts first");
    check(media.media.mse[0].segments[1].url.endsWith("seg-1.m4s"),
      "the media segments keep their order");
    check(media.media.mse[0].incomplete === false, "a complete log is not marked incomplete");
    check(media.media.pairs.length === 0, "segments are not offered as a track pair");
  });
}

async function testAddMediaHandoff() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    let response = null;
    env.onMessage()({
      type: "download-media",
      tabId: 3,
      candidate: {
        url: "https://cdn.example/master.m3u8",
        kind: "hls",
        pageUrl: "https://cdn.example/watch",
        pageTitle: "Episode 1",
        quality: "1080p",
        group: "pair-1234abcd",
        durationSec: 600,
        positionSec: 30,
        elementTag: "video",
        protected: false,
        candidates: [
          { url: "https://cdn.example/1080.m3u8", kind: "hls", label: "1080p", bandwidth: 5000000 },
          { url: "https://cdn.example/720.m3u8", kind: "hls", label: "720p", bandwidth: 2500000 }
        ]
      }
    }, {}, (value) => { response = value; });
    await settle(() => response !== null);
    check(response?.ok === true, "the media handoff is accepted");

    await settle(() => env.calls.sockets.some((socket) =>
      socket.sent.some((message) => message.cmd === "addMedia")));
    const socket = env.calls.sockets.find((candidate) =>
      candidate.sent.some((message) => message.cmd === "addMedia"));
    const sent = socket.sent.find((message) => message.cmd === "addMedia");
    check(Boolean(sent), "addMedia is sent to the app");
    check(sent.data.url === "https://cdn.example/master.m3u8", "addMedia carries the URL");
    check(sent.data.kind === "hls", "addMedia carries the kind");
    check(sent.data.quality === "1080p", "addMedia carries the quality label");
    check(sent.data.group === "pair-1234abcd", "addMedia carries the group token");
    check(sent.data.candidates.length === 2, "addMedia carries the variant list");
    check(sent.data.durationSec === 600, "addMedia carries the duration");
    check(sent.data.positionSec === 30, "addMedia carries the play position");
    check(sent.data.pageTitle === "Episode 1", "addMedia carries the page title");
    check(sent.data.protected === false, "addMedia carries the protection flag");
  });
}

async function testMediaRejectsProtectedAndBlob() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();

    let response = null;
    env.onMessage()({
      type: "download-media",
      candidate: { url: "https://cdn.example/drm.mpd", kind: "dash", protected: true }
    }, {}, (value) => { response = value; });
    await settle(() => response !== null);
    check(response?.ok === false, "protected media is refused");
    check(/DRM|protected/i.test(response.error ?? ""), "the refusal explains DRM");

    response = null;
    env.onMessage()({
      type: "download-media",
      candidate: { url: "blob:https://site.example/abc", kind: "file" }
    }, {}, (value) => { response = value; });
    await settle(() => response !== null);
    check(response?.ok === false, "a blob: URL without segments is refused");

    check(!env.calls.sockets.some((socket) =>
      socket.sent.some((message) => message.cmd === "addMedia")),
      "no addMedia is sent for a refused candidate");
  });
}

async function testConfigPushReachesContentScripts() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    env.onMessage()({ type: "get-connection-status" }, {}, () => {});
    await settle(() => env.calls.sockets.some((candidate) =>
      candidate.sent.some((message) => message.cmd === "subscribe")));

    const socket = env.calls.sockets.find((candidate) =>
      candidate.sent.some((message) => message.cmd === "subscribe"));
    check(Boolean(socket), "a socket subscribes to app events");
    socket.emit("message", {
      data: JSON.stringify({ v: 1, event: "config", data: { mediaOverlay: false } })
    });
    await settle(() => env.calls.storageSets.some((entry) => entry.appConfig));
    check(env.calls.storageSets.some((entry) => entry.appConfig?.mediaOverlay === false),
      "the pushed config is persisted");
    await settle(() => env.calls.tabMessages.some((entry) =>
      entry.message?.type === "media-config" && entry.message.mediaOverlay === false));
    check(env.calls.tabMessages.some((entry) =>
      entry.message?.type === "media-config" && entry.message.mediaOverlay === false),
      "the content scripts are told to hide the overlay");
  });
}

async function testPairingTokenTravelsOnTheSocketUrl() {
  await forEachFlavour(async ({ check, createEnvironment }) => {
    const env = createEnvironment();
    env.onMessage()({ type: "get-connection-status" }, {}, () => {});
    await settle(() => env.calls.sockets.some((candidate) =>
      candidate.sent.some((message) => message.cmd === "subscribe")));
    const socket = env.calls.sockets.find((candidate) =>
      candidate.sent.some((message) => message.cmd === "subscribe"));
    check(!socket.url.includes("?t="), "the first connection carries no token");

    socket.emit("message", {
      data: JSON.stringify({ v: 1, event: "pair", data: { token: "tok-abc" } })
    });
    await settle(() => env.calls.storageSets.some((entry) => entry.pairingToken === "tok-abc"));
    check(env.calls.storageSets.some((entry) => entry.pairingToken === "tok-abc"),
      "the pairing token is stored");
  });
}

(async () => {
  await testSuccessfulHandoff();
  await testFirefoxItemWithoutFinalUrl();
  await testSpaceInUrlIsEncodedForHandoff();
  await testCommandSocketIsReused();
  await testFallbackWhenAppIsDown();
  await testTransientConnectionRetry();
  await testNoCookiePermissionSendsEmptyArray();
  await testPopupAsksForTheCookiePermission();
  await testPopupHandoffCarriesTheSession();
  await testPopupHandoffWithoutCookies();
  await testDisabledToggle();
  await testToolbarIconFollowsTheSwitch();
  await testIgnoredItems();
  await testCrossOriginReferrerIsDropped();
  await testLegacyBypassMigratesToAutoInterceptOff();
  await testContextMenusAreRecreated();
  await testPostBodyAndHeadersAreForwarded();
  await testRawPostBodyIsBase64Encoded();
  await testDetectedMediaStreams();
  await testPairedHandoffSharesTheGroupToken();
  await testPairTokenIsSanitized();
  await testStatusChannel();
  await testMediaDomScanIsMerged();
  await testMseStreamIsOffered();
  await testAddMediaHandoff();
  await testMediaRejectsProtectedAndBlob();
  await testConfigPushReachesContentScripts();
  await testPairingTokenTravelsOnTheSocketUrl();

  if (failures) {
    console.error(`${failures} failure(s)`);
    process.exit(1);
  }
  console.log("Extension logic tests passed");
})();
