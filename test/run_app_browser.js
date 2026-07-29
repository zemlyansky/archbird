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

if (process.argv.length !== 8) {
  throw new Error(
    "usage: run_app_browser.js APP_ROOT GRAPH_VIEW_JSON MAP_JSON VERIFICATION_JSON SOURCE_DIRECTORY SCREENSHOT",
  );
}

const appRoot = path.resolve(process.argv[2]);
const graphView = path.resolve(process.argv[3]);
const map = path.resolve(process.argv[4]);
const verification = path.resolve(process.argv[5]);
const sourceDirectory = path.resolve(process.argv[6]);
const projectConfiguration = path.join(sourceDirectory, "archbird.json");
const screenshot = path.resolve(process.argv[7]);

function browserPlan() {
  const sha = (character) => character.repeat(64);
  const output = path.join(path.dirname(screenshot), "plan.json");
  fs.writeFileSync(output, JSON.stringify({
    schema_version: 1,
    artifact: "plan",
    provenance: "derived",
    tool: {
      name: "archbird",
      version: "0.0.1",
      implementation_sha256: sha("a"),
    },
    source: {
      project: "map-base",
      map: {
        sha256: sha("b"),
        input_sha256: sha("c"),
        configuration_sha256: sha("d"),
        producer_implementation_sha256: sha("e"),
      },
      verification: {
        sha256: sha("f"),
        policy_sha256: sha("1"),
        producer_implementation_sha256: sha("2"),
      },
    },
    objective: "Provide the reviewed implementation for the missing API.",
    items: [{
      id: "implement-api",
      statement: "Implement the missing API in js/index.js.",
      provenance: "derived",
      origins: [{
        constraint_id: "JAVASCRIPT-ENTRY",
        constraint_result_sha256: sha("3"),
        issue_fingerprint: sha("4"),
      }],
      evidence: [],
      depends_on: [],
      operation: {
        action: "manual",
        instructions: "Provide reviewed implementation code.",
        candidate_paths: ["js/index.js"],
      },
      acceptance: { constraints: ["JAVASCRIPT-ENTRY"] },
      unknowns: ["implementation-required"],
      executable: false,
      non_executable_reasons: [
        "Verification does not establish implementation semantics.",
      ],
    }],
    preserved_constraints: [],
    unknowns: [{
      id: "implementation-required",
      statement: "Implementation code requires review.",
      item_id: "implement-api",
      constraint_id: "JAVASCRIPT-ENTRY",
    }],
  }));
  return output;
}

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

async function loadArtifact(page, file, kind) {
  await page.setInputFiles('input[accept="application/json,.json"]', file);
  await page.waitForFunction(
    (expected) => {
      const meta = document.querySelector(".artifact-meta small");
      const status = document.querySelector(".artifact-meta .status");
      return meta?.textContent?.includes(`${expected} ·`) && !status?.classList.contains("busy");
    },
    kind,
    { timeout: 60_000 },
  );
  if (await page.locator(".graph-canvas").count()) {
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
  }
  await page.locator(".artifact-drop small")
    .getByText("Saved artifact · opened in this browser", { exact: true })
    .waitFor();
}

async function saveDownload(page, button, output) {
  const pending = page.waitForEvent("download");
  await page.getByRole("button", { name: button, exact: true }).click();
  await (await pending).saveAs(output);
}

async function selectGraphResult(page, value) {
  await page.locator(".search input").fill(value);
  const result = page.locator(".results button").first();
  await result.waitFor({ timeout: 10_000 });
  await result.click();
}

async function activateSelected(page, name) {
  await page.getByRole("button", { name, exact: true }).click();
  await assertAnchoredRelayout(page);
}

async function changeMapAxis(page, name, attribute, value) {
  await page.getByRole("button", { name, exact: true }).click();
  await page.locator(
    `.graph-canvas[data-${attribute}="${value}"][data-layout-ready="true"]`,
  ).waitFor({ timeout: 30_000 });
}

