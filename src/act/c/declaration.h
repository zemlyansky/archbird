#ifndef ARCHBIRD_ACT_C_DECLARATION_H
#define ARCHBIRD_ACT_C_DECLARATION_H

#include "act_executor_internal.h"

ArchbirdStatus ab_act_c_declare_symbol(AbActContext *context,
                                       const AbValue *operation,
                                       const AbString *item_id);

#endif
