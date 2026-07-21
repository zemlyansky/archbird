#include "verify_runtime.h"

#include "verify_checks.h"

#include <string.h>

#define VERIFY_RENDER_TRY(expression)                                          \
  do {                                                                         \
    ArchbirdStatus render_status__ = (expression);                             \
    if (render_status__ != ARCHBIRD_OK)                                        \
      return render_status__;                                                  \
  } while (0)

static ArchbirdStatus render_value_or(AbBuffer *buffer, const AbValue *object,
                                      const char *name, const char *fallback) {
  const AbValue *value = ab_value_member(object, name);
  return value ? ab_value_render(buffer, value)
               : ab_buffer_literal(buffer, fallback);
}

static ArchbirdStatus render_path_or_empty(AbBuffer *buffer,
                                           const AbValue *object,
                                           const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value ? ab_verify_render_normalized_path(buffer, value)
               : ab_buffer_literal(buffer, "\"\"");
}

static const AbValue *named_input_row(const AbValue *rows,
                                      const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *row_name = ab_value_member(row, "name");
    if (row_name && row_name->kind == AB_VALUE_STRING &&
        ab_string_equal(&row_name->as.text, name))
      return row;
  }
  return NULL;
}

static ArchbirdStatus render_source_lock(AbVerificationContext *context,
                                         const AbObjectField *project,
                                         AbBuffer *buffer) {
  const AbValue *lock = ab_value_member(&project->value, "source_lock");
  AbVerifySourceLockState overall =
      ab_verify_source_lock_state(context, &project->name);
  size_t index;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"files\":["));
  for (index = 0; lock && index < lock->as.object.count; index++) {
    const AbObjectField *field = &lock->as.object.fields[index];
    char observed[65];
    int available = ab_verify_source_lock_observed_sha256(
        context, &project->name, &field->name, observed);
    const char *state = !available ? "unavailable"
                        : memcmp(observed, field->value.as.text.data, 64) == 0
                            ? "current"
                            : "mismatch";
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"actual_sha256\":"));
    VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, available ? observed : "",
                                            available ? 64 : 0));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"expected_sha256\":"));
    VERIFY_RENDER_TRY(ab_value_render(buffer, &field->value));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, field->name.data, field->name.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"state\":"));
    VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, state, strlen(state)));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"state\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_json_string(buffer, ab_verify_source_lock_state_name(overall),
                            strlen(ab_verify_source_lock_state_name(overall))));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_projects(AbVerificationContext *context,
                                      AbBuffer *buffer) {
  size_t index;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->suite.projects->as.object.count; index++) {
    const AbObjectField *field =
        &context->suite.projects->as.object.fields[index];
    const AbValue *input =
        named_input_row(context->input.projects, &field->name);
    const AbValue *map = input ? ab_value_member(input, "map") : NULL;
    const AbValue *evidence = map ? ab_value_member(map, "evidence") : NULL;
    const AbValue *tool = map ? ab_value_member(map, "tool") : NULL;
    const AbValue *revision = ab_value_member(&field->value, "revision");
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"capabilities\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, &field->value, "capabilities", "[]"));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"config_sha256\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, evidence, "config_sha256", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"input_sha256\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, evidence, "input_sha256", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, field->name.data, field->name.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"parameters\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, &field->value, "parameters", "{}"));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"producer\":{"
                                                "\"implementation_sha256\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, tool, "implementation_sha256", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, tool, "name", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"version\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, tool, "version", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "},\"profile\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, &field->value, "profile", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, map, "project", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"revision\":"));
    VERIFY_RENDER_TRY(
        render_value_or(buffer, &field->value, "revision", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"revision_provenance\":"));
    VERIFY_RENDER_TRY(ab_buffer_json_string(
        buffer, revision ? "asserted" : "not_declared", revision ? 8 : 12));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"source_lock\":"));
    VERIFY_RENDER_TRY(render_source_lock(context, field, buffer));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_relation_rows(AbBuffer *buffer,
                                           const AbValue *extractor) {
  const AbValue *rows = ab_value_member(extractor, "rows");
  size_t index;
  if (!rows)
    return ab_buffer_literal(buffer, "[]");
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"kind\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, row, "kind", "\"*\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"source\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, row, "source", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"target\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, row, "target", "\"\""));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_extractor(AbBuffer *buffer,
                                       const AbObjectField *field) {
  const AbValue *row = &field->value;
  const AbValue *kind = ab_value_member(row, "kind");
  int macro = ab_verify_string_is(kind, "c_macro_set");
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"attribute\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "attribute", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"auto_start\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "auto_start", "1"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"call\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "call", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"class\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "class", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"configured_only\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "configured_only", "false"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"exclude\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "exclude", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"from_paths\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "from_paths", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"group\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "group", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"include\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "include", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  VERIFY_RENDER_TRY(ab_value_render(buffer, kind));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"kinds\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "kinds", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"layer\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "layer", "\"\""));
  if (ab_verify_string_is(kind, "file_metrics")) {
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"metric\":"));
    VERIFY_RENDER_TRY(render_value_or(buffer, row, "metric", "\"\""));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_json_string(buffer, field->name.data, field->name.length));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name_value\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "name", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  VERIFY_RENDER_TRY(render_path_or_empty(buffer, row, "path"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"paths\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "paths", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "project", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"public_only\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "public_only", "false"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"rows\":"));
  VERIFY_RENDER_TRY(render_relation_rows(buffer, row));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"selector\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "selector", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"selector_argument\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "selector_argument", "0"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"selectors\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "selectors", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"strip_prefix\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "strip_prefix", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"strip_suffix\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "strip_suffix", "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"to_paths\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "to_paths", "[]"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"values\":"));
  VERIFY_RENDER_TRY(render_value_or(buffer, row, "values", "null"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"values_from_argument\":"));
  VERIFY_RENDER_TRY(
      render_value_or(buffer, row, "values_from_argument", macro ? "1" : "0"));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_suite_path(AbBuffer *buffer,
                                        const AbValue *value) {
  size_t index;
  size_t segment = 0;
  size_t basename = 0;
  int parent = 0;
  if (!value || value->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index <= value->as.text.length; index++) {
    int separator = index == value->as.text.length ||
                    value->as.text.data[index] == '/' ||
                    value->as.text.data[index] == '\\';
    if (!separator)
      continue;
    if (index - segment == 2 && value->as.text.data[segment] == '.' &&
        value->as.text.data[segment + 1] == '.')
      parent = 1;
    if (index > segment)
      basename = segment;
    segment = index + 1;
  }
  if (parent)
    return ab_buffer_json_string(buffer, value->as.text.data + basename,
                                 value->as.text.length - basename);
  return ab_verify_render_normalized_path(buffer, value);
}

