// Optional cookie-permission helpers shared by the service worker (which can
// read cookies after a grant but cannot prompt) and the popup (whose click is
// a user gesture, so it is the one place that can ask). `cookies` is a single
// optional API permission and HTTP(S) host access is already declared in the
// manifest, so one grant covers the sites the extension serves. Cookies
// themselves stay structured and site-scoped - see docs/spec/04-protocols.md
// section 3.
const swiftdmCookies = (() => {
  const apiPermission = { permissions: ["cookies"] };

  // The origin to ask about when the grant comes from a page.
  function siteOrigin(rawUrl) {
    try {
      const url = new URL(rawUrl);
      if (url.protocol !== "http:" && url.protocol !== "https:") return "";
      return `${url.protocol}//${url.hostname}/*`;
    } catch {
      return "";
    }
  }

  async function granted() {
    try {
      return await chrome.permissions.contains(apiPermission);
    } catch {
      return false;
    }
  }

  // Only call from a user gesture (the context-menu action or the popup).
  async function request(rawUrl) {
    const origin = siteOrigin(rawUrl);
    const wanted = origin
      ? { permissions: ["cookies"], origins: [origin] }
      : apiPermission;
    try {
      return (await chrome.permissions.request(wanted)) === true;
    } catch {
      return false;
    }
  }

  async function drop() {
    try {
      return (await chrome.permissions.remove(apiPermission)) === true;
    } catch {
      return false;
    }
  }

  return { siteOrigin, granted, request, drop };
})();
