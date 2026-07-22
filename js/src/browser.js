"use strict";

const { Buffer } = require("buffer");
const ts = require("typescript");
const { typescriptProviderBundles } = require("./providers/typescript");
const { createArchbirdCore } = require("./wasm");

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object" && !Buffer.isBuffer(value)) {
    return Object.fromEntries(
      Object.keys(value).sort(utf8Compare).map((key) => [key, canonical(value[key])]),
    );
  }
  return value;
}

function queryRequest(options = {}) {
  const request = {
    artifacts: options.artifacts || [],
    components: options.components || [],
    depth: options.depth ?? 1,
    direction: options.direction || "both",
    focus: options.focus || [],
    packages: options.packages || [],
    paths: options.paths || [],
    search: options.search || [],
    search_limit: options.searchLimit ?? 8,
    symbols: options.symbols || [],
    test_depth: options.testDepth ?? 8,
  };
  if (options.context !== undefined && options.context !== null) {
    request.context = options.context;
  }
  return canonical(request);
}

function queryProjection(options = {}) {
  const views = { focused: 0, changes: 1 };
  const details = { compact: 0, standard: 1, full: 2 };
  const view = options.view ?? "focused";
  const detail = options.detail ?? "standard";
  const compact = options.compact ?? false;
  const full = options.full ?? false;
  if (!Object.hasOwn(views, view)) {
    throw new RangeError("view must be focused or changes");
  }
  if (!Object.hasOwn(details, detail)) {
    throw new RangeError("detail must be compact, standard, or full");
  }
  if (compact && full) throw new RangeError("compact and full conflict");
  if ((compact || full) && detail !== "standard") {
    throw new RangeError("detail conflicts with compact/full alias");
  }
  return {
    detail: details[compact ? "compact" : (full ? "full" : detail)],
    view: views[view],
  };
}

function sourceRows(values) {
  return values.map((value) => {
    const split = value.indexOf("=");
    if (split <= 0 || split === value.length - 1) {
      throw new Error(`source override expects LANGUAGE=GLOB: ${value}`);
    }
    return { glob: value.slice(split + 1), language: value.slice(0, split) };
  });
}

function mapRequest(options = {}) {
  const request = {
    artifact: "archbird-map-request",
    exclude: [...(options.exclude || [])],
    ignore_files: [...(options.ignoreFiles || [])],
    only: [...(options.only || [])],
    schema_version: 1,
    sources: sourceRows(options.source || []),
  };
  if (options.project !== undefined && options.project !== null) {
    request.project = options.project;
  }
  if (options.ignore === false) request.ignore = false;
  if (options.defaultExcludes === false) request.default_excludes = false;
  if (options.maxFileBytes !== undefined && options.maxFileBytes !== null) {
    if (!Number.isSafeInteger(options.maxFileBytes) || options.maxFileBytes <= 0) {
      throw new Error("maxFileBytes must be a positive safe integer");
    }
    request.max_file_bytes = options.maxFileBytes;
  }
  if (options.maxIndexBytes !== undefined && options.maxIndexBytes !== null) {
    if (!Number.isSafeInteger(options.maxIndexBytes) || options.maxIndexBytes <= 0) {
      throw new Error("maxIndexBytes must be a positive safe integer");
    }
    request.max_index_bytes = options.maxIndexBytes;
  }
  return Buffer.from(JSON.stringify(canonical(request)));
}

function standardIgnore(pathname) {
  const leaf = pathname.slice(pathname.lastIndexOf("/") + 1);
  return [".gitignore", ".ignore", ".archbirdignore"].includes(leaf);
}

