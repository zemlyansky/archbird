"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const CACHE_CONTRACT = Buffer.from("archbird-provider-cache-v1");
const DEFAULT_MAX_BYTES = 1024 * 1024 * 1024;

function defaultProviderCacheDir() {
  if (process.env.ARCHBIRD_CACHE_DIR) {
    return path.resolve(process.env.ARCHBIRD_CACHE_DIR);
  }
  if (process.env.XDG_CACHE_HOME) {
    return path.resolve(process.env.XDG_CACHE_HOME, "archbird");
  }
  return path.join(os.homedir(), ".cache", "archbird");
}

function defaultProviderCacheMaxBytes() {
  const configured = process.env.ARCHBIRD_CACHE_MAX_BYTES;
  if (configured === undefined) return DEFAULT_MAX_BYTES;
  if (!/^[0-9]+$/.test(configured)) {
    throw new TypeError("ARCHBIRD_CACHE_MAX_BYTES must be a positive integer");
  }
  const value = Number(configured);
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new TypeError("ARCHBIRD_CACHE_MAX_BYTES must be a positive safe integer");
  }
  return value;
}

function emptyProviderCacheStats() {
  return {
    bytes: 0, errors: 0, evictions: 0, hits: 0, invalid: 0,
    misses: 0, noSpace: 0, skipped: 0, temporariesRemoved: 0, writes: 0,
  };
}

function frame(hash, value) {
  const length = Buffer.alloc(8);
  length.writeBigUInt64BE(BigInt(value.length));
  hash.update(length);
  hash.update(value);
}

function cacheKey({ namespace, project, providerId, path: subjectPath, sourceSha256 }) {
  const hash = crypto.createHash("sha256");
  for (const value of [
    CACHE_CONTRACT,
    Buffer.from(namespace, "ascii"),
    Buffer.from(project, "utf8"),
    Buffer.from(providerId, "utf8"),
    Buffer.from(subjectPath, "utf8"),
    Buffer.from(sourceSha256, "ascii"),
  ]) {
    frame(hash, value);
  }
  return hash.digest("hex");
}

class ProviderCache {
  constructor(root, { maxBytes = defaultProviderCacheMaxBytes() } = {}) {
    if (!Number.isSafeInteger(maxBytes) || maxBytes <= 0) {
      throw new TypeError("provider cache maxBytes must be a positive safe integer");
    }
    this.root = path.resolve(root);
    this.maxBytes = maxBytes;
    this.stats = emptyProviderCacheStats();
    this.entries = new Map();
    this.inventory();
  }

  inventory() {
    const base = path.join(this.root, "providers-v1");
    const pending = [base];
    while (pending.length) {
      const directory = pending.pop();
      let entries;
      try {
        entries = fs.readdirSync(directory, { withFileTypes: true });
      } catch (error) {
        if (error.code !== "ENOENT") this.stats.errors += 1;
        continue;
      }
      for (const entry of entries) {
        const candidate = path.join(directory, entry.name);
        if (entry.isDirectory()) {
          pending.push(candidate);
          continue;
        }
        if (!entry.isFile()) continue;
        if (entry.name.startsWith(".") && entry.name.endsWith(".tmp")) {
          try {
            fs.unlinkSync(candidate);
            this.stats.temporariesRemoved += 1;
          } catch (error) {
            if (error.code !== "ENOENT") this.stats.errors += 1;
          }
          continue;
        }
        if (!entry.name.endsWith(".json")) continue;
        try {
          const metadata = fs.statSync(candidate);
          this.entries.set(candidate, {
            mtime: metadata.mtimeMs,
            size: metadata.size,
          });
          this.stats.bytes += metadata.size;
        } catch (error) {
          if (error.code !== "ENOENT") this.stats.errors += 1;
        }
      }
    }
    this.prune(0);
  }

