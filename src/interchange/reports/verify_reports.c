#include "verify_reports.h"

#include "verify_checks.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define REPORT_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static ArchbirdStatus render_value_or(AbBuffer *buffer, const AbValue *object,
                                      const char *name, const char *fallback) {
  const AbValue *value = object ? ab_value_member(object, name) : NULL;
  return value ? ab_value_render(buffer, value)
               : ab_buffer_literal(buffer, fallback);
}

static int finding_blocks(const AbVerifyFinding *finding,
                          const AbValue *check) {
  const AbValue *severity = ab_value_member(check, "severity");
  return (!severity || ab_verify_string_is(severity, "error")) &&
         string_literal(&finding->applicability, "applicable") &&
         string_literal(&finding->disposition, "open") &&
         (!string_literal(&finding->comparison, "equal") ||
          string_literal(&finding->evidence_state, "unknown") ||
          string_literal(&finding->evidence_state, "stale"));
}

static ArchbirdStatus render_string_array(AbBuffer *buffer,
                                          const AbValue *array) {
  size_t index;
  REPORT_TRY(ab_buffer_literal(buffer, "["));
  if (array && array->kind == AB_VALUE_STRING) {
    REPORT_TRY(ab_value_render(buffer, array));
  } else if (array)
    for (index = 0; index < array->as.array.count; index++) {
      if (index)
        REPORT_TRY(ab_buffer_literal(buffer, ","));
      REPORT_TRY(ab_value_render(buffer, &array->as.array.items[index]));
    }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_sarif_uri(AbBuffer *buffer,
                                       const AbVerifyEvidence *evidence) {
  if (evidence->project.length && evidence->path.length) {
    AbBuffer uri;
    ArchbirdStatus status;
    ab_buffer_init(&uri, buffer->engine);
    status = ab_buffer_append(&uri, evidence->project.data,
                              evidence->project.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&uri, "/");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&uri, evidence->path.data, evidence->path.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, (const char *)uri.data, uri.length);
    ab_buffer_free(&uri);
    return status;
  }
  return ab_buffer_json_string(
      buffer,
      evidence->path.length ? evidence->path.data : evidence->project.data,
      evidence->path.length ? evidence->path.length : evidence->project.length);
}