static ArchbirdStatus render_contract(AbVerificationContext *context,
                                      AbBuffer *buffer) {
  size_t index;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"attestations\":["));
  if (context->suite.attestations)
    for (index = 0; index < context->suite.attestations->as.object.count;
         index++) {
      const AbObjectField *field =
          &context->suite.attestations->as.object.fields[index];
      if (index)
        VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"name\":"));
      VERIFY_RENDER_TRY(
          ab_buffer_json_string(buffer, field->name.data, field->name.length));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
      VERIFY_RENDER_TRY(
          render_suite_path(buffer, ab_value_member(&field->value, "path")));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
      VERIFY_RENDER_TRY(
          render_value_or(buffer, &field->value, "project", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
    }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"candidate\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_literal(buffer, context->suite.candidate ? "true" : "false"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"extractors\":["));
  for (index = 0; index < context->suite.extractors->as.object.count; index++) {
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(render_extractor(
        buffer, &context->suite.extractors->as.object.fields[index]));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"mappings\":["));
  if (context->suite.mappings)
    for (index = 0; index < context->suite.mappings->as.object.count; index++) {
      const AbObjectField *field =
          &context->suite.mappings->as.object.fields[index];
      if (index)
        VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"actual_to_expected\":"));
      VERIFY_RENDER_TRY(
          render_value_or(buffer, &field->value, "actual_to_expected", "{}"));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
      VERIFY_RENDER_TRY(
          ab_buffer_json_string(buffer, field->name.data, field->name.length));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
    }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"waivers\":["));
  if (context->suite.waivers)
    for (index = 0; index < context->suite.waivers->as.array.count; index++) {
      const AbValue *row = &context->suite.waivers->as.array.items[index];
      if (index)
        VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"check\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "check", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"comparison\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "comparison", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"expires_on\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "expires_on", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"fingerprint\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "fingerprint", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"id\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "id", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "key", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"owner\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "owner", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"rationale\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "rationale", "\"\""));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"until_inputs\":"));
      VERIFY_RENDER_TRY(render_value_or(buffer, row, "until_inputs", "{}"));
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
    }
  return ab_buffer_literal(buffer, "]}");
}

