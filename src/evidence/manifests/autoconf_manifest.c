#include "manifests/autoconf_manifest.h"

#include "archbird_internal.h"
#include "utf8.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct SourceSpan {
  size_t start;
  size_t end;
} SourceSpan;

typedef struct Invocation {
  SourceSpan arguments[5];
  size_t argument_count;
  size_t end;
} Invocation;

typedef struct Definition {
  AbString name;
  AbString value;
} Definition;

enum {
  MAX_LITERAL_EXPANSIONS = 8,
  MAX_OUTER_QUOTES = 8,
  MAX_SIMPLE_DEFINITIONS = 256
};

static int token_equals(const uint8_t *source, SourceSpan token,
                        const char *literal) {
  size_t length = strlen(literal);
  return token.end - token.start == length &&
         !memcmp(source + token.start, literal, length);
}

static void trim_span(const uint8_t *source, SourceSpan *span) {
  while (span->start < span->end && isspace((unsigned char)source[span->start]))
    span->start++;
  while (span->end > span->start &&
         isspace((unsigned char)source[span->end - 1]))
    span->end--;
}

static int identifier_start(uint8_t byte) {
  return isalpha((unsigned char)byte) || byte == '_';
}

static int identifier_continue(uint8_t byte) {
  return isalnum((unsigned char)byte) || byte == '_';
}

static int parse_invocation(const uint8_t *source, size_t length, size_t open,
                            Invocation *out) {
  size_t index = open + 1;
  size_t argument_start = index;
  size_t parentheses = 1;
  size_t brackets = 0;
  memset(out, 0, sizeof(*out));
  while (index < length) {
    uint8_t byte = source[index];
    if (byte == '[') {
      brackets++;
      index++;
      continue;
    }
    if (byte == ']' && brackets) {
      brackets--;
      index++;
      continue;
    }
    if (!brackets && byte == '(') {
      parentheses++;
      index++;
      continue;
    }
    if (!brackets && byte == ')') {
      parentheses--;
      if (!parentheses) {
        if (out->argument_count <
            sizeof(out->arguments) / sizeof(out->arguments[0])) {
          out->arguments[out->argument_count].start = argument_start;
          out->arguments[out->argument_count].end = index;
          trim_span(source, &out->arguments[out->argument_count]);
          out->argument_count++;
        }
        out->end = index + 1;
        return 1;
      }
      index++;
      continue;
    }
    if (!brackets && parentheses == 1 && byte == ',') {
      if (out->argument_count <
          sizeof(out->arguments) / sizeof(out->arguments[0])) {
        out->arguments[out->argument_count].start = argument_start;
        out->arguments[out->argument_count].end = index;
        trim_span(source, &out->arguments[out->argument_count]);
        out->argument_count++;
      }
      argument_start = index + 1;
    }
    index++;
  }
  return 0;
}

static int outer_brackets(const uint8_t *source, SourceSpan span) {
  size_t depth = 0;
  size_t index;
  if (span.end - span.start < 2 || source[span.start] != '[' ||
      source[span.end - 1] != ']')
    return 0;
  for (index = span.start; index < span.end; index++) {
    if (source[index] == '[')
      depth++;
    else if (source[index] == ']') {
      if (!depth)
        return 0;
      depth--;
      if (!depth && index != span.end - 1)
        return 0;
    }
  }
  return depth == 0;
}

static int strip_outer_brackets(const uint8_t *source, SourceSpan *span) {
  size_t depth;
  for (depth = 0; depth < MAX_OUTER_QUOTES; depth++) {
    if (!outer_brackets(source, *span))
      return 1;
    span->start++;
    span->end--;
    trim_span(source, span);
  }
  return !outer_brackets(source, *span);
}

static int unwrap_span(const uint8_t *source, size_t length, SourceSpan input,
                       SourceSpan *out) {
  SourceSpan span = input;
  size_t depth;
  trim_span(source, &span);
  for (depth = 0; depth < MAX_LITERAL_EXPANSIONS; depth++) {
    size_t token_end;
    size_t open;
    Invocation invocation;
    if (!strip_outer_brackets(source, &span))
      return 0;
    token_end = span.start;
    while (token_end < span.end && identifier_continue(source[token_end]))
      token_end++;
    if (token_end == span.start ||
        !token_equals(source, (SourceSpan){span.start, token_end},
                      "m4_normalize"))
      break;
    open = token_end;
    while (open < span.end && isspace((unsigned char)source[open]))
      open++;
    if (open == span.end || source[open] != '(' ||
        !parse_invocation(source, length, open, &invocation) ||
        invocation.end != span.end || !invocation.argument_count)
      return 0;
    span = invocation.arguments[0];
  }
  if (!strip_outer_brackets(source, &span))
    return 0;
  *out = span;
  return span.start < span.end;
}

