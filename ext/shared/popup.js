const status = document.querySelector("#status");
const appStatus = document.querySelector("#app-status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");
const cookies = document.querySelector("#cookies");
const media = document.querySelector("#media");
const mediaList = document.querySelector("#media-list");
const appVersion = document.querySelector("#app-version");
const connection = document.querySelector("#connection");
const connectionLabel = document.querySelector("#connection-label");

let lastAppStatus = null;

// The switches are plain buttons with role="switch", so their state lives in
// aria-checked instead of a checkbox input.
function setSwitch(element, enabled) {
  element.setAttribute("aria-checked", enabled ? "true" : "false");
}

function switchState(element) {
  return element.getAttribute("aria-checked") === "true";
}

void (async () => {
  const values = await swiftdmApi.storage.get(["autoIntercept", "appStatus"]);
  setSwitch(autoIntercept, values.autoIntercept !== false);
  await swiftdmActionIcon.apply(values.autoIntercept !== false);
  renderAppStatus(values.appStatus ?? null);
})();

autoIntercept.addEventListener("click", () => {
  const enabled = !switchState(autoIntercept);
  setSwitch(autoIntercept, enabled);
  void swiftdmApi.storage.set({ autoIntercept: enabled });
  void swiftdmActionIcon.apply(enabled);
  renderAppStatus(null);
});

swiftdmApi.storage.onChanged((changes, area) => {
  if (area !== "local") return;
  if (changes.appStatus) renderAppStatus(changes.appStatus.newValue ?? null);
  if (changes.autoIntercept) {
    setSwitch(autoIntercept, changes.autoIntercept.newValue !== false);
    void swiftdmActionIcon.apply(changes.autoIntercept.newValue !== false);
    renderAppStatus(null);
  }
});

function renderAppStatus(value) {
  if (value !== null) lastAppStatus = value;
  if (!switchState(autoIntercept)) {
    appStatus.hidden = false;
    appStatus.className = "status";
    appStatus.textContent = "Automatic handoff is off: new downloads stay in the browser.";
    return;
  }
  if (!lastAppStatus) {
    appStatus.hidden = true;
    appStatus.textContent = "";
    return;
  }
  const active = Number(lastAppStatus.active ?? 0);
  appStatus.hidden = false;
  appStatus.className = "status";
  appStatus.textContent = lastAppStatus.waiting
    ? "SwiftDM is idle."
    : `SwiftDM is downloading ${active} file${active === 1 ? "" : "s"}.`;
}

// tone is "error" for failures, "ok" for confirmations and "" for neutral
// progress messages.
function setStatus(text, tone = "") {
  status.textContent = text;
  status.classList.toggle("error", tone === "error");
  status.classList.toggle("ok", tone === "ok");
}

// The pill in the header mirrors the connection state; the line below it
// carries the details (and the errors).
function setConnection(state, label) {
  connection.classList.toggle("busy", state === "busy");
  connection.classList.toggle("off", state === "off");
  connectionLabel.textContent = label;
}

function extensionVersion() {
  return swiftdmApi.runtime.manifest().version ?? "";
}

async function checkConnection() {
  download.disabled = true;
  retry.hidden = true;
  setConnection("busy", "Checking…");
  setStatus("Checking connection…");
  let response = null;
  try {
    response = await swiftdmApi.runtime.sendMessage({ type: "get-connection-status" });
  } catch (error) {
    setConnection("off", "Offline");
    setStatus(error instanceof Error ? error.message : String(error), "error");
    retry.hidden = false;
    return;
  }
  if (!response?.ok) {
    setConnection("off", "Offline");
    setStatus(response?.error ?? "SwiftDM is not running on 127.0.0.1:10007.", "error");
    retry.hidden = false;
    return;
  }
  setConnection("ok", "Connected");
  setStatus(`Connected to SwiftDM ${response.version}.`, "ok");
  appVersion.textContent = `v${response.version}`;
  appVersion.title = "SwiftDM app version";
  download.disabled = false;
}

// Without a running app there is no app version to show; fall back to the
// extension version so the header never shows a stale number.
appVersion.textContent = `v${extensionVersion()}`;
appVersion.title = "SwiftDM extension version";

void checkConnection();
retry.addEventListener("click", () => void checkConnection());

