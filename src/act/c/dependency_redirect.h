#ifndef ARCHBIRD_ACT_C_DEPENDENCY_REDIRECT_H
#define ARCHBIRD_ACT_C_DEPENDENCY_REDIRECT_H

#include "dependency_redirect_internal.h"

ArchbirdStatus
ab_act_c_dependency_redirect(AbActContext *context,
                             const AbActDependencyRedirect *redirect,
                             const AbString *item_id);

#endif
