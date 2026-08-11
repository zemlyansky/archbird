#ifndef ARCHBIRD_PROJECT_PROVIDER_STORE_H
#define ARCHBIRD_PROJECT_PROVIDER_STORE_H

#include "evidence/project/project_model.h"

void ab_project_provider_store_destroy(ArchbirdEngine *engine,
                                       AbProjectProviderStore *store);

void ab_project_provider_store_canonicalize(AbProjectProviderStore *store);

#endif
