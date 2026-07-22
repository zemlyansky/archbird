#include "act_internal.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus artifact_invalid(ArchbirdEngine *engine,
                                       const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int nonblank(const AbValue *value) {
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

static int tool_valid(const AbValue *tool) {
  static const char *const allowed[] = {
      "implementation_sha256",
      "name",
      "version",
  };
  return ab_act_object_fields_allowed(tool, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) &&
         ab_value_string_is(ab_value_member(tool, "name"), "archbird") &&
         nonblank(ab_value_member(tool, "version")) &&
         ab_act_lowercase_sha256(
             ab_value_member(tool, "implementation_sha256"));
}

static int strings_unique(const AbValue *rows) {
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *value = &rows->as.array.items[index];
    if (!nonblank(value))
      return 0;
    for (previous = 0; previous < index; previous++) {
      if (ab_string_equal(&value->as.text,
                          &rows->as.array.items[previous].as.text))
        return 0;
    }
  }
  return 1;
}

static int object_ids_unique(const AbValue *rows, const char *field) {
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *id = ab_value_member(&rows->as.array.items[index], field);
    if (!ab_act_identifier(id))
      return 0;
    for (previous = 0; previous < index; previous++) {
      const AbValue *other =
          ab_value_member(&rows->as.array.items[previous], field);
      if (other && ab_string_equal(&id->as.text, &other->as.text))
        return 0;
    }
  }
  return 1;
}

static ArchbirdStatus validate_evidence_rows(ArchbirdEngine *engine,
                                             const AbValue *rows) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return artifact_invalid(engine, "invalid Act evidence array");
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

