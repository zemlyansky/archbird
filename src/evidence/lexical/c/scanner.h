#ifndef ARCHBIRD_LEXICAL_C_SCANNER_H
#define ARCHBIRD_LEXICAL_C_SCANNER_H

#include "fact_builder.h"

typedef struct AbNameRef {
  const uint8_t *data;
  size_t length;
} AbNameRef;

typedef struct AbNameSet {
  ArchbirdEngine *engine;
  AbNameRef *items;
  size_t count;
  size_t capacity;
} AbNameSet;

void ab_name_set_free(AbNameSet *names);

ArchbirdStatus ab_c_collect_public_names(ArchbirdEngine *engine,
                                         const uint8_t *source,
                                         size_t source_length,
                                         AbNameSet *names);

ArchbirdStatus ab_scan_c_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32], const AbNameSet *public_names,
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle);

#endif
