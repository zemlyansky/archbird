#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const { createHash } = require("node:crypto");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { TextDecoder } = require("node:util");
const archbird = require("./index");
const native = require("./native");
const {
  applyAcceptedAct,
  observePlanSources,
  actOverlay,
  gateFailureDetails,
  renderAct,
  runActGates,
} = require("./act-transport");
const {
  MAX_ACT_BYTES,
  MAX_OPERATION_TEXT_BYTES,
  MAX_PLAN_BYTES,
} = require("./plan-limits");

const COMMANDS = new Set([
  "act",
  "apply",
  "config",
  "diff",
  "export",
  "freshness",
  "impact",
  "map",
  "observe",
  "path",
  "plan",
  "query",
  "serve",
  "support",
  "verify",
  "workspace",
]);

const COMMON = {
  config: { aliases: ["c"], type: "string" },
  root: { type: "string" },
  output: { aliases: ["o"], default: "-", type: "string" },
  format: { type: "string" },
  pretty: { type: "boolean" },
  check: { type: "boolean" },
  help: { aliases: ["h"], type: "boolean" },
  version: { type: "boolean" },
  noTypescript: { flag: "no-typescript", type: "boolean" },
  cacheDir: { flag: "cache-dir", type: "string" },
  cacheMaxBytes: { flag: "cache-max-bytes", type: "number" },
  noCache: { flag: "no-cache", type: "boolean" },
};

const DISCOVERY = {
  ...COMMON,
  noConfig: { flag: "no-config", type: "boolean" },
  project: { type: "string" },
  source: { type: "multiple" },
  only: { type: "multiple" },
  exclude: { type: "multiple" },
  ignoreFile: { flag: "ignore-file", type: "multiple" },
  noIgnore: { flag: "no-ignore", type: "boolean" },
  noDefaultExcludes: { flag: "no-default-excludes", type: "boolean" },
  maxFileBytes: { flag: "max-file-bytes", type: "number" },
  maxIndexBytes: { flag: "max-index-bytes", type: "number" },
  progress: { default: "auto", type: "string" },
};

function usage(command = "map") {
  const rows = {
    map: "archbird map [ROOT] [--config PROJECT.json] [--view overview|architecture|tests|evidence|source] [--dump] [--group-by KIND] [--level KIND] [--relations KINDS] [--detail compact|standard|full] [--progress auto|always|never] [--format markdown|json] [--check]",
    observe: "archbird observe [ROOT] --map MAP.json --request COVERAGE.json [--output OBSERVATIONS.json]",
    query: "archbird query [QUERY|ROOT] [--root PROJECT | --map MAP.json] [SELECTORS] [--view focused|changes|source] [--detail compact|standard|full] [--dump] [--check]",
    impact: "archbird impact [QUERY|ROOT] [--root PROJECT | --map MAP.json] [SELECTORS] [--view focused|changes|source] [--detail compact|standard|full] [--dump] [--check]",
    path: "archbird path SOURCE TARGET [--root PROJECT | --map MAP.json] [--level component|file|symbol] [--relation FAMILY] [--direction downstream|upstream|both] [--format markdown|json] [--check]",
    config: "archbird config show|init [ROOT] [--config PROJECT.json]",
    diff: "archbird diff --before OLD.json --after NEW.json [--check[=CATEGORIES]]",
    freshness: "archbird freshness [ROOT] --snapshot MAP_OR_QUERY.json [--config PROJECT.json] [--check]",
    workspace: "archbird workspace --config WORKSPACE.json [--check]",
    verify: "archbird verify [CONSTRAINT ...] [--root PROJECT | --map MAP.json] [--config archbird.json] [--baseline FILE | --freeze FILE] [--format markdown|json|sarif|junit] [--check]",
    plan: "archbird plan [ROOT|CONSTRAINT ...] [--root PROJECT | --map MAP.json] [--before-map OLD.json | --git-diff REVISION] [--config archbird.json] [--objective TEXT] [--rename OLD=NEW] [--redirect FROM=TO] [--format json|markdown] [--output PLAN.json]",
    act: "archbird act PLAN.json [--root PROJECT] [--submit ITEM=FILE] [--format markdown|json|patch] [--output ACT.json]",
    apply: "archbird apply ACT.json [--root PROJECT]",
    export: "archbird export graphml|json|mermaid --map MAP_OR_QUERY.json [--output FILE]",
    serve: "archbird serve [--root PROJECT] [--config PROJECT.json] [--host 127.0.0.1] [--port 4177]",
    support: "archbird support",
  };
  const selectorHelp = ["query", "impact"].includes(command)
    ? "\n--symbol accepts PATTERN or repository-relative PATH:PATTERN; repeated selectors form a union.\n" +
      "--search KEYWORDS ranks advisory lexical seeds from repository vocabulary; it does not interpret questions.\n" +
      "--git-diff REVISION seeds tracked current paths and retains deletions as change evidence.\n" +
      "--view source renders hash-checked selected source; --dump renders every selected file in full.\n" +
      "Context profiles exact|change|architecture|audit control Markdown; " +
      "--max-chars is only the final guard.\n"
    : "";
  return `${rows[command]}\n${selectorHelp}\nMap → Query → Verify → Plan → Act with deterministic native/Wasm evidence.\n`;
}

function topLevelUsage() {
  return (
    "usage: archbird COMMAND [OPTIONS]\n" +
    "       archbird [ROOT] [MAP OPTIONS]\n\n" +
    "Map codebases, query evidence, verify architecture, and apply reviewed Plans.\n\n" +
    "commands:\n" +
    "  map, config, query, impact, path, diff, observe, freshness, workspace\n" +
    "  verify, plan, act, apply, export, serve, support\n\n" +
    "Run `archbird COMMAND --help` for command-specific options. With no command, " +
    "Archbird maps the current directory; an existing or path-shaped positional " +
    "argument is the Map root.\n"
  );
}

function camel(flag) {
  return flag.replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
}

function parse(argv, definitions, { positionals = 0 } = {}) {
  const byFlag = new Map();
  const result = { _: [] };
  for (const [name, definition] of Object.entries(definitions)) {
    const flags = [definition.flag || name.replace(/[A-Z]/g, (value) => `-${value.toLowerCase()}`)];
    for (const alias of definition.aliases || []) flags.push(alias);
    for (const flag of flags) byFlag.set(flag, [name, definition]);
    if (definition.default !== undefined) result[name] = definition.default;
    else if (definition.type === "multiple") result[name] = [];
    else if (definition.type === "boolean") result[name] = false;
  }
  for (let index = 0; index < argv.length; index += 1) {
    const raw = argv[index];
    if (raw === "--") {
      result._.push(...argv.slice(index + 1));
      break;
    }
    if (!raw.startsWith("-") || raw === "-") {
      result._.push(raw);
      continue;
    }
    const long = raw.startsWith("--");
    const body = raw.slice(long ? 2 : 1);
    const equal = body.indexOf("=");
    const flag = equal < 0 ? body : body.slice(0, equal);
    const inline = equal < 0 ? null : body.slice(equal + 1);
    const entry = byFlag.get(flag);
    if (!entry) throw new Error(`unknown option: ${raw}`);
    const [name, definition] = entry;
    if (definition.type === "boolean") {
      if (inline !== null) throw new Error(`${raw}: boolean option takes no value`);
      result[name] = true;
      continue;
    }
    if (definition.type === "optional") {
      if (inline !== null) result[name] = inline;
      else if (argv[index + 1] && !argv[index + 1].startsWith("-")) result[name] = argv[++index];
      else result[name] = definition.const;
      continue;
    }
    const value = inline !== null ? inline : argv[++index];
    if (value === undefined) throw new Error(`${raw}: expected a value`);
    if (definition.type === "multiple") result[name].push(value);
    else if (definition.type === "number") {
      result[name] = Number(value);
      if (!Number.isSafeInteger(result[name]) || result[name] < 0) {
        throw new Error(`${raw}: expected a nonnegative safe integer`);
      }
    } else result[name] = value;
  }
  if (result._.length > positionals) throw new Error(`unexpected argument: ${result._[positionals]}`);
  return result;
}

function required(options, ...names) {
  for (const name of names) {
    if (options[name] === undefined || options[name] === "") {
      throw new Error(`--${name.replace(/[A-Z]/g, (value) => `-${value.toLowerCase()}`)} is required`);
    }
  }
}

function read(name) {
  return fs.readFileSync(path.resolve(name));
}

function readBounded(name, maximum, description) {
  const resolved = path.resolve(name);
  const before = fs.lstatSync(resolved);
  if (!before.isFile()) throw new Error(`${description} is not a regular file`);
  const descriptor = fs.openSync(
    resolved,
    fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0),
  );
  try {
    const metadata = fs.fstatSync(descriptor);
    if (!metadata.isFile()) throw new Error(`${description} is not a regular file`);
    if (metadata.dev !== before.dev || metadata.ino !== before.ino) {
      throw new Error(`${description} changed while it was opened`);
    }
    if (metadata.size > maximum) {
      throw new Error(`${description} exceeds ${maximum} bytes`);
    }
    const value = fs.readFileSync(descriptor);
    if (value.length > maximum) {
      throw new Error(`${description} exceeds ${maximum} bytes`);
    }
    return value;
  } finally {
    fs.closeSync(descriptor);
  }
}

function actExecutorSubmissions(values) {
  const seen = new Set();
  const files = [];
  const items = values.map((value) => {
    const separator = value.indexOf("=");
    if (separator <= 0 || separator === value.length - 1) {
      throw new Error("--submit expects ITEM=FILE");
    }
    const itemId = value.slice(0, separator);
    if (seen.has(itemId)) {
      throw new Error(`--submit repeats Plan item ${itemId}`);
    }
    const file = path.resolve(value.slice(separator + 1));
    const content = readBounded(
      file,
      MAX_OPERATION_TEXT_BYTES,
      "executor submission",
    );
    seen.add(itemId);
    files.push(file);
    return {
      content_base64: content.toString("base64"),
      item_id: itemId,
      kind: "write_file",
    };
  });
  if (!items.length) {
    return { json: Buffer.alloc(0), files };
  }
  items.sort((left, right) =>
    Buffer.compare(Buffer.from(left.item_id), Buffer.from(right.item_id))
  );
  return {
    json: native.jsonCanonicalize(Buffer.from(JSON.stringify({ items }))),
    files,
  };
}

function write(value, output = "-") {
  const bytes = Buffer.from(value);
  const encoded = bytes.length && bytes[bytes.length - 1] === 10
    ? bytes
    : Buffer.concat([bytes, Buffer.from("\n")]);
  if (output === "-") process.stdout.write(encoded);
  else fs.writeFileSync(path.resolve(output), encoded);
}

