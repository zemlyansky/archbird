#include "projection/projection_comparison.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *const *left = (const AbString *const *)left_raw;
  const AbString *const *right = (const AbString *const *)right_raw;
  return ab_string_compare(*left, *right);
}

static int group_compare(const void *left_raw, const void *right_raw) {
  const AbComparisonGroup *left = (const AbComparisonGroup *)left_raw;
  const AbComparisonGroup *right = (const AbComparisonGroup *)right_raw;
  return ab_string_compare(left->left, right->left);
}

static int complete(const AbProjectionData *data) {
  return !strcmp(ab_projection_data_classification(data), "complete");
}

void ab_projection_comparison_free(ArchbirdEngine *engine,
                                   AbProjectionComparisonInput *input) {
  if (!input)
    return;
  ab_free(engine, input->left);
  ab_free(engine, input->right);
  ab_free(engine, input->groups);
  ab_free(engine, input->targets);
  memset(input, 0, sizeof(*input));
}

ArchbirdStatus ab_projection_comparison_prepare(
    ArchbirdEngine *engine, const AbProjectionData *left,
    const AbProjectionData *right, const AbValue *mapping,
    int require_left_coverage, int require_right_coverage,
    AbProjectionComparisonInput *out, AbComparisonPlan *plan) {
  size_t target_count = 0;
  size_t target_offset = 0;
  size_t index;
  if (!engine || !left || !right || !mapping ||
      mapping->kind != AB_VALUE_OBJECT || !out || !plan)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  memset(plan, 0, sizeof(*plan));
  for (index = 0; index < mapping->as.object.count; index++) {
    const AbValue *targets = &mapping->as.object.fields[index].value;
    if (targets->kind != AB_VALUE_ARRAY || !targets->as.array.count)
      return invalid(engine, "correspondence targets must be non-empty arrays");
    if (targets->as.array.count > SIZE_MAX - target_count)
      return ARCHBIRD_LIMIT_EXCEEDED;
    target_count += targets->as.array.count;
  }
  if (left->item_count) {
    out->left = (AbComparisonItem *)ab_calloc(engine, left->item_count,
                                              sizeof(*out->left));
    if (!out->left)
      goto out_of_memory;
  }
  if (right->item_count) {
    out->right = (AbComparisonItem *)ab_calloc(engine, right->item_count,
                                               sizeof(*out->right));
    if (!out->right)
      goto out_of_memory;
  }
  if (mapping->as.object.count) {
    out->groups = (AbComparisonGroup *)ab_calloc(
        engine, mapping->as.object.count, sizeof(*out->groups));
    if (!out->groups)
      goto out_of_memory;
  }
  if (target_count) {
    out->targets = (const AbString **)ab_calloc(engine, target_count,
                                                sizeof(*out->targets));
    if (!out->targets)
      goto out_of_memory;
  }
  for (index = 0; index < left->item_count; index++) {
    out->left[index].key = &left->items[index].key;
    out->left[index].value = &left->items[index].value;
    out->left[index].subject = &left->items[index];
  }
  for (index = 0; index < right->item_count; index++) {
    out->right[index].key = &right->items[index].key;
    out->right[index].value = &right->items[index].value;
    out->right[index].subject = &right->items[index];
  }
  for (index = 0; index < mapping->as.object.count; index++) {
    const AbObjectField *entry = &mapping->as.object.fields[index];
    AbComparisonGroup *group = &out->groups[index];
    size_t target_index;
    group->left = &entry->name;
    group->rights = out->targets + target_offset;
    group->right_count = entry->value.as.array.count;
    group->subject = entry;
    for (target_index = 0; target_index < entry->value.as.array.count;
         target_index++) {
      const AbValue *target = &entry->value.as.array.items[target_index];
      if (target->kind != AB_VALUE_STRING || !target->as.text.length) {
        ab_projection_comparison_free(engine, out);
        return invalid(engine,
                       "correspondence target must be a non-empty string");
      }
      out->targets[target_offset + target_index] = &target->as.text;
    }
    if (group->right_count > 1)
      qsort(out->targets + target_offset, group->right_count,
            sizeof(*out->targets), string_pointer_compare);
    for (target_index = 1; target_index < group->right_count; target_index++)
      if (!ab_string_compare(group->rights[target_index - 1],
                             group->rights[target_index])) {
        ab_projection_comparison_free(engine, out);
        return invalid(engine, "correspondence targets must be unique");
      }
    target_offset += group->right_count;
  }
  if (mapping->as.object.count > 1)
    qsort(out->groups, mapping->as.object.count, sizeof(*out->groups),
          group_compare);
  for (index = 1; index < mapping->as.object.count; index++)
    if (!ab_string_compare(out->groups[index - 1].left,
                           out->groups[index].left)) {
      ab_projection_comparison_free(engine, out);
      return invalid(engine, "correspondence sources must be unique");
    }
  plan->join = AB_COMPARISON_CORRESPONDENCE;
  plan->left =
      (AbComparisonPopulation){out->left, left->item_count, complete(left)};
  plan->right =
      (AbComparisonPopulation){out->right, right->item_count, complete(right)};
  plan->groups = out->groups;
  plan->group_count = mapping->as.object.count;
  plan->require_left_coverage = require_left_coverage;
  plan->require_right_coverage = require_right_coverage;
  return ARCHBIRD_OK;

out_of_memory:
  ab_projection_comparison_free(engine, out);
  return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                            "out of memory preparing correspondence");
}
