"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const native = require("./native");

const MAX_FILE_BYTES = 64 * 1024 * 1024;
const GATE_DEFAULT_OUTPUT_BYTES = 1024 * 1024;
const GATE_TAIL_BYTES = 64 * 1024;

function relativePath(value) {
  if (
    typeof value !== "string" ||
    value.length === 0 ||
    value.includes("\\") ||
    path.posix.isAbsolute(value) ||
    /^[A-Za-z]:/.test(value)
  ) {
    throw new Error(`unsafe repository path: ${String(value)}`);
  }
  const normalized = path.posix.normalize(value);
  if (
    normalized !== value ||
    value.split("/").some((part) => !part || part === "." || part === "..")
  ) {
    throw new Error(`unsafe repository path: ${value}`);
  }
  return value;
}

function repositoryRoot(value) {
  const requested = path.resolve(value);
  const resolved = fs.realpathSync(requested);
  const metadata = fs.lstatSync(resolved);
  if (!metadata.isDirectory()) {
    throw new Error("repository root must resolve to a directory");
  }
  return resolved;
}

function candidate(root, relative) {
  return path.join(root, ...relative.split("/"));
}

function checkParents(root, relative) {
  const parts = relative.split("/").slice(0, -1);
  let cursor = root;
  for (const part of parts) {
    cursor = path.join(cursor, part);
    try {
      const metadata = fs.lstatSync(cursor);
      if (metadata.isSymbolicLink() || !metadata.isDirectory()) {
        throw new Error(`unsafe repository path parent: ${relative}`);
      }
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
  }
}

function readRegular(root, relative) {
  checkParents(root, relative);
  const filePath = candidate(root, relative);
  const before = fs.lstatSync(filePath);
  if (before.isSymbolicLink() || !before.isFile()) {
    throw new Error(`source is not a regular file: ${relative}`);
  }
  if (before.size > MAX_FILE_BYTES) {
    throw new Error(
      `source exceeds the ${MAX_FILE_BYTES}-byte Act limit: ${relative}`,
    );
  }
  const descriptor = fs.openSync(
    filePath,
    fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0),
  );
  try {
    const opened = fs.fstatSync(descriptor, { bigint: true });
    if (
      !opened.isFile() ||
      opened.dev !== BigInt(before.dev) ||
      opened.ino !== BigInt(before.ino)
    ) {
      throw new Error(`source changed while opening: ${relative}`);
    }
    const length = Number(opened.size);
    if (!Number.isSafeInteger(length) || length > MAX_FILE_BYTES) {
      throw new Error(
        `source exceeds the ${MAX_FILE_BYTES}-byte Act limit: ${relative}`,
      );
    }
    const data = Buffer.alloc(length);
    let offset = 0;
    while (offset < data.length) {
      const count = fs.readSync(
        descriptor,
        data,
        offset,
        data.length - offset,
        offset,
      );
      if (count === 0) {
        throw new Error(`source changed while reading: ${relative}`);
      }
      offset += count;
    }
    const after = fs.fstatSync(descriptor, { bigint: true });
    if (
      after.dev !== opened.dev ||
      after.ino !== opened.ino ||
      after.size !== opened.size ||
      after.mtimeNs !== opened.mtimeNs
    ) {
      throw new Error(`source changed while reading: ${relative}`);
    }
    return { data, mode: Number(opened.mode) };
  } finally {
    fs.closeSync(descriptor);
  }
}

function requireAbsent(root, relative) {
  checkParents(root, relative);
  try {
    fs.lstatSync(candidate(root, relative));
  } catch (error) {
    if (error.code === "ENOENT") return;
    throw error;
  }
  throw new Error(`Act destination already exists: ${relative}`);
}

