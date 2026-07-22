#ifndef ARCHBIRD_QUERY_INTERNAL_H
#define ARCHBIRD_QUERY_INTERNAL_H

#include "archbird_internal.h"
#include "json_value.h"
#include "projection_internal.h"
#include "render_internal.h"

typedef struct AbQueryProjection {
  AbProjectionPlan plan;
  AbProjectionResult result;
} AbQueryProjection;

typedef struct AbQueryProjectionSet {
  AbQueryProjection *items;
  size_t count;
} AbQueryProjectionSet;

ArchbirdStatus ab_query_plan_compile_definition(
    ArchbirdEngine *engine, const AbString *id, const AbValue *definition,
    const AbValue *overrides, const AbValue *configured_projections,
    const char *map_config_sha256, const char *project_configuration_sha256,
    AbValue *out_plan);

ArchbirdStatus ab_query_plan_compile_ad_hoc(ArchbirdEngine *engine,
                                            const AbValue *request,
                                            AbValue *out_plan);

ArchbirdStatus ab_query_projections_evaluate(ArchbirdEngine *engine,
                                             const AbValue *serialized_plans,
                                             const AbValue *map,
                                             const AbValue *resolution,
                                             AbQueryProjectionSet *out);
void ab_query_projection_set_free(ArchbirdEngine *engine,
                                  AbQueryProjectionSet *set);
ArchbirdStatus
ab_query_projection_identities_render(AbBuffer *buffer,
                                      const AbQueryProjectionSet *set);

ArchbirdStatus ab_query_execute_value(ArchbirdEngine *engine,
                                      const AbValue *map,
                                      const AbValue *resolution,
                                      const AbValue *request, AbBuffer *out);

#endif
