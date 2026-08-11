#!/usr/bin/env node
"use strict";

const path = require("node:path");
const {
  beginBrowserDiagnostics,
  browserEnvironment,
  clickAndWaitForGraphLayout,
  loadChromium,
  sharedChromium,
} = require("./browser_harness");

if (process.argv.length !== 5) {
  throw new Error(
    "usage: run_large_live_app_browser.js APP_ROOT REPOSITORY_ROOT SCREENSHOT",
  );
}

const appRoot = path.resolve(process.argv[2]);
const repositoryRoot = path.resolve(process.argv[3]);
const screenshot = path.resolve(process.argv[4]);

async function select(page, value) {
  await page.locator(".search input").fill(value);
  const result = page.locator(".results button").first();
  await result.waitFor({ timeout: 30_000 });
  await result.click();
}

async function expand(page, name) {
  await page.getByRole("button", { name, exact: true }).click();
  await page.locator('.graph-canvas[data-layout-ready="true"]')
    .waitFor({ timeout: 60_000 });
}

async function changeGrouping(page, grouping) {
  await page.getByRole("button", { name: grouping, exact: true }).click();
  await page.locator(".operation-progress").waitFor({
    state: "visible",
    timeout: 5_000,
  });
  await page.locator(
    `.graph-canvas[data-group-by="${grouping}"][data-layout-ready="true"]`,
  ).waitFor({ timeout: 60_000 });
  await page.locator(".operation-progress").waitFor({
    state: "hidden",
    timeout: 5_000,
  });
}

async function changeMapView(page, view) {
  await page.getByRole("button", { name: view, exact: true }).click();
  await page.locator(".operation-progress").waitFor({
    state: "visible",
    timeout: 5_000,
  });
  await page.locator(
    `.graph-canvas[data-map-view="${view}"][data-layout-ready="true"]`,
  ).waitFor({ timeout: 60_000 });
  await page.locator(".operation-progress").waitFor({
    state: "hidden",
    timeout: 5_000,
  });
}

async function nodeCount(page) {
  return Number(await page.locator(".graph-canvas").getAttribute("data-node-count"));
}

async function layoutExtent(page, spacing, previous = null) {
  await page.waitForFunction(({ expectedSpacing, prior }) => {
    const canvas = document.querySelector(".graph-canvas");
    const width = canvas?.getAttribute("data-layout-width");
    const height = canvas?.getAttribute("data-layout-height");
    return canvas?.getAttribute("data-layout-ready") === "true"
      && canvas?.getAttribute("data-spacing") === String(expectedSpacing)
      && width !== null && height !== null
      && Number(width) > 0 && Number(height) > 0
      && (!prior || Number(width) !== prior.width || Number(height) !== prior.height);
  }, { expectedSpacing: spacing, prior: previous }, { timeout: 60_000 });
  return page.locator(".graph-canvas").evaluate((element) => ({
    height: Number(element.getAttribute("data-layout-height")),
    width: Number(element.getAttribute("data-layout-width")),
  }));
}

async function assertSelectionFocus(page) {
  await page.waitForFunction(() => {
    const canvas = document.querySelector(".graph-canvas");
    return Number(canvas?.getAttribute("data-focus-count")) > 1
      && Number(canvas?.getAttribute("data-dimmed-count")) > 0;
  }, undefined, { timeout: 10_000 });
}

