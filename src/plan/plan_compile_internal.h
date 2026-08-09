#ifndef ARCHBIRD_PLAN_COMPILE_INTERNAL_H
#define ARCHBIRD_PLAN_COMPILE_INTERNAL_H

#include "base/json_value.h"
#include "projection/projection_internal.h"
#include "verify/verification_artifact.h"

#define AB_PLAN_COMPILE_MAX_ROWS 4096u

typedef struct AbPlanItemBuilder {
  ArchbirdEngine *engine;
  const AbVerificationArtifact *verification;
  AbBuffer items;
  AbBuffer unknowns;
  const AbString **targeted;
  size_t targeted_count;
  size_t item_count;
  size_t unknown_count;
  int first_item;
  int first_unknown;
} AbPlanItemBuilder;

typedef struct AbPlanItemSpec {
  const AbValue *constraint;
  const AbValue *const *findings;
  size_t finding_count;
  const AbProjectionData *projection_evidence;
  const char *statement;
  const char *provenance;
  const AbBuffer *operation;
  const AbBuffer *projection_deltas;
  int executable;
  const char *const *reasons;
  size_t reason_count;
} AbPlanItemSpec;

typedef struct AbPlanSourceLock {
  const AbValue *map_file;
  const AbValue *sha256;
  ArchbirdSourceView source;
} AbPlanSourceLock;

typedef struct AbPlanFindingGroup {
  const AbValue **rows;
  size_t count;
  const AbValue *representative;
} AbPlanFindingGroup;

typedef struct AbPlanFindingGroups {
  AbPlanFindingGroup *groups;
  const AbValue **rows;
  size_t count;
} AbPlanFindingGroups;

ArchbirdStatus
ab_plan_item_builder_init(AbPlanItemBuilder *builder, ArchbirdEngine *engine,
                          const AbVerificationArtifact *verification);
void ab_plan_item_builder_free(AbPlanItemBuilder *builder);
ArchbirdStatus ab_plan_item_builder_append(AbPlanItemBuilder *builder,
                                           const AbPlanItemSpec *spec);
ArchbirdStatus ab_plan_item_builder_render_items(AbPlanItemBuilder *builder,
                                                 AbBuffer *out);
int ab_plan_item_builder_targeted(const AbPlanItemBuilder *builder,
                                  const AbString *constraint_id);

ArchbirdStatus ab_plan_source_lock(ArchbirdEngine *engine,
                                   const ArchbirdProject *project,
                                   const AbValue *map, const AbString *path,
                                   AbPlanSourceLock *out);

int ab_plan_finding_current(const AbValue *finding);
ArchbirdStatus ab_plan_finding_groups_collect(ArchbirdEngine *engine,
                                              const AbValue *findings,
                                              AbPlanFindingGroups *out);
void ab_plan_finding_groups_free(ArchbirdEngine *engine,
                                 AbPlanFindingGroups *groups);

int ab_plan_renamed_text_equal(const AbString *before, const AbString *old_name,
                               const AbString *new_name,
                               const AbString *current);

ArchbirdStatus ab_plan_compile_symbol_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbVerificationArtifact *verification, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbProjectionData *actual, const AbValue *renames,
    uint8_t *rename_used, int *out_handled);

ArchbirdStatus ab_plan_compile_symbol_residual(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbValue *before_map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition, int *out_handled);

ArchbirdStatus ab_plan_compile_edge_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    AbPlanItemBuilder *builder, const AbValue *constraint,
    const AbValue *definition, const AbProjectionData *actual,
    const AbValue *redirects, uint8_t *redirect_used, int *out_handled);

ArchbirdStatus ab_plan_compile_surface_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbValue *before_map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbValue *renames, uint8_t *rename_used, int *out_handled);

ArchbirdStatus ab_plan_compile_package_constraint(
    ArchbirdEngine *engine, const AbValue *map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbProjectionData *actual, int *out_handled);

ArchbirdStatus ab_plan_compile_test_constraint(
    ArchbirdEngine *engine, const AbValue *map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition, int *out_handled);

#endif
