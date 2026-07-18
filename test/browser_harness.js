"use strict";

const fs = require("node:fs");
const http = require("node:http");
const path = require("node:path");

const mime = new Map([
  [".css", "text/css; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json; charset=utf-8"],
  [".map", "application/json; charset=utf-8"],
  [".svg", "image/svg+xml"],
  [".wasm", "application/wasm"],
]);

function executableExists(candidate) {
  try {
    fs.accessSync(candidate, fs.constants.X_OK);
    return true;
  } catch (_) {
    return false;
  }
}

function loadChromium() {
  const moduleName = process.env.ARCHBIRD_PLAYWRIGHT || "playwright";
  try {
    return require(moduleName).chromium;
  } catch (error) {
    throw new Error(
      `cannot load Playwright (${moduleName}); set ARCHBIRD_PLAYWRIGHT to a shared installation: ${error.message}`,
    );
  }
}

function sharedChromium(chromium) {
  const explicit = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
  if (explicit) {
    if (!executableExists(explicit)) {
      throw new Error(`configured Chromium executable is unavailable: ${explicit}`);
    }
    return explicit;
  }

  for (const candidate of [
    chromium.executablePath(),
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ]) {
    if (candidate && executableExists(candidate)) return candidate;
  }

  const cacheRoot = process.env.PLAYWRIGHT_BROWSERS_PATH || "/opt/ms-playwright";
  try {
    const candidates = fs.readdirSync(cacheRoot)
      .sort()
      .reverse()
      .flatMap((entry) => [
        path.join(cacheRoot, entry, "chrome-linux64", "chrome"),
        path.join(cacheRoot, entry, "chrome-headless-shell-linux64", "chrome-headless-shell"),
      ]);
    for (const candidate of candidates) {
      if (executableExists(candidate)) return candidate;
    }
  } catch (_) {
    // The shared cache is optional; the error below describes supported routes.
  }

  throw new Error(
    "no existing Chromium executable found; set PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH " +
      "or provide a shared Playwright browser cache",
  );
}

function createStaticServer(rootValue) {
  const root = path.resolve(rootValue);
  return http.createServer((request, response) => {
    const requestPath = decodeURIComponent((request.url || "/").split("?", 1)[0]);
    const relative = requestPath === "/" ? "index.html" : requestPath.replace(/^\/+/, "");
    const candidate = path.resolve(root, relative);
    if (candidate !== root && !candidate.startsWith(`${root}${path.sep}`)) {
      response.writeHead(403);
      response.end("forbidden");
      return;
    }
    fs.readFile(candidate, (error, data) => {
      if (error) {
        response.writeHead(404);
        response.end("not found");
        return;
      }
      response.writeHead(200, {
        "Content-Type": mime.get(path.extname(candidate)) || "application/octet-stream",
      });
      response.end(data);
    });
  });
}

function browserEnvironment(rootValue) {
  const explicit = process.env.ARCHBIRD_BROWSER_TMP;
  const base = path.resolve(
    explicit || path.join(path.dirname(path.resolve(rootValue)), ".browser-tmp"),
  );
  if (!explicit) fs.rmSync(base, { recursive: true, force: true });
  fs.mkdirSync(base, { recursive: true });
  return { ...process.env, TEMP: base, TMP: base, TMPDIR: base };
}

async function listen(server) {
  await new Promise((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, "127.0.0.1", resolve);
  });
  return `http://127.0.0.1:${server.address().port}/`;
}

module.exports = {
  browserEnvironment,
  createStaticServer,
  listen,
  loadChromium,
  sharedChromium,
};
