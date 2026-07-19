#ifndef ARCHBIRD_MAP_INTERNAL_H
#define ARCHBIRD_MAP_INTERNAL_H

#include "config.h"
#include "path_match.h"
#include "project_internal.h"
#include "render_internal.h"

typedef struct AbMapSymbolReference {
  const AbManifestFile *file;
  const AbFact *fact;
  const char *leaf;
  size_t leaf_length;
} AbMapSymbolReference;

const char *ab_map_symbol_language_family(const AbManifestFile *file,
                                          size_t *out_length);
void ab_map_symbol_leaf(const AbFact *fact, const char **out_leaf,
                        size_t *out_length);
int ab_map_symbol_reference_compare(const void *left_raw,
                                    const void *right_raw);
int ab_map_symbol_definition_compare(const AbFact *left, const AbFact *right);
void ab_map_symbol_reference_range(const AbMapSymbolReference *symbols,
                                   size_t symbol_count,
                                   const AbManifestFile *source,
                                   const AbString *name, size_t *out_start,
                                   size_t *out_end);
int ab_map_symbol_reference_usable(const AbMapSymbolReference *symbol,
                                   const AbManifestFile *source,
                                   int allow_methods, int allow_declarations);
const AbObjectField *ab_map_fact_attribute(const AbFact *fact,
                                           const char *name);
const AbString *ab_map_fact_string_attribute(const AbFact *fact,
                                             const char *name);
int ab_map_fact_u64_attribute(const AbFact *fact, const char *name,
                              uint64_t *out);
const AbFact *ab_map_enclosing_symbol(const ArchbirdProject *project,
                                      const AbFact *occurrence);
const AbFact *ab_map_unique_semantic_target(const ArchbirdProject *project,
                                            const AbFact *occurrence);

typedef struct AbMapNamedReference {
  const AbManifestFile *file;
  const AbString *name;
  size_t count;
  int builtin;
  unsigned binding_mask;
  unsigned import_delimiter_mask;
} AbMapNamedReference;

typedef struct AbMapEdgeMention {
  AbString kind;
  const AbString *source;
  AbString target;
  AbString name;
  AbString evidence_basis;
  AbString evidence_provider;
  AbString evidence_state;
  int has_evidence;
} AbMapEdgeMention;

typedef struct AbMapCallResolution {
  const AbString *source;
  const AbString *name;
  const char *kind;
  size_t count;
  AbString *candidates;
  size_t candidate_count;
} AbMapCallResolution;

typedef struct AbMapGraph {
  AbMapEdgeMention *edges;
  size_t edge_count;
  size_t edge_capacity;
  AbMapCallResolution *resolutions;
  size_t resolution_count;
  size_t resolution_capacity;
} AbMapGraph;

typedef struct AbMapDiagnostic {
  AbString severity;
  AbString code;
  AbString message;
  AbString path;
  size_t span_start;
  size_t span_end;
  int has_span;
} AbMapDiagnostic;

typedef struct AbMapDependency {
  AbString name;
  AbString requirement;
  AbString scope;
} AbMapDependency;

typedef struct AbMapExportOrigin {
  AbString name;
  AbString target_symbol;
  AbStringArray paths;
  int target_symbol_ambiguous;
} AbMapExportOrigin;

typedef struct AbMapEntrypointSurface {
  AbString path;
  AbStringArray exports;
  AbMapExportOrigin *export_origins;
  size_t export_origin_count;
  int partial;
} AbMapEntrypointSurface;

typedef struct AbMapPackage {
  AbString name;
  AbString kind;
  AbString layer;
  AbString manifest;
  AbString identity;
  AbString version;
  AbStringArray aliases;
  AbMapDependency *dependencies;
  size_t dependency_count;
  AbStringPair *entrypoints;
  size_t entrypoint_count;
  AbStringArray npm_runtime_entries;
  int npm_has_exports;
  AbStringArray exports;
  AbMapExportOrigin *export_origins;
  size_t export_origin_count;
  AbMapEntrypointSurface *entrypoint_surfaces;
  size_t entrypoint_surface_count;
  int export_surface_partial;
  AbStringPair *scripts;
  size_t script_count;
} AbMapPackage;

typedef struct AbMapBuildRoute {
  AbString source;
  AbString name;
  AbStringArray deps;
  AbStringArray paths;
  AbString command;
  AbStringArray conditions;
} AbMapBuildRoute;

typedef struct AbMapArtifactInput {
  AbString path;
  AbStringArray evidence;
} AbMapArtifactInput;

typedef struct AbMapArtifactLoader {
  AbString path;
  AbString pattern;
  size_t matches;
} AbMapArtifactLoader;

