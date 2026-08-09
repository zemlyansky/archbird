#ifndef ARCHBIRD_ACT_MAKE_PROVIDER_CAPABILITY_H
#define ARCHBIRD_ACT_MAKE_PROVIDER_CAPABILITY_H

#include "act/act_executor_internal.h"

ArchbirdStatus ab_act_make_provider_capability(AbActContext *context,
                                               const AbValue *operation,
                                               const AbString *item_id);

#endif
