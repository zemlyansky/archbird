"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { parentPort, workerData } = require("node:worker_threads");
const { Project } = require("./index");

function configBytes(root, configured) {
  if (workerData.configJson) return Buffer.from(workerData.configJson);
  if (workerData.noConfig) return null;
  if (configured) return fs.readFileSync(path.resolve(configured));
  const candidate = path.join(root, "archbird.json");
  try {
    const metadata = fs.lstatSync(candidate);
    return metadata.isFile() && !metadata.isSymbolicLink()
      ? fs.readFileSync(candidate)
      : null;
  } catch (error) {
    if (error && error.code === "ENOENT") return null;
    throw error;
  }
}

function main() {
  const root = path.resolve(workerData.root);
  const project = Project.fromRepository(root, {
    config: configBytes(root, workerData.config),
    ...(workerData.options || {}),
    typescript: workerData.typescript,
  });
  try {
    const map = project.mapJson();
    const document = JSON.parse(map.toString("utf8"));
    const generation = document.evidence?.input_sha256;
    if (typeof generation !== "string" || !/^[0-9a-f]{64}$/.test(generation)) {
      throw new Error("generated Map has no valid evidence.input_sha256");
    }
    const bytes = Uint8Array.from(map);
    parentPort.postMessage({
      files: document.files.map((row) => ({
        bytes: row.bytes,
        path: row.path,
        sha256: row.sha256,
      })),
      generation,
      map: bytes.buffer,
      project: document.project,
    }, [bytes.buffer]);
  } finally {
    project.dispose();
  }
}

try {
  main();
} catch (error) {
  const prefixes = [
    `${path.resolve(workerData.root)}${path.sep}`,
    ...(workerData.config
      ? [`${path.dirname(path.resolve(workerData.config))}${path.sep}`]
      : []),
  ];
  let message = String(error && (error.stack || error.message) || error);
  for (const prefix of prefixes) message = message.replaceAll(prefix, "");
  parentPort.postMessage({
    error: message,
  });
}
