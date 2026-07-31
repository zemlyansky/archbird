"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const native = require("./native");

const MAX_FILE_BYTES = 64 * 1024 * 1024;

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
  const metadata = fs.lstatSync(requested);
  if (metadata.isSymbolicLink() || !metadata.isDirectory()) {
    throw new Error("repository root must be a non-symlink directory");
  }
  return fs.realpathSync(requested);
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
    Object.keys(document).sort().join(",") !== "absent,files" ||
    !Array.isArray(document.files) ||
    !Array.isArray(document.absent)
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
  actOverlay,
  renderAct,
};
