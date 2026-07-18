#include "config_resolution.h"

#include "archbird_internal.h"
#include "config.h"
#include "gitignore.h"
#include "json_value.h"
#include "lexical/tokenizer.h"
#include "path_match.h"
#include "python_manifest.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ResolutionRequest {
  const AbValue *project;
  const AbValue *sources;
  const AbValue *only;
  const AbValue *exclude;
  const AbValue *ignore_files;
  size_t max_file_bytes;
  size_t max_index_bytes;
  int has_max_file_bytes;
  int has_max_index_bytes;
  int use_default_excludes;
  int use_ignore_files;
} ResolutionRequest;

typedef struct InventoryFile {
  const AbString *path;
  size_t bytes;
} InventoryFile;

typedef struct InventoryIgnore {
  const AbValue *row;
  const AbString *path;
  size_t depth;
  size_t basename_priority;
  size_t original_index;
  int custom;
} InventoryIgnore;

typedef struct ResolutionDiagnostic {
  const char *code;
  const char *severity;
  AbString path;
  size_t bytes;
  size_t limit;
} ResolutionDiagnostic;

typedef struct ResolutionState {
  ArchbirdEngine *engine;
  int configured;
  ResolutionRequest request;
  InventoryFile *files;
  size_t file_count;
  AbIgnoreSet ignores;
  AbValue request_document;
  AbValue inventory_document;
  AbValue effective;
  AbValue plan;
  ResolutionDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t ignored_count;
  size_t oversized_count;
  size_t unsupported_count;
  size_t asset_count;
  const AbValue *pruned_directories;
  int has_package_json;
  int has_pyproject;
  int has_description;
  int has_c_translation_unit;
  int has_cpp_translation_unit;
  /* 1 = package.json, 2 = pyproject.toml, 3 = DESCRIPTION. */
  int package_identity;
} ResolutionState;

static const char default_config[] =
    "{\"layers\":["
    "{\"globs\":[\"**/*.c\",\"**/"
    "*.h\"],\"language\":\"c\",\"name\":\"auto-c\",\"required\":false},"
    "{\"globs\":[\"**/*.cc\",\"**/*.cpp\",\"**/*.cxx\",\"**/*.hh\",\"**/"
    "*.hpp\",\"**/"
    "*.hxx\"],\"language\":\"cpp\",\"name\":\"auto-cpp\",\"required\":false},"
    "{\"globs\":[\"**/*.py\",\"**/*.pyi\",\"**/"
    "*.pyw\"],\"language\":\"python\",\"name\":\"auto-python\",\"required\":"
    "false},"
    "{\"globs\":[\"**/*.js\",\"**/*.mjs\",\"**/*.cjs\",\"**/"
    "*.jsx\"],\"language\":\"javascript\",\"name\":\"auto-javascript\","
    "\"required\":false},"
    "{\"globs\":[\"**/*.ts\",\"**/*.mts\",\"**/*.cts\",\"**/"
    "*.tsx\"],\"language\":\"typescript\",\"name\":\"auto-typescript\","
    "\"required\":false},"
    "{\"globs\":[\"**/"
    "*.vue\"],\"language\":\"vue\",\"name\":\"auto-vue\",\"required\":false},"
    "{\"globs\":[\"**/*.R\",\"**/"
    "*.r\",\"NAMESPACE\"],\"language\":\"r\",\"name\":\"auto-r\","
    "\"required\":false}"
    "],\"project\":\"repository\",\"schema_version\":1}";

static int field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int value_string_compare(const void *left_raw, const void *right_raw) {
  const AbValue *left = (const AbValue *)left_raw;
  const AbValue *right = (const AbValue *)right_raw;
  return ab_string_compare(&left->as.text, &right->as.text);
}

static AbObjectField *mutable_member(AbValue *object, const char *name) {
  size_t index;
  size_t length = strlen(name);
  if (!object || object->kind != AB_VALUE_OBJECT)
    return NULL;
  for (index = 0; index < object->as.object.count; index++) {
    AbObjectField *field = &object->as.object.fields[index];
    if (field->name.length == length && !memcmp(field->name.data, name, length))
      return field;
  }
  return NULL;
}

static ArchbirdStatus value_string(ArchbirdEngine *engine, AbValue *out,
                                   const char *data, size_t length) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_STRING;
  return ab_string_copy(engine, &out->as.text, data, length);
}

static ArchbirdStatus value_integer(ArchbirdEngine *engine, AbValue *out,
                                    size_t number) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%zu", number);
  if (length < 0 || (size_t)length >= sizeof(text))
    return ARCHBIRD_LIMIT_EXCEEDED;
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_INTEGER;
  return ab_string_copy(engine, &out->as.text, text, (size_t)length);
}

