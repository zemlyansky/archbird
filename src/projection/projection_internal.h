#ifndef ARCHBIRD_PROJECTION_INTERNAL_H
#define ARCHBIRD_PROJECTION_INTERNAL_H

#include "base/json_value.h"
#include "projection/projection_model.h"

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
  const AbValue *files;
  size_t *file_slots;
  size_t file_slot_count;
  const AbValue *inputs;
  size_t *input_slots;
  size_t input_slot_count;
} AbProjectionContext;

typedef enum AbProjectionKind {
  AB_PROJECTION_KIND_ARTIFACT_ROUTES = 0,
  AB_PROJECTION_KIND_BUILD_ROUTES,
  AB_PROJECTION_KIND_COMPONENT_EDGES,
  AB_PROJECTION_KIND_COMPONENT_MEMBERSHIP,
  AB_PROJECTION_KIND_COMPONENTS,
  AB_PROJECTION_KIND_CONSTANT_MEMBERSHIPS,
  AB_PROJECTION_KIND_CONSTANT_VALUES,
  AB_PROJECTION_KIND_FILE_EDGES,
  AB_PROJECTION_KIND_FILE_METRICS,
  AB_PROJECTION_KIND_GRAPH,
  AB_PROJECTION_KIND_INVENTORY_PATHS,
  AB_PROJECTION_KIND_MACRO_MEMBERS,
  AB_PROJECTION_KIND_MAPPED_PATHS,
  AB_PROJECTION_KIND_PACKAGE_ENTRYPOINTS,
  AB_PROJECTION_KIND_PACKAGE_EXPORTS,
  AB_PROJECTION_KIND_PROVIDER_SURFACE,
  AB_PROJECTION_KIND_SEARCH_DOMAIN,
  AB_PROJECTION_KIND_SYMBOL_ENTITIES,
  AB_PROJECTION_KIND_SYMBOL_OCCURRENCES,
  AB_PROJECTION_KIND_SYMBOL_RELATIONS,
  AB_PROJECTION_KIND_SYMBOLS,
  AB_PROJECTION_KIND_TEST_ROUTES,
  AB_PROJECTION_KIND_TEST_SELECTORS,
  AB_PROJECTION_KIND_COUNT
} AbProjectionKind;

typedef ArchbirdStatus (*AbProjectionExtractor)(AbProjectionContext *context,
                                                const AbProjectionPlan *plan,
                                                AbProjectionData *out);

#define AB_PROJECTION_FIELD_ARTIFACTS (UINT64_C(1) << 0)
#define AB_PROJECTION_FIELD_BUILDS (UINT64_C(1) << 1)
#define AB_PROJECTION_FIELD_CALL (UINT64_C(1) << 2)
#define AB_PROJECTION_FIELD_COMPONENTS (UINT64_C(1) << 3)
#define AB_PROJECTION_FIELD_CONFIGURED_ONLY (UINT64_C(1) << 4)
#define AB_PROJECTION_FIELD_CONTAINER (UINT64_C(1) << 5)
#define AB_PROJECTION_FIELD_EXCLUDE (UINT64_C(1) << 6)
#define AB_PROJECTION_FIELD_FROM_PATHS (UINT64_C(1) << 7)
#define AB_PROJECTION_FIELD_GROUP (UINT64_C(1) << 8)
#define AB_PROJECTION_FIELD_GROUP_BY (UINT64_C(1) << 9)
#define AB_PROJECTION_FIELD_INCLUDE (UINT64_C(1) << 10)
#define AB_PROJECTION_FIELD_KIND_PATTERNS (UINT64_C(1) << 11)
#define AB_PROJECTION_FIELD_KINDS (UINT64_C(1) << 12)
#define AB_PROJECTION_FIELD_LAYER (UINT64_C(1) << 13)
#define AB_PROJECTION_FIELD_LEVEL (UINT64_C(1) << 14)
#define AB_PROJECTION_FIELD_METRIC (UINT64_C(1) << 15)
#define AB_PROJECTION_FIELD_NAME (UINT64_C(1) << 16)
#define AB_PROJECTION_FIELD_NAME_PATTERNS (UINT64_C(1) << 17)
#define AB_PROJECTION_FIELD_NAMES (UINT64_C(1) << 18)
#define AB_PROJECTION_FIELD_OVERLAYS (UINT64_C(1) << 19)
#define AB_PROJECTION_FIELD_PACKAGES (UINT64_C(1) << 20)
#define AB_PROJECTION_FIELD_PATHS (UINT64_C(1) << 21)
#define AB_PROJECTION_FIELD_PUBLIC_ONLY (UINT64_C(1) << 22)
#define AB_PROJECTION_FIELD_RELATIONS (UINT64_C(1) << 23)
#define AB_PROJECTION_FIELD_RESOLUTIONS (UINT64_C(1) << 24)
#define AB_PROJECTION_FIELD_ROUTES (UINT64_C(1) << 25)
#define AB_PROJECTION_FIELD_SELECTOR (UINT64_C(1) << 26)
#define AB_PROJECTION_FIELD_SELECTOR_ARGUMENT (UINT64_C(1) << 27)
#define AB_PROJECTION_FIELD_SELECTORS (UINT64_C(1) << 28)
#define AB_PROJECTION_FIELD_STRIP_PREFIX (UINT64_C(1) << 29)
#define AB_PROJECTION_FIELD_STRIP_SUFFIX (UINT64_C(1) << 30)
#define AB_PROJECTION_FIELD_TARGET_PATHS (UINT64_C(1) << 31)
#define AB_PROJECTION_FIELD_TO_PATHS (UINT64_C(1) << 32)
#define AB_PROJECTION_FIELD_VALUES_FROM_ARGUMENT (UINT64_C(1) << 33)

#define AB_PROJECTION_FIELDS_NORMALIZED                                        \
  (AB_PROJECTION_FIELD_EXCLUDE | AB_PROJECTION_FIELD_INCLUDE |                 \
   AB_PROJECTION_FIELD_STRIP_PREFIX | AB_PROJECTION_FIELD_STRIP_SUFFIX)

typedef struct AbProjectionDescriptor {
  AbProjectionKind kind;
  const char *name;
  const char *shape;
  uint64_t allowed_fields;
  uint64_t required_fields;
  const char *required_message;
  AbProjectionExtractor extract;
} AbProjectionDescriptor;

const AbProjectionDescriptor *
ab_projection_descriptor_find(const AbString *name);

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
ArchbirdStatus ab_projection_extract_graph(AbProjectionContext *context,
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
