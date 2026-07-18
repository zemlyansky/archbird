#include "okf_publish_internal.h"

#include <string.h>

#define ACT_TRY(expression)                                                    \
  do {                                                                         \
    ArchbirdStatus act_status_ = (expression);                                 \
    if (act_status_ != ARCHBIRD_OK)                                            \
      return act_status_;                                                      \
  } while (0)

static const AbValue EMPTY_ARRAY = {.kind = AB_VALUE_ARRAY};
static const AbString EMPTY_TEXT = {(char *)"", 0};

static const AbValue *array_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = object ? ab_value_member(object, name) : NULL;
  return !value ? &EMPTY_ARRAY : value->kind == AB_VALUE_ARRAY ? value : NULL;
}

static const AbString *text_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = object ? ab_value_member(object, name) : NULL;
  return !value                           ? &EMPTY_TEXT
         : value->kind == AB_VALUE_STRING ? &value->as.text
                                          : NULL;
}

static ArchbirdStatus md_header(AbBuffer *body, const char *const *headers,
                                size_t count) {
  size_t index;
  ACT_TRY(ab_buffer_literal(body, "| "));
  for (index = 0; index < count; index++) {
    if (index)
      ACT_TRY(ab_buffer_literal(body, " | "));
    ACT_TRY(ab_buffer_literal(body, headers[index]));
  }
  ACT_TRY(ab_buffer_literal(body, " |\n| "));
  for (index = 0; index < count; index++) {
    if (index)
      ACT_TRY(ab_buffer_literal(body, " | "));
    ACT_TRY(ab_buffer_literal(body, "---"));
  }
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus md_cell(AbBuffer *body) {
  return ab_buffer_literal(body, " | ");
}

static ArchbirdStatus md_end(AbBuffer *body) {
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus artifact_path(AbOkfPublication *pub, const char *prefix,
                                    const char sha256[65], AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus relation_path(AbOkfPublication *pub,
                                    AbOkfPubRelationList *relations,
                                    const char *kind, const AbString *path) {
  AbBuffer target;
  ArchbirdStatus status;
  if (!path || path->length < 3 ||
      memcmp(path->data + path->length - 3, ".md", 3))
    return ab_okf_pub_error(pub, "invalid Act OKF relation path");
  ab_buffer_init(&target, pub->engine);
  status = ab_buffer_append(&target, path->data, path->length - 3);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, relations, kind,
                                        (const char *)target.data);
  ab_buffer_free(&target);
  return status;
}

static int verification_has_finding(const AbOkfPublication *pub,
                                    const AbString *fingerprint) {
  size_t check_index;
  if (!pub->has_verification || !fingerprint)
    return 0;
  for (check_index = 0; check_index < pub->verification.checks->as.array.count;
       check_index++) {
    const AbValue *findings = array_or_empty(
        &pub->verification.checks->as.array.items[check_index], "findings");
    size_t finding_index;
    if (!findings)
      return 0;
    for (finding_index = 0; finding_index < findings->as.array.count;
         finding_index++) {
      const AbString *candidate = ab_okf_pub_text(
          &findings->as.array.items[finding_index], "fingerprint");
      if (candidate && ab_string_equal(candidate, fingerprint))
        return 1;
    }
  }
  return 0;
}

static ArchbirdStatus finding_path(AbOkfPublication *pub,
                                   const AbString *fingerprint, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "verification/findings/");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, fingerprint->data, fingerprint->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus count_description(AbOkfPublication *pub, AbString *out,
                                        const char *prefix, size_t count,
                                        const char *suffix) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, suffix);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_proposal(AbOkfPublication *pub,
                                   const AbString *proposal_path,
                                   const AbString *contract_path) {
  const AbValue *origin = pub->proposal.origin;
  const AbValue *finding =
      ab_okf_pub_member(origin, "finding", AB_VALUE_OBJECT);
  const AbString *check_id = ab_okf_pub_text(origin, "check");
  const AbString *fingerprint = ab_okf_pub_text(finding, "fingerprint");
  const AbString *message = ab_okf_pub_text(finding, "message");
  const AbValue *postconditions = pub->proposal.postconditions;
  const AbValue *candidates = pub->proposal.candidates;
  const AbValue *unknowns = pub->proposal.unknowns;
  const char *post_headers[] = {"ID", "Assertion", "Derivation", "Coverage",
                                "Domain"};
  const char *candidate_headers[] = {"ID",   "Project", "Path",
                                     "Kind", "Reason",  "Coverage"};
  const char *unknown_headers[] = {"ID", "Code", "Scope", "Message"};
  AbString finding_page = {0};
  int has_finding;
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[1] = {{"artifact_sha256", {0}}};
  AbString entity = {(char *)pub->proposal.sha256, 64};
  AbString title = {0};
  AbString description = {0};
  AbString tags[] = {{(char *)"change", 6}, {(char *)"proposal", 8}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!origin || !finding || !check_id || !fingerprint || !message ||
      !postconditions || !candidates || !unknowns)
    return ab_okf_pub_error(pub, "invalid change proposal publication");
  has_finding = verification_has_finding(pub, fingerprint);
  if (has_finding)
    status = finding_path(pub, fingerprint, &finding_page);
  if (status == ARCHBIRD_OK && has_finding)
    status =
        relation_path(pub, &relations, "derived_from_finding", &finding_page);
  if (status == ARCHBIRD_OK && pub->has_contract)
    status = relation_path(pub, &relations, "reviewed_as", contract_path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &extra[0].json, pub->proposal.sha256);
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_literal(&value, "Proposal for ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&value, message->data, message->length);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &title);
    ab_buffer_free(&value);
  }
  if (status == ARCHBIRD_OK)
    status =
        count_description(pub, &description, "Derived change proposal with ",
                          postconditions->as.array.count, " postconditions.");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Change proposal ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&body, pub->proposal.sha256, 12);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\nDerived from ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, check_id);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, " finding ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, fingerprint);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ".\n\n# Origin\n\n");
  if (status == ARCHBIRD_OK && has_finding)
    status = ab_okf_pub_relative_link(&body, proposal_path->data,
                                      finding_page.data, message);
  else if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, message);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n# Postconditions\n\n");
  if (status == ARCHBIRD_OK && postconditions->as.array.count)
    status = md_header(&body, post_headers, 5);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0;
       status == ARCHBIRD_OK && index < postconditions->as.array.count;
       index++) {
    const AbValue *row = &postconditions->as.array.items[index];
    const AbValue *check = ab_okf_pub_member(row, "check", AB_VALUE_OBJECT);
    const AbValue *coverage =
        ab_okf_pub_member(row, "coverage", AB_VALUE_OBJECT);
    const AbString *id = ab_okf_pub_text(row, "id");
    const AbString *assertion = ab_okf_pub_text(check, "assert");
    const AbString *strength = ab_okf_pub_text(row, "derivation_strength");
    const AbString *classification =
        ab_okf_pub_text(coverage, "classification");
    const AbString *domain = ab_okf_pub_text(coverage, "domain");
    if (!check || !coverage || !id || !assertion || !strength ||
        !classification || !domain) {
      status = ab_okf_pub_error(pub, "invalid change proposal postcondition");
      break;
    }
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, assertion);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, strength);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, classification);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, domain);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Candidate edit sites\n\n");
  if (status == ARCHBIRD_OK && candidates->as.array.count)
    status = md_header(&body, candidate_headers, 6);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < candidates->as.array.count;
       index++) {
    const AbValue *row = &candidates->as.array.items[index];
    const AbValue *coverage =
        ab_okf_pub_member(row, "coverage", AB_VALUE_OBJECT);
    const AbString *id = ab_okf_pub_text(row, "id");
    const AbString *project = ab_okf_pub_text(row, "project");
    const AbString *path = ab_okf_pub_text(row, "path");
    const AbString *kind = ab_okf_pub_text(row, "kind");
    const AbString *reason = ab_okf_pub_text(row, "reason");
    const AbString *classification =
        ab_okf_pub_text(coverage, "classification");
    if (!coverage || !id || !project || !path || !kind || !reason ||
        !classification) {
      status = ab_okf_pub_error(pub, "invalid change proposal candidate");
      break;
    }
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, project);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, path);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, kind);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, reason);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, classification);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Unknown frontier\n\n");
  if (status == ARCHBIRD_OK && unknowns->as.array.count)
    status = md_header(&body, unknown_headers, 4);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < unknowns->as.array.count;
       index++) {
    const AbValue *row = &unknowns->as.array.items[index];
    const AbString *id = ab_okf_pub_text(row, "id");
    const AbString *code = ab_okf_pub_text(row, "code");
    const AbString *scope = ab_okf_pub_text(row, "scope");
    const AbString *unknown_message = ab_okf_pub_text(row, "message");
    if (!id || !code || !scope || !unknown_message) {
      status = ab_okf_pub_error(pub, "invalid change proposal unknown");
      break;
    }
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, code);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, scope);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, unknown_message);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body,
        "\nCandidate paths are evidence, not edit authorization. Unknowns are "
        "not pass states.\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->proposal_source,
                             proposal_path->data,
                             "Archbird Change Proposal",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "change_proposal",
                             &entity,
                             tags,
                             2,
                             &relations,
                             extra,
                             1,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &finding_page);
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_fields_free(pub, extra, 1);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus bullet_codes(AbOkfPublication *pub, AbBuffer *body,
                                   const AbValue *rows, int none_if_empty) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid Act string inventory");
  if (!rows->as.array.count && none_if_empty)
    return ab_buffer_literal(body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *value = &rows->as.array.items[index];
    if (value->kind != AB_VALUE_STRING)
      return ab_okf_pub_error(pub, "invalid Act string inventory item");
    status = ab_buffer_literal(body, "* ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(body, &value->as.text);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(body, "\n");
  }
  return status;
}

