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

if (process.argv.length !== 3) {
  throw new Error("usage: run_browser_release.js BROWSER_ROOT");
}

const root = path.resolve(process.argv[2]);
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
      project: "browser-release",
      semanticEdges: 1,
      version: "0.0.1",
    };
    if (JSON.stringify(parsed) !== JSON.stringify(expected)) {
      throw new Error(`unexpected browser result: ${result}`);
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
