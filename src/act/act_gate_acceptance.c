#include "act_gate_acceptance.h"

#include "artifact_validation.h"
#include "base64.h"
#include "gate.h"

#include <string.h>

#define AB_GATE_RESULT_MAX_TAIL_BYTES (64u * 1024u)
#define AB_GATE_RESULT_MAX_TAIL_BASE64                                         \
  ((((AB_GATE_RESULT_MAX_TAIL_BYTES) + 2u) / 3u) * 4u)

static ArchbirdStatus reject(ArchbirdEngine *engine, ArchbirdStatus status,
                             const char *message, const AbValue *id) {
  if (id && id->kind == AB_VALUE_STRING)
    return archbird_error_set(
        engine, status, ARCHBIRD_NO_OFFSET, "act acceptance gate '%.*s': %s",
        (int)id->as.text.length, id->as.text.data, message);
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act acceptance gates: %s", message);
}

static const AbValue *result_by_id(const AbValue *results, const AbString *id) {
  size_t low = 0;
  size_t high = results->as.array.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbValue *row = &results->as.array.items[middle];
    const AbValue *row_id = ab_value_member(row, "id");
    int compared = ab_string_compare(&row_id->as.text, id);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return row;
  }
  return NULL;
}

static int valid_tail(ArchbirdEngine *engine, const AbValue *value) {
  uint8_t *decoded = NULL;
  size_t decoded_length = 0;
  ArchbirdStatus status;
  if (!value || value->kind != AB_VALUE_STRING ||
      value->as.text.length > AB_GATE_RESULT_MAX_TAIL_BASE64)
    return 0;
  status = ab_base64_decode(engine, value->as.text.data, value->as.text.length,
                            &decoded, &decoded_length);
  ab_free(engine, decoded);
  return status == ARCHBIRD_OK &&
         decoded_length <= AB_GATE_RESULT_MAX_TAIL_BYTES;
}

