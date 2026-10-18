const SOCKET_URL = "ws://127.0.0.1:10007/";
const SUBPROTOCOL = "swiftdm.v1";
const MENU_LINK = "swiftdm-download-link";
const MENU_PAGE = "swiftdm-download-page";
const autoIntercepting = new Set();

chrome.runtime.onInstalled.addListener(() => {
  chrome.storage.local.get("autoIntercept", ({ autoIntercept }) => {
    if (typeof autoIntercept !== "boolean") {
      chrome.storage.local.set({ autoIntercept: true });
    }
  });
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: MENU_LINK,
      title: "Download link with SwiftDM",
      contexts: ["link"],
      targetUrlPatterns: ["http://*/*", "https://*/*"]
    });
    chrome.contextMenus.create({
      id: MENU_PAGE,
      title: "Download page with SwiftDM",
      contexts: ["page"],
      documentUrlPatterns: ["http://*/*", "https://*/*"]
    });
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
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

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type === "download-current-tab") {
    void sendDownload(message.url, "", message.title ?? "", {
      useGrantedCookiePermission: message.cookiesGranted === true
    })
      .then(() => sendResponse({ ok: true }))
      .catch((error) => sendResponse({ ok: false, error: errorText(error) }));
    return true;
  }
  if (message?.type === "get-connection-status") {
    void sendCommand("ping", {}).then(
      (response) => sendResponse({ ok: true, version: response.version }),
      (error) => sendResponse({ ok: false, error: errorText(error) })
    );
    return true;
  }
});

chrome.downloads.onCreated.addListener((item) => {
  if (item.state !== "in_progress" || item.incognito || !validHttpUrl(item.url)) return;
  // Downloads started by this extension (the fallback path below) are already
  // handled; intercepting them again would loop forever.
  if (item.byExtensionId) return;
  if (autoIntercepting.has(item.id)) return;
  autoIntercepting.add(item.id);
  void interceptDownload(item).finally(() => autoIntercepting.delete(item.id));
});

async function interceptDownload(item) {
  const { autoIntercept = true } = await chrome.storage.local.get("autoIntercept");
  if (!autoIntercept) return;

  // Cancel immediately rather than pause: a paused item can still finish for
  // tiny files, which is exactly the case where the browser would "win" before
  // SwiftDM could take over. If SwiftDM is unreachable, the fallback below asks
  // the browser to download the URL again.
  try {
    await chrome.downloads.cancel(item.id);
    try { await chrome.downloads.erase({ id: item.id }); } catch { /* already gone */ }
  } catch {
    // The download already completed (for example a tiny data response); keep
    // the browser's copy instead of downloading it twice.
    return;
  }

  try {
    await sendDownloadWithRetry(item.url, item.referrer ?? "", "", {
      useGrantedCookiePermission: true
    });
    clearActionError();
  } catch (error) {
    showActionError(new Error(`SwiftDM unavailable, browser download restarted: ${errorText(error)}`));
    try {
      await chrome.downloads.download({ url: item.url, conflictAction: "uniquify" });
    } catch (fallbackError) {
      showActionError(new Error(
        `SwiftDM unavailable and the browser fallback failed: ${errorText(fallbackError)}`
      ));
    }
  }
}

async function sendDownloadWithRetry(...args) {
  try {
    return await sendDownload(...args);
  } catch {
    // The app may still be starting, or the service worker may have raced the
    // port becoming available. One short retry avoids needlessly restoring the
    // browser download.
    await new Promise((resolve) => setTimeout(resolve, 250));
    return sendDownload(...args);
  }
}

async function sendDownload(rawUrl, pageUrl, pageTitle, options = {}) {
  const url = validHttpUrl(rawUrl);
  if (!url) throw new Error("Only HTTP and HTTPS URLs can be sent to SwiftDM.");

  const permissionOrigin = `${url.protocol}//${url.hostname}/*`;
  const permissions = { permissions: ["cookies"], origins: [permissionOrigin] };
  let granted = false;
  try {
    if (options.requestCookiePermission) {
      granted = await chrome.permissions.request(permissions);
    } else if (options.useGrantedCookiePermission) {
      granted = await chrome.permissions.contains(permissions);
    }
  } catch {
    // Permission prompts are unavailable from some service-worker events.
    // Continue without cookies rather than failing the whole handoff.
    granted = false;
  }
  let cookies = [];
  if (granted) {
    try {
      const siteCookies = await chrome.cookies.getAll({ url: url.href });
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
    pageUrl: validHttpUrl(pageUrl)?.href ?? "",
    pageTitle: String(pageTitle).slice(0, 1024),
    referer: source?.origin === url.origin ? source.href : "",
    cookies,
    userAgent: navigator.userAgent
  };
  return sendCommand("addDownload", data);
}

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

function sendCommand(cmd, data) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const id = crypto.randomUUID();
    let socket;
    const timeout = setTimeout(() => finish(new Error(
      "SwiftDM did not respond on 127.0.0.1:10007. Check that SwiftDM is running and the port is free."
    )), 5000);

    function finish(error, response) {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      try { socket?.close(); } catch { /* already closed */ }
      if (error) reject(error);
      else if (!response.ok) reject(new Error(response.error ?? "SwiftDM rejected the request."));
      else resolve(response.data ?? {});
    }

    try {
      socket = new WebSocket(SOCKET_URL, SUBPROTOCOL);
    } catch (error) {
      finish(new Error(`Cannot create the SwiftDM connection: ${errorText(error)}`));
      return;
    }

    socket.addEventListener("open", () => {
      socket.send(JSON.stringify({ v: 1, cmd, id, data }));
    }, { once: true });
    socket.addEventListener("message", (event) => {
      try {
        const response = JSON.parse(event.data);
        if (response.id !== id) return;
        finish(null, response);
      } catch {
        finish(new Error("SwiftDM sent an invalid response."));
      }
    });
    socket.addEventListener("error", () => finish(new Error(
      "Cannot connect to SwiftDM on 127.0.0.1:10007. Make sure SwiftDM is running and check its Extensions dialog for the listen status or a rejected-handshake reason."
    )), { once: true });
    socket.addEventListener("close", (event) => {
      if (settled) return;
      const detail = event.code && event.code !== 1005 ? ` (close ${event.code})` : "";
      finish(new Error(`SwiftDM closed the connection${detail}.`));
    }, { once: true });
  });
}

function errorText(error) {
  return error instanceof Error ? error.message : String(error);
}

function showActionError(error) {
  const message = errorText(error);
  console.error("SwiftDM integration:", message);
  chrome.action.setBadgeBackgroundColor({ color: "#c43c3c" });
  chrome.action.setBadgeText({ text: "!" });
  chrome.action.setTitle({ title: `SwiftDM: ${message}` });
}

function clearActionError() {
  chrome.action.setBadgeText({ text: "" });
  chrome.action.setTitle({ title: "SwiftDM" });
}
