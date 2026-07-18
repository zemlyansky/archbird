"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const CACHE_CONTRACT = Buffer.from("archbird-provider-cache-v1");

function defaultProviderCacheDir() {
  if (process.env.ARCHBIRD_CACHE_DIR) {
    return path.resolve(process.env.ARCHBIRD_CACHE_DIR);
  }
  if (process.env.XDG_CACHE_HOME) {
    return path.resolve(process.env.XDG_CACHE_HOME, "archbird");
  }
  return path.join(os.homedir(), ".cache", "archbird");
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
  constructor(root) {
    this.root = path.resolve(root);
    this.stats = { errors: 0, hits: 0, invalid: 0, misses: 0, writes: 0 };
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
    } catch (_) {
      // A missing or raced entry is already rejected.
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
      this.stats.writes += 1;
    } catch (_) {
      this.stats.errors += 1;
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
};
