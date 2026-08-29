#ifndef ARCHBIRD_COMPARISON_H
#define ARCHBIRD_COMPARISON_H

#include "base/model.h"

typedef enum AbComparisonJoin {
  AB_COMPARISON_IDENTITY = 1,
  AB_COMPARISON_CORRESPONDENCE = 2
} AbComparisonJoin;

typedef enum AbComparisonKind {
  AB_COMPARISON_MATCHED = 1,
  AB_COMPARISON_CHANGED = 2,
  AB_COMPARISON_LEFT_ONLY = 3,
  AB_COMPARISON_RIGHT_ONLY = 4,
  AB_COMPARISON_LEFT_UNMAPPED = 5,
  AB_COMPARISON_RIGHT_UNMAPPED = 6,
  AB_COMPARISON_MAPPING_LEFT_MISSING = 7,
  AB_COMPARISON_MAPPING_RIGHT_MISSING = 8,
  AB_COMPARISON_UNRESOLVED = 9
} AbComparisonKind;

typedef struct AbComparisonItem {
  const AbString *key;
  const void *value;
  const void *subject;
} AbComparisonItem;

typedef struct AbComparisonPopulation {
  const AbComparisonItem *items;
  size_t count;
  int complete;
} AbComparisonPopulation;

typedef struct AbComparisonGroup {
  const AbString *left;
  const AbString *const *rights;
  size_t right_count;
  const void *subject;
} AbComparisonGroup;

typedef enum AbComparisonGroupState {
  AB_COMPARISON_GROUP_COMPLETE = 1,
  AB_COMPARISON_GROUP_PARTIAL = 2,
  AB_COMPARISON_GROUP_SOURCE_MISSING = 3
} AbComparisonGroupState;

typedef int (*AbComparisonValueEqual)(const void *left, const void *right);

typedef struct AbComparisonPlan {
  AbComparisonJoin join;
  AbComparisonPopulation left;
  AbComparisonPopulation right;
  const AbComparisonGroup *groups;
  size_t group_count;
  AbComparisonValueEqual value_equal;
  int compare_values;
  int require_left_coverage;
  int require_right_coverage;
} AbComparisonPlan;

typedef struct AbComparisonRecord {
  AbComparisonKind kind;
  const AbComparisonItem *left;
  const AbComparisonItem *right;
  const AbString *left_key;
  const AbString *right_key;
  size_t group_index;
} AbComparisonRecord;

typedef struct AbComparisonGroupResult {
  const AbComparisonGroup *group;
  const AbComparisonItem *left;
  AbComparisonGroupState state;
  size_t record_start;
  size_t record_count;
  size_t matched_count;
  size_t missing_count;
} AbComparisonGroupResult;

typedef struct AbComparisonResult {
  AbComparisonRecord *records;
  size_t count;
  size_t capacity;
  AbComparisonGroupResult *groups;
  size_t group_count;
  int incomplete;
} AbComparisonResult;

ArchbirdStatus ab_comparison_run(ArchbirdEngine *engine,
                                 const AbComparisonPlan *plan,
                                 AbComparisonResult *out);
void ab_comparison_result_free(ArchbirdEngine *engine,
                               AbComparisonResult *result);

#endif
