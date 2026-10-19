const status = document.querySelector("#status");
const appStatus = document.querySelector("#app-status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");
const bypass = document.querySelector("#bypass");

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
