"use strict";

const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const native = require("./native");
const {
  applyAcceptedAct,
  observeActSources,
  observePlanSources,
  runActGates,
  actOverlay,
  renderAct,
} = require("./act-transport");
const { okfNormalization } = require("./adapters/okf/normalization");
const { compileTestObservations: compileCoverageObservations } = require("./adapters/coverage");
const {
  ProviderCache,
  defaultProviderCacheDir,
  defaultProviderCacheMaxBytes,
  emptyMapCacheStats,
  emptyProviderCacheStats,
} = require("./provider-cache");
const { mapProjectionRequest } = require("./map-view");
const { typescriptProviderBundles } = require("./providers/typescript");

const NATIVE_LEXICAL_PROVIDERS = Object.freeze([
  "lexical:c",
  "lexical:javascript",
  "lexical:python",
  "lexical:r",
]);
const NATIVE_SYNTAX_PROVIDERS = Object.freeze([
  "syntax:tree-sitter:c",
  "syntax:tree-sitter:cpp",
  "syntax:tree-sitter:python",
  "syntax:tree-sitter:javascript",
  "syntax:tree-sitter:typescript",
  "syntax:tree-sitter:tsx",
  "syntax:tree-sitter:r",
]);
const NATIVE_SEMANTIC_PROVIDERS = Object.freeze(["semantic:scip"]);
const HOST_PROVIDERS = Object.freeze(["compiler:typescript"]);

const NATIVE_CACHE_PROVIDERS = [
  ["lexical:c",
    (source) => ["c", "cpp"].includes(source.language), "support"],
  ["lexical:javascript",
    (source) => ["javascript", "typescript", "vue"].includes(source.language), "support"],
  ["lexical:python",
    (source) => source.language === "python", "support"],
  ["lexical:r",
    (source) => source.language === "r", "support"],
  ["syntax:tree-sitter:c",
    (source) => source.language === "c", "primary"],
  ["syntax:tree-sitter:cpp",
    (source) => source.language === "cpp", "primary"],
  ["syntax:tree-sitter:python",
    (source) => source.language === "python", "primary"],
  ["syntax:tree-sitter:javascript",
    (source) => source.language === "javascript", "primary"],
  ["syntax:tree-sitter:typescript",
    (source) => source.language === "typescript" && !source.path.endsWith(".tsx"), "primary"],
  ["syntax:tree-sitter:tsx",
    (source) => source.language === "typescript" && source.path.endsWith(".tsx"), "primary"],
  ["syntax:tree-sitter:r",
    (source) => source.language === "r", "primary"],
];

const PROVIDER_SUPPORT = Object.freeze({
  host: HOST_PROVIDERS,
  portable: Object.freeze([
    ...NATIVE_LEXICAL_PROVIDERS,
    ...NATIVE_SYNTAX_PROVIDERS,
    ...NATIVE_SEMANTIC_PROVIDERS,
  ]),
  precision: Object.freeze({
    c: "tree-sitter+lexical",
    cpp: "tree-sitter+lexical",
    javascript: "typescript-compiler+tree-sitter+lexical",
    python: "tree-sitter+lexical",
    r: "tree-sitter+lexical",
    tsx: "typescript-compiler+tree-sitter+lexical",
    typescript: "typescript-compiler+tree-sitter+lexical",
    vue: "lexical",
  }),
});

function sha256(bytes) {
  return crypto.createHash("sha256").update(bytes).digest("hex");
}

function compileTestObservations(mapJson, requestJson, options) {
  const result = compileCoverageObservations(mapJson, requestJson, {
    ...options,
    canonicalize: native.jsonCanonicalize,
    implementationSha256: sha256(Buffer.from([
      "archbird-node-coverage-adapter-v1",
      native.IMPLEMENTATION_SHA256,
      native.VERSION,
      sha256(fs.readFileSync(require.resolve("./adapters/coverage"))),
    ].join("\0"))),
    version: native.VERSION,
  });
  native.testSymbolObservationsValidate(result);
  return result;
}

function nativeCacheNamespace() {
  return sha256(
    Buffer.from(
      `archbird-native-provider-cache-v1\0${native.IMPLEMENTATION_SHA256}`,
      "ascii",
    ),
  );
}

function typescriptProviderIdentity() {
  const version = require("typescript").version;
  return {
    implementation: sha256(
      Buffer.concat([
        fs.readFileSync(require.resolve("./providers/typescript")),
        Buffer.from(`\0typescript:${version}`),
      ]),
    ),
    runtime: `node-${process.versions.node};typescript-${version}`,
  };
}

function mapCacheNamespace(typescript, mode) {
  const host = typescript
    ? typescriptProviderIdentity()
    : { implementation: "disabled", runtime: "disabled" };
  return sha256(Buffer.from([
    "archbird-node-map-result-cache-v1",
    native.IMPLEMENTATION_SHA256,
    nativeCacheNamespace(),
    host.implementation,
    host.runtime,
    `mode=${mode}`,
  ].join("\0"), "ascii"));
}

function utf8Compare(left, right) {
  return Buffer.compare(Buffer.from(left), Buffer.from(right));
}

function canonicalForDigest(value) {
  if (Array.isArray(value)) return value.map(canonicalForDigest);
  if (value && typeof value === "object" && !Buffer.isBuffer(value)) {
    return Object.fromEntries(
      Object.keys(value)
        .sort(utf8Compare)
        .map((key) => [key, canonicalForDigest(value[key])]),
    );
  }
  return value;
}

function projectConfigurationForResolution(configJson, effectiveConfig) {
  let authored = {};
  if (configJson && configJson.length) {
    authored = JSON.parse(Buffer.from(configJson).toString("utf8"));
  }
  const result = { ...effectiveConfig };
  for (const field of ["projections", "queries", "constraints"]) {
    if (Object.hasOwn(authored, field)) result[field] = authored[field];
  }
  return Buffer.from(JSON.stringify(canonicalForDigest(result)));
}

class Source {
  constructor(path, data, { language = "", layer = "", roles = ["source"] } = {}) {
    if (!Buffer.isBuffer(data)) data = Buffer.from(data);
    this.path = path;
    this.data = data;
    this.language = language;
    this.layer = layer;
    this.roles = [...new Set(roles)].sort(utf8Compare);
  }
}

class Project {
  constructor(
    project,
    sources,
    { configurationSha256 = null, resolution = null } = {},
  ) {
    this.project = project;
    this.root = null;
    this.resolutionJson = null;
    this.sources = [...sources].sort((left, right) => utf8Compare(left.path, right.path));
    if (new Set(this.sources.map((source) => source.path)).size !== this.sources.length) {
      throw new Error("source paths must be unique");
    }
    const classifications = this.sources.map((source) => ({
      language: source.language,
      layer: source.layer,
      path: source.path,
      roles: source.roles,
    }));
    const files = this.sources.map((source) => {
      const row = {
        bytes: source.data.length,
        path: source.path,
        roles: source.roles,
        sha256: sha256(source.data),
      };
      if (source.language) row.language = source.language;
      if (source.layer) row.layer = source.layer;
      return row;
    });
    const manifest = {
      artifact: "archbird-source-manifest",
      configuration_sha256:
        configurationSha256 ||
        sha256(Buffer.from(JSON.stringify(canonicalForDigest(classifications)))),
      files,
      producer: {
        implementation_sha256: sha256(Buffer.from(fsSourceIdentity())),
        name: "archbird-node-host",
        version: "1",
      },
      project,
      schema_version: 1,
    };
    if (resolution) {
      manifest.resolution = {
        coverage: { ...resolution.coverage },
        profile: { ...resolution.profile },
        sha256: resolution.sha256,
      };
    }
    this.manifestJson = Buffer.from(JSON.stringify(manifest));
    this._handle = native.projectCreate(this.manifestJson);
    for (const source of this.sources) {
      native.projectAddSource(this._handle, source.path, source.data);
    }
    native.projectFinalizeSources(this._handle);
    this._providersFinalized = false;
    this.cacheStats = emptyProviderCacheStats();
    this.mapCacheStats = emptyMapCacheStats();
    this._configJson = null;
    this._projectConfigurationJson = null;
    this._authoredConfigJson = null;
    this._discoveryOptions = null;
    this._cachedMap = null;
    this._mapCache = null;
    this._mapCacheParameters = null;
    this._deferredScan = null;
  }

