#!/usr/bin/env node
// Release helper for the Chromium extension.
//
// The extension ships as an unpacked ZIP because current Chrome builds reject
// self-signed CRX files dragged onto chrome://extensions (the browser demands a
// Chrome Web Store proof, CRX_REQUIRED_PROOF_MISSING), and Windows/macOS refuse
// local CRX installs outright. The ZIP is what users extract and load with
// "Load unpacked".
//
// Two things must stay true for that to work:
//   1. The ZIP holds the extension files at its root, so manifest.json sits next
//      to the icons directory, not inside a wrapper folder.
//   2. The ID derived from the manifest's fixed public key matches the ID the
//      app accepts in src/ui/browser_integration.cpp. A mismatch means the app
//      silently rejects every handshake from the packaged extension.
//
// Usage:
//   node tools/extension_pack.mjs verify [--manifest FILE] [--source FILE]
//   node tools/extension_pack.mjs id     [--manifest FILE]
//   node tools/extension_pack.mjs zip --out FILE [--dir DIR]

import { readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { deflateRawSync } from "node:zlib";
import { pathToFileURL } from "node:url";
import path from "node:path";
import process from "node:process";

const defaultManifest = "ext/chromium/manifest.json";
const defaultSource = "src/ui/browser_integration.cpp";
const defaultDir = "ext/chromium";

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

function readManifest(file) {
  let manifest;
  try {
    manifest = JSON.parse(readFileSync(file, "utf8"));
  } catch (error) {
    fail(`cannot read ${file}: ${error.message}`);
  }
  if (!manifest.key) {
    // Without the key an unpacked load hashes the directory path instead, so
    // the ID would change on every machine and never match the app.
    fail(`${file} has no "key"; the packaged extension needs a fixed ID`);
  }
  return manifest;
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
  const manifestFile = options.manifest ?? defaultManifest;

  switch (options.command) {
    case "id": {
      console.log(extensionIdFromKey(readManifest(manifestFile).key));
      return;
    }
    case "verify": {
      const derived = extensionIdFromKey(readManifest(manifestFile).key);
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
      readManifest(manifestFile);
      const dir = options.dir ?? defaultDir;
      const files = collectFiles(dir);
      if (!files.some((file) => file.name === "manifest.json")) {
        fail(`${dir} has no manifest.json at its root`);
      }
      writeFileSync(options.out, buildZip(files));
      console.log(`wrote ${options.out} (${files.length} files)`);
      return;
    }
    default:
      fail("expected one of: verify, id, zip");
  }
}

// Compare URLs rather than raw paths: a Windows path such as C:\... is not a
// valid "file://" string, so the helper would silently do nothing there.
if (import.meta.url === pathToFileURL(process.argv[1]).href) main();
