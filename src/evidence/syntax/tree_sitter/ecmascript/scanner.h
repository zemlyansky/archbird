#ifndef ARCHBIRD_TREE_SITTER_ECMASCRIPT_SCANNER_H
#define ARCHBIRD_TREE_SITTER_ECMASCRIPT_SCANNER_H

#include "syntax/tree_sitter/scanner.h"

/* Strip expression wrappers that preserve the identity of their operand.
 * JavaScript uses parentheses; TypeScript additionally uses assertion and
 * satisfaction wrappers. */
TSNode ab_tree_sitter_unwrap_ecmascript_expression(TSNode node);

ArchbirdStatus ab_scan_tree_sitter_ecmascript_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32],
    const AbTreeSitterDescriptor *descriptor, AbProviderBundle *out_bundle);

ArchbirdStatus ab_tree_sitter_visit_ecmascript(AbTreeSitterScan *scan,
                                               const AbTreeSitterFrame *frame,
                                               AbTreeSitterFrame *child_frame);

#endif
