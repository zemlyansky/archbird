#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"
#include "verify_checks.h"
#include "verify_runtime.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define AUTHOR_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus author_status__ = (expression);                             \
    if (author_status__ != ARCHBIRD_OK)                                        \
      return author_status__;                                                  \
  } while (0)

static int text_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || !memcmp(value->data, literal, length));
}

static int value_text_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         text_is(&value->as.text, literal);
}

static int author_finding_blocks(const AbVerifyFinding *finding,
                                 const AbValue *check) {
  const AbValue *severity = ab_value_member(check, "severity");
  return (!severity || value_text_is(severity, "error")) &&
         text_is(&finding->applicability, "applicable") &&
         text_is(&finding->disposition, "open") &&
         (!text_is(&finding->comparison, "equal") ||
          text_is(&finding->evidence_state, "unknown") ||
          text_is(&finding->evidence_state, "stale"));
}

static ArchbirdStatus author_invalid(ArchbirdEngine *engine,
                                     const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static ArchbirdStatus render_suite_name(AbBuffer *buffer,
                                        const AbString *project) {
  AbBuffer normalized;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  int dash = 0;
  ab_buffer_init(&normalized, buffer->engine);
  for (index = 0; index < project->length; index++) {
    unsigned char byte = (unsigned char)project->data[index];
    int allowed = (byte >= 'A' && byte <= 'Z') ||
                  (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
                  byte == ':';
    if (allowed) {
      status = ab_buffer_append(&normalized, &byte, 1);
      if (status != ARCHBIRD_OK)
        goto cleanup;
      dash = 0;
    } else if (!dash && normalized.length) {
      status = ab_buffer_literal(&normalized, "-");
      if (status != ARCHBIRD_OK)
        goto cleanup;
      dash = 1;
    }
  }
  while (normalized.length && normalized.data[normalized.length - 1] == '-')
    normalized.length--;
  if (!normalized.length || !isalnum((unsigned char)normalized.data[0])) {
    normalized.length = 0;
    status = ab_buffer_literal(&normalized, "project");
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  status = ab_buffer_literal(&normalized, "-architecture");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, (const char *)normalized.data,
                                   normalized.length);
cleanup:
  ab_buffer_free(&normalized);
  return status;
}

static ArchbirdStatus render_draft(ArchbirdEngine *engine, const AbValue *map,
                                   const char *project_config,
                                   size_t project_config_length,
                                   AbBuffer *buffer) {
  const AbValue *artifact = ab_value_member(map, "artifact");
  const AbValue *project = ab_value_member(map, "project");
  const AbValue *components = ab_value_member(map, "components");
  size_t component_index;
  int first = 1;
  if (!map || map->kind != AB_VALUE_OBJECT || !value_text_is(artifact, "map") ||
      !project || project->kind != AB_VALUE_STRING ||
      !project->as.text.length || !components ||
      components->kind != AB_VALUE_ARRAY)
    return author_invalid(engine,
                          "verification draft requires a canonical Map");
  if (!components->as.array.count)
    return author_invalid(
        engine, "verify --init requires configured architecture components");
  AUTHOR_TRY(ab_buffer_literal(
      buffer, "{\"candidate\":true,\"checks\":[{\"actual\":"
              "\"architecture.actual\",\"assert\":\"allowed_edges\","
              "\"expected\":\"architecture.allowed\",\"id\":"
              "\"ARCH-COMPONENT-EDGES\",\"owner\":"
              "\"architecture\",\"rationale\":"
              "\"Component dependencies must remain within the "
              "reviewed allowed matrix.\",\"severity\":\"error\","
              "\"tags\":[\"candidate-review-required\"]}],"
              "\"description\":\"DRAFT generated from current "
              "component edges. Review intended boundaries; remove "
              "candidate=true only after human approval.\","
              "\"extractors\":{\"architecture.actual\":{"
              "\"kind\":\"component_edges\",\"project\":\"subject\"},"
              "\"architecture.allowed\":{\"kind\":"
              "\"literal_relation\",\"rows\":["));
  for (component_index = 0; component_index < components->as.array.count;
       component_index++) {
    const AbValue *component = &components->as.array.items[component_index];
    const AbValue *source = ab_value_member(component, "name");
    const AbValue *outgoing = ab_value_member(component, "outgoing");
    size_t target_index;
    if (!source || source->kind != AB_VALUE_STRING || !outgoing ||
        outgoing->kind != AB_VALUE_OBJECT)
      return author_invalid(engine,
                            "invalid Map component in verification draft");
    for (target_index = 0; target_index < outgoing->as.object.count;
         target_index++) {
      const AbObjectField *target = &outgoing->as.object.fields[target_index];
      if (target->value.kind != AB_VALUE_ARRAY || !target->value.as.array.count)
        continue;
      if (!first)
        AUTHOR_TRY(ab_buffer_literal(buffer, ","));
      first = 0;
      AUTHOR_TRY(ab_buffer_literal(buffer, "{\"kind\":\"*\",\"source\":"));
      AUTHOR_TRY(ab_value_render(buffer, source));
      AUTHOR_TRY(ab_buffer_literal(buffer, ",\"target\":"));
      AUTHOR_TRY(ab_buffer_json_string(buffer, target->name.data,
                                       target->name.length));
      AUTHOR_TRY(ab_buffer_literal(buffer, "}"));
    }
  }
  AUTHOR_TRY(
      ab_buffer_literal(buffer, "]}},\"projects\":{\"subject\":{\"config\":"));
  AUTHOR_TRY(
      ab_buffer_json_string(buffer, project_config, project_config_length));
  AUTHOR_TRY(ab_buffer_literal(buffer, "}},\"schema_version\":1,\"suite\":"));
  AUTHOR_TRY(render_suite_name(buffer, &project->as.text));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus
archbird_verification_draft(ArchbirdEngine *engine, const uint8_t *map_json,
                            size_t map_length, const char *project_config,
                            size_t project_config_length, uint32_t json_flags,
                            ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbBuffer draft;
  ArchbirdStatus status;
  if (!engine || (!map_json && map_length) ||
      (!project_config && project_config_length) || !project_config_length ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&draft, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK)
    status = render_draft(engine, &map, project_config, project_config_length,
                          &draft);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, draft.data, draft.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&draft);
  ab_value_free(engine, &map);
  return status;
}

typedef struct AuthorFinding {
  const AbVerifyFinding *finding;
  const AbValue *check;
} AuthorFinding;

static int author_finding_compare(const void *left_raw, const void *right_raw) {
  const AuthorFinding *left = (const AuthorFinding *)left_raw;
  const AuthorFinding *right = (const AuthorFinding *)right_raw;
  return ab_string_compare(&left->finding->fingerprint,
                           &right->finding->fingerprint);
}

static int author_current_contains(const AuthorFinding *rows, size_t count,
                                   const AbString *fingerprint) {
  size_t index;
  for (index = 0; index < count; index++)
    if (ab_string_equal(&rows[index].finding->fingerprint, fingerprint))
      return 1;
  return 0;
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *const *left = (const AbString *const *)left_raw;
  const AbString *const *right = (const AbString *const *)right_raw;
  return ab_string_compare(*left, *right);
}

static const AbValue *object_member_string(const AbValue *object,
                                           const AbString *name) {
  size_t low = 0;
  size_t high =
      object && object->kind == AB_VALUE_OBJECT ? object->as.object.count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbObjectField *field = &object->as.object.fields[middle];
    int compared = ab_string_compare(&field->name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &field->value;
  }
  return NULL;
}

static ArchbirdStatus render_sorted_value_union(AbBuffer *buffer,
                                                const AbValue *previous,
                                                const AbStringArray *current) {
  const AbString **rows;
  size_t previous_count = previous && previous->kind == AB_VALUE_ARRAY
                              ? previous->as.array.count
                              : 0;
  size_t capacity = previous_count + (current ? current->count : 0);
  size_t count = 0;
  size_t index;
  ArchbirdStatus status;
  rows = capacity ? (const AbString **)ab_malloc(buffer->engine,
                                                 capacity * sizeof(*rows))
                  : NULL;
  if (capacity && !rows)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; index < previous_count; index++)
    rows[count++] = &previous->as.array.items[index].as.text;
  for (index = 0; current && index < current->count; index++) {
    size_t old;
    for (old = 0; old < count; old++)
      if (ab_string_equal(rows[old], &current->items[index]))
        break;
    if (old == count)
      rows[count++] = &current->items[index];
  }
  if (count > 1)
    qsort(rows, count, sizeof(*rows), string_pointer_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; index < count; index++) {
    if (status == ARCHBIRD_OK && index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, rows[index]->data, rows[index]->length);
  }
  ab_free(buffer->engine, rows);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_baseline(AbVerificationContext *context,
                                      const char *owner, size_t owner_length,
                                      const char *rationale,
                                      size_t rationale_length,
                                      AbBuffer *buffer) {
  const AbValue *previous = context->input.baseline;
  const AbValue *previous_active = previous && previous->kind == AB_VALUE_OBJECT
                                       ? ab_value_member(previous, "active")
                                       : NULL;
  const AbValue *previous_resolved =
      previous && previous->kind == AB_VALUE_OBJECT
          ? ab_value_member(previous, "resolved")
          : NULL;
  const AbValue *previous_coverage =
      previous && previous->kind == AB_VALUE_OBJECT
          ? ab_value_member(previous, "coverage")
          : NULL;
  AuthorFinding *current = NULL;
  const AbString **resolved = NULL;
  const AbString **coverage_keys = NULL;
  size_t current_count = 0;
  size_t resolved_count = 0;
  size_t coverage_key_count = 0;
  size_t check_index;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (check_index = 0; check_index < context->check_count; check_index++) {
    AbVerifyCheckResult *result = &context->checks[check_index];
    size_t finding_index;
    for (finding_index = 0; finding_index < result->finding_count;
         finding_index++)
      if (author_finding_blocks(&result->findings[finding_index], result->spec))
        current_count++;
  }
  current = current_count
                ? (AuthorFinding *)ab_malloc(context->engine,
                                             current_count * sizeof(*current))
                : NULL;
  if (current_count && !current)
    return ARCHBIRD_OUT_OF_MEMORY;
  current_count = 0;
  for (check_index = 0; check_index < context->check_count; check_index++) {
    AbVerifyCheckResult *result = &context->checks[check_index];
    size_t finding_index;
    for (finding_index = 0; finding_index < result->finding_count;
         finding_index++) {
      AbVerifyFinding *finding = &result->findings[finding_index];
      if (author_finding_blocks(finding, result->spec)) {
        current[current_count].finding = finding;
        current[current_count].check = result->spec;
        current_count++;
      }
    }
  }
  if (current_count > 1)
    qsort(current, current_count, sizeof(*current), author_finding_compare);
  {
    size_t old_resolved =
        previous_resolved && previous_resolved->kind == AB_VALUE_ARRAY
            ? previous_resolved->as.array.count
            : 0;
    size_t old_active =
        previous_active && previous_active->kind == AB_VALUE_ARRAY
            ? previous_active->as.array.count
            : 0;
    resolved = old_resolved + old_active
                   ? (const AbString **)ab_malloc(context->engine,
                                                  (old_resolved + old_active) *
                                                      sizeof(*resolved))
                   : NULL;
    if ((old_resolved + old_active) && !resolved) {
      ab_free(context->engine, current);
      return ARCHBIRD_OUT_OF_MEMORY;
    }
    for (index = 0; index < old_resolved; index++)
      resolved[resolved_count++] =
          &previous_resolved->as.array.items[index].as.text;
    for (index = 0; index < old_active; index++) {
      const AbValue *fingerprint = ab_value_member(
          &previous_active->as.array.items[index], "fingerprint");
      size_t duplicate;
      if (!fingerprint || fingerprint->kind != AB_VALUE_STRING ||
          author_current_contains(current, current_count,
                                  &fingerprint->as.text))
        continue;
      for (duplicate = 0; duplicate < resolved_count; duplicate++)
        if (ab_string_equal(resolved[duplicate], &fingerprint->as.text))
          break;
      if (duplicate == resolved_count)
        resolved[resolved_count++] = &fingerprint->as.text;
    }
    if (resolved_count > 1)
      qsort(resolved, resolved_count, sizeof(*resolved),
            string_pointer_compare);
  }
  {
    size_t previous_count =
        previous_coverage && previous_coverage->kind == AB_VALUE_OBJECT
            ? previous_coverage->as.object.count
            : 0;
    size_t capacity = previous_count + context->check_count;
    coverage_keys =
        capacity ? (const AbString **)ab_malloc(
                       context->engine, capacity * sizeof(*coverage_keys))
                 : NULL;
    if (capacity && !coverage_keys) {
      ab_free(context->engine, resolved);
      ab_free(context->engine, current);
      return ARCHBIRD_OUT_OF_MEMORY;
    }
    for (index = 0; index < previous_count; index++)
      coverage_keys[coverage_key_count++] =
          &previous_coverage->as.object.fields[index].name;
    for (check_index = 0; check_index < context->check_count; check_index++) {
      const AbValue *id =
          ab_value_member(context->checks[check_index].spec, "id");
      size_t duplicate;
      for (duplicate = 0; duplicate < coverage_key_count; duplicate++)
        if (ab_string_equal(coverage_keys[duplicate], &id->as.text))
          break;
      if (duplicate == coverage_key_count)
        coverage_keys[coverage_key_count++] = &id->as.text;
    }
    if (coverage_key_count > 1)
      qsort(coverage_keys, coverage_key_count, sizeof(*coverage_keys),
            string_pointer_compare);
  }
  status = ab_buffer_literal(buffer, "{\"active\":[");
  for (index = 0; status == ARCHBIRD_OK && index < current_count; index++) {
    const AbValue *id = ab_value_member(current[index].check, "id");
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"check\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, id);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"comparison\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, current[index].finding->comparison.data,
                                current[index].finding->comparison.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"fingerprint\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(
          buffer, current[index].finding->fingerprint.data,
          current[index].finding->fingerprint.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"key\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, current[index].finding->key.data,
                                     current[index].finding->key.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, "],\"artifact\":\"verification-baseline\",\"coverage\":{");
  for (index = 0; status == ARCHBIRD_OK && index < coverage_key_count;
       index++) {
    const AbStringArray *current_coverage = NULL;
    const AbValue *old =
        object_member_string(previous_coverage, coverage_keys[index]);
    for (check_index = 0; check_index < context->check_count; check_index++) {
      AbVerifyCheckResult *result = &context->checks[check_index];
      const AbValue *id = ab_value_member(result->spec, "id");
      if (ab_string_equal(&id->as.text, coverage_keys[index])) {
        current_coverage = &result->coverage;
        break;
      }
    }
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, coverage_keys[index]->data,
                                     coverage_keys[index]->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":");
    if (status == ARCHBIRD_OK)
      status = render_sorted_value_union(buffer, old, current_coverage);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"owner\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, owner, owner_length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"rationale\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, rationale, rationale_length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"resolved\":[");
  for (index = 0; status == ARCHBIRD_OK && index < resolved_count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, resolved[index]->data,
                                     resolved[index]->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"schema_version\":1,\"suite\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, context->suite.name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"suite_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, context->suite.sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  ab_free(context->engine, coverage_keys);
  ab_free(context->engine, resolved);
  ab_free(context->engine, current);
  return status;
}

ArchbirdStatus archbird_verification_freeze(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    const char *owner, size_t owner_length, const char *rationale,
    size_t rationale_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbValue suite_document = {0};
  AbValue input_document = {0};
  AbVerificationContext context = {0};
  AbBuffer baseline;
  ArchbirdStatus status;
  if (!engine || !owner || !owner_length || !rationale || !rationale_length ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&baseline, engine);
  status = ab_verification_context_analyze(
      engine, suite_json, suite_length, verification_input_json,
      verification_input_length, &suite_document, &input_document, &context);
  if (status == ARCHBIRD_OK)
    status = render_baseline(&context, owner, owner_length, rationale,
                             rationale_length, &baseline);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, baseline.data, baseline.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&baseline);
  ab_verification_context_free(&context);
  ab_value_free(engine, &input_document);
  ab_value_free(engine, &suite_document);
  return status;
}
