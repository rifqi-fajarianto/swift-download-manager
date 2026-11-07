#!/usr/bin/env node
// Release helper for the browser extensions (Chromium and Firefox).
//
// Both extensions are assembled from one set of shared sources in ext/shared/
// plus a per-browser manifest and bootstrap:
//
//   ext/shared/     popup.html, popup.css, popup.js, api.js, core.js,
//                   cookie-permission.js, action-icon.js, icons/
//   ext/chromium/   manifest.json (MV3 + fixed "key"), background.js
//   ext/firefox/    manifest.json (MV3 + gecko.id), background.js
//
// The assembled copies inside ext/chromium/ and ext/firefox/ are generated and
// ignored by git: `build` writes them for a checkout that is loaded directly,
// and `zip` assembles a throwaway copy so packaging never depends on the state
// of the working tree. CMake performs the same copy for the install rules, so a
// system install needs no Node.
//
// The Chromium extension ships as an unpacked ZIP because current Chrome builds
// reject self-signed CRX files dragged onto chrome://extensions
// (CRX_REQUIRED_PROOF_MISSING), and Windows/macOS refuse local CRX installs
// outright. The Firefox extension ships as an unpacked ZIP too: an XPI needs a
// Mozilla signature to install permanently, so the ZIP is what users load from
// about:debugging or sign with `web-ext sign`.
//
// Two things must stay true for the Chromium ZIP to work:
//   1. The ZIP holds the extension files at its root, so manifest.json sits next
//      to the icons directory, not inside a wrapper folder.
//   2. The ID derived from the manifest's fixed public key matches the ID the
//      app accepts in src/ui/browser_integration.cpp. A mismatch means the app
//      silently rejects every handshake from the packaged extension.
//
// Usage:
//   node tools/extension_pack.mjs build  --browser chromium|firefox [--out DIR]
//   node tools/extension_pack.mjs verify [--manifest FILE] [--source FILE]
//   node tools/extension_pack.mjs version
//   node tools/extension_pack.mjs id     [--manifest FILE]
//   node tools/extension_pack.mjs zip --out FILE [--browser B] [--dir DIR]

import {
  copyFileSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { createHash } from "node:crypto";
import { tmpdir } from "node:os";
import { deflateRawSync } from "node:zlib";
import path from "node:path";
import process from "node:process";

const browsers = {
  chromium: {
    dir: "ext/chromium",
    manifest: "ext/chromium/manifest.json",
    // Files that live in the browser directory instead of ext/shared/.
    own: ["manifest.json", "background.js"],
  },
  firefox: {
    dir: "ext/firefox",
    manifest: "ext/firefox/manifest.json",
    own: ["manifest.json", "background.js"],
  },
};

const sharedDir = "ext/shared";
const defaultSource = "src/ui/browser_integration.cpp";

// Files that belong to the repository but not to the installed extension.
const excluded = new Set(["README.md", ".DS_Store"]);

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

function readOptions(argv) {
  const options = { command: argv[0] };
  for (let i = 1; i < argv.length; i += 1) {
    const flag = argv[i];
    if (!flag.startsWith("--")) fail(`unexpected argument '${flag}'`);
    const value = argv[i + 1];
    if (value === undefined) fail(`missing value for ${flag}`);
    options[flag.slice(2)] = value;
    i += 1;
  }
  return options;
}

function browserName(options) {
  const name = options.browser ?? "chromium";
  if (!browsers[name]) {
    fail(`unknown browser '${name}'; expected chromium or firefox`);
  }
  return name;
}

function readJson(file) {
  try {
    return JSON.parse(readFileSync(file, "utf8"));
  } catch (error) {
    fail(`cannot read ${file}: ${error.message}`);
  }
}

function readManifest(file) {
  const manifest = readJson(file);
  if (!manifest.key) {
    // Without the key an unpacked Chromium load hashes the directory path
    // instead, so the ID would change on every machine and never match the app.
    fail(`${file} has no "key"; the packaged Chromium extension needs a fixed ID`);
  }
  return manifest;
}

// Both manifests must carry the same version: one release ships one extension
// number for every browser, and the ZIP file names use it.
export function extensionVersion(manifests = browsers) {
  const versions = new Map();
  for (const [name, spec] of Object.entries(manifests)) {
    versions.set(name, readJson(spec.manifest).version);
  }
  const unique = new Set(versions.values());
  if (unique.size !== 1) {
    const detail = [...versions].map(([name, version]) => `${name} ${version}`).join(", ");
    fail(`the extension manifests disagree on the version: ${detail}`);
  }
  return [...unique][0];
}

// Chromium derives the ID from the first 16 bytes of SHA-256 over the DER
// public key, with each hex digit mapped to the letters a-p.
export function extensionIdFromKey(base64Key) {
  const digest = createHash("sha256").update(Buffer.from(base64Key, "base64")).digest();
  return [...digest.subarray(0, 16).toString("hex")]
    .map((digit) => String.fromCharCode("a".charCodeAt(0) + parseInt(digit, 16)))
    .join("");
}

function pinnedExtensionId(sourceFile) {
  let source;
  try {
    source = readFileSync(sourceFile, "utf8");
  } catch (error) {
    fail(`cannot read ${sourceFile}: ${error.message}`);
  }
  const match = source.match(/kExtensionId\[\]\s*=\s*"([a-p]{32})"/);
  if (!match) fail(`cannot find kExtensionId in ${sourceFile}`);
  return match[1];
}

function collectFiles(root) {
  const files = [];
  const walk = (dir, prefix) => {
    for (const name of readdirSync(dir).sort()) {
      if (excluded.has(name) || name.startsWith(".")) continue;
      const full = path.join(dir, name);
      const entry = prefix ? `${prefix}/${name}` : name;
      if (statSync(full).isDirectory()) walk(full, entry);
      else files.push({ name: entry, data: readFileSync(full) });
    }
  };
  walk(root, "");
  return files;
}

// Copies the browser's own files, then ext/shared/ on top of them, into
// `target`. Returns the shared file names it wrote.
function assemble(name, target) {
  const spec = browsers[name];
  // The version check runs first so a half-written directory is impossible.
  extensionVersion();
  if (name === "chromium") readManifest(spec.manifest);
  else readJson(spec.manifest);

  mkdirSync(target, { recursive: true });
  for (const own of spec.own) {
    copyFileSync(path.join(spec.dir, own), path.join(target, own));
  }

  const written = [];
  const walk = (dir, prefix) => {
    for (const name of readdirSync(dir).sort()) {
      if (name.startsWith(".") || excluded.has(name)) continue;
      const full = path.join(dir, name);
      const entry = prefix ? `${prefix}/${name}` : name;
      if (statSync(full).isDirectory()) {
        walk(full, entry);
        continue;
      }
      const destination = path.join(target, entry);
      mkdirSync(path.dirname(destination), { recursive: true });
      copyFileSync(full, destination);
      written.push(entry);
    }
  };
  walk(sharedDir, "");
  return written;
}

function buildDirectory(name, out) {
  const target = out ?? browsers[name].dir;
  const written = assemble(name, target);
  console.log(`assembled ${target} (${written.length} shared files)`);
  return target;
}

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let value = n;
  for (let bit = 0; bit < 8; bit += 1) {
    value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
  }
  return value >>> 0;
});

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

