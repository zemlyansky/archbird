#include "evidence/manifests/pyproject_manifest.h"

#include "base/archbird_internal.h"
#include "base/utf8.h"
#include "evidence/manifests/python_package_metadata.h"

#include <stdlib.h>
#include <string.h>

typedef struct ParsedStringArray {
  AbString *items;
  size_t count;
  size_t capacity;
} ParsedStringArray;

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

static void parsed_array_free(ArchbirdEngine *engine,
                              ParsedStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static void skip_array_space(const uint8_t *text, size_t length,
                             size_t *index) {
  while (*index < length) {
    uint8_t value = text[*index];
    if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
      (*index)++;
      continue;
    }
    if (value != '#')
      break;
    while (*index < length && text[*index] != '\n')
      (*index)++;
  }
}

static ArchbirdStatus append_parsed_string(ArchbirdEngine *engine,
                                           ParsedStringArray *array,
                                           const char *data, size_t length) {
  AbString *resized;
  ArchbirdStatus status;
  if (array->count == array->capacity) {
    size_t next = array->capacity ? array->capacity * 2 : 8;
    if (next < array->capacity || next > SIZE_MAX / sizeof(*array->items))
      return ARCHBIRD_LIMIT_EXCEEDED;
    resized = (AbString *)ab_realloc(engine, array->items,
                                     next * sizeof(*array->items));
    if (!resized)
      return ARCHBIRD_OUT_OF_MEMORY;
    array->items = resized;
    array->capacity = next;
  }
  memset(&array->items[array->count], 0, sizeof(*array->items));
  status = ab_string_copy(engine, &array->items[array->count], data, length);
  if (status == ARCHBIRD_OK)
    array->count++;
  return status;
}

static ArchbirdStatus parse_array_quoted_string(ArchbirdEngine *engine,
                                                const uint8_t *text,
                                                size_t length, size_t *index,
                                                ParsedStringArray *array,
                                                int *supported) {
  uint8_t quote = text[(*index)++];
  char *decoded;
  size_t write = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (*index + 1 < length && text[*index] == quote &&
      text[*index + 1] == quote) {
    *supported = 0;
    return ARCHBIRD_OK;
  }
  decoded = (char *)ab_malloc(engine, length - *index + 1);
  if (!decoded)
    return ARCHBIRD_OUT_OF_MEMORY;
  while (*index < length) {
    uint8_t value = text[(*index)++];
    if (value == quote) {
      status = append_parsed_string(engine, array, decoded, write);
      ab_free(engine, decoded);
      return status;
    }
    if (value == '\n') {
      *supported = 0;
      break;
    }
    if (quote == '\'' || value != '\\') {
      decoded[write++] = (char)value;
      continue;
    }
    if (*index >= length) {
      *supported = 0;
      break;
    }
    value = text[(*index)++];
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
      *supported = 0;
      break;
    }
    if (!*supported)
      break;
  }
  ab_free(engine, decoded);
  if (*supported)
    *supported = 0;
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_string_array(ArchbirdEngine *engine,
                                         const uint8_t *text, size_t length,
                                         size_t start, ParsedStringArray *out,
                                         size_t *consumed, int *supported) {
  size_t index = start;
  int expect_value = 1;
  int closed = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  *supported = 1;
  skip_array_space(text, length, &index);
  if (index >= length || text[index++] != '[') {
    *supported = 0;
    *consumed = index;
    return ARCHBIRD_OK;
  }
  while (status == ARCHBIRD_OK && *supported) {
    skip_array_space(text, length, &index);
    if (index >= length) {
      *supported = 0;
      break;
    }
    if (text[index] == ']') {
      index++;
      closed = 1;
      break;
    }
    if (!expect_value || (text[index] != '\'' && text[index] != '"')) {
      *supported = 0;
      break;
    }
    status =
        parse_array_quoted_string(engine, text, length, &index, out, supported);
    if (status != ARCHBIRD_OK || !*supported)
      break;
    skip_array_space(text, length, &index);
    if (index < length && text[index] == ',') {
      index++;
      expect_value = 1;
    } else {
      expect_value = 0;
    }
  }
  if (status == ARCHBIRD_OK && *supported && closed) {
    while (index < length &&
           (text[index] == ' ' || text[index] == '\t' || text[index] == '\r'))
      index++;
    if (index < length && text[index] == '#')
      while (index < length && text[index] != '\n')
        index++;
    if (index < length && text[index] != '\n')
      *supported = 0;
  } else if (status == ARCHBIRD_OK && *supported) {
    *supported = 0;
  }
  *consumed = index;
  if (status != ARCHBIRD_OK || !*supported)
    parsed_array_free(engine, out);
  return status;
}

