#ifndef ARCHBIRD_PATCH_SOURCE_H
#define ARCHBIRD_PATCH_SOURCE_H

#include "json_value.h"

#define AB_PATCH_MAX_TRANSITIONS 4096u

ArchbirdStatus ab_patch_source_metadata_load(ArchbirdEngine *engine,
                                             const uint8_t *json, size_t length,
                                             AbValue *out);
const AbValue *ab_patch_source_file(const AbValue *metadata,
                                    const AbString *path);
int ab_patch_source_path_absent(const AbValue *metadata, const AbString *path);

#endif