static int string_equals(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static size_t check_status_count(const AbVerificationContext *context,
                                 const char *status) {
  size_t index;
  size_t count = 0;
  for (index = 0; index < context->check_count; index++)
    if (string_equals(&context->checks[index].status, status))
      count++;
  return count;
}

static size_t finding_field_count(const AbVerificationContext *context,
                                  const char *field, const char *value) {
  size_t check_index;
  size_t count = 0;
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    size_t finding_index;
    for (finding_index = 0; finding_index < check->finding_count;
         finding_index++) {
      const AbVerifyFinding *finding = &check->findings[finding_index];
      const AbString *selected = NULL;
      if (!strcmp(field, "comparison"))
        selected = &finding->comparison;
      else if (!strcmp(field, "evidence_state"))
        selected = &finding->evidence_state;
      else if (!strcmp(field, "applicability"))
        selected = &finding->applicability;
      else if (!strcmp(field, "disposition"))
        selected = &finding->disposition;
      else if (!strcmp(field, "baseline_state"))
        selected = &finding->baseline_state;
      if (selected && string_equals(selected, value))
        count++;
    }
  }
  return count;
}

static size_t finding_total(const AbVerificationContext *context) {
  size_t index;
  size_t count = 0;
  for (index = 0; index < context->check_count; index++)
    count += context->checks[index].finding_count;
  return count;
}

static size_t coverage_total(const AbVerificationContext *context) {
  size_t index;
  size_t count = 0;
  for (index = 0; index < context->check_count; index++)
    count += context->checks[index].coverage.count;
  return count;
}

static int finding_blocks(const AbVerifyFinding *finding,
                          const AbValue *check) {
  const AbValue *severity = ab_value_member(check, "severity");
  return (!severity || ab_verify_string_is(severity, "error")) &&
         string_equals(&finding->applicability, "applicable") &&
         string_equals(&finding->disposition, "open") &&
         (!string_equals(&finding->comparison, "equal") ||
          string_equals(&finding->evidence_state, "unknown") ||
          string_equals(&finding->evidence_state, "stale"));
}

static int verification_blocks(const AbVerificationContext *context) {
  size_t diagnostic_index;
  size_t check_index;
  for (diagnostic_index = 0; diagnostic_index < context->diagnostic_count;
       diagnostic_index++)
    if (string_equals(&context->diagnostics[diagnostic_index].severity,
                      "error"))
      return 1;
  if (context->baseline.coverage_regression_count)
    return 1;
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    size_t finding_index;
    for (finding_index = 0; finding_index < check->finding_count;
         finding_index++)
      if (finding_blocks(&check->findings[finding_index], check->spec)) {
        if (!context->baseline.enabled ||
            string_equals(&check->findings[finding_index].baseline_state,
                          "new") ||
            string_equals(&check->findings[finding_index].baseline_state,
                          "reintroduced"))
          return 1;
      }
  }
  return 0;
}

static ArchbirdStatus render_counter(AbVerificationContext *context,
                                     AbBuffer *buffer, const char *field,
                                     const char *const *values,
                                     size_t value_count, int exclude_none) {
  size_t index;
  size_t emitted = 0;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{"));
  for (index = 0; index < value_count; index++) {
    size_t count;
    if (exclude_none && !strcmp(values[index], "none"))
      continue;
    count = finding_field_count(context, field, values[index]);
    if (!count)
      continue;
    if (emitted++)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, values[index], strlen(values[index])));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ":"));
    VERIFY_RENDER_TRY(ab_buffer_u64(buffer, count));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_summary(AbVerificationContext *context,
                                     AbBuffer *buffer) {
  static const char *const applicability[] = {"applicable", "not_applicable"};
  static const char *const baselines[] = {"known", "new", "none",
                                          "reintroduced"};
  static const char *const comparisons[] = {"different", "equal", "extra",
                                            "missing"};
  static const char *const dispositions[] = {"open", "waived"};
  static const char *const evidence_states[] = {"current", "stale", "unknown"};
  size_t regression_index;
  size_t regression_count = 0;
  for (regression_index = 0;
       regression_index < context->baseline.coverage_regression_count;
       regression_index++)
    regression_count +=
        context->baseline.coverage_regressions[regression_index].values.count;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"blocking\":"));
  VERIFY_RENDER_TRY(ab_buffer_literal(
      buffer, verification_blocks(context) ? "true" : "false"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"checks\":{\"fail\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, check_status_count(context, "fail")));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"not_applicable\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_u64(buffer, check_status_count(context, "not_applicable")));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"pass\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, check_status_count(context, "pass")));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"unknown\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_u64(buffer, check_status_count(context, "unknown")));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"waived\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_u64(buffer, check_status_count(context, "waived")));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "},\"coverage\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, coverage_total(context)));
  VERIFY_RENDER_TRY(ab_buffer_literal(
      buffer, ",\"coverage_aggregation\":\"sum_of_unique_keys_per_check\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"coverage_regressions\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, regression_count));
  VERIFY_RENDER_TRY(ab_buffer_literal(
      buffer, ",\"coverage_unit\":\"check_fact_or_route_key\""));
  VERIFY_RENDER_TRY(
      ab_buffer_literal(buffer, ",\"findings\":{\"applicability\":"));
  VERIFY_RENDER_TRY(
      render_counter(context, buffer, "applicability", applicability, 2, 0));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"baseline_state\":"));
  VERIFY_RENDER_TRY(
      render_counter(context, buffer, "baseline_state", baselines, 4, 1));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"comparison\":"));
  VERIFY_RENDER_TRY(
      render_counter(context, buffer, "comparison", comparisons, 4, 0));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"disposition\":"));
  VERIFY_RENDER_TRY(
      render_counter(context, buffer, "disposition", dispositions, 2, 0));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"evidence_state\":"));
  VERIFY_RENDER_TRY(
      render_counter(context, buffer, "evidence_state", evidence_states, 3, 0));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"total\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, finding_total(context)));
  return ab_buffer_literal(buffer, "}}");
}

