#include "evidence/syntax/tree_sitter/typescript/scanner.h"

#include "base/archbird_internal.h"
#include "evidence/syntax/tree_sitter/ecmascript/scanner.h"

#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
const TSLanguage *tree_sitter_typescript(void);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
const TSLanguage *tree_sitter_tsx(void);
#endif

ArchbirdStatus ab_scan_tree_sitter_typescript_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbMapConfig *config, const AbManifestFile *file,
    const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], int tsx,
    AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct identifier call_expression nodes"},
      {"export-bindings",
       "exact local/origin identifiers associated with static exports"},
      {"exports", "statically named ESM and bounded CommonJS properties"},
      {"imported-name-groups", "static ESM module requests"},
      {"imported-names", "static ESM default, namespace, and named imports"},
      {"imports", "static import_statement string sources"},
      {"method-calls", "member call property names without receiver identity"},
      {"module-reexports", "static ESM star and CommonJS require routes"},
      {"module-surface-unknowns",
       "dynamic or conditionally mutated CommonJS surfaces"},
      {"source-metric-contributors",
       "exact TypeScript or TSX branch and control syntax inside named "
       "functions and methods"},
      {"source-metrics",
       "versioned decision, cyclomatic, and control-nesting summaries for "
       "named functions and methods"},
      {"symbols", "named classes, functions, methods, variable-bound function "
                  "values, and statically proven descriptor-helper members"}};
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
  static const AbTreeSitterDescriptor typescript = {
      "archbird-tree-sitter-typescript",
      "5",
      "archbird-tree-sitter-typescript-v5;runtime=0.26.9;grammar=0.23.2;abi=14",
      "tree-sitter-0.26.9;tree-sitter-typescript-0.23.2;grammar-abi-14",
      "typescript",
      tree_sitter_typescript,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      ab_tree_sitter_visit_ecmascript,
      NULL,
      NULL,
      "source.control-flow.typescript-v1",
      ab_tree_sitter_classify_ecmascript_metric,
      ab_tree_sitter_ecmascript_callable_boundary};
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
  static const AbTreeSitterDescriptor typescript_react = {
      "archbird-tree-sitter-tsx",
      "5",
      "archbird-tree-sitter-tsx-v5;runtime=0.26.9;grammar=0.23.2;abi=14",
      "tree-sitter-0.26.9;tree-sitter-tsx-0.23.2;grammar-abi-14",
      "tsx",
      tree_sitter_tsx,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      ab_tree_sitter_visit_ecmascript,
      NULL,
      NULL,
      "source.control-flow.tsx-v1",
      ab_tree_sitter_classify_ecmascript_metric,
      ab_tree_sitter_ecmascript_callable_boundary};
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
  if (tsx)
    return ab_scan_tree_sitter_ecmascript_file(
        engine, manifest, config, file, source, source_length,
        source_manifest_sha256, implementation_sha256, &typescript_react,
        out_bundle);
#else
  (void)tsx;
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
  return ab_scan_tree_sitter_ecmascript_file(
      engine, manifest, config, file, source, source_length,
      source_manifest_sha256, implementation_sha256, &typescript, out_bundle);
#else
  return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                            "TypeScript syntax pack is unavailable");
#endif
}
