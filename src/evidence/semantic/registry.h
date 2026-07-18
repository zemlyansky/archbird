#ifndef ARCHBIRD_SEMANTIC_REGISTRY_H
#define ARCHBIRD_SEMANTIC_REGISTRY_H

#include <archbird/archbird.h>

int ab_semantic_provider_known(const char *provider_id,
                               size_t provider_id_length);

ArchbirdStatus ab_scan_semantic_providers(ArchbirdEngine *engine,
                                          ArchbirdProject *project,
                                          ArchbirdProviderMode mode);

ArchbirdStatus ab_scan_semantic_provider(ArchbirdEngine *engine,
                                         ArchbirdProject *project,
                                         const char *provider_id,
                                         size_t provider_id_length,
                                         ArchbirdProviderMode mode);

#endif
