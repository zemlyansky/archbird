"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { writeBrowserIdentities } = require("./identities");

const packageRoot = path.resolve(__dirname, "..");
const repositoryRoot = path.resolve(packageRoot, "..");
const packageVersion = JSON.parse(
  fs.readFileSync(path.join(packageRoot, "package.json"), "utf8"),
).version;
const source = path.resolve(
  process.env.ARCHBIRD_NATIVE_ADDON ||
    path.join(repositoryRoot, "build/release-native/node/_native.node"),
);
const wasmBuild = path.resolve(
  process.env.ARCHBIRD_WASM_BUILD || path.join(repositoryRoot, "build/wasm/wasm"),
);
const appBuild = path.resolve(
  process.env.ARCHBIRD_APP_BUILD || path.join(repositoryRoot, "app/dist"),
);

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function linuxLibc() {
  if (process.platform !== "linux") return null;
  try {
    if (process.report?.getReport()?.header?.glibcVersionRuntime) return "glibc";
  } catch (_) {
    // Staging still records a conservative musl target when reports are disabled.
  }
  return "musl";
}

function platformKey() {
  return [process.platform, process.arch, linuxLibc()].filter(Boolean).join("-");
}

function validateAddon(bytes) {
  const prefix = bytes.subarray(0, 4).toString("hex");
  const valid =
    (process.platform === "linux" && prefix === "7f454c46") ||
    (process.platform === "win32" && prefix.startsWith("4d5a")) ||
    (process.platform === "darwin" && [
      "cffaedfe", "feedfacf", "cafebabe", "bebafeca",
    ].includes(prefix));
  if (!valid) throw new Error(`native addon has the wrong binary format: ${source}`);
}

function replaceDirectory(destination, markerArtifact) {
  if (fs.existsSync(destination)) {
    const marker = path.join(destination, ".archbird-meta.json");
    const document = fs.existsSync(marker)
      ? JSON.parse(fs.readFileSync(marker, "utf8"))
      : null;
    if (document?.artifact !== markerArtifact) {
      throw new Error(`refusing to replace unrecognized ${destination}`);
    }
    fs.rmSync(destination, { recursive: true });
  }
  fs.mkdirSync(destination, { recursive: true });
}

const metadata = fs.statSync(source);
if (!metadata.isFile()) throw new Error(`native addon is not a file: ${source}`);
if (Number(process.versions.napi || 0) < 8) {
  throw new Error("staging requires Node-API v8 or newer");
}
const nativeBytes = fs.readFileSync(source);
validateAddon(nativeBytes);
const nativeRoot = path.join(packageRoot, "native");
replaceDirectory(nativeRoot, "archbird-node-native-directory");
const nativeDestination = path.join(nativeRoot, platformKey());
fs.mkdirSync(nativeDestination, { recursive: true });
fs.writeFileSync(path.join(nativeDestination, "_native.node"), nativeBytes, { mode: 0o755 });
fs.writeFileSync(
  path.join(nativeDestination, "_native.meta.json"),
  `${JSON.stringify({
    artifact: "archbird-node-addon",
    architecture: process.arch,
    napi: 8,
    platform: process.platform,
    libc: linuxLibc(),
    sha256: sha256(nativeBytes),
  }, null, 2)}\n`,
);

for (const required of ["index.html", "archbird.wasm"]) {
  if (!fs.statSync(path.join(appBuild, required)).isFile()) {
    throw new Error(`missing visualization output: ${path.join(appBuild, required)}`);
  }
}
if (!fs.readFileSync(path.join(appBuild, "archbird.wasm")).equals(
  fs.readFileSync(path.join(wasmBuild, "archbird.wasm")),
)) {
  throw new Error("visualization and package Wasm runtimes differ");
}
const appRoot = path.join(packageRoot, "app");
replaceDirectory(appRoot, "archbird-visualization-application");
fs.cpSync(appBuild, appRoot, { recursive: true });
fs.writeFileSync(
  path.join(appRoot, ".archbird-meta.json"),
  `${JSON.stringify({ artifact: "archbird-visualization-application" })}\n`,
);
fs.writeFileSync(
  path.join(nativeRoot, ".archbird-meta.json"),
  `${JSON.stringify({ artifact: "archbird-node-native-directory" })}\n`,
);

const wasmRoot = path.join(packageRoot, "wasm");
replaceDirectory(wasmRoot, "archbird-wasm-directory");
for (const name of ["archbird.js", "archbird.wasm", "archbird.sync.js"]) {
  const candidate = path.join(wasmBuild, name);
  if (!fs.statSync(candidate).isFile()) throw new Error(`missing Wasm output: ${candidate}`);
  fs.copyFileSync(candidate, path.join(wasmRoot, name));
}
fs.writeFileSync(
  path.join(wasmRoot, ".archbird-meta.json"),
  `${JSON.stringify({ artifact: "archbird-wasm-directory" })}\n`,
);

writeBrowserIdentities(packageRoot);

fs.copyFileSync(path.join(repositoryRoot, "LICENSE"), path.join(packageRoot, "LICENSE"));
console.error(
  `staged archbird@${packageVersion}: ${platformKey()} native + async/sync Wasm + offline app`,
);
