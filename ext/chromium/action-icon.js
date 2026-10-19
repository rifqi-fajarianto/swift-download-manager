// Toolbar icon switching, shared by the popup (which flips the switch) and the
// service worker (which restores the icon when it wakes). The active icon is
// the colored SwiftDM mark; the deactivated one is the grayscale variant used
// while "Automatically use SwiftDM for downloads" is off. Chromium cannot load
// SVGs for the toolbar, so these are PNGs rendered from the same sources in
// ui-assets/app-icon/.
const swiftdmActionIcon = (() => {
  const active = {
    16: "icons/icon16.png",
    32: "icons/icon32.png",
    48: "icons/icon48.png",
    128: "icons/icon128.png",
  };
  const off = {
    16: "icons/icon-off16.png",
    32: "icons/icon-off32.png",
    48: "icons/icon-off48.png",
    128: "icons/icon-off128.png",
  };

  function apply(enabled) {
    try {
      const result = chrome.action.setIcon({ path: enabled ? active : off });
      // Older Chromium returns undefined; newer builds return a promise that can
      // reject when the worker is shutting down. Neither is actionable here.
      if (result?.catch) result.catch(() => {});
    } catch {
      // The icon is cosmetic; never fail a handoff because of it.
    }
  }

  return { apply };
})();
