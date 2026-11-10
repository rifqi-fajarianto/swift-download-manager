// Headless checks for the popup markup and styling of both extensions. The
// popup is a browser page, so it cannot be exercised from the C++ tests; this
// test parses the files instead and guards the contract between popup.html,
// popup.js, api.js and the extension page CSP.
//
// Both extensions share one popup: ext/shared/ is the source and the browser
// directories are assembled copies, so the same assertions run against the
// Chromium and the Firefox directory.

const fs = require("node:fs");
const path = require("node:path");

const root = path.join(__dirname, "..");
const sharedDir = path.join(root, "ext", "shared");
const extensionDirs = {
  chromium: path.join(root, "ext", "chromium"),
  firefox: path.join(root, "ext", "firefox"),
};

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

const html = fs.readFileSync(path.join(sharedDir, "popup.html"), "utf8");
const css = fs.readFileSync(path.join(sharedDir, "popup.css"), "utf8");
const script = fs.readFileSync(path.join(sharedDir, "popup.js"), "utf8");

// Every element popup.js drives must exist in the markup.
for (const id of [
  "status",
  "app-status",
  "download",
  "retry",
  "auto-intercept",
  "cookies",
  "media",
  "media-hint",
  "app-version",
  "connection",
]) {
  check(html.includes(`id="${id}"`), `popup.html defines #${id}`);
  check(script.includes(`#${id}`), `popup.js uses #${id}`);
}

// The switches are buttons with role="switch"; their state is aria-checked.
check(/id="auto-intercept"[^>]*role="switch"/.test(html),
  "the auto-intercept control is a switch");
check(/id="cookies"[^>]*role="switch"/.test(html),
  "the cookies control is a switch");
check(script.includes('setAttribute("aria-checked"'),
  "popup.js keeps the switch state in aria-checked");

// The extension page CSP is script-src 'self': no inline script and no inline
// event handlers are allowed in the popup.
check(!/<script(?![^>]*\bsrc=)[^>]*>[\s\S]*?\S[\s\S]*?<\/script>/.test(html),
  "popup.html has no inline script");
check(!/\son[a-z]+\s*=/i.test(html),
  "popup.html has no inline event handler attributes");

// api.js must load before the files that use swiftdmApi.
const apiIndex = html.indexOf('src="api.js"');
check(apiIndex > 0, "popup.html loads api.js");
for (const file of ["cookie-permission.js", "action-icon.js", "media-detect.js", "popup.js"]) {
  const index = html.indexOf(`src="${file}"`);
  check(index > apiIndex, `popup.html loads ${file} after api.js`);
}
// media-detect.js defines the helpers popup.js calls, so it must come first.
check(html.indexOf('src="media-detect.js"') < html.indexOf('src="popup.js"'),
  "popup.html loads media-detect.js before popup.js");

// The design tokens and building blocks of the UI template are present.
for (const token of [
  "--bg",
  "--card",
  "--line",
  "--text",
  "--muted",
  "--accent",
  "--ok",
]) {
  check(css.includes(token), `popup.css defines ${token}`);
}
for (const selector of [".pill", ".sw", ".cta", ".group", ".row", ".foot"]) {
  check(css.includes(selector), `popup.css defines ${selector}`);
}
check(css.includes("prefers-color-scheme"),
  "popup.css follows the light and dark color schemes");

check(html.includes("popup.css"), "popup.html links the stylesheet");

// The popup must not call a browser namespace directly: api.js is the only
// place that knows about chrome vs. browser.
check(!/\bchrome\s*\./.test(script), "popup.js goes through swiftdmApi, not chrome.*");
check(!/\bbrowser\.(runtime|storage|tabs|action|menus|permissions|cookies|downloads)/.test(script),
  "popup.js goes through swiftdmApi, not browser.*");
check(script.includes("swiftdmApi."), "popup.js uses the shared API layer");

// The stream chooser lives in the overlay over the player, not in the popup
// (docs/spec/08-media-download.md §4.1). The popup only points at it.
const content = fs.readFileSync(path.join(sharedDir, "content.js"), "utf8");
const detectScript = fs.readFileSync(path.join(sharedDir, "media-detect.js"), "utf8");
for (const kind of ["hls", "dash", "pairs", "dom", "mse"]) {
  check(detectScript.includes(`detected.${kind}`),
    `media-detect.js reads detected.${kind}`);
}
check(detectScript.includes("buildMediaItems"),
  "media-detect.js builds the shared stream rows");
check(detectScript.includes("mediaCandidate"),
  "media-detect.js builds the shared addMedia candidate");
check(content.includes("buildMediaItems"),
  "content.js renders the stream rows in the overlay menu");
check(content.includes("download-media"),
  "content.js hands the picked stream off with download-media");
check(content.includes("get-media-streams"),
  "content.js asks the worker for the media list");
check(content.includes("response?.error"),
  "content.js shows why a handoff was refused");
check(!content.includes("media-list"),
  "the overlay menu is not the popup list");
check(script.includes("get-media-streams"),
  "popup.js still asks the worker whether the page has streams");
check(script.includes("buildMediaItems"),
  "popup.js counts streams with the shared row builder");
