#ifndef ARCHBIRD_PROJECTION_INTERNAL_H
#define ARCHBIRD_PROJECTION_INTERNAL_H

#include "json_value.h"
#include "projection_model.h"

typedef struct AbProjectionPlan {
  AbString id;
  AbValue definition;
  char definition_sha256[65];
} AbProjectionPlan;

typedef struct AbProjectionResult {
  AbProjectionData data;
  char result_sha256[65];
} AbProjectionResult;

typedef struct AbProjectionContext {
  ArchbirdEngine *engine;
  const AbValue *map;
  const AbValue *resolution;
} AbProjectionContext;

ArchbirdStatus ab_projection_map_validate(ArchbirdEngine *engine,
                                          const AbValue *map,
                                          const char *where);
ArchbirdStatus ab_projection_resolution_validate(ArchbirdEngine *engine,
                                                 const AbValue *resolution,
                                                 const AbValue *map,
                                                 const char *where);

ArchbirdStatus ab_projection_definition_sha256(ArchbirdEngine *engine,
                                               const AbValue *definition,
                                               char out[65]);
ArchbirdStatus ab_projection_plan_compile(ArchbirdEngine *engine,
                                          const AbValue *definition,
                                          const AbString *id,
                                          AbProjectionPlan *out);
void ab_projection_plan_free(ArchbirdEngine *engine, AbProjectionPlan *plan);
ArchbirdStatus ab_projection_extract_map(ArchbirdEngine *engine,
                                         const AbValue *map,
                                         const AbValue *resolution,
                                         const AbProjectionPlan *plan,
                                         AbProjectionData *out);
ArchbirdStatus ab_projection_extract_literal(ArchbirdEngine *engine,
                                             const AbObjectField *operand,
                                             AbProjectionData *out);
ArchbirdStatus ab_projection_normalized_name(ArchbirdEngine *engine,
                                             const AbValue *spec,
                                             const AbString *raw, AbString *out,
                                             int *selected);
ArchbirdStatus ab_projection_plan_evaluate(ArchbirdEngine *engine,
                                           const AbProjectionPlan *plan,
                                           const AbValue *map,
                                           const AbValue *resolution,
                                           AbProjectionResult *out);
void ab_projection_result_free(ArchbirdEngine *engine,
                               AbProjectionResult *result);

#endif
