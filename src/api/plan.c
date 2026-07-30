#include "archbird/archbird.h"

#include "plan_internal.h"

ArchbirdStatus archbird_plan_validate(ArchbirdEngine *engine,
                                      const uint8_t *plan_json,
                                      size_t plan_length) {
  AbPlan plan = {0};
  ArchbirdStatus status;
  if (!engine || !plan_json || !plan_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  ab_plan_free(engine, &plan);
  return status;
}
