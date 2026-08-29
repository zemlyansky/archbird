#include <archbird/archbird.h>

/* Compose Map and projection kernels behind the public Diff operation. */

#include "base/archbird_internal.h"
#include "base/comparison.h"
#include "base/json_value.h"
#include "base/render_internal.h"
#include "base/sha256.h"
#include "map/diff_internal.h"
#include "projection/projection_comparison.h"
#include "projection/projection_internal.h"

#include <stdint.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "diff correspondence: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         !memcmp(value->data, literal, length);
}

static ArchbirdStatus request_validate(ArchbirdEngine *engine,
                                       const AbValue *request,
                                       const AbValue **before,
                                       const AbValue **after,
                                       const AbValue **mapping) {
  size_t index;
  if (!request || request->kind != AB_VALUE_OBJECT)
    return invalid(engine, "request must be an object");
  *before = ab_value_member(request, "before");
  *after = ab_value_member(request, "after");
  *mapping = ab_value_member(request, "mapping");
  if (!*before || (*before)->kind != AB_VALUE_OBJECT || !*after ||
      (*after)->kind != AB_VALUE_OBJECT || !*mapping ||
      (*mapping)->kind != AB_VALUE_OBJECT)
    return invalid(engine,
                   "request requires before, after, and mapping objects");
  for (index = 0; index < request->as.object.count; index++) {
    const AbString *name = &request->as.object.fields[index].name;
    if (!string_is(name, "before") && !string_is(name, "after") &&
        !string_is(name, "mapping"))
      return invalid(engine, "request contains an unknown field");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus resolution_decode(ArchbirdEngine *engine,
                                        const uint8_t *json, size_t length,
                                        AbValue *out) {
  if (!length)
    return ARCHBIRD_OK;
  if (!json)
    return ARCHBIRD_INVALID_ARGUMENT;
  return ab_json_value_decode(engine, json, length, out);
}

static ArchbirdStatus plan_compile(ArchbirdEngine *engine,
                                   const AbValue *definition,
                                   AbProjectionPlan *out) {
  const AbValue *id = ab_value_member(definition, "id");
  if (!id || id->kind != AB_VALUE_STRING)
    return invalid(engine, "each projection requires a stable id");
  return ab_projection_plan_compile(engine, definition, &id->as.text, out);
}

static ArchbirdStatus
render_projection_identity(AbBuffer *buffer, const AbProjectionPlan *plan,
                           const AbProjectionResult *result) {
  TRY(ab_buffer_literal(buffer, "{\"completeness\":"));
  TRY(ab_projection_completeness_render(buffer, &result->data));
  TRY(ab_buffer_literal(buffer, ",\"definition\":"));
  TRY(ab_value_render(buffer, &plan->definition));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, plan->definition_sha256, 64));
  TRY(ab_buffer_literal(buffer, ",\"projection_result_sha256\":"));
  TRY(ab_buffer_json_string(buffer, result->result_sha256, 64));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_item_or_null(AbBuffer *buffer,
                                          const AbComparisonItem *item) {
  if (!item)
    return ab_buffer_literal(buffer, "null");
  return ab_projection_item_render(buffer,
                                   (const AbProjectionItem *)item->subject);
}

static const char *group_state(AbComparisonGroupState state) {
  if (state == AB_COMPARISON_GROUP_COMPLETE)
    return "complete";
  if (state == AB_COMPARISON_GROUP_PARTIAL)
    return "partial";
  return "source-missing";
}

static ArchbirdStatus render_group(AbBuffer *buffer,
                                   const AbComparisonResult *comparison,
                                   const AbComparisonGroupResult *group) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{\"before\":"));
  TRY(render_item_or_null(buffer, group->left));
  TRY(ab_buffer_literal(buffer, ",\"source\":"));
  TRY(ab_buffer_json_string(buffer, group->group->left->data,
                            group->group->left->length));
  TRY(ab_buffer_literal(buffer, ",\"state\":"));
  TRY(ab_buffer_json_string(buffer, group_state(group->state),
                            strlen(group_state(group->state))));
  TRY(ab_buffer_literal(buffer, ",\"targets\":["));
  if (group->state == AB_COMPARISON_GROUP_SOURCE_MISSING) {
    for (index = 0; index < group->group->right_count; index++) {
      if (index)
        TRY(ab_buffer_literal(buffer, ","));
      TRY(ab_buffer_literal(buffer, "{\"after\":null,\"key\":"));
      TRY(ab_buffer_json_string(buffer, group->group->rights[index]->data,
                                group->group->rights[index]->length));
      TRY(ab_buffer_literal(buffer, ",\"state\":\"unresolved\"}"));
    }
  } else {
    for (index = 0; index < group->record_count; index++) {
      const AbComparisonRecord *record =
          &comparison->records[group->record_start + index];
      if (index)
        TRY(ab_buffer_literal(buffer, ","));
      TRY(ab_buffer_literal(buffer, "{\"after\":"));
      TRY(render_item_or_null(buffer, record->right));
      TRY(ab_buffer_literal(buffer, ",\"key\":"));
      TRY(ab_buffer_json_string(buffer, record->right_key->data,
                                record->right_key->length));
      TRY(ab_buffer_literal(buffer, ",\"state\":"));
      if (record->kind == AB_COMPARISON_MATCHED)
        TRY(ab_buffer_literal(buffer, "\"matched\""));
      else
        TRY(ab_buffer_literal(buffer, "\"missing\""));
      TRY(ab_buffer_literal(buffer, "}"));
    }
  }
  return ab_buffer_literal(buffer, "]}");
}