async function assertMiddlePanPreservesNode(page) {
  const canvas = page.locator(".graph-canvas");
  const before = await canvas.evaluate((element) => ({
    modelX: element.getAttribute("data-selected-model-x"),
    modelY: element.getAttribute("data-selected-model-y"),
    panX: element.getAttribute("data-pan-x"),
    panY: element.getAttribute("data-pan-y"),
  }));
  if (
    before.modelX === null || before.modelY === null
    || before.panX === null || before.panY === null
  ) {
    throw new Error("selected node or viewport coordinates were not published");
  }
  const bounds = await canvas.boundingBox();
  if (!bounds) throw new Error("graph canvas has no browser bounds");
  const origin = {
    x: bounds.x + bounds.width / 2,
    y: bounds.y + bounds.height / 2,
  };
  await page.mouse.move(origin.x, origin.y);
  await page.mouse.down({ button: "middle" });
  await page.mouse.move(origin.x + 96, origin.y + 64, { steps: 4 });
  await page.mouse.up({ button: "middle" });
  await page.waitForFunction(({ panX, panY }) => {
    const element = document.querySelector(".graph-canvas");
    return element?.getAttribute("data-pan-x") !== panX
      || element?.getAttribute("data-pan-y") !== panY;
  }, { panX: before.panX, panY: before.panY }, { timeout: 10_000 });
  const after = await canvas.evaluate((element) => ({
    modelX: element.getAttribute("data-selected-model-x"),
    modelY: element.getAttribute("data-selected-model-y"),
  }));
  if (after.modelX !== before.modelX || after.modelY !== before.modelY) {
    throw new Error(
      `middle-button pan moved the selected node: `
      + `${before.modelX},${before.modelY} -> ${after.modelX},${after.modelY}`,
    );
  }
}

async function waitForMappedWorkspace(page, timeout) {
  await Promise.race([
    page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout },
    ),
    page.locator(".error-banner, .error-card").first()
      .waitFor({ timeout }).then(async () => {
      throw new Error(
        `large live app failed before Map presentation: `
        + `${(await page.locator(".error-banner, .error-card").first().innerText()).trim()}`,
      );
    }),
  ]);
}

