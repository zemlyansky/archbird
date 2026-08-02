"use strict";

const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const Module = require("node:module");
const path = require("node:path");

const repositoryRoot = path.resolve(process.argv[2] || path.join(__dirname, ".."));
const buildRoot = path.join(repositoryRoot, "build");
fs.mkdirSync(buildRoot, { recursive: true });
const temporaryRoot = fs.mkdtempSync(path.join(buildRoot, "identity-staging-"));
const packageRoot = path.join(temporaryRoot, "js");
const browserPaths = [
  "src/browser.js",
  "src/providers/typescript.js",
  "src/wasm-facade.js",
  "src/wasm.js",
];
for (const relative of browserPaths) {
  const destination = path.join(packageRoot, relative);
  fs.mkdirSync(path.dirname(destination), { recursive: true });
  fs.copyFileSync(path.join(repositoryRoot, "js", relative), destination);
}

const helperPath = path.join(repositoryRoot, "js", "scripts", "identities.js");
const originalLoad = Module._load;
try {
  Module._load = function denyHiddenTypescriptInstall(request, parent, isMain) {
    if (request === "typescript") {
      throw new Error("identity writer unexpectedly loaded TypeScript");
    }
    return Reflect.apply(originalLoad, this, [request, parent, isMain]);
  };
  delete require.cache[require.resolve(helperPath)];
  const { writeBrowserIdentities } = require(helperPath);
  const typescriptVersion = "9.8.7-injected-test";
  const destination = writeBrowserIdentities(packageRoot, { typescriptVersion });
  const identities = JSON.parse(fs.readFileSync(destination, "utf8"));
  const providerBytes = fs.readFileSync(
    path.join(packageRoot, "src/providers/typescript.js"),
  );
  const expectedProviderIdentity = crypto.createHash("sha256").update(Buffer.concat([
    providerBytes,
    Buffer.from(`\0typescript:${typescriptVersion}`),
  ])).digest("hex");
  assert.equal(identities.artifact, "archbird-javascript-identities");
  assert.equal(identities.schema_version, 1);
  assert.equal(identities.typescript_provider_sha256, expectedProviderIdentity);
  assert.match(identities.browser_host_sha256, /^[0-9a-f]{64}$/);
} finally {
  Module._load = originalLoad;
  delete require.cache[require.resolve(helperPath)];
  fs.rmSync(temporaryRoot, { recursive: true, force: true });
}

console.log("JavaScript identity staging injection passed");
