#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"
#include "verify_checks.h"
#include "verify_runtime.h"

#include <string.h>

#define DEBUG_TRY(expression)                                                  \
  do {                                                                         \
    ArchbirdStatus debug_status__ = (expression);                              \
    if (debug_status__ != ARCHBIRD_OK)                                         \
      return debug_status__;                                                   \
  } while (0)

typedef struct DebugRequest {
  const AbString *view;
  const AbString *check;
  const AbString *extractor;
} DebugRequest;

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int value_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         string_is(&value->as.text, literal);
}

static int nonblank_string(const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING && value->as.text.length;
}

static int fields_allowed(const AbValue *object) {
  static const char *const names[] = {
      "artifact", "check", "extractor", "schema_version", "view",
  };
  size_t field_index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (field_index = 0; field_index < object->as.object.count; field_index++) {
    const AbString *name = &object->as.object.fields[field_index].name;
    size_t name_index;
    for (name_index = 0; name_index < sizeof(names) / sizeof(names[0]);
         name_index++)
      if (string_is(name, names[name_index]))
        break;
    if (name_index == sizeof(names) / sizeof(names[0]))
      return 0;
  }
  return 1;
}

static const AbValue *check_with_id(const AbVerifySuiteView *suite,
                                    const AbString *id, size_t *out_index) {
  size_t index;
  for (index = 0; index < suite->checks->as.array.count; index++) {
    const AbValue *check = &suite->checks->as.array.items[index];
    const AbValue *candidate = ab_value_member(check, "id");
    if (candidate && ab_string_equal(&candidate->as.text, id)) {
      if (out_index)
        *out_index = index;
      return check;
    }
  }
  return NULL;
}

static int extractor_exists(const AbVerifySuiteView *suite,
                            const AbString *name) {
  size_t index;
  for (index = 0; index < suite->extractors->as.object.count; index++)
    if (ab_string_equal(&suite->extractors->as.object.fields[index].name, name))
      return 1;
  return 0;
}

static ArchbirdStatus decode_request(ArchbirdEngine *engine,
                                     const AbVerifySuiteView *suite,
                                     const AbValue *document,
                                     DebugRequest *out) {
  const AbValue *schema = ab_value_member(document, "schema_version");
  const AbValue *view = ab_value_member(document, "view");
  const AbValue *check = ab_value_member(document, "check");
  const AbValue *extractor = ab_value_member(document, "extractor");
  uint64_t schema_number;
  if (!fields_allowed(document) ||
      !value_is(ab_value_member(document, "artifact"),
                "verification-debug-request") ||
      !schema || !ab_value_u64(schema, &schema_number) || schema_number != 1 ||
      (!value_is(view, "selection") && !value_is(view, "unknown")) ||
      (check && !nonblank_string(check)) ||
      (extractor && !nonblank_string(extractor)))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid verification debug request");
  if (check && !check_with_id(suite, &check->as.text, NULL))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "verification debug check is unknown");
  if (extractor && !extractor_exists(suite, &extractor->as.text))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "verification debug extractor is unknown");
  memset(out, 0, sizeof(*out));
  out->view = &view->as.text;
  out->check = check ? &check->as.text : NULL;
  out->extractor = extractor ? &extractor->as.text : NULL;
  return ARCHBIRD_OK;
}

static int check_references(const AbValue *check, const AbString *extractor) {
  static const char *const fields[] = {"actual", "expected"};
  const AbValue *assertion = ab_value_member(check, "assert");
  size_t index;
  if (value_is(assertion, "attestation_equal"))
    return 0;
  for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
    const AbValue *value = ab_value_member(check, fields[index]);
    if (value && value->kind == AB_VALUE_STRING &&
        ab_string_equal(&value->as.text, extractor))
      return 1;
  }
  return 0;
}

static int fact_selected(const AbVerificationContext *context,
                         const DebugRequest *request,
                         const AbVerifyFactSet *fact) {
  const AbValue *check;
  if (request->extractor && !ab_string_equal(request->extractor, &fact->name))
    return 0;
  if (!request->check)
    return 1;
  check = check_with_id(&context->suite, request->check, NULL);
  return check && check_references(check, &fact->name);
}

static int check_selected(const DebugRequest *request, const AbValue *check) {
  const AbValue *id = ab_value_member(check, "id");
  if (request->check && !ab_string_equal(request->check, &id->as.text))
    return 0;
  if (request->extractor && !check_references(check, request->extractor))
    return 0;
  return 1;
}