static ArchbirdStatus object_set(ArchbirdEngine *engine, AbValue *object,
                                 const char *name, AbValue *value) {
  AbObjectField *field = mutable_member(object, name);
  AbObjectField *resized;
  ArchbirdStatus status;
  if (!object || object->kind != AB_VALUE_OBJECT || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (field) {
    ab_value_free(engine, &field->value);
    field->value = *value;
    memset(value, 0, sizeof(*value));
    return ARCHBIRD_OK;
  }
  if (object->as.object.count == SIZE_MAX / sizeof(*object->as.object.fields))
    return ARCHBIRD_LIMIT_EXCEEDED;
  resized = (AbObjectField *)ab_realloc(engine, object->as.object.fields,
                                        (object->as.object.count + 1) *
                                            sizeof(*object->as.object.fields));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory extending resolution object");
  object->as.object.fields = resized;
  field = &object->as.object.fields[object->as.object.count];
  memset(field, 0, sizeof(*field));
  status = ab_string_copy(engine, &field->name, name, strlen(name));
  if (status != ARCHBIRD_OK)
    return status;
  field->value = *value;
  memset(value, 0, sizeof(*value));
  object->as.object.count++;
  if (object->as.object.count > 1)
    qsort(object->as.object.fields, object->as.object.count,
          sizeof(*object->as.object.fields), field_compare);
  return ARCHBIRD_OK;
}

static ArchbirdStatus object_set_string(ArchbirdEngine *engine, AbValue *object,
                                        const char *name, const char *data,
                                        size_t length) {
  AbValue value = {0};
  ArchbirdStatus status = value_string(engine, &value, data, length);
  if (status == ARCHBIRD_OK)
    status = object_set(engine, object, name, &value);
  ab_value_free(engine, &value);
  return status;
}

static ArchbirdStatus object_set_bool(ArchbirdEngine *engine, AbValue *object,
                                      const char *name, int boolean) {
  AbValue value = {0};
  value.kind = AB_VALUE_BOOL;
  value.as.boolean = boolean;
  return object_set(engine, object, name, &value);
}

static ArchbirdStatus object_set_integer(ArchbirdEngine *engine,
                                         AbValue *object, const char *name,
                                         size_t number) {
  AbValue value = {0};
  ArchbirdStatus status = value_integer(engine, &value, number);
  if (status == ARCHBIRD_OK)
    status = object_set(engine, object, name, &value);
  ab_value_free(engine, &value);
  return status;
}

static ArchbirdStatus array_append(ArchbirdEngine *engine, AbValue *array,
                                   AbValue *value) {
  AbValue *resized;
  if (!array || array->kind != AB_VALUE_ARRAY || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (array->as.array.count == SIZE_MAX / sizeof(*array->as.array.items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  resized = (AbValue *)ab_realloc(engine, array->as.array.items,
                                  (array->as.array.count + 1) *
                                      sizeof(*array->as.array.items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory extending resolution array");
  array->as.array.items = resized;
  array->as.array.items[array->as.array.count++] = *value;
  memset(value, 0, sizeof(*value));
  return ARCHBIRD_OK;
}

static ArchbirdStatus array_append_string(ArchbirdEngine *engine,
                                          AbValue *array, const char *data,
                                          size_t length) {
  AbValue value = {0};
  ArchbirdStatus status = value_string(engine, &value, data, length);
  if (status == ARCHBIRD_OK)
    status = array_append(engine, array, &value);
  ab_value_free(engine, &value);
  return status;
}

static int allowed_fields(const AbValue *object, const char *const *names,
                          size_t name_count) {
  size_t field_index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (field_index = 0; field_index < object->as.object.count; field_index++) {
    const AbString *name = &object->as.object.fields[field_index].name;
    size_t index;
    int found = 0;
    for (index = 0; index < name_count; index++) {
      size_t length = strlen(names[index]);
      if (name->length == length && !memcmp(name->data, names[index], length)) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

static int nonblank_string(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static int portable_project_name(const char *data, size_t length,
                                 const char **out_data, size_t *out_length) {
  size_t start = 0;
  size_t index;
  for (index = 0; index < length; index++) {
    if (data[index] == '/')
      start = index + 1;
  }
  if (start == length)
    return 0;
  if (!((data[start] >= 'A' && data[start] <= 'Z') ||
        (data[start] >= 'a' && data[start] <= 'z') ||
        (data[start] >= '0' && data[start] <= '9')))
    return 0;
  for (index = start + 1; index < length; index++) {
    char byte = data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' ||
          byte == ':' || byte == '-'))
      return 0;
  }
  *out_data = data + start;
  *out_length = length - start;
  return 1;
}

static int string_has_suffix(const AbString *value, const char *suffix) {
  size_t length = strlen(suffix);
  return value->length >= length &&
         !memcmp(value->data + value->length - length, suffix, length);
}

static int string_array(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    if (!nonblank_string(&value->as.array.items[index]))
      return 0;
  }
  return 1;
}

static int language_supported(const AbValue *value) {
  static const char *const languages[] = {
      "c", "cpp", "python", "javascript", "typescript", "vue", "r"};
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING)
    return 0;
  for (index = 0; index < sizeof(languages) / sizeof(languages[0]); index++) {
    if (ab_value_string_is(value, languages[index]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus request_error(ArchbirdEngine *engine,
                                    const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "map request: %s", message);
}

static int repository_path_valid(const AbString *path);

static ArchbirdStatus decode_request(ResolutionState *state,
                                     const uint8_t *json, size_t json_length) {
  static const char *const fields[] = {
      "artifact",     "default_excludes", "exclude",         "ignore",
      "ignore_files", "max_file_bytes",   "max_index_bytes", "only",
      "project",      "schema_version",   "sources"};
  static const char *const source_fields[] = {"glob", "language"};
  AbValue *root = &state->request_document;
  const AbValue *artifact;
  const AbValue *schema;
  const AbValue *boolean;
  uint64_t version;
  size_t index;
  ArchbirdStatus status =
      ab_json_value_decode(state->engine, json, json_length, root);
  if (status != ARCHBIRD_OK)
    return status;
  artifact = ab_value_member(root, "artifact");
  schema = ab_value_member(root, "schema_version");
  if (!allowed_fields(root, fields, sizeof(fields) / sizeof(fields[0])) ||
      !ab_value_string_is(artifact, "archbird-map-request") ||
      !ab_value_u64(schema, &version) || version != 1) {
    return request_error(state->engine, "invalid artifact or fields");
  }
  state->request.project = ab_value_member(root, "project");
  state->request.sources = ab_value_member(root, "sources");
  state->request.only = ab_value_member(root, "only");
  state->request.exclude = ab_value_member(root, "exclude");
  state->request.ignore_files = ab_value_member(root, "ignore_files");
  state->request.use_default_excludes = 1;
  state->request.use_ignore_files = 1;
  if (state->request.project && !nonblank_string(state->request.project))
    status = request_error(state->engine, "project must be a nonblank string");
  if (status == ARCHBIRD_OK && state->request.only &&
      !string_array(state->request.only))
    status = request_error(state->engine, "only must be a string array");
  if (status == ARCHBIRD_OK && state->request.exclude &&
      !string_array(state->request.exclude))
    status = request_error(state->engine, "exclude must be a string array");
  if (status == ARCHBIRD_OK && state->request.ignore_files &&
      !string_array(state->request.ignore_files))
    status =
        request_error(state->engine, "ignore_files must be a string array");
  for (index = 0; status == ARCHBIRD_OK && state->request.ignore_files &&
                  index < state->request.ignore_files->as.array.count;
       index++) {
    if (!repository_path_valid(
            &state->request.ignore_files->as.array.items[index].as.text))
      status = request_error(
          state->engine,
          "ignore_files paths must be canonical and repository-relative");
  }
  if (status == ARCHBIRD_OK && state->request.sources) {
    if (state->request.sources->kind != AB_VALUE_ARRAY)
      status = request_error(state->engine, "sources must be an array");
    for (index = 0; status == ARCHBIRD_OK &&
                    index < state->request.sources->as.array.count;
         index++) {
      const AbValue *row = &state->request.sources->as.array.items[index];
      if (!allowed_fields(row, source_fields, 2) ||
          !language_supported(ab_value_member(row, "language")) ||
          !nonblank_string(ab_value_member(row, "glob")))
        status = request_error(
            state->engine,
            "sources rows require supported language and nonblank glob");
    }
  }
  boolean = ab_value_member(root, "default_excludes");
  if (status == ARCHBIRD_OK && boolean) {
    if (boolean->kind != AB_VALUE_BOOL)
      status = request_error(state->engine, "default_excludes must be boolean");
    else
      state->request.use_default_excludes = boolean->as.boolean;
  }
  boolean = ab_value_member(root, "ignore");
  if (status == ARCHBIRD_OK && boolean) {
    if (boolean->kind != AB_VALUE_BOOL)
      status = request_error(state->engine, "ignore must be boolean");
    else
      state->request.use_ignore_files = boolean->as.boolean;
  }
  if (status == ARCHBIRD_OK) {
    const AbValue *limit = ab_value_member(root, "max_file_bytes");
    uint64_t number;
    if (limit) {
      if (!ab_value_u64(limit, &number) || !number || number > SIZE_MAX)
        status = request_error(
            state->engine,
            "max_file_bytes must be a positive platform-sized integer");
      else {
        state->request.max_file_bytes = (size_t)number;
        state->request.has_max_file_bytes = 1;
      }
    }
  }
  if (status == ARCHBIRD_OK) {
    const AbValue *limit = ab_value_member(root, "max_index_bytes");
    uint64_t number;
    if (limit) {
      if (!ab_value_u64(limit, &number) || !number || number > SIZE_MAX)
        status = request_error(
            state->engine,
            "max_index_bytes must be a positive platform-sized integer");
      else {
        state->request.max_index_bytes = (size_t)number;
        state->request.has_max_index_bytes = 1;
      }
    }
  }
  return status;
}

static int repository_path_valid(const AbString *path) {
  size_t segment = 0;
  size_t index;
  if (!path || !path->length || path->data[0] == '/' ||
      path->data[path->length - 1] == '/')
    return 0;
  for (index = 0; index <= path->length; index++) {
    if (index < path->length && path->data[index] != '/') {
      if (path->data[index] == '\\' || path->data[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment ||
        (index - segment == 1 && path->data[segment] == '.') ||
        (index - segment == 2 && path->data[segment] == '.' &&
         path->data[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static int inventory_compare(const void *left_raw, const void *right_raw) {
  const InventoryFile *left = (const InventoryFile *)left_raw;
  const InventoryFile *right = (const InventoryFile *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static size_t ignore_path_depth(const AbString *path) {
  size_t index;
  size_t depth = 0;
  for (index = 0; index < path->length; index++)
    depth += path->data[index] == '/';
  return depth;
}

static size_t ignore_basename_priority(const AbString *path) {
  const char *leaf = path->data;
  size_t length = path->length;
  size_t index;
  for (index = path->length; index; index--)
    if (path->data[index - 1] == '/') {
      leaf = path->data + index;
      length = path->length - index;
      break;
    }
  if (length == 10 && !memcmp(leaf, ".gitignore", 10))
    return 0;
  if (length == 7 && !memcmp(leaf, ".ignore", 7))
    return 1;
  if (length == 15 && !memcmp(leaf, ".archbirdignore", 15))
    return 2;
  return 3;
}

static int inventory_ignore_compare(const void *left_raw,
                                    const void *right_raw) {
  const InventoryIgnore *left = (const InventoryIgnore *)left_raw;
  const InventoryIgnore *right = (const InventoryIgnore *)right_raw;
  if (left->custom != right->custom)
    return left->custom ? 1 : -1;
  if (left->custom && left->original_index != right->original_index)
    return left->original_index < right->original_index ? -1 : 1;
  if (left->depth != right->depth)
    return left->depth < right->depth ? -1 : 1;
  {
    size_t left_base = left->path->length;
    size_t right_base = right->path->length;
    AbString left_directory;
    AbString right_directory;
    while (left_base && left->path->data[left_base - 1] != '/')
      left_base--;
    while (right_base && right->path->data[right_base - 1] != '/')
      right_base--;
    left_directory.data = left->path->data;
    left_directory.length = left_base;
    right_directory.data = right->path->data;
    right_directory.length = right_base;
    {
      int compared = ab_string_compare(&left_directory, &right_directory);
      if (compared)
        return compared;
    }
  }
  if (left->basename_priority != right->basename_priority)
    return left->basename_priority < right->basename_priority ? -1 : 1;
  return ab_string_compare(left->path, right->path);
}

static int request_ignore_position(const ResolutionState *state,
                                   const AbString *path, size_t *out_index) {
  size_t index;
  if (!state->request.ignore_files)
    return 0;
  for (index = 0; index < state->request.ignore_files->as.array.count; index++)
    if (ab_string_equal(
            &state->request.ignore_files->as.array.items[index].as.text,
            path)) {
      *out_index = index;
      return 1;
    }
  return 0;
}

static int ignored_parent(ResolutionState *state, const AbString *path) {
  size_t length = path->length;
  AbString parent;
  while (length && path->data[length - 1] != '/')
    length--;
  if (!length)
    return 0;
  parent.data = path->data;
  parent.length = length - 1;
  return ab_ignore_set_matches(&state->ignores, &parent, 1);
}

static int hex_nibble(unsigned char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  return -1;
}

static ArchbirdStatus decode_hex(ArchbirdEngine *engine, const AbValue *value,
                                 uint8_t **out, size_t *out_length) {
  size_t index;
  uint8_t *bytes;
  *out = NULL;
  *out_length = 0;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length % 2)
    return ARCHBIRD_INVALID_SCHEMA;
  bytes = (uint8_t *)ab_malloc(engine, value->as.text.length / 2 + 1);
  if (!bytes)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; index < value->as.text.length; index += 2) {
    int high = hex_nibble((unsigned char)value->as.text.data[index]);
    int low = hex_nibble((unsigned char)value->as.text.data[index + 1]);
    if (high < 0 || low < 0) {
      ab_free(engine, bytes);
      return ARCHBIRD_INVALID_SCHEMA;
    }
    bytes[index / 2] = (uint8_t)((high << 4) | low);
  }
  *out = bytes;
  *out_length = value->as.text.length / 2;
  return ARCHBIRD_OK;
}

static ArchbirdStatus inventory_error(ArchbirdEngine *engine,
                                      const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "repository inventory: %s", message);
}

static ArchbirdStatus parse_package_document(ResolutionState *state,
                                             const uint8_t *bytes,
                                             size_t length, AbString *name,
                                             AbString *version) {
  AbValue document = {0};
  const AbValue *value;
  ArchbirdStatus status =
      ab_json_value_decode(state->engine, bytes, length, &document);
  if (status != ARCHBIRD_OK)
    return inventory_error(state->engine,
                           "root package.json is not strict JSON");
  if (document.kind != AB_VALUE_OBJECT) {
    ab_value_free(state->engine, &document);
    return inventory_error(state->engine,
                           "root package.json must contain an object");
  }
  value = ab_value_member(&document, "name");
  if (value && nonblank_string(value))
    status = ab_string_copy(state->engine, name, value->as.text.data,
                            value->as.text.length);
  value = ab_value_member(&document, "version");
  if (status == ARCHBIRD_OK && value && value->kind == AB_VALUE_STRING)
    status = ab_string_copy(state->engine, version, value->as.text.data,
                            value->as.text.length);
  ab_value_free(state->engine, &document);
  return status;
}

static ArchbirdStatus parse_description_document(ResolutionState *state,
                                                 const uint8_t *bytes,
                                                 size_t length, AbString *name,
                                                 AbString *version) {
  size_t line_start = 0;
  ArchbirdStatus status = ab_utf8_validate(state->engine, bytes, length);
  while (status == ARCHBIRD_OK && line_start < length) {
    size_t line_end = line_start;
    size_t colon;
    size_t value_start;
    size_t value_end;
    while (line_end < length && bytes[line_end] != '\n')
      line_end++;
    colon = line_start;
    while (colon < line_end && bytes[colon] != ':')
      colon++;
    if (colon == line_end) {
      line_start = line_end + 1;
      continue;
    }
    value_start = colon + 1;
    value_end = line_end;
    while (value_start < value_end &&
           (bytes[value_start] == ' ' || bytes[value_start] == '\t'))
      value_start++;
    while (value_end > value_start &&
           (bytes[value_end - 1] == ' ' || bytes[value_end - 1] == '\t' ||
            bytes[value_end - 1] == '\r'))
      value_end--;
    if (!name->length && colon - line_start == 7 &&
        !memcmp(bytes + line_start, "Package", 7))
      status =
          ab_string_copy(state->engine, name, (const char *)bytes + value_start,
                         value_end - value_start);
    else if (!version->length && colon - line_start == 7 &&
             !memcmp(bytes + line_start, "Version", 7))
      status = ab_string_copy(state->engine, version,
                              (const char *)bytes + value_start,
                              value_end - value_start);
    line_start = line_end + 1;
  }
  return status;
}

static ArchbirdStatus add_default_npm(ResolutionState *state,
                                      const AbString *name,
                                      const AbString *version) {
  AbValue packages = {0};
  AbValue builds = {0};
  AbValue package = {0};
  AbValue build = {0};
  AbObjectField *field;
  int had_packages;
  ArchbirdStatus status = ARCHBIRD_OK;
  field = mutable_member(&state->effective, "packages");
  had_packages = field != NULL;
  if (field) {
    packages = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    packages.kind = AB_VALUE_ARRAY;
  }
  field = mutable_member(&state->effective, "builds");
  if (field) {
    builds = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    builds.kind = AB_VALUE_ARRAY;
  }
  package.kind = AB_VALUE_OBJECT;
  build.kind = AB_VALUE_OBJECT;
  if (name->length)
    status = object_set_string(state->engine, &package, "identity", name->data,
                               name->length);
  if (status == ARCHBIRD_OK && name->length)
    status = object_set_string(state->engine, &package, "kind", "npm", 3);
  if (status == ARCHBIRD_OK && name->length)
    status = object_set_string(state->engine, &package, "layer",
                               "auto-javascript", 15);
  if (status == ARCHBIRD_OK && name->length)
    status = object_set_string(state->engine, &package, "name", "npm-root", 8);
  if (status == ARCHBIRD_OK && name->length)
    status =
        object_set_string(state->engine, &package, "path", "package.json", 12);
  if (status == ARCHBIRD_OK && name->length && version->length)
    status = object_set_string(state->engine, &package, "version",
                               version->data, version->length);
  if (status == ARCHBIRD_OK && name->length)
    status = array_append(state->engine, &packages, &package);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "kind", "npm", 3);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "name", "npm", 3);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &build, "path", "package.json", 12);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &builds, &build);
  if (status == ARCHBIRD_OK && (had_packages || name->length))
    status =
        object_set(state->engine, &state->effective, "packages", &packages);
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "builds", &builds);
  ab_value_free(state->engine, &packages);
  ab_value_free(state->engine, &builds);
  ab_value_free(state->engine, &package);
  ab_value_free(state->engine, &build);
  return status;
}

static ArchbirdStatus add_default_python(ResolutionState *state,
                                         const AbPyprojectMetadata *metadata) {
  AbValue packages = {0};
  AbValue package = {0};
  AbValue aliases = {0};
  AbObjectField *field = mutable_member(&state->effective, "packages");
  ArchbirdStatus status = ARCHBIRD_OK;
  if (field) {
    packages = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    packages.kind = AB_VALUE_ARRAY;
  }
  package.kind = AB_VALUE_OBJECT;
  aliases.kind = AB_VALUE_ARRAY;
  status = object_set_string(state->engine, &package, "kind", "python", 6);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &package, "layer", "auto-python", 11);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &package, "name", "python-root", 11);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "path",
                               "pyproject.toml", 14);
  if (status == ARCHBIRD_OK && metadata->name.length)
    status = object_set_string(state->engine, &package, "identity",
                               metadata->name.data, metadata->name.length);
  if (status == ARCHBIRD_OK && metadata->version.length)
    status =
        object_set_string(state->engine, &package, "version",
                          metadata->version.data, metadata->version.length);
  if (status == ARCHBIRD_OK && metadata->module.length)
    status = array_append_string(state->engine, &aliases, metadata->module.data,
                                 metadata->module.length);
  if (status == ARCHBIRD_OK && aliases.as.array.count)
    status = object_set(state->engine, &package, "aliases", &aliases);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &packages, &package);
  if (status == ARCHBIRD_OK)
    status =
        object_set(state->engine, &state->effective, "packages", &packages);
  ab_value_free(state->engine, &aliases);
  ab_value_free(state->engine, &package);
  ab_value_free(state->engine, &packages);
  return status;
}

static ArchbirdStatus add_default_r(ResolutionState *state,
                                    const AbString *name,
                                    const AbString *version) {
  AbValue packages = {0};
  AbValue package = {0};
  AbObjectField *field = mutable_member(&state->effective, "packages");
  ArchbirdStatus status = ARCHBIRD_OK;
  if (field) {
    packages = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    packages.kind = AB_VALUE_ARRAY;
  }
  package.kind = AB_VALUE_OBJECT;
  status = object_set_string(state->engine, &package, "identity", name->data,
                             name->length);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "kind", "r", 1);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "layer", "auto-r", 6);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "name", "r-root", 6);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &package, "path", "DESCRIPTION", 11);
  if (status == ARCHBIRD_OK && version->length)
    status = object_set_string(state->engine, &package, "version",
                               version->data, version->length);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &packages, &package);
  if (status == ARCHBIRD_OK)
    status =
        object_set(state->engine, &state->effective, "packages", &packages);
  ab_value_free(state->engine, &package);
  ab_value_free(state->engine, &packages);
  return status;
}

static ArchbirdStatus add_default_make(ResolutionState *state) {
  AbObjectField *field = mutable_member(&state->effective, "builds");
  AbValue builds = {0};
  AbValue build = {0};
  ArchbirdStatus status;
  if (field) {
    builds = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    builds.kind = AB_VALUE_ARRAY;
  }
  build.kind = AB_VALUE_OBJECT;
  status = object_set_string(state->engine, &build, "kind", "make", 4);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "name", "make", 4);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "path", "Makefile", 8);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &builds, &build);
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "builds", &builds);
  ab_value_free(state->engine, &builds);
  ab_value_free(state->engine, &build);
  return status;
}

