#ifndef ARCHBIRD_RENDER_INTERNAL_H
#define ARCHBIRD_RENDER_INTERNAL_H

#include "archbird_internal.h"

typedef struct AbBuffer {
  ArchbirdEngine *engine;
  uint8_t *data;
  size_t length;
  size_t capacity;
} AbBuffer;

void ab_buffer_init(AbBuffer *buffer, ArchbirdEngine *engine);
void ab_buffer_free(AbBuffer *buffer);
ArchbirdStatus ab_buffer_append(AbBuffer *buffer, const void *bytes,
                                size_t length);
ArchbirdStatus ab_buffer_literal(AbBuffer *buffer, const char *literal);
ArchbirdStatus ab_buffer_json_string(AbBuffer *buffer, const char *value,
                                     size_t length);
ArchbirdStatus ab_buffer_u64(AbBuffer *buffer, uint64_t value);

#endif
