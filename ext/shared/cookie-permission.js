// Optional cookie-permission helpers shared by the background context (which
// can read cookies after a grant but cannot prompt) and the popup (whose click
// is a user gesture, so it is the one place that can ask). `cookies` is a single
// optional API permission and HTTP(S) host access is already declared in the
// manifest, so one grant covers the sites the extension serves. Cookies
// themselves stay structured and site-scoped - see docs/spec/04-protocols.md
// section 3.
//
// The permission calls go through `swiftdmApi` because Chromium's
// `permissions.request` is callback-based while Firefox's returns a promise.

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
      return await swiftdmApi.permissions.contains(apiPermission);
    } catch {
      return false;
    }
  }

  // Only call from a user gesture (the context-menu action or the popup).
  // The site-scoped form gives Chromium a prompt that names the site; Firefox
  // only grants origins that are declared in optional_host_permissions, so a
  // rejected scoped request falls back to the plain API permission (host access
  // for HTTP(S) is already granted by host_permissions there).
  async function request(rawUrl) {
    const origin = siteOrigin(rawUrl);
    if (origin) {
      try {
        if ((await swiftdmApi.permissions.request({
          permissions: ["cookies"], origins: [origin]
        })) === true) return true;
      } catch {
        // fall through to the permission-only request
      }
    }
    try {
      return (await swiftdmApi.permissions.request(apiPermission)) === true;
    } catch {
      return false;
    }
  }

  async function drop() {
    try {
      return (await swiftdmApi.permissions.remove(apiPermission)) === true;
    } catch {
      return false;
    }
  }

  return { siteOrigin, granted, request, drop };
})();
