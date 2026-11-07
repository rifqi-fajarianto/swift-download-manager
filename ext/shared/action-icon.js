// Toolbar icon switching, shared by the popup (which flips the switch) and the
// background context (which restores the icon when it starts). The active icon
// is the colored SwiftDM mark; the deactivated one is the grayscale variant used
// while "Automatically use SwiftDM for downloads" is off. Neither browser can
// load an SVG for the toolbar button, so these are PNGs rendered from the same
// sources in ui-assets/app-icon/.

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

  async function apply(enabled) {
    try {
      await swiftdmApi.action.setIcon({ path: enabled ? active : off });
    } catch {
      // The icon is cosmetic; never fail a handoff because of it. Callers may
      // ignore the returned promise.
    }
  }

  return { apply };
})();
