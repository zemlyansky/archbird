#include <archbird/archbird.h>

#include "base/archbird_internal.h"
#include "base/artifact_validation.h"
#include "base/json_value.h"
#include "base/render_internal.h"
#include "base/sha256.h"
#include "projection/projection_internal.h"
#include "projection/projection_model.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static int stable_id(const AbString *value) {
  return ab_artifact_stable_id_string(value);
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "projection: %s", message);
}

static ArchbirdStatus render_nullable(AbBuffer *buffer, uint64_t value,
                                      int present) {
  return present ? ab_buffer_u64(buffer, value)
                 : ab_buffer_literal(buffer, "null");
}

ArchbirdStatus ab_projection_completeness_render(AbBuffer *buffer,
                                                 const AbProjectionData *fact) {
  const AbProjectionCompleteness *selection = &fact->selection;
  const char *classification = ab_projection_data_classification(fact);
  TRY(ab_buffer_literal(buffer, "{\"classification\":"));
  TRY(ab_buffer_json_string(buffer, classification, strlen(classification)));
  TRY(ab_buffer_literal(buffer, ",\"counts\":{\"evaluated\":"));
  TRY(render_nullable(buffer, selection->evaluated, selection->has_evaluated));
  TRY(ab_buffer_literal(buffer, ",\"excluded\":"));
  TRY(render_nullable(buffer, selection->excluded, selection->has_excluded));
  TRY(ab_buffer_literal(buffer, ",\"selected\":"));
  TRY(render_nullable(buffer, selection->selected, selection->has_selected));
  TRY(ab_buffer_literal(buffer, ",\"universe\":"));
  TRY(render_nullable(buffer, selection->universe, selection->has_universe));
  TRY(ab_buffer_literal(buffer, ",\"unknown\":"));
  TRY(render_nullable(buffer, selection->unknown, selection->has_unknown));
  TRY(ab_buffer_literal(buffer, ",\"unsupported\":"));
  TRY(render_nullable(buffer, selection->unsupported,
                      selection->has_unsupported));
  TRY(ab_buffer_literal(buffer, "},\"exhaustive\":"));
  TRY(ab_buffer_literal(buffer, !strcmp(classification, "complete") ? "true"
                                                                    : "false"));
  TRY(ab_buffer_literal(buffer, ",\"truncated\":"));
  if (selection->has_truncated)
    TRY(ab_buffer_literal(buffer, selection->truncated ? "true" : "false"));
  else
    TRY(ab_buffer_literal(buffer, "null"));
  TRY(ab_buffer_literal(buffer, ",\"unit\":"));
  TRY(ab_buffer_json_string(buffer, selection->unit.data,
                            selection->unit.length));
  return ab_buffer_literal(buffer, "}");
}

static const AbValue *member_path(const AbValue *root, const char *first,
                                  const char *second) {
  const AbValue *value = ab_value_member(root, first);
  return value && value->kind == AB_VALUE_OBJECT
             ? ab_value_member(value, second)
             : NULL;
}

static int lowercase_sha256(const AbValue *value) {
  return ab_artifact_sha256(value);
}