static ArchbirdStatus render_prefixed_json_string(AbBuffer *buffer,
                                                  const char *prefix,
                                                  const AbString *value) {
  AbBuffer text;
  ArchbirdStatus status;
  ab_buffer_init(&text, buffer->engine);
  status = ab_buffer_literal(&text, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&text, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(buffer, (const char *)text.data, text.length);
  ab_buffer_free(&text);
  return status;
}

static ArchbirdStatus render_sarif_finding(AbBuffer *buffer,
                                           const AbVerifyCheckResult *check,
                                           const AbVerifyFinding *finding) {
  const AbValue *id = ab_value_member(check->spec, "id");
  const AbValue *severity = ab_value_member(check->spec, "severity");
  const AbValue *owner = ab_value_member(check->spec, "owner");
  const AbValue *requirements = ab_value_member(check->spec, "requirement");
  size_t index;
  size_t locations = 0;
  REPORT_TRY(ab_buffer_literal(buffer, "{\"level\":"));
  REPORT_TRY(severity ? ab_value_render(buffer, severity)
                      : ab_buffer_literal(buffer, "\"error\""));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"message\":{\"text\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->message.data,
                                   finding->message.length));
  REPORT_TRY(ab_buffer_literal(
      buffer, "},\"partialFingerprints\":{\"archbirdFinding/v1\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->fingerprint.data,
                                   finding->fingerprint.length));
  REPORT_TRY(ab_buffer_literal(buffer, "},\"properties\":{"));
  REPORT_TRY(ab_buffer_literal(buffer, "\"applicability\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->applicability.data,
                                   finding->applicability.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"baselineState\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->baseline_state.data,
                                   finding->baseline_state.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"comparison\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->comparison.data,
                                   finding->comparison.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"disposition\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->disposition.data,
                                   finding->disposition.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"evidenceState\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, finding->evidence_state.data,
                                   finding->evidence_state.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  REPORT_TRY(
      ab_buffer_json_string(buffer, finding->key.data, finding->key.length));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"owner\":"));
  REPORT_TRY(owner ? ab_value_render(buffer, owner)
                   : ab_buffer_literal(buffer, "\"\""));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
  REPORT_TRY(render_string_array(buffer, requirements));
  REPORT_TRY(ab_buffer_literal(buffer, "}"));
  for (index = 0; index < finding->evidence_count; index++)
    if (finding->evidence[index].path.length ||
        finding->evidence[index].project.length)
      locations++;
  if (locations) {
    size_t emitted = 0;
    REPORT_TRY(ab_buffer_literal(buffer, ",\"locations\":["));
    for (index = 0; index < finding->evidence_count; index++) {
      const AbVerifyEvidence *evidence = &finding->evidence[index];
      if (!evidence->path.length && !evidence->project.length)
        continue;
      if (emitted++)
        REPORT_TRY(ab_buffer_literal(buffer, ","));
      REPORT_TRY(ab_buffer_literal(
          buffer, "{\"physicalLocation\":{\"artifactLocation\":{"
                  "\"uri\":"));
      REPORT_TRY(render_sarif_uri(buffer, evidence));
      REPORT_TRY(ab_buffer_literal(buffer, ",\"uriBaseId\":\"%SRCROOT%\"}"));
      if (evidence->line) {
        REPORT_TRY(ab_buffer_literal(buffer, ",\"region\":{\"startLine\":"));
        REPORT_TRY(ab_buffer_u64(buffer, evidence->line));
        REPORT_TRY(ab_buffer_literal(buffer, "}"));
      }
      REPORT_TRY(ab_buffer_literal(buffer, "}}"));
    }
    REPORT_TRY(ab_buffer_literal(buffer, "]"));
  }
  if (string_literal(&finding->baseline_state, "known"))
    REPORT_TRY(ab_buffer_literal(buffer, ",\"baselineState\":\"unchanged\""));
  else if (string_literal(&finding->baseline_state, "new") ||
           string_literal(&finding->baseline_state, "reintroduced"))
    REPORT_TRY(ab_buffer_literal(buffer, ",\"baselineState\":\"new\""));
  if (string_literal(&finding->disposition, "waived")) {
    REPORT_TRY(
        ab_buffer_literal(buffer, ",\"suppressions\":[{\"justification\":"));
    REPORT_TRY(render_prefixed_json_string(buffer, "Archbird waiver ",
                                           &finding->waiver));
    REPORT_TRY(ab_buffer_literal(
        buffer, ",\"kind\":\"external\",\"status\":\"accepted\"}]"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, ",\"ruleId\":"));
  REPORT_TRY(ab_value_render(buffer, id));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus
render_sarif_regression(AbBuffer *buffer, const AbVerifyCoverageRegression *row,
                        const AbString *value) {
  AbBuffer message;
  AbBuffer fingerprint;
  uint8_t digest[32];
  char hex[65];
  ArchbirdStatus status;
  ab_buffer_init(&message, buffer->engine);
  ab_buffer_init(&fingerprint, buffer->engine);
  status = ab_buffer_append(&message, row->check.data, row->check.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message,
                               ": previously covered evidence disappeared: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&fingerprint, row->check.data, row->check.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&fingerprint, "\0", 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&fingerprint, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(fingerprint.data, fingerprint.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, hex);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer,
                               "{\"level\":\"error\",\"message\":{\"text\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, (const char *)message.data,
                                   message.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, "},\"partialFingerprints\":{\"archbirdFinding/v1\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, hex, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer,
                               "},\"ruleId\":\"ARCHBIRD-COVERAGE-RATCHET\"}");
  ab_buffer_free(&fingerprint);
  ab_buffer_free(&message);
  return status;
}

ArchbirdStatus ab_verify_render_sarif(AbVerificationContext *context,
                                      AbBuffer *buffer) {
  size_t check_index;
  size_t emitted = 0;
  REPORT_TRY(ab_buffer_literal(
      buffer, "{\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\","
              "\"runs\":[{\"automationDetails\":{\"id\":"));
  REPORT_TRY(ab_value_render(buffer, context->suite.name));
  REPORT_TRY(ab_buffer_literal(buffer, "},\"results\":["));
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    size_t finding_index;
    for (finding_index = 0; finding_index < check->finding_count;
         finding_index++) {
      const AbVerifyFinding *finding = &check->findings[finding_index];
      if (string_literal(&finding->comparison, "equal") &&
          string_literal(&finding->evidence_state, "current"))
        continue;
      if (emitted++)
        REPORT_TRY(ab_buffer_literal(buffer, ","));
      REPORT_TRY(render_sarif_finding(buffer, check, finding));
    }
  }
  for (check_index = 0;
       check_index < context->baseline.coverage_regression_count;
       check_index++) {
    const AbVerifyCoverageRegression *row =
        &context->baseline.coverage_regressions[check_index];
    size_t value_index;
    for (value_index = 0; value_index < row->values.count; value_index++) {
      if (emitted++)
        REPORT_TRY(ab_buffer_literal(buffer, ","));
      REPORT_TRY(render_sarif_regression(buffer, row,
                                         &row->values.items[value_index]));
    }
  }
  REPORT_TRY(ab_buffer_literal(
      buffer, "],\"tool\":{\"driver\":{\"informationUri\":"
              "\"https://github.com/zemlyansky/archbird\",\"name\":"
              "\"Archbird\",\"rules\":["));
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbValue *check = context->checks[check_index].spec;
    if (check_index)
      REPORT_TRY(ab_buffer_literal(buffer, ","));
    REPORT_TRY(ab_buffer_literal(buffer, "{\"id\":"));
    REPORT_TRY(ab_value_render(buffer, ab_value_member(check, "id")));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    REPORT_TRY(ab_value_render(buffer, ab_value_member(check, "assert")));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"properties\":{\"owner\":"));
    REPORT_TRY(render_value_or(buffer, check, "owner", "\"\""));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
    REPORT_TRY(
        render_string_array(buffer, ab_value_member(check, "requirement")));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
    REPORT_TRY(render_value_or(buffer, check, "severity", "\"error\""));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"tags\":"));
    REPORT_TRY(render_string_array(buffer, ab_value_member(check, "tags")));
    REPORT_TRY(ab_buffer_literal(buffer, "},\"shortDescription\":{\"text\":"));
    REPORT_TRY(render_value_or(buffer, check, "rationale", "\"\""));
    REPORT_TRY(ab_buffer_literal(buffer, "}}"));
  }
  if (context->baseline.coverage_regression_count) {
    if (context->check_count)
      REPORT_TRY(ab_buffer_literal(buffer, ","));
    REPORT_TRY(ab_buffer_literal(
        buffer, "{\"id\":\"ARCHBIRD-COVERAGE-RATCHET\",\"name\":"
                "\"coverage-ratchet\",\"shortDescription\":{\"text\":"
                "\"Previously covered evidence must not disappear\"}}"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "],\"semanticVersion\":\""));
  REPORT_TRY(ab_buffer_literal(buffer, ARCHBIRD_VERSION));
  return ab_buffer_literal(buffer, "\"}}}],\"version\":\"2.1.0\"}");
}

static ArchbirdStatus xml_escape(AbBuffer *buffer, const char *data,
                                 size_t length, int attribute) {
  size_t index;
  size_t start = 0;
  for (index = 0; index < length; index++) {
    const char *replacement = NULL;
    if (data[index] == '&')
      replacement = "&amp;";
    else if (data[index] == '<')
      replacement = "&lt;";
    else if (data[index] == '>')
      replacement = "&gt;";
    else if (attribute && data[index] == '"')
      replacement = "&quot;";
    if (!replacement)
      continue;
    REPORT_TRY(ab_buffer_append(buffer, data + start, index - start));
    REPORT_TRY(ab_buffer_literal(buffer, replacement));
    start = index + 1;
  }
  return ab_buffer_append(buffer, data + start, length - start);
}

static ArchbirdStatus xml_attribute(AbBuffer *buffer, const char *name,
                                    const char *data, size_t length) {
  REPORT_TRY(ab_buffer_literal(buffer, " "));
  REPORT_TRY(ab_buffer_literal(buffer, name));
  REPORT_TRY(ab_buffer_literal(buffer, "=\""));
  REPORT_TRY(xml_escape(buffer, data, length, 1));
  return ab_buffer_literal(buffer, "\"");
}