function observeSourceRequirements(rootValue, requirementsJson) {
  const root = repositoryRoot(rootValue);
  const document = JSON.parse(Buffer.from(requirementsJson).toString("utf8"));
  if (
    !document ||
    Array.isArray(document) ||
    Object.keys(document).sort().join(",") !== "absent,files,observe" ||
    !Array.isArray(document.files) ||
    !Array.isArray(document.absent) ||
    !Array.isArray(document.observe)
  ) {
    throw new Error("native source requirements have an invalid shape");
  }
  const files = document.files.map((rawPath) => {
    const filePath = relativePath(rawPath);
    const state = readRegular(root, filePath);
    return {
      path: filePath,
      sha256: crypto.createHash("sha256").update(state.data).digest("hex"),
      executable: Boolean(state.mode & 0o111),
    };
  }).sort((left, right) => Buffer.compare(
    Buffer.from(left.path),
    Buffer.from(right.path),
  ));
  const absent = document.absent.map((rawPath) => {
    const filePath = relativePath(rawPath);
    requireAbsent(root, filePath);
    return filePath;
  }).sort((left, right) => Buffer.compare(
    Buffer.from(left),
    Buffer.from(right),
  ));
  for (const rawPath of document.observe) {
    const filePath = relativePath(rawPath);
    try {
      const state = readRegular(root, filePath);
      files.push({
        path: filePath,
        sha256: crypto.createHash("sha256").update(state.data).digest("hex"),
        executable: Boolean(state.mode & 0o111),
      });
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
      absent.push(filePath);
    }
  }
  files.sort((left, right) => Buffer.compare(
    Buffer.from(left.path),
    Buffer.from(right.path),
  ));
  absent.sort((left, right) => Buffer.compare(
    Buffer.from(left),
    Buffer.from(right),
  ));
  return native.jsonCanonicalize(
    Buffer.from(JSON.stringify({ files, absent })),
  );
}

function observePlanSources(
  root,
  planJson,
  executorSubmissionsJson = Buffer.alloc(0),
) {
  return observeSourceRequirements(
    root,
    native.planSourceRequirements(
      Buffer.from(planJson),
      Buffer.from(executorSubmissionsJson),
      false,
    ),
  );
}

function observeActSources(root, actJson) {
  const repository = repositoryRoot(root);
  const requirements = JSON.parse(
    native.actSourceRequirements(Buffer.from(actJson), false).toString("utf8"),
  );
  if (
    !requirements ||
    Array.isArray(requirements) ||
    Object.keys(requirements).join(",") !== "paths" ||
    !Array.isArray(requirements.paths)
  ) {
    throw new Error("native Act source requirements have an invalid shape");
  }
  const files = [];
  const absent = [];
  for (const rawPath of requirements.paths) {
    const filePath = relativePath(rawPath);
    try {
      const state = readRegular(repository, filePath);
      files.push({
        path: filePath,
        sha256: crypto.createHash("sha256").update(state.data).digest("hex"),
        executable: Boolean(state.mode & 0o111),
      });
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
      absent.push(filePath);
    }
  }
  files.sort((left, right) => Buffer.compare(
    Buffer.from(left.path),
    Buffer.from(right.path),
  ));
  absent.sort((left, right) => Buffer.compare(
    Buffer.from(left),
    Buffer.from(right),
  ));
  return native.jsonCanonicalize(
    Buffer.from(JSON.stringify({ files, absent })),
  );
}

function actOverlay(actJson) {
  native.actValidate(Buffer.from(actJson));
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  const overlay = Object.create(null);
  for (const transition of document.transitions) {
    const kind = transition.kind;
    const filePath = relativePath(transition.path);
    if (kind === "delete") {
      overlay[filePath] = null;
      continue;
    }
    const data = Buffer.from(transition.after.content_base64, "base64");
    if (kind === "move") {
      overlay[relativePath(transition.source_path)] = null;
    }
    overlay[filePath] = data;
  }
  return Object.freeze(overlay);
}

