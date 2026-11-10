const status = document.querySelector("#status");
const appStatus = document.querySelector("#app-status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");
const cookies = document.querySelector("#cookies");
const media = document.querySelector("#media");
const mediaHint = document.querySelector("#media-hint");
const extensionVersionLabel = document.querySelector("#extension-version");
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
  // The only place the app version appears: confirmed by a live connection.
  setStatus(`Connected to SwiftDM ${response.version}.`, "ok");
  download.disabled = false;
}

// The header always shows the extension's own version; the app version only
// appears in the status line once a connection has been confirmed, so the
// popup never shows a number it has not verified.
extensionVersionLabel.textContent = `v${extensionVersion()}`;
extensionVersionLabel.title = "SwiftDM extension version";

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
// Detected media
// ---------------------------------------------------------------------------

// The stream list lives in the overlay button over the player, where the user
// can see what each row refers to (docs/spec/08-media-download.md §4.1). The
// popup only says that something was found and where to pick it, so the two
// surfaces cannot drift apart.
void (async () => {
  const [tab] = await swiftdmApi.tabs.query({ active: true, currentWindow: true });
  if (typeof tab?.id !== "number") return;
  let count = 0;
  try {
    const response = await swiftdmApi.runtime.sendMessage({
      type: "get-media-streams",
      tabId: tab.id
    });
    if (response?.ok) count = swiftdmMediaDetect.buildMediaItems(response.media ?? {}).length;
  } catch {
    // The background context may be restarting; the popup simply says nothing.
  }
  media.hidden = count === 0;
  mediaHint.textContent = count === 0
    ? ""
    : `${count} stream${count === 1 ? "" : "s"} found: use the SwiftDM button over the player to pick one.`;
})();
