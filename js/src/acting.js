"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const native = require("./native");
const {
  MAX_CHANGE_PATCH_BYTES,
  MAX_COLLECTION_ITEMS,
  MAX_FILE_BYTES,
  MAX_METADATA_BYTES,
  MAX_OPERATION_TEXT_BYTES,
  MAX_PATCH_BYTES,
  MAX_PLAN_BYTES,
  MAX_TOUCHED_FILES,
  MAX_TOUCHED_SOURCE_BYTES,
} = require("./plan-limits");

const SHA256 = /^[0-9a-f]{64}$/;
const STABLE_ID = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$/;
const SUPPORTED_ACTIONS = new Set([
  "replace_range",
  "create_file",
  "delete_file",
  "move_file",
  "edit_json_pointer",
  "edit_make_variable_token",
  "insert_make_variable_token",
  "rename_symbol",
]);
const PORTABLE_IDENTIFIER = /^[A-Za-z_][A-Za-z0-9_]*$/;
const RENAME_ROLES = new Set([
  "binding",
  "declaration",
  "export",
  "import",
  "reference",
]);

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
    if (value.length > MAX_COLLECTION_ITEMS) {
      throw new TypeError(
        `JSON array exceeds ${MAX_COLLECTION_ITEMS} items`,
      );
    }
    for (const row of value) assertJson(row, active);
  } else {
    for (const row of Object.values(value)) assertJson(row, active);
  }
  active.delete(value);
}

function canonicalBytes(value, maximum = null) {
  assertJson(value);
  const rendered = JSON.stringify(value);
  if (rendered === undefined) throw new TypeError("non-JSON value");
  const encoded = Buffer.from(rendered, "utf8");
  if (maximum !== null && encoded.length > maximum) {
    throw new TypeError(`Plan document exceeds ${maximum} UTF-8 bytes`);
  }
  return native.jsonCanonicalize(encoded);
}