function safeSymlink(root, relative) {
  const source = candidate(root, relative);
  const target = fs.readlinkSync(source);
  if (path.isAbsolute(target)) {
    throw new Error(`absolute symbolic link is not isolated: ${relative}`);
  }
  const resolved = path.resolve(path.dirname(source), target);
  const relation = path.relative(root, resolved);
  if (relation === ".." || relation.startsWith(`..${path.sep}`)) {
    throw new Error(`symbolic link escapes the repository: ${relative}`);
  }
  return target;
}

function recursiveInventory(root, prefix = "") {
  const rows = [];
  const start = prefix ? candidate(root, prefix) : root;
  function visit(directory) {
    const entries = fs.readdirSync(directory, { withFileTypes: true })
      .sort((left, right) => Buffer.compare(
        Buffer.from(left.name),
        Buffer.from(right.name),
      ));
    for (const entry of entries) {
      if (
        entry.name === ".git" ||
        entry.name.startsWith(".archbird-gates-")
      ) {
        continue;
      }
      const absolute = path.join(directory, entry.name);
      const relative = path.relative(root, absolute).split(path.sep).join("/");
      if (relative === ".archbird-apply.lock") continue;
      const metadata = fs.lstatSync(absolute);
      if (metadata.isSymbolicLink() || metadata.isFile()) {
        rows.push(relativePath(relative));
      } else if (metadata.isDirectory()) {
        visit(absolute);
      } else {
        throw new Error(
          `unsupported repository entry in gate workspace: ${relative}`,
        );
      }
    }
  }
  visit(start);
  return rows;
}

function repositoryInventory(root) {
  const top = spawnSync(
    "git",
    ["-C", root, "rev-parse", "--show-toplevel"],
    { encoding: "utf8", timeout: 10000, windowsHide: true },
  );
  if (
    top.status !== 0 ||
    !top.stdout ||
    fs.realpathSync(top.stdout.trim()) !== root
  ) {
    return recursiveInventory(root);
  }
  const listed = spawnSync(
    "git",
    [
      "-C",
      root,
      "ls-files",
      "-z",
      "--cached",
      "--others",
      "--exclude-standard",
    ],
    {
      encoding: null,
      maxBuffer: 64 * 1024 * 1024,
      timeout: 30000,
      windowsHide: true,
    },
  );
  if (listed.status !== 0 || !Buffer.isBuffer(listed.stdout)) {
    return recursiveInventory(root);
  }
  const rows = [];
  for (const raw of listed.stdout.toString("utf8").split("\0")) {
    if (!raw) continue;
    const relative = relativePath(raw);
    const source = candidate(root, relative);
    let metadata;
    try {
      metadata = fs.lstatSync(source);
    } catch (error) {
      if (error.code === "ENOENT") continue;
      throw error;
    }
    if (metadata.isDirectory()) {
      rows.push(...recursiveInventory(root, relative));
    } else {
      rows.push(relative);
    }
  }
  return [...new Set(rows)].sort((left, right) =>
    Buffer.compare(Buffer.from(left), Buffer.from(right))
  );
}

function copyGateWorkspace(root, workspace) {
  for (const relative of repositoryInventory(root)) {
    const source = candidate(root, relative);
    const destination = candidate(workspace, relative);
    const metadata = fs.lstatSync(source);
    fs.mkdirSync(path.dirname(destination), { recursive: true });
    if (metadata.isSymbolicLink()) {
      fs.symlinkSync(safeSymlink(root, relative), destination);
    } else if (metadata.isFile()) {
      fs.copyFileSync(source, destination);
      fs.chmodSync(destination, metadata.mode & 0o777);
    } else {
      throw new Error(
        `unsupported repository entry in gate workspace: ${relative}`,
      );
    }
  }
}

function writeWorkspaceFile(filePath, data, executable) {
  fs.mkdirSync(path.dirname(filePath), { recursive: true });
  fs.writeFileSync(filePath, data);
  let mode = fs.statSync(filePath).mode & 0o777;
  mode = executable ? mode | 0o111 : mode & ~0o111;
  fs.chmodSync(filePath, mode);
}

