#include "base64.h"

#include <string.h>

static int decode_digit(unsigned char value) {
  if (value >= 'A' && value <= 'Z')
    return (int)(value - 'A');
  if (value >= 'a' && value <= 'z')
    return (int)(value - 'a') + 26;
  if (value >= '0' && value <= '9')
    return (int)(value - '0') + 52;
  if (value == '+')
    return 62;
  if (value == '/')
    return 63;
  return -1;
}

ArchbirdStatus ab_base64_encode(AbBuffer *buffer, const uint8_t *bytes,
                                size_t length) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t index;
  if (!buffer || (!bytes && length))
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index + 3 <= length; index += 3) {
    char encoded[4];
    uint32_t value = ((uint32_t)bytes[index] << 16) |
                     ((uint32_t)bytes[index + 1] << 8) |
                     (uint32_t)bytes[index + 2];
    encoded[0] = alphabet[(value >> 18) & 63u];
    encoded[1] = alphabet[(value >> 12) & 63u];
    encoded[2] = alphabet[(value >> 6) & 63u];
    encoded[3] = alphabet[value & 63u];
    {
      ArchbirdStatus status =
          ab_buffer_append(buffer, encoded, sizeof(encoded));
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  if (index < length) {
    char encoded[4];
    uint32_t value = (uint32_t)bytes[index] << 16;
    encoded[0] = alphabet[(value >> 18) & 63u];
    if (index + 1 < length) {
      value |= (uint32_t)bytes[index + 1] << 8;
      encoded[1] = alphabet[(value >> 12) & 63u];
      encoded[2] = alphabet[(value >> 6) & 63u];
      encoded[3] = '=';
    } else {
      encoded[1] = alphabet[(value >> 12) & 63u];
      encoded[2] = '=';
      encoded[3] = '=';
    }
    return ab_buffer_append(buffer, encoded, sizeof(encoded));
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_base64_decode(ArchbirdEngine *engine, const char *text,
                                size_t length, uint8_t **out_bytes,
                                size_t *out_length) {
  uint8_t *decoded = NULL;
  size_t groups;
  size_t padding = 0;
  size_t index;
  size_t output = 0;
  if (!engine || (!text && length) || !out_bytes || !out_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_bytes = NULL;
  *out_length = 0;
  if (length == 0)
    return ARCHBIRD_OK;
  if (length % 4 != 0)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "base64 length is not divisible by four");
  if (text[length - 1] == '=')
    padding++;
  if (text[length - 2] == '=')
    padding++;
  groups = length / 4;
  if (groups > (SIZE_MAX - 2) / 3 || groups * 3 < padding)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "decoded base64 is too large");
  decoded = (uint8_t *)ab_malloc(engine, groups * 3 - padding);
  if (!decoded && groups * 3 != padding)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding base64");
  for (index = 0; index < length; index += 4) {
    int a = decode_digit((unsigned char)text[index]);
    int b = decode_digit((unsigned char)text[index + 1]);
    int c = text[index + 2] == '='
                ? -2
                : decode_digit((unsigned char)text[index + 2]);
    int d = text[index + 3] == '='
                ? -2
                : decode_digit((unsigned char)text[index + 3]);
    uint32_t value;
    int final = index + 4 == length;
    if (a < 0 || b < 0 || c == -1 || d == -1 ||
        (!final && (c == -2 || d == -2)) || (c == -2 && d != -2) ||
        (c == -2 && (b & 15) != 0) || (d == -2 && c >= 0 && (c & 3) != 0)) {
      ab_free(engine, decoded);
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "base64 text is not canonical");
    }
    value = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
            (c >= 0 ? (uint32_t)c << 6 : 0u) | (d >= 0 ? (uint32_t)d : 0u);
    decoded[output++] = (uint8_t)(value >> 16);
    if (c >= 0)
      decoded[output++] = (uint8_t)(value >> 8);
    if (d >= 0)
      decoded[output++] = (uint8_t)value;
  }
  *out_bytes = decoded;
  *out_length = output;
  return ARCHBIRD_OK;
}