typedef struct AbMapArtifactBuild {
  AbString source;
  AbString target;
} AbMapArtifactBuild;

typedef struct AbMapArtifact {
  AbString name;
  AbString output;
  AbMapArtifactInput *inputs;
  size_t input_count;
  AbMapArtifactLoader *loaders;
  size_t loader_count;
  AbMapArtifactBuild *builds;
  size_t build_count;
  AbStringArray depends_on;
} AbMapArtifact;

typedef struct AbMapSurfaceDeclaration {
  AbString path;
  AbString source;
} AbMapSurfaceDeclaration;

typedef struct AbMapSurfaceUse {
  AbString path;
  size_t count;
} AbMapSurfaceUse;

typedef struct AbMapSurfaceName {
  AbString name;
  AbString declaration;
  AbMapSurfaceDeclaration *declarations;
  size_t declaration_count;
  AbMapSurfaceUse *uses;
  size_t use_count;
  AbStringArray candidates;
  AbString resolution;
  AbStringArray declaration_signatures;
  AbStringArray implementation_signatures;
  int ignored;
} AbMapSurfaceName;

typedef struct AbMapSurface {
  AbString name;
  AbString kind;
  int provider_configured;
  AbMapSurfaceDeclaration *providers;
  size_t provider_count;
  AbMapSurfaceName *names;
  size_t name_count;
} AbMapSurface;

typedef struct AbMapRouteCount {
  AbString path;
  size_t count;
} AbMapRouteCount;

typedef struct AbMapRouteEvidence {
  AbString target;
  AbString target_symbol;
  AbString fact_id;
  AbString relation;
  AbString provenance;
  AbString provider;
  AbString claim;
  AbString enclosing;
  AbString name;
  AbString scope;
  AbString observation_sha256;
  AbString evidence_slice_sha256;
  AbString producer_version;
  AbString producer_implementation_sha256;
  AbString producer_configuration_sha256;
  AbString runtime;
  size_t line;
  size_t span_start;
  size_t span_end;
  size_t hits;
  int has_hits;
} AbMapRouteEvidence;

typedef struct AbMapTestCase {
  AbString selector;
  AbString evidence_kind;
  size_t line;
  AbMapRouteCount *routes;
  size_t route_count;
  AbMapRouteEvidence *route_evidence;
  size_t route_evidence_count;
  AbStringArray configured_routes;
} AbMapTestCase;

typedef struct AbMapTest {
  AbString path;
  AbString group;
  AbString language;
  AbString framework;
  size_t count;
  int generated;
  AbStringArray generated_from;
  AbStringArray selectors;
  AbMapRouteCount *routes;
  size_t route_count;
  AbMapRouteEvidence *route_evidence;
  size_t route_evidence_count;
  AbMapTestCase *cases;
  size_t case_count;
  int candidate;
} AbMapTest;

typedef struct AbMapNamedEntryPath {
  AbString path;
  AbStringArray names;
} AbMapNamedEntryPath;

typedef struct AbMapNamedEntry {
  AbString name;
  AbMapNamedEntryPath *paths;
  size_t path_count;
} AbMapNamedEntry;

typedef struct AbMapParityEvidence {
  AbString name;
  AbStringArray locations;
} AbMapParityEvidence;

typedef struct AbMapParityMember {
  AbString label;
  AbStringArray values;
  AbMapParityEvidence *evidence;
  size_t evidence_count;
  AbStringArray missing;
} AbMapParityMember;

typedef struct AbMapParity {
  AbString name;
  int enforce;
  AbStringArray shared;
  AbMapParityMember *members;
  size_t member_count;
} AbMapParity;

typedef struct AbMapIndex {
  AbString name;
  AbString format;
  AbString path;
  AbString path_prefix;
  AbString evidence_state;
  AbString sha256;
  AbString tool_name;
  AbString tool_version;
  size_t documents_total;
  size_t documents_mapped;
  size_t documents_stale;
  size_t documents_source_unverified;
  size_t documents_source_verified;
  size_t symbols;
  size_t occurrences;
  size_t invalid_ranges;
  size_t source_mismatches;
  size_t position_encoding_fallback_documents;
  size_t references;
  size_t reference_facts;
  size_t resolved_unique;
  size_t resolved_ambiguous;
  size_t unresolved;
  size_t relationships;
  size_t relationship_edges;
  size_t edge_count;
  uint32_t position_encoding_fallback;
} AbMapIndex;

