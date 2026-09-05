#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const {
  beginBrowserDiagnostics,
  browserEnvironment,
  clickAndWaitForGraphLayout,
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
    schema_version: 9,
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
    gates: {},
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
      acceptance: {
        constraints: ["JAVASCRIPT-ENTRY"],
        projection_deltas: [],
      },
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
  const result = page.getByRole("option").first();
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
  const manifestRoundDirectory = path.join(
    path.dirname(screenshot),
    "source-manifest-rounds",
  );
  sourceArchive(archive);
  fs.rmSync(noConfigDirectory, { force: true, recursive: true });
  fs.cpSync(sourceDirectory, noConfigDirectory, { recursive: true });
  fs.rmSync(path.join(noConfigDirectory, "archbird.json"));
  fs.rmSync(manifestRoundDirectory, { force: true, recursive: true });
  fs.mkdirSync(path.join(manifestRoundDirectory, "packages/member"), {
    recursive: true,
  });
  fs.writeFileSync(
    path.join(manifestRoundDirectory, "package.json"),
    '{"name":"browser-workspace","version":"1.0.0",' +
      '"workspaces":["packages/*"]}\n',
  );
  fs.writeFileSync(
    path.join(manifestRoundDirectory, "packages/member/package.json"),
    '{"name":"@browser/member","version":"2.0.0"}\n',
  );
  fs.writeFileSync(
    path.join(manifestRoundDirectory, "packages/member/index.js"),
    "export const member = true;\n",
  );
  let browser;
  let context;
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
    context = await browser.newContext({
      acceptDownloads: true,
      colorScheme: "dark",
      viewport: { width: 1440, height: 900 },
    });
    page = await context.newPage();
    diagnostics = await beginBrowserDiagnostics(context, page, screenshot);
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

    const wordmark = page.locator('.brand-wordmark[aria-label="ǎrchbird"]');
    await wordmark.waitFor();
    const initial = wordmark.locator(".brand-initial");
    if (await initial.textContent() !== "ǎ") {
      throw new Error("application wordmark does not render the composed ǎ glyph");
    }
    if (!await initial.evaluate((node) =>
      getComputedStyle(node).backgroundImage.includes("linear-gradient"))) {
      throw new Error("application wordmark does not color the ǎ glyph by region");
    }
    const selectedWordmark = await wordmark.evaluate((node) => {
      const selection = getSelection();
      const range = document.createRange();
      range.selectNodeContents(node);
      selection?.removeAllRanges();
      selection?.addRange(range);
      const text = selection?.toString() || "";
      selection?.removeAllRanges();
      return text;
    });
    if (selectedWordmark !== "ǎrchbird") {
      throw new Error(`application wordmark copies as ${JSON.stringify(selectedWordmark)}`);
    }
    if (await page.getByText("codebase explorer", { exact: true }).count()) {
      throw new Error("obsolete application subtitle is still rendered");
    }
    const favicon = await page.locator('link[rel="icon"]').getAttribute("href");
    const faviconSvg = await page.evaluate(async (href) => {
      const response = await fetch(href);
      return response.text();
    }, favicon);
    if (!faviconSvg.includes(">ǎ</text>") || faviconSvg.includes("<rect")) {
      throw new Error("application favicon is not the plain ǎ wordmark glyph");
    }
    await page.getByRole("heading", { name: "See how a codebase is built." }).waitFor();
    await page.getByRole("button", { name: /Project folder/ }).waitFor();
    await page.getByRole("button", { name: /Source ZIP/ }).waitFor();
    await page.getByRole("button", { name: /Saved artifact/ }).waitFor();
    await page.getByText("Nothing is sent to a remote service.", { exact: false }).waitFor();

    await page.getByLabel("Theme").selectOption("light");
    await page.waitForFunction(() => getComputedStyle(document.body).backgroundColor === "rgb(244, 246, 248)");
    await page.getByLabel("Theme").selectOption("dark");

    await loadArtifact(page, graphView, "archbird-graph-view");
    await page.waitForSelector(".graph-canvas canvas", { timeout: 30_000 });
    await page.getByLabel("Evidence").selectOption("unique");
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

    await page.locator(".search input").fill("js/index.js add");
    const result = page.getByRole("option").first();
    await result.waitFor({ timeout: 10_000 });
    await result.click();
    await page.waitForSelector(".inspector code", { timeout: 10_000 });
    const initialZoom = zoomControl(page).locator('input[type="range"]');
    await initialZoom.fill("300");
    const zoomedActions = page.locator(".graph-node-actions");
    await zoomedActions.waitFor({ timeout: 10_000 });
    if (Number(await zoomedActions.getAttribute("data-rendered-inset")) < 14) {
      throw new Error("node controls do not clear the scaled selected border");
    }
    const actionBackground = await zoomedActions.locator("button").first()
      .evaluate((button) => getComputedStyle(button).backgroundColor);
    if (actionBackground !== "rgb(134, 239, 172)") {
      throw new Error(`node controls do not inherit the symbol accent: ${actionBackground}`);
    }
    const actionRadius = await zoomedActions.locator("button").first()
      .evaluate((button) => getComputedStyle(button).borderRadius);
    if (actionRadius !== "50%") {
      throw new Error(`node controls are not circular: ${actionRadius}`);
    }
    const actionSize = await zoomedActions.locator("button").first().boundingBox();
    if (!actionSize || Math.abs(actionSize.width - actionSize.height) > 0.1) {
      throw new Error(
        `node controls are oval: ${actionSize?.width || 0}x${actionSize?.height || 0}`,
      );
    }
    await page.screenshot({
      path: path.join(path.dirname(screenshot), "node-controls-zoomed.png"),
      fullPage: true,
    });
    await initialZoom.fill("125");
    let nodePosition = await selectedGraphPoint(page);
    await page.mouse.move(nodePosition.clientX, nodePosition.clientY);
    await page.mouse.down();
    await page.mouse.move(
      nodePosition.clientX + 48,
      nodePosition.clientY + 32,
      { steps: 6 },
    );
    await page.locator('.graph-canvas[data-dragging-node="true"]').waitFor();
    if (await page.locator(".graph-node-actions").count()
        || await page.locator(".graph-tooltip").count()) {
      throw new Error("node overlays remained visible at their pre-drag position");
    }
    await page.mouse.up();
    await page.locator('.graph-canvas[data-dragging-node="false"]').waitFor();
    nodePosition = await selectedGraphPoint(page);
    const edgeCount = await page.locator(".graph-canvas").getAttribute("data-edge-count");
    await page.getByRole("button", { name: "Remove node from view", exact: true }).click();
    await page.locator('.graph-canvas[data-hidden-node-count="1"][data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    if (await page.locator(".graph-canvas").getAttribute("data-edge-count") !== edgeCount) {
      throw new Error("hiding a node removed graph topology and changed its layout");
    }
    if (Number(await page.locator(".graph-canvas").getAttribute("data-hidden-edge-count")) < 1) {
      throw new Error("hiding a connected node did not mute its incident relations");
    }
    await page.mouse.dblclick(nodePosition.clientX, nodePosition.clientY, { delay: 70 });
    await page.locator('.graph-canvas[data-hidden-node-count="0"][data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    const restoredPosition = await selectedGraphPoint(page);
    if (Math.hypot(
      restoredPosition.x - nodePosition.x,
      restoredPosition.y - nodePosition.y,
    ) > 1) {
      throw new Error("restoring a hidden node moved it from its presentation position");
    }
    await page.mouse.move(nodePosition.clientX, nodePosition.clientY);
    await page.locator(".graph-canvas").focus();
    await page.keyboard.press("Escape");
    await page.locator(".graph-node-actions").waitFor({ timeout: 10_000 });
    await page.screenshot({
      path: path.join(path.dirname(screenshot), "node-controls.png"),
      fullPage: true,
    });
    await page.keyboard.press("ArrowDown");
    await page.waitForSelector(".inspector code", { timeout: 10_000 });
    await page.locator(".graph-canvas").focus();
    await page.keyboard.press("Escape");
    const canvas = page.locator(".graph-canvas");
    if (await canvas.getAttribute("data-layout-mode") !== "flow"
      || await canvas.getAttribute("data-notation") !== "architecture") {
      throw new Error("graph did not start in Flow/Architecture presentation");
    }
    for (const layout of ["force", "radial", "flow"]) {
      await page.getByLabel("Layout").selectOption(layout);
      await page.locator(
        `.graph-canvas[data-layout-mode="${layout}"][data-layout-ready="true"]`,
      ).waitFor({ timeout: 30_000 });
    }
    await page.getByLabel("Notation").selectOption("compact");
    await page.locator(
      '.graph-canvas[data-notation="compact"][data-layout-ready="true"]',
    ).waitFor({ timeout: 30_000 });
    await page.getByLabel("Notation").selectOption("architecture");
    await page.locator(
      '.graph-canvas[data-notation="architecture"][data-layout-ready="true"]',
    ).waitFor({ timeout: 30_000 });
    await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });

    await page.getByText("Focused Query", { exact: true }).click();
    const mapViewport = await page.locator(".graph-canvas").evaluate((canvas) => ({
      panX: canvas.getAttribute("data-pan-x"),
      panY: canvas.getAttribute("data-pan-y"),
      selectedId: canvas.getAttribute("data-selected-id"),
    }));
    await page.locator(".query-panel input").first().fill("js/index.js:add");
    await page.getByRole("button", { name: "Run Query", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
      undefined,
      { timeout: 30_000 },
    );
    const graphBack = page.getByRole("button", { name: "Back to previous graph", exact: true });
    if (await page.getByRole("button", { name: "Back to Map", exact: true }).count()) {
      throw new Error("focused graph duplicates graph-local Back with a rail Back control");
    }
    const [backBox, graphBox] = await Promise.all([
      graphBack.boundingBox(),
      page.locator(".graph-stage").boundingBox(),
    ]);
    if (!backBox || !graphBox || backBox.x < graphBox.x || backBox.y < graphBox.y
      || backBox.x > graphBox.x + graphBox.width / 2
      || backBox.y > graphBox.y + graphBox.height / 2) {
      throw new Error(`graph Back control is not in the graph's upper-left: ${JSON.stringify({ backBox, graphBox })}`);
    }
    await graphBack.click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await page.waitForFunction((expected) => {
      const canvas = document.querySelector(".graph-canvas");
      return canvas?.getAttribute("data-pan-x") === expected.panX
        && canvas?.getAttribute("data-pan-y") === expected.panY
        && canvas?.getAttribute("data-selected-id") === expected.selectedId;
    }, mapViewport, { timeout: 30_000 });
    await assertDesktopWorkspaceVisible(page, "Query to Map navigation");
    await changeMapAxis(page, "component", "group-by", "component");
    await selectGraphResult(page, "javascript");
    await page.locator(".inspector h2").getByText("javascript", { exact: true }).waitFor();
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
    await page.getByRole("button", { name: "Maximize node", exact: true }).click();
    await assertAnchoredRelayout(page);
    await clickAndWaitForGraphLayout(
      page,
      page.getByRole("button", { name: "Collapse all", exact: true }),
    );
    await zoomSlider.fill("150");
    await zoomControl(page).getByText("150%", { exact: true }).waitFor();

    await loadArtifact(page, verification, "verification");
    await page.waitForSelector(".verification-editor", { timeout: 10_000 });
    await page.getByLabel("Owner").fill("architecture");
    await page.getByLabel("Rationale").fill("Temporary browser-test waiver.");
    await page.getByLabel("Expires on").fill("2026-08-31");
    const waiver = path.join(path.dirname(screenshot), "waiver.json");
    await saveDownload(page, "Save waiver candidate", waiver);
    if (!JSON.parse(fs.readFileSync(waiver, "utf8")).fingerprint) {
      throw new Error("waiver download lacks a finding fingerprint");
    }

    await loadArtifact(page, projectConfiguration, "project-configuration");
    await page.waitForSelector(".verification-editor", { timeout: 10_000 });
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
    await page.locator(".metrics-panel").waitFor({ timeout: 30_000 });
    if (await page.locator(".metric-accordion").count() !== 7) {
      throw new Error("Metrics sidebar does not expose all seven interactive metrics");
    }
    const complexity = page.locator(
      '[data-metric="source.cyclomatic_complexity"]',
    );
    if (await complexity.locator(":scope > .metric-box-plot").count()) {
      throw new Error("unopened metric eagerly loaded its distribution");
    }
    await complexity.locator(".metric-summary").click();
    await complexity.locator(".metric-distribution").waitFor();
    await complexity.locator(".metric-summary").click();
    if (await complexity.locator(":scope > .metric-box-plot").count() !== 1) {
      throw new Error("collapsed metric does not expose its quartile plot");
    }
    if (await complexity.locator(".metric-bar-fill").count()) {
      throw new Error("metric retained the misleading median/maximum ratio bar");
    }
    await complexity.locator(".metric-summary").click();
    await complexity.locator(".metric-distribution").waitFor();
    if (await complexity.locator(".metric-box-plot").count() !== 1) {
      throw new Error("expanded metric duplicated its quartile plot");
    }
    await complexity.getByText("Measurements are not quality scores.", { exact: true })
      .waitFor();
    await complexity.getByRole("button", { name: "Show on graph", exact: true }).click();
    await complexity.getByRole("button", { name: "Remove graph overlay", exact: true })
      .waitFor();
    const tailPoint = page.locator("button.metric-plot-point").first();
    if (await tailPoint.count()) {
      const pointLabel = await tailPoint.getAttribute("aria-label");
      if (!pointLabel || !pointLabel.includes(":")) {
        throw new Error(`metric tail point lacks source identity: ${pointLabel}`);
      }
      await tailPoint.click();
      await page.waitForFunction(
        () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
        undefined,
        { timeout: 30_000 },
      );
      await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
      await page.waitForFunction(
        () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
        undefined,
        { timeout: 30_000 },
      );
    }
    const metricHotspot = complexity.locator(".metric-hotspot").first();
    await metricHotspot.click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("query ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.getByRole("button", { name: "Back to previous graph", exact: true }).click();
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("map ·"),
      undefined,
      { timeout: 30_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    await changeMapAxis(page, "component", "group-by", "component");
    const firstConstraint = page.locator(".constraint-row").first();
    await firstConstraint.click();
    await page.getByText("Architecture constraint", { exact: true }).waitFor();
    await selectGraphResult(page, "javascript");
    await page.locator(".metric-scope")
      .getByRole("button", { name: "Selection", exact: true }).click();
    if (await complexity.locator(".metric-summary-copy small").count()) {
      throw new Error("metric row repeats the shared header scope");
    }
    if (!await page.locator(".metric-scope")
      .getByRole("button", { name: "Selection", exact: true })
      .evaluate((button) => button.classList.contains("active"))) {
      throw new Error("Metrics header did not retain selected-region scope");
    }
    if (await firstConstraint.evaluate((row) => row.classList.contains("active"))) {
      throw new Error("graph selection left the previous constraint selected");
    }
    await activateSelected(page, "Expand member files");
    const expandedComponentNodes = Number(
      await page.locator(".graph-canvas").getAttribute("data-node-count"),
    );
    await page.getByRole("button", { name: "Remove node from view", exact: true }).click();
    await page.locator('.graph-canvas[data-hidden-node-count="1"][data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    const hiddenComponentNodes = Number(
      await page.locator(".graph-canvas").getAttribute("data-node-count"),
    );
    if (hiddenComponentNodes >= expandedComponentNodes) {
      throw new Error(
        `hiding expanded component did not collapse it: ${expandedComponentNodes} -> ${hiddenComponentNodes}`,
      );
    }
    if (await page.locator(".graph-canvas").getAttribute("data-hidden-edge-opacity") !== "0") {
      throw new Error("hidden incident relations are not fully transparent");
    }
    await page.getByRole("button", { name: "Restore node", exact: true }).click();
    await page.locator('.graph-canvas[data-hidden-node-count="0"][data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
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
    if (await page.locator('.graph-canvas[data-group-by="layer"]').count() !== 1) {
      throw new Error("zero-config layer grouping was not rendered");
    }
    await changeMapAxis(page, "directory", "group-by", "directory");
    await selectGraphResult(page, "js/index.js");
    await selectGraphResult(page, "add");
    await page.locator(".inspector h2").getByText("add", { exact: true }).waitFor();

    await page.setInputFiles("input[webkitdirectory]", manifestRoundDirectory);
    await page.waitForFunction(
      () => document.querySelector(".artifact-meta small")?.textContent?.includes("browser-live"),
      undefined,
      { timeout: 60_000 },
    );
    await page.locator('.graph-canvas[data-layout-ready="true"]')
      .waitFor({ timeout: 30_000 });
    const manifestRoundMap = path.join(
      path.dirname(screenshot),
      "manifest-round-map.json",
    );
    await saveDownload(page, "Save canonical artifact", manifestRoundMap);
    const manifestRoundDocument = JSON.parse(
      fs.readFileSync(manifestRoundMap, "utf8"),
    );
    const manifestRoundPackages = manifestRoundDocument.packages
      .map((row) => row.identity)
      .sort();
    if (
      manifestRoundDocument.project !== "browser-workspace"
      || JSON.stringify(manifestRoundPackages)
        !== JSON.stringify(["@browser/member", "browser-workspace"])
    ) {
      throw new Error(
        `browser manifest rounds lost package evidence: ${JSON.stringify({
          packages: manifestRoundPackages,
          project: manifestRoundDocument.project,
        })}`,
      );
    }

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
    await loadArtifact(mobile, map, "map");
    await mobile.locator(".metrics-panel").waitFor({ timeout: 30_000 });
    if (await mobile.locator(".metric-accordion").count() !== 7) {
      throw new Error("mobile Metrics sidebar does not expose all seven metrics");
    }
    const mobileComplexity = mobile.locator('[data-metric="source.cyclomatic_complexity"]');
    await mobileComplexity.locator(".metric-summary").click();
    await mobileComplexity.locator(".metric-facts").waitFor();
    await selectGraphResult(mobile, "js/");
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
  } catch (error) {
    if (diagnostics) await diagnostics.retain(error);
    throw error;
  } finally {
    if (diagnostics) await diagnostics.close();
    if (browser) await browser.close();
    await new Promise((resolve) => server.close(resolve));
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
