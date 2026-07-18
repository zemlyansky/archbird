#ifndef ARCHBIRD_PYTHON_MANIFEST_H
#define ARCHBIRD_PYTHON_MANIFEST_H

#include "model.h"

typedef struct AbPyprojectMetadata {
  AbString name;
  AbString version;
  AbString module;
} AbPyprojectMetadata;

/* Extract only explicit, single-line PEP 621 and Flit identities.  This is a
 * bounded metadata reader, not a TOML validator: unsupported value forms stay
 * absent instead of being guessed. */
ArchbirdStatus ab_pyproject_metadata(ArchbirdEngine *engine,
                                     const uint8_t *text, size_t length,
                                     AbPyprojectMetadata *out);
void ab_pyproject_metadata_free(ArchbirdEngine *engine,
                                AbPyprojectMetadata *metadata);

#endif