function applyMaterializedAct(workspace, actJson) {
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  for (const transition of document.transitions) {
    const kind = transition.kind;
    const filePath = relativePath(transition.path);
    const destination = candidate(workspace, filePath);
    const sourcePath = kind === "move"
      ? relativePath(transition.source_path)
      : filePath;
    const source = candidate(workspace, sourcePath);
    if (kind === "create") {
      if (fs.existsSync(destination)) {
        throw new Error(`gate workspace destination exists: ${filePath}`);
      }
    } else {
      const state = readRegular(workspace, sourcePath);
      const before = transition.before;
      if (
        crypto.createHash("sha256").update(state.data).digest("hex") !==
          before.sha256 ||
        Boolean(state.mode & 0o111) !== before.executable
      ) {
        throw new Error(`gate workspace source differs from Act: ${sourcePath}`);
      }
    }
    if (kind === "delete") {
      fs.unlinkSync(source);
      continue;
    }
    if (kind === "move" && fs.existsSync(destination)) {
      throw new Error(`gate workspace destination exists: ${filePath}`);
    }
    const after = transition.after;
    writeWorkspaceFile(
      destination,
      Buffer.from(after.content_base64, "base64"),
      after.executable,
    );
    if (kind === "move") fs.unlinkSync(source);
  }
}

function workspaceSha256(workspace) {
  const digest = crypto.createHash("sha256");
  for (const relative of recursiveInventory(workspace)) {
    const filePath = candidate(workspace, relative);
    const metadata = fs.lstatSync(filePath);
    if (metadata.isSymbolicLink()) {
      digest.update(Buffer.from("L\0"));
      digest.update(Buffer.from(relative));
      digest.update(Buffer.from("\0"));
      digest.update(Buffer.from(fs.readlinkSync(filePath)));
      digest.update(Buffer.from("\0"));
    } else if (metadata.isFile()) {
      digest.update(Buffer.from("F\0"));
      digest.update(Buffer.from(relative));
      digest.update(Buffer.from("\0"));
      digest.update(Buffer.from(metadata.mode & 0o111 ? "1\0" : "0\0"));
      digest.update(
        crypto.createHash("sha256").update(fs.readFileSync(filePath)).digest(),
      );
    } else {
      throw new Error(`unsupported gate workspace entry: ${relative}`);
    }
  }
  return digest.digest("hex");
}

function gateEnvironment(cwd) {
  return { ...process.env, PWD: cwd };
}

function environmentSha256(environment) {
  const ordered = Object.fromEntries(
    Object.entries(environment).sort(([left], [right]) =>
      Buffer.compare(Buffer.from(left), Buffer.from(right))
    ),
  );
  const canonical = native.jsonCanonicalize(
    Buffer.from(JSON.stringify(ordered)),
  );
  return crypto.createHash("sha256").update(canonical).digest("hex");
}

function definitionSha256(definition) {
  const canonical = native.jsonCanonicalize(
    Buffer.from(JSON.stringify(definition)),
  );
  return crypto.createHash("sha256").update(canonical).digest("hex");
}

function tailBase64(value) {
  const data = Buffer.isBuffer(value) ? value : Buffer.alloc(0);
  return data.subarray(Math.max(0, data.length - GATE_TAIL_BYTES))
    .toString("base64");
}

