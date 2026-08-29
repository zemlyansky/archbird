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

static int discard_write(void *user_data, const uint8_t *bytes, size_t length) {
  size_t *written = (size_t *)user_data;
  (void)bytes;
  *written += length;
  return 0;
}

static int test_sectioned_map_diff(void) {
  static const char map[] =
      "{\"artifact\":\"map\",\"artifacts\":[],\"builds\":[],"
      "\"call_resolutions\":[],\"components\":[],\"diagnostics\":[],"
      "\"edges\":[],\"evidence\":{\"config_sha256\":\"0\","
      "\"input_sha256\":\"0\"},\"facts\":[],\"files\":[],"
      "\"indexes\":[],\"inputs\":[],\"layers\":[],\"limits\":{},"
      "\"named_entries\":{},\"packages\":[],\"parity\":[],"
      "\"project\":\"sectioned\",\"schema_version\":9,\"surfaces\":[],"
      "\"symbol_calls\":[],\"symbol_references\":[],\"tests\":[],"
      "\"tool\":{\"implementation_sha256\":\"0\",\"name\":\"archbird\","
      "\"version\":\"test\"}}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  size_t written = 0;
  archbird_engine_options_init(&options);
  options.max_values = 16;
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_diff(engine, (const uint8_t *)map, sizeof(map) - 1,
                               (const uint8_t *)map, sizeof(map) - 1, 0,
                               discard_write, &written);
  if (status != ARCHBIRD_OK) {
    expect(0,
           engine ? archbird_engine_error(engine) : "engine creation failed");
    archbird_engine_destroy(engine);
    return 0;
  }
  archbird_engine_destroy(engine);
  return expect(written > 0,
                "sectioned Map Diff produced no structural artifact");
}