static int attestation_selected(const AbVerificationContext *context,
                                const DebugRequest *request,
                                const AbVerifyAttestationState *state) {
  const AbValue *check;
  const AbValue *actual;
  const AbValue *expected;
  if (request->extractor)
    return 0;
  if (!request->check)
    return 1;
  check = check_with_id(&context->suite, request->check, NULL);
  if (!check ||
      !value_is(ab_value_member(check, "assert"), "attestation_equal"))
    return 0;
  actual = ab_value_member(check, "actual");
  expected = ab_value_member(check, "expected");
  return (actual && ab_string_equal(&actual->as.text, &state->name)) ||
         (expected && ab_string_equal(&expected->as.text, &state->name));
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value ? value->data : "",
                               value ? value->length : 0);
}

static ArchbirdStatus render_nullable_count(AbBuffer *buffer, uint64_t value,
                                            int present) {
  return present ? ab_buffer_u64(buffer, value)
                 : ab_buffer_literal(buffer, "null");
}

static const char *selection_unit(const AbObjectField *spec,
                                  const AbVerifyFactSet *fact) {
  const AbValue *kind = ab_value_member(&spec->value, "kind");
  if (value_is(kind, "symbols"))
    return "symbol";
  if (value_is(kind, "file_metrics"))
    return "mapped_source_file";
  if (value_is(kind, "file_edges") || value_is(kind, "component_edges") ||
      value_is(kind, "literal_relation"))
    return "relation";
  if (value_is(kind, "test_routes"))
    return "test_route";
  if (value_is(kind, "test_selectors"))
    return "test_selector";
  if (value_is(kind, "provider_surface"))
    return "provider_capability";
  if (value_is(kind, "python_enum") || value_is(kind, "python_set") ||
      value_is(kind, "c_enum") || value_is(kind, "c_designated_initializer") ||
      value_is(kind, "c_macro_set") || value_is(kind, "literal_set") ||
      value_is(kind, "literal_values"))
    return "value";
  return fact->selection.unit.length ? fact->selection.unit.data : "item";
}

static ArchbirdStatus render_reason(AbBuffer *buffer, size_t *count,
                                    const char *reason) {
  if ((*count)++)
    DEBUG_TRY(ab_buffer_literal(buffer, ","));
  return ab_buffer_json_string(buffer, reason, strlen(reason));
}

