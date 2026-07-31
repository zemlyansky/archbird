#ifndef ARCHBIRD_ACT_DEPENDENCY_REDIRECT_H
#define ARCHBIRD_ACT_DEPENDENCY_REDIRECT_H

#include "act_executor_internal.h"

ArchbirdStatus ab_act_dependency_redirect(AbActContext *context,
                                          const AbValue *operation,
                                          const AbString *item_id);

#endif
