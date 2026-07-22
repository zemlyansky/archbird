#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"
#include "verify_checks.h"
#include "verify_runtime.h"

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
  const AbValue *previous = context->baseline_input;
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
      status = ab_buffer_literal(buffer, "{\"constraint\":");
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
        buffer, "],\"artifact\":\"constraint-baseline\",\"coverage\":{");
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
    status = ab_buffer_literal(buffer, "],\"constraint_policy_sha256\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(buffer, context->constraint_policy_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"schema_version\":1");
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

ArchbirdStatus ab_constraints_render_baseline(
    AbVerificationContext *context, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, AbBuffer *buffer) {
  return render_baseline(context, owner, owner_length, rationale,
                         rationale_length, buffer);
}
