const status = document.querySelector("#status");
const download = document.querySelector("#download");
const retry = document.querySelector("#retry");
const autoIntercept = document.querySelector("#auto-intercept");

chrome.storage.local.get("autoIntercept", ({ autoIntercept: enabled }) => {
  autoIntercept.checked = enabled !== false;
});

autoIntercept.addEventListener("change", () => {
  chrome.storage.local.set({ autoIntercept: autoIntercept.checked });
});

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