typedef struct AbMapState {
  ArchbirdEngine *engine;
  const ArchbirdProject *project;
  const AbSourceManifest *manifest;
  const AbMapConfig *config;
  AbMapGraph graph;
  AbMapPackage *packages;
  size_t package_count;
  AbMapBuildRoute *builds;
  size_t build_count;
  AbMapArtifact *artifacts;
  size_t artifact_count;
  AbMapSurface *surfaces;
  size_t surface_count;
  AbMapTest *tests;
  size_t test_count;
  AbMapNamedEntry *named_entries;
  size_t named_entry_count;
  AbMapParity *parity;
  size_t parity_count;
  AbMapIndex *indexes;
  size_t index_count;
  AbMapDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
} AbMapState;

ArchbirdStatus ab_map_add_diagnostic(AbMapState *state, const char *severity,
                                     const char *code, const char *message,
                                     const AbString *path);
ArchbirdStatus ab_map_add_diagnostic_span(AbMapState *state,
                                          const char *severity,
                                          const char *code, const char *message,
                                          const AbString *path,
                                          size_t span_start, size_t span_end);
void ab_map_diagnostics_sort_unique(AbMapState *state);

void ab_map_state_free(AbMapState *state);
void ab_map_graph_free(ArchbirdEngine *engine, AbMapGraph *graph);
ArchbirdStatus ab_map_graph_add_edge(ArchbirdEngine *engine, AbMapGraph *graph,
                                     const char *kind, const AbString *source,
                                     const char *target, size_t target_length,
                                     const AbString *name);
ArchbirdStatus ab_map_graph_add_edge_evidence(
    ArchbirdEngine *engine, AbMapGraph *graph, const char *kind,
    const AbString *source, const char *target, size_t target_length,
    const AbString *name, const char *evidence_basis,
    const AbString *evidence_provider, const AbString *evidence_state);
void ab_map_graph_sort(AbMapGraph *graph);
void ab_map_package_clear(ArchbirdEngine *engine, AbMapPackage *package);

const char *ab_map_package_manager(const AbString *kind);
const char *ab_map_language_manager(const AbString *language);
AbString ab_map_external_import_name(const AbString *language,
                                     const AbString *imported);
int ab_map_package_alias_matches(const AbMapPackage *package,
                                 const char *manager, const AbString *external);

ArchbirdStatus ab_map_analyze_packages(AbMapState *state);
ArchbirdStatus ab_map_render_packages(AbBuffer *buffer,
                                      const AbMapState *state);

ArchbirdStatus ab_map_analyze_builds(AbMapState *state);
ArchbirdStatus ab_map_render_builds(AbBuffer *buffer, const AbMapState *state);

ArchbirdStatus ab_map_analyze_artifacts(AbMapState *state);
ArchbirdStatus ab_map_render_artifacts(AbBuffer *buffer,
                                       const AbMapState *state);

ArchbirdStatus ab_map_analyze_bridges(AbMapState *state);
ArchbirdStatus ab_map_render_surfaces(AbBuffer *buffer,
                                      const AbMapState *state);

ArchbirdStatus ab_map_analyze_tests(AbMapState *state);
ArchbirdStatus ab_map_render_tests(AbBuffer *buffer, const AbMapState *state);

ArchbirdStatus ab_map_analyze_named_entries(AbMapState *state);
ArchbirdStatus ab_map_render_named_entries(AbBuffer *buffer,
                                           const AbMapState *state);

ArchbirdStatus ab_map_render_symbol_calls(AbBuffer *buffer,
                                          const AbMapState *state);
ArchbirdStatus ab_map_render_symbol_references(AbBuffer *buffer,
                                               const AbMapState *state);

ArchbirdStatus ab_map_analyze_parity(AbMapState *state);
ArchbirdStatus ab_map_render_parity(AbBuffer *buffer, const AbMapState *state);

ArchbirdStatus ab_map_analyze_indexes(AbMapState *state);
ArchbirdStatus ab_map_render_indexes(AbBuffer *buffer, const AbMapState *state);

ArchbirdStatus ab_make_variable_value(ArchbirdEngine *engine,
                                      const uint8_t *text, size_t text_length,
                                      const AbString *name, AbString *out,
                                      int *found);

const AbManifestFile *ab_map_manifest_file(const AbSourceManifest *manifest,
                                           const char *path,
                                           size_t path_length);

ArchbirdStatus
ab_map_resolve_import(ArchbirdEngine *engine, const AbSourceManifest *manifest,
                      const AbMapConfig *config, const AbManifestFile *file,
                      const AbString *imported, const AbManifestFile **out);

ArchbirdStatus ab_map_run_legacy_checks(AbMapState *state);

#endif
