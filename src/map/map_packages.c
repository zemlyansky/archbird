#include "map_internal.h"

#include "archbird_internal.h"
#include "lexical/tokenizer.h"
#include "package_json.h"
#include "pattern.h"
#include "python_manifest.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

const char *ab_map_package_manager(const AbString *kind) {
  if (string_literal(kind, "npm"))
    return "npm";
  if (string_literal(kind, "python"))
    return "pypi";
  if (string_literal(kind, "r"))
    return "cran";
  return "generic";
}

const char *ab_map_language_manager(const AbString *language) {
  if (string_literal(language, "javascript") ||
      string_literal(language, "typescript") || string_literal(language, "vue"))
    return "npm";
  if (string_literal(language, "python"))
    return "pypi";
  if (string_literal(language, "r"))
    return "cran";
  return "";
}

static size_t normalized_package_next(const char *manager,
                                      const AbString *value, size_t *index,
                                      unsigned char *out) {
  size_t cursor = *index;
  unsigned char byte;
  if (cursor >= value->length)
    return 0;
  byte = (unsigned char)value->data[cursor++];
  if ((strcmp(manager, "pypi") == 0 || strcmp(manager, "cran") == 0) &&
      (byte == '-' || byte == '_' || byte == '.')) {
    while (cursor < value->length &&
           (value->data[cursor] == '-' || value->data[cursor] == '_' ||
            value->data[cursor] == '.'))
      cursor++;
    byte = '-';
  }
  if (strcmp(manager, "npm") == 0 || strcmp(manager, "pypi") == 0 ||
      strcmp(manager, "cran") == 0)
    byte = (unsigned char)tolower(byte);
  *index = cursor;
  *out = byte;
  return 1;
}

static int normalized_package_equal(const char *manager, const AbString *left,
                                    const AbString *right) {
  size_t left_index = 0;
  size_t right_index = 0;
  unsigned char left_byte;
  unsigned char right_byte;
  while (normalized_package_next(manager, left, &left_index, &left_byte)) {
    if (!normalized_package_next(manager, right, &right_index, &right_byte) ||
        left_byte != right_byte)
      return 0;
  }
  return !normalized_package_next(manager, right, &right_index, &right_byte);
}

AbString ab_map_external_import_name(const AbString *language,
                                     const AbString *imported) {
  AbString result = *imported;
  size_t index;
  const char *manager = ab_map_language_manager(language);
  if (strcmp(manager, "npm") == 0) {
    if (result.length && result.data[0] == '@') {
      size_t slashes = 0;
      for (index = 0; index < result.length; index++) {
        if (result.data[index] == '/' && ++slashes == 2) {
          result.length = index;
          break;
        }
      }
    } else {
      for (index = 0; index < result.length; index++) {
        if (result.data[index] == '/') {
          result.length = index;
          break;
        }
      }
    }
  } else {
    while (result.length && result.data[0] == '.') {
      result.data++;
      result.length--;
    }
    for (index = 0; index < result.length; index++) {
      if (result.data[index] == '.') {
        result.length = index;
        break;
      }
    }
  }
  return result;
}

