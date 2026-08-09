#include "act/act_projection_acceptance.h"

#include "base/artifact_validation.h"
#include "projection/projection_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbProjectionKeySet {
  const AbString **values;
  size_t count;
} AbProjectionKeySet;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(ArchbirdEngine *engine, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act acceptance: %s", message);
}

static ArchbirdStatus reject_key(ArchbirdEngine *engine, const char *message,
                                 const AbString *key) {
  return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                            ARCHBIRD_NO_OFFSET, "act acceptance: %s: %.*s",
                            message, (int)key->length, key->data);
}

static int key_compare(const void *left, const void *right) {
  const AbString *const *a = (const AbString *const *)left;
  const AbString *const *b = (const AbString *const *)right;
  return ab_string_compare(*a, *b);
}

static int result_has_key(const AbProjectionResult *result,
                          const AbString *key) {
  size_t index;
  for (index = 0; index < result->data.item_count; index++)
    if (ab_string_equal(&result->data.items[index].key, key))
      return 1;
  return 0;
}

static ArchbirdStatus collect_delta(ArchbirdEngine *engine,
                                    const AbProjectionResult *before,
                                    const AbProjectionResult *after,
                                    AbProjectionKeySet *added,
                                    AbProjectionKeySet *removed) {
  size_t index;
  added->values = NULL;
  added->count = 0;
  removed->values = NULL;
  removed->count = 0;
  if (after->data.item_count) {
    added->values = (const AbString **)ab_calloc(engine, after->data.item_count,
                                                 sizeof(*added->values));
    if (!added->values)
      return reject(engine, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory comparing after-state projection");
  }
  if (before->data.item_count) {
    removed->values = (const AbString **)ab_calloc(
        engine, before->data.item_count, sizeof(*removed->values));
    if (!removed->values) {
      ab_free(engine, added->values);
      added->values = NULL;
      return reject(engine, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory comparing before-state projection");
    }
  }
  for (index = 0; index < after->data.item_count; index++)
    if (!result_has_key(before, &after->data.items[index].key))
      added->values[added->count++] = &after->data.items[index].key;
  for (index = 0; index < before->data.item_count; index++)
    if (!result_has_key(after, &before->data.items[index].key))
      removed->values[removed->count++] = &before->data.items[index].key;
  if (added->count > 1)
    qsort(added->values, added->count, sizeof(*added->values), key_compare);
  if (removed->count > 1)
    qsort(removed->values, removed->count, sizeof(*removed->values),
          key_compare);
  return ARCHBIRD_OK;
}

static void key_set_free(ArchbirdEngine *engine, AbProjectionKeySet *set) {
  ab_free(engine, set->values);
  set->values = NULL;
  set->count = 0;
}

static ArchbirdStatus validate_complete(ArchbirdEngine *engine,
                                        const AbProjectionResult *result,
                                        const char *side) {
  if (strcmp(ab_projection_data_classification(&result->data), "complete") == 0)
    return ARCHBIRD_OK;
  return archbird_error_set(
      engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
      "act acceptance: %s projection is not exhaustive and complete", side);
}

static ArchbirdStatus compare_expected(ArchbirdEngine *engine,
                                       const AbProjectionKeySet *observed,
                                       const AbValue *allowed,
                                       const char *unexpected_message,
                                       const char *missing_message) {
  size_t left = 0;
  size_t right = 0;
  while (left < observed->count && right < allowed->as.array.count) {
    const AbString *actual = observed->values[left];
    const AbString *expected = &allowed->as.array.items[right].as.text;
    int compared = ab_string_compare(actual, expected);
    if (compared < 0)
      return reject_key(engine, unexpected_message, actual);
    if (compared > 0)
      return reject_key(engine, missing_message, expected);
    left++;
    right++;
  }
  if (left < observed->count)
    return reject_key(engine, unexpected_message, observed->values[left]);
  if (right < allowed->as.array.count)
    return reject_key(engine, missing_message,
                      &allowed->as.array.items[right].as.text);
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_keys(AbBuffer *buffer,
                                  const AbProjectionKeySet *set) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < set->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, set->values[index]->data,
                                     set->values[index]->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus accept_one(ArchbirdEngine *engine,
                                 const AbValue *requirement, size_t index,
                                 const AbValue *before_map,
                                 const AbValue *after_map, AbBuffer *rendered) {
  const AbValue *projection = field(requirement, "projection");
  const AbValue *allowed_added = field(requirement, "allowed_added");
  const AbValue *allowed_removed = field(requirement, "allowed_removed");
  const AbValue *declared_id = field(projection, "id");
  char generated_id[64];
  int generated_length;
  AbString id;
  AbProjectionPlan plan = {0};
  AbProjectionResult before = {0};
  AbProjectionResult after = {0};
  AbProjectionKeySet added = {0};
  AbProjectionKeySet removed = {0};
  ArchbirdStatus status;
  if (declared_id) {
    id = declared_id->as.text;
  } else {
    generated_length =
        snprintf(generated_id, sizeof(generated_id), "act.delta.%zu", index);
    if (generated_length < 0 ||
        (size_t)generated_length >= sizeof(generated_id))
      return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                    "projection acceptance identity is too long");
    id.data = generated_id;
    id.length = (size_t)generated_length;
  }
  status = ab_projection_plan_compile(engine, projection, &id, &plan);
  if (status == ARCHBIRD_OK)
    status =
        ab_projection_plan_evaluate(engine, &plan, before_map, NULL, &before);
  if (status == ARCHBIRD_OK)
    status =
        ab_projection_plan_evaluate(engine, &plan, after_map, NULL, &after);
  if (status == ARCHBIRD_OK)
    status = validate_complete(engine, &before, "before-state");
  if (status == ARCHBIRD_OK)
    status = validate_complete(engine, &after, "after-state");
  if (status == ARCHBIRD_OK)
    status = collect_delta(engine, &before, &after, &added, &removed);
  if (status == ARCHBIRD_OK)
    status =
        compare_expected(engine, &added, allowed_added,
                         "after-state adds an unreviewed projection key",
                         "after-state omits a reviewed projection addition");
  if (status == ARCHBIRD_OK)
    status =
        compare_expected(engine, &removed, allowed_removed,
                         "after-state removes an unreviewed projection key",
                         "after-state omits a reviewed projection removal");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, "{\"after_result_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(rendered, after.result_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"allowed_added\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(rendered, allowed_added);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"allowed_removed\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(rendered, allowed_removed);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"before_result_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(rendered, before.result_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"observed_added\":");
  if (status == ARCHBIRD_OK)
    status = render_keys(rendered, &added);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"observed_removed\":");
  if (status == ARCHBIRD_OK)
    status = render_keys(rendered, &removed);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(rendered, projection);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, ",\"status\":\"satisfied\"}");
  key_set_free(engine, &removed);
  key_set_free(engine, &added);
  ab_projection_result_free(engine, &after);
  ab_projection_result_free(engine, &before);
  ab_projection_plan_free(engine, &plan);
  return status;
}

ArchbirdStatus ab_act_projection_deltas_accept(ArchbirdEngine *engine,
                                               const AbValue *requirements,
                                               const AbValue *before_map,
                                               const AbValue *after_map,
                                               AbBuffer *rendered) {
  size_t index;
  ArchbirdStatus status;
  if (!engine || !requirements || requirements->kind != AB_VALUE_ARRAY ||
      !before_map || !after_map || !rendered)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_buffer_literal(rendered, "[");
  for (index = 0; status == ARCHBIRD_OK && index < requirements->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(rendered, ",");
    if (status == ARCHBIRD_OK)
      status = accept_one(engine, &requirements->as.array.items[index], index,
                          before_map, after_map, rendered);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(rendered, "]");
  return status;
}