static Definition *definition(Definition *definitions, size_t count,
                              const uint8_t *name, size_t length) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (definitions[index].name.length == length &&
        !memcmp(definitions[index].name.data, name, length))
      return &definitions[index];
  }
  return NULL;
}

static int literal_span(const uint8_t *source, size_t length, SourceSpan input,
                        Definition *definitions, size_t definition_count,
                        SourceSpan *out, const uint8_t **out_source,
                        size_t *out_length) {
  SourceSpan span;
  Definition *resolved;
  const uint8_t *literal_source = source;
  size_t literal_length = length;
  size_t depth;
  size_t index;
  if (!unwrap_span(source, length, input, &span))
    return 0;
  for (depth = 0; depth < MAX_LITERAL_EXPANSIONS; depth++) {
    resolved = definition(definitions, definition_count,
                          literal_source + span.start, span.end - span.start);
    if (!resolved)
      break;
    literal_source = (const uint8_t *)resolved->value.data;
    literal_length = resolved->value.length;
    out->start = 0;
    out->end = resolved->value.length;
    span = *out;
  }
  if (definition(definitions, definition_count, literal_source + span.start,
                 span.end - span.start))
    return 0;
  for (index = span.start; index < span.end; index++) {
    uint8_t byte = literal_source[index];
    if (byte == '$' || byte == '`' || byte == '[' || byte == ']' ||
        byte == '(' || byte == ')')
      return 0;
  }
  *out = span;
  *out_source = literal_source;
  *out_length = literal_length;
  return 1;
}

static ArchbirdStatus copy_literal(ArchbirdEngine *engine,
                                   const uint8_t *source, size_t length,
                                   SourceSpan input, Definition *definitions,
                                   size_t definition_count, AbString *out) {
  const uint8_t *literal_source;
  size_t literal_length;
  SourceSpan span;
  if (!literal_span(source, length, input, definitions, definition_count, &span,
                    &literal_source, &literal_length))
    return ARCHBIRD_OK;
  (void)literal_length;
  return ab_string_copy(engine, out, (const char *)literal_source + span.start,
                        span.end - span.start);
}

static int repository_path(const uint8_t *data, size_t length) {
  size_t segment = 0;
  size_t index;
  if (!length || data[0] == '/')
    return 0;
  for (index = 0; index <= length; index++) {
    if (index == length || data[index] == '/') {
      size_t segment_length = index - segment;
      if (!segment_length || (segment_length == 1 && data[segment] == '.') ||
          (segment_length == 2 && data[segment] == '.' &&
           data[segment + 1] == '.'))
        return 0;
      segment = index + 1;
      continue;
    }
    if (!(isalnum((unsigned char)data[index]) || data[index] == '_' ||
          data[index] == '-' || data[index] == '.' || data[index] == '+'))
      return 0;
  }
  return 1;
}

