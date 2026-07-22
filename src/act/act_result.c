#include "act_internal.h"

#include <stdlib.h>
#include <string.h>

#define RESULT_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus formatted(ArchbirdEngine *engine, AbString *out,
                                const char *prefix, const AbString *value) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static int value_string_equal(const AbValue *left, const AbValue *right) {
  return left && right && left->kind == AB_VALUE_STRING &&
         right->kind == AB_VALUE_STRING &&
         ab_string_equal(&left->as.text, &right->as.text);
}

static const AbValue *row_by_id(const AbValue *rows, const AbString *id) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *value = ab_value_member(row, "id");
    if (value && value->kind == AB_VALUE_STRING &&
        ab_string_equal(&value->as.text, id))
      return row;
  }
  return NULL;
}

static int ids_match_strings(const AbValue *objects, const AbValue *strings) {
  size_t index;
  if (!objects || !strings || objects->kind != AB_VALUE_ARRAY ||
      strings->kind != AB_VALUE_ARRAY ||
      objects->as.array.count != strings->as.array.count)
    return 0;
  for (index = 0; index < objects->as.array.count; index++) {
    const AbValue *id = ab_value_member(&objects->as.array.items[index], "id");
    if (!value_string_equal(id, &strings->as.array.items[index]))
      return 0;
  }
  return 1;
}

static ArchbirdStatus validate_contract(ArchbirdEngine *engine,
                                        const AbActProposalView *proposal,
                                        const AbActContractView *contract) {
  const AbValue *proposal_sha =
      ab_value_member(&contract->root, "proposal_sha256");
  const AbValue *proposal_origin_constraint =
      ab_value_member(proposal->origin, "constraint");
  const AbValue *proposal_finding =
      ab_value_member(proposal->origin, "finding");
  const AbValue *proposal_fingerprint =
      proposal_finding ? ab_value_member(proposal_finding, "fingerprint")
                       : NULL;
  const AbValue *contract_constraint =
      ab_value_member(contract->origin, "constraint");
  const AbValue *contract_fingerprint =
      ab_value_member(contract->origin, "fingerprint");
  size_t index;
  if (!proposal_sha || proposal_sha->kind != AB_VALUE_STRING ||
      memcmp(proposal_sha->as.text.data, proposal->sha256, 64) != 0)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "change contract proposal SHA-256 does not match proposal");
  if (!value_string_equal(proposal_origin_constraint, contract_constraint) ||
      !value_string_equal(proposal_fingerprint, contract_fingerprint))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "change contract origin does not match proposal");
  if (!ids_match_strings(proposal->postconditions, contract->postconditions))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "change contract must approve every proposal postcondition in order");
  if (!ids_match_strings(proposal->unknowns, contract->acknowledged_unknowns))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "change contract must acknowledge every proposal unknown in order");
  for (index = 0; index < contract->selected_candidates->as.array.count;
       index++) {
    const AbString *id =
        &contract->selected_candidates->as.array.items[index].as.text;
    if (!row_by_id(proposal->candidates, id))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "change contract contains unknown candidate");
  }
  for (index = 0; index < contract->preserved->as.array.count; index++) {
    const AbValue *row = &contract->preserved->as.array.items[index];
    const AbValue *id = ab_value_member(row, "id");
    const AbValue *expected =
        id ? row_by_id(proposal->preserved, &id->as.text) : NULL;
    if (!expected || !ab_value_equal(row, expected))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "change contract preserved constraint does not "
                                "match proposal evidence");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus string_list_add(ArchbirdEngine *engine,
                                      AbActStringList *list, const char *value,
                                      size_t length) {
  AbString *resized;
  ArchbirdStatus status;
  if (list->engine && list->engine != engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  list->engine = engine;
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*list->items))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too many Act diagnostics");
    resized = (AbString *)ab_realloc(engine, list->items,
                                     capacity * sizeof(*list->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing Act diagnostics");
    list->items = resized;
    list->capacity = capacity;
  }
  memset(&list->items[list->count], 0, sizeof(*list->items));
  status = ab_string_copy(engine, &list->items[list->count], value, length);
  if (status == ARCHBIRD_OK)
    list->count++;
  return status;
}

static ArchbirdStatus diagnostic_literal(ArchbirdEngine *engine,
                                         AbActStringList *list,
                                         const char *message) {
  return string_list_add(engine, list, message, strlen(message));
}

