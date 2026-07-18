#include <archbird/archbird.h>

#include "act_reports.h"

ArchbirdStatus archbird_change_proposal_report(
    ArchbirdEngine *engine, const uint8_t *verification_json,
    size_t verification_length, const char *finding_fingerprint,
    size_t fingerprint_length, int full, size_t max_candidates,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbActVerification verification = {0};
  AbActProposalData compiled = {0};
  AbActProposalView proposal = {0};
  AbBuffer canonical;
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || (!verification_json && verification_length) ||
      !finding_fingerprint || (full != 0 && full != 1) || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&canonical, engine);
  ab_buffer_init(&report, engine);
  status = ab_act_verification_load(engine, verification_json,
                                    verification_length, &verification);
  if (status == ARCHBIRD_OK)
    status = ab_act_proposal_compile(engine, &verification, finding_fingerprint,
                                     fingerprint_length, &compiled);
  if (status == ARCHBIRD_OK)
    status = ab_act_proposal_render_json(&canonical, &compiled);
  if (status == ARCHBIRD_OK)
    status = ab_act_proposal_load(engine, canonical.data, canonical.length,
                                  &proposal);
  if (status == ARCHBIRD_OK)
    status = ab_act_proposal_render_markdown(&report, &proposal, full,
                                             max_candidates);
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, report.data, report.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "change proposal report callback failed");
  ab_buffer_free(&report);
  ab_buffer_free(&canonical);
  ab_act_proposal_view_free(&proposal);
  ab_act_proposal_data_free(&compiled);
  ab_act_verification_free(&verification);
  return status;
}

ArchbirdStatus archbird_change_contract_report(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *review_json, size_t review_length,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbActProposalView proposal = {0};
  AbActContractView contract = {0};
  AbBuffer canonical;
  AbBuffer report;
  ArchbirdStatus status;
  if (!engine || (!proposal_json && proposal_length) ||
      (!review_json && review_length) || !write_fn)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&canonical, engine);
  ab_buffer_init(&report, engine);
  status =
      ab_act_proposal_load(engine, proposal_json, proposal_length, &proposal);
  if (status == ARCHBIRD_OK)
    status = ab_act_contract_create_json(engine, &proposal, review_json,
                                         review_length, &canonical);
  if (status == ARCHBIRD_OK)
    status = ab_act_contract_load(engine, canonical.data, canonical.length,
                                  &contract);
  if (status == ARCHBIRD_OK)
    status = ab_act_contract_render_markdown(&report, &contract);
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, report.data, report.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "change contract report callback failed");
  ab_buffer_free(&report);
  ab_buffer_free(&canonical);
  ab_act_contract_view_free(&contract);
  ab_act_proposal_view_free(&proposal);
  return status;
}

ArchbirdStatus archbird_change_verify_report(
    ArchbirdEngine *engine, const uint8_t *proposal_json,
    size_t proposal_length, const uint8_t *contract_json,
    size_t contract_length, const uint8_t *before_verification_json,
    size_t before_length, const uint8_t *after_verification_json,
    size_t after_length, ArchbirdChangeFormat format, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbActProposalView proposal = {0};
  AbActContractView contract = {0};
  AbActVerification before = {0};
  AbActVerification after = {0};
  AbActResultData result = {0};
  AbBuffer canonical;
  AbBuffer rendered;
  ArchbirdStatus status;
  if (!engine || (!proposal_json && proposal_length) ||
      (!contract_json && contract_length) ||
      (!before_verification_json && before_length) ||
      (!after_verification_json && after_length) || !write_fn ||
      format < ARCHBIRD_CHANGE_JSON || format > ARCHBIRD_CHANGE_JUNIT ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (format == ARCHBIRD_CHANGE_JSON)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "change report format must not be JSON");
  ab_buffer_init(&canonical, engine);
  ab_buffer_init(&rendered, engine);
  status =
      ab_act_proposal_load(engine, proposal_json, proposal_length, &proposal);
  if (status == ARCHBIRD_OK)
    status =
        ab_act_contract_load(engine, contract_json, contract_length, &contract);
  if (status == ARCHBIRD_OK)
    status = ab_act_verification_load(engine, before_verification_json,
                                      before_length, &before);
  if (status == ARCHBIRD_OK)
    status = ab_act_verification_load(engine, after_verification_json,
                                      after_length, &after);
  if (status == ARCHBIRD_OK)
    status = ab_act_result_verify(engine, &proposal, &contract, &before, &after,
                                  &result);
  if (status == ARCHBIRD_OK)
    status = ab_act_result_render_json(&canonical, &result);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_CHANGE_MARKDOWN)
    status = ab_act_result_render_markdown(&rendered, &result);
  else if (status == ARCHBIRD_OK && format == ARCHBIRD_CHANGE_SARIF)
    status = ab_act_result_render_sarif(&rendered, &result);
  else if (status == ARCHBIRD_OK)
    status = ab_act_result_render_junit(&rendered, &result);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_CHANGE_SARIF)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, rendered.data, rendered.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "change result report callback failed");
  ab_buffer_free(&rendered);
  ab_buffer_free(&canonical);
  ab_act_result_data_free(&result);
  ab_act_verification_free(&after);
  ab_act_verification_free(&before);
  ab_act_contract_view_free(&contract);
  ab_act_proposal_view_free(&proposal);
  return status;
}
