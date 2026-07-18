#ifndef ARCHBIRD_SCIP_PROTOBUF_H
#define ARCHBIRD_SCIP_PROTOBUF_H

#include "archbird_internal.h"

typedef struct AbPbCursor {
  const uint8_t *bytes;
  size_t length;
  size_t offset;
} AbPbCursor;

typedef struct AbPbField {
  uint32_t number;
  uint8_t wire;
  uint64_t integer;
  const uint8_t *bytes;
  size_t length;
} AbPbField;

void ab_pb_cursor_init(AbPbCursor *cursor, const uint8_t *bytes, size_t length);

ArchbirdStatus ab_pb_next(ArchbirdEngine *engine, AbPbCursor *cursor,
                          AbPbField *out, int *has_field);

ArchbirdStatus ab_pb_packed_i32(ArchbirdEngine *engine, const AbPbField *field,
                                uint32_t *values, size_t capacity,
                                size_t *inout_count);

#endif
