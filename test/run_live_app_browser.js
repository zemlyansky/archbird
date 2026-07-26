#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { spawn } = require("node:child_process");
const {
  browserEnvironment,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (process.argv.length < 6) {
  throw new Error(
    "usage: run_live_app_browser.js APP_ROOT FIXTURE_ROOT TEMP_ROOT SCREENSHOT "
    + "[--empty] [--no-config] [--python]",
  );
}

const appRoot = path.resolve(process.argv[2]);
const fixtureRoot = path.resolve(process.argv[3]);
const temporaryRoot = path.resolve(process.argv[4]);
const screenshot = path.resolve(process.argv[5]);
const options = new Set(process.argv.slice(6));
for (const option of options) {
  if (!["--empty", "--no-config", "--python"].includes(option)) {
    throw new Error(`unsupported live-browser option: ${option}`);
  }
}
const noConfig = options.has("--no-config");
const empty = options.has("--empty");
const pythonHost = options.has("--python");
const repositoryRoot = path.join(temporaryRoot, "repository");

function pythonServer() {
  const root = path.resolve(__dirname, "..");
  const args = [
    "-m", "archbird", "serve",
    "--root", repositoryRoot,
    "--port", "0",
    ...(noConfig ? ["--no-config"] : []),
  ];
  const child = spawn(process.env.PYTHON || "python", args, {
    cwd: root,
    env: {
      ...process.env,
      PYTHONPATH: path.join(root, "py"),
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stderr = "";
  child.stderr.on("data", (chunk) => {
    stderr += chunk.toString("utf8");
  });
  const url = new Promise((resolve, reject) => {
    let stdout = "";
    const exited = (code, signal) => {
      reject(new Error(
        `Python live server exited before announcing its URL `
        + `(status ${code}, signal ${signal}): ${stderr}`,
      ));
    };
    child.once("exit", exited);
    child.stdout.on("data", (chunk) => {
      stdout += chunk.toString("utf8");
      const line = stdout.split(/\r?\n/, 1)[0];
      if (/^http:\/\/127\.0\.0\.1:\d+\/$/.test(line)) {
        child.off("exit", exited);
        resolve(line);
      }
    });
  });
  return {
    child,
    close: async () => {
      if (child.exitCode !== null) return;
      child.kill("SIGTERM");
      await new Promise((resolve) => child.once("exit", resolve));
    },
    url,
  };
}

async function changeMapAxis(page, name, attribute, value) {
  await page.getByRole("button", { name, exact: true }).click();
  const expected = page.locator(
    `.graph-canvas[data-${attribute}="${value}"][data-layout-ready="true"]`,
  );
  try {
    await expected.waitFor({ timeout: 30_000 });
  } catch (cause) {
    const canvas = page.locator(".graph-canvas");
    const state = await canvas.count()
      ? {
        groupBy: await canvas.getAttribute("data-group-by"),
        layoutReady: await canvas.getAttribute("data-layout-ready"),
        mapView: await canvas.getAttribute("data-map-view"),
      }
      : { canvas: "absent" };
    const error = await page.locator(".error-banner").count()
      ? (await page.locator(".error-banner").innerText()).trim()
      : "no app error";
    throw new Error(
      `map axis ${attribute}=${value} did not evaluate: `
      + `${JSON.stringify(state)}; ${error}`,
      { cause },
    );
  }
}

async function waitForInitialMap(page, timeout = 30_000) {
  await Promise.race([
    page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout },
    ),
    page.locator(".error-banner, .error-card").first()
      .waitFor({ timeout }).then(async () => {
        throw new Error(
          `live app failed before Map presentation: `
          + `${(await page.locator(".error-banner, .error-card").first().innerText()).trim()}`,
        );
      }),
  ]);
}

async function main() {
  fs.rmSync(temporaryRoot, { force: true, recursive: true });
  fs.mkdirSync(temporaryRoot, { recursive: true });
  fs.cpSync(fixtureRoot, repositoryRoot, { recursive: true });
  if (empty) {
    fs.rmSync(repositoryRoot, { force: true, recursive: true });
    fs.mkdirSync(repositoryRoot, { recursive: true });
  }
  // Keep Chromium's Unix-domain socket paths below the platform limit; the
  // repository fixture itself remains isolated under temporaryRoot.
  const environment = browserEnvironment(path.dirname(temporaryRoot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  let server;
  let browser;
  let originalCandidate;
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
    if (pythonHost) {
      const process = pythonServer();
      server = {
        close: process.close,
        url: await process.url,
      };
    } else {
      const { createLiveServer, LiveRepository } = require("../js/src/serve");
      originalCandidate = LiveRepository.prototype.candidate;
      LiveRepository.prototype.candidate = async function delayedCandidate() {
        await new Promise((resolve) => setTimeout(resolve, 900));
        return originalCandidate.call(this);
      };
      server = await createLiveServer({
        app: appRoot,
        noConfig,
        port: 0,
        root: repositoryRoot,
      });
    }
    const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    const page = await context.newPage();
    const errors = [];
    const consoleErrors = [];
    const hostMethods = [];
    const remoteRequests = [];
    const wasmRequests = [];
    page.on("pageerror", (error) => errors.push(error.stack || error.message));
    page.on("console", (message) => {
      if (message.type() === "error") {
        const location = message.location();
        consoleErrors.push(`${message.text()}${location.url ? ` (${location.url})` : ""}`);
      }
    });
    page.on("request", (request) => {
      const target = new URL(request.url());
      if (target.pathname.endsWith(".wasm")) wasmRequests.push(request.url());
      if (target.origin !== new URL(server.url).origin) {
        remoteRequests.push(request.url());
      }
      if (target.pathname === "/api/v1/request" && request.method() === "POST") {
        try {
          hostMethods.push(request.postDataJSON().method);
        } catch {
          hostMethods.push("<invalid>");
        }
      }
    });
    await page.goto(server.url, { waitUntil: "domcontentloaded", timeout: 30_000 });
    if (!pythonHost) {
      await page.getByRole("heading", { name: "Mapping project", exact: true })
        .waitFor({ timeout: 10_000 });
      if (await page.getByRole("heading", { name: "Open a project or saved artifact" }).count()) {
        throw new Error("live app showed the empty workspace while Map analysis was running");
      }
    }
    if (noConfig) {
      await waitForInitialMap(page);
    } else {
      await page.getByText("map-base", { exact: true }).waitFor({ timeout: 30_000 });
    }
    if (originalCandidate) {
      const { LiveRepository } = require("../js/src/serve");
      LiveRepository.prototype.candidate = originalCandidate;
    }
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("server"),
      undefined,
      { timeout: 30_000 },
    );
    if (empty) {
      await page.getByRole("heading", { name: "No source files were mapped", exact: true })
        .waitFor({ timeout: 30_000 });
      if (await page.locator(".graph-canvas").count()) {
        throw new Error("empty mapped scope rendered a blank graph canvas");
      }
      if (errors.length) throw new Error(errors.join("\n"));
      if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
      if (remoteRequests.length) {
        throw new Error(`local app made remote requests: ${remoteRequests.join(", ")}`);
      }
      if (wasmRequests.length) {
        throw new Error(`live server app requested browser Wasm: ${wasmRequests.join(", ")}`);
      }
      if (hostMethods.includes("map")) {
        throw new Error(
          `live exploration downloaded the canonical Map: ${hostMethods.join(", ")}`,
        );
      }
      if (!hostMethods.includes("projection")) {
        throw new Error(
          `live exploration missed typed server operations: ${hostMethods.join(", ")}`,
        );
      }
      console.log(
        `local ${pythonHost ? "Python" : "Node"} empty-scope state passed `
        + `(${browser.version()})`,
      );
      return;
    }
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    const initialNodes = Number(
      await page.locator(".graph-canvas").getAttribute("data-node-count"),
    );
    const initialEdges = Number(
      await page.locator(".graph-canvas").getAttribute("data-edge-count"),
    );
    if (!Number.isSafeInteger(initialNodes) || initialNodes <= 0) {
      throw new Error(
        `live ${noConfig ? "zero-config" : "configured"} overview is empty: `
        + `${initialNodes} nodes / ${initialEdges} edges`,
      );
    }
    if (noConfig) {
      if (!pythonHost) {
        let rejectProjection = true;
        await page.route("**/api/v1/request", async (route) => {
          const request = route.request();
          let envelope;
          try {
            envelope = request.postDataJSON();
          } catch {
            await route.continue();
            return;
          }
          if (rejectProjection && envelope?.method === "projection") {
            rejectProjection = false;
            await route.fulfill({
              body: JSON.stringify({
                error: {
                  code: "injected-projection-failure",
                  message: "injected projection evaluation failure",
                },
                id: envelope.id,
                ok: false,
                protocol_version: 1,
                session: envelope.session,
              }),
              contentType: "application/json",
              status: 200,
            });
            return;
          }
          await route.continue();
        });
        fs.appendFileSync(
          path.join(repositoryRoot, "js/index.js"),
          "\nexport const projectionFailureProbe = true;\n",
        );
        await page.getByText(
          "Could not complete operation; current view is unchanged.",
          { exact: true },
        ).waitFor({ timeout: 30_000 });
        if (Number(await page.locator(".graph-canvas").getAttribute("data-node-count")) <= 0) {
          throw new Error("projection failure discarded the last-good graph");
        }
        await page.unroute("**/api/v1/request");
        fs.appendFileSync(
          path.join(repositoryRoot, "js/index.js"),
          "\nexport const projectionRecoveryProbe = true;\n",
        );
        await page.locator(".error-banner").waitFor({ state: "detached", timeout: 30_000 });
      }
      if (await page.getByRole("button", { name: "component", exact: true }).count()) {
        throw new Error("zero-config app exposed reviewed component grouping");
      }
      for (const view of ["architecture", "tests", "evidence", "overview"]) {
        await changeMapAxis(page, view, "map-view", view);
      }
      await changeMapAxis(page, "layer", "group-by", "layer");
      await page.locator(".search input").fill("JavaScript");
      await page.locator(".results button").first().click();
      await page.getByText("inferred layer group", { exact: true }).waitFor();
      await changeMapAxis(page, "directory", "group-by", "directory");
      await page.locator(".search input").fill("js/");
      await page.locator(".results button").first().click();
      await page.getByText("inferred directory group", { exact: true }).waitFor();
      await page.getByRole("button", { name: "Expand area", exact: true }).click();
      await page.locator('.graph-canvas[data-layout-ready="true"]')
        .waitFor({ timeout: 30_000 });
      await page.locator(".search input").fill("js/index.js");
      await page.locator(".results button").first().click();
      await page.getByRole("button", { name: "Expand symbols", exact: true }).click();
      await page.locator('.graph-canvas[data-layout-ready="true"]')
        .waitFor({ timeout: 30_000 });
      await page.locator(".search input").fill("add");
      await page.locator(".results button").first().click();
      await page.locator(".inspector h2").getByText("add", { exact: true }).waitFor();
      await page.locator(".search input").fill("js/index.js");
      await page.locator(".results button").first().click();
      await page.getByRole("button", { name: "Open js/index.js", exact: true }).click();
      await page.getByText("Source · utf-8").waitFor();
      await page.screenshot({ path: screenshot, fullPage: true });
      if (errors.length) throw new Error(errors.join("\n"));
      if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
      if (remoteRequests.length) {
        throw new Error(`local app made remote requests: ${remoteRequests.join(", ")}`);
      }
      if (wasmRequests.length) {
        throw new Error(`live server app requested browser Wasm: ${wasmRequests.join(", ")}`);
      }
      if (hostMethods.includes("map")) {
        throw new Error(
          `live exploration downloaded the canonical Map: ${hostMethods.join(", ")}`,
        );
      }
      if (!hostMethods.includes("projection") || !hostMethods.includes("source")) {
        throw new Error(
          `live exploration missed typed server operations: ${hostMethods.join(", ")}`,
        );
      }
      console.log(
        `local ${pythonHost ? "Python" : "Node"} zero-config live app exploration passed `
        + `(${initialNodes} nodes, ${initialEdges} edges, ${browser.version()})`,
      );
      return;
    }
    await page.getByText("Constraints", { exact: true }).waitFor({ timeout: 10_000 });
    await page.getByRole("button", { name: "directory", exact: true }).waitFor();
    await page.getByRole("button", { name: "component", exact: true }).waitFor();
    await page.getByRole("button", { name: "layer", exact: true }).waitFor();
    await page.getByRole("button", { name: "language", exact: true }).waitFor();
    await changeMapAxis(page, "component", "group-by", "component");
    await page.locator(".search input").fill("javascript");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Expand member files", exact: true }).click();
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.locator(".search input").fill("js/");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Expand directory", exact: true }).click();
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.locator(".search input").fill("js/index.js");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Expand symbols", exact: true }).click();
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.locator(".search input").fill("add");
    await page.locator(".results button").first().click();
    await page.locator(".inspector h2").getByText("add", { exact: true }).waitFor();
    await page.locator(".search input").fill("js/index.js");
    await page.locator(".results button").first().click();
    await page.getByRole("button", { name: "Open js/index.js", exact: true }).click();
    await page.getByText("Source · utf-8").waitFor();

    fs.appendFileSync(path.join(repositoryRoot, "js/index.js"), "\nexport const live = true;\n");
    await page.waitForFunction(
      () => document.querySelectorAll(".snapshot-button").length === 2,
      undefined,
      { timeout: 30_000 },
    );
    await page.getByRole("button", { name: "Compare", exact: true }).click();
    await page.getByRole("heading", { name: "Structural comparison", exact: true })
      .waitFor({ timeout: 30_000 });
    await page.screenshot({
      path: screenshot.replace(/(\.[^.]+)$/, "-diff$1"),
      fullPage: true,
    });
    await page.getByRole("button", { name: "Back to Map", exact: true }).click();
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.locator(".snapshot-button").last().click();
    await page.getByRole("button", { name: "Return to live Map", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("server-live"),
      undefined,
      { timeout: 30_000 },
    );

    fs.writeFileSync(path.join(repositoryRoot, "archbird.json"), "{}\n");
    await page.getByText("Candidate failed; showing last good view.", { exact: true })
      .waitFor({ timeout: 30_000 });
    await page.waitForSelector(".graph-canvas canvas");
    fs.copyFileSync(
      path.join(fixtureRoot, "archbird.json"),
      path.join(repositoryRoot, "archbird.json"),
    );
    await page.locator(".error-banner").waitFor({ state: "detached", timeout: 30_000 });
    await page.screenshot({ path: screenshot, fullPage: true });

    if (errors.length) throw new Error(errors.join("\n"));
    if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
    if (remoteRequests.length) {
      throw new Error(`local app made remote requests: ${remoteRequests.join(", ")}`);
    }
    if (wasmRequests.length) {
      throw new Error(`live server app requested browser Wasm: ${wasmRequests.join(", ")}`);
    }
    console.log(
      `local live app watch/diff/last-good passed (${browser.version()}, ${executablePath})`,
    );
  } finally {
    if (originalCandidate) {
      const { LiveRepository } = require("../js/src/serve");
      LiveRepository.prototype.candidate = originalCandidate;
    }
    if (browser) await browser.close();
    if (server) await server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
