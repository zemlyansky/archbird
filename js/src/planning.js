"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const native = require("./native");
const { validatePlanDocument } = require("./acting");
const { MAX_COLLECTION_ITEMS } = require("./plan-limits");

const SHA256 = /^[0-9a-f]{64}$/;
const STABLE_ID = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$/;

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function assertJson(value, active = new Set()) {
  if (
    value === null ||
    typeof value === "string" ||
    typeof value === "boolean"
  ) return;
  if (typeof value === "number") {
    if (!Number.isFinite(value)) throw new TypeError("non-finite number");
    return;
  }
  if (typeof value !== "object" || Buffer.isBuffer(value)) {
    throw new TypeError("non-JSON value");
  }
  if (active.has(value)) throw new TypeError("cyclic value");
  active.add(value);
  if (Array.isArray(value)) {
    for (const row of value) assertJson(row, active);
  } else {
    for (const [key, row] of Object.entries(value)) {
      if (typeof key !== "string") throw new TypeError("non-string object key");
      assertJson(row, active);
    }
  }
  active.delete(value);
}

function canonicalBytes(value) {
  try {
    assertJson(value);
    const rendered = JSON.stringify(value);
    if (rendered === undefined) throw new TypeError("non-JSON value");
    return native.jsonCanonicalize(Buffer.from(rendered, "utf8"));
  } catch (error) {
    throw new Error("Plan inputs must be JSON values", { cause: error });
  }
}

function sha256Bytes(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function sha256(value) {
  return sha256Bytes(canonicalBytes(value));
}

function implementationDigest() {
  // Plan derivation is host code, so identify these exact frontend sources in
  // addition to the native engine they call.
  const digest = crypto.createHash("sha256");
  digest.update("archbird-node-plan-act-v1\0", "ascii");
  digest.update(native.IMPLEMENTATION_SHA256, "ascii");
  digest.update("\0", "ascii");
  for (const name of ["acting.js", "plan-limits.js", "planning.js"]) {
    digest.update(name, "utf8");
    digest.update("\0", "ascii");
    digest.update(crypto.createHash("sha256")
      .update(fs.readFileSync(path.join(__dirname, name)))
      .digest());
    digest.update("\0", "ascii");
  }
  return digest.digest("hex");
}

function appendPlanRows(items, unknowns, item, rows) {
  if (items.length >= MAX_COLLECTION_ITEMS) {
    throw new Error(`generated Plan exceeds ${MAX_COLLECTION_ITEMS} items`);
  }
  if (
    !Array.isArray(rows) ||
    rows.length > MAX_COLLECTION_ITEMS - unknowns.length
  ) {
    throw new Error(`generated Plan exceeds ${MAX_COLLECTION_ITEMS} unknowns`);
  }
  items.push(item);
  unknowns.push(...rows);
}

function sealedVerificationSha256(document) {
  const supplied = document.verification_result_sha256;
  if (typeof supplied !== "string" || !SHA256.test(supplied)) {
    throw new Error("Verification is missing verification_result_sha256");
  }
  const unsigned = Object.fromEntries(
    Object.entries(document)
      .filter(([key]) => key !== "verification_result_sha256"),
  );
  if (sha256(unsigned) !== supplied) {
    throw new Error("Verification result digest does not match its content");
  }
  return supplied;
}

function requiredString(container, key, description) {
  const value = container[key];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(`${description} is missing ${key}`);
  }
  return value;
}

function requiredSha256(container, key, description) {
  const value = requiredString(container, key, description);
  if (!SHA256.test(value)) {
    throw new Error(`${description} has an invalid ${key}`);
  }
  return value;
}

function sourceIdentity(mapDocument, verificationDocument) {
  const project = requiredString(mapDocument, "project", "Map");
  const mapEvidence = mapDocument.evidence;
  const mapTool = mapDocument.tool;
  const verificationPolicy = verificationDocument.policy;
  const verificationTool = verificationDocument.tool;
  if (!isObject(mapEvidence) || !isObject(mapTool)) {
    throw new Error("Map is missing evidence or producer identity");
  }
  if (!isObject(verificationPolicy) || !isObject(verificationTool)) {
    throw new Error("Verification is missing policy or producer identity");
  }
  const source = {
    project,
    map: {
      sha256: sha256(mapDocument),
      input_sha256: requiredSha256(mapEvidence, "input_sha256", "Map evidence"),
      configuration_sha256: requiredSha256(
        mapEvidence,
        "config_sha256",
        "Map evidence",
      ),
      producer_implementation_sha256: requiredSha256(
        mapTool,
        "implementation_sha256",
        "Map tool",
      ),
    },
    verification: {
      sha256: sealedVerificationSha256(verificationDocument),
      policy_sha256: requiredSha256(
        verificationPolicy,
        "constraint_policy_sha256",
        "Verification policy",
      ),
      producer_implementation_sha256: requiredSha256(
        verificationTool,
        "implementation_sha256",
        "Verification tool",
      ),
    },
  };
  validateCurrentEvaluation(source, verificationDocument);
  return source;
}

function validateCurrentEvaluation(source, verificationDocument) {
  if (!Array.isArray(verificationDocument.evaluations)) {
    throw new Error("Verification is missing Map evaluations");
  }
  const current = verificationDocument.evaluations
    .filter((row) => isObject(row) && row.id === "current");
  if (current.length !== 1) {
    throw new Error("Verification must contain exactly one current Map evaluation");
  }
  const expected = {
    project: source.project,
    map_input_sha256: source.map.input_sha256,
    map_config_sha256: source.map.configuration_sha256,
    map_producer_implementation_sha256:
      source.map.producer_implementation_sha256,
  };
  for (const [key, value] of Object.entries(expected)) {
    if (current[0][key] !== value) {
      throw new Error(`Verification current Map evaluation mismatches ${key}`);
    }
  }
}