static ArchbirdStatus validate_seal(ArchbirdEngine *engine, const AbValue *root,
                                    char output[65],
                                    const char *invalid_message) {
  const AbValue *declared = ab_value_member(root, "sha256");
  char actual[65];
  ArchbirdStatus status;
  if (!ab_act_lowercase_sha256(declared))
    return artifact_invalid(engine, invalid_message);
  status = ab_act_value_digest_without_sha256(engine, root, actual);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(actual, declared->as.text.data, 64) != 0)
    return artifact_invalid(engine, invalid_message);
  memcpy(output, actual, 65);
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_proposal_facts(ArchbirdEngine *engine,
                                              const AbValue *facts) {
  size_t index;
  if (!facts || facts->kind != AB_VALUE_ARRAY)
    return artifact_invalid(engine, "invalid Act proposal fact inventory");
  for (index = 0; index < facts->as.array.count; index++) {
    AbProjectionData fact = {0};
    ArchbirdStatus status = ab_projection_data_decode_artifact(
        engine, &facts->as.array.items[index], &fact);
    ab_projection_data_free(engine, &fact);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return object_ids_unique(facts, "name")
             ? ARCHBIRD_OK
             : artifact_invalid(engine, "invalid Act proposal fact identities");
}

static ArchbirdStatus validate_evidence_slice(ArchbirdEngine *engine,
                                              const AbValue *slice) {
  static const char *const allowed[] = {"entries", "sha256"};
  static const char *const entry_allowed[] = {"kind", "name", "sha256"};
  const AbValue *entries = ab_value_member(slice, "entries");
  const AbValue *declared = ab_value_member(slice, "sha256");
  char actual[65];
  size_t index;
  ArchbirdStatus status;
  if (!ab_act_object_fields_allowed(slice, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !entries || entries->kind != AB_VALUE_ARRAY ||
      entries->as.array.count < 4 || !ab_act_lowercase_sha256(declared))
    return artifact_invalid(engine, "invalid Act evidence slice");
  status = ab_act_value_digest(engine, entries, actual);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(actual, declared->as.text.data, 64) != 0)
    return artifact_invalid(engine, "Act evidence slice seal does not match");
  for (index = 0; index < entries->as.array.count; index++) {
    const AbValue *row = &entries->as.array.items[index];
    if (!ab_act_object_fields_allowed(row, entry_allowed,
                                      sizeof(entry_allowed) /
                                          sizeof(entry_allowed[0])) ||
        !ab_act_identifier(ab_value_member(row, "kind")) ||
        !nonblank(ab_value_member(row, "name")) ||
        !ab_act_lowercase_sha256(ab_value_member(row, "sha256")))
      return artifact_invalid(engine, "invalid Act evidence slice entry");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_proposal_rows(ArchbirdEngine *engine,
                       const AbActProposalView *proposal) {
  size_t index;
  if (!object_ids_unique(proposal->postconditions, "id") ||
      !object_ids_unique(proposal->preserved, "id") ||
      !object_ids_unique(proposal->candidates, "id") ||
      !object_ids_unique(proposal->unknowns, "id"))
    return artifact_invalid(engine, "invalid Act proposal row identities");
  {
    ArchbirdStatus status = validate_evidence_rows(engine, proposal->evidence);
    if (status != ARCHBIRD_OK)
      return status;
  }
  for (index = 0; index < proposal->postconditions->as.array.count; index++) {
    const AbValue *row = &proposal->postconditions->as.array.items[index];
    if (!nonblank(ab_value_member(row, "derivation_strength")) ||
        !ab_value_member(row, "constraint") ||
        ab_value_member(row, "constraint")->kind != AB_VALUE_OBJECT ||
        !ab_value_member(row, "coverage") ||
        ab_value_member(row, "coverage")->kind != AB_VALUE_OBJECT)
      return artifact_invalid(engine, "invalid Act proposal postcondition");
    {
      ArchbirdStatus status =
          validate_evidence_rows(engine, ab_value_member(row, "evidence"));
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index < proposal->candidates->as.array.count; index++) {
    const AbValue *row = &proposal->candidates->as.array.items[index];
    if (!ab_act_identifier(ab_value_member(row, "kind")) ||
        !ab_act_identifier(ab_value_member(row, "project")) ||
        !nonblank(ab_value_member(row, "path")) ||
        !nonblank(ab_value_member(row, "reason")))
      return artifact_invalid(engine, "invalid Act proposal candidate");
    {
      ArchbirdStatus status =
          validate_evidence_rows(engine, ab_value_member(row, "evidence"));
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index < proposal->unknowns->as.array.count; index++) {
    const AbValue *row = &proposal->unknowns->as.array.items[index];
    if (!ab_act_identifier(ab_value_member(row, "code")) ||
        !nonblank(ab_value_member(row, "scope")) ||
        !nonblank(ab_value_member(row, "message")))
      return artifact_invalid(engine, "invalid Act proposal unknown");
    {
      ArchbirdStatus status =
          validate_evidence_rows(engine, ab_value_member(row, "evidence"));
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_proposal_load(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length,
                                    AbActProposalView *out) {
  static const char *const allowed[] = {
      "artifact",       "candidates",     "evidence",
      "evidence_slice", "facts",          "mutable_sources",
      "origin",         "postconditions", "preserved_invariants",
      "projections",    "provenance",     "schema_version",
      "sha256",         "source",         "tool",
      "unknowns",
  };
  const AbValue *schema;
  uint64_t version;
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
  out->source = ab_value_member(&out->root, "source");
  out->origin = ab_value_member(&out->root, "origin");
  out->mutable_sources = ab_value_member(&out->root, "mutable_sources");
  out->evidence_slice = ab_value_member(&out->root, "evidence_slice");
  out->facts = ab_value_member(&out->root, "facts");
  out->projections = ab_value_member(&out->root, "projections");
  out->postconditions = ab_value_member(&out->root, "postconditions");
  out->preserved = ab_value_member(&out->root, "preserved_invariants");
  out->candidates = ab_value_member(&out->root, "candidates");
  out->unknowns = ab_value_member(&out->root, "unknowns");
  out->evidence = ab_value_member(&out->root, "evidence");
  if (!ab_act_object_fields_allowed(&out->root, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_value_u64(schema, &version) || version != 2 ||
      !ab_value_string_is(ab_value_member(&out->root, "artifact"),
                          "change-proposal") ||
      !ab_value_string_is(ab_value_member(&out->root, "provenance"),
                          "derived") ||
      !tool_valid(out->tool) || !out->source ||
      out->source->kind != AB_VALUE_OBJECT || !out->origin ||
      out->origin->kind != AB_VALUE_OBJECT ||
      !strings_unique(out->mutable_sources) || !out->projections ||
      out->projections->kind != AB_VALUE_ARRAY || !out->postconditions ||
      out->postconditions->kind != AB_VALUE_ARRAY || !out->preserved ||
      out->preserved->kind != AB_VALUE_ARRAY || !out->candidates ||
      out->candidates->kind != AB_VALUE_ARRAY || !out->unknowns ||
      out->unknowns->kind != AB_VALUE_ARRAY) {
    status = artifact_invalid(
        engine, "invalid or unsealed canonical architecture change proposal");
    goto fail;
  }
  status = validate_evidence_slice(engine, out->evidence_slice);
  if (status == ARCHBIRD_OK)
    status = validate_proposal_facts(engine, out->facts);
  if (status == ARCHBIRD_OK)
    status = validate_proposal_rows(engine, out);
  if (status == ARCHBIRD_OK)
    status = validate_seal(
        engine, &out->root, out->sha256,
        "invalid or unsealed canonical architecture change proposal");
  if (status != ARCHBIRD_OK)
    goto fail;
  return ARCHBIRD_OK;

fail:
  ab_act_proposal_view_free(out);
  return status;
}

void ab_act_proposal_view_free(AbActProposalView *proposal) {
  if (!proposal)
    return;
  ab_value_free(proposal->engine, &proposal->root);
  memset(proposal, 0, sizeof(*proposal));
}

ArchbirdStatus ab_act_contract_load(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length,
                                    AbActContractView *out) {
  static const char *const allowed[] = {
      "acknowledged_unknowns",
      "artifact",
      "objective",
      "origin",
      "owner",
      "postconditions",
      "preserved_constraints",
      "proposal_sha256",
      "provenance",
      "rationale",
      "schema_version",
      "selected_candidates",
      "sha256",
      "tool",
  };
  const AbValue *schema;
  uint64_t version;
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
  out->origin = ab_value_member(&out->root, "origin");
  out->postconditions = ab_value_member(&out->root, "postconditions");
  out->preserved = ab_value_member(&out->root, "preserved_constraints");
  out->selected_candidates = ab_value_member(&out->root, "selected_candidates");
  out->acknowledged_unknowns =
      ab_value_member(&out->root, "acknowledged_unknowns");
  if (!ab_act_object_fields_allowed(&out->root, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_value_u64(schema, &version) || version != 2 ||
      !ab_value_string_is(ab_value_member(&out->root, "artifact"),
                          "change-contract") ||
      !ab_value_string_is(ab_value_member(&out->root, "provenance"),
                          "asserted") ||
      !tool_valid(out->tool) || !out->origin ||
      out->origin->kind != AB_VALUE_OBJECT ||
      !ab_act_lowercase_sha256(
          ab_value_member(&out->root, "proposal_sha256")) ||
      !nonblank(ab_value_member(&out->root, "objective")) ||
      !nonblank(ab_value_member(&out->root, "owner")) ||
      !nonblank(ab_value_member(&out->root, "rationale")) ||
      !strings_unique(out->postconditions) ||
      !object_ids_unique(out->preserved, "id") ||
      !strings_unique(out->selected_candidates) ||
      !strings_unique(out->acknowledged_unknowns)) {
    status = artifact_invalid(
        engine, "invalid or unsealed canonical architecture change contract");
    goto contract_fail;
  }
  status = validate_seal(
      engine, &out->root, out->sha256,
      "invalid or unsealed canonical architecture change contract");
  if (status != ARCHBIRD_OK)
    goto contract_fail;
  return ARCHBIRD_OK;

contract_fail:
  ab_act_contract_view_free(out);
  return status;
}

void ab_act_contract_view_free(AbActContractView *contract) {
  if (!contract)
    return;
  ab_value_free(contract->engine, &contract->root);
  memset(contract, 0, sizeof(*contract));
}
