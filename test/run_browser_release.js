#!/usr/bin/env node
"use strict";

const path = require("node:path");
const {
  browserEnvironment,
  createStaticServer,
  listen,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (![4, 5].includes(process.argv.length)) {
  throw new Error("usage: run_browser_release.js BROWSER_ROOT VERSION [OUTPUT]");
}

const root = path.resolve(process.argv[2]);
const version = process.argv[3];
const output = process.argv[4];
async function main() {
  const environment = browserEnvironment(root);
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  const server = createStaticServer(root);
  const url = await listen(server);

  let browser;
  try {
    browser = await chromium.launch({
      executablePath,
      headless: true,
      env: environment,
      args: [
        "--disable-crash-reporter",
        `--crash-dumps-dir=${root}`,
        "--disable-dev-shm-usage",
        "--no-sandbox",
        "--disable-setuid-sandbox",
      ],
    });
    const page = await browser.newPage();
    const pageErrors = [];
    page.on("pageerror", (error) => pageErrors.push(error.stack || error.message));
    await page.goto(url, { waitUntil: "load", timeout: 30_000 });
    await page.waitForFunction(
      () => document.body.textContent !== "pending",
      undefined,
      { timeout: 30_000 },
    );
    const result = await page.textContent("body");
    if (pageErrors.length) throw new Error(pageErrors.join("\n"));
    if (result.startsWith("ERROR:")) throw new Error(result);
    const parsed = JSON.parse(result);
    const expected = {
      engine: "wasm",
      files: 2,
      freshness: "current",
      indexes: 1,
      membershipFinding: "src/defs.js",
      membershipOverlap: "src/defs.js",
      project: "browser-release",
      sameLineKinds: ["declaration", "function"],
      semanticEdges: 1,
      version,
    };
    const implementationSha256 = parsed.implementationSha256;
    delete parsed.implementationSha256;
    if (
      !/^[0-9a-f]{64}$/.test(implementationSha256 || "")
      || JSON.stringify(parsed) !== JSON.stringify(expected)
    ) {
      throw new Error(`unexpected browser result: ${result}`);
    }
    if (output) {
      require("node:fs").writeFileSync(output, `${JSON.stringify({
        artifact: "archbird-browser-release-conformance",
        engine: "wasm",
        implementation_sha256: implementationSha256,
        operations: [
          "freshness",
          "map",
          "projection",
          "semantic-index",
        ],
        version,
      }, null, 2)}\n`);
    }
    console.log(`real-browser packaged Wasm Map passed (${browser.version()}, ${executablePath})`);
  } finally {
    if (browser) await browser.close();
    await new Promise((resolve) => server.close(resolve));
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