static ArchbirdStatus append_path(ArchbirdEngine *engine, AbStringArray *array,
                                  size_t *capacity, const uint8_t *data,
                                  size_t length) {
  AbString *resized;
  size_t next_capacity;
  if (array->count == engine->options.max_values)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many Autoconf literal paths");
  if (array->count == *capacity) {
    next_capacity = *capacity ? *capacity * 2 : 16;
    if (next_capacity < *capacity || next_capacity > engine->options.max_values)
      next_capacity = engine->options.max_values;
    if (next_capacity <= *capacity ||
        next_capacity > SIZE_MAX / sizeof(*array->items))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many Autoconf literal paths");
    resized = (AbString *)ab_realloc(engine, array->items,
                                     next_capacity * sizeof(*array->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory parsing configure.ac");
    array->items = resized;
    *capacity = next_capacity;
  }
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], (const char *)data,
                     length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus collect_paths(ArchbirdEngine *engine,
                                    const uint8_t *source, size_t length,
                                    SourceSpan input, Definition *definitions,
                                    size_t definition_count, AbStringArray *out,
                                    size_t *capacity) {
  const uint8_t *literal_source;
  size_t literal_length;
  SourceSpan span;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!literal_span(source, length, input, definitions, definition_count, &span,
                    &literal_source, &literal_length))
    return ARCHBIRD_OK;
  (void)literal_length;
  index = span.start;
  while (status == ARCHBIRD_OK && index < span.end) {
    size_t start;
    size_t end;
    size_t colon;
    while (index < span.end && isspace((unsigned char)literal_source[index]))
      index++;
    start = index;
    while (index < span.end && !isspace((unsigned char)literal_source[index]))
      index++;
    end = index;
    colon = start;
    while (colon < end && literal_source[colon] != ':')
      colon++;
    if (repository_path(literal_source + start, colon - start))
      status = append_path(engine, out, capacity, literal_source + start,
                           colon - start);
  }
  return status;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static void sort_unique(ArchbirdEngine *engine, AbStringArray *array) {
  size_t read;
  size_t write = 0;
  if (array->count > 1)
    qsort(array->items, array->count, sizeof(*array->items), string_compare);
  for (read = 0; read < array->count; read++) {
    if (write &&
        !ab_string_compare(&array->items[write - 1], &array->items[read])) {
      ab_string_free(engine, &array->items[read]);
      continue;
    }
    if (write != read)
      array->items[write] = array->items[read];
    write++;
  }
  array->count = write;
}

static int definition_macro(const uint8_t *source, SourceSpan token) {
  static const char *const names[] = {"AC_DEFUN", "AC_DEFUN_ONCE", "m4_defun",
                                      "m4_defun_once"};
  size_t index;
  for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
    if (token_equals(source, token, names[index]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus set_definition(ArchbirdEngine *engine,
                                     const uint8_t *source, size_t length,
                                     const Invocation *invocation,
                                     Definition **definitions,
                                     size_t *definition_count) {
  SourceSpan name;
  SourceSpan value;
  Definition *found;
  Definition *resized;
  if (invocation->argument_count < 2 ||
      !unwrap_span(source, length, invocation->arguments[0], &name) ||
      !unwrap_span(source, length, invocation->arguments[1], &value))
    return ARCHBIRD_OK;
  if (name.start == name.end)
    return ARCHBIRD_OK;
  found = definition(*definitions, *definition_count, source + name.start,
                     name.end - name.start);
  if (!found) {
    /* Constant lookup remains bounded even for generated M4 inputs. */
    if (*definition_count == MAX_SIMPLE_DEFINITIONS)
      return ARCHBIRD_OK;
    resized = (Definition *)ab_realloc(
        engine, *definitions, (*definition_count + 1) * sizeof(**definitions));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory parsing configure.ac");
    *definitions = resized;
    found = &resized[*definition_count];
    memset(found, 0, sizeof(*found));
    if (ab_string_copy(engine, &found->name, (const char *)source + name.start,
                       name.end - name.start) != ARCHBIRD_OK)
      return ARCHBIRD_OUT_OF_MEMORY;
    (*definition_count)++;
  }
  ab_string_free(engine, &found->value);
  return ab_string_copy(engine, &found->value,
                        (const char *)source + value.start,
                        value.end - value.start);
}

static void definitions_free(ArchbirdEngine *engine, Definition *definitions,
                             size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &definitions[index].name);
    ab_string_free(engine, &definitions[index].value);
  }
  ab_free(engine, definitions);
}

ArchbirdStatus ab_autoconf_metadata(ArchbirdEngine *engine,
                                    const uint8_t *source, size_t length,
                                    AbAutoconfMetadata *out) {
  Definition *definitions = NULL;
  size_t definition_count = 0;
  size_t files_capacity = 0;
  size_t headers_capacity = 0;
  size_t subdirectories_capacity = 0;
  size_t index = 0;
  ArchbirdStatus status;
  if (!engine || (!source && length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_utf8_validate(engine, source, length);
  while (status == ARCHBIRD_OK && index < length) {
    SourceSpan token;
    size_t open;
    Invocation invocation;
    if (source[index] == '#') {
      while (index < length && source[index] != '\n')
        index++;
      continue;
    }
    if (!identifier_start(source[index])) {
      index++;
      continue;
    }
    token.start = index++;
    while (index < length && identifier_continue(source[index]))
      index++;
    token.end = index;
    if (token_equals(source, token, "dnl")) {
      while (index < length && source[index] != '\n')
        index++;
      continue;
    }
    if (token_equals(source, token, "AC_OUTPUT")) {
      out->has_output = 1;
      open = index;
      while (open < length && isspace((unsigned char)source[open]))
        open++;
      if (open < length && source[open] == '(') {
        if (!parse_invocation(source, length, open, &invocation))
          break;
        if (invocation.argument_count)
          status = collect_paths(
              engine, source, length, invocation.arguments[0], definitions,
              definition_count, &out->files, &files_capacity);
        index = invocation.end;
      }
      continue;
    }
    if (!definition_macro(source, token) &&
        !token_equals(source, token, "m4_define") &&
        !token_equals(source, token, "AC_INIT") &&
        !token_equals(source, token, "AC_CONFIG_FILES") &&
        !token_equals(source, token, "AC_CONFIG_HEADERS") &&
        !token_equals(source, token, "AC_CONFIG_LINKS") &&
        !token_equals(source, token, "AC_CONFIG_SUBDIRS"))
      continue;
    open = index;
    while (open < length && isspace((unsigned char)source[open]))
      open++;
    if (open == length || source[open] != '(')
      continue;
    if (!parse_invocation(source, length, open, &invocation))
      break;
    if (definition_macro(source, token)) {
      index = invocation.end;
      continue;
    }
    if (token_equals(source, token, "m4_define")) {
      status = set_definition(engine, source, length, &invocation, &definitions,
                              &definition_count);
      index = invocation.end;
      continue;
    }
    if (token_equals(source, token, "AC_INIT")) {
      if (!out->package.length && invocation.argument_count)
        status = copy_literal(engine, source, length, invocation.arguments[0],
                              definitions, definition_count, &out->package);
      if (status == ARCHBIRD_OK && !out->version.length &&
          invocation.argument_count > 1)
        status = copy_literal(engine, source, length, invocation.arguments[1],
                              definitions, definition_count, &out->version);
      index = invocation.end;
      continue;
    }
    if (token_equals(source, token, "AC_CONFIG_FILES") &&
        invocation.argument_count)
      status = collect_paths(engine, source, length, invocation.arguments[0],
                             definitions, definition_count, &out->files,
                             &files_capacity);
    else if (token_equals(source, token, "AC_CONFIG_HEADERS") &&
             invocation.argument_count)
      status = collect_paths(engine, source, length, invocation.arguments[0],
                             definitions, definition_count, &out->headers,
                             &headers_capacity);
    else if (token_equals(source, token, "AC_CONFIG_LINKS") &&
             invocation.argument_count)
      status = collect_paths(engine, source, length, invocation.arguments[0],
                             definitions, definition_count, &out->files,
                             &files_capacity);
    else if (token_equals(source, token, "AC_CONFIG_SUBDIRS") &&
             invocation.argument_count)
      status = collect_paths(engine, source, length, invocation.arguments[0],
                             definitions, definition_count,
                             &out->subdirectories, &subdirectories_capacity);
    if (status == ARCHBIRD_OK &&
        (token_equals(source, token, "AC_CONFIG_FILES") ||
         token_equals(source, token, "AC_CONFIG_HEADERS") ||
         token_equals(source, token, "AC_CONFIG_LINKS") ||
         token_equals(source, token, "AC_CONFIG_SUBDIRS")))
      index = invocation.end;
  }
  definitions_free(engine, definitions, definition_count);
  if (status == ARCHBIRD_OK) {
    sort_unique(engine, &out->files);
    sort_unique(engine, &out->headers);
    sort_unique(engine, &out->subdirectories);
  }
  if (status != ARCHBIRD_OK)
    ab_autoconf_metadata_free(engine, out);
  return status;
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

void ab_autoconf_metadata_free(ArchbirdEngine *engine,
                               AbAutoconfMetadata *metadata) {
  if (!engine || !metadata)
    return;
  ab_string_free(engine, &metadata->package);
  ab_string_free(engine, &metadata->version);
  string_array_free(engine, &metadata->files);
  string_array_free(engine, &metadata->headers);
  string_array_free(engine, &metadata->subdirectories);
  memset(metadata, 0, sizeof(*metadata));
}
