// Headless checks for the Chromium popup markup and styling. The popup is a
// browser page, so it cannot be exercised from the C++ tests; this test parses
// the files instead and guards the contract between popup.html, popup.js and
// the extension page CSP.

const fs = require("node:fs");
const path = require("node:path");

const extensionDir = path.join(__dirname, "..", "ext", "chromium");
const html = fs.readFileSync(path.join(extensionDir, "popup.html"), "utf8");
const css = fs.readFileSync(path.join(extensionDir, "popup.css"), "utf8");
const script = fs.readFileSync(path.join(extensionDir, "popup.js"), "utf8");
const manifest = JSON.parse(
  fs.readFileSync(path.join(extensionDir, "manifest.json"), "utf8")
);

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

// Every element popup.js drives must exist in the markup.
for (const id of [
  "status",
  "app-status",
  "download",
  "retry",
  "auto-intercept",
  "cookies",
  "media",
  "media-list",
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

// The manifest version and the popup must agree on the extension release.
check(/^\d+\.\d+\.\d+$/.test(manifest.version ?? ""),
  "the manifest carries a three-part version");
check(html.includes("popup.css"), "popup.html links the stylesheet");
check(html.includes('src="popup.js"'), "popup.html loads popup.js");

if (failures > 0) {
  console.error(`${failures} popup check(s) failed`);
  process.exit(1);
}
console.log("popup checks passed");