static ArchbirdStatus render_selection_reasons(AbBuffer *buffer,
                                               const AbVerifyFactSet *fact) {
  const AbVerifySelection *selection = &fact->selection;
  size_t count = 0;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  if (string_is(&fact->state, "unknown"))
    DEBUG_TRY(render_reason(buffer, &count, "fact-unknown"));
  else if (!string_is(&fact->state, "current"))
    DEBUG_TRY(render_reason(buffer, &count, "fact-stale"));
  if (!selection->has_universe)
    DEBUG_TRY(render_reason(buffer, &count, "universe-unreported"));
  if (!selection->has_selected)
    DEBUG_TRY(render_reason(buffer, &count, "selected-unreported"));
  if (!selection->has_excluded)
    DEBUG_TRY(render_reason(buffer, &count, "excluded-unreported"));
  if (!selection->has_unsupported)
    DEBUG_TRY(render_reason(buffer, &count, "unsupported-unreported"));
  else if (selection->unsupported)
    DEBUG_TRY(render_reason(buffer, &count, "unsupported-selected-entities"));
  if (!selection->has_truncated)
    DEBUG_TRY(render_reason(buffer, &count, "truncation-unreported"));
  else if (selection->truncated)
    DEBUG_TRY(render_reason(buffer, &count, "enumeration-truncated"));
  if (selection->has_unknown && selection->unknown)
    DEBUG_TRY(render_reason(buffer, &count, "unknown-selected-entities"));
  if (selection->has_selected && selection->has_evaluated &&
      selection->has_unknown &&
      (selection->evaluated > UINT64_MAX - selection->unknown ||
       selection->evaluated + selection->unknown != selection->selected))
    DEBUG_TRY(render_reason(buffer, &count, "selected-entities-unaccounted"));
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_selection(AbBuffer *buffer,
                                       const AbObjectField *spec,
                                       const AbVerifyFactSet *fact) {
  const AbVerifySelection *selection = &fact->selection;
  const AbValue *kind = ab_value_member(&spec->value, "kind");
  const char *classification = ab_verify_fact_selection_classification(fact);
  DEBUG_TRY(ab_buffer_literal(buffer, "{\"classification\":"));
  DEBUG_TRY(
      ab_buffer_json_string(buffer, classification, strlen(classification)));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"counts\":{\"evaluated\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->evaluated,
                                  selection->has_evaluated));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"excluded\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->excluded,
                                  selection->has_excluded));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"selected\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->selected,
                                  selection->has_selected));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"unknown\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->unknown,
                                  selection->has_unknown));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"universe\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->universe,
                                  selection->has_universe));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"unsupported\":"));
  DEBUG_TRY(render_nullable_count(buffer, selection->unsupported,
                                  selection->has_unsupported));
  DEBUG_TRY(ab_buffer_literal(buffer, "},\"extractor\":"));
  DEBUG_TRY(render_string(buffer, &fact->name));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"fact\":{\"message\":"));
  DEBUG_TRY(render_string(buffer, &fact->message));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  DEBUG_TRY(ab_buffer_json_string(buffer, fact->sha256, 64));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"state\":"));
  DEBUG_TRY(render_string(buffer, &fact->state));
  DEBUG_TRY(ab_buffer_literal(buffer, "},\"kind\":"));
  DEBUG_TRY(ab_value_render(buffer, kind));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  DEBUG_TRY(render_string(buffer, &fact->project));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"reasons\":"));
  DEBUG_TRY(render_selection_reasons(buffer, fact));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"selection_complete\":"));
  if (!strcmp(classification, "complete"))
    DEBUG_TRY(ab_buffer_literal(buffer, "true"));
  else if (!strcmp(classification, "incomplete"))
    DEBUG_TRY(ab_buffer_literal(buffer, "false"));
  else
    DEBUG_TRY(ab_buffer_literal(buffer, "null"));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"enumeration_complete\":"));
  if (selection->has_truncated)
    DEBUG_TRY(
        ab_buffer_literal(buffer, selection->truncated ? "false" : "true"));
  else
    DEBUG_TRY(ab_buffer_literal(buffer, "null"));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"truncated\":"));
  if (selection->has_truncated)
    DEBUG_TRY(
        ab_buffer_literal(buffer, selection->truncated ? "true" : "false"));
  else
    DEBUG_TRY(ab_buffer_literal(buffer, "null"));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"unit\":"));
  DEBUG_TRY(ab_buffer_json_string(buffer, selection_unit(spec, fact),
                                  strlen(selection_unit(spec, fact))));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_selections(AbBuffer *buffer,
                                        const AbVerificationContext *context,
                                        const DebugRequest *request) {
  size_t index;
  size_t emitted = 0;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->fact_count; index++) {
    const AbVerifyFactSet *fact = &context->facts[index];
    const char *classification;
    if (!fact_selected(context, request, fact))
      continue;
    classification = ab_verify_fact_selection_classification(fact);
    if (string_is(request->view, "unknown") &&
        !strcmp(classification, "complete"))
      continue;
    if (emitted++)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(render_selection(
        buffer, &context->suite.extractors->as.object.fields[index], fact));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_check_extractors(AbBuffer *buffer,
                                              const AbValue *check) {
  static const char *const fields[] = {"actual", "expected"};
  const AbValue *assertion = ab_value_member(check, "assert");
  const AbString *first = NULL;
  size_t index;
  size_t emitted = 0;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  if (!value_is(assertion, "attestation_equal")) {
    for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
      const AbValue *value = ab_value_member(check, fields[index]);
      if (!value || value->kind != AB_VALUE_STRING ||
          (first && ab_string_equal(first, &value->as.text)))
        continue;
      if (emitted++)
        DEBUG_TRY(ab_buffer_literal(buffer, ","));
      DEBUG_TRY(render_string(buffer, &value->as.text));
      first = &value->as.text;
    }
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_checks(AbBuffer *buffer,
                                    const AbVerificationContext *context,
                                    const DebugRequest *request) {
  size_t index;
  size_t emitted = 0;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->check_count; index++) {
    const AbValue *check = context->checks[index].spec;
    const AbValue *id = ab_value_member(check, "id");
    const AbValue *assertion = ab_value_member(check, "assert");
    if (!check_selected(request, check))
      continue;
    if (emitted++)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(ab_buffer_literal(buffer, "{\"assert\":"));
    DEBUG_TRY(ab_value_render(buffer, assertion));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"extractors\":"));
    DEBUG_TRY(render_check_extractors(buffer, check));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    DEBUG_TRY(ab_value_render(buffer, id));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"status\":"));
    DEBUG_TRY(render_string(buffer, &context->checks[index].status));
    DEBUG_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_diagnostics(AbBuffer *buffer,
                                         const AbVerificationContext *context) {
  size_t index;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->diagnostic_count; index++) {
    const AbVerifyDiagnostic *row = &context->diagnostics[index];
    if (index)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(ab_buffer_literal(buffer, "{\"code\":"));
    DEBUG_TRY(render_string(buffer, &row->code));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"message\":"));
    DEBUG_TRY(render_string(buffer, &row->message));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    DEBUG_TRY(render_string(buffer, &row->path));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
    DEBUG_TRY(render_string(buffer, &row->severity));
    DEBUG_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_coverage_value(AbBuffer *buffer,
                                            const AbValue *coverage,
                                            const char *name, int *complete) {
  const AbValue *value = coverage ? ab_value_member(coverage, name) : NULL;
  uint64_t number;
  if (!value || !ab_value_u64(value, &number)) {
    *complete = 0;
    return ab_buffer_literal(buffer, "null");
  }
  return ab_buffer_u64(buffer, number);
}

