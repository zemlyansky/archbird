#ifndef ARCHBIRD_PYPROJECT_MANIFEST_H
#define ARCHBIRD_PYPROJECT_MANIFEST_H

#include "evidence/manifests/python_package_metadata.h"

typedef struct AbPyprojectMetadata {
  AbPythonPackageMetadata package;
  AbString *workspace_members;
  size_t workspace_member_count;
  AbString *workspace_excludes;
  size_t workspace_exclude_count;
  int workspace_members_present;
  int workspace_members_supported;
  int workspace_excludes_present;
  int workspace_excludes_supported;
} AbPyprojectMetadata;

/* Extract explicit PEP 621 identity, agreeing Flit/setuptools module hints,
 * one unambiguous setuptools find root, and uv workspace member/exclude
 * patterns. This is a bounded metadata reader, not a TOML validator:
 * unsupported or conflicting layout forms are marked unsupported instead of
 * being guessed. Quoted string arrays may span lines. */
ArchbirdStatus ab_pyproject_metadata(ArchbirdEngine *engine,
                                     const uint8_t *text, size_t length,
                                     AbPyprojectMetadata *out);
void ab_pyproject_metadata_free(ArchbirdEngine *engine,
                                AbPyprojectMetadata *metadata);

#endif
