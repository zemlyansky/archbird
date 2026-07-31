#ifndef ARCHBIRD_ACT_RENAME_INTERNAL_H
#define ARCHBIRD_ACT_RENAME_INTERNAL_H

#include "json_value.h"

typedef struct AbActRenameEvidence {
  const AbValue *file;
  const AbValue *providers;
  const AbString *role;
} AbActRenameEvidence;

int ab_act_python_rename_evidence_supported(const AbActRenameEvidence *evidence,
                                            const char **out_reason);
int ab_act_ecmascript_rename_evidence_supported(
    const AbActRenameEvidence *evidence, const char **out_reason);

#endif