int ab_map_package_alias_matches(const AbMapPackage *package,
                                 const char *manager,
                                 const AbString *external) {
  size_t index;
  if (package->identity.length &&
      normalized_package_equal(manager, &package->identity, external))
    return 1;
  for (index = 0; index < package->aliases.count; index++) {
    if (normalized_package_equal(manager, &package->aliases.items[index],
                                 external))
      return 1;
  }
  return 0;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static int pair_compare(const void *left_raw, const void *right_raw) {
  const AbStringPair *left = (const AbStringPair *)left_raw;
  const AbStringPair *right = (const AbStringPair *)right_raw;
  return ab_string_compare(&left->key, &right->key);
}

static int dependency_compare(const void *left_raw, const void *right_raw) {
  const AbMapDependency *left = (const AbMapDependency *)left_raw;
  const AbMapDependency *right = (const AbMapDependency *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus copy_string(ArchbirdEngine *engine, AbString *out,
                                  const AbString *source) {
  return ab_string_copy(engine, out, source->data, source->length);
}

static const AbManifestFile *
mapped_file(const AbMapState *state, const AbString *path, size_t *out_index) {
  const AbManifestFile *file =
      ab_map_manifest_file(state->manifest, path->data, path->length);
  if (!file)
    return NULL;
  if (out_index)
    *out_index = (size_t)(file - state->manifest->files);
  return file;
}

static const AbObjectField *fact_attribute(const AbFact *fact,
                                           const char *name) {
  AbString wanted = {(char *)name, strlen(name)};
  size_t low = 0;
  size_t high = fact->attribute_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&fact->attributes[middle].name, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &fact->attributes[middle];
  }
  return NULL;
}

static const AbString *fact_string_attribute(const AbFact *fact,
                                             const char *name) {
  const AbObjectField *field = fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_STRING ? &field->value.as.text
                                                       : NULL;
}

static int fact_integer_attribute(const AbFact *fact, const char *name,
                                  uint64_t *out) {
  const AbObjectField *field = fact_attribute(fact, name);
  uint64_t value = 0;
  size_t index;
  if (!field || field->value.kind != AB_VALUE_INTEGER)
    return 0;
  for (index = 0; index < field->value.as.text.length; index++) {
    unsigned char digit = (unsigned char)field->value.as.text.data[index];
    if (digit < '0' || digit > '9' ||
        value > (UINT64_MAX - (uint64_t)(digit - '0')) / 10)
      return 0;
    value = value * 10 + (uint64_t)(digit - '0');
  }
  *out = value;
  return 1;
}

static ArchbirdStatus append_unique_string(ArchbirdEngine *engine,
                                           AbStringArray *array,
                                           const char *data, size_t length) {
  AbString *resized;
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (array->items[index].length == length &&
        (!length || memcmp(array->items[index].data, data, length) == 0))
      return ARCHBIRD_OK;
  }
  if (array->count == SIZE_MAX / sizeof(*array->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many package string values");
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting package values");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_unique_pair(ArchbirdEngine *engine,
                                         AbStringPair **pairs, size_t *count,
                                         const char *key, size_t key_length,
                                         const char *value, size_t value_length,
                                         int setdefault) {
  AbStringPair *resized;
  AbStringPair *pair;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < *count; index++) {
    if ((*pairs)[index].key.length == key_length &&
        memcmp((*pairs)[index].key.data, key, key_length) == 0) {
      AbString replacement = {0};
      if (setdefault)
        return ARCHBIRD_OK;
      status = ab_string_copy(engine, &replacement, value, value_length);
      if (status == ARCHBIRD_OK) {
        ab_string_free(engine, &(*pairs)[index].value);
        (*pairs)[index].value = replacement;
      }
      return status;
    }
  }
  if (*count == SIZE_MAX / sizeof(**pairs))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many package key/value entries");
  resized = (AbStringPair *)ab_realloc(engine, *pairs,
                                       (*count + 1) * sizeof(**pairs));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting package entries");
  *pairs = resized;
  pair = &(*pairs)[*count];
  memset(pair, 0, sizeof(*pair));
  status = ab_string_copy(engine, &pair->key, key, key_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &pair->value, value, value_length);
  if (status == ARCHBIRD_OK)
    (*count)++;
  else {
    ab_string_free(engine, &pair->key);
    ab_string_free(engine, &pair->value);
  }
  return status;
}

static ArchbirdStatus
append_dependency(ArchbirdEngine *engine, AbMapPackage *package,
                  const char *name, size_t name_length, const char *requirement,
                  size_t requirement_length, const char *scope) {
  AbMapDependency *resized;
  AbMapDependency *item;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < package->dependency_count; index++) {
    if (package->dependencies[index].name.length == name_length &&
        memcmp(package->dependencies[index].name.data, name, name_length) == 0)
      return ARCHBIRD_OK;
  }
  if (package->dependency_count == SIZE_MAX / sizeof(*package->dependencies))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many package dependencies");
  resized = (AbMapDependency *)ab_realloc(engine, package->dependencies,
                                          (package->dependency_count + 1) *
                                              sizeof(*package->dependencies));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting package dependencies");
  package->dependencies = resized;
  item = &package->dependencies[package->dependency_count];
  memset(item, 0, sizeof(*item));
  status = ab_string_copy(engine, &item->name, name, name_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &item->requirement, requirement,
                            requirement_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &item->scope, scope, strlen(scope));
  if (status == ARCHBIRD_OK)
    package->dependency_count++;
  else {
    ab_string_free(engine, &item->name);
    ab_string_free(engine, &item->requirement);
    ab_string_free(engine, &item->scope);
  }
  return status;
}

static void trim(const uint8_t *text, size_t *start, size_t *end) {
  while (*start < *end && isspace((unsigned char)text[*start]))
    (*start)++;
  while (*end > *start && isspace((unsigned char)text[*end - 1]))
    (*end)--;
}

static int bytes_equal(const uint8_t *text, size_t start, size_t end,
                       const char *literal) {
  size_t length = strlen(literal);
  return end - start == length &&
         (!length || memcmp(text + start, literal, length) == 0);
}

static void requirement_name(const char *requirement, size_t length,
                             size_t *out_start, size_t *out_length) {
  size_t index = 0;
  while (index < length && isspace((unsigned char)requirement[index]))
    index++;
  if (index < length && isalnum((unsigned char)requirement[index])) {
    size_t start = index++;
    while (index < length &&
           (isalnum((unsigned char)requirement[index]) ||
            requirement[index] == '.' || requirement[index] == '_' ||
            requirement[index] == '-'))
      index++;
    *out_start = start;
    *out_length = index - start;
  } else {
    *out_start = 0;
    *out_length = length;
    while (*out_length && isspace((unsigned char)requirement[*out_length - 1]))
      (*out_length)--;
  }
}

static ArchbirdStatus quoted_values(AbMapState *state, AbMapPackage *package,
                                    const uint8_t *text, size_t length,
                                    const char *scope) {
  size_t index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  while (status == ARCHBIRD_OK && index < length) {
    uint8_t quote;
    size_t start;
    size_t write;
    char *value;
    size_t name_start;
    size_t name_length;
    while (index < length && text[index] != '\'' && text[index] != '"')
      index++;
    if (index == length)
      break;
    quote = text[index++];
    start = index;
    value = (char *)ab_malloc(state->engine, length - start + 1);
    if (!value)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding package requirement");
    write = 0;
    while (index < length && text[index] != quote) {
      if (text[index] == '\\' && index + 1 < length) {
        index++;
        if (text[index] == 'n')
          value[write++] = '\n';
        else if (text[index] == 'r')
          value[write++] = '\r';
        else if (text[index] == 't')
          value[write++] = '\t';
        else
          value[write++] = (char)text[index];
      } else {
        value[write++] = (char)text[index];
      }
      index++;
    }
    if (index < length)
      index++;
    value[write] = '\0';
    name_length = write;
    requirement_name(value, write, &name_start, &name_length);
    if (name_length)
      status = append_dependency(state->engine, package, value + name_start,
                                 name_length, value, write, scope);
    ab_free(state->engine, value);
  }
  return status;
}

static int array_complete(const uint8_t *text, size_t length) {
  int depth = 0;
  uint8_t quote = 0;
  int escaped = 0;
  size_t index;
  for (index = 0; index < length; index++) {
    uint8_t value = text[index];
    if (quote) {
      if (escaped)
        escaped = 0;
      else if (value == '\\')
        escaped = 1;
      else if (value == quote)
        quote = 0;
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (value == '[') {
      depth++;
    } else if (value == ']') {
      depth--;
      if (depth == 0)
        return 1;
    }
  }
  return 0;
}

static ArchbirdStatus parse_toml(AbMapState *state, AbMapPackage *package,
                                 const uint8_t *text, size_t length) {
  size_t line_start = 0;
  AbString section = {0};
  AbPyprojectMetadata metadata = {0};
  ArchbirdStatus status =
      ab_pyproject_metadata(state->engine, text, length, &metadata);
  if (status == ARCHBIRD_OK && !package->identity.length &&
      metadata.name.length)
    status = copy_string(state->engine, &package->identity, &metadata.name);
  if (status == ARCHBIRD_OK && !package->version.length &&
      metadata.version.length)
    status = copy_string(state->engine, &package->version, &metadata.version);
  if (status == ARCHBIRD_OK && metadata.module.length)
    status = append_unique_string(state->engine, &package->aliases,
                                  metadata.module.data, metadata.module.length);
  ab_pyproject_metadata_free(state->engine, &metadata);
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
    if (end > start + 1 && text[start] == '[' && text[end - 1] == ']') {
      ab_string_free(state->engine, &section);
      status = ab_string_copy(state->engine, &section,
                              (const char *)text + start + 1, end - start - 2);
      line_start = line_end + 1;
      continue;
    }
    equal = start;
    while (equal < end && text[equal] != '=')
      equal++;
    if (equal < end) {
      size_t key_start = start;
      size_t key_end = equal;
      size_t value_start = equal + 1;
      size_t value_end = end;
      trim(text, &key_start, &key_end);
      trim(text, &value_start, &value_end);
      if (string_literal(&section, "project") &&
          (bytes_equal(text, key_start, key_end, "name") ||
           bytes_equal(text, key_start, key_end, "version")) &&
          value_end > value_start + 1 && text[value_start] == '"' &&
          text[value_end - 1] == '"') {
        AbString *target = bytes_equal(text, key_start, key_end, "name")
                               ? &package->identity
                               : &package->version;
        if (!target->length)
          status = ab_string_copy(state->engine, target,
                                  (const char *)text + value_start + 1,
                                  value_end - value_start - 2);
      } else if (string_literal(&section, "project.scripts") &&
                 value_end > value_start + 1 && text[value_start] == '"' &&
                 text[value_end - 1] == '"') {
        size_t clean_key_start = key_start;
        size_t clean_key_end = key_end;
        if (clean_key_end > clean_key_start + 1 &&
            text[clean_key_start] == '"' && text[clean_key_end - 1] == '"') {
          clean_key_start++;
          clean_key_end--;
        }
        status = append_unique_pair(state->engine, &package->scripts,
                                    &package->script_count,
                                    (const char *)text + clean_key_start,
                                    clean_key_end - clean_key_start,
                                    (const char *)text + value_start + 1,
                                    value_end - value_start - 2, 0);
        if (status == ARCHBIRD_OK) {
          size_t route_length = 7 + clean_key_end - clean_key_start;
          char *route = (char *)ab_malloc(state->engine, route_length);
          if (!route)
            status = archbird_error_set(
                state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                "out of memory decoding Python script route");
          else {
            memcpy(route, "script:", 7);
            memcpy(route + 7, text + clean_key_start,
                   clean_key_end - clean_key_start);
            status = append_unique_pair(state->engine, &package->entrypoints,
                                        &package->entrypoint_count, route,
                                        route_length,
                                        (const char *)text + value_start + 1,
                                        value_end - value_start - 2, 0);
            ab_free(state->engine, route);
          }
        }
      } else if ((string_literal(&section, "project") &&
                  bytes_equal(text, key_start, key_end, "dependencies")) ||
                 string_literal(&section, "project.optional-dependencies")) {
        size_t body_end = line_end;
        const char *scope = "runtime";
        char *optional_scope = NULL;
        while (!array_complete(text + value_start, body_end - value_start) &&
               body_end < length) {
          body_end++;
          while (body_end < length && text[body_end] != '\n')
            body_end++;
        }
        if (!string_literal(&section, "project")) {
          size_t key_length = key_end - key_start;
          optional_scope = (char *)ab_malloc(state->engine, 9 + key_length + 1);
          if (!optional_scope) {
            status = archbird_error_set(
                state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                "out of memory decoding optional dependency scope");
          } else {
            memcpy(optional_scope, "optional:", 9);
            memcpy(optional_scope + 9, text + key_start, key_length);
            optional_scope[9 + key_length] = '\0';
            scope = optional_scope;
          }
        }
        if (status == ARCHBIRD_OK)
          status = quoted_values(state, package, text + value_start,
                                 body_end - value_start, scope);
        ab_free(state->engine, optional_scope);
        line_end = body_end;
      }
    }
    if (line_end == length)
      break;
    line_start = line_end + 1;
  }
  ab_string_free(state->engine, &section);
  return status;
}

typedef struct CaptureCopy {
  ArchbirdEngine *engine;
  const uint8_t *subject;
  AbString *target;
} CaptureCopy;

static ArchbirdStatus copy_first_capture(void *user_data,
                                         const AbPatternMatch *match) {
  CaptureCopy *copy = (CaptureCopy *)user_data;
  if (copy->target->length || !match->capture_present)
    return ARCHBIRD_OK;
  return ab_string_copy(copy->engine, copy->target,
                        (const char *)copy->subject + match->capture_start,
                        match->capture_end - match->capture_start);
}

static ArchbirdStatus capture_string(AbMapState *state, const uint8_t *text,
                                     size_t length, const char *pattern_text,
                                     AbString *target) {
  AbString source = {(char *)pattern_text, strlen(pattern_text)};
  AbPattern *pattern = NULL;
  CaptureCopy copy = {state->engine, text, target};
  size_t matches = 0;
  ArchbirdStatus status =
      ab_pattern_compile(state->engine, &source, 1, &pattern);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_scan(state->engine, pattern, text, length, 1,
                             copy_first_capture, &copy, &matches);
  ab_pattern_free(pattern);
  return status;
}

typedef struct RequirementCapture {
  AbMapState *state;
  AbMapPackage *package;
  const uint8_t *subject;
} RequirementCapture;

static ArchbirdStatus capture_requirements(void *user_data,
                                           const AbPatternMatch *match) {
  RequirementCapture *capture = (RequirementCapture *)user_data;
  if (!match->capture_present)
    return ARCHBIRD_OK;
  return quoted_values(capture->state, capture->package,
                       capture->subject + match->capture_start,
                       match->capture_end - match->capture_start, "runtime");
}

static ArchbirdStatus parse_setup_py(AbMapState *state, AbMapPackage *package,
                                     const uint8_t *text, size_t length) {
  static const char name_pattern[] = "\\bname\\s*=\\s*['\"]([^'\"]+)['\"]";
  static const char version_pattern[] =
      "\\bversion\\s*=\\s*['\"]([^'\"]+)['\"]";
  static const char requirements_pattern[] =
      "(?s)\\binstall_requires\\s*=\\s*\\[(.*?)\\]";
  AbString source = {(char *)requirements_pattern,
                     sizeof(requirements_pattern) - 1};
  AbPattern *pattern = NULL;
  RequirementCapture capture = {state, package, text};
  size_t matches = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!package->identity.length)
    status =
        capture_string(state, text, length, name_pattern, &package->identity);
  if (status == ARCHBIRD_OK && !package->version.length)
    status =
        capture_string(state, text, length, version_pattern, &package->version);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_compile(state->engine, &source, 1, &pattern);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_scan(state->engine, pattern, text, length, 1,
                             capture_requirements, &capture, &matches);
  ab_pattern_free(pattern);
  return status;
}