async function main() {
  const environment = browserEnvironment(path.dirname(screenshot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  const { defaultProviderCacheDir } = require("../js/src");
  const { createLiveServer } = require("../js/src/serve");
  const started = performance.now();
  const server = await createLiveServer({
    app: appRoot,
    noConfig: true,
    port: 0,
    projectOptions: { cacheDir: defaultProviderCacheDir() },
    root: repositoryRoot,
  });
  let browser;
  let diagnostics;
  let page;
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
    page = await browser.newPage({ viewport: { width: 1600, height: 1000 } });
    diagnostics = await beginBrowserDiagnostics(page.context(), page, screenshot);
    const pageErrors = [];
    const consoleErrors = [];
    const hostMethods = [];
    const wasmRequests = [];
    const remoteRequests = [];
    page.on("pageerror", (error) => pageErrors.push(error.stack || error.message));
    page.on("console", (message) => {
      if (message.type() === "error") consoleErrors.push(message.text());
    });
    page.on("request", (request) => {
      const target = new URL(request.url());
      if (target.pathname.endsWith(".wasm")) wasmRequests.push(request.url());
      if (target.origin !== new URL(server.url).origin) remoteRequests.push(request.url());
      if (target.pathname === "/api/v1/request" && request.method() === "POST") {
        try {
          hostMethods.push(request.postDataJSON().method);
        } catch {
          hostMethods.push("<invalid>");
        }
      }
    });
    await page.goto(server.url, { waitUntil: "domcontentloaded", timeout: 30_000 });
    await waitForMappedWorkspace(page, 180_000);
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 60_000 });
    const readyMs = performance.now() - started;
    const initial = await nodeCount(page);
    if (!Number.isSafeInteger(initial) || initial < 2 || initial > 40) {
      throw new Error(`large zero-config overview is not bounded: ${initial} nodes`);
    }
    const published = server.repository.current;
    const refreshStarted = performance.now();
    await page.reload({ waitUntil: "domcontentloaded", timeout: 30_000 });
    await waitForMappedWorkspace(page, 60_000);
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 60_000 });
    const refreshMs = performance.now() - refreshStarted;
    if (server.repository.current !== published) {
      throw new Error("browser refresh rebuilt the published Map generation");
    }
    if (await nodeCount(page) !== initial) {
      throw new Error("browser refresh changed the structural overview");
    }
    const arrowDirection = page.getByLabel("Arrows");
    if (await arrowDirection.inputValue() !== "flow") {
      throw new Error("graph did not default to provider-to-consumer dependency flow");
    }
    await arrowDirection.selectOption("uses");
    if (await arrowDirection.inputValue() !== "uses") {
      throw new Error("graph did not expose consumer-to-provider uses direction");
    }
    await arrowDirection.selectOption("flow");
    const spacing = page.locator('.zoom-control input[type="range"]').nth(1);
    if (await spacing.getAttribute("min") !== "5") {
      throw new Error("graph spacing does not expose the 5% dense-layout floor");
    }
    if (await spacing.inputValue() !== "60") {
      throw new Error("large graph did not use the compact spacing default");
    }
    const extentAt60 = await layoutExtent(page, 60);
    await spacing.fill("5");
    await page.locator('.graph-canvas[data-spacing="5"][data-layout-ready="true"]')
      .waitFor({ timeout: 60_000 });
    const extentAt5 = await layoutExtent(page, 5, extentAt60);
    if (
      !Number.isFinite(extentAt60.height) || !Number.isFinite(extentAt60.width)
      || !Number.isFinite(extentAt5.height) || !Number.isFinite(extentAt5.width)
      || extentAt5.height * extentAt5.width
        >= extentAt60.height * extentAt60.width * 0.7
    ) {
      throw new Error(
        `5% spacing did not materially compact the layout: `
        + `${extentAt60.width}x${extentAt60.height} -> `
        + `${extentAt5.width}x${extentAt5.height}`,
      );
    }

    if (await page.getByRole("button", { name: "component", exact: true }).count()) {
      throw new Error("large zero-config app exposed reviewed component grouping");
    }
    await changeGrouping(page, "layer");
    await select(page, "layer:auto-c");
    await page.getByText("inferred layer group", { exact: true }).waitFor();
    await changeMapView(page, "architecture");
    await changeGrouping(page, "directory");
    const collapsedArchitecture = await nodeCount(page);
    await select(page, "src/");
    await page.getByText("inferred directory group", { exact: true }).waitFor();
    await expand(page, "Expand area");
    const rootExpanded = await nodeCount(page);
    if (rootExpanded <= initial || rootExpanded > 60) {
      throw new Error(
        `top-level src expansion is not progressive: ${initial} -> ${rootExpanded} nodes`,
      );
    }

    await select(page, "src/projection/");
    await expand(page, "Expand directory");
    await select(page, "src/projection/fact_projection.c");
    await assertSelectionFocus(page);
    await assertMiddlePanPreservesNode(page);
    await expand(page, "Expand symbols");
    await select(page, "extract_file_edges");
    await page.locator(".inspector h2")
      .getByText("extract_file_edges", { exact: true })
      .waitFor({ timeout: 30_000 });
    await select(page, "src/projection/fact_projection.c");
    await page.getByRole(
      "button",
      { name: "Open src/projection/fact_projection.c", exact: true },
    ).click();
    await page.getByText("Source · utf-8").waitFor({ timeout: 30_000 });
    await page.screenshot({ path: screenshot });

    await clickAndWaitForGraphLayout(
      page,
      page.getByRole("button", { name: "Collapse all", exact: true }),
      60_000,
    );
    if (await nodeCount(page) !== collapsedArchitecture) {
      throw new Error("collapse-all did not restore the current architecture frontier");
    }
    if (pageErrors.length) throw new Error(pageErrors.join("\n"));
    if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
    if (wasmRequests.length) {
      throw new Error(`native live app requested browser Wasm: ${wasmRequests.join(", ")}`);
    }
    if (remoteRequests.length) {
      throw new Error(`native live app made remote requests: ${remoteRequests.join(", ")}`);
    }
    if (hostMethods.includes("map")) {
      throw new Error(
        `native live exploration downloaded the canonical Map: ${hostMethods.join(", ")}`,
      );
    }
    if (!hostMethods.includes("projection") || !hostMethods.includes("source")) {
      throw new Error(
        `native live exploration missed typed server operations: ${hostMethods.join(", ")}`,
      );
    }
    console.log(
      `large zero-config exploration passed `
      + `(${initial} overview nodes, ${Math.round(readyMs)} ms to interactive, `
      + `${Math.round(refreshMs)} ms refresh, `
      + `${browser.version()})`,
    );
  } catch (error) {
    if (diagnostics) await diagnostics.retain(error);
    throw error;
  } finally {
    if (diagnostics) await diagnostics.close();
    if (browser) await browser.close();
    await server.close();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
