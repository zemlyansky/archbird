#ifndef ARCHBIRD_PROJECTION_INTERNAL_H
#define ARCHBIRD_PROJECTION_INTERNAL_H

#include "json_value.h"
#include "verify_runtime.h"

typedef struct AbProjectionEvaluation {
  AbObjectField prepared;
  AbVerifyFactSet fact;
  char definition_sha256[65];
  char result_sha256[65];
} AbProjectionEvaluation;

ArchbirdStatus ab_projection_map_validate(ArchbirdEngine *engine,
                                          const AbValue *map,
                                          const char *where);
ArchbirdStatus ab_projection_resolution_validate(ArchbirdEngine *engine,
                                                 const AbValue *resolution,
                                                 const AbValue *map,
                                                 const char *where);

ArchbirdStatus ab_projection_spec_prepare(ArchbirdEngine *engine,
                                          const AbValue *definition,
                                          const AbValue *map,
                                          const AbString *name,
                                          AbObjectField *out);

ArchbirdStatus ab_projection_definition_sha256(ArchbirdEngine *engine,
                                               const AbValue *definition,
                                               char out[65]);
ArchbirdStatus
ab_projection_evaluate_fact(ArchbirdEngine *engine, const AbValue *definition,
                            const AbValue *map, const AbValue *resolution,
                            const AbString *name, AbProjectionEvaluation *out);
void ab_projection_evaluation_free(ArchbirdEngine *engine,
                                   AbProjectionEvaluation *evaluation);

#endif
