#ifndef ARCHBIRD_SYNTAX_REGISTRY_H
#define ARCHBIRD_SYNTAX_REGISTRY_H

#include <archbird/archbird.h>

int ab_syntax_provider_known(const char *provider_id,
                             size_t provider_id_length);

ArchbirdStatus ab_scan_syntax_providers(ArchbirdEngine *engine,
                                        ArchbirdProject *project,
                                        ArchbirdProviderMode mode);

ArchbirdStatus ab_scan_syntax_provider(ArchbirdEngine *engine,
                                       ArchbirdProject *project,
                                       const char *provider_id,
                                       size_t provider_id_length,
                                       ArchbirdProviderMode mode);

ArchbirdStatus
ab_scan_syntax_provider_file(ArchbirdEngine *engine, ArchbirdProject *project,
                             const char *provider_id, size_t provider_id_length,
                             const char *path, size_t path_length,
                             ArchbirdProviderMode mode);

#endif