function gateResult(gateId, definition, workspace) {
  const started = process.hrtime.bigint();
  const maxOutput = definition.max_output_bytes ?? GATE_DEFAULT_OUTPUT_BYTES;
  const cwd = candidate(workspace, definition.cwd || ".");
  const environment = gateEnvironment(cwd);
  const environmentHash = environmentSha256(environment);
  const command = spawnSync(
    definition.argv[0],
    definition.argv.slice(1),
    {
      cwd,
      encoding: null,
      env: environment,
      input: Buffer.alloc(0),
      killSignal: "SIGKILL",
      maxBuffer: Math.max(1, maxOutput + 1),
      timeout: definition.timeout_seconds * 1000,
      windowsHide: true,
    },
  );
  const stdout = Buffer.isBuffer(command.stdout)
    ? command.stdout
    : Buffer.alloc(0);
  const stderr = Buffer.isBuffer(command.stderr)
    ? command.stderr
    : Buffer.alloc(0);
  let status = "error";
  let exitCode = null;
  if (
    stdout.length + stderr.length > maxOutput ||
    command.error?.code === "ENOBUFS"
  ) {
    status = "output_limit";
  } else if (command.error?.code === "ETIMEDOUT") {
    status = "timeout";
  } else if (command.status === 0) {
    status = "pass";
    exitCode = 0;
  } else if (Number.isSafeInteger(command.status) && command.status >= 0) {
    status = "fail";
    exitCode = command.status;
  }
  const durationMs = Number((process.hrtime.bigint() - started) / 1000000n);
  return {
    id: gateId,
    definition_sha256: definitionSha256(definition),
    status,
    exit_code: exitCode,
    duration_ms: durationMs,
    environment_sha256: environmentHash,
    stdout_sha256: crypto.createHash("sha256").update(stdout).digest("hex"),
    stderr_sha256: crypto.createHash("sha256").update(stderr).digest("hex"),
    stdout_tail_base64: tailBase64(stdout),
    stderr_tail_base64: tailBase64(stderr),
  };
}

function gateOrder(gates) {
  const remaining = new Set(Object.keys(gates));
  const emitted = [];
  while (remaining.size) {
    const ready = [...remaining]
      .filter((gateId) =>
        (gates[gateId].depends_on || []).every((id) => emitted.includes(id))
      )
      .sort((left, right) =>
        Buffer.compare(Buffer.from(left), Buffer.from(right))
      );
    if (!ready.length) throw new Error("gate dependencies contain a cycle");
    emitted.push(ready[0]);
    remaining.delete(ready[0]);
  }
  return emitted;
}

function runActGates(rootValue, actJson) {
  const root = repositoryRoot(rootValue);
  native.actValidate(Buffer.from(actJson));
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  if (!document.gates || Array.isArray(document.gates)) {
    throw new Error("Act has no valid gate definitions");
  }
  if (!Object.keys(document.gates).length) return Buffer.alloc(0);
  const workspace = fs.mkdtempSync(
    path.join(path.dirname(root), ".archbird-gates-"),
  );
  try {
    copyGateWorkspace(root, workspace);
    applyMaterializedAct(workspace, actJson);
    const workspaceHash = workspaceSha256(workspace);
    const results = Object.create(null);
    const emptyHash = crypto.createHash("sha256").update(Buffer.alloc(0))
      .digest("hex");
    for (const gateId of gateOrder(document.gates)) {
      const definition = document.gates[gateId];
      if (
        (definition.depends_on || [])
          .some((dependency) => results[dependency].status !== "pass")
      ) {
        results[gateId] = {
          id: gateId,
          definition_sha256: definitionSha256(definition),
          status: "blocked",
          exit_code: null,
          duration_ms: 0,
          environment_sha256: environmentSha256(
            gateEnvironment(candidate(workspace, definition.cwd || ".")),
          ),
          stdout_sha256: emptyHash,
          stderr_sha256: emptyHash,
          stdout_tail_base64: "",
          stderr_tail_base64: "",
        };
      } else {
        results[gateId] = gateResult(gateId, definition, workspace);
      }
    }
    return native.jsonCanonicalize(Buffer.from(JSON.stringify({
      workspace_sha256: workspaceHash,
      results: Object.keys(results).sort().map((gateId) => results[gateId]),
    })));
  } finally {
    fs.rmSync(workspace, { force: true, recursive: true });
  }
}

