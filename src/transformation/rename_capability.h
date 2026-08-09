#ifndef ARCHBIRD_TRANSFORMATION_RENAME_CAPABILITY_H
#define ARCHBIRD_TRANSFORMATION_RENAME_CAPABILITY_H

#include "base/json_value.h"
#include "projection/projection_model.h"

typedef struct AbRenameEvidence {
  const AbValue *file;
  const AbValue *providers;
  const AbString *role;
} AbRenameEvidence;

int ab_rename_evidence_supported(const AbProjectionItem *item,
                                 const AbValue *file, const char **out_reason);

#endif
