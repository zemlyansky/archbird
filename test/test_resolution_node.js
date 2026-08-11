"use strict";

const path = require("node:path");

if (process.argv.length !== 5 && process.argv.length !== 6) {
  throw new Error(
    "usage: test_resolution_node.js ADDON REPOSITORY FIXTURE [default]",
  );
}
process.env.ARCHBIRD_NATIVE_ADDON = path.resolve(process.argv[2]);
const { resolveDiscovery } = require(path.join(path.resolve(process.argv[3]), "js/src/index.js"));
const fixture = path.resolve(process.argv[4]);
const mode = process.argv[5] || "matrix";
let outputs;
if (mode === "default") {
  outputs = [resolveDiscovery(fixture)];
} else if (mode === "authored") {
  outputs = [resolveDiscovery(fixture, {
    config: Buffer.from(JSON.stringify({
      layers: [{
        globs: ["**/*.js"],
        language: "javascript",
        name: "authored",
      }],
      packages: [{
        kind: "npm",
        layer: "authored",
        name: "authored",
        path: "package.json",
      }],
      project: "authored",
    })),
  })];
} else if (mode === "ignore-overlay") {
  outputs = [resolveDiscovery(fixture, {
    _sourceOverlay: {
      ".archbirdignore": Buffer.from(
        "# virtual after-state\npackages/ignored/\n",
      ),
    },
  })];
} else {
  outputs = [
      resolveDiscovery(fixture),
      resolveDiscovery(fixture, {
        project: "cli",
        ignoreFiles: [".customignore"],
        maxFileBytes: 100,
        maxIndexBytes: 1000,
      }),
      resolveDiscovery(fixture, {
        ignore: false,
        ignoreFiles: [".customignore"],
      }),
  ];
}
process.stdout.write(`${outputs.map((value) => value.toString("hex")).join("\n")}\n`);