static ArchbirdStatus render_projects(AbBuffer *buffer,
                                      const AbVerificationContext *context) {
  size_t index;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->input.projects->as.array.count; index++) {
    const AbValue *project = &context->input.projects->as.array.items[index];
    const AbValue *name = ab_value_member(project, "name");
    const AbValue *map = ab_value_member(project, "map");
    const AbValue *discovery = map ? ab_value_member(map, "discovery") : NULL;
    const AbValue *coverage =
        discovery ? ab_value_member(discovery, "coverage") : NULL;
    const AbValue *files = map ? ab_value_member(map, "files") : NULL;
    int complete = files && files->kind == AB_VALUE_ARRAY;
    if (index)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(ab_buffer_literal(buffer, "{\"coverage\":{\"assets\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "assets", &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"ignored\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "ignored", &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"inventory_files\":"));
    DEBUG_TRY(
        render_coverage_value(buffer, coverage, "inventory_files", &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"oversized\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "oversized", &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"pruned_directories\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "pruned_directories",
                                    &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"selected\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "selected", &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"unsupported_known\":"));
    DEBUG_TRY(render_coverage_value(buffer, coverage, "unsupported_known",
                                    &complete));
    DEBUG_TRY(ab_buffer_literal(buffer, "},\"mapped_files\":"));
    if (files && files->kind == AB_VALUE_ARRAY)
      DEBUG_TRY(ab_buffer_u64(buffer, (uint64_t)files->as.array.count));
    else
      DEBUG_TRY(ab_buffer_literal(buffer, "null"));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    DEBUG_TRY(ab_value_render(buffer, name));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"resolution_supplied\":"));
    DEBUG_TRY(ab_buffer_literal(
        buffer, ab_value_member(project, "resolution") ? "true" : "false"));
    DEBUG_TRY(ab_buffer_literal(buffer, ",\"state\":"));
    DEBUG_TRY(ab_buffer_json_string(buffer, complete ? "complete" : "unknown",
                                    complete ? 8 : 7));
    DEBUG_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus
