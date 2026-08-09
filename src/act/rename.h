#ifndef ARCHBIRD_ACT_RENAME_H
#define ARCHBIRD_ACT_RENAME_H

#include "act/act_executor_internal.h"

ArchbirdStatus ab_act_rename_symbol(AbActContext *context,
                                    const AbValue *operation,
                                    const AbString *item_id);

#endif