static ArchbirdStatus
decode_inventory(ResolutionState *state, const uint8_t *json,
                 size_t json_length, AbString *package,
                 AbString *package_version, AbPyprojectMetadata *pyproject,
                 AbString *r_package, AbString *r_version, int *has_make) {
  static const char *const fields[] = {
      "artifact",     "documents",          "files",
      "ignore_files", "pruned_directories", "schema_version"};
  static const char *const file_fields[] = {"bytes", "path"};
  static const char *const input_fields[] = {"content_hex", "path"};
  AbValue *root = &state->inventory_document;
  const AbValue *schema;
  const AbValue *files;
  const AbValue *ignores;
  const AbValue *documents;
  const AbValue *pruned;
  InventoryIgnore *ignore_inputs = NULL;
  uint64_t version;
  size_t index;
  ArchbirdStatus status =
      ab_json_value_decode(state->engine, json, json_length, root);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(root, "schema_version");
  files = ab_value_member(root, "files");
  ignores = ab_value_member(root, "ignore_files");
  documents = ab_value_member(root, "documents");
  pruned = ab_value_member(root, "pruned_directories");
  if (!allowed_fields(root, fields, 6) ||
      !ab_value_string_is(ab_value_member(root, "artifact"),
                          "archbird-repository-inventory") ||
      !ab_value_u64(schema, &version) || version != 1 || !files ||
      files->kind != AB_VALUE_ARRAY || !ignores ||
      ignores->kind != AB_VALUE_ARRAY || !documents ||
      documents->kind != AB_VALUE_ARRAY ||
      (pruned && pruned->kind != AB_VALUE_ARRAY)) {
    status = inventory_error(state->engine, "invalid artifact or fields");
    goto done;
  }
  state->pruned_directories = pruned;
  if (pruned) {
    for (index = 0; index < pruned->as.array.count; index++) {
      const AbValue *path = &pruned->as.array.items[index];
      if (!nonblank_string(path) || !repository_path_valid(&path->as.text) ||
          (index &&
           ab_string_compare(&pruned->as.array.items[index - 1].as.text,
                             &path->as.text) >= 0)) {
        status = inventory_error(
            state->engine,
            "pruned directories must be sorted unique repository paths");
        goto done;
      }
    }
  }
  state->file_count = files->as.array.count;
  if (state->file_count) {
    state->files = (InventoryFile *)ab_calloc(state->engine, state->file_count,
                                              sizeof(*state->files));
    if (!state->files) {
      status = ARCHBIRD_OUT_OF_MEMORY;
      goto done;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < state->file_count; index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *bytes = ab_value_member(row, "bytes");
    uint64_t number;
    if (!allowed_fields(row, file_fields, 2) || !nonblank_string(path) ||
        !repository_path_valid(&path->as.text) ||
        !ab_value_u64(bytes, &number) || number > SIZE_MAX) {
      status = inventory_error(state->engine, "invalid file row");
      break;
    }
    state->files[index].path = &path->as.text;
    state->files[index].bytes = (size_t)number;
    if (ab_value_string_is(path, "Makefile"))
      *has_make = 1;
    if (string_has_suffix(&path->as.text, ".c"))
      state->has_c_translation_unit = 1;
    if (string_has_suffix(&path->as.text, ".cc") ||
        string_has_suffix(&path->as.text, ".cpp") ||
        string_has_suffix(&path->as.text, ".cxx"))
      state->has_cpp_translation_unit = 1;
  }
  if (status == ARCHBIRD_OK && state->file_count > 1) {
    qsort(state->files, state->file_count, sizeof(*state->files),
          inventory_compare);
    for (index = 1; index < state->file_count; index++) {
      if (ab_string_equal(state->files[index - 1].path,
                          state->files[index].path)) {
        status = inventory_error(state->engine, "duplicate file path");
        break;
      }
    }
  }
  if (status == ARCHBIRD_OK && ignores->as.array.count) {
    ignore_inputs = (InventoryIgnore *)ab_calloc(
        state->engine, ignores->as.array.count, sizeof(*ignore_inputs));
    if (!ignore_inputs)
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  for (index = 0; status == ARCHBIRD_OK && index < ignores->as.array.count;
       index++) {
    const AbValue *row = &ignores->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    if (!allowed_fields(row, input_fields, 2) || !nonblank_string(path) ||
        !repository_path_valid(&path->as.text)) {
      status = inventory_error(state->engine, "invalid ignore-file row");
      break;
    }
    ignore_inputs[index].row = row;
    ignore_inputs[index].path = &path->as.text;
    ignore_inputs[index].depth = ignore_path_depth(&path->as.text);
    ignore_inputs[index].basename_priority =
        ignore_basename_priority(&path->as.text);
    ignore_inputs[index].original_index = index;
    ignore_inputs[index].custom = request_ignore_position(
        state, &path->as.text, &ignore_inputs[index].original_index);
  }
  if (status == ARCHBIRD_OK && ignores->as.array.count > 1)
    qsort(ignore_inputs, ignores->as.array.count, sizeof(*ignore_inputs),
          inventory_ignore_compare);
  for (index = 0; status == ARCHBIRD_OK && index < ignores->as.array.count;
       index++) {
    const InventoryIgnore *input = &ignore_inputs[index];
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (!input->custom) {
      status = ab_ignore_set_finalize(&state->ignores);
      if (status == ARCHBIRD_OK && ignored_parent(state, input->path))
        continue;
    }
    if (status == ARCHBIRD_OK)
      status =
          decode_hex(state->engine, ab_value_member(input->row, "content_hex"),
                     &bytes, &length);
    if (status == ARCHBIRD_OK)
      status = ab_ignore_set_add(&state->ignores, input->path->data,
                                 input->path->length, bytes, length);
    ab_free(state->engine, bytes);
  }
  for (index = 0; status == ARCHBIRD_OK && index < documents->as.array.count;
       index++) {
    const AbValue *row = &documents->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (!allowed_fields(row, input_fields, 2) || !nonblank_string(path) ||
        !repository_path_valid(&path->as.text)) {
      status = inventory_error(state->engine, "invalid root document row");
      break;
    }
    status = decode_hex(state->engine, ab_value_member(row, "content_hex"),
                        &bytes, &length);
    if (status == ARCHBIRD_OK && ab_value_string_is(path, "package.json")) {
      state->has_package_json = 1;
      status = parse_package_document(state, bytes, length, package,
                                      package_version);
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(path, "pyproject.toml")) {
      state->has_pyproject = 1;
      status = ab_pyproject_metadata(state->engine, bytes, length, pyproject);
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(path, "DESCRIPTION")) {
      state->has_description = 1;
      status = parse_description_document(state, bytes, length, r_package,
                                          r_version);
    }
    ab_free(state->engine, bytes);
  }
  if (status == ARCHBIRD_OK)
    status = ab_ignore_set_finalize(&state->ignores);
done:
  ab_free(state->engine, ignore_inputs);
  return status;
}

static ArchbirdStatus append_cli_sources(ResolutionState *state) {
  AbObjectField *field;
  AbValue original = {0};
  AbValue combined = {0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!state->request.sources || !state->request.sources->as.array.count)
    return ARCHBIRD_OK;
  field = mutable_member(&state->effective, "layers");
  if (!field || field->value.kind != AB_VALUE_ARRAY)
    return request_error(state->engine, "effective config has no layer array");
  original = field->value;
  memset(&field->value, 0, sizeof(field->value));
  combined.kind = AB_VALUE_ARRAY;
  for (index = state->request.sources->as.array.count;
       status == ARCHBIRD_OK && index; index--) {
    const AbValue *source = &state->request.sources->as.array.items[index - 1];
    const AbValue *language = ab_value_member(source, "language");
    const AbValue *glob = ab_value_member(source, "glob");
    AbValue layer = {0};
    AbValue globs = {0};
    char name[80];
    int name_length;
    globs.kind = AB_VALUE_ARRAY;
    layer.kind = AB_VALUE_OBJECT;
    name_length =
        snprintf(name, sizeof(name), "cli-%.*s-%04zu",
                 (int)language->as.text.length, language->as.text.data, index);
    if (name_length < 0 || (size_t)name_length >= sizeof(name))
      status = ARCHBIRD_LIMIT_EXCEEDED;
    if (status == ARCHBIRD_OK)
      status = array_append_string(state->engine, &globs, glob->as.text.data,
                                   glob->as.text.length);
    if (status == ARCHBIRD_OK)
      status = object_set(state->engine, &layer, "globs", &globs);
    if (status == ARCHBIRD_OK)
      status =
          object_set_string(state->engine, &layer, "language",
                            language->as.text.data, language->as.text.length);
    if (status == ARCHBIRD_OK)
      status = object_set_string(state->engine, &layer, "name", name,
                                 (size_t)name_length);
    if (status == ARCHBIRD_OK)
      status = object_set_bool(state->engine, &layer, "required", 1);
    if (status == ARCHBIRD_OK)
      status = array_append(state->engine, &combined, &layer);
    ab_value_free(state->engine, &globs);
    ab_value_free(state->engine, &layer);
  }
  for (index = 0; status == ARCHBIRD_OK && index < original.as.array.count;
       index++) {
    AbValue value = original.as.array.items[index];
    memset(&original.as.array.items[index], 0, sizeof(value));
    status = array_append(state->engine, &combined, &value);
    ab_value_free(state->engine, &value);
  }
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "layers", &combined);
  ab_value_free(state->engine, &original);
  ab_value_free(state->engine, &combined);
  return status;
}

static ArchbirdStatus apply_overlays(ResolutionState *state) {
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  if (state->request.project)
    status = object_set_string(state->engine, &state->effective, "project",
                               state->request.project->as.text.data,
                               state->request.project->as.text.length);
  if (status == ARCHBIRD_OK)
    status = append_cli_sources(state);
  if (status == ARCHBIRD_OK && state->request.exclude &&
      state->request.exclude->as.array.count) {
    AbObjectField *field = mutable_member(&state->effective, "exclude");
    AbValue values = {0};
    if (field) {
      if (field->value.kind != AB_VALUE_ARRAY)
        return request_error(state->engine,
                             "effective config exclude is not an array");
      values = field->value;
      memset(&field->value, 0, sizeof(field->value));
    } else {
      values.kind = AB_VALUE_ARRAY;
    }
    for (index = 0; status == ARCHBIRD_OK &&
                    index < state->request.exclude->as.array.count;
         index++) {
      const AbString *pattern =
          &state->request.exclude->as.array.items[index].as.text;
      status = array_append_string(state->engine, &values, pattern->data,
                                   pattern->length);
    }
    if (status == ARCHBIRD_OK)
      status = object_set(state->engine, &state->effective, "exclude", &values);
    ab_value_free(state->engine, &values);
  }
  if (status == ARCHBIRD_OK && !state->request.use_default_excludes) {
    AbValue discovery = {0};
    discovery.kind = AB_VALUE_OBJECT;
    status = object_set_bool(state->engine, &discovery, "default_excludes", 0);
    if (status == ARCHBIRD_OK)
      status =
          object_set(state->engine, &state->effective, "discovery", &discovery);
    ab_value_free(state->engine, &discovery);
  }
  if (status == ARCHBIRD_OK && state->request.has_max_file_bytes) {
    AbObjectField *field = mutable_member(&state->effective, "limits");
    AbValue limits = {0};
    if (field) {
      if (field->value.kind != AB_VALUE_OBJECT)
        return request_error(state->engine,
                             "effective config limits is not an object");
      limits = field->value;
      memset(&field->value, 0, sizeof(field->value));
    } else {
      limits.kind = AB_VALUE_OBJECT;
    }
    status = object_set_integer(state->engine, &limits, "max_file_bytes",
                                state->request.max_file_bytes);
    if (status == ARCHBIRD_OK)
      status = object_set(state->engine, &state->effective, "limits", &limits);
    ab_value_free(state->engine, &limits);
  }
  if (status == ARCHBIRD_OK && state->request.has_max_index_bytes) {
    AbObjectField *field = mutable_member(&state->effective, "limits");
    AbValue limits = {0};
    if (field) {
      if (field->value.kind != AB_VALUE_OBJECT)
        return request_error(state->engine,
                             "effective config limits is not an object");
      limits = field->value;
      memset(&field->value, 0, sizeof(field->value));
    } else {
      limits.kind = AB_VALUE_OBJECT;
    }
    status = object_set_integer(state->engine, &limits, "max_index_bytes",
                                state->request.max_index_bytes);
    if (status == ARCHBIRD_OK)
      status = object_set(state->engine, &state->effective, "limits", &limits);
    ab_value_free(state->engine, &limits);
  }
  return status;
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static ArchbirdStatus render_value(ArchbirdEngine *engine, const AbValue *value,
                                   AbBuffer *out) {
  AbBuffer raw;
  ArchbirdStatus status;
  ab_buffer_init(&raw, engine);
  status = ab_value_render(&raw, value);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, raw.data, raw.length, 0,
                                        buffer_write, out);
  ab_buffer_free(&raw);
  return status;
}

static ArchbirdStatus scope_discovered_records(ResolutionState *state);

static ArchbirdStatus prefer_cpp_headers(ResolutionState *state) {
  AbValue *c_globs = NULL;
  AbValue *cpp_globs = NULL;
  AbObjectField *layers = mutable_member(&state->effective, "layers");
  size_t layer_index;
  size_t glob_index;
  if (!layers || layers->value.kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  for (layer_index = 0; layer_index < layers->value.as.array.count;
       layer_index++) {
    AbValue *layer = &layers->value.as.array.items[layer_index];
    const AbValue *name = ab_value_member(layer, "name");
    AbObjectField *globs = mutable_member(layer, "globs");
    if (!globs || globs->value.kind != AB_VALUE_ARRAY)
      return ARCHBIRD_INVALID_SCHEMA;
    if (ab_value_string_is(name, "auto-c"))
      c_globs = &globs->value;
    else if (ab_value_string_is(name, "auto-cpp"))
      cpp_globs = &globs->value;
  }
  if (!c_globs || !cpp_globs)
    return ARCHBIRD_INVALID_SCHEMA;
  for (glob_index = 0; glob_index < c_globs->as.array.count; glob_index++) {
    AbValue *glob = &c_globs->as.array.items[glob_index];
    if (!ab_value_string_is(glob, "**/*.h"))
      continue;
    ab_value_free(state->engine, glob);
    memmove(glob, glob + 1,
            (c_globs->as.array.count - glob_index - 1) * sizeof(*glob));
    c_globs->as.array.count--;
    memset(&c_globs->as.array.items[c_globs->as.array.count], 0, sizeof(*glob));
    return array_append_string(state->engine, cpp_globs, "**/*.h", 6);
  }
  return ARCHBIRD_INVALID_SCHEMA;
}

static ArchbirdStatus
prepare_effective(ResolutionState *state, const uint8_t *config_json,
                  size_t config_length, const AbString *package,
                  const AbString *version, const AbPyprojectMetadata *pyproject,
                  const AbString *r_package, const AbString *r_version,
                  int has_make) {
  ArchbirdStatus status;
  status = ab_json_value_decode(
      state->engine,
      config_length ? config_json : (const uint8_t *)default_config,
      config_length ? config_length : sizeof(default_config) - 1,
      &state->effective);
  if (status == ARCHBIRD_OK && !state->configured &&
      state->has_cpp_translation_unit && !state->has_c_translation_unit)
    status = prefer_cpp_headers(state);
  if (status == ARCHBIRD_OK && !state->configured && !state->request.project &&
      (package->length || pyproject->name.length || r_package->length)) {
    const AbString *identity = package->length          ? package
                               : pyproject->name.length ? &pyproject->name
                                                        : r_package;
    const char *project_data;
    size_t project_length;
    if (portable_project_name(identity->data, identity->length, &project_data,
                              &project_length)) {
      status = object_set_string(state->engine, &state->effective, "project",
                                 project_data, project_length);
      if (status == ARCHBIRD_OK)
        state->package_identity = package->length          ? 1
                                  : pyproject->name.length ? 2
                                                           : 3;
    }
  }
  if (status == ARCHBIRD_OK && !state->configured && state->has_package_json)
    status = add_default_npm(state, package, version);
  if (status == ARCHBIRD_OK && !state->configured && state->has_pyproject)
    status = add_default_python(state, pyproject);
  if (status == ARCHBIRD_OK && !state->configured && state->has_description &&
      r_package->length)
    status = add_default_r(state, r_package, r_version);
  if (status == ARCHBIRD_OK && !state->configured && has_make)
    status = add_default_make(state);
  if (status == ARCHBIRD_OK)
    status = apply_overlays(state);
  if (status == ARCHBIRD_OK)
    status = scope_discovered_records(state);
  return status;
}

static InventoryFile *inventory_find(ResolutionState *state,
                                     const AbString *path) {
  size_t low = 0;
  size_t high = state->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(state->files[middle].path, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &state->files[middle];
  }
  return NULL;
}

static int path_matches_values(const AbString *path, const AbValue *patterns) {
  size_t index;
  if (!patterns || !patterns->as.array.count)
    return 1;
  for (index = 0; index < patterns->as.array.count; index++) {
    const AbString *pattern = &patterns->as.array.items[index].as.text;
    if (ab_map_collection_match(path, pattern) ||
        ab_map_path_match(path, pattern))
      return 1;
  }
  return 0;
}

static ArchbirdStatus scope_discovered_collection(ResolutionState *state,
                                                  const char *name) {
  AbObjectField *field = mutable_member(&state->effective, name);
  size_t read_index;
  size_t write_index = 0;
  if (!field)
    return ARCHBIRD_OK;
  if (field->value.kind != AB_VALUE_ARRAY)
    return request_error(state->engine,
                         "discovered configuration collection is not an array");
  for (read_index = 0; read_index < field->value.as.array.count; read_index++) {
    AbValue *row = &field->value.as.array.items[read_index];
    const AbValue *path = ab_value_member(row, "path");
    int keep = path && path->kind == AB_VALUE_STRING &&
               path_matches_values(&path->as.text, state->request.only);
    if (keep) {
      if (write_index != read_index) {
        field->value.as.array.items[write_index] = *row;
        memset(row, 0, sizeof(*row));
      }
      write_index++;
    } else {
      ab_value_free(state->engine, row);
    }
  }
  field->value.as.array.count = write_index;
  return ARCHBIRD_OK;
}

static ArchbirdStatus scope_discovered_records(ResolutionState *state) {
  ArchbirdStatus status;
  if (state->configured || !state->request.only ||
      !state->request.only->as.array.count)
    return ARCHBIRD_OK;
  status = scope_discovered_collection(state, "packages");
  if (status == ARCHBIRD_OK)
    status = scope_discovered_collection(state, "builds");
  return status;
}

static int path_segment(const AbString *path, const char *wanted) {
  size_t wanted_length = strlen(wanted);
  size_t start = 0;
  while (start < path->length) {
    size_t end = start;
    while (end < path->length && path->data[end] != '/')
      end++;
    if (end - start == wanted_length &&
        !memcmp(path->data + start, wanted, wanted_length))
      return 1;
    start = end + 1;
  }
  return 0;
}

static int ends_with(const AbString *value, const char *suffix) {
  size_t length = strlen(suffix);
  return value->length >= length &&
         !memcmp(value->data + value->length - length, suffix, length);
}

static int bytes_contains(const char *data, size_t length, const char *needle) {
  size_t needle_length = strlen(needle);
  size_t index;
  if (needle_length > length)
    return 0;
  for (index = 0; index + needle_length <= length; index++)
    if (!memcmp(data + index, needle, needle_length))
      return 1;
  return 0;
}

static int leaf_stem_is(const char *leaf, size_t leaf_length,
                        const char *wanted) {
  const char *dot = NULL;
  size_t index;
  for (index = leaf_length; index; index--)
    if (leaf[index - 1] == '.') {
      dot = leaf + index - 1;
      break;
    }
  size_t stem_length = dot ? (size_t)(dot - leaf) : leaf_length;
  size_t wanted_length = strlen(wanted);
  return stem_length == wanted_length &&
         (!wanted_length || !memcmp(leaf, wanted, wanted_length));
}

static ArchbirdStatus add_role(ResolutionState *state, AbValue *row,
                               const char *role) {
  AbObjectField *field = mutable_member(row, "roles");
  size_t index;
  ArchbirdStatus status;
  if (!field || field->value.kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index < field->value.as.array.count; index++) {
    if (ab_value_string_is(&field->value.as.array.items[index], role))
      return ARCHBIRD_OK;
  }
  status =
      array_append_string(state->engine, &field->value, role, strlen(role));
  if (status == ARCHBIRD_OK && field->value.as.array.count > 1)
    qsort(field->value.as.array.items, field->value.as.array.count,
          sizeof(*field->value.as.array.items), value_string_compare);
  return status;
}

static ArchbirdStatus candidate_roles(ResolutionState *state, AbValue *row,
                                      const AbString *path) {
  const char *leaf = path->data;
  size_t leaf_length = path->length;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = path->length; index; index--) {
    if (path->data[index - 1] == '/') {
      leaf = path->data + index;
      leaf_length = path->length - index;
      break;
    }
  }
  if (path_segment(path, "test") || path_segment(path, "tests") ||
      path_segment(path, "__tests__") || path_segment(path, "spec") ||
      path_segment(path, "specs") || leaf_stem_is(leaf, leaf_length, "test") ||
      leaf_stem_is(leaf, leaf_length, "spec") ||
      (leaf_length >= 5 && !memcmp(leaf, "test_", 5)) ||
      (leaf_length >= 5 && !memcmp(leaf, "test-", 5)) ||
      bytes_contains(leaf, leaf_length, ".test.") ||
      bytes_contains(leaf, leaf_length, ".spec.") ||
      bytes_contains(leaf, leaf_length, "_test.") ||
      bytes_contains(leaf, leaf_length, "-test.") ||
      (leaf_length >= 5 && !memcmp(leaf + leaf_length - 5, "_test", 5)))
    status = add_role(state, row, "test-candidate");
  if (status == ARCHBIRD_OK &&
      (path_segment(path, "vendor") || path_segment(path, "vendored") ||
       path_segment(path, "third_party") || path_segment(path, "third-party")))
    status = add_role(state, row, "third-party-candidate");
  if (status == ARCHBIRD_OK &&
      (path_segment(path, "generated") || ends_with(path, ".generated.c") ||
       ends_with(path, ".generated.h") || ends_with(path, ".generated.js") ||
       ends_with(path, ".generated.ts")))
    status = add_role(state, row, "generated-candidate");
  return status;
}

static int extension_known_unsupported(const AbString *path) {
  static const char *const suffixes[] = {
      ".rs", ".go",  ".java",  ".cs",  ".rb", ".php", ".swift",
      ".kt", ".kts", ".scala", ".lua", ".sh", ".sql"};
  size_t index;
  for (index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
    if (ends_with(path, suffixes[index]))
      return 1;
  }
  return 0;
}

static int extension_supported(const AbString *path) {
  static const char *const suffixes[] = {
      ".c",   ".h",   ".cc",  ".cpp", ".cxx", ".hh",  ".hpp", ".hxx",
      ".py",  ".pyi", ".pyw", ".js",  ".mjs", ".cjs", ".jsx", ".ts",
      ".mts", ".cts", ".tsx", ".vue", ".R",   ".r"};
  size_t index;
  for (index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
    if (ends_with(path, suffixes[index]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus append_diagnostic(ResolutionState *state,
                                        const char *code, const char *severity,
                                        const AbString *path, size_t bytes,
                                        size_t limit) {
  ResolutionDiagnostic *resized;
  resized = (ResolutionDiagnostic *)ab_realloc(
      state->engine, state->diagnostics,
      (state->diagnostic_count + 1) * sizeof(*state->diagnostics));
  if (!resized)
    return ARCHBIRD_OUT_OF_MEMORY;
  state->diagnostics = resized;
  memset(&state->diagnostics[state->diagnostic_count], 0,
         sizeof(*state->diagnostics));
  state->diagnostics[state->diagnostic_count].code = code;
  state->diagnostics[state->diagnostic_count].severity = severity;
  state->diagnostics[state->diagnostic_count].bytes = bytes;
  state->diagnostics[state->diagnostic_count].limit = limit;
  if (ab_string_copy(state->engine,
                     &state->diagnostics[state->diagnostic_count].path,
                     path->data, path->length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  state->diagnostic_count++;
  return ARCHBIRD_OK;
}

static int plan_row_has_role(const AbValue *row, const char *role) {
  const AbValue *roles = ab_value_member(row, "roles");
  size_t index;
  if (!roles || roles->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < roles->as.array.count; index++)
    if (ab_value_string_is(&roles->as.array.items[index], role))
      return 1;
  return 0;
}

static ArchbirdStatus filter_plan(ResolutionState *state, size_t max_file_bytes,
                                  size_t max_index_bytes) {
  AbObjectField *field = mutable_member(&state->plan, "files");
  AbValue *items;
  size_t read_index;
  size_t write_index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!field || field->value.kind != AB_VALUE_ARRAY)
    return inventory_error(state->engine, "native plan has no files");
  items = field->value.as.array.items;
  for (read_index = 0;
       status == ARCHBIRD_OK && read_index < field->value.as.array.count;
       read_index++) {
    AbValue *row = &items[read_index];
    const AbValue *path_value = ab_value_member(row, "path");
    const AbString *path;
    InventoryFile *inventory;
    size_t byte_limit;
    int is_index;
    int keep = 1;
    if (!path_value || path_value->kind != AB_VALUE_STRING) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    path = &path_value->as.text;
    is_index = plan_row_has_role(row, "index");
    byte_limit = is_index ? max_index_bytes : max_file_bytes;
    inventory = inventory_find(state, path);
    if (!inventory) {
      status = inventory_error(state->engine,
                               "native plan selected an unknown file");
      break;
    }
    if (!path_matches_values(path, state->request.only))
      keep = 0;
    if (keep &&
        (state->request.use_ignore_files ||
         (state->request.ignore_files &&
          state->request.ignore_files->as.array.count)) &&
        ab_ignore_set_matches(&state->ignores, path, 0)) {
      state->ignored_count++;
      keep = 0;
    }
    if (keep && inventory->bytes > byte_limit) {
      if (state->configured)
        return archbird_error_set(
            state->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
            "configured %s exceeds limits.%s: %.*s: %zu > %zu",
            is_index ? "index" : "source",
            is_index ? "max_index_bytes" : "max_file_bytes", (int)path->length,
            path->data, inventory->bytes, byte_limit);
      state->oversized_count++;
      status = append_diagnostic(state,
                                 is_index ? "discovery-index-oversized"
                                          : "discovery-file-oversized",
                                 "warning", path, inventory->bytes, byte_limit);
      keep = 0;
    }
    if (keep && !state->configured)
      status = candidate_roles(state, row, path);
    if (keep) {
      if (write_index != read_index) {
        items[write_index] = *row;
        memset(row, 0, sizeof(*row));
      }
      write_index++;
    } else {
      ab_value_free(state->engine, row);
    }
  }
  if (status == ARCHBIRD_OK)
    field->value.as.array.count = write_index;
  return status;
}

static ArchbirdStatus build_plan(ResolutionState *state, const uint8_t *config,
                                 size_t config_length,
                                 size_t *out_max_file_bytes,
                                 size_t *out_max_index_bytes) {
  ArchbirdDiscovery *discovery = NULL;
  AbBuffer encoded;
  size_t index;
  ArchbirdStatus status = archbird_discovery_create(state->engine, config,
                                                    config_length, &discovery);
  ab_buffer_init(&encoded, state->engine);
  for (index = 0; status == ARCHBIRD_OK && index < state->file_count; index++)
    status = archbird_discovery_add_path(state->engine, discovery,
                                         state->files[index].path->data,
                                         state->files[index].path->length);
  if (status == ARCHBIRD_OK)
    status = archbird_discovery_render(state->engine, discovery, 0,
                                       buffer_write, &encoded);
  archbird_discovery_destroy(discovery);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(state->engine, encoded.data, encoded.length,
                                  &state->plan);
  if (status == ARCHBIRD_OK) {
    uint64_t file_value;
    uint64_t index_value;
    if (!ab_value_u64(ab_value_member(&state->plan, "max_file_bytes"),
                      &file_value) ||
        !file_value || file_value > SIZE_MAX ||
        !ab_value_u64(ab_value_member(&state->plan, "max_index_bytes"),
                      &index_value) ||
        !index_value || index_value > SIZE_MAX)
      status = ARCHBIRD_INVALID_SCHEMA;
    else {
      *out_max_file_bytes = (size_t)file_value;
      *out_max_index_bytes = (size_t)index_value;
    }
  }
  ab_buffer_free(&encoded);
  return status;
}

static void count_inventory_coverage(ResolutionState *state) {
  size_t index;
  for (index = 0; index < state->file_count; index++) {
    if (extension_known_unsupported(state->files[index].path))
      state->unsupported_count++;
    else if (!extension_supported(state->files[index].path))
      state->asset_count++;
  }
}

static ArchbirdStatus render_diagnostics(AbBuffer *buffer,
                                         const ResolutionState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->diagnostic_count;
       index++) {
    const ResolutionDiagnostic *row = &state->diagnostics[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"bytes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->bytes);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"code\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->code, strlen(row->code));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"limit\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->limit);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->path.data, row->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"severity\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, row->severity, strlen(row->severity));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_ignore_sources(AbBuffer *buffer,
                                            const ResolutionState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->ignores.source_count;
       index++) {
    const AbIgnoreSource *source = &state->ignores.sources[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"path\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, source->path.data, source->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"rules\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, source->rule_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, source->sha256, 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int string_array_contains(const AbValue *array, const AbString *value) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *item = &array->as.array.items[index];
    if (item->kind == AB_VALUE_STRING && ab_string_equal(&item->as.text, value))
      return 1;
  }
  return 0;
}

static ArchbirdStatus render_origin(AbBuffer *buffer, int *first,
                                    const char *pointer, const char *source,
                                    const char *evidence,
                                    size_t evidence_length) {
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!*first)
    status = ab_buffer_literal(buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "{\"pointer\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, pointer, strlen(pointer));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"source\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, source, strlen(source));
  if (status == ARCHBIRD_OK && evidence) {
    status = ab_buffer_literal(buffer, ",\"evidence\":[");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, evidence, evidence_length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "]");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  if (status == ARCHBIRD_OK)
    *first = 0;
  return status;
}

static ArchbirdStatus render_indexed_origin(AbBuffer *buffer, int *first,
                                            const char *collection,
                                            size_t index, const char *source,
                                            const AbString *evidence) {
  char pointer[96];
  int length = snprintf(pointer, sizeof(pointer), "/%s/%zu", collection, index);
  if (length < 0 || (size_t)length >= sizeof(pointer))
    return ARCHBIRD_LIMIT_EXCEEDED;
  return render_origin(buffer, first, pointer, source,
                       evidence ? evidence->data : NULL,
                       evidence ? evidence->length : 0);
}

static ArchbirdStatus render_origins(AbBuffer *buffer,
                                     const ResolutionState *state) {
  const AbValue *layers = ab_value_member(&state->effective, "layers");
  const AbValue *excludes = ab_value_member(&state->effective, "exclude");
  const AbValue *request_default =
      ab_value_member(&state->request_document, "default_excludes");
  const AbValue *request_ignore =
      ab_value_member(&state->request_document, "ignore");
  size_t cli_layers =
      state->request.sources ? state->request.sources->as.array.count : 0;
  size_t cli_excludes =
      state->request.exclude ? state->request.exclude->as.array.count : 0;
  size_t base_excludes = excludes && excludes->kind == AB_VALUE_ARRAY &&
                                 excludes->as.array.count >= cli_excludes
                             ? excludes->as.array.count - cli_excludes
                             : 0;
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  if (status == ARCHBIRD_OK)
    status = render_origin(
        buffer, &first, "/", state->configured ? "config" : "discovery",
        state->configured ? "project-config" : "archbird-discovery-v1",
        state->configured ? 14 : 21);
  if (status == ARCHBIRD_OK)
    status = render_origin(buffer, &first, "/discovery/default_excludes",
                           request_default     ? "cli"
                           : state->configured ? "config"
                                               : "discovery",
                           request_default ? "--no-default-excludes" : NULL,
                           request_default ? 21 : 0);
  for (index = 0;
       status == ARCHBIRD_OK && excludes && index < excludes->as.array.count;
       index++) {
    const AbValue *item = &excludes->as.array.items[index];
    status = render_indexed_origin(
        buffer, &first, "exclude", index,
        index < base_excludes ? state->configured ? "config" : "discovery"
                              : "cli",
        &item->as.text);
  }
  for (index = 0; status == ARCHBIRD_OK && index < state->ignores.source_count;
       index++) {
    const AbIgnoreSource *ignore = &state->ignores.sources[index];
    const char *source =
        string_array_contains(state->request.ignore_files, &ignore->path)
            ? "cli-ignore"
            : "repository-ignore";
    status = render_indexed_origin(buffer, &first, "ignore_files", index,
                                   source, &ignore->path);
  }
  for (index = 0;
       status == ARCHBIRD_OK && layers && index < layers->as.array.count;
       index++) {
    const AbValue *layer = &layers->as.array.items[index];
    const AbValue *name = ab_value_member(layer, "name");
    status = render_indexed_origin(
        buffer, &first, "layers", index,
        index < cli_layers  ? "cli"
        : state->configured ? "config"
                            : "discovery",
        name && name->kind == AB_VALUE_STRING ? &name->as.text : NULL);
  }
  if (status == ARCHBIRD_OK)
    status = render_origin(
        buffer, &first, "/limits/max_file_bytes",
        state->request.has_max_file_bytes ? "cli"
        : state->configured               ? "config"
                                          : "discovery",
        state->request.has_max_file_bytes ? "--max-file-bytes" : NULL,
        state->request.has_max_file_bytes ? 16 : 0);
  if (status == ARCHBIRD_OK)
    status = render_origin(
        buffer, &first, "/limits/max_index_bytes",
        state->request.has_max_index_bytes ? "cli"
        : state->configured                ? "config"
                                           : "discovery",
        state->request.has_max_index_bytes ? "--max-index-bytes" : NULL,
        state->request.has_max_index_bytes ? 17 : 0);
  if (status == ARCHBIRD_OK) {
    const char *source = state->request.project    ? "cli"
                         : state->configured       ? "config"
                         : state->package_identity ? "manifest"
                                                   : "discovery";
    const char *evidence = state->request.project         ? "--project"
                           : state->package_identity == 1 ? "package.json"
                           : state->package_identity == 2 ? "pyproject.toml"
                           : state->package_identity == 3 ? "DESCRIPTION"
                           : !state->configured ? "stable-literal:repository"
                                                : NULL;
    status = render_origin(buffer, &first, "/project", source, evidence,
                           evidence ? strlen(evidence) : 0);
  }
  if (status == ARCHBIRD_OK)
    status = render_origin(buffer, &first, "/root",
                           state->configured ? "config" : "discovery", NULL, 0);
  if (status == ARCHBIRD_OK && request_ignore)
    status = render_origin(buffer, &first, "/selection/ignore", "cli",
                           "--no-ignore", 11);
  for (index = 0; status == ARCHBIRD_OK && state->request.only &&
                  index < state->request.only->as.array.count;
       index++) {
    status = render_indexed_origin(
        buffer, &first, "selection/only", index, "cli",
        &state->request.only->as.array.items[index].as.text);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus
render_resolution(ResolutionState *state, size_t max_file_bytes,
                  size_t max_index_bytes, uint32_t json_flags,
                  ArchbirdWriteFn write_fn, void *user_data) {
  const AbValue *files = ab_value_member(&state->plan, "files");
  const AbValue *configuration =
      ab_value_member(&state->plan, "configuration_sha256");
  const AbValue *project = ab_value_member(&state->plan, "project");
  const AbValue *root = ab_value_member(&state->plan, "root");
  AbBuffer body;
  AbBuffer canonical;
  AbBuffer final;
  ArchbirdSha256Context digest_context;
  uint8_t digest[32];
  char digest_hex[65];
  ArchbirdStatus status;
  if (!files || !configuration || !project || !root)
    return ARCHBIRD_INVALID_SCHEMA;
  ab_buffer_init(&body, state->engine);
  ab_buffer_init(&canonical, state->engine);
  ab_buffer_init(&final, state->engine);
  status = ab_buffer_literal(
      &body,
      "{\"artifact\":\"archbird-config-resolution\",\"configuration_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&body, configuration);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"coverage\":{\"assets\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, state->asset_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"ignored\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, state->ignored_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"inventory_files\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, state->file_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"oversized\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, state->oversized_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"pruned_directories\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_u64(&body, state->pruned_directories
                                 ? state->pruned_directories->as.array.count
                                 : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"selected\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, files->as.array.count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"unsupported_known\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, state->unsupported_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "},\"diagnostics\":");
  if (status == ARCHBIRD_OK)
    status = render_diagnostics(&body, state);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"effective_config\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&body, &state->effective);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"files\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&body, files);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"ignore_files\":");
  if (status == ARCHBIRD_OK)
    status = render_ignore_sources(&body, state);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"max_file_bytes\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, max_file_bytes);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"max_index_bytes\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, max_index_bytes);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"origins\":");
  if (status == ARCHBIRD_OK)
    status = render_origins(&body, state);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body, ",\"profile\":{\"implementation_sha256\":"
               "\"" ARCHBIRD_IMPLEMENTATION_SHA256
               "\",\"name\":\"archbird-discovery-v1\"},\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&body, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"pruned_directories\":");
  if (status == ARCHBIRD_OK) {
    if (state->pruned_directories)
      status = ab_value_render(&body, state->pruned_directories);
    else
      status = ab_buffer_literal(&body, "[]");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"root\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&body, root);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"schema_version\":1}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(state->engine, body.data, body.length,
                                        0, buffer_write, &canonical);
  if (status == ARCHBIRD_OK) {
    archbird_sha256_init(&digest_context);
    status = archbird_sha256_update(&digest_context, canonical.data,
                                    canonical.length);
  }
  if (status == ARCHBIRD_OK) {
    archbird_sha256_final(&digest_context, digest);
    archbird_sha256_hex(digest, digest_hex);
    status = ab_buffer_append(&final, canonical.data, canonical.length - 1);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&final, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&final, digest_hex, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&final, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(state->engine, final.data, final.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&body);
  ab_buffer_free(&canonical);
  ab_buffer_free(&final);
  return status;
}

static void resolution_free(ResolutionState *state) {
  size_t index;
  if (!state)
    return;
  ab_free(state->engine, state->files);
  ab_ignore_set_free(&state->ignores);
  ab_value_free(state->engine, &state->request_document);
  ab_value_free(state->engine, &state->inventory_document);
  ab_value_free(state->engine, &state->effective);
  ab_value_free(state->engine, &state->plan);
  for (index = 0; index < state->diagnostic_count; index++)
    ab_string_free(state->engine, &state->diagnostics[index].path);
  ab_free(state->engine, state->diagnostics);
  memset(state, 0, sizeof(*state));
}

ArchbirdStatus
ab_discovery_resolve(ArchbirdEngine *engine, const uint8_t *config_json,
                     size_t config_length, const uint8_t *request_json,
                     size_t request_length, const uint8_t *inventory_json,
                     size_t inventory_length, uint32_t json_flags,
                     ArchbirdWriteFn write_fn, void *user_data) {
  ResolutionState state = {0};
  AbString package = {0};
  AbString package_version = {0};
  AbString r_package = {0};
  AbString r_version = {0};
  AbPyprojectMetadata pyproject = {0};
  AbBuffer effective_json;
  AbMapConfig validated = {0};
  int has_make = 0;
  size_t max_file_bytes = 0;
  size_t max_index_bytes = 0;
  ArchbirdStatus status;
  if (!engine || (!config_json && config_length) || !request_json ||
      !inventory_json || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  state.engine = engine;
  state.configured = config_length != 0;
  ab_ignore_set_init(&state.ignores, engine);
  ab_buffer_init(&effective_json, engine);
  status = decode_request(&state, request_json, request_length);
  if (status == ARCHBIRD_OK)
    status = decode_inventory(&state, inventory_json, inventory_length,
                              &package, &package_version, &pyproject,
                              &r_package, &r_version, &has_make);
  if (status == ARCHBIRD_OK)
    status = prepare_effective(&state, config_json, config_length, &package,
                               &package_version, &pyproject, &r_package,
                               &r_version, has_make);
  if (status == ARCHBIRD_OK)
    status = render_value(engine, &state.effective, &effective_json);
  if (status == ARCHBIRD_OK)
    status = ab_decode_map_config(engine, effective_json.data,
                                  effective_json.length, &validated);
  if (status == ARCHBIRD_OK)
    status = build_plan(&state, effective_json.data, effective_json.length,
                        &max_file_bytes, &max_index_bytes);
  if (status == ARCHBIRD_OK)
    status = filter_plan(&state, max_file_bytes, max_index_bytes);
  if (status == ARCHBIRD_OK) {
    count_inventory_coverage(&state);
    status = render_resolution(&state, max_file_bytes, max_index_bytes,
                               json_flags, write_fn, user_data);
  }
  ab_map_config_free(engine, &validated);
  ab_buffer_free(&effective_json);
  ab_string_free(engine, &package);
  ab_string_free(engine, &package_version);
  ab_string_free(engine, &r_package);
  ab_string_free(engine, &r_version);
  ab_pyproject_metadata_free(engine, &pyproject);
  resolution_free(&state);
  return status;
}

ArchbirdStatus
archbird_discovery_resolve(ArchbirdEngine *engine, const uint8_t *config_json,
                           size_t config_length, const uint8_t *request_json,
                           size_t request_length, const uint8_t *inventory_json,
                           size_t inventory_length, uint32_t json_flags,
                           ArchbirdWriteFn write_fn, void *user_data) {
  return ab_discovery_resolve(engine, config_json, config_length, request_json,
                              request_length, inventory_json, inventory_length,
                              json_flags, write_fn, user_data);
}
