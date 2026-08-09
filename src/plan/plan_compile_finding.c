#include "plan/plan_compile_internal.h"

#include "base/artifact_validation.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbFindingEntry {
  const AbValue *row;
  size_t input_index;
} AbFindingEntry;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, ArchbirdStatus status,
                              const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "plan compilation: %s", message);
}

int ab_plan_finding_current(const AbValue *finding) {
  return ab_artifact_text_is(field(finding, "applicability"), "applicable") &&
         ab_artifact_text_is(field(finding, "disposition"), "open") &&
         ab_artifact_text_is(field(finding, "evidence_state"), "current");
}

static int entry_compare(const void *left_raw, const void *right_raw) {
  const AbFindingEntry *left = (const AbFindingEntry *)left_raw;
  const AbFindingEntry *right = (const AbFindingEntry *)right_raw;
  const AbString *left_fingerprint = &field(left->row, "fingerprint")->as.text;
  const AbString *right_fingerprint =
      &field(right->row, "fingerprint")->as.text;
  size_t common = left_fingerprint->length < right_fingerprint->length
                      ? left_fingerprint->length
                      : right_fingerprint->length;
  int compared =
      common ? memcmp(left_fingerprint->data, right_fingerprint->data, common)
             : 0;
  if (compared)
    return compared;
  if (left_fingerprint->length != right_fingerprint->length)
    return left_fingerprint->length < right_fingerprint->length ? -1 : 1;
  return left->input_index < right->input_index   ? -1
         : left->input_index > right->input_index ? 1
                                                  : 0;
}

static int same_fingerprint(const AbValue *left, const AbValue *right) {
  const AbValue *left_fingerprint = field(left, "fingerprint");
  const AbValue *right_fingerprint = field(right, "fingerprint");
  return ab_value_equal(left_fingerprint, right_fingerprint);
}

ArchbirdStatus ab_plan_finding_groups_collect(ArchbirdEngine *engine,
                                              const AbValue *findings,
                                              AbPlanFindingGroups *out) {
  AbFindingEntry *entries = NULL;
  size_t index;
  size_t group_count = 0;
  memset(out, 0, sizeof(*out));
  if (!findings || findings->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_OK;
  if (findings->as.array.count > AB_PLAN_COMPILE_MAX_ROWS)
    return invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                   "too many Verification findings");
  if (!findings->as.array.count)
    return ARCHBIRD_OK;
  entries = (AbFindingEntry *)ab_calloc(engine, findings->as.array.count,
                                        sizeof(*entries));
  out->rows = (const AbValue **)ab_calloc(engine, findings->as.array.count,
                                          sizeof(*out->rows));
  out->groups = (AbPlanFindingGroup *)ab_calloc(
      engine, findings->as.array.count, sizeof(*out->groups));
  if (!entries || !out->rows || !out->groups) {
    ab_free(engine, entries);
    ab_plan_finding_groups_free(engine, out);
    return invalid(engine, ARCHBIRD_OUT_OF_MEMORY,
                   "out of memory grouping Verification findings");
  }
  for (index = 0; index < findings->as.array.count; index++) {
    const AbValue *row = &findings->as.array.items[index];
    if (!ab_artifact_sha256(field(row, "fingerprint"))) {
      ab_free(engine, entries);
      ab_plan_finding_groups_free(engine, out);
      return invalid(engine, ARCHBIRD_CONFLICT,
                     "Verification finding has no stable fingerprint");
    }
    entries[index].row = row;
    entries[index].input_index = index;
  }
  qsort(entries, findings->as.array.count, sizeof(*entries), entry_compare);
  for (index = 0; index < findings->as.array.count; index++)
    out->rows[index] = entries[index].row;
  index = 0;
  while (index < findings->as.array.count) {
    AbPlanFindingGroup *group = &out->groups[group_count++];
    const AbValue *first = out->rows[index];
    const AbValue *key = field(first, "key");
    const AbValue *comparison = field(first, "comparison");
    size_t end = index + 1;
    group->rows = &out->rows[index];
    group->representative = first;
    while (end < findings->as.array.count &&
           same_fingerprint(first, out->rows[end])) {
      const AbValue *candidate = out->rows[end];
      if (!key || !comparison ||
          !ab_value_equal(key, field(candidate, "key")) ||
          !ab_value_equal(comparison, field(candidate, "comparison"))) {
        ab_free(engine, entries);
        ab_plan_finding_groups_free(engine, out);
        return invalid(
            engine, ARCHBIRD_CONFLICT,
            "one issue fingerprint identifies distinct Verification issues");
      }
      if (!ab_plan_finding_current(group->representative) &&
          ab_plan_finding_current(candidate))
        group->representative = candidate;
      end++;
    }
    group->count = end - index;
    index = end;
  }
  out->count = group_count;
  ab_free(engine, entries);
  return ARCHBIRD_OK;
}

void ab_plan_finding_groups_free(ArchbirdEngine *engine,
                                 AbPlanFindingGroups *groups) {
  if (!groups)
    return;
  ab_free(engine, groups->groups);
  ab_free(engine, groups->rows);
  memset(groups, 0, sizeof(*groups));
}