  static fromConfig(
    configPath,
    {
      root = null, scan = true, typescript = true, cacheDir = null,
      cacheMaxBytes = null, mapCache = true,
    } = {},
  ) {
    const resolvedConfig = path.resolve(configPath);
    const configJson = fs.readFileSync(resolvedConfig);
    const metadata = JSON.parse(
      native.discoveryPlan(configJson, []).toString("utf8"),
    );
    const repository = root
      ? path.resolve(root)
      : path.resolve(path.dirname(resolvedConfig), metadata.root);
    if (!fs.statSync(repository).isDirectory()) {
      throw new Error(`root is not a directory: ${repository}`);
    }
    const resolutionJson = resolveDiscovery(repository, {
      config: configJson,
      ignore: false,
    });
    const resolution = JSON.parse(resolutionJson.toString("utf8"));
    const project = new Project(
      resolution.project,
      readSources(repository, resolution),
      {
        configurationSha256: resolution.configuration_sha256,
        resolution,
      },
    );
    project.resolutionJson = resolutionJson;
    project.root = repository;
    const effectiveConfig = Buffer.from(
      JSON.stringify(canonicalForDigest(resolution.effective_config)),
    );
    project.setConfig(
      effectiveConfig,
      projectConfigurationForResolution(configJson, resolution.effective_config),
    );
    project._authoredConfigJson = Buffer.from(configJson);
    project._discoveryOptions = {
      project: null,
      source: [],
      only: [],
      exclude: [],
      ignore: false,
      ignoreFiles: [],
      defaultExcludes: true,
      maxFileBytes: null,
      maxIndexBytes: null,
      _transientExclude: [],
    };
    if (scan) project.scan("primary", {
      typescript, cacheDir, cacheMaxBytes, mapCache,
    });
    return project;
  }

  static fromRepository(
    root = ".",
    {
      config = null,
      project: projectName = null,
      source = [],
      only = [],
      exclude = [],
      ignore = true,
      ignoreFiles = [],
      defaultExcludes = true,
      maxFileBytes = null,
      maxIndexBytes = null,
      scan = true,
      typescript = true,
      cacheDir = null,
      cacheMaxBytes = null,
      mapCache = true,
      _transientExclude = [],
    } = {},
  ) {
    const repository = path.resolve(root);
    if (!fs.statSync(repository).isDirectory()) {
      throw new Error(`root is not a directory: ${repository}`);
    }
    const configJson = configBytes(config);
    const resolutionJson = resolveDiscovery(repository, {
      config: configJson,
      project: projectName,
      source,
      only,
      exclude,
      ignore,
      ignoreFiles,
      defaultExcludes,
      maxFileBytes,
      maxIndexBytes,
      _transientExclude,
    });
    const resolution = JSON.parse(resolutionJson.toString("utf8"));
    const effectiveConfig = Buffer.from(
      JSON.stringify(canonicalForDigest(resolution.effective_config)),
    );
    const current = new Project(
      resolution.project,
      readSources(repository, resolution),
      {
        configurationSha256: resolution.configuration_sha256,
        resolution,
      },
    );
    current.root = repository;
    current.resolutionJson = resolutionJson;
    current.setConfig(
      effectiveConfig,
      projectConfigurationForResolution(configJson, resolution.effective_config),
    );
    current._authoredConfigJson = Buffer.from(configJson);
    current._discoveryOptions = {
      project: projectName,
      source: [...source],
      only: [...only],
      exclude: [...exclude],
      ignore,
      ignoreFiles: [...ignoreFiles],
      defaultExcludes,
      maxFileBytes,
      maxIndexBytes,
      _transientExclude: [..._transientExclude],
    };
    if (scan) current.scan("primary", {
      typescript, cacheDir, cacheMaxBytes, mapCache,
    });
    return current;
  }

  withSourceOverlay(
    overlay,
    {
      config = null,
      scan = true,
      typescript = true,
      cacheDir = null,
      cacheMaxBytes = null,
      mapCache = true,
    } = {},
  ) {
    if (!this.root || !this.resolutionJson || !this._discoveryOptions) {
      throw new Error("source overlays require a repository-backed Project");
    }
    const normalized = normalizeSourceOverlay(overlay);
    const configJson = config === null
      ? Buffer.from(this._authoredConfigJson || Buffer.alloc(0))
      : configBytes(config);
    const resolutionJson = resolveDiscovery(this.root, {
      config: configJson,
      ...this._discoveryOptions,
      _sourceOverlay: normalized,
    });
    const resolution = JSON.parse(resolutionJson.toString("utf8"));
    const effectiveConfig = Buffer.from(
      JSON.stringify(canonicalForDigest(resolution.effective_config)),
    );
    const current = new Project(
      resolution.project,
      readSources(this.root, resolution, normalized),
      {
        configurationSha256: resolution.configuration_sha256,
        resolution,
      },
    );
    current.root = this.root;
    current.resolutionJson = resolutionJson;
    current.setConfig(
      effectiveConfig,
      projectConfigurationForResolution(configJson, resolution.effective_config),
    );
    current._authoredConfigJson = Buffer.from(configJson);
    current._discoveryOptions = {
      ...this._discoveryOptions,
      source: [...this._discoveryOptions.source],
      only: [...this._discoveryOptions.only],
      exclude: [...this._discoveryOptions.exclude],
      ignoreFiles: [...this._discoveryOptions.ignoreFiles],
      _transientExclude: [...this._discoveryOptions._transientExclude],
    };
    if (scan) current.scan("primary", {
      typescript, cacheDir, cacheMaxBytes, mapCache,
    });
    return current;
  }

  get manifestSha256() {
    return native.projectManifestSha256(this._handle);
  }

  get mapInputSha256() {
    return native.projectMapInputSha256(this._handle);
  }

  get counts() {
    this.materialize();
    return native.projectCounts(this._handle);
  }

  get configSha256() {
    return native.projectConfigSha256(this._handle);
  }

  setConfig(configJson, projectConfigurationJson = configJson) {
    if (this._providersFinalized) throw new Error("providers are already finalized");
    this._configJson = Buffer.from(configJson);
    this._projectConfigurationJson = Buffer.from(projectConfigurationJson);
    native.projectSetConfig(this._handle, this._configJson);
  }

  get verificationConfigured() {
    if (this._projectConfigurationJson === null) return false;
    const document = JSON.parse(this._projectConfigurationJson.toString("utf8"));
    return document.constraints && typeof document.constraints === "object"
      && Object.keys(document.constraints).length > 0;
  }

