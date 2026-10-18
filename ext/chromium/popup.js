const status = document.querySelector("#status");
const download = document.querySelector("#download");

chrome.runtime.sendMessage({ type: "get-connection-status" }, (response) => {
  if (chrome.runtime.lastError || !response?.ok) {
    status.textContent = "SwiftDM is not running. Start SwiftDM, then reopen this popup.";
    status.classList.add("error");
    return;
  }
  status.textContent = `Connected to SwiftDM ${response.version}`;
  download.disabled = false;
});

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
  status.classList.remove("error");
  status.textContent = "Sending download…";
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    const response = await chrome.runtime.sendMessage({
      type: "download-current-tab",
      url: tab?.url ?? "",
      title: tab?.title ?? "",
      cookiesGranted: download.dataset.cookiesGranted === "true"
    });
    if (!response?.ok) throw new Error(response?.error ?? "Request failed.");
    status.textContent = "Download added to SwiftDM.";
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
    status.classList.add("error");
  } finally {
    download.disabled = false;
  }
});
