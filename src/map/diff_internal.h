#ifndef ARCHBIRD_MAP_DIFF_INTERNAL_H
#define ARCHBIRD_MAP_DIFF_INTERNAL_H

#include "base/json_value.h"

typedef struct AbDiffContext {
  ArchbirdEngine *engine;
} AbDiffContext;

ArchbirdStatus ab_diff_map_validate(AbDiffContext *context, const AbValue *map);
ArchbirdStatus ab_diff_map_identity_render(AbDiffContext *context,
                                           AbBuffer *buffer,
                                           const AbValue *map);
ArchbirdStatus ab_diff_map_select(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t length, const char *const *fields,
                                  size_t field_count, AbValue *out);
ArchbirdStatus ab_diff_map_decode(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t length, AbValue *out);
ArchbirdStatus ab_projects_map_diff(ArchbirdEngine *engine,
                                    const ArchbirdProject *before_project,
                                    const ArchbirdProject *after_project,
                                    uint32_t json_flags,
                                    ArchbirdWriteFn write_fn, void *user_data);
#endif