static ArchbirdStatus render_evidence_label(AbBuffer *buffer,
                                            const AbVerifyEvidence *row) {
  if (row->project.length && row->path.length) {
    REPORT_TRY(
        ab_buffer_append(buffer, row->project.data, row->project.length));
    REPORT_TRY(ab_buffer_literal(buffer, ":"));
    REPORT_TRY(ab_buffer_append(buffer, row->path.data, row->path.length));
  } else if (row->project.length) {
    REPORT_TRY(
        ab_buffer_append(buffer, row->project.data, row->project.length));
  } else if (row->path.length) {
    REPORT_TRY(ab_buffer_append(buffer, row->path.data, row->path.length));
  } else {
    REPORT_TRY(
        ab_buffer_append(buffer, row->provenance.data, row->provenance.length));
  }
  if (row->line) {
    REPORT_TRY(ab_buffer_literal(buffer, ":"));
    REPORT_TRY(ab_buffer_u64(buffer, row->line));
  }
  if (row->sha256.length) {
    size_t prefix = row->sha256.length < 16 ? row->sha256.length : 16;
    REPORT_TRY(ab_buffer_literal(buffer, " `"));
    REPORT_TRY(ab_buffer_append(buffer, row->sha256.data, prefix));
    REPORT_TRY(ab_buffer_literal(buffer, "`"));
  }
  if (row->detail.length) {
    REPORT_TRY(ab_buffer_literal(buffer, " — "));
    REPORT_TRY(ab_buffer_append(buffer, row->detail.data, row->detail.length));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_finding_text(AbBuffer *buffer,
                                          const AbVerifyFinding *finding,
                                          int *first) {
  size_t index;
  if (!*first)
    REPORT_TRY(ab_buffer_literal(buffer, "\n"));
  *first = 0;
  REPORT_TRY(ab_buffer_append(buffer, finding->fingerprint.data,
                              finding->fingerprint.length));
  REPORT_TRY(ab_buffer_literal(buffer, " "));
  REPORT_TRY(ab_buffer_append(buffer, finding->comparison.data,
                              finding->comparison.length));
  REPORT_TRY(ab_buffer_literal(buffer, " evidence="));
  REPORT_TRY(ab_buffer_append(buffer, finding->evidence_state.data,
                              finding->evidence_state.length));
  REPORT_TRY(ab_buffer_literal(buffer, " applicability="));
  REPORT_TRY(ab_buffer_append(buffer, finding->applicability.data,
                              finding->applicability.length));
  REPORT_TRY(ab_buffer_literal(buffer, " disposition="));
  REPORT_TRY(ab_buffer_append(buffer, finding->disposition.data,
                              finding->disposition.length));
  REPORT_TRY(ab_buffer_literal(buffer, " baseline="));
  REPORT_TRY(ab_buffer_append(buffer, finding->baseline_state.data,
                              finding->baseline_state.length));
  REPORT_TRY(ab_buffer_literal(buffer, ": "));
  REPORT_TRY(ab_buffer_append(buffer, finding->key.data, finding->key.length));
  REPORT_TRY(ab_buffer_literal(buffer, ": "));
  REPORT_TRY(
      ab_buffer_append(buffer, finding->message.data, finding->message.length));
  for (index = 0; index < finding->evidence_count; index++) {
    REPORT_TRY(ab_buffer_literal(buffer, "\n  "));
    REPORT_TRY(render_evidence_label(buffer, &finding->evidence[index]));
  }
  return ARCHBIRD_OK;
}

static int check_is_skipped(const AbVerifyCheckResult *check) {
  return string_literal(&check->status, "waived") ||
         string_literal(&check->status, "not_applicable");
}

ArchbirdStatus ab_verify_render_junit(AbVerificationContext *context,
                                      AbBuffer *buffer) {
  size_t check_index;
  size_t regression_index;
  size_t failures = 0;
  size_t errors = 0;
  size_t skipped = 0;
  size_t tests = context->check_count;
  const AbString *suite_name = &context->suite.name->as.text;
  for (regression_index = 0;
       regression_index < context->baseline.coverage_regression_count;
       regression_index++)
    tests++;
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    size_t index;
    int any_blocking = 0;
    int any_active = 0;
    int any_unknown = 0;
    for (index = 0; index < check->finding_count; index++) {
      const AbVerifyFinding *finding = &check->findings[index];
      int active;
      if (!finding_blocks(finding, check->spec))
        continue;
      any_blocking = 1;
      active = !context->baseline.enabled ||
               string_literal(&finding->baseline_state, "new") ||
               string_literal(&finding->baseline_state, "reintroduced");
      if (!active)
        continue;
      any_active = 1;
      if (string_literal(&finding->evidence_state, "unknown") ||
          string_literal(&finding->evidence_state, "stale"))
        any_unknown = 1;
    }
    if (any_unknown)
      errors++;
    else if (any_active)
      failures++;
    else if (any_blocking || check_is_skipped(check))
      skipped++;
  }
  failures += context->baseline.coverage_regression_count;
  REPORT_TRY(ab_buffer_literal(buffer, "<testsuite"));
  REPORT_TRY(
      xml_attribute(buffer, "name", suite_name->data, suite_name->length));
  {
    AbBuffer number;
    ab_buffer_init(&number, context->engine);
    REPORT_TRY(ab_buffer_u64(&number, tests));
    REPORT_TRY(xml_attribute(buffer, "tests", (const char *)number.data,
                             number.length));
    number.length = 0;
    REPORT_TRY(ab_buffer_u64(&number, failures));
    REPORT_TRY(xml_attribute(buffer, "failures", (const char *)number.data,
                             number.length));
    number.length = 0;
    REPORT_TRY(ab_buffer_u64(&number, errors));
    REPORT_TRY(xml_attribute(buffer, "errors", (const char *)number.data,
                             number.length));
    number.length = 0;
    REPORT_TRY(ab_buffer_u64(&number, skipped));
    REPORT_TRY(xml_attribute(buffer, "skipped", (const char *)number.data,
                             number.length));
    ab_buffer_free(&number);
  }
  REPORT_TRY(ab_buffer_literal(buffer, ">\n  <properties>\n"));
  REPORT_TRY(ab_buffer_literal(
      buffer, "    <property name=\"archbird.version\" value=\""));
  REPORT_TRY(ab_buffer_literal(buffer, ARCHBIRD_VERSION));
  REPORT_TRY(ab_buffer_literal(buffer, "\" />\n"));
  REPORT_TRY(ab_buffer_literal(
      buffer, "    <property name=\"archbird.implementation_sha256\" "
              "value=\"" ARCHBIRD_IMPLEMENTATION_SHA256 "\" />\n"));
  REPORT_TRY(ab_buffer_literal(
      buffer, "    <property name=\"archbird.suite_sha256\" value=\""));
  REPORT_TRY(ab_buffer_append(buffer, context->suite.sha256, 64));
  REPORT_TRY(ab_buffer_literal(buffer, "\" />\n  </properties>"));
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    const AbValue *id = ab_value_member(check->spec, "id");
    const AbValue *assertion = ab_value_member(check->spec, "assert");
    const AbValue *owner = ab_value_member(check->spec, "owner");
    size_t index;
    int any_blocking = 0;
    int any_active = 0;
    int any_unknown = 0;
    int first = 1;
    REPORT_TRY(ab_buffer_literal(buffer, "\n  <testcase"));
    REPORT_TRY(xml_attribute(buffer, "classname", suite_name->data,
                             suite_name->length));
    REPORT_TRY(
        xml_attribute(buffer, "name", id->as.text.data, id->as.text.length));
    REPORT_TRY(ab_buffer_literal(buffer, ">"));
    for (index = 0; index < check->finding_count; index++) {
      const AbVerifyFinding *finding = &check->findings[index];
      int active;
      if (!finding_blocks(finding, check->spec))
        continue;
      any_blocking = 1;
      active = !context->baseline.enabled ||
               string_literal(&finding->baseline_state, "new") ||
               string_literal(&finding->baseline_state, "reintroduced");
      if (!active)
        continue;
      any_active = 1;
      if (string_literal(&finding->evidence_state, "unknown") ||
          string_literal(&finding->evidence_state, "stale"))
        any_unknown = 1;
    }
    if (any_unknown || any_active) {
      const char *tag = any_unknown ? "error" : "failure";
      AbBuffer text;
      ab_buffer_init(&text, context->engine);
      for (index = 0; index < check->finding_count; index++) {
        const AbVerifyFinding *finding = &check->findings[index];
        int active = finding_blocks(finding, check->spec) &&
                     (!context->baseline.enabled ||
                      string_literal(&finding->baseline_state, "new") ||
                      string_literal(&finding->baseline_state, "reintroduced"));
        int unknown = string_literal(&finding->evidence_state, "unknown") ||
                      string_literal(&finding->evidence_state, "stale");
        if (!active || (any_unknown && !unknown))
          continue;
        REPORT_TRY(render_finding_text(&text, finding, &first));
      }
      if (any_unknown)
        for (index = 0; index < check->finding_count; index++) {
          const AbVerifyFinding *finding = &check->findings[index];
          int active =
              finding_blocks(finding, check->spec) &&
              (!context->baseline.enabled ||
               string_literal(&finding->baseline_state, "new") ||
               string_literal(&finding->baseline_state, "reintroduced"));
          int unknown = string_literal(&finding->evidence_state, "unknown") ||
                        string_literal(&finding->evidence_state, "stale");
          if (active && !unknown)
            REPORT_TRY(render_finding_text(&text, finding, &first));
        }
      REPORT_TRY(ab_buffer_literal(buffer, "\n    <"));
      REPORT_TRY(ab_buffer_literal(buffer, tag));
      REPORT_TRY(ab_buffer_literal(buffer, " message=\""));
      REPORT_TRY(
          xml_escape(buffer, check->status.data, check->status.length, 1));
      REPORT_TRY(ab_buffer_literal(buffer, "\">"));
      REPORT_TRY(xml_escape(buffer, (const char *)text.data, text.length, 0));
      REPORT_TRY(ab_buffer_literal(buffer, "</"));
      REPORT_TRY(ab_buffer_literal(buffer, tag));
      REPORT_TRY(ab_buffer_literal(buffer, ">"));
      ab_buffer_free(&text);
    } else if (any_blocking || check_is_skipped(check)) {
      AbBuffer text;
      const char *reason = any_blocking ? "known baseline finding" : NULL;
      ab_buffer_init(&text, context->engine);
      first = 1;
      for (index = 0; index < check->finding_count; index++)
        if ((any_blocking &&
             finding_blocks(&check->findings[index], check->spec)) ||
            (!any_blocking))
          REPORT_TRY(
              render_finding_text(&text, &check->findings[index], &first));
      REPORT_TRY(ab_buffer_literal(buffer, "\n    <skipped message=\""));
      if (reason)
        REPORT_TRY(ab_buffer_literal(buffer, reason));
      else
        REPORT_TRY(
            xml_escape(buffer, check->status.data, check->status.length, 1));
      REPORT_TRY(ab_buffer_literal(buffer, "\">"));
      REPORT_TRY(xml_escape(buffer, (const char *)text.data, text.length, 0));
      REPORT_TRY(ab_buffer_literal(buffer, "</skipped>"));
      ab_buffer_free(&text);
    }
    REPORT_TRY(ab_buffer_literal(buffer, "\n    <system-out>assert="));
    REPORT_TRY(xml_escape(buffer, assertion->as.text.data,
                          assertion->as.text.length, 0));
    REPORT_TRY(ab_buffer_literal(buffer, " owner="));
    if (owner)
      REPORT_TRY(
          xml_escape(buffer, owner->as.text.data, owner->as.text.length, 0));
    REPORT_TRY(ab_buffer_literal(buffer, " status="));
    REPORT_TRY(xml_escape(buffer, check->status.data, check->status.length, 0));
    REPORT_TRY(ab_buffer_literal(buffer, " coverage="));
    REPORT_TRY(ab_buffer_u64(buffer, check->coverage.count));
    REPORT_TRY(ab_buffer_literal(buffer, "</system-out>\n  </testcase>"));
  }
  for (regression_index = 0;
       regression_index < context->baseline.coverage_regression_count;
       regression_index++) {
    const AbVerifyCoverageRegression *row =
        &context->baseline.coverage_regressions[regression_index];
    size_t value_index;
    REPORT_TRY(ab_buffer_literal(buffer, "\n  <testcase"));
    REPORT_TRY(xml_attribute(buffer, "classname", suite_name->data,
                             suite_name->length));
    REPORT_TRY(ab_buffer_literal(buffer, " name=\"coverage:"));
    REPORT_TRY(xml_escape(buffer, row->check.data, row->check.length, 1));
    REPORT_TRY(ab_buffer_literal(
        buffer, "\">\n    <failure message=\"coverage regression\">"));
    for (value_index = 0; value_index < row->values.count; value_index++) {
      if (value_index)
        REPORT_TRY(ab_buffer_literal(buffer, "\n"));
      REPORT_TRY(xml_escape(buffer, row->values.items[value_index].data,
                            row->values.items[value_index].length, 0));
    }
    REPORT_TRY(ab_buffer_literal(buffer, "</failure>\n  </testcase>"));
  }
  return ab_buffer_literal(buffer, "\n</testsuite>\n");
}

