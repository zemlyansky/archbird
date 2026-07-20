#include "act_reports.h"

#include <string.h>

#define REPORT_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus append_prefix(AbBuffer *buffer, const AbValue *value,
                                    size_t length) {
  size_t count =
      value->as.text.length < length ? value->as.text.length : length;
  return ab_buffer_append(buffer, value->as.text.data, count);
}

static ArchbirdStatus append_string_prefix(AbBuffer *buffer, const char *value,
                                           size_t available, size_t length) {
  size_t count = available < length ? available : length;
  return ab_buffer_append(buffer, value, count);
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
    REPORT_TRY(ab_buffer_literal(buffer, " `"));
    REPORT_TRY(
        ab_buffer_append(buffer, row->sha256.data,
                         row->sha256.length < 16 ? row->sha256.length : 16));
    REPORT_TRY(ab_buffer_literal(buffer, "`"));
  }
  if (row->detail.length) {
    REPORT_TRY(ab_buffer_literal(buffer, " — "));
    REPORT_TRY(ab_buffer_append(buffer, row->detail.data, row->detail.length));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_evidence_value_label(AbBuffer *buffer,
                                                  const AbValue *value) {
  AbVerifyEvidence evidence = {0};
  ArchbirdStatus status =
      ab_verify_evidence_decode_artifact(buffer->engine, value, &evidence);
  if (status == ARCHBIRD_OK)
    status = render_evidence_label(buffer, &evidence);
  ab_verify_evidence_free(buffer->engine, &evidence);
  return status;
}

ArchbirdStatus
ab_act_proposal_render_markdown(AbBuffer *buffer,
                                const AbActProposalView *proposal, int full,
                                size_t max_candidates) {
  const AbValue *origin_check = ab_value_member(proposal->origin, "check");
  const AbValue *origin_assert = ab_value_member(proposal->origin, "assert");
  const AbValue *finding = ab_value_member(proposal->origin, "finding");
  const AbValue *fingerprint = ab_value_member(finding, "fingerprint");
  const AbValue *source_suite = ab_value_member(proposal->source, "suite");
  const AbValue *suite_sha = ab_value_member(source_suite, "sha256");
  const AbValue *sha = ab_value_member(&proposal->root, "sha256");
  size_t candidate_count = proposal->candidates->as.array.count;
  size_t shown_candidates = full || max_candidates > candidate_count
                                ? candidate_count
                                : max_candidates;
  size_t index;
  REPORT_TRY(ab_buffer_literal(buffer, "# Architecture change proposal: "));
  REPORT_TRY(ab_buffer_append(buffer, origin_check->as.text.data,
                              origin_check->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\nDerived proposal `"));
  REPORT_TRY(append_prefix(buffer, sha, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "` from finding `"));
  REPORT_TRY(append_prefix(buffer, fingerprint, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "` in suite `"));
  REPORT_TRY(append_prefix(buffer, suite_sha, 16));
  REPORT_TRY(ab_buffer_literal(
      buffer,
      "`.\n\nThis artifact is derived evidence, not authorization. A reviewed "
      "change contract must reference its full digest.\n\n## "
      "Origin\n\n```text\n"
      "check="));
  REPORT_TRY(ab_buffer_append(buffer, origin_check->as.text.data,
                              origin_check->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, " assert="));
  REPORT_TRY(ab_buffer_append(buffer, origin_assert->as.text.data,
                              origin_assert->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\ncomparison="));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(finding, "comparison")->as.text.data,
      ab_value_member(finding, "comparison")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, " key="));
  REPORT_TRY(ab_buffer_append(buffer,
                              ab_value_member(finding, "key")->as.text.data,
                              ab_value_member(finding, "key")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\nevidence="));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(finding, "evidence_state")->as.text.data,
      ab_value_member(finding, "evidence_state")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, " applicability="));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(finding, "applicability")->as.text.data,
      ab_value_member(finding, "applicability")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n```\n\n"));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(finding, "message")->as.text.data,
      ab_value_member(finding, "message")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\n## Required postconditions\n\n"));
  if (proposal->postconditions->as.array.count) {
    for (index = 0; index < proposal->postconditions->as.array.count; index++) {
      const AbValue *row = &proposal->postconditions->as.array.items[index];
      const AbValue *id = ab_value_member(row, "id");
      const AbValue *check = ab_value_member(row, "check");
      const AbValue *coverage = ab_value_member(row, "coverage");
      REPORT_TRY(ab_buffer_literal(buffer, "### "));
      REPORT_TRY(
          ab_buffer_append(buffer, id->as.text.data, id->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "\n\n`"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(check, "assert")->as.text.data,
          ab_value_member(check, "assert")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` over `"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(check, "expected")->as.text.data,
          ab_value_member(check, "expected")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` → `"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(check, "actual")->as.text.data,
          ab_value_member(check, "actual")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "`; derivation `"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(row, "derivation_strength")->as.text.data,
          ab_value_member(row, "derivation_strength")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "`; coverage `"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(coverage, "classification")->as.text.data,
          ab_value_member(coverage, "classification")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` ("));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(coverage, "domain")->as.text.data,
          ab_value_member(coverage, "domain")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, ").\n\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(
        buffer,
        "No static fact predicate is derivable. Completion is the current, "
        "applicable resolution of the origin finding.\n\n"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "## Candidate edit evidence\n\n"));
  if (shown_candidates) {
    for (index = 0; index < shown_candidates; index++) {
      const AbValue *row = &proposal->candidates->as.array.items[index];
      REPORT_TRY(ab_buffer_literal(buffer, "- `"));
      REPORT_TRY(ab_buffer_append(buffer,
                                  ab_value_member(row, "id")->as.text.data,
                                  ab_value_member(row, "id")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` "));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "kind")->as.text.data,
                           ab_value_member(row, "kind")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, " `"));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(row, "project")->as.text.data,
          ab_value_member(row, "project")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, ":"));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "path")->as.text.data,
                           ab_value_member(row, "path")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` — "));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "reason")->as.text.data,
                           ab_value_member(row, "reason")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "\n"));
    }
    if (shown_candidates != candidate_count) {
      REPORT_TRY(ab_buffer_literal(buffer, "- … "));
      REPORT_TRY(ab_buffer_u64(buffer, candidate_count - shown_candidates));
      REPORT_TRY(ab_buffer_literal(buffer,
                                   " more candidate rows omitted from compact "
                                   "Markdown; JSON is complete.\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(buffer, "- none\n"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Unknown frontier\n\n"));
  if (proposal->unknowns->as.array.count) {
    for (index = 0; index < proposal->unknowns->as.array.count; index++) {
      const AbValue *row = &proposal->unknowns->as.array.items[index];
      REPORT_TRY(ab_buffer_literal(buffer, "- `"));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "code")->as.text.data,
                           ab_value_member(row, "code")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` (`"));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "scope")->as.text.data,
                           ab_value_member(row, "scope")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "`): "));
      REPORT_TRY(ab_buffer_append(
          buffer, ab_value_member(row, "message")->as.text.data,
          ab_value_member(row, "message")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, " ["));
      REPORT_TRY(ab_buffer_append(buffer,
                                  ab_value_member(row, "id")->as.text.data,
                                  ab_value_member(row, "id")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "]\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(buffer, "- none\n"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Suggested preserved checks\n\n"));
  if (proposal->preserved->as.array.count) {
    for (index = 0; index < proposal->preserved->as.array.count; index++) {
      const AbValue *row = &proposal->preserved->as.array.items[index];
      REPORT_TRY(ab_buffer_literal(buffer, "- `"));
      REPORT_TRY(ab_buffer_append(buffer,
                                  ab_value_member(row, "id")->as.text.data,
                                  ab_value_member(row, "id")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` before="));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "status")->as.text.data,
                           ab_value_member(row, "status")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, " `"));
      REPORT_TRY(append_prefix(buffer, ab_value_member(row, "sha256"), 16));
      REPORT_TRY(ab_buffer_literal(buffer, "`\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(buffer, "- none\n"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Evidence\n\n"));
  {
    size_t evidence_count = proposal->evidence->as.array.count;
    size_t shown = full || evidence_count < 20 ? evidence_count : 20;
    for (index = 0; index < shown; index++) {
      REPORT_TRY(ab_buffer_literal(buffer, "- "));
      REPORT_TRY(render_evidence_value_label(
          buffer, &proposal->evidence->as.array.items[index]));
      REPORT_TRY(ab_buffer_literal(buffer, "\n"));
    }
    if (shown != evidence_count) {
      REPORT_TRY(ab_buffer_literal(buffer, "- … "));
      REPORT_TRY(ab_buffer_u64(buffer, evidence_count - shown));
      REPORT_TRY(
          ab_buffer_literal(buffer, " more rows omitted; JSON is complete.\n"));
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_id_list(AbBuffer *buffer, const AbValue *rows,
                                     int objects, const char *empty) {
  size_t index;
  if (!rows->as.array.count)
    return ab_buffer_literal(buffer, empty);
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *value =
        objects ? ab_value_member(&rows->as.array.items[index], "id")
                : &rows->as.array.items[index];
    REPORT_TRY(ab_buffer_literal(buffer, "- `"));
    REPORT_TRY(
        ab_buffer_append(buffer, value->as.text.data, value->as.text.length));
    REPORT_TRY(ab_buffer_literal(buffer, "`\n"));
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_act_contract_render_markdown(AbBuffer *buffer,
                                const AbActContractView *contract) {
  const AbValue *origin_check = ab_value_member(contract->origin, "check");
  const AbValue *sha = ab_value_member(&contract->root, "sha256");
  const AbValue *proposal_sha =
      ab_value_member(&contract->root, "proposal_sha256");
  size_t index;
  REPORT_TRY(
      ab_buffer_literal(buffer, "# Reviewed architecture change contract: "));
  REPORT_TRY(ab_buffer_append(buffer, origin_check->as.text.data,
                              origin_check->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\nContract `"));
  REPORT_TRY(append_prefix(buffer, sha, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "` asserts review of proposal `"));
  REPORT_TRY(append_prefix(buffer, proposal_sha, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "`.\n\nOwner: "));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(&contract->root, "owner")->as.text.data,
      ab_value_member(&contract->root, "owner")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\nObjective: "));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(&contract->root, "objective")->as.text.data,
      ab_value_member(&contract->root, "objective")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\nRationale: "));
  REPORT_TRY(ab_buffer_append(
      buffer, ab_value_member(&contract->root, "rationale")->as.text.data,
      ab_value_member(&contract->root, "rationale")->as.text.length));
  REPORT_TRY(ab_buffer_literal(buffer, "\n\n## Approved postconditions\n\n"));
  REPORT_TRY(render_id_list(buffer, contract->postconditions, 0, "- none\n"));
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Preserved checks\n\n"));
  if (contract->preserved->as.array.count) {
    for (index = 0; index < contract->preserved->as.array.count; index++) {
      const AbValue *row = &contract->preserved->as.array.items[index];
      REPORT_TRY(ab_buffer_literal(buffer, "- `"));
      REPORT_TRY(ab_buffer_append(buffer,
                                  ab_value_member(row, "id")->as.text.data,
                                  ab_value_member(row, "id")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, "` before="));
      REPORT_TRY(
          ab_buffer_append(buffer, ab_value_member(row, "status")->as.text.data,
                           ab_value_member(row, "status")->as.text.length));
      REPORT_TRY(ab_buffer_literal(buffer, " `"));
      REPORT_TRY(append_prefix(buffer, ab_value_member(row, "sha256"), 16));
      REPORT_TRY(ab_buffer_literal(buffer, "`\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(buffer, "- none\n"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Selected candidates\n\n"));
  REPORT_TRY(
      render_id_list(buffer, contract->selected_candidates, 0, "- none\n"));
  REPORT_TRY(ab_buffer_literal(buffer, "\n## Acknowledged unknowns\n\n"));
  REPORT_TRY(
      render_id_list(buffer, contract->acknowledged_unknowns, 0, "- none\n"));
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_result_render_markdown(AbBuffer *buffer,
                                             const AbActResultData *result) {
  static const char *const states[] = {
      "satisfied", "missing", "unexpected", "unknown", "stale", "superseded",
  };
  size_t counts[6] = {0};
  size_t index;
  size_t state_index;
  for (index = 0; index < result->outcome_count; index++)
    for (state_index = 0; state_index < 6; state_index++)
      if (strcmp(result->outcomes[index].status, states[state_index]) == 0)
        counts[state_index]++;
  REPORT_TRY(
      ab_buffer_literal(buffer, "# Architecture change result\n\nVerdict: **"));
  REPORT_TRY(ab_buffer_literal(buffer, result->status));
  REPORT_TRY(ab_buffer_literal(buffer, "**; freshness `"));
  REPORT_TRY(ab_buffer_literal(buffer, result->freshness));
  REPORT_TRY(ab_buffer_literal(buffer, "`; result `"));
  REPORT_TRY(append_string_prefix(buffer, result->sha256, 64, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "`.\n\nProposal `"));
  REPORT_TRY(append_string_prefix(buffer, result->proposal_sha256, 64, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "`; contract `"));
  REPORT_TRY(append_string_prefix(buffer, result->contract_sha256, 64, 16));
  REPORT_TRY(ab_buffer_literal(buffer, "`.\n\n```text\n"));
  for (state_index = 0; state_index < 6; state_index++) {
    if (state_index)
      REPORT_TRY(ab_buffer_literal(buffer, " "));
    REPORT_TRY(ab_buffer_literal(buffer, states[state_index]));
    REPORT_TRY(ab_buffer_literal(buffer, "="));
    REPORT_TRY(ab_buffer_u64(buffer, counts[state_index]));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "\n```\n\n## Outcomes\n\n"));
  if (result->outcome_count) {
    for (index = 0; index < result->outcome_count; index++) {
      const AbActOutcome *row = &result->outcomes[index];
      size_t evidence_index;
      REPORT_TRY(ab_buffer_literal(buffer, "### "));
      REPORT_TRY(ab_buffer_append(buffer, row->id.data, row->id.length));
      REPORT_TRY(ab_buffer_literal(buffer, "\n\n`"));
      REPORT_TRY(ab_buffer_literal(buffer, row->kind));
      REPORT_TRY(ab_buffer_literal(buffer, "` → **"));
      REPORT_TRY(ab_buffer_literal(buffer, row->status));
      REPORT_TRY(ab_buffer_literal(buffer, "**\n\n"));
      REPORT_TRY(ab_buffer_literal(buffer, row->message));
      REPORT_TRY(ab_buffer_literal(buffer, "\n\n"));
      for (evidence_index = 0; evidence_index < row->evidence.count;
           evidence_index++) {
        REPORT_TRY(ab_buffer_literal(buffer, "- "));
        REPORT_TRY(render_evidence_label(buffer,
                                         &row->evidence.items[evidence_index]));
        REPORT_TRY(ab_buffer_literal(buffer, "\n"));
      }
      if (row->evidence.count)
        REPORT_TRY(ab_buffer_literal(buffer, "\n"));
    }
  } else {
    REPORT_TRY(ab_buffer_literal(buffer, "- none\n"));
  }
  if (result->diagnostics.count) {
    REPORT_TRY(ab_buffer_literal(buffer, "\n## Diagnostics\n\n"));
    for (index = 0; index < result->diagnostics.count; index++) {
      REPORT_TRY(ab_buffer_literal(buffer, "- "));
      REPORT_TRY(ab_buffer_append(buffer, result->diagnostics.items[index].data,
                                  result->diagnostics.items[index].length));
      REPORT_TRY(ab_buffer_literal(buffer, "\n"));
    }
  }
  return ARCHBIRD_OK;
}

static const char *sarif_level(const char *status) {
  if (strcmp(status, "missing") == 0 || strcmp(status, "unexpected") == 0 ||
      strcmp(status, "stale") == 0)
    return "error";
  if (strcmp(status, "unknown") == 0)
    return "warning";
  if (strcmp(status, "superseded") == 0)
    return "note";
  return "none";
}

static ArchbirdStatus render_sarif_locations(AbBuffer *buffer,
                                             const AbActOutcome *row) {
  size_t index;
  const AbVerifyEvidence *found = NULL;
  for (index = 0; index < row->evidence.count; index++)
    if (row->evidence.items[index].path.length) {
      found = &row->evidence.items[index];
      break;
    }
  REPORT_TRY(ab_buffer_literal(buffer, "["));
  if (found) {
    REPORT_TRY(ab_buffer_literal(
        buffer, "{\"physicalLocation\":{\"artifactLocation\":{\"uri\":"));
    REPORT_TRY(
        ab_buffer_json_string(buffer, found->path.data, found->path.length));
    REPORT_TRY(ab_buffer_literal(buffer, "}"));
    if (found->line) {
      REPORT_TRY(ab_buffer_literal(buffer, ",\"region\":{\"startLine\":"));
      REPORT_TRY(ab_buffer_u64(buffer, found->line));
      REPORT_TRY(ab_buffer_literal(buffer, "}"));
    }
    REPORT_TRY(ab_buffer_literal(buffer, "}}"));
  }
  return ab_buffer_literal(buffer, "]");
}

ArchbirdStatus ab_act_result_render_sarif(AbBuffer *buffer,
                                          const AbActResultData *result) {
  size_t index;
  size_t emitted = 0;
  REPORT_TRY(ab_buffer_literal(
      buffer, "{\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\","
              "\"runs\":[{\"properties\":{\"freshness\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, result->freshness,
                                   strlen(result->freshness)));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"result_sha256\":"));
  REPORT_TRY(ab_buffer_json_string(buffer, result->sha256, 64));
  REPORT_TRY(ab_buffer_literal(buffer, ",\"status\":"));
  REPORT_TRY(
      ab_buffer_json_string(buffer, result->status, strlen(result->status)));
  REPORT_TRY(ab_buffer_literal(buffer, "},\"results\":["));
  for (index = 0; index < result->outcome_count; index++) {
    const AbActOutcome *row = &result->outcomes[index];
    if (strcmp(row->status, "satisfied") == 0)
      continue;
    if (emitted++)
      REPORT_TRY(ab_buffer_literal(buffer, ","));
    REPORT_TRY(ab_buffer_literal(buffer, "{\"level\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, sarif_level(row->status),
                                     strlen(sarif_level(row->status))));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"locations\":"));
    REPORT_TRY(render_sarif_locations(buffer, row));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"message\":{\"text\":"));
    REPORT_TRY(
        ab_buffer_json_string(buffer, row->message, strlen(row->message)));
    REPORT_TRY(
        ab_buffer_literal(buffer, "},\"properties\":{\"contract_sha256\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, result->contract_sha256, 64));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, row->kind, strlen(row->kind)));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"proposal_sha256\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, result->proposal_sha256, 64));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"status\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, row->status, strlen(row->status)));
    REPORT_TRY(ab_buffer_literal(buffer, "},\"ruleId\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, row->id.data, row->id.length));
    REPORT_TRY(ab_buffer_literal(buffer, "}"));
  }
  REPORT_TRY(ab_buffer_literal(
      buffer,
      "],\"tool\":{\"driver\":{\"informationUri\":"
      "\"https://github.com/zemlyansky/archbird\",\"name\":\"Archbird Act\","
      "\"rules\":["));
  for (index = 0; index < result->outcome_count; index++) {
    const AbActOutcome *row = &result->outcomes[index];
    if (index)
      REPORT_TRY(ab_buffer_literal(buffer, ","));
    REPORT_TRY(ab_buffer_literal(buffer, "{\"id\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, row->id.data, row->id.length));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    REPORT_TRY(ab_buffer_json_string(buffer, row->kind, strlen(row->kind)));
    REPORT_TRY(ab_buffer_literal(buffer, ",\"shortDescription\":{\"text\":"));
    REPORT_TRY(
        ab_buffer_json_string(buffer, row->message, strlen(row->message)));
    REPORT_TRY(ab_buffer_literal(buffer, "}}"));
  }
  REPORT_TRY(ab_buffer_literal(buffer, "],\"version\":\"" ARCHBIRD_VERSION
                                       "\"}}}],\"version\":\"2.1.0\"}"));
  return ARCHBIRD_OK;
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

static int junit_failure(const char *status) {
  return strcmp(status, "missing") == 0 || strcmp(status, "unexpected") == 0 ||
         strcmp(status, "stale") == 0;
}

static ArchbirdStatus render_junit_outcome(AbBuffer *buffer,
                                           const AbActOutcome *row) {
  size_t index;
  REPORT_TRY(ab_buffer_literal(buffer, "  <testcase"));
  REPORT_TRY(xml_attribute(buffer, "classname", "archbird.change", 15));
  REPORT_TRY(xml_attribute(buffer, "name", row->id.data, row->id.length));
  if (strcmp(row->status, "satisfied") == 0) {
    return ab_buffer_literal(buffer, " />\n");
  }
  REPORT_TRY(ab_buffer_literal(buffer, ">\n"));
  if (junit_failure(row->status) || strcmp(row->status, "unknown") == 0) {
    const char *tag = junit_failure(row->status) ? "failure" : "error";
    REPORT_TRY(ab_buffer_literal(buffer, "    <"));
    REPORT_TRY(ab_buffer_literal(buffer, tag));
    REPORT_TRY(xml_attribute(buffer, "type", row->status, strlen(row->status)));
    REPORT_TRY(
        xml_attribute(buffer, "message", row->message, strlen(row->message)));
    REPORT_TRY(ab_buffer_literal(buffer, ">"));
    for (index = 0; index < row->evidence.count; index++) {
      AbBuffer label;
      ArchbirdStatus status;
      if (index)
        REPORT_TRY(ab_buffer_literal(buffer, "\n"));
      ab_buffer_init(&label, buffer->engine);
      status = render_evidence_label(&label, &row->evidence.items[index]);
      if (status == ARCHBIRD_OK)
        status = xml_escape(buffer, (const char *)label.data, label.length, 0);
      ab_buffer_free(&label);
      if (status != ARCHBIRD_OK)
        return status;
    }
    REPORT_TRY(ab_buffer_literal(buffer, "</"));
    REPORT_TRY(ab_buffer_literal(buffer, tag));
    REPORT_TRY(ab_buffer_literal(buffer, ">\n"));
  } else if (strcmp(row->status, "superseded") == 0) {
    REPORT_TRY(ab_buffer_literal(buffer, "    <skipped"));
    REPORT_TRY(
        xml_attribute(buffer, "message", row->message, strlen(row->message)));
    REPORT_TRY(ab_buffer_literal(buffer, " />\n"));
  }
  return ab_buffer_literal(buffer, "  </testcase>\n");
}

ArchbirdStatus ab_act_result_render_junit(AbBuffer *buffer,
                                          const AbActResultData *result) {
  size_t failures = 0;
  size_t errors = 0;
  size_t skipped = 0;
  size_t tests = result->outcome_count ? result->outcome_count : 1;
  size_t index;
  for (index = 0; index < result->outcome_count; index++) {
    failures += junit_failure(result->outcomes[index].status);
    errors += strcmp(result->outcomes[index].status, "unknown") == 0;
    skipped += strcmp(result->outcomes[index].status, "superseded") == 0;
  }
  if (!result->outcome_count) {
    failures = junit_failure(result->status);
    errors = strcmp(result->status, "unknown") == 0;
    skipped = strcmp(result->status, "superseded") == 0;
  }
  REPORT_TRY(ab_buffer_literal(
      buffer, "<?xml version='1.0' encoding='utf-8'?>\n<testsuite"));
  REPORT_TRY(xml_attribute(buffer, "name", "archbird-change-contract", 24));
  {
    AbBuffer number;
    ab_buffer_init(&number, buffer->engine);
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
  REPORT_TRY(ab_buffer_literal(buffer, ">\n"));
  if (result->outcome_count) {
    for (index = 0; index < result->outcome_count; index++)
      REPORT_TRY(render_junit_outcome(buffer, &result->outcomes[index]));
  } else {
    AbActOutcome synthetic = {0};
    AbBuffer message;
    ab_buffer_init(&message, buffer->engine);
    synthetic.id.data = (char *)"change-contract";
    synthetic.id.length = 15;
    synthetic.kind = "contract";
    synthetic.status = result->status;
    for (index = 0; index < result->diagnostics.count; index++) {
      if (index)
        REPORT_TRY(ab_buffer_literal(&message, "; "));
      REPORT_TRY(ab_buffer_append(&message,
                                  result->diagnostics.items[index].data,
                                  result->diagnostics.items[index].length));
    }
    if (!message.length)
      REPORT_TRY(ab_buffer_literal(&message, result->status));
    synthetic.message = (const char *)message.data;
    REPORT_TRY(render_junit_outcome(buffer, &synthetic));
    ab_buffer_free(&message);
  }
  return ab_buffer_literal(buffer, "</testsuite>\n");
}