static int test_sectioned_correspondence_diff(void) {
  static const char map[] =
      "{\"artifact\":\"map\",\"artifacts\":[],\"builds\":[],"
      "\"call_resolutions\":[],\"components\":[],"
      "\"diagnostics\":[0,1,2,3,4,5,6,7],\"edges\":[],"
      "\"evidence\":{\"config_sha256\":"
      "\"0000000000000000000000000000000000000000000000000000000000000000\","
      "\"input_sha256\":"
      "\"0000000000000000000000000000000000000000000000000000000000000000\"},"
      "\"facts\":[],\"files\":[],\"indexes\":[],\"inputs\":[],"
      "\"layers\":[],\"limits\":{},\"named_entries\":{},"
      "\"packages\":[],\"parity\":[],\"project\":\"sectioned\","
      "\"schema_version\":13,\"surfaces\":[],\"symbol_calls\":[],"
      "\"symbol_references\":[],\"tests\":[],"
      "\"tool\":{\"implementation_sha256\":"
      "\"0000000000000000000000000000000000000000000000000000000000000000\","
      "\"name\":\"archbird\",\"version\":\"test\"}}";
  static const char request[] =
      "{\"after\":{\"container\":\"rules.c:rules\","
      "\"id\":\"after-rules\",\"select\":\"ordered_rows\"},"
      "\"before\":{\"container\":\"rules.c:rules\","
      "\"id\":\"before-rules\",\"select\":\"ordered_rows\"},"
      "\"mapping\":{}}";
  static const char general_request[] =
      "{\"after\":{\"id\":\"after-symbols\",\"select\":\"symbols\"},"
      "\"before\":{\"id\":\"before-symbols\",\"select\":\"symbols\"},"
      "\"mapping\":{}}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  size_t written = 0;
  archbird_engine_options_init(&options);
  options.max_values = 32;
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_diff(engine, (const uint8_t *)map, sizeof(map) - 1, NULL,
                           0, (const uint8_t *)map, sizeof(map) - 1, NULL, 0,
                           (const uint8_t *)request, sizeof(request) - 1, 0,
                           discard_write, &written);
  if (status != ARCHBIRD_OK) {
    expect(0,
           engine ? archbird_engine_error(engine) : "engine creation failed");
    archbird_engine_destroy(engine);
    return 0;
  }
  written = 0;
  status =
      archbird_diff(engine, (const uint8_t *)map, sizeof(map) - 1, NULL, 0,
                    (const uint8_t *)map, sizeof(map) - 1, NULL, 0,
                    (const uint8_t *)general_request,
                    sizeof(general_request) - 1, 0, discard_write, &written);
  if (status != ARCHBIRD_OK) {
    expect(0, archbird_engine_error(engine));
    archbird_engine_destroy(engine);
    return 0;
  }
  archbird_engine_destroy(engine);
  return expect(written > 0,
                "sectioned correspondence Diff produced no artifact");
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

static int test_incomplete_correspondence(ArchbirdEngine *engine) {
  static const AbString a = TEXT("a");
  static const AbString b = TEXT("b");
  static const AbString missing = TEXT("missing");
  static const AbString x = TEXT("x");
  static const AbString y = TEXT("y");
  static const AbString z = TEXT("z");
  static const AbComparisonItem left[] = {
      {&a, NULL, NULL},
      {&b, NULL, NULL},
  };
  static const AbComparisonItem right[] = {
      {&x, NULL, NULL},
      {&z, NULL, NULL},
  };
  static const AbString *const a_targets[] = {&x, &y};
  static const AbString *const missing_targets[] = {&x};
  static const AbComparisonGroup groups[] = {
      {&a, a_targets, 2, NULL},
      {&missing, missing_targets, 1, NULL},
  };
  AbComparisonPlan plan = {0};
  AbComparisonResult result;
  int ok;
  plan.join = AB_COMPARISON_CORRESPONDENCE;
  plan.left = (AbComparisonPopulation){left, 2, 0};
  plan.right = (AbComparisonPopulation){right, 2, 0};
  plan.groups = groups;
  plan.group_count = 2;
  plan.require_left_coverage = 1;
  plan.require_right_coverage = 1;
  if (ab_comparison_run(engine, &plan, &result) != ARCHBIRD_OK)
    return expect(0, archbird_engine_error(engine));
  ok = expect(result.incomplete, "partial correspondence remains incomplete") &&
       expect(result.group_count == 2,
              "partial correspondence retains reviewed groups") &&
       expect(result.groups[0].matched_count == 1,
              "partial correspondence retains present match") &&
       expect(result.groups[0].missing_count == 1,
              "partial correspondence records unresolved target count") &&
       expect(result.groups[0].state == AB_COMPARISON_GROUP_PARTIAL,
              "unresolved target keeps group partial") &&
       expect(result.groups[1].state == AB_COMPARISON_GROUP_PARTIAL,
              "absent source in open population is not declared missing") &&
       expect(result.count == 5,
              "partial correspondence retains unresolved witnesses") &&
       expect(result.records[0].kind == AB_COMPARISON_MATCHED,
              "present partial correspondence is matched") &&
       expect(result.records[1].kind == AB_COMPARISON_UNRESOLVED &&
                  result.records[1].left == &left[0] &&
                  result.records[1].right == NULL,
              "absent target in an open population remains unresolved") &&
       expect(result.records[2].kind == AB_COMPARISON_UNRESOLVED &&
                  result.records[2].left == NULL &&
                  result.records[2].right == &right[0],
              "present target remains visible when its source is unresolved") &&
       expect(result.records[3].kind == AB_COMPARISON_LEFT_UNMAPPED,
              "present unmapped source remains definitive") &&
       expect(result.records[4].kind == AB_COMPARISON_UNRESOLVED &&
                  result.records[4].right == &right[1],
              "unmapped target in an open source population is unresolved");
  ab_comparison_result_free(engine, &result);
  return ok;
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  int ok;
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 1;
  ok = test_sectioned_map_diff() && test_sectioned_correspondence_diff() &&
       test_identity(engine) && test_grouped_correspondence(engine) &&
       test_incomplete(engine) && test_incomplete_correspondence(engine);
  archbird_engine_destroy(engine);
  return ok ? 0 : 1;
}