typedef struct ExportCapture {
  AbMapState *state;
  AbMapPackage *package;
  const uint8_t *subject;
} ExportCapture;

static ArchbirdStatus capture_exports(void *user_data,
                                      const AbPatternMatch *match) {
  ExportCapture *capture = (ExportCapture *)user_data;
  size_t start;
  size_t index;
  if (!match->capture_present)
    return ARCHBIRD_OK;
  start = match->capture_start;
  for (index = start; index <= match->capture_end; index++) {
    if (index == match->capture_end || capture->subject[index] == ',') {
      size_t left = start;
      size_t right = index;
      trim(capture->subject, &left, &right);
      while (right > left &&
             (capture->subject[left] == '\'' || capture->subject[left] == '"'))
        left++;
      while (right > left && (capture->subject[right - 1] == '\'' ||
                              capture->subject[right - 1] == '"'))
        right--;
      if (right > left) {
        ArchbirdStatus status = append_unique_string(
            capture->state->engine, &capture->package->exports,
            (const char *)capture->subject + left, right - left);
        if (status != ARCHBIRD_OK)
          return status;
      }
      start = index + 1;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_r_manifest(AbMapState *state,
                                       const AbConfigPackage *config,
                                       AbMapPackage *package,
                                       const uint8_t *text, size_t length) {
  static const char exports_pattern[] = "(?s)\\bexport\\s*\\((.*?)\\)";
  const char *base = config->path.data;
  size_t base_length = config->path.length;
  size_t index;
  AbString source = {(char *)exports_pattern, sizeof(exports_pattern) - 1};
  AbPattern *pattern = NULL;
  ExportCapture capture = {state, package, text};
  size_t matches = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  int is_description;
  for (index = 0; index < config->path.length; index++) {
    if (config->path.data[index] == '/') {
      base = config->path.data + index + 1;
      base_length = config->path.length - index - 1;
    }
  }
  is_description = base_length == 11 && memcmp(base, "DESCRIPTION", 11) == 0;
  if (is_description) {
    size_t line_start = 0;
    while (line_start < length) {
      size_t line_end = line_start;
      size_t colon;
      while (line_end < length && text[line_end] != '\n')
        line_end++;
      colon = line_start;
      while (colon < line_end && text[colon] != ':')
        colon++;
      if (colon < line_end) {
        size_t value_start = colon + 1;
        size_t value_end = line_end;
        trim(text, &value_start, &value_end);
        if (!package->identity.length &&
            bytes_equal(text, line_start, colon, "Package"))
          status = ab_string_copy(state->engine, &package->identity,
                                  (const char *)text + value_start,
                                  value_end - value_start);
        if (status == ARCHBIRD_OK && !package->version.length &&
            bytes_equal(text, line_start, colon, "Version"))
          status = ab_string_copy(state->engine, &package->version,
                                  (const char *)text + value_start,
                                  value_end - value_start);
      }
      if (status != ARCHBIRD_OK)
        return status;
      line_start = line_end + 1;
    }
    if (status == ARCHBIRD_OK) {
      AbBuffer namespace_path;
      const AbManifestFile *namespace_file;
      AbString namespace_name;
      size_t namespace_index = 0;
      size_t directory_length = config->path.length - base_length;
      ab_buffer_init(&namespace_path, state->engine);
      status = ab_buffer_append(&namespace_path, config->path.data,
                                directory_length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&namespace_path, "NAMESPACE");
      namespace_name.data = (char *)namespace_path.data;
      namespace_name.length = namespace_path.length;
      namespace_file =
          status == ARCHBIRD_OK
              ? mapped_file(state, &namespace_name, &namespace_index)
              : NULL;
      if (namespace_file)
        text = ab_project_source_bytes(state->project, namespace_index);
      else
        text = NULL;
      length = namespace_file ? namespace_file->byte_length : 0;
      capture.subject = text;
      ab_buffer_free(&namespace_path);
    }
  }
  if (status == ARCHBIRD_OK && text)
    status = ab_utf8_validate(state->engine, text, length);
  if (status == ARCHBIRD_OK && text)
    status = ab_pattern_compile(state->engine, &source, 1, &pattern);
  if (status == ARCHBIRD_OK)
    status = pattern ? ab_pattern_scan(state->engine, pattern, text, length, 1,
                                       capture_exports, &capture, &matches)
                     : ARCHBIRD_OK;
  ab_pattern_free(pattern);
  return status;
}

static ArchbirdStatus package_config_values(AbMapState *state,
                                            const AbConfigPackage *config,
                                            AbMapPackage *package) {
  size_t index;
  ArchbirdStatus status =
      copy_string(state->engine, &package->name, &config->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(state->engine, &package->kind, &config->kind);
  if (status == ARCHBIRD_OK)
    status = copy_string(state->engine, &package->layer, &config->layer);
  if (status == ARCHBIRD_OK)
    status = copy_string(state->engine, &package->manifest, &config->path);
  if (status == ARCHBIRD_OK && config->identity.length)
    status = copy_string(state->engine, &package->identity, &config->identity);
  if (status == ARCHBIRD_OK && config->version.length)
    status = copy_string(state->engine, &package->version, &config->version);
  for (index = 0; status == ARCHBIRD_OK && index < config->aliases.count;
       index++)
    status = append_unique_string(state->engine, &package->aliases,
                                  config->aliases.items[index].data,
                                  config->aliases.items[index].length);
  return status;
}

static ArchbirdStatus invalid_manifest_diagnostic(AbMapState *state,
                                                  const char *code,
                                                  const AbString *path) {
  const char *message = archbird_engine_error(state->engine);
  ArchbirdStatus status = ab_map_add_diagnostic(
      state, "error", code,
      message && message[0] ? message : "invalid package manifest", path);
  archbird_error_clear(state->engine);
  return status;
}

static ArchbirdStatus analyze_manifest(AbMapState *state,
                                       const AbConfigPackage *config,
                                       AbMapPackage *package) {
  size_t file_index = 0;
  const AbManifestFile *file = mapped_file(state, &config->path, &file_index);
  const uint8_t *text;
  ArchbirdStatus status;
  if (!file) {
    char message[256];
    snprintf(message, sizeof(message), "package %.*s", (int)config->name.length,
             config->name.data);
    return ab_map_add_diagnostic(state, "error", "missing-package-manifest",
                                 message, &config->path);
  }
  text = ab_project_source_bytes(state->project, file_index);
  if (!text)
    return ab_map_add_diagnostic(state, "error", "invalid-package-manifest",
                                 "package manifest bytes are absent",
                                 &config->path);
  status = ab_utf8_validate(state->engine, text, file->byte_length);
  if (status != ARCHBIRD_OK)
    return invalid_manifest_diagnostic(state, "invalid-package-manifest",
                                       &config->path);
  if (string_literal(&config->kind, "npm")) {
    status =
        ab_decode_npm_manifest(state->engine, text, file->byte_length, package);
    if (status == ARCHBIRD_INVALID_JSON || status == ARCHBIRD_DUPLICATE_KEY ||
        status == ARCHBIRD_INVALID_SCHEMA)
      return invalid_manifest_diagnostic(state, "invalid-package-json",
                                         &config->path);
    return status;
  }
  if (string_literal(&config->kind, "python") && config->path.length >= 5 &&
      memcmp(config->path.data + config->path.length - 5, ".toml", 5) == 0)
    return parse_toml(state, package, text, file->byte_length);
  if (string_literal(&config->kind, "python") && config->path.length >= 8 &&
      memcmp(config->path.data + config->path.length - 8, "setup.py", 8) == 0)
    return parse_setup_py(state, package, text, file->byte_length);
  if (string_literal(&config->kind, "r"))
    return parse_r_manifest(state, config, package, text, file->byte_length);
  return ARCHBIRD_OK;
}

static int python_module_name(const AbString *alias, char *out, size_t capacity,
                              size_t *out_length) {
  size_t index;
  if (!alias->length || alias->length + 1 > capacity)
    return 0;
  for (index = 0; index < alias->length; index++) {
    unsigned char value = (unsigned char)alias->data[index];
    if (value == '-' || value == '.')
      out[index] = '_';
    else if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9') || value == '_')
      out[index] = (char)value;
    else
      return 0;
  }
  if (out[0] >= '0' && out[0] <= '9')
    return 0;
  out[alias->length] = '\0';
  *out_length = alias->length;
  return 1;
}

static ArchbirdStatus infer_python_entries(AbMapState *state,
                                           const AbConfigPackage *config,
                                           const AbMapPackage *package,
                                           AbStringArray *entries) {
  static const char *const prefixes[] = {"src/", ""};
  static const char *const suffixes[] = {"/__init__.py", ".py"};
  size_t base_length = config->path.length;
  size_t alias_index;
  while (base_length && config->path.data[base_length - 1] != '/')
    base_length--;
  for (alias_index = 0; alias_index < package->aliases.count; alias_index++) {
    const AbString *alias = &package->aliases.items[alias_index];
    char module[256];
    size_t module_length;
    const AbManifestFile *matched = NULL;
    size_t matches = 0;
    size_t prefix_index;
    if (!python_module_name(alias, module, sizeof(module), &module_length))
      continue;
    for (prefix_index = 0;
         prefix_index < sizeof(prefixes) / sizeof(prefixes[0]);
         prefix_index++) {
      size_t suffix_index;
      for (suffix_index = 0;
           suffix_index < sizeof(suffixes) / sizeof(suffixes[0]);
           suffix_index++) {
        AbBuffer candidate;
        const AbManifestFile *file;
        ArchbirdStatus status;
        ab_buffer_init(&candidate, state->engine);
        status = ab_buffer_append(&candidate, config->path.data, base_length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&candidate, prefixes[prefix_index]);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&candidate, module, module_length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&candidate, suffixes[suffix_index]);
        if (status != ARCHBIRD_OK) {
          ab_buffer_free(&candidate);
          return status;
        }
        file = ab_map_manifest_file(
            state->manifest, (const char *)candidate.data, candidate.length);
        if (file && file->has_layer &&
            ab_string_equal(&file->layer, &package->layer)) {
          matched = file;
          matches++;
        }
        ab_buffer_free(&candidate);
      }
    }
    if (matches == 1) {
      ArchbirdStatus status = append_unique_string(
          state->engine, entries, matched->path.data, matched->path.length);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus resolve_npm_entry(AbMapState *state,
                                        const AbMapPackage *package,
                                        const AbString *target,
                                        const AbManifestFile **out_file) {
  const AbManifestFile *manifest_file;
  AbManifestFile source;
  AbString relative;
  char *relative_bytes = NULL;
  ArchbirdStatus status;

  *out_file = NULL;
  if (!target || !target->length)
    return ARCHBIRD_OK;
  manifest_file = mapped_file(state, &package->manifest, NULL);
  if (!manifest_file)
    return ARCHBIRD_OK;
  source = *manifest_file;
  source.has_language = 1;
  source.language.data = "javascript";
  source.language.length = 10;
  relative = *target;
  if (target->data[0] != '.' && target->data[0] != '/') {
    relative_bytes = (char *)ab_malloc(state->engine, target->length + 3);
    if (!relative_bytes)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory resolving npm package entry");
    relative_bytes[0] = '.';
    relative_bytes[1] = '/';
    memcpy(relative_bytes + 2, target->data, target->length);
    relative_bytes[target->length + 2] = '\0';
    relative.data = relative_bytes;
    relative.length = target->length + 2;
  }
  status = ab_map_resolve_import(state->engine, state->manifest, state->config,
                                 &source, &relative, out_file);
  ab_free(state->engine, relative_bytes);
  return status;
}

static ArchbirdStatus collect_entries(AbMapState *state,
                                      const AbConfigPackage *config,
                                      AbMapPackage *package,
                                      AbStringArray *entries) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < config->entries.count;
       index++) {
    const AbString *entry = &config->entries.items[index];
    const char *data = entry->data;
    size_t length = entry->length;
    if (length >= 2 && data[0] == '.' && data[1] == '/') {
      data += 2;
      length -= 2;
    }
    status = append_unique_string(state->engine, entries, data, length);
  }
  if (status == ARCHBIRD_OK && !entries->count &&
      string_literal(&package->kind, "python"))
    status = infer_python_entries(state, config, package, entries);
  if (string_literal(&package->kind, "npm")) {
    const AbStringPair *main_entry = NULL;
    size_t target_count =
        package->npm_has_exports ? package->npm_runtime_entries.count : 0;
    for (index = 0;
         !package->npm_has_exports && index < package->entrypoint_count;
         index++) {
      if (string_literal(&package->entrypoints[index].key, "main")) {
        main_entry = &package->entrypoints[index];
        target_count = 1;
        break;
      }
    }
    if (status == ARCHBIRD_OK && !package->npm_has_exports && !main_entry) {
      static const AbString default_main = {(char *)"index.js", 8};
      const AbManifestFile *file = NULL;
      status = resolve_npm_entry(state, package, &default_main, &file);
      if (status == ARCHBIRD_OK && file && file->has_layer) {
        status = append_unique_pair(state->engine, &package->entrypoints,
                                    &package->entrypoint_count, "main", 4,
                                    default_main.data, default_main.length, 1);
        if (status == ARCHBIRD_OK) {
          main_entry = &package->entrypoints[package->entrypoint_count - 1];
          target_count = 1;
        }
      }
    }
    for (index = 0; status == ARCHBIRD_OK && index < target_count; index++) {
      const AbString *target = package->npm_has_exports
                                   ? &package->npm_runtime_entries.items[index]
                               : main_entry ? &main_entry->value
                                            : NULL;
      const AbManifestFile *file = NULL;
      if (target)
        status = resolve_npm_entry(state, package, target, &file);
      if (file && file->has_layer)
        status = append_unique_string(state->engine, entries, file->path.data,
                                      file->path.length);
    }
  }
  if (status == ARCHBIRD_OK && entries->count > 1)
    qsort(entries->items, entries->count, sizeof(*entries->items),
          string_compare);
  return status;
}

static int fact_domain(const AbFact *fact, const char *domain) {
  size_t length = strlen(domain);
  return fact->domain.length == length &&
         memcmp(fact->domain.data, domain, length) == 0;
}

static int entry_defines_export(const AbMapState *state, const AbString *entry,
                                const AbString *name) {
  size_t start;
  size_t end;
  size_t index;
  ab_project_merged_fact_range(state->project, entry, "symbols", &start, &end);
  for (index = start; index < end; index++) {
    const AbFact *fact = ab_project_merged_fact_by_path(state->project, index);
    if (!fact->has_name || !ab_string_equal(&fact->name, name) ||
        string_literal(&fact->kind, "declaration"))
      continue;
    return 1;
  }
  return 0;
}

static ArchbirdStatus ensure_export_origin(AbMapState *state,
                                           AbMapPackage *package,
                                           const AbString *name,
                                           AbMapExportOrigin **out) {
  AbMapExportOrigin *resized;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < package->export_origin_count; index++) {
    if (ab_string_equal(&package->export_origins[index].name, name)) {
      *out = &package->export_origins[index];
      return ARCHBIRD_OK;
    }
  }
  if (package->export_origin_count ==
      SIZE_MAX / sizeof(*package->export_origins))
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many package export origins");
  resized = (AbMapExportOrigin *)ab_realloc(
      state->engine, package->export_origins,
      (package->export_origin_count + 1) * sizeof(*package->export_origins));
  if (!resized)
    return archbird_error_set(
        state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory collecting package export origins");
  package->export_origins = resized;
  *out = &package->export_origins[package->export_origin_count];
  memset(*out, 0, sizeof(**out));
  status = copy_string(state->engine, &(*out)->name, name);
  if (status == ARCHBIRD_OK)
    package->export_origin_count++;
  return status;
}

static int export_origin_compare(const void *left_raw, const void *right_raw) {
  const AbMapExportOrigin *left = (const AbMapExportOrigin *)left_raw;
  const AbMapExportOrigin *right = (const AbMapExportOrigin *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus collect_file_exports(AbMapState *state,
                                           AbMapPackage *package,
                                           const AbManifestFile *file,
                                           const AbManifestFile **stack,
                                           size_t depth, int include_default) {
  size_t fact_index;
  size_t stack_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (stack_index = 0; stack_index < depth; stack_index++)
    if (stack[stack_index] == file)
      return ARCHBIRD_OK;
  if (depth >= state->manifest->file_count)
    return archbird_error_set(state->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "package re-export traversal exceeded files");
  stack[depth++] = file;
  for (fact_index = 0;
       status == ARCHBIRD_OK &&
       fact_index < ab_project_merged_fact_count(state->project);
       fact_index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
    AbMapExportOrigin *origin_row;
    const AbString *origin;
    const AbString *origin_name;
    const AbManifestFile *target = NULL;
    uint64_t local_definition = 0;
    if (!ab_string_equal(&fact->path, &file->path))
      continue;
    if (fact_domain(fact, "module-surface-unknowns")) {
      char message[384];
      snprintf(message, sizeof(message),
               "package %.*s export surface is incomplete: %.*s",
               (int)package->name.length, package->name.data,
               fact->has_name ? (int)fact->name.length : 7,
               fact->has_name ? fact->name.data : "unknown");
      status = ab_map_add_diagnostic_span(
          state, "warning", "package-export-surface-partial", message,
          &file->path, fact->span_start, fact->span_end);
      package->export_surface_partial = 1;
      continue;
    }
    if (fact_domain(fact, "module-reexports") && fact->has_name) {
      status = ab_map_resolve_import(state->engine, state->manifest,
                                     state->config, file, &fact->name, &target);
      if (status != ARCHBIRD_OK)
        break;
      if (target && target->has_layer) {
        status = collect_file_exports(
            state, package, target, stack, depth,
            include_default && !string_literal(&fact->kind, "esm-star"));
      } else {
        char message[384];
        snprintf(message, sizeof(message),
                 "package %.*s re-export %.*s cannot be resolved to a "
                 "mapped source file",
                 (int)package->name.length, package->name.data,
                 (int)fact->name.length, fact->name.data);
        status = ab_map_add_diagnostic_span(
            state, "warning", "package-reexport-unresolved", message,
            &file->path, fact->span_start, fact->span_end);
        package->export_surface_partial = 1;
      }
      continue;
    }
    if (!fact->has_name || !fact_domain(fact, "exports") ||
        (!include_default && string_literal(&fact->name, "default")))
      continue;
    status = append_unique_string(state->engine, &package->exports,
                                  fact->name.data, fact->name.length);
    if (status != ARCHBIRD_OK)
      break;
    status = ensure_export_origin(state, package, &fact->name, &origin_row);
    if (status != ARCHBIRD_OK)
      break;
    origin_name = fact_string_attribute(fact, "origin_name");
    if (origin_name && origin_name->length) {
      if (!origin_row->target_symbol.length &&
          !origin_row->target_symbol_ambiguous)
        status =
            copy_string(state->engine, &origin_row->target_symbol, origin_name);
      else if (!origin_row->target_symbol_ambiguous &&
               !ab_string_equal(&origin_row->target_symbol, origin_name)) {
        char message[384];
        snprintf(message, sizeof(message),
                 "package %.*s export %.*s has multiple target symbols",
                 (int)package->name.length, package->name.data,
                 (int)fact->name.length, fact->name.data);
        ab_string_free(state->engine, &origin_row->target_symbol);
        origin_row->target_symbol_ambiguous = 1;
        status = ab_map_add_diagnostic_span(
            state, "warning", "package-export-origin-ambiguous", message,
            &file->path, fact->span_start, fact->span_end);
      }
    }
    if (status != ARCHBIRD_OK)
      break;
    origin = fact_string_attribute(fact, "origin");
    if (origin && origin->length) {
      status = ab_map_resolve_import(state->engine, state->manifest,
                                     state->config, file, origin, &target);
      if (status != ARCHBIRD_OK)
        break;
    }
    (void)fact_integer_attribute(fact, "local_definition", &local_definition);
    if (target || local_definition ||
        entry_defines_export(state, &file->path, &fact->name))
      status = append_unique_string(
          state->engine, &origin_row->paths,
          target ? target->path.data : file->path.data,
          target ? target->path.length : file->path.length);
  }
  return status;
}

static ArchbirdStatus collect_exports(AbMapState *state, AbMapPackage *package,
                                      const AbStringArray *entries) {
  size_t entry_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  const AbManifestFile **stack = NULL;
  if (state->manifest->file_count) {
    stack = (const AbManifestFile **)ab_calloc(
        state->engine, state->manifest->file_count, sizeof(*stack));
    if (!stack)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory traversing package re-exports");
  }
  for (entry_index = 0; status == ARCHBIRD_OK && entry_index < entries->count;
       entry_index++) {
    const AbString *entry = &entries->items[entry_index];
    const AbManifestFile *file = mapped_file(state, entry, NULL);
    AbMapEntrypointSurface *resized;
    AbMapEntrypointSurface *surface;
    AbMapPackage collected = {0};
    size_t export_index;
    if (!file || !file->has_layer) {
      char message[256];
      snprintf(message, sizeof(message),
               "package %.*s entry is not in a source layer",
               (int)package->name.length, package->name.data);
      status = ab_map_add_diagnostic(state, "warning", "package-entry-unmapped",
                                     message, entry);
      continue;
    }
    if (package->entrypoint_surface_count ==
        SIZE_MAX / sizeof(*package->entrypoint_surfaces)) {
      status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "too many package entrypoint surfaces");
      break;
    }
    resized = (AbMapEntrypointSurface *)ab_realloc(
        state->engine, package->entrypoint_surfaces,
        (package->entrypoint_surface_count + 1) *
            sizeof(*package->entrypoint_surfaces));
    if (!resized) {
      status = archbird_error_set(
          state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory collecting package entrypoint surfaces");
      break;
    }
    package->entrypoint_surfaces = resized;
    surface = &package->entrypoint_surfaces[package->entrypoint_surface_count];
    memset(surface, 0, sizeof(*surface));
    status = copy_string(state->engine, &surface->path, entry);
    if (status != ARCHBIRD_OK)
      break;
    package->entrypoint_surface_count++;
    collected.name = package->name;
    status = collect_file_exports(state, &collected, file, stack, 0, 1);
    surface->exports = collected.exports;
    surface->export_origins = collected.export_origins;
    surface->export_origin_count = collected.export_origin_count;
    surface->partial = collected.export_surface_partial;
    package->export_surface_partial |= collected.export_surface_partial;
    if (status != ARCHBIRD_OK)
      break;
    if (surface->exports.count > 1)
      qsort(surface->exports.items, surface->exports.count,
            sizeof(*surface->exports.items), string_compare);
    if (surface->export_origin_count > 1)
      qsort(surface->export_origins, surface->export_origin_count,
            sizeof(*surface->export_origins), export_origin_compare);
    for (export_index = 0;
         status == ARCHBIRD_OK && export_index < surface->export_origin_count;
         export_index++) {
      AbMapExportOrigin *source = &surface->export_origins[export_index];
      AbMapExportOrigin *target = NULL;
      size_t path_index;
      if (source->paths.count > 1)
        qsort(source->paths.items, source->paths.count,
              sizeof(*source->paths.items), string_compare);
      status = append_unique_string(state->engine, &package->exports,
                                    source->name.data, source->name.length);
      if (status == ARCHBIRD_OK)
        status = ensure_export_origin(state, package, &source->name, &target);
      if (status == ARCHBIRD_OK && source->target_symbol.length &&
          !target->target_symbol.length && !target->target_symbol_ambiguous)
        status = copy_string(state->engine, &target->target_symbol,
                             &source->target_symbol);
      else if (status == ARCHBIRD_OK && source->target_symbol.length &&
               target->target_symbol.length &&
               !ab_string_equal(&source->target_symbol,
                                &target->target_symbol)) {
        char message[384];
        snprintf(message, sizeof(message),
                 "package %.*s export %.*s has multiple target symbols",
                 (int)package->name.length, package->name.data,
                 (int)source->name.length, source->name.data);
        ab_string_free(state->engine, &target->target_symbol);
        target->target_symbol_ambiguous = 1;
        status = ab_map_add_diagnostic(state, "warning",
                                       "package-export-origin-ambiguous",
                                       message, &surface->path);
      }
      if (status == ARCHBIRD_OK && source->target_symbol_ambiguous) {
        ab_string_free(state->engine, &target->target_symbol);
        target->target_symbol_ambiguous = 1;
      }
      for (path_index = 0;
           status == ARCHBIRD_OK && path_index < source->paths.count;
           path_index++)
        status = append_unique_string(state->engine, &target->paths,
                                      source->paths.items[path_index].data,
                                      source->paths.items[path_index].length);
    }
  }
  for (entry_index = 0;
       status == ARCHBIRD_OK && entry_index < package->exports.count;
       entry_index++) {
    AbMapExportOrigin *row = NULL;
    status = ensure_export_origin(state, package,
                                  &package->exports.items[entry_index], &row);
  }
  if (status == ARCHBIRD_OK && package->exports.count > 1)
    qsort(package->exports.items, package->exports.count,
          sizeof(*package->exports.items), string_compare);
  if (status == ARCHBIRD_OK && package->export_origin_count > 1)
    qsort(package->export_origins, package->export_origin_count,
          sizeof(*package->export_origins), export_origin_compare);
  for (entry_index = 0;
       status == ARCHBIRD_OK && entry_index < package->export_origin_count;
       entry_index++) {
    AbStringArray *paths = &package->export_origins[entry_index].paths;
    if (paths->count > 1)
      qsort(paths->items, paths->count, sizeof(*paths->items), string_compare);
  }
  ab_free(state->engine, stack);
  return status;
}

static int package_compare(const void *left_raw, const void *right_raw) {
  const AbMapPackage *left = (const AbMapPackage *)left_raw;
  const AbMapPackage *right = (const AbMapPackage *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

ArchbirdStatus ab_map_analyze_packages(AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!state || !state->engine || !state->project || !state->manifest ||
      !state->config)
    return ARCHBIRD_INVALID_ARGUMENT;
  state->package_count = state->config->package_count;
  if (state->package_count) {
    state->packages = (AbMapPackage *)ab_calloc(
        state->engine, state->package_count, sizeof(*state->packages));
    if (!state->packages)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting package surfaces");
  }
  for (index = 0; status == ARCHBIRD_OK && index < state->package_count;
       index++) {
    const AbConfigPackage *config = &state->config->packages[index];
    AbMapPackage *package = &state->packages[index];
    AbStringArray entries = {0};
    size_t entry;
    status = package_config_values(state, config, package);
    if (status == ARCHBIRD_OK)
      status = analyze_manifest(state, config, package);
    if (status == ARCHBIRD_OK && !package->identity.length)
      status = copy_string(state->engine, &package->identity, &config->name);
    if (status == ARCHBIRD_OK)
      status = append_unique_string(state->engine, &package->aliases,
                                    package->identity.data,
                                    package->identity.length);
    if (status == ARCHBIRD_OK)
      status = collect_entries(state, config, package, &entries);
    for (entry = 0; status == ARCHBIRD_OK && entry < entries.count; entry++) {
      char key[64];
      int key_length = snprintf(key, sizeof(key), "configured:%zu", entry);
      if (key_length < 0 || (size_t)key_length >= sizeof(key))
        status = archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                                    ARCHBIRD_NO_OFFSET,
                                    "cannot format package entrypoint key");
      else
        status = append_unique_pair(
            state->engine, &package->entrypoints, &package->entrypoint_count,
            key, (size_t)key_length, entries.items[entry].data,
            entries.items[entry].length, 1);
    }
    if (status == ARCHBIRD_OK)
      status = collect_exports(state, package, &entries);
    for (entry = 0; entry < entries.count; entry++)
      ab_string_free(state->engine, &entries.items[entry]);
    ab_free(state->engine, entries.items);
    if (status == ARCHBIRD_OK && package->aliases.count > 1)
      qsort(package->aliases.items, package->aliases.count,
            sizeof(*package->aliases.items), string_compare);
    if (status == ARCHBIRD_OK && package->dependency_count > 1)
      qsort(package->dependencies, package->dependency_count,
            sizeof(*package->dependencies), dependency_compare);
    if (status == ARCHBIRD_OK && package->entrypoint_count > 1)
      qsort(package->entrypoints, package->entrypoint_count,
            sizeof(*package->entrypoints), pair_compare);
    if (status == ARCHBIRD_OK && package->script_count > 1)
      qsort(package->scripts, package->script_count, sizeof(*package->scripts),
            pair_compare);
  }
  if (status == ARCHBIRD_OK && state->package_count > 1)
    qsort(state->packages, state->package_count, sizeof(*state->packages),
          package_compare);
  return status;
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

ArchbirdStatus ab_map_render_packages(AbBuffer *buffer,
                                      const AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->package_count;
       index++) {
    const AbMapPackage *package = &state->packages[index];
    size_t nested;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"aliases\":[");
    for (nested = 0; status == ARCHBIRD_OK && nested < package->aliases.count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->aliases.items[nested]);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"dependencies\":[");
    for (nested = 0;
         status == ARCHBIRD_OK && nested < package->dependency_count;
         nested++) {
      const AbMapDependency *dependency = &package->dependencies[nested];
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"name\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &dependency->name);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"requirement\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &dependency->requirement);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"scope\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &dependency->scope);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"entrypoints\":{");
    for (nested = 0;
         status == ARCHBIRD_OK && nested < package->entrypoint_count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->entrypoints[nested].key);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->entrypoints[nested].value);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"entrypoint_surfaces\":[");
    for (nested = 0;
         status == ARCHBIRD_OK && nested < package->entrypoint_surface_count;
         nested++) {
      const AbMapEntrypointSurface *surface =
          &package->entrypoint_surfaces[nested];
      size_t item;
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"evidence_state\":\"");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer,
                                   surface->partial ? "partial" : "complete");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "\",\"export_origins\":{");
      for (item = 0;
           status == ARCHBIRD_OK && item < surface->export_origin_count;
           item++) {
        const AbMapExportOrigin *origin = &surface->export_origins[item];
        size_t path;
        if (item)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &origin->name);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ":[");
        for (path = 0; status == ARCHBIRD_OK && path < origin->paths.count;
             path++) {
          if (path)
            status = ab_buffer_literal(buffer, ",");
          if (status == ARCHBIRD_OK)
            status = json_string(buffer, &origin->paths.items[path]);
        }
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "]");
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "},\"exports\":[");
      for (item = 0; status == ARCHBIRD_OK && item < surface->exports.count;
           item++) {
        if (item)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &surface->exports.items[item]);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "],\"path\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &surface->path);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"export_origins\":{");
    for (nested = 0;
         status == ARCHBIRD_OK && nested < package->export_origin_count;
         nested++) {
      const AbMapExportOrigin *origin = &package->export_origins[nested];
      size_t path;
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &origin->name);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":[");
      for (path = 0; status == ARCHBIRD_OK && path < origin->paths.count;
           path++) {
        if (path)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &origin->paths.items[path]);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "]");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"exports\":[");
    for (nested = 0; status == ARCHBIRD_OK && nested < package->exports.count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->exports.items[nested]);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"identity\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->identity);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"layer\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->layer);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"manifest\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->manifest);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"scripts\":{");
    for (nested = 0; status == ARCHBIRD_OK && nested < package->script_count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->scripts[nested].key);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &package->scripts[nested].value);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"version\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &package->version);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