static ArchbirdStatus
diagnostic_named(ArchbirdEngine *engine, AbActStringList *list,
                 const char *prefix, const AbString *first,
                 const char *separator, const AbString *second) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, first->data, first->length);
  if (status == ARCHBIRD_OK && separator)
    status = ab_buffer_literal(&buffer, separator);
  if (status == ARCHBIRD_OK && second)
    status = ab_buffer_append(&buffer, second->data, second->length);
  if (status == ARCHBIRD_OK)
    status =
        string_list_add(engine, list, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static void string_list_finish(AbActStringList *list) {
  size_t read;
  size_t write = 0;
  if (list->count > 1)
    qsort(list->items, list->count, sizeof(*list->items), string_compare);
  for (read = 0; read < list->count; read++) {
    if (write && ab_string_equal(&list->items[write - 1], &list->items[read])) {
      ab_string_free(list->engine, &list->items[read]);
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

static void string_list_free(AbActStringList *list) {
  size_t index;
  for (index = 0; list->items && index < list->count; index++)
    ab_string_free(list->engine, &list->items[index]);
  ab_free(list->engine, list->items);
  memset(list, 0, sizeof(*list));
}

static const AbValue *finding_by_fingerprint(const AbValue *check,
                                             const AbString *fingerprint) {
  const AbValue *findings = check ? ab_value_member(check, "findings") : NULL;
  size_t index;
  if (!findings || findings->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < findings->as.array.count; index++) {
    const AbValue *finding = &findings->as.array.items[index];
    const AbValue *value = ab_value_member(finding, "fingerprint");
    if (value && value->kind == AB_VALUE_STRING &&
        ab_string_equal(&value->as.text, fingerprint))
      return finding;
  }
  return NULL;
}

static int slice_value(ArchbirdEngine *engine,
                       const AbActVerification *artifact, const AbString *kind,
                       const AbString *name, char output[65]) {
  const AbValue *row;
  if (kind->length == 17 && memcmp(kind->data, "verification_tool", 17) == 0) {
    const AbValue *value =
        ab_value_member(artifact->tool, "implementation_sha256");
    memcpy(output, value->as.text.data, 64);
    output[64] = '\0';
    return 1;
  }
  if (kind->length == 6 && memcmp(kind->data, "policy", 6) == 0) {
    const AbValue *project = ab_value_member(artifact->policy, "project");
    const AbValue *value =
        ab_value_member(artifact->policy, "constraint_policy_sha256");
    if (!ab_string_equal(&project->as.text, name))
      return 0;
    memcpy(output, value->as.text.data, 64);
    output[64] = '\0';
    return 1;
  }
  if (kind->length == 10 && memcmp(kind->data, "constraint", 10) == 0) {
    row = ab_act_verification_constraint(artifact, name);
    return row && ab_act_value_digest(engine, row, output) == ARCHBIRD_OK;
  }
  if (kind->length == 7 && memcmp(kind->data, "finding", 7) == 0) {
    const AbValue *check = NULL;
    row = ab_act_verification_finding(artifact, name, &check);
    (void)check;
    return row && ab_act_value_digest(engine, row, output) == ARCHBIRD_OK;
  }
  if (kind->length == 4 && memcmp(kind->data, "fact", 4) == 0) {
    const AbVerifyFactSet *fact = NULL;
    row = ab_act_verification_fact_value(artifact, name, &fact);
    if (!row || !fact)
      return 0;
    memcpy(output, fact->sha256, 65);
    return 1;
  }
  if (kind->length == 7 && memcmp(kind->data, "mapping", 7) == 0) {
    row = ab_act_verification_mapping(artifact, name);
    return row && ab_act_value_digest(engine, row, output) == ARCHBIRD_OK;
  }
  if (kind->length == 11 && memcmp(kind->data, "observation", 11) == 0) {
    row = ab_act_verification_observation(artifact, name);
    return row && ab_act_value_digest(engine, row, output) == ARCHBIRD_OK;
  }
  return 0;
}

static int mutable_source(const AbActProposalView *proposal,
                          const AbString *name) {
  size_t index;
  for (index = 0; index < proposal->mutable_sources->as.array.count; index++) {
    if (ab_string_equal(
            &proposal->mutable_sources->as.array.items[index].as.text, name))
      return 1;
  }
  return 0;
}

static int finding_same_key_open(const AbValue *check, const AbString *key) {
  const AbValue *findings = ab_value_member(check, "findings");
  size_t index;
  for (index = 0; index < findings->as.array.count; index++) {
    const AbValue *row = &findings->as.array.items[index];
    const AbValue *row_key = ab_value_member(row, "key");
    if (row_key && ab_string_equal(&row_key->as.text, key) &&
        ab_value_string_is(ab_value_member(row, "applicability"),
                           "applicable") &&
        !ab_value_string_is(ab_value_member(row, "comparison"), "equal"))
      return 1;
  }
  return 0;
}

static ArchbirdStatus proposal_freshness(ArchbirdEngine *engine,
                                         const AbActProposalView *proposal,
                                         const AbActContractView *contract,
                                         const AbActVerification *before,
                                         const char **out_freshness,
                                         AbActStringList *diagnostics) {
  const AbValue *proposal_impl =
      ab_value_member(proposal->tool, "implementation_sha256");
  const AbValue *contract_impl =
      ab_value_member(contract->tool, "implementation_sha256");
  const AbValue *source_policy = ab_value_member(proposal->source, "policy");
  const AbValue *source_policy_sha = ab_value_member(source_policy, "sha256");
  const AbValue *source_verify_impl = ab_value_member(
      proposal->source, "verification_tool_implementation_sha256");
  const AbValue *before_policy_sha =
      ab_value_member(before->policy, "constraint_policy_sha256");
  const AbValue *before_impl =
      ab_value_member(before->tool, "implementation_sha256");
  const AbValue *origin_constraint_id =
      ab_value_member(proposal->origin, "constraint");
  const AbValue *origin_finding = ab_value_member(proposal->origin, "finding");
  const AbValue *origin_fingerprint =
      ab_value_member(origin_finding, "fingerprint");
  const AbValue *origin_key = ab_value_member(origin_finding, "key");
  const AbValue *current_check =
      ab_act_verification_constraint(before, &origin_constraint_id->as.text);
  const AbValue *exact =
      current_check
          ? finding_by_fingerprint(current_check, &origin_fingerprint->as.text)
          : NULL;
  const AbValue *entries = ab_value_member(proposal->evidence_slice, "entries");
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (memcmp(proposal_impl->as.text.data, ARCHBIRD_IMPLEMENTATION_SHA256, 64) !=
      0)
    status =
        diagnostic_literal(engine, diagnostics,
                           "planner implementation differs from the proposal");
  if (status == ARCHBIRD_OK && memcmp(contract_impl->as.text.data,
                                      ARCHBIRD_IMPLEMENTATION_SHA256, 64) != 0)
    status = diagnostic_literal(
        engine, diagnostics,
        "contract implementation differs from the current tool");
  if (status == ARCHBIRD_OK &&
      !value_string_equal(source_policy_sha, before_policy_sha))
    status = diagnostic_literal(engine, diagnostics,
                                "constraint policy digest changed");
  if (status == ARCHBIRD_OK &&
      !value_string_equal(source_verify_impl, before_impl))
    status = diagnostic_literal(engine, diagnostics,
                                "verification implementation changed");
  if (status != ARCHBIRD_OK)
    return status;
  if (!current_check) {
    status = diagnostic_literal(
        engine, diagnostics,
        "origin constraint is absent from the current before-state");
    *out_freshness = "stale";
    string_list_finish(diagnostics);
    return status;
  }
  if (!exact) {
    if (finding_same_key_open(current_check, &origin_key->as.text)) {
      status = diagnostic_literal(
          engine, diagnostics, "origin finding changed comparison or payload");
      *out_freshness = "stale";
    } else {
      status = diagnostic_literal(
          engine, diagnostics,
          "origin finding was already resolved before execution");
      *out_freshness = "superseded";
    }
    string_list_finish(diagnostics);
    return status;
  }
  for (index = 0; status == ARCHBIRD_OK && index < entries->as.array.count;
       index++) {
    const AbValue *entry = &entries->as.array.items[index];
    const AbValue *kind = ab_value_member(entry, "kind");
    const AbValue *name = ab_value_member(entry, "name");
    const AbValue *sha = ab_value_member(entry, "sha256");
    char current[65];
    if (!slice_value(engine, before, &kind->as.text, &name->as.text, current) ||
        memcmp(current, sha->as.text.data, 64) != 0)
      status = diagnostic_named(engine, diagnostics,
                                "derivation evidence changed: ", &kind->as.text,
                                ":", &name->as.text);
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < contract->preserved->as.array.count;
       index++) {
    const AbValue *preserved = &contract->preserved->as.array.items[index];
    const AbValue *id = ab_value_member(preserved, "id");
    const AbValue *sha = ab_value_member(preserved, "sha256");
    const AbValue *check = ab_act_verification_constraint(before, &id->as.text);
    char current[65];
    if (!check || ab_act_value_digest(engine, check, current) != ARCHBIRD_OK ||
        memcmp(current, sha->as.text.data, 64) != 0)
      status = diagnostic_named(
          engine, diagnostics,
          "preserved constraint evidence changed: ", &id->as.text, NULL, NULL);
  }
  string_list_finish(diagnostics);
  if (status != ARCHBIRD_OK)
    return status;
  if (diagnostics->count)
    *out_freshness = "stale";
  else {
    const AbValue *source_sha =
        ab_value_member(proposal->source, "verification_sha256");
    if (memcmp(source_sha->as.text.data, before->sha256, 64) != 0) {
      *out_freshness = "context_drift";
      status = diagnostic_literal(
          engine, diagnostics,
          "whole verification artifact changed outside the derivation slice");
    } else {
      *out_freshness = "current";
    }
  }
  return status;
}

static ArchbirdStatus after_compatible(ArchbirdEngine *engine,
                                       const AbActProposalView *proposal,
                                       const AbActVerification *after,
                                       int *out_compatible,
                                       AbActStringList *diagnostics) {
  const AbValue *source_policy = ab_value_member(proposal->source, "policy");
  const AbValue *source_policy_sha = ab_value_member(source_policy, "sha256");
  const AbValue *source_impl = ab_value_member(
      proposal->source, "verification_tool_implementation_sha256");
  const AbValue *after_policy_sha =
      ab_value_member(after->policy, "constraint_policy_sha256");
  const AbValue *after_impl =
      ab_value_member(after->tool, "implementation_sha256");
  const AbValue *entries = ab_value_member(proposal->evidence_slice, "entries");
  size_t diagnostic_start = diagnostics->count;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!value_string_equal(source_policy_sha, after_policy_sha))
    status = diagnostic_literal(engine, diagnostics,
                                "after-state constraint policy digest changed");
  if (status == ARCHBIRD_OK && !value_string_equal(source_impl, after_impl))
    status = diagnostic_literal(
        engine, diagnostics, "after-state verification implementation changed");
  for (index = 0; status == ARCHBIRD_OK && index < entries->as.array.count;
       index++) {
    const AbValue *entry = &entries->as.array.items[index];
    const AbValue *kind = ab_value_member(entry, "kind");
    const AbValue *name = ab_value_member(entry, "name");
    const AbValue *sha = ab_value_member(entry, "sha256");
    char current[65];
    if (ab_value_string_is(kind, "constraint") ||
        ab_value_string_is(kind, "finding"))
      continue;
    if ((ab_value_string_is(kind, "fact") ||
         ab_value_string_is(kind, "observation")) &&
        mutable_source(proposal, &name->as.text))
      continue;
    if (!slice_value(engine, after, &kind->as.text, &name->as.text, current) ||
        memcmp(current, sha->as.text.data, 64) != 0)
      status = diagnostic_named(engine, diagnostics,
                                "immutable target evidence changed: ",
                                &kind->as.text, ":", &name->as.text);
  }
  string_list_finish(diagnostics);
  *out_compatible = diagnostics->count == diagnostic_start;
  return status;
}

static ArchbirdStatus outcome_add(ArchbirdEngine *engine,
                                  AbActResultData *result,
                                  const char *id_prefix,
                                  const AbString *id_value, const char *kind,
                                  const char *status_value, const char *message,
                                  AbActEvidenceList *evidence) {
  AbActOutcome *resized;
  AbActOutcome *row;
  ArchbirdStatus status;
  if (result->outcome_count == SIZE_MAX / sizeof(*resized)) {
    ab_act_evidence_list_free(evidence);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many Act outcomes");
  }
  resized = (AbActOutcome *)ab_realloc(engine, result->outcomes,
                                       (result->outcome_count + 1) *
                                           sizeof(*result->outcomes));
  if (!resized) {
    ab_act_evidence_list_free(evidence);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing Act outcomes");
  }
  result->outcomes = resized;
  row = &result->outcomes[result->outcome_count];
  memset(row, 0, sizeof(*row));
  status = formatted(engine, &row->id, id_prefix, id_value);
  if (status == ARCHBIRD_OK) {
    row->kind = kind;
    row->status = status_value;
    row->message = message;
    row->evidence = *evidence;
    memset(evidence, 0, sizeof(*evidence));
    result->outcome_count++;
  } else {
    ab_string_free(engine, &row->id);
    ab_act_evidence_list_free(evidence);
    memset(row, 0, sizeof(*row));
  }
  return status;
}

static ArchbirdStatus evidence_from_rows(ArchbirdEngine *engine,
                                         AbActEvidenceList *evidence,
                                         const AbValue *rows) {
  ArchbirdStatus status =
      ab_act_evidence_list_add_array(engine, evidence, rows);
  if (status == ARCHBIRD_OK)
    ab_act_evidence_list_finish(evidence);
  return status;
}

static ArchbirdStatus finding_evidence_for_key(ArchbirdEngine *engine,
                                               const AbValue *check,
                                               const AbString *key,
                                               AbActEvidenceList *evidence,
                                               size_t *out_matches) {
  const AbValue *findings = ab_value_member(check, "findings");
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_matches = 0;
  for (index = 0; status == ARCHBIRD_OK && index < findings->as.array.count;
       index++) {
    const AbValue *finding = &findings->as.array.items[index];
    const AbValue *row_key = ab_value_member(finding, "key");
    if (!row_key || !ab_string_equal(&row_key->as.text, key))
      continue;
    (*out_matches)++;
    status = ab_act_evidence_list_add_array(
        engine, evidence, ab_value_member(finding, "evidence"));
  }
  if (status == ARCHBIRD_OK && !evidence->count)
    status = ab_act_evidence_list_add_array(
        engine, evidence, ab_value_member(check, "witnesses"));
  if (status == ARCHBIRD_OK)
    ab_act_evidence_list_finish(evidence);
  return status;
}

static ArchbirdStatus origin_outcome(ArchbirdEngine *engine,
                                     const AbActProposalView *proposal,
                                     const AbActVerification *after,
                                     AbActResultData *result) {
  const AbValue *origin_constraint_id =
      ab_value_member(proposal->origin, "constraint");
  const AbValue *origin_finding = ab_value_member(proposal->origin, "finding");
  const AbValue *fingerprint = ab_value_member(origin_finding, "fingerprint");
  const AbValue *key = ab_value_member(origin_finding, "key");
  const AbValue *check =
      ab_act_verification_constraint(after, &origin_constraint_id->as.text);
  AbActEvidenceList evidence = {0};
  size_t matches = 0;
  size_t index;
  ArchbirdStatus status;
  if (!check)
    return outcome_add(
        engine, result, "origin:", &fingerprint->as.text, "origin", "stale",
        "origin constraint is absent from the after-state", &evidence);
  status = finding_evidence_for_key(engine, check, &key->as.text, &evidence,
                                    &matches);
  if (status != ARCHBIRD_OK) {
    ab_act_evidence_list_free(&evidence);
    return status;
  }
  for (index = 0; index < ab_value_member(check, "findings")->as.array.count;
       index++) {
    const AbValue *finding =
        &ab_value_member(check, "findings")->as.array.items[index];
    const AbValue *row_key = ab_value_member(finding, "key");
    if (!row_key || !ab_string_equal(&row_key->as.text, &key->as.text))
      continue;
    if (!ab_value_string_is(ab_value_member(finding, "evidence_state"),
                            "current"))
      return outcome_add(
          engine, result, "origin:", &fingerprint->as.text, "origin", "unknown",
          "origin evidence is stale or unknown in the after-state", &evidence);
  }
  for (index = 0; index < ab_value_member(check, "findings")->as.array.count;
       index++) {
    const AbValue *finding =
        &ab_value_member(check, "findings")->as.array.items[index];
    const AbValue *row_key = ab_value_member(finding, "key");
    if (row_key && ab_string_equal(&row_key->as.text, &key->as.text) &&
        !ab_value_string_is(ab_value_member(finding, "applicability"),
                            "applicable"))
      return outcome_add(
          engine, result, "origin:", &fingerprint->as.text, "origin",
          "unexpected",
          "origin applicability changed instead of resolving the finding",
          &evidence);
  }
  for (index = 0; index < ab_value_member(check, "findings")->as.array.count;
       index++) {
    const AbValue *finding =
        &ab_value_member(check, "findings")->as.array.items[index];
    const AbValue *row_key = ab_value_member(finding, "key");
    if (row_key && ab_string_equal(&row_key->as.text, &key->as.text) &&
        !ab_value_string_is(ab_value_member(finding, "comparison"), "equal") &&
        (ab_value_string_is(ab_value_member(finding, "disposition"), "open") ||
         ab_value_string_is(ab_value_member(finding, "disposition"), "waived")))
      return outcome_add(
          engine, result, "origin:", &fingerprint->as.text, "origin", "missing",
          "origin finding remains open in the after-state", &evidence);
  }
  (void)matches;
  if (ab_value_string_is(ab_value_member(check, "status"), "unknown"))
    return outcome_add(
        engine, result, "origin:", &fingerprint->as.text, "origin", "unknown",
        "origin constraint is unknown in the after-state", &evidence);
  if (ab_value_string_is(ab_value_member(check, "status"), "not_applicable"))
    return outcome_add(engine, result, "origin:", &fingerprint->as.text,
                       "origin", "unexpected",
                       "origin constraint became not applicable", &evidence);
  return outcome_add(
      engine, result, "origin:", &fingerprint->as.text, "origin", "satisfied",
      "origin finding is resolved with current applicable evidence", &evidence);
}

static ArchbirdStatus
result_evidence_from_check(ArchbirdEngine *engine,
                           const AbVerifyCheckResult *check,
                           AbActEvidenceList *evidence) {
  size_t finding_index;
  size_t evidence_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (finding_index = 0;
       status == ARCHBIRD_OK && finding_index < check->finding_count;
       finding_index++) {
    for (evidence_index = 0;
         status == ARCHBIRD_OK &&
         evidence_index < check->findings[finding_index].evidence_count;
         evidence_index++)
      status = ab_act_evidence_list_add(
          engine, evidence,
          &check->findings[finding_index].evidence[evidence_index]);
  }
  if (status == ARCHBIRD_OK && !evidence->count) {
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < check->witness_count;
         evidence_index++)
      status = ab_act_evidence_list_add(engine, evidence,
                                        &check->witnesses[evidence_index]);
  }
  if (status == ARCHBIRD_OK)
    ab_act_evidence_list_finish(evidence);
  return status;
}

static int fact_shallow_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyFactSet *left = (const AbVerifyFactSet *)left_raw;
  const AbVerifyFactSet *right = (const AbVerifyFactSet *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus postcondition_outcomes(ArchbirdEngine *engine,
                                             const AbActProposalView *proposal,
                                             const AbActVerification *after,
                                             AbActResultData *result) {
  size_t literal_count = proposal->facts->as.array.count;
  size_t projection_count = proposal->projections->as.array.count;
  size_t combined_count = after->fact_count + literal_count + projection_count;
  AbVerifyFactSet *literal = NULL;
  AbVerifyFactSet *projected = NULL;
  AbVerifyFactSet *combined = NULL;
  AbVerificationContext context = {0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (literal_count)
    literal =
        (AbVerifyFactSet *)ab_calloc(engine, literal_count, sizeof(*literal));
  if (projection_count)
    projected = (AbVerifyFactSet *)ab_calloc(engine, projection_count,
                                             sizeof(*projected));
  if (combined_count)
    combined =
        (AbVerifyFactSet *)ab_calloc(engine, combined_count, sizeof(*combined));
  if ((literal_count && !literal) || (projection_count && !projected) ||
      (combined_count && !combined)) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory evaluating Act postconditions");
    goto cleanup;
  }
  for (index = 0; status == ARCHBIRD_OK && index < literal_count; index++)
    status = ab_verify_fact_decode_artifact(
        engine, &proposal->facts->as.array.items[index], &literal[index]);
  for (index = 0; status == ARCHBIRD_OK && index < projection_count; index++) {
    const AbValue *spec = &proposal->projections->as.array.items[index];
    const AbValue *name = ab_value_member(spec, "name");
    const AbValue *source_name = ab_value_member(spec, "source");
    const AbValue *selection = ab_value_member(spec, "selection");
    const AbValue *keys = ab_value_member(spec, "keys");
    const AbValue *aliases = ab_value_member(spec, "aliases");
    const AbVerifyFactSet *source = NULL;
    ab_act_verification_fact_value(after, &source_name->as.text, &source);
    if (!source) {
      AbString empty = {0};
      status =
          ab_verify_fact_unknown(engine, &projected[index], &name->as.text,
                                 &empty, "unknown", "source fact is absent");
    } else {
      status =
          ab_act_project_fact(engine, source, &name->as.text, aliases,
                              selection->as.text.data, keys, &projected[index]);
    }
  }
  for (index = 0; index < after->fact_count; index++)
    combined[index] = after->decoded_facts[index];
  for (index = 0; index < literal_count; index++)
    combined[after->fact_count + index] = literal[index];
  for (index = 0; index < projection_count; index++)
    combined[after->fact_count + literal_count + index] = projected[index];
  if (combined_count > 1)
    qsort(combined, combined_count, sizeof(*combined), fact_shallow_compare);
  context.engine = engine;
  context.facts = combined;
  context.fact_count = combined_count;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < proposal->postconditions->as.array.count;
       index++) {
    const AbValue *postcondition =
        &proposal->postconditions->as.array.items[index];
    const AbValue *id = ab_value_member(postcondition, "id");
    const AbValue *check = ab_value_member(postcondition, "constraint");
    const AbValue *origin_finding =
        ab_value_member(proposal->origin, "finding");
    const AbValue *origin_key = ab_value_member(origin_finding, "key");
    AbVerifyCheckResult check_result = {0};
    AbActEvidenceList evidence = {0};
    const char *outcome_status;
    const char *message;
    size_t finding_index;
    int unexpected = 0;
    status = ab_verify_evaluate_check(&context, check, &check_result);
    if (status != ARCHBIRD_OK) {
      ab_verify_check_result_free(engine, &check_result);
      status = evidence_from_rows(engine, &evidence,
                                  ab_value_member(postcondition, "evidence"));
      if (status == ARCHBIRD_OK)
        status = outcome_add(engine, result, "postcondition:", &id->as.text,
                             "postcondition", "unknown",
                             "postcondition could not be evaluated", &evidence);
      continue;
    }
    status = result_evidence_from_check(engine, &check_result, &evidence);
    if (status != ARCHBIRD_OK) {
      ab_verify_check_result_free(engine, &check_result);
      ab_act_evidence_list_free(&evidence);
      break;
    }
    if (check_result.status.length == 4 &&
        memcmp(check_result.status.data, "pass", 4) == 0) {
      outcome_status = "satisfied";
      message = "derived Verify predicate passed";
    } else if (check_result.status.length == 7 &&
               memcmp(check_result.status.data, "unknown", 7) == 0) {
      outcome_status = "unknown";
      message = "derived Verify predicate has unknown or stale evidence";
    } else {
      for (finding_index = 0; finding_index < check_result.finding_count;
           finding_index++) {
        const AbVerifyFinding *finding = &check_result.findings[finding_index];
        if (finding->comparison.length == 5 &&
            memcmp(finding->comparison.data, "extra", 5) == 0 &&
            !ab_string_equal(&finding->key, &origin_key->as.text)) {
          unexpected = 1;
          break;
        }
      }
      outcome_status = unexpected ? "unexpected" : "missing";
      message =
          unexpected
              ? "after-state contains an unexpected fact outside the transition"
              : "required fact transition is incomplete";
    }
    status = outcome_add(engine, result, "postcondition:", &id->as.text,
                         "postcondition", outcome_status, message, &evidence);
    ab_verify_check_result_free(engine, &check_result);
    ab_act_evidence_list_free(&evidence);
  }

cleanup:
  for (index = 0; index < literal_count; index++)
    ab_verify_fact_free(engine, &literal[index]);
  for (index = 0; index < projection_count; index++)
    ab_verify_fact_free(engine, &projected[index]);
  ab_free(engine, literal);
  ab_free(engine, projected);
  ab_free(engine, combined);
  return status;
}

static ArchbirdStatus preserved_outcomes(ArchbirdEngine *engine,
                                         const AbActContractView *contract,
                                         const AbActVerification *after,
                                         AbActResultData *result) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0;
       status == ARCHBIRD_OK && index < contract->preserved->as.array.count;
       index++) {
    const AbValue *preserved = &contract->preserved->as.array.items[index];
    const AbValue *id = ab_value_member(preserved, "id");
    const AbValue *check = ab_act_verification_constraint(after, &id->as.text);
    AbActEvidenceList evidence = {0};
    const char *outcome_status;
    const char *message;
    if (!check) {
      outcome_status = "stale";
      message = "preserved constraint is absent from the after-state";
    } else if (ab_value_string_is(ab_value_member(check, "status"),
                                  "unknown")) {
      outcome_status = "unknown";
      message = "preserved constraint became unknown";
      status = evidence_from_rows(engine, &evidence,
                                  ab_value_member(check, "witnesses"));
    } else if (ab_value_string_is(ab_value_member(check, "status"), "fail")) {
      const AbValue *findings = ab_value_member(check, "findings");
      size_t finding_index;
      outcome_status = "unexpected";
      message = "preserved constraint became blocking";
      for (finding_index = 0;
           status == ARCHBIRD_OK && finding_index < findings->as.array.count;
           finding_index++)
        status = ab_act_evidence_list_add_array(
            engine, &evidence,
            ab_value_member(&findings->as.array.items[finding_index],
                            "evidence"));
      ab_act_evidence_list_finish(&evidence);
    } else {
      const AbValue *check_status = ab_value_member(check, "status");
      outcome_status = "satisfied";
      if (ab_value_string_is(check_status, "pass"))
        message = "preserved constraint remains nonblocking (pass)";
      else if (ab_value_string_is(check_status, "waived"))
        message = "preserved constraint remains nonblocking (waived)";
      else
        message = "preserved constraint remains nonblocking (not_applicable)";
      if (status == ARCHBIRD_OK)
        status = evidence_from_rows(engine, &evidence,
                                    ab_value_member(check, "witnesses"));
      if (status == ARCHBIRD_OK)
        status = outcome_add(engine, result, "preserved:", &id->as.text,
                             "preserved_constraint", outcome_status, message,
                             &evidence);
      if (status == ARCHBIRD_OK)
        continue;
    }
    if (status == ARCHBIRD_OK)
      status = outcome_add(engine, result, "preserved:", &id->as.text,
                           "preserved_constraint", outcome_status, message,
                           &evidence);
    ab_act_evidence_list_free(&evidence);
  }
  return status;
}