// Cookies are how private or signed-in downloads (for example a Google Drive
// share) carry the browser session. Only a user gesture may ask for the
// optional permission, and the popup is the place that has one; the background
// context can only read cookies after a grant.
let pageUrl = "";

void (async () => {
  const [tab] = await swiftdmApi.tabs.query({ active: true, currentWindow: true });
  pageUrl = tab?.url ?? "";
  setSwitch(cookies, await swiftdmCookies.granted());
})();

cookies.addEventListener("click", async () => {
  const wanted = !switchState(cookies);
  if (wanted) {
    const granted = await swiftdmCookies.request(pageUrl);
    setSwitch(cookies, granted);
    setStatus(granted
      ? "Browser cookies will travel with new downloads."
      : "Cookie permission was not granted.", granted ? "ok" : "error");
    return;
  }
  await swiftdmCookies.drop();
  setSwitch(cookies, false);
  setStatus("Browser cookies are no longer sent.");
});

download.addEventListener("click", async () => {
  download.disabled = true;
  setStatus("Sending download…");
  try {
    const [tab] = await swiftdmApi.tabs.query({ active: true, currentWindow: true });
    const response = await swiftdmApi.runtime.sendMessage({
      type: "download-current-tab",
      url: tab?.url ?? "",
      title: tab?.title ?? "",
      cookiesGranted: switchState(cookies)
    });
    if (!response?.ok) throw new Error(response?.error ?? "Request failed.");
    setStatus("Download added to SwiftDM.", "ok");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), "error");
  } finally {
    download.disabled = false;
  }
});

// ---------------------------------------------------------------------------
// Detected media (HLS playlists and adaptive video/audio pairs)
// ---------------------------------------------------------------------------

void (async () => {
  const [tab] = await swiftdmApi.tabs.query({ active: true, currentWindow: true });
  if (typeof tab?.id !== "number") return;
  try {
    const response = await swiftdmApi.runtime.sendMessage({
      type: "get-media-streams",
      tabId: tab.id
    });
    if (!response?.ok) return;
    renderMedia(response.media ?? { hls: [], pairs: [] });
  } catch {
    // The background context may be restarting; the popup simply lists nothing.
  }
})();

function shortName(url) {
  try {
    const parsed = new URL(url);
    const name = parsed.pathname.split("/").filter(Boolean).pop() ?? parsed.hostname;
    return name.length > 32 ? `${name.slice(0, 29)}…` : name;
  } catch {
    return url;
  }
}

function renderMedia(detected) {
  const items = [];
  for (const url of new Set(detected.hls ?? [])) {
    items.push({ label: `HLS playlist · ${shortName(url)}`, urls: [url] });
  }
  for (const pair of detected.pairs ?? []) {
    items.push({
      label: `Video + audio · ${shortName(pair.video)}`,
      urls: [pair.video, pair.audio],
      group: pair.group
    });
  }

  mediaList.textContent = "";
  media.hidden = items.length === 0;
  for (const item of items) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "row";
    const text = document.createElement("span");
    text.className = "txt";
    const title = document.createElement("b");
    title.textContent = item.label;
    const hint = document.createElement("small");
    hint.textContent = item.urls.length > 1
      ? "Sent to SwiftDM as a pair"
      : "Sent to SwiftDM as one file";
    text.appendChild(title);
    text.appendChild(hint);
    button.appendChild(text);
    button.addEventListener("click", () => sendDetected(item, button));
    mediaList.appendChild(button);
  }
}

async function sendDetected(item, button) {
  button.disabled = true;
  setStatus(item.urls.length > 1 ? "Sending video and audio…" : "Sending media…");
  try {
    const [tab] = await swiftdmApi.tabs.query({ active: true, currentWindow: true });
    for (const url of item.urls) {
      const response = await swiftdmApi.runtime.sendMessage({
        type: "download-url",
        url,
        pageUrl: tab?.url ?? "",
        title: tab?.title ?? "",
        group: item.group ?? "",
        cookiesGranted: switchState(cookies)
      });
      if (!response?.ok) throw new Error(response?.error ?? "Request failed.");
    }
    setStatus(item.urls.length > 1
      ? "Video and audio added to SwiftDM."
      : "Media added to SwiftDM.", "ok");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), "error");
  } finally {
    button.disabled = false;
  }
}