function repositoryArtifactPath(repository, locator) {
  if (locator === "-") return null;
  const relative = path.relative(
    repository,
    path.resolve(locator),
  ).split(path.sep).join("/");
  if (!relative || relative === "." || relative === ".." ||
      relative.startsWith("../")) {
    return null;
  }
  return relative;
}

function hasErrors(document) {
  return (document.diagnostics || []).some((row) => row.severity === "error");
}

class Progress {
  constructor(mode) {
    if (!["auto", "always", "never"].includes(mode)) {
      throw new Error("--progress must be auto, always, or never");
    }
    this.mode = mode;
    this.interactive = mode === "auto" && process.stderr.isTTY;
    this.enabled = mode === "always" || this.interactive;
    this.started = process.hrtime.bigint();
    this.lastUpdate = this.started;
    this.lastMessage = "";
    this.lastWidth = 0;
    this.visible = false;
  }

  emit(event) {
    if (!this.enabled) return;
    const now = process.hrtime.bigint();
    const elapsed = Number(now - this.started) / 1e9;
    const phase = String(event.phase || "working");
    const state = String(event.state || "");
    const provider = String(event.provider || "");
    let detail;
    if (phase === "providers") {
      if (event.total === 0) return;
      if (
        state === "progress" && event.completed !== event.total &&
        Number(now - this.lastUpdate) / 1e9 < 1
      ) return;
      if (Number.isSafeInteger(event.completed) && Number.isSafeInteger(event.total)) {
        const percent = event.total ? Math.round(event.completed * 100 / event.total) : 100;
        detail = `${provider} ${event.completed}/${event.total} files (${percent}%)`;
      } else if (state === "start") detail = `${provider} started`;
      else detail = `${provider} complete`;
    } else if (phase === "discovery") detail = "scanning repository";
    else if (phase === "selected") detail = `${event.files || 0} files`;
    else if (phase === "joining") {
      detail = state === "start" ? "merging normalized facts" : "fact graph ready";
    } else if (phase === "rendering") detail = String(event.artifact || "output");
    else if (phase === "complete") detail = "done";
    else detail = state || "working";
    const message = `archbird [${elapsed.toFixed(1)}s] ${phase}: ${detail}`;
    if (message === this.lastMessage) return;
    if (this.interactive) {
      if (elapsed < 0.75) {
        this.lastMessage = message;
        return;
      }
      if (
        this.visible && Number(now - this.lastUpdate) / 1e9 < 0.2 &&
        !["complete", "rendering"].includes(phase)
      ) {
        this.lastMessage = message;
        return;
      }
      const padding = " ".repeat(Math.max(0, this.lastWidth - message.length));
      process.stderr.write(`\r${message}${padding}`);
      this.lastWidth = message.length;
      this.visible = true;
    } else process.stderr.write(`${message}\n`);
    this.lastMessage = message;
    this.lastUpdate = now;
  }

  finish() {
    if (!this.enabled) return;
    if (this.interactive) {
      if (this.visible) {
        const elapsed = Number(process.hrtime.bigint() - this.started) / 1e9;
        const message = `archbird [${elapsed.toFixed(1)}s] complete`;
        const padding = " ".repeat(Math.max(0, this.lastWidth - message.length));
        process.stderr.write(`\r${message}${padding}\n`);
        this.visible = false;
      }
      return;
    }
    this.emit({ phase: "complete" });
  }

  clear() {
    if (this.interactive && this.visible) {
      process.stderr.write(`\r${" ".repeat(this.lastWidth)}\r`);
      this.visible = false;
    }
  }
}

function validateProjectConfiguration(configJson) {
  let document;
  try {
    document = JSON.parse(configJson.toString("utf8"));
  } catch (error) {
    throw new Error(`invalid project configuration JSON: ${error.message}`);
  }
  if (!document || Array.isArray(document) || typeof document !== "object") {
    throw new Error("project configuration must be an object");
  }
  if (Object.hasOwn(document, "root")) {
    throw new Error("archbird.json does not allow root; use --root");
  }
}

function repositoryInputs(options) {
  if (options.config && options.noConfig) {
    throw new Error("--config and --no-config are mutually exclusive");
  }
  const positional = options._[0] ? path.resolve(options._[0]) : null;
  const override = options.root ? path.resolve(options.root) : null;
  if (positional && override && positional !== override) {
    throw new Error("positional ROOT and --root select different directories");
  }
  let repository = positional || override || path.resolve(".");
  let configPath = null;
  let configJson = Buffer.alloc(0);
  if (options.config) {
    if (options.config === "-") {
      configJson = fs.readFileSync(0);
    }
    else {
      configPath = path.resolve(options.config);
      configJson = fs.readFileSync(configPath);
    }
    validateProjectConfiguration(configJson);
  } else if (!options.noConfig) {
    const candidates = ["archbird.json"]
      .map((name) => path.join(repository, name))
      .filter((candidate) => {
        try {
          const metadata = fs.lstatSync(candidate);
          return metadata.isFile() && !metadata.isSymbolicLink();
        } catch (error) {
          if (error && error.code === "ENOENT") return false;
          throw error;
        }
      });
    if (candidates.length) {
      configPath = candidates[0];
      configJson = fs.readFileSync(configPath);
      validateProjectConfiguration(configJson);
    }
  }
  if (!fs.statSync(repository).isDirectory()) {
    throw new Error(`root is not a directory: ${repository}`);
  }
  return { repository, configJson, configPath };
}

function hasDiscoveryOverrides(options) {
  return Boolean(
    options.noConfig || options.project || options.source.length || options.only.length ||
    options.exclude.length || options.ignoreFile.length || options.noIgnore ||
    options.noDefaultExcludes || options.maxFileBytes !== undefined ||
    options.maxIndexBytes !== undefined || options.cacheDir ||
    options.cacheMaxBytes !== undefined || options.noCache
  );
}

function cacheMaxBytes(options) {
  const value = options.cacheMaxBytes === undefined
    ? archbird.defaultProviderCacheMaxBytes()
    : options.cacheMaxBytes;
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error("--cache-max-bytes must be a positive safe integer");
  }
  return value;
}

function cacheFailureContext(stats, options) {
  const cachePath = path.resolve(
    options.cacheDir || archbird.defaultProviderCacheDir(),
  );
  let probe = cachePath;
  while (!fs.existsSync(probe) && path.dirname(probe) !== probe) {
    probe = path.dirname(probe);
  }
  let free = "unavailable";
  try {
    const filesystem = fs.statfsSync(probe);
    free = String(filesystem.bavail * filesystem.bsize);
  } catch (_) {
    // A cache warning must survive an unavailable filesystem-stat call.
  }
  return [
    `path=${cachePath}`,
    `quota=${cacheMaxBytes(options)} bytes`,
    `attempted=${stats.attemptedBytes || 0} bytes`,
    `errno=${stats.errorErrno || 0}`,
    `free=${free} bytes`,
  ].join("; ");
}

function warnCacheStats(stats, options) {
  if (!stats.noSpace && !stats.skipped) return;
  const context = cacheFailureContext(stats, options);
  if (stats.noSpace) {
    process.stderr.write(
      "archbird: warning: provider-cache write failed because storage is full; " +
      `${context}; analysis remains valid. Use --cache-dir, increase ` +
      "--cache-max-bytes, " +
      "or use --no-cache.\n",
    );
  }
  if (stats.skipped) {
    process.stderr.write(
      "archbird: warning: provider-cache entries exceeded the configured " +
      `budget and were not stored; ${context}; analysis remains valid. Increase ` +
      "--cache-max-bytes or use --no-cache.\n",
    );
  }
}

function warnMapCacheStats(stats, options) {
  if (!stats.noSpace && !stats.skipped) return;
  const context = cacheFailureContext(stats, options);
  if (stats.noSpace) {
    process.stderr.write(
      "archbird: warning: canonical Map cache write failed because storage " +
      `is full; ${context}; analysis remains valid.\n`,
    );
  }
  if (stats.skipped) {
    process.stderr.write(
      "archbird: warning: canonical Map exceeded the configured cache budget " +
      `and was not stored; ${context}; analysis remains valid.\n`,
    );
  }
}

function discoverProject(options, progress = null, resolvedInputs = null) {
  if (progress !== null) progress.emit({ phase: "discovery", state: "start" });
  const { repository, configJson } = resolvedInputs || repositoryInputs(options);
  const current = archbird.Project.fromRepository(repository, {
    config: configJson.length ? configJson : null,
    project: options.project || null,
    source: options.source,
    only: options.only,
    exclude: options.exclude,
    ignore: !options.noIgnore,
    ignoreFiles: options.ignoreFile,
    defaultExcludes: !options.noDefaultExcludes,
    maxFileBytes: options.maxFileBytes ?? null,
    maxIndexBytes: options.maxIndexBytes ?? null,
    _transientExclude: options._transientExclude || [],
    scan: false,
    typescript: !options.noTypescript,
  });
  if (progress !== null) {
    progress.emit({ phase: "selected", files: current.sources.length });
  }
  return current;
}

function project(options, progress = null, resolvedInputs = null) {
  const current = discoverProject(options, progress, resolvedInputs);
  if (options.mergeLedger && path.resolve(options.mergeLedger) === path.resolve(options.output)) {
    throw new Error("--merge-ledger and --output must be different paths");
  }
  try {
    current.scan("primary", {
      cacheDir: options.noCache
        ? null
        : (options.cacheDir || archbird.defaultProviderCacheDir()),
      cacheMaxBytes: cacheMaxBytes(options),
      typescript: !options.noTypescript,
      progress: progress === null ? null : (event) => progress.emit(event),
      mapCache: !(options.testSymbolObservations || []).length,
    });
  } catch (error) {
    if (options.mergeLedger) {
      write(current.mergeConflictsJson({ pretty: true }), options.mergeLedger);
    }
    throw error;
  }
  warnCacheStats(current.cacheStats, options);
  if (options.mergeLedger) {
    write(current.mergeConflictsJson({ pretty: true }), options.mergeLedger);
  }
  for (const observationPath of options.testSymbolObservations || []) {
    current.addTestSymbolObservations(read(observationPath));
  }
  return current;
}

function configForSavedMap(options, purpose) {
  let configJson;
  if (options.config === "-") configJson = fs.readFileSync(0);
  else if (options.config) configJson = read(options.config);
  else {
    const candidate = path.join(path.resolve("."), "archbird.json");
    if (!fs.existsSync(candidate)) {
      throw new Error(`${purpose} requires archbird.json or --config with --map`);
    }
    configJson = fs.readFileSync(candidate);
  }
  validateProjectConfiguration(configJson);
  return configJson;
}

