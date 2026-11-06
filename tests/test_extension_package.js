// Headless checks for the release ZIP of the Chromium extension. The ZIP is
// what users extract and load with "Load unpacked", so it must keep the files
// at its root and stay installable as-is; this test builds it with the same
// helper the release workflow uses and inspects the archive.

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const zlib = require("node:zlib");
const { execFileSync } = require("node:child_process");

const root = path.join(__dirname, "..");
const extensionDir = path.join(root, "ext", "chromium");
const helper = path.join(root, "tools", "extension_pack.mjs");

let failures = 0;

function check(condition, description) {
  if (!condition) {
    console.error(`FAIL ${description}`);
    ++failures;
  }
}

const output = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "swiftdm-ext-")), "ext.zip");

function pack() {
  execFileSync(process.execPath, [helper, "zip", "--out", output], {
    cwd: root,
    stdio: "pipe",
  });
  return fs.readFileSync(output);
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

const first = pack();
const entries = readZip(first);
const names = entries.map((entry) => entry.name);

check(entries.length > 0, "the archive is not empty");
check(names.includes("manifest.json"), "manifest.json sits at the archive root");
check(
  !names.some((name) => name.startsWith("chromium/")),
  "no wrapper directory hides the extension files"
);
check(
  !names.includes("README.md"),
  "the repository README stays out of the installable archive"
);
check(names.includes("service-worker.js"), "the background service worker ships");
check(names.includes("icons/icon128.png"), "the toolbar icons ship");

// Every file the manifest points at must exist, otherwise Chrome refuses to
// load the unpacked directory with a misleading "file not found" error.
const manifestEntry = entries.find((entry) => entry.name === "manifest.json");
const manifest = manifestEntry ? JSON.parse(manifestEntry.data.toString("utf8")) : {};
for (const size of ["16", "32", "48", "128"]) {
  check(
    names.includes(`icons/icon${size}.png`),
    `manifest icon ${size} is packaged`
  );
}
for (const file of ["service-worker.js", manifest.action?.default_popup]) {
  if (file) check(names.includes(file), `manifest reference ${file} is packaged`);
}

check(
  /^\d+\.\d+\.\d+$/.test(manifest.version ?? ""),
  "the packaged manifest carries a three-part version"
);
check(
  typeof manifest.key === "string" && manifest.key.length > 0,
  "the packaged manifest keeps the fixed key so the ID is stable"
);

// A rebuilt archive must be byte-identical: the published SHA256SUMS entry
// should only change when the extension itself changes.
const second = pack();
check(
  first.equals(second),
  "packing twice produces the same bytes (deterministic archive)"
);

if (failures > 0) {
  console.error(`${failures} extension package check(s) failed`);
  process.exit(1);
}
console.log("extension package checks passed");
