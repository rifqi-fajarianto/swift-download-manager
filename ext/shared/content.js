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
  let shadow = null;
  let button = null;
  let menu = null;
  let currentTarget = null;
  let lastScanAt = 0;
  let scanTimer = null;
  let hideTimer = null;
  let menuOpen = false;
  let busy = false;

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
    shadow = root;
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
    menu = document.createElement("div");
    menu.className = "swiftdm-media-menu";
    menu.setAttribute("role", "menu");
    menu.hidden = true;
    // The overlay must not vanish while the pointer is on it: the page's
    // pointerout handler only knows about the media element, and it fires
    // before the menu receives its own pointerover. Events from inside the
    // closed shadow root reach the host through the composed path.
    host.addEventListener("pointerover", () => cancelHide());
    host.addEventListener("pointerout", (event) => {
      const path = event.composedPath?.() ?? [];
      if (path.includes(host)) return;
      scheduleHide();
    });
    root.appendChild(style);
    root.appendChild(button);
    root.appendChild(menu);
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
      ".swiftdm-media-label { white-space: nowrap; }",
      ".swiftdm-media-menu { position: absolute; top: calc(100% + 6px); left: 0;",
      "  display: flex; flex-direction: column; width: 300px; max-height: 280px;",
      "  overflow-y: auto; padding: 4px; border: 1px solid rgba(255,255,255,.18);",
      "  border-radius: 10px; background: rgba(18,20,24,.97); color: #f2f4f8;",
      "  font: 500 12px/1.35 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;",
      "  pointer-events: auto; box-shadow: 0 8px 26px rgba(0,0,0,.45); }",
      ".swiftdm-media-menu[hidden] { display: none; }",
      ".swiftdm-media-menu.above { top: auto; bottom: calc(100% + 6px); }",
      ".swiftdm-media-item { display: block; width: 100%; margin: 0; padding: 7px 8px;",
      "  border: 0; border-radius: 7px; background: transparent; color: inherit;",
      "  font: inherit; text-align: left; cursor: pointer; }",
      ".swiftdm-media-item:hover { background: rgba(255,255,255,.09); }",
      ".swiftdm-media-item:focus-visible { outline: 2px solid #6ea8fe; outline-offset: -2px; }",
      ".swiftdm-media-item b { display: block; font-weight: 600; overflow-wrap: anywhere; }",
      ".swiftdm-media-item small { display: block; color: #9aa0a6; font-size: 11px;",
      "  overflow-wrap: anywhere; }",
      ".swiftdm-media-item[disabled] { cursor: default; opacity: .55; }",
      ".swiftdm-media-item[disabled]:hover { background: transparent; }",
      ".swiftdm-media-note { margin: 0; padding: 7px 8px; color: #9aa0a6; font-size: 11px; }",
      ".swiftdm-media-note.error { color: #ff9a9a; }"
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
    if (menuOpen) closeMenu();
    if (host) host.style.display = "none";
    currentTarget = null;
  }

  function cancelHide() {
    if (hideTimer) {
      clearTimeout(hideTimer);
      hideTimer = null;
    }
  }

  function scheduleHide() {
    if (menuOpen || busy) return;
    cancelHide();
    hideTimer = setTimeout(() => hideOverlay(), 900);
  }

  function setLabel(text) {
    if (!button) return;
    const label = button.querySelector(".swiftdm-media-label");
    if (label) label.textContent = text;
  }

  // Clicking the button opens the chooser: the page may offer a manifest, a
  // paired track, the playing element and a recorded blob: stream, and the user
  // picks which one SwiftDM should download (docs/spec/08-media-download.md
  // §4.1). A failure always says why instead of doing nothing.
  function onOverlayClick(event) {
    event.preventDefault();
    event.stopPropagation();
    if (!currentTarget) return;
    if (menuOpen) {
      closeMenu();
      return;
    }
    if (busy) return;
    busy = true;
    cancelHide();
    setLabel("Scanning…");
    try {
      chrome.runtime.sendMessage({ type: "get-media-streams" }, (response) => {
        busy = false;
        if (chrome.runtime.lastError) {
          setLabel("SwiftDM");
          showMenuError(chrome.runtime.lastError.message ?? "SwiftDM is unreachable.");
          return;
        }
        if (!response?.ok) {
          setLabel("SwiftDM");
          showMenuError(response?.error ?? "SwiftDM is unreachable.");
          return;
        }
        const items = detect.buildMediaItems(response.media ?? {});
        // The element in front of the user is always offered, even when the
        // network saw nothing: its src may be the only address available.
        const own = buildCandidate(currentDescriptor());
        if (own && !items.some((item) => item.urls?.[0] === own.url)) {
          items.unshift({
            label: `This player · ${own.videoWidth ? `${own.videoWidth}×${own.videoHeight}` : "media"}`,
            hint: own.blob
              ? "Recorded stream; needs the segments the page fetched"
              : "The element you are watching",
            kind: "file",
            urls: [own.url],
            candidate: own,
            disabled: own.protected === true
          });
        }
        setLabel("SwiftDM");
        renderMenu(items);
      });
    } catch (error) {
      busy = false;
      setLabel("SwiftDM");
      showMenuError(error instanceof Error ? error.message : String(error));
    }
  }

  function showMenuError(text) {
    if (!ensureMenu()) return;
    menu.textContent = "";
    const note = document.createElement("p");
    note.className = "swiftdm-media-note error";
    note.textContent = text;
    menu.appendChild(note);
    openMenu();
  }

  function renderMenu(items) {
    if (!ensureMenu()) return;
    menu.textContent = "";
    if (!items.length) {
      const note = document.createElement("p");
      note.className = "swiftdm-media-note";
      note.textContent = "No downloadable media found on this page.";
      menu.appendChild(note);
      openMenu();
      return;
    }
    for (const item of items) {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "swiftdm-media-item";
      row.setAttribute("role", "menuitem");
      row.disabled = item.disabled === true;
      const title = document.createElement("b");
      title.textContent = item.label;
      const hint = document.createElement("small");
      hint.textContent = item.hint;
      row.appendChild(title);
      row.appendChild(hint);
      if (item.disabled !== true) {
        row.addEventListener("click", (event) => {
          event.preventDefault();
          event.stopPropagation();
          void chooseItem(item, row);
        });
      }
      menu.appendChild(row);
    }
    openMenu();
  }

  // Sends one row to the worker. The worker owns the protocol payload: the
  // content script only names what the user picked.
  function chooseItem(item, row) {
    if (!ensureMenu()) return;
    const hint = row.querySelector("small");
    if (hint) hint.textContent = "Sending…";
    row.disabled = true;
    // A paired row is two independent downloads that share a group token; every
    // other row is a single addMedia request.
    const pair = item.urls && item.urls.length > 1 ? item.urls : null;
    if (pair) {
      sendPair(item, pair, row, hint);
      return;
    }
    const candidate = detect.mediaCandidate(item);
    try {
      chrome.runtime.sendMessage({ type: "download-media", candidate }, (response) => {
        if (chrome.runtime.lastError) {
          if (hint) hint.textContent = chrome.runtime.lastError.message ?? "SwiftDM is unreachable.";
          row.disabled = false;
          return;
        }
        if (response?.ok) {
          closeMenu();
          setLabel("Sent");
          setTimeout(() => setLabel("SwiftDM"), 2500);
          return;
        }
        // A refusal (DRM, no recorded segments, app offline) must be visible;
        // silently re-enabling the row was the old bug.
        if (hint) hint.textContent = response?.error ?? "SwiftDM refused the request.";
        row.disabled = false;
      });
    } catch (error) {
      if (hint) hint.textContent = error instanceof Error ? error.message : String(error);
      row.disabled = false;
    }
  }

  function sendPair(item, urls, row, hint) {
    let index = 0;
    const next = () => {
      if (index >= urls.length) {
        closeMenu();
        setLabel("Sent");
        setTimeout(() => setLabel("SwiftDM"), 2500);
        return;
      }
      const url = urls[index++];
      try {
        chrome.runtime.sendMessage({ type: "download-url", url, group: item.group ?? "" },
          (response) => {
            if (chrome.runtime.lastError) {
              if (hint) hint.textContent =
                chrome.runtime.lastError.message ?? "SwiftDM is unreachable.";
              row.disabled = false;
              return;
            }
            if (!response?.ok) {
              if (hint) hint.textContent = response?.error ?? "SwiftDM refused the request.";
              row.disabled = false;
              return;
            }
            next();
          });
      } catch (error) {
        if (hint) hint.textContent = error instanceof Error ? error.message : String(error);
        row.disabled = false;
      }
    };
    next();
  }

  function ensureMenu() {
    if (!host || !host.isConnected) ensureOverlay();
    return Boolean(menu);
  }

  function openMenu() {
    if (!menu) return;
    cancelHide();
    menu.hidden = false;
    menuOpen = true;
    // Flip above the button when the menu would run past the viewport.
    menu.classList.remove("above");
    const rect = menu.getBoundingClientRect();
    if (rect.bottom > window.innerHeight - 8 && rect.top > rect.height + 16) {
      menu.classList.add("above");
    }
    if (typeof document.addEventListener === "function") {
      document.addEventListener("pointerdown", onDocumentPointerDown, true);
      document.addEventListener("keydown", onDocumentKeyDown, true);
    }
  }

  function closeMenu() {
    if (!menu) return;
    menu.hidden = true;
    menu.textContent = "";
    menuOpen = false;
    if (typeof document.removeEventListener === "function") {
      document.removeEventListener("pointerdown", onDocumentPointerDown, true);
      document.removeEventListener("keydown", onDocumentKeyDown, true);
    }
    scheduleHide();
  }

  function onDocumentPointerDown(event) {
    if (!menuOpen || !host) return;
    if (event.composedPath?.().includes(host)) return;
    closeMenu();
  }

  function onDocumentKeyDown(event) {
    if (event.key === "Escape") {
      closeMenu();
      hideOverlay();
    }
  }

  // Turns the winning descriptor into the payload the worker forwards.
  function buildCandidate(descriptor) {
    if (!descriptor) return null;
    const described = detect.describeCandidate(descriptor);
    if (!described) return null;
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

  // The element under the pointer, described only when it is actually needed:
  // describe() reads computed style and hit-tests the centre, which is far too
  // expensive to run on every pointerover.
  function currentDescriptor() {
    return currentTarget ? scanner.describe(currentTarget) : null;
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
