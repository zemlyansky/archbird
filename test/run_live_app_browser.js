#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");
const { spawn } = require("node:child_process");
const {
  beginBrowserDiagnostics,
  browserEnvironment,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (process.argv.length < 6) {
  throw new Error(
    "usage: run_live_app_browser.js APP_ROOT FIXTURE_ROOT TEMP_ROOT SCREENSHOT "
    + "[--empty] [--insecure-origin] [--no-config] [--python]",
  );
}

const appRoot = path.resolve(process.argv[2]);
const fixtureRoot = path.resolve(process.argv[3]);
const temporaryRoot = path.resolve(process.argv[4]);
const screenshot = path.resolve(process.argv[5]);
const options = new Set(process.argv.slice(6));
for (const option of options) {
  if (!["--empty", "--insecure-origin", "--no-config", "--python"].includes(option)) {
    throw new Error(`unsupported live-browser option: ${option}`);
  }
}
const noConfig = options.has("--no-config");
const empty = options.has("--empty");
const insecureOrigin = options.has("--insecure-origin");
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

function networkIPv4() {
  for (const rows of Object.values(os.networkInterfaces())) {
    for (const row of rows || []) {
      if (row.family === "IPv4" && !row.internal) return row.address;
    }
  }
  throw new Error("insecure-origin browser test requires a non-loopback IPv4 address");
}

async function createInsecureProxy(upstreamValue) {
  const upstream = new URL(upstreamValue);
  let publicOrigin = "";
  const proxy = http.createServer((request, response) => {
    const headers = { ...request.headers, host: upstream.host };
    if (headers.origin === publicOrigin) headers.origin = upstream.origin;
    const forwarded = http.request(
      new URL(request.url || "/", upstream),
      { headers, method: request.method },
      (incoming) => {
        const responseHeaders = { ...incoming.headers };
        delete responseHeaders["cross-origin-embedder-policy"];
        delete responseHeaders["cross-origin-opener-policy"];
        response.writeHead(incoming.statusCode || 500, responseHeaders);
        incoming.pipe(response);
      },
    );
    forwarded.on("error", (error) => response.destroy(error));
    request.pipe(forwarded);
  });
  await new Promise((resolve, reject) => {
    proxy.once("error", reject);
    proxy.listen(0, "0.0.0.0", resolve);
  });
  publicOrigin = `http://${networkIPv4()}:${proxy.address().port}`;
  return {
    close: () => new Promise((resolve, reject) => {
      proxy.close((error) => error ? reject(error) : resolve());
    }),
    url: `${publicOrigin}/`,
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

async function agentRequest(url, method, params, display) {
  const response = await fetch(new URL("api/v1/request", url), {
    body: JSON.stringify({
      method,
      params,
      ...(display === undefined ? {} : { display }),
    }),
    headers: { "Content-Type": "application/json" },
    method: "POST",
  });
  const result = await response.json();
  if (!response.ok || !result.ok) {
    throw new Error(`agent request failed: ${result.error?.message || response.status}`);
  }
  return result;
}

async function waitForObservedDisplay(url, predicate, timeout = 10_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const state = (await agentRequest(url, "state", {})).result?.observed_display;
    if (state && predicate(state)) return state;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error("browser-observed display state did not become current");
}

async function expectSelectedDocumentation(page, label, text) {
  await page.locator(".inspector h2").getByText(label, { exact: true }).waitFor();
  await page.getByText("Documentation · jsdoc", { exact: true }).waitFor();
  const canvas = page.locator(".graph-canvas");
  const box = await canvas.boundingBox();
  const x = Number(await canvas.getAttribute("data-selected-x"));
  const y = Number(await canvas.getAttribute("data-selected-y"));
  if (!box || !Number.isFinite(x) || !Number.isFinite(y)) {
    throw new Error(`selected ${label} node has no rendered hover position`);
  }
  const tooltip = page.locator(".graph-tooltip p")
    .getByText(text, { exact: true });
  await page.mouse.move(box.x + 2, box.y + 2);
  for (const [dx, dy] of [[0, 0], [-12, 0], [12, 0], [0, -8], [0, 8]]) {
    await page.mouse.move(box.x + x + dx, box.y + y + dy, { steps: 5 });
    if (await tooltip.isVisible()) return;
  }
  const target = await page.evaluate(
    ([left, top]) => document.elementFromPoint(left, top)?.outerHTML || "none",
    [box.x + x, box.y + y],
  );
  throw new Error(`documented ${label} node hover missed (${x}, ${y}; ${target})`);
}

async function expectDocumentedAdd(page) {
  await expectSelectedDocumentation(page, "add", "Add two values.");
}

async function expectDragHidesNodeOverlays(page) {
  const canvas = page.locator(".graph-canvas");
  const box = await canvas.boundingBox();
  const x = Number(await canvas.getAttribute("data-selected-x"));
  const y = Number(await canvas.getAttribute("data-selected-y"));
  if (!box || !Number.isFinite(x) || !Number.isFinite(y)) {
    throw new Error("selected graph node has no draggable position");
  }
  await page.mouse.move(box.x + x, box.y + y);
  await page.mouse.down();
  await page.mouse.move(box.x + x + 48, box.y + y + 32, { steps: 6 });
  await page.locator('.graph-canvas[data-dragging-node="true"]').waitFor();
  if (await page.locator(".graph-node-actions").count()
      || await page.locator(".graph-tooltip").count()) {
    throw new Error("node overlays remained visible while dragging");
  }
  await page.mouse.up();
  await page.locator('.graph-canvas[data-dragging-node="false"]').waitFor();
}

async function expectEntitySearch(page) {
  const input = page.getByRole("combobox", { name: "Search project map" });
  await input.fill("js/index.js");
  const exactFile = page.locator(".entity-search-results button").first();
  await exactFile.getByText("js/index.js", { exact: true }).first()
    .waitFor({ timeout: 10_000 });
  await page.locator(".brand").click();
  await page.locator(".entity-search-results").waitFor({ state: "detached" });
  if (await input.inputValue() !== "js/index.js") {
    throw new Error("outside-click dismissal cleared the search query");
  }
  await input.focus();
  await page.locator(".entity-search-results").waitFor({ timeout: 10_000 });
  await input.press("Escape");
  await page.locator(".entity-search-results").waitFor({ state: "detached" });
  if (await input.inputValue() !== "js/index.js") {
    throw new Error("Escape dismissal cleared the search query");
  }
  await input.fill("function");
  const typedResult = page.locator(".entity-search-results button").first();
  await typedResult.getByText(/^function · /).waitFor({ timeout: 10_000 });
  await typedResult.getByText("kind · exact-field", { exact: true }).waitFor();
  await input.fill("Fixture arithmetic");
  const result = page.locator(".entity-search-results button").first();
  await result.getByText("js/index.js", { exact: true }).waitFor({ timeout: 10_000 });
  await result.getByText("documentation", { exact: false }).waitFor();
  await result.click();
  await page.locator(".entity-search-results").waitFor({ state: "detached" });
  await expectSelectedDocumentation(page, "js/index.js", "Fixture arithmetic module.");
  await expectDragHidesNodeOverlays(page);
  await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
  await input.fill("");
}

async function openEntitySearchResult(page, search, name) {
  const input = page.getByRole("combobox", { name: "Search project map" });
  await input.fill(search);
  const result = page.locator(".entity-search-results button").filter({
    has: page.getByText(name, { exact: true }),
  }).first();
  await result.waitFor({ timeout: 10_000 });
  await result.click();
}

async function main() {
  fs.rmSync(temporaryRoot, { force: true, recursive: true });
  fs.mkdirSync(temporaryRoot, { recursive: true });
  fs.cpSync(fixtureRoot, repositoryRoot, { recursive: true });
  if (empty) {
    fs.rmSync(repositoryRoot, { force: true, recursive: true });
    fs.mkdirSync(repositoryRoot, { recursive: true });
  } else {
    const documentedSource = path.join(repositoryRoot, "js/index.js");
    fs.writeFileSync(
      documentedSource,
      `/** @fileoverview Fixture arithmetic module. */\n`
        + `/** Add two values. */\n${fs.readFileSync(documentedSource, "utf8")}`,
    );
  }
  // Keep Chromium's Unix-domain socket paths below the platform limit; the
  // repository fixture itself remains isolated under temporaryRoot.
  const environment = browserEnvironment(path.dirname(temporaryRoot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  let server;
  let insecureProxy;
  let browser;
  let diagnostics;
  let originalCandidate;
  let page;
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
    insecureProxy = insecureOrigin ? await createInsecureProxy(server.url) : null;
    const browserUrl = insecureProxy?.url || server.url;
    const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    page = await context.newPage();
    diagnostics = await beginBrowserDiagnostics(context, page, screenshot);
    const errors = [];
    const consoleErrors = [];
    const hostMethods = [];
    const metricRequests = [];
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
      if (target.origin !== new URL(browserUrl).origin) {
        remoteRequests.push(request.url());
      }
      if (target.pathname === "/api/v1/request" && request.method() === "POST") {
        try {
          const envelope = request.postDataJSON();
          hostMethods.push(envelope.method);
          if (envelope.method === "projections"
            || (envelope.method === "projection" && envelope.params?.plan?.select === "metrics")) {
            metricRequests.push(envelope);
          }
        } catch {
          hostMethods.push("<invalid>");
        }
      }
    });
    await page.goto(browserUrl, { waitUntil: "domcontentloaded", timeout: 30_000 });
    if (!pythonHost) {
      await page.getByRole("heading", { name: "Mapping project", exact: true })
        .waitFor({ timeout: 10_000 });
      await page.locator(".artifact-drop small")
        .getByText(
          "Live project · analysis in progress on the local Archbird server",
          { exact: true },
        )
        .waitFor();
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
    await page.locator(".artifact-drop small")
      .getByText("Live project · analyzed by the local Archbird server", { exact: true })
      .waitFor();
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
    await page.locator(".metrics-panel").waitFor({ timeout: 30_000 });
    if (await page.locator(".metric-accordion").count() !== 7) {
      throw new Error("live Metrics sidebar does not expose all seven metrics");
    }
    if (metricRequests.length) {
      throw new Error("initial overview eagerly requested metric evidence");
    }
    const complexity = page.locator('[data-metric="source.cyclomatic_complexity"]');
    await complexity.locator(".metric-summary").click();
    await complexity.locator(".metric-facts").waitFor({ timeout: 30_000 });
    if (metricRequests.length !== 1
      || metricRequests[0].params?.plan?.metric !== "source.cyclomatic_complexity") {
      throw new Error(`opening one metric requested unrelated evidence: ${JSON.stringify(metricRequests)}`);
    }
    await complexity.locator(".metric-summary").click();
    await complexity.locator(".metric-summary").click();
    await complexity.locator(".metric-facts").waitFor();
    if (metricRequests.length !== 1) throw new Error("reopening a metric bypassed its cache");
    await agentRequest(server.url, "query", {
      depth: 0,
      symbols: ["add"],
      test_depth: 0,
    });
    if (!await page.locator(".artifact-meta small").textContent()
      .then((value) => value?.includes("map ·"))) {
      throw new Error("private agent Query changed the browser display");
    }
    await agentRequest(server.url, "query", {
      depth: 0,
      symbols: ["add"],
      test_depth: 0,
    }, {});
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
      undefined,
      { timeout: 10_000 },
    );
    await page.reload({ waitUntil: "domcontentloaded" });
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.getByRole("button", { name: "Back to previous graph", exact: true })
      .click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout: 10_000 },
    );
    await agentRequest(server.url, "map", {}, {
      group_by: "language",
      view: "architecture",
    });
    await page.locator(
      '.graph-canvas[data-map-view="architecture"][data-group-by="language"]'
        + '[data-layout-ready="true"]',
    ).waitFor({ timeout: 10_000 });
    await page.getByRole("button", { name: "Back to previous graph", exact: true })
      .click();
    await page.locator(
      '.graph-canvas[data-map-view="overview"][data-group-by="directory"]'
        + '[data-layout-ready="true"]',
    ).waitFor({ timeout: 10_000 });
    await agentRequest(server.url, "query", {
      depth: 0,
      symbols: ["js/index.js:add"],
      test_depth: 0,
    }, { view: "source" });
    await page.locator(".inspector h2").getByText("add", { exact: true })
      .waitFor({ timeout: 10_000 });
    await page.getByText("Source · utf-8", { exact: true }).waitFor();
    await page.getByRole("button", { name: "Back to previous graph", exact: true })
      .click();
    await agentRequest(server.url, "path", {
      direction: "downstream",
      level: "symbol",
      max_depth: 2,
      max_paths: 2,
      relations: ["calls"],
      source: { kind: "symbol", patterns: ["twice"] },
      target: { kind: "symbol", patterns: ["add"] },
    }, {});
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("path ·"),
      undefined,
      { timeout: 10_000 },
    );
    const pathCanvas = page.locator('.graph-canvas[data-layout-ready="true"]');
    await pathCanvas.waitFor({ timeout: 10_000 });
    await page.getByText("Path · found", { exact: true }).waitFor();
    if (Number(await pathCanvas.getAttribute("data-node-count")) < 2
        || Number(await pathCanvas.getAttribute("data-edge-count")) < 1) {
      throw new Error("published Path did not render its witness graph");
    }
    await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 10_000 });
    await expectEntitySearch(page);
    if (insecureOrigin) {
      const security = await page.evaluate(() => ({
        secureContext: isSecureContext,
        subtle: Boolean(globalThis.crypto?.subtle),
      }));
      if (security.secureContext || security.subtle) {
        throw new Error(`insecure-origin test unexpectedly has Web Crypto: ${JSON.stringify(security)}`);
      }
      if (errors.length) throw new Error(errors.join("\n"));
      if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
      console.log(
        `local insecure-origin live app passed without Web Crypto `
        + `(${initialNodes} nodes, ${initialEdges} edges, ${browser.version()})`,
      );
      return;
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
                ok: false,
                protocol_version: 2,
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
        await page.locator(".inspector h2")
          .getByText(`${view[0].toUpperCase()}${view.slice(1)} view`, { exact: true })
          .waitFor();
      }
      await changeMapAxis(page, "layer", "group-by", "layer");
      await page.locator(".inspector").getByText("layer", { exact: true }).first().waitFor();
      await changeMapAxis(page, "directory", "group-by", "directory");
      await openEntitySearchResult(page, "js/index.js", "js/index.js");
      await expectSelectedDocumentation(
        page,
        "js/index.js",
        "Fixture arithmetic module.",
      );
      await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
      await openEntitySearchResult(page, "Add two values", "add");
      await expectDocumentedAdd(page);
      await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
      await openEntitySearchResult(page, "js/index.js", "js/index.js");
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
      if (!hostMethods.includes("projection") || !hostMethods.includes("query")
          || !hostMethods.includes("source")) {
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
    const firstConstraint = page.locator(".constraint-row").first();
    const firstConstraintId = (await firstConstraint.locator("strong").textContent()).trim();
    await firstConstraint.click();
    await page.locator(".inspector h2").getByText(firstConstraintId, { exact: true })
      .waitFor();
    await page.getByRole("button", { name: "Show all", exact: true }).click();
    await page.getByRole("button", { name: "directory", exact: true }).waitFor();
    await page.getByRole("button", { name: "component", exact: true }).waitFor();
    await page.getByRole("button", { name: "layer", exact: true }).waitFor();
    await page.getByRole("button", { name: "language", exact: true }).waitFor();
    await changeMapAxis(page, "component", "group-by", "component");
    await openEntitySearchResult(page, "js/index.js", "js/index.js");
    await expectSelectedDocumentation(
      page,
      "js/index.js",
      "Fixture arithmetic module.",
    );
    await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
    await openEntitySearchResult(page, "Add two values", "add");
    await expectDocumentedAdd(page);
    const observed = await waitForObservedDisplay(
      server.url,
      (state) => state.selected?.label === "add" && state.graph?.view === "symbols",
    );
    if (observed.selected.identity !== 'symbol:["js/index.js","add"]'
        || observed.selected.path !== "js/index.js") {
      throw new Error(`browser selection context is incomplete: ${JSON.stringify(observed)}`);
    }
    await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
    await openEntitySearchResult(page, "js/index.js", "js/index.js");
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

    fs.writeFileSync(
      path.join(repositoryRoot, "archbird.json"),
      '{"schema_version":2}\n',
    );
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
  } catch (error) {
    if (diagnostics) await diagnostics.retain(error);
    throw error;
  } finally {
    if (diagnostics) await diagnostics.close();
    if (originalCandidate) {
      const { LiveRepository } = require("../js/src/serve");
      LiveRepository.prototype.candidate = originalCandidate;
    }
    if (browser) await browser.close();
    if (insecureProxy) await insecureProxy.close();
    if (server) await server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
