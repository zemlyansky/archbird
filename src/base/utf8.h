#ifndef ARCHBIRD_UTF8_H
#define ARCHBIRD_UTF8_H

#include "archbird_internal.h"

size_t ab_utf8_scalar_length(const uint8_t *source, size_t source_length,
                             size_t offset);

ArchbirdStatus ab_utf8_validate(ArchbirdEngine *engine, const uint8_t *source,
                                size_t source_length);

#endif