  addProvider(providerJson, mode = "primary") {
    if (this._providersFinalized) throw new Error("providers are already finalized");
    native.projectAddProvider(this._handle, mode, Buffer.from(providerJson));
  }

  addTestSymbolObservations(observationsJson) {
    this.materialize();
    this._mapCache = null;
    this._mapCacheParameters = null;
    this._cachedMap = null;
    native.projectAddTestSymbolObservations(
      this._handle,
      Buffer.from(observationsJson),
    );
  }

  scanBuiltinProviderFile(providerId, path, mode = "primary") {
    if (this._providersFinalized) throw new Error("providers are already finalized");
    native.projectScanBuiltinProviderFile(this._handle, providerId, path, mode);
  }

  cachedBuiltinProvider(
    cache,
    { namespace, providerId, source, mode },
  ) {
    const sourceSha256 = sha256(source.data);
    const parameters = {
      namespace,
      project: this.project,
      providerId,
      path: source.path,
      sourceSha256,
    };
    const bundle = cache.load(parameters);
    if (bundle !== null) {
      try {
        this.addProvider(bundle, mode);
        return;
      } catch (_) {
        cache.reject(parameters);
      }
    }
    const providerIndex = Number(this.counts.providers);
    this.scanBuiltinProviderFile(providerId, source.path, mode);
    cache.store(this.providerFactsJson(providerIndex), parameters);
  }

  mapCacheParameters(typescript, mode) {
    if (this._configJson === null) return null;
    return {
      namespace: mapCacheNamespace(typescript, mode),
      project: this.project,
      manifestSha256: this.manifestSha256,
      configSha256: this.configSha256,
    };
  }

  cachedMapIsCurrent(data) {
    let document;
    try {
      document = JSON.parse(data.toString("utf8"));
    } catch (_) {
      return false;
    }
    return document && document.artifact === "map" &&
      document.project === this.project &&
      Number.isInteger(document.schema_version) &&
      document.tool &&
      document.tool.implementation_sha256 === native.IMPLEMENTATION_SHA256 &&
      document.evidence &&
      document.evidence.config_sha256 === this.configSha256 &&
      document.evidence.input_sha256 === this.mapInputSha256;
  }

  resetHandle() {
    native.projectDestroy(this._handle);
    this._handle = native.projectCreate(this.manifestJson);
    for (const source of this.sources) {
      native.projectAddSource(this._handle, source.path, source.data);
    }
    native.projectFinalizeSources(this._handle);
    if (this._configJson !== null) {
      native.projectSetConfig(this._handle, this._configJson);
    }
    this._providersFinalized = false;
  }

  materialize() {
    if (this._cachedMap === null) return;
    const deferred = { ...(this._deferredScan || {}) };
    this._cachedMap = null;
    this._mapCache = null;
    this._mapCacheParameters = null;
    this._deferredScan = null;
    this.resetHandle();
    this.scan(deferred.mode || "primary", { ...deferred, mapCache: false });
  }

  scan(
    mode = "primary",
    {
      typescript = true, cacheDir = null, cacheMaxBytes = null, progress = null,
      mapCache = true,
    } = {},
  ) {
    if (this._providersFinalized) throw new Error("providers are already finalized");
    if (progress !== null && typeof progress !== "function") {
      throw new TypeError("progress must be a function or null");
    }
    const report = (event) => {
      if (progress !== null) progress(event);
    };
    const supportMode = mode === "primary" ? "augment" : mode;
    const cache = cacheDir === null ? null : new ProviderCache(
      cacheDir,
      { maxBytes: cacheMaxBytes === null ? defaultProviderCacheMaxBytes() : cacheMaxBytes },
    );
    const mapParameters = cache !== null && mapCache
      ? this.mapCacheParameters(typescript, mode)
      : null;
    if (cache !== null && mapParameters !== null) {
      const cachedMap = cache.loadMap(mapParameters);
      if (cachedMap !== null) {
        if (this.cachedMapIsCurrent(cachedMap)) {
          this._cachedMap = cachedMap;
          this._mapCache = cache;
          this._mapCacheParameters = mapParameters;
          this._deferredScan = {
            mode, typescript, cacheDir, cacheMaxBytes,
          };
          this._providersFinalized = true;
          this.cacheStats = { ...cache.stats };
          this.mapCacheStats = { ...cache.mapStats };
          report({ phase: "cache", artifact: "map", state: "hit" });
          return;
        }
        cache.rejectMap(mapParameters);
      }
    }
    if (cache === null) {
      for (const providerId of NATIVE_LEXICAL_PROVIDERS) {
        report({ phase: "providers", provider: providerId, state: "start" });
        native.projectScanBuiltinProvider(this._handle, providerId, supportMode);
        report({ phase: "providers", provider: providerId, state: "complete" });
      }
      for (const providerId of NATIVE_SYNTAX_PROVIDERS.filter(
        (value) => ![
          "syntax:tree-sitter:javascript",
          "syntax:tree-sitter:typescript",
          "syntax:tree-sitter:tsx",
        ].includes(value),
      )) {
        report({ phase: "providers", provider: providerId, state: "start" });
        native.projectScanBuiltinProvider(this._handle, providerId, mode);
        report({ phase: "providers", provider: providerId, state: "complete" });
      }
      for (const providerId of [
        "syntax:tree-sitter:javascript",
        "syntax:tree-sitter:typescript",
        "syntax:tree-sitter:tsx",
      ]) {
        report({ phase: "providers", provider: providerId, state: "start" });
        native.projectScanBuiltinProvider(
          this._handle,
          providerId,
          typescript ? supportMode : mode,
        );
        report({ phase: "providers", provider: providerId, state: "complete" });
      }
    } else {
      const namespace = nativeCacheNamespace();
      for (const [providerId, matches, providerMode] of
        NATIVE_CACHE_PROVIDERS) {
        const selectedMode = providerMode === "support" || (
          typescript &&
          ["syntax:tree-sitter:javascript", "syntax:tree-sitter:typescript",
            "syntax:tree-sitter:tsx"].includes(providerId)
        ) ? supportMode : mode;
        const matched = this.sources.filter(matches);
        report({
          phase: "providers", provider: providerId, state: "start",
          total: matched.length,
        });
        for (let index = 0; index < matched.length; index += 1) {
          this.cachedBuiltinProvider(cache, {
            namespace,
            providerId,
            source: matched[index],
            mode: selectedMode,
          });
          report({
            phase: "providers", provider: providerId, state: "progress",
            completed: index + 1, total: matched.length,
          });
        }
        report({
          phase: "providers", provider: providerId, state: "complete",
          completed: matched.length, total: matched.length,
        });
      }
    }
    if (
      typescript &&
      this.sources.some((source) => ["javascript", "typescript"].includes(source.language))
    ) {
      const hostSources = this.sources.filter(
        (source) => ["javascript", "typescript"].includes(source.language),
      );
      report({
        phase: "providers", provider: "compiler:typescript", state: "start",
        total: hostSources.length,
      });
      let completed = 0;
      const typescriptIdentity = typescriptProviderIdentity();
      for (const bundle of typescriptProviderBundles({
        project: this.project,
        sourceManifestSha256: this.manifestSha256,
        sources: this.sources,
        hashBytes: sha256,
        implementationSha256: typescriptIdentity.implementation,
        runtime: typescriptIdentity.runtime,
      })) {
        this.addProvider(bundle, mode);
        completed += 1;
        report({
          phase: "providers", provider: "compiler:typescript",
          state: "progress", completed, total: hostSources.length,
        });
      }
      report({
        phase: "providers", provider: "compiler:typescript", state: "complete",
        completed, total: hostSources.length,
      });
    }
    for (const providerId of NATIVE_SEMANTIC_PROVIDERS) {
      report({ phase: "providers", provider: providerId, state: "start" });
      native.projectScanBuiltinProvider(this._handle, providerId, supportMode);
      report({ phase: "providers", provider: providerId, state: "complete" });
    }
    report({ phase: "joining", state: "start" });
    try {
      this.finalizeProviders();
    } catch (error) {
      if (this._providersFinalized && error && typeof error === "object") {
        try {
          error.mergeConflictsJson = this.mergeConflictsJson();
        } catch {
          // Preserve the original finalization error.
        }
      }
      throw error;
    }
    report({ phase: "joining", state: "complete" });
    this.cacheStats = cache === null
      ? emptyProviderCacheStats()
      : { ...cache.stats };
    this.mapCacheStats = cache === null
      ? emptyMapCacheStats()
      : { ...cache.mapStats };
    this._mapCache = mapParameters === null ? null : cache;
    this._mapCacheParameters = mapParameters;
  }

