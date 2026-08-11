#ifndef ARCHBIRD_NPM_WORKSPACE_MANIFEST_H
#define ARCHBIRD_NPM_WORKSPACE_MANIFEST_H

#include "base/model.h"

typedef struct AbNpmDiscoveryMetadata {
  AbString name;
  AbString version;
  AbString *workspaces;
  size_t workspace_count;
  int workspaces_present;
  int workspaces_supported;
} AbNpmDiscoveryMetadata;

/* Decode only package identity plus npm/Yarn-compatible workspace path
 * patterns. The package document itself remains strict JSON. An unsupported
 * workspaces value is reported through metadata instead of invalidating the
 * otherwise useful package identity. */
ArchbirdStatus ab_npm_discovery_metadata(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbNpmDiscoveryMetadata *out);
void ab_npm_discovery_metadata_free(ArchbirdEngine *engine,
                                    AbNpmDiscoveryMetadata *metadata);

#endif
