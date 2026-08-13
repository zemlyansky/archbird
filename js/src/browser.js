"use strict";

const { Buffer } = require("buffer");
const ts = require("typescript");
const { mapProjectionRequest } = require("./map-view");
const { typescriptProviderBundles } = require("./providers/typescript");
const { createArchbirdCore } = require("./wasm");

const DISCOVERY_MANIFEST_MAX_BYTES = 256 * 1024;
const DISCOVERY_MANIFEST_LIMITS = Object.freeze({
  cmake: 1,
  npm: 128,
  python: 32,
  "setup-cfg": 1,
});
const DISCOVERY_MANIFEST_SOURCES = Object.freeze({
  cmake: new Set(["cmake-root-project"]),
  npm: new Set(["npm-workspace"]),
  python: new Set(["python-top-level", "python-workspace"]),
  "setup-cfg": new Set(["python-root-setup-cfg"]),
});

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

function projectConfigurationForResolution(configJson, effectiveConfig) {
  const authored = configJson && configJson.length
    ? JSON.parse(Buffer.from(configJson).toString("utf8"))
    : {};
  const result = { ...effectiveConfig };
  for (const field of ["projections", "queries", "constraints"]) {
    if (Object.hasOwn(authored, field)) result[field] = authored[field];
  }
  return Buffer.from(JSON.stringify(canonical(result)));
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

function pathRequest(options = {}) {
  const request = {
    artifact: "path-request",
    direction: options.direction || "downstream",
    level: options.level || "file",
    max_depth: options.maxDepth ?? 8,
    max_paths: options.maxPaths ?? 8,
    producer_policy: options.producerPolicy || "compatible",
    schema_version: 1,
    source: options.source,
    target: options.target,
  };
  if (options.relations !== undefined && options.relations !== null) {
    request.relations = [...options.relations];
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

function sourceRenderOptions(options = {}) {
  const details = { compact: 0, standard: 1, full: 2 };
  const detail = options.detail ?? "standard";
  const compact = options.compact ?? false;
  const full = options.full ?? false;
  const maxChars = options.maxChars ?? 0;
  if (!Object.hasOwn(details, detail)) {
    throw new RangeError("detail must be compact, standard, or full");
  }
  if (compact && full) throw new RangeError("compact and full conflict");
  if ((compact || full) && detail !== "standard") {
    throw new RangeError("detail conflicts with compact/full alias");
  }
  if (!Number.isSafeInteger(maxChars) || maxChars < 0) {
    throw new RangeError("maxChars must be a nonnegative safe integer");
  }
  const selectedDetail = compact ? "compact" : (full ? "full" : detail);
  if (selectedDetail === "full" && maxChars) {
    throw new RangeError("full source detail cannot be combined with maxChars");
  }
  return { detail: details[selectedDetail], maxChars };
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
      {
        typescript = true,
        resolution = null,
        resolutionJson = null,
        projectConfigurationJson = null,
      } = {},
    ) {
      this.configJson = Buffer.from(configJson);
      this.projectConfigurationJson = Buffer.from(projectConfigurationJson ?? configJson);
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

    static discoveryContentPaths(paths, options = {}) {
      const normalized = [...new Set(paths.map(
        (pathname) => new Source(pathname, Buffer.alloc(0)).path,
      ))];
      const custom = [...new Set((options.ignoreFiles || []).map(
        (value) => new Source(value, Buffer.alloc(0)).path,
      ))];
      for (const pathname of custom) {
        if (!normalized.includes(pathname)) {
          throw new Error(`custom ignore file is unavailable: ${pathname}`);
        }
      }
      const customSet = new Set(custom);
      const standard = options.ignore === false
        ? []
        : normalized
          .filter((pathname) => standardIgnore(pathname) && !customSet.has(pathname));
      const documents = [
        "package.json",
        "pyproject.toml",
        "DESCRIPTION",
        "configure.ac",
        "setup.cfg",
        "CMakeLists.txt",
      ]
        .filter((pathname) => normalized.includes(pathname));
      return [...new Set([...standard, ...custom, ...documents])].sort(utf8Compare);
    }

    static resolveInventory(suppliedFiles, options = {}) {
      const rows = [...suppliedFiles].map((value) => {
        const path = new Source(value.path, Buffer.alloc(0)).path;
        if (!Number.isSafeInteger(value.bytes) || value.bytes < 0) {
          throw new Error(`inventory byte size is invalid: ${path}`);
        }
        return {
          bytes: value.bytes,
          data: value.data === undefined ? null : Buffer.from(value.data),
          path,
        };
      });
      const byPath = new Map(rows.map((row) => [row.path, row]));
      if (byPath.size !== rows.length) throw new Error("inventory paths must be unique");
      const contentPaths = Project.discoveryContentPaths([...byPath.keys()], options);
      const customIgnorePaths = new Set((options.ignoreFiles || []).map(
        (pathname) => new Source(pathname, Buffer.alloc(0)).path,
      ));
      for (const pathname of contentPaths) {
        const row = byPath.get(pathname);
        if (!row?.data || row.data.length !== row.bytes) {
          throw new Error(`discovery content is unavailable: ${pathname}`);
        }
      }
      const ignorePaths = contentPaths.filter((pathname) =>
        standardIgnore(pathname) || customIgnorePaths.has(pathname));
      const directDocuments = new Set([
        "package.json",
        "pyproject.toml",
        "DESCRIPTION",
        "configure.ac",
      ]);
      const documents = contentPaths
        .filter((pathname) => directDocuments.has(pathname))
        .map((pathname) => ({
          content_hex: byPath.get(pathname).data.toString("hex"),
          path: pathname,
        }));
      const inventory = canonical({
        artifact: "archbird-repository-inventory",
        documents,
        files: rows
          .map((row) => ({ bytes: row.bytes, path: row.path }))
          .sort((left, right) => utf8Compare(left.path, right.path)),
        ignore_files: ignorePaths.map((pathname) => ({
          content_hex: byPath.get(pathname).data.toString("hex"),
          path: pathname,
        })),
        schema_version: 1,
      });
      const config = options.config ? Buffer.from(options.config) : Buffer.alloc(0);
      const request = mapRequest({
        ...options,
        ignoreFiles: [...customIgnorePaths],
      });
      let resolutionJson = core.discoveryResolve(
        config,
        request,
        Buffer.from(JSON.stringify(inventory)),
      );
      let resolution = JSON.parse(resolutionJson.toString("utf8"));
      const requestedPaths = new Set();
      const suppliedRequestPaths = [];
      const requestCounts = Object.fromEntries(
        Object.keys(DISCOVERY_MANIFEST_LIMITS).map((kind) => [kind, 0]),
      );
      for (const requestRow of resolution.manifest_requests || []) {
        const { kind, path, source } = requestRow;
        if (typeof path !== "string" || requestRow.fulfilled !== false ||
            requestRow.max_bytes !== DISCOVERY_MANIFEST_MAX_BYTES ||
            requestedPaths.has(path) || !Object.hasOwn(DISCOVERY_MANIFEST_LIMITS, kind) ||
            !DISCOVERY_MANIFEST_SOURCES[kind].has(source)) {
          throw new Error("native manifest request is invalid");
        }
        requestCounts[kind] += 1;
        if (requestCounts[kind] > DISCOVERY_MANIFEST_LIMITS[kind]) {
          throw new Error("native manifest request limit was exceeded");
        }
        requestedPaths.add(path);
        const row = byPath.get(path);
        if (!row) throw new Error("native manifest request is invalid");
        if (!row?.data) {
          if (["cmake-root-project", "python-root-setup-cfg"].includes(
            source,
          )) {
            throw new Error(`discovery content is unavailable: ${path}`);
          }
          continue;
        }
        if (row.data.length !== row.bytes || row.data.length > requestRow.max_bytes) {
          throw new Error(`discovery content is invalid: ${path}`);
        }
        documents.push({
          content_hex: row.data.toString("hex"),
          path,
        });
        suppliedRequestPaths.push(path);
      }
      if (documents.length !== inventory.documents.length) {
        inventory.documents = documents;
        resolutionJson = core.discoveryResolve(
          config,
          request,
          Buffer.from(JSON.stringify(inventory)),
        );
        resolution = JSON.parse(resolutionJson.toString("utf8"));
        const fulfilled = new Set(
          (resolution.manifest_requests || [])
            .filter((row) => row.fulfilled === true)
            .map((row) => row.path),
        );
        for (const path of suppliedRequestPaths) {
          if (!fulfilled.has(path)) {
            throw new Error(`native manifest request was not fulfilled: ${path}`);
          }
        }
      }
      return {
        projectConfigurationJson: projectConfigurationForResolution(
          config,
          resolution.effective_config,
        ),
        resolution,
        resolutionJson,
      };
    }

    static fromResolvedFiles(suppliedSources, resolved, options = {}) {
      const sourceList = [...suppliedSources].map((source) =>
        source instanceof Source ? source : new Source(source.path, source.data));
      const resolution = resolved?.resolution;
      const resolutionJson = resolved?.resolutionJson;
      if (!resolution || !resolutionJson) throw new Error("resolved discovery is required");
      const effective = Buffer.from(JSON.stringify(canonical(resolution.effective_config)));
      return new Project(effective, sourceList, {
        projectConfigurationJson: resolved.projectConfigurationJson,
        resolution,
        resolutionJson,
        typescript: options.typescript ?? true,
      });
    }

    static fromFiles(suppliedSources, options = {}) {
      const sourceList = [...suppliedSources].map((source) =>
        source instanceof Source ? source : new Source(source.path, source.data));
      const resolved = Project.resolveInventory(
        sourceList.map((source) => ({
          bytes: source.data.length,
          data: source.data,
          path: source.path,
        })),
        options,
      );
      return Project.fromResolvedFiles(sourceList, resolved, options);
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

    mapMarkdown(options = {}) {
      const request = mapProjectionRequest(options);
      return core.projectionRenderMarkdown(
        this.mapJson(),
        options.resolutionJson ?? this.resolutionJson ?? Buffer.alloc(0),
        Buffer.from(JSON.stringify(request.definition)),
        request.detail,
        request.maxChars,
      );
    }

    sourceMarkdown(options = {}) {
      const renderOptions = sourceRenderOptions(options);
      return core.projectSourceMarkdown(
        this._handle,
        options.artifactJson ?? this.mapJson(),
        renderOptions.detail,
        renderOptions.maxChars,
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

    pathJson(options = {}) {
      return core.mapPath(
        this.mapJson(),
        this.resolutionJson ?? Buffer.alloc(0),
        Buffer.from(JSON.stringify(pathRequest(options))),
        options.pretty ?? false,
      );
    }

    path(options = {}) {
      return JSON.parse(this.pathJson(options).toString("utf8"));
    }

    pathMarkdown(options = {}) {
      return core.pathRenderMarkdown(
        this.pathJson({ ...options, pretty: false }),
        options.maxChars ?? 0,
      );
    }

    verifyJson({ constraintIds = [], policyDate = null, pretty = false } = {}) {
      const request = {};
      if (constraintIds.length) request.ids = [...constraintIds];
      if (policyDate !== null) request.policy_date = policyDate;
      return core.constraintsEvaluate(
        this.projectConfigurationJson,
        this.mapJson(),
        this.resolutionJson ?? Buffer.alloc(0),
        Object.keys(request).length
          ? Buffer.from(JSON.stringify(canonical(request)))
          : Buffer.alloc(0),
        pretty,
      );
    }

    get verificationConfigured() {
      const constraints = JSON.parse(this.projectConfigurationJson.toString("utf8")).constraints;
      return constraints && typeof constraints === "object"
        && Object.keys(constraints).length > 0;
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
