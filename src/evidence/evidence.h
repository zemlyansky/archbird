#ifndef ARCHBIRD_EVIDENCE_H
#define ARCHBIRD_EVIDENCE_H

#include "model.h"

ArchbirdStatus ab_decode_source_manifest(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbSourceManifest *out);

ArchbirdStatus ab_decode_provider_bundle(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbProviderBundle *out);

#endif