function namedDocuments(values, option) {
  const result = {};
  for (const value of values) {
    const split = value.indexOf("=");
    if (split <= 0 || split === value.length - 1) {
      throw new Error(`${option}: expected ID=PATH, got ${JSON.stringify(value)}`);
    }
    const id = value.slice(0, split);
    if (Object.hasOwn(result, id)) throw new Error(`${option}: duplicate id ${JSON.stringify(id)}`);
    const document = JSON.parse(read(value.slice(split + 1)).toString("utf8"));
    if (!document || Array.isArray(document) || typeof document !== "object") {
      throw new Error(`${option}: ${value.slice(split + 1)} must contain a JSON object`);
    }
    result[id] = document;
  }
  return result;
}

function constraintRequest(
  options,
  configJson,
  { baselinePath = options.baseline || null } = {},
) {
  const observations = namedDocuments(
    options.observation || [],
    "--observation",
  );
  const mapDocuments = namedDocuments(options.mapInput || [], "--map-input");
  const resolutionDocuments = namedDocuments(
    options.resolutionInput || [],
    "--resolution-input",
  );
  const unmatched = Object.keys(resolutionDocuments)
    .filter((id) => !Object.hasOwn(mapDocuments, id));
  if (unmatched.length) {
    throw new Error(
      `--resolution-input has no matching --map-input: ${unmatched.sort().join(", ")}`,
    );
  }
  const maps = {};
  for (const [id, map] of Object.entries(mapDocuments)) {
    maps[id] = { map };
    if (Object.hasOwn(resolutionDocuments, id)) {
      maps[id].resolution = resolutionDocuments[id];
    }
  }
  const configDocument = JSON.parse(configJson.toString("utf8"));
  const configuredConstraints = Array.isArray(configDocument.constraints)
    ? configDocument.constraints
    : Object.values(configDocument.constraints || {});
  const hasExpiringWaiver = configuredConstraints.some((constraint) =>
    constraint && Array.isArray(constraint.waivers) &&
    constraint.waivers.some((waiver) => waiver && waiver.expires_on));
  const policyDate = options.policyDate ||
    (hasExpiringWaiver ? new Date().toISOString().slice(0, 10) : null);
  const request = {};
  if (baselinePath) {
    request.baseline = JSON.parse(read(baselinePath).toString("utf8"));
  }
  if (Object.keys(maps).length) request.maps = maps;
  if (Object.keys(observations).length) request.observations = observations;
  if (policyDate !== null) request.policy_date = policyDate;
  return request;
}

function constraintContext(
  options,
  progress,
  {
    baselinePath = options.baseline || null,
    resolvedInputs = null,
  } = {},
) {
  if (options.noConfig) {
    throw new Error(
      "Verify and Plan require constraints from archbird.json; " +
      "--no-config is not supported",
    );
  }
  const inputs = resolvedInputs || repositoryInputs(options);
  const { repository, configJson } = inputs;
  if (!configJson.length) {
    throw new Error(
      `no archbird.json found in ${repository}; ` +
      "Verify and Plan require reviewed constraints",
    );
  }
  if (options.resolution && !options.map) {
    throw new Error("--resolution requires --map");
  }
  let mapJson;
  let resolutionJson;
  let current;
  if (options.map) {
    mapJson = read(options.map);
    resolutionJson = options.resolution
      ? read(options.resolution)
      : Buffer.alloc(0);
    current = discoverProject(options, progress, inputs);
  } else {
    current = project(options, progress, inputs);
    mapJson = current.mapJson();
    resolutionJson = current.resolutionJson || Buffer.alloc(0);
    warnMapCacheStats(current.mapCacheStats, options);
  }
  return {
    repository,
    configJson,
    mapJson,
    resolutionJson,
    project: current,
    request: constraintRequest(options, configJson, { baselinePath }),
  };
}

function mapMain(argv) {
  const options = parse(argv, {
    ...DISCOVERY,
    format: { default: "markdown", type: "string" },
    view: { default: "overview", type: "string" },
    groupBy: { flag: "group-by", default: "", type: "string" },
    level: { default: "", type: "string" },
    relations: { type: "multiple" },
    overlay: { type: "multiple" },
    detail: { default: "standard", type: "string" },
    compact: { type: "boolean" },
    full: { type: "boolean" },
    dump: { type: "boolean" },
    maxChars: { flag: "max-chars", default: 0, type: "number" },
    mergeLedger: { flag: "merge-ledger", type: "string" },
    testSymbolObservations: { flag: "test-symbol-observations", type: "multiple" },
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage("map"));
    return 0;
  }
  if (options.version) {
    process.stdout.write(`${archbird.VERSION}\n`);
    return 0;
  }
  if (options.dump) {
    if (!["overview", "source"].includes(options.view)) {
      throw new Error("--dump conflicts with a non-source --view");
    }
    if (options.compact || options.detail !== "standard") {
      throw new Error("--dump conflicts with compact/detail options");
    }
    options.view = "source";
    options.full = true;
  }
  if (!["json", "markdown"].includes(options.format)) throw new Error("--format must be json or markdown");
  if (!["overview", "architecture", "tests", "evidence", "source"].includes(options.view)) {
    throw new Error("--view must be overview, architecture, tests, evidence, or source");
  }
  if (!["compact", "standard", "full"].includes(options.detail)) {
    throw new Error("--detail must be compact, standard, or full");
  }
  if (options.compact && options.full) throw new Error("--compact and --full conflict");
  if ((options.compact || options.full) && options.detail !== "standard") {
    throw new Error("--detail conflicts with --compact/--full");
  }
  if (options.format === "json" && (
    options.compact || options.full || options.dump || options.maxChars ||
    options.detail !== "standard" || options.view !== "overview" ||
    options.groupBy || options.level || options.relations.length ||
    options.overlay.length
  )) {
    throw new Error("--view, graph projection axes, and detail options apply only to Markdown");
  }
  if (options.format === "markdown" && options.pretty) {
    throw new Error("--pretty applies only to JSON");
  }
  if (options.view === "source" && (
    options.groupBy || options.level || options.relations.length ||
    options.overlay.length
  )) {
    throw new Error(
      "source view does not accept graph grouping, level, relations, or overlays",
    );
  }
  if (options.view === "source" && options.full && options.maxChars) {
    throw new Error("full source view cannot be combined with --max-chars");
  }
  const progress = new Progress(options.progress);
  const current = project(options, progress);
  progress.emit({ phase: "rendering", artifact: "canonical Map" });
  const mapJson = current.mapJson({ pretty: options.pretty && options.format === "json" });
  warnMapCacheStats(current.mapCacheStats, options);
  const output = options.format === "json"
    ? mapJson
    : options.view === "source"
      ? current.sourceMarkdown({
        artifactJson: mapJson,
        detail: options.detail,
        compact: options.compact,
        full: options.full,
        maxChars: options.maxChars,
      })
      : current.mapMarkdown({
      view: options.view,
      detail: options.detail,
      compact: options.compact,
      full: options.full,
      groupBy: options.groupBy,
      level: options.level,
      maxChars: options.maxChars,
      overlays: options.overlay.length
        ? options.overlay.flatMap((value) =>
          value.split(",").map((part) => part.trim()).filter(Boolean))
        : undefined,
      relations: options.relations.length
        ? options.relations.flatMap((value) =>
          value.split(",").map((part) => part.trim()).filter(Boolean))
        : undefined,
      resolutionJson: current.resolutionJson || Buffer.alloc(0),
    });
  progress.finish();
  write(output, options.output);
  return options.check && hasErrors(JSON.parse(mapJson)) ? 1 : 0;
}

function selectorDefinitions() {
  return {
    ...DISCOVERY,
    map: { type: "string" },
    focus: { type: "multiple" },
    path: { type: "multiple" },
    symbol: { type: "multiple" },
    component: { type: "multiple" },
    package: { type: "multiple" },
    artifact: { type: "multiple" },
    search: { type: "multiple" },
    searchLimit: { flag: "search-limit", default: 8, type: "number" },
    gitDiff: { flag: "git-diff", type: "string" },
    verificationResult: { flag: "verification-result", type: "string" },
    direction: { type: "string" },
    depth: { default: 1, type: "number" },
    testDepth: { flag: "test-depth", default: 8, type: "number" },
    contextProfile: { flag: "context-profile", type: "string" },
    routeProvenance: { flag: "route-provenance", type: "multiple" },
    routeConfidence: { flag: "route-confidence", type: "multiple" },
    maxSeedDistance: { flag: "max-seed-distance", type: "number" },
    candidate: { type: "string" },
    conservative: { type: "string" },
    contextQuota: { flag: "context-quota", type: "multiple" },
    contextOffset: { flag: "context-offset", type: "multiple" },
    view: { type: "string" },
    detail: { default: "standard", type: "string" },
    compact: { type: "boolean" },
    full: { type: "boolean" },
    dump: { type: "boolean" },
    maxChars: { flag: "max-chars", default: 0, type: "number" },
    testSymbolObservations: { flag: "test-symbol-observations", type: "multiple" },
    format: { default: "markdown", type: "string" },
  };
}

function pathDefinitions() {
  return {
    ...DISCOVERY,
    map: { type: "string" },
    resolution: { type: "string" },
    level: { default: "file", type: "string" },
    sourceKind: { flag: "source-kind", type: "string" },
    targetKind: { flag: "target-kind", type: "string" },
    relation: { type: "multiple" },
    direction: { default: "downstream", type: "string" },
    maxDepth: { flag: "max-depth", default: 8, type: "number" },
    maxPaths: { flag: "max-paths", default: 8, type: "number" },
    maxChars: { flag: "max-chars", default: 0, type: "number" },
    testSymbolObservations: {
      flag: "test-symbol-observations",
      type: "multiple",
    },
    format: { default: "markdown", type: "string" },
  };
}

const GIT_CHANGE_STATUS = Object.freeze({
  A: "added",
  B: "broken-pair",
  C: "copied",
  D: "deleted",
  M: "modified",
  R: "renamed",
  T: "type-changed",
  U: "unmerged",
  X: "unknown",
});

function decodeGitStatus(value) {
  if ([...value].some((byte) => byte > 0x7f)) {
    throw new Error("git diff emitted a non-ASCII status");
  }
  return value.toString("ascii");
}

