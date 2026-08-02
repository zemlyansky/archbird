#ifndef ARCHBIRD_ACT_RENAME_INTERNAL_H
#define ARCHBIRD_ACT_RENAME_INTERNAL_H

#include "json_value.h"
#include "projection_model.h"
#include "rename_capability.h"

#include <stddef.h>
#include <stdint.h>

int ab_act_python_rename_replacement_span(const AbRenameEvidence *evidence,
                                          const uint8_t *source,
                                          size_t source_length,
                                          const AbString *old_name,
                                          size_t *start, size_t *end,
                                          const char **out_reason);

#endif
