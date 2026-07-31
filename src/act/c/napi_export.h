#ifndef ARCHBIRD_ACT_C_NAPI_EXPORT_H
#define ARCHBIRD_ACT_C_NAPI_EXPORT_H

#include "c/provider_capability.h"

ArchbirdStatus ab_act_c_napi_export_provider_capability(
    AbActContext *context, const AbActCProviderCapability *provider,
    const AbValue *operation, const AbString *item_id);

#endif
