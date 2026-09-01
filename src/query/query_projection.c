#include "query/query_internal.h"

#include "base/artifact_validation.h"
#include "projection/projection_internal.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "query projections: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int field_allowed(const AbString *name, const char *const *allowed,
                         size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_is(name, allowed[index]))
      return 1;
  return 0;
}

static int sha256_value(const AbValue *value) {
  return ab_artifact_sha256(value);
}

ArchbirdStatus ab_query_projections_evaluate(ArchbirdEngine *engine,
                                             const AbValue *serialized_plans,
                                             const AbValue *map,
                                             const AbValue *resolution,
                                             AbQueryProjectionSet *out) {
  static const char *const allowed[] = {"id", "operation",
                                        "projection_definition_sha256"};
  size_t count;
  size_t index;
  AbProjectionContext context = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !serialized_plans ||
      serialized_plans->kind != AB_VALUE_ARRAY || !map || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  count = serialized_plans->as.array.count;
  if (!count)
    return ARCHBIRD_OK;
  if (count > SIZE_MAX / sizeof(*out->items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->items =
      (AbQueryProjection *)ab_calloc(engine, count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory evaluating query projections");
  out->count = count;
  status = ab_projection_context_init(&context, engine, map, resolution);
  for (index = 0; status == ARCHBIRD_OK && index < out->count; index++) {
    const AbValue *row = &serialized_plans->as.array.items[index];
    const AbValue *definition;
    const AbValue *id;
    const AbValue *expected_sha;
    size_t field;
    if (row->kind != AB_VALUE_OBJECT) {
      status = invalid(engine, "plan row must be an object");
      break;
    }
    for (field = 0; field < row->as.object.count; field++)
      if (!field_allowed(&row->as.object.fields[field].name, allowed,
                         sizeof(allowed) / sizeof(allowed[0]))) {
        status = invalid(engine, "plan row contains an unknown field");
        break;
      }
    if (status != ARCHBIRD_OK)
      break;
    definition = ab_value_member(row, "operation");
    id = ab_value_member(row, "id");
    expected_sha = ab_value_member(row, "projection_definition_sha256");
    if (!definition || definition->kind != AB_VALUE_OBJECT || !id ||
        id->kind != AB_VALUE_STRING || !sha256_value(expected_sha)) {
      status = invalid(engine, "plan row identity is invalid");
      break;
    }
    status = ab_projection_plan_compile(engine, definition, &id->as.text,
                                        &out->items[index].plan);
    if (status == ARCHBIRD_OK &&
        memcmp(out->items[index].plan.definition_sha256,
               expected_sha->as.text.data, 64) != 0)
      status = invalid(engine, "projection definition digest is stale");
    if (status == ARCHBIRD_OK)
      status = ab_projection_plan_evaluate_context(
          &context, &out->items[index].plan, &out->items[index].result);
  }
  ab_projection_context_free(&context);
  if (status != ARCHBIRD_OK)
    ab_query_projection_set_free(engine, out);
  return status;
}

void ab_query_projection_set_free(ArchbirdEngine *engine,
                                  AbQueryProjectionSet *set) {
  size_t index;
  if (!set)
    return;
  for (index = 0; index < set->count; index++) {
    ab_projection_result_free(engine, &set->items[index].result);
    ab_projection_plan_free(engine, &set->items[index].plan);
  }
  ab_free(engine, set->items);
  memset(set, 0, sizeof(*set));
}

ArchbirdStatus
ab_query_projection_identities_render(AbBuffer *buffer,
                                      const AbQueryProjectionSet *set) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < set->count; index++) {
    const AbQueryProjection *projection = &set->items[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"completeness\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_completeness_render(buffer, &projection->result.data);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"id\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, projection->plan.id.data,
                                     projection->plan.id.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"projection_definition_sha256\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, projection->plan.definition_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"projection_result_sha256\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, projection->result.result_sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"message\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, projection->result.data.message.data,
                                projection->result.data.message.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"state\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, projection->result.data.state.data,
                                     projection->result.data.state.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
