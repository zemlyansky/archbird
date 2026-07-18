#ifndef ARCHBIRD_TREE_SITTER_PYTHON_SCANNER_H
#define ARCHBIRD_TREE_SITTER_PYTHON_SCANNER_H

#include "fact_builder.h"

ArchbirdStatus ab_scan_tree_sitter_python_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle);

#endif
