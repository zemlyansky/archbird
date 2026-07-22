#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "projection_internal.h"
#include "projection_model.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static int stable_id(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
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
  TRY(ab_buffer_literal(buffer, ",\"unknown\":"));
  TRY(render_nullable(buffer, selection->unknown, selection->has_unknown));
  TRY(ab_buffer_literal(buffer, ",\"universe\":"));
  TRY(render_nullable(buffer, selection->universe, selection->has_universe));
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
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
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

static ArchbirdStatus render_projection_base(
    AbBuffer *buffer, const AbString *id, const AbValue *definition,
    const char definition_sha256[65], const AbValue *map,
    const AbValue *resolution, const AbProjectionData *fact) {
  TRY(ab_buffer_literal(
      buffer, "{\"artifact\":\"projection-result\",\"completeness\":"));
  TRY(ab_projection_completeness_render(buffer, fact));
  TRY(ab_buffer_literal(buffer, ",\"definition\":"));
  TRY(ab_value_render(buffer, definition));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, definition_sha256, 64));
  TRY(ab_buffer_literal(buffer, ",\"evaluation\":"));
  TRY(render_evaluation(buffer, map, resolution));
  TRY(ab_buffer_literal(buffer, ",\"fact\":"));
  TRY(ab_projection_data_render(buffer, fact, 1));
  TRY(ab_buffer_literal(buffer, ",\"id\":"));
  TRY(ab_buffer_json_string(buffer, id->data, id->length));
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
  AbBuffer base;
  ArchbirdStatus status;
  if (!engine || !plan || !plan->id.length ||
      plan->definition.kind != AB_VALUE_OBJECT || !map ||
      map->kind != AB_VALUE_OBJECT || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_projection_resolution_validate(engine, resolution, map,
                                             "projection input");
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&base, engine);
  status = ab_projection_extract_map(engine, map, resolution, plan, &out->data);
  if (status == ARCHBIRD_OK)
    status = render_projection_result_identity(&base, plan->definition_sha256,
                                               map, resolution, &out->data);
  if (status == ARCHBIRD_OK)
    status = digest_buffer(&base, out->result_sha256);
  ab_buffer_free(&base);
  if (status != ARCHBIRD_OK)
    ab_projection_result_free(engine, out);
  return status;
}

void ab_projection_result_free(ArchbirdEngine *engine,
                               AbProjectionResult *result) {
  if (!result)
    return;
  ab_projection_data_free(engine, &result->data);
  memset(result, 0, sizeof(*result));
}

ArchbirdStatus archbird_projection_evaluate(
    ArchbirdEngine *engine, const uint8_t *map_json, size_t map_length,
    const uint8_t *resolution_json, size_t resolution_length,
    const uint8_t *projection_json, size_t projection_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue definition = {0};
  AbProjectionPlan plan = {0};
  AbProjectionResult result = {0};
  AbBuffer base;
  AbBuffer full;
  const AbValue *id_value;
  const AbString *id = NULL;
  ArchbirdStatus status;
  if (!engine || !map_json || !map_length ||
      (!resolution_json && resolution_length) || !projection_json ||
      !projection_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&base, engine);
  ab_buffer_init(&full, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, projection_json, projection_length,
                                  &definition);
  if (status == ARCHBIRD_OK && definition.kind != AB_VALUE_OBJECT)
    status = invalid(engine, "definition must be an object");
  id_value = status == ARCHBIRD_OK ? ab_value_member(&definition, "id") : NULL;
  if (id_value && id_value->kind == AB_VALUE_STRING)
    id = &id_value->as.text;
  if (status == ARCHBIRD_OK && (!id || !stable_id(id)))
    status = invalid(engine, "id must be a stable non-empty identifier");
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_compile(engine, &definition, id, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &plan, &map, resolution_length ? &resolution : NULL, &result);
  if (status == ARCHBIRD_OK)
    status = render_projection_base(
        &base, &plan.id, &plan.definition, plan.definition_sha256, &map,
        resolution_length ? &resolution : NULL, &result.data);
  if (status == ARCHBIRD_OK) {
    if (!base.length || base.data[base.length - 1] != '}')
      status = ARCHBIRD_CONFLICT;
    else {
      status = ab_buffer_append(&full, base.data, base.length - 1);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&full, ",\"projection_result_sha256\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(&full, result.result_sha256, 64);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&full, "}");
    }
  }
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, full.data, full.length,
                                        json_flags, write_fn, user_data);
  ab_projection_result_free(engine, &result);
  ab_projection_plan_free(engine, &plan);
  ab_value_free(engine, &definition);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_buffer_free(&full);
  ab_buffer_free(&base);
  return status;
}

#undef TRY