  finalizeProviders() {
    if (!this._providersFinalized) {
      try {
        native.projectFinalizeProviders(this._handle);
      } catch (error) {
        try {
          native.projectMergeSummary(this._handle);
          this._providersFinalized = true;
          if (error && typeof error === "object") {
            error.mergeConflictsJson = this.mergeConflictsJson();
          }
        } catch {
          // Finalization failed before a merge result existed.
        }
        throw error;
      }
      this._providersFinalized = true;
    }
  }

  fileFactsJson({ pretty = false } = {}) {
    this.materialize();
    return native.projectFileFacts(this._handle, pretty);
  }

  fileFacts() {
    return JSON.parse(this.fileFactsJson().toString("utf8"));
  }

  mergeLedgerJson({ pretty = false } = {}) {
    this.materialize();
    return native.projectMergeLedger(this._handle, pretty);
  }

  mergeConflictsJson({ pretty = false } = {}) {
    this.materialize();
    return native.projectMergeConflicts(this._handle, pretty);
  }

  mergeSummary() {
    this.materialize();
    return native.projectMergeSummary(this._handle);
  }

  mapJson({ pretty = false } = {}) {
    if (this._cachedMap !== null) {
      return pretty
        ? native.jsonCanonicalize(this._cachedMap, true, false, true)
        : this._cachedMap;
    }
    const data = native.projectMap(this._handle, false);
    if (this._mapCache !== null && this._mapCacheParameters !== null) {
      this._mapCache.storeMap(data, this._mapCacheParameters);
      this.mapCacheStats = { ...this._mapCache.mapStats };
      this.cacheStats = { ...this._mapCache.stats };
      this._cachedMap = data;
    }
    return pretty ? native.jsonCanonicalize(data, true, false, true) : data;
  }

  map() {
    return JSON.parse(this.mapJson().toString("utf8"));
  }

  mapMarkdown(options = {}) {
    return renderMapMarkdown(this.mapJson(), {
      ...options,
      resolutionJson: options.resolutionJson ?? this.resolutionJson ?? Buffer.alloc(0),
    });
  }

  sourceMarkdown(options = {}) {
    return renderSourceMarkdown(
      this,
      options.artifactJson ?? this.mapJson(),
      options,
    );
  }

  queryJson(options = {}) {
    return queryMap(this.mapJson(), {
      ...options,
      resolutionJson: options.resolutionJson ?? this.resolutionJson ?? Buffer.alloc(0),
    });
  }

  query(options = {}) {
    return JSON.parse(this.queryJson(options).toString("utf8"));
  }

  queryMarkdown(options = {}) {
    return queryMapMarkdown(this.mapJson(), {
      ...options,
      resolutionJson: options.resolutionJson ?? this.resolutionJson ?? Buffer.alloc(0),
    });
  }

  pathJson(options = {}) {
    return pathMap(this.mapJson(), {
      ...options,
      resolutionJson: options.resolutionJson ?? this.resolutionJson ?? Buffer.alloc(0),
    });
  }

  path(options = {}) {
    return JSON.parse(this.pathJson(options).toString("utf8"));
  }

  pathMarkdown(options = {}) {
    return renderPathMarkdown(this.pathJson(options), options);
  }

  verifyJson({
    constraintIds = [],
    baseline = null,
    maps = null,
    observations = null,
    policyDate = null,
    pretty = false,
  } = {}) {
    if (this._projectConfigurationJson === null) {
      throw new Error("verification requires project configuration");
    }
    const request = {};
    if (constraintIds.length) request.ids = [...constraintIds];
    if (baseline !== null) request.baseline = baseline;
    if (maps !== null) request.maps = maps;
    if (observations !== null) request.observations = observations;
    if (policyDate !== null) request.policy_date = policyDate;
    return evaluateConstraints(this._projectConfigurationJson, this.mapJson(), {
      pretty,
      requestJson: Object.keys(request).length
        ? Buffer.from(JSON.stringify(canonicalForDigest(request)))
        : Buffer.alloc(0),
      resolutionJson: this.resolutionJson ?? Buffer.alloc(0),
    });
  }

  graphViewJson({
    view = "components",
    query = {},
    maxNodes = 200,
    maxEdgeNames = 3,
  } = {}) {
    const artifact = view === "symbols" ? this.queryJson(query) : this.mapJson();
    return exportGraph(artifact, {
      format: "json",
      view,
      maxNodes,
      maxEdgeNames,
    });
  }

  providerFactsJson(index, { pretty = false } = {}) {
    this.materialize();
    return native.projectProviderFacts(this._handle, index, pretty);
  }

  dispose() {
    if (this._handle !== null) {
      native.projectDestroy(this._handle);
      this._handle = null;
    }
  }
}

class Workspace {
  constructor(configJson, projects, { configPath = null } = {}) {
    this.configJson = Buffer.from(configJson);
    this.projects = [...projects];
    this.configPath = configPath;
  }

  static fromConfig(
    configPath,
    { cacheDir = null, cacheMaxBytes = null, typescript = true } = {},
  ) {
    const resolved = path.resolve(configPath);
    const configJson = fs.readFileSync(resolved);
    const plan = JSON.parse(
      native.workspacePlan(configJson, false).toString("utf8"),
    );
    const seen = new Set();
    const projects = plan.projects.map((row, index) => {
      const projectConfig = path.resolve(path.dirname(resolved), row.config);
      if (seen.has(projectConfig)) {
        throw new Error(
          `workspace.projects[${index}].config: duplicate project config`,
        );
      }
      seen.add(projectConfig);
      const root = row.root === null
        ? null
        : path.resolve(path.dirname(resolved), row.root);
      return Project.fromConfig(projectConfig, {
        root, cacheDir, cacheMaxBytes, typescript,
      });
    });
    return new Workspace(configJson, projects, { configPath: resolved });
  }

  json({ pretty = false } = {}) {
    return analyzeWorkspace(
      this.configJson,
      this.projects.map((project) => project.mapJson()),
      { pretty },
    );
  }

  data() {
    return JSON.parse(this.json().toString("utf8"));
  }

  dispose() {
    for (const project of this.projects) project.dispose();
  }
}

