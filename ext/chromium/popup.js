const status = document.querySelector("#status");
const appStatus = document.querySelector("#app-status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");
const bypass = document.querySelector("#bypass");
const media = document.querySelector("#media");
const mediaList = document.querySelector("#media-list");

let lastAppStatus = null;

chrome.storage.local.get(["autoIntercept", "bypassDownloads", "appStatus"], (values) => {
  autoIntercept.checked = values.autoIntercept !== false;
  bypass.checked = values.bypassDownloads === true;
  renderAppStatus(values.appStatus ?? null);
});

autoIntercept.addEventListener("change", () => {
  chrome.storage.local.set({ autoIntercept: autoIntercept.checked });
});

bypass.addEventListener("change", () => {
  chrome.storage.local.set({ bypassDownloads: bypass.checked });
  renderAppStatus(null);
});

chrome.storage.onChanged.addListener((changes, area) => {
  if (area !== "local") return;
  if (changes.appStatus) renderAppStatus(changes.appStatus.newValue ?? null);
  if (changes.bypassDownloads) {
    bypass.checked = changes.bypassDownloads.newValue === true;
    renderAppStatus(null);
  }
});

function renderAppStatus(value) {
  if (value !== null) lastAppStatus = value;
  if (bypass.checked) {
    appStatus.hidden = false;
    appStatus.textContent = "Bypass is on: new downloads stay in the browser.";
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

chrome.permissions.contains({ permissions: ["cookies"] }, (granted) => {
  download.dataset.cookiesGranted = granted ? "true" : "false";
});

chrome.permissions.onAdded.addListener((permissions) => {
  if (permissions.permissions?.includes("cookies")) {
    download.dataset.cookiesGranted = "true";
  }
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
      cookiesGranted: download.dataset.cookiesGranted === "true"
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
        cookiesGranted: download.dataset.cookiesGranted === "true"
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
