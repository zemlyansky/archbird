#include "python_manifest.h"

#include "archbird_internal.h"

#include <string.h>

static void trim(const uint8_t *text, size_t *start, size_t *end) {
  while (*start < *end &&
         (text[*start] == ' ' || text[*start] == '\t' || text[*start] == '\r'))
    (*start)++;
  while (*end > *start && (text[*end - 1] == ' ' || text[*end - 1] == '\t' ||
                           text[*end - 1] == '\r'))
    (*end)--;
}

static int bytes_equal(const uint8_t *text, size_t start, size_t end,
                       const char *literal) {
  size_t length = strlen(literal);
  return end - start == length && !memcmp(text + start, literal, length);
}

static int section_equal(const AbString *section, const char *literal) {
  size_t length = strlen(literal);
  return section->length == length &&
         (!length || !memcmp(section->data, literal, length));
}

static int trailing_comment(const uint8_t *text, size_t index, size_t end) {
  while (index < end &&
         (text[index] == ' ' || text[index] == '\t' || text[index] == '\r'))
    index++;
  return index == end || text[index] == '#';
}

static ArchbirdStatus parse_string(ArchbirdEngine *engine, const uint8_t *text,
                                   size_t start, size_t end, AbString *out) {
  uint8_t quote;
  char *decoded;
  size_t index;
  size_t write = 0;
  ArchbirdStatus status;
  if (out->length || start >= end ||
      (text[start] != '\'' && text[start] != '"'))
    return ARCHBIRD_OK;
  quote = text[start++];
  if (start + 1 < end && text[start] == quote && text[start + 1] == quote)
    return ARCHBIRD_OK;
  decoded = (char *)ab_malloc(engine, end - start + 1);
  if (!decoded)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = start; index < end; index++) {
    uint8_t value = text[index];
    if (value == quote) {
      if (!trailing_comment(text, index + 1, end)) {
        ab_free(engine, decoded);
        return ARCHBIRD_OK;
      }
      decoded[write] = '\0';
      status = ab_string_copy(engine, out, decoded, write);
      ab_free(engine, decoded);
      return status;
    }
    if (quote == '\'' || value != '\\') {
      decoded[write++] = (char)value;
      continue;
    }
    if (++index >= end) {
      ab_free(engine, decoded);
      return ARCHBIRD_OK;
    }
    value = text[index];
    switch (value) {
    case 'b':
      decoded[write++] = '\b';
      break;
    case 't':
      decoded[write++] = '\t';
      break;
    case 'n':
      decoded[write++] = '\n';
      break;
    case 'f':
      decoded[write++] = '\f';
      break;
    case 'r':
      decoded[write++] = '\r';
      break;
    case '"':
    case '\\':
      decoded[write++] = (char)value;
      break;
    default:
      /* Unicode escapes and multiline continuations need a complete TOML
       * decoder.  Leave the field absent rather than mis-decoding identity. */
      ab_free(engine, decoded);
      return ARCHBIRD_OK;
    }
  }
  ab_free(engine, decoded);
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_pyproject_metadata(ArchbirdEngine *engine,
                                     const uint8_t *text, size_t length,
                                     AbPyprojectMetadata *out) {
  size_t line_start = 0;
  AbString section = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || (!text && length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  while (status == ARCHBIRD_OK && line_start <= length) {
    size_t line_end = line_start;
    size_t start;
    size_t end;
    size_t equal;
    while (line_end < length && text[line_end] != '\n')
      line_end++;
    start = line_start;
    end = line_end;
    trim(text, &start, &end);
    if (start < end && text[start] != '#') {
      if (end > start + 1 && text[start] == '[' && text[end - 1] == ']' &&
          (end == start + 2 || text[start + 1] != '[')) {
        size_t section_start = start + 1;
        size_t section_end = end - 1;
        trim(text, &section_start, &section_end);
        ab_string_free(engine, &section);
        status =
            ab_string_copy(engine, &section, (const char *)text + section_start,
                           section_end - section_start);
      } else {
        equal = start;
        while (equal < end && text[equal] != '=')
          equal++;
        if (equal < end) {
          size_t key_start = start;
          size_t key_end = equal;
          size_t value_start = equal + 1;
          size_t value_end = end;
          AbString *target = NULL;
          trim(text, &key_start, &key_end);
          trim(text, &value_start, &value_end);
          if (section_equal(&section, "project") &&
              bytes_equal(text, key_start, key_end, "name"))
            target = &out->name;
          else if (section_equal(&section, "project") &&
                   bytes_equal(text, key_start, key_end, "version"))
            target = &out->version;
          else if (section_equal(&section, "tool.flit.module") &&
                   bytes_equal(text, key_start, key_end, "name"))
            target = &out->module;
          if (target)
            status = parse_string(engine, text, value_start, value_end, target);
        }
      }
    }
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  ab_string_free(engine, &section);
  if (status != ARCHBIRD_OK)
    ab_pyproject_metadata_free(engine, out);
  return status;
}

void ab_pyproject_metadata_free(ArchbirdEngine *engine,
                                AbPyprojectMetadata *metadata) {
  if (!metadata)
    return;
  ab_string_free(engine, &metadata->name);
  ab_string_free(engine, &metadata->version);
  ab_string_free(engine, &metadata->module);
  memset(metadata, 0, sizeof(*metadata));
}
