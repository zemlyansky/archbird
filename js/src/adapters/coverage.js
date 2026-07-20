"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");

const FORMATS = new Set(["gcov", "istanbul", "llvm", "v8"]);
const ID = /^[A-Za-z0-9][A-Za-z0-9_.:-]*$/;
const SHA256 = /^[0-9a-f]{64}$/;

class CoverageAdapterError extends Error {}

function canonical(value) {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.keys(value).sort((left, right) => (
      Buffer.compare(Buffer.from(left), Buffer.from(right))
    )).map((key) => (
      `${JSON.stringify(key)}:${canonical(value[key])}`
    )).join(",")}}`;
  }
  return JSON.stringify(value);
}

function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function object(value, required, optional, label) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new CoverageAdapterError(`${label} must be an object`);
  }
  const keys = new Set(Object.keys(value));
  const missing = [...required].filter((key) => !keys.has(key));
  const extra = [...keys].filter((key) => !required.has(key) && !optional.has(key));
  if (missing.length || extra.length) {
    throw new CoverageAdapterError(
      `${label} keys differ: missing=${JSON.stringify(missing.sort())} extra=${JSON.stringify(extra.sort())}`,
    );
  }
  return value;
}

function relative(value, label) {
  if (typeof value !== "string" || !value || value.includes("\\") || value.includes("//") ||
      value.startsWith("/") || value.split("/").some((part) => !part || part === "." || part === "..")) {
    throw new CoverageAdapterError(`${label} must be a repository-relative path`);
  }
  return value;
}

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function request(value) {
  const root = object(
    value,
    new Set(["artifact", "cases", "format", "group", "reports", "runner_paths", "schema_version"]),
    new Set(),
    "coverage request",
  );
  if (root.schema_version !== 1 || root.artifact !== "archbird-coverage-observation-request") {
    throw new CoverageAdapterError("coverage request has unsupported identity");
  }
  if (!FORMATS.has(root.format)) {
    if (root.format === "coverage.py") {
      throw new CoverageAdapterError("coverage.py observations require the PyPI host with CPython AST");
    }
    throw new CoverageAdapterError(`unsupported coverage format: ${JSON.stringify(root.format)}`);
  }
  if (typeof root.group !== "string" || !root.group) {
    throw new CoverageAdapterError("coverage request group must be non-empty");
  }
  if (!Array.isArray(root.runner_paths) || !root.runner_paths.length) {
    throw new CoverageAdapterError("runner_paths must be a non-empty array");
  }
  const runners = root.runner_paths.map((value) => relative(value, "runner_paths"));
  const sortedRunners = [...new Set(runners)].sort(utf8Compare);
  if (canonical(runners) !== canonical(sortedRunners)) {
    throw new CoverageAdapterError("runner_paths must be sorted and unique");
  }
  if (!Array.isArray(root.reports) || !root.reports.length) {
    throw new CoverageAdapterError("reports must be a non-empty array");
  }
  const reports = root.reports.map((raw, index) => {
    const row = object(raw, new Set(["id", "path"]), new Set(), `reports[${index}]`);
    if (typeof row.id !== "string" || !ID.test(row.id)) {
      throw new CoverageAdapterError(`reports[${index}].id is invalid`);
    }
    return { id: row.id, path: relative(row.path, `reports[${index}].path`) };
  });
  const sortedReports = [...reports].sort((left, right) => (
    utf8Compare(left.id, right.id) || utf8Compare(left.path, right.path)
  ));
  if (canonical(reports) !== canonical(sortedReports) || new Set(reports.map((row) => row.id)).size !== reports.length) {
    throw new CoverageAdapterError("reports must be sorted with unique ids");
  }
  const reportIds = new Set(reports.map((row) => row.id));
  if (!Array.isArray(root.cases) || !root.cases.length) {
    throw new CoverageAdapterError("cases must be a non-empty array");
  }
  const cases = root.cases.map((raw, index) => {
    const row = object(raw, new Set(["path", "report", "selector"]), new Set(["context"]), `cases[${index}]`);
    if (row.context !== undefined) {
      throw new CoverageAdapterError(`${root.format} cases must use one isolated report per case`);
    }
    if (!reportIds.has(row.report)) throw new CoverageAdapterError(`cases[${index}].report is unknown`);
    if (typeof row.selector !== "string" || !row.selector) {
      throw new CoverageAdapterError(`cases[${index}].selector must be non-empty`);
    }
    return {
      path: relative(row.path, `cases[${index}].path`),
      report: row.report,
      selector: row.selector,
    };
  });
  const sortedCases = [...cases].sort((left, right) => (
    utf8Compare(left.path, right.path) || utf8Compare(left.selector, right.selector)
  ));
  if (canonical(cases) !== canonical(sortedCases) ||
      new Set(cases.map((row) => `${row.path}\0${row.selector}`)).size !== cases.length) {
    throw new CoverageAdapterError("cases must be sorted and unique by path/selector");
  }
  if (new Set(cases.map((row) => row.report)).size !== cases.length) {
    throw new CoverageAdapterError(`${root.format} requires one isolated report per case`);
  }
  return { cases, format: root.format, group: root.group, raw: root, reports, runners };
}

function mapIndex(value) {
  if (!value || typeof value !== "object" || value.artifact !== "map" ||
      !ID.test(value.project || "") || !value.evidence || !Array.isArray(value.files)) {
    throw new CoverageAdapterError("map input must be a canonical Map");
  }
  if (!SHA256.test(value.evidence.config_sha256 || "") || !SHA256.test(value.evidence.input_sha256 || "")) {
    throw new CoverageAdapterError("Map digests are invalid");
  }
  const files = new Map();
  value.files.forEach((row, index) => {
    if (!row || typeof row !== "object" || !Array.isArray(row.symbols) || !SHA256.test(row.sha256 || "")) {
      throw new CoverageAdapterError(`Map files[${index}] is invalid`);
    }
    const name = relative(row.path, `Map files[${index}].path`);
    if (files.has(name)) throw new CoverageAdapterError(`duplicate Map file: ${name}`);
    files.set(name, row);
  });
  return {
    config: value.evidence.config_sha256,
    files,
    inputs: value.evidence.input_sha256,
    project: value.project,
  };
}

function reportPath(raw, repository, files) {
  let candidate = raw;
  if (candidate.startsWith("file://")) candidate = candidate.slice(7);
  if (path.isAbsolute(candidate)) {
    candidate = path.relative(repository, path.resolve(candidate)).split(path.sep).join("/");
    if (candidate === ".." || candidate.startsWith("../")) return null;
  } else {
    candidate = candidate.replace(/^\.\//, "");
  }
  return files.has(candidate) ? candidate : null;
}

function symbolAtLine(file, line, reportedName = null) {
  const matches = file.symbols.filter((row) => row && row.line === line && typeof row.name === "string" && row.name);
  if (reportedName) {
    const normalized = reportedName.split("(", 1)[0].trim();
    const leaf = normalized.split("::").pop();
    const named = matches.filter((row) => row.name === normalized || row.name.split(".").pop() === leaf);
    if (named.length === 1) return named[0].name;
  }
  return matches.length === 1 ? matches[0].name : null;
}

function addHit(result, file, symbol, count) {
  const key = `${file}\0${symbol}`;
  const current = result.get(key);
  result.set(key, { count: (current ? current.count : 0) + count, path: file, symbol });
}

function istanbulHits(report, testCase, repository, files) {
  const result = new Map();
  for (const [rawPath, coverage] of Object.entries(report)) {
    const filePath = reportPath(rawPath, repository, files);
    if (!filePath || filePath === testCase.path || !coverage || typeof coverage !== "object") continue;
    if (!coverage.fnMap || typeof coverage.fnMap !== "object" || !coverage.f || typeof coverage.f !== "object") {
      throw new CoverageAdapterError("Istanbul file coverage requires fnMap and f objects");
    }
    for (const [identity, fn] of Object.entries(coverage.fnMap)) {
      const count = coverage.f[identity];
      const location = fn && (fn.decl || fn.loc);
      const line = location && location.start && location.start.line;
      if (!Number.isSafeInteger(count) || count <= 0 || !Number.isSafeInteger(line) || line < 1) continue;
      const symbol = symbolAtLine(files.get(filePath), line, typeof fn.name === "string" ? fn.name : null);
      if (symbol) addHit(result, filePath, symbol, count);
    }
  }
  return result;
}

function v8Hits(report, testCase, repository, files) {
  if (!Array.isArray(report.result)) throw new CoverageAdapterError("V8 coverage requires a result array");
  const result = new Map();
  for (const script of report.result) {
    if (!script || typeof script.url !== "string" || !Array.isArray(script.functions)) continue;
    const filePath = reportPath(script.url, repository, files);
    if (!filePath || filePath === testCase.path) continue;
    const bytes = fs.readFileSync(path.join(repository, filePath));
    if (sha256(bytes) !== files.get(filePath).sha256) {
      throw new CoverageAdapterError(`mapped JavaScript source is stale: ${filePath}`);
    }
    const source = bytes.toString("utf8");
    for (const fn of script.functions) {
      if (!fn || !Array.isArray(fn.ranges)) continue;
      const positive = fn.ranges.filter((row) => row && Number.isSafeInteger(row.count) && row.count > 0);
      if (!positive.length) continue;
      const start = positive[0].startOffset;
      if (!Number.isSafeInteger(start) || start < 0 || start > source.length) continue;
      // JavaScript string indexes are UTF-16 code-unit offsets, matching V8.
      const line = source.slice(0, start).split("\n").length;
      const symbol = symbolAtLine(
        files.get(filePath),
        line,
        typeof fn.functionName === "string" ? fn.functionName : null,
      );
      if (symbol) addHit(result, filePath, symbol, Math.max(...positive.map((row) => row.count)));
    }
  }
  return result;
}

function llvmHits(report, testCase, repository, files) {
  if (report.type !== "llvm.coverage.json.export" || !Array.isArray(report.data)) {
    throw new CoverageAdapterError("LLVM report is not llvm-cov export JSON");
  }
  const result = new Map();
  for (const unit of report.data) {
    for (const fn of (unit && Array.isArray(unit.functions) ? unit.functions : [])) {
      if (!fn || !Number.isSafeInteger(fn.count) || fn.count <= 0 ||
          !Array.isArray(fn.filenames) || !Array.isArray(fn.regions)) continue;
      for (const region of fn.regions) {
        if (!Array.isArray(region) || region.length < 6) continue;
        const line = region[0];
        const fileId = region[5];
        if (!Number.isSafeInteger(line) || !Number.isSafeInteger(fileId) || fileId < 0 || fileId >= fn.filenames.length) continue;
        const filePath = reportPath(String(fn.filenames[fileId]), repository, files);
        if (!filePath || filePath === testCase.path) continue;
        const symbol = symbolAtLine(files.get(filePath), line, typeof fn.name === "string" ? fn.name : null);
        if (symbol) {
          addHit(result, filePath, symbol, fn.count);
          break;
        }
      }
    }
  }
  return result;
}

function gcovHits(report, testCase, repository, files) {
  if (typeof report.format_version !== "string" || !Array.isArray(report.files)) {
    throw new CoverageAdapterError("gcov report is not --json-format output");
  }
  const result = new Map();
  for (const file of report.files) {
    const rawPath = file && (file.file || file.file_name);
    const filePath = typeof rawPath === "string" ? reportPath(rawPath, repository, files) : null;
    if (!filePath || filePath === testCase.path || !Array.isArray(file.functions)) continue;
    for (const fn of file.functions) {
      if (!fn || !Number.isSafeInteger(fn.execution_count) || fn.execution_count <= 0 ||
          !Number.isSafeInteger(fn.start_line)) continue;
      const name = typeof fn.demangled_name === "string" ? fn.demangled_name : fn.name;
      const symbol = symbolAtLine(files.get(filePath), fn.start_line, typeof name === "string" ? name : null);
      if (symbol) addHit(result, filePath, symbol, fn.execution_count);
    }
  }
  return result;
}

function compileTestObservations(mapJson, requestJson, options) {
  const repository = path.resolve(options.repository);
  const requestDirectory = path.resolve(options.requestDirectory);
  let mapDocument;
  let requestDocument;
  try {
    mapDocument = JSON.parse(Buffer.from(mapJson).toString("utf8"));
    requestDocument = JSON.parse(Buffer.from(requestJson).toString("utf8"));
  } catch (error) {
    throw new CoverageAdapterError(`invalid JSON input: ${error.message}`);
  }
  const spec = request(requestDocument);
  const indexed = mapIndex(mapDocument);
  const reportBytes = new Map();
  const reports = new Map();
  for (const row of spec.reports) {
    const bytes = fs.readFileSync(path.join(requestDirectory, row.path));
    let document;
    try { document = JSON.parse(bytes.toString("utf8")); } catch (error) {
      throw new CoverageAdapterError(`cannot read coverage report ${row.path}: ${error.message}`);
    }
    if (!document || typeof document !== "object" || Array.isArray(document)) {
      throw new CoverageAdapterError(`coverage report ${row.path} must be an object`);
    }
    reportBytes.set(row.id, bytes);
    reports.set(row.id, document);
  }
  const extractor = { gcov: gcovHits, istanbul: istanbulHits, llvm: llvmHits, v8: v8Hits }[spec.format];
  const outputCases = [];
  const subjects = new Set();
  for (const testCase of spec.cases) {
    if (!indexed.files.has(testCase.path)) throw new CoverageAdapterError(`case test path is not mapped: ${testCase.path}`);
    const hits = extractor(reports.get(testCase.report), testCase, repository, indexed.files);
    if (!hits.size) throw new CoverageAdapterError(`case ${JSON.stringify(testCase.selector)} has no exact mapped symbol hits`);
    const symbols = [...hits.values()].sort((left, right) => (
      utf8Compare(left.path, right.path) || utf8Compare(left.symbol, right.symbol)
    )).map((row) => ({ hits: row.count, path: row.path, symbol: row.symbol }));
    symbols.forEach((row) => subjects.add(row.path));
    outputCases.push({ group: spec.group, path: testCase.path, selector: testCase.selector, symbols });
  }
  const evidence = [];
  for (const [role, paths] of [
    ["runner", spec.runners],
    ["subject", [...subjects].sort(utf8Compare)],
    ["test_inventory", [...new Set(spec.cases.map((row) => row.path))].sort(utf8Compare)],
  ]) {
    for (const filePath of paths) {
      const mapped = indexed.files.get(filePath);
      if (!mapped) throw new CoverageAdapterError(`${role} evidence path is not mapped: ${filePath}`);
      const current = fs.readFileSync(path.join(repository, filePath));
      if (sha256(current) !== mapped.sha256) throw new CoverageAdapterError(`mapped ${role} evidence is stale: ${filePath}`);
      evidence.push({ path: filePath, role, sha256: mapped.sha256 });
    }
  }
  evidence.sort((left, right) => utf8Compare(left.role, right.role) || utf8Compare(left.path, right.path));
  const inputRows = spec.reports.map((row) => ({ id: row.id, sha256: sha256(reportBytes.get(row.id)) }));
  const artifact = {
    artifact: "archbird-test-symbol-observations",
    cases: outputCases,
    producer: {
      configuration_sha256: sha256(Buffer.from(canonical(spec.raw))),
      implementation_sha256: options.implementationSha256,
      input_sha256: sha256(Buffer.from(canonical(inputRows))),
      name: `archbird-${spec.format}-adapter`,
      runtime: `node-${process.versions.node}`,
      version: options.version,
    },
    project: indexed.project,
    provenance: "observed",
    schema_version: 1,
    source: {
      config_sha256: indexed.config,
      evidence,
      evidence_slice_sha256: sha256(Buffer.from(canonical(evidence))),
      map_input_sha256: indexed.inputs,
    },
  };
  return Buffer.from(`${canonical(artifact)}\n`);
}

module.exports = { CoverageAdapterError, compileTestObservations };
