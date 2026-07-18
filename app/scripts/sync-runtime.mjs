import { copyFileSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const require = createRequire(import.meta.url);
const { writeBrowserIdentities } = require("../../js/scripts/identities.js");
const build = process.env.ARCHBIRD_WASM_BUILD || resolve(root, "build/wasm/wasm");
const outputs = [
  [resolve(build, "archbird.wasm"), resolve(root, "app/public/archbird.wasm")],
  [resolve(build, "archbird.js"), resolve(root, "js/wasm/archbird.js")],
];
for (const [source, destination] of outputs) {
  if (!statSync(source).isFile()) {
    throw new Error(`missing staged Archbird Wasm output: ${source}`);
  }
  mkdirSync(dirname(destination), { recursive: true });
  copyFileSync(source, destination);
}
writeFileSync(
  resolve(root, "js/wasm/.archbird-meta.json"),
  `${JSON.stringify({ artifact: "archbird-wasm-directory" })}\n`,
);
const identities = writeBrowserIdentities(resolve(root, "js"));
if (!statSync(identities).isFile()) {
  throw new Error(`missing staged Archbird browser identities: ${identities}`);
}
const destination = outputs[0][1];
const digest = createHash("sha256").update(readFileSync(destination)).digest("hex");
console.error(`synced archbird Wasm runtime ${digest}`);
