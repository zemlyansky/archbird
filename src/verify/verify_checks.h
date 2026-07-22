#ifndef ARCHBIRD_VERIFY_CHECKS_H
#define ARCHBIRD_VERIFY_CHECKS_H

#include "verify_runtime.h"

typedef struct AbVerifyFinding {
  AbString fingerprint;
  AbString comparison;
  AbString evidence_state;
  AbString applicability;
  AbString disposition;
  AbString key;
  AbString message;
  AbVerifyEvidence *evidence;
  size_t evidence_count;
  size_t evidence_capacity;
  AbString waiver;
  AbString waiver_note;
  AbString baseline_state;
} AbVerifyFinding;

typedef struct AbVerifyCheckResult {
  const AbValue *spec;
  AbString status;
  AbVerifyFinding *findings;
  size_t finding_count;
  size_t finding_capacity;
  AbStringArray coverage;
  AbVerifyEvidence *witnesses;
  size_t witness_count;
  size_t witness_capacity;
} AbVerifyCheckResult;

void ab_verify_check_result_free(ArchbirdEngine *engine,
                                 AbVerifyCheckResult *result);
ArchbirdStatus ab_verify_evaluate_check(AbVerificationContext *context,
                                        const AbValue *check,
                                        AbVerifyCheckResult *result);
ArchbirdStatus ab_verify_evaluate_checks(AbVerificationContext *context);
ArchbirdStatus ab_verify_check_refresh_status(ArchbirdEngine *engine,
                                              AbVerifyCheckResult *result);
ArchbirdStatus ab_verify_check_render(AbBuffer *buffer,
                                      const AbVerifyCheckResult *result);

#endif