render_unknown_row(AbBuffer *buffer, const char *scope, const AbString *check,
                   const AbString *extractor, const AbString *key,
                   const AbString *project, const AbString *state,
                   const AbString *message, const AbVerifyEvidence *evidence,
                   size_t evidence_count) {
  size_t index;
  DEBUG_TRY(ab_buffer_literal(buffer, "{\"check\":"));
  DEBUG_TRY(render_string(buffer, check));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"evidence\":["));
  for (index = 0; index < evidence_count; index++) {
    if (index)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(ab_verify_evidence_render(buffer, &evidence[index]));
  }
  DEBUG_TRY(ab_buffer_literal(buffer, "],\"extractor\":"));
  DEBUG_TRY(render_string(buffer, extractor));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  DEBUG_TRY(render_string(buffer, key));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"message\":"));
  DEBUG_TRY(render_string(buffer, message));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  DEBUG_TRY(render_string(buffer, project));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"scope\":"));
  DEBUG_TRY(ab_buffer_json_string(buffer, scope, strlen(scope)));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"state\":"));
  DEBUG_TRY(render_string(buffer, state));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_unknowns(AbBuffer *buffer,
                                      const AbVerificationContext *context,
                                      const DebugRequest *request,
                                      size_t *out_count) {
  static const AbString empty = {0};
  static const AbString unknown = {(char *)"unknown", 7};
  static const AbString check_message = {(char *)"check evaluated to unknown",
                                         26};
  size_t emitted = 0;
  size_t fact_index;
  DEBUG_TRY(ab_buffer_literal(buffer, "["));
  for (fact_index = 0; fact_index < context->fact_count; fact_index++) {
    const AbVerifyFactSet *fact = &context->facts[fact_index];
    size_t item_index;
    if (!fact_selected(context, request, fact))
      continue;
    if (!string_is(&fact->state, "current")) {
      if (emitted++)
        DEBUG_TRY(ab_buffer_literal(buffer, ","));
      DEBUG_TRY(render_unknown_row(buffer, "fact", &empty, &fact->name, &empty,
                                   &fact->project, &fact->state, &fact->message,
                                   NULL, 0));
    }
    for (item_index = 0; item_index < fact->item_count; item_index++) {
      const AbVerifyFactItem *item = &fact->items[item_index];
      if (string_is(&item->state, "current"))
        continue;
      if (emitted++)
        DEBUG_TRY(ab_buffer_literal(buffer, ","));
      DEBUG_TRY(render_unknown_row(
          buffer, "fact-item", &empty, &fact->name, &item->key, &fact->project,
          &item->state, &item->message, item->evidence, item->evidence_count));
    }
  }
  for (fact_index = 0; fact_index < context->attestation_count; fact_index++) {
    const AbVerifyAttestationState *state = &context->attestations[fact_index];
    const AbString *check = request->check ? request->check : &empty;
    if (!attestation_selected(context, request, state) ||
        string_is(&state->state, "current"))
      continue;
    if (emitted++)
      DEBUG_TRY(ab_buffer_literal(buffer, ","));
    DEBUG_TRY(render_unknown_row(buffer, "attestation", check, &empty,
                                 &state->name, &state->project, &state->state,
                                 &state->message, state->witnesses,
                                 state->witness_count));
  }
  for (fact_index = 0; fact_index < context->check_count; fact_index++) {
    const AbVerifyCheckResult *result = &context->checks[fact_index];
    const AbValue *id_value = ab_value_member(result->spec, "id");
    size_t finding_index;
    if (!check_selected(request, result->spec))
      continue;
    if (string_is(&result->status, "unknown")) {
      if (emitted++)
        DEBUG_TRY(ab_buffer_literal(buffer, ","));
      DEBUG_TRY(render_unknown_row(buffer, "check", &id_value->as.text, &empty,
                                   &empty, &empty, &unknown, &check_message,
                                   result->witnesses, result->witness_count));
    }
    for (finding_index = 0; finding_index < result->finding_count;
         finding_index++) {
      const AbVerifyFinding *finding = &result->findings[finding_index];
      if (string_is(&finding->evidence_state, "current"))
        continue;
      if (emitted++)
        DEBUG_TRY(ab_buffer_literal(buffer, ","));
      DEBUG_TRY(render_unknown_row(buffer, "finding", &id_value->as.text,
                                   &empty, &finding->key, &empty,
                                   &finding->evidence_state, &finding->message,
                                   finding->evidence, finding->evidence_count));
    }
  }
  DEBUG_TRY(ab_buffer_literal(buffer, "]"));
  *out_count = emitted;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_filters(AbBuffer *buffer,
                                     const DebugRequest *request) {
  DEBUG_TRY(ab_buffer_literal(buffer, "{\"check\":"));
  if (request->check)
    DEBUG_TRY(render_string(buffer, request->check));
  else
    DEBUG_TRY(ab_buffer_literal(buffer, "null"));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"extractor\":"));
  if (request->extractor)
    DEBUG_TRY(render_string(buffer, request->extractor));
  else
    DEBUG_TRY(ab_buffer_literal(buffer, "null"));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_json(AbBuffer *buffer,
                                  const AbVerificationContext *context,
                                  const DebugRequest *request) {
  size_t unknown_count = 0;
  size_t selection_index;
  size_t complete = 0;
  size_t bounded = 0;
  size_t incomplete = 0;
  size_t unknown = 0;
  for (selection_index = 0; selection_index < context->fact_count;
       selection_index++) {
    const AbVerifyFactSet *fact = &context->facts[selection_index];
    const char *classification;
    if (!fact_selected(context, request, fact))
      continue;
    classification = ab_verify_fact_selection_classification(fact);
    if (string_is(request->view, "unknown") &&
        !strcmp(classification, "complete"))
      continue;
    if (!strcmp(classification, "complete"))
      complete++;
    else if (!strcmp(classification, "bounded"))
      bounded++;
    else if (!strcmp(classification, "incomplete"))
      incomplete++;
    else
      unknown++;
  }
  DEBUG_TRY(ab_buffer_literal(
      buffer, "{\"artifact\":\"verification-debug\",\"checks\":"));
  DEBUG_TRY(render_checks(buffer, context, request));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"diagnostics\":"));
  DEBUG_TRY(render_diagnostics(buffer, context));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"filters\":"));
  DEBUG_TRY(render_filters(buffer, request));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"projects\":"));
  DEBUG_TRY(render_projects(buffer, context));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"schema_version\":1,\"selections\":"));
  DEBUG_TRY(render_selections(buffer, context, request));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"suite\":{\"name\":"));
  DEBUG_TRY(ab_value_render(buffer, context->suite.name));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  DEBUG_TRY(ab_buffer_json_string(buffer, context->suite.sha256, 64));
  DEBUG_TRY(ab_buffer_literal(buffer, "},\"unknowns\":"));
  DEBUG_TRY(render_unknowns(buffer, context, request, &unknown_count));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"summary\":{\"diagnostics\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, (uint64_t)context->diagnostic_count));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"selections\":{\"bounded\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, bounded));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"complete\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, complete));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"incomplete\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, incomplete));
  DEBUG_TRY(ab_buffer_literal(buffer, ",\"unknown\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, unknown));
  DEBUG_TRY(ab_buffer_literal(buffer, "},\"unknowns\":"));
  DEBUG_TRY(ab_buffer_u64(buffer, unknown_count));
  DEBUG_TRY(
      ab_buffer_literal(buffer, "},\"tool\":{\"implementation_sha256\":\""));
  DEBUG_TRY(ab_buffer_literal(buffer, ARCHBIRD_IMPLEMENTATION_SHA256));
  DEBUG_TRY(
      ab_buffer_literal(buffer, "\",\"name\":\"archbird\",\"version\":\""));
  DEBUG_TRY(ab_buffer_literal(buffer, ARCHBIRD_VERSION));
  DEBUG_TRY(ab_buffer_literal(buffer, "\"},\"view\":"));
  DEBUG_TRY(render_string(buffer, request->view));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus append_markdown_count(AbBuffer *buffer, uint64_t value,
                                            int present) {
  return present ? ab_buffer_u64(buffer, value)
                 : ab_buffer_literal(buffer, "?");
}

static ArchbirdStatus append_markdown_field(AbBuffer *buffer, const char *name,
                                            const AbString *value) {
  if (!value || !value->length)
    return ARCHBIRD_OK;
  DEBUG_TRY(ab_buffer_literal(buffer, " "));
  DEBUG_TRY(ab_buffer_literal(buffer, name));
  DEBUG_TRY(ab_buffer_literal(buffer, "="));
  return ab_buffer_append(buffer, value->data, value->length);
}

static ArchbirdStatus render_markdown_unknown_row(
    AbBuffer *buffer, const char *scope, const AbString *check,
    const AbString *extractor, const AbString *key, const AbString *project,
    const AbString *state, const AbString *message,
    const AbVerifyEvidence *evidence, size_t evidence_count) {
  size_t index;
  DEBUG_TRY(ab_buffer_literal(buffer, "- scope="));
  DEBUG_TRY(ab_buffer_literal(buffer, scope));
  DEBUG_TRY(append_markdown_field(buffer, "check", check));
  DEBUG_TRY(append_markdown_field(buffer, "extractor", extractor));
  DEBUG_TRY(append_markdown_field(buffer, "key", key));
  DEBUG_TRY(append_markdown_field(buffer, "project", project));
  DEBUG_TRY(append_markdown_field(buffer, "state", state));
  DEBUG_TRY(append_markdown_field(buffer, "message", message));
  DEBUG_TRY(ab_buffer_literal(buffer, "\n"));
  for (index = 0; index < evidence_count; index++) {
    const AbVerifyEvidence *row = &evidence[index];
    DEBUG_TRY(ab_buffer_literal(buffer, "  evidence"));
    DEBUG_TRY(append_markdown_field(buffer, "provenance", &row->provenance));
    DEBUG_TRY(append_markdown_field(buffer, "project", &row->project));
    DEBUG_TRY(append_markdown_field(buffer, "path", &row->path));
    if (row->line) {
      DEBUG_TRY(ab_buffer_literal(buffer, " line="));
      DEBUG_TRY(ab_buffer_u64(buffer, row->line));
    }
    DEBUG_TRY(append_markdown_field(buffer, "detail", &row->detail));
    DEBUG_TRY(ab_buffer_literal(buffer, "\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
render_markdown_unknowns(AbBuffer *buffer, const AbVerificationContext *context,
                         const DebugRequest *request, size_t *out_count) {
  static const AbString empty = {0};
  static const AbString unknown = {(char *)"unknown", 7};
  static const AbString check_message = {(char *)"check evaluated to unknown",
                                         26};
  size_t emitted = 0;
  size_t index;
  for (index = 0; index < context->fact_count; index++) {
    const AbVerifyFactSet *fact = &context->facts[index];
    size_t item_index;
    if (!fact_selected(context, request, fact))
      continue;
    if (!string_is(&fact->state, "current")) {
      DEBUG_TRY(render_markdown_unknown_row(
          buffer, "fact", &empty, &fact->name, &empty, &fact->project,
          &fact->state, &fact->message, NULL, 0));
      emitted++;
    }
    for (item_index = 0; item_index < fact->item_count; item_index++) {
      const AbVerifyFactItem *item = &fact->items[item_index];
      if (string_is(&item->state, "current"))
        continue;
      DEBUG_TRY(render_markdown_unknown_row(
          buffer, "fact-item", &empty, &fact->name, &item->key, &fact->project,
          &item->state, &item->message, item->evidence, item->evidence_count));
      emitted++;
    }
  }
  for (index = 0; index < context->attestation_count; index++) {
    const AbVerifyAttestationState *state = &context->attestations[index];
    const AbString *check = request->check ? request->check : &empty;
    if (!attestation_selected(context, request, state) ||
        string_is(&state->state, "current"))
      continue;
    DEBUG_TRY(render_markdown_unknown_row(
        buffer, "attestation", check, &empty, &state->name, &state->project,
        &state->state, &state->message, state->witnesses,
        state->witness_count));
    emitted++;
  }
  for (index = 0; index < context->check_count; index++) {
    const AbVerifyCheckResult *result = &context->checks[index];
    const AbValue *id = ab_value_member(result->spec, "id");
    size_t finding_index;
    if (!check_selected(request, result->spec))
      continue;
    if (string_is(&result->status, "unknown")) {
      DEBUG_TRY(render_markdown_unknown_row(
          buffer, "check", &id->as.text, &empty, &empty, &empty, &unknown,
          &check_message, result->witnesses, result->witness_count));
      emitted++;
    }
    for (finding_index = 0; finding_index < result->finding_count;
         finding_index++) {
      const AbVerifyFinding *finding = &result->findings[finding_index];
      if (string_is(&finding->evidence_state, "current"))
        continue;
      DEBUG_TRY(render_markdown_unknown_row(
          buffer, "finding", &id->as.text, &empty, &finding->key, &empty,
          &finding->evidence_state, &finding->message, finding->evidence,
          finding->evidence_count));
      emitted++;
    }
  }
  if (!emitted)
    DEBUG_TRY(ab_buffer_literal(buffer, "(none)\n"));
  *out_count = emitted;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_markdown(AbBuffer *buffer,
                                      const AbVerificationContext *context,
                                      const DebugRequest *request) {
  size_t index;
  size_t emitted = 0;
  size_t unknown_count = 0;
  DEBUG_TRY(ab_buffer_literal(buffer, "# Verification debug\n\nSuite `"));
  DEBUG_TRY(ab_buffer_append(buffer, context->suite.name->as.text.data,
                             context->suite.name->as.text.length));
  DEBUG_TRY(ab_buffer_literal(buffer, "`; view `"));
  DEBUG_TRY(
      ab_buffer_append(buffer, request->view->data, request->view->length));
  DEBUG_TRY(ab_buffer_literal(buffer, "`.\n\n## Selections\n\n```text\n"));
  for (index = 0; index < context->fact_count; index++) {
    const AbVerifyFactSet *fact = &context->facts[index];
    const AbVerifySelection *selection = &fact->selection;
    const char *classification;
    if (!fact_selected(context, request, fact))
      continue;
    classification = ab_verify_fact_selection_classification(fact);
    if (string_is(request->view, "unknown") &&
        !strcmp(classification, "complete"))
      continue;
    emitted++;
    DEBUG_TRY(ab_buffer_append(buffer, fact->name.data, fact->name.length));
    DEBUG_TRY(ab_buffer_literal(buffer, ": classification="));
    DEBUG_TRY(ab_buffer_literal(buffer, classification));
    DEBUG_TRY(ab_buffer_literal(buffer, " universe="));
    DEBUG_TRY(append_markdown_count(buffer, selection->universe,
                                    selection->has_universe));
    DEBUG_TRY(ab_buffer_literal(buffer, " selected="));
    DEBUG_TRY(append_markdown_count(buffer, selection->selected,
                                    selection->has_selected));
    DEBUG_TRY(ab_buffer_literal(buffer, " evaluated="));
    DEBUG_TRY(append_markdown_count(buffer, selection->evaluated,
                                    selection->has_evaluated));
    DEBUG_TRY(ab_buffer_literal(buffer, " excluded="));
    DEBUG_TRY(append_markdown_count(buffer, selection->excluded,
                                    selection->has_excluded));
    DEBUG_TRY(ab_buffer_literal(buffer, " unsupported="));
    DEBUG_TRY(append_markdown_count(buffer, selection->unsupported,
                                    selection->has_unsupported));
    DEBUG_TRY(ab_buffer_literal(buffer, " unknown="));
    DEBUG_TRY(append_markdown_count(buffer, selection->unknown,
                                    selection->has_unknown));
    DEBUG_TRY(ab_buffer_literal(buffer, " truncated="));
    if (selection->has_truncated)
      DEBUG_TRY(
          ab_buffer_literal(buffer, selection->truncated ? "true" : "false"));
    else
      DEBUG_TRY(ab_buffer_literal(buffer, "?"));
    DEBUG_TRY(ab_buffer_literal(buffer, " unit="));
    DEBUG_TRY(ab_buffer_literal(
        buffer,
        selection_unit(&context->suite.extractors->as.object.fields[index],
                       fact)));
    DEBUG_TRY(ab_buffer_literal(buffer, "\n"));
  }
  if (!emitted)
    DEBUG_TRY(ab_buffer_literal(buffer, "(none)\n"));
  DEBUG_TRY(ab_buffer_literal(buffer, "```\n\n## Unresolved evidence\n\n"));
  DEBUG_TRY(render_markdown_unknowns(buffer, context, request, &unknown_count));
  (void)unknown_count;
  if (context->diagnostic_count) {
    DEBUG_TRY(ab_buffer_literal(buffer, "\n## Diagnostics\n\n"));
    for (index = 0; index < context->diagnostic_count; index++) {
      const AbVerifyDiagnostic *row = &context->diagnostics[index];
      DEBUG_TRY(ab_buffer_literal(buffer, "- severity="));
      DEBUG_TRY(
          ab_buffer_append(buffer, row->severity.data, row->severity.length));
      DEBUG_TRY(append_markdown_field(buffer, "code", &row->code));
      DEBUG_TRY(append_markdown_field(buffer, "path", &row->path));
      DEBUG_TRY(append_markdown_field(buffer, "message", &row->message));
      DEBUG_TRY(ab_buffer_literal(buffer, "\n"));
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_verification_debug(
    ArchbirdEngine *engine, const uint8_t *suite_json, size_t suite_length,
    const uint8_t *verification_input_json, size_t verification_input_length,
    const uint8_t *request_json, size_t request_length,
    ArchbirdVerificationFormat format, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbValue suite_document = {0};
  AbValue input_document = {0};
  AbValue request_document = {0};
  AbVerificationContext context = {0};
  DebugRequest request = {0};
  AbBuffer rendered;
  ArchbirdStatus status;
  if (!engine || (!suite_json && suite_length) ||
      (!verification_input_json && verification_input_length) ||
      (!request_json && request_length) || !request_length || !write_fn ||
      (format != ARCHBIRD_VERIFICATION_JSON &&
       format != ARCHBIRD_VERIFICATION_MARKDOWN) ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&rendered, engine);
  status = ab_build_identity_validate(engine);
  if (status == ARCHBIRD_OK)
    status = ab_verification_context_analyze(
        engine, suite_json, suite_length, verification_input_json,
        verification_input_length, &suite_document, &input_document, &context);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, request_json, request_length,
                                  &request_document);
  if (status == ARCHBIRD_OK)
    status =
        decode_request(engine, &context.suite, &request_document, &request);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_JSON)
    status = render_json(&rendered, &context, &request);
  else if (status == ARCHBIRD_OK)
    status = render_markdown(&rendered, &context, &request);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_JSON)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK) {
    if (write_fn(user_data, rendered.data, rendered.length) != 0)
      status =
          archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                             "verification debug callback failed");
  }
  ab_buffer_free(&rendered);
  ab_value_free(engine, &request_document);
  ab_verification_context_free(&context);
  ab_value_free(engine, &input_document);
  ab_value_free(engine, &suite_document);
  return status;
}

#undef DEBUG_TRY
