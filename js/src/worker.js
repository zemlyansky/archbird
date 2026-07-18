"use strict";

const { createBrowserArchbird } = require("./browser");

let runtimePromise;

function runtime() {
  if (!runtimePromise) runtimePromise = createBrowserArchbird();
  return runtimePromise;
}

async function handleRequest(request) {
  if (!request || request.method !== "map") {
    throw new Error("Archbird Worker supports method=map");
  }
  const archbird = await runtime();
  const project = new archbird.Project(request.config, request.sources, {
    typescript: request.typescript !== false,
  });
  try {
    if (request.format === "markdown") {
      return project.mapMarkdown({
        view: request.view || "overview",
        detail: request.detail || "standard",
        compact: Boolean(request.compact),
        full: Boolean(request.full),
        maxChars: request.maxChars || 0,
      }).toString("utf8");
    }
    return project.mapJson({ pretty: Boolean(request.pretty) }).toString("utf8");
  } finally {
    project.dispose();
  }
}

if (typeof globalThis.addEventListener === "function" && typeof globalThis.postMessage === "function") {
  globalThis.addEventListener("message", async (event) => {
    const id = event.data?.id;
    try {
      globalThis.postMessage({ id, ok: true, result: await handleRequest(event.data) });
    } catch (error) {
      globalThis.postMessage({ id, ok: false, error: String(error.message || error) });
    }
  });
}

module.exports = { handleRequest };