function inventoryState(
  configJson,
  root,
  {
    includeStandardIgnores = false,
    ignoreFiles = [],
    transientExclude = [],
  } = {},
) {
  const files = [];
  const pruned = [];
  let pending = [["", root]];
  const standardIgnores = [];
  const customSet = new Set(ignoreFiles);
  const transientSet = new Set(transientExclude);
  const customIgnores = ignoreFiles.map((relative) => ({
    data: fs.readFileSync(path.join(root, ...relative.split("/"))),
    path: relative,
  }));
  while (pending.length) {
    const directories = [];
    for (const [directoryRelative, directory] of pending) {
      const entries = fs.readdirSync(directory, { withFileTypes: true })
        .sort((left, right) => utf8Compare(left.name, right.name));
      for (const entry of entries) {
        const candidate = path.join(directory, entry.name);
        const relative = path.relative(root, candidate).split(path.sep).join("/");
        if (entry.isDirectory()) directories.push([relative, candidate]);
        else if (entry.isFile() && !transientSet.has(relative)) {
          files.push(relative);
        }
      }
      if (includeStandardIgnores) {
        for (const name of [".gitignore", ".ignore", ".archbirdignore"]) {
          const relative = directoryRelative ? `${directoryRelative}/${name}` : name;
          if (customSet.has(relative)) continue;
          const candidate = path.join(directory, name);
          try {
            const metadata = fs.lstatSync(candidate);
            if (metadata.isFile() && !metadata.isSymbolicLink()) {
              standardIgnores.push({ data: fs.readFileSync(candidate), path: relative });
            }
          } catch (error) {
            if (!error || error.code !== "ENOENT") throw error;
          }
        }
      }
    }
    directories.sort((left, right) => utf8Compare(left[0], right[0]));
    const activeIgnores = [...standardIgnores, ...customIgnores];
    const decisions = native.discoveryDescend(
      configJson,
      directories.map(([relative]) => relative),
      activeIgnores.map((row) => row.path),
      activeIgnores.map((row) => row.data),
    );
    pending = directories
      .filter((_, index) => decisions[index])
      .map(([relative, candidate]) => [relative, candidate]);
    directories.forEach(([relative], index) => {
      if (!decisions[index]) pruned.push(relative);
    });
  }
  return {
    files: files.sort(utf8Compare),
    pruned: pruned.sort(utf8Compare),
  };
}

function configBytes(config) {
  if (config === null || config === undefined) return Buffer.alloc(0);
  if (Buffer.isBuffer(config)) return config;
  return fs.readFileSync(path.resolve(config));
}

function sourceRows(values) {
  return values.map((value) => {
    const split = value.indexOf("=");
    if (split <= 0 || split === value.length - 1) {
      throw new Error(`--source expects LANGUAGE=GLOB: ${value}`);
    }
    return { language: value.slice(0, split), glob: value.slice(split + 1) };
  });
}

function mapRequest({
  project = null,
  source = [],
  only = [],
  exclude = [],
  ignoreFiles = [],
  useIgnoreFiles = true,
  defaultExcludes = true,
  maxFileBytes = null,
  maxIndexBytes = null,
} = {}) {
  const request = {
    artifact: "archbird-map-request",
    exclude: [...exclude],
    ignore_files: [...ignoreFiles],
    only: [...only],
    schema_version: 1,
    sources: sourceRows(source),
  };
  if (!defaultExcludes) request.default_excludes = false;
  if (!useIgnoreFiles) request.ignore = false;
  if (project !== null) request.project = project;
  if (maxFileBytes !== null) {
    if (!Number.isSafeInteger(maxFileBytes) || maxFileBytes <= 0) {
      throw new Error("--max-file-bytes must be a positive safe integer");
    }
    request.max_file_bytes = maxFileBytes;
  }
  if (maxIndexBytes !== null) {
    if (!Number.isSafeInteger(maxIndexBytes) || maxIndexBytes <= 0) {
      throw new Error("--max-index-bytes must be a positive safe integer");
    }
    request.max_index_bytes = maxIndexBytes;
  }
  return Buffer.from(JSON.stringify(canonicalForDigest(request)));
}

function fileRow(root, relative) {
  const candidate = path.join(root, ...relative.split("/"));
  let metadata;
  try {
    metadata = fs.lstatSync(candidate);
  } catch (error) {
    if (error && error.code === "ENOENT") return null;
    throw new Error(`cannot stat repository file ${relative}: ${error.message}`);
  }
  if (!metadata.isFile() || metadata.isSymbolicLink()) return null;
  return { bytes: metadata.size, path: relative };
}

function inventoryRows(configJson, root, options = {}) {
  const state = inventoryState(configJson, root, options);
  return {
    pruned: state.pruned,
    rows: state.files
      .map((relative) => fileRow(root, relative))
      .filter(Boolean),
  };
}

function safeRelative(root, value) {
  const candidate = path.resolve(root, value);
  const relative = path.relative(root, candidate).split(path.sep).join("/");
  if (!relative || relative === "." || relative === ".." || relative.startsWith("../")) {
    throw new Error(`path is outside repository root: ${value}`);
  }
  return relative;
}

function ignoreSortKey(relative) {
  const parts = relative.split("/");
  const priorities = { ".gitignore": 0, ".ignore": 1, ".archbirdignore": 2 };
  return [parts.length - 1, parts.slice(0, -1).join("/"), priorities[parts.at(-1)] ?? 3, relative];
}

function compareTuple(left, right) {
  for (let index = 0; index < left.length; index += 1) {
    if (left[index] < right[index]) return -1;
    if (left[index] > right[index]) return 1;
  }
  return 0;
}

function normalizeSourceOverlay(overlay) {
  if (!overlay || typeof overlay !== "object" || Array.isArray(overlay)) {
    throw new Error("source overlay must be a path-to-bytes object");
  }
  const normalized = Object.create(null);
  for (const [relative, data] of Object.entries(overlay)) {
    if (
      !relative ||
      relative.startsWith("/") ||
      relative.includes("\\") ||
      relative.split("/").some((part) => !part || part === "." || part === "..")
    ) {
      throw new Error(`invalid source overlay path: ${JSON.stringify(relative)}`);
    }
    if (data !== null && !Buffer.isBuffer(data)) {
      throw new Error(`source overlay value must be a Buffer or null: ${relative}`);
    }
    normalized[relative] = data === null ? null : Buffer.from(data);
  }
  return normalized;
}

function overlayChangesIgnoreInputs(
  root,
  overlay,
  customIgnoreFiles,
  includeStandardIgnores,
) {
  const ignoreNames = new Set([".gitignore", ".ignore", ".archbirdignore"]);
  const protectedIgnores = new Set();
  for (const relative of Object.keys(overlay)) {
    if (
      customIgnoreFiles.includes(relative) ||
      (includeStandardIgnores && ignoreNames.has(path.posix.basename(relative)))
    ) {
      protectedIgnores.add(relative);
    }
  }
  for (const relative of [...protectedIgnores].sort(utf8Compare)) {
    let current = null;
    try {
      current = fs.readFileSync(path.join(root, ...relative.split("/")));
    } catch (error) {
      if (!error || error.code !== "ENOENT") {
        throw new Error(
          `cannot read discovery ignore input ${relative}: ${error.message}`,
        );
      }
    }
    const after = overlay[relative];
    if (
      (after === null) !== (current === null) ||
      (after !== null && current !== null && !after.equals(current))
    ) {
      return true;
    }
  }
  return false;
}

