#include "fuzz_common.h"

static void fuzz_okf(ArchbirdEngine *engine, const uint8_t *verification,
                     size_t verification_length, const uint8_t *proposal,
                     size_t proposal_length, const uint8_t *contract,
                     size_t contract_length, const uint8_t *result,
                     size_t result_length) {
  (void)archbird_okf_publish(engine, fuzz_map_json, sizeof(fuzz_map_json) - 1,
                             verification, verification_length, proposal,
                             proposal_length, contract, contract_length, result,
                             result_length, NULL, 0, 0, fuzz_discard, NULL);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  FuzzActChain chain;
  if (!engine)
    return 0;
  if (!fuzz_build_act_chain(engine, &chain)) {
    archbird_engine_destroy(engine);
    abort();
  }
#if defined(ARCHBIRD_FUZZ_ACT_VERIFICATION)
  (void)archbird_change_proposal(engine, data, size, chain.fingerprint, 64, 0,
                                 fuzz_discard, NULL);
  (void)archbird_change_proposal_report(engine, data, size, chain.fingerprint,
                                        64, 0, 100, fuzz_discard, NULL);
  fuzz_okf(engine, data, size, NULL, 0, NULL, 0, NULL, 0);
#elif defined(ARCHBIRD_FUZZ_ACT_PROPOSAL)
  (void)archbird_change_contract(engine, data, size, fuzz_review_json,
                                 sizeof(fuzz_review_json) - 1, 0, fuzz_discard,
                                 NULL);
  (void)archbird_change_contract_report(engine, data, size, fuzz_review_json,
                                        sizeof(fuzz_review_json) - 1,
                                        fuzz_discard, NULL);
  fuzz_okf(engine, chain.verification.data, chain.verification.length, data,
           size, NULL, 0, NULL, 0);
#elif defined(ARCHBIRD_FUZZ_ACT_REVIEW)
  (void)archbird_change_contract(engine, chain.proposal.data,
                                 chain.proposal.length, data, size, 0,
                                 fuzz_discard, NULL);
  (void)archbird_change_contract_report(engine, chain.proposal.data,
                                        chain.proposal.length, data, size,
                                        fuzz_discard, NULL);
#elif defined(ARCHBIRD_FUZZ_ACT_CONTRACT)
  (void)archbird_change_verify(
      engine, chain.proposal.data, chain.proposal.length, data, size,
      chain.verification.data, chain.verification.length,
      chain.verification.data, chain.verification.length, 0, fuzz_discard,
      NULL);
  fuzz_okf(engine, chain.verification.data, chain.verification.length,
           chain.proposal.data, chain.proposal.length, data, size, NULL, 0);
#elif defined(ARCHBIRD_FUZZ_ACT_RESULT)
  fuzz_okf(engine, chain.verification.data, chain.verification.length,
           chain.proposal.data, chain.proposal.length, chain.contract.data,
           chain.contract.length, data, size);
#else
  ArchbirdChangeFormat format;
  (void)archbird_change_verify(
      engine, chain.proposal.data, chain.proposal.length, chain.contract.data,
      chain.contract.length, chain.verification.data, chain.verification.length,
      data, size, 0, fuzz_discard, NULL);
  for (format = ARCHBIRD_CHANGE_MARKDOWN; format <= ARCHBIRD_CHANGE_JUNIT;
       format = (ArchbirdChangeFormat)(format + 1))
    (void)archbird_change_verify_report(
        engine, chain.proposal.data, chain.proposal.length, chain.contract.data,
        chain.contract.length, chain.verification.data,
        chain.verification.length, data, size, format, 0, fuzz_discard, NULL);
#endif
  fuzz_act_chain_free(&chain);
  archbird_engine_destroy(engine);
  return 0;
}
