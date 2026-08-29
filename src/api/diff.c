#include <archbird/archbird.h>

/* Compose Map and projection kernels behind the public Diff operation. */

#include "base/archbird_internal.h"
#include "base/comparison.h"
#include "base/json_value.h"
#include "base/render_internal.h"
#include "base/sha256.h"
#include "map/diff_internal.h"
#include "map/map_internal.h"
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

static int fact_backed_projection(const AbProjectionPlan *plan) {
  const AbValue *select = ab_value_member(&plan->definition, "select");
  return ab_value_string_is(select, "ordered_rows");
}

static ArchbirdStatus projection_map_decode(ArchbirdEngine *engine,
                                            const uint8_t *json, size_t length,
                                            const AbProjectionPlan *plan,
                                            AbValue *out) {
  static const char *const ordered_fields[] = {
      "artifact", "discovery", "evidence",       "facts", "files",
      "inputs",   "project",   "schema_version", "tool"};
  if (fact_backed_projection(plan))
    return ab_diff_map_select(
        engine, json, length, ordered_fields,
        sizeof(ordered_fields) / sizeof(ordered_fields[0]), out);
  return ab_diff_map_decode(engine, json, length, out);
}

static int bytes_suffix(const AbString *value, const char *suffix) {
  size_t length = strlen(suffix);
  return value->length >= length &&
         !memcmp(value->data + value->length - length, suffix, length);
}

static ArchbirdStatus render_plan_requirements(AbBuffer *buffer,
                                               const AbProjectionPlan *plan) {
  const AbValue *select = ab_value_member(&plan->definition, "select");
  const AbValue *container = ab_value_member(&plan->definition, "container");
  const char *separator;
  size_t path_length;
  const char *language = NULL;
  TRY(ab_buffer_literal(buffer, "{\"live_project\":"));
  TRY(ab_buffer_literal(buffer,
                        fact_backed_projection(plan) ? "true" : "false"));
  TRY(ab_buffer_literal(buffer, ",\"ordered_containers\":["));
  if (!ab_value_string_is(select, "ordered_rows") || !container ||
      container->kind != AB_VALUE_STRING)
    return ab_buffer_literal(buffer, "]}");
  separator = memchr(container->as.text.data, ':', container->as.text.length);
  if (!separator || separator == container->as.text.data ||
      separator + 1 == container->as.text.data + container->as.text.length)
    return invalid(buffer->engine,
                   "ordered_rows container must be PATH:QUALIFIED_NAME");
  path_length = (size_t)(separator - container->as.text.data);
  {
    AbString path = {container->as.text.data, path_length};
    if (bytes_suffix(&path, ".py"))
      language = "python";
    else if (bytes_suffix(&path, ".c") || bytes_suffix(&path, ".h"))
      language = "c";
  }
  if (!language)
    return ab_buffer_literal(buffer, "]}");
  TRY(ab_buffer_literal(buffer, "{\"language\":"));
  TRY(ab_buffer_json_string(buffer, language, strlen(language)));
  TRY(ab_buffer_literal(buffer, ",\"names\":["));
  TRY(ab_buffer_json_string(buffer, separator + 1,
                            container->as.text.length - path_length -
                                (size_t)1));
  TRY(ab_buffer_literal(buffer, "],\"paths\":["));
  TRY(ab_buffer_json_string(buffer, container->as.text.data, path_length));
  return ab_buffer_literal(buffer, "]}]}");
}

