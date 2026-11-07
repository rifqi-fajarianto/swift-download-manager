// Firefox Manifest V3 bootstrap.
//
// Firefox does not run extension service workers, so the background context is
// an event page assembled from `background.scripts` in manifest.json. Those
// scripts share one global scope and load in order, which is why this file has
// no importScripts: api.js, cookie-permission.js, action-icon.js and core.js are
// listed directly in the manifest instead.
//
// The shared files live in ../shared/; tools/extension_pack.mjs copies them next
// to this file when it assembles the directory that is loaded or packaged:
//
//   node tools/extension_pack.mjs build --browser firefox --out build/extensions/firefox
//
// Nothing here needs to run at load time; this file only documents the layout
// and keeps the directory non-empty for the checked-in manifest.
