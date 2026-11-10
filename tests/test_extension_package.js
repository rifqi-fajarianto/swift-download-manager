// Headless checks for the release ZIPs of both extensions. A ZIP is what users
// extract and load ("Load unpacked" in Chromium, "Load Temporary Add-on" in
// Firefox), so it must keep the files at its root and stay installable as-is;
// this test builds it with the same helper the release workflow uses and
// inspects the archive.
//
// It also guards the shared-source layout: ext/shared/ is the only copy of the
// extension logic, and the browser directories must match it exactly. A stale
// copy would ship code that differs from what the tests exercise.

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const zlib = require("node:zlib");
const { execFileSync } = require("node:child_process");

const root = path.join(__dirname, "..");
const helper = path.join(root, "tools", "extension_pack.mjs");
const sharedDir = path.join(root, "ext", "shared");
const browserDirs = {
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

const workDir = fs.mkdtempSync(path.join(os.tmpdir(), "swiftdm-ext-"));

// The zip command assembles a throwaway directory from ext/shared/, so it never
// depends on the generated copies in the source tree.
function pack(browser) {
  const output = path.join(workDir, `${browser}.zip`);
  execFileSync(process.execPath, [helper, "zip", "--browser", browser, "--out", output], {
    cwd: root,
    stdio: "pipe",
  });
  return fs.readFileSync(output);
}

// `build --out` writes the same assembled directory the release ships.
function assemble(browser) {
  const target = path.join(workDir, browser);
  execFileSync(
    process.execPath,
    [helper, "build", "--browser", browser, "--out", target],
    { cwd: root, stdio: "pipe" }
  );
  return target;
}

function runHelper(...args) {
  return execFileSync(process.execPath, [helper, ...args], { cwd: root, encoding: "utf8" }).trim();
}

// Every command must actually run when the helper is executed directly. A
// broken "is this the entry point?" check makes the process exit successfully
// without doing anything, which is invisible until a release asset is missing.
// That happened on Windows, where `new URL(...).pathname` yields "/D:/...".
for (const args of [["version"], ["id"], ["verify"]]) {
  let output = "";
  try {
    output = runHelper(...args);
  } catch (error) {
    check(false, `node tools/extension_pack.mjs ${args.join(" ")} runs (${error.message.split("\n")[0]})`);
    continue;
  }
  check(output.length > 0, `node tools/extension_pack.mjs ${args.join(" ")} prints a result`);
}

// Minimal central-directory reader: enough to list entries and inflate them
// without adding a zip dependency to the test suite.
function readZip(buffer) {
  const endOffset = buffer.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  check(endOffset >= 0, "the archive has an end-of-central-directory record");
  if (endOffset < 0) return [];

  const count = buffer.readUInt16LE(endOffset + 10);
  let cursor = buffer.readUInt32LE(endOffset + 16);
  const entries = [];

  for (let i = 0; i < count; i += 1) {
    check(buffer.readUInt32LE(cursor) === 0x02014b50, `entry ${i} is a central header`);
    const method = buffer.readUInt16LE(cursor + 10);
    const compressedSize = buffer.readUInt32LE(cursor + 20);
    const nameLength = buffer.readUInt16LE(cursor + 28);
    const extraLength = buffer.readUInt16LE(cursor + 30);
    const commentLength = buffer.readUInt16LE(cursor + 32);
    const localOffset = buffer.readUInt32LE(cursor + 42);
    const name = buffer.toString("utf8", cursor + 46, cursor + 46 + nameLength);

    const localNameLength = buffer.readUInt16LE(localOffset + 26);
    const localExtraLength = buffer.readUInt16LE(localOffset + 28);
    const dataStart = localOffset + 30 + localNameLength + localExtraLength;
    const raw = buffer.subarray(dataStart, dataStart + compressedSize);
    const data = method === 8 ? zlib.inflateRawSync(raw) : raw;

    entries.push({ name, data });
    cursor += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

// The files that must be at the root of both archives.
const required = [
  "manifest.json",
  "popup.html",
  "popup.css",
  "popup.js",
  "api.js",
  "core.js",
  "cookie-permission.js",
  "action-icon.js",
  // Media detection: the helpers are loaded as content scripts, the hook is a
  // web-accessible resource the page world loads.
  "media-detect.js",
  "media-mse.js",
  "content.js",
  "content.css",
  "page-hook.js",
  "icons/icon16.png",
  "icons/icon128.png",
  "icons/icon-off16.png",
];

const archives = {};
for (const browser of Object.keys(browserDirs)) {
  const entries = readZip(pack(browser));
  archives[browser] = entries;
  const names = entries.map((entry) => entry.name);

  check(entries.length > 0, `the ${browser} archive is not empty`);
  check(names.includes("manifest.json"),
    `manifest.json sits at the root of the ${browser} archive`);
  check(!names.some((name) => name.startsWith("chromium/") || name.startsWith("firefox/")),
    `no wrapper directory hides the ${browser} extension files`);
  check(!names.includes("README.md"),
    `the repository README stays out of the ${browser} archive`);
  for (const name of required) {
    check(names.includes(name), `the ${browser} archive ships ${name}`);
  }
  check(!names.includes("service-worker.js"),
    `the ${browser} archive has no stale service-worker.js`);

  // The bootstrap must load exactly the shared files that are shipped.
  const background = entries.find((entry) => entry.name === "background.js");
  check(Boolean(background), `the ${browser} archive ships background.js`);
  const manifestEntry = entries.find((entry) => entry.name === "manifest.json");
  const manifest = JSON.parse(manifestEntry.data.toString("utf8"));
  const scripts = manifest.background?.scripts ?? [];
  if (browser === "chromium") {
    check(manifest.background?.service_worker === "background.js",
      "the Chromium archive points the worker at background.js");
    const imports = [...background.data.toString("utf8")
      .matchAll(/importScripts\(([^)]*)\)/g)]
      .flatMap((match) => [...match[1].matchAll(/"([^"]+)"/g)].map((m) => m[1]));
    for (const file of imports) {
      check(names.includes(file), `the Chromium worker imports ${file}, which is shipped`);
    }
    check(imports.includes("core.js"), "the Chromium worker imports core.js");
  } else {
    for (const file of scripts) {
      check(names.includes(file), `the Firefox manifest lists ${file}, which is shipped`);
    }
  }
}

// Both archives must carry the same version, and it must be the one the helper
// reports so the release file names cannot drift apart.
const versions = Object.entries(archives).map(([browser, entries]) => {
  const manifest = JSON.parse(entries.find((entry) => entry.name === "manifest.json").data);
  return { browser, version: manifest.version };
});
check(new Set(versions.map((entry) => entry.version)).size === 1,
  `both archives carry one version (${versions.map((v) => `${v.browser} ${v.version}`).join(", ")})`);
check(versions[0]?.version === runHelper("version"),
  "the packaged version matches the version the helper reports");

// The Chromium extension must keep the ID the application accepts.
check(/^[a-p]{32}$/.test(runHelper("id")), "the Chromium extension keeps a pinned ID");
runHelper("verify");

// ext/shared/ is the single source; the assembled directories must match it.
function walk(dir, prefix = "") {
  const files = [];
  for (const name of fs.readdirSync(dir).sort()) {
    if (name.startsWith(".") || name === "README.md") continue;
    const full = path.join(dir, name);
    const entry = prefix ? `${prefix}/${name}` : name;
    if (fs.statSync(full).isDirectory()) files.push(...walk(full, entry));
    else files.push(entry);
  }
  return files;
}

const sharedFiles = walk(sharedDir);
// Files that belong to a browser directory instead of ext/shared/.
const ownFiles = new Set(["manifest.json", "background.js"]);
for (const browser of Object.keys(browserDirs)) {
  const dir = assemble(browser);
  // A renamed shared file must not leave a stale copy behind: the archive would
  // then ship two versions of the same code and the browser would load the old
  // one.
  for (const file of walk(dir)) {
    check(
      sharedFiles.includes(file) || ownFiles.has(file),
      `${browser}/${file} is either shared or browser-specific (stale copy from a rename?)`
    );
  }
  for (const file of sharedFiles) {
    const copied = path.join(dir, file);
    if (!fs.existsSync(copied)) {
      check(false, `${browser}/${file} is assembled from ext/shared/${file}`);
      continue;
    }
    check(
      fs.readFileSync(copied).equals(fs.readFileSync(path.join(sharedDir, file))),
      `${browser}/${file} matches ext/shared/${file}`
    );
  }
  // The manifest and the bootstrap must be the browser's own files.
  for (const own of ownFiles) {
    check(
      fs.readFileSync(path.join(dir, own))
        .equals(fs.readFileSync(path.join(browserDirs[browser], own))),
      `${browser}/${own} is copied from ext/${browser}/${own}`
    );
  }
}

if (failures > 0) {
  console.error(`${failures} extension package check(s) failed`);
  process.exit(1);
}
console.log("extension package checks passed");
