#include "registry.h"

#include "scip/scanner.h"

#include <string.h>

static int provider_is(const char *provider_id, size_t provider_id_length,
                       const char *wanted) {
  size_t length = strlen(wanted);
  return provider_id_length == length &&
         memcmp(provider_id, wanted, length) == 0;
}

int ab_semantic_provider_known(const char *provider_id,
                               size_t provider_id_length) {
  return provider_id &&
         provider_is(provider_id, provider_id_length, "semantic:scip");
}

ArchbirdStatus ab_scan_semantic_providers(ArchbirdEngine *engine,
                                          ArchbirdProject *project,
                                          ArchbirdProviderMode mode) {
  return ab_scan_scip_indexes(engine, project, mode);
}

ArchbirdStatus ab_scan_semantic_provider(ArchbirdEngine *engine,
                                         ArchbirdProject *project,
                                         const char *provider_id,
                                         size_t provider_id_length,
                                         ArchbirdProviderMode mode) {
  if (!ab_semantic_provider_known(provider_id, provider_id_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  return ab_scan_scip_indexes(engine, project, mode);
}
