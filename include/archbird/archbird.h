#ifndef ARCHBIRD_ARCHBIRD_H
#define ARCHBIRD_ARCHBIRD_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(ARCHBIRD_SHARED_BUILD)
#define ARCHBIRD_API __declspec(dllexport)
#elif defined(ARCHBIRD_SHARED_USE)
#define ARCHBIRD_API __declspec(dllimport)
#else
#define ARCHBIRD_API
#endif
#elif (defined(__GNUC__) || defined(__clang__)) &&                             \
    defined(ARCHBIRD_SHARED_BUILD)
#define ARCHBIRD_API __attribute__((visibility("default")))
#else
#define ARCHBIRD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental native ABI. Version 0 is intentionally not stability-frozen. */
#define ARCHBIRD_NATIVE_ABI_VERSION 0u
/* One parser defines configured-pattern semantics for every frontend. */
#define ARCHBIRD_PATTERN_CONTRACT_VERSION 1u
#define ARCHBIRD_PATTERN_CONTRACT "archbird-pcre2-v1"
#define ARCHBIRD_PATTERN_ENGINE "PCRE2 10.47"
#define ARCHBIRD_PATTERN_UNICODE "UCD 16.0.0"
#define ARCHBIRD_PATTERN_OPTIONS                                               \
  "UTF,UCP,NEWLINE_LF,BSR_UNICODE,NEVER_BACKSLASH_C,NEVER_CALLOUT,JIT_"        \
  "DISABLED"
#define ARCHBIRD_NO_OFFSET SIZE_MAX

typedef struct ArchbirdEngine ArchbirdEngine;
typedef struct ArchbirdProject ArchbirdProject;
typedef struct ArchbirdDiscovery ArchbirdDiscovery;

typedef enum ArchbirdStatus {
  ARCHBIRD_OK = 0,
  ARCHBIRD_INVALID_ARGUMENT = 1,
  ARCHBIRD_OUT_OF_MEMORY = 2,
  ARCHBIRD_INVALID_JSON = 3,
  ARCHBIRD_DUPLICATE_KEY = 4,
  ARCHBIRD_LIMIT_EXCEEDED = 5,
  ARCHBIRD_UNSUPPORTED_NUMBER = 6,
  ARCHBIRD_WRITE_FAILED = 7,
  ARCHBIRD_INVALID_SCHEMA = 8,
  ARCHBIRD_CONFLICT = 9,
  ARCHBIRD_POLICY_REJECTED = 10
} ArchbirdStatus;

typedef void *(*ArchbirdAllocateFn)(void *user_data, size_t size);
typedef void *(*ArchbirdReallocateFn)(void *user_data, void *pointer,
                                      size_t size);
typedef void (*ArchbirdDeallocateFn)(void *user_data, void *pointer);

typedef struct ArchbirdEngineOptions {
  size_t struct_size;
  size_t max_input_bytes;
  size_t max_depth;
  size_t max_values;
  size_t max_string_bytes;
  size_t max_files;
  size_t max_file_bytes;
  size_t max_index_bytes;
  size_t max_source_bytes;
  size_t max_syntax_bytes;
  size_t max_provider_bundles;
  size_t max_facts;
  size_t max_pattern_matches;
  uint32_t regex_match_limit;
  uint32_t regex_depth_limit;
  uint32_t regex_heap_limit_kib;
  ArchbirdAllocateFn allocate;
  ArchbirdReallocateFn reallocate;
  ArchbirdDeallocateFn deallocate;
  void *allocator_user_data;
} ArchbirdEngineOptions;

/* One-shot frontend engines must opt into the larger saved-artifact budget.
 * Raw byte length never becomes a semantic value-count limit. */
typedef enum ArchbirdInputProfile {
  ARCHBIRD_INPUT_DEFAULT = 0,
  ARCHBIRD_INPUT_SAVED_ARTIFACT = 1
} ArchbirdInputProfile;

typedef enum ArchbirdProviderMode {
  ARCHBIRD_PROVIDER_PRIMARY = 0,
  ARCHBIRD_PROVIDER_AUGMENT = 1,
  ARCHBIRD_PROVIDER_AUDIT = 2
} ArchbirdProviderMode;