function gitChangeSet(repository, revision) {
  if (
    !revision || revision !== revision.trim() || revision.startsWith("-") ||
    revision.includes("\0") || revision.includes("\n") || revision.includes("\r")
  ) {
    throw new Error("--git-diff requires one safe Git revision or range");
  }
  const completed = spawnSync(
    "git",
    [
      "-C", repository, "diff", "--no-ext-diff", "--no-textconv",
      "--name-status", "-z", "--find-renames", revision, "--",
    ],
    {
      encoding: null,
      env: { ...process.env, GIT_OPTIONAL_LOCKS: "0" },
      maxBuffer: 64 * 1024 * 1024,
      windowsHide: true,
    },
  );
  if (completed.error) throw new Error(`cannot run git diff: ${completed.error.message}`);
  if (completed.status !== 0) {
    const detail = Buffer.from(completed.stderr || []).toString("utf8").trim();
    throw new Error(`git diff failed for ${JSON.stringify(revision)}: ${detail}`);
  }
  const output = Buffer.from(completed.stdout || []);
  const fields = [];
  let start = 0;
  for (let index = 0; index < output.length; index += 1) {
    if (output[index] !== 0) continue;
    fields.push(output.subarray(start, index));
    start = index + 1;
  }
  if (start !== output.length) throw new Error("git diff emitted unterminated name-status evidence");
  const decoder = new TextDecoder("utf-8", { fatal: true });
  const entries = [];
  for (let index = 0; index < fields.length;) {
    const rawStatus = decodeGitStatus(fields[index++]);
    const code = rawStatus.slice(0, 1);
    const status = GIT_CHANGE_STATUS[code];
    const pathCount = ["C", "R"].includes(code) ? 2 : 1;
    if (!status || index + pathCount > fields.length) {
      throw new Error("git diff emitted malformed name-status evidence");
    }
    let paths;
    try {
      paths = fields.slice(index, index + pathCount).map((value) => decoder.decode(value));
    } catch (error) {
      throw new Error("git diff path is not UTF-8 and cannot enter canonical evidence");
    }
    index += pathCount;
    const entry = { path: paths.at(-1), status };
    if (pathCount === 2) entry.previous_path = paths[0];
    entries.push(entry);
  }
  if (!entries.length) throw new Error(`git diff ${JSON.stringify(revision)} contains no changed paths`);
  entries.sort((left, right) => {
    for (const key of ["path", "status", "previous_path"]) {
      const compared = Buffer.compare(
        Buffer.from(left[key] || "", "utf8"),
        Buffer.from(right[key] || "", "utf8"),
      );
      if (compared) return compared;
    }
    return 0;
  });
  for (let index = 1; index < entries.length; index += 1) {
    if (JSON.stringify(entries[index - 1]) === JSON.stringify(entries[index])) {
      throw new Error("git diff emitted duplicate change entries");
    }
  }
  return {
    entries,
    source: { identity: revision, kind: "git-diff" },
  };
}

function gitCommand(repository, arguments_, description) {
  const completed = spawnSync(
    "git",
    ["-C", repository, ...arguments_],
    {
      encoding: null,
      env: { ...process.env, GIT_OPTIONAL_LOCKS: "0" },
      maxBuffer: 64 * 1024 * 1024,
      windowsHide: true,
    },
  );
  if (completed.error) {
    throw new Error(`${description} failed: ${completed.error.message}`);
  }
  if (completed.status !== 0) {
    const detail = Buffer.from(completed.stderr || []).toString("utf8").trim();
    throw new Error(`${description} failed: ${detail}`);
  }
  return Buffer.from(completed.stdout || []);
}

function gitWriteBlob(repository, objectId, target, relative) {
  const descriptor = fs.openSync(target, "wx");
  let completed;
  try {
    completed = spawnSync(
      "git",
      ["-C", repository, "cat-file", "blob", objectId],
      {
        env: { ...process.env, GIT_OPTIONAL_LOCKS: "0" },
        stdio: ["ignore", descriptor, "pipe"],
        windowsHide: true,
      },
    );
  } finally {
    fs.closeSync(descriptor);
  }
  if (completed.error) {
    throw new Error(
      `read Git blob for ${JSON.stringify(relative)} failed: ` +
        completed.error.message,
    );
  }
  if (completed.status !== 0) {
    const detail = Buffer.from(completed.stderr || []).toString("utf8").trim();
    throw new Error(
      `read Git blob for ${JSON.stringify(relative)} failed: ${detail}`,
    );
  }
}

function gitCommit(repository, revision) {
  if (
    !revision || revision !== revision.trim() || revision.startsWith("-") ||
    revision.includes("\0") || revision.includes("\n") || revision.includes("\r")
  ) {
    throw new Error("--git-diff requires one safe Git commit");
  }
  const commit = gitCommand(
    repository,
    ["rev-parse", "--verify", `${revision}^{commit}`],
    `git revision ${JSON.stringify(revision)}`,
  ).toString("ascii").trim();
  if (!/^(?:[0-9a-f]{40}|[0-9a-f]{64})$/.test(commit)) {
    throw new Error("git rev-parse emitted an invalid commit object id");
  }
  return commit;
}

function gitProjectPrefix(repository) {
  const prefix = gitCommand(
    repository,
    ["rev-parse", "--show-prefix"],
    "locate project root within Git repository",
  ).toString("utf8").replace(/\n$/, "");
  const parts = prefix ? prefix.slice(0, -1).split("/") : [];
  if (
    prefix && (
      !prefix.endsWith("/") || prefix.startsWith("/") ||
      prefix.includes("\\") ||
      parts.some((part) => ["", ".", ".."].includes(part))
    )
  ) {
    throw new Error("git rev-parse emitted an unsafe project prefix");
  }
  return prefix;
}

