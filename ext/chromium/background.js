// Chromium Manifest V3 service worker bootstrap.
//
// The extension logic lives in ../shared/ and is shared with the Firefox build
// (see ext/firefox/). A service worker cannot load files from a parent
// directory, so the release helper copies the shared files next to this one;
// tools/extension_pack.mjs assembles the directory that is actually loaded.
// The checked-in copy here is what `Load unpacked` points at, so the shared
// files must exist in this folder - the pack helper keeps them in sync and
// `node tools/extension_pack.mjs build --browser chromium` regenerates them.
//
// media-detect.js and media-mse.js are loaded here too: the worker reuses their
// pure logic (segment classification, ranking helpers) and the content script
// loads the same files as its own content scripts.

importScripts(
  "api.js",
  "cookie-permission.js",
  "action-icon.js",
  "media-detect.js",
  "media-mse.js",
  "core.js"
);