function isRepositoryPath(value) {
  if (
    typeof value !== "string" ||
    value.length === 0 ||
    value.length > 4096 ||
    value === "." ||
    value.includes("\\") ||
    value.includes("\0") ||
    value.startsWith("/") ||
    /^[A-Za-z]:/.test(value)
  ) return false;
  const parts = value.split("/");
  return (
    parts.every((part) => part !== "" && part !== "." && part !== "..") &&
    path.posix.normalize(value) === value
  );
}

function safeRepositoryRoot(root) {
  const requested = path.resolve(String(root));
  const metadata = fs.lstatSync(requested);
  if (metadata.isSymbolicLink()) {
    throw new Error("Plan project root must not be a symlink");
  }
  const resolved = fs.realpathSync(requested);
  if (!fs.statSync(resolved).isDirectory()) {
    throw new Error("Plan project root must be an existing directory");
  }
  return resolved;
}

function candidate(root, relative) {
  return path.join(root, ...relative.split("/"));
}

function checkPathComponents(root, relative, includeLeaf) {
  const parts = relative.split("/");
  const limit = includeLeaf ? parts.length : parts.length - 1;
  let cursor = root;
  for (let index = 0; index < limit; index += 1) {
    cursor = path.join(cursor, parts[index]);
    let metadata;
    try {
      metadata = fs.lstatSync(cursor);
    } catch (error) {
      if (error.code === "ENOENT") return;
      throw error;
    }
    if (metadata.isSymbolicLink()) {
      throw new Error(`path traverses a symlink: ${relative}`);
    }
    if (index < limit - 1 && !metadata.isDirectory()) {
      throw new Error(`path parent is not a directory: ${relative}`);
    }
  }
}

function mapFiles(mapDocument) {
  if (!Array.isArray(mapDocument.files)) throw new Error("Map is missing files");
  const index = new Map();
  const duplicates = new Set();
  for (const row of mapDocument.files) {
    if (!isObject(row) || typeof row.path !== "string") continue;
    if (index.has(row.path)) duplicates.add(row.path);
    else index.set(row.path, row);
  }
  return { index, duplicates };
}

function policyResults(verificationDocument) {
  if (
    !isObject(verificationDocument.policy) ||
    !Array.isArray(verificationDocument.policy.constraints)
  ) return new Map();
  return new Map(
    verificationDocument.policy.constraints
      .filter((row) => isObject(row) && typeof row.id === "string")
      .map((row) => [row.id, row]),
  );
}

function origin(constraintId, finding, policyResult) {
  const digest = policyResult?.constraint_result_sha256;
  if (typeof digest !== "string" || !SHA256.test(digest)) {
    throw new Error(
      `Verification policy is missing the result digest for ${constraintId}`,
    );
  }
  const fingerprint = finding ? finding.fingerprint : digest;
  if (typeof fingerprint !== "string" || !SHA256.test(fingerprint)) {
    throw new Error(
      `Verification issue for ${constraintId} has no valid fingerprint`,
    );
  }
  return {
    constraint_id: constraintId,
    constraint_result_sha256: digest,
    issue_fingerprint: fingerprint,
  };
}

function evidence(finding) {
  if (!isObject(finding) || !Array.isArray(finding.evidence)) return [];
  const rows = [];
  const identities = new Set();
  for (const raw of finding.evidence) {
    if (!isObject(raw)) continue;
    const normalized = {};
    for (const key of [
      "provenance",
      "project",
      "path",
      "line",
      "sha256",
      "detail",
    ]) {
      if (Object.hasOwn(raw, key)) normalized[key] = structuredClone(raw[key]);
    }
    if (
      Object.keys(normalized).length !== 6 ||
      !["derived", "asserted", "observed"].includes(normalized.provenance) ||
      typeof normalized.project !== "string" ||
      typeof normalized.path !== "string" ||
      (normalized.path !== "" && !isRepositoryPath(normalized.path)) ||
      !Number.isInteger(normalized.line) ||
      normalized.line < 0 ||
      typeof normalized.sha256 !== "string" ||
      (normalized.sha256 !== "" && !SHA256.test(normalized.sha256)) ||
      typeof normalized.detail !== "string"
    ) continue;
    const identity = canonicalBytes(normalized).toString("hex");
    if (!identities.has(identity)) {
      identities.add(identity);
      rows.push(normalized);
    }
  }
  return rows;
}

function candidatePaths(finding, actualDefinition) {
  const paths = new Set();
  for (const row of evidence(finding)) {
    if (isRepositoryPath(row.path)) paths.add(row.path);
  }
  if (isObject(actualDefinition)) {
    for (const field of ["paths", "source_paths", "target_paths"]) {
      if (!Array.isArray(actualDefinition[field])) continue;
      for (const value of actualDefinition[field]) {
        if (
          isRepositoryPath(value) &&
          ![..."*?[]{}"].some((character) => value.includes(character))
        ) paths.add(value);
      }
    }
  }
  return [...paths].sort(utf8Compare);
}

function constraintForm(constraint, definitions) {
  if (!isObject(constraint.operands)) return ["unsupported", null];
  const actualName = constraint.operands.actual;
  const actual = typeof actualName === "string"
    ? definitions[actualName]
    : null;
  if (!isObject(actual)) return ["unsupported", null];
  const select = actual.select;
  const assertion = constraint.assert;
  if (
    select === "inventory_paths" &&
    assertion === "cardinality" &&
    constraint.operands.exact === 0
  ) return ["forbidden_paths", actual];
  if (select === "mapped_paths" && assertion === "required_subset") {
    return ["required_paths", actual];
  }
  if (select === "symbols" && assertion === "disjoint") {
    return ["forbidden_symbols", actual];
  }
  if (select === "symbols" && assertion === "required_subset") {
    return ["required_symbols", actual];
  }
  if (select === "component_membership") {
    return ["component_membership", actual];
  }
  if (select === "file_metrics" && actual.metric === "bytes") {
    return ["max_file_bytes", actual];
  }
  return [{
    component_edges: "component_edges",
    file_edges: "file_edges",
    package_entrypoints: "required_package_entrypoint",
    provider_surface: "provider_surface",
    test_routes: "test_routes",
  }[String(select)] || "unsupported", actual];
}

