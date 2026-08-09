#ifndef ARCHBIRD_TREE_SITTER_SCANNER_ABI_COMPAT_H
#define ARCHBIRD_TREE_SITTER_SCANNER_ABI_COMPAT_H

/*
 * Generated parsers store these callbacks as `void *(*)(void)`, but the
 * pinned upstream scanner sources define them with old-style empty parameter
 * lists.  A visible prototype gives each definition the required composite
 * function type without modifying the pinned submodules.
 */
void *tree_sitter_cpp_external_scanner_create(void);
void *tree_sitter_javascript_external_scanner_create(void);
void *tree_sitter_python_external_scanner_create(void);
void *tree_sitter_r_external_scanner_create(void);
void *tree_sitter_tsx_external_scanner_create(void);
void *tree_sitter_typescript_external_scanner_create(void);

#endif
