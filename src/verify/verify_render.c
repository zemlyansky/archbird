#include "verify_runtime.h"

#include "verify_checks.h"

#include <string.h>

#define VERIFY_RENDER_TRY(expression)                                          \
  do {                                                                         \
    ArchbirdStatus render_status__ = (expression);                             \
    if (render_status__ != ARCHBIRD_OK)                                        \
      return render_status__;                                                  \
  } while (0)

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
  return (!severity || ab_projection_value_is(severity, "error")) &&
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

ArchbirdStatus ab_constraints_render_summary(AbVerificationContext *context,
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
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"constraints\":{\"fail\":"));
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
      buffer,
      ",\"coverage_aggregation\":\"sum_of_unique_keys_per_constraint\""));
  VERIFY_RENDER_TRY(ab_buffer_literal(buffer, ",\"coverage_regressions\":"));
  VERIFY_RENDER_TRY(ab_buffer_u64(buffer, regression_count));
  VERIFY_RENDER_TRY(ab_buffer_literal(
      buffer, ",\"coverage_unit\":\"constraint_operand_or_route_key\""));
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

ArchbirdStatus ab_verify_render_diagnostics(AbVerificationContext *context,
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

#undef VERIFY_RENDER_TRY