function findingIsCurrent(finding) {
  return (
    (finding.applicability ?? "applicable") === "applicable" &&
    (finding.disposition ?? "open") === "open" &&
    (finding.evidence_state ?? "current") === "current"
  );
}

function destructiveRelationFrontier(mapDocument) {
  const definition = {
    id: "plan-destructive-relations",
    select: "graph",
    level: "file",
    relations: [
      "builds",
      "bridges",
      "calls",
      "declarations",
      "imports",
      "packages",
      "references",
      "tests",
    ],
  };
  let result;
  try {
    result = JSON.parse(native.projectionEvaluate(
      canonicalBytes(mapDocument),
      Buffer.alloc(0),
      canonicalBytes(definition),
      false,
    ).toString("utf8"));
  } catch (error) {
    return `Destructive relation projection could not be evaluated: ${error.message}.`;
  }
  const completeness = result.completeness;
  const fact = result.fact;
  if (
    result.artifact !== "projection-result" ||
    !isObject(fact) ||
    fact.state !== "current" ||
    !isObject(completeness) ||
    completeness.classification !== "complete" ||
    completeness.exhaustive !== true ||
    completeness.truncated !== false
  ) {
    return "Destructive relation evidence is not current, complete, " +
      "exhaustive, and untruncated.";
  }
  return null;
}

function operationId(constraintId, finding, operation) {
  return `plan-${sha256({
    constraint_id: constraintId,
    finding_fingerprint: finding ? finding.fingerprint ?? null : null,
    operation,
  }).slice(0, 20)}`;
}

function unknownId(itemId, statement) {
  return `unknown-${sha256({
    item_id: itemId,
    statement,
  }).slice(0, 20)}`;
}

function makeItem({
  constraintId,
  policyResult,
  finding,
  statement,
  operation,
  executable,
  reasons = [],
}) {
  const uniqueReasons = [...new Set(reasons)];
  if (executable && uniqueReasons.length > 0) {
    throw new Error("Executable Plan items cannot have non-executable reasons");
  }
  if (!executable && uniqueReasons.length === 0) {
    throw new Error("Non-executable Plan items require at least one reason");
  }
  const itemId = operationId(constraintId, finding, operation);
  const unknownRows = uniqueReasons.map((reason) => ({
    id: unknownId(itemId, reason),
    statement: reason,
    item_id: itemId,
    constraint_id: constraintId,
  }));
  return [{
    id: itemId,
    statement,
    provenance: "derived",
    origins: [origin(constraintId, finding, policyResult)],
    evidence: evidence(finding),
    depends_on: [],
    operation: structuredClone(operation),
    acceptance: { constraints: [constraintId] },
    unknowns: unknownRows.map((row) => row.id),
    executable,
    non_executable_reasons: uniqueReasons,
  }, unknownRows];
}

function pathLock(filePath, files, mapDocument) {
  const hashes = new Set();
  const mapped = files.get(filePath);
  if (isObject(mapped) && typeof mapped.sha256 === "string") {
    hashes.add(mapped.sha256);
  }
  for (const row of Array.isArray(mapDocument.inputs) ? mapDocument.inputs : []) {
    if (
      isObject(row) &&
      row.path === filePath &&
      typeof row.sha256 === "string"
    ) {
      hashes.add(row.sha256);
    }
  }
  const valid = [...hashes].filter((value) => SHA256.test(value));
  if (valid.length === 0) {
    return [null, `No source SHA-256 is available for ${filePath}.`];
  }
  if (valid.length !== 1) {
    return [
      null,
      `Map source rows disagree on the source hash for ${filePath}.`,
    ];
  }
  return [valid[0], null];
}

function readLockedSource(root, filePath, expectedSha256) {
  if (!isRepositoryPath(filePath)) {
    return [
      null,
      `Repository path ${JSON.stringify(filePath)} is unsafe or escapes the project root.`,
    ];
  }
  try {
    checkPathComponents(root, filePath, true);
    const target = candidate(root, filePath);
    const metadata = fs.lstatSync(target);
    if (metadata.isSymbolicLink() || !metadata.isFile()) {
      return [null, `Source path ${filePath} is absent or is not a regular file.`];
    }
    const source = fs.readFileSync(target);
    if (sha256Bytes(source) !== expectedSha256) {
      return [
        null,
        `Source path ${filePath} no longer matches the Map source hash.`,
      ];
    }
    return [source, null];
  } catch (error) {
    if (error.code === "ENOENT") {
      return [null, `Source path ${filePath} is absent or is not a regular file.`];
    }
    return [null, `Source path ${filePath} cannot be read: ${error.message}.`];
  }
}

function candidateTargetsPath(value, filePath) {
  return (
    value === filePath ||
    (isObject(value) && value.path === filePath)
  );
}

