#ifndef ARCHBIRD_TREE_SITTER_ECMASCRIPT_MODULE_H
#define ARCHBIRD_TREE_SITTER_ECMASCRIPT_MODULE_H

#include "syntax/tree_sitter/scanner.h"

/* Extract the statically determined module surface of one ECMAScript program.
 * ESM records come directly from export/import declarations. CommonJS uses a
 * bounded, file-local alias/property analysis; dynamic boundaries become
 * module-surface-unknown facts rather than guessed names. */
ArchbirdStatus ab_tree_sitter_extract_ecmascript_module(AbTreeSitterScan *scan,
                                                        TSNode program);

#endif