check(!script.includes("download-media"),
  "popup.js no longer hands streams off (the overlay does)");

// Both manifests must describe a Manifest V3 extension with the same version,
// the same popup and the icons the popup references.
const manifests = {};
for (const [name, dir] of Object.entries(extensionDirs)) {
  const file = path.join(dir, "manifest.json");
  check(fs.existsSync(file), `${name} manifest exists`);
  if (!fs.existsSync(file)) continue;
  const manifest = JSON.parse(fs.readFileSync(file, "utf8"));
  manifests[name] = manifest;
  check(manifest.manifest_version === 3, `${name} manifest is Manifest V3`);
  check(/^\d+\.\d+\.\d+$/.test(manifest.version ?? ""),
    `${name} manifest carries a three-part version`);
  check(manifest.action?.default_popup === "popup.html",
    `${name} manifest opens popup.html`);
  check(manifest.action?.default_icon?.["16"] === "icons/icon16.png",
    `${name} manifest has a toolbar icon`);
  check(manifest.permissions?.includes("downloads"),
    `${name} manifest requests downloads`);
  check(manifest.permissions?.includes("storage"),
    `${name} manifest requests storage`);
  check(manifest.optional_permissions?.includes("cookies"),
    `${name} keeps the cookie permission optional`);
  check(manifest.content_security_policy?.extension_pages?.includes("ws://127.0.0.1:10007"),
    `${name} CSP allows the local WebSocket`);

  // The popup assets live in ext/shared/ (the assembled copies are generated),
  // so the source is what has to exist; the manifest and the bootstrap are the
  // browser directory's own files.
  for (const asset of ["popup.html", "popup.css", "popup.js", "api.js",
                       "cookie-permission.js", "action-icon.js",
                       "media-detect.js", "media-mse.js", "content.js",
                       "content.css", "page-hook.js"]) {
    check(fs.existsSync(path.join(sharedDir, asset)), `ext/shared ships ${asset}`);
  }
  for (const size of ["16", "32", "48", "128"]) {
    const icon = `icons/icon${size}.png`;
    check(fs.existsSync(path.join(sharedDir, icon)), `ext/shared ships ${icon}`);
  }
  check(fs.existsSync(path.join(dir, "background.js")),
    `${name} directory ships its background bootstrap`);

  // Media detection runs as a content script in every frame; the MAIN-world
  // hook has to be web accessible so the page can load it.
  const contentScripts = manifest.content_scripts ?? [];
  const mediaScripts = contentScripts.find((entry) =>
    (entry.js ?? []).includes("content.js"));
  check(Boolean(mediaScripts), `${name} manifest injects content.js`);
  if (mediaScripts) {
    check(mediaScripts.all_frames === true, `${name} content script runs in all frames`);
    check(mediaScripts.run_at === "document_start",
      `${name} content script runs at document_start`);
    check((mediaScripts.js ?? []).indexOf("media-detect.js") >= 0 &&
          (mediaScripts.js ?? []).indexOf("media-mse.js") >= 0,
      `${name} content script loads the media helpers first`);
    check((mediaScripts.js ?? []).indexOf("media-detect.js") <
          (mediaScripts.js ?? []).indexOf("content.js"),
      `${name} media helpers load before content.js`);
    check((mediaScripts.css ?? []).includes("content.css"),
      `${name} content script ships its stylesheet`);
  }
  const resources = (manifest.web_accessible_resources ?? [])
    .flatMap((entry) => entry.resources ?? []);
  check(resources.includes("page-hook.js"),
    `${name} exposes page-hook.js to the page world`);
  check(manifest.permissions?.includes("scripting"),
    `${name} manifest requests scripting`);
}

if (manifests.chromium && manifests.firefox) {
  check(manifests.chromium.version === manifests.firefox.version,
    "both extensions carry the same version");
  check(typeof manifests.chromium.background?.service_worker === "string",
    "the Chromium manifest uses a service worker");
  check(Array.isArray(manifests.firefox.background?.scripts),
    "the Firefox manifest uses background scripts");
  check(!manifests.firefox.background?.service_worker,
    "the Firefox manifest does not ask for a service worker");
  check(typeof manifests.firefox.browser_specific_settings?.gecko?.id === "string",
    "the Firefox manifest declares a gecko add-on id");
  check(!manifests.firefox.key,
    "the Firefox manifest does not carry a Chromium key");
  check(typeof manifests.chromium.key === "string",
    "the Chromium manifest keeps its pinned key");
}

// The Firefox background scripts must be exactly the shared files plus the
// bootstrap, in the order the event page needs them.
if (manifests.firefox) {
  const scripts = manifests.firefox.background?.scripts ?? [];
  check(scripts[0] === "api.js", "the Firefox event page loads api.js first");
  check(scripts.at(-1) === "core.js", "the Firefox event page loads core.js last");
  check(scripts.includes("cookie-permission.js") && scripts.includes("action-icon.js"),
    "the Firefox event page loads the shared helpers");
}

if (failures > 0) {
  console.error(`${failures} popup check(s) failed`);
  process.exit(1);
}
console.log("popup checks passed");
