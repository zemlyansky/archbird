#include "act_internal.h"

#include <stdlib.h>
#include <string.h>

static int string_nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

int ab_act_lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

int ab_act_identifier(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    int allowed = (byte >= 'A' && byte <= 'Z') ||
                  (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') ||
                  (index > 0 &&
                   (byte == '_' || byte == '.' || byte == ':' || byte == '-'));
    if (!allowed)
      return 0;
  }
  return 1;
}

int ab_act_object_fields_allowed(const AbValue *object,
                                 const char *const *allowed, size_t count) {
  size_t field_index;
  size_t name_index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (field_index = 0; field_index < object->as.object.count; field_index++) {
    const AbString *name = &object->as.object.fields[field_index].name;
    int found = 0;
    for (name_index = 0; name_index < count; name_index++) {
      size_t length = strlen(allowed[name_index]);
      if (name->length == length &&
          (!length || memcmp(name->data, allowed[name_index], length) == 0)) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

ArchbirdStatus ab_act_value_digest(ArchbirdEngine *engine, const AbValue *value,
                                   char output[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  if (!engine || !value || !output)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  ab_buffer_free(&buffer);
  return status;
}

ArchbirdStatus ab_act_value_digest_without_sha256(ArchbirdEngine *engine,
                                                  const AbValue *value,
                                                  char output[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  size_t index;
  size_t written = 0;
  if (!engine || !value || value->kind != AB_VALUE_OBJECT || !output)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < value->as.object.count;
       index++) {
    const AbObjectField *field = &value->as.object.fields[index];
    if (field->name.length == 6 && memcmp(field->name.data, "sha256", 6) == 0)
      continue;
    if (written++)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(&buffer, field->name.data, field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&buffer, &field->value);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  ab_buffer_free(&buffer);
  return status;
}

static int string_array(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    if (!string_nonblank(&value->as.array.items[index]))
      return 0;
  }
  return 1;
}

static int string_in(const AbValue *value, const char *const *values,
                     size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (ab_value_string_is(value, values[index]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int validate_tool(const AbValue *tool) {
  static const char *const allowed[] = {
      "implementation_sha256",
      "name",
      "version",
  };
  return ab_act_object_fields_allowed(tool, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) &&
         ab_value_string_is(ab_value_member(tool, "name"), "archbird") &&
         string_nonblank(ab_value_member(tool, "version")) &&
         ab_act_lowercase_sha256(
             ab_value_member(tool, "implementation_sha256"));
}

static ArchbirdStatus validate_evidence_array(ArchbirdEngine *engine,
                                              const AbValue *rows) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return invalid(engine, "invalid canonical verification evidence array");
  for (index = 0; index < rows->as.array.count; index++) {
    AbProjectionEvidence evidence = {0};
    ArchbirdStatus status = ab_projection_evidence_decode_artifact(
        engine, &rows->as.array.items[index], &evidence);
    ab_projection_evidence_free(engine, &evidence);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_finding(ArchbirdEngine *engine,
                                       const AbValue *row) {
  static const char *const allowed[] = {
      "applicability", "baseline_state", "comparison",  "disposition",
      "evidence",      "evidence_state", "fingerprint", "key",
      "message",       "waiver",         "waiver_note",
  };
  static const char *const comparisons[] = {
      "different",
      "equal",
      "extra",
      "missing",
  };
  static const char *const evidence_states[] = {
      "current",
      "stale",
      "unknown",
  };
  static const char *const applicability[] = {
      "applicable",
      "not_applicable",
  };
  static const char *const dispositions[] = {"open", "waived"};
  const AbValue *waiver = ab_value_member(row, "waiver");
  const AbValue *waiver_note = ab_value_member(row, "waiver_note");
  const AbValue *baseline = ab_value_member(row, "baseline_state");
  if (!ab_act_object_fields_allowed(row, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_act_lowercase_sha256(ab_value_member(row, "fingerprint")) ||
      !string_in(ab_value_member(row, "comparison"), comparisons,
                 sizeof(comparisons) / sizeof(comparisons[0])) ||
      !string_in(ab_value_member(row, "evidence_state"), evidence_states,
                 sizeof(evidence_states) / sizeof(evidence_states[0])) ||
      !string_in(ab_value_member(row, "applicability"), applicability,
                 sizeof(applicability) / sizeof(applicability[0])) ||
      !string_in(ab_value_member(row, "disposition"), dispositions,
                 sizeof(dispositions) / sizeof(dispositions[0])) ||
      !string_nonblank(ab_value_member(row, "key")) ||
      !string_nonblank(ab_value_member(row, "message")) || !waiver ||
      waiver->kind != AB_VALUE_STRING || !waiver_note ||
      waiver_note->kind != AB_VALUE_STRING || !baseline ||
      baseline->kind != AB_VALUE_STRING)
    return invalid(engine, "invalid canonical verification finding");
  return validate_evidence_array(engine, ab_value_member(row, "evidence"));
}

static ArchbirdStatus validate_check(ArchbirdEngine *engine,
                                     const AbValue *row) {
  static const char *const allowed[] = {
      "assert",   "coverage", "findings",  "id",
      "operands", "owner",    "rationale", "requirements",
      "severity", "status",   "tags",      "witnesses",
  };
  static const char *const statuses[] = {
      "fail", "not_applicable", "pass", "unknown", "waived",
  };
  const AbValue *findings = ab_value_member(row, "findings");
  const AbValue *operands = ab_value_member(row, "operands");
  const AbValue *coverage = ab_value_member(row, "coverage");
  size_t index;
  if (!ab_act_object_fields_allowed(row, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_act_identifier(ab_value_member(row, "id")) ||
      !string_nonblank(ab_value_member(row, "assert")) ||
      !string_nonblank(ab_value_member(row, "severity")) ||
      !string_nonblank(ab_value_member(row, "owner")) ||
      !string_nonblank(ab_value_member(row, "rationale")) ||
      !string_array(ab_value_member(row, "requirements")) ||
      !string_array(ab_value_member(row, "tags")) || !operands ||
      operands->kind != AB_VALUE_OBJECT ||
      !string_in(ab_value_member(row, "status"), statuses,
                 sizeof(statuses) / sizeof(statuses[0])) ||
      !string_array(coverage) || !findings || findings->kind != AB_VALUE_ARRAY)
    return invalid(engine, "invalid canonical verification check");
  {
    ArchbirdStatus status =
        validate_evidence_array(engine, ab_value_member(row, "witnesses"));
    if (status != ARCHBIRD_OK)
      return status;
  }
  for (index = 0; index < findings->as.array.count; index++) {
    ArchbirdStatus status =
        validate_finding(engine, &findings->as.array.items[index]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int named_rows_unique(const AbValue *rows, const char *field,
                             int identifiers) {
  size_t index;
  size_t other;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *name = ab_value_member(&rows->as.array.items[index], field);
    if ((identifiers && !ab_act_identifier(name)) ||
        (!identifiers && !string_nonblank(name)))
      return 0;
    for (other = 0; other < index; other++) {
      const AbValue *previous =
          ab_value_member(&rows->as.array.items[other], field);
      if (previous && ab_string_equal(&previous->as.text, &name->as.text))
        return 0;
    }
  }
  return 1;
}

static const AbValue *named_row(const AbValue *rows, const char *field,
                                const AbString *name);

ArchbirdStatus ab_act_verification_load(ArchbirdEngine *engine,
                                        const uint8_t *json, size_t json_length,
                                        AbActVerification *out) {
  static const char *const root_allowed[] = {
      "artifact",
      "constraints",
      "diagnostics",
      "evaluations",
      "mappings",
      "observations",
      "operands",
      "operand_definitions",
      "policy",
      "schema_version",
      "summary",
      "tool",
      "verification_result_sha256",
  };
  static const char *const evaluation_allowed[] = {
      "id",
      "map_config_sha256",
      "map_input_sha256",
      "map_producer_implementation_sha256",
      "project",
      "resolution_sha256",
  };
  static const char *const policy_allowed[] = {
      "configured_count",
      "constraint_policy_sha256",
      "constraints",
      "evaluated_count",
      "kind",
      "omitted_count",
      "project",
      "project_configuration_sha256",
      "requested_ids",
  };
  const AbValue *schema;
  const AbValue *identities;
  const AbValue *diagnostics;
  const AbValue *summary;
  const AbValue *result_sha;
  uint64_t schema_version;
  size_t index;
  static const AbString current = {(char *)"current", 7};
  ArchbirdStatus status;
  if (!engine || (!json && json_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  status = ab_json_value_decode(engine, json, json_length, &out->root);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(&out->root, "schema_version");
  out->tool = ab_value_member(&out->root, "tool");
  out->policy = ab_value_member(&out->root, "policy");
  out->evaluations = ab_value_member(&out->root, "evaluations");
  out->mappings = ab_value_member(&out->root, "mappings");
  out->observations = ab_value_member(&out->root, "observations");
  out->operand_definitions = ab_value_member(&out->root, "operand_definitions");
  out->operands = ab_value_member(&out->root, "operands");
  out->constraints = ab_value_member(&out->root, "constraints");
  identities = out->policy ? ab_value_member(out->policy, "constraints") : NULL;
  diagnostics = ab_value_member(&out->root, "diagnostics");
  summary = ab_value_member(&out->root, "summary");
  result_sha = ab_value_member(&out->root, "verification_result_sha256");
  if (!ab_act_object_fields_allowed(&out->root, root_allowed,
                                    sizeof(root_allowed) /
                                        sizeof(root_allowed[0])) ||
      !ab_value_u64(schema, &schema_version) || schema_version != 2 ||
      !ab_value_string_is(ab_value_member(&out->root, "artifact"),
                          "verification") ||
      !validate_tool(out->tool) || !out->evaluations ||
      out->evaluations->kind != AB_VALUE_ARRAY ||
      !named_rows_unique(out->evaluations, "id", 1) ||
      !(out->evaluation = named_row(out->evaluations, "id", &current)) ||
      !ab_act_object_fields_allowed(out->evaluation, evaluation_allowed,
                                    sizeof(evaluation_allowed) /
                                        sizeof(evaluation_allowed[0])) ||
      !ab_act_lowercase_sha256(
          ab_value_member(out->evaluation, "map_config_sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(out->evaluation, "map_input_sha256")) ||
      !ab_act_lowercase_sha256(ab_value_member(
          out->evaluation, "map_producer_implementation_sha256")) ||
      !string_nonblank(ab_value_member(out->evaluation, "project")) ||
      !ab_act_object_fields_allowed(out->policy, policy_allowed,
                                    sizeof(policy_allowed) /
                                        sizeof(policy_allowed[0])) ||
      !ab_act_lowercase_sha256(
          ab_value_member(out->policy, "constraint_policy_sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(out->policy, "project_configuration_sha256")) ||
      !string_nonblank(ab_value_member(out->policy, "project")) ||
      !identities || identities->kind != AB_VALUE_ARRAY || !out->operands ||
      out->operands->kind != AB_VALUE_ARRAY ||
      !named_rows_unique(out->operands, "name", 1) || !out->mappings ||
      out->mappings->kind != AB_VALUE_OBJECT || !out->observations ||
      out->observations->kind != AB_VALUE_ARRAY ||
      !named_rows_unique(out->observations, "id", 1) ||
      !out->operand_definitions ||
      out->operand_definitions->kind != AB_VALUE_OBJECT || !out->constraints ||
      out->constraints->kind != AB_VALUE_ARRAY ||
      !named_rows_unique(out->constraints, "id", 1) || !summary ||
      summary->kind != AB_VALUE_OBJECT || !diagnostics ||
      diagnostics->kind != AB_VALUE_ARRAY ||
      !ab_act_lowercase_sha256(result_sha)) {
    status = invalid(engine, "invalid canonical verification artifact");
    goto fail;
  }
  for (index = 0; index < out->evaluations->as.array.count; index++) {
    const AbValue *evaluation = &out->evaluations->as.array.items[index];
    const AbValue *resolution =
        ab_value_member(evaluation, "resolution_sha256");
    if (!ab_act_object_fields_allowed(evaluation, evaluation_allowed,
                                      sizeof(evaluation_allowed) /
                                          sizeof(evaluation_allowed[0])) ||
        !ab_act_identifier(ab_value_member(evaluation, "id")) ||
        !ab_act_lowercase_sha256(
            ab_value_member(evaluation, "map_config_sha256")) ||
        !ab_act_lowercase_sha256(
            ab_value_member(evaluation, "map_input_sha256")) ||
        !ab_act_lowercase_sha256(ab_value_member(
            evaluation, "map_producer_implementation_sha256")) ||
        !string_nonblank(ab_value_member(evaluation, "project")) ||
        (resolution && resolution->kind != AB_VALUE_NULL &&
         !ab_act_lowercase_sha256(resolution))) {
      status = invalid(engine, "invalid constraint Map evaluation");
      goto fail;
    }
  }
  for (index = 0; index < identities->as.array.count; index++) {
    const AbValue *identity = &identities->as.array.items[index];
    if (!ab_act_identifier(ab_value_member(identity, "id")) ||
        !ab_act_lowercase_sha256(
            ab_value_member(identity, "constraint_definition_sha256")) ||
        !ab_act_lowercase_sha256(
            ab_value_member(identity, "constraint_plan_sha256")) ||
        !ab_act_lowercase_sha256(
            ab_value_member(identity, "constraint_result_sha256"))) {
      status = invalid(engine, "invalid constraint result identity");
      goto fail;
    }
  }
  out->fact_count = out->operands->as.array.count;
  if (out->fact_count > SIZE_MAX / sizeof(*out->decoded_facts)) {
    status =
        archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
                           "too many Act verification facts");
    goto fail;
  }
  if (out->fact_count) {
    out->decoded_facts = (AbProjectionData *)ab_calloc(
        engine, out->fact_count, sizeof(*out->decoded_facts));
    if (!out->decoded_facts) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory loading Act facts");
      goto fail;
    }
  }
  for (index = 0; index < out->fact_count; index++) {
    status = ab_projection_data_decode_artifact(
        engine, &out->operands->as.array.items[index],
        &out->decoded_facts[index]);
    if (status != ARCHBIRD_OK)
      goto fail;
  }
  for (index = 0; index < out->constraints->as.array.count; index++) {
    status = validate_check(engine, &out->constraints->as.array.items[index]);
    if (status != ARCHBIRD_OK)
      goto fail;
  }
  status = ab_act_value_digest(engine, &out->root, out->sha256);
  if (status != ARCHBIRD_OK)
    goto fail;
  return ARCHBIRD_OK;

fail:
  ab_act_verification_free(out);
  return status;
}

void ab_act_verification_free(AbActVerification *artifact) {
  ArchbirdEngine *engine;
  size_t index;
  if (!artifact)
    return;
  engine = artifact->engine;
  for (index = 0; artifact->decoded_facts && index < artifact->fact_count;
       index++)
    ab_projection_data_free(engine, &artifact->decoded_facts[index]);
  ab_free(engine, artifact->decoded_facts);
  ab_value_free(engine, &artifact->root);
  memset(artifact, 0, sizeof(*artifact));
}

static const AbValue *named_row(const AbValue *rows, const char *field,
                                const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY || !name)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *value = ab_value_member(row, field);
    if (value && value->kind == AB_VALUE_STRING &&
        ab_string_equal(&value->as.text, name))
      return row;
  }
  return NULL;
}

static const AbValue *named_object_value(const AbValue *object,
                                         const AbString *name) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT || !name)
    return NULL;
  for (index = 0; index < object->as.object.count; index++)
    if (ab_string_equal(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index].value;
  return NULL;
}

const AbValue *ab_act_verification_constraint(const AbActVerification *artifact,
                                              const AbString *id) {
  return named_row(artifact ? artifact->constraints : NULL, "id", id);
}

const AbValue *ab_act_verification_finding(const AbActVerification *artifact,
                                           const AbString *fingerprint,
                                           const AbValue **out_check) {
  const AbValue *match = NULL;
  size_t check_index;
  size_t matches = 0;
  if (out_check)
    *out_check = NULL;
  if (!artifact || !fingerprint)
    return NULL;
  for (check_index = 0; check_index < artifact->constraints->as.array.count;
       check_index++) {
    const AbValue *check = &artifact->constraints->as.array.items[check_index];
    const AbValue *findings = ab_value_member(check, "findings");
    size_t finding_index;
    for (finding_index = 0; finding_index < findings->as.array.count;
         finding_index++) {
      const AbValue *finding = &findings->as.array.items[finding_index];
      const AbValue *value = ab_value_member(finding, "fingerprint");
      if (value && ab_string_equal(&value->as.text, fingerprint)) {
        match = finding;
        matches++;
        if (out_check)
          *out_check = check;
      }
    }
  }
  return matches == 1 ? match : NULL;
}

const AbValue *
ab_act_verification_fact_value(const AbActVerification *artifact,
                               const AbString *name,
                               const AbProjectionData **out_fact) {
  size_t index;
  if (out_fact)
    *out_fact = NULL;
  if (!artifact || !name)
    return NULL;
  for (index = 0; index < artifact->fact_count; index++) {
    if (ab_string_equal(&artifact->decoded_facts[index].name, name)) {
      if (out_fact)
        *out_fact = &artifact->decoded_facts[index];
      return &artifact->operands->as.array.items[index];
    }
  }
  return NULL;
}

const AbValue *ab_act_verification_mapping(const AbActVerification *artifact,
                                           const AbString *name) {
  return named_object_value(artifact ? artifact->mappings : NULL, name);
}

const AbValue *
ab_act_verification_operand_definition(const AbActVerification *artifact,
                                       const AbString *name) {
  return named_object_value(artifact ? artifact->operand_definitions : NULL,
                            name);
}

const AbValue *
ab_act_verification_observation(const AbActVerification *artifact,
                                const AbString *name) {
  return named_row(artifact ? artifact->observations : NULL, "id", name);
}

ArchbirdStatus ab_act_evidence_list_add(ArchbirdEngine *engine,
                                        AbActEvidenceList *list,
                                        const AbProjectionEvidence *evidence) {
  AbProjectionEvidence *resized;
  ArchbirdStatus status;
  if (!engine || !list || !evidence)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (list->engine && list->engine != engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  list->engine = engine;
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*list->items))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too much Act evidence");
    resized = (AbProjectionEvidence *)ab_realloc(
        engine, list->items, capacity * sizeof(*list->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing Act evidence");
    list->items = resized;
    list->capacity = capacity;
  }
  memset(&list->items[list->count], 0, sizeof(*list->items));
  status = ab_projection_evidence_init(
      engine, &list->items[list->count], evidence->provenance.data,
      &evidence->project, &evidence->path, evidence->line,
      evidence->sha256.data, evidence->detail.data, evidence->detail.length);
  if (status == ARCHBIRD_OK)
    list->count++;
  return status;
}

ArchbirdStatus ab_act_evidence_list_add_array(ArchbirdEngine *engine,
                                              AbActEvidenceList *list,
                                              const AbValue *rows) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return invalid(engine, "Act evidence must be an array");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    AbProjectionEvidence evidence = {0};
    status = ab_projection_evidence_decode_artifact(
        engine, &rows->as.array.items[index], &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_act_evidence_list_add(engine, list, &evidence);
    ab_projection_evidence_free(engine, &evidence);
  }
  return status;
}

void ab_act_evidence_list_finish(AbActEvidenceList *list) {
  size_t read;
  size_t write = 0;
  if (!list)
    return;
  if (list->count > 1)
    qsort(list->items, list->count, sizeof(*list->items),
          ab_projection_evidence_compare);
  for (read = 0; read < list->count; read++) {
    if (write && ab_projection_evidence_compare(&list->items[write - 1],
                                                &list->items[read]) == 0) {
      ab_projection_evidence_free(list->engine, &list->items[read]);
      continue;
    }
    if (write != read) {
      list->items[write] = list->items[read];
      memset(&list->items[read], 0, sizeof(list->items[read]));
    }
    write++;
  }
  list->count = write;
}

void ab_act_evidence_list_free(AbActEvidenceList *list) {
  size_t index;
  if (!list)
    return;
  for (index = 0; list->items && index < list->count; index++)
    ab_projection_evidence_free(list->engine, &list->items[index]);
  ab_free(list->engine, list->items);
  memset(list, 0, sizeof(*list));
}

ArchbirdStatus ab_act_evidence_list_render(AbBuffer *buffer,
                                           const AbActEvidenceList *list) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < list->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_projection_evidence_render(buffer, &list->items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
