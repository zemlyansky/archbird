#include "utf8.h"

size_t ab_utf8_scalar_length(const uint8_t *source, size_t length,
                             size_t offset) {
  uint8_t first = source[offset];
  size_t width;
  uint32_t value;
  size_t index;
  if (first < 0x80)
    return 1;
  if (first >= 0xc2 && first <= 0xdf) {
    width = 2;
    value = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    width = 3;
    value = first & 0x0f;
  } else if (first >= 0xf0 && first <= 0xf4) {
    width = 4;
    value = first & 0x07;
  } else {
    return 0;
  }
  if (width > length - offset)
    return 0;
  for (index = 1; index < width; index++) {
    uint8_t next = source[offset + index];
    if ((next & 0xc0) != 0x80)
      return 0;
    value = (value << 6) | (uint32_t)(next & 0x3f);
  }
  if ((width == 2 && value < 0x80) || (width == 3 && value < 0x800) ||
      (width == 4 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff))
    return 0;
  return width;
}

ArchbirdStatus ab_utf8_validate(ArchbirdEngine *engine, const uint8_t *source,
                                size_t source_length) {
  size_t index = 0;
  if (!engine || (!source && source_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  while (index < source_length) {
    size_t width = ab_utf8_scalar_length(source, source_length, index);
    if (!width)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, index,
                                "source is not valid UTF-8");
    index += width;
  }
  return ARCHBIRD_OK;
}
