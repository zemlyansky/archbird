#ifndef ARCHBIRD_TREE_SITTER_SCANNER_H
#define ARCHBIRD_TREE_SITTER_SCANNER_H

#include "fact_builder.h"
#include "tree_sitter/api.h"

typedef struct AbTreeSitterScan AbTreeSitterScan;

typedef struct AbTreeSitterRecovery {
  size_t start;
  size_t end;
} AbTreeSitterRecovery;

typedef struct AbTreeSitterFactRegion {
  size_t fact_index;
  size_t start;
  size_t end;
} AbTreeSitterFactRegion;

typedef struct AbTreeSitterFrame {
  TSNode node;
  const uint8_t *enclosing;
  size_t enclosing_length;
  unsigned context;
  int has_enclosing;
  int identity_partial;
} AbTreeSitterFrame;

enum {
  AB_TS_CONTEXT_FUNCTION = 1u << 0,
  AB_TS_CONTEXT_CLASS = 1u << 1,
  AB_TS_CONTEXT_CLASS_BINDING = 1u << 2
};

typedef struct AbTreeSitterCapabilitySpec {
  const char *domain;
  const char *boundary;
} AbTreeSitterCapabilitySpec;

typedef ArchbirdStatus (*AbTreeSitterVisitFn)(AbTreeSitterScan *scan,
                                              const AbTreeSitterFrame *frame,
                                              AbTreeSitterFrame *child_frame);
typedef ArchbirdStatus (*AbTreeSitterPrepareFn)(AbTreeSitterScan *scan);
typedef void (*AbTreeSitterCleanupFn)(AbTreeSitterScan *scan);

typedef struct AbTreeSitterDescriptor {
  const char *provider_name;
  const char *provider_version;
  const char *configuration_identity;
  const char *runtime_identity;
  const char *language_name;
  const TSLanguage *(*language)(void);
  const AbTreeSitterCapabilitySpec *capabilities;
  size_t capability_count;
  AbTreeSitterVisitFn visit;
  AbTreeSitterPrepareFn prepare;
  AbTreeSitterCleanupFn cleanup;
} AbTreeSitterDescriptor;

struct AbTreeSitterScan {
  ArchbirdEngine *engine;
  AbBundleBuilder *builder;
  const AbTreeSitterDescriptor *descriptor;
  const AbManifestFile *file;
  const uint8_t *source;
  size_t source_length;
  void *language_data;
  const char *inapplicable_code;
  const char *inapplicable_message;
  size_t inapplicable_start;
  size_t inapplicable_end;
  int has_inapplicable_span;
  size_t error_count;
  size_t missing_count;
  size_t first_error_start;
  size_t first_error_end;
  size_t first_missing_start;
  size_t first_missing_end;
  AbTreeSitterRecovery *recoveries;
  size_t recovery_count;
  size_t recovery_capacity;
  AbTreeSitterFactRegion *fact_regions;
  size_t fact_region_count;
  size_t fact_region_capacity;
};

int ab_tree_sitter_node_type(TSNode node, const char *type);
int ab_tree_sitter_node_slice(const AbTreeSitterScan *scan, TSNode node,
                              size_t *out_start, size_t *out_end);
int ab_tree_sitter_node_has_text(const AbTreeSitterScan *scan, TSNode node);
TSNode ab_tree_sitter_child(TSNode node, const char *field);
ArchbirdStatus ab_tree_sitter_add_line(AbTreeSitterScan *scan, AbFact *fact,
                                       TSNode node);
ArchbirdStatus
ab_tree_sitter_add_named_fact(AbTreeSitterScan *scan, const char *domain,
                              const char *kind, TSNode node, const uint8_t *key,
                              size_t key_length, AbFact **out_fact);
ArchbirdStatus ab_tree_sitter_add_node_fact(AbTreeSitterScan *scan,
                                            const char *domain,
                                            const char *kind, TSNode node,
                                            AbFact **out_fact);
ArchbirdStatus ab_tree_sitter_add_quoted_fact(AbTreeSitterScan *scan,
                                              const char *domain,
                                              const char *kind, TSNode node,
                                              AbFact **out_fact);
ArchbirdStatus
ab_tree_sitter_add_qualified_fact(AbTreeSitterScan *scan, const char *domain,
                                  const char *kind, TSNode owner, TSNode name,
                                  const char *const *container_types,
                                  size_t container_count, AbFact **out_fact);
ArchbirdStatus ab_tree_sitter_add_enclosing(AbTreeSitterScan *scan,
                                            AbFact *fact,
                                            const AbTreeSitterFrame *frame);
void ab_tree_sitter_set_enclosing_fact(AbTreeSitterFrame *frame,
                                       const AbFact *fact);
ArchbirdStatus ab_tree_sitter_track_fact_region(AbTreeSitterScan *scan,
                                                const AbFact *fact,
                                                TSNode region);

ArchbirdStatus ab_tree_sitter_scan_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32],
    const AbTreeSitterDescriptor *descriptor, AbProviderBundle *out_bundle);

#endif