async function assertAnchoredRelayout(page) {
  await page.locator(
    '.graph-canvas[data-layout-ready="false"][data-has-layout="true"]',
  ).waitFor({ timeout: 30_000 });
  await page.locator('.graph-canvas[data-layout-ready="true"]')
    .waitFor({ timeout: 30_000 });
  const drift = Number(await page.locator(".graph-canvas").getAttribute("data-anchor-drift"));
  if (!Number.isFinite(drift) || drift > 1) {
    throw new Error(`graph expansion moved its selected anchor by ${drift}px`);
  }
}

async function selectedGraphPoint(page) {
  const canvas = page.locator(".graph-canvas");
  const [box, x, y] = await Promise.all([
    canvas.boundingBox(),
    canvas.getAttribute("data-selected-x"),
    canvas.getAttribute("data-selected-y"),
  ]);
  if (!box || x === null || y === null) {
    throw new Error("selected graph node has no rendered position");
  }
  return {
    clientX: box.x + Number(x),
    clientY: box.y + Number(y),
    x: Number(x),
    y: Number(y),
  };
}

async function hoverSelected(page) {
  const point = await selectedGraphPoint(page);
  for (const [x, y] of [[0, 0], [-4, 0], [4, 0], [0, -4], [0, 4]]) {
    await page.mouse.move(point.clientX + x, point.clientY + y);
    if (await page.locator(".graph-tooltip").isVisible()) return;
    await page.waitForTimeout(50);
  }
  await page.locator(".graph-tooltip").waitFor({ timeout: 10_000 });
}

async function assertDesktopWorkspaceVisible(page, operation) {
  const geometry = await page.evaluate(() => {
    const canvas = document.querySelector(".graph-canvas");
    const topbar = document.querySelector(".topbar");
    if (!(canvas instanceof HTMLElement) || !(topbar instanceof HTMLElement)) {
      return null;
    }
    const canvasBounds = canvas.getBoundingClientRect();
    const topbarBounds = topbar.getBoundingClientRect();
    return {
      canvasBottom: canvasBounds.bottom,
      canvasTop: canvasBounds.top,
      documentHeight: document.documentElement.scrollHeight,
      scrollY: window.scrollY,
      topbarBottom: topbarBounds.bottom,
      viewportHeight: window.innerHeight,
    };
  });
  if (!geometry) {
    throw new Error(`${operation} has no rendered graph workspace`);
  }
  if (
    Math.abs(geometry.scrollY) > 1
    || geometry.documentHeight > geometry.viewportHeight + 1
    || geometry.canvasTop < geometry.topbarBottom - 1
    || geometry.canvasBottom > geometry.viewportHeight + 1
  ) {
    throw new Error(
      `${operation} left graph outside desktop viewport: ${JSON.stringify(geometry)}`,
    );
  }
}

function zoomControl(page) {
  return page.locator("label.zoom-control").filter({ hasText: "Zoom" });
}

async function dispatchWheel(page, deltaY, deltaMode) {
  const point = await selectedGraphPoint(page);
  await page.locator(".graph-canvas").dispatchEvent("wheel", {
    bubbles: true,
    cancelable: true,
    clientX: point.clientX,
    clientY: point.clientY,
    deltaMode,
    deltaY,
  });
  await page.waitForTimeout(50);
  const zoom = Number(
    (await zoomControl(page).locator("output").textContent()).replace("%", ""),
  );
  const after = await selectedGraphPoint(page);
  const drift = Math.hypot(after.x - point.x, after.y - point.y);
  if (drift > 1) {
    throw new Error(`pointer-centered wheel zoom moved its anchor by ${drift}px`);
  }
  return zoom;
}

async function doubleClickSelected(page) {
  const point = await selectedGraphPoint(page);
  await page.mouse.dblclick(point.clientX, point.clientY, { delay: 70 });
  await assertAnchoredRelayout(page);
}