static ArchbirdStatus add_contract(AbOkfPublication *pub,
                                   const AbString *proposal_path,
                                   const AbString *contract_path,
                                   const AbString *result_path) {
  const AbValue *root = &pub->contract.root;
  const AbString *objective = ab_okf_pub_text(root, "objective");
  const AbString *owner = ab_okf_pub_text(root, "owner");
  const AbString *rationale = ab_okf_pub_text(root, "rationale");
  const AbValue *postconditions = pub->contract.postconditions;
  const AbValue *selected = pub->contract.selected_candidates;
  const AbValue *unknowns = pub->contract.acknowledged_unknowns;
  const char *headers[] = {"Owner", "Rationale", "Proposal"};
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[1] = {{"artifact_sha256", {0}}};
  AbString entity = {(char *)pub->contract.sha256, 64};
  AbString title = {0};
  AbString description = {0};
  AbString tags[] = {{(char *)"change", 6}, {(char *)"contract", 8}};
  AbBuffer body;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!objective || !owner || !rationale || !postconditions || !selected ||
      !unknowns)
    return ab_okf_pub_error(pub, "invalid change contract publication");
  status = relation_path(pub, &relations, "reviews_proposal", proposal_path);
  if (status == ARCHBIRD_OK && pub->has_result)
    status = relation_path(pub, &relations, "judged_by", result_path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &extra[0].json, pub->contract.sha256);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, &title, objective->data, objective->length);
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_literal(&value, "Reviewed change contract owned by ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&value, owner->data, owner->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ".");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &description);
    ab_buffer_free(&value);
  }
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, objective);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, headers, 3);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, owner);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, rationale);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK) {
    AbString label = {(char *)pub->proposal.sha256, 12};
    status = ab_okf_pub_relative_link(&body, contract_path->data,
                                      proposal_path->data, &label);
  }
  if (status == ARCHBIRD_OK)
    status = md_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Approved postconditions\n\n");
  if (status == ARCHBIRD_OK)
    status = bullet_codes(pub, &body, postconditions, 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Selected candidates\n\n");
  if (status == ARCHBIRD_OK)
    status = bullet_codes(pub, &body, selected, 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Acknowledged unknowns\n\n");
  if (status == ARCHBIRD_OK)
    status = bullet_codes(pub, &body, unknowns, 1);
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->contract_source,
                             contract_path->data,
                             "Archbird Change Contract",
                             &title,
                             NULL,
                             &description,
                             "asserted",
                             "change_contract",
                             &entity,
                             tags,
                             2,
                             &relations,
                             extra,
                             1,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_fields_free(pub, extra, 1);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus add_result(AbOkfPublication *pub,
                                 const AbString *proposal_path,
                                 const AbString *contract_path,
                                 const AbString *result_path) {
  const AbString *result_status = ab_okf_pub_text(&pub->result, "status");
  const AbString *freshness = ab_okf_pub_text(&pub->result, "freshness");
  const AbString *before =
      ab_okf_pub_text(&pub->result, "before_verification_sha256");
  const AbString *after =
      text_or_empty(&pub->result, "after_verification_sha256");
  const AbValue *outcomes = array_or_empty(&pub->result, "outcomes");
  const AbValue *diagnostics = array_or_empty(&pub->result, "diagnostics");
  const AbString *declared_sha = ab_okf_pub_text(&pub->result, "sha256");
  const char *summary_headers[] = {"Status", "Freshness", "Before", "After"};
  const char *outcome_headers[] = {"ID", "Kind", "Status", "Message"};
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[1] = {{"artifact_sha256", {0}}};
  AbString title = {0};
  AbString description = {0};
  AbString tags[4] = {{(char *)"change", 6}, {(char *)"result", 6}, {0}, {0}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!result_status || !freshness || !before || !after || !outcomes ||
      !diagnostics || !declared_sha)
    return ab_okf_pub_error(pub, "invalid change result publication");
  tags[2] = *result_status;
  tags[3] = *freshness;
  status = relation_path(pub, &relations, "judges_proposal", proposal_path);
  if (status == ARCHBIRD_OK)
    status = relation_path(pub, &relations, "judges_contract", contract_path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[0].json, declared_sha);
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_literal(&value, "Change result: ");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&value, result_status->data, result_status->length);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &title);
    ab_buffer_free(&value);
  }
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status =
        ab_buffer_literal(&value, "Derived contract judgment with freshness ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&value, freshness->data, freshness->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&value, ".");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &description);
    ab_buffer_free(&value);
  }
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Change result: ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, result_status);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, summary_headers, 4);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, result_status);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, freshness);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, before);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, after);
  if (status == ARCHBIRD_OK)
    status = md_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Reviewed transition\n\n* ");
  if (status == ARCHBIRD_OK) {
    AbString label = {(char *)"Change proposal", 15};
    status = ab_okf_pub_relative_link(&body, result_path->data,
                                      proposal_path->data, &label);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n* ");
  if (status == ARCHBIRD_OK) {
    AbString label = {(char *)"Change contract", 15};
    status = ab_okf_pub_relative_link(&body, result_path->data,
                                      contract_path->data, &label);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n# Outcomes\n\n");
  if (status == ARCHBIRD_OK && outcomes->as.array.count)
    status = md_header(&body, outcome_headers, 4);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < outcomes->as.array.count;
       index++) {
    const AbValue *row = &outcomes->as.array.items[index];
    const AbString *id = ab_okf_pub_text(row, "id");
    const AbString *kind = ab_okf_pub_text(row, "kind");
    const AbString *outcome_status = ab_okf_pub_text(row, "status");
    const AbString *message = ab_okf_pub_text(row, "message");
    if (!id || !kind || !outcome_status || !message) {
      status = ab_okf_pub_error(pub, "invalid change result outcome");
      break;
    }
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, kind);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, outcome_status);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, message);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Diagnostics\n\n");
  if (status == ARCHBIRD_OK && !diagnostics->as.array.count)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < diagnostics->as.array.count;
       index++) {
    const AbValue *value = &diagnostics->as.array.items[index];
    if (value->kind != AB_VALUE_STRING) {
      status = ab_okf_pub_error(pub, "invalid change result diagnostic");
      break;
    }
    status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &value->as.text);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->result_source,
                             result_path->data,
                             "Archbird Change Result",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "change_result",
                             declared_sha,
                             tags,
                             4,
                             &relations,
                             extra,
                             1,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_fields_free(pub, extra, 1);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

