#ifndef ARCHBIRD_BASE64_H
#define ARCHBIRD_BASE64_H

#include "render_internal.h"

ArchbirdStatus ab_base64_encode(AbBuffer *buffer, const uint8_t *bytes,
                                size_t length);

ArchbirdStatus ab_base64_decode(ArchbirdEngine *engine, const char *text,
                                size_t length, uint8_t **out_bytes,
                                size_t *out_length);

#endif
