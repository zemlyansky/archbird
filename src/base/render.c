#include "render_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ab_buffer_init(AbBuffer *buffer, ArchbirdEngine *engine) {
  memset(buffer, 0, sizeof(*buffer));
  buffer->engine = engine;
}

void ab_buffer_free(AbBuffer *buffer) {
  ab_free(buffer->engine, buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

ArchbirdStatus ab_buffer_append(AbBuffer *buffer, const void *bytes,
                                size_t length) {
  size_t required;
  uint8_t *resized;
  if (length == 0)
    return ARCHBIRD_OK;
  if (length > SIZE_MAX - buffer->length - 1)
    return archbird_error_set(buffer->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "rendered artifact is too large");
  required = buffer->length + length + 1;
  if (required > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 1024;
    while (capacity < required) {
      if (capacity > SIZE_MAX / 2) {
        capacity = required;
        break;
      }
      capacity *= 2;
    }
    resized = (uint8_t *)ab_realloc(buffer->engine, buffer->data, capacity);
    if (!resized)
      return archbird_error_set(buffer->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory rendering artifact");
    buffer->data = resized;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, bytes, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_buffer_literal(AbBuffer *buffer, const char *literal) {
  return ab_buffer_append(buffer, literal, strlen(literal));
}

ArchbirdStatus ab_buffer_json_string(AbBuffer *buffer, const char *value,
                                     size_t length) {
  static const char hex[] = "0123456789abcdef";
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "\"");
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; index < length; index++) {
    unsigned char byte = (unsigned char)value[index];
    const char *escape = NULL;
    switch (byte) {
    case '"':
      escape = "\\\"";
      break;
    case '\\':
      escape = "\\\\";
      break;
    case '\b':
      escape = "\\b";
      break;
    case '\f':
      escape = "\\f";
      break;
    case '\n':
      escape = "\\n";
      break;
    case '\r':
      escape = "\\r";
      break;
    case '\t':
      escape = "\\t";
      break;
    default:
      break;
    }
    if (escape) {
      status = ab_buffer_literal(buffer, escape);
    } else if (byte < 0x20) {
      char encoded[6] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0xf]};
      status = ab_buffer_append(buffer, encoded, sizeof(encoded));
    } else {
      status = ab_buffer_append(buffer, &value[index], 1);
    }
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ab_buffer_literal(buffer, "\"");
}

ArchbirdStatus ab_buffer_u64(AbBuffer *buffer, uint64_t value) {
  char bytes[32];
  int length = snprintf(bytes, sizeof(bytes), "%" PRIu64, value);
  if (length < 0 || (size_t)length >= sizeof(bytes))
    return archbird_error_set(buffer->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "failed to render integer");
  return ab_buffer_append(buffer, bytes, (size_t)length);
}