typedef struct MarkdownCheckView {
  const AbVerifyFinding **findings;
  size_t count;
  size_t visible;
} MarkdownCheckView;

static int markdown_state_rank(const AbString *value) {
  if (string_literal(value, "stale"))
    return 0;
  if (string_literal(value, "unknown"))
    return 1;
  if (string_literal(value, "current"))
    return 2;
  return 3;
}

static int markdown_comparison_rank(const AbString *value) {
  if (string_literal(value, "missing"))
    return 0;
  if (string_literal(value, "different"))
    return 1;
  if (string_literal(value, "extra"))
    return 2;
  if (string_literal(value, "equal"))
    return 3;
  return 4;
}

static int markdown_finding_compare(const void *left_raw,
                                    const void *right_raw) {
  const AbVerifyFinding *left = *(const AbVerifyFinding *const *)left_raw;
  const AbVerifyFinding *right = *(const AbVerifyFinding *const *)right_raw;
  int compared = (string_literal(&left->disposition, "open") ? 0 : 1) -
                 (string_literal(&right->disposition, "open") ? 0 : 1);
  if (!compared)
    compared = (string_literal(&left->applicability, "applicable") ? 0 : 1) -
               (string_literal(&right->applicability, "applicable") ? 0 : 1);
  if (!compared)
    compared = markdown_state_rank(&left->evidence_state) -
               markdown_state_rank(&right->evidence_state);
  if (!compared)
    compared = markdown_comparison_rank(&left->comparison) -
               markdown_comparison_rank(&right->comparison);
  if (!compared)
    compared = ab_string_compare(&left->key, &right->key);
  return compared;
}

