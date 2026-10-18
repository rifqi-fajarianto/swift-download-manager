const SOCKET_URL = "ws://127.0.0.1:10007/";
const SUBPROTOCOL = "swiftdm.v1";
const MENU_LINK = "swiftdm-download-link";
const MENU_PAGE = "swiftdm-download-page";

chrome.runtime.onInstalled.addListener(() => {
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
    void sendDownload(info.linkUrl, info.pageUrl, tab?.title ?? "", true)
      .then(() => clearActionError())
      .catch(showActionError);
  } else if (info.menuItemId === MENU_PAGE) {
    void sendDownload(info.pageUrl, info.pageUrl, tab?.title ?? "", true)
      .then(() => clearActionError())
      .catch(showActionError);
  }
});

chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
  if (message?.type === "download-current-tab") {
    void sendDownload(message.url, "", message.title ?? "", false,
      message.cookiesGranted === true)
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

async function sendDownload(rawUrl, pageUrl, pageTitle, allowPermissionPrompt,
                            cookiesPermissionGranted = false) {
  const url = validHttpUrl(rawUrl);
  if (!url) throw new Error("Only HTTP and HTTPS URLs can be sent to SwiftDM.");

  const permissionOrigin = `${url.protocol}//${url.hostname}/*`;
  const permissions = { permissions: ["cookies"], origins: [permissionOrigin] };
  let granted = false;
  if (allowPermissionPrompt) {
    granted = await chrome.permissions.request(permissions);
  } else if (cookiesPermissionGranted) {
    granted = await chrome.permissions.contains(permissions);
  }
  let cookies = "";
  if (granted) {
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
    const socket = new WebSocket(SOCKET_URL, SUBPROTOCOL);
    const timeout = setTimeout(() => finish(new Error("SwiftDM did not respond.")), 5000);

    function finish(error, response) {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      try { socket.close(); } catch { /* already closed */ }
      if (error) reject(error);
      else if (!response.ok) reject(new Error(response.error ?? "SwiftDM rejected the request."));
      else resolve(response.data ?? {});
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
    socket.addEventListener("error", () => finish(new Error("Cannot connect to SwiftDM.")), { once: true });
    socket.addEventListener("close", () => {
      if (!settled) finish(new Error("SwiftDM closed the connection."));
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
