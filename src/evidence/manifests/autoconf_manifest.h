#ifndef ARCHBIRD_AUTOCONF_MANIFEST_H
#define ARCHBIRD_AUTOCONF_MANIFEST_H

#include "model.h"

typedef struct AbAutoconfMetadata {
  AbString package;
  AbString version;
  AbStringArray files;
  AbStringArray headers;
  AbStringArray subdirectories;
  int has_output;
} AbAutoconfMetadata;

/* Extract literal AC_INIT and config.status metadata with bounded M4-aware
 * scanning. This is a build-description reader, not an M4 evaluator. */
ArchbirdStatus ab_autoconf_metadata(ArchbirdEngine *engine,
                                    const uint8_t *source, size_t length,
                                    AbAutoconfMetadata *out);
void ab_autoconf_metadata_free(ArchbirdEngine *engine,
                               AbAutoconfMetadata *metadata);

#endif
