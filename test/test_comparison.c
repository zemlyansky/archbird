#include "base/comparison.h"

#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

#define TEXT(value)                                                            \
  { (char *)(value), sizeof(value) - 1 }

static int integer_equal(const void *left, const void *right) {
  return *(const int *)left == *(const int *)right;
}

static int expect(int condition, const char *message) {
  if (condition)
    return 1;
  fprintf(stderr, "%s\n", message);
  return 0;
}

static int test_identity(ArchbirdEngine *engine) {
  static const AbString a = TEXT("a");
  static const AbString b = TEXT("b");
  static const AbString c = TEXT("c");
  static const int one = 1;
  static const int two = 2;
  static const int three = 3;
  static const AbComparisonItem left[] = {
      {&a, &one, NULL},
      {&b, &two, NULL},
  };
  static const AbComparisonItem right[] = {
      {&b, &three, NULL},
      {&c, &one, NULL},
  };
  AbComparisonPlan plan = {0};
  AbComparisonResult result;
  int ok;
  plan.join = AB_COMPARISON_IDENTITY;
  plan.left = (AbComparisonPopulation){left, 2, 1};
  plan.right = (AbComparisonPopulation){right, 2, 1};
  plan.compare_values = 1;
  plan.value_equal = integer_equal;
  if (ab_comparison_run(engine, &plan, &result) != ARCHBIRD_OK)
    return expect(0, archbird_engine_error(engine));
  ok = expect(result.count == 3, "identity result count") &&
       expect(result.records[0].kind == AB_COMPARISON_LEFT_ONLY,
              "identity left-only") &&
       expect(result.records[1].kind == AB_COMPARISON_CHANGED,
              "identity changed") &&
       expect(result.records[2].kind == AB_COMPARISON_RIGHT_ONLY,
              "identity right-only");
  ab_comparison_result_free(engine, &result);
  return ok;
}

static int test_grouped_correspondence(ArchbirdEngine *engine) {
  static const AbString a = TEXT("a");
  static const AbString b = TEXT("b");
  static const AbString c = TEXT("c");
  static const AbString missing_source = TEXT("missing-source");
  static const AbString missing_target = TEXT("missing-target");
  static const AbString x = TEXT("x");
  static const AbString y = TEXT("y");
  static const AbString z = TEXT("z");
  static const AbComparisonItem left[] = {
      {&a, NULL, NULL},
      {&b, NULL, NULL},
      {&c, NULL, NULL},
  };
  static const AbComparisonItem right[] = {
      {&x, NULL, NULL},
      {&y, NULL, NULL},
      {&z, NULL, NULL},
  };
  static const AbString *const a_targets[] = {&x, &y};
  static const AbString *const b_targets[] = {&missing_target, &y};
  static const AbString *const missing_targets[] = {&z};
  static const AbComparisonGroup groups[] = {
      {&a, a_targets, 2, NULL},
      {&b, b_targets, 2, NULL},
      {&missing_source, missing_targets, 1, NULL},
  };
  AbComparisonPlan plan = {0};
  AbComparisonResult result;
  int ok;
  plan.join = AB_COMPARISON_CORRESPONDENCE;
  plan.left = (AbComparisonPopulation){left, 3, 1};
  plan.right = (AbComparisonPopulation){right, 3, 1};
  plan.groups = groups;
  plan.group_count = 3;
  plan.require_left_coverage = 1;
  plan.require_right_coverage = 1;
  if (ab_comparison_run(engine, &plan, &result) != ARCHBIRD_OK)
    return expect(0, archbird_engine_error(engine));
  ok = expect(result.group_count == 3, "correspondence group count") &&
       expect(result.groups[0].state == AB_COMPARISON_GROUP_COMPLETE,
              "one-to-many group complete") &&
       expect(result.groups[0].matched_count == 2,
              "one-to-many matched targets") &&
       expect(result.groups[1].state == AB_COMPARISON_GROUP_PARTIAL,
              "missing target makes group partial") &&
       expect(result.groups[1].matched_count == 1,
              "partial group matched count") &&
       expect(result.groups[1].missing_count == 1,
              "partial group missing count") &&
       expect(result.groups[2].state == AB_COMPARISON_GROUP_SOURCE_MISSING,
              "missing source group state");
  ok = expect(result.count == 7, "correspondence record count") && ok;
  ok = expect(result.records[0].kind == AB_COMPARISON_MATCHED,
              "first mapped target") &&
       ok;
  ok = expect(result.records[1].kind == AB_COMPARISON_MATCHED,
              "second mapped target") &&
       ok;
  ok = expect(result.records[2].kind == AB_COMPARISON_MAPPING_RIGHT_MISSING,
              "partial missing target") &&
       ok;
  ok = expect(result.records[3].kind == AB_COMPARISON_MATCHED,
              "shared target remains valid") &&
       ok;
  ok = expect(result.records[4].kind == AB_COMPARISON_MAPPING_LEFT_MISSING,
              "mapping source missing") &&
       ok;
  ok = expect(result.records[5].kind == AB_COMPARISON_LEFT_UNMAPPED,
              "unmapped left population") &&
       ok;
  ok = expect(result.records[6].kind == AB_COMPARISON_RIGHT_UNMAPPED,
              "right target from invalid source is uncovered") &&
       ok;
  ok = expect(ab_string_equal(result.records[6].right_key, &z),
              "invalid source does not cover z") &&
       ok;
  ok = expect(ab_string_equal(result.records[2].right_key, &missing_target),
              "missing target identity retained") &&
       ok;
  ab_comparison_result_free(engine, &result);
  return ok;
}

static int test_incomplete(ArchbirdEngine *engine) {
  static const AbString a = TEXT("a");
  static const AbComparisonItem item[] = {{&a, NULL, NULL}};
  AbComparisonPlan plan = {0};
  AbComparisonResult result;
  int ok;
  plan.join = AB_COMPARISON_IDENTITY;
  plan.left = (AbComparisonPopulation){item, 1, 0};
  plan.right = (AbComparisonPopulation){item, 1, 1};
  if (ab_comparison_run(engine, &plan, &result) != ARCHBIRD_OK)
    return expect(0, archbird_engine_error(engine));
  ok = expect(result.incomplete, "incomplete comparison state") &&
       expect(result.count == 0,
              "incomplete comparison has no classifications") &&
       expect(result.group_count == 0,
              "incomplete comparison has no group classifications");
  ab_comparison_result_free(engine, &result);
  plan.join = (AbComparisonJoin)99;
  if (ab_comparison_run(engine, &plan, &result) == ARCHBIRD_OK) {
    ab_comparison_result_free(engine, &result);
    return expect(0, "incomplete comparison accepted an invalid join");
  }
  return ok;
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  int ok;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 1;
  ok = test_identity(engine) && test_grouped_correspondence(engine) &&
       test_incomplete(engine);
  archbird_engine_destroy(engine);
  return ok ? 0 : 1;
}
