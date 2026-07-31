#ifndef ARCHBIRD_ACT_SOURCE_H
#define ARCHBIRD_ACT_SOURCE_H

#include "json_value.h"

#define AB_ACT_MAX_TRANSITIONS 4096u

ArchbirdStatus ab_act_source_metadata_load(ArchbirdEngine *engine,
                                           const uint8_t *json, size_t length,
                                           AbValue *out);
const AbValue *ab_act_source_file(const AbValue *metadata,
                                  const AbString *path);
int ab_act_source_path_absent(const AbValue *metadata, const AbString *path);

#endif