static ArchbirdStatus validate_result(ArchbirdEngine *engine,
                                      const AbValue *row) {
  static const char *const fields[] = {"id",
                                       "definition_sha256",
                                       "status",
                                       "exit_code",
                                       "duration_ms",
                                       "environment_sha256",
                                       "stdout_sha256",
                                       "stderr_sha256",
                                       "stdout_tail_base64",
                                       "stderr_tail_base64"};
  const AbValue *id;
  const AbValue *status;
  const AbValue *exit_code;
  uint64_t number;
  if (!ab_artifact_object_exact(row, fields, 10))
    return reject(engine, ARCHBIRD_INVALID_SCHEMA, "result has invalid fields",
                  NULL);
  id = ab_value_member(row, "id");
  status = ab_value_member(row, "status");
  exit_code = ab_value_member(row, "exit_code");
  if (!ab_artifact_stable_id(id) ||
      !ab_artifact_sha256(ab_value_member(row, "definition_sha256")) ||
      (!ab_artifact_text_is(status, "pass") &&
       !ab_artifact_text_is(status, "fail") &&
       !ab_artifact_text_is(status, "timeout") &&
       !ab_artifact_text_is(status, "output_limit") &&
       !ab_artifact_text_is(status, "blocked") &&
       !ab_artifact_text_is(status, "error")) ||
      (exit_code->kind != AB_VALUE_NULL &&
       !ab_artifact_safe_integer(exit_code, &number)) ||
      !ab_artifact_safe_integer(ab_value_member(row, "duration_ms"), &number) ||
      !ab_artifact_sha256(ab_value_member(row, "environment_sha256")) ||
      !ab_artifact_sha256(ab_value_member(row, "stdout_sha256")) ||
      !ab_artifact_sha256(ab_value_member(row, "stderr_sha256")) ||
      !valid_tail(engine, ab_value_member(row, "stdout_tail_base64")) ||
      !valid_tail(engine, ab_value_member(row, "stderr_tail_base64")))
    return reject(engine, ARCHBIRD_INVALID_SCHEMA, "result is invalid", id);
  if (ab_artifact_text_is(status, "pass") &&
      (!ab_artifact_safe_integer(exit_code, &number) || number != 0))
    return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                  "passing result must exit with status zero", id);
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_gate_acceptance_validate(ArchbirdEngine *engine,
                                               const AbValue *gates,
                                               const AbValue *workspace_sha256,
                                               const AbValue *results) {
  size_t index;
  ArchbirdStatus status;
  if (!engine || !gates || !workspace_sha256 || !results ||
      !ab_gate_definitions_valid(gates))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (results->kind != AB_VALUE_ARRAY ||
      results->as.array.count != gates->as.object.count ||
      ((gates->as.object.count != 0) != ab_artifact_sha256(workspace_sha256)) ||
      (!gates->as.object.count && workspace_sha256->kind != AB_VALUE_NULL))
    return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                  "result set does not match configured gates", NULL);
  for (index = 0; index < results->as.array.count; index++) {
    const AbValue *row = &results->as.array.items[index];
    const AbValue *id = ab_value_member(row, "id");
    status = validate_result(engine, row);
    if (status != ARCHBIRD_OK)
      return status;
    if (index && ab_string_compare(
                     &ab_value_member(&results->as.array.items[index - 1], "id")
                          ->as.text,
                     &id->as.text) >= 0) {
      status = reject(engine, ARCHBIRD_INVALID_SCHEMA,
                      "results are not uniquely sorted", id);
      return status;
    }
  }
  for (index = 0; index < gates->as.object.count; index++) {
    const AbString *id = &gates->as.object.fields[index].name;
    const AbValue *definition = &gates->as.object.fields[index].value;
    const AbValue *row = result_by_id(results, id);
    char expected[65];
    if (!row) {
      status = reject(engine, ARCHBIRD_INVALID_SCHEMA,
                      "configured gate has no result", NULL);
      return status;
    }
    status = ab_gate_definition_sha256(engine, definition, expected);
    if (status != ARCHBIRD_OK)
      return status;
    if (memcmp(expected,
               ab_value_member(row, "definition_sha256")->as.text.data,
               64) != 0) {
      status = reject(engine, ARCHBIRD_CONFLICT, "definition identity differs",
                      ab_value_member(row, "id"));
      return status;
    }
    if (!ab_artifact_text_is(ab_value_member(row, "status"), "pass")) {
      status = reject(engine, ARCHBIRD_POLICY_REJECTED, "command did not pass",
                      ab_value_member(row, "id"));
      return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_gate_acceptance_load(ArchbirdEngine *engine,
                                           const AbValue *gates,
                                           const uint8_t *json, size_t length,
                                           AbActGateAcceptance *out) {
  static const char *const fields[] = {"workspace_sha256", "results"};
  static const uint8_t empty[] = "{\"results\":[],\"workspace_sha256\":null}";
  ArchbirdStatus status;
  if (!engine || !gates || !out || !ab_gate_definitions_valid(gates) ||
      (!json && length))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!length) {
    if (gates->as.object.count)
      return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                    "configured gates have no execution results", NULL);
    json = empty;
    length = sizeof(empty) - 1;
  }
  status = ab_json_value_decode(engine, json, length, &out->document);
  if (status != ARCHBIRD_OK)
    return status;
  out->workspace_sha256 = ab_value_member(&out->document, "workspace_sha256");
  out->results = ab_value_member(&out->document, "results");
  if (!ab_artifact_object_exact(&out->document, fields, 2)) {
    status = reject(engine, ARCHBIRD_INVALID_SCHEMA,
                    "execution result has invalid fields", NULL);
  } else {
    status = ab_act_gate_acceptance_validate(
        engine, gates, out->workspace_sha256, out->results);
  }
  if (status != ARCHBIRD_OK)
    ab_act_gate_acceptance_free(engine, out);
  return status;
}

void ab_act_gate_acceptance_free(ArchbirdEngine *engine,
                                 AbActGateAcceptance *acceptance) {
  if (!acceptance)
    return;
  ab_value_free(engine, &acceptance->document);
  memset(acceptance, 0, sizeof(*acceptance));
}