async function createBrowserArchbird(moduleOptions = {}) {
  const core = await createArchbirdCore(moduleOptions);
  const identities = require("./generated/identities.json");

  class Source {
    constructor(path, data) {
      if (typeof path !== "string" || !path) throw new TypeError("source path is required");
      this.path = path.replaceAll("\\", "/").replace(/^\.\//, "");
      this.data = Buffer.from(data);
    }
  }

  class Project {
    constructor(
      configJson,
      suppliedSources,
      { typescript = true, resolution = null, resolutionJson = null } = {},
    ) {
      this.configJson = Buffer.from(configJson);
      const sourceList = [...suppliedSources];
      const byPath = new Map(
        sourceList.map((source) => {
          const normalized = source instanceof Source
            ? source
            : new Source(source.path, source.data);
          return [normalized.path, normalized];
        }),
      );
      if (byPath.size !== sourceList.length) throw new Error("source paths must be unique");
      const discovery = resolution || JSON.parse(
        core.discoveryPlan(this.configJson, [...byPath.keys()].sort(utf8Compare)).toString("utf8"),
      );
      this.project = discovery.project;
      this.resolutionJson = resolutionJson;
      this.sources = discovery.files.map((row) => {
        const source = byPath.get(row.path);
        const isIndex = row.roles.includes("index");
        const byteLimit = isIndex
          ? discovery.max_index_bytes
          : discovery.max_file_bytes;
        const limitName = isIndex ? "max_index_bytes" : "max_file_bytes";
        if (!source) throw new Error(`selected source is unavailable: ${row.path}`);
        if (source.data.length > byteLimit) {
          throw new Error(
            `selected ${isIndex ? "index" : "source"} exceeds ` +
              `limits.${limitName}: ${row.path}`,
          );
        }
        return {
          ...row,
          data: source.data,
        };
      });
      const manifest = canonical({
        artifact: "archbird-source-manifest",
        configuration_sha256: discovery.configuration_sha256,
        files: this.sources.map((source) => ({
          bytes: source.data.length,
          ...(source.language ? { language: source.language } : {}),
          ...(source.layer ? { layer: source.layer } : {}),
          path: source.path,
          roles: source.roles,
          sha256: core.sha256(source.data),
        })),
        producer: {
          implementation_sha256: identities.browser_host_sha256,
          name: "archbird-browser-host",
          version: "1",
        },
        project: discovery.project,
        ...(resolution ? {
          resolution: {
            coverage: { ...resolution.coverage },
            profile: { ...resolution.profile },
            sha256: resolution.sha256,
          },
        } : {}),
        schema_version: 1,
      });
      this._handle = core.projectCreate(Buffer.from(JSON.stringify(manifest)));
      for (const source of this.sources) {
        core.projectAddSource(this._handle, source.path, source.data);
      }
      core.projectFinalizeSources(this._handle);
      core.projectSetConfig(this._handle, this.configJson);
      const supportMode = "augment";
      for (const providerId of [
        "lexical:c",
        "lexical:javascript",
        "lexical:python",
        "lexical:r",
      ]) {
        core.projectScanBuiltinProvider(this._handle, providerId, supportMode);
      }
      for (const providerId of [
        "syntax:tree-sitter:c",
        "syntax:tree-sitter:cpp",
        "syntax:tree-sitter:python",
        "syntax:tree-sitter:r",
      ]) {
        core.projectScanBuiltinProvider(this._handle, providerId, "primary");
      }
      for (const providerId of [
        "syntax:tree-sitter:javascript",
        "syntax:tree-sitter:typescript",
        "syntax:tree-sitter:tsx",
      ]) {
        core.projectScanBuiltinProvider(
          this._handle,
          providerId,
          typescript ? supportMode : "primary",
        );
      }
      if (
        typescript &&
        this.sources.some((source) => ["javascript", "typescript"].includes(source.language))
      ) {
        for (const bundle of typescriptProviderBundles({
          project: this.project,
          sources: this.sources,
          sourceManifestSha256: core.projectManifestSha256(this._handle),
          hashBytes: core.sha256,
          implementationSha256: identities.typescript_provider_sha256,
          runtime: `browser;typescript-${ts.version}`,
        })) {
          core.projectAddProvider(this._handle, "primary", bundle);
        }
      }
      core.projectScanBuiltinProvider(this._handle, "semantic:scip", supportMode);
      try {
        core.projectFinalizeProviders(this._handle);
      } catch (error) {
        if (error && typeof error === "object") {
          try {
            error.mergeConflictsJson = core.projectMergeConflicts(
              this._handle,
              false,
            );
          } catch {
            // Preserve the original finalization error.
          }
        }
        core.projectDestroy(this._handle);
        this._handle = null;
        throw error;
      }
    }

    static fromFiles(suppliedSources, options = {}) {
      const sourceList = [...suppliedSources].map((source) =>
        source instanceof Source ? source : new Source(source.path, source.data));
      const byPath = new Map(sourceList.map((source) => [source.path, source]));
      if (byPath.size !== sourceList.length) throw new Error("source paths must be unique");
      const custom = [...new Set((options.ignoreFiles || []).map(
        (value) => new Source(value, Buffer.alloc(0)).path,
      ))];
      for (const pathname of custom) {
        if (!byPath.has(pathname)) {
          throw new Error(`custom ignore file is unavailable: ${pathname}`);
        }
      }
      const customSet = new Set(custom);
      const standard = options.ignore === false
        ? []
        : [...byPath.keys()]
          .filter((pathname) => standardIgnore(pathname) && !customSet.has(pathname));
      const ignorePaths = [...standard, ...custom];
      const documents = ["package.json", "pyproject.toml", "DESCRIPTION", "configure.ac"]
        .filter((pathname) => byPath.has(pathname))
        .map((pathname) => ({
          content_hex: byPath.get(pathname).data.toString("hex"),
          path: pathname,
        }));
      const inventory = canonical({
        artifact: "archbird-repository-inventory",
        documents,
        files: [...byPath.values()]
          .map((source) => ({ bytes: source.data.length, path: source.path }))
          .sort((left, right) => utf8Compare(left.path, right.path)),
        ignore_files: ignorePaths.map((pathname) => ({
          content_hex: byPath.get(pathname).data.toString("hex"),
          path: pathname,
        })),
        schema_version: 1,
      });
      const config = options.config ? Buffer.from(options.config) : Buffer.alloc(0);
      const request = mapRequest({ ...options, ignoreFiles: custom });
      const resolutionJson = core.discoveryResolve(
        config,
        request,
        Buffer.from(JSON.stringify(inventory)),
      );
      const resolution = JSON.parse(resolutionJson.toString("utf8"));
      const effective = Buffer.from(JSON.stringify(canonical(resolution.effective_config)));
      return new Project(effective, sourceList, {
        resolution,
        resolutionJson,
        typescript: options.typescript ?? true,
      });
    }

    get counts() {
      return core.projectCounts(this._handle);
    }

    get mapInputSha256() {
      return core.projectMapInputSha256(this._handle);
    }

    addTestSymbolObservations(observationsJson) {
      core.projectAddTestSymbolObservations(
        this._handle,
        Buffer.from(observationsJson),
      );
    }

    mapJson({ pretty = false } = {}) {
      return core.projectMap(this._handle, pretty);
    }

    map() {
      return JSON.parse(this.mapJson().toString("utf8"));
    }

    mapMarkdown({
      view = "overview",
      detail = "standard",
      compact = false,
      full = false,
      maxChars = 0,
    } = {}) {
      const views = { overview: 0, architecture: 1, audit: 2 };
      const details = { compact: 0, standard: 1, full: 2 };
      if (!Object.hasOwn(views, view)) {
        throw new RangeError("view must be overview, architecture, or audit");
      }
      if (!Object.hasOwn(details, detail)) {
        throw new RangeError("detail must be compact, standard, or full");
      }
      if (compact && full) throw new RangeError("compact and full conflict");
      if ((compact || full) && detail !== "standard") {
        throw new RangeError("detail conflicts with compact/full alias");
      }
      const selected = compact ? "compact" : (full ? "full" : detail);
      return core.mapMarkdownView(
        this.mapJson(), views[view], details[selected], maxChars,
      );
    }

    mergeLedgerJson({ pretty = false } = {}) {
      return core.projectMergeLedger(this._handle, pretty);
    }

    mergeConflictsJson({ pretty = false } = {}) {
      return core.projectMergeConflicts(this._handle, pretty);
    }

    queryJson(options = {}) {
      return core.mapQuery(
        this.mapJson(),
        this.resolutionJson ?? Buffer.alloc(0),
        Buffer.from(JSON.stringify(queryRequest(options))),
        options.pretty ?? false,
      );
    }

    queryMarkdown(options = {}) {
      const projection = queryProjection(options);
      const verificationResult = options.verificationResult ?? Buffer.alloc(0);
      if (verificationResult.length && projection.view !== 1) {
        throw new RangeError("verificationResult requires the changes view");
      }
      return core.mapQueryMarkdownView(
        this.mapJson(),
        this.resolutionJson ?? Buffer.alloc(0),
        Buffer.from(JSON.stringify(queryRequest(options))),
        projection.view,
        projection.detail,
        options.maxChars ?? 0,
        Buffer.from(verificationResult),
      );
    }

    graphViewJson({
      view = "components",
      query = {},
      maxNodes = 200,
      maxEdgeNames = 3,
    } = {}) {
      const artifact = view === "symbols" ? this.queryJson(query) : this.mapJson();
      return core.mapExportGraph(
        artifact,
        "json",
        view,
        "LR",
        maxNodes,
        maxEdgeNames,
      );
    }

    dispose() {
      if (this._handle) {
        core.projectDestroy(this._handle);
        this._handle = null;
      }
    }
  }

  return Object.freeze({
    ENGINE: core.ENGINE,
    NATIVE_ABI_VERSION: core.NATIVE_ABI_VERSION,
    PATTERN_CONTRACT: core.PATTERN_CONTRACT,
    PATTERN_CONTRACT_VERSION: core.PATTERN_CONTRACT_VERSION,
    Project,
    Source,
    VERSION: core.VERSION,
    auditMapFreshness: (snapshot, current, { pretty = false } = {}) =>
      core.mapFreshness(
        Buffer.from(snapshot),
        Buffer.from(current),
        pretty,
      ),
    core,
  });
}

module.exports = { createBrowserArchbird };
