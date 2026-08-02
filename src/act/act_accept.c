#include <archbird/archbird.h>

#include "act_internal.h"
#include "act_projection_acceptance.h"
#include "artifact_validation.h"
#include "projection_internal.h"
#include "render_internal.h"
#include "sha256.h"
#include "verification_artifact.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(ArchbirdEngine *engine, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act acceptance: %s", message);
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static int map_identity_matches(const AbValue *map, const char *sha256,
                                const AbValue *identity) {
  const AbValue *evidence = field(map, "evidence");
  const AbValue *tool = field(map, "tool");
  return evidence && tool &&
         memcmp(sha256, field(identity, "sha256")->as.text.data, 64) == 0 &&
         ab_value_equal(field(identity, "input_sha256"),
                        field(evidence, "input_sha256")) &&
         ab_value_equal(field(identity, "configuration_sha256"),
                        field(evidence, "config_sha256")) &&
         ab_value_equal(field(identity, "producer_implementation_sha256"),
                        field(tool, "implementation_sha256"));
}

static int diagnostic_identity_equal(const AbValue *left,
                                     const AbValue *right) {
  static const char *const fields[] = {"severity", "code", "path"};
  size_t index;
  for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
    const AbValue *a = field(left, fields[index]);
    const AbValue *b = field(right, fields[index]);
    if ((!a && b) || (a && !b) || (a && !ab_value_equal(a, b)))
      return 0;
  }
  return 1;
}

static int diagnostic_count(const AbValue *rows, const AbValue *needle) {
  size_t index;
  int count = 0;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return -1;
  for (index = 0; index < rows->as.array.count; index++)
    if (diagnostic_identity_equal(&rows->as.array.items[index], needle))
      count++;
  return count;
}

