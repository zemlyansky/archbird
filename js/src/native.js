"use strict";

const fs = require("node:fs");
const path = require("node:path");

const packageRoot = path.resolve(__dirname, "..");

function libcName() {
  if (process.platform !== "linux") return null;
  try {
    const report = process.report && process.report.getReport();
    if (report?.header?.glibcVersionRuntime) return "glibc";
  } catch (_) {
    // A disabled process report is not evidence that a native addon is bad.
  }
  return "musl";
}

function platformKey() {
  const libc = libcName();
  return [process.platform, process.arch, libc].filter(Boolean).join("-");
}

function addonCandidates() {
  const candidates = [];
  if (process.env.ARCHBIRD_NATIVE_ADDON) {
    candidates.push({
      kind: "override",
      path: path.resolve(process.env.ARCHBIRD_NATIVE_ADDON),
    });
  }
  candidates.push({
    kind: "prebuilt",
    path: path.join(packageRoot, "native", platformKey(), "_native.node"),
  });
  candidates.push({
    kind: "source-build",
    path: path.join(packageRoot, "build", "Release", "_native.node"),
  });
  return candidates;
}

function decorate(engine, information) {
  Object.defineProperties(engine, {
    ENGINE: { enumerable: true, value: Object.freeze(information) },
  });
  return engine;
}

function tryNative(attempts) {
  for (const candidate of addonCandidates()) {
    if (!fs.existsSync(candidate.path)) {
      attempts.push(`${candidate.kind}: missing`);
      continue;
    }
    try {
      return decorate(require(candidate.path), {
        architecture: process.arch,
        kind: "native",
        platform: process.platform,
        source: candidate.kind,
      });
    } catch (error) {
      attempts.push(`${candidate.kind}: ${error.message}`);
    }
  }
  return null;
}

function tryWasm(attempts) {
  const candidate = path.join(packageRoot, "wasm", "archbird.sync.js");
  if (!fs.existsSync(candidate)) {
    attempts.push("wasm: missing");
    return null;
  }
  try {
    const module = require(candidate);
    const { createWasmFacade } = require("./wasm-facade");
    return createWasmFacade(module, { mode: "node-sync" });
  } catch (error) {
    attempts.push(`wasm: ${error.message}`);
    return null;
  }
}

function loadEngine() {
  const requested = (process.env.ARCHBIRD_ENGINE || "auto").toLowerCase();
  if (!["auto", "native", "wasm"].includes(requested)) {
    throw new Error("ARCHBIRD_ENGINE must be auto, native, or wasm");
  }
  const attempts = [];
  const native = requested === "wasm" ? null : tryNative(attempts);
  if (native) return native;
  if (requested === "native") {
    throw new Error(`Archbird native addon is unavailable\n${attempts.join("\n")}`);
  }
  const wasm = tryWasm(attempts);
  if (wasm) return wasm;
  throw new Error(`Archbird engine is unavailable\n${attempts.join("\n")}`);
}

module.exports = loadEngine();
