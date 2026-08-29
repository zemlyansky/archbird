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
#endif