static void markdown_views_free(ArchbirdEngine *engine,
                                MarkdownCheckView *views, size_t count) {
  size_t index;
  if (!views)
    return;
  for (index = 0; index < count; index++)
    ab_free(engine, views[index].findings);
  ab_free(engine, views);
}

static const AbValue *named_project_input(const AbVerificationContext *context,
                                          const AbString *name) {
  size_t index;
  for (index = 0; index < context->input.projects->as.array.count; index++) {
    const AbValue *row = &context->input.projects->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "name");
    if (candidate && ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

static int report_verification_blocks(const AbVerificationContext *context) {
  size_t index;
  for (index = 0; index < context->diagnostic_count; index++)
    if (string_literal(&context->diagnostics[index].severity, "error"))
      return 1;
  if (context->baseline.coverage_regression_count)
    return 1;
  for (index = 0; index < context->check_count; index++) {
    size_t finding_index;
    for (finding_index = 0;
         finding_index < context->checks[index].finding_count;
         finding_index++) {
      const AbVerifyFinding *finding =
          &context->checks[index].findings[finding_index];
      if (finding_blocks(finding, context->checks[index].spec) &&
          (!context->baseline.enabled ||
           string_literal(&finding->baseline_state, "new") ||
           string_literal(&finding->baseline_state, "reintroduced")))
        return 1;
    }
  }
  return 0;
}

static ArchbirdStatus markdown_upper(AbBuffer *buffer, const AbString *value) {
  size_t index;
  for (index = 0; index < value->length; index++) {
    char byte = value->data[index];
    if (byte >= 'a' && byte <= 'z')
      byte = (char)(byte - 'a' + 'A');
    REPORT_TRY(ab_buffer_append(buffer, &byte, 1));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus markdown_string_list(AbBuffer *buffer,
                                           const AbValue *value) {
  size_t index;
  if (!value)
    return ARCHBIRD_OK;
  if (value->kind == AB_VALUE_STRING)
    return ab_buffer_append(buffer, value->as.text.data, value->as.text.length);
  for (index = 0; index < value->as.array.count; index++) {
    if (index)
      REPORT_TRY(ab_buffer_literal(buffer, ","));
    REPORT_TRY(ab_buffer_append(buffer,
                                value->as.array.items[index].as.text.data,
                                value->as.array.items[index].as.text.length));
  }
  return ARCHBIRD_OK;
}

static size_t markdown_field_count(const AbVerificationContext *context,
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
      else if (!strcmp(field, "disposition"))
        selected = &finding->disposition;
      if (selected && string_literal(selected, value))
        count++;
    }
  }
  return count;
}

static ArchbirdStatus
markdown_counter_line(AbVerificationContext *context, AbBuffer *buffer,
                      const char *prefix, const char *field,
                      const char *const *values, size_t value_count) {
  size_t index;
  size_t emitted = 0;
  REPORT_TRY(ab_buffer_literal(buffer, prefix));
  for (index = 0; index < value_count; index++) {
    size_t count = markdown_field_count(context, field, values[index]);
    if (!count)
      continue;
    if (emitted++)
      REPORT_TRY(ab_buffer_literal(buffer, " "));
    REPORT_TRY(ab_buffer_literal(buffer, values[index]));
    REPORT_TRY(ab_buffer_literal(buffer, "="));
    REPORT_TRY(ab_buffer_u64(buffer, count));
  }
  if (!emitted)
    REPORT_TRY(ab_buffer_literal(buffer, "none"));
  return ab_buffer_literal(buffer, "\n");
}

ArchbirdStatus ab_verify_render_markdown(AbVerificationContext *context,
                                         AbBuffer *buffer,
                                         size_t max_findings) {
  static const char *const statuses[] = {"pass", "fail", "unknown", "waived",
                                         "not_applicable"};
  static const char *const comparisons[] = {"different", "equal", "extra",
                                            "missing"};
  static const char *const evidence_states[] = {"current", "stale", "unknown"};
  static const char *const dispositions[] = {"open", "waived"};
  MarkdownCheckView *views = NULL;
  size_t check_index;
  size_t coverage = 0;
  size_t regressions = 0;
  size_t remaining = max_findings;
  ArchbirdStatus status = ARCHBIRD_OK;
#define MD_TRY(expression)                                                     \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto cleanup;                                                            \
  } while (0)

  if (context->check_count) {
    views = (MarkdownCheckView *)ab_calloc(
        context->engine, context->check_count, sizeof(*views));
    if (!views)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory ranking Markdown findings");
  }
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    size_t index;
    views[check_index].count = check->finding_count;
    if (check->finding_count) {
      views[check_index].findings = (const AbVerifyFinding **)ab_calloc(
          context->engine, check->finding_count,
          sizeof(*views[check_index].findings));
      if (!views[check_index].findings) {
        status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory ranking Markdown findings");
        goto cleanup;
      }
    }
    for (index = 0; index < check->finding_count; index++)
      views[check_index].findings[index] = &check->findings[index];
    if (check->finding_count > 1)
      qsort(views[check_index].findings, check->finding_count,
            sizeof(*views[check_index].findings), markdown_finding_compare);
    if (max_findings == SIZE_MAX)
      views[check_index].visible = check->finding_count;
  }
  while (max_findings != SIZE_MAX && remaining) {
    int progressed = 0;
    for (check_index = 0; check_index < context->check_count && remaining;
         check_index++) {
      if (views[check_index].visible >= views[check_index].count)
        continue;
      views[check_index].visible++;
      remaining--;
      progressed = 1;
    }
    if (!progressed)
      break;
  }

  MD_TRY(ab_buffer_literal(buffer, "# Architecture verification: "));
  MD_TRY(ab_buffer_append(buffer, context->suite.name->as.text.data,
                          context->suite.name->as.text.length));
  MD_TRY(ab_buffer_literal(buffer, "\n\n"));
  if (context->suite.description) {
    MD_TRY(ab_buffer_append(buffer, context->suite.description->as.text.data,
                            context->suite.description->as.text.length));
    MD_TRY(ab_buffer_literal(buffer, "\n\n"));
  }
  MD_TRY(
      ab_buffer_literal(buffer, "Evidence: Archbird " ARCHBIRD_VERSION " `"));
  MD_TRY(ab_buffer_append(buffer, ARCHBIRD_IMPLEMENTATION_SHA256, 16));
  MD_TRY(ab_buffer_literal(buffer, "`; suite `"));
  MD_TRY(ab_buffer_append(buffer, context->suite.sha256, 16));
  MD_TRY(ab_buffer_literal(buffer, "`; projects="));
  MD_TRY(ab_buffer_u64(buffer, context->suite.projects->as.object.count));
  MD_TRY(ab_buffer_literal(buffer, "; checks="));
  MD_TRY(ab_buffer_u64(buffer, context->check_count));
  MD_TRY(ab_buffer_literal(buffer, "; blocking="));
  MD_TRY(ab_buffer_literal(buffer,
                           report_verification_blocks(context) ? "yes" : "no"));
  MD_TRY(ab_buffer_literal(
      buffer,
      ".\n\nStatic facts are derived; contracts/waivers are asserted; "
      "attestations "
      "are observed. Unknown, stale, and not-applicable evidence is never "
      "rendered as pass.\n\n## Verdict summary\n\n```text\nchecks "));
  for (check_index = 0; check_index < sizeof(statuses) / sizeof(statuses[0]);
       check_index++) {
    size_t row;
    size_t count = 0;
    for (row = 0; row < context->check_count; row++)
      if (string_literal(&context->checks[row].status, statuses[check_index]))
        count++;
    if (check_index)
      MD_TRY(ab_buffer_literal(buffer, " "));
    MD_TRY(ab_buffer_literal(buffer, statuses[check_index]));
    MD_TRY(ab_buffer_literal(buffer, "="));
    MD_TRY(ab_buffer_u64(buffer, count));
  }
  MD_TRY(ab_buffer_literal(buffer, "\n"));
  MD_TRY(markdown_counter_line(context, buffer, "findings ", "comparison",
                               comparisons, 4));
  MD_TRY(markdown_counter_line(context, buffer, "evidence ", "evidence_state",
                               evidence_states, 3));
  MD_TRY(markdown_counter_line(context, buffer, "policy ", "disposition",
                               dispositions, 2));
  for (check_index = 0; check_index < context->check_count; check_index++)
    coverage += context->checks[check_index].coverage.count;
  for (check_index = 0;
       check_index < context->baseline.coverage_regression_count; check_index++)
    regressions +=
        context->baseline.coverage_regressions[check_index].values.count;
  MD_TRY(ab_buffer_literal(buffer, "coverage_keys="));
  MD_TRY(ab_buffer_u64(buffer, coverage));
  MD_TRY(ab_buffer_literal(
      buffer, " unit=check_fact_or_route_key "
              "aggregation=sum_of_unique_keys_per_check regressions="));
  MD_TRY(ab_buffer_u64(buffer, regressions));
  MD_TRY(ab_buffer_literal(buffer, "\n```\n\n## Project targets\n\n```text\n"));
  for (check_index = 0; check_index < context->suite.projects->as.object.count;
       check_index++) {
    const AbObjectField *spec =
        &context->suite.projects->as.object.fields[check_index];
    const AbValue *input = named_project_input(context, &spec->name);
    const AbValue *map = ab_value_member(input, "map");
    const AbValue *evidence = ab_value_member(map, "evidence");
    const AbValue *revision = ab_value_member(&spec->value, "revision");
    const AbValue *profile = ab_value_member(&spec->value, "profile");
    MD_TRY(ab_buffer_append(buffer, spec->name.data, spec->name.length));
    MD_TRY(ab_buffer_literal(buffer, ": project="));
    MD_TRY(ab_buffer_append(buffer,
                            ab_value_member(map, "project")->as.text.data,
                            ab_value_member(map, "project")->as.text.length));
    if (revision && revision->as.text.length) {
      MD_TRY(ab_buffer_literal(buffer, " declared_revision="));
      MD_TRY(ab_buffer_append(buffer, revision->as.text.data,
                              revision->as.text.length));
      MD_TRY(ab_buffer_literal(buffer, " revision_provenance=asserted"));
    }
    if (profile && profile->as.text.length) {
      MD_TRY(ab_buffer_literal(buffer, " profile="));
      MD_TRY(ab_buffer_append(buffer, profile->as.text.data,
                              profile->as.text.length));
    }
    MD_TRY(ab_buffer_literal(buffer, " config="));
    MD_TRY(ab_buffer_append(
        buffer, ab_value_member(evidence, "config_sha256")->as.text.data, 16));
    MD_TRY(ab_buffer_literal(buffer, " inputs="));
    MD_TRY(ab_buffer_append(
        buffer, ab_value_member(evidence, "input_sha256")->as.text.data, 16));
    MD_TRY(ab_buffer_literal(buffer, " source_lock="));
    MD_TRY(ab_buffer_literal(
        buffer, ab_verify_source_lock_state_name(
                    ab_verify_source_lock_state(context, &spec->name))));
    MD_TRY(ab_buffer_literal(buffer, "\n"));
    {
      const AbValue *lock = ab_value_member(&spec->value, "source_lock");
      size_t lock_index;
      for (lock_index = 0; lock && lock_index < lock->as.object.count;
           lock_index++) {
        const AbObjectField *locked = &lock->as.object.fields[lock_index];
        char observed[65];
        int available = ab_verify_source_lock_observed_sha256(
            context, &spec->name, &locked->name, observed);
        int current =
            available && memcmp(observed, locked->value.as.text.data, 64) == 0;
        if (current)
          continue;
        MD_TRY(ab_buffer_literal(buffer, "  lock path="));
        MD_TRY(
            ab_buffer_append(buffer, locked->name.data, locked->name.length));
        MD_TRY(ab_buffer_literal(buffer, " state="));
        MD_TRY(
            ab_buffer_literal(buffer, available ? "mismatch" : "unavailable"));
        MD_TRY(ab_buffer_literal(buffer, " expected="));
        MD_TRY(ab_buffer_append(buffer, locked->value.as.text.data, 64));
        MD_TRY(ab_buffer_literal(buffer, " actual="));
        if (available)
          MD_TRY(ab_buffer_append(buffer, observed, 64));
        else
          MD_TRY(ab_buffer_literal(buffer, "unavailable"));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
    }
  }
  MD_TRY(ab_buffer_literal(buffer, "```\n\n"));

  if (context->attestation_count) {
    MD_TRY(ab_buffer_literal(buffer, "## Observed evidence\n\n```text\n"));
    for (check_index = 0; check_index < context->attestation_count;
         check_index++) {
      const AbVerifyAttestationState *row = &context->attestations[check_index];
      MD_TRY(ab_buffer_append(buffer, row->name.data, row->name.length));
      MD_TRY(ab_buffer_literal(buffer, ": state="));
      MD_TRY(ab_buffer_append(buffer, row->state.data, row->state.length));
      if (!row->has_data) {
        MD_TRY(ab_buffer_literal(buffer, " cases=0 whole_map=no\n"));
      } else {
        MD_TRY(ab_buffer_literal(buffer, " profile="));
        MD_TRY(ab_buffer_append(buffer, row->data.profile->data,
                                row->data.profile->length));
        MD_TRY(ab_buffer_literal(buffer, " cases="));
        MD_TRY(ab_buffer_u64(buffer, row->data.case_count));
        MD_TRY(ab_buffer_literal(buffer, " whole_map="));
        MD_TRY(
            ab_buffer_literal(buffer, row->whole_map_matches ? "yes" : "no"));
        MD_TRY(ab_buffer_literal(buffer, " closure="));
        MD_TRY(ab_buffer_append(buffer, row->data.evidence_slice_sha256->data,
                                16));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
      if (row->message.length) {
        MD_TRY(ab_buffer_literal(buffer, "  "));
        MD_TRY(
            ab_buffer_append(buffer, row->message.data, row->message.length));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
    }
    MD_TRY(ab_buffer_literal(buffer, "```\n\n"));
  }

  if (context->baseline.enabled) {
    MD_TRY(ab_buffer_literal(buffer, "## Baseline ratchet\n\nBaseline `"));
    MD_TRY(ab_buffer_append(buffer, context->baseline.sha256, 16));
    MD_TRY(ab_buffer_literal(buffer, "`; known active="));
    MD_TRY(ab_buffer_u64(buffer, context->baseline.active_count));
    MD_TRY(ab_buffer_literal(buffer, "; resolved history="));
    MD_TRY(ab_buffer_u64(buffer, context->baseline.resolved_count));
    MD_TRY(ab_buffer_literal(buffer, ".\nOwner="));
    MD_TRY(ab_buffer_append(buffer, context->baseline.owner.data,
                            context->baseline.owner.length));
    MD_TRY(ab_buffer_literal(buffer, "; rationale: "));
    MD_TRY(ab_buffer_append(buffer, context->baseline.rationale.data,
                            context->baseline.rationale.length));
    MD_TRY(ab_buffer_literal(buffer, "\n\n"));
    for (check_index = 0;
         check_index < context->baseline.coverage_regression_count;
         check_index++) {
      const AbVerifyCoverageRegression *row =
          &context->baseline.coverage_regressions[check_index];
      size_t value_index;
      MD_TRY(ab_buffer_literal(buffer, "- **COVERAGE REGRESSION "));
      MD_TRY(ab_buffer_append(buffer, row->check.data, row->check.length));
      MD_TRY(ab_buffer_literal(buffer, "**: "));
      for (value_index = 0; value_index < row->values.count; value_index++) {
        if (value_index)
          MD_TRY(ab_buffer_literal(buffer, ", "));
        MD_TRY(ab_buffer_append(buffer, row->values.items[value_index].data,
                                row->values.items[value_index].length));
      }
      MD_TRY(ab_buffer_literal(buffer, "\n"));
    }
    if (context->baseline.coverage_regression_count)
      MD_TRY(ab_buffer_literal(buffer, "\n"));
  }

  if (context->diagnostic_count) {
    MD_TRY(ab_buffer_literal(buffer, "## Verification diagnostics\n\n"));
    for (check_index = 0; check_index < context->diagnostic_count;
         check_index++) {
      const AbVerifyDiagnostic *row = &context->diagnostics[check_index];
      MD_TRY(ab_buffer_literal(buffer, "- **"));
      MD_TRY(markdown_upper(buffer, &row->severity));
      MD_TRY(ab_buffer_literal(buffer, " "));
      MD_TRY(ab_buffer_append(buffer, row->code.data, row->code.length));
      MD_TRY(ab_buffer_literal(buffer, "**"));
      if (row->path.length) {
        MD_TRY(ab_buffer_literal(buffer, " `"));
        MD_TRY(ab_buffer_append(buffer, row->path.data, row->path.length));
        MD_TRY(ab_buffer_literal(buffer, "`"));
      }
      MD_TRY(ab_buffer_literal(buffer, ": "));
      MD_TRY(ab_buffer_append(buffer, row->message.data, row->message.length));
      MD_TRY(ab_buffer_literal(buffer, "\n"));
    }
    MD_TRY(ab_buffer_literal(buffer, "\n"));
  }

  MD_TRY(ab_buffer_literal(buffer, "## Checks\n\n"));
  for (check_index = 0; check_index < context->check_count; check_index++) {
    const AbVerifyCheckResult *check = &context->checks[check_index];
    const AbValue *id = ab_value_member(check->spec, "id");
    const AbValue *assertion = ab_value_member(check->spec, "assert");
    const AbValue *severity = ab_value_member(check->spec, "severity");
    const AbValue *owner = ab_value_member(check->spec, "owner");
    const AbValue *rationale = ab_value_member(check->spec, "rationale");
    const AbValue *requirements = ab_value_member(check->spec, "requirement");
    const AbValue *expected = ab_value_member(check->spec, "expected");
    const AbValue *actual = ab_value_member(check->spec, "actual");
    const AbValue *mapping = ab_value_member(check->spec, "mapping");
    const AbValue *exact = ab_value_member(check->spec, "exact");
    const AbValue *minimum = ab_value_member(check->spec, "min");
    const AbValue *maximum = ab_value_member(check->spec, "max");
    const AbValue *routes = ab_value_member(check->spec, "required_routes");
    size_t index;
    int any_operand = 0;
    MD_TRY(ab_buffer_literal(buffer, "### "));
    MD_TRY(markdown_upper(buffer, &check->status));
    MD_TRY(ab_buffer_literal(buffer, " "));
    MD_TRY(ab_buffer_append(buffer, id->as.text.data, id->as.text.length));
    MD_TRY(ab_buffer_literal(buffer, "\n\n`"));
    MD_TRY(ab_buffer_append(buffer, assertion->as.text.data,
                            assertion->as.text.length));
    MD_TRY(ab_buffer_literal(buffer, "` severity="));
    if (severity)
      MD_TRY(ab_buffer_append(buffer, severity->as.text.data,
                              severity->as.text.length));
    else
      MD_TRY(ab_buffer_literal(buffer, "error"));
    MD_TRY(ab_buffer_literal(buffer, " owner="));
    MD_TRY(
        ab_buffer_append(buffer, owner->as.text.data, owner->as.text.length));
    MD_TRY(ab_buffer_literal(buffer, "; coverage="));
    MD_TRY(ab_buffer_u64(buffer, check->coverage.count));
    MD_TRY(ab_buffer_literal(buffer, "."));
    if (requirements) {
      MD_TRY(ab_buffer_literal(buffer, " requirements="));
      MD_TRY(markdown_string_list(buffer, requirements));
    }
    MD_TRY(ab_buffer_literal(buffer, "\n"));
    if ((expected && expected->as.text.length) ||
        (actual && actual->as.text.length) ||
        (mapping && mapping->as.text.length) || exact || minimum || maximum ||
        (routes && routes->as.array.count)) {
      MD_TRY(ab_buffer_literal(buffer, "Operands: `"));
#define MD_OPERAND_TEXT(label, value)                                          \
  do {                                                                         \
    if ((value) && (value)->as.text.length) {                                  \
      if (any_operand)                                                         \
        MD_TRY(ab_buffer_literal(buffer, " "));                                \
      MD_TRY(ab_buffer_literal(buffer, label));                                \
      MD_TRY(ab_buffer_append(buffer, (value)->as.text.data,                   \
                              (value)->as.text.length));                       \
      any_operand = 1;                                                         \
    }                                                                          \
  } while (0)
      MD_OPERAND_TEXT("expected=", expected);
      MD_OPERAND_TEXT("actual=", actual);
      MD_OPERAND_TEXT("mapping=", mapping);
#undef MD_OPERAND_TEXT
      if (exact || minimum || maximum) {
        const AbValue *numbers[] = {exact, minimum, maximum};
        const char *labels[] = {"exact=", "min=", "max="};
        for (index = 0; index < 3; index++)
          if (numbers[index]) {
            if (any_operand)
              MD_TRY(ab_buffer_literal(buffer, " "));
            MD_TRY(ab_buffer_literal(buffer, labels[index]));
            MD_TRY(ab_value_render(buffer, numbers[index]));
            any_operand = 1;
          }
      }
      if (routes && routes->as.array.count) {
        if (any_operand)
          MD_TRY(ab_buffer_literal(buffer, " "));
        MD_TRY(ab_buffer_literal(buffer, "routes="));
        MD_TRY(markdown_string_list(buffer, routes));
      }
      MD_TRY(ab_buffer_literal(buffer, "`\n"));
    }
    MD_TRY(ab_buffer_literal(buffer, "\n"));
    MD_TRY(ab_buffer_append(buffer, rationale->as.text.data,
                            rationale->as.text.length));
    MD_TRY(ab_buffer_literal(buffer, "\n\n"));
    for (index = 0; index < views[check_index].visible; index++) {
      const AbVerifyFinding *finding = views[check_index].findings[index];
      size_t evidence_index;
      size_t evidence_visible =
          max_findings == SIZE_MAX
              ? finding->evidence_count
              : (finding->evidence_count < 4 ? finding->evidence_count : 4);
      MD_TRY(ab_buffer_literal(buffer, "- `"));
      MD_TRY(ab_buffer_append(buffer, finding->fingerprint.data, 16));
      MD_TRY(ab_buffer_literal(buffer, "` **"));
      MD_TRY(ab_buffer_append(buffer, finding->comparison.data,
                              finding->comparison.length));
      MD_TRY(ab_buffer_literal(buffer, "** evidence="));
      MD_TRY(ab_buffer_append(buffer, finding->evidence_state.data,
                              finding->evidence_state.length));
      MD_TRY(ab_buffer_literal(buffer, " applicability="));
      MD_TRY(ab_buffer_append(buffer, finding->applicability.data,
                              finding->applicability.length));
      MD_TRY(ab_buffer_literal(buffer, " disposition="));
      MD_TRY(ab_buffer_append(buffer, finding->disposition.data,
                              finding->disposition.length));
      if (!string_literal(&finding->baseline_state, "none")) {
        MD_TRY(ab_buffer_literal(buffer, " baseline="));
        MD_TRY(ab_buffer_append(buffer, finding->baseline_state.data,
                                finding->baseline_state.length));
      }
      if (finding->waiver.length) {
        MD_TRY(ab_buffer_literal(buffer, " waiver="));
        MD_TRY(ab_buffer_append(buffer, finding->waiver.data,
                                finding->waiver.length));
      }
      MD_TRY(ab_buffer_literal(buffer, ": "));
      MD_TRY(ab_buffer_append(buffer, finding->key.data, finding->key.length));
      MD_TRY(ab_buffer_literal(buffer, " — "));
      MD_TRY(ab_buffer_append(buffer, finding->message.data,
                              finding->message.length));
      MD_TRY(ab_buffer_literal(buffer, "\n"));
      if (finding->waiver_note.length) {
        MD_TRY(ab_buffer_literal(buffer, "  - waiver not applied: "));
        MD_TRY(ab_buffer_append(buffer, finding->waiver_note.data,
                                finding->waiver_note.length));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
      for (evidence_index = 0; evidence_index < evidence_visible;
           evidence_index++) {
        MD_TRY(ab_buffer_literal(buffer, "  - "));
        MD_TRY(
            render_evidence_label(buffer, &finding->evidence[evidence_index]));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
      if (evidence_visible < finding->evidence_count) {
        MD_TRY(ab_buffer_literal(buffer, "  - … "));
        MD_TRY(
            ab_buffer_u64(buffer, finding->evidence_count - evidence_visible));
        MD_TRY(ab_buffer_literal(buffer, " more witnesses in JSON/SARIF\n"));
      }
    }
    if (views[check_index].visible < views[check_index].count) {
      MD_TRY(ab_buffer_literal(buffer, "- … "));
      MD_TRY(ab_buffer_u64(buffer, views[check_index].count -
                                       views[check_index].visible));
      MD_TRY(ab_buffer_literal(
          buffer,
          " findings omitted from compact Markdown; JSON is complete\n"));
    }
    if (!views[check_index].count)
      for (index = 0; index < check->witness_count; index++) {
        MD_TRY(ab_buffer_literal(buffer, "- "));
        MD_TRY(render_evidence_label(buffer, &check->witnesses[index]));
        MD_TRY(ab_buffer_literal(buffer, "\n"));
      }
    MD_TRY(ab_buffer_literal(buffer, "\n"));
  }
  while (buffer->length && buffer->data[buffer->length - 1] == '\n')
    buffer->length--;
  MD_TRY(ab_buffer_literal(buffer, "\n"));

cleanup:
  markdown_views_free(context->engine, views, context->check_count);
#undef MD_TRY
  return status;
}

#undef REPORT_TRY