function fileConsumers(mapDocument, filePath) {
  const consumers = new Set();
  for (const edge of Array.isArray(mapDocument.edges) ? mapDocument.edges : []) {
    if (
      isObject(edge) &&
      edge.target === filePath &&
      edge.source !== filePath &&
      typeof edge.source === "string"
    ) consumers.add(edge.source);
  }
  for (const collection of [
    "call_resolutions",
    "symbol_calls",
    "symbol_references",
  ]) {
    for (const row of Array.isArray(mapDocument[collection])
      ? mapDocument[collection] : []) {
      if (
        !isObject(row) ||
        !Array.isArray(row.candidates) ||
        !row.candidates.some((value) => candidateTargetsPath(value, filePath))
      ) continue;
      const sourcePath = isObject(row.source)
        ? row.source.path
        : typeof row.source === "string" ? row.source : null;
      if (typeof sourcePath === "string" && sourcePath !== filePath) {
        consumers.add(sourcePath);
      }
    }
  }
  for (const build of Array.isArray(mapDocument.builds) ? mapDocument.builds : []) {
    if (!isObject(build)) continue;
    const label = typeof build.name === "string" && build.name
      ? build.name : "unknown";
    if (build.source === filePath) consumers.add(`build:${label}:definition`);
    if (Array.isArray(build.paths) && build.paths.includes(filePath)) {
      consumers.add(`build:${label}:path`);
    }
    if (Array.isArray(build.deps) && build.deps.includes(filePath)) {
      consumers.add(`build:${label}:dependency`);
    }
  }
  for (const artifact of Array.isArray(mapDocument.artifacts)
    ? mapDocument.artifacts : []) {
    if (!isObject(artifact)) continue;
    const label = typeof artifact.name === "string" && artifact.name
      ? artifact.name : "unknown";
    if (artifact.output === filePath) consumers.add(`artifact:${label}:output`);
    for (const input of Array.isArray(artifact.inputs) ? artifact.inputs : []) {
      if (isObject(input) && input.path === filePath) {
        consumers.add(`artifact:${label}:input`);
      }
    }
    for (const loader of Array.isArray(artifact.loaded_by)
      ? artifact.loaded_by : []) {
      if (isObject(loader) && loader.path === filePath) {
        consumers.add(`artifact:${label}:loader`);
      }
    }
    for (const build of Array.isArray(artifact.builds) ? artifact.builds : []) {
      if (!isObject(build)) continue;
      if (build.source === filePath) {
        consumers.add(`artifact:${label}:build-definition`);
      }
      if (build.target === filePath) {
        consumers.add(`artifact:${label}:build-target`);
      }
    }
  }
  for (const surface of Array.isArray(mapDocument.surfaces)
    ? mapDocument.surfaces : []) {
    if (!isObject(surface)) continue;
    const label = typeof surface.name === "string" && surface.name
      ? surface.name : "unknown";
    for (const provider of Array.isArray(surface.providers)
      ? surface.providers : []) {
      if (isObject(provider) && provider.path === filePath) {
        consumers.add(`bridge:${label}:provider`);
      }
    }
    for (const surfaceName of Array.isArray(surface.names)
      ? surface.names : []) {
      if (!isObject(surfaceName)) continue;
      if (
        Array.isArray(surfaceName.candidates) &&
        surfaceName.candidates.includes(filePath)
      ) consumers.add(`bridge:${label}:implementation`);
      for (const declaration of Array.isArray(surfaceName.declarations)
        ? surfaceName.declarations : []) {
        if (isObject(declaration) && declaration.path === filePath) {
          consumers.add(`bridge:${label}:declaration`);
        }
      }
      for (const use of Array.isArray(surfaceName.uses)
        ? surfaceName.uses : []) {
        if (isObject(use) && use.path === filePath) {
          consumers.add(`bridge:${label}:use`);
        }
      }
    }
  }
  if (isObject(mapDocument.named_entries)) {
    for (const [name, entries] of Object.entries(mapDocument.named_entries)) {
      if (isObject(entries) && Object.hasOwn(entries, filePath)) {
        consumers.add(`named-entry:${name}`);
      }
    }
  }
  for (const packageRow of Array.isArray(mapDocument.packages)
    ? mapDocument.packages : []) {
    if (!isObject(packageRow)) continue;
    const label = typeof packageRow.name === "string" && packageRow.name
      ? packageRow.name : "unknown";
    if (packageRow.manifest === filePath) {
      consumers.add(`package:${label}:manifest`);
    }
    if (
      isObject(packageRow.entrypoints) &&
      Object.values(packageRow.entrypoints).includes(filePath)
    ) consumers.add(`package:${label}:entrypoint`);
    if (
      isObject(packageRow.export_origins) &&
      Object.values(packageRow.export_origins).some(
        (paths) => Array.isArray(paths) && paths.includes(filePath),
      )
    ) consumers.add(`package:${label}:export-origin`);
    for (const surface of Array.isArray(packageRow.entrypoint_surfaces)
      ? packageRow.entrypoint_surfaces : []) {
      if (!isObject(surface)) continue;
      if (surface.path === filePath) {
        consumers.add(`package:${label}:entrypoint-surface`);
      }
      if (
        isObject(surface.export_origins) &&
        Object.values(surface.export_origins).some(
          (paths) => Array.isArray(paths) && paths.includes(filePath),
        )
      ) {
        consumers.add(`package:${label}:surface-export-origin`);
      }
    }
  }
  for (const test of Array.isArray(mapDocument.tests) ? mapDocument.tests : []) {
    if (!isObject(test)) continue;
    const testPath = test.path;
    const label = typeof testPath === "string" && testPath
      ? testPath : "unknown";
    if (testPath === filePath) consumers.add(`test:${label}:file`);
    if (
      Array.isArray(test.generated_from) &&
      test.generated_from.includes(filePath)
    ) consumers.add(`test:${label}:generated-from`);
    if (isObject(test.routes) && Object.hasOwn(test.routes, filePath)) {
      consumers.add(`test:${label}:route`);
    }
    for (const route of Array.isArray(test.route_evidence)
      ? test.route_evidence : []) {
      if (isObject(route) && route.target === filePath) {
        consumers.add(`test:${label}:route-evidence`);
      }
    }
    for (const testCase of Array.isArray(test.cases) ? test.cases : []) {
      if (!isObject(testCase)) continue;
      const caseLabel = typeof testCase.selector === "string" && testCase.selector
        ? testCase.selector : "unknown";
      if (
        Array.isArray(testCase.configured_routes) &&
        testCase.configured_routes.includes(filePath)
      ) consumers.add(`test:${label}:${caseLabel}:configured-route`);
      if (
        isObject(testCase.routes) &&
        Object.hasOwn(testCase.routes, filePath)
      ) consumers.add(`test:${label}:${caseLabel}:route`);
      for (const route of Array.isArray(testCase.route_evidence)
        ? testCase.route_evidence : []) {
        if (isObject(route) && route.target === filePath) {
          consumers.add(`test:${label}:${caseLabel}:route-evidence`);
        }
      }
    }
  }
  const escapedPath = filePath.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const parityPath = new RegExp(`@${escapedPath}(?::[0-9]+)?$`);
  for (const parity of Array.isArray(mapDocument.parity)
    ? mapDocument.parity : []) {
    if (!isObject(parity)) continue;
    const label = typeof parity.name === "string" && parity.name
      ? parity.name : "unknown";
    for (const member of Array.isArray(parity.members) ? parity.members : []) {
      if (!isObject(member) || !isObject(member.evidence)) continue;
      const matches = Object.values(member.evidence).some(
        (locations) => Array.isArray(locations) && locations.some(
          (location) => typeof location === "string" && parityPath.test(location),
        ),
      );
      if (matches) {
        const memberLabel = typeof member.label === "string" && member.label
          ? member.label : "unknown";
        consumers.add(`parity:${label}:${memberLabel}`);
      }
    }
  }
  return [...consumers].sort(utf8Compare);
}

