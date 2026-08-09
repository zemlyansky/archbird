#ifndef ARCHBIRD_ACT_C_DECLARATION_H
#define ARCHBIRD_ACT_C_DECLARATION_H

#include "act/c/provider_capability.h"

ArchbirdStatus ab_act_c_declare_symbol(AbActContext *context,
                                       const AbValue *operation,
                                       const AbString *item_id);
ArchbirdStatus ab_act_c_file_provider_capability(
    AbActContext *context, const AbActCProviderCapability *provider,
    const AbValue *operation, const AbString *item_id);

#endif