ArchbirdStatus
archbird_diff_requirements(ArchbirdEngine *engine, const uint8_t *request_json,
                           size_t request_length, uint32_t json_flags,
                           ArchbirdWriteFn write_fn, void *user_data) {
  AbValue request = {0};
  const AbValue *before_definition = NULL;
  const AbValue *after_definition = NULL;
  const AbValue *mapping = NULL;
  AbProjectionPlan before_plan = {0};
  AbProjectionPlan after_plan = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || !request_json || !request_length || !write_fn ||
      !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = request_validate(engine, &request, &before_definition,
                              &after_definition, &mapping);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, before_definition, &before_plan);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, after_definition, &after_plan);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "{\"after\":");
  if (status == ARCHBIRD_OK)
    status = render_plan_requirements(&buffer, &after_plan);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"before\":");
  if (status == ARCHBIRD_OK)
    status = render_plan_requirements(&buffer, &before_plan);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK && !json_flags) {
    if (write_fn(user_data, buffer.data, buffer.length) != 0)
      status =
          archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                             "diff output writer failed");
  } else if (status == ARCHBIRD_OK) {
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  }
  ab_buffer_free(&buffer);
  ab_projection_plan_free(engine, &after_plan);
  ab_projection_plan_free(engine, &before_plan);
  ab_value_free(engine, &request);
  return status;
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
  for (index = 0; index < group->group->right_count; index++) {
    const AbString *target = group->group->rights[index];
    const AbComparisonRecord *record = NULL;
    size_t record_index;
    for (record_index = 0; record_index < group->record_count; record_index++) {
      const AbComparisonRecord *candidate =
          &comparison->records[group->record_start + record_index];
      if (candidate->right_key &&
          !ab_string_compare(candidate->right_key, target)) {
        record = candidate;
        break;
      }
    }
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_buffer_literal(buffer, "{\"after\":"));
    TRY(render_item_or_null(buffer, record ? record->right : NULL));
    TRY(ab_buffer_literal(buffer, ",\"key\":"));
    TRY(ab_buffer_json_string(buffer, target->data, target->length));
    TRY(ab_buffer_literal(buffer, ",\"state\":"));
    if (record && record->kind == AB_COMPARISON_MATCHED)
      TRY(ab_buffer_literal(buffer, "\"matched\""));
    else if (record && record->kind == AB_COMPARISON_MAPPING_RIGHT_MISSING)
      TRY(ab_buffer_literal(buffer, "\"missing\""));
    else
      TRY(ab_buffer_literal(buffer, "\"unresolved\""));
    TRY(ab_buffer_literal(buffer, "}"));
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

static ArchbirdStatus projection_comparison_render(
    ArchbirdEngine *engine, const AbValue *before_map,
    const AbProjectionPlan *before_plan,
    const AbProjectionResult *before_result, const AbValue *after_map,
    const AbProjectionPlan *after_plan, const AbProjectionResult *after_result,
    const AbValue *mapping, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbProjectionComparisonInput input = {0};
  AbComparisonPlan comparison_plan = {0};
  AbComparisonResult comparison = {0};
  AbDiffContext context = {engine};
  AbBuffer buffer;
  char correspondence_sha256[65] = {0};
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_projection_comparison_prepare(engine, &before_result->data,
                                            &after_result->data, mapping, 1, 1,
                                            &input, &comparison_plan);
  if (status == ARCHBIRD_OK)
    status = ab_comparison_run(engine, &comparison_plan, &comparison);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, mapping, correspondence_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "{\"after\":{\"map\":");
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_identity_render(&context, &buffer, after_map);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = render_projection_identity(&buffer, after_plan, after_result);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer,
                               "},\"artifact\":\"diff\",\"before\":{\"map\":");
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_identity_render(&context, &buffer, before_map);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = render_projection_identity(&buffer, before_plan, before_result);
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
  if (status == ARCHBIRD_OK && !json_flags) {
    if (write_fn(user_data, buffer.data, buffer.length) != 0)
      status =
          archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                             "diff output writer failed");
  } else if (status == ARCHBIRD_OK) {
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  }
  ab_buffer_free(&buffer);
  ab_comparison_result_free(engine, &comparison);
  ab_projection_comparison_free(engine, &input);
  return status;
}