function symbolConsumers(mapDocument, filePath, name) {
  const consumers = new Set();
  for (const row of Array.isArray(mapDocument.files) ? mapDocument.files : []) {
    if (
      isObject(row) &&
      row.path === filePath &&
      Array.isArray(row.exports) &&
      row.exports.includes(name)
    ) consumers.add(`${filePath}:export`);
  }
  for (const collection of ["symbol_calls", "symbol_references"]) {
    for (const row of Array.isArray(mapDocument[collection])
      ? mapDocument[collection] : []) {
      if (
        !isObject(row) ||
        !Array.isArray(row.candidates) ||
        !row.candidates.some((value) => (
          isObject(value) &&
          value.path === filePath &&
          value.symbol === name
        ))
      ) continue;
      const sourcePath = isObject(row.source) ? row.source.path : null;
      const sourceSymbol = isObject(row.source) ? row.source.symbol : null;
      if (
        collection === "symbol_references" &&
        sourcePath === filePath &&
        sourceSymbol === name
      ) {
        for (const value of row.candidates) {
          if (isObject(value) && typeof value.path === "string") {
            consumers.add(`${value.path}:${value.symbol ?? name}`);
          }
        }
        continue;
      }
      if (sourcePath === filePath && sourceSymbol === name) continue;
      if (typeof sourcePath === "string") {
        consumers.add(
          typeof sourceSymbol === "string"
            ? `${sourcePath}:${sourceSymbol}`
            : sourcePath,
        );
      }
    }
  }
  for (const surface of Array.isArray(mapDocument.surfaces)
    ? mapDocument.surfaces : []) {
    if (!isObject(surface) || !Array.isArray(surface.names)) continue;
    for (const row of surface.names) {
      if (
        isObject(row) &&
        row.name === name &&
        Array.isArray(row.candidates) &&
        row.candidates.includes(filePath)
      ) consumers.add(`surface:${surface.name ?? "unknown"}`);
    }
  }
  return [...consumers].sort(utf8Compare);
}

function manualItem({
  constraintId,
  policyResult,
  finding,
  statement,
  instructions,
  reasons,
  candidatePaths: paths = [],
}) {
  return makeItem({
    constraintId,
    policyResult,
    finding,
    statement,
    operation: {
      action: "manual",
      instructions,
      candidate_paths: [...new Set(paths.filter(isRepositoryPath))]
        .sort(utf8Compare),
    },
    executable: false,
    reasons,
  });
}

function forbiddenPathItem({
  root,
  mapDocument,
  files,
  duplicatePaths,
  constraintId,
  policyResult,
  finding,
  relationFrontierError,
}) {
  const key = finding.key;
  if (typeof key !== "string" || key.length === 0) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove the path forbidden by ${constraintId}.`,
      instructions: "Identify the exact forbidden path and remove it.",
      reasons: ["Verification did not identify one exact forbidden path."],
    });
  }
  if (!isRepositoryPath(key)) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove the unsafe path reported by ${constraintId}.`,
      instructions:
        "Resolve the unsafe path evidence without using it as a filesystem path.",
      reasons: [
        `Verification path ${JSON.stringify(key)} is not repository-relative.`,
      ],
    });
  }
  const [sourceSha256, lockError] = pathLock(key, files, mapDocument);
  const reasons = [];
  if (duplicatePaths.has(key)) {
    reasons.push(`Map contains duplicate file rows for ${key}.`);
  }
  if (lockError) reasons.push(lockError);
  if (relationFrontierError) reasons.push(relationFrontierError);
  if (sourceSha256) {
    const [, readError] = readLockedSource(root, key, sourceSha256);
    if (readError) reasons.push(readError);
  }
  const consumers = fileConsumers(mapDocument, key);
  if (consumers.length > 0) {
    reasons.push(
      `Known consumers of ${key} require a reviewed rewrite: ` +
      `${consumers.join(", ")}.`,
    );
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove forbidden path ${key}.`,
      instructions:
        `Rewrite or remove the named consumers of ${key}, then remove ` +
        "the source-locked path.",
      reasons,
      candidatePaths: [key],
    });
  }
  if (!sourceSha256) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove forbidden path ${key}.`,
      instructions: `Remove ${key} after establishing an exact source lock.`,
      reasons,
      candidatePaths: [key],
    });
  }
  return makeItem({
    constraintId,
    policyResult,
    finding,
    statement: `Delete forbidden path ${key}.`,
    operation: {
      action: "delete_file",
      path: key,
      source_sha256: sourceSha256,
    },
    executable: reasons.length === 0,
    reasons,
  });
}