function overlayRows(rows, overlay) {
  const indexed = new Map(rows.map((row) => [
    row.path,
    { bytes: row.bytes, path: row.path },
  ]));
  for (const [relative, data] of Object.entries(overlay)) {
    if (data === null) indexed.delete(relative);
    else indexed.set(relative, { bytes: data.length, path: relative });
  }
  return [...indexed.values()].sort((left, right) =>
    utf8Compare(left.path, right.path));
}

function encodedInput(root, relative, overlay = null) {
  let data;
  if (overlay && Object.hasOwn(overlay, relative)) {
    data = overlay[relative];
    if (data === null) {
      throw new Error(`discovery input was removed by source overlay: ${relative}`);
    }
  } else {
    const candidate = path.join(root, ...relative.split("/"));
    const metadata = fs.lstatSync(candidate);
    if (!metadata.isFile() || metadata.isSymbolicLink()) {
      throw new Error(`discovery input is not a regular file: ${relative}`);
    }
    data = fs.readFileSync(candidate);
  }
  return { content_hex: data.toString("hex"), path: relative };
}

function repositoryInventory(
  root,
  rows,
  {
    includeStandardIgnores = true,
    ignoreFiles = [],
    prunedDirectories = [],
    overlay = null,
  } = {},
) {
  const paths = new Set(rows.map((row) => row.path));
  const standard = [...paths]
    .filter((relative) => [".gitignore", ".ignore", ".archbirdignore"].includes(path.posix.basename(relative)))
    .sort((left, right) => compareTuple(ignoreSortKey(left), ignoreSortKey(right)));
  const custom = [...ignoreFiles];
  const customSet = new Set(custom);
  const selected = includeStandardIgnores
    ? standard.filter((relative) => !customSet.has(relative))
    : [];
  const seen = new Set(selected);
  for (const relative of custom) {
    if (!seen.has(relative)) {
      selected.push(relative);
      seen.add(relative);
    }
  }
  const documents = ["package.json", "pyproject.toml", "DESCRIPTION", "configure.ac"]
    .filter((relative) => paths.has(relative))
    .map((relative) => encodedInput(root, relative, overlay));
  return Buffer.from(JSON.stringify(canonicalForDigest({
    artifact: "archbird-repository-inventory",
    documents,
    files: [...rows],
    ignore_files: selected.map((relative) => encodedInput(root, relative, overlay)),
    pruned_directories: [...prunedDirectories],
    schema_version: 1,
  })));
}

function rootRows(root) {
  return [
    ".archbirdignore",
    ".gitignore",
    ".ignore",
    "Makefile",
    "DESCRIPTION",
    "NAMESPACE",
    "configure.ac",
    "package.json",
    "pyproject.toml",
  ]
    .map((relative) => fileRow(root, relative))
    .filter(Boolean);
}

function resolveDiscovery(
  root = ".",
  {
    config = null,
    project = null,
    source = [],
    only = [],
    exclude = [],
    ignore = true,
    ignoreFiles = [],
    defaultExcludes = true,
    maxFileBytes = null,
    maxIndexBytes = null,
    pretty = false,
    _transientExclude = [],
    _sourceOverlay = null,
  } = {},
) {
  const repository = path.resolve(root);
  if (!fs.statSync(repository).isDirectory()) {
    throw new Error(`root is not a directory: ${repository}`);
  }
  const configJson = configBytes(config);
  const overlay = normalizeSourceOverlay(_sourceOverlay || {});
  const normalizedIgnoreFiles = [...new Set(
    ignoreFiles.map((value) => safeRelative(repository, value)),
  )];
  const normalizedTransientExclude = [...new Set(
    _transientExclude.map((value) => safeRelative(repository, value)),
  )];
  const ignoreInputsChanged = overlayChangesIgnoreInputs(
    repository,
    overlay,
    normalizedIgnoreFiles,
    ignore,
  );
  const request = mapRequest({
    project,
    source,
    only,
    exclude,
    ignoreFiles: normalizedIgnoreFiles,
    useIgnoreFiles: ignore,
    defaultExcludes,
    maxFileBytes,
    maxIndexBytes,
  });
  const bootstrapPaths = new Set([
    ".archbirdignore",
    ".gitignore",
    ".ignore",
    "Makefile",
    "DESCRIPTION",
    "NAMESPACE",
    "configure.ac",
    "package.json",
    "pyproject.toml",
  ]);
  const bootstrapOverlay = Object.create(null);
  for (const [relative, data] of Object.entries(overlay)) {
    if (bootstrapPaths.has(relative)) bootstrapOverlay[relative] = data;
  }
  const bootstrapRows = overlayRows(
    rootRows(repository).filter(
      (row) => !normalizedTransientExclude.includes(row.path),
    ),
    bootstrapOverlay,
  );
  const bootstrapInventory = repositoryInventory(
    repository,
    bootstrapRows,
    {
      includeStandardIgnores: ignore,
      ignoreFiles: normalizedIgnoreFiles,
      overlay,
    },
  );
  const bootstrap = JSON.parse(
    native.discoveryResolve(configJson, request, bootstrapInventory, false).toString("utf8"),
  );
  const effectiveConfig = Buffer.from(
    JSON.stringify(canonicalForDigest(bootstrap.effective_config)),
  );
  const inventoryState = inventoryRows(effectiveConfig, repository, {
    // Do not prune the virtual after-state with stale disk ignore rules.
    // The native resolver applies the overlaid ignore documents below.
    includeStandardIgnores: ignore && !ignoreInputsChanged,
    ignoreFiles: ignoreInputsChanged ? [] : normalizedIgnoreFiles,
    transientExclude: normalizedTransientExclude,
  });
  const finalOverlay = Object.create(null);
  for (const [relative, data] of Object.entries(overlay)) {
    if (!normalizedTransientExclude.includes(relative)) {
      finalOverlay[relative] = data;
    }
  }
  const rows = overlayRows(inventoryState.rows, finalOverlay);
  const inventory = repositoryInventory(repository, rows, {
    includeStandardIgnores: ignore,
    ignoreFiles: normalizedIgnoreFiles,
    prunedDirectories: inventoryState.pruned,
    overlay,
  });
  return native.discoveryResolve(configJson, request, inventory, pretty);
}

function readSources(root, plan, overlay = null) {
  return plan.files.map((row) => {
    const isIndex = row.roles.includes("index");
    const byteLimit = isIndex ? plan.max_index_bytes : plan.max_file_bytes;
    const limitName = isIndex ? "max_index_bytes" : "max_file_bytes";
    let data;
    if (overlay && Object.hasOwn(overlay, row.path)) {
      data = overlay[row.path];
      if (data === null) {
        throw new Error(
          `selected source was removed by source overlay: ${row.path}`,
        );
      }
    } else {
      const candidate = path.join(root, ...row.path.split("/"));
      const metadata = fs.lstatSync(candidate);
      if (!metadata.isFile()) {
        throw new Error(`selected source is no longer a regular file: ${row.path}`);
      }
      if (metadata.size > byteLimit) {
        throw new Error(
          `selected ${isIndex ? "index" : "source"} exceeds limits.${limitName}: ` +
            `${row.path}: ${metadata.size} > ${byteLimit}`,
        );
      }
      data = fs.readFileSync(candidate);
    }
    if (data.length > byteLimit) {
      throw new Error(
        `selected ${isIndex ? "index" : "source"} exceeds ` +
          `limits.${limitName} while reading: ${row.path}`,
      );
    }
    return new Source(row.path, data, {
      language: row.language,
      layer: row.layer,
      roles: row.roles,
    });
  });
}

