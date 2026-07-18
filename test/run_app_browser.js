#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const {
  browserEnvironment,
  createStaticServer,
  listen,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (process.argv.length !== 7) {
  throw new Error(
    "usage: run_app_browser.js APP_ROOT GRAPH_VIEW_JSON MAP_JSON SOURCE_DIRECTORY SCREENSHOT",
  );
}

const appRoot = path.resolve(process.argv[2]);
const graphView = path.resolve(process.argv[3]);
const map = path.resolve(process.argv[4]);
const sourceDirectory = path.resolve(process.argv[5]);
const screenshot = path.resolve(process.argv[6]);

function sourceArchive(output) {
  const { zipSync } = require(path.join(path.dirname(appRoot), "node_modules/fflate"));
  const rootName = path.basename(sourceDirectory);
  const files = {};
  const pending = [sourceDirectory];
  while (pending.length) {
    const directory = pending.pop();
    for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
      const candidate = path.join(directory, entry.name);
      if (entry.isDirectory()) pending.push(candidate);
      else if (entry.isFile()) {
        const relative = path.relative(sourceDirectory, candidate).split(path.sep).join("/");
        files[`${rootName}/${relative}`] = fs.readFileSync(candidate);
      }
    }
  }
  fs.writeFileSync(output, Buffer.from(zipSync(files)));
}

async function main() {
  const environment = browserEnvironment(path.dirname(screenshot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  const server = createStaticServer(appRoot);
  const url = await listen(server);
  const archive = path.join(path.dirname(screenshot), "source.zip");
  sourceArchive(archive);
  let browser;
  try {
    browser = await chromium.launch({
      executablePath,
      headless: true,
      env: environment,
      args: [
        "--disable-crash-reporter",
        `--crash-dumps-dir=${path.dirname(screenshot)}`,
        "--disable-dev-shm-usage",
        "--no-sandbox",
        "--disable-setuid-sandbox",
      ],
    });
    const context = await browser.newContext({
      acceptDownloads: true,
      viewport: { width: 1440, height: 900 },
    });
    const page = await context.newPage();
    const errors = [];
    const remoteRequests = [];
    page.on("pageerror", (error) => errors.push(error.stack || error.message));
    page.on("request", (request) => {
      const target = new URL(request.url());
      if (target.origin !== new URL(url).origin) remoteRequests.push(request.url());
    });
    await page.goto(url, { waitUntil: "networkidle", timeout: 30_000 });
    await page.setInputFiles('input[accept="application/json,.json"]', graphView);
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.locator(".search input").fill("helper");
    const result = page.locator(".results button").first();
    await result.waitFor({ timeout: 10_000 });
    await result.click();
    await page.waitForSelector(".inspector code", { timeout: 10_000 });
    const downloadPromise = page.waitForEvent("download");
    await page.getByRole("button", { name: "Save exact view JSON" }).click();
    const download = await downloadPromise;
    const downloadPath = path.join(path.dirname(screenshot), "downloaded-view.json");
    await download.saveAs(downloadPath);
    if (!fs.readFileSync(downloadPath).equals(fs.readFileSync(graphView))) {
      throw new Error("saved graph view differs from the loaded artifact bytes");
    }
    await page.screenshot({ path: screenshot, fullPage: true });

    await page.setInputFiles('input[accept="application/json,.json"]', map);
    await page.getByText("map-base", { exact: true }).waitFor({ timeout: 30_000 });
    await page.getByRole("button", { name: "files", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".inspector")?.textContent?.includes("Projectionfiles"),
      { timeout: 30_000 },
    );

    await page.setInputFiles("input[webkitdirectory]", sourceDirectory);
    await page.locator(".snapshot-button").first().waitFor({ timeout: 60_000 });
    await page.getByRole("button", { name: "files", exact: true }).click();
    await page.locator(".search input").fill("js/index.js");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Open js/index.js", exact: true }).click();
    await page.getByText("Source · utf-8").waitFor({ timeout: 10_000 });
    if (!await page.locator(".inspector pre").first().textContent().then((text) => text.includes("function"))) {
      throw new Error("live source inspector did not return js/index.js contents");
    }

    await page.setInputFiles('input[accept="application/zip,.zip"]', archive);
    await page.waitForFunction(
      () => !document.querySelector(".status.busy"),
      { timeout: 60_000 },
    );
    await page.getByText("map-base", { exact: true }).waitFor({ timeout: 10_000 });
    await page.screenshot({ path: screenshot, fullPage: true });
    if (errors.length) throw new Error(errors.join("\n"));
    if (remoteRequests.length) {
      throw new Error(`offline app made remote requests: ${remoteRequests.join(", ")}`);
    }
    console.log(
      `offline graph-view + Map + directory/ZIP app passed (${browser.version()}, ${executablePath})`,
    );
  } finally {
    if (browser) await browser.close();
    await new Promise((resolve) => server.close(resolve));
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
