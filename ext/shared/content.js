// Content script for the SwiftDM extension.
//
// Two jobs:
//   1. Find the media element that is actually playing and show a small
//      "Download with SwiftDM" button over it (docs/spec/08-media-download.md
//      §4.1). The button is the only page UI the extension adds.
//   2. Bridge the MAIN-world hook (page-hook.js) to the background worker, so
//      the detector learns which URLs the page itself fetched and can rebuild
//      MSE (blob:) streams.
//
// Runs in every frame, at document_start. Nothing is scanned until the
// background asks for it or the user hovers a media element, so an idle tab
// costs nothing.

(() => {
  if (window.__swiftdmContentInstalled) return;
  window.__swiftdmContentInstalled = true;

  const detect = globalThis.swiftdmMediaDetect;
  const mse = globalThis.swiftdmMediaMse;
  if (!detect || !mse) return;  // the manifest loads the helpers first

  const HOST_ID = "swiftdm-media-overlay";
  const NONCE = createNonce();
  const scanner = new detect.MediaScanner(document, {
    knownUrls: new Set(),
    mouseX: -1,
    mouseY: -1
  });

  let overlayEnabled = true;
  let host = null;
  let button = null;
  let currentTarget = null;
  let lastScanAt = 0;
  let scanTimer = null;
  let hideTimer = null;

  function createNonce() {
    const bytes = new Uint8Array(16);
    try {
      crypto.getRandomValues(bytes);
    } catch {
      for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
    }
    return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
  }

  // -------------------------------------------------------------------------
  // Overlay
  // -------------------------------------------------------------------------

  function ensureOverlay() {
    if (host && host.isConnected) return;
    host = document.createElement("div");
    host.id = HOST_ID;
    host.className = "swiftdm-media-host";
    host.style.display = "none";
    // Shadow DOM keeps the button out of the page's CSS and out of its
    // querySelectorAll results.
    const root = host.attachShadow ? host.attachShadow({ mode: "closed" }) : host;
    const style = document.createElement("style");
    style.textContent = overlayStyles();
    button = document.createElement("button");
    button.type = "button";
    button.className = "swiftdm-media-button";
    button.innerHTML =
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4"' +
      ' stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      '<path d="M12 4v11"/><path d="m7 11 5 5 5-5"/><path d="M5 20h14"/></svg>' +
      '<span class="swiftdm-media-label">SwiftDM</span>';
    button.addEventListener("click", onOverlayClick);
    root.appendChild(style);
    root.appendChild(button);
    (document.body ?? document.documentElement).appendChild(host);
  }

  // A stylesheet inside the shadow root: the page CSP cannot block it and the
  // manifest's content.css stays available for the non-shadow fallback.
  function overlayStyles() {
    return [
      ":host { all: initial; }",
      ".swiftdm-media-button { display: inline-flex; align-items: center; gap: 6px;",
      "  margin: 0; padding: 6px 10px; border: 1px solid rgba(255,255,255,.22);",
      "  border-radius: 8px; background: rgba(18,20,24,.92); color: #f2f4f8;",
      "  font: 600 12px/1.2 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;",
      "  cursor: pointer; pointer-events: auto;",
      "  box-shadow: 0 2px 10px rgba(0,0,0,.35); }",
      ".swiftdm-media-button:hover { background: rgba(38,43,52,.96); }",
      ".swiftdm-media-button:focus-visible { outline: 2px solid #6ea8fe; outline-offset: 2px; }",
      ".swiftdm-media-button svg { width: 14px; height: 14px; flex: 0 0 auto; }",
      ".swiftdm-media-label { white-space: nowrap; }"
    ].join("\n");
  }

  function positionOverlay(element) {
    if (!host || !element) return;
    const rect = element.getBoundingClientRect();
    if (rect.width < detect.MIN_VIDEO_WIDTH || rect.height < detect.MIN_VIDEO_HEIGHT) {
      hideOverlay();
      return;
    }
    // Anchor to the top-left corner of the player, inside its box.
    host.style.left = `${Math.max(0, Math.round(rect.left) + 8)}px`;
    host.style.top = `${Math.max(0, Math.round(rect.top) + 8)}px`;
    host.style.display = "block";
  }

  function hideOverlay() {
    if (host) host.style.display = "none";
    currentTarget = null;
  }

  function scheduleHide() {
    if (hideTimer) clearTimeout(hideTimer);
    hideTimer = setTimeout(() => hideOverlay(), 900);
  }

  function onOverlayClick(event) {
    event.preventDefault();
    event.stopPropagation();
    if (!currentTarget) return;
    const candidate = buildCandidate(currentTarget);
    if (!candidate) return;
    if (button) button.disabled = true;
    try {
      chrome.runtime.sendMessage({ type: "download-media", candidate }, (response) => {
        if (button) button.disabled = false;
        if (chrome.runtime.lastError) return;
        if (response?.ok) {
          if (button) {
            button.querySelector(".swiftdm-media-label").textContent = "Sent";
            setTimeout(() => {
              if (button) button.querySelector(".swiftdm-media-label").textContent = "SwiftDM";
            }, 2500);
          }
        }
      });
    } catch {
      if (button) button.disabled = false;
    }
  }

  // Turns the winning descriptor into the payload the worker forwards.
  function buildCandidate(descriptor) {
    if (!descriptor) return null;
    const described = detect.describeCandidate(descriptor);
    if (!described) return null;
    const element = descriptor.element;
    return {
      url: described.url,
      kind: "file",
      elementTag: described.elementTag,
      durationSec: described.durationSec,
      positionSec: described.positionSec,
      protected: described.protected,
      width: descriptor.width,
      height: descriptor.height,
      videoWidth: descriptor.videoWidth,
      videoHeight: descriptor.videoHeight,
      // blob: URLs are not downloadable; the worker resolves the MSE stream.
      blob: String(described.url).startsWith("blob:")
    };
  }

  // -------------------------------------------------------------------------
  // Scanning
  // -------------------------------------------------------------------------

  function scan(hint = {}) {
    lastScanAt = Date.now();
    const best = scanner.findBest(hint);
    return best ? buildCandidate(best) : null;
  }

  function scanThrottled() {
    if (scanTimer) return;
    const wait = Math.max(0, 1000 - (Date.now() - lastScanAt));
    scanTimer = setTimeout(() => {
      scanTimer = null;
      const best = scanner.findBest({ mouseX: scanner.mouseX, mouseY: scanner.mouseY });
      if (!best) {
        scheduleHide();
        return;
      }
      currentTarget = best.element;
      positionOverlay(best.element);
    }, wait);
  }

  function onPointerOver(event) {
    if (!overlayEnabled) return;
    const target = event.target;
    if (!target || !detect.MEDIA_TAGS.includes(String(target.localName ?? "").toLowerCase())) {
      return;
    }
    scanner.mouseX = event.clientX;
    scanner.mouseY = event.clientY;
    ensureOverlay();
    currentTarget = target;
    positionOverlay(target);
  }

  function onPointerOut(event) {
    if (!host || !currentTarget) return;
    if (event.relatedTarget && currentTarget.contains?.(event.relatedTarget)) return;
    scheduleHide();
  }

  // The overlay must follow the player when the page scrolls or resizes.
  function reposition() {
    if (!currentTarget || !host || host.style.display === "none") return;
    positionOverlay(currentTarget);
  }

  document.addEventListener("pointerover", onPointerOver, { passive: true, capture: true });
  document.addEventListener("pointerout", onPointerOut, { passive: true, capture: true });
  window.addEventListener("scroll", reposition, { passive: true, capture: true });
  window.addEventListener("resize", reposition, { passive: true });

  // A player that appears later (SPA navigation) is picked up without polling:
  // the observer only schedules a throttled scan.
  if (typeof MutationObserver === "function" && document.documentElement) {
    const observer = new MutationObserver(() => {
      if (!overlayEnabled) return;
      if (!document.querySelector("video,audio,object,embed")) return;
      scanThrottled();
    });
    observer.observe(document.documentElement, { childList: true, subtree: true });
  }

  // -------------------------------------------------------------------------
  // MAIN-world hook bridge
  // -------------------------------------------------------------------------

  // The hook is injected by the worker through scripting.executeScript with
  // world:"MAIN". When that is unavailable (older Firefox), the fallback is a
  // <script src> pointing at the web-accessible resource.
  function installHook() {
    const detail = JSON.stringify({ nonce: NONCE });
    document.dispatchEvent(new CustomEvent("swiftdm-hook-init", { detail }));
    if (window.__swiftdmHookInstalled) return;
    try {
      const script = document.createElement("script");
      script.src = chrome.runtime.getURL("page-hook.js");
      script.async = false;
      script.dataset.swiftdm = NONCE;
      script.onload = () => script.remove();
      (document.head ?? document.documentElement).appendChild(script);
    } catch {
      // A page with a strict CSP may block the fallback; the overlay still
      // works and only the MSE ordering hint is lost.
    }
  }

  document.addEventListener("swiftdm-hook-record", (event) => {
    let payload = null;
    try {
      payload = JSON.parse(event.detail || "{}");
    } catch {
      return;
    }
    // Only records carrying this page's nonce are accepted, so a page script
    // cannot inject fake media segments.
    if (!payload || payload.nonce !== NONCE || !payload.record) return;
    try {
      chrome.runtime.sendMessage({ type: "media-segment", record: payload.record }, () => {
        // The worker may be restarting; dropping a hint is harmless.
        void chrome.runtime.lastError;
      });
    } catch {
      // extension context invalidated
    }
  });

  // -------------------------------------------------------------------------
  // Messages from the worker
  // -------------------------------------------------------------------------

  chrome.runtime.onMessage.addListener((message, _sender, sendResponse) => {
    if (message?.type === "scan-media") {
      const candidate = scan({ urls: message.urls ?? [] });
      sendResponse({ ok: true, candidate });
      return false;
    }
    if (message?.type === "media-config") {
      overlayEnabled = message.mediaOverlay !== false;
      if (!overlayEnabled) hideOverlay();
      sendResponse({ ok: true });
      return false;
    }
    if (message?.type === "known-urls") {
      for (const url of message.urls ?? []) scanner.knownUrls.add(url);
      sendResponse({ ok: true });
      return false;
    }
    return false;
  });

  installHook();
})();
