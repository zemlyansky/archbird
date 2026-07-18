#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const {
  browserEnvironment,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (process.argv.length !== 6) {
  throw new Error("usage: run_live_app_browser.js APP_ROOT FIXTURE_ROOT TEMP_ROOT SCREENSHOT");
}

const appRoot = path.resolve(process.argv[2]);
const fixtureRoot = path.resolve(process.argv[3]);
const temporaryRoot = path.resolve(process.argv[4]);
const screenshot = path.resolve(process.argv[5]);
const repositoryRoot = path.join(temporaryRoot, "repository");

async function main() {
  fs.rmSync(temporaryRoot, { force: true, recursive: true });
  fs.mkdirSync(temporaryRoot, { recursive: true });
  fs.cpSync(fixtureRoot, repositoryRoot, { recursive: true });
  // Keep Chromium's Unix-domain socket paths below the platform limit; the
  // repository fixture itself remains isolated under temporaryRoot.
  const environment = browserEnvironment(path.dirname(temporaryRoot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  let server;
  let browser;
  try {
    browser = await chromium.launch({
      executablePath,
      headless: true,
      env: environment,
      args: [
        "--disable-crash-reporter",
        `--crash-dumps-dir=${temporaryRoot}`,
        "--disable-dev-shm-usage",
        "--no-sandbox",
        "--disable-setuid-sandbox",
      ],
    });
    const { createLiveServer } = require("../js/src/serve");
    server = await createLiveServer({ app: appRoot, port: 0, root: repositoryRoot });
    const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    const page = await context.newPage();
    const errors = [];
    const remoteRequests = [];
    page.on("pageerror", (error) => errors.push(error.stack || error.message));
    page.on("request", (request) => {
      if (new URL(request.url()).origin !== new URL(server.url).origin) {
        remoteRequests.push(request.url());
      }
    });
    await page.goto(server.url, { waitUntil: "domcontentloaded", timeout: 30_000 });
    await page.getByText("map-base", { exact: true }).waitFor({ timeout: 30_000 });
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("server"),
      { timeout: 30_000 },
    );
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.getByRole("button", { name: "files", exact: true }).click();
    await page.locator(".search input").fill("js/index.js");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Open js/index.js", exact: true }).click();
    await page.getByText("Source · utf-8").waitFor();

    fs.appendFileSync(path.join(repositoryRoot, "js/index.js"), "\nexport const live = true;\n");
    await page.waitForFunction(
      () => document.querySelectorAll(".snapshot-button").length === 2,
      { timeout: 30_000 },
    );

    fs.writeFileSync(path.join(repositoryRoot, ".archbird.json"), "{}\n");
    await page.getByText("Candidate failed; showing last good view.", { exact: true })
      .waitFor({ timeout: 30_000 });
    await page.waitForSelector(".graph-canvas canvas");
    await page.screenshot({ path: screenshot, fullPage: true });

    if (errors.length) throw new Error(errors.join("\n"));
    if (remoteRequests.length) {
      throw new Error(`local app made remote requests: ${remoteRequests.join(", ")}`);
    }
    console.log(
      `local live app watch/last-good passed (${browser.version()}, ${executablePath})`,
    );
  } finally {
    if (browser) await browser.close();
    if (server) await server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
