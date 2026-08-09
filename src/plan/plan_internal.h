#ifndef ARCHBIRD_PLAN_INTERNAL_H
#define ARCHBIRD_PLAN_INTERNAL_H

#include "base/json_value.h"

typedef struct AbPlan {
  AbValue document;
  const AbValue *source;
  const AbValue *gates;
  const AbValue *items;
  const AbValue *preserved_constraints;
  const AbValue *unknowns;
  char sha256[65];
} AbPlan;

ArchbirdStatus ab_plan_load(ArchbirdEngine *engine, const uint8_t *json,
                            size_t length, AbPlan *out);
void ab_plan_free(ArchbirdEngine *engine, AbPlan *plan);

#endif