// A fixed timestamp keeps the ZIP byte-identical across rebuilds, so the
// published checksum only changes when the extension itself changes.
const dosDate = 0x0021; // 1980-01-01
const dosTime = 0x0000;

function buildZip(files) {
  const chunks = [];
  const central = [];
  let offset = 0;

  for (const file of files) {
    const name = Buffer.from(file.name, "utf8");
    const deflated = deflateRawSync(file.data, { level: 9 });
    const checksum = crc32(file.data);

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0, 6);
    local.writeUInt16LE(8, 8);
    local.writeUInt16LE(dosTime, 10);
    local.writeUInt16LE(dosDate, 12);
    local.writeUInt32LE(checksum, 14);
    local.writeUInt32LE(deflated.length, 18);
    local.writeUInt32LE(file.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    local.writeUInt16LE(0, 28);

    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0);
    entry.writeUInt16LE(20, 4);
    entry.writeUInt16LE(20, 6);
    entry.writeUInt16LE(0, 8);
    entry.writeUInt16LE(8, 10);
    entry.writeUInt16LE(dosTime, 12);
    entry.writeUInt16LE(dosDate, 14);
    entry.writeUInt32LE(checksum, 16);
    entry.writeUInt32LE(deflated.length, 20);
    entry.writeUInt32LE(file.data.length, 24);
    entry.writeUInt16LE(name.length, 28);
    entry.writeUInt16LE(0, 30);
    entry.writeUInt16LE(0, 32);
    entry.writeUInt16LE(0, 34);
    entry.writeUInt16LE(0, 36);
    entry.writeUInt32LE(0, 38);
    entry.writeUInt32LE(offset, 42);

    chunks.push(local, name, deflated);
    central.push(entry, name);
    offset += local.length + name.length + deflated.length;
  }

  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(0, 4);
  end.writeUInt16LE(0, 6);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  end.writeUInt16LE(0, 20);

  return Buffer.concat([...chunks, directory, end]);
}

function main() {
  const options = readOptions(process.argv.slice(2));

  switch (options.command) {
    case "id": {
      const file = options.manifest ?? browsers.chromium.manifest;
      console.log(extensionIdFromKey(readManifest(file).key));
      return;
    }
    case "version": {
      console.log(extensionVersion());
      return;
    }
    case "build": {
      buildDirectory(browserName(options), options.out);
      return;
    }
    case "verify": {
      const derived = extensionIdFromKey(readManifest(options.manifest ?? browsers.chromium.manifest).key);
      const pinned = pinnedExtensionId(options.source ?? defaultSource);
      if (derived !== pinned) {
        fail(
          `extension ID mismatch: manifest derives ${derived} but ` +
            `${options.source ?? defaultSource} pins ${pinned}`,
        );
      }
      console.log(`extension ID ${derived} matches the app allowlist`);
      return;
    }
    case "zip": {
      if (!options.out) fail("zip needs --out FILE");
      const name = browserName(options);
      // Without --dir the archive is built from a fresh assembly, so a stale
      // checkout copy can never leak into a release.
      let dir = options.dir;
      let temporary = null;
      if (!dir) {
        temporary = mkdtempSync(path.join(tmpdir(), `swiftdm-${name}-`));
        dir = path.join(temporary, name);
        assemble(name, dir);
      }
      try {
        const files = collectFiles(dir);
        if (!files.some((file) => file.name === "manifest.json")) {
          fail(`${dir} has no manifest.json at its root`);
        }
        writeFileSync(options.out, buildZip(files));
        console.log(`wrote ${options.out} (${files.length} files)`);
      } finally {
        if (temporary) rmSync(temporary, { recursive: true, force: true });
      }
      return;
    }
    default:
      fail("expected one of: build, verify, version, id, zip");
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname)) {
  main();
}