static int nonblank_string(const AbString *value) {
  size_t index;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static ArchbirdStatus merge_module_hint(ArchbirdEngine *engine,
                                        AbPyprojectMetadata *metadata,
                                        const char *candidate,
                                        size_t candidate_length,
                                        int supported) {
  return ab_python_module_hint_merge(engine, &metadata->package.module,
                                     &metadata->package.module_hints_present,
                                     &metadata->package.module_hints_supported,
                                     candidate, candidate_length, supported);
}

static ArchbirdStatus merge_array_module_hint(ArchbirdEngine *engine,
                                              AbPyprojectMetadata *metadata,
                                              const ParsedStringArray *values,
                                              int patterns, int supported) {
  const char *candidate = NULL;
  size_t candidate_length = 0;
  size_t index;
  if (!supported || !values->count)
    return merge_module_hint(engine, metadata, NULL, 0, 0);
  for (index = 0; supported && index < values->count; index++) {
    const char *current;
    size_t current_length;
    if (!ab_python_module_candidate(&values->items[index], patterns, &current,
                                    &current_length)) {
      supported = 0;
      break;
    }
    if (!candidate) {
      candidate = current;
      candidate_length = current_length;
    } else if (candidate_length != current_length ||
               memcmp(candidate, current, current_length)) {
      supported = 0;
    }
  }
  return merge_module_hint(engine, metadata, candidate, candidate_length,
                           supported);
}

static ArchbirdStatus merge_source_root(ArchbirdEngine *engine,
                                        AbPyprojectMetadata *metadata,
                                        const ParsedStringArray *values,
                                        int supported) {
  const AbString *candidate = values->count == 1 ? &values->items[0] : NULL;
  return ab_python_source_root_merge(engine, &metadata->package.source_root,
                                     &metadata->package.source_root_present,
                                     &metadata->package.source_root_supported,
                                     candidate, supported && candidate != NULL);
}

static ArchbirdStatus
copy_workspace_patterns(ArchbirdEngine *engine, AbString **out_items,
                        size_t *out_count, int *out_present, int *out_supported,
                        ParsedStringArray *values, int supported) {
  size_t index;
  if (*out_present) {
    for (index = 0; index < *out_count; index++)
      ab_string_free(engine, &(*out_items)[index]);
    ab_free(engine, *out_items);
    *out_items = NULL;
    *out_count = 0;
    *out_supported = 0;
    return ARCHBIRD_OK;
  }
  *out_present = 1;
  *out_supported = supported;
  if (!supported)
    return ARCHBIRD_OK;
  *out_items = values->items;
  *out_count = values->count;
  memset(values, 0, sizeof(*values));
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
  ab_python_package_metadata_init(&out->package);
  out->workspace_members_supported = 1;
  out->workspace_excludes_supported = 1;
  status = ab_utf8_validate(engine, text, length);
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
          int module_hint = 0;
          int array_target = 0;
          int array_kind = 0;
          trim(text, &key_start, &key_end);
          trim(text, &value_start, &value_end);
          if (section_equal(&section, "project") &&
              bytes_equal(text, key_start, key_end, "name"))
            target = &out->package.name;
          else if (section_equal(&section, "project") &&
                   bytes_equal(text, key_start, key_end, "version"))
            target = &out->package.version;
          else if (section_equal(&section, "tool.flit.module") &&
                   bytes_equal(text, key_start, key_end, "name"))
            module_hint = 1;
          else if (section_equal(&section, "tool.setuptools") &&
                   bytes_equal(text, key_start, key_end, "packages")) {
            array_target = 1;
            array_kind = 1;
          } else if (section_equal(&section, "tool.setuptools.packages.find") &&
                     bytes_equal(text, key_start, key_end, "include")) {
            array_target = 1;
            array_kind = 2;
          } else if (section_equal(&section, "tool.setuptools.packages.find") &&
                     bytes_equal(text, key_start, key_end, "where")) {
            array_target = 1;
            array_kind = 3;
          } else if (section_equal(&section, "tool.uv.workspace") &&
                     bytes_equal(text, key_start, key_end, "members")) {
            array_target = 1;
            array_kind = 4;
          } else if (section_equal(&section, "tool.uv.workspace") &&
                     bytes_equal(text, key_start, key_end, "exclude")) {
            array_target = 1;
            array_kind = 5;
          }
          if (target)
            status = parse_string(engine, text, value_start, value_end, target);
          else if (module_hint) {
            AbString value = {0};
            status = parse_string(engine, text, value_start, value_end, &value);
            if (status == ARCHBIRD_OK)
              status = merge_module_hint(
                  engine, out, value.data, value.length,
                  value.length &&
                      ab_python_identifier_valid(value.data, value.length));
            ab_string_free(engine, &value);
          } else if (array_target) {
            ParsedStringArray values = {0};
            size_t consumed = value_start;
            int supported = 0;
            status = parse_string_array(engine, text, length, value_start,
                                        &values, &consumed, &supported);
            if (status == ARCHBIRD_OK && array_kind == 1)
              status =
                  merge_array_module_hint(engine, out, &values, 0, supported);
            else if (status == ARCHBIRD_OK && array_kind == 2)
              status =
                  merge_array_module_hint(engine, out, &values, 1, supported);
            else if (status == ARCHBIRD_OK && array_kind == 3)
              status = merge_source_root(engine, out, &values, supported);
            else if (status == ARCHBIRD_OK && array_kind == 4)
              status = copy_workspace_patterns(
                  engine, &out->workspace_members, &out->workspace_member_count,
                  &out->workspace_members_present,
                  &out->workspace_members_supported, &values, supported);
            else if (status == ARCHBIRD_OK && array_kind == 5)
              status = copy_workspace_patterns(
                  engine, &out->workspace_excludes,
                  &out->workspace_exclude_count,
                  &out->workspace_excludes_present,
                  &out->workspace_excludes_supported, &values, supported);
            parsed_array_free(engine, &values);
            while (line_end < length && line_end < consumed)
              line_end++;
            while (line_end < length && text[line_end] != '\n')
              line_end++;
          }
        }
      }
    }
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  ab_string_free(engine, &section);
  if (status == ARCHBIRD_OK && out->package.name.length &&
      !ab_python_distribution_name_valid(&out->package.name))
    ab_string_free(engine, &out->package.name);
  if (status == ARCHBIRD_OK && out->package.version.length &&
      !nonblank_string(&out->package.version))
    ab_string_free(engine, &out->package.version);
  if (status != ARCHBIRD_OK)
    ab_pyproject_metadata_free(engine, out);
  return status;
}

void ab_pyproject_metadata_free(ArchbirdEngine *engine,
                                AbPyprojectMetadata *metadata) {
  size_t index;
  if (!metadata)
    return;
  ab_python_package_metadata_free(engine, &metadata->package);
  for (index = 0; index < metadata->workspace_member_count; index++)
    ab_string_free(engine, &metadata->workspace_members[index]);
  ab_free(engine, metadata->workspace_members);
  for (index = 0; index < metadata->workspace_exclude_count; index++)
    ab_string_free(engine, &metadata->workspace_excludes[index]);
  ab_free(engine, metadata->workspace_excludes);
  memset(metadata, 0, sizeof(*metadata));
}