async function main() {
  const environment = browserEnvironment(path.dirname(screenshot));
  Object.assign(process.env, environment);
  const chromium = loadChromium();
  const executablePath = sharedChromium(chromium);
  const server = createStaticServer(appRoot);
  const url = await listen(server);
  const archive = path.join(path.dirname(screenshot), "source.zip");
  const noConfigDirectory = path.join(path.dirname(screenshot), "source-no-config");
  sourceArchive(archive);
  fs.rmSync(noConfigDirectory, { force: true, recursive: true });
  fs.cpSync(sourceDirectory, noConfigDirectory, { recursive: true });
  fs.rmSync(path.join(noConfigDirectory, "archbird.json"));
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
      colorScheme: "dark",
      viewport: { width: 1440, height: 900 },
    });
    const page = await context.newPage();
    const errors = [];
    const consoleErrors = [];
    const unexpectedApiRequests = [];
    const remoteRequests = [];
    page.on("pageerror", (error) => errors.push(error.stack || error.message));
    page.on("request", (request) => {
      const target = new URL(request.url());
      if (target.origin !== new URL(url).origin) remoteRequests.push(request.url());
      if (target.pathname.startsWith("/api/")) unexpectedApiRequests.push(request.url());
    });
    page.on("console", (message) => {
      if (message.type() === "error") {
        const location = message.location();
        consoleErrors.push(`${message.text()}${location.url ? ` (${location.url})` : ""}`);
      }
    });
    await page.goto(url, { waitUntil: "networkidle", timeout: 30_000 });

    await page.getByLabel("Theme").selectOption("light");
    await page.waitForFunction(() => getComputedStyle(document.body).backgroundColor === "rgb(244, 246, 248)");
    await page.getByLabel("Theme").selectOption("dark");

    await loadArtifact(page, graphView, "archbird-graph-view");
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.locator(".search input").fill("helper");
    const result = page.locator(".results button").first();
    await result.waitFor({ timeout: 10_000 });
    await result.click();
    await page.waitForSelector(".inspector code", { timeout: 10_000 });
    await page.locator(".graph-canvas").focus();
    await page.keyboard.press("Escape");
    await page.keyboard.press("ArrowDown");
    await page.waitForSelector(".inspector code", { timeout: 10_000 });
    await page.getByLabel("Evidence").selectOption("unique");
    await page.locator(".graph-canvas").focus();
    await page.keyboard.press("Escape");
    await page.getByText("6 / 1", { exact: true }).waitFor({ timeout: 10_000 });
    await page.getByLabel("Evidence").selectOption("all");
    const downloadedView = path.join(path.dirname(screenshot), "downloaded-view.json");
    await saveDownload(page, "Save view JSON", downloadedView);
    if (!fs.readFileSync(downloadedView).equals(fs.readFileSync(graphView))) {
      throw new Error("saved graph view differs from the loaded artifact bytes");
    }

    await loadArtifact(page, map, "map");
    const downloadedMap = path.join(path.dirname(screenshot), "downloaded-map.json");
    await saveDownload(page, "Save canonical artifact", downloadedMap);
    if (!fs.readFileSync(downloadedMap).equals(fs.readFileSync(map))) {
      throw new Error("saved canonical Map differs from the loaded artifact bytes");
    }
    const graphml = path.join(path.dirname(screenshot), "map.graphml");
    await saveDownload(page, "Export", graphml);
    if (!fs.readFileSync(graphml, "utf8").startsWith("<?xml")) {
      throw new Error("GraphML export is not XML");
    }
    await page.getByLabel("Structural export format").selectOption("mermaid");
    const mermaid = path.join(path.dirname(screenshot), "map.mmd");
    await saveDownload(page, "Export", mermaid);
    if (!fs.readFileSync(mermaid, "utf8").startsWith("%% Archbird")) {
      throw new Error("Mermaid export is not an Archbird graph");
    }

    await page.getByText("Focused Query", { exact: true }).click();
    await page.locator(".query-panel input").first().fill("js/index.js:add");
    await page.getByRole("button", { name: "Run Query", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.getByRole("button", { name: "Back to Map", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await assertDesktopWorkspaceVisible(page, "Query to Map navigation");
    await changeMapAxis(page, "component", "group-by", "component");
    const zoomBeforeSelection = await zoomControl(page).locator("output").textContent();
    await selectGraphResult(page, "javascript");
    const zoomAfterSelection = await zoomControl(page).locator("output").textContent();
    if (zoomAfterSelection !== zoomBeforeSelection) {
      throw new Error(
        `selecting a graph node changed zoom from ${zoomBeforeSelection} to ${zoomAfterSelection}`,
      );
    }
    await hoverSelected(page);
    await page.locator(".graph-tooltip strong").getByText("javascript", { exact: true }).waitFor();

    const zoomSlider = zoomControl(page).locator('input[type="range"]');
    await zoomSlider.fill("100");
    const pixelZoom = await dispatchWheel(page, -96, 0);
    await zoomSlider.fill("100");
    const lineZoom = await dispatchWheel(page, -3, 1);
    if (Math.abs(pixelZoom - lineZoom) > 2) {
      throw new Error(`wheel delta modes disagree: pixel=${pixelZoom}%, line=${lineZoom}%`);
    }
    await zoomSlider.fill("100");
    const cappedZoom = await dispatchWheel(page, -10_000, 0);
    if (cappedZoom > 143) {
      throw new Error(`one wheel frame exceeded the zoom cap: ${cappedZoom}%`);
    }
    await page.getByRole("button", { name: "Fit graph", exact: true }).click();
    await doubleClickSelected(page);
    await selectGraphResult(page, "js/");
    await activateSelected(page, "Expand directory");
    await selectGraphResult(page, "js/index.js");
    await activateSelected(page, "Expand symbols");
    await selectGraphResult(page, "add");
    await page.locator(".inspector h2").getByText("add", { exact: true }).waitFor();
    await page.getByRole("button", { name: "Collapse all", exact: true }).click();
    await zoomSlider.fill("150");
    await page.getByText("150%", { exact: true }).waitFor();

    await loadArtifact(page, verification, "verification");
    await page.waitForSelector(".contract-editor", { timeout: 10_000 });
    await page.getByLabel("Owner").fill("architecture");
    await page.getByLabel("Rationale").fill("Temporary browser-test waiver.");
    await page.getByLabel("Expires on").fill("2026-08-31");
    const waiver = path.join(path.dirname(screenshot), "waiver.json");
    await saveDownload(page, "Save waiver candidate", waiver);
    if (!JSON.parse(fs.readFileSync(waiver, "utf8")).fingerprint) {
      throw new Error("waiver download lacks a finding fingerprint");
    }

    await loadArtifact(page, projectConfiguration, "project-configuration");
    await page.waitForSelector(".contract-editor", { timeout: 10_000 });
    const reviewed = path.join(path.dirname(screenshot), "reviewed-archbird.json");
    await saveDownload(page, "Save project configuration", reviewed);
    const reviewedConfiguration = JSON.parse(fs.readFileSync(reviewed, "utf8"));
    if (
      Object.hasOwn(reviewedConfiguration, "schema_version") ||
      Object.hasOwn(reviewedConfiguration, "version")
    ) {
      throw new Error("reviewed project configuration contains a version field");
    }

    await loadArtifact(page, browserPlan(), "plan");
    await page.getByRole("heading", { name: "Structural change plan", exact: true }).waitFor();

    await page.setInputFiles('input[accept="application/json,.json"]', map);
    await page.setInputFiles('input[accept="application/json,.json"]', verification);
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("verification ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.waitForTimeout(500);
    if (!await page.locator(".artifact-meta small").textContent()
      .then((text) => text.includes("verification ·"))) {
      throw new Error("an older artifact request replaced the latest selection");
    }

    await page.setInputFiles("input[webkitdirectory]", sourceDirectory);
    await page.locator(".snapshot-button").first().waitFor({ timeout: 60_000 });
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.getByText("Constraints", { exact: true }).waitFor({ timeout: 10_000 });
    await changeMapAxis(page, "component", "group-by", "component");
    const firstConstraint = page.locator(".constraint-row").first();
    await firstConstraint.click();
    await page.getByText("Architecture constraint", { exact: true }).waitFor();
    await selectGraphResult(page, "javascript");
    if (await firstConstraint.evaluate((row) => row.classList.contains("active"))) {
      throw new Error("graph selection left the previous constraint selected");
    }
    await activateSelected(page, "Expand member files");
    await selectGraphResult(page, "js/");
    await activateSelected(page, "Expand directory");
    await selectGraphResult(page, "js/index.js");
    await page.getByRole("button", { name: "Open js/index.js", exact: true }).click();
    await page.getByText("Source · utf-8").waitFor({ timeout: 10_000 });
    if (!await page.locator(".inspector pre").first().textContent().then((text) => text.includes("function"))) {
      throw new Error("live source inspector did not return js/index.js contents");
    }
    await page.locator(".snapshot-button").first().click();
    await page.getByRole("button", { name: "Return to live Map", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("browser-live"),
      undefined,
      { timeout: 30_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });

    await page.setInputFiles("input[webkitdirectory]", noConfigDirectory);
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("browser-live"),
      undefined,
      { timeout: 60_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    const noConfigNodes = Number(
      await page.locator(".graph-canvas").getAttribute("data-node-count"),
    );
    if (!Number.isSafeInteger(noConfigNodes) || noConfigNodes <= 0) {
      throw new Error(`browser zero-config folder produced ${noConfigNodes} overview nodes`);
    }
    if (await page.getByRole("button", { name: "component", exact: true }).count()) {
      throw new Error("browser zero-config folder exposed reviewed component grouping");
    }
    await changeMapAxis(page, "layer", "group-by", "layer");
    await selectGraphResult(page, "JavaScript");
    await page.getByText("inferred layer group", { exact: true }).waitFor();
    await changeMapAxis(page, "directory", "group-by", "directory");
    await selectGraphResult(page, "js/");
    await page.getByText("inferred directory group", { exact: true }).waitFor();
    await activateSelected(page, "Expand area");
    await selectGraphResult(page, "js/index.js");
    await activateSelected(page, "Expand symbols");
    await selectGraphResult(page, "add");
    await page.locator(".inspector h2").getByText("add", { exact: true }).waitFor();

    await page.setInputFiles('input[accept="application/zip,.zip"]', archive);
    await page.waitForFunction(
      () => !document.querySelector(".status.busy"),
      undefined,
      { timeout: 60_000 },
    );
    await page.getByText("map-base", { exact: true }).waitFor({ timeout: 10_000 });
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.screenshot({ path: screenshot, fullPage: true });

    const mobileContext = await browser.newContext({
      colorScheme: "dark",
      viewport: { width: 390, height: 844 },
    });
    const mobile = await mobileContext.newPage();
    await mobile.goto(url, { waitUntil: "networkidle", timeout: 30_000 });
    await mobile.waitForFunction(() =>
      getComputedStyle(document.body).backgroundColor === "rgb(11, 15, 20)");
    await loadArtifact(mobile, graphView, "archbird-graph-view");
    await mobile.locator(".search input").fill("helper");
    await mobile.locator(".results button").first().click();
    const geometry = await mobile.evaluate(() => {
      const graph = document.querySelector(".graph-stage")?.getBoundingClientRect();
      const inspector = document.querySelector(".inspector")?.getBoundingClientRect();
      return graph && inspector ? { graphBottom: graph.bottom, inspectorTop: inspector.top } : null;
    });
    if (!geometry || geometry.inspectorTop < geometry.graphBottom) {
      throw new Error(`mobile inspector overlaps graph: ${JSON.stringify(geometry)}`);
    }
    await mobile.getByRole("button", { name: "Close inspector", exact: true }).click();
    if (await mobile.locator(".inspector").isVisible()) {
      throw new Error("mobile inspector did not close");
    }
    await mobile.evaluate(() => window.scrollTo(0, 0));
    await mobile.screenshot({
      path: screenshot.replace(/(\.[^.]+)$/, "-mobile$1"),
      fullPage: true,
    });
    await mobileContext.close();

    if (errors.length) throw new Error(errors.join("\n"));
    if (consoleErrors.length) throw new Error(consoleErrors.join("\n"));
    if (unexpectedApiRequests.length) {
      throw new Error(`static app made API requests: ${unexpectedApiRequests.join(", ")}`);
    }
    if (remoteRequests.length) {
      throw new Error(`offline app made remote requests: ${remoteRequests.join(", ")}`);
    }
    console.log(
      `offline app workflow passed (${browser.version()}, ${executablePath})`,
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
