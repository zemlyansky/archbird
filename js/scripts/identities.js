"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const ts = require("typescript");

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function writeBrowserIdentities(packageRoot) {
  const browserPaths = [
    "src/browser.js",
    "src/providers/typescript.js",
    "src/wasm-facade.js",
    "src/wasm.js",
  ];
  const browserMaterial = browserPaths.map((relative) => {
    const bytes = fs.readFileSync(path.join(packageRoot, relative));
    return `${relative}:${sha256(bytes)}\n`;
  }).join("");
  const providerPath = path.join(packageRoot, "src/providers/typescript.js");
  const identities = {
    artifact: "archbird-javascript-identities",
    browser_host_sha256: sha256(Buffer.from(browserMaterial)),
    schema_version: 1,
    typescript_provider_sha256: sha256(Buffer.concat([
      fs.readFileSync(providerPath),
      Buffer.from(`\0typescript:${ts.version}`),
    ])),
  };
  const generated = path.join(packageRoot, "src/generated");
  fs.mkdirSync(generated, { recursive: true });
  const destination = path.join(generated, "identities.json");
  fs.writeFileSync(destination, `${JSON.stringify(identities, null, 2)}\n`);
  return destination;
}

module.exports = { writeBrowserIdentities };

if (require.main === module) {
  writeBrowserIdentities(path.resolve(__dirname, ".."));
}