static const char *overall_status(const char *freshness,
                                  const AbActResultData *result) {
  static const char *const precedence[] = {"unexpected", "missing", "unknown"};
  size_t status_index;
  size_t outcome_index;
  if (strcmp(freshness, "stale") == 0)
    return "stale";
  for (outcome_index = 0; outcome_index < result->outcome_count;
       outcome_index++) {
    if (strcmp(result->outcomes[outcome_index].status, "stale") == 0)
      return "stale";
  }
  if (strcmp(freshness, "superseded") == 0)
    return "superseded";
  for (status_index = 0;
       status_index < sizeof(precedence) / sizeof(precedence[0]);
       status_index++) {
    for (outcome_index = 0; outcome_index < result->outcome_count;
         outcome_index++) {
      if (strcmp(result->outcomes[outcome_index].status,
                 precedence[status_index]) == 0)
        return precedence[status_index];
    }
  }
  return "satisfied";
}

ArchbirdStatus ab_act_result_verify(ArchbirdEngine *engine,
                                    const AbActProposalView *proposal,
                                    const AbActContractView *contract,
                                    const AbActVerification *before,
                                    const AbActVerification *after,
                                    AbActResultData *out) {
  const char *freshness = "stale";
  int compatible = 0;
  ArchbirdStatus status;
  if (!engine || !proposal || !contract || !before || !after || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  memcpy(out->proposal_sha256, proposal->sha256, 65);
  memcpy(out->contract_sha256, contract->sha256, 65);
  memcpy(out->before_sha256, before->sha256, 65);
  memcpy(out->after_sha256, after->sha256, 65);
  status = validate_contract(engine, proposal, contract);
  if (status == ARCHBIRD_OK)
    status = proposal_freshness(engine, proposal, contract, before, &freshness,
                                &out->diagnostics);
  out->freshness = freshness;
  if (status != ARCHBIRD_OK)
    goto fail;
  if (strcmp(freshness, "superseded") == 0) {
    const AbValue *finding = ab_value_member(proposal->origin, "finding");
    const AbValue *fingerprint = ab_value_member(finding, "fingerprint");
    AbActEvidenceList evidence = {0};
    status = outcome_add(
        engine, out, "origin:", &fingerprint->as.text, "origin", "superseded",
        "origin finding was resolved before contract execution", &evidence);
  } else if (strcmp(freshness, "stale") != 0) {
    status = after_compatible(engine, proposal, after, &compatible,
                              &out->diagnostics);
    if (status == ARCHBIRD_OK && !compatible) {
      freshness = "stale";
      out->freshness = freshness;
    } else if (status == ARCHBIRD_OK) {
      status = origin_outcome(engine, proposal, after, out);
      if (status == ARCHBIRD_OK)
        status = postcondition_outcomes(engine, proposal, after, out);
      if (status == ARCHBIRD_OK)
        status = preserved_outcomes(engine, contract, after, out);
    }
  }
  if (status == ARCHBIRD_OK) {
    string_list_finish(&out->diagnostics);
    out->status = overall_status(out->freshness, out);
  }
  if (status != ARCHBIRD_OK)
    goto fail;
  return ARCHBIRD_OK;

fail:
  ab_act_result_data_free(out);
  return status;
}

void ab_act_result_data_free(AbActResultData *result) {
  ArchbirdEngine *engine;
  size_t index;
  if (!result)
    return;
  engine = result->engine;
  for (index = 0; result->outcomes && index < result->outcome_count; index++) {
    ab_string_free(engine, &result->outcomes[index].id);
    ab_act_evidence_list_free(&result->outcomes[index].evidence);
  }
  ab_free(engine, result->outcomes);
  string_list_free(&result->diagnostics);
  memset(result, 0, sizeof(*result));
}

static ArchbirdStatus render_diagnostics(AbBuffer *buffer,
                                         const AbActStringList *list) {
  size_t index;
  RESULT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < list->count; index++) {
    if (index)
      RESULT_TRY(ab_buffer_literal(buffer, ","));
    RESULT_TRY(ab_buffer_json_string(buffer, list->items[index].data,
                                     list->items[index].length));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_outcomes(AbBuffer *buffer,
                                      const AbActResultData *result) {
  size_t index;
  RESULT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < result->outcome_count; index++) {
    const AbActOutcome *row = &result->outcomes[index];
    if (index)
      RESULT_TRY(ab_buffer_literal(buffer, ","));
    RESULT_TRY(ab_buffer_literal(buffer, "{\"evidence\":"));
    RESULT_TRY(ab_act_evidence_list_render(buffer, &row->evidence));
    RESULT_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    RESULT_TRY(ab_buffer_json_string(buffer, row->id.data, row->id.length));
    RESULT_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    RESULT_TRY(ab_buffer_json_string(buffer, row->kind, strlen(row->kind)));
    RESULT_TRY(ab_buffer_literal(buffer, ",\"message\":"));
    RESULT_TRY(
        ab_buffer_json_string(buffer, row->message, strlen(row->message)));
    RESULT_TRY(ab_buffer_literal(buffer, ",\"status\":"));
    RESULT_TRY(ab_buffer_json_string(buffer, row->status, strlen(row->status)));
    RESULT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_result_document(AbBuffer *buffer,
                                             const AbActResultData *result,
                                             int include_sha256,
                                             const char sha256[65]) {
  RESULT_TRY(ab_buffer_literal(buffer, "{\"after_verification_sha256\":"));
  RESULT_TRY(ab_buffer_json_string(buffer, result->after_sha256, 64));
  RESULT_TRY(ab_buffer_literal(
      buffer,
      ",\"artifact\":\"change-result\",\"before_verification_sha256\":"));
  RESULT_TRY(ab_buffer_json_string(buffer, result->before_sha256, 64));
  RESULT_TRY(ab_buffer_literal(buffer, ",\"contract_sha256\":"));
  RESULT_TRY(ab_buffer_json_string(buffer, result->contract_sha256, 64));
  RESULT_TRY(ab_buffer_literal(buffer, ",\"diagnostics\":"));
  RESULT_TRY(render_diagnostics(buffer, &result->diagnostics));
  RESULT_TRY(ab_buffer_literal(buffer, ",\"freshness\":"));
  RESULT_TRY(ab_buffer_json_string(buffer, result->freshness,
                                   strlen(result->freshness)));
  RESULT_TRY(ab_buffer_literal(buffer, ",\"outcomes\":"));
  RESULT_TRY(render_outcomes(buffer, result));
  RESULT_TRY(ab_buffer_literal(buffer, ",\"proposal_sha256\":"));
  RESULT_TRY(ab_buffer_json_string(buffer, result->proposal_sha256, 64));
  RESULT_TRY(ab_buffer_literal(
      buffer, ",\"provenance\":\"derived\",\"schema_version\":2"));
  if (include_sha256) {
    RESULT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    RESULT_TRY(ab_buffer_json_string(buffer, sha256, 64));
  }
  RESULT_TRY(ab_buffer_literal(buffer, ",\"status\":"));
  RESULT_TRY(
      ab_buffer_json_string(buffer, result->status, strlen(result->status)));
  return ab_buffer_literal(
      buffer,
      ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
}

ArchbirdStatus ab_act_result_render_json(AbBuffer *buffer,
                                         AbActResultData *result) {
  AbBuffer payload;
  uint8_t digest[32];
  char sha256[65];
  ArchbirdStatus status;
  if (!buffer || !buffer->engine || !result)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(buffer->engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&payload, buffer->engine);
  status = render_result_document(&payload, result, 0, NULL);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(payload.data, payload.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, sha256);
  if (status == ARCHBIRD_OK)
    memcpy(result->sha256, sha256, 65);
  if (status == ARCHBIRD_OK)
    status = render_result_document(buffer, result, 1, sha256);
  ab_buffer_free(&payload);
  return status;
}
