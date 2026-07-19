#include "syntax/tree_sitter/javascript/scanner.h"

#include "syntax/tree_sitter/ecmascript/scanner.h"

const TSLanguage *tree_sitter_javascript(void);

ArchbirdStatus ab_scan_tree_sitter_javascript_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct identifier call_expression nodes"},
      {"exports", "statically named ESM and bounded CommonJS properties"},
      {"imported-name-groups", "static ESM module requests"},
      {"imported-names", "static ESM default, namespace, and named imports"},
      {"imports", "static import_statement string sources"},
      {"method-calls", "member call property names without receiver identity"},
      {"module-reexports", "static ESM star and CommonJS require routes"},
      {"module-surface-unknowns",
       "dynamic or conditionally mutated CommonJS surfaces"},
      {"symbols", "named classes, functions, methods, variable-bound function "
                  "values, and statically proven descriptor-helper members"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-javascript",
      "3",
      "archbird-tree-sitter-javascript-v3;runtime=0.26.9;grammar=0.25.0;abi=15",
      "tree-sitter-0.26.9;tree-sitter-javascript-0.25.0;grammar-abi-15",
      "javascript",
      tree_sitter_javascript,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      ab_tree_sitter_visit_ecmascript,
      NULL,
      NULL};
  return ab_scan_tree_sitter_ecmascript_file(
      engine, manifest, file, source, source_length, source_manifest_sha256,
      implementation_sha256, &descriptor, out_bundle);
}
