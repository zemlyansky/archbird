#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { TextDecoder } = require("node:util");
const archbird = require("./index");

const COMMANDS = new Set([
  "config",
  "contract",
  "diff",
  "export",
  "freshness",
  "impact",
  "map",
  "plan",
  "query",
  "serve",
  "support",
  "verify",
  "verify-plan",
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
    map: "archbird map [ROOT] [--config PROJECT.json] [--view overview|architecture|audit] [--detail compact|standard|full] [--progress auto|always|never] [--format markdown|json] [--check]",
    query: "archbird query [ROOT] [--config PROJECT.json | --map MAP.json] [SELECTORS] [--check]",
    impact: "archbird impact [ROOT] [--config PROJECT.json | --map MAP.json] [SELECTORS] [--check]",
    config: "archbird config show|init [ROOT] [--config PROJECT.json]",
    diff: "archbird diff --before OLD.json --after NEW.json [--check[=CATEGORIES]]",
    freshness: "archbird freshness [ROOT] --snapshot MAP_OR_QUERY.json [--config PROJECT.json] [--check]",
    workspace: "archbird workspace --config WORKSPACE.json [--check]",
    verify: "archbird verify --config SUITE.verify.json [--project-root NAME=PATH] [--check]",
    plan: "archbird plan --verification RESULT.json --finding FINGERPRINT",
    contract: "archbird contract --proposal PROPOSAL.json --objective TEXT --owner NAME --rationale TEXT",
    "verify-plan": "archbird verify-plan --proposal P.json --contract C.json --before-verification B.json --after-verification A.json [--check]",
    export: "archbird export graphml|json|mermaid --map MAP_OR_QUERY.json [--output FILE]",
    serve: "archbird serve [ROOT] [--config PROJECT.json] [--host 127.0.0.1] [--port 4177]",
    support: "archbird support",
  };
  const selectorHelp = ["query", "impact"].includes(command)
    ? "\n--symbol accepts PATTERN or repository-relative PATH:PATTERN; repeated selectors form a union.\n" +
      "--git-diff REVISION seeds tracked current paths and retains deletions as change evidence.\n" +
      "Context profiles exact|change|architecture|audit control Markdown; " +
      "--max-chars is only the final guard.\n"
    : "";
  return `${rows[command]}\n${selectorHelp}\nMap → Verify → Act with deterministic native/Wasm evidence.\n`;
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

function write(value, output = "-") {
  const bytes = Buffer.from(value);
  const encoded = bytes.length && bytes[bytes.length - 1] === 10
    ? bytes
    : Buffer.concat([bytes, Buffer.from("\n")]);
  if (output === "-") process.stdout.write(encoded);
  else fs.writeFileSync(path.resolve(output), encoded);
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

function configRoot(configJson) {
  return JSON.parse(archbird.discoveryPlan(configJson, []).toString("utf8")).root;
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
      if (!positional && !override) {
        repository = path.resolve(repository, configRoot(configJson));
      }
    }
    else {
      configPath = path.resolve(options.config);
      configJson = fs.readFileSync(configPath);
    }
    if (!positional && !override && configPath) {
      repository = path.resolve(path.dirname(configPath), configRoot(configJson));
    }
  } else if (!options.noConfig) {
    const candidates = ["archbird.json", ".archbird.json"]
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
    if (candidates.length > 1) {
      throw new Error("repository contains both archbird.json and .archbird.json");
    }
    if (candidates.length) {
      configPath = candidates[0];
      configJson = fs.readFileSync(configPath);
      repository = path.resolve(repository, configRoot(configJson));
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

function warnCacheStats(stats) {
  if (stats.noSpace) {
    process.stderr.write(
      "archbird: warning: provider-cache write failed because storage is full; " +
      "analysis remains valid. Use --cache-dir, increase --cache-max-bytes, " +
      "or use --no-cache.\n",
    );
  }
  if (stats.skipped) {
    process.stderr.write(
      "archbird: warning: provider-cache entries exceeded the configured " +
      "budget and were not stored; analysis remains valid. Increase " +
      "--cache-max-bytes or use --no-cache.\n",
    );
  }
}

function warnMapCacheStats(stats) {
  if (stats.noSpace) {
    process.stderr.write(
      "archbird: warning: canonical Map cache write failed because storage " +
      "is full; analysis remains valid.\n",
    );
  }
  if (stats.skipped) {
    process.stderr.write(
      "archbird: warning: canonical Map exceeded the configured cache budget " +
      "and was not stored; analysis remains valid.\n",
    );
  }
}

function project(options, progress = null) {
  if (progress !== null) progress.emit({ phase: "discovery", state: "start" });
  const { repository, configJson } = repositoryInputs(options);
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
    scan: false,
    typescript: !options.noTypescript,
  });
  if (progress !== null) {
    progress.emit({ phase: "selected", files: current.sources.length });
  }
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
  warnCacheStats(current.cacheStats);
  if (options.mergeLedger) {
    write(current.mergeConflictsJson({ pretty: true }), options.mergeLedger);
  }
  for (const observationPath of options.testSymbolObservations || []) {
    current.addTestSymbolObservations(read(observationPath));
  }
  return current;
}

function mapMain(argv) {
  const options = parse(argv, {
    ...DISCOVERY,
    format: { default: "markdown", type: "string" },
    view: { default: "overview", type: "string" },
    detail: { default: "standard", type: "string" },
    compact: { type: "boolean" },
    full: { type: "boolean" },
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
  if (!["json", "markdown"].includes(options.format)) throw new Error("--format must be json or markdown");
  if (!["overview", "architecture", "audit"].includes(options.view)) {
    throw new Error("--view must be overview, architecture, or audit");
  }
  if (!["compact", "standard", "full"].includes(options.detail)) {
    throw new Error("--detail must be compact, standard, or full");
  }
  if (options.compact && options.full) throw new Error("--compact and --full conflict");
  if ((options.compact || options.full) && options.detail !== "standard") {
    throw new Error("--detail conflicts with --compact/--full");
  }
  if (options.format === "json" && (
    options.compact || options.full || options.maxChars ||
    options.detail !== "standard" || options.view !== "overview"
  )) {
    throw new Error("--view, --detail, --compact, --full, and --max-chars apply only to Markdown");
  }
  const progress = new Progress(options.progress);
  const current = project(options, progress);
  progress.emit({ phase: "rendering", artifact: "canonical Map" });
  const mapJson = current.mapJson({ pretty: options.pretty && options.format === "json" });
  warnMapCacheStats(current.mapCacheStats);
  const output = options.format === "json"
    ? mapJson
    : current.mapMarkdown({
      view: options.view,
      detail: options.detail,
      compact: options.compact,
      full: options.full,
      maxChars: options.maxChars,
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
    gitDiff: { flag: "git-diff", type: "string" },
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
    maxChars: { flag: "max-chars", default: 0, type: "number" },
    testSymbolObservations: { flag: "test-symbol-observations", type: "multiple" },
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
    const rawStatus = fields[index++].toString("ascii");
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

function queryMain(argv, command) {
  const options = parse(argv, selectorDefinitions(), { positionals: 1 });
  if (options.help) {
    process.stdout.write(usage(command));
    return 0;
  }
  if (options.map && (
    options.config || options.noConfig || options._.length || options.root ||
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
  if (options.maxSeedDistance !== undefined && options.maxSeedDistance < 0) {
    throw new Error("--max-seed-distance must be nonnegative");
  }
  if (options.compact && options.full) {
    throw new Error("--compact and --full conflict");
  }
  if ((options.compact || options.full) && options.detail !== "standard") {
    throw new Error("--detail conflicts with --compact/--full");
  }
  const progress = new Progress(options.progress);
  let source;
  let changeSet = null;
  if (options.map) source = read(options.map);
  else {
    if (options.gitDiff) {
      changeSet = gitChangeSet(repositoryInputs(options).repository, options.gitDiff);
    }
    const current = project(options, progress);
    progress.emit({ phase: "rendering", artifact: "canonical Map" });
    source = current.mapJson();
    warnMapCacheStats(current.mapCacheStats);
  }
  const sourceDocument = JSON.parse(source);
  const queryOptions = {
    artifacts: options.artifact,
    components: options.component,
    changeSet,
    depth: options.depth,
    direction: options.direction || (command === "impact" ? "upstream" : "both"),
    focus: options.focus,
    packages: options.package,
    paths: options.path,
    producerPolicy: options.check && options.map ? "current" : "compatible",
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
  try {
    if (options.format === "json") {
      if (options.maxChars) throw new Error("--max-chars applies only to Markdown");
      progress.finish();
      write(archbird.queryMap(source, { ...queryOptions, pretty: options.pretty }), options.output);
    } else if (options.format === "markdown") {
      progress.finish();
      write(archbird.queryMapMarkdown(source, {
        ...queryOptions,
        compact: options.compact,
        detail: options.detail,
        full: options.full,
        maxChars: options.maxChars,
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

function configMain(argv) {
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
  "public-api": [["public_symbols", "removed"], ["package_exports", "removed"], ["package_entrypoint_surfaces", "removed"], ["entrypoints", "removed"]],
  bridges: [["bridges", "any"], ["bridge_surfaces", "any"]],
  calls: [["call_resolutions", "any"], ["symbol_calls", "any"], ["symbol_references", "any"]],
  parity: [["parity_gaps", "added"]],
  tests: [["test_route_evidence", "any"], ["test_routes", "removed"]],
  architecture: [["artifacts", "any"], ["build_routes", "any"], ["component_routes", "any"], ["package_dependencies", "any"]],
};

function diffRisk(document, raw) {
  const categories = raw.split(",").filter(Boolean);
  const selected = categories.includes("all") ? Object.keys(DIFF_POLICIES) : categories;
  for (const category of selected) {
    if (!DIFF_POLICIES[category]) throw new Error(`unknown diff risk category: ${category}`);
    for (const [name, policy] of DIFF_POLICIES[category]) {
      const section = document.sections[name];
      if (!section) continue;
      if (policy === "any" && (section.added?.length || section.changed?.length || section.removed?.length)) return true;
      if (policy === "added" && (section.added?.length || section.changed?.length)) return true;
      if (policy === "removed" && (section.removed?.length || section.changed?.length)) return true;
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
  warnMapCacheStats(currentProject.mapCacheStats);
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

function projectRoots(values) {
  const result = {};
  for (const value of values) {
    const split = value.indexOf("=");
    if (split <= 0 || split === value.length - 1) throw new Error(`--project-root expects NAME=PATH: ${value}`);
    const name = value.slice(0, split);
    if (Object.hasOwn(result, name)) throw new Error(`duplicate project root: ${name}`);
    result[name] = path.resolve(value.slice(split + 1));
  }
  return result;
}

function verifyMain(argv) {
  const options = parse(argv, {
    ...COMMON,
    format: { default: "json", type: "string" },
    projectRoot: { flag: "project-root", type: "multiple" },
    baseline: { type: "string" },
    full: { type: "boolean" },
    maxFindings: { flag: "max-findings", default: 200, type: "number" },
  });
  if (options.help) { process.stdout.write(usage("verify")); return 0; }
  required(options, "config");
  const verification = archbird.Verification.fromConfig(options.config, {
    baseline: options.baseline,
    cacheDir: options.noCache
      ? null
      : (options.cacheDir || archbird.defaultProviderCacheDir()),
    cacheMaxBytes: cacheMaxBytes(options),
    projectRoots: projectRoots(options.projectRoot),
    typescript: !options.noTypescript,
  });
  const encoded = verification.report({
    format: options.format,
    full: options.full,
    maxFindings: options.maxFindings,
    pretty: options.pretty || options.format === "sarif",
  });
  write(encoded, options.output);
  return options.check && verification.hasErrors() ? 1 : 0;
}

function planMain(argv) {
  const options = parse(argv, {
    verification: { type: "string" }, finding: { type: "string" },
    format: { default: "json", type: "string" }, full: { type: "boolean" },
    maxCandidates: { flag: "max-candidates", default: 100, type: "number" },
    pretty: COMMON.pretty, output: COMMON.output, help: COMMON.help,
  });
  if (options.help) { process.stdout.write(usage("plan")); return 0; }
  required(options, "verification", "finding");
  write(archbird.compileChangeProposal(read(options.verification), options.finding, {
    format: options.format, full: options.full, maxCandidates: options.maxCandidates, pretty: options.pretty,
  }), options.output);
  return 0;
}

function contractMain(argv) {
  const options = parse(argv, {
    proposal: { type: "string" }, objective: { type: "string" }, owner: { type: "string" },
    rationale: { type: "string" }, preserveCheck: { flag: "preserve-check", type: "multiple" },
    preserveAll: { flag: "preserve-all", type: "boolean" },
    selectCandidate: { flag: "select-candidate", type: "multiple" },
    format: { default: "json", type: "string" }, pretty: COMMON.pretty,
    output: COMMON.output, help: COMMON.help,
  });
  if (options.help) { process.stdout.write(usage("contract")); return 0; }
  required(options, "proposal", "objective", "owner", "rationale");
  const proposal = read(options.proposal);
  let preserveChecks = options.preserveCheck;
  if (options.preserveAll) {
    preserveChecks = JSON.parse(proposal).preserved_invariants.map((row) => row.id);
  }
  write(archbird.createChangeContract(proposal, {
    objective: options.objective, owner: options.owner, rationale: options.rationale,
    preserveChecks, selectedCandidates: options.selectCandidate,
    format: options.format, pretty: options.pretty,
  }), options.output);
  return 0;
}

function verifyPlanMain(argv) {
  const options = parse(argv, {
    proposal: { type: "string" }, contract: { type: "string" },
    beforeVerification: { flag: "before-verification", type: "string" },
    afterVerification: { flag: "after-verification", type: "string" },
    format: { default: "json", type: "string" }, pretty: COMMON.pretty,
    output: COMMON.output, check: COMMON.check, help: COMMON.help,
  });
  if (options.help) { process.stdout.write(usage("verify-plan")); return 0; }
  required(options, "proposal", "contract", "beforeVerification", "afterVerification");
  const inputs = [options.proposal, options.contract, options.beforeVerification, options.afterVerification].map(read);
  const encoded = archbird.verifyChangeContract(...inputs, {
    format: options.format, pretty: options.pretty || options.format === "sarif",
  });
  write(encoded, options.output);
  if (!options.check) return 0;
  const result = options.format === "json" ? JSON.parse(encoded) : JSON.parse(
    archbird.verifyChangeContract(...inputs, { format: "json", pretty: false }),
  );
  return ["satisfied", "superseded"].includes(result.status) ? 0 : 1;
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
  }, { positionals: 1 });
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
  const command = argv[0] && COMMANDS.has(argv[0]) ? argv[0] : "map";
  const rest = command === "map" && argv[0] !== "map" ? argv : argv.slice(1);
  if (command === "config") return configMain(rest);
  if (command === "query" || command === "impact") {
    return queryMain(rest, command);
  }
  if (command === "diff") return diffMain(rest);
  if (command === "freshness") return freshnessMain(rest);
  if (command === "workspace") return workspaceMain(rest);
  if (command === "verify") return verifyMain(rest);
  if (command === "plan") return planMain(rest);
  if (command === "contract") return contractMain(rest);
  if (command === "verify-plan") return verifyPlanMain(rest);
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

module.exports = { main };
