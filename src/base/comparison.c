#include "base/comparison.h"

#include <stdint.h>
#include <string.h>

static ArchbirdStatus comparison_error(ArchbirdEngine *engine,
                                       const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static ArchbirdStatus
validate_population(ArchbirdEngine *engine,
                    const AbComparisonPopulation *population) {
  size_t index;
  if (population->count && !population->items)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < population->count; index++) {
    const AbComparisonItem *item = &population->items[index];
    if (!item->key || !item->key->length)
      return comparison_error(engine,
                              "comparison population has an empty identity");
    if (index &&
        ab_string_compare(population->items[index - 1].key, item->key) >= 0)
      return comparison_error(
          engine, "comparison population identities are not unique and sorted");
  }
  return ARCHBIRD_OK;
}

static const AbComparisonItem *
find_item(const AbComparisonPopulation *population, const AbString *key,
          size_t *out_index) {
  size_t low = 0;
  size_t high = population->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(population->items[middle].key, key);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      if (out_index)
        *out_index = middle;
      return &population->items[middle];
    }
  }
  return NULL;
}

static ArchbirdStatus validate_plan(ArchbirdEngine *engine,
                                    const AbComparisonPlan *plan) {
  size_t group_index;
  if (plan->join == AB_COMPARISON_IDENTITY) {
    if (plan->group_count || plan->groups ||
        (plan->compare_values && !plan->value_equal))
      return ARCHBIRD_INVALID_ARGUMENT;
    return ARCHBIRD_OK;
  }
  if (plan->join != AB_COMPARISON_CORRESPONDENCE || plan->compare_values ||
      (plan->group_count && !plan->groups))
    return ARCHBIRD_INVALID_ARGUMENT;
  for (group_index = 0; group_index < plan->group_count; group_index++) {
    const AbComparisonGroup *group = &plan->groups[group_index];
    size_t target_index;
    if (!group->left || !group->left->length || !group->right_count ||
        !group->rights)
      return comparison_error(engine, "correspondence group is empty");
    if (group_index &&
        ab_string_compare(plan->groups[group_index - 1].left, group->left) >= 0)
      return comparison_error(
          engine, "correspondence groups are not unique and sorted");
    for (target_index = 0; target_index < group->right_count; target_index++)
      if (!group->rights[target_index] ||
          !group->rights[target_index]->length ||
          (target_index && ab_string_compare(group->rights[target_index - 1],
                                             group->rights[target_index]) >= 0))
        return comparison_error(
            engine, "correspondence targets are not unique and sorted");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
add_record(ArchbirdEngine *engine, AbComparisonResult *result,
           AbComparisonKind kind, const AbComparisonItem *left,
           const AbComparisonItem *right, const AbString *left_key,
           const AbString *right_key, size_t group_index) {
  AbComparisonRecord *resized;
  if (result->count == result->capacity) {
    size_t capacity = result->capacity ? result->capacity * 2 : 32;
    if (capacity > SIZE_MAX / sizeof(*result->records))
      return ARCHBIRD_LIMIT_EXCEEDED;
    resized = (AbComparisonRecord *)ab_realloc(
        engine, result->records, capacity * sizeof(*result->records));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory classifying comparison");
    result->records = resized;
    result->capacity = capacity;
  }
  result->records[result->count++] =
      (AbComparisonRecord){kind, left, right, left_key, right_key, group_index};
  return ARCHBIRD_OK;
}

static ArchbirdStatus identity_join(ArchbirdEngine *engine,
                                    const AbComparisonPlan *plan,
                                    AbComparisonResult *result) {
  size_t left = 0;
  size_t right = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  while (status == ARCHBIRD_OK &&
         (left < plan->left.count || right < plan->right.count)) {
    const AbComparisonItem *left_item =
        left < plan->left.count ? &plan->left.items[left] : NULL;
    const AbComparisonItem *right_item =
        right < plan->right.count ? &plan->right.items[right] : NULL;
    int compared = !left_item ? 1
                   : !right_item
                       ? -1
                       : ab_string_compare(left_item->key, right_item->key);
    if (compared < 0) {
      status = add_record(engine, result, AB_COMPARISON_LEFT_ONLY, left_item,
                          NULL, left_item->key, NULL, SIZE_MAX);
      left++;
    } else if (compared > 0) {
      status = add_record(engine, result, AB_COMPARISON_RIGHT_ONLY, NULL,
                          right_item, NULL, right_item->key, SIZE_MAX);
      right++;
    } else {
      AbComparisonKind kind = AB_COMPARISON_MATCHED;
      if (plan->compare_values && plan->value_equal &&
          !plan->value_equal(left_item->value, right_item->value))
        kind = AB_COMPARISON_CHANGED;
      status = add_record(engine, result, kind, left_item, right_item,
                          left_item->key, right_item->key, SIZE_MAX);
      left++;
      right++;
    }
  }
  return status;
}

static ArchbirdStatus correspondence_join(ArchbirdEngine *engine,
                                          const AbComparisonPlan *plan,
                                          AbComparisonResult *result) {
  unsigned char *left_covered = NULL;
  unsigned char *right_covered = NULL;
  size_t group_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (plan->left.count)
    left_covered = (unsigned char *)ab_calloc(engine, plan->left.count, 1);
  if (plan->right.count)
    right_covered = (unsigned char *)ab_calloc(engine, plan->right.count, 1);
  if ((plan->left.count && !left_covered) ||
      (plan->right.count && !right_covered)) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory tracking correspondence");
    goto done;
  }
  if (plan->group_count) {
    if (plan->group_count > SIZE_MAX / sizeof(*result->groups)) {
      status = ARCHBIRD_LIMIT_EXCEEDED;
      goto done;
    }
    result->groups = (AbComparisonGroupResult *)ab_calloc(
        engine, plan->group_count, sizeof(*result->groups));
    if (!result->groups) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory classifying correspondence");
      goto done;
    }
    result->group_count = plan->group_count;
  }
  for (group_index = 0;
       status == ARCHBIRD_OK && group_index < plan->group_count;
       group_index++) {
    const AbComparisonGroup *group = &plan->groups[group_index];
    AbComparisonGroupResult *group_result = &result->groups[group_index];
    const AbComparisonItem *left_item;
    size_t left_index = 0;
    size_t right_index;
    size_t target_index;
    group_result->group = group;
    group_result->record_start = result->count;
    left_item = find_item(&plan->left, group->left, &left_index);
    group_result->left = left_item;
    if (!left_item) {
      status = add_record(engine, result, AB_COMPARISON_MAPPING_LEFT_MISSING,
                          NULL, NULL, group->left, NULL, group_index);
      group_result->state = AB_COMPARISON_GROUP_SOURCE_MISSING;
      group_result->missing_count = group->right_count;
      group_result->record_count = result->count - group_result->record_start;
      continue;
    }
    left_covered[left_index] = 1;
    for (target_index = 0;
         status == ARCHBIRD_OK && target_index < group->right_count;
         target_index++) {
      const AbString *right_key = group->rights[target_index];
      const AbComparisonItem *right_item =
          find_item(&plan->right, right_key, &right_index);
      if (!right_item) {
        status =
            add_record(engine, result, AB_COMPARISON_MAPPING_RIGHT_MISSING,
                       left_item, NULL, group->left, right_key, group_index);
        group_result->missing_count++;
        continue;
      }
      right_covered[right_index] = 1;
      status = add_record(engine, result, AB_COMPARISON_MATCHED, left_item,
                          right_item, group->left, right_key, group_index);
      group_result->matched_count++;
    }
    group_result->record_count = result->count - group_result->record_start;
    group_result->state = group_result->missing_count
                              ? AB_COMPARISON_GROUP_PARTIAL
                              : AB_COMPARISON_GROUP_COMPLETE;
  }
  if (status == ARCHBIRD_OK && plan->require_left_coverage)
    for (group_index = 0;
         status == ARCHBIRD_OK && group_index < plan->left.count; group_index++)
      if (!left_covered[group_index])
        status = add_record(engine, result, AB_COMPARISON_LEFT_UNMAPPED,
                            &plan->left.items[group_index], NULL,
                            plan->left.items[group_index].key, NULL, SIZE_MAX);
  if (status == ARCHBIRD_OK && plan->require_right_coverage)
    for (group_index = 0;
         status == ARCHBIRD_OK && group_index < plan->right.count;
         group_index++)
      if (!right_covered[group_index])
        status = add_record(engine, result, AB_COMPARISON_RIGHT_UNMAPPED, NULL,
                            &plan->right.items[group_index], NULL,
                            plan->right.items[group_index].key, SIZE_MAX);
done:
  ab_free(engine, left_covered);
  ab_free(engine, right_covered);
  return status;
}

ArchbirdStatus ab_comparison_run(ArchbirdEngine *engine,
                                 const AbComparisonPlan *plan,
                                 AbComparisonResult *out) {
  ArchbirdStatus status;
  if (!engine || !plan || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = validate_population(engine, &plan->left);
  if (status == ARCHBIRD_OK)
    status = validate_population(engine, &plan->right);
  if (status == ARCHBIRD_OK)
    status = validate_plan(engine, plan);
  if (status != ARCHBIRD_OK)
    return status;
  if (!plan->left.complete || !plan->right.complete) {
    out->incomplete = 1;
    return ARCHBIRD_OK;
  }
  if (plan->join == AB_COMPARISON_IDENTITY)
    return identity_join(engine, plan, out);
  if (plan->join == AB_COMPARISON_CORRESPONDENCE)
    return correspondence_join(engine, plan, out);
  return ARCHBIRD_INVALID_ARGUMENT;
}

void ab_comparison_result_free(ArchbirdEngine *engine,
                               AbComparisonResult *result) {
  if (!result)
    return;
  ab_free(engine, result->records);
  ab_free(engine, result->groups);
  memset(result, 0, sizeof(*result));
}
