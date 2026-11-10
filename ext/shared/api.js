// Thin compatibility layer over the WebExtension APIs, so the shared extension
// files (core.js, popup.js, cookie-permission.js, action-icon.js) run unchanged
// in Chromium (Manifest V3 service worker) and in Firefox (Manifest V3 event
// page).
//
// Firefox exposes promise-returning APIs under `browser.*`; Chromium exposes
// callback-based APIs under `chrome.*` (Manifest V3 also returns promises from
// the action APIs). Every wrapper below returns a promise, so callers never
// branch on the browser:
//
//   const values = await swiftdmApi.storage.get(["autoIntercept"]);
//   await swiftdmApi.action.setIcon({ path: "icons/icon16.png" });
//
// `chrome` and `browser` are the only globals read here, and the file works as
// a classic script both in a service worker (via importScripts) and in an event
// page (via background.scripts).

const swiftdmApi = (() => {
  const global = globalThis;
  // Firefox defines `browser`; Chromium only defines `chrome`. Both are read
  // lazily so the file also loads in a Node test harness that provides one.
  const browserApi = typeof global.browser !== "undefined" ? global.browser : null;
  const chromeApi = typeof global.chrome !== "undefined" ? global.chrome : null;
  const isFirefox = browserApi !== null;
  const api = browserApi ?? chromeApi ?? {};

  function unavailable(name) {
    return new Error(`this browser does not provide ${name}()`);
  }

  function asError(error) {
    return error instanceof Error ? error : new Error(String(error));
  }

  // Callback-style APIs: storage, tabs, permissions, cookies, downloads and
  // runtime.sendMessage. Firefox returns a promise; Chromium is called with a
  // callback and reports failures through `runtime.lastError`.
  function call(target, method, ...args) {
    const fn = target?.[method];
    if (typeof fn !== "function") return Promise.reject(unavailable(method));
    if (isFirefox) {
      try {
        return Promise.resolve(fn.call(target, ...args));
      } catch (error) {
        return Promise.reject(asError(error));
      }
    }
    return new Promise((resolve, reject) => {
      let settled = false;
      const settle = (error, value) => {
        if (settled) return;
        settled = true;
        if (error) reject(asError(error));
        else resolve(value);
      };
      try {
        const result = fn.call(target, ...args, (value) => {
          const lastError = chromeApi?.runtime?.lastError;
          settle(lastError ? lastError.message ?? String(lastError) : null, value);
        });
        // Newer Chromium also returns a promise when no callback is expected;
        // both completion paths are safe because only the first one settles.
        if (result && typeof result.then === "function") {
          result.then((value) => settle(null, value), (error) => settle(error));
        }
      } catch (error) {
        settle(error);
      }
    });
  }

  // Promise-only APIs: the Manifest V3 action namespace returns a promise in
  // both browsers and takes no callback in Chromium.
  function callPromise(target, method, ...args) {
    const fn = target?.[method];
    if (typeof fn !== "function") return Promise.reject(unavailable(method));
    try {
      return Promise.resolve(fn.call(target, ...args));
    } catch (error) {
      return Promise.reject(asError(error));
    }
  }

  // Synchronously returning APIs whose failures surface as `runtime.lastError`:
  // contextMenus.create. Firefox returns a promise, Chromium returns the id.
  function callSync(target, method, ...args) {
    const fn = target?.[method];
    if (typeof fn !== "function") return Promise.reject(unavailable(method));
    try {
      const result = fn.call(target, ...args);
      if (result && typeof result.then === "function") return Promise.resolve(result);
      const lastError = chromeApi?.runtime?.lastError;
      if (lastError) return Promise.reject(new Error(lastError.message ?? String(lastError)));
      return Promise.resolve(result);
    } catch (error) {
      return Promise.reject(asError(error));
    }
  }

  // Event registration is synchronous in both browsers and returns whether the
  // event exists, so callers can register optional events without guarding.
  function listen(event, handler) {
    if (event && typeof event.addListener === "function") {
      event.addListener(handler);
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // storage.local
  // -------------------------------------------------------------------------

  const storage = {
    async get(keys) {
      if (!api.storage?.local) return {};
      const values = await call(api.storage.local, "get", keys);
      return values && typeof values === "object" ? values : {};
    },
    async set(values) {
      if (!api.storage?.local) return;
      await call(api.storage.local, "set", values);
    },
    async remove(keys) {
      if (!api.storage?.local) return;
      await call(api.storage.local, "remove", keys);
    },
    onChanged(handler) {
      return listen(api.storage?.onChanged, handler);
    },
  };

  // -------------------------------------------------------------------------
  // runtime
  // -------------------------------------------------------------------------

  const runtime = {
    // Manifest of the running extension; the popup uses its version.
    manifest() {
      try {
        return api.runtime?.getManifest?.() ?? {};
      } catch {
        return {};
      }
    },
    onInstalled(handler) {
      return listen(api.runtime?.onInstalled, handler);
    },
    onStartup(handler) {
      return listen(api.runtime?.onStartup, handler);
    },
    // The handler answers through `sendResponse` and returns true for async
    // replies; Firefox accepts the same contract, so no adapter is needed.
    onMessage(handler) {
      return listen(api.runtime?.onMessage, handler);
    },
    async sendMessage(message) {
      return call(api.runtime, "sendMessage", message);
    },
    lastError() {
      return chromeApi?.runtime?.lastError?.message ?? "";
    },
  };

  // -------------------------------------------------------------------------
  // tabs, downloads, menus, action, permissions, cookies
  // -------------------------------------------------------------------------

  const tabs = {
    async query(info) {
      const value = await call(api.tabs, "query", info);
      return Array.isArray(value) ? value : [];
    },
    // Sends a message to the content scripts of one tab. Without an explicit
    // frameId the browser delivers to every frame and answers with one response
    // per frame (or a single response when only one frame listens), so the
    // caller always gets an array.
    async sendMessage(tabId, message) {
      try {
        const value = await call(api.tabs, "sendMessage", tabId, message);
        return Array.isArray(value) ? value.filter(Boolean) : (value ? [value] : []);
      } catch {
        // No content script in that tab/frame: not an error for the caller.
        return [];
      }
    },
    onRemoved(handler) {
      return listen(api.tabs?.onRemoved, handler);
    },
    onUpdated(handler) {
      return listen(api.tabs?.onUpdated, handler);
    },
  };

  const downloads = {
    onCreated(handler) {
      return listen(api.downloads?.onCreated, handler);
    },
    async cancel(id) {
      return call(api.downloads, "cancel", id);
    },
    async erase(query) {
      return call(api.downloads, "erase", query);
    },
    async download(options) {
      return call(api.downloads, "download", options);
    },
  };

  // Firefox calls the context-menu API `menus` and keeps `contextMenus` as an
  // alias; Chromium only has `contextMenus`.
  const menuApi = () => api.menus ?? api.contextMenus;

  const menus = {
    // Failures are swallowed on purpose: a duplicate id or a missing API must
    // not abort the install handler that recreates the menus.
    async removeAll() {
      try {
        await call(menuApi(), "removeAll");
      } catch {
        // ignored: the menus are recreated right after
      }
    },
    async create(properties) {
      try {
        await callSync(menuApi(), "create", properties);
        return true;
      } catch {
        // ignored: a missing menu is a convenience loss, not a handoff failure
        return false;
      }
    },
    onClicked(handler) {
      return listen(menuApi()?.onClicked, handler);
    },
  };

  // Firefox calls the toolbar API `action` for Manifest V3 and keeps
  // `browserAction` for Manifest V2; Chromium uses `action` for MV3.
  const actionApi = () => api.action ?? api.browserAction;

  const action = {
    async setIcon(options) {
      await callPromise(actionApi(), "setIcon", options);
    },
    async setBadgeText(options) {
      await callPromise(actionApi(), "setBadgeText", options);
    },
    async setBadgeBackgroundColor(options) {
      await callPromise(actionApi(), "setBadgeBackgroundColor", options);
    },
    async setTitle(options) {
      await callPromise(actionApi(), "setTitle", options);
    },
  };

  const permissions = {
    async contains(wanted) {
      return (await call(api.permissions, "contains", wanted)) === true;
    },
    async request(wanted) {
      return (await call(api.permissions, "request", wanted)) === true;
    },
    async remove(wanted) {
      return (await call(api.permissions, "remove", wanted)) === true;
    },
    onAdded(handler) {
      return listen(api.permissions?.onAdded, handler);
    },
  };

  const cookies = {
    async getAll(query) {
      const value = await call(api.cookies, "getAll", query);
      return Array.isArray(value) ? value : [];
    },
  };

  // webRequest listeners share one signature in both browsers:
  // addListener(handler, filter, extraInfoSpec). `available()` is false when the
  // browser withheld the API, for example because the user revoked the host
  // permission Firefox asks for.
  const webRequest = {
    available() {
      return Boolean(api.webRequest?.onBeforeRequest);
    },
    listen(eventName, handler, filter, extraInfoSpec) {
      const event = api.webRequest?.[eventName];
      if (!event) return false;
      event.addListener(handler, filter, extraInfoSpec);
      return true;
    },
  };

  // `scripting` injects the MAIN-world hook (page-hook.js). It is optional: an
  // older Firefox without it falls back to a web-accessible <script> tag.
  const scripting = {
    available() {
      return Boolean(api.scripting?.executeScript);
    },
    async executeInPage(tabId, files) {
      if (!api.scripting?.executeScript) throw unavailable("executeScript");
      return call(api.scripting, "executeScript", {
        target: { tabId, allFrames: true },
        files,
        world: "MAIN",
      });
    },
  };

  return {
    isFirefox,
    storage,
    runtime,
    tabs,
    downloads,
    menus,
    action,
    permissions,
    cookies,
    webRequest,
    scripting,
  };
})();
