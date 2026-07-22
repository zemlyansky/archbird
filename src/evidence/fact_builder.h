#ifndef ARCHBIRD_FACT_BUILDER_H
#define ARCHBIRD_FACT_BUILDER_H

#include "model.h"

typedef struct AbBundleBuilder {
  ArchbirdEngine *engine;
  AbProviderBundle bundle;
  size_t capability_capacity;
  size_t fact_capacity;
} AbBundleBuilder;

ArchbirdStatus ab_bundle_builder_init_file(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]);

ArchbirdStatus ab_bundle_builder_init_file_manifest(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const uint8_t source_manifest_sha256[32], const char *producer_name,
    const char *producer_version, const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]);

ArchbirdStatus ab_bundle_builder_init_file_sources(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const AbManifestFile *file,
    const AbManifestFile *const *inputs, size_t input_count,
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]);

ArchbirdStatus ab_bundle_builder_init_project(
    AbBundleBuilder *builder, ArchbirdEngine *engine,
    const AbSourceManifest *manifest, const uint8_t source_manifest_sha256[32],
    const char *producer_name, const char *producer_version,
    const uint8_t implementation_sha256[32],
    const uint8_t configuration_sha256[32]);

ArchbirdStatus ab_bundle_builder_set_runtime(AbBundleBuilder *builder,
                                             const char *runtime);

void ab_bundle_builder_abort(AbBundleBuilder *builder);

ArchbirdStatus ab_bundle_builder_add_capability(AbBundleBuilder *builder,
                                                const char *domain,
                                                const char *coverage,
                                                const char *claim,
                                                const char *boundary);

ArchbirdStatus
ab_bundle_builder_add_fact(AbBundleBuilder *builder, const char *domain,
                           const char *kind, const char *claim, size_t start,
                           size_t end, const uint8_t *key, size_t key_length,
                           const uint8_t *name, size_t name_length,
                           AbFact **out_fact);

ArchbirdStatus ab_bundle_builder_add_fact_at(
    AbBundleBuilder *builder, const AbManifestFile *file, const char *domain,
    const char *kind, const char *claim, size_t start, size_t end,
    const uint8_t *key, size_t key_length, const uint8_t *name,
    size_t name_length, AbFact **out_fact);

/* Built-in scanners emit source occurrences by default. Aggregated or
   synthetic facts must opt back into key-qualified correlation explicitly. */
void ab_fact_set_keyed_correlation(AbFact *fact);

ArchbirdStatus ab_fact_set_resolution(ArchbirdEngine *engine, AbFact *fact,
                                      const char *state,
                                      const AbString *targets,
                                      size_t target_count, const char *reason);

ArchbirdStatus ab_fact_add_string_attribute(ArchbirdEngine *engine,
                                            AbFact *fact, const char *name,
                                            const uint8_t *value,
                                            size_t value_length);

ArchbirdStatus ab_fact_add_u64_attribute(ArchbirdEngine *engine, AbFact *fact,
                                         const char *name, uint64_t value);

ArchbirdStatus ab_fact_add_i64_attribute(ArchbirdEngine *engine, AbFact *fact,
                                         const char *name, int64_t value);

ArchbirdStatus
ab_bundle_builder_add_diagnostic(AbBundleBuilder *builder, const char *severity,
                                 const char *code, const char *message,
                                 size_t start, size_t end, int has_span);

ArchbirdStatus ab_bundle_builder_finish(AbBundleBuilder *builder,
                                        AbProviderBundle *out_bundle);

#endif
