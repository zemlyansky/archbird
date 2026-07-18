#ifndef ARCHBIRD_PROJECT_INTERNAL_H
#define ARCHBIRD_PROJECT_INTERNAL_H

#include "config.h"
#include "model.h"

const AbSourceManifest *ab_project_manifest(const ArchbirdProject *project);

const uint8_t *ab_project_source_bytes(const ArchbirdProject *project,
                                       size_t index);

const uint8_t *ab_project_manifest_sha256_bytes(const ArchbirdProject *project);

size_t ab_project_test_observation_count(const ArchbirdProject *project);
const AbValue *ab_project_test_observation(const ArchbirdProject *project,
                                           size_t index);

const AbMapConfig *ab_project_config(const ArchbirdProject *project);

ArchbirdStatus ab_project_take_provider_bundle(ArchbirdEngine *engine,
                                               ArchbirdProject *project,
                                               ArchbirdProviderMode mode,
                                               AbProviderBundle *bundle);

ArchbirdStatus ab_project_take_provider_bundles(ArchbirdEngine *engine,
                                                ArchbirdProject *project,
                                                ArchbirdProviderMode mode,
                                                AbProviderBundle *bundles,
                                                size_t bundle_count);

int ab_project_providers_finalized(const ArchbirdProject *project);

size_t ab_project_merged_fact_count(const ArchbirdProject *project);

const AbFact *ab_project_merged_fact(const ArchbirdProject *project,
                                     size_t index);

const AbProviderBundle *
ab_project_merged_fact_provider(const ArchbirdProject *project, size_t index);

void ab_project_merged_fact_range(const ArchbirdProject *project,
                                  const AbString *path, const char *domain,
                                  size_t *out_start, size_t *out_end);

const AbFact *ab_project_merged_fact_by_path(const ArchbirdProject *project,
                                             size_t index);

const AbProviderBundle *
ab_project_provider_bundle(const ArchbirdProject *project, size_t index);

#endif