ArchbirdStatus ab_okf_pub_act(AbOkfPublication *pub) {
  AbString proposal_path = {0};
  AbString contract_path = {0};
  AbString result_path = {0};
  ArchbirdStatus status;
  if (!pub || !pub->has_proposal)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = artifact_path(pub, "changes/proposals/", pub->proposal.sha256,
                         &proposal_path);
  if (status == ARCHBIRD_OK && pub->has_contract)
    status = artifact_path(pub, "changes/contracts/", pub->contract.sha256,
                           &contract_path);
  if (status == ARCHBIRD_OK && pub->has_result) {
    const AbString *sha = ab_okf_pub_text(&pub->result, "sha256");
    if (!sha || sha->length != 64)
      status = ab_okf_pub_error(pub, "invalid change result SHA-256");
    else {
      char value[65];
      memcpy(value, sha->data, 64);
      value[64] = '\0';
      status = artifact_path(pub, "changes/results/", value, &result_path);
    }
  }
  if (status == ARCHBIRD_OK)
    status = add_proposal(pub, &proposal_path, &contract_path);
  if (status == ARCHBIRD_OK && pub->has_contract)
    status = add_contract(pub, &proposal_path, &contract_path, &result_path);
  if (status == ARCHBIRD_OK && pub->has_result)
    status = add_result(pub, &proposal_path, &contract_path, &result_path);
  ab_string_free(pub->engine, &proposal_path);
  ab_string_free(pub->engine, &contract_path);
  ab_string_free(pub->engine, &result_path);
  return status;
}
