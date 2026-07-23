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

static int string_value(const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING;
}

static int null_or_u64(const AbValue *value) {
  uint64_t number;
  return value &&
         (value->kind == AB_VALUE_NULL || ab_value_u64(value, &number));
}

static int value_is_one_of(const AbValue *value, const char *const *allowed,
                           size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (ab_value_string_is(value, allowed[index]))
      return 1;
  }
  return 0;
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
  status = ab_act_value_digest_without_field(engine, root, "sha256", actual);
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

static int string_name_nonblank(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_proposal_projections(ArchbirdEngine *engine,
                                                    const AbValue *rows) {
  static const char *const allowed[] = {
      "aliases", "keys", "name", "selection", "source",
  };
  size_t index;
  if (!object_ids_unique(rows, "name"))
    return artifact_invalid(engine,
                            "invalid Act proposal projection identities");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *selection = ab_value_member(row, "selection");
    const AbValue *aliases = ab_value_member(row, "aliases");
    size_t alias_index;
    if (!ab_act_object_fields_allowed(row, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) ||
        !ab_act_identifier(ab_value_member(row, "source")) ||
        !(ab_value_string_is(selection, "all") ||
          ab_value_string_is(selection, "include") ||
          ab_value_string_is(selection, "exclude")) ||
        !strings_unique(ab_value_member(row, "keys")) || !aliases ||
        aliases->kind != AB_VALUE_OBJECT)
      return artifact_invalid(engine, "invalid Act proposal projection");
    for (alias_index = 0; alias_index < aliases->as.object.count;
         alias_index++) {
      const AbObjectField *alias = &aliases->as.object.fields[alias_index];
      if (!string_name_nonblank(&alias->name) || !nonblank(&alias->value))
        return artifact_invalid(engine, "invalid Act proposal projection");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_proposal_source(ArchbirdEngine *engine,
                                               const AbValue *source) {
  static const char *const allowed[] = {
      "evaluation",
      "policy",
      "verification_sha256",
      "verification_tool_implementation_sha256",
  };
  static const char *const evaluation_allowed[] = {
      "id",
      "map_config_sha256",
      "map_input_sha256",
      "map_producer_implementation_sha256",
      "project",
      "resolution_sha256",
  };
  static const char *const policy_allowed[] = {"project", "sha256"};
  const AbValue *evaluation = ab_value_member(source, "evaluation");
  const AbValue *policy = ab_value_member(source, "policy");
  const AbValue *resolution =
      evaluation ? ab_value_member(evaluation, "resolution_sha256") : NULL;
  const AbValue *evaluation_project =
      evaluation ? ab_value_member(evaluation, "project") : NULL;
  const AbValue *policy_project =
      policy ? ab_value_member(policy, "project") : NULL;
  if (!ab_act_object_fields_allowed(source, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !evaluation ||
      !ab_act_object_fields_allowed(evaluation, evaluation_allowed,
                                    sizeof(evaluation_allowed) /
                                        sizeof(evaluation_allowed[0])) ||
      !ab_act_identifier(ab_value_member(evaluation, "id")) ||
      !nonblank(evaluation_project) ||
      !ab_act_lowercase_sha256(
          ab_value_member(evaluation, "map_config_sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(evaluation, "map_input_sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(evaluation, "map_producer_implementation_sha256")) ||
      !resolution ||
      (resolution->kind != AB_VALUE_NULL &&
       !ab_act_lowercase_sha256(resolution)) ||
      !policy ||
      !ab_act_object_fields_allowed(policy, policy_allowed,
                                    sizeof(policy_allowed) /
                                        sizeof(policy_allowed[0])) ||
      !nonblank(policy_project) ||
      !ab_act_string_values_equal(evaluation_project, policy_project) ||
      !ab_act_lowercase_sha256(ab_value_member(policy, "sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(source, "verification_sha256")) ||
      !ab_act_lowercase_sha256(
          ab_value_member(source, "verification_tool_implementation_sha256")))
    return artifact_invalid(engine, "invalid Act proposal source");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_proposal_origin(ArchbirdEngine *engine,
                                               const AbValue *origin) {
  static const char *const allowed[] = {
      "actual",  "assert",         "constraint", "constraint_sha256",
      "finding", "finding_sha256",
  };
  const AbValue *constraint_sha = ab_value_member(origin, "constraint_sha256");
  const AbValue *finding = ab_value_member(origin, "finding");
  const AbValue *finding_sha = ab_value_member(origin, "finding_sha256");
  char actual[65];
  ArchbirdStatus status;
  if (!ab_act_object_fields_allowed(origin, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_act_identifier(ab_value_member(origin, "constraint")) ||
      !ab_act_lowercase_sha256(constraint_sha) ||
      !nonblank(ab_value_member(origin, "assert")) ||
      !string_value(ab_value_member(origin, "actual")) ||
      !ab_act_lowercase_sha256(finding_sha))
    return artifact_invalid(engine, "invalid Act proposal origin");
  status = ab_act_validate_finding(engine, finding);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_act_value_digest(engine, finding, actual);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(actual, finding_sha->as.text.data, 64) != 0)
    return artifact_invalid(engine,
                            "Act proposal finding identity does not match");
  return ARCHBIRD_OK;
}

static int coverage_valid(const AbValue *coverage) {
  static const char *const allowed[] = {
      "classification",
      "domain",
      "providers",
      "unknowns",
  };
  static const char *const classifications[] = {
      "bounded",
      "complete",
      "partial",
  };
  return ab_act_object_fields_allowed(coverage, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) &&
         value_is_one_of(
             ab_value_member(coverage, "classification"), classifications,
             sizeof(classifications) / sizeof(classifications[0])) &&
         nonblank(ab_value_member(coverage, "domain")) &&
         strings_unique(ab_value_member(coverage, "providers")) &&
         strings_unique(ab_value_member(coverage, "unknowns"));
}

static int proposal_constraint_valid(const AbValue *constraint) {
  static const char *const allowed[] = {
      "actual",
      "assert",
      "exact",
      "expected",
      "id",
      "mapping",
      "max",
      "min",
      "owner",
      "rationale",
      "reference_route",
      "required_routes",
      "requirements",
      "severity",
      "tags",
  };
  static const char *const assertions[] = {
      "acyclic",          "allowed_edges",
      "cardinality",      "forbidden_edges",
      "mapped_set_equal", "mapped_values_equal",
      "min_test_routes",  "observations_equal",
      "required_edges",   "set_equal",
      "subset",           "values_equal",
  };
  static const char *const severities[] = {"error", "note", "warning"};
  return ab_act_object_fields_allowed(constraint, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) &&
         ab_act_identifier(ab_value_member(constraint, "id")) &&
         value_is_one_of(ab_value_member(constraint, "assert"), assertions,
                         sizeof(assertions) / sizeof(assertions[0])) &&
         value_is_one_of(ab_value_member(constraint, "severity"), severities,
                         sizeof(severities) / sizeof(severities[0])) &&
         nonblank(ab_value_member(constraint, "owner")) &&
         nonblank(ab_value_member(constraint, "rationale")) &&
         strings_unique(ab_value_member(constraint, "requirements")) &&
         strings_unique(ab_value_member(constraint, "tags")) &&
         string_value(ab_value_member(constraint, "expected")) &&
         string_value(ab_value_member(constraint, "actual")) &&
         string_value(ab_value_member(constraint, "mapping")) &&
         null_or_u64(ab_value_member(constraint, "min")) &&
         null_or_u64(ab_value_member(constraint, "max")) &&
         null_or_u64(ab_value_member(constraint, "exact")) &&
         strings_unique(ab_value_member(constraint, "required_routes")) &&
         string_value(ab_value_member(constraint, "reference_route"));
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
    size_t previous;
    if (!ab_act_object_fields_allowed(row, entry_allowed,
                                      sizeof(entry_allowed) /
                                          sizeof(entry_allowed[0])) ||
        !ab_act_identifier(ab_value_member(row, "kind")) ||
        !nonblank(ab_value_member(row, "name")) ||
        !ab_act_lowercase_sha256(ab_value_member(row, "sha256")))
      return artifact_invalid(engine, "invalid Act evidence slice entry");
    for (previous = 0; previous < index; previous++) {
      const AbValue *prior = &entries->as.array.items[previous];
      if (ab_act_string_values_equal(ab_value_member(row, "kind"),
                                     ab_value_member(prior, "kind")) &&
          ab_act_string_values_equal(ab_value_member(row, "name"),
                                     ab_value_member(prior, "name")) &&
          ab_act_string_values_equal(ab_value_member(row, "sha256"),
                                     ab_value_member(prior, "sha256")))
        return artifact_invalid(engine, "duplicate Act evidence slice entry");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_proposal_rows(ArchbirdEngine *engine,
                       const AbActProposalView *proposal) {
  static const char *const postcondition_allowed[] = {
      "constraint", "coverage", "derivation_strength", "evidence", "id",
  };
  static const char *const derivation_strengths[] = {
      "constrained_choice",
      "exact_fact",
      "oracle_only",
  };
  static const char *const preserved_allowed[] = {"id", "sha256", "status"};
  static const char *const preserved_statuses[] = {
      "fail", "not_applicable", "pass", "unknown", "waived",
  };
  static const char *const candidate_allowed[] = {
      "coverage", "evidence", "id", "kind", "path", "project", "reason",
  };
  static const char *const unknown_allowed[] = {
      "code", "evidence", "id", "message", "scope",
  };
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
    if (!ab_act_object_fields_allowed(row, postcondition_allowed,
                                      sizeof(postcondition_allowed) /
                                          sizeof(postcondition_allowed[0])) ||
        !value_is_one_of(
            ab_value_member(row, "derivation_strength"), derivation_strengths,
            sizeof(derivation_strengths) / sizeof(derivation_strengths[0])) ||
        !proposal_constraint_valid(ab_value_member(row, "constraint")) ||
        !coverage_valid(ab_value_member(row, "coverage")))
      return artifact_invalid(engine, "invalid Act proposal postcondition");
    {
      ArchbirdStatus status =
          validate_evidence_rows(engine, ab_value_member(row, "evidence"));
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index < proposal->preserved->as.array.count; index++) {
    const AbValue *row = &proposal->preserved->as.array.items[index];
    if (!ab_act_object_fields_allowed(row, preserved_allowed,
                                      sizeof(preserved_allowed) /
                                          sizeof(preserved_allowed[0])) ||
        !value_is_one_of(ab_value_member(row, "status"), preserved_statuses,
                         sizeof(preserved_statuses) /
                             sizeof(preserved_statuses[0])) ||
        !ab_act_lowercase_sha256(ab_value_member(row, "sha256")))
      return artifact_invalid(engine,
                              "invalid Act proposal preserved constraint");
  }
  for (index = 0; index < proposal->candidates->as.array.count; index++) {
    const AbValue *row = &proposal->candidates->as.array.items[index];
    if (!ab_act_object_fields_allowed(row, candidate_allowed,
                                      sizeof(candidate_allowed) /
                                          sizeof(candidate_allowed[0])) ||
        !ab_act_identifier(ab_value_member(row, "kind")) ||
        !ab_act_identifier(ab_value_member(row, "project")) ||
        !nonblank(ab_value_member(row, "path")) ||
        !nonblank(ab_value_member(row, "reason")) ||
        !coverage_valid(ab_value_member(row, "coverage")))
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
    if (!ab_act_object_fields_allowed(row, unknown_allowed,
                                      sizeof(unknown_allowed) /
                                          sizeof(unknown_allowed[0])) ||
        !ab_act_identifier(ab_value_member(row, "code")) ||
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
    status = validate_proposal_source(engine, out->source);
  if (status == ARCHBIRD_OK)
    status = validate_proposal_origin(engine, out->origin);
  if (status == ARCHBIRD_OK)
    status = validate_proposal_facts(engine, out->facts);
  if (status == ARCHBIRD_OK)
    status = validate_proposal_projections(engine, out->projections);
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

static int contract_origin_valid(const AbValue *origin) {
  static const char *const allowed[] = {"constraint", "fingerprint"};
  return ab_act_object_fields_allowed(origin, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) &&
         ab_act_identifier(ab_value_member(origin, "constraint")) &&
         ab_act_lowercase_sha256(ab_value_member(origin, "fingerprint"));
}

static ArchbirdStatus validate_contract_preserved(ArchbirdEngine *engine,
                                                  const AbValue *rows) {
  static const char *const allowed[] = {"id", "sha256", "status"};
  static const char *const statuses[] = {
      "fail", "not_applicable", "pass", "unknown", "waived",
  };
  size_t index;
  if (!object_ids_unique(rows, "id"))
    return artifact_invalid(
        engine, "invalid Act contract preserved constraint identities");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    if (!ab_act_object_fields_allowed(row, allowed,
                                      sizeof(allowed) / sizeof(allowed[0])) ||
        !value_is_one_of(ab_value_member(row, "status"), statuses,
                         sizeof(statuses) / sizeof(statuses[0])) ||
        !ab_act_lowercase_sha256(ab_value_member(row, "sha256")))
      return artifact_invalid(engine,
                              "invalid Act contract preserved constraint");
  }
  return ARCHBIRD_OK;
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
      !contract_origin_valid(out->origin) ||
      !ab_act_lowercase_sha256(
          ab_value_member(&out->root, "proposal_sha256")) ||
      !nonblank(ab_value_member(&out->root, "objective")) ||
      !nonblank(ab_value_member(&out->root, "owner")) ||
      !nonblank(ab_value_member(&out->root, "rationale")) ||
      !strings_unique(out->postconditions) ||
      !strings_unique(out->selected_candidates) ||
      !strings_unique(out->acknowledged_unknowns)) {
    status = artifact_invalid(
        engine, "invalid or unsealed canonical architecture change contract");
    goto contract_fail;
  }
  status = validate_contract_preserved(engine, out->preserved);
  if (status != ARCHBIRD_OK)
    goto contract_fail;
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
