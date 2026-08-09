#ifndef ARCHBIRD_ACT_JSON_PACKAGE_ENTRYPOINT_H
#define ARCHBIRD_ACT_JSON_PACKAGE_ENTRYPOINT_H

#include "act/act_executor_internal.h"

ArchbirdStatus ab_act_json_package_entrypoint(AbActContext *context,
                                              const AbValue *operation,
                                              const AbString *item_id);

#endif