function forbiddenPathItems({
  root,
  mapDocument,
  verificationDocument,
  files,
  duplicatePaths,
  constraint,
  policyResult,
  relationFrontierError,
}) {
  const constraintId = constraint.id;
  const actualName = isObject(constraint.operands)
    ? constraint.operands.actual
    : null;
  const actualMatches = (
    Array.isArray(verificationDocument.operands) &&
    typeof actualName === "string"
  ) ? verificationDocument.operands.filter(
      (row) => isObject(row) && row.name === actualName,
    ) : [];
  const findingRows = Array.isArray(constraint.findings)
    ? constraint.findings.filter(isObject)
    : [];
  const fallbackFinding = findingRows[0] || null;

  function manual(reason, paths = []) {
    return [manualItem({
      constraintId,
      policyResult,
      finding: fallbackFinding,
      statement: `Remove paths forbidden by ${constraintId}.`,
      instructions:
        "Review the exhaustive inventory-path operand and provide " +
        "source-locked deletions for each forbidden path.",
      reasons: [reason],
      candidatePaths: paths,
    })];
  }

  if (actualMatches.length !== 1) {
    return manual(
      "Verification does not contain one exact actual ProjectionResult " +
      `for ${constraintId}.`,
    );
  }
  const actual = actualMatches[0];
  const completeness = actual.completeness;
  if (
    actual.state !== "current" ||
    actual.shape !== "set" ||
    !isObject(completeness) ||
    completeness.classification !== "complete" ||
    completeness.exhaustive !== true ||
    completeness.truncated !== false
  ) {
    return manual(
      "The forbidden-path actual ProjectionResult is not current, " +
      "complete, exhaustive, and untruncated.",
    );
  }
  if (typeof actual.sha256 !== "string" || !SHA256.test(actual.sha256)) {
    return manual(
      "The forbidden-path actual ProjectionResult has no identity.",
    );
  }
  const correlated = findingRows.filter((finding) => (
    findingIsCurrent(finding) &&
    evidence(finding).some((row) => (
      row.sha256 === actual.sha256 ||
      (
        typeof row.detail === "string" &&
        typeof actualName === "string" &&
        row.detail.includes(actualName)
      )
    ))
  ));
  if (correlated.length !== 1) {
    return manual(
      "Verification does not contain one current issue correlated to the " +
      "forbidden-path actual ProjectionResult.",
    );
  }
  const finding = correlated[0];
  if (!Array.isArray(actual.items) || actual.items.length === 0) {
    return manual(
      "The failing forbidden-path ProjectionResult contains no path items.",
    );
  }
  const concrete = [];
  for (const row of actual.items) {
    if (
      !isObject(row) ||
      row.state !== "current" ||
      !isRepositoryPath(row.key)
    ) {
      return manual(
        "The forbidden-path ProjectionResult contains a non-current or " +
        "non-concrete path item.",
      );
    }
    const itemFinding = {
      ...finding,
      evidence: [
        ...(Array.isArray(row.evidence) ? row.evidence : []),
        ...(Array.isArray(finding.evidence) ? finding.evidence : []),
      ],
    };
    const itemEvidence = evidence(itemFinding);
    if (!itemEvidence.some((item) => item.path === row.key)) {
      return manual(
        `Projection item ${row.key} has no directly correlated path evidence.`,
        [row.key],
      );
    }
    concrete.push([row.key, itemEvidence]);
  }
  const paths = concrete.map(([filePath]) => filePath);
  if (new Set(paths).size !== paths.length) {
    return manual(
      "The forbidden-path ProjectionResult contains duplicate path items.",
      paths,
    );
  }
  return concrete
    .sort(([left], [right]) => utf8Compare(left, right))
    .map(([filePath, itemEvidence]) => forbiddenPathItem({
      root,
      mapDocument,
      files,
      duplicatePaths,
      constraintId,
      policyResult,
      finding: {
        ...finding,
        key: filePath,
        evidence: itemEvidence,
      },
      relationFrontierError,
    }));
}

function symbolCandidates(files, finding, name) {
  const evidencePaths = new Set(
    evidence(finding).map((row) => row.path).filter(Boolean),
  );
  const candidates = [];
  for (const [filePath, fileRow] of files) {
    if (evidencePaths.size > 0 && !evidencePaths.has(filePath)) continue;
    if (!Array.isArray(fileRow.symbols)) continue;
    for (const symbol of fileRow.symbols) {
      if (isObject(symbol) && symbol.name === name) {
        candidates.push([filePath, fileRow, symbol]);
      }
    }
  }
  return candidates;
}

function decodeUtf8(value) {
  return new TextDecoder("utf-8", { fatal: true }).decode(value);
}

