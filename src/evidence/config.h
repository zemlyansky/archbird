#ifndef ARCHBIRD_CONFIG_H
#define ARCHBIRD_CONFIG_H

#include "model.h"

typedef struct AbExternalNamespace {
  AbString prefix;
  AbString package;
} AbExternalNamespace;

typedef struct AbConfigLayer {
  AbString name;
  AbString role;
  AbString language;
  AbStringArray globs;
  AbStringArray public_headers;
  AbStringArray import_roots;
  AbExternalNamespace *external_namespaces;
  size_t external_namespace_count;
  int required;
} AbConfigLayer;

typedef struct AbConfigComponent {
  AbString name;
  AbString description;
  AbStringArray paths;
  int required;
} AbConfigComponent;

typedef struct AbConfigPackage {
  AbString name;
  AbString kind;
  AbString path;
  AbString layer;
  AbStringArray entries;
  AbString identity;
  AbString version;
  AbStringArray aliases;
} AbConfigPackage;

typedef struct AbConfigBuild {
  AbString name;
  AbString kind;
  AbString path;
  AbString variant;
} AbConfigBuild;

typedef struct AbConfigIndex {
  AbString name;
  AbString format;
  AbString path;
  AbString path_prefix;
  AbString variant;
  uint32_t position_encoding_fallback;
  int required;
} AbConfigIndex;

typedef struct AbConfigArtifactLoader {
  AbStringArray paths;
  AbString pattern;
  int required;
} AbConfigArtifactLoader;

typedef struct AbConfigArtifactBuild {
  AbString source;
  AbString target;
} AbConfigArtifactBuild;

typedef struct AbConfigArtifact {
  AbString name;
  AbString output;
  AbStringArray inputs;
  AbConfigArtifactLoader *loaders;
  size_t loader_count;
  AbConfigArtifactBuild *builds;
  size_t build_count;
  int required;
} AbConfigArtifact;

typedef struct AbConfigProvider {
  AbString kind;
  AbString path;
  AbString variable;
  AbString pattern;
} AbConfigProvider;

typedef struct AbConfigBridge {
  AbString name;
  AbString kind;
  AbStringArray from_layers;
  AbStringArray from_paths;
  AbStringArray exclude_from_paths;
  AbStringArray to_layers;
  AbStringArray prefixes;
  int bidirectional;
  AbStringArray message_keys;
  AbStringArray ignore;
  AbConfigProvider *providers;
  size_t provider_count;
} AbConfigBridge;

typedef struct AbConfigTestCaseRoute {
  AbString selector;
  AbStringArray paths;
  AbStringArray targets;
  AbStringArray target_symbols;
} AbConfigTestCaseRoute;

typedef struct AbConfigTestCaseExtractor {
  AbString kind;
  AbString call;
  AbString name;
  size_t *selector_arguments;
  size_t selector_argument_count;
  size_t selector_argument;
  AbString separator;
} AbConfigTestCaseExtractor;

typedef struct AbConfigTestGeneratedFile {
  AbStringArray globs;
  AbStringArray sources;
} AbConfigTestGeneratedFile;

typedef struct AbConfigTest {
  AbString name;
  AbString language;
  AbStringArray globs;
  AbStringArray route_to;
  AbConfigTestCaseRoute *case_routes;
  size_t case_route_count;
  AbConfigTestCaseExtractor *case_extractors;
  size_t case_extractor_count;
  AbConfigTestGeneratedFile *generated_files;
  size_t generated_file_count;
  int required;
  int discovered;
} AbConfigTest;

typedef struct AbConfigNamedEntry {
  AbString name;
  AbString kind;
  AbStringArray functions;
  size_t argument;
  AbStringArray globs;
} AbConfigNamedEntry;

typedef struct AbConfigParityMember {
  AbString label;
  AbString source;
  AbString layer;
  AbString package;
  AbString bridge;
  AbStringArray include;
  AbStringArray exclude;
  AbStringArray kinds;
} AbConfigParityMember;

typedef struct AbStringPair {
  AbString key;
  AbString value;
} AbStringPair;

typedef struct AbConfigParity {
  AbString name;
  AbConfigParityMember *members;
  size_t member_count;
  AbString case_name;
  AbStringArray strip_prefixes;
  AbStringArray strip_suffixes;
  AbStringPair *aliases;
  size_t alias_count;
  AbStringArray ignore;
  int enforce;
} AbConfigParity;

typedef struct AbMapConfig {
  AbString project;
  AbString description;
  AbString root;
  AbStringArray exclude;
  AbConfigLayer *layers;
  size_t layer_count;
  AbConfigComponent *components;
  size_t component_count;
  AbConfigPackage *packages;
  size_t package_count;
  AbConfigBuild *builds;
  size_t build_count;
  AbConfigIndex *indexes;
  size_t index_count;
  AbConfigArtifact *artifacts;
  size_t artifact_count;
  AbConfigBridge *bridges;
  size_t bridge_count;
  AbConfigTest *tests;
  size_t test_count;
  AbConfigNamedEntry *named_entries;
  size_t named_entry_count;
  AbConfigParity *parity;
  size_t parity_count;
  size_t max_file_bytes;
  size_t max_index_bytes;
  size_t compact_symbols;
  size_t compact_edge_names;
  int default_excludes;
  uint8_t sha256[32];
  char sha256_hex[65];
} AbMapConfig;

ArchbirdStatus ab_decode_map_config(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbMapConfig *out);

void ab_map_config_free(ArchbirdEngine *engine, AbMapConfig *config);

const AbConfigLayer *ab_map_config_layer(const AbMapConfig *config,
                                         const AbString *name);

#endif