static ArchbirdStatus render_unmapped(AbBuffer *buffer,
                                      const AbComparisonResult *comparison,
                                      AbComparisonKind kind) {
  size_t index;
  int emitted = 0;
  TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < comparison->count; index++) {
    const AbComparisonRecord *record = &comparison->records[index];
    if (record->kind != kind)
      continue;
    if (emitted++)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(render_item_or_null(buffer, kind == AB_COMPARISON_LEFT_UNMAPPED
                                        ? record->left
                                        : record->right));
  }
  return ab_buffer_literal(buffer, "]");
}

static void comparison_counts(const AbComparisonResult *comparison,
                              size_t counts[5]) {
  size_t index;
  memset(counts, 0, sizeof(size_t) * 5);
  for (index = 0; index < comparison->count; index++) {
    switch (comparison->records[index].kind) {
    case AB_COMPARISON_MATCHED:
      counts[0]++;
      break;
    case AB_COMPARISON_LEFT_UNMAPPED:
      counts[1]++;
      break;
    case AB_COMPARISON_RIGHT_UNMAPPED:
      counts[2]++;
      break;
    case AB_COMPARISON_MAPPING_LEFT_MISSING:
      counts[3]++;
      break;
    case AB_COMPARISON_MAPPING_RIGHT_MISSING:
      counts[4]++;
      break;
    default:
      break;
    }
  }
}

