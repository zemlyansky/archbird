#ifndef ARCHBIRD_LEXICAL_R_SCANNER_H
#define ARCHBIRD_LEXICAL_R_SCANNER_H

#include "fact_builder.h"

ArchbirdStatus ab_scan_r_file(ArchbirdEngine *engine,
                              const AbSourceManifest *manifest,
                              const AbManifestFile *file, const uint8_t *source,
                              size_t source_length,
                              const uint8_t implementation_sha256[32],
                              AbProviderBundle *out_bundle);

#endif
