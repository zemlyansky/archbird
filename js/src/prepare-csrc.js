"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");

const MAGIC = Buffer.from("ARCHBIRD_CSRC_BUNDLE_V1\n", "ascii");
const packageRoot = path.resolve(__dirname, "..");
const bundlePath = path.join(packageRoot, "csrc.snapshot.gz");
const destination = path.join(packageRoot, "csrc");

function safeRelative(value) {
  if (
    typeof value !== "string" ||
    value.length === 0 ||
    value.includes("\\") ||
    path.posix.isAbsolute(value)
  ) {
    throw new Error(`unsafe C snapshot path: ${JSON.stringify(value)}`);
  }
  const normalized = path.posix.normalize(value);
  if (normalized !== value || normalized === "." || normalized.startsWith("../")) {
    throw new Error(`unsafe C snapshot path: ${JSON.stringify(value)}`);
  }
  return value;
}

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function decodeBundle(encoded) {
  const payload = zlib.gunzipSync(encoded);
  if (payload.length < MAGIC.length + 8 || !payload.subarray(0, MAGIC.length).equals(MAGIC)) {
    throw new Error("invalid C source bundle signature");
  }
  const headerLength = Number(payload.readBigUInt64BE(MAGIC.length));
  const headerStart = MAGIC.length + 8;
  const headerEnd = headerStart + headerLength;
  if (!Number.isSafeInteger(headerLength) || headerEnd > payload.length) {
    throw new Error("truncated C source bundle header");
  }
  const header = JSON.parse(payload.subarray(headerStart, headerEnd).toString("utf8"));
  if (header?.artifact !== "archbird-c-source-bundle" || !Array.isArray(header.files)) {
    throw new Error("invalid C source bundle inventory");
  }
  const content = payload.subarray(headerEnd);
  const files = [];
  let expectedOffset = 0;
  const names = new Set();
  for (const row of header.files) {
    const relative = safeRelative(row?.path);
    if (
      !Number.isSafeInteger(row?.offset) ||
      !Number.isSafeInteger(row?.bytes) ||
      row.offset !== expectedOffset ||
      row.bytes < 0 ||
      names.has(relative)
    ) {
      throw new Error(`invalid C source bundle row: ${relative}`);
    }
    const end = row.offset + row.bytes;
    if (!Number.isSafeInteger(end) || end > content.length) {
      throw new Error(`truncated C source bundle member: ${relative}`);
    }
    const bytes = content.subarray(row.offset, end);
    if (sha256(bytes) !== row.sha256) {
      throw new Error(`C source bundle member digest differs: ${relative}`);
    }
    names.add(relative);
    files.push({ relative, bytes });
    expectedOffset = end;
  }
  if (expectedOffset !== content.length) throw new Error("C source bundle has trailing content");
  return files;
}

const hasExpandedManifest = fs.existsSync(path.join(destination, ".archbird-manifest.json"));
if (hasExpandedManifest && !fs.existsSync(bundlePath)) {
  process.exit(0);
}
if (!fs.existsSync(bundlePath)) {
  throw new Error("C source snapshot is absent; rebuild the package snapshot before build:native");
}

const files = decodeBundle(fs.readFileSync(bundlePath));
if (hasExpandedManifest) {
  for (const { relative, bytes } of files) {
    const current = path.join(destination, ...relative.split("/"));
    if (!fs.existsSync(current) || !fs.readFileSync(current).equals(bytes)) {
      throw new Error(`expanded C source differs from its package snapshot: ${relative}`);
    }
  }
  process.exit(0);
}
const temporary = path.join(
  packageRoot,
  `.csrc-${process.pid}-${crypto.randomBytes(8).toString("hex")}`,
);
fs.mkdirSync(temporary, { recursive: false });
try {
  for (const { relative, bytes } of files) {
    const output = path.join(temporary, ...relative.split("/"));
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, bytes, { mode: 0o644 });
  }
  if (fs.existsSync(destination)) {
    throw new Error(`refusing to replace unrecognized C source directory: ${destination}`);
  }
  fs.renameSync(temporary, destination);
} catch (error) {
  fs.rmSync(temporary, { recursive: true, force: true });
  throw error;
}

console.error(`expanded ${files.length} verified C source files for explicit native build`);
