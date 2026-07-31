#ifndef ARCHBIRD_ACT_DEPENDENCY_REDIRECT_INTERNAL_H
#define ARCHBIRD_ACT_DEPENDENCY_REDIRECT_INTERNAL_H

#include "act_executor_internal.h"
#include "component_membership.h"
#include "projection_internal.h"

typedef struct AbActDependencyRedirect {
  const AbValue *map;
  const AbValue *projection;
  const AbProjectionItem *relation;
  const AbValue *relation_sites;
  const AbString *relation_target;
  const AbString *from_symbol;
  const AbString *to_symbol;
  AbProjectionMembershipIndex membership;
} AbActDependencyRedirect;

int ab_act_dependency_redirect_target_matches(
    const AbActDependencyRedirect *redirect, const AbString *candidate_path);

ArchbirdStatus
ab_act_c_dependency_redirect(AbActContext *context,
                             const AbActDependencyRedirect *redirect,
                             const AbString *item_id);
ArchbirdStatus
ab_act_python_dependency_redirect(AbActContext *context,
                                  const AbActDependencyRedirect *redirect,
                                  const AbString *item_id);
ArchbirdStatus
ab_act_ecmascript_dependency_redirect(AbActContext *context,
                                      const AbActDependencyRedirect *redirect,
                                      const AbString *item_id);

#endif
