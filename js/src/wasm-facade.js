"use strict";

const { Buffer } = require("buffer");

const JSON_PRETTY = 1;
const JSON_TRAILING_NEWLINE = 2;

function createWasmFacade(module, { mode = "wasm" } = {}) {
  if (!module || typeof module._ab_wasm_project_create !== "function") {
    throw new TypeError("invalid Archbird Wasm module");
  }

  const projectFinalizer = typeof FinalizationRegistry === "function"
    ? new FinalizationRegistry((pointer) => module._ab_wasm_project_destroy(pointer))
    : null;

  function asBuffer(value) {
    if (Buffer.isBuffer(value)) return value;
    if (value instanceof Uint8Array) {
      return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
    }
    if (typeof value === "string") return Buffer.from(value);
    return Buffer.from(value);
  }

  function allocate(value) {
    const bytes = asBuffer(value);
    const pointer = module._malloc(Math.max(bytes.length, 1));
    if (!pointer) throw new Error("Archbird Wasm input allocation failed");
    if (bytes.length) module.HEAPU8.set(bytes, pointer);
    return { bytes, length: bytes.length, pointer };
  }

  function withInputs(values, callback) {
    const inputs = [];
    try {
      for (const value of values) inputs.push(allocate(value));
      return callback(inputs);
    } finally {
      for (let index = inputs.length - 1; index >= 0; index -= 1) {
        module._free(inputs[index].pointer);
      }
    }
  }

  function readBytes(pointer, length) {
    if (!length) return Buffer.alloc(0);
    if (!pointer) throw new Error("Archbird Wasm returned a null result pointer");
    return Buffer.from(module.HEAPU8.slice(pointer, pointer + length));
  }

  function readCString(pointer) {
    if (!pointer) return "";
    let end = pointer;
    while (module.HEAPU8[end] !== 0) end += 1;
    return readBytes(pointer, end - pointer).toString("utf8");
  }

  function errorFromCore(status) {
    const pointer = module._ab_wasm_error_data();
    const length = module._ab_wasm_error_length();
    const message = pointer && length
      ? readBytes(pointer, length).toString("utf8")
      : `native Archbird operation failed (status=${status})`;
    const error = new Error(message);
    error.code = `ARCHBIRD_STATUS_${status}`;
    error.status = status;
    const offset = module._ab_wasm_error_offset();
    if (offset !== -1 && offset !== 0xffffffff) error.offset = offset;
    return error;
  }

  function result(status) {
    if (status !== 0) throw errorFromCore(status);
    return readBytes(module._ab_wasm_output_data(), module._ab_wasm_output_length());
  }

  function success(status) {
    if (status !== 0) throw errorFromCore(status);
  }

  function boolFlags(pretty, trailing = false) {
    return (pretty ? JSON_PRETTY : 0) | (trailing ? JSON_TRAILING_NEWLINE : 0);
  }

  function modeValue(value) {
    const modes = { primary: 0, augment: 1, audit: 2 };
    if (!Object.hasOwn(modes, value)) {
      throw new RangeError("mode must be primary, augment, or audit");
    }
    return modes[value];
  }

  function checkedProject(handle) {
    if (!handle || typeof handle !== "object" || !handle.pointer || handle.disposed) {
      throw new TypeError("expected live Wasm Project");
    }
    return handle.pointer;
  }

  function projectCreate(manifest) {
    return withInputs([manifest], ([input]) => {
      const pointer = module._ab_wasm_project_create(input.pointer, input.length);
      if (!pointer) throw errorFromCore(module._ab_wasm_last_status());
      const handle = { pointer, disposed: false };
      if (projectFinalizer) projectFinalizer.register(handle, pointer, handle);
      return handle;
    });
  }

  function projectDestroy(handle) {
    const pointer = checkedProject(handle);
    if (projectFinalizer) projectFinalizer.unregister(handle);
    module._ab_wasm_project_destroy(pointer);
    handle.pointer = 0;
    handle.disposed = true;
  }

  function projectAddSource(handle, sourcePath, data) {
    const pointer = checkedProject(handle);
    return withInputs([sourcePath, data], ([pathInput, dataInput]) => {
      success(module._ab_wasm_project_add_source(
        pointer,
        pathInput.pointer,
        pathInput.length,
        dataInput.pointer,
        dataInput.length,
      ));
    });
  }

  function projectFinalizeSources(handle) {
    success(module._ab_wasm_project_finalize_sources(checkedProject(handle)));
  }

  function projectSetConfig(handle, config) {
    const pointer = checkedProject(handle);
    return withInputs([config], ([input]) => {
      success(module._ab_wasm_project_set_config(pointer, input.pointer, input.length));
    });
  }

  function projectAddProvider(handle, providerMode, provider) {
    const pointer = checkedProject(handle);
    return withInputs([provider], ([input]) => {
      success(module._ab_wasm_project_add_provider(
        pointer,
        modeValue(providerMode),
        input.pointer,
        input.length,
      ));
    });
  }

  function projectAddTestSymbolObservations(handle, observations) {
    const pointer = checkedProject(handle);
    return withInputs([observations], ([input]) => {
      success(module._ab_wasm_project_add_test_symbol_observations(
        pointer,
        input.pointer,
        input.length,
      ));
    });
  }

  function projectScanBuiltin(handle, providerMode = "primary") {
    success(module._ab_wasm_project_scan_builtin(
      checkedProject(handle),
      modeValue(providerMode),
    ));
  }

  function projectScanBuiltinProvider(handle, providerId, providerMode = "primary") {
    const pointer = checkedProject(handle);
    return withInputs([providerId], ([input]) => {
      success(module._ab_wasm_project_scan_builtin_provider(
        pointer,
        input.pointer,
        input.length,
        modeValue(providerMode),
      ));
    });
  }

  function projectScanBuiltinProviderFile(
    handle,
    providerId,
    path,
    providerMode = "primary",
  ) {
    const pointer = checkedProject(handle);
    return withInputs([providerId, path], ([provider, sourcePath]) => {
      success(module._ab_wasm_project_scan_builtin_provider_file(
        pointer,
        provider.pointer,
        provider.length,
        sourcePath.pointer,
        sourcePath.length,
        modeValue(providerMode),
      ));
    });
  }

  function projectFinalizeProviders(handle) {
    success(module._ab_wasm_project_finalize_providers(checkedProject(handle)));
  }

  function projectDigest(handle, functionName) {
    return result(module[functionName](checkedProject(handle))).toString("ascii");
  }

  function exactObject(encoded) {
    return Object.fromEntries(
      Object.entries(JSON.parse(encoded.toString("utf8"))).map(([key, value]) => [
        key,
        BigInt(value),
      ]),
    );
  }

  function projectCounts(handle) {
    return exactObject(result(module._ab_wasm_project_counts(checkedProject(handle))));
  }

  function projectMergeSummary(handle) {
    return exactObject(result(module._ab_wasm_project_merge_summary(checkedProject(handle))));
  }

  function projectRender(handle, functionName, pretty = false) {
    return result(module[functionName](checkedProject(handle), boolFlags(pretty)));
  }

  function projectProviderFacts(handle, index, pretty = false) {
    return result(module._ab_wasm_project_provider_facts(
      checkedProject(handle),
      index,
      boolFlags(pretty),
    ));
  }

  function discoveryHandle(config) {
    return withInputs([config], ([input]) => {
      const pointer = module._ab_wasm_discovery_create(input.pointer, input.length);
      if (!pointer) throw errorFromCore(module._ab_wasm_last_status());
      return pointer;
    });
  }

  function withDiscovery(config, callback) {
    const pointer = discoveryHandle(config);
    try {
      return callback(pointer);
    } finally {
      module._ab_wasm_discovery_destroy(pointer);
    }
  }

  function discoveryPlan(config, paths, pretty = false) {
    return withDiscovery(config, (handle) => {
      for (const sourcePath of paths) {
        withInputs([sourcePath], ([input]) => {
          success(module._ab_wasm_discovery_add_path(handle, input.pointer, input.length));
        });
      }
      return result(module._ab_wasm_discovery_render(handle, boolFlags(pretty)));
    });
  }

  function discoveryDescend(config, paths) {
    return withDiscovery(config, (handle) => paths.map((sourcePath) =>
      withInputs([sourcePath], ([input]) => {
        const value = module._ab_wasm_discovery_should_descend(
          handle,
          input.pointer,
          input.length,
        );
        if (value < 0) throw errorFromCore(module._ab_wasm_last_status());
        return Boolean(value);
      })));
  }

  function oneInput(value, functionName, ...arguments_) {
    return withInputs([value], ([input]) => result(module[functionName](
      input.pointer,
      input.length,
      ...arguments_,
    )));
  }

  function twoInputs(left, right, functionName, ...arguments_) {
    return withInputs([left, right], ([first, second]) => result(module[functionName](
      first.pointer,
      first.length,
      second.pointer,
      second.length,
      ...arguments_,
    )));
  }

  function threeInputs(firstValue, secondValue, thirdValue, functionName, ...arguments_) {
    return withInputs(
      [firstValue, secondValue, thirdValue],
      ([first, second, third]) => result(module[functionName](
        first.pointer,
        first.length,
        second.pointer,
        second.length,
        third.pointer,
        third.length,
        ...arguments_,
      )),
    );
  }

  function formatValue(value, values, label) {
    const index = values.indexOf(value);
    if (index < 0) throw new RangeError(`${label} must be ${values.join(", ")}`);
    return index;
  }

  function sizeValue(value, label) {
    if (!Number.isSafeInteger(value) || value < 0 || value > 0xffffffff) {
      throw new TypeError(`${label} must be a nonnegative safe integer`);
    }
    return value;
  }

  const facade = {
    ENGINE: Object.freeze({ kind: "wasm", mode }),
    NATIVE_ABI_VERSION: module._ab_wasm_native_abi_version(),
    IMPLEMENTATION_SHA256: readCString(
      module._ab_wasm_implementation_sha256(),
    ),
    PATTERN_CONTRACT_VERSION: module._ab_wasm_pattern_contract_version(),
    PATTERN_CONTRACT: readCString(module._ab_wasm_pattern_contract()),
    PATTERN_ENGINE: readCString(module._ab_wasm_pattern_engine()),
    PATTERN_UNICODE: readCString(module._ab_wasm_pattern_unicode()),
    PATTERN_OPTIONS: readCString(module._ab_wasm_pattern_options()),
    VERSION: readCString(module._ab_wasm_version()),
    projectCreate,
    projectDestroy,
    projectAddSource,
    projectFinalizeSources,
    projectSetConfig,
    projectAddProvider,
    projectAddTestSymbolObservations,
    projectScanBuiltin,
    projectScanBuiltinProvider,
    projectScanBuiltinProviderFile,
    projectFinalizeProviders,
    projectManifestSha256: (handle) =>
      projectDigest(handle, "_ab_wasm_project_manifest_sha256"),
    projectMapInputSha256: (handle) =>
      projectDigest(handle, "_ab_wasm_project_map_input_sha256"),
    projectConfigSha256: (handle) =>
      projectDigest(handle, "_ab_wasm_project_config_sha256"),
    projectCounts,
    projectMergeSummary,
    projectFileFacts: (handle, pretty = false) =>
      projectRender(handle, "_ab_wasm_project_file_facts", pretty),
    projectMergeLedger: (handle, pretty = false) =>
      projectRender(handle, "_ab_wasm_project_merge_ledger", pretty),
    projectMergeConflicts: (handle, pretty = false) =>
      projectRender(handle, "_ab_wasm_project_merge_conflicts", pretty),
    projectMap: (handle, pretty = false) =>
      projectRender(handle, "_ab_wasm_project_map", pretty),
    projectProviderFacts,
    discoveryPlan,
    discoveryDescend,
    discoveryResolve: (config, request, inventory, pretty = false) =>
      threeInputs(
        config,
        request,
        inventory,
        "_ab_wasm_discovery_resolve",
        boolFlags(pretty),
      ),
    sha256: (value) => oneInput(value, "_ab_wasm_sha256").toString("ascii"),
    jsonCanonicalize: (value, pretty = false, trailing = false) =>
      oneInput(value, "_ab_wasm_json_canonicalize", boolFlags(pretty, trailing)),
    mapMarkdown: (map, full = false, maxChars = 0) =>
      oneInput(
        map,
        "_ab_wasm_map_markdown",
        full ? 1 : 0,
        sizeValue(maxChars, "maxChars"),
      ),
    mapMarkdownView: (map, view = 0, detail = 1, maxChars = 0) =>
      oneInput(
        map,
        "_ab_wasm_map_markdown_view",
        sizeValue(view, "view"),
        sizeValue(detail, "detail"),
        sizeValue(maxChars, "maxChars"),
      ),
    mapQuery: (map, query, pretty = false) =>
      twoInputs(map, query, "_ab_wasm_map_query", boolFlags(pretty)),
    mapQueryMarkdown: (map, query, maxChars = 0) =>
      twoInputs(
        map,
        query,
        "_ab_wasm_map_query_markdown",
        sizeValue(maxChars, "maxChars"),
      ),
    mapQueryMarkdownView: (
      map,
      query,
      view = 0,
      detail = 1,
      maxChars = 0,
      verification = Buffer.alloc(0),
    ) =>
      threeInputs(
        map,
        query,
        verification,
        "_ab_wasm_map_query_markdown_view_with_verification",
        sizeValue(view, "view"),
        sizeValue(detail, "detail"),
        sizeValue(maxChars, "maxChars"),
      ),
    mapDiff: (before, after, pretty = false) =>
      twoInputs(before, after, "_ab_wasm_map_diff", boolFlags(pretty)),
    mapFreshness: (snapshot, current, pretty = false) =>
      twoInputs(
        snapshot,
        current,
        "_ab_wasm_map_freshness",
        boolFlags(pretty),
      ),
    mapExportGraph(map, format, view, direction, maxNodes, maxEdgeNames) {
      return oneInput(
        map,
        "_ab_wasm_map_export_graph",
        formatValue(format, ["graphml", "mermaid", "json"], "graph format"),
        formatValue(view, ["components", "files", "symbols"], "graph view"),
        formatValue(direction, ["LR", "RL", "TB", "BT"], "graph direction"),
        sizeValue(maxNodes, "maxNodes"),
        sizeValue(maxEdgeNames, "maxEdgeNames"),
      );
    },
    okfAnalyze(source, query, format, includeBody = false, pretty = true) {
      return twoInputs(
        source,
        query,
        "_ab_wasm_okf_analyze",
        formatValue(format, ["json", "markdown"], "OKF format"),
        includeBody ? 1 : 0,
        boolFlags(pretty, true),
      );
    },
    okfPublish(map, verification, proposal, contract, changeResult, normalization, pretty = false) {
      return withInputs(
        [map, verification, proposal, contract, changeResult, normalization],
        ([mapInput, verificationInput, proposalInput, contractInput, resultInput, normalizationInput]) =>
          result(module._ab_wasm_okf_publish(
            mapInput.pointer,
            mapInput.length,
            verificationInput.pointer,
            verificationInput.length,
            proposalInput.pointer,
            proposalInput.length,
            contractInput.pointer,
            contractInput.length,
            resultInput.pointer,
            resultInput.length,
            normalizationInput.pointer,
            normalizationInput.length,
            boolFlags(pretty, true),
          )),
      );
    },
    workspacePlan: (config, pretty = false) =>
      oneInput(config, "_ab_wasm_workspace_plan", boolFlags(pretty)),
    workspaceAnalyze: (config, maps, pretty = false) =>
      twoInputs(config, maps, "_ab_wasm_workspace_analyze", boolFlags(pretty)),
    verificationPlan: (suite, pretty = false) =>
      oneInput(suite, "_ab_wasm_verification_plan", boolFlags(pretty)),
    verificationRecipeCatalog: (recipe = "", pretty = false) =>
      oneInput(recipe, "_ab_wasm_verification_recipe_catalog", boolFlags(pretty)),
    verificationRecipeCompile: (request, pretty = false) =>
      oneInput(request, "_ab_wasm_verification_recipe_compile", boolFlags(pretty)),
    verificationAnalyze: (suite, input, pretty = false) =>
      twoInputs(suite, input, "_ab_wasm_verification_analyze", boolFlags(pretty)),
    verificationDraft: (map, projectConfig, pretty = false) =>
      twoInputs(map, projectConfig, "_ab_wasm_verification_draft", boolFlags(pretty, true)),
    verificationFreeze(suite, input, owner, rationale, pretty = false) {
      return withInputs(
        [suite, input, owner, rationale],
        ([suiteInput, inputInput, ownerInput, rationaleInput]) =>
          result(module._ab_wasm_verification_freeze(
            suiteInput.pointer,
            suiteInput.length,
            inputInput.pointer,
            inputInput.length,
            ownerInput.pointer,
            ownerInput.length,
            rationaleInput.pointer,
            rationaleInput.length,
            boolFlags(pretty, true),
          )),
      );
    },
    verificationReport(suite, input, format, maxFindings = 200, pretty = false) {
      const formatNumber = formatValue(
        format,
        ["json", "markdown", "sarif", "junit"],
        "verification format",
      );
      if (formatNumber === 0) {
        return facade.verificationAnalyze(suite, input, pretty);
      }
      return twoInputs(
        suite,
        input,
        "_ab_wasm_verification_report",
        formatNumber,
        sizeValue(maxFindings, "maxFindings"),
        boolFlags(pretty, format === "sarif"),
      );
    },
    changeProposal(verification, fingerprint, format, full, maxCandidates, pretty) {
      return twoInputs(
        verification,
        fingerprint,
        "_ab_wasm_change_proposal",
        formatValue(format, ["json", "markdown"], "change proposal format"),
        full ? 1 : 0,
        sizeValue(maxCandidates, "maxCandidates"),
        boolFlags(pretty),
      );
    },
    changeContract(proposal, review, format, pretty) {
      return twoInputs(
        proposal,
        review,
        "_ab_wasm_change_contract",
        formatValue(format, ["json", "markdown"], "change contract format"),
        boolFlags(pretty),
      );
    },
    changeVerify(proposal, contract, before, after, format, pretty) {
      return withInputs(
        [proposal, contract, before, after],
        ([proposalInput, contractInput, beforeInput, afterInput]) =>
          result(module._ab_wasm_change_verify(
            proposalInput.pointer,
            proposalInput.length,
            contractInput.pointer,
            contractInput.length,
            beforeInput.pointer,
            beforeInput.length,
            afterInput.pointer,
            afterInput.length,
            formatValue(
              format,
              ["json", "markdown", "sarif", "junit"],
              "change result format",
            ),
            boolFlags(pretty, format === "sarif"),
          )),
      );
    },
  };
  return Object.freeze(facade);
}

module.exports = { createWasmFacade };
