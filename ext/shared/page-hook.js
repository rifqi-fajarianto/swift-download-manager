// In-page hook for the SwiftDM extension.
//
// Runs in the page's own JavaScript world (MAIN world), because the isolated
// content-script world cannot see the page's XMLHttpRequest/fetch calls. It
// records *metadata only* — never request or response bodies — and forwards it
// to the content script with a per-page nonce, so a page cannot forge records.
//
// This is not a download mechanism: it exists so the detector can tell "the
// page's own code fetched this URL" and so MSE streams (blob: players) can be
// put back in order. See docs/spec/08-media-download.md §3.3 and §6.
//
// The file must stay ES5-compatible-ish and dependency free: it is injected as
// a classic script into arbitrary pages.

(function () {
  "use strict";

  // A second injection (SPA re-navigation, extension reload) must not wrap the
  // APIs twice.
  if (window.__swiftdmHookInstalled) return;
  window.__swiftdmHookInstalled = true;

  var MAX_RECORDS = 2000;
  var nonce = "";
  var sequence = 0;
  var records = [];

  // The content script installs the nonce and the sink before the hooks fire.
  document.addEventListener("swiftdm-hook-init", function (event) {
    try {
      var detail = JSON.parse(event.detail || "{}");
      if (detail && typeof detail.nonce === "string") nonce = detail.nonce;
    } catch (error) {
      // A malformed init leaves the hook inert rather than throwing on every
      // page request.
    }
  });

  function post(record) {
    if (!nonce) return;
    records.push(record);
    if (records.length > MAX_RECORDS) records.shift();
    try {
      document.dispatchEvent(new CustomEvent("swiftdm-hook-record", {
        detail: JSON.stringify({ nonce: nonce, record: record })
      }));
    } catch (error) {
      // The content script may be gone (tab closing); records are a hint only.
    }
  }

  function absolute(url) {
    try {
      return new URL(String(url), location.href).href;
    } catch (error) {
      return String(url || "");
    }
  }

  function headerValue(headers, name) {
    if (!headers) return "";
    var wanted = name.toLowerCase();
    try {
      if (typeof headers.get === "function") return String(headers.get(name) || "");
      if (Array.isArray(headers)) {
        for (var i = 0; i < headers.length; i++) {
          var entry = headers[i];
          if (entry && String(entry[0] || "").toLowerCase() === wanted) {
            return String(entry[1] || "");
          }
        }
      } else if (typeof headers === "object") {
        for (var key in headers) {
          if (Object.prototype.hasOwnProperty.call(headers, key) &&
              key.toLowerCase() === wanted) {
            return String(headers[key] || "");
          }
        }
      }
    } catch (error) {
      return "";
    }
    return "";
  }

  // ---------------------------------------------------------------------
  // XMLHttpRequest
  // ---------------------------------------------------------------------
  var xhrOpen = XMLHttpRequest.prototype.open;
  var xhrSend = XMLHttpRequest.prototype.send;
  var xhrSetHeader = XMLHttpRequest.prototype.setRequestHeader;
  var xhrState = new WeakMap();

  XMLHttpRequest.prototype.open = function (method, url) {
    xhrState.set(this, {
      method: String(method || "GET").toUpperCase(),
      url: absolute(url),
      contentType: "",
      requestType: "",
      order: ++sequence
    });
    return xhrOpen.apply(this, arguments);
  };

  XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
    var state = xhrState.get(this);
    if (state) {
      var lower = String(name || "").toLowerCase();
      if (lower === "content-type") state.requestType = String(value || "");
      if (lower === "accept") state.accept = String(value || "");
    }
    return xhrSetHeader.apply(this, arguments);
  };

  XMLHttpRequest.prototype.send = function () {
    var state = xhrState.get(this);
    if (!state) return xhrSend.apply(this, arguments);
    // The body is deliberately not read: metadata only.
    this.addEventListener("loadend", function () {
      var contentType = "";
      var contentLength = 0;
      try {
        contentType = this.getResponseHeader("content-type") || state.requestType || "";
        contentLength = Number(this.getResponseHeader("content-length") || 0) || 0;
      } catch (error) {
        // Cross-origin or aborted request: record what we know.
      }
      post({
        url: state.url,
        method: state.method,
        order: state.order,
        contentType: String(contentType),
        contentLength: contentLength,
        via: "xhr"
      });
    });
    return xhrSend.apply(this, arguments);
  };

  // ---------------------------------------------------------------------
  // fetch
  // ---------------------------------------------------------------------
  if (typeof window.fetch === "function") {
    var originalFetch = window.fetch;
    window.fetch = function (input, init) {
      var url = "";
      var method = "GET";
      var requestType = "";
      try {
        if (typeof input === "string" || input instanceof URL) {
          url = absolute(input);
        } else if (input && typeof input.url === "string") {
          url = absolute(input.url);
          method = String(input.method || "GET").toUpperCase();
          requestType = headerValue(input.headers, "content-type");
        }
        if (init && init.method) method = String(init.method).toUpperCase();
        if (init && init.headers) {
          requestType = headerValue(init.headers, "content-type") || requestType;
        }
      } catch (error) {
        url = "";
      }
      var order = ++sequence;
      var promise = originalFetch.apply(this, arguments);
      if (!url) return promise;
      return promise.then(function (response) {
        try {
          var clone = response.clone();
          post({
            url: url,
            method: method,
            order: order,
            contentType: String(clone.headers.get("content-type") || requestType || ""),
            contentLength: Number(clone.headers.get("content-length") || 0) || 0,
            via: "fetch"
          });
        } catch (error) {
          // A response that cannot be cloned (already consumed) is skipped.
        }
        return response;
      });
    };
  }

  // ---------------------------------------------------------------------
  // MediaSource: proves the player is an MSE player even before segments
  // are observed, which lets the popup explain why there is no direct URL.
  // ---------------------------------------------------------------------
  if (typeof window.MediaSource === "function") {
    var addSourceBuffer = MediaSource.prototype.addSourceBuffer;
    MediaSource.prototype.addSourceBuffer = function (mime) {
      try {
        post({
          url: location.href,
          method: "MSE",
          order: ++sequence,
          contentType: String(mime || ""),
          contentLength: 0,
          via: "mse"
        });
      } catch (error) {
        // ignored: the marker is a hint
      }
      return addSourceBuffer.apply(this, arguments);
    };
  }
})();