function fsSourceIdentity() {
  // The published JavaScript adapter is reviewable source; its exact bytes
  // identify host behavior without mtime, checkout root, or generated paths.
  return fs.readFileSync(__filename);
}

function strictDocument(raw, label) {
  let value;
  try {
    value = JSON.parse(native.jsonCanonicalize(Buffer.from(raw)).toString("utf8"));
  } catch (error) {
    throw new Error(`invalid ${label}: ${error.message}`);
  }
  if (!value || Array.isArray(value) || typeof value !== "object") {
    throw new Error(`invalid ${label}: expected object`);
  }
  return value;
}

function discoveryPlan(configJson, paths, { pretty = false } = {}) {
  return native.discoveryPlan(Buffer.from(configJson), [...paths], pretty);
}

function queryRequest(
  {
    focus = [],
    paths = [],
    symbols = [],
    components = [],
    packages = [],
    artifacts = [],
    search = [],
    searchLimit = 8,
    changeSet = null,
    context = null,
    plan = null,
    direction = "both",
    producerPolicy = "compatible",
    depth = 1,
    testDepth = 8,
  } = {},
) {
  if (plan !== null) {
    if (
      focus.length || paths.length || symbols.length || components.length ||
      packages.length || artifacts.length || search.length || context !== null ||
      searchLimit !== 8 || direction !== "both" || depth !== 1 || testDepth !== 8
    ) {
      throw new TypeError(
        "QueryPlan execution accepts only changeSet and producerPolicy; " +
        "compile query operations into the plan",
      );
    }
    const request = { plan, producer_policy: producerPolicy };
    if (changeSet !== null) request.change_set = changeSet;
    return canonicalForDigest(request);
  }
  const request = {
    artifacts,
    components,
    depth,
    direction,
    focus,
    packages,
    paths,
    producer_policy: producerPolicy,
    search,
    search_limit: searchLimit,
    symbols,
    test_depth: testDepth,
  };
  if (changeSet !== null) request.change_set = changeSet;
  if (context !== null) request.context = context;
  return canonicalForDigest(request);
}

function queryMap(mapJson, options = {}) {
  const request = queryRequest(options);
  return native.mapQuery(
    Buffer.from(mapJson),
    Buffer.from(options.resolutionJson ?? Buffer.alloc(0)),
    Buffer.from(JSON.stringify(request)),
    options.pretty ?? false,
  );
}

function pathRequest({
  source,
  target,
  level = "file",
  relations = null,
  direction = "downstream",
  maxDepth = 8,
  maxPaths = 8,
  producerPolicy = "compatible",
} = {}) {
  const request = {
    artifact: "path-request",
    direction,
    level,
    max_depth: maxDepth,
    max_paths: maxPaths,
    producer_policy: producerPolicy,
    schema_version: 1,
    source,
    target,
  };
  if (relations !== null) request.relations = [...relations];
  return canonicalForDigest(request);
}

function pathMap(mapJson, options = {}) {
  return native.mapPath(
    Buffer.from(mapJson),
    Buffer.from(options.resolutionJson ?? Buffer.alloc(0)),
    Buffer.from(JSON.stringify(pathRequest(options))),
    options.pretty ?? false,
  );
}

function pathMapMarkdown(mapJson, options = {}) {
  return native.mapPathMarkdown(
    Buffer.from(mapJson),
    Buffer.from(options.resolutionJson ?? Buffer.alloc(0)),
    Buffer.from(JSON.stringify(pathRequest(options))),
    options.maxChars ?? 0,
  );
}

function renderPathMarkdown(pathJson, options = {}) {
  return native.pathRenderMarkdown(
    Buffer.from(pathJson),
    options.maxChars ?? 0,
  );
}

function renderMapMarkdown(
  mapJson,
  options = {},
) {
  const request = mapProjectionRequest(options);
  return native.projectionRenderMarkdown(
    Buffer.from(mapJson),
    Buffer.from(options.resolutionJson ?? Buffer.alloc(0)),
    Buffer.from(JSON.stringify(request.definition)),
    request.detail,
    request.maxChars,
  );
}

function queryMapMarkdown(mapJson, options = {}) {
  const request = queryRequest(options);
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
  const selectedDetail = compact ? "compact" : (full ? "full" : detail);
  const verificationResult = options.verificationResult ?? Buffer.alloc(0);
  if (verificationResult.length && view !== "changes") {
    throw new RangeError("verificationResult requires the changes view");
  }
  return native.mapQueryMarkdownView(
    Buffer.from(mapJson),
    Buffer.from(options.resolutionJson ?? Buffer.alloc(0)),
    Buffer.from(JSON.stringify(request)),
    views[view],
    details[selectedDetail],
    options.maxChars ?? 0,
    Buffer.from(verificationResult),
  );
}

function renderSourceMarkdown(project, artifactJson, options = {}) {
  if (!(project instanceof Project)) {
    throw new TypeError("source Markdown requires a Project");
  }
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
  return native.projectSourceMarkdown(
    project._handle,
    Buffer.from(artifactJson),
    details[selectedDetail],
    maxChars,
  );
}

function diffMaps(before, after, { pretty = false } = {}) {
  return native.mapDiff(Buffer.from(before), Buffer.from(after), pretty);
}

function auditMapFreshness(snapshot, current, { pretty = false } = {}) {
  return native.mapFreshness(
    Buffer.from(snapshot),
    Buffer.from(current),
    pretty,
  );
}

function jsonArray(buffers) {
  const parts = [Buffer.from("[")];
  buffers.forEach((value, index) => {
    if (index) parts.push(Buffer.from(","));
    parts.push(Buffer.from(value));
  });
  parts.push(Buffer.from("]"));
  return Buffer.concat(parts);
}

function analyzeWorkspace(configJson, maps, { pretty = false } = {}) {
  return native.workspaceAnalyze(
    Buffer.from(configJson),
    jsonArray([...maps]),
    pretty,
  );
}

function compileProjectConfiguration(configJson, { pretty = false } = {}) {
  return native.projectConfigurationCompile(Buffer.from(configJson), pretty);
}

function evaluateProjection(
  mapJson,
  projectionJson,
  { resolutionJson = Buffer.alloc(0), pretty = false } = {},
) {
  return native.projectionEvaluate(
    Buffer.from(mapJson),
    Buffer.from(resolutionJson),
    Buffer.from(projectionJson),
    pretty,
  );
}

function compileQueryPlan(
  configJson,
  queryId,
  {
    overridesJson = Buffer.alloc(0),
    pretty = false,
  } = {},
) {
  return native.queryPlanCompile(
    Buffer.from(configJson),
    queryId,
    Buffer.from(overridesJson),
    pretty,
  );
}

function evaluateConstraints(
  configJson,
  mapJson,
  {
    resolutionJson = Buffer.alloc(0),
    requestJson = Buffer.alloc(0),
    pretty = false,
  } = {},
) {
  return native.constraintsEvaluate(
    Buffer.from(configJson),
    Buffer.from(mapJson),
    Buffer.from(resolutionJson),
    Buffer.from(requestJson),
    pretty,
  );
}

function validatePlan(planJson) {
  native.planValidate(Buffer.from(planJson));
}

