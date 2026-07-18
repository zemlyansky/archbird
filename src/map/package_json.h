#ifndef ARCHBIRD_PACKAGE_JSON_H
#define ARCHBIRD_PACKAGE_JSON_H

#include "map_internal.h"

ArchbirdStatus ab_decode_npm_manifest(ArchbirdEngine *engine,
                                      const uint8_t *json, size_t json_length,
                                      AbMapPackage *package);

#endif