  prune(incoming, preserve = null) {
    if (incoming > this.maxBytes) return;
    const ordered = [...this.entries.entries()].sort((left, right) =>
      left[1].mtime - right[1].mtime || Buffer.compare(
        Buffer.from(left[0]), Buffer.from(right[0]),
      ));
    for (const [candidate, metadata] of ordered) {
      if (this.stats.bytes + incoming <= this.maxBytes) break;
      if (candidate === preserve) continue;
      try {
        fs.unlinkSync(candidate);
      } catch (error) {
        if (error.code !== "ENOENT") {
          this.stats.errors += 1;
          continue;
        }
      }
      this.entries.delete(candidate);
      this.stats.bytes -= metadata.size;
      this.stats.evictions += 1;
    }
  }

  target(parameters) {
    const key = cacheKey(parameters);
    return path.join(this.root, "providers-v1", key.slice(0, 2), `${key}.json`);
  }

  load({
    namespace,
    project,
    providerId,
    path: subjectPath,
    sourceSha256,
  }) {
    const target = this.target({
      namespace,
      project,
      providerId,
      path: subjectPath,
      sourceSha256,
    });
    try {
      const data = fs.readFileSync(target);
      this.stats.hits += 1;
      return data;
    } catch (error) {
      if (error.code !== "ENOENT") {
        this.stats.errors += 1;
      }
      this.stats.misses += 1;
      return null;
    }
  }

  reject({ namespace, project, providerId, path: subjectPath, sourceSha256 }) {
    const target = this.target({
      namespace,
      project,
      providerId,
      path: subjectPath,
      sourceSha256,
    });
    if (this.stats.hits) this.stats.hits -= 1;
    this.stats.invalid += 1;
    this.stats.misses += 1;
    try {
      fs.unlinkSync(target);
    } catch (error) {
      if (error.code !== "ENOENT") {
        this.stats.errors += 1;
        return;
      }
    }
    const prior = this.entries.get(target);
    if (prior !== undefined) {
      this.entries.delete(target);
      this.stats.bytes -= prior.size;
    }
  }

  store(
    data,
    { namespace, project, providerId, path: subjectPath, sourceSha256 },
  ) {
    const target = this.target({
      namespace,
      project,
      providerId,
      path: subjectPath,
      sourceSha256,
    });
    if (data.length > this.maxBytes) {
      this.stats.skipped += 1;
      return;
    }
    const prior = this.entries.get(target);
    const priorSize = prior === undefined ? 0 : prior.size;
    const incoming = Math.max(0, data.length - priorSize);
    this.prune(incoming, target);
    if (this.stats.bytes + incoming > this.maxBytes) {
      this.stats.skipped += 1;
      return;
    }
    const temporary = path.join(
      path.dirname(target),
      `.${path.basename(target)}.${process.pid}.${crypto.randomBytes(8).toString("hex")}.tmp`,
    );
    try {
      fs.mkdirSync(path.dirname(target), { recursive: true });
      const descriptor = fs.openSync(temporary, "wx");
      try {
        fs.writeFileSync(descriptor, data);
        fs.fsyncSync(descriptor);
      } finally {
        fs.closeSync(descriptor);
      }
      fs.renameSync(temporary, target);
      const metadata = fs.statSync(target);
      this.stats.bytes += metadata.size - priorSize;
      this.entries.set(target, { mtime: metadata.mtimeMs, size: metadata.size });
      this.stats.writes += 1;
    } catch (error) {
      this.stats.errors += 1;
      if (["ENOSPC", "EDQUOT"].includes(error.code)) this.stats.noSpace += 1;
    } finally {
      try {
        fs.unlinkSync(temporary);
      } catch (_) {
        // The successful atomic rename consumes the temporary path.
      }
    }
  }
}

module.exports = {
  ProviderCache,
  cacheKey,
  defaultProviderCacheDir,
  defaultProviderCacheMaxBytes,
  emptyProviderCacheStats,
};
