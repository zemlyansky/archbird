#include "verify_checks.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus invalid(AbVerificationContext *context,
                              const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static int name_allowed(const AbString *name, const char *const *allowed,
                        size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    size_t length = strlen(allowed[index]);
    if (name->length == length &&
        (!length || !memcmp(name->data, allowed[index], length)))
      return 1;
  }
  return 0;
}

static int exact_fields(const AbValue *object, const char *const *allowed,
                        size_t count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT ||
      object->as.object.count != count)
    return 0;
  for (index = 0; index < object->as.object.count; index++)
    if (!name_allowed(&object->as.object.fields[index].name, allowed, count))
      return 0;
  return 1;
}

static int subset_fields(const AbValue *object, const char *const *allowed,
                         size_t count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < object->as.object.count; index++)
    if (!name_allowed(&object->as.object.fields[index].name, allowed, count))
      return 0;
  return 1;
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    char byte = value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int string_array_valid(const AbValue *rows, int sha256) {
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    if (row->kind != AB_VALUE_STRING || (sha256 && !lowercase_sha256(row)))
      return 0;
    for (previous = 0; previous < index; previous++)
      if (ab_string_equal(&row->as.text,
                          &rows->as.array.items[previous].as.text))
        return 0;
  }
  return 1;
}

static int finding_rows_valid(const AbValue *rows, const char *identity) {
  const char *fields[] = {identity, "comparison", "fingerprint", "key"};
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *fingerprint;
    size_t field_index;
    if (!exact_fields(row, fields, sizeof(fields) / sizeof(fields[0])))
      return 0;
    fingerprint = ab_value_member(row, "fingerprint");
    if (!lowercase_sha256(fingerprint))
      return 0;
    for (field_index = 0; field_index < sizeof(fields) / sizeof(fields[0]);
         field_index++) {
      const AbValue *field = ab_value_member(row, fields[field_index]);
      if (!field || field->kind != AB_VALUE_STRING)
        return 0;
    }
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&rows->as.array.items[previous], "fingerprint");
      if (ab_string_equal(&fingerprint->as.text, &old->as.text))
        return 0;
    }
  }
  return 1;
}

static const AbValue *coverage_for(const AbValue *coverage,
                                   const AbString *check) {
  size_t low = 0;
  size_t high = coverage ? coverage->as.object.count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbObjectField *field = &coverage->as.object.fields[middle];
    int compared = ab_string_compare(&field->name, check);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &field->value;
  }
  return NULL;
}

static int strings_contain(const AbStringArray *rows, const AbString *value) {
  size_t index;
  for (index = 0; index < rows->count; index++)
    if (ab_string_equal(&rows->items[index], value))
      return 1;
  return 0;
}

static int baseline_contains(const AbValue *rows, const char *field,
                             const AbString *fingerprint) {
  size_t index;
  if (!rows)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *value =
        field ? ab_value_member(&rows->as.array.items[index], field)
              : &rows->as.array.items[index];
    if (value && ab_string_equal(&value->as.text, fingerprint))
      return 1;
  }
  return 0;
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || !memcmp(value->data, literal, length));
}

static int finding_blocks(const AbVerifyFinding *finding,
                          const AbValue *check) {
  const AbValue *severity = ab_value_member(check, "severity");
  return (!severity || ab_projection_value_is(severity, "error")) &&
         string_is(&finding->applicability, "applicable") &&
         string_is(&finding->disposition, "open") &&
         (!string_is(&finding->comparison, "equal") ||
          string_is(&finding->evidence_state, "unknown") ||
          string_is(&finding->evidence_state, "stale"));
}