function renderPlanMarkdown(planJson) {
  return native.planRenderMarkdown(Buffer.from(planJson));
}

function compilePlan(
  project,
  mapJson,
  verificationJson,
  {
    beforeMapJson = Buffer.alloc(0),
    requestJson = Buffer.alloc(0),
    pretty = false,
  } = {},
) {
  if (!(project instanceof Project)) {
    throw new TypeError("Plan compilation requires a Project");
  }
  return native.planCompile(
    project._handle,
    Buffer.from(mapJson),
    Buffer.from(verificationJson),
    Buffer.from(beforeMapJson),
    Buffer.from(requestJson),
    pretty,
  );
}

function validateAct(actJson) {
  native.actValidate(Buffer.from(actJson));
}

function planSourceRequirements(
  planJson,
  {
    executorSubmissionsJson = Buffer.alloc(0),
    pretty = false,
  } = {},
) {
  return native.planSourceRequirements(
    Buffer.from(planJson),
    Buffer.from(executorSubmissionsJson),
    pretty,
  );
}

function actSourceRequirements(actJson, { pretty = false } = {}) {
  return native.actSourceRequirements(Buffer.from(actJson), pretty);
}

function materializeAct(
  project,
  planJson,
  mapJson,
  verificationJson,
  sourceMetadataJson,
  {
    executorSubmissionsJson = Buffer.alloc(0),
    pretty = false,
  } = {},
) {
  if (!(project instanceof Project)) {
    throw new TypeError("Act materialization requires a Project");
  }
  return native.actMaterialize(
    project._handle,
    Buffer.from(planJson),
    Buffer.from(mapJson),
    Buffer.from(verificationJson),
    Buffer.from(sourceMetadataJson),
    Buffer.from(executorSubmissionsJson),
    pretty,
  );
}

function acceptAct(
  actJson,
  beforeMapJson,
  afterMapJson,
  verificationJson,
  gateResultsJson = Buffer.alloc(0),
  { pretty = false } = {},
) {
  return native.actAccept(
    Buffer.from(actJson),
    Buffer.from(beforeMapJson),
    Buffer.from(afterMapJson),
    Buffer.from(verificationJson),
    Buffer.from(gateResultsJson),
    pretty,
  );
}

function preflightActApply(actJson, sourceMetadataJson) {
  const state = native.actPreflightApply(
    Buffer.from(actJson),
    Buffer.from(sourceMetadataJson),
  );
  if (state === 0) return "ready";
  if (state === 1) return "already_satisfied";
  throw new Error(`native Act preflight returned unknown state ${state}`);
}

function reportConstraints(
  configJson,
  mapJson,
  {
    resolutionJson = Buffer.alloc(0),
    requestJson = Buffer.alloc(0),
    format = "markdown",
    maxFindings = 200,
    pretty = false,
  } = {},
) {
  if (!Number.isInteger(maxFindings) || maxFindings < 0 || maxFindings > 0xffffffff) {
    throw new RangeError("maxFindings must be an integer in [0, 2^32-1]");
  }
  return native.constraintsReport(
    Buffer.from(configJson),
    Buffer.from(mapJson),
    Buffer.from(resolutionJson),
    Buffer.from(requestJson),
    format,
    maxFindings,
    pretty,
  );
}

function freezeConstraints(
  configJson,
  mapJson,
  {
    resolutionJson = Buffer.alloc(0),
    requestJson = Buffer.alloc(0),
    owner,
    rationale,
    pretty = true,
  } = {},
) {
  if (typeof owner !== "string" || !owner.trim()) {
    throw new TypeError("baseline owner must be non-empty");
  }
  if (typeof rationale !== "string" || !rationale.trim()) {
    throw new TypeError("baseline rationale must be non-empty");
  }
  return native.constraintsFreeze(
    Buffer.from(configJson),
    Buffer.from(mapJson),
    Buffer.from(resolutionJson),
    Buffer.from(requestJson),
    owner,
    rationale,
    pretty,
  );
}

function exportGraph(
  artifactJson,
  {
    format,
    view = "components",
    direction = "LR",
    maxNodes = 200,
    maxEdgeNames = 3,
  } = {},
) {
  if (!Number.isInteger(maxNodes) || maxNodes < 0 || maxNodes > 0xffffffff) {
    throw new RangeError("maxNodes must be an integer in [0, 2^32-1]");
  }
  if (
    !Number.isInteger(maxEdgeNames) ||
    maxEdgeNames < 0 ||
    maxEdgeNames > 0xffffffff
  ) {
    throw new RangeError("maxEdgeNames must be an integer in [0, 2^32-1]");
  }
  return native.mapExportGraph(
    Buffer.from(artifactJson),
    format,
    view,
    direction,
    maxNodes,
    maxEdgeNames,
  );
}

function analyzeOkfSource(
  sourceBundleJson,
  {
    queryJson = Buffer.alloc(0),
    format = "json",
    includeBody = false,
    pretty = true,
  } = {},
) {
  return native.okfAnalyze(
    Buffer.from(sourceBundleJson),
    Buffer.from(queryJson),
    format,
    includeBody,
    pretty,
  );
}

function publishOkfBundle(
  mapJson,
  {
    verificationJson = Buffer.alloc(0),
    normalizationJson = null,
    pretty = false,
  } = {},
) {
  const artifacts = [mapJson, verificationJson].map((value) =>
    Buffer.from(value)
  );
  let normalization = normalizationJson;
  if (normalization === null) {
    normalization = okfNormalization(artifacts);
  }
  return native.okfPublish(
    ...artifacts,
    Buffer.from(normalization),
    pretty,
  );
}

module.exports = {
  ENGINE: native.ENGINE,
  IMPLEMENTATION_SHA256: native.IMPLEMENTATION_SHA256,
  NATIVE_ABI_VERSION: native.NATIVE_ABI_VERSION,
  PATTERN_CONTRACT: native.PATTERN_CONTRACT,
  PATTERN_CONTRACT_VERSION: native.PATTERN_CONTRACT_VERSION,
  PATTERN_ENGINE: native.PATTERN_ENGINE,
  PATTERN_OPTIONS: native.PATTERN_OPTIONS,
  PATTERN_UNICODE: native.PATTERN_UNICODE,
  PROVIDER_SUPPORT,
  VERSION: native.VERSION,
  Project,
  Source,
  Workspace,
  auditMapFreshness,
  analyzeOkfSource,
  planSourceRequirements,
  publishOkfBundle,
  analyzeWorkspace,
  applyAcceptedAct,
  acceptAct,
  compilePlan,
  compileProjectConfiguration,
  compileQueryPlan,
  compileTestObservations,
  defaultProviderCacheDir,
  defaultProviderCacheMaxBytes,
  discoveryPlan,
  resolveDiscovery,
  diffMaps,
  evaluateConstraints,
  evaluateProjection,
  exportGraph,
  queryMap,
  queryMapMarkdown,
  reportConstraints,
  renderMapMarkdown,
  renderPathMarkdown,
  renderPlanMarkdown,
  renderSourceMarkdown,
  freezeConstraints,
  materializeAct,
  observeActSources,
  observePlanSources,
  pathMap,
  pathMapMarkdown,
  actOverlay,
  actSourceRequirements,
  preflightActApply,
  renderAct,
  runActGates,
  validateAct,
  validatePlan,
  jsonCanonicalize: native.jsonCanonicalize,
};