function forbiddenSymbolItem({
  root,
  mapDocument,
  files,
  duplicatePaths,
  constraintId,
  policyResult,
  finding,
  relationFrontierError,
}) {
  const name = finding.key;
  if (typeof name !== "string" || name.length === 0) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove the symbol forbidden by ${constraintId}.`,
      instructions: "Identify and remove the exact forbidden declaration.",
      reasons: ["Verification did not identify one exact forbidden symbol."],
    });
  }
  const candidates = symbolCandidates(files, finding, name);
  if (candidates.length !== 1) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove forbidden symbol ${name}.`,
      instructions:
        `Choose and remove every declaration of forbidden symbol ${name}.`,
      reasons: [
        `Expected one exact declaration extent for ${name}, found ` +
        `${candidates.length}.`,
      ],
      candidatePaths: [...new Set(candidates.map(([filePath]) => filePath))]
        .sort(utf8Compare),
    });
  }
  const [filePath, , symbol] = candidates[0];
  const reasons = [];
  const extent = symbol.extent;
  if (
    !isObject(extent) ||
    !Number.isInteger(extent.start) ||
    !Number.isInteger(extent.end) ||
    extent.start < 0 ||
    extent.end <= extent.start
  ) reasons.push(`Map has no exact declaration extent for ${name}.`);
  if (symbol.syntax_recovery) {
    reasons.push(`Declaration ${name} was recovered from invalid syntax.`);
  }
  if (name.includes(".") || symbol.kind === "method") {
    reasons.push(
      `Removing nested declaration ${name} requires a contextual syntax rewrite.`,
    );
  }
  if (relationFrontierError) reasons.push(relationFrontierError);
  if (duplicatePaths.has(filePath)) {
    reasons.push(`Map contains duplicate file rows for ${filePath}.`);
  }
  const [sourceSha256, lockError] = pathLock(filePath, files, mapDocument);
  if (lockError) reasons.push(lockError);
  let source = null;
  if (sourceSha256) {
    const [readSource, readError] = readLockedSource(
      root,
      filePath,
      sourceSha256,
    );
    source = readSource;
    if (readError) reasons.push(readError);
  }
  const consumers = symbolConsumers(mapDocument, filePath, name);
  if (consumers.length > 0) {
    reasons.push(
      `Known consumers of ${name} require a reviewed rewrite: ` +
      `${consumers.join(", ")}.`,
    );
  }
  let before = null;
  if (
    source !== null &&
    isObject(extent) &&
    Number.isInteger(extent.start) &&
    Number.isInteger(extent.end) &&
    extent.start >= 0 &&
    extent.end <= source.length
  ) {
    try {
      before = decodeUtf8(source.subarray(extent.start, extent.end));
    } catch (_) {
      reasons.push(`Declaration extent for ${name} is not valid UTF-8 text.`);
    }
  } else if (isObject(extent)) {
    reasons.push(`Declaration extent for ${name} exceeds ${filePath}.`);
  }
  if (before === null || sourceSha256 === null || !isObject(extent)) {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Remove forbidden symbol ${name}.`,
      instructions:
        `Remove ${name} after selecting one complete, source-locked ` +
        "declaration extent and reviewing its consumers.",
      reasons: [...new Set(reasons)],
      candidatePaths: [filePath],
    });
  }
  return makeItem({
    constraintId,
    policyResult,
    finding,
    statement: `Remove forbidden symbol ${name} from ${filePath}.`,
    operation: {
      action: "replace_range",
      path: filePath,
      source_sha256: sourceSha256,
      start_byte: extent.start,
      end_byte: extent.end,
      before,
      replacement: "",
    },
    executable: reasons.length === 0,
    reasons: [...new Set(reasons)],
  });
}

function nonExecutableItem({
  form,
  constraintId,
  policyResult,
  finding,
  actualDefinition,
}) {
  const key = finding?.key;
  const subject = typeof key === "string" && key.length > 0
    ? key
    : constraintId;
  const paths = candidatePaths(finding, actualDefinition);
  if (form === "required_paths") {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Create required path ${subject}.`,
      instructions:
        `Provide reviewed content for ${subject}, then replace this manual ` +
        "operation with create_file.",
      reasons: [
        `Verification requires ${subject} but does not specify file content.`,
      ],
      candidatePaths: typeof subject === "string" ? [subject] : [],
    });
  }
  if (form === "required_symbols") {
    return manualItem({
      constraintId,
      policyResult,
      finding,
      statement: `Add required symbol ${subject}.`,
      instructions:
        `Provide the signature, implementation, and destination for ${subject}.`,
      reasons: [
        `Verification requires ${subject} but does not define its code.`,
      ],
      candidatePaths: paths,
    });
  }
  const details = {
    component_membership: [
      "Select an exact file move or an exact archbird.json component-path " +
      "edit after reviewing ownership overlaps.",
      "Component membership evidence does not uniquely determine a file " +
      "move or configuration edit.",
    ],
    max_file_bytes: [
      "Select declarations to extract or simplify, including their " +
      "destination and reference rewrites.",
      "A file-size violation does not determine a behavior-preserving edit.",
    ],
    file_edges: [
      "Select a replacement dependency route and provide exact source rewrites.",
      "Dependency evidence does not identify the intended replacement route.",
    ],
    component_edges: [
      "Select a replacement dependency route and provide exact source rewrites.",
      "Dependency evidence does not identify the intended replacement route.",
    ],
    required_package_entrypoint: [
      "Select the package manifest entrypoint edit and its reviewed target.",
      "Entrypoint evidence does not provide an exact manifest edit.",
    ],
    provider_surface: [
      "Provide the reviewed bridge or registration transformation for this provider surface.",
      "Provider evidence does not define bridge code or registration syntax.",
    ],
    test_routes: [
      "Provide the reviewed test route or test template to add.",
      "Route evidence does not define test code or registration syntax.",
    ],
  }[form] || [
    "Review the Verification evidence and provide a source-locked transformation.",
    "This compiled constraint form has no deterministic edit operator.",
  ];
  return manualItem({
    constraintId,
    policyResult,
    finding,
    statement: `Resolve ${constraintId}: ${subject}.`,
    instructions: details[0],
    reasons: [details[1]],
    candidatePaths: paths,
  });
}