typedef struct ArchbirdSourceView {
  size_t struct_size;
  const char *path;
  size_t path_length;
  const uint8_t *bytes;
  size_t byte_length;
  const char *language;
  size_t language_length;
  const char *layer;
  size_t layer_length;
} ArchbirdSourceView;

typedef struct ArchbirdMergeSummary {
  size_t struct_size;
  size_t providers;
  size_t selections;
  size_t selected_facts;
  size_t contributed;
  size_t deduplicated;
  size_t enriched;
  size_t variations;
  size_t conflicts;
  size_t audit_matches;
  size_t audit_differences;
} ArchbirdMergeSummary;

typedef int (*ArchbirdWriteFn)(void *user_data, const uint8_t *bytes,
                               size_t length);

enum ArchbirdJsonFlags {
  ARCHBIRD_JSON_PRETTY = 1u << 0,
  ARCHBIRD_JSON_TRAILING_NEWLINE = 1u << 1
};

typedef enum ArchbirdVerificationFormat {
  ARCHBIRD_VERIFICATION_JSON = 0,
  ARCHBIRD_VERIFICATION_MARKDOWN = 1,
  ARCHBIRD_VERIFICATION_SARIF = 2,
  ARCHBIRD_VERIFICATION_JUNIT = 3
} ArchbirdVerificationFormat;

typedef enum ArchbirdChangeFormat {
  ARCHBIRD_CHANGE_JSON = 0,
  ARCHBIRD_CHANGE_MARKDOWN = 1,
  ARCHBIRD_CHANGE_SARIF = 2,
  ARCHBIRD_CHANGE_JUNIT = 3
} ArchbirdChangeFormat;

typedef enum ArchbirdGraphFormat {
  ARCHBIRD_GRAPH_GRAPHML = 0,
  ARCHBIRD_GRAPH_MERMAID = 1,
  ARCHBIRD_GRAPH_JSON = 2
} ArchbirdGraphFormat;

typedef enum ArchbirdOkfFormat {
  ARCHBIRD_OKF_JSON = 0,
  ARCHBIRD_OKF_MARKDOWN = 1
} ArchbirdOkfFormat;

typedef enum ArchbirdGraphView {
  ARCHBIRD_GRAPH_COMPONENTS = 0,
  ARCHBIRD_GRAPH_FILES = 1,
  ARCHBIRD_GRAPH_SYMBOLS = 2
} ArchbirdGraphView;

typedef enum ArchbirdGraphDirection {
  ARCHBIRD_GRAPH_LR = 0,
  ARCHBIRD_GRAPH_RL = 1,
  ARCHBIRD_GRAPH_TB = 2,
  ARCHBIRD_GRAPH_BT = 3
} ArchbirdGraphDirection;

typedef struct ArchbirdGraphOptions {
  size_t struct_size;
  ArchbirdGraphFormat format;
  ArchbirdGraphView view;
  ArchbirdGraphDirection direction;
  size_t max_nodes;
  size_t max_edge_names;
} ArchbirdGraphOptions;

ARCHBIRD_API void archbird_engine_options_init(ArchbirdEngineOptions *options);

ARCHBIRD_API ArchbirdStatus archbird_engine_options_init_for_input(
    ArchbirdEngineOptions *options, ArchbirdInputProfile profile,
    size_t input_length);

ARCHBIRD_API void archbird_graph_options_init(ArchbirdGraphOptions *options);

/* Return the lowercase SHA-256 identity of the compiled native core.
 * The returned process-lifetime string is owned by Archbird. */
ARCHBIRD_API const char *archbird_implementation_sha256(void);

ARCHBIRD_API ArchbirdStatus archbird_engine_create(
    const ArchbirdEngineOptions *options, ArchbirdEngine **out_engine);

ARCHBIRD_API void archbird_engine_destroy(ArchbirdEngine *engine);

ARCHBIRD_API ArchbirdStatus archbird_discovery_create(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    ArchbirdDiscovery **out_discovery);
ARCHBIRD_API ArchbirdStatus archbird_discovery_add_path(
    ArchbirdEngine *engine, ArchbirdDiscovery *discovery, const char *path,
    size_t path_length);
