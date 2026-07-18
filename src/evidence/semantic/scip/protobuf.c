#include "protobuf.h"

#include <limits.h>
#include <string.h>

#define AB_SCIP_MAX_FIELD_BYTES ((size_t)64 * 1024 * 1024)

static ArchbirdStatus read_varint(ArchbirdEngine *engine, AbPbCursor *cursor,
                                  uint64_t *out) {
  uint64_t value = 0;
  unsigned shift = 0;
  size_t index;
  for (index = 0; index < 10; index++) {
    uint8_t byte;
    if (cursor->offset >= cursor->length)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
                                "SCIP protobuf contains a truncated varint");
    byte = cursor->bytes[cursor->offset++];
    if (index == 9 && byte > 1)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                cursor->offset - 1,
                                "SCIP protobuf varint exceeds 64 bits");
    value |= (uint64_t)(byte & 0x7fu) << shift;
    if (!(byte & 0x80u)) {
      *out = value;
      return ARCHBIRD_OK;
    }
    shift += 7;
  }
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
                            "SCIP protobuf varint exceeds 10 bytes");
}

void ab_pb_cursor_init(AbPbCursor *cursor, const uint8_t *bytes,
                       size_t length) {
  cursor->bytes = bytes;
  cursor->length = length;
  cursor->offset = 0;
}

ArchbirdStatus ab_pb_next(ArchbirdEngine *engine, AbPbCursor *cursor,
                          AbPbField *out, int *has_field) {
  uint64_t key;
  ArchbirdStatus status;
  if (!engine || !cursor || !out || !has_field ||
      (!cursor->bytes && cursor->length))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  *has_field = 0;
  if (cursor->offset == cursor->length)
    return ARCHBIRD_OK;
  status = read_varint(engine, cursor, &key);
  if (status != ARCHBIRD_OK)
    return status;
  out->number = (uint32_t)(key >> 3);
  out->wire = (uint8_t)(key & 7u);
  if (out->number == 0 || (uint64_t)out->number != key >> 3)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
                              "SCIP protobuf contains an invalid field number");
  switch (out->wire) {
  case 0:
    status = read_varint(engine, cursor, &out->integer);
    break;
  case 1:
    if (cursor->length - cursor->offset < 8)
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
          "SCIP protobuf contains a truncated fixed64 field");
    else {
      out->bytes = cursor->bytes + cursor->offset;
      out->length = 8;
      cursor->offset += 8;
      status = ARCHBIRD_OK;
    }
    break;
  case 2: {
    uint64_t length;
    status = read_varint(engine, cursor, &length);
    if (status != ARCHBIRD_OK)
      break;
    if (length > AB_SCIP_MAX_FIELD_BYTES || length > SIZE_MAX ||
        (size_t)length > cursor->length - cursor->offset) {
      status = archbird_error_set(
          engine,
          length > AB_SCIP_MAX_FIELD_BYTES ? ARCHBIRD_LIMIT_EXCEEDED
                                           : ARCHBIRD_INVALID_SCHEMA,
          cursor->offset, "SCIP protobuf field length is invalid or too large");
      break;
    }
    out->bytes = cursor->bytes + cursor->offset;
    out->length = (size_t)length;
    cursor->offset += (size_t)length;
    break;
  }
  case 5:
    if (cursor->length - cursor->offset < 4)
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
          "SCIP protobuf contains a truncated fixed32 field");
    else {
      out->bytes = cursor->bytes + cursor->offset;
      out->length = 4;
      cursor->offset += 4;
      status = ARCHBIRD_OK;
    }
    break;
  default:
    status = archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, cursor->offset,
        "SCIP protobuf uses unsupported group wire encoding");
    break;
  }
  if (status == ARCHBIRD_OK)
    *has_field = 1;
  return status;
}

ArchbirdStatus ab_pb_packed_i32(ArchbirdEngine *engine, const AbPbField *field,
                                uint32_t *values, size_t capacity,
                                size_t *inout_count) {
  AbPbCursor cursor;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !field || !values || !inout_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (field->wire == 0) {
    if (*inout_count >= capacity || field->integer > INT32_MAX)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "SCIP range contains an invalid int32 value");
    values[(*inout_count)++] = (uint32_t)field->integer;
    return ARCHBIRD_OK;
  }
  if (field->wire != 2)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "SCIP packed range has an invalid wire type");
  ab_pb_cursor_init(&cursor, field->bytes, field->length);
  while (status == ARCHBIRD_OK && cursor.offset < cursor.length) {
    uint64_t value;
    if (*inout_count >= capacity)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "SCIP range has too many coordinates");
    status = read_varint(engine, &cursor, &value);
    if (status == ARCHBIRD_OK && value > INT32_MAX)
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "SCIP range contains a negative or overflowing coordinate");
    if (status == ARCHBIRD_OK)
      values[(*inout_count)++] = (uint32_t)value;
  }
  return status;
}