function digest(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function planSha256(plan) {
  return digest(canonicalBytes(plan, MAX_PLAN_BYTES));
}

function implementationDigest() {
  // This is a frontend implementation identity. It deliberately is not the
  // native core identity because Plan derivation and Act execution live here.
  const hash = crypto.createHash("sha256");
  hash.update("archbird-node-plan-act-v1\0", "ascii");
  hash.update(native.IMPLEMENTATION_SHA256, "ascii");
  hash.update("\0", "ascii");
  for (const name of ["acting.js", "plan-limits.js", "planning.js"]) {
    hash.update(name, "utf8");
    hash.update("\0", "ascii");
    hash.update(crypto.createHash("sha256")
      .update(fs.readFileSync(path.join(__dirname, name)))
      .digest());
    hash.update("\0", "ascii");
  }
  return hash.digest("hex");
}

function boundedMessage(value) {
  const encoded = Buffer.from(String(value), "utf8");
  if (encoded.length <= MAX_METADATA_BYTES) return encoded.toString("utf8");
  const suffix = Buffer.from("... [truncated]", "ascii");
  let end = MAX_METADATA_BYTES - suffix.length;
  while (end > 0 && (encoded[end] & 0xc0) === 0x80) end -= 1;
  return Buffer.concat([encoded.subarray(0, end), suffix]).toString("utf8");
}

function diagnostic(code, message, itemId = null, filePath = null) {
  return {
    code,
    severity: "error",
    message: boundedMessage(message),
    item_id: itemId || null,
    path: filePath || null,
  };
}

function result(
  planDigest,
  status,
  changes,
  diagnostics,
  acceptance = null,
) {
  let boundedDiagnostics = diagnostics;
  if (boundedDiagnostics.length > MAX_COLLECTION_ITEMS) {
    boundedDiagnostics = [
      ...boundedDiagnostics.slice(0, MAX_COLLECTION_ITEMS - 1),
      diagnostic(
        "resource_limit",
        `diagnostics exceed ${MAX_COLLECTION_ITEMS} records`,
      ),
    ];
  }
  const orderedDiagnostics = [...boundedDiagnostics].sort((left, right) => {
    for (const key of ["item_id", "path", "code", "message"]) {
      const comparison = utf8Compare(left[key] || "", right[key] || "");
      if (comparison !== 0) return comparison;
    }
    return 0;
  });
  return {
    schema_version: 1,
    artifact: "act-result",
    provenance: "derived",
    tool: {
      name: "archbird",
      version: native.VERSION,
      implementation_sha256: implementationDigest(),
    },
    plan_sha256: planDigest,
    status,
    changes,
    acceptance: acceptance || {
      status: "not_evaluated",
      verification_sha256: null,
      constraints: [],
    },
    diagnostics: orderedDiagnostics,
  };
}

function exactKeys(value, expected, description) {
  const actual = new Set(Object.keys(value));
  const missing = [...expected].filter((key) => !actual.has(key)).sort(utf8Compare);
  const extra = [...actual].filter((key) => !expected.has(key)).sort(utf8Compare);
  if (missing.length > 0 || extra.length > 0) {
    const detail = [];
    if (missing.length > 0) detail.push(`missing ${missing.join(", ")}`);
    if (extra.length > 0) detail.push(`unexpected ${extra.join(", ")}`);
    throw new Error(`${description} has ${detail.join(", ")}`);
  }
}

function validId(value) {
  return typeof value === "string" && STABLE_ID.test(value);
}

function validSha256(value) {
  return typeof value === "string" && SHA256.test(value);
}

function validateText(
  value,
  description,
  maximum,
  { nonempty = false } = {},
) {
  if (
    typeof value !== "string" ||
    (nonempty && value.length === 0) ||
    Buffer.byteLength(value, "utf8") > maximum
  ) {
    throw new Error(
      `${description} must be${nonempty ? " non-empty" : ""} text no larger ` +
      `than ${maximum} UTF-8 bytes`,
    );
  }
  return value;
}

function validateIds(value, description, { nonempty = false } = {}) {
  if (
    !Array.isArray(value) ||
    value.length > MAX_COLLECTION_ITEMS ||
    (nonempty && value.length === 0) ||
    value.some((row) => !validId(row)) ||
    new Set(value).size !== value.length
  ) throw new Error(`${description} must contain unique stable IDs`);
  return value;
}

function canonicalIdentities(values, description) {
  const identities = new Set();
  for (const value of values) {
    const identity = canonicalBytes(value).toString("hex");
    if (identities.has(identity)) {
      throw new Error(`${description} must contain unique values`);
    }
    identities.add(identity);
  }
}

function validateTool(value) {
  if (!isObject(value)) throw new Error("Plan tool must be an object");
  exactKeys(
    value,
    new Set(["name", "version", "implementation_sha256"]),
    "Plan tool",
  );
  if (
    value.name !== "archbird" ||
    !validSha256(value.implementation_sha256)
  ) throw new Error("Plan tool identity is invalid");
  validateText(value.version, "Plan tool version", 256, { nonempty: true });
}

function validateSource(value) {
  if (!isObject(value)) throw new Error("Plan source must be an object");
  exactKeys(value, new Set(["project", "map", "verification"]), "Plan source");
  validateText(
    value.project,
    "Plan source project",
    MAX_METADATA_BYTES,
    { nonempty: true },
  );
  const identities = [
    [
      value.map,
      new Set([
        "sha256",
        "input_sha256",
        "configuration_sha256",
        "producer_implementation_sha256",
      ]),
      "Map",
    ],
    [
      value.verification,
      new Set([
        "sha256",
        "policy_sha256",
        "producer_implementation_sha256",
      ]),
      "Verification",
    ],
  ];
  for (const [identity, fields, description] of identities) {
    if (!isObject(identity)) {
      throw new Error(`Plan ${description} identity must be an object`);
    }
    exactKeys(identity, fields, `Plan ${description} identity`);
    if ([...fields].some((field) => !validSha256(identity[field]))) {
      throw new Error(`Plan ${description} identity has an invalid SHA-256`);
    }
  }
}

function safeRelativePath(value) {
  if (
    typeof value !== "string" ||
    value.length === 0 ||
    value.length > 4096
  ) throw new Error("path must be a non-empty string");
  if (
    value.includes("\\") ||
    value.includes("\0") ||
    [...value].some((character) => character.codePointAt(0) < 32)
  ) throw new Error("path must use canonical repository-relative syntax");
  const parts = value.split("/");
  if (
    value.startsWith("/") ||
    /^[A-Za-z]:/.test(value) ||
    parts.some((part) => part === "" || part === "." || part === "..") ||
    path.posix.normalize(value) !== value
  ) throw new Error("path must be canonical and repository-relative");
  return value;
}

function validateOperationShape(operation) {
  if (!isObject(operation)) {
    throw new Error("Plan item operation must be an object");
  }
  const fields = {
    replace_range: new Set([
      "action",
      "path",
      "source_sha256",
      "start_byte",
      "end_byte",
      "before",
      "replacement",
    ]),
    create_file: new Set(["action", "path", "content"]),
    delete_file: new Set(["action", "path", "source_sha256"]),
    move_file: new Set([
      "action",
      "source_path",
      "destination_path",
      "source_sha256",
    ]),
    edit_json_pointer: new Set([
      "action",
      "path",
      "source_sha256",
      "pointer",
      "expected_absent",
      "replacement",
    ]),
    edit_make_variable_token: new Set([
      "action",
      "path",
      "source_sha256",
      "variable",
      "expected_token",
      "replacement_token",
    ]),
    insert_make_variable_token: new Set([
      "action",
      "path",
      "source_sha256",
      "variable",
      "token",
      "anchor_token",
      "position",
    ]),
    rename_symbol: new Set([
      "action",
      "symbol",
      "new_name",
      "projection",
      "projection_result_sha256",
      "sites",
      "coverage",
    ]),
    manual: new Set(["action", "instructions", "candidate_paths"]),
  };
  const action = operation.action;
  if (typeof action !== "string" || !Object.hasOwn(fields, action)) {
    throw new Error("Plan item operation action is unsupported");
  }
  const expectedFields = new Set(fields[action]);
  if (action === "edit_json_pointer" && Object.hasOwn(operation, "expected")) {
    expectedFields.add("expected");
  }
  if (action === "manual" && Object.hasOwn(operation, "candidate_sites")) {
    expectedFields.add("candidate_sites");
  }
  exactKeys(operation, expectedFields, `${action} operation`);
  if (
    [
      "replace_range",
      "delete_file",
      "move_file",
      "edit_json_pointer",
      "edit_make_variable_token",
      "insert_make_variable_token",
    ].includes(action) &&
    !validSha256(operation.source_sha256)
  ) throw new Error(`${action} operation has an invalid source_sha256`);
  if (action === "edit_json_pointer") {
    safeRelativePath(operation.path);
    const expectedPresent = Object.hasOwn(operation, "expected");
    if (
      typeof operation.expected_absent !== "boolean" ||
      operation.expected_absent === expectedPresent
    ) {
      throw new Error(
        "edit_json_pointer requires expected exactly when " +
        "expected_absent is false",
      );
    }
    validateText(
      operation.pointer,
      "edit_json_pointer pointer",
      MAX_METADATA_BYTES,
    );
    if (expectedPresent) {
      canonicalBytes(operation.expected, MAX_OPERATION_TEXT_BYTES);
    }
    canonicalBytes(operation.replacement, MAX_OPERATION_TEXT_BYTES);
  }
  if (action === "edit_make_variable_token") {
    safeRelativePath(operation.path);
    validateText(
      operation.variable,
      "edit_make_variable_token variable",
      256,
    );
    validateText(
      operation.expected_token,
      "edit_make_variable_token expected_token",
      MAX_METADATA_BYTES,
    );
    validateText(
      operation.replacement_token,
      "edit_make_variable_token replacement_token",
      MAX_METADATA_BYTES,
    );
    if (
      !PORTABLE_IDENTIFIER.test(operation.variable) ||
      operation.expected_token.length === 0 ||
      /[\s#]/u.test(operation.expected_token) ||
      /[\s#]/u.test(operation.replacement_token) ||
      operation.expected_token === operation.replacement_token
    ) {
      throw new Error(
        "edit_make_variable_token requires a Make variable and " +
        "distinct direct tokens",
      );
    }
  }
  if (action === "insert_make_variable_token") {
    safeRelativePath(operation.path);
    validateText(
      operation.variable,
      "insert_make_variable_token variable",
      256,
    );
    validateText(
      operation.token,
      "insert_make_variable_token token",
      MAX_METADATA_BYTES,
    );
    validateText(
      operation.anchor_token,
      "insert_make_variable_token anchor_token",
      MAX_METADATA_BYTES,
    );
    if (
      !PORTABLE_IDENTIFIER.test(operation.variable) ||
      operation.token.length === 0 ||
      operation.anchor_token.length === 0 ||
      /[\s#]/u.test(operation.token) ||
      /[\s#]/u.test(operation.anchor_token) ||
      operation.token === operation.anchor_token ||
      !["before", "after"].includes(operation.position)
    ) {
      throw new Error(
        "insert_make_variable_token requires a Make variable, " +
        "distinct direct tokens, and explicit placement",
      );
    }
  }
  if (action === "create_file") {
    safeRelativePath(operation.path);
    validateText(
      operation.content,
      "create_file content",
      MAX_OPERATION_TEXT_BYTES,
    );
  }
  if (action === "delete_file") safeRelativePath(operation.path);
  if (action === "move_file") {
    const source = safeRelativePath(operation.source_path);
    const destination = safeRelativePath(operation.destination_path);
    if (source === destination) {
      throw new Error("move_file source and destination must differ");
    }
  }
  if (action === "replace_range") {
    safeRelativePath(operation.path);
    for (const name of ["start_byte", "end_byte"]) {
      if (!Number.isSafeInteger(operation[name]) || operation[name] < 0) {
        throw new Error(
          `replace_range ${name} must be a nonnegative safe integer`,
        );
      }
    }
    if (operation.start_byte > operation.end_byte) {
      throw new Error("replace_range start_byte must not exceed end_byte");
    }
    if (
      typeof operation.before !== "string" ||
      typeof operation.replacement !== "string"
    ) throw new Error("replace_range before and replacement must be text");
    validateText(
      operation.before,
      "replace_range before",
      MAX_OPERATION_TEXT_BYTES,
    );
    validateText(
      operation.replacement,
      "replace_range replacement",
      MAX_OPERATION_TEXT_BYTES,
    );
  }
  if (action === "rename_symbol") {
    validateText(
      operation.symbol,
      "rename_symbol symbol",
      MAX_METADATA_BYTES,
      { nonempty: true },
    );
    validateText(
      operation.new_name,
      "rename_symbol new_name",
      256,
      { nonempty: true },
    );
    const oldMatch = operation.symbol.match(/[A-Za-z_][A-Za-z0-9_]*$/);
    if (
      oldMatch === null ||
      !PORTABLE_IDENTIFIER.test(operation.new_name) ||
      oldMatch[0] === operation.new_name
    ) {
      throw new Error(
        "rename_symbol requires distinct portable identifier leaves",
      );
    }
    if (!isObject(operation.projection)) {
      throw new Error("rename_symbol projection must be an object");
    }
    exactKeys(
      operation.projection,
      new Set([
        "select",
        "names",
        ...(Object.hasOwn(operation.projection, "paths") ? ["paths"] : []),
      ]),
      "rename_symbol projection",
    );
    if (
      operation.projection.select !== "symbol_occurrences" ||
      !Array.isArray(operation.projection.names) ||
      operation.projection.names.length !== 1 ||
      operation.projection.names[0] !== operation.symbol
    ) {
      throw new Error(
        "rename_symbol projection must select exactly its source symbol",
      );
    }
    const projectionPaths = operation.projection.paths ?? [];
    if (
      !Array.isArray(projectionPaths) ||
      projectionPaths.length > MAX_COLLECTION_ITEMS ||
      new Set(projectionPaths).size !== projectionPaths.length
    ) throw new Error("rename_symbol projection paths must be a unique array");
    for (const projectionPath of projectionPaths) {
      safeRelativePath(projectionPath);
    }
    if (!validSha256(operation.projection_result_sha256)) {
      throw new Error("rename_symbol projection_result_sha256 is invalid");
    }
    if (
      !Array.isArray(operation.sites) ||
      operation.sites.length === 0 ||
      operation.sites.length > MAX_COLLECTION_ITEMS
    ) throw new Error("rename_symbol sites must be a non-empty bounded array");
    canonicalIdentities(operation.sites, "rename_symbol sites");
    for (const site of operation.sites) {
      if (!isObject(site)) throw new Error("rename_symbol site must be an object");
      exactKeys(
        site,
        new Set([
          "path",
          "source_sha256",
          "start_byte",
          "end_byte",
          "before",
          "role",
          "fact_ids",
          "providers",
        ]),
        "rename_symbol site",
      );
      safeRelativePath(site.path);
      if (!validSha256(site.source_sha256)) {
        throw new Error("rename_symbol site has an invalid source_sha256");
      }
      if (
        !Number.isSafeInteger(site.start_byte) ||
        !Number.isSafeInteger(site.end_byte) ||
        site.start_byte < 0 ||
        site.end_byte <= site.start_byte
      ) throw new Error("rename_symbol site has an invalid byte range");
      validateText(
        site.before,
        "rename_symbol site before",
        MAX_METADATA_BYTES,
        { nonempty: true },
      );
      if (site.before !== oldMatch[0]) {
        throw new Error(
          "rename_symbol site before must equal the declaration leaf",
        );
      }
      if (!RENAME_ROLES.has(site.role)) {
        throw new Error("rename_symbol site role is unsupported");
      }
      validateIds(site.fact_ids, "rename_symbol site fact_ids", {
        nonempty: true,
      });
      if (
        !Array.isArray(site.providers) ||
        site.providers.length > MAX_COLLECTION_ITEMS ||
        site.providers.some(
          (provider) => typeof provider !== "string" || provider.length === 0,
        ) ||
        new Set(site.providers).size !== site.providers.length
      ) {
        throw new Error(
          "rename_symbol site providers must contain unique non-empty strings",
        );
      }
      for (const provider of site.providers) {
        validateText(
          provider,
          "rename_symbol site provider",
          MAX_METADATA_BYTES,
          { nonempty: true },
        );
      }
    }
    if (!isObject(operation.coverage)) {
      throw new Error("rename_symbol coverage must be an object");
    }
    exactKeys(
      operation.coverage,
      new Set([
        "classification",
        "exhaustive",
        "selected",
        "unknown",
        "unsupported",
      ]),
      "rename_symbol coverage",
    );
    if (
      !["complete", "incomplete", "unknown"].includes(
        operation.coverage.classification,
      ) ||
      typeof operation.coverage.exhaustive !== "boolean"
    ) throw new Error("rename_symbol coverage metadata is invalid");
    for (const field of ["selected", "unknown", "unsupported"]) {
      if (
        !Number.isSafeInteger(operation.coverage[field]) ||
        operation.coverage[field] < 0
      ) {
        throw new Error(
          `rename_symbol coverage ${field} must be a nonnegative safe integer`,
        );
      }
    }
    if (operation.coverage.selected !== operation.sites.length) {
      throw new Error(
        "rename_symbol coverage selected must equal the site count",
      );
    }
  }
  if (action === "manual") {
    if (
      typeof operation.instructions !== "string" ||
      operation.instructions.length === 0 ||
      !Array.isArray(operation.candidate_paths) ||
      operation.candidate_paths.length > MAX_COLLECTION_ITEMS
    ) throw new Error("manual operation fields are invalid");
    validateText(
      operation.instructions,
      "manual instructions",
      MAX_METADATA_BYTES,
      { nonempty: true },
    );
    if (
      new Set(operation.candidate_paths).size !==
      operation.candidate_paths.length
    ) throw new Error("manual candidate_paths must be unique");
    for (const filePath of operation.candidate_paths) safeRelativePath(filePath);
    const sites = operation.candidate_sites ?? [];
    if (!Array.isArray(sites) || sites.length > MAX_COLLECTION_ITEMS) {
      throw new Error("manual candidate_sites must be a bounded array");
    }
    const identities = new Set();
    for (const site of sites) {
      if (!isObject(site)) {
        throw new Error("manual candidate site must be an object");
      }
      exactKeys(
        site,
        new Set([
          "fact_id",
          "path",
          "line",
          "source_sha256",
          "start_byte",
          "end_byte",
          "before",
          "name",
        ]),
        "manual candidate site",
      );
      validateText(site.fact_id, "manual candidate site fact_id",
        MAX_METADATA_BYTES, { nonempty: true });
      safeRelativePath(site.path);
      if (!validSha256(site.source_sha256)) {
        throw new Error("manual candidate site source_sha256 is invalid");
      }
      for (const field of ["line", "start_byte", "end_byte"]) {
        if (!Number.isSafeInteger(site[field]) || site[field] < 0) {
          throw new Error(
            `manual candidate site ${field} must be a nonnegative safe integer`,
          );
        }
      }
      if (site.start_byte >= site.end_byte) {
        throw new Error("manual candidate site range is invalid");
      }
      validateText(site.before, "manual candidate site before",
        MAX_METADATA_BYTES, { nonempty: true });
      validateText(site.name, "manual candidate site name",
        MAX_METADATA_BYTES, { nonempty: true });
      if (
        site.before !== site.name ||
        Buffer.byteLength(site.before, "utf8") !==
          site.end_byte - site.start_byte
      ) {
        throw new Error("manual candidate site source anchor is inconsistent");
      }
      const identity = canonicalBytes(site).toString("hex");
      if (identities.has(identity)) {
        throw new Error("manual candidate_sites must be unique");
      }
      identities.add(identity);
    }
  }
  return action;
}

function validatePlanShape(plan) {
  if (!isObject(plan)) throw new Error("Plan must be an object");
  exactKeys(
    plan,
    new Set([
      "schema_version",
      "artifact",
      "provenance",
      "tool",
      "source",
      "objective",
      "items",
      "preserved_constraints",
      "unknowns",
    ]),
    "Plan",
  );
  if (plan.schema_version !== 1 || plan.artifact !== "plan") {
    throw new Error("Plan schema_version or artifact is invalid");
  }
  if (!["derived", "asserted"].includes(plan.provenance)) {
    throw new Error("Plan provenance is invalid");
  }
  validateText(
    plan.objective,
    "Plan objective",
    MAX_METADATA_BYTES,
    { nonempty: true },
  );
  validateTool(plan.tool);
  validateSource(plan.source);
  validateIds(plan.preserved_constraints, "preserved_constraints");
  if (
    !Array.isArray(plan.unknowns) ||
    plan.unknowns.length > MAX_COLLECTION_ITEMS
  ) {
    throw new Error("Plan unknowns must be an array");
  }
  canonicalIdentities(plan.unknowns, "Plan unknowns");
  const unknownIds = new Set();
  for (const unknown of plan.unknowns) {
    if (!isObject(unknown)) throw new Error("Plan unknown must be an object");
    exactKeys(
      unknown,
      new Set(["id", "statement", "item_id", "constraint_id"]),
      "Plan unknown",
    );
    if (
      !validId(unknown.id) ||
      unknownIds.has(unknown.id) ||
      (unknown.item_id !== null && !validId(unknown.item_id)) ||
      (unknown.constraint_id !== null && !validId(unknown.constraint_id))
    ) throw new Error("Plan unknown fields are invalid");
    validateText(
      unknown.statement,
      "Plan unknown statement",
      MAX_METADATA_BYTES,
      { nonempty: true },
    );
    unknownIds.add(unknown.id);
  }
  if (
    !Array.isArray(plan.items) ||
    plan.items.length > MAX_COLLECTION_ITEMS
  ) throw new Error("Plan items must be an array");
  canonicalIdentities(plan.items, "Plan items");
  const itemIds = new Set();
  const dependencies = new Map();
  const referencedUnknowns = new Set();
  for (const item of plan.items) {
    if (!isObject(item)) throw new Error("Plan item must be an object");
    exactKeys(
      item,
      new Set([
        "id",
        "statement",
        "provenance",
        "origins",
        "evidence",
        "depends_on",
        "operation",
        "acceptance",
        "unknowns",
        "executable",
        "non_executable_reasons",
      ]),
      "Plan item",
    );
    if (!validId(item.id) || itemIds.has(item.id)) {
      throw new Error("Plan item IDs must be unique stable IDs");
    }
    itemIds.add(item.id);
    if (
      !["derived", "asserted"].includes(item.provenance) ||
      typeof item.executable !== "boolean"
    ) throw new Error(`Plan item ${item.id} metadata is invalid`);
    validateText(
      item.statement,
      `Plan item ${item.id} statement`,
      MAX_METADATA_BYTES,
      { nonempty: true },
    );
    if (
      !Array.isArray(item.origins) ||
      item.origins.length === 0 ||
      item.origins.length > MAX_COLLECTION_ITEMS
    ) {
      throw new Error(`Plan item ${item.id} must have origins`);
    }
    canonicalIdentities(item.origins, `Plan item ${item.id} origins`);
    for (const origin of item.origins) {
      if (!isObject(origin)) {
        throw new Error(`Plan item ${item.id} origin must be an object`);
      }
      exactKeys(
        origin,
        new Set([
          "constraint_id",
          "constraint_result_sha256",
          "issue_fingerprint",
        ]),
        "Plan item origin",
      );
      if (
        !validId(origin.constraint_id) ||
        !validSha256(origin.constraint_result_sha256) ||
        !validSha256(origin.issue_fingerprint)
      ) throw new Error(`Plan item ${item.id} origin is invalid`);
    }
    if (
      !Array.isArray(item.evidence) ||
      item.evidence.length > MAX_COLLECTION_ITEMS
    ) {
      throw new Error(`Plan item ${item.id} evidence must be an array`);
    }
    canonicalIdentities(item.evidence, `Plan item ${item.id} evidence`);
    for (const row of item.evidence) {
      if (!isObject(row)) {
        throw new Error(`Plan item ${item.id} evidence must contain objects`);
      }
      exactKeys(
        row,
        new Set([
          "provenance",
          "project",
          "path",
          "line",
          "sha256",
          "detail",
        ]),
        "Plan item evidence",
      );
      if (
        !["derived", "asserted", "observed"].includes(row.provenance) ||
        !Number.isSafeInteger(row.line) ||
        row.line < 0 ||
        (row.sha256 !== "" && !validSha256(row.sha256))
      ) throw new Error(`Plan item ${item.id} evidence is invalid`);
      validateText(
        row.project,
        `Plan item ${item.id} evidence project`,
        MAX_METADATA_BYTES,
      );
      validateText(
        row.detail,
        `Plan item ${item.id} evidence detail`,
        MAX_METADATA_BYTES,
      );
      if (row.path !== "") safeRelativePath(row.path);
    }
    dependencies.set(
      item.id,
      validateIds(item.depends_on, `Plan item ${item.id} depends_on`),
    );
    for (const identifier of validateIds(
      item.unknowns,
      `Plan item ${item.id} unknowns`,
    )) referencedUnknowns.add(identifier);
    if (!isObject(item.acceptance)) {
      throw new Error(`Plan item ${item.id} acceptance must be an object`);
    }
    exactKeys(item.acceptance, new Set(["constraints"]), "Plan item acceptance");
    validateIds(
      item.acceptance.constraints,
      `Plan item ${item.id} acceptance constraints`,
      { nonempty: true },
    );
    const action = validateOperationShape(item.operation);
    if (
      !Array.isArray(item.non_executable_reasons) ||
      item.non_executable_reasons.length > MAX_COLLECTION_ITEMS ||
      item.non_executable_reasons.some(
        (reason) => (
          typeof reason !== "string" ||
          reason.length === 0 ||
          Buffer.byteLength(reason, "utf8") > MAX_METADATA_BYTES
        ),
      ) ||
      new Set(item.non_executable_reasons).size !==
      item.non_executable_reasons.length
    ) {
      throw new Error(
        `Plan item ${item.id} non-executable reasons are invalid`,
      );
    }
    if (
      item.executable &&
      (item.non_executable_reasons.length > 0 || action === "manual")
    ) throw new Error(`Plan item ${item.id} has an invalid executable gate`);
    if (
      item.executable &&
      [
        "rename_symbol",
        "edit_json_pointer",
        "edit_make_variable_token",
        "insert_make_variable_token",
      ].includes(action) &&
      item.provenance !== "asserted"
    ) {
      throw new Error(
        `Plan item ${item.id} requires asserted ${
          action === "rename_symbol" ? "rename" : action
        } intent`,
      );
    }
    if (!item.executable && item.non_executable_reasons.length === 0) {
      throw new Error(`Plan item ${item.id} requires a blocking reason`);
    }
  }
  for (const [itemId, dependsOn] of dependencies) {
    if (dependsOn.some((dependency) => !itemIds.has(dependency))) {
      throw new Error(`Plan item ${itemId} has a dangling dependency`);
    }
    if (dependsOn.includes(itemId)) {
      throw new Error(`Plan item ${itemId} depends on itself`);
    }
  }
  for (const unknown of plan.unknowns) {
    if (unknown.item_id !== null && !itemIds.has(unknown.item_id)) {
      throw new Error(`Plan unknown ${unknown.id} has a dangling item_id`);
    }
  }
  for (const identifier of referencedUnknowns) {
    if (!unknownIds.has(identifier)) {
      throw new Error("Plan item has a dangling unknown reference");
    }
  }
  const visiting = new Set();
  const visited = new Set();
  function visit(itemId) {
    if (visiting.has(itemId)) {
      throw new Error("Plan item dependency graph contains a cycle");
    }
    if (visited.has(itemId)) return;
    visiting.add(itemId);
    for (const dependency of dependencies.get(itemId)) visit(dependency);
    visiting.delete(itemId);
    visited.add(itemId);
  }
  for (const itemId of [...itemIds].sort(utf8Compare)) visit(itemId);
}

function expectedAcceptanceIds(plan) {
  const identifiers = new Set(plan.preserved_constraints);
  for (const item of plan.items) {
    for (const identifier of item.acceptance.constraints) {
      identifiers.add(identifier);
    }
  }
  return [...identifiers].sort(utf8Compare);
}

function validateAcceptance(value, expectedIds) {
  if (!isObject(value)) {
    throw new Error("acceptance callback must return an object");
  }
  exactKeys(
    value,
    new Set(["status", "verification_sha256", "constraints"]),
    "Act acceptance",
  );
  if (!["satisfied", "not_satisfied", "unknown"].includes(value.status)) {
    throw new Error("applied acceptance status is invalid");
  }
  if (!validSha256(value.verification_sha256)) {
    throw new Error("applied acceptance verification_sha256 is invalid");
  }
  if (
    !Array.isArray(value.constraints) ||
    value.constraints.length > MAX_COLLECTION_ITEMS
  ) {
    throw new Error("applied acceptance constraints must be an array");
  }
  const identifiers = new Set();
  const statuses = [];
  const normalized = [];
  for (const row of value.constraints) {
    if (!isObject(row)) {
      throw new Error("applied acceptance constraint must be an object");
    }
    exactKeys(row, new Set(["id", "status"]), "Act acceptance constraint");
    if (
      !validId(row.id) ||
      identifiers.has(row.id) ||
      !["pass", "fail", "unknown", "waived", "not_applicable"]
        .includes(row.status)
    ) throw new Error("applied acceptance constraint is invalid");
    identifiers.add(row.id);
    statuses.push(row.status);
    normalized.push({ id: row.id, status: row.status });
  }
  const actualIds = [...identifiers].sort(utf8Compare);
  if (
    actualIds.length !== expectedIds.length ||
    actualIds.some((identifier, index) => identifier !== expectedIds[index])
  ) {
    throw new Error(
      "applied acceptance constraints do not exactly cover the Plan",
    );
  }
  if (
    value.status === "satisfied" &&
    statuses.some(
      (status) => !["pass", "waived", "not_applicable"].includes(status),
    )
  ) {
    throw new Error("satisfied acceptance contains an unsatisfied constraint");
  }
  if (value.status === "not_satisfied" && !statuses.includes("fail")) {
    throw new Error("not_satisfied acceptance has no failing constraint");
  }
  if (
    value.status === "unknown" &&
    (!statuses.includes("unknown") || statuses.includes("fail"))
  ) throw new Error("unknown acceptance has contradictory constraints");
  if (normalized.length === 0 && value.status !== "satisfied") {
    throw new Error("empty applied acceptance must be satisfied");
  }
  return {
    status: value.status,
    verification_sha256: value.verification_sha256,
    constraints: normalized,
  };
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

function readRegular(root, relative) {
  checkPathComponents(root, relative, true);
  const target = candidate(root, relative);
  let descriptor;
  try {
    const noFollow = fs.constants.O_NOFOLLOW || 0;
    descriptor = fs.openSync(target, fs.constants.O_RDONLY | noFollow);
  } catch (error) {
    if (error.code === "ENOENT") {
      throw new Error(`source file does not exist: ${relative}`);
    }
    throw new Error(`cannot safely open source file ${relative}: ${error.message}`);
  }
  try {
    const metadata = fs.fstatSync(descriptor);
    if (!metadata.isFile()) {
      throw new Error(`source is not a regular file: ${relative}`);
    }
    if (metadata.size > MAX_FILE_BYTES) {
      throw new Error(
        `source file exceeds ${MAX_FILE_BYTES} bytes: ${relative}`,
      );
    }
    const data = fs.readFileSync(descriptor);
    if (data.length > MAX_FILE_BYTES) {
      throw new Error(
        `source file exceeds ${MAX_FILE_BYTES} bytes: ${relative}`,
      );
    }
    return {
      data,
      mode: metadata.mode & 0o7777,
      sha256: digest(data),
    };
  } finally {
    fs.closeSync(descriptor);
  }
}

function existingState(root, relative, initial, sourceBudget) {
  if (!initial.has(relative)) {
    if (initial.size >= MAX_TOUCHED_FILES) {
      throw new Error(`Plan touches more than ${MAX_TOUCHED_FILES} files`);
    }
    const state = readRegular(root, relative);
    if (
      state.data.length >
      MAX_TOUCHED_SOURCE_BYTES - sourceBudget.bytes
    ) {
      throw new Error(
        `Plan source exceeds ${MAX_TOUCHED_SOURCE_BYTES} aggregate bytes`,
      );
    }
    sourceBudget.bytes += state.data.length;
    initial.set(relative, state);
  }
  return initial.get(relative);
}

function destinationAbsent(root, relative) {
  checkPathComponents(root, relative, true);
  try {
    fs.lstatSync(candidate(root, relative));
  } catch (error) {
    if (error.code === "ENOENT") return;
    throw error;
  }
  throw new Error(`destination already exists: ${relative}`);
}

function rangesOverlap(left, right) {
  if (left.start === left.end) {
    if (right.start === right.end) return left.start === right.start;
    return right.start <= left.start && left.start < right.end;
  }
  if (right.start === right.end) {
    return left.start <= right.start && right.start < left.end;
  }
  return left.start < right.end && right.start < left.end;
}

function unifiedDiff(
  before,
  after,
  beforePath,
  afterPath,
  prefix = [],
) {
  const metadata = Buffer.from(
    prefix.map((line) => `${line}\n`).join(""),
    "utf8",
  );
  return native.unifiedDiff(
    before,
    after,
    beforePath,
    afterPath,
    metadata,
  ).toString("utf8");
}

function renameProjectionSites(operation, mapDocument) {
  if (mapDocument === null || mapDocument === undefined) {
    throw new Error(
      "rename_symbol requires the current source Map for closure validation",
    );
  }
  const encodedMap = Buffer.isBuffer(mapDocument)
    ? mapDocument
    : canonicalBytes(mapDocument);
  let result;
  try {
    result = JSON.parse(native.projectionEvaluate(
      encodedMap,
      Buffer.alloc(0),
      canonicalBytes({
        id: "act-symbol-occurrences",
        ...operation.projection,
      }),
      false,
    ).toString("utf8"));
  } catch (error) {
    throw new Error(
      `rename_symbol closure could not be evaluated: ${error.message}`,
    );
  }
  if (
    result.projection_result_sha256 !== operation.projection_result_sha256
  ) {
    throw new Error(
      "rename_symbol ProjectionResult does not match the current Map",
    );
  }
  if (
    result.artifact !== "projection-result" ||
    !isObject(result.fact) ||
    !Array.isArray(result.fact.items) ||
    !isObject(result.completeness)
  ) throw new Error("rename_symbol closure is not a valid ProjectionResult");
  const leaf = operation.symbol.match(/[A-Za-z_][A-Za-z0-9_]*$/)[0];
  const sites = result.fact.items
    .filter((item) =>
      isObject(item) &&
      item.state === "current" &&
      isObject(item.attributes)
    )
    .map(({ attributes }) => ({
      path: attributes.path,
      source_sha256: attributes.source_sha256,
      start_byte: attributes.start_byte,
      end_byte: attributes.end_byte,
      before: leaf,
      role: attributes.role,
      fact_ids: [...(attributes.fact_ids ?? [])].sort(utf8Compare),
      providers: [...(attributes.providers ?? [])].sort(utf8Compare),
    }))
    .sort((left, right) =>
      utf8Compare(left.path, right.path) || left.start_byte - right.start_byte
    );
  const expectedSites = structuredClone(operation.sites)
    .sort((left, right) =>
      utf8Compare(left.path, right.path) || left.start_byte - right.start_byte
    );
  if (!canonicalBytes(sites).equals(canonicalBytes(expectedSites))) {
    throw new Error(
      "rename_symbol sites do not equal the current exhaustive projection",
    );
  }
  const counts = result.completeness.counts;
  if (!isObject(counts) || !isObject(operation.coverage)) {
    throw new Error("rename_symbol closure has invalid completeness");
  }
  const actualCoverage = {
    classification: result.completeness.classification,
    exhaustive: result.completeness.exhaustive,
    selected: sites.length,
    unknown: counts.unknown,
    unsupported: counts.unsupported,
  };
  if (!canonicalBytes(actualCoverage).equals(canonicalBytes(operation.coverage))) {
    throw new Error(
      "rename_symbol coverage does not equal the current projection",
    );
  }
  return sites;
}

function prepare(plan, root, mapDocument = null) {
  const diagnostics = [];
  const initial = new Map();
  const replacements = new Map();
  const creates = new Map();
  const deletes = new Map();
  const moves = new Map();
  const destinations = new Set();
  const touched = new Set();
  const sourceBudget = { bytes: 0 };

  for (const item of plan.items) {
    const itemId = item.id;
    const operation = item.operation;
    const action = operation.action;
    if (!item.executable || action === "manual") {
      const suffix = item.non_executable_reasons.length > 0
        ? `: ${item.non_executable_reasons.join(", ")}`
        : "";
      diagnostics.push(diagnostic(
        "non_executable",
        `item is not executable${suffix}`,
        itemId,
      ));
      continue;
    }
    if (!SUPPORTED_ACTIONS.has(action)) {
      diagnostics.push(diagnostic(
        "unsupported_action",
        `unsupported action: ${action || "<missing>"}`,
        itemId,
      ));
      continue;
    }
    let filePath = null;
    try {
      let destination = "";
      if (action === "rename_symbol") {
        renameProjectionSites(operation, mapDocument);
        const coverage = operation.coverage;
        if (
          coverage.classification !== "complete" ||
          coverage.exhaustive !== true ||
          coverage.unknown !== 0 ||
          coverage.unsupported !== 0
        ) {
          throw new Error(
            "rename_symbol requires complete exhaustive coverage without " +
            "unknown or unsupported sites",
          );
        }
        const replacement = Buffer.from(operation.new_name, "utf8");
        for (const site of operation.sites) {
          filePath = safeRelativePath(site.path);
          touched.add(filePath);
          if (touched.size > MAX_TOUCHED_FILES) {
            throw new Error(
              `Plan touches more than ${MAX_TOUCHED_FILES} files`,
            );
          }
          const state = existingState(root, filePath, initial, sourceBudget);
          if (state.sha256 !== site.source_sha256) {
            throw new Error(
              `source SHA-256 is stale: expected ${site.source_sha256}, ` +
              `found ${state.sha256}`,
            );
          }
          const start = site.start_byte;
          const end = site.end_byte;
          const before = Buffer.from(site.before, "utf8");
          if (end > state.data.length) {
            throw new Error(
              "rename_symbol site exceeds current source bytes",
            );
          }
          try {
            const decoder = new TextDecoder("utf-8", { fatal: true });
            decoder.decode(state.data);
            decoder.decode(state.data.subarray(0, start));
            decoder.decode(state.data.subarray(start, end));
          } catch (_) {
            throw new Error(
              "rename_symbol offsets must bound valid UTF-8 text",
            );
          }
          if (!state.data.subarray(start, end).equals(before)) {
            throw new Error(
              "rename_symbol site does not match current source bytes",
            );
          }
          if (!replacements.has(filePath)) replacements.set(filePath, []);
          replacements.get(filePath).push({
            itemId,
            path: filePath,
            start,
            end,
            before,
            replacement,
            sourceSha256: site.source_sha256,
          });
        }
        continue;
      }
      if (action === "move_file") {
        filePath = safeRelativePath(operation.source_path);
        destination = safeRelativePath(operation.destination_path);
      } else {
        filePath = safeRelativePath(operation.path);
      }
      for (
        const touchedPath of action === "move_file"
          ? [filePath, destination]
          : [filePath]
      ) {
        touched.add(touchedPath);
        if (touched.size > MAX_TOUCHED_FILES) {
          throw new Error(`Plan touches more than ${MAX_TOUCHED_FILES} files`);
        }
      }
      if (action === "replace_range") {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        const { start_byte: start, end_byte: end } = operation;
        if (
          !Number.isSafeInteger(start) ||
          !Number.isSafeInteger(end) ||
          start < 0 ||
          end < start ||
          end > state.data.length
        ) throw new Error("replace_range has an invalid UTF-8 byte range");
        const before = Buffer.from(operation.before, "utf8");
        const replacement = Buffer.from(operation.replacement, "utf8");
        try {
          const decoder = new TextDecoder("utf-8", { fatal: true });
          decoder.decode(state.data);
          decoder.decode(state.data.subarray(0, start));
          decoder.decode(state.data.subarray(start, end));
        } catch (_) {
          throw new Error("replace_range offsets must bound valid UTF-8 text");
        }
        if (!state.data.subarray(start, end).equals(before)) {
          throw new Error("replace_range before text does not match source bytes");
        }
        if (!replacements.has(filePath)) replacements.set(filePath, []);
        replacements.get(filePath).push({
          itemId,
          path: filePath,
          start,
          end,
          before,
          replacement,
          sourceSha256: operation.source_sha256,
        });
      } else if (action === "edit_json_pointer") {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        const expected = operation.expected_absent
          ? Buffer.alloc(0)
          : canonicalBytes(operation.expected, MAX_OPERATION_TEXT_BYTES);
        const edit = native.jsonPointerEdit(
          state.data,
          operation.source_sha256,
          operation.pointer,
          expected,
          canonicalBytes(operation.replacement, MAX_OPERATION_TEXT_BYTES),
          operation.expected_absent,
        );
        const {
          startByte: start,
          endByte: end,
          replacement,
        } = edit;
        if (
          !Number.isSafeInteger(start) ||
          !Number.isSafeInteger(end) ||
          start < 0 ||
          end < start ||
          end > state.data.length ||
          !Buffer.isBuffer(replacement)
        ) {
          throw new Error(
            "native JSON Pointer edit returned an invalid range",
          );
        }
        if (!replacements.has(filePath)) replacements.set(filePath, []);
        replacements.get(filePath).push({
          itemId,
          path: filePath,
          start,
          end,
          before: state.data.subarray(start, end),
          replacement,
          sourceSha256: operation.source_sha256,
        });
      } else if (action === "edit_make_variable_token") {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        const edit = native.makeVariableTokenEdit(
          state.data,
          operation.source_sha256,
          operation.variable,
          operation.expected_token,
          operation.replacement_token,
        );
        const {
          startByte: start,
          endByte: end,
          matchedTokens,
          replacement,
        } = edit;
        if (
          !Number.isSafeInteger(start) ||
          !Number.isSafeInteger(end) ||
          start < 0 ||
          end <= start ||
          end > state.data.length ||
          matchedTokens !== 1 ||
          !Buffer.isBuffer(replacement)
        ) {
          throw new Error(
            "native Make variable token edit returned an invalid range",
          );
        }
        if (!replacements.has(filePath)) replacements.set(filePath, []);
        replacements.get(filePath).push({
          itemId,
          path: filePath,
          start,
          end,
          before: state.data.subarray(start, end),
          replacement,
          sourceSha256: operation.source_sha256,
        });
      } else if (action === "insert_make_variable_token") {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        const edit = native.makeVariableTokenInsert(
          state.data,
          operation.source_sha256,
          operation.variable,
          operation.token,
          operation.anchor_token,
          operation.position,
        );
        const {
          startByte: start,
          endByte: end,
          matchedTokens,
          matchedAnchors,
          replacement,
        } = edit;
        if (
          !Number.isSafeInteger(start) ||
          !Number.isSafeInteger(end) ||
          start < 0 ||
          end !== start ||
          end > state.data.length ||
          matchedTokens !== 0 ||
          matchedAnchors !== 1 ||
          !Buffer.isBuffer(replacement)
        ) {
          throw new Error(
            "native Make variable token insertion returned an invalid range",
          );
        }
        if (!replacements.has(filePath)) replacements.set(filePath, []);
        replacements.get(filePath).push({
          itemId,
          path: filePath,
          start,
          end,
          before: Buffer.alloc(0),
          replacement,
          sourceSha256: operation.source_sha256,
        });
      } else if (action === "create_file") {
        if (destinations.has(filePath)) {
          throw new Error(`destination is claimed more than once: ${filePath}`);
        }
        destinationAbsent(root, filePath);
        destinations.add(filePath);
        creates.set(filePath, {
          itemId,
          data: Buffer.from(operation.content, "utf8"),
        });
      } else if (action === "delete_file") {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        if (deletes.has(filePath) || moves.has(filePath)) {
          throw new Error(`source is consumed more than once: ${filePath}`);
        }
        deletes.set(filePath, itemId);
      } else {
        const state = existingState(root, filePath, initial, sourceBudget);
        if (state.sha256 !== operation.source_sha256) {
          throw new Error(
            `source SHA-256 is stale: expected ${operation.source_sha256}, ` +
            `found ${state.sha256}`,
          );
        }
        if (deletes.has(filePath) || moves.has(filePath)) {
          throw new Error(`source is consumed more than once: ${filePath}`);
        }
        if (destinations.has(destination)) {
          throw new Error(
            `destination is claimed more than once: ${destination}`,
          );
        }
        destinationAbsent(root, destination);
        destinations.add(destination);
        moves.set(filePath, { itemId, destination });
      }
    } catch (error) {
      diagnostics.push(diagnostic(
        "invalid_operation",
        error.message,
        itemId,
        filePath,
      ));
    }
  }

  for (const [filePath, edits] of replacements) {
    const ordered = [...edits].sort((left, right) => (
      left.start - right.start ||
      left.end - right.end ||
      utf8Compare(left.itemId, right.itemId)
    ));
    for (let index = 0; index < ordered.length; index += 1) {
      for (const right of ordered.slice(index + 1)) {
        if (right.start > ordered[index].end) break;
        if (rangesOverlap(ordered[index], right)) {
          diagnostics.push(diagnostic(
            "overlapping_edits",
            `replace ranges overlap with item ${right.itemId}`,
            ordered[index].itemId,
            filePath,
          ));
        }
      }
    }
    if (deletes.has(filePath)) {
      diagnostics.push(diagnostic(
        "conflicting_actions",
        "a file cannot be replaced and deleted in the same Plan",
        deletes.get(filePath),
        filePath,
      ));
    }
    if (creates.has(filePath)) {
      diagnostics.push(diagnostic(
        "conflicting_actions",
        "a file cannot be replaced and created in the same Plan",
        creates.get(filePath).itemId,
        filePath,
      ));
    }
  }

  if (diagnostics.length > 0) {
    return {
      initial,
      final: new Map(),
      changes: [],
      diagnostics,
      destinations,
    };
  }

  let projectedFinalBytes = sourceBudget.bytes;
  for (const filePath of deletes.keys()) {
    projectedFinalBytes -= initial.get(filePath).data.length;
  }
  for (const [filePath, edits] of replacements) {
    let projectedFileBytes = initial.get(filePath).data.length;
    for (const edit of edits) {
      projectedFileBytes += edit.replacement.length - (edit.end - edit.start);
    }
    if (projectedFileBytes > MAX_FILE_BYTES) {
      diagnostics.push(diagnostic(
        "resource_limit",
        `final source file exceeds ${MAX_FILE_BYTES} bytes`,
        null,
        filePath,
      ));
      break;
    }
    projectedFinalBytes +=
      projectedFileBytes - initial.get(filePath).data.length;
  }
  for (const create of creates.values()) {
    projectedFinalBytes += create.data.length;
  }
  if (
    diagnostics.length === 0 &&
    projectedFinalBytes > MAX_TOUCHED_SOURCE_BYTES
  ) {
    diagnostics.push(diagnostic(
      "resource_limit",
      `final Plan source exceeds ${MAX_TOUCHED_SOURCE_BYTES} aggregate bytes`,
    ));
  }
  if (diagnostics.length > 0) {
    return {
      initial,
      final: new Map(),
      changes: [],
      diagnostics,
      destinations,
    };
  }

  const final = new Map(
    [...initial].map(([filePath, state]) => [
      filePath,
      { data: Buffer.from(state.data), mode: state.mode },
    ]),
  );
  for (const [filePath, edits] of [...replacements].sort(
    ([left], [right]) => utf8Compare(left, right),
  )) {
    let data = initial.get(filePath).data;
    for (const edit of [...edits].sort((left, right) => (
      right.start - left.start ||
      right.end - left.end ||
      utf8Compare(right.itemId, left.itemId)
    ))) {
      data = Buffer.concat([
        data.subarray(0, edit.start),
        edit.replacement,
        data.subarray(edit.end),
      ]);
    }
    final.set(filePath, { data, mode: initial.get(filePath).mode });
  }
  for (const filePath of deletes.keys()) final.delete(filePath);
  for (const [source, move] of moves) {
    const state = final.get(source);
    final.delete(source);
    final.set(move.destination, state);
  }
  for (const [filePath, create] of creates) {
    final.set(filePath, { data: create.data, mode: 0o644 });
  }
  let finalBytes = 0;
  for (const [filePath, state] of final) {
    if (state.data.length > MAX_FILE_BYTES) {
      diagnostics.push(diagnostic(
        "resource_limit",
        `final source file exceeds ${MAX_FILE_BYTES} bytes`,
        null,
        filePath,
      ));
      break;
    }
    if (state.data.length > MAX_TOUCHED_SOURCE_BYTES - finalBytes) {
      diagnostics.push(diagnostic(
        "resource_limit",
        `final Plan source exceeds ${MAX_TOUCHED_SOURCE_BYTES} aggregate bytes`,
      ));
      break;
    }
    finalBytes += state.data.length;
  }
  if (diagnostics.length > 0) {
    return { initial, final, changes: [], diagnostics, destinations };
  }

  const changes = [];
  const movedDestinations = new Set();
  for (const [source, move] of [...moves].sort(
    ([left], [right]) => utf8Compare(left, right),
  )) {
    const before = initial.get(source);
    const after = final.get(move.destination);
    movedDestinations.add(move.destination);
    const prefix = before.data.equals(after.data)
      ? [
        "similarity index 100%",
        `rename from ${source}`,
        `rename to ${move.destination}`,
      ]
      : [`rename from ${source}`, `rename to ${move.destination}`];
    changes.push({
      item_ids: [
        move.itemId,
        ...(replacements.get(source) || []).map((edit) => edit.itemId),
      ].sort(utf8Compare),
      kind: "move",
      path: move.destination,
      source_path: source,
      before_sha256: before.sha256,
      after_sha256: digest(after.data),
      unified_diff: unifiedDiff(
        before.data,
        after.data,
        source,
        move.destination,
        prefix,
      ),
    });
  }
  const paths = new Set([...initial.keys(), ...final.keys()]);
  for (const filePath of [...paths].sort(utf8Compare)) {
    if (moves.has(filePath) || movedDestinations.has(filePath)) continue;
    const beforeState = initial.get(filePath);
    const afterState = final.get(filePath);
    const before = beforeState?.data || Buffer.alloc(0);
    const after = afterState?.data || Buffer.alloc(0);
    if (beforeState && afterState && before.equals(after)) continue;
    let kind;
    let itemIds;
    let prefix;
    if (!beforeState) {
      kind = "create";
      itemIds = [creates.get(filePath).itemId];
      prefix = ["new file mode 100644"];
    } else if (!afterState) {
      kind = "delete";
      itemIds = [deletes.get(filePath)];
      prefix = [`deleted file mode ${beforeState.mode.toString(8).padStart(6, "0")}`];
    } else {
      kind = "modify";
      itemIds = [...new Set(
        replacements.get(filePath).map((edit) => edit.itemId),
      )].sort(utf8Compare);
      prefix = [];
    }
    changes.push({
      item_ids: itemIds,
      kind,
      path: filePath,
      source_path: null,
      before_sha256: beforeState?.sha256 || null,
      after_sha256: afterState ? digest(after) : null,
      unified_diff: unifiedDiff(
        before,
        after,
        beforeState ? filePath : null,
        afterState ? filePath : null,
        prefix,
      ),
    });
  }
  changes.sort((left, right) => (
    utf8Compare(left.path, right.path) || utf8Compare(left.kind, right.kind)
  ));
  let patchBytes = 0;
  for (const change of changes) {
    const size = Buffer.byteLength(change.unified_diff, "utf8");
    if (size > MAX_CHANGE_PATCH_BYTES) {
      diagnostics.push(diagnostic(
        "resource_limit",
        `file patch exceeds ${MAX_CHANGE_PATCH_BYTES} UTF-8 bytes`,
        null,
        change.path,
      ));
      break;
    }
    if (size > MAX_PATCH_BYTES - patchBytes) {
      diagnostics.push(diagnostic(
        "resource_limit",
        `Plan patch exceeds ${MAX_PATCH_BYTES} aggregate UTF-8 bytes`,
      ));
      break;
    }
    patchBytes += size;
  }
  if (diagnostics.length > 0) {
    return { initial, final, changes: [], diagnostics, destinations };
  }
  return { initial, final, changes, diagnostics, destinations };
}

function writeStageFile(filePath, data, mode) {
  const descriptor = fs.openSync(
    filePath,
    fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_EXCL,
    0o600,
  );
  try {
    let offset = 0;
    while (offset < data.length) {
      offset += fs.writeSync(descriptor, data, offset, data.length - offset);
    }
    fs.fsyncSync(descriptor);
    fs.fchmodSync(descriptor, mode);
  } finally {
    fs.closeSync(descriptor);
  }
}

function makeParents(root, relative, created) {
  const parts = relative.split("/").slice(0, -1);
  let cursor = root;
  for (const part of parts) {
    cursor = path.join(cursor, part);
    try {
      const metadata = fs.lstatSync(cursor);
      if (metadata.isSymbolicLink() || !metadata.isDirectory()) {
        throw new Error(`unsafe destination parent appeared: ${relative}`);
      }
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
      fs.mkdirSync(cursor, { mode: 0o755 });
      created.push(cursor);
    }
  }
}

function revalidate(root, prepared) {
  for (const [filePath, expected] of prepared.initial) {
    const current = readRegular(root, filePath);
    if (current.sha256 !== expected.sha256) {
      throw new Error(`source changed before apply: ${filePath}`);
    }
  }
  for (const destination of prepared.destinations) {
    destinationAbsent(root, destination);
  }
}

function replaceFile(source, destination) {
  try {
    fs.renameSync(source, destination);
  } catch (error) {
    if (!["EEXIST", "EPERM"].includes(error.code)) throw error;
    const metadata = fs.lstatSync(destination);
    if (metadata.isSymbolicLink() || !metadata.isFile()) throw error;
    fs.unlinkSync(destination);
    fs.renameSync(source, destination);
  }
}

function sourceOverlay(prepared) {
  const overlay = Object.create(null);
  const paths = new Set([...prepared.initial.keys(), ...prepared.final.keys()]);
  for (const filePath of [...paths].sort(utf8Compare)) {
    overlay[filePath] = prepared.final.has(filePath)
      ? Buffer.from(prepared.final.get(filePath).data)
      : null;
  }
  return Object.freeze(overlay);
}

function commitPrepared(root, prepared) {
  const affected = new Set();
  for (const change of prepared.changes) {
    affected.add(change.source_path || change.path);
    affected.add(change.path);
  }
  const stage = fs.mkdtempSync(path.join(root, ".archbird-act-"));
  const stagedNew = new Map();
  const stagedOld = new Map();
  const createdDirectories = [];
  let committed = false;
  let mutated = false;
  let primaryError = null;
  try {
    const ordered = [...affected].sort(utf8Compare);
    for (let index = 0; index < ordered.length; index += 1) {
      const filePath = ordered[index];
      if (prepared.final.has(filePath)) {
        const state = prepared.final.get(filePath);
        const temporary = path.join(stage, `new-${index}`);
        writeStageFile(temporary, state.data, state.mode);
        stagedNew.set(filePath, temporary);
      }
      if (prepared.initial.has(filePath)) {
        const state = prepared.initial.get(filePath);
        const backup = path.join(stage, `old-${index}`);
        writeStageFile(backup, state.data, state.mode);
        stagedOld.set(filePath, backup);
      }
    }
    revalidate(root, prepared);
    for (const [filePath, temporary] of [...stagedNew].sort(
      ([left], [right]) => utf8Compare(left, right),
    )) {
      makeParents(root, filePath, createdDirectories);
      checkPathComponents(root, filePath, false);
      replaceFile(temporary, candidate(root, filePath));
      mutated = true;
    }
    for (const filePath of [...prepared.initial.keys()]
      .filter((value) => !prepared.final.has(value))
      .sort(utf8Compare)) {
      checkPathComponents(root, filePath, true);
      fs.unlinkSync(candidate(root, filePath));
      mutated = true;
    }
    committed = true;
  } catch (error) {
    primaryError = error;
    throw error;
  } finally {
    const rollbackErrors = [];
    if (mutated && !committed) {
      for (const filePath of [...prepared.final.keys()]
        .filter((value) => !prepared.initial.has(value))
        .sort(utf8Compare)
        .reverse()) {
        try {
          const target = candidate(root, filePath);
          const metadata = fs.lstatSync(target);
          if (!metadata.isSymbolicLink() && metadata.isFile()) fs.unlinkSync(target);
        } catch (error) {
          if (error.code !== "ENOENT") {
            rollbackErrors.push(`remove ${filePath}: ${error.message}`);
          }
        }
      }
      for (const [filePath, backup] of [...stagedOld].sort(
        ([left], [right]) => utf8Compare(left, right),
      )) {
        try {
          if (!fs.existsSync(backup)) {
            rollbackErrors.push(`restore ${filePath}: backup is missing`);
            continue;
          }
          makeParents(root, filePath, createdDirectories);
          replaceFile(backup, candidate(root, filePath));
        } catch (error) {
          rollbackErrors.push(`restore ${filePath}: ${error.message}`);
        }
      }
    }
    fs.rmSync(stage, { force: true, recursive: true });
    if (!committed) {
      for (const directory of createdDirectories.reverse()) {
        try {
          fs.rmdirSync(directory);
        } catch (_) {
          // Non-empty or externally changed directories are preserved.
        }
      }
    }
    if (rollbackErrors.length) {
      const prefix = primaryError ? `${primaryError.message}; ` : "";
      throw new Error(
        `${prefix}rollback was incomplete: ${rollbackErrors.join("; ")}`,
      );
    }
  }
}

function execute(plan, root, apply, verifyAcceptance, mapDocument = null) {
  let planDigest;
  let planSnapshot;
  try {
    const canonicalPlan = canonicalBytes(plan, MAX_PLAN_BYTES);
    planDigest = digest(canonicalPlan);
    planSnapshot = JSON.parse(canonicalPlan.toString("utf8"));
    if (!isObject(planSnapshot)) throw new Error("Plan must be a JSON object");
  } catch (error) {
    const fallback = digest(Buffer.from(
      `invalid-plan\0${String(error.message).slice(0, MAX_METADATA_BYTES)}`,
      "utf8",
    ));
    return result(fallback, "blocked", [], [
      diagnostic(
        "invalid_plan",
        `Plan is not canonical JSON: ${error.message}`,
      ),
    ]);
  }
  try {
    validatePlanShape(planSnapshot);
  } catch (error) {
    return result(planDigest, "blocked", [], [
      diagnostic("invalid_plan", error.message),
    ]);
  }
  let repository;
  let prepared;
  try {
    const requested = path.resolve(String(root));
    const metadata = fs.lstatSync(requested);
    if (metadata.isSymbolicLink()) {
      throw new Error("repository root must not be a symlink");
    }
    repository = fs.realpathSync(requested);
    if (!fs.statSync(repository).isDirectory()) {
      throw new Error("repository root must be a directory");
    }
    prepared = prepare(planSnapshot, repository, mapDocument);
  } catch (error) {
    return result(planDigest, "blocked", [], [
      diagnostic("invalid_root", error.message),
    ]);
  }
  if (prepared.diagnostics.length > 0) {
    return result(planDigest, "blocked", [], prepared.diagnostics);
  }
  if (!apply) return result(planDigest, "preview", prepared.changes, []);
  const requiredConstraintIds = expectedAcceptanceIds(planSnapshot);
  let acceptance;
  try {
    revalidate(repository, prepared);
    acceptance = validateAcceptance(
      verifyAcceptance(
        structuredClone(planSnapshot),
        repository,
        sourceOverlay(prepared),
      ),
      requiredConstraintIds,
    );
  } catch (error) {
    return result(planDigest, "failed", prepared.changes, [
      diagnostic(
        "acceptance_failed",
        "Plan changes were not written because isolated acceptance " +
        "evaluation failed: " +
        error.message,
      ),
    ]);
  }
  if (acceptance.status !== "satisfied") {
    return result(
      planDigest,
      "rejected",
      prepared.changes,
      [
        diagnostic(
          "acceptance_rejected",
          "Plan changes were not written because isolated fresh acceptance is " +
          `${acceptance.status}.`,
        ),
      ],
      acceptance,
    );
  }
  try {
    commitPrepared(repository, prepared);
  } catch (error) {
    return result(
      planDigest,
      "failed",
      prepared.changes,
      [
        diagnostic(
          "commit_failed",
          "Accepted Plan changes could not be committed transactionally: " +
          error.message,
        ),
      ],
      acceptance,
    );
  }
  return result(planDigest, "applied", prepared.changes, [], acceptance);
}

function previewPlan(plan, root, mapDocument = null) {
  return execute(plan, root, false, null, mapDocument);
}

function applyPlan(plan, root, verifyAcceptance, mapDocument = null) {
  if (typeof verifyAcceptance !== "function") {
    throw new TypeError("applyPlan requires an acceptance callback");
  }
  return execute(plan, root, true, verifyAcceptance, mapDocument);
}

module.exports = {
  applyPlan,
  planSha256,
  previewPlan,
  validatePlanDocument(plan) {
    const canonical = canonicalBytes(plan, MAX_PLAN_BYTES);
    const snapshot = JSON.parse(canonical.toString("utf8"));
    if (!isObject(snapshot)) throw new Error("Plan must be a JSON object");
    validatePlanShape(snapshot);
    return snapshot;
  },
};