static ArchbirdStatus render_baseline(AbVerificationContext *context,
                                      AbBuffer *buffer) {
  const AbVerifyBaselineState *baseline = &context->baseline;
  size_t index;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"active\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, baseline->active_count));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"coverage_regressions\":{"));
  for (index = 0; index < baseline->coverage_regression_count; index++) {
    const AbVerifyCoverageRegression *row =
        &baseline->coverage_regressions[index];
    size_t value_index;
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, row->check.data, row->check.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ":["));
    for (value_index = 0; value_index < row->values.count; value_index++) {
      if (value_index)
        VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
      VERIFY_RENDER_TRY(
          ab_buffer_json_string(buffer, row->values.items[value_index].data,
                                row->values.items[value_index].length));
    }
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "]"));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "},\"enabled\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_literal(buffer, baseline->enabled ? "true" : "false"));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"owner\":"));
  VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, baseline->owner.data,
                                          baseline->owner.length));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"rationale\":"));
  VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, baseline->rationale.data,
                                          baseline->rationale.length));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"resolved\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, baseline->resolved_count));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  VERIFY_RENDER_TRY(
      ab_buffer_json_string(buffer, baseline->enabled ? baseline->sha256 : "",
                            baseline->enabled ? 64 : 0));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_diagnostics(AbVerificationContext *context,
                                         AbBuffer *buffer) {
  size_t index;
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->diagnostic_count; index++) {
    const AbVerifyDiagnostic *row = &context->diagnostics[index];
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "{\"code\":"));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, row->code.data, row->code.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"message\":"));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, row->message.data, row->message.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    VERIFY_RENDER_TRY(
        ab_buffer_json_string(buffer, row->path.data, row->path.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
    VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, row->severity.data,
                                            row->severity.length));
    VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

ArchbirdStatus ab_verify_render_result(AbVerificationContext *context,
                                       AbBuffer *buffer) {
  size_t index;
  if (!context || !context->engine || !buffer)
    return ARCHBIRD_INVALID_ARGUMENT;
  VERIFY_RENDER_TRY(ab_build_identity_validate(context->engine));
  VERIFY_RENDER_TRY(ab_buffer_literal(
      buffer, "{\"artifact\":\"verification\",\"attestations\":"));
  VERIFY_RENDER_TRY(ab_verify_attestations_render(context, buffer));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"baseline\":"));
  VERIFY_RENDER_TRY(render_baseline(context, buffer));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"checks\":["));
  for (index = 0; index < context->check_count; index++) {
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_verify_check_render(buffer, &context->checks[index]));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"contract\":"));
  VERIFY_RENDER_TRY(render_contract(context, buffer));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"diagnostics\":"));
  VERIFY_RENDER_TRY(render_diagnostics(context, buffer));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"facts\":["));
  for (index = 0; index < context->fact_count; index++) {
    if (index)
      VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ","));
    VERIFY_RENDER_TRY(ab_verify_fact_render(buffer, &context->facts[index], 1));
  }
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "],\"projects\":"));
  VERIFY_RENDER_TRY(render_projects(context, buffer));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer,
                                      ",\"schema_version\":1,\"suite\":{"
                                      "\"description\":"));
  VERIFY_RENDER_TRY(context->suite.description
                        ? ab_value_render(buffer, context->suite.description)
                        : ab_buffer_literal(buffer, "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  VERIFY_RENDER_TRY(ab_value_render(buffer, context->suite.name));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"policy_date\":"));
  VERIFY_RENDER_TRY(context->suite.policy_date
                        ? ab_value_render(buffer, context->suite.policy_date)
                        : ab_buffer_literal(buffer, "\"\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  VERIFY_RENDER_TRY(ab_buffer_json_string(buffer, context->suite.sha256, 64));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, "},\"summary\":"));
  VERIFY_RENDER_TRY(render_summary(context, buffer));
  return ab_buffer_literal(
      buffer,
      ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
}

#undef VERIFY_RENDER_TRY