static ArchbirdStatus render_evaluation(AbBuffer *buffer, const AbValue *map,
                                        const AbValue *resolution) {
  const AbValue *config = member_path(map, "evidence", "config_sha256");
  const AbValue *input = member_path(map, "evidence", "input_sha256");
  const AbValue *implementation =
      member_path(map, "tool", "implementation_sha256");
  if (!config || !input)
    return invalid(buffer->engine,
                   "Map is missing canonical evidence identity");
  TRY(ab_buffer_literal(buffer, "{\"map_config_sha256\":"));
  TRY(ab_value_render(buffer, config));
  TRY(ab_buffer_literal(buffer, ",\"map_input_sha256\":"));
  TRY(ab_value_render(buffer, input));
  TRY(ab_buffer_literal(buffer, ",\"map_producer_implementation_sha256\":"));
  if (lowercase_sha256(implementation))
    TRY(ab_value_render(buffer, implementation));
  else
    TRY(ab_buffer_literal(buffer, "null"));
  TRY(ab_buffer_literal(buffer, ",\"resolution_sha256\":"));
  if (resolution) {
    const AbValue *sha = ab_value_member(resolution, "sha256");
    if (!sha || sha->kind != AB_VALUE_STRING)
      return invalid(buffer->engine,
                     "configuration resolution is missing its identity");
    TRY(ab_value_render(buffer, sha));
  } else {
    TRY(ab_buffer_literal(buffer, "null"));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus digest_buffer(const AbBuffer *buffer, char output[65]) {
  uint8_t digest[32];
  ArchbirdStatus status = archbird_sha256(buffer->data, buffer->length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  return status;
}

ArchbirdStatus ab_projection_definition_sha256(ArchbirdEngine *engine,
                                               const AbValue *definition,
                                               char out[65]) {
  AbBuffer canonical;
  ArchbirdStatus status;
  if (!engine || !definition || definition->kind != AB_VALUE_OBJECT || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&canonical, engine);
  status = ab_value_render(&canonical, definition);
  if (status == ARCHBIRD_OK)
    status = digest_buffer(&canonical, out);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus render_projection_result(
    AbBuffer *buffer, const AbString *id, const AbValue *definition,
    const char definition_sha256[65], const AbValue *map,
    const AbValue *resolution, const AbProjectionData *fact,
    const char result_sha256[65]) {
  TRY(ab_buffer_literal(
      buffer, "{\"artifact\":\"projection-result\",\"completeness\":"));
  TRY(ab_projection_completeness_render(buffer, fact));
  TRY(ab_buffer_literal(buffer, ",\"definition\":"));
  TRY(ab_value_render(buffer, definition));
  TRY(ab_buffer_literal(buffer, ",\"evaluation\":"));
  TRY(render_evaluation(buffer, map, resolution));
  TRY(ab_buffer_literal(buffer, ",\"fact\":"));
  TRY(ab_projection_data_render(buffer, fact, 1));
  TRY(ab_buffer_literal(buffer, ",\"id\":"));
  TRY(ab_buffer_json_string(buffer, id->data, id->length));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, definition_sha256, 64));
  TRY(ab_buffer_literal(buffer, ",\"projection_result_sha256\":"));
  TRY(ab_buffer_json_string(buffer, result_sha256, 64));
  return ab_buffer_literal(buffer, ",\"schema_version\":1}");
}

static ArchbirdStatus render_projection_result_identity(
    AbBuffer *buffer, const char definition_sha256[65], const AbValue *map,
    const AbValue *resolution, const AbProjectionData *fact) {
  TRY(ab_buffer_literal(buffer, "{\"completeness\":"));
  TRY(ab_projection_completeness_render(buffer, fact));
  TRY(ab_buffer_literal(buffer, ",\"evaluation\":"));
  TRY(render_evaluation(buffer, map, resolution));
  TRY(ab_buffer_literal(buffer, ",\"fact\":"));
  TRY(ab_projection_data_render_content(buffer, fact));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, definition_sha256, 64));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_projection_plan_evaluate(ArchbirdEngine *engine,
                                           const AbProjectionPlan *plan,
                                           const AbValue *map,
                                           const AbValue *resolution,
                                           AbProjectionResult *out) {
  AbProjectionContext context = {0};
  ArchbirdStatus status =
      ab_projection_context_init(&context, engine, map, resolution);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate_context(&context, plan, out);
  ab_projection_context_free(&context);
  return status;
}

ArchbirdStatus ab_projection_plan_evaluate_context(AbProjectionContext *context,
                                                   const AbProjectionPlan *plan,
                                                   AbProjectionResult *out) {
  AbBuffer base;
  ArchbirdStatus status;
  if (!context || !context->engine || !context->map || !plan ||
      !plan->id.length || plan->definition.kind != AB_VALUE_OBJECT || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  ab_buffer_init(&base, context->engine);
  status = ab_projection_extract_context(context, plan, &out->data);
  if (status == ARCHBIRD_OK)
    status = render_projection_result_identity(&base, plan->definition_sha256,
                                               context->map,
                                               context->resolution, &out->data);
  if (status == ARCHBIRD_OK)
    status = digest_buffer(&base, out->result_sha256);
  ab_buffer_free(&base);
  if (status != ARCHBIRD_OK)
    ab_projection_result_free(context->engine, out);
  return status;
}

void ab_projection_result_free(ArchbirdEngine *engine,
                               AbProjectionResult *result) {
  if (!result)
    return;
  ab_projection_data_free(engine, &result->data);
  memset(result, 0, sizeof(*result));
}

static ArchbirdStatus
evaluate_projection_definition(AbProjectionContext *context,
                               const AbValue *definition, AbBuffer *out) {
  AbProjectionPlan plan = {0};
  AbProjectionResult result = {0};
  const AbValue *id_value;
  const AbString *id = NULL;
  ArchbirdStatus status;
  if (!context || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!definition || definition->kind != AB_VALUE_OBJECT)
    return invalid(context->engine, "definition must be an object");
  id_value = ab_value_member(definition, "id");
  if (id_value && id_value->kind == AB_VALUE_STRING)
    id = &id_value->as.text;
  if (!id || !stable_id(id))
    return invalid(context->engine, "id must be a stable non-empty identifier");
  status = ab_projection_plan_compile(context->engine, definition, id, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate_context(context, &plan, &result);
  if (status == ARCHBIRD_OK)
    status = render_projection_result(
        out, &plan.id, &plan.definition, plan.definition_sha256, context->map,
        context->resolution, &result.data, result.result_sha256);
  ab_projection_result_free(context->engine, &result);
  ab_projection_plan_free(context->engine, &plan);
  return status;
}

ArchbirdStatus archbird_projection_evaluate(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *projection_json, size_t projection_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue definition = {0};
  AbProjectionContext context = {0};
  AbBuffer full;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !projection_json ||
      !projection_length || !write_fn || !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&full, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, projection_json, projection_length,
                                  &definition);
  if (status == ARCHBIRD_OK)
    status = ab_projection_context_init(&context, engine, &map,
                                        resolution_length ? &resolution : NULL);
  if (status == ARCHBIRD_OK)
    status = evaluate_projection_definition(&context, &definition, &full);
  if (status == ARCHBIRD_OK && json_flags)
    status = archbird_json_canonicalize(engine, full.data, full.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, full.data, full.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "projection output writer failed");
  ab_projection_context_free(&context);
  ab_value_free(engine, &definition);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&full);
  return status;
}

ArchbirdStatus archbird_projections_evaluate(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *projections_json, size_t projections_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue definitions = {0};
  AbProjectionContext context = {0};
  AbBuffer full;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !projections_json ||
      !projections_length || !write_fn || !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&full, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, projections_json, projections_length,
                                  &definitions);
  if (status == ARCHBIRD_OK && definitions.kind != AB_VALUE_ARRAY)
    status = invalid(engine, "definitions must be an array");
  if (status == ARCHBIRD_OK)
    status = ab_projection_context_init(&context, engine, &map,
                                        resolution_length ? &resolution : NULL);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full, "[");
  for (index = 0; status == ARCHBIRD_OK && index < definitions.as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(&full, ",");
    if (status == ARCHBIRD_OK)
      status = evaluate_projection_definition(
          &context, &definitions.as.array.items[index], &full);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&full, "]");
  if (status == ARCHBIRD_OK && json_flags)
    status = archbird_json_canonicalize(engine, full.data, full.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, full.data, full.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "projection output writer failed");
  ab_projection_context_free(&context);
  ab_value_free(engine, &definitions);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&full);
  return status;
}

#undef TRY