function generatePlan(mapDocument, verificationDocument, constraintIds, root) {
  if (!isObject(mapDocument) || mapDocument.artifact !== "map") {
    throw new Error("generatePlan requires a Map artifact");
  }
  if (
    !isObject(verificationDocument) ||
    verificationDocument.artifact !== "verification"
  ) {
    throw new Error("generatePlan requires a Verification artifact");
  }
  const repository = safeRepositoryRoot(root);
  const source = sourceIdentity(mapDocument, verificationDocument);
  const constraints = verificationDocument.constraints;
  const definitions = verificationDocument.operand_definitions;
  if (!Array.isArray(constraints) || !isObject(definitions)) {
    throw new Error("Verification is missing constraints or operand definitions");
  }
  const constraintRows = constraints.filter(
    (row) => isObject(row) && typeof row.id === "string",
  );
  const checks = new Map(constraintRows.map((row) => [row.id, row]));
  if (checks.size !== constraints.length) {
    throw new Error(
      "Verification constraints must have unique non-empty string IDs",
    );
  }
  const invalidIds = [...checks.keys()]
    .filter((identifier) => !STABLE_ID.test(identifier))
    .sort(utf8Compare);
  if (invalidIds.length > 0) {
    throw new Error(
      `Verification contains invalid constraint IDs: ${invalidIds.join(", ")}`,
    );
  }

  let requested;
  if (constraintIds === undefined || constraintIds === null) {
    requested = [...checks.keys()];
  } else {
    if (!Array.isArray(constraintIds)) {
      throw new Error("constraintIds must be a sequence of IDs");
    }
    requested = [...new Set(constraintIds)];
    if (requested.some((item) => typeof item !== "string" || item.length === 0)) {
      throw new Error("constraintIds must contain non-empty strings");
    }
    const missing = requested.filter((item) => !checks.has(item)).sort(utf8Compare);
    if (missing.length > 0) {
      throw new Error(
        `Verification does not contain requested constraints: ${missing.join(", ")}`,
      );
    }
  }
  const requestedSet = new Set(requested);
  const orderedChecks = constraintRows
    .filter((row) => requestedSet.has(row.id));
  const { index: files, duplicates: duplicatePaths } = mapFiles(mapDocument);
  const policies = policyResults(verificationDocument);
  if (
    policies.size !== checks.size ||
    [...checks.keys()].some((identifier) => !policies.has(identifier))
  ) {
    throw new Error(
      "Verification policy results do not match evaluated constraints",
    );
  }
  for (const [identifier, policy] of policies) {
    if (
      typeof policy?.constraint_result_sha256 !== "string" ||
      !SHA256.test(policy.constraint_result_sha256)
    ) {
      throw new Error(
        "Verification policy has an invalid result digest for " + identifier,
      );
    }
  }
  const items = [];
  const unknowns = [];
  const relationFrontierError = destructiveRelationFrontier(mapDocument);

  for (const constraint of orderedChecks) {
    const constraintId = constraint.id;
    if (["pass", "waived", "not_applicable"].includes(constraint.status)) {
      continue;
    }
    const [form, actualDefinition] = constraintForm(constraint, definitions);
    if (form === "forbidden_paths") {
      for (const [item, rows] of forbiddenPathItems({
        root: repository,
        mapDocument,
        verificationDocument,
        files,
        duplicatePaths,
        constraint,
        policyResult: policies.get(constraintId),
        relationFrontierError,
      })) {
        appendPlanRows(items, unknowns, item, rows);
      }
      continue;
    }
    const findingRows = Array.isArray(constraint.findings)
      ? constraint.findings.filter(isObject)
      : [];
    if (findingRows.length === 0) {
      const [item, rows] = nonExecutableItem({
        form,
        constraintId,
        policyResult: policies.get(constraintId),
        finding: null,
        actualDefinition,
      });
      appendPlanRows(items, unknowns, item, rows);
      continue;
    }
    for (const finding of findingRows) {
      let item;
      let rows;
      if (form === "forbidden_symbols" && findingIsCurrent(finding)) {
        [item, rows] = forbiddenSymbolItem({
          root: repository,
          mapDocument,
          files,
          duplicatePaths,
          constraintId,
          policyResult: policies.get(constraintId),
          finding,
          relationFrontierError,
        });
      } else {
        [item, rows] = nonExecutableItem({
          form,
          constraintId,
          policyResult: policies.get(constraintId),
          finding,
          actualDefinition,
        });
        if (!findingIsCurrent(finding)) {
          const reason =
            "Finding evidence is waived, stale, inapplicable, or otherwise " +
            "not current executable evidence.";
          if (!item.non_executable_reasons.includes(reason)) {
            item.non_executable_reasons.push(reason);
          }
          const identifier = unknownId(item.id, reason);
          item.unknowns.push(identifier);
          rows.push({
            id: identifier,
            statement: reason,
            item_id: item.id,
            constraint_id: constraintId,
          });
        }
      }
      appendPlanRows(items, unknowns, item, rows);
    }
  }

  const plan = {
    schema_version: 1,
    artifact: "plan",
    provenance: "derived",
    tool: {
      name: "archbird",
      version: native.VERSION,
      implementation_sha256: implementationDigest(),
    },
    source,
    objective:
      "Satisfy the selected Verification constraints without regressing " +
      "preserved constraints.",
    items,
    preserved_constraints: [...checks.keys()].filter((identifier) =>
      !items.some((item) => item.acceptance.constraints.includes(identifier))
    ),
    unknowns,
  };
  validatePlanDocument(plan);
  return plan;
}

module.exports = {
  generatePlan,
};
