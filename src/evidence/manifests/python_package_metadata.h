#ifndef ARCHBIRD_PYTHON_PACKAGE_METADATA_H
#define ARCHBIRD_PYTHON_PACKAGE_METADATA_H

#include "base/model.h"

typedef enum AbPythonSourceShape {
  /* Accept a module file, a regular package, or a PEP 420 namespace. This is
   * the conservative default for PEP 621 metadata and automatic discovery. */
  AB_PYTHON_SOURCE_ANY,
  /* An explicit packages/find: result must name a package with __init__.py. */
  AB_PYTHON_SOURCE_PACKAGE,
  /* An explicit py_modules value must name a top-level module file. */
  AB_PYTHON_SOURCE_MODULE
} AbPythonSourceShape;

typedef struct AbPythonPackageMetadata {
  AbString name;
  AbString version;
  AbString module;
  AbString source_root;
  int module_hints_present;
  int module_hints_supported;
  int source_root_present;
  int source_root_supported;
  AbPythonSourceShape source_shape;
  /* Malformed or ambiguous metadata. Identity is omitted. */
  int identity_invalid;
  /* Valid input that this reader will not evaluate: interpolation, attr:,
   * file:, or other dynamic name evidence. Identity is omitted. */
  int identity_unsupported;
} AbPythonPackageMetadata;

void ab_python_package_metadata_init(AbPythonPackageMetadata *metadata);
void ab_python_package_metadata_free(ArchbirdEngine *engine,
                                     AbPythonPackageMetadata *metadata);

int ab_python_identifier_valid(const char *data, size_t length);
int ab_python_distribution_name_valid(const AbString *value);

/* Return the importable top-level package named by a literal module or
 * package-discovery pattern. The returned view borrows value. */
int ab_python_module_candidate(const AbString *value, int pattern,
                               const char **data, size_t *length);

int ab_python_source_root_valid(const AbString *value);

/* Merge independent declarative layout hints. A missing, unsupported, or
 * conflicting hint makes the complete hint family unsupported instead of
 * selecting an arbitrary witness. */
ArchbirdStatus
ab_python_module_hint_merge(ArchbirdEngine *engine, AbString *module,
                            int *present, int *supported, const char *candidate,
                            size_t candidate_length, int candidate_supported);

ArchbirdStatus ab_python_source_root_merge(ArchbirdEngine *engine,
                                           AbString *source_root, int *present,
                                           int *supported,
                                           const AbString *candidate,
                                           int candidate_supported);

#endif