static ArchbirdStatus render_comparison(AbBuffer *buffer,
                                        const AbComparisonResult *comparison) {
  size_t counts[5];
  size_t index;
  comparison_counts(comparison, counts);
  TRY(ab_buffer_literal(buffer, "{\"after_unmapped\":"));
  TRY(render_unmapped(buffer, comparison, AB_COMPARISON_RIGHT_UNMAPPED));
  TRY(ab_buffer_literal(buffer, ",\"before_unmapped\":"));
  TRY(render_unmapped(buffer, comparison, AB_COMPARISON_LEFT_UNMAPPED));
  TRY(ab_buffer_literal(buffer, ",\"counts\":{\"after_unmapped\":"));
  TRY(ab_buffer_u64(buffer, (uint64_t)counts[2]));
  TRY(ab_buffer_literal(buffer, ",\"before_unmapped\":"));
  TRY(ab_buffer_u64(buffer, (uint64_t)counts[1]));
  TRY(ab_buffer_literal(buffer, ",\"mapping_after_missing\":"));
  TRY(ab_buffer_u64(buffer, (uint64_t)counts[4]));
  TRY(ab_buffer_literal(buffer, ",\"mapping_before_missing\":"));
  TRY(ab_buffer_u64(buffer, (uint64_t)counts[3]));
  TRY(ab_buffer_literal(buffer, ",\"matched\":"));
  TRY(ab_buffer_u64(buffer, (uint64_t)counts[0]));
  TRY(ab_buffer_literal(buffer, "},\"groups\":["));
  if (!comparison->incomplete)
    for (index = 0; index < comparison->group_count; index++) {
      if (index)
        TRY(ab_buffer_literal(buffer, ","));
      TRY(render_group(buffer, comparison, &comparison->groups[index]));
    }
  TRY(ab_buffer_literal(buffer, "],\"state\":"));
  if (comparison->incomplete)
    TRY(ab_buffer_literal(buffer, "\"unknown\""));
  else
    TRY(ab_buffer_literal(buffer, "\"complete\""));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus digest_value(ArchbirdEngine *engine, const AbValue *value,
                                   char output[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus
ab_projection_diff(ArchbirdEngine *engine, const uint8_t *before_json,
                   size_t before_length, const uint8_t *before_resolution_json,
                   size_t before_resolution_length, const uint8_t *after_json,
                   size_t after_length, const uint8_t *after_resolution_json,
                   size_t after_resolution_length, const uint8_t *request_json,
                   size_t request_length, uint32_t json_flags,
                   ArchbirdWriteFn write_fn, void *user_data) {
  AbValue before = {0};
  AbValue after = {0};
  AbValue before_resolution = {0};
  AbValue after_resolution = {0};
  AbValue request = {0};
  const AbValue *before_definition = NULL;
  const AbValue *after_definition = NULL;
  const AbValue *mapping = NULL;
  AbProjectionPlan before_plan = {0};
  AbProjectionPlan after_plan = {0};
  AbProjectionResult before_result = {0};
  AbProjectionResult after_result = {0};
  AbProjectionComparisonInput input = {0};
  AbComparisonPlan comparison_plan = {0};
  AbComparisonResult comparison = {0};
  AbDiffContext context = {engine};
  AbBuffer buffer;
  char correspondence_sha256[65] = {0};
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_json_value_decode(engine, before_json, before_length, &before);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, after_json, after_length, &after);
  if (status == ARCHBIRD_OK)
    status = resolution_decode(engine, before_resolution_json,
                               before_resolution_length, &before_resolution);
  if (status == ARCHBIRD_OK)
    status = resolution_decode(engine, after_resolution_json,
                               after_resolution_length, &after_resolution);
  if (status == ARCHBIRD_OK)
    status =
        ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_validate(&context, &before);
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_validate(&context, &after);
  if (status == ARCHBIRD_OK)
    status = request_validate(engine, &request, &before_definition,
                              &after_definition, &mapping);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, before_definition, &before_plan);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, after_definition, &after_plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &before_plan, &before,
        before_resolution_length ? &before_resolution : NULL, &before_result);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &after_plan, &after,
        after_resolution_length ? &after_resolution : NULL, &after_result);
  if (status == ARCHBIRD_OK)
    status = ab_projection_comparison_prepare(engine, &before_result.data,
                                              &after_result.data, mapping, 1, 1,
                                              &input, &comparison_plan);
  if (status == ARCHBIRD_OK)
    status = ab_comparison_run(engine, &comparison_plan, &comparison);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, mapping, correspondence_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "{\"after\":{\"map\":");
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_identity_render(&context, &buffer, &after);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = render_projection_identity(&buffer, &after_plan, &after_result);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer,
                               "},\"artifact\":\"diff\",\"before\":{\"map\":");
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_identity_render(&context, &buffer, &before);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = render_projection_identity(&buffer, &before_plan, &before_result);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "},\"comparison\":");
  if (status == ARCHBIRD_OK)
    status = render_comparison(&buffer, &comparison);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"correspondence_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, correspondence_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &buffer,
        ",\"join\":\"correspondence\",\"schema_version\":8,\"tool\":{"
        "\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  ab_comparison_result_free(engine, &comparison);
  ab_projection_comparison_free(engine, &input);
  ab_projection_result_free(engine, &after_result);
  ab_projection_result_free(engine, &before_result);
  ab_projection_plan_free(engine, &after_plan);
  ab_projection_plan_free(engine, &before_plan);
  ab_value_free(engine, &request);
  ab_value_free(engine, &after_resolution);
  ab_value_free(engine, &before_resolution);
  ab_value_free(engine, &after);
  ab_value_free(engine, &before);
  return status;
}

ArchbirdStatus
archbird_diff(ArchbirdEngine *engine, const uint8_t *before_json,
              size_t before_length, const uint8_t *before_resolution_json,
              size_t before_resolution_length, const uint8_t *after_json,
              size_t after_length, const uint8_t *after_resolution_json,
              size_t after_resolution_length, const uint8_t *request_json,
              size_t request_length, uint32_t json_flags,
              ArchbirdWriteFn write_fn, void *user_data) {
  if (!engine || !before_json || !before_length ||
      (!before_resolution_json && before_resolution_length) || !after_json ||
      !after_length || (!after_resolution_json && after_resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!request_length)
    return archbird_map_diff(engine, before_json, before_length, after_json,
                             after_length, json_flags, write_fn, user_data);
  return ab_projection_diff(engine, before_json, before_length,
                            before_resolution_json, before_resolution_length,
                            after_json, after_length, after_resolution_json,
                            after_resolution_length, request_json,
                            request_length, json_flags, write_fn, user_data);
}

#undef TRY