static ArchbirdStatus validate_diagnostic_delta(ArchbirdEngine *engine,
                                                const AbValue *before,
                                                const AbValue *after) {
  const AbValue *before_rows = field(before, "diagnostics");
  const AbValue *after_rows = field(after, "diagnostics");
  size_t index;
  if (!before_rows || before_rows->kind != AB_VALUE_ARRAY || !after_rows ||
      after_rows->kind != AB_VALUE_ARRAY)
    return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                  "Maps have no diagnostic inventories");
  for (index = 0; index < after_rows->as.array.count; index++) {
    const AbValue *row = &after_rows->as.array.items[index];
    const AbValue *severity = field(row, "severity");
    if (ab_artifact_text_is(severity, "error"))
      return reject(engine, ARCHBIRD_POLICY_REJECTED,
                    "after-state Map contains an error diagnostic");
    if (diagnostic_count(after_rows, row) > diagnostic_count(before_rows, row))
      return reject(engine, ARCHBIRD_POLICY_REJECTED,
                    "after-state Map introduces a diagnostic");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_verification_binding(
    ArchbirdEngine *engine, const AbAct *act, const AbValue *after_map,
    const AbVerificationArtifact *verification, const char after_map_sha[65]) {
  const AbValue *source_project = field(act->source, "project");
  const AbValue *map_project = field(after_map, "project");
  const AbValue *evaluation = verification->evaluation;
  const AbValue *policy = verification->policy;
  const AbValue *map_evidence = field(after_map, "evidence");
  const AbValue *map_tool = field(after_map, "tool");
  const AbValue *source_verification = field(act->source, "verification");
  (void)after_map_sha;
  if (!source_project || !map_project ||
      !ab_value_equal(source_project, map_project) ||
      !ab_value_equal(source_project, field(evaluation, "project")) ||
      !ab_value_equal(source_project, field(policy, "project")) ||
      !ab_value_equal(field(evaluation, "map_input_sha256"),
                      field(map_evidence, "input_sha256")) ||
      !ab_value_equal(field(evaluation, "map_config_sha256"),
                      field(map_evidence, "config_sha256")) ||
      !ab_value_equal(field(evaluation, "map_producer_implementation_sha256"),
                      field(map_tool, "implementation_sha256")) ||
      !ab_value_equal(field(source_verification, "policy_sha256"),
                      field(policy, "constraint_policy_sha256")))
    return reject(engine, ARCHBIRD_CONFLICT,
                  "after Map and Verification identities differ");
  return ARCHBIRD_OK;
}

static ArchbirdStatus
collect_accepted_constraints(ArchbirdEngine *engine, const AbAct *act,
                             const AbVerificationArtifact *verification,
                             const AbValue ***out_rows, size_t *out_count) {
  const AbValue *requirements = field(act->acceptance, "constraints");
  const AbValue **rows = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_rows = NULL;
  *out_count = 0;
  if (requirements->as.array.count != verification->constraints->as.array.count)
    return reject(engine, ARCHBIRD_POLICY_REJECTED,
                  "Plan acceptance does not cover the complete policy");
  if (requirements->as.array.count) {
    rows = (const AbValue **)ab_calloc(engine, requirements->as.array.count,
                                       sizeof(*rows));
    if (!rows)
      return reject(engine, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory collecting constraint results");
  }
  for (index = 0; index < requirements->as.array.count; index++) {
    const AbValue *requirement = &requirements->as.array.items[index];
    const AbValue *id = field(requirement, "id");
    const AbValue *row =
        ab_verification_artifact_constraint(verification, &id->as.text);
    const AbValue *row_status = field(row, "status");
    if (!row) {
      status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                      "fresh Verification omits a required constraint");
      break;
    }
    if (ab_artifact_text_is(row_status, "fail")) {
      status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                      "fresh Verification has a failing constraint");
      break;
    }
    if (ab_artifact_text_is(row_status, "unknown")) {
      status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                      "fresh Verification has an unknown constraint");
      break;
    }
    if (!ab_artifact_text_is(row_status, "pass") &&
        !ab_artifact_text_is(row_status, "waived") &&
        !ab_artifact_text_is(row_status, "not_applicable")) {
      status = reject(engine, ARCHBIRD_INVALID_SCHEMA,
                      "fresh Verification has an invalid status");
      break;
    }
    rows[index] = row;
  }
  if (status != ARCHBIRD_OK) {
    ab_free(engine, rows);
    return status;
  }
  *out_rows = rows;
  *out_count = requirements->as.array.count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_identity(AbBuffer *buffer, const AbValue *map,
                                      const char map_sha[65]) {
  const AbValue *evidence = field(map, "evidence");
  const AbValue *tool = field(map, "tool");
  ArchbirdStatus status =
      ab_buffer_literal(buffer, "{\"configuration_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(evidence, "config_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(evidence, "input_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"producer_implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(tool, "implementation_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, map_sha, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus
render_verification_identity(AbBuffer *buffer,
                             const AbVerificationArtifact *verification) {
  const AbValue *result =
      field(&verification->root, "verification_result_sha256");
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"policy_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(
        buffer, field(verification->policy, "constraint_policy_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"producer_implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(
        buffer, field(verification->tool, "implementation_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, result);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_constraint_results(AbBuffer *buffer,
                                                const AbValue *const *rows,
                                                size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"id\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, field(rows[index], "id"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"status\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, field(rows[index], "status"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_accepted_document(
    AbBuffer *buffer, const AbAct *act, const AbValue *after_map,
    const char after_map_sha[65], const AbVerificationArtifact *verification,
    const AbValue *const *constraint_rows, size_t constraint_count,
    const AbBuffer *projection_deltas, const char *seal) {
  const AbValue *result_sha =
      field(&verification->root, "verification_result_sha256");
  ArchbirdStatus status =
      ab_buffer_literal(buffer, "{\"acceptance\":{\"constraints\":");
  if (status == ARCHBIRD_OK)
    status =
        render_constraint_results(buffer, constraint_rows, constraint_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"projection_deltas\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, projection_deltas->data,
                              projection_deltas->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, ",\"status\":\"satisfied\",\"verification_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, result_sha);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"after\":{\"map\":");
  if (status == ARCHBIRD_OK)
    status = render_identity(buffer, after_map, after_map_sha);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"verification\":");
  if (status == ARCHBIRD_OK)
    status = render_verification_identity(buffer, verification);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "},\"artifact\":\"act\","
                                       "\"executors\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(&act->document, "executors"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"plan_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(&act->document, "plan_sha256"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, ",\"provenance\":\"derived\",\"schema_version\":4,\"seal\":");
  if (status == ARCHBIRD_OK) {
    if (seal) {
      status = ab_buffer_literal(buffer, "{\"content_sha256\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, seal, 64);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    } else {
      status = ab_buffer_literal(buffer, "null");
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"source\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, act->source);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"source_locks\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, act->source_locks);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"state\":\"accepted\",\"tool\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(&act->document, "tool"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"transitions\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, field(&act->document, "transitions"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

ArchbirdStatus
archbird_act_accept(ArchbirdEngine *engine, const uint8_t *act_json,
                    size_t act_length, const uint8_t *before_map_json,
                    size_t before_map_length, const uint8_t *after_map_json,
                    size_t after_map_length, const uint8_t *verification_json,
                    size_t verification_length, uint32_t json_flags,
                    ArchbirdWriteFn write_fn, void *user_data) {
  AbAct act = {0};
  AbVerificationArtifact verification = {0};
  AbValue before_map = {0};
  AbValue after_map = {0};
  const AbValue **constraint_rows = NULL;
  size_t constraint_count = 0;
  AbBuffer unsealed;
  AbBuffer canonical;
  AbBuffer sealed;
  AbBuffer accepted;
  AbBuffer projection_deltas;
  AbAct accepted_act = {0};
  char before_sha[65];
  char after_sha[65];
  char seal[65];
  ArchbirdStatus status;
  if (!engine || !act_json || !act_length || !before_map_json ||
      !before_map_length || !after_map_json || !after_map_length ||
      !verification_json || !verification_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&unsealed, engine);
  ab_buffer_init(&canonical, engine);
  ab_buffer_init(&sealed, engine);
  ab_buffer_init(&accepted, engine);
  ab_buffer_init(&projection_deltas, engine);
  status = ab_act_load(engine, act_json, act_length, &act);
  if (status == ARCHBIRD_OK &&
      !ab_artifact_text_is(field(&act.document, "state"), "materialized"))
    status = reject(engine, ARCHBIRD_INVALID_SCHEMA,
                    "input Act is not materialized");
  if (status == ARCHBIRD_OK &&
      memcmp(field(field(&act.document, "tool"), "implementation_sha256")
                 ->as.text.data,
             archbird_implementation_sha256(), 64) != 0)
    status = reject(engine, ARCHBIRD_CONFLICT,
                    "materializer implementation differs");
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, before_map_json, before_map_length,
                                  &before_map);
  if (status == ARCHBIRD_OK)
    status = ab_projection_map_validate(engine, &before_map, "Act before Map");
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, after_map_json, after_map_length,
                                  &after_map);
  if (status == ARCHBIRD_OK)
    status = ab_projection_map_validate(engine, &after_map, "Act after Map");
  if (status == ARCHBIRD_OK)
    status = ab_artifact_json_sha256(engine, before_map_json, before_map_length,
                                     before_sha);
  if (status == ARCHBIRD_OK &&
      !map_identity_matches(&before_map, before_sha, field(act.source, "map")))
    status = reject(engine, ARCHBIRD_CONFLICT,
                    "materialized Act does not match the before Map");
  if (status == ARCHBIRD_OK)
    status = ab_artifact_json_sha256(engine, after_map_json, after_map_length,
                                     after_sha);
  if (status == ARCHBIRD_OK)
    status = ab_verification_artifact_load(engine, verification_json,
                                           verification_length, &verification);
  if (status == ARCHBIRD_OK)
    status = validate_verification_binding(engine, &act, &after_map,
                                           &verification, after_sha);
  if (status == ARCHBIRD_OK)
    status = validate_diagnostic_delta(engine, &before_map, &after_map);
  if (status == ARCHBIRD_OK)
    status = ab_act_projection_deltas_accept(
        engine, field(act.acceptance, "projection_deltas"), &before_map,
        &after_map, &projection_deltas);
  if (status == ARCHBIRD_OK)
    status = collect_accepted_constraints(engine, &act, &verification,
                                          &constraint_rows, &constraint_count);
  if (status == ARCHBIRD_OK)
    status = render_accepted_document(
        &unsealed, &act, &after_map, after_sha, &verification, constraint_rows,
        constraint_count, &projection_deltas, NULL);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, unsealed.data, unsealed.length,
                                        0, buffer_write, &canonical);
  if (status == ARCHBIRD_OK)
    status =
        ab_artifact_json_sha256(engine, canonical.data, canonical.length, seal);
  if (status == ARCHBIRD_OK)
    status = render_accepted_document(
        &sealed, &act, &after_map, after_sha, &verification, constraint_rows,
        constraint_count, &projection_deltas, seal);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, sealed.data, sealed.length, 0,
                                        buffer_write, &accepted);
  if (status == ARCHBIRD_OK)
    status = ab_act_load(engine, accepted.data, accepted.length, &accepted_act);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, accepted.data, accepted.length,
                                        json_flags, write_fn, user_data);
  ab_act_free(engine, &accepted_act);
  ab_buffer_free(&accepted);
  ab_buffer_free(&projection_deltas);
  ab_free(engine, constraint_rows);
  ab_buffer_free(&sealed);
  ab_buffer_free(&canonical);
  ab_buffer_free(&unsealed);
  ab_value_free(engine, &after_map);
  ab_value_free(engine, &before_map);
  ab_verification_artifact_free(&verification);
  ab_act_free(engine, &act);
  return status;
}
