#ifndef ARCHBIRD_ACT_GATE_ACCEPTANCE_H
#define ARCHBIRD_ACT_GATE_ACCEPTANCE_H

#include "base/json_value.h"

typedef struct AbActGateAcceptance {
  AbValue document;
  const AbValue *workspace_sha256;
  const AbValue *results;
} AbActGateAcceptance;

ArchbirdStatus ab_act_gate_acceptance_validate(ArchbirdEngine *engine,
                                               const AbValue *gates,
                                               const AbValue *workspace_sha256,
                                               const AbValue *results);
ArchbirdStatus ab_act_gate_acceptance_load(ArchbirdEngine *engine,
                                           const AbValue *gates,
                                           const uint8_t *json, size_t length,
                                           AbActGateAcceptance *out);
void ab_act_gate_acceptance_free(ArchbirdEngine *engine,
                                 AbActGateAcceptance *acceptance);

#endif