function gitMaterializeTree(
  repository,
  commit,
  destination,
  { projectPrefix = "", depth = 0 } = {},
) {
  if (depth > 32) {
    throw new Error("Git submodule nesting exceeds 32 levels");
  }
  const inventoryArguments = ["ls-tree", "-rz", "--full-tree", commit];
  if (projectPrefix) {
    inventoryArguments.push("--", `:(literal)${projectPrefix.slice(0, -1)}`);
  }
  const inventory = gitCommand(
    repository,
    inventoryArguments,
    `Git tree inventory for ${commit}`,
  );
  if (inventory.length && inventory.at(-1) !== 0) {
    throw new Error("git ls-tree emitted an unterminated record");
  }
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let start = 0;
  for (let index = 0; index < inventory.length; index += 1) {
    if (inventory[index] !== 0) continue;
    const record = inventory.subarray(start, index);
    start = index + 1;
    if (!record.length) continue;
    const tab = record.indexOf(9);
    const metadata = tab < 0
      ? []
      : record.subarray(0, tab).toString("ascii").split(" ");
    if (tab < 0 || metadata.length !== 3) {
      throw new Error("git ls-tree emitted a malformed record");
    }
    const [mode, objectType, objectId] = metadata;
    let committedPath;
    try {
      committedPath = decoder.decode(record.subarray(tab + 1));
    } catch (_) {
      throw new Error("Git snapshot paths must be UTF-8");
    }
    if (!committedPath.startsWith(projectPrefix)) {
      throw new Error("Git tree entry is outside the project root");
    }
    const relative = committedPath.slice(projectPrefix.length);
    const parts = relative.split("/");
    if (
      !relative || relative.startsWith("/") || relative.includes("\\") ||
      parts.some((part) => ["", ".", ".."].includes(part))
    ) {
      throw new Error(
        `Git snapshot contains unsafe path: ${JSON.stringify(relative)}`,
      );
    }
    if (!/^(?:[0-9a-f]{40}|[0-9a-f]{64})$/.test(objectId)) {
      throw new Error("git ls-tree emitted an invalid object id");
    }
    const target = path.join(destination, ...parts);
    if (objectType === "commit") {
      const localSubmodule = path.join(repository, ...parts);
      const gitMarker = path.join(localSubmodule, ".git");
      let available = false;
      try {
        const directory = fs.lstatSync(localSubmodule);
        const marker = fs.lstatSync(gitMarker);
        available = directory.isDirectory() &&
          !directory.isSymbolicLink() && !marker.isSymbolicLink();
      } catch (error) {
        if (!error || error.code !== "ENOENT") throw error;
      }
      if (!available) continue;
      gitCommand(
        localSubmodule,
        ["cat-file", "-e", `${objectId}^{commit}`],
        `resolve Git submodule ${JSON.stringify(relative)}`,
      );
      fs.mkdirSync(target, { recursive: true });
      gitMaterializeTree(localSubmodule, objectId, target, {
        depth: depth + 1,
      });
      continue;
    }
    if (mode === "120000") continue;
    if (objectType !== "blob" || !["100644", "100755"].includes(mode)) {
      throw new Error(
        `Git snapshot contains unsupported entry: ${JSON.stringify(relative)}`,
      );
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    gitWriteBlob(repository, objectId, target, relative);
    fs.chmodSync(target, mode === "100755" ? 0o755 : 0o644);
  }
}

function withGitSnapshot(repository, revision, callback) {
  const commit = gitCommit(repository, revision);
  const projectPrefix = gitProjectPrefix(repository);
  const temporaryRoot = path.join(
    archbird.defaultProviderCacheDir(),
    "temporary-snapshots",
  );
  fs.mkdirSync(temporaryRoot, { recursive: true });
  const parent = fs.mkdtempSync(path.join(temporaryRoot, "git-"));
  const snapshot = path.join(parent, path.basename(repository) || "repository");
  try {
    fs.mkdirSync(snapshot);
    gitMaterializeTree(repository, commit, snapshot, { projectPrefix });
    return callback(snapshot);
  } finally {
    fs.rmSync(parent, { force: true, recursive: true });
  }
}

function contextCounts(values, option) {
  const allowed = new Set([
    "files",
    "symbol_calls",
    "symbol_references",
    "test_matches",
  ]);
  const result = {};
  for (const value of values) {
    const split = value.indexOf("=");
    const kind = split < 0 ? "" : value.slice(0, split);
    const raw = split < 0 ? "" : value.slice(split + 1);
    if (!allowed.has(kind) || !/^[0-9]+$/.test(raw)) {
      throw new Error(
        `${option} expects KIND=N for files, symbol_calls, ` +
        "symbol_references, or test_matches",
      );
    }
    if (Object.hasOwn(result, kind)) throw new Error(`${option} repeats ${kind}`);
    result[kind] = Number(raw);
    if (!Number.isSafeInteger(result[kind])) {
      throw new Error(`${option} value exceeds the JavaScript safe integer range`);
    }
  }
  return result;
}

function queryPositionalIsRoot(value) {
  return Boolean(
    value &&
    (value === "." || value === ".." || path.isAbsolute(value) ||
      value.includes("/") || value.includes("\\")),
  );
}

function mapShortcut(argv) {
  if (!argv.length || argv[0].startsWith("-")) return true;
  if (queryPositionalIsRoot(argv[0])) return true;
  try {
    return fs.statSync(path.resolve(argv[0])).isDirectory();
  } catch (error) {
    if (error && error.code === "ENOENT") return false;
    throw error;
  }
}

function sourceSnapshotProject(artifactJson, root) {
  const artifact = JSON.parse(Buffer.from(artifactJson).toString("utf8"));
  if (!artifact || Array.isArray(artifact) ||
      !["map", "query"].includes(artifact.artifact)) {
    throw new Error("source view requires a canonical Map or Query");
  }
  if (typeof artifact.project !== "string" || !artifact.project ||
      !Array.isArray(artifact.files)) {
    throw new Error("source artifact identity or file inventory is invalid");
  }
  const repository = fs.realpathSync(path.resolve(root));
  const sources = [];
  const seen = new Set();
  for (const row of artifact.files) {
    const sourcePath = row?.path;
    if (typeof sourcePath !== "string" || seen.has(sourcePath)) {
      throw new Error("source selection contains an invalid or duplicate path");
    }
    if (typeof row.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(row.sha256)) {
      throw new Error(
        `source artifact has an invalid digest for ${JSON.stringify(sourcePath)}`,
      );
    }
    let cursor = repository;
    for (const part of sourcePath.split("/")) {
      cursor = path.join(cursor, part);
      if (fs.lstatSync(cursor).isSymbolicLink()) {
        throw new Error(`source path traverses a symlink: ${sourcePath}`);
      }
    }
    const resolved = fs.realpathSync(path.join(repository, ...sourcePath.split("/")));
    if (resolved !== repository &&
        !resolved.startsWith(`${repository}${path.sep}`)) {
      throw new Error(`source path escapes repository root: ${sourcePath}`);
    }
    if (!fs.statSync(resolved).isFile()) {
      throw new Error(`source path is not a regular file: ${sourcePath}`);
    }
    const data = fs.readFileSync(resolved);
    const actual = createHash("sha256").update(data).digest("hex");
    if (actual !== row.sha256) {
      throw new Error(
        `source bytes changed since artifact creation: ${sourcePath} ` +
        `(artifact ${row.sha256}, live ${actual})`,
      );
    }
    sources.push(new archbird.Source(sourcePath, data, {
      language: row.language || "",
      layer: row.layer || "",
    }));
    seen.add(sourcePath);
  }
  return new archbird.Project(artifact.project, sources);
}

function queryMain(argv, command) {
  const options = parse(argv, selectorDefinitions(), { positionals: 1 });
  const positional = options._[0] || null;
  const positionalRoot = queryPositionalIsRoot(positional);
  const queryId = positionalRoot ? null : positional;
  const repositoryOptions = {
    ...options,
    _: positionalRoot ? [positional] : [],
  };
  if (options.help) {
    process.stdout.write(usage(command));
    return 0;
  }
  if (options.dump) {
    if (options.view && !["focused", "source"].includes(options.view)) {
      throw new Error("--dump conflicts with a non-source --view");
    }
    if (options.compact || options.detail !== "standard") {
      throw new Error("--dump conflicts with compact/detail options");
    }
    options.view = "source";
    options.full = true;
  }
  if (options.view && !["focused", "changes", "source"].includes(options.view)) {
    throw new Error("--view must be focused, changes, or source");
  }
  if (options.map && (
    positionalRoot || options.noConfig ||
    (options.root && options.view !== "source") ||
    hasDiscoveryOverrides(options)
  )) {
    throw new Error("--map cannot be combined with repository discovery options");
  }
  if (options.map && options.testSymbolObservations.length) {
    throw new Error("--test-symbol-observations requires a live repository, not --map");
  }
  if (options.map && options.gitDiff) {
    throw new Error("--git-diff requires a live repository, not --map");
  }
  if (options.resolution && !options.map) {
    throw new Error("--resolution requires --map");
  }
  if (options.verificationResult &&
      (options.format !== "markdown" || (options.view || "focused") !== "changes")) {
    throw new Error(
      "--verification-result requires --format markdown --view changes",
    );
  }
  if (options.maxSeedDistance !== undefined && options.maxSeedDistance < 0) {
    throw new Error("--max-seed-distance must be nonnegative");
  }
  if (!Number.isSafeInteger(options.searchLimit) ||
      options.searchLimit < 1 || options.searchLimit > 100) {
    throw new Error("--search-limit must be from 1 to 100");
  }
  if (options.compact && options.full) {
    throw new Error("--compact and --full conflict");
  }
  if ((options.compact || options.full) && options.detail !== "standard") {
    throw new Error("--detail conflicts with --compact/--full");
  }
  if (options.format === "json" && (
    options.compact || options.full || options.dump || options.maxChars ||
    options.detail !== "standard" || (options.view && options.view !== "focused")
  )) {
    throw new Error("--view and detail options apply only to Markdown");
  }
  if (options.format === "markdown" && options.pretty) {
    throw new Error("--pretty applies only to JSON");
  }
  if (options.view === "source" && options.full && options.maxChars) {
    throw new Error("full source view cannot be combined with --max-chars");
  }
  const progress = new Progress(options.progress);
  let source;
  let current = null;
  let configJson = Buffer.alloc(0);
  let resolutionJson = Buffer.alloc(0);
  let changeSet = null;
  if (options.map) {
    source = read(options.map);
    if (options.resolution) resolutionJson = read(options.resolution);
    if (queryId || options.config) configJson = configForSavedMap(options, "named query");
  }
  else {
    const resolvedInputs = repositoryInputs(repositoryOptions);
    if (options.gitDiff) {
      changeSet = gitChangeSet(resolvedInputs.repository, options.gitDiff);
    }
    ({ configJson } = resolvedInputs);
    current = project(repositoryOptions, progress, resolvedInputs);
    progress.emit({ phase: "rendering", artifact: "canonical Map" });
    source = current.mapJson();
    resolutionJson = current.resolutionJson || Buffer.alloc(0);
    warnMapCacheStats(current.mapCacheStats, options);
  }
  const sourceDocument = JSON.parse(source);
  if (options.check && options.map) {
    const producer = sourceDocument.tool?.implementation_sha256;
    if (producer !== archbird.IMPLEMENTATION_SHA256) {
      process.stderr.write(
        `archbird: check failed: saved Map core ${producer || "missing"} ` +
        `does not match active core ${archbird.IMPLEMENTATION_SHA256}\n`,
      );
      return 1;
    }
  }
  let queryOptions = {
    artifacts: options.artifact,
    components: options.component,
    changeSet,
    depth: options.depth,
    direction: options.direction || (command === "impact" ? "upstream" : "both"),
    focus: options.focus,
    packages: options.package,
    paths: options.path,
    producerPolicy: options.check && options.map ? "current" : "compatible",
    resolutionJson,
    search: options.search,
    searchLimit: options.searchLimit,
    symbols: options.symbol,
    testDepth: options.testDepth,
  };
  const context = {};
  if (options.contextProfile) context.profile = options.contextProfile;
  if (options.routeProvenance.length) context.provenance = options.routeProvenance;
  if (options.routeConfidence.length) context.confidence = options.routeConfidence;
  if (options.maxSeedDistance !== undefined) {
    context.max_seed_distance = options.maxSeedDistance;
  }
  if (options.candidate) context.candidate = options.candidate;
  if (options.conservative) context.conservative = options.conservative;
  const quotas = contextCounts(options.contextQuota, "--context-quota");
  const offsets = contextCounts(options.contextOffset, "--context-offset");
  if (Object.keys(quotas).length) context.quotas = quotas;
  if (Object.keys(offsets).length) context.offsets = offsets;
  if (Object.keys(context).length) queryOptions.context = context;
  if (queryId) {
    if (!configJson.length) {
      throw new Error(`named query ${JSON.stringify(queryId)} requires archbird.json`);
    }
    const overrides = {};
    for (const [name, value] of [
      ["focus", options.focus], ["paths", options.path],
      ["symbols", options.symbol], ["components", options.component],
      ["packages", options.package], ["artifacts", options.artifact],
      ["search", options.search],
    ]) {
      if (value.length) overrides[name] = value;
    }
    if (options.searchLimit !== 8) overrides.search_limit = options.searchLimit;
    if (options.direction) overrides.direction = options.direction;
    if (options.depth !== 1) overrides.depth = options.depth;
    if (options.testDepth !== 8) overrides.test_depth = options.testDepth;
    if (Object.keys(context).length) overrides.context = context;
    const artifact = JSON.parse(archbird.compileQueryPlan(
      configJson,
      queryId,
      {
        overridesJson: Buffer.from(JSON.stringify(overrides)),
      },
    ).toString("utf8"));
    queryOptions = {
      plan: artifact.plan,
      producerPolicy: options.check && options.map ? "current" : "compatible",
      resolutionJson,
      changeSet,
    };
  }
  try {
    if (options.format === "json") {
      progress.finish();
      write(archbird.queryMap(source, { ...queryOptions, pretty: options.pretty }), options.output);
    } else if (options.format === "markdown" && options.view === "source") {
      const queryJson = archbird.queryMap(source, queryOptions);
      const sourceProject =
        current || sourceSnapshotProject(queryJson, options.root || ".");
      progress.finish();
      write(sourceProject.sourceMarkdown({
        artifactJson: queryJson,
        compact: options.compact,
        detail: options.detail,
        full: options.full,
        maxChars: options.maxChars,
      }), options.output);
    } else if (options.format === "markdown") {
      progress.finish();
      write(archbird.queryMapMarkdown(source, {
        ...queryOptions,
        compact: options.compact,
        detail: options.detail,
        full: options.full,
        maxChars: options.maxChars,
        verificationResult: options.verificationResult
          ? read(options.verificationResult)
          : Buffer.alloc(0),
        view: options.view || "focused",
      }), options.output);
    } else throw new Error("--format must be json or markdown");
  } catch (error) {
    if (options.check && error?.code === "ARCHBIRD_STATUS_10") {
      progress.clear();
      process.stderr.write(`archbird: check failed: ${error.message}\n`);
      return 1;
    }
    throw error;
  }
  return options.check && hasErrors(sourceDocument) ? 1 : 0;
}

function pathMain(argv) {
  const options = parse(argv, pathDefinitions(), { positionals: 2 });
  if (options.help) {
    process.stdout.write(usage("path"));
    return 0;
  }
  const [sourcePattern, targetPattern] = options._;
  if (!sourcePattern || !targetPattern) {
    throw new Error("path requires SOURCE and TARGET endpoint patterns");
  }
  if (!["component", "file", "symbol"].includes(options.level)) {
    throw new Error("--level must be component, file, or symbol");
  }
  if (!["downstream", "upstream", "both"].includes(options.direction)) {
    throw new Error("--direction must be downstream, upstream, or both");
  }
  const relationFamilies = new Set([
    "bridges", "builds", "calls", "declarations",
    "imports", "packages", "references", "tests",
  ]);
  if (options.relation.some((value) => !relationFamilies.has(value))) {
    throw new Error("--relation contains an unsupported family");
  }
  if (!Number.isSafeInteger(options.maxDepth) ||
      options.maxDepth < 0 || options.maxDepth > 64) {
    throw new Error("--max-depth must be from 0 to 64");
  }
  if (!Number.isSafeInteger(options.maxPaths) ||
      options.maxPaths < 1 || options.maxPaths > 100) {
    throw new Error("--max-paths must be from 1 to 100");
  }
  if (!Number.isSafeInteger(options.maxChars) || options.maxChars < 0) {
    throw new Error("--max-chars must be a nonnegative integer");
  }
  if (!["json", "markdown"].includes(options.format)) {
    throw new Error("--format must be json or markdown");
  }
  if (options.format === "json" && options.maxChars) {
    throw new Error("--max-chars applies only to Markdown");
  }
  if (options.format === "markdown" && options.pretty) {
    throw new Error("--pretty applies only to JSON");
  }
  if (options.resolution && !options.map) {
    throw new Error("--resolution requires --map");
  }
  if (options.map && (
    options.config || options.root || options.noConfig ||
    hasDiscoveryOverrides(options)
  )) {
    throw new Error("--map cannot be combined with repository discovery options");
  }
  if (options.map && options.testSymbolObservations.length) {
    throw new Error("--test-symbol-observations requires a live repository, not --map");
  }
  const progress = new Progress(options.progress);
  let mapJson;
  let resolutionJson = Buffer.alloc(0);
  if (options.map) {
    mapJson = read(options.map);
    if (options.resolution) resolutionJson = read(options.resolution);
  } else {
    const repositoryOptions = { ...options, _: [] };
    const resolvedInputs = repositoryInputs(repositoryOptions);
    const current = project(repositoryOptions, progress, resolvedInputs);
    progress.emit({ phase: "rendering", artifact: "canonical Map" });
    mapJson = current.mapJson();
    resolutionJson = current.resolutionJson || Buffer.alloc(0);
    warnMapCacheStats(current.mapCacheStats, options);
  }
  const mapDocument = JSON.parse(mapJson);
  if (options.check && hasErrors(mapDocument)) return 1;
  const pathOptions = {
    source: {
      kind: options.sourceKind || options.level,
      patterns: [sourcePattern],
    },
    target: {
      kind: options.targetKind || options.level,
      patterns: [targetPattern],
    },
    level: options.level,
    relations: options.relation.length ? options.relation : null,
    direction: options.direction,
    maxDepth: options.maxDepth,
    maxPaths: options.maxPaths,
    producerPolicy: options.check && options.map ? "current" : "compatible",
    resolutionJson,
  };
  let pathJson;
  try {
    pathJson = archbird.pathMap(mapJson, {
      ...pathOptions,
      pretty: options.pretty,
    });
  } catch (error) {
    if (options.check && error?.code === "ARCHBIRD_STATUS_10") {
      progress.clear();
      process.stderr.write(`archbird: check failed: ${error.message}\n`);
      return 1;
    }
    throw error;
  }
  const artifact = JSON.parse(pathJson);
  if (options.check && artifact.outcome !== "found") {
    progress.clear();
    process.stderr.write(
      `archbird: check failed: connection path outcome is ${artifact.outcome}\n`,
    );
    return 1;
  }
  const encoded = options.format === "json"
    ? pathJson
    : archbird.renderPathMarkdown(pathJson, {
      maxChars: options.maxChars,
    });
  progress.finish();
  write(encoded, options.output);
  return 0;
}

function configMain(argv) {
  if (["-h", "--help", "help"].includes(argv[0])) {
    process.stdout.write(usage("config"));
    return 0;
  }
  const command = argv[0];
  if (!["show", "init"].includes(command)) {
    throw new Error("archbird config requires show or init");
  }
  const options = parse(argv.slice(1), {
    ...DISCOVERY,
    output: {
      aliases: ["o"],
      default: command === "show" ? "-" : "archbird.json",
      type: "string",
    },
    format: { default: "json", type: "string" },
    force: { type: "boolean" },
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage("config"));
    return 0;
  }
  if (options.format !== "json") throw new Error("--format must be json");
  const { repository, configJson } = repositoryInputs(options);
  const resolutionJson = archbird.resolveDiscovery(repository, {
    config: configJson.length ? configJson : null,
    project: options.project || null,
    source: options.source,
    only: options.only,
    exclude: options.exclude,
    ignore: !options.noIgnore,
    ignoreFiles: options.ignoreFile,
    defaultExcludes: !options.noDefaultExcludes,
    maxFileBytes: options.maxFileBytes ?? null,
    maxIndexBytes: options.maxIndexBytes ?? null,
    pretty: options.pretty,
  });
  const resolution = JSON.parse(resolutionJson.toString("utf8"));
  if (command === "show") write(resolutionJson, options.output);
  else {
    const output = options.output;
    if (output !== "-" && fs.existsSync(path.resolve(output)) && !options.force) {
      throw new Error(`refusing to replace existing configuration: ${output}`);
    }
    const encoded = Buffer.from(`${JSON.stringify(resolution.effective_config, null, 2)}\n`);
    write(encoded, output);
  }
  return options.check && hasErrors(resolution) ? 1 : 0;
}

const DIFF_POLICIES = {
  "public-api": [["public_symbols", "removed_changed"], ["package_exports", "removed_changed"], ["package_export_origins", "removed_changed"], ["package_entrypoint_surfaces", "removed_changed"], ["entrypoints", "removed_changed"]],
  bridges: [["bridges", "any"], ["bridge_surfaces", "any"]],
  calls: [["call_resolutions", "any"], ["symbol_calls", "any"], ["symbol_references", "any"]],
  parity: [["parity_gaps", "added_changed"]],
  tests: [["test_route_evidence", "any"], ["test_routes", "removed_changed"]],
  architecture: [["artifacts", "any"], ["build_routes", "any"], ["component_routes", "any"], ["package_dependencies", "any"]],
};

function diffSectionMatches(section, policy) {
  if (!section || Array.isArray(section) || typeof section !== "object") {
    throw new Error("native diff section is invalid");
  }
  const added = Boolean(section.added?.length);
  const changed = Boolean(section.changed?.length);
  const removed = Boolean(section.removed?.length);
  if (policy === "any") return added || changed || removed;
  if (policy === "removed_changed") return removed || changed;
  if (policy === "added_changed") return added || changed;
  throw new Error(`unknown diff risk policy: ${policy}`);
}

function diffRisk(document, raw) {
  if (
    !document ||
    Array.isArray(document) ||
    typeof document !== "object" ||
    !document.sections ||
    Array.isArray(document.sections) ||
    typeof document.sections !== "object"
  ) {
    throw new Error("native diff result has no sections");
  }
  const categories = [...new Set(raw.split(",")
    .map((value) => value.trim())
    .filter(Boolean))].sort();
  const unknown = categories.filter(
    (category) => category !== "all" && !DIFF_POLICIES[category],
  );
  if (unknown.length) {
    throw new Error(`diff.check: unknown categories: ${unknown.join(", ")}`);
  }
  if (categories.includes("all")) {
    return Object.values(document.sections)
      .some((section) => diffSectionMatches(section, "any"));
  }
  for (const category of categories) {
    for (const [name, policy] of DIFF_POLICIES[category]) {
      const section = document.sections[name];
      if (!section) throw new Error(`native diff result has no ${name} section`);
      if (diffSectionMatches(section, policy)) return true;
    }
  }
  return false;
}

function diffMain(argv) {
  const options = parse(argv, {
    before: { type: "string" }, after: { type: "string" },
    output: COMMON.output, pretty: COMMON.pretty,
    check: { const: "public-api,bridges,parity,tests,architecture", type: "optional" },
    help: COMMON.help,
  });
  if (options.help) { process.stdout.write(usage("diff")); return 0; }
  required(options, "before", "after");
  const encoded = archbird.diffMaps(read(options.before), read(options.after), { pretty: options.pretty });
  write(encoded, options.output);
  return options.check && diffRisk(JSON.parse(encoded), options.check) ? 1 : 0;
}

function observeMain(argv) {
  const options = parse(argv, {
    map: { type: "string" },
    request: { type: "string" },
    output: { aliases: ["o"], default: "-", type: "string" },
    help: { aliases: ["h"], type: "boolean" },
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(`${usage("observe")}\nArchbird reads coverage reports; it never runs the tests.\n`);
    return 0;
  }
  required(options, "map", "request");
  const requestPath = path.resolve(options.request);
  const encoded = archbird.compileTestObservations(
    read(options.map),
    fs.readFileSync(requestPath),
    {
      repository: path.resolve(options._[0] || "."),
      requestDirectory: path.dirname(requestPath),
    },
  );
  write(encoded, options.output);
  return 0;
}

function freshnessMain(argv) {
  const options = parse(argv, {
    ...DISCOVERY,
    snapshot: { type: "string" },
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage("freshness"));
    return 0;
  }
  required(options, "snapshot");
  const progress = new Progress(options.progress);
  const currentProject = project(options, progress);
  progress.emit({ phase: "rendering", artifact: "canonical Map" });
  const currentMap = currentProject.mapJson();
  warnMapCacheStats(currentProject.mapCacheStats, options);
  progress.emit({ phase: "rendering", artifact: "freshness audit" });
  const encoded = archbird.auditMapFreshness(
    read(options.snapshot),
    currentMap,
    { pretty: options.pretty },
  );
  write(encoded, options.output);
  progress.finish();
  if (!options.check) return 0;
  const current = JSON.parse(currentMap);
  return JSON.parse(encoded).status === "current" && !hasErrors(current) ? 0 : 1;
}

function workspaceMain(argv) {
  const options = parse(argv, { ...COMMON });
  if (options.help) { process.stdout.write(usage("workspace")); return 0; }
  required(options, "config");
  const workspace = archbird.Workspace.fromConfig(options.config, {
    cacheDir: options.noCache
      ? null
      : (options.cacheDir || archbird.defaultProviderCacheDir()),
    cacheMaxBytes: cacheMaxBytes(options),
    typescript: !options.noTypescript,
  });
  const encoded = workspace.json({ pretty: options.pretty });
  write(encoded, options.output);
  const document = JSON.parse(encoded);
  return options.check && (hasErrors(document) || (document.projects || []).some((row) => row.diagnostics?.errors)) ? 1 : 0;
}

function verifyMain(argv) {
  const options = parse(argv, {
    ...DISCOVERY,
    map: { type: "string" },
    resolution: { type: "string" },
    baseline: { type: "string" },
    policyDate: { flag: "policy-date", type: "string" },
    observation: { type: "multiple" },
    mapInput: { flag: "map-input", type: "multiple" },
    resolutionInput: { flag: "resolution-input", type: "multiple" },
    freeze: { type: "string" },
    freezeOwner: { flag: "freeze-owner", type: "string" },
    freezeRationale: { flag: "freeze-rationale", type: "string" },
    format: { default: "markdown", type: "string" },
    full: { type: "boolean" },
    maxFindings: { flag: "max-findings", type: "number" },
  }, { positionals: Number.POSITIVE_INFINITY });
  if (options.help) { process.stdout.write(usage("verify")); return 0; }
  if (!["json", "markdown", "sarif", "junit"].includes(options.format)) {
    throw new Error("--format must be json, markdown, sarif, or junit");
  }
  if (options.noConfig) {
    throw new Error(
      "Verify requires reviewed constraints from archbird.json; " +
      "--no-config is not supported",
    );
  }
  if (options.full && options.maxFindings !== undefined) {
    throw new Error("--full and --max-findings conflict");
  }
  if (options.format !== "markdown" &&
      (options.full || options.maxFindings !== undefined)) {
    throw new Error("--full and --max-findings apply only to Markdown");
  }
  if (options.pretty && options.format !== "json") {
    throw new Error("--pretty applies only to JSON");
  }
  const maxFindings = options.maxFindings ?? 200;
  if (options.freeze && (!options.freezeOwner || !options.freezeRationale)) {
    throw new Error("--freeze requires --freeze-owner and --freeze-rationale");
  }
  if (!options.freeze && (options.freezeOwner || options.freezeRationale)) {
    throw new Error("--freeze-owner/--freeze-rationale require --freeze");
  }
  const constraintIds = [...options._];
  const repositoryOptions = { ...options, _: [] };
  const baselinePath = options.baseline ||
    (options.freeze && fs.existsSync(path.resolve(options.freeze))
      ? options.freeze
      : null);
  const progress = new Progress(options.progress);
  const {
    configJson,
    mapJson,
    resolutionJson,
    request: baseRequest,
  } = constraintContext(
    repositoryOptions,
    progress,
    { baselinePath },
  );
  const selectedRequest = { ...baseRequest };
  if (constraintIds.length) selectedRequest.ids = constraintIds;
  const requestJson = Object.keys(selectedRequest).length
    ? Buffer.from(JSON.stringify(selectedRequest))
    : Buffer.alloc(0);

  let blocking = false;
  let encoded;
  if (options.format === "json") {
    encoded = archbird.evaluateConstraints(configJson, mapJson, {
      resolutionJson,
      requestJson,
      pretty: options.pretty,
    });
    blocking = Boolean(JSON.parse(encoded.toString("utf8")).summary.blocking);
  } else {
    const report = native.constraintsReportWithBlocking(
      Buffer.from(configJson),
      Buffer.from(mapJson),
      Buffer.from(resolutionJson),
      Buffer.from(requestJson),
      options.format,
      options.full ? 0xffffffff : maxFindings,
      options.pretty || options.format === "sarif",
    );
    encoded = report.report;
    blocking = report.blocking;
  }
  write(encoded, options.output);

  if (options.freeze) {
    const freezeRequest = Object.keys(baseRequest).length
      ? Buffer.from(JSON.stringify(baseRequest))
      : Buffer.alloc(0);
    write(archbird.freezeConstraints(configJson, mapJson, {
      resolutionJson,
      requestJson: freezeRequest,
      owner: options.freezeOwner,
      rationale: options.freezeRationale,
      pretty: true,
    }), options.freeze);
  }
  progress.finish();
  if (!options.check) return 0;
  return blocking ? 1 : 0;
}

function planMain(argv) {
  const options = parse(argv, {
    ...DISCOVERY,
    map: { type: "string" },
    beforeMap: { flag: "before-map", type: "string" },
    gitDiff: { flag: "git-diff", type: "string" },
    resolution: { type: "string" },
    baseline: { type: "string" },
    policyDate: { flag: "policy-date", type: "string" },
    observation: { type: "multiple" },
    mapInput: { flag: "map-input", type: "multiple" },
    resolutionInput: { flag: "resolution-input", type: "multiple" },
    objective: { type: "string" },
    rename: { type: "multiple" },
    redirect: { type: "multiple" },
    format: { default: "json", type: "string" },
  }, { positionals: Number.POSITIVE_INFINITY });
  if (options.help) { process.stdout.write(usage("plan")); return 0; }
  if (!["json", "markdown"].includes(options.format)) {
    throw new Error("--format must be json or markdown");
  }
  if (options.pretty && options.format !== "json") {
    throw new Error("--pretty requires --format json");
  }
  const positionals = [...options._];
  const positionalRoot = queryPositionalIsRoot(positionals[0])
    ? positionals.shift()
    : null;
  const repositoryOptions = {
    ...options,
    _: positionalRoot ? [positionalRoot] : [],
  };
  if (options.beforeMap && options.gitDiff) {
    throw new Error("--before-map and --git-diff are mutually exclusive");
  }
  if (options.map && options.gitDiff) {
    throw new Error("--git-diff requires a live repository, not --map");
  }
  const repositoryHint = path.resolve(positionalRoot || options.root || ".");
  const transientOutput = repositoryArtifactPath(repositoryHint, options.output);
  repositoryOptions._transientExclude = transientOutput
    ? [transientOutput]
    : [];
  const progress = new Progress(options.progress);
  const resolvedInputs = repositoryInputs(repositoryOptions);
  const { repository, configJson } = resolvedInputs;
  if (!configJson.length) {
    throw new Error(
      `no archbird.json found in ${repository}; ` +
      "Verify and Plan require reviewed constraints",
    );
  }
  let beforeMapJson = options.beforeMap
    ? fs.readFileSync(options.beforeMap)
    : Buffer.alloc(0);
  if (options.gitDiff) {
    progress.emit({
      phase: "discovery",
      revision: options.gitDiff,
      state: "historical",
    });
    beforeMapJson = withGitSnapshot(
      repository,
      options.gitDiff,
      (snapshot) => {
        const historical = project(
          repositoryOptions,
          progress,
          { repository: snapshot, configJson },
        );
        try {
          return historical.mapJson();
        } finally {
          historical.dispose();
        }
      },
    );
  }
  const {
    mapJson,
    resolutionJson,
    project: current,
    request,
  } = constraintContext(repositoryOptions, progress, { resolvedInputs });
  const verificationJson = archbird.evaluateConstraints(configJson, mapJson, {
    resolutionJson,
    requestJson: Object.keys(request).length
      ? Buffer.from(JSON.stringify(request))
      : Buffer.alloc(0),
    pretty: false,
  });
  const mapDocument = JSON.parse(mapJson.toString("utf8"));
  if (hasErrors(mapDocument)) {
    throw new Error(
      "Plan requires a Map without error diagnostics; " +
      "fix Map evidence before deriving edits",
    );
  }
  const renameDirectives = {};
  for (const directive of options.rename || []) {
    const separator = directive.indexOf("=");
    const oldName = separator >= 0 ? directive.slice(0, separator) : "";
    const newName = separator >= 0 ? directive.slice(separator + 1) : "";
    if (
      separator < 1 ||
      newName.length === 0 ||
      Object.hasOwn(renameDirectives, oldName)
    ) {
      throw new Error(
        "--rename requires unique non-empty OLD=NEW directives",
      );
    }
    renameDirectives[oldName] = newName;
  }
  const planRequest = {};
  const configurationPlan = JSON.parse(
    archbird.compileProjectConfiguration(configJson).toString("utf8"),
  );
  if (Object.keys(configurationPlan.gates || {}).length) {
    planRequest.gates = configurationPlan.gates;
  }
  if (positionals.length) planRequest.constraint_ids = positionals;
  if (Object.keys(renameDirectives).length) {
    planRequest.renames = renameDirectives;
  }
  const redirectDirectives = {};
  for (const directive of options.redirect || []) {
    const separator = directive.indexOf("=");
    const from = separator >= 0 ? directive.slice(0, separator) : "";
    const to = separator >= 0 ? directive.slice(separator + 1) : "";
    if (
      separator < 1 ||
      to.length === 0 ||
      Object.hasOwn(redirectDirectives, from)
    ) {
      throw new Error(
        "--redirect requires unique non-empty FROM=TO directives",
      );
    }
    redirectDirectives[from] = to;
  }
  if (Object.keys(redirectDirectives).length) {
    planRequest.redirects = redirectDirectives;
  }
  if (options.objective) planRequest.objective = options.objective;
  const encoded = archbird.compilePlan(
    current,
    mapJson,
    verificationJson,
    {
      requestJson: Object.keys(planRequest).length
        ? Buffer.from(JSON.stringify(planRequest))
        : Buffer.alloc(0),
      beforeMapJson,
      pretty: options.pretty,
    },
  );
  const generated = JSON.parse(encoded.toString("utf8"));
  write(
    options.format === "markdown"
      ? archbird.renderPlanMarkdown(encoded)
      : encoded,
    options.output,
  );
  if (options.output !== "-") {
    const executable = generated.items.filter((item) => item.executable).length;
    process.stdout.write(
      "Result: " +
      `items=${generated.items.length}; ` +
      `executable=${executable}; ` +
      `non-executable=${generated.items.length - executable}; ` +
      `unknowns=${generated.unknowns.length}; ` +
      `preserved-constraints=${generated.preserved_constraints.length}\n`,
    );
  }
  progress.finish();
  return 0;
}

function actProject(options, repository, configJson, progress) {
  progress.emit({ phase: "discovery", state: "start" });
  const current = archbird.Project.fromRepository(repository, {
    config: configJson,
    _transientExclude: options._transientExclude || [],
    scan: false,
    typescript: !options.noTypescript,
  });
  progress.emit({ phase: "selected", files: current.sources.length });
  current.scan("primary", {
    cacheDir: options.noCache
      ? null
      : (options.cacheDir || archbird.defaultProviderCacheDir()),
    cacheMaxBytes: cacheMaxBytes(options),
    typescript: !options.noTypescript,
    progress: (event) => progress.emit(event),
    mapCache: true,
  });
  warnCacheStats(current.cacheStats, options);
  warnMapCacheStats(current.mapCacheStats, options);
  return current;
}

function actOverlayProject(options, before, configJson, overlay, progress) {
  progress.emit({ phase: "discovery", state: "overlay" });
  const current = before.withSourceOverlay(overlay, {
    config: configJson,
    scan: false,
    typescript: !options.noTypescript,
  });
  progress.emit({ phase: "selected", files: current.sources.length });
  current.scan("primary", {
    cacheDir: options.noCache
      ? null
      : (options.cacheDir || archbird.defaultProviderCacheDir()),
    cacheMaxBytes: cacheMaxBytes(options),
    typescript: !options.noTypescript,
    progress: (event) => progress.emit(event),
    mapCache: true,
  });
  warnCacheStats(current.cacheStats, options);
  warnMapCacheStats(current.mapCacheStats, options);
  return current;
}

function actMain(argv) {
  const options = parse(argv, {
    ...COMMON,
    progress: { default: "auto", type: "string" },
    baseline: { type: "string" },
    policyDate: { flag: "policy-date", type: "string" },
    observation: { type: "multiple" },
    mapInput: { flag: "map-input", type: "multiple" },
    resolutionInput: { flag: "resolution-input", type: "multiple" },
    submit: { type: "multiple" },
    format: { default: "markdown", type: "string" },
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage("act"));
    return 0;
  }
  if (!options._[0]) throw new Error("Act requires a Plan JSON path");
  if (!["markdown", "json", "patch"].includes(options.format)) {
    throw new Error("--format must be markdown, json, or patch");
  }
  if (options.pretty && options.format !== "json") {
    throw new Error("--pretty applies only to JSON");
  }
  const planJson = readBounded(options._[0], MAX_PLAN_BYTES, "Plan JSON");
  const repository = path.resolve(options.root || ".");
  const executorSubmissions = actExecutorSubmissions(options.submit);
  options._transientExclude = [
    repositoryArtifactPath(repository, options._[0]),
    repositoryArtifactPath(repository, options.output),
    ...executorSubmissions.files.map((file) =>
      repositoryArtifactPath(repository, file)
    ),
  ].filter((value, index, values) =>
    value !== null && values.indexOf(value) === index
  );
  const progress = new Progress(options.progress);
  const resolvedInputs = repositoryInputs({ ...options, _: [], noConfig: false });
  if (!resolvedInputs.configJson.length) {
    throw new Error(
      `no archbird.json found in ${resolvedInputs.repository}; ` +
      "Act acceptance requires reviewed constraints",
    );
  }
  const request = constraintRequest(options, resolvedInputs.configJson);
  const requestJson = Object.keys(request).length
    ? Buffer.from(JSON.stringify(request))
    : Buffer.alloc(0);
  const beforeProject = actProject(
    options,
    resolvedInputs.repository,
    resolvedInputs.configJson,
    progress,
  );
  const beforeMap = beforeProject.mapJson();
  const beforeVerification = archbird.evaluateConstraints(
    resolvedInputs.configJson,
    beforeMap,
    {
      resolutionJson: beforeProject.resolutionJson || Buffer.alloc(0),
      requestJson,
      pretty: false,
    },
  );
  const sourceMetadata = observePlanSources(
    resolvedInputs.repository,
    planJson,
    executorSubmissions.json,
  );
  const materializedAct = archbird.materializeAct(
    beforeProject,
    planJson,
    beforeMap,
    beforeVerification,
    sourceMetadata,
    { executorSubmissionsJson: executorSubmissions.json },
  );
  const overlay = actOverlay(materializedAct);
  let afterConfigJson = resolvedInputs.configJson;
  if (resolvedInputs.configPath) {
    const relativeConfig = path.relative(
      resolvedInputs.repository,
      resolvedInputs.configPath,
    ).split(path.sep).join("/");
    if (
      relativeConfig &&
      relativeConfig !== ".." &&
      !relativeConfig.startsWith("../") &&
      Object.hasOwn(overlay, relativeConfig)
    ) {
      if (overlay[relativeConfig] === null) {
        throw new Error(
          "Act cannot remove the project configuration used for acceptance",
        );
      }
      afterConfigJson = overlay[relativeConfig];
      validateProjectConfiguration(afterConfigJson);
    }
  }
  const afterProject = actOverlayProject(
    options,
    beforeProject,
    afterConfigJson,
    overlay,
    progress,
  );
  const afterMap = afterProject.mapJson();
  const afterRequest = constraintRequest(options, afterConfigJson);
  const afterVerification = archbird.evaluateConstraints(
    afterConfigJson,
    afterMap,
    {
      resolutionJson: afterProject.resolutionJson || Buffer.alloc(0),
      requestJson: Object.keys(afterRequest).length
        ? Buffer.from(JSON.stringify(afterRequest))
        : Buffer.alloc(0),
      pretty: false,
    },
  );
  const gateResults = runActGates(
    resolvedInputs.repository,
    materializedAct,
  );
  let acceptedAct;
  try {
    acceptedAct = archbird.acceptAct(
      materializedAct,
      beforeMap,
      afterMap,
      afterVerification,
      gateResults,
    );
  } catch (error) {
    const details = gateFailureDetails(gateResults);
    if (details) throw new Error(`${error.message}\n${details}`);
    throw error;
  }
  write(
    renderAct(resolvedInputs.repository, acceptedAct, {
      format: options.format,
      pretty: options.pretty,
    }),
    options.output,
  );
  progress.finish();
  return 0;
}

function applyMain(argv) {
  const options = parse(argv, {
    root: COMMON.root,
    help: COMMON.help,
  }, { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage("apply"));
    return 0;
  }
  if (!options._[0]) throw new Error("Apply requires an Act JSON path");
  const actJson = readBounded(
    options._[0],
    MAX_ACT_BYTES,
    "Act JSON",
  );
  const transitions = applyAcceptedAct(
    path.resolve(options.root || "."),
    actJson,
  );
  const state = transitions ? "applied" : "already-satisfied";
  process.stdout.write(
    `Result: applied-transitions=${transitions}; state=${state}\n`,
  );
  return 0;
}

function exportMain(argv) {
  const options = parse(argv, {
    map: { type: "string" }, output: COMMON.output,
    view: { default: "components", type: "string" },
    direction: { default: "LR", type: "string" },
    maxNodes: { flag: "max-nodes", default: 200, type: "number" },
    maxEdgeNames: { flag: "max-edge-names", default: 3, type: "number" },
    help: COMMON.help,
  }, { positionals: 1 });
  if (options.help) { process.stdout.write(usage("export")); return 0; }
  required(options, "map");
  const format = options._[0];
  if (!["graphml", "json", "mermaid"].includes(format)) {
    throw new Error("export format must be graphml, json, or mermaid");
  }
  write(archbird.exportGraph(read(options.map), {
    format, view: options.view, direction: options.direction,
    maxNodes: options.maxNodes, maxEdgeNames: options.maxEdgeNames,
  }), options.output);
  return 0;
}

function supportMain(argv) {
  const options = parse(argv, { help: COMMON.help, pretty: COMMON.pretty });
  if (options.help) { process.stdout.write(usage("support")); return 0; }
  const report = {
    core_implementation_sha256: archbird.IMPLEMENTATION_SHA256,
    engine: archbird.ENGINE,
    native_abi_version: archbird.NATIVE_ABI_VERSION,
    pattern: {
      contract: archbird.PATTERN_CONTRACT,
      contract_version: archbird.PATTERN_CONTRACT_VERSION,
      engine: archbird.PATTERN_ENGINE,
      options: archbird.PATTERN_OPTIONS,
      unicode: archbird.PATTERN_UNICODE,
    },
    providers: archbird.PROVIDER_SUPPORT,
    runtime: {
      executable: path.resolve(process.execPath),
      implementation: "Node.js",
      kind: "node",
      version: process.version,
    },
    version: archbird.VERSION,
  };
  write(Buffer.from(JSON.stringify(report, null, options.pretty ? 2 : 0)));
  return 0;
}

async function serveMain(argv) {
  const options = parse(argv, {
    config: { aliases: ["c"], type: "string" },
    root: { type: "string" },
    noConfig: { flag: "no-config", type: "boolean" },
    project: { type: "string" },
    source: { type: "multiple" },
    only: { type: "multiple" },
    exclude: { type: "multiple" },
    ignoreFile: { flag: "ignore-file", type: "multiple" },
    noIgnore: { flag: "no-ignore", type: "boolean" },
    noDefaultExcludes: { flag: "no-default-excludes", type: "boolean" },
    maxFileBytes: { flag: "max-file-bytes", type: "number" },
    maxIndexBytes: { flag: "max-index-bytes", type: "number" },
    noTypescript: { flag: "no-typescript", type: "boolean" },
    cacheDir: { flag: "cache-dir", type: "string" },
    cacheMaxBytes: { flag: "cache-max-bytes", type: "number" },
    noCache: { flag: "no-cache", type: "boolean" },
    host: { default: "127.0.0.1", type: "string" },
    port: { default: 4177, type: "number" },
    app: { type: "string" },
    help: { aliases: ["h"], type: "boolean" },
  });
  if (options.help) {
    process.stdout.write(`${usage("serve")}\nThe local host keeps the last good generation while rebuilding on repository changes.\n`);
    return 0;
  }
  if (options.config === "-") throw new Error("archbird serve requires a filesystem config path");
  const { repository, configJson, configPath } = repositoryInputs({
    ...options,
    output: "-",
  });
  const { createLiveServer } = require("./serve");
  const server = await createLiveServer({
    app: options.app || null,
    config: configPath,
    configJson: configPath ? null : (configJson.length ? configJson : null),
    host: options.host,
    noConfig: options.noConfig,
    port: options.port,
    projectOptions: {
      defaultExcludes: !options.noDefaultExcludes,
      exclude: options.exclude,
      ignore: !options.noIgnore,
      ignoreFiles: options.ignoreFile,
      maxFileBytes: options.maxFileBytes ?? null,
      maxIndexBytes: options.maxIndexBytes ?? null,
      only: options.only,
      project: options.project || null,
      source: options.source,
      cacheDir: options.noCache
        ? null
        : (options.cacheDir || archbird.defaultProviderCacheDir()),
      cacheMaxBytes: cacheMaxBytes(options),
    },
    root: repository,
    typescript: !options.noTypescript,
  });
  process.stdout.write(`${server.url}\n`);
  let closing = false;
  const close = async () => {
    if (closing) return;
    closing = true;
    await server.close();
  };
  process.once("SIGINT", () => void close());
  process.once("SIGTERM", () => void close());
  return await new Promise((resolve) => server.server.once("close", () => resolve(0)));
}

function main(argv = process.argv.slice(2)) {
  if (["-h", "--help", "help"].includes(argv[0])) {
    process.stdout.write(topLevelUsage());
    return 0;
  }
  if (argv[0] === "--version") {
    process.stdout.write(`${archbird.VERSION}\n`);
    return 0;
  }
  if (argv.length && !COMMANDS.has(argv[0]) && !mapShortcut(argv)) {
    throw new Error(
      `unknown command ${JSON.stringify(argv[0])}; run \`archbird --help\``,
    );
  }
  const command = argv[0] && COMMANDS.has(argv[0]) ? argv[0] : "map";
  const rest = command === "map" && argv[0] !== "map" ? argv : argv.slice(1);
  if (command === "config") return configMain(rest);
  if (command === "query" || command === "impact") {
    return queryMain(rest, command);
  }
  if (command === "path") return pathMain(rest);
  if (command === "diff") return diffMain(rest);
  if (command === "observe") return observeMain(rest);
  if (command === "freshness") return freshnessMain(rest);
  if (command === "workspace") return workspaceMain(rest);
  if (command === "verify") return verifyMain(rest);
  if (command === "plan") return planMain(rest);
  if (command === "act") return actMain(rest);
  if (command === "apply") return applyMain(rest);
  if (command === "export") return exportMain(rest);
  if (command === "serve") return serveMain(rest);
  if (command === "support") return supportMain(rest);
  return mapMain(rest);
}

if (require.main === module) {
  Promise.resolve()
    .then(() => main())
    .then((code) => { process.exitCode = code; })
    .catch((error) => {
      process.stderr.write(`archbird: error: ${error.message || error}\n`);
      process.exitCode = 2;
    });
}

module.exports = { _decodeGitStatus: decodeGitStatus, main };