ARCHBIRD_API ArchbirdStatus archbird_discovery_add_ignore(
    ArchbirdEngine *engine, ArchbirdDiscovery *discovery, const char *path,
    size_t path_length, const uint8_t *bytes, size_t byte_length);
ARCHBIRD_API ArchbirdStatus archbird_discovery_should_descend(
    ArchbirdEngine *engine, ArchbirdDiscovery *discovery, const char *directory,
    size_t directory_length, int *out_should_descend);
ARCHBIRD_API ArchbirdStatus archbird_discovery_render(
    ArchbirdEngine *engine, ArchbirdDiscovery *discovery, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);
ARCHBIRD_API void archbird_discovery_destroy(ArchbirdDiscovery *discovery);

/* Resolve deterministic discovery defaults, an optional schema-1 project
 * configuration, and explicit CLI overlays against a host-provided repository
 * inventory. The result contains the effective configuration, selected file
 * plan, origin ledger, coverage, ignore-input hashes, and a content digest. */
ARCHBIRD_API ArchbirdStatus archbird_discovery_resolve(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *request_json, size_t request_length,
    const uint8_t *inventory_json, size_t inventory_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API const char *archbird_engine_error(const ArchbirdEngine *engine);

/* Returns a byte offset for parser errors, or ARCHBIRD_NO_OFFSET otherwise. */
ARCHBIRD_API size_t archbird_engine_error_offset(const ArchbirdEngine *engine);

ARCHBIRD_API ArchbirdStatus archbird_json_validate(ArchbirdEngine *engine,
                                                   const uint8_t *input,
                                                   size_t input_length);

ARCHBIRD_API ArchbirdStatus archbird_source_manifest_validate(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length);

ARCHBIRD_API ArchbirdStatus archbird_provider_facts_validate(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length);

ARCHBIRD_API ArchbirdStatus
archbird_project_create(ArchbirdEngine *engine, const uint8_t *manifest_json,
                        size_t manifest_length, ArchbirdProject **out_project);

ARCHBIRD_API void archbird_project_destroy(ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus archbird_project_add_source(
    ArchbirdEngine *engine, ArchbirdProject *project, const char *path,
    size_t path_length, const uint8_t *bytes, size_t byte_length);

ARCHBIRD_API ArchbirdStatus archbird_project_finalize_sources(
    ArchbirdEngine *engine, ArchbirdProject *project);

ARCHBIRD_API size_t
archbird_project_source_count(const ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus
archbird_project_source(const ArchbirdProject *project, size_t index,
                        ArchbirdSourceView *out_source);

ARCHBIRD_API const char *
archbird_project_manifest_sha256(const ArchbirdProject *project);

/* Stable digest of sorted repository path/content-digest pairs used by Map. */
ARCHBIRD_API const char *
archbird_project_map_input_sha256(const ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus
archbird_project_set_config(ArchbirdEngine *engine, ArchbirdProject *project,
                            const uint8_t *config_json, size_t config_length);

ARCHBIRD_API const char *
archbird_project_config_sha256(const ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus archbird_project_add_provider_facts(
    ArchbirdEngine *engine, ArchbirdProject *project, ArchbirdProviderMode mode,
    const uint8_t *provider_json, size_t provider_length);

/* Add one strict project-owned observed test-to-symbol artifact. */
ARCHBIRD_API ArchbirdStatus archbird_project_add_test_symbol_observations(
    ArchbirdEngine *engine, ArchbirdProject *project,
    const uint8_t *observations_json, size_t observations_length);

ARCHBIRD_API ArchbirdStatus archbird_test_symbol_observations_validate(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length);

ARCHBIRD_API size_t
archbird_project_provider_count(const ArchbirdProject *project);

ARCHBIRD_API size_t
archbird_project_provider_fact_count(const ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus archbird_project_render_provider_facts(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    size_t provider_index, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

ARCHBIRD_API ArchbirdStatus
archbird_project_scan_builtin(ArchbirdEngine *engine, ArchbirdProject *project,
                              ArchbirdProviderMode mode);

/* Run one portable provider by stable ID: lexical:c, lexical:javascript,
 * lexical:python, lexical:r, semantic:scip, or a compiled
 * syntax:tree-sitter:{c,cpp,python,javascript,typescript,tsx,r} pack.
 * Lexical/protocol, syntax, host-native, and supplied semantic providers all
 * emit the same normalized provider-facts IR. */
ARCHBIRD_API ArchbirdStatus archbird_project_scan_builtin_provider(
    ArchbirdEngine *engine, ArchbirdProject *project, const char *provider_id,
    size_t provider_id_length, ArchbirdProviderMode mode);

/* Run one file-local lexical or syntax provider for an exact manifest path.
 * Project-scoped semantic providers intentionally reject this API. */
ARCHBIRD_API ArchbirdStatus archbird_project_scan_builtin_provider_file(
    ArchbirdEngine *engine, ArchbirdProject *project, const char *provider_id,
    size_t provider_id_length, const char *path, size_t path_length,
    ArchbirdProviderMode mode);

ARCHBIRD_API ArchbirdStatus archbird_project_render_file_facts(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_project_render_map(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Project a saved schema-4/5/6/7 Map as the deterministic agent-facing
 * Markdown report.  Compact output defaults to a 30,000-character budget. If
 * the complete overview does not fit, it selects complete semantic sections
 * before ranked file blocks and emits an omission ledger; the Map remains
 * unchanged. Full output is complete and cannot be combined with a nonzero
 * character budget.
 */
ARCHBIRD_API ArchbirdStatus archbird_map_render_markdown(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    int full, size_t max_chars, ArchbirdWriteFn write_fn, void *user_data);

typedef enum ArchbirdMapView {
  ARCHBIRD_MAP_VIEW_OVERVIEW = 0,
  ARCHBIRD_MAP_VIEW_ARCHITECTURE = 1,
  ARCHBIRD_MAP_VIEW_AUDIT = 2
} ArchbirdMapView;

typedef enum ArchbirdReportDetail {
  ARCHBIRD_REPORT_DETAIL_COMPACT = 0,
  ARCHBIRD_REPORT_DETAIL_STANDARD = 1,
  ARCHBIRD_REPORT_DETAIL_FULL = 2
} ArchbirdReportDetail;

typedef enum ArchbirdQueryView {
  ARCHBIRD_QUERY_VIEW_FOCUSED = 0,
  ARCHBIRD_QUERY_VIEW_CHANGES = 1
} ArchbirdQueryView;

/*
 * Render a human projection of the complete Map IR. Views choose the question
 * being answered; detail controls how much evidence that projection exposes.
 * Neither option changes or truncates the canonical Map.
 */
ARCHBIRD_API ArchbirdStatus archbird_map_render_markdown_view(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    ArchbirdMapView view, ArchbirdReportDetail detail, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data);

/* Query requests accept producer_policy="compatible" (the default) to read
 * any supported Map schema, or producer_policy="current" to require the
 * saved Map's core implementation digest to equal the active core. This
 * producer policy is separate from live source/config freshness. */
ARCHBIRD_API ArchbirdStatus archbird_map_query(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *query_json, size_t query_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Evaluate a saved-Map query and project its ranked neighborhood as
 * deterministic Markdown. A zero character budget requests all selected
 * nodes. A positive budget first drops complete lower-ranked file blocks; if
 * the fixed report still does not fit, it emits a smaller section projection
 * with an omission ledger. The canonical Query remains unchanged.
 */
ARCHBIRD_API ArchbirdStatus archbird_map_query_markdown(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *query_json, size_t query_length, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Render the same canonical Query IR for a particular human task. FOCUSED is
 * the compatibility report; CHANGES presents seeds, affected code, strongest
 * routes, tests, delivery surfaces, uncertainty, and omitted evidence as a
 * deterministic change brief. Detail changes presentation only.
 */
ARCHBIRD_API ArchbirdStatus archbird_map_query_markdown_view(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *query_json, size_t query_length, ArchbirdQueryView view,
    ArchbirdReportDetail detail, size_t max_chars, ArchbirdWriteFn write_fn,
    void *user_data);

/*
 * Render a change brief with an optional canonical schema-1 Verification
 * result. The overlay correlates only exact source paths and reports Map/
 * Verification producer and input freshness; it does not modify either
 * canonical artifact or infer that a check applies without evidence overlap.
 */
ARCHBIRD_API ArchbirdStatus archbird_map_query_markdown_view_with_verification(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *query_json, size_t query_length,
    const uint8_t *verification_json, size_t verification_length,
    ArchbirdQueryView view, ArchbirdReportDetail detail, size_t max_chars,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_map_diff(
    ArchbirdEngine *engine, const uint8_t *before_json, size_t before_length,
    const uint8_t *after_json, size_t after_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Compare a saved schema-4/5/6/7 Map or Query snapshot with a freshly derived
 * current Map. The pure core reads no filesystem state: hosts derive the
 * current Map through their normal discovery/provider pipeline, then this
 * function classifies source, configuration, discovery, and producer
 * freshness and cites exact mapped-file deltas where the snapshot permits it.
 */
ARCHBIRD_API ArchbirdStatus
archbird_map_freshness(ArchbirdEngine *engine, const uint8_t *snapshot_json,
                       size_t snapshot_length, const uint8_t *current_map_json,
                       size_t current_map_length, uint32_t json_flags,
                       ArchbirdWriteFn write_fn, void *user_data);

/* Project a canonical saved Map (components/files) or Query (symbols).
   JSON is the typed interactive view; GraphML/Mermaid accept Map views. */
ARCHBIRD_API ArchbirdStatus archbird_map_export_graph(
    ArchbirdEngine *engine, const uint8_t *artifact_json,
    size_t artifact_length, const ArchbirdGraphOptions *options,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Validate and index host-decoded OKF syntax.  The host owns filesystem,
 * YAML, CommonMark, URI, and Unicode case-fold decoding; the native kernel
 * owns shared link resolution, explicit requirement linkage, query policy,
 * diagnostics, and deterministic rendering.  A null/empty query produces an
 * index; a schema-1 okf-query-input produces a concept query.  Prose is never
 * interpreted as a verification check.
 */
ARCHBIRD_API ArchbirdStatus archbird_okf_analyze(
    ArchbirdEngine *engine, const uint8_t *source_bundle_json,
    size_t source_bundle_length, const uint8_t *query_json, size_t query_length,
    ArchbirdOkfFormat format, int include_body, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Project canonical Map/Verify/Act artifacts into one content-addressed OKF
 * output bundle. Optional artifacts must form the complete ordered chain.
 * normalization_json is an optional schema-1 okf-text-normalization artifact
 * supplying host Unicode NFKD/case-fold evidence; ASCII-only inputs need none.
 * The core performs no filesystem writes.
 */
ARCHBIRD_API ArchbirdStatus archbird_okf_publish(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *verification_json, size_t verification_length,
    const uint8_t *proposal_json, size_t proposal_length,
    const uint8_t *contract_json, size_t contract_length,
    const uint8_t *result_json, size_t result_length,
    const uint8_t *normalization_json, size_t normalization_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Validate a workspace configuration and render the path-bearing host plan.
 * Paths remain exactly as asserted in the configuration; resolving and reading
 * them is deliberately owned by the host distribution rather than the core.
 */
ARCHBIRD_API ArchbirdStatus
archbird_workspace_plan(ArchbirdEngine *engine, const uint8_t *workspace_json,
                        size_t workspace_length, uint32_t json_flags,
                        ArchbirdWriteFn write_fn, void *user_data);

/*
 * Join already-derived project maps into one deterministic workspace artifact.
 * maps_json is a JSON array containing one schema-4/5/6/7 map for each
 * configured workspace project. The core performs no repository I/O or project
 * execution.
 */
ARCHBIRD_API ArchbirdStatus archbird_workspace_analyze(
    ArchbirdEngine *engine, const uint8_t *workspace_json,
    size_t workspace_length, const uint8_t *maps_json, size_t maps_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/* Validate a verification suite and render its deterministic host I/O plan. */
ARCHBIRD_API ArchbirdStatus archbird_verification_plan(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Evaluate a reviewed verification suite over host-supplied, content-addressed
 * project evidence.  verification_input_json is a schema-1
 * "verification-input" artifact produced from the deterministic host plan;
 * the core performs no filesystem or project I/O.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_analyze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Evaluate the same reviewed suite and project evidence, then project the
 * canonical result as Markdown, SARIF 2.1.0, or JUnit XML.  JSON callers use
 * archbird_verification_analyze.  max_findings bounds compact Markdown only;
 * SIZE_MAX requests full Markdown.  SARIF and JUnit always remain complete.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_analyze_report(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    ArchbirdVerificationFormat format, size_t max_findings, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Draft a candidate-only component dependency suite from one canonical Map.
 * project_config is the portable path that the resulting suite will use; the
 * core performs no path resolution and never promotes the draft to reviewed
 * architecture intent.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_draft(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const char *project_config, size_t project_config_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Render the portable built-in verification recipe catalog.  An empty recipe
 * name lists every recipe; a non-empty name returns exactly one recipe or
 * fails when it is unknown.  Recipes describe explicit policy inputs and do
 * not infer architectural intent from the current repository graph.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_recipe_catalog(
    ArchbirdEngine *engine, const char *recipe, size_t recipe_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Compile a schema-1 verification-recipe-request into an ordinary reviewed
 * verification suite.  The resulting suite is evaluated by the existing
 * Verify kernel and can be stored, reviewed, baselined, or consumed by any
 * frontend.  The core performs no repository I/O.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_recipe_compile(
    ArchbirdEngine *engine, const uint8_t *request_json, size_t request_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/*
 * Render a reviewed violation/coverage baseline from the same suite and input
 * used by Verify.  Existing baseline state in verification_input_json is
 * carried forward so resolved findings and coverage form a monotonic ratchet.
 */
ARCHBIRD_API ArchbirdStatus archbird_verification_freeze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    const char *owner, size_t owner_length, const char *rationale,
    size_t rationale_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

/*
 * Compile one immutable derived architecture-change proposal from one exact
 * finding in a canonical verification artifact.  The core performs no
 * repository I/O and does not authorize or apply edits.
 */
ARCHBIRD_API ArchbirdStatus archbird_change_proposal(
    ArchbirdEngine *engine, const uint8_t *verification_json,
    size_t verification_length, const char *finding_fingerprint,
    size_t fingerprint_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

/* Seal explicit human review metadata as an asserted change contract. */
ARCHBIRD_API ArchbirdStatus archbird_change_contract(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *review_json, size_t review_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data);

/* Judge an asserted change contract against supplied before/after evidence. */
ARCHBIRD_API ArchbirdStatus archbird_change_verify(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *contract_json,
    size_t contract_length, const uint8_t *before_verification_json,
    size_t before_length, const uint8_t *after_verification_json,
    size_t after_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_change_proposal_report(
    ArchbirdEngine *engine, const uint8_t *verification_json,
    size_t verification_length, const char *finding_fingerprint,
    size_t fingerprint_length, int full, size_t max_candidates,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_change_contract_report(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *review_json, size_t review_length,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_change_verify_report(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *contract_json,
    size_t contract_length, const uint8_t *before_verification_json,
    size_t before_length, const uint8_t *after_verification_json,
    size_t after_length, ArchbirdChangeFormat format, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_project_finalize_providers(
    ArchbirdEngine *engine, ArchbirdProject *project);

ARCHBIRD_API ArchbirdStatus archbird_project_merge_summary(
    const ArchbirdProject *project, ArchbirdMergeSummary *out_summary);

ARCHBIRD_API ArchbirdStatus archbird_project_render_merge_ledger(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

ARCHBIRD_API ArchbirdStatus archbird_project_render_merge_conflicts(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

/*
 * Canonicalize strict UTF-8 JSON using Archbird's current Python-compatible
 * key ordering, escaping, and finite-real formatting. Integers retain arbitrary
 * precision; real syntax is decoded as IEEE-754 binary64 and rendered with the
 * same shortest representation and exponent policy as Python's JSON encoder.
 * Real values outside the finite binary64 domain are rejected.
 *
 * The core performs no I/O. A nonzero callback return stops rendering and
 * produces ARCHBIRD_WRITE_FAILED. Callback bytes from a failed call are not a
 * valid artifact and must be discarded by the host.
 */
ARCHBIRD_API ArchbirdStatus archbird_json_canonicalize(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length,
    uint32_t flags, ArchbirdWriteFn write_fn, void *user_data);

#ifdef __cplusplus
}
#endif

#endif