static ArchbirdStatus project_projection_evaluate(
    ArchbirdEngine *engine, const AbProjectionPlan *plan,
    const ArchbirdProject *project, const AbValue *resolution,
    AbProjectionResult *out, AbValue *out_map_identity) {
  static const char *const ordered_domains[] = {"ordered-containers",
                                                "ordered-rows"};
  AbMapFactSelection selection = {0};
  const AbValue *container = ab_value_member(&plan->definition, "container");
  AbBuffer input;
  ArchbirdStatus status;
  memset(out_map_identity, 0, sizeof(*out_map_identity));
  if (!fact_backed_projection(plan))
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "live project projection is not fact-backed");
  selection.domains = ordered_domains;
  selection.domain_count = sizeof(ordered_domains) / sizeof(ordered_domains[0]);
  selection.container = &container->as.text;
  ab_buffer_init(&input, engine);
  status = ab_map_render_fact_projection_input(&input, project, &selection);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, input.data, input.length,
                                  out_map_identity);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(engine, plan, out_map_identity,
                                         resolution, out);
  ab_buffer_free(&input);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out_map_identity);
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
  AbDiffContext context = {engine};
  ArchbirdStatus status;
  status = ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = request_validate(engine, &request, &before_definition,
                              &after_definition, &mapping);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, before_definition, &before_plan);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, after_definition, &after_plan);
  if (status == ARCHBIRD_OK)
    status = projection_map_decode(engine, before_json, before_length,
                                   &before_plan, &before);
  if (status == ARCHBIRD_OK)
    status = projection_map_decode(engine, after_json, after_length,
                                   &after_plan, &after);
  if (status == ARCHBIRD_OK)
    status = resolution_decode(engine, before_resolution_json,
                               before_resolution_length, &before_resolution);
  if (status == ARCHBIRD_OK)
    status = resolution_decode(engine, after_resolution_json,
                               after_resolution_length, &after_resolution);
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_validate(&context, &before);
  if (status == ARCHBIRD_OK)
    status = ab_diff_map_validate(&context, &after);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &before_plan, &before,
        before_resolution_length ? &before_resolution : NULL, &before_result);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &after_plan, &after,
        after_resolution_length ? &after_resolution : NULL, &after_result);
  if (status == ARCHBIRD_OK)
    status = projection_comparison_render(
        engine, &before, &before_plan, &before_result, &after, &after_plan,
        &after_result, mapping, json_flags, write_fn, user_data);
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

ArchbirdStatus archbird_projects_diff(
    ArchbirdEngine *engine, const ArchbirdProject *before_project,
    const uint8_t *before_resolution_json, size_t before_resolution_length,
    const ArchbirdProject *after_project, const uint8_t *after_resolution_json,
    size_t after_resolution_length, const uint8_t *request_json,
    size_t request_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbValue before_resolution = {0};
  AbValue after_resolution = {0};
  AbValue request = {0};
  AbValue before_map = {0};
  AbValue after_map = {0};
  const AbValue *before_definition = NULL;
  const AbValue *after_definition = NULL;
  const AbValue *mapping = NULL;
  AbProjectionPlan before_plan = {0};
  AbProjectionPlan after_plan = {0};
  AbProjectionResult before_result = {0};
  AbProjectionResult after_result = {0};
  ArchbirdStatus status;
  if (!engine || !before_project ||
      (!before_resolution_json && before_resolution_length) || !after_project ||
      (!after_resolution_json && after_resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!request_length)
    return ab_projects_map_diff(engine, before_project, after_project,
                                json_flags, write_fn, user_data);
  status = resolution_decode(engine, before_resolution_json,
                             before_resolution_length, &before_resolution);
  if (status == ARCHBIRD_OK)
    status = resolution_decode(engine, after_resolution_json,
                               after_resolution_length, &after_resolution);
  if (status == ARCHBIRD_OK)
    status =
        ab_json_value_decode(engine, request_json, request_length, &request);
  if (status == ARCHBIRD_OK)
    status = request_validate(engine, &request, &before_definition,
                              &after_definition, &mapping);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, before_definition, &before_plan);
  if (status == ARCHBIRD_OK)
    status = plan_compile(engine, after_definition, &after_plan);
  if (status == ARCHBIRD_OK)
    status = project_projection_evaluate(
        engine, &before_plan, before_project,
        before_resolution_length ? &before_resolution : NULL, &before_result,
        &before_map);
  if (status == ARCHBIRD_OK)
    status = project_projection_evaluate(
        engine, &after_plan, after_project,
        after_resolution_length ? &after_resolution : NULL, &after_result,
        &after_map);
  if (status == ARCHBIRD_OK)
    status = projection_comparison_render(
        engine, &before_map, &before_plan, &before_result, &after_map,
        &after_plan, &after_result, mapping, json_flags, write_fn, user_data);
  ab_value_free(engine, &after_map);
  ab_value_free(engine, &before_map);
  ab_projection_result_free(engine, &after_result);
  ab_projection_result_free(engine, &before_result);
  ab_projection_plan_free(engine, &after_plan);
  ab_projection_plan_free(engine, &before_plan);
  ab_value_free(engine, &request);
  ab_value_free(engine, &after_resolution);
  ab_value_free(engine, &before_resolution);
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
