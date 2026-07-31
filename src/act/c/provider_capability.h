#ifndef ARCHBIRD_ACT_C_PROVIDER_CAPABILITY_H
#define ARCHBIRD_ACT_C_PROVIDER_CAPABILITY_H

#include "act_executor_internal.h"
#include "config.h"

typedef struct AbActCProviderCapability {
  const AbString *definition_sha256;
  const AbString *kind;
  const AbString *surface;
  const AbString *path;
  const AbString *capability;
  const AbConfigProvider *configuration;
  const AbValue *mapped_surface;
  const AbValue *target;
} AbActCProviderCapability;

ArchbirdStatus ab_act_c_provider_capability(AbActContext *context,
                                            const AbValue *operation,
                                            const AbString *item_id);

#endif