function gateFailureDetails(gateResultsJson) {
  if (!gateResultsJson?.length) return "";
  const document = JSON.parse(Buffer.from(gateResultsJson).toString("utf8"));
  const details = [];
  for (const row of document.results || []) {
    if (row.status === "pass") continue;
    const exitText = row.exit_code === null
      ? "no exit code"
      : `exit ${row.exit_code}`;
    details.push(
      `Gate ${row.id || "<unknown>"}: ${row.status || "error"} ` +
      `(${exitText}, ${row.duration_ms || 0} ms)`,
    );
    for (const stream of ["stderr", "stdout"]) {
      const encoded = row[`${stream}_tail_base64`];
      if (!encoded) continue;
      const tail = Buffer.from(encoded, "base64").toString("utf8")
        .replace(/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/g, "?")
        .trim();
      if (tail) {
        details.push(`${stream} tail:\n${tail.slice(-4096)}`);
        break;
      }
    }
  }
  return details.join("\n");
}

function renderAct(rootValue, actJson, { format, pretty = false }) {
  const root = repositoryRoot(rootValue);
  native.actValidate(Buffer.from(actJson));
  if (format === "json") {
    return native.jsonCanonicalize(Buffer.from(actJson), pretty, false);
  }
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  const diffs = [];
  for (const transition of document.transitions) {
    const kind = transition.kind;
    const filePath = relativePath(transition.path);
    const sourcePath = kind === "move"
      ? relativePath(transition.source_path)
      : filePath;
    const before = kind === "create"
      ? Buffer.alloc(0)
      : readRegular(root, sourcePath).data;
    const after = kind === "delete"
      ? Buffer.alloc(0)
      : Buffer.from(transition.after.content_base64, "base64");
    diffs.push(native.unifiedDiff(
      before,
      after,
      kind === "create" ? null : sourcePath,
      kind === "delete" ? null : filePath,
      Buffer.alloc(0),
    ));
  }
  const patch = Buffer.concat(diffs);
  if (format === "patch") return patch;
  if (format !== "markdown") {
    throw new Error("Act format must be markdown, json, or patch");
  }
  const lines = [
    "# Accepted Act",
    "",
    `- Plan: \`${document.plan_sha256}\``,
    `- Transitions: ${document.transitions.length}`,
    `- Acceptance: \`${document.acceptance.status}\``,
  ];
  if (document.acceptance.constraints.length) {
    lines.push("", "## Constraints", "");
    for (const row of document.acceptance.constraints) {
      lines.push(`- \`${row.id}\`: \`${row.status}\``);
    }
  }
  if (document.acceptance.gate_results.length) {
    lines.push("", "## Gates", "");
    for (const row of document.acceptance.gate_results) {
      lines.push(`- \`${row.id}\`: \`${row.status}\` (${row.duration_ms} ms)`);
    }
  }
  if (patch.length) {
    lines.push(
      "",
      "## Diff",
      "",
      "```diff",
      patch.toString("utf8").replace(/\n$/, ""),
      "```",
    );
  }
  return Buffer.from(`${lines.join("\n")}\n`, "utf8");
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
    fs.fchmodSync(descriptor, mode);
    fs.fsyncSync(descriptor);
  } finally {
    fs.closeSync(descriptor);
  }
}