static ArchbirdStatus replace_literal(ArchbirdEngine *engine, AbString *target,
                                      const char *value) {
  ArchbirdStatus status;
  ab_string_free(engine, target);
  status = ab_string_copy(engine, target, value, strlen(value));
  return status;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus add_regression(AbVerificationContext *context,
                                     const AbString *check,
                                     const AbValue *expected,
                                     const AbStringArray *actual) {
  AbVerifyCoverageRegression *resized;
  AbVerifyCoverageRegression *regression;
  size_t index;
  size_t count = 0;
  for (index = 0; index < expected->as.array.count; index++)
    if (!strings_contain(actual, &expected->as.array.items[index].as.text))
      count++;
  if (!count)
    return ARCHBIRD_OK;
  resized = (AbVerifyCoverageRegression *)ab_realloc(
      context->engine, context->baseline.coverage_regressions,
      (context->baseline.coverage_regression_count + 1) * sizeof(*resized));
  if (!resized)
    return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing coverage regressions");
  context->baseline.coverage_regressions = resized;
  regression = &resized[context->baseline.coverage_regression_count];
  memset(regression, 0, sizeof(*regression));
  if (ab_string_copy(context->engine, &regression->check, check->data,
                     check->length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  regression->values.items = (AbString *)ab_calloc(
      context->engine, count, sizeof(*regression->values.items));
  if (!regression->values.items)
    return archbird_error_set(
        context->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory storing coverage regression values");
  for (index = 0; index < expected->as.array.count; index++) {
    const AbString *value = &expected->as.array.items[index].as.text;
    if (!strings_contain(actual, value)) {
      AbString *target = &regression->values.items[regression->values.count];
      if (ab_string_copy(context->engine, target, value->data, value->length) !=
          ARCHBIRD_OK)
        return ARCHBIRD_OUT_OF_MEMORY;
      regression->values.count++;
    }
  }
  if (regression->values.count > 1)
    qsort(regression->values.items, regression->values.count,
          sizeof(*regression->values.items), string_compare);
  context->baseline.coverage_regression_count++;
  return ARCHBIRD_OK;
}

static int regression_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyCoverageRegression *left =
      (const AbVerifyCoverageRegression *)left_raw;
  const AbVerifyCoverageRegression *right =
      (const AbVerifyCoverageRegression *)right_raw;
  return ab_string_compare(&left->check, &right->check);
}

ArchbirdStatus ab_constraints_apply_baseline(AbVerificationContext *context) {
  static const char *const root_fields[] = {
      "active",    "artifact", "constraint_policy_sha256", "coverage", "owner",
      "rationale", "resolved", "schema_version",           "tool",
  };
  static const char *const tool_fields[] = {
      "implementation_sha256",
      "name",
      "version",
  };
  const AbValue *root;
  const AbValue *schema;
  const AbValue *active;
  const AbValue *resolved;
  const AbValue *coverage;
  const AbValue *tool;
  uint64_t schema_version;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  root = context->baseline_input;
  if (!root || root->kind == AB_VALUE_NULL)
    return ARCHBIRD_OK;
  if (!subset_fields(root, root_fields,
                     sizeof(root_fields) / sizeof(root_fields[0])))
    return invalid(context, "constraint baseline has unknown fields");
  schema = ab_value_member(root, "schema_version");
  if (!ab_value_u64(schema, &schema_version) || schema_version != 1)
    return invalid(context, "constraint baseline schema_version must be 1");
  if (!ab_projection_value_is(ab_value_member(root, "artifact"),
                              "constraint-baseline"))
    return invalid(context, "invalid constraint baseline artifact");
  {
    const AbValue *policy = ab_value_member(root, "constraint_policy_sha256");
    if (!policy || policy->kind != AB_VALUE_STRING ||
        policy->as.text.length != 64 ||
        memcmp(policy->as.text.data, context->constraint_policy_sha256, 64))
      return invalid(context, "constraint baseline policy identity differs");
  }
  if (!ab_projection_nonblank(ab_value_member(root, "owner")) ||
      !ab_projection_nonblank(ab_value_member(root, "rationale")))
    return invalid(context,
                   "verification baseline requires owner and rationale");
  tool = ab_value_member(root, "tool");
  if (!exact_fields(tool, tool_fields, 3) ||
      !ab_projection_value_is(ab_value_member(tool, "name"), "archbird") ||
      !ab_projection_nonblank(ab_value_member(tool, "version")) ||
      !lowercase_sha256(ab_value_member(tool, "implementation_sha256")))
    return invalid(context, "invalid verification baseline producer");
  active = ab_value_member(root, "active");
  resolved = ab_value_member(root, "resolved");
  coverage = ab_value_member(root, "coverage");
  if (!active) {
    static const AbValue empty_active = {AB_VALUE_ARRAY, {.array = {NULL, 0}}};
    active = &empty_active;
  }
  if (!resolved) {
    static const AbValue empty_resolved = {AB_VALUE_ARRAY,
                                           {.array = {NULL, 0}}};
    resolved = &empty_resolved;
  }
  if (!coverage) {
    static const AbValue empty_coverage = {AB_VALUE_OBJECT,
                                           {.object = {NULL, 0}}};
    coverage = &empty_coverage;
  }
  if (!finding_rows_valid(active, "constraint") ||
      !string_array_valid(resolved, 1) || coverage->kind != AB_VALUE_OBJECT)
    return invalid(context, "invalid constraint baseline rows");
  for (index = 0; index < coverage->as.object.count; index++)
    if (!coverage->as.object.fields[index].name.length ||
        !string_array_valid(&coverage->as.object.fields[index].value, 0))
      return invalid(context, "invalid verification baseline coverage");
  {
    AbBuffer canonical;
    uint8_t digest[32];
    ab_buffer_init(&canonical, context->engine);
    status = ab_value_render(&canonical, root);
    if (status == ARCHBIRD_OK)
      status = archbird_sha256(canonical.data, canonical.length, digest);
    if (status == ARCHBIRD_OK)
      archbird_sha256_hex(digest, context->baseline.sha256);
    ab_buffer_free(&canonical);
  }
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(context->engine, &context->baseline.owner,
                            ab_value_member(root, "owner")->as.text.data,
                            ab_value_member(root, "owner")->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(context->engine, &context->baseline.rationale,
                            ab_value_member(root, "rationale")->as.text.data,
                            ab_value_member(root, "rationale")->as.text.length);
  if (status != ARCHBIRD_OK)
    return status;
  context->baseline.enabled = 1;
  context->baseline.active_count = active->as.array.count;
  context->baseline.resolved_count = resolved->as.array.count;
  for (index = 0; status == ARCHBIRD_OK && index < context->check_count;
       index++) {
    AbVerifyCheckResult *result = &context->checks[index];
    const AbValue *id = ab_value_member(result->spec, "id");
    const AbValue *expected_coverage = coverage_for(coverage, &id->as.text);
    size_t finding_index;
    for (finding_index = 0; finding_index < result->finding_count;
         finding_index++) {
      AbVerifyFinding *finding = &result->findings[finding_index];
      const char *state = "none";
      if (finding_blocks(finding, result->spec)) {
        if (baseline_contains(resolved, NULL, &finding->fingerprint))
          state = "reintroduced";
        else if (baseline_contains(active, "fingerprint",
                                   &finding->fingerprint))
          state = "known";
        else
          state = "new";
      }
      status =
          replace_literal(context->engine, &finding->baseline_state, state);
      if (status != ARCHBIRD_OK)
        break;
    }
    if (status == ARCHBIRD_OK && expected_coverage)
      status = add_regression(context, &id->as.text, expected_coverage,
                              &result->coverage);
  }
  if (status == ARCHBIRD_OK && context->baseline.coverage_regression_count > 1)
    qsort(context->baseline.coverage_regressions,
          context->baseline.coverage_regression_count,
          sizeof(*context->baseline.coverage_regressions), regression_compare);
  if (status != ARCHBIRD_OK)
    ab_verify_baseline_free(context->engine, &context->baseline);
  return status;
}

void ab_verify_baseline_free(ArchbirdEngine *engine,
                             AbVerifyBaselineState *baseline) {
  size_t index;
  if (!baseline)
    return;
  ab_string_free(engine, &baseline->owner);
  ab_string_free(engine, &baseline->rationale);
  for (index = 0; baseline->coverage_regressions &&
                  index < baseline->coverage_regression_count;
       index++) {
    AbVerifyCoverageRegression *row = &baseline->coverage_regressions[index];
    size_t value_index;
    ab_string_free(engine, &row->check);
    for (value_index = 0; row->values.items && value_index < row->values.count;
         value_index++)
      ab_string_free(engine, &row->values.items[value_index]);
    ab_free(engine, row->values.items);
  }
  ab_free(engine, baseline->coverage_regressions);
  memset(baseline, 0, sizeof(*baseline));
}
