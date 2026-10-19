const status = document.querySelector("#status");
const appStatus = document.querySelector("#app-status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");
const cookies = document.querySelector("#cookies");
const media = document.querySelector("#media");
const mediaList = document.querySelector("#media-list");

let lastAppStatus = null;

chrome.storage.local.get(["autoIntercept", "appStatus"], (values) => {
  autoIntercept.checked = values.autoIntercept !== false;
  renderAppStatus(values.appStatus ?? null);
});

autoIntercept.addEventListener("change", () => {
  chrome.storage.local.set({ autoIntercept: autoIntercept.checked });
  renderAppStatus(null);
});

chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== "local") return;
  if (changes.appStatus) renderAppStatus(changes.appStatus.newValue ?? null);
  if (changes.autoIntercept) {
    autoIntercept.checked = changes.autoIntercept.newValue !== false;
    renderAppStatus(null);
  }
});

function renderAppStatus(value) {
  if (value !== null) lastAppStatus = value;
  if (!autoIntercept.checked) {
    appStatus.hidden = false;
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
  appStatus.textContent = lastAppStatus.waiting
    ? "SwiftDM is idle."
    : `SwiftDM is downloading ${active} file${active === 1 ? "" : "s"}.`;
}

function setStatus(text, isError = false) {
  status.textContent = text;
  status.classList.toggle("error", isError);
}

function checkConnection() {
  download.disabled = true;
  retry.hidden = true;
  setStatus("Checking connection…");
  chrome.runtime.sendMessage({ type: "get-connection-status" }, (response) => {
    if (chrome.runtime.lastError) {
      setStatus(chrome.runtime.lastError.message, true);
      retry.hidden = false;
      return;
    }
    if (!response?.ok) {
      setStatus(response?.error ?? "SwiftDM is not running on 127.0.0.1:10007.", true);
      retry.hidden = false;
      return;
    }
    setStatus(`Connected to SwiftDM ${response.version}`);
    download.disabled = false;
  });
}

checkConnection();
retry.addEventListener("click", checkConnection);

// Cookies are how private or signed-in downloads (for example a Google Drive
// share) carry the browser session. Only a user gesture may ask for the
// optional permission, and the popup is the place that has one; the service
// worker can only read cookies after a grant.
let pageUrl = "";

chrome.tabs.query({ active: true, currentWindow: true }, async ([tab]) => {
  pageUrl = tab?.url ?? "";
  cookies.checked = await swiftdmCookies.granted();
});

cookies.addEventListener("change", async () => {
  if (cookies.checked) {
    const granted = await swiftdmCookies.request(pageUrl);
    cookies.checked = granted;
    setStatus(granted
      ? "Browser cookies will travel with new downloads."
      : "Cookie permission was not granted.", !granted);
    return;
  }
  await swiftdmCookies.drop();
  setStatus("Browser cookies are no longer sent.");
});

download.addEventListener("click", async () => {
  download.disabled = true;
  setStatus("Sending download…");
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    const response = await chrome.runtime.sendMessage({
      type: "download-current-tab",
      url: tab?.url ?? "",
      title: tab?.title ?? "",
      cookiesGranted: cookies.checked
    });
    if (!response?.ok) throw new Error(response?.error ?? "Request failed.");
    setStatus("Download added to SwiftDM.");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), true);
  } finally {
    download.disabled = false;
  }
});

// ---------------------------------------------------------------------------
// Detected media (HLS playlists and adaptive video/audio pairs)
// ---------------------------------------------------------------------------

chrome.tabs.query({ active: true, currentWindow: true }, ([tab]) => {
  if (typeof tab?.id !== "number") return;
  chrome.runtime.sendMessage({ type: "get-media-streams", tabId: tab.id }, (response) => {
    if (chrome.runtime.lastError || !response?.ok) return;
    renderMedia(response.media ?? { hls: [], pairs: [] });
  });
});

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
    button.textContent = item.label;
    button.addEventListener("click", () => sendDetected(item, button));
    mediaList.appendChild(button);
  }
}

async function sendDetected(item, button) {
  button.disabled = true;
  setStatus(item.urls.length > 1 ? "Sending video and audio…" : "Sending media…");
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    for (const url of item.urls) {
      const response = await chrome.runtime.sendMessage({
        type: "download-url",
        url,
        pageUrl: tab?.url ?? "",
        title: tab?.title ?? "",
        group: item.group ?? "",
        cookiesGranted: cookies.checked
      });
      if (!response?.ok) throw new Error(response?.error ?? "Request failed.");
    }
    setStatus(item.urls.length > 1
      ? "Video and audio added to SwiftDM."
      : "Media added to SwiftDM.");
  } catch (error) {
    setStatus(error instanceof Error ? error.message : String(error), true);
  } finally {
    button.disabled = false;
  }
}