function makeParents(root, relative, created) {
  let cursor = root;
  for (const part of relative.split("/").slice(0, -1)) {
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

function transitionStates(root, actJson) {
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  const initial = new Map();
  const final = new Map();
  for (const transition of document.transitions) {
    const kind = transition.kind;
    const filePath = relativePath(transition.path);
    let sourcePath = filePath;
    if (kind === "move") {
      sourcePath = relativePath(transition.source_path);
      initial.set(sourcePath, readRegular(root, sourcePath));
    } else if (kind !== "create") {
      initial.set(filePath, readRegular(root, filePath));
    }
    if (kind !== "delete") {
      const data = Buffer.from(transition.after.content_base64, "base64");
      let mode = transition.after.executable ? 0o755 : 0o644;
      if (kind === "modify") mode = initial.get(filePath).mode & 0o777;
      if (kind === "move") mode = initial.get(sourcePath).mode & 0o777;
      mode = transition.after.executable ? mode | 0o111 : mode & ~0o111;
      final.set(filePath, { data, mode });
    }
  }
  return { initial, final };
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

function commitAct(root, actJson) {
  const { initial, final } = transitionStates(root, actJson);
  const affected = [...new Set([...initial.keys(), ...final.keys()])]
    .sort((left, right) => Buffer.compare(Buffer.from(left), Buffer.from(right)));
  const stage = fs.mkdtempSync(path.join(root, ".archbird-act-"));
  const stagedNew = new Map();
  const stagedOld = new Map();
  const createdDirectories = [];
  let mutated = false;
  let committed = false;
  let primaryError = null;
  try {
    for (let index = 0; index < affected.length; index += 1) {
      const filePath = affected[index];
      if (final.has(filePath)) {
        const state = final.get(filePath);
        const temporary = path.join(stage, `new-${index}`);
        writeStageFile(temporary, state.data, state.mode);
        stagedNew.set(filePath, temporary);
      }
      if (initial.has(filePath)) {
        const state = initial.get(filePath);
        const backup = path.join(stage, `old-${index}`);
        writeStageFile(backup, state.data, state.mode & 0o777);
        stagedOld.set(filePath, backup);
      }
    }
    const metadata = observeActSources(root, actJson);
    if (native.actPreflightApply(Buffer.from(actJson), metadata) !== 0) {
      throw new Error("Act became already satisfied during commit");
    }
    for (const [filePath, temporary] of stagedNew) {
      makeParents(root, filePath, createdDirectories);
      checkParents(root, filePath);
      replaceFile(temporary, candidate(root, filePath));
      mutated = true;
    }
    for (const filePath of [...initial.keys()]
      .filter((value) => !final.has(value))
      .sort()) {
      checkParents(root, filePath);
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
      for (const filePath of [...final.keys()]
        .filter((value) => !initial.has(value))
        .sort()
        .reverse()) {
        try {
          const target = candidate(root, filePath);
          const metadata = fs.lstatSync(target);
          if (!metadata.isSymbolicLink() && metadata.isFile()) {
            fs.unlinkSync(target);
          }
        } catch (error) {
          if (error.code !== "ENOENT") {
            rollbackErrors.push(`remove ${filePath}: ${error.message}`);
          }
        }
      }
      for (const [filePath, backup] of stagedOld) {
        if (!fs.existsSync(backup)) {
          rollbackErrors.push(`restore ${filePath}: backup is missing`);
          continue;
        }
        try {
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
          // Preserve non-empty or externally changed directories.
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

function applyAcceptedAct(rootValue, actJson) {
  const root = repositoryRoot(rootValue);
  const metadata = observeActSources(root, actJson);
  if (native.actPreflightApply(Buffer.from(actJson), metadata) === 1) return 0;
  const lockPath = path.join(root, ".archbird-apply.lock");
  const descriptor = fs.openSync(
    lockPath,
    fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_EXCL,
    0o600,
  );
  try {
    fs.writeFileSync(
      descriptor,
      `${os.hostname()}\n${process.pid}\n`,
      { encoding: "ascii" },
    );
    fs.fsyncSync(descriptor);
    const lockedMetadata = observeActSources(root, actJson);
    if (
      native.actPreflightApply(Buffer.from(actJson), lockedMetadata) === 1
    ) {
      return 0;
    }
    commitAct(root, actJson);
  } finally {
    fs.closeSync(descriptor);
    try {
      fs.unlinkSync(lockPath);
    } catch (error) {
      if (error.code !== "ENOENT") throw error;
    }
  }
  const document = JSON.parse(Buffer.from(actJson).toString("utf8"));
  return document.transitions.length;
}

module.exports = {
  applyAcceptedAct,
  observeActSources,
  observePlanSources,
  observeSourceRequirements,
  runActGates,
  gateFailureDetails,
  actOverlay,
  renderAct,
};
