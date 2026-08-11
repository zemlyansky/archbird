#ifndef ARCHBIRD_PROJECT_SOURCE_STORE_H
#define ARCHBIRD_PROJECT_SOURCE_STORE_H

#include "evidence/project/project_model.h"

ArchbirdStatus ab_project_source_store_init(ArchbirdEngine *engine,
                                            const uint8_t *manifest_json,
                                            size_t manifest_length,
                                            AbProjectSourceStore *store);

void ab_project_source_store_destroy(ArchbirdEngine *engine,
                                     AbProjectSourceStore *store);

AbManifestFile *ab_project_source_store_find(AbProjectSourceStore *store,
                                             const char *path,
                                             size_t path_length,
                                             size_t *out_index);

#endif
