#include "evidence/config_resolution.h"

#include "base/archbird_internal.h"
#include "base/json_value.h"
#include "base/path_match.h"
#include "base/render_internal.h"
#include "base/sha256.h"
#include "base/utf8.h"
#include "configuration/project_configuration.h"
#include "evidence/config.h"
#include "evidence/config_manifest_discovery.h"
#include "evidence/gitignore.h"
#include "evidence/manifests/autoconf_manifest.h"
#include "evidence/manifests/cmake_project_manifest.h"
#include "evidence/manifests/npm_workspace_manifest.h"
#include "evidence/manifests/pyproject_manifest.h"
#include "evidence/manifests/setup_cfg_manifest.h"

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

typedef AbManifestInventoryFile InventoryFile;

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
  const char *metric;
  AbString path;
  size_t observed;
  size_t limit;
} ResolutionDiagnostic;

typedef struct InferredImportRoot {
  AbString root;
  AbString manifest;
} InferredImportRoot;

typedef struct ResolutionState {
  ArchbirdEngine *engine;
  ResolutionRequest request;
  InventoryFile *files;
  size_t file_count;
  AbIgnoreSet ignores;
  AbValue request_document;
  AbValue inventory_document;
  AbValue configured_map_overlay;
  AbValue effective;
  AbValue plan;
  ResolutionDiagnostic *diagnostics;
  size_t diagnostic_count;
  size_t diagnostic_capacity;
  AbManifestDiscovery manifests;
  InferredImportRoot *python_import_roots;
  size_t python_import_root_count;
  size_t python_import_root_capacity;
  size_t ignored_count;
  size_t oversized_count;
  size_t unsupported_count;
  size_t asset_count;
  const AbValue *pruned_directories;
  int has_package_json;
  int has_pyproject;
  int has_setup_cfg;
  int has_description;
  int has_autoconf;
  int has_compile_commands;
  int has_c_translation_unit;
  int has_cpp_translation_unit;
  int has_scip_index;
  /* 1 = package.json, 2 = pyproject.toml, 3 = DESCRIPTION, 4 = configure.ac,
   * 5 = agreeing nested workspace manifests, 6 = setup.cfg,
   * 7 = CMakeLists.txt. */
  int package_identity;
} ResolutionState;

static InventoryFile *inventory_find(ResolutionState *state,
                                     const AbString *path);
static ArchbirdStatus
append_metric_diagnostic(ResolutionState *state, const char *code,
                         const char *severity, const AbString *path,
                         const char *metric, size_t observed, size_t limit);
static ArchbirdStatus append_plain_diagnostic(ResolutionState *state,
                                              const char *code,
                                              const char *severity,
                                              const AbString *path);
static ArchbirdStatus append_diagnostic(ResolutionState *state,
                                        const char *code, const char *severity,
                                        const AbString *path, size_t bytes,
                                        size_t limit);
static ArchbirdStatus manifest_diagnostic(void *user_data, const char *code,
                                          const char *severity,
                                          const AbString *path,
                                          const char *metric, size_t observed,
                                          size_t limit);

static int configured_map_field(const ResolutionState *state,
                                const char *name) {
  return ab_value_member(&state->configured_map_overlay, name) != NULL;
}

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

static ArchbirdStatus object_set_name(ArchbirdEngine *engine, AbValue *object,
                                      const AbString *name, AbValue *value) {
  AbObjectField *field = NULL;
  AbObjectField *resized;
  size_t index;
  ArchbirdStatus status;
  if (!object || object->kind != AB_VALUE_OBJECT || !name || !name->length ||
      !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < object->as.object.count; index++) {
    if (ab_string_equal(&object->as.object.fields[index].name, name)) {
      field = &object->as.object.fields[index];
      break;
    }
  }
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
  status = ab_string_copy(engine, &field->name, name->data, name->length);
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
                                             size_t length,
                                             AbNpmDiscoveryMetadata *out) {
  ArchbirdStatus status =
      ab_npm_discovery_metadata(state->engine, bytes, length, out);
  if (status == ARCHBIRD_INVALID_JSON || status == ARCHBIRD_DUPLICATE_KEY) {
    archbird_error_clear(state->engine);
    return inventory_error(state->engine,
                           "root package.json is not strict JSON");
  }
  if (status == ARCHBIRD_INVALID_SCHEMA) {
    archbird_error_clear(state->engine);
    return inventory_error(state->engine,
                           "root package.json must contain an object");
  }
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

static ArchbirdStatus
add_default_python(ResolutionState *state,
                   const AbPythonPackageMetadata *metadata,
                   const char *manifest, size_t manifest_length) {
  AbValue packages = {0};
  AbValue package = {0};
  AbValue aliases = {0};
  AbObjectField *field = mutable_member(&state->effective, "packages");
  int had_packages = field != NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (field) {
    packages = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    packages.kind = AB_VALUE_ARRAY;
  }
  if (metadata->name.length) {
    package.kind = AB_VALUE_OBJECT;
    aliases.kind = AB_VALUE_ARRAY;
    status = object_set_string(state->engine, &package, "kind", "python", 6);
  }
  if (status == ARCHBIRD_OK && metadata->name.length)
    status =
        object_set_string(state->engine, &package, "layer", "auto-python", 11);
  if (status == ARCHBIRD_OK && metadata->name.length)
    status =
        object_set_string(state->engine, &package, "name", "python-root", 11);
  if (status == ARCHBIRD_OK && metadata->name.length)
    status = object_set_string(state->engine, &package, "path", manifest,
                               manifest_length);
  if (status == ARCHBIRD_OK && metadata->name.length)
    status = object_set_string(state->engine, &package, "identity",
                               metadata->name.data, metadata->name.length);
  if (status == ARCHBIRD_OK && metadata->name.length &&
      metadata->version.length)
    status =
        object_set_string(state->engine, &package, "version",
                          metadata->version.data, metadata->version.length);
  if (status == ARCHBIRD_OK && metadata->name.length && metadata->module.length)
    status = array_append_string(state->engine, &aliases, metadata->module.data,
                                 metadata->module.length);
  if (status == ARCHBIRD_OK && aliases.as.array.count)
    status = object_set(state->engine, &package, "aliases", &aliases);
  if (status == ARCHBIRD_OK && metadata->name.length)
    status = array_append(state->engine, &packages, &package);
  if (status == ARCHBIRD_OK && (had_packages || metadata->name.length))
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

static ArchbirdStatus add_default_compile_commands(ResolutionState *state) {
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
  status =
      object_set_string(state->engine, &build, "kind", "compile_commands", 16);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "name",
                               "compile_commands", 16);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "path",
                               "compile_commands.json", 21);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "variant", "default", 7);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &builds, &build);
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "builds", &builds);
  ab_value_free(state->engine, &build);
  ab_value_free(state->engine, &builds);
  return status;
}

static ArchbirdStatus add_default_scip_index(ResolutionState *state) {
  AbObjectField *field = mutable_member(&state->effective, "indexes");
  AbValue indexes = {0};
  AbValue index = {0};
  ArchbirdStatus status;
  if (field) {
    indexes = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    indexes.kind = AB_VALUE_ARRAY;
  }
  index.kind = AB_VALUE_OBJECT;
  status = object_set_string(state->engine, &index, "format", "scip", 4);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &index, "name", "scip", 4);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &index, "path", "index.scip", 10);
  if (status == ARCHBIRD_OK)
    status = object_set_bool(state->engine, &index, "required", 1);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &indexes, &index);
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "indexes", &indexes);
  ab_value_free(state->engine, &index);
  ab_value_free(state->engine, &indexes);
  return status;
}

static ArchbirdStatus add_default_autoconf(ResolutionState *state,
                                           const AbAutoconfMetadata *metadata) {
  AbObjectField *field;
  AbValue packages = {0};
  AbValue builds = {0};
  AbValue package = {0};
  AbValue build = {0};
  const char *layer = state->has_c_translation_unit     ? "auto-c"
                      : state->has_cpp_translation_unit ? "auto-cpp"
                                                        : NULL;
  size_t layer_length = layer ? strlen(layer) : 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  field = mutable_member(&state->effective, "packages");
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
  if (metadata->package.length && layer)
    status =
        object_set_string(state->engine, &package, "identity",
                          metadata->package.data, metadata->package.length);
  if (status == ARCHBIRD_OK && metadata->package.length && layer)
    status = object_set_string(state->engine, &package, "kind", "generic", 7);
  if (status == ARCHBIRD_OK && metadata->package.length && layer)
    status = object_set_string(state->engine, &package, "layer", layer,
                               layer_length);
  if (status == ARCHBIRD_OK && metadata->package.length && layer)
    status =
        object_set_string(state->engine, &package, "name", "autoconf-root", 13);
  if (status == ARCHBIRD_OK && metadata->package.length && layer)
    status =
        object_set_string(state->engine, &package, "path", "configure.ac", 12);
  if (status == ARCHBIRD_OK && metadata->package.length && layer &&
      metadata->version.length)
    status =
        object_set_string(state->engine, &package, "version",
                          metadata->version.data, metadata->version.length);
  if (status == ARCHBIRD_OK && metadata->package.length && layer)
    status = array_append(state->engine, &packages, &package);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "kind", "autoconf", 8);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &build, "name", "autoconf", 8);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &build, "path", "configure.ac", 12);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &builds, &build);
  if (status == ARCHBIRD_OK && (packages.as.array.count ||
                                mutable_member(&state->effective, "packages")))
    status =
        object_set(state->engine, &state->effective, "packages", &packages);
  if (status == ARCHBIRD_OK)
    status = object_set(state->engine, &state->effective, "builds", &builds);
  ab_value_free(state->engine, &package);
  ab_value_free(state->engine, &build);
  ab_value_free(state->engine, &packages);
  ab_value_free(state->engine, &builds);
  return status;
}

static ArchbirdStatus
decode_inventory(ResolutionState *state, const uint8_t *json,
                 size_t json_length, AbNpmDiscoveryMetadata *npm,
                 AbPyprojectMetadata *pyproject,
                 AbPythonPackageMetadata *setup_cfg, AbString *r_package,
                 AbString *r_version, AbAutoconfMetadata *autoconf,
                 AbCmakeProjectMetadata *cmake, int *has_make) {
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
    if (ab_value_string_is(path, "compile_commands.json"))
      state->has_compile_commands = 1;
    if (ab_value_string_is(path, "index.scip"))
      state->has_scip_index = 1;
    if (ab_value_string_is(path, "configure.ac"))
      state->has_autoconf = 1;
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
  if (status == ARCHBIRD_OK)
    status = ab_ignore_set_finalize(&state->ignores);
  for (index = 0; status == ARCHBIRD_OK && index < documents->as.array.count;
       index++) {
    const AbValue *row = &documents->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    size_t previous;
    if (!allowed_fields(row, input_fields, 2) || !nonblank_string(path) ||
        !repository_path_valid(&path->as.text)) {
      status = inventory_error(state->engine, "invalid document row");
      break;
    }
    for (previous = 0; previous < index; previous++) {
      const AbValue *other =
          ab_value_member(&documents->as.array.items[previous], "path");
      if (other && other->kind == AB_VALUE_STRING &&
          ab_string_equal(&other->as.text, &path->as.text)) {
        status = inventory_error(state->engine, "duplicate document path");
        break;
      }
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < documents->as.array.count;
       index++) {
    const AbValue *row = &documents->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    uint8_t *bytes = NULL;
    size_t length = 0;
    int root_document = ab_value_string_is(path, "package.json") ||
                        ab_value_string_is(path, "pyproject.toml") ||
                        ab_value_string_is(path, "DESCRIPTION") ||
                        ab_value_string_is(path, "configure.ac");
    if (!root_document)
      continue;
    status = decode_hex(state->engine, ab_value_member(row, "content_hex"),
                        &bytes, &length);
    if (status == ARCHBIRD_OK && ab_value_string_is(path, "package.json")) {
      state->has_package_json = 1;
      status = parse_package_document(state, bytes, length, npm);
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(path, "pyproject.toml")) {
      state->has_pyproject = 1;
      status = ab_pyproject_metadata(state->engine, bytes, length, pyproject);
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(path, "DESCRIPTION")) {
      state->has_description = 1;
      status = parse_description_document(state, bytes, length, r_package,
                                          r_version);
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(path, "configure.ac")) {
      state->has_autoconf = 1;
      status = ab_autoconf_metadata(state->engine, bytes, length, autoconf);
    }
    ab_free(state->engine, bytes);
  }
  if (status == ARCHBIRD_OK)
    status = ab_manifest_discovery_select(
        &state->manifests, state->files, state->file_count, &state->ignores,
        state->request.use_ignore_files ||
            (state->request.ignore_files &&
             state->request.ignore_files->as.array.count),
        npm, pyproject, manifest_diagnostic, state);
  for (index = 0; status == ARCHBIRD_OK && index < documents->as.array.count;
       index++) {
    const AbValue *row = &documents->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    AbManifestCandidate *candidate;
    uint8_t *bytes = NULL;
    size_t length = 0;
    if (ab_value_string_is(path, "package.json") ||
        ab_value_string_is(path, "pyproject.toml") ||
        ab_value_string_is(path, "DESCRIPTION") ||
        ab_value_string_is(path, "configure.ac"))
      continue;
    candidate = ab_manifest_discovery_find(&state->manifests, &path->as.text);
    if (!candidate) {
      status = inventory_error(state->engine, "unrequested manifest document");
      break;
    }
    status = decode_hex(state->engine, ab_value_member(row, "content_hex"),
                        &bytes, &length);
    if (status == ARCHBIRD_OK && (length != candidate->bytes ||
                                  length > AB_MANIFEST_DISCOVERY_MAX_BYTES))
      status = inventory_error(
          state->engine,
          "manifest document bytes disagree with bounded inventory row");
    if (status == ARCHBIRD_OK &&
        candidate->kind == AB_MANIFEST_CANDIDATE_SETUP_CFG) {
      state->has_setup_cfg = 1;
      candidate->supplied = 1;
      status = ab_setup_cfg_metadata(state->engine, bytes, length, setup_cfg);
    } else if (status == ARCHBIRD_OK &&
               candidate->kind == AB_MANIFEST_CANDIDATE_CMAKE_PROJECT) {
      candidate->supplied = 1;
      status = ab_cmake_project_metadata(state->engine, bytes, length, cmake);
    } else if (status == ARCHBIRD_OK) {
      status = ab_manifest_discovery_supply(&state->manifests, candidate, bytes,
                                            length, manifest_diagnostic, state);
    }
    ab_free(state->engine, bytes);
  }
  if (status == ARCHBIRD_OK)
    status = ab_manifest_discovery_report_missing(&state->manifests,
                                                  manifest_diagnostic, state);
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

static int inferred_package_layer_matches(const AbValue *discovered_layer,
                                          const AbValue *language) {
  if (!discovered_layer || !language ||
      discovered_layer->kind != AB_VALUE_STRING ||
      language->kind != AB_VALUE_STRING)
    return 0;
  if (ab_value_string_is(discovered_layer, "auto-python"))
    return ab_value_string_is(language, "python");
  if (ab_value_string_is(discovered_layer, "auto-r"))
    return ab_value_string_is(language, "r");
  if (ab_value_string_is(discovered_layer, "auto-c"))
    return ab_value_string_is(language, "c");
  if (ab_value_string_is(discovered_layer, "auto-cpp"))
    return ab_value_string_is(language, "cpp");
  if (ab_value_string_is(discovered_layer, "auto-javascript"))
    return ab_value_string_is(language, "javascript") ||
           ab_value_string_is(language, "typescript") ||
           ab_value_string_is(language, "vue");
  return 0;
}

static int effective_layer_exists(const AbValue *layers,
                                  const AbValue *wanted) {
  size_t index;
  if (!layers || layers->kind != AB_VALUE_ARRAY || !wanted ||
      wanted->kind != AB_VALUE_STRING)
    return 0;
  for (index = 0; index < layers->as.array.count; index++) {
    const AbValue *name =
        ab_value_member(&layers->as.array.items[index], "name");
    if (name && name->kind == AB_VALUE_STRING &&
        ab_string_equal(&name->as.text, &wanted->as.text))
      return 1;
  }
  return 0;
}

static ArchbirdStatus reconcile_inferred_packages(ResolutionState *state) {
  AbObjectField *package_field;
  const AbValue *layers;
  size_t read_index;
  size_t write_index = 0;
  if (!configured_map_field(state, "layers") ||
      configured_map_field(state, "packages"))
    return ARCHBIRD_OK;
  package_field = mutable_member(&state->effective, "packages");
  layers = ab_value_member(&state->effective, "layers");
  if (!package_field || package_field->value.kind != AB_VALUE_ARRAY ||
      !layers || layers->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_OK;
  for (read_index = 0; read_index < package_field->value.as.array.count;
       read_index++) {
    AbValue *package = &package_field->value.as.array.items[read_index];
    const AbValue *discovered_layer = ab_value_member(package, "layer");
    const AbValue *replacement = NULL;
    size_t layer_index;
    size_t matches = 0;
    ArchbirdStatus status = ARCHBIRD_OK;
    if (!effective_layer_exists(layers, discovered_layer)) {
      for (layer_index = 0; layer_index < layers->as.array.count;
           layer_index++) {
        const AbValue *layer = &layers->as.array.items[layer_index];
        const AbValue *language = ab_value_member(layer, "language");
        if (!inferred_package_layer_matches(discovered_layer, language))
          continue;
        replacement = ab_value_member(layer, "name");
        matches++;
      }
      if (matches == 1)
        status = object_set_string(state->engine, package, "layer",
                                   replacement->as.text.data,
                                   replacement->as.text.length);
      else {
        ab_value_free(state->engine, package);
        continue;
      }
    }
    if (status != ARCHBIRD_OK)
      return status;
    if (write_index != read_index) {
      package_field->value.as.array.items[write_index] = *package;
      memset(package, 0, sizeof(*package));
    }
    write_index++;
  }
  package_field->value.as.array.count = write_index;
  return ARCHBIRD_OK;
}

static int normalized_python_module(const AbPythonPackageMetadata *metadata,
                                    char *buffer, size_t capacity,
                                    size_t *out_length) {
  if ((metadata->module_hints_present && !metadata->module_hints_supported) ||
      (metadata->module_hints_present && !metadata->module.length))
    return 0;
  const AbString *source =
      metadata->module.length ? &metadata->module : &metadata->name;
  size_t index;
  if (!source->length || source->length + 1 > capacity)
    return 0;
  for (index = 0; index < source->length; index++) {
    unsigned char value = (unsigned char)source->data[index];
    if (value == '-' || value == '.')
      buffer[index] = '_';
    else if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9') || value == '_')
      buffer[index] = (char)value;
    else
      return 0;
  }
  if (buffer[0] >= '0' && buffer[0] <= '9')
    return 0;
  buffer[source->length] = '\0';
  *out_length = source->length;
  return 1;
}

static int discovery_path_ignored(ResolutionState *state,
                                  const AbString *path) {
  return (state->request.use_ignore_files ||
          (state->request.ignore_files &&
           state->request.ignore_files->as.array.count)) &&
         ab_ignore_set_matches(&state->ignores, path, 0);
}

static int inventory_path_before_children(const AbString *path,
                                          const char *prefix,
                                          size_t prefix_length) {
  size_t shared = path->length < prefix_length ? path->length : prefix_length;
  int compared = shared ? memcmp(path->data, prefix, shared) : 0;
  if (compared)
    return compared < 0;
  if (path->length <= prefix_length)
    return 1;
  return (unsigned char)path->data[prefix_length] < (unsigned char)'/';
}

static size_t inventory_first_child(const ResolutionState *state,
                                    const char *prefix, size_t prefix_length) {
  size_t low = 0;
  size_t high = state->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (inventory_path_before_children(state->files[middle].path, prefix,
                                       prefix_length))
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

static int inventory_python_source_below(ResolutionState *state,
                                         const char *prefix,
                                         size_t prefix_length) {
  size_t index = inventory_first_child(state, prefix, prefix_length);
  for (; index < state->file_count; index++) {
    const AbString *path = state->files[index].path;
    if (path->length <= prefix_length || path->data[prefix_length] != '/' ||
        memcmp(path->data, prefix, prefix_length))
      break;
    if (!discovery_path_ignored(state, path) &&
        (string_has_suffix(path, ".py") || string_has_suffix(path, ".pyi") ||
         string_has_suffix(path, ".pyw")))
      return 1;
  }
  return 0;
}

static int inventory_python_module_exists(ResolutionState *state,
                                          const char *prefix,
                                          size_t prefix_length,
                                          AbPythonSourceShape source_shape) {
  static const char *const suffixes[] = {
      ".py", ".pyi", ".pyw", "/__init__.py", "/__init__.pyi", "/__init__.pyw"};
  size_t suffix_index;
  size_t suffix_start = source_shape == AB_PYTHON_SOURCE_PACKAGE ? 3 : 0;
  size_t suffix_end = source_shape == AB_PYTHON_SOURCE_MODULE
                          ? 3
                          : sizeof(suffixes) / sizeof(suffixes[0]);
  for (suffix_index = suffix_start; suffix_index < suffix_end; suffix_index++) {
    AbBuffer candidate;
    AbString path;
    int found;
    ab_buffer_init(&candidate, state->engine);
    if (ab_buffer_append(&candidate, prefix, prefix_length) != ARCHBIRD_OK ||
        ab_buffer_literal(&candidate, suffixes[suffix_index]) != ARCHBIRD_OK) {
      ab_buffer_free(&candidate);
      return -1;
    }
    path.data = (char *)candidate.data;
    path.length = candidate.length;
    found = inventory_find(state, &path) != NULL &&
            !discovery_path_ignored(state, &path);
    ab_buffer_free(&candidate);
    if (found)
      return 1;
  }
  return source_shape == AB_PYTHON_SOURCE_ANY
             ? inventory_python_source_below(state, prefix, prefix_length)
             : 0;
}

typedef enum PythonImportRootOutcome {
  PYTHON_IMPORT_ROOT_UNRESOLVED,
  PYTHON_IMPORT_ROOT_RESOLVED,
  PYTHON_IMPORT_ROOT_CONFLICT,
  PYTHON_IMPORT_ROOT_UNSUPPORTED
} PythonImportRootOutcome;

static ArchbirdStatus
python_import_root_candidate(ResolutionState *state, const AbString *manifest,
                             const AbPythonPackageMetadata *metadata,
                             AbString *out,
                             PythonImportRootOutcome *out_outcome) {
  static const char *const conventional_roots[] = {"src", ""};
  char module[256];
  size_t module_length;
  size_t directory_length = manifest->length;
  size_t root_index;
  size_t matches = 0;
  AbString matched = {0};
  *out_outcome = PYTHON_IMPORT_ROOT_UNRESOLVED;
  if ((metadata->module_hints_present && !metadata->module_hints_supported) ||
      (metadata->source_root_present && !metadata->source_root_supported)) {
    *out_outcome = PYTHON_IMPORT_ROOT_UNSUPPORTED;
    return append_plain_diagnostic(state, "discovery-python-layout-unsupported",
                                   "warning", manifest);
  }
  if (!normalized_python_module(metadata, module, sizeof(module),
                                &module_length))
    return ARCHBIRD_OK;
  while (directory_length && manifest->data[directory_length - 1] != '/')
    directory_length--;
  if (directory_length)
    directory_length--;
  for (root_index = 0; root_index < (metadata->source_root.length ? 1 : 2);
       root_index++) {
    const char *root_data = metadata->source_root.length
                                ? metadata->source_root.data
                                : conventional_roots[root_index];
    size_t root_length = metadata->source_root.length
                             ? metadata->source_root.length
                             : strlen(conventional_roots[root_index]);
    AbBuffer prefix;
    AbBuffer root;
    int exists;
    if (root_length == 1 && root_data[0] == '.')
      root_length = 0;
    ab_buffer_init(&prefix, state->engine);
    ab_buffer_init(&root, state->engine);
    if (directory_length && ab_buffer_append(&root, manifest->data,
                                             directory_length) != ARCHBIRD_OK)
      exists = -1;
    else if (directory_length && root_length &&
             ab_buffer_literal(&root, "/") != ARCHBIRD_OK)
      exists = -1;
    else if (root_length &&
             ab_buffer_append(&root, root_data, root_length) != ARCHBIRD_OK)
      exists = -1;
    else if (root.length &&
             ab_buffer_append(&prefix, root.data, root.length) != ARCHBIRD_OK)
      exists = -1;
    else if (root.length && ab_buffer_literal(&prefix, "/") != ARCHBIRD_OK)
      exists = -1;
    else if (ab_buffer_append(&prefix, module, module_length) != ARCHBIRD_OK)
      exists = -1;
    else
      exists =
          inventory_python_module_exists(state, (const char *)prefix.data,
                                         prefix.length, metadata->source_shape);
    if (exists < 0) {
      ab_buffer_free(&prefix);
      ab_buffer_free(&root);
      return ARCHBIRD_OUT_OF_MEMORY;
    }
    if (exists) {
      AbString candidate;
      candidate.data = root.length ? (char *)root.data : (char *)".";
      candidate.length = root.length ? root.length : 1;
      matches++;
      ab_string_free(state->engine, &matched);
      if (ab_string_copy(state->engine, &matched, candidate.data,
                         candidate.length) != ARCHBIRD_OK) {
        ab_buffer_free(&prefix);
        ab_buffer_free(&root);
        return ARCHBIRD_OUT_OF_MEMORY;
      }
    }
    ab_buffer_free(&prefix);
    ab_buffer_free(&root);
  }
  if (matches == 1) {
    *out = matched;
    *out_outcome = PYTHON_IMPORT_ROOT_RESOLVED;
  } else {
    ab_string_free(state->engine, &matched);
    if (matches > 1) {
      *out_outcome = PYTHON_IMPORT_ROOT_CONFLICT;
      return append_metric_diagnostic(
          state, "discovery-python-import-root-conflict", "warning", manifest,
          "matches", matches, 1);
    }
  }
  return ARCHBIRD_OK;
}

static int package_files_use_typescript(ResolutionState *state,
                                        const AbString *manifest) {
  size_t directory_length = manifest->length;
  size_t index;
  int javascript = 0;
  int typescript = 0;
  while (directory_length && manifest->data[directory_length - 1] != '/')
    directory_length--;
  if (directory_length)
    directory_length--;
  if (!directory_length)
    return 0;
  index = inventory_first_child(state, manifest->data, directory_length);
  for (; index < state->file_count; index++) {
    const AbString *path = state->files[index].path;
    if (path->length <= directory_length ||
        path->data[directory_length] != '/' ||
        memcmp(path->data, manifest->data, directory_length))
      break;
    if (discovery_path_ignored(state, path))
      continue;
    javascript |=
        string_has_suffix(path, ".js") || string_has_suffix(path, ".mjs") ||
        string_has_suffix(path, ".cjs") || string_has_suffix(path, ".jsx");
    typescript |=
        string_has_suffix(path, ".ts") || string_has_suffix(path, ".mts") ||
        string_has_suffix(path, ".cts") || string_has_suffix(path, ".tsx");
  }
  return typescript && !javascript;
}

static int package_identity_exists(const AbValue *packages, const char *kind,
                                   const AbString *identity) {
  size_t index;
  if (!packages || packages->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *package = &packages->as.array.items[index];
    const AbValue *package_kind = ab_value_member(package, "kind");
    const AbValue *package_identity = ab_value_member(package, "identity");
    if (ab_value_string_is(package_kind, kind) && package_identity &&
        package_identity->kind == AB_VALUE_STRING &&
        ab_string_equal(&package_identity->as.text, identity))
      return 1;
  }
  return 0;
}

static ArchbirdStatus
append_discovered_package(ResolutionState *state, const char *kind,
                          const char *layer, const AbString *manifest,
                          const AbString *identity, const AbString *version,
                          const AbString *alias) {
  AbObjectField *field = mutable_member(&state->effective, "packages");
  AbValue packages = {0};
  AbValue package = {0};
  AbValue aliases = {0};
  AbBuffer name;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (field) {
    packages = field->value;
    memset(&field->value, 0, sizeof(field->value));
  } else {
    packages.kind = AB_VALUE_ARRAY;
  }
  ab_buffer_init(&name, state->engine);
  if (package_identity_exists(&packages, kind, identity)) {
    status = append_plain_diagnostic(
        state, "discovery-package-identity-conflict", "warning", manifest);
    goto done;
  }
  package.kind = AB_VALUE_OBJECT;
  aliases.kind = AB_VALUE_ARRAY;
  status = ab_buffer_literal(&name, kind);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&name, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&name, identity->data, identity->length);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "identity",
                               identity->data, identity->length);
  if (status == ARCHBIRD_OK)
    status =
        object_set_string(state->engine, &package, "kind", kind, strlen(kind));
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "layer", layer,
                               strlen(layer));
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "name",
                               (const char *)name.data, name.length);
  if (status == ARCHBIRD_OK)
    status = object_set_string(state->engine, &package, "path", manifest->data,
                               manifest->length);
  if (status == ARCHBIRD_OK && version->length)
    status = object_set_string(state->engine, &package, "version",
                               version->data, version->length);
  if (status == ARCHBIRD_OK && alias && alias->length)
    status = array_append_string(state->engine, &aliases, alias->data,
                                 alias->length);
  if (status == ARCHBIRD_OK && aliases.as.array.count)
    status = object_set(state->engine, &package, "aliases", &aliases);
  if (status == ARCHBIRD_OK)
    status = array_append(state->engine, &packages, &package);
done:
  if (status == ARCHBIRD_OK)
    status =
        object_set(state->engine, &state->effective, "packages", &packages);
  ab_buffer_free(&name);
  ab_value_free(state->engine, &aliases);
  ab_value_free(state->engine, &package);
  ab_value_free(state->engine, &packages);
  return status;
}

static ArchbirdStatus record_python_import_root(ResolutionState *state,
                                                const AbString *root,
                                                const AbString *manifest) {
  AbObjectField *layer_field = mutable_member(&state->effective, "layers");
  size_t layer_index;
  if (!root->length || !layer_field ||
      layer_field->value.kind != AB_VALUE_ARRAY)
    return ARCHBIRD_OK;
  for (layer_index = 0; layer_index < layer_field->value.as.array.count;
       layer_index++) {
    AbValue *layer = &layer_field->value.as.array.items[layer_index];
    AbObjectField *roots_field;
    AbValue roots = {0};
    size_t root_index;
    InferredImportRoot *resized;
    ArchbirdStatus status;
    if (!ab_value_string_is(ab_value_member(layer, "name"), "auto-python"))
      continue;
    roots_field = mutable_member(layer, "import_roots");
    if (roots_field) {
      roots = roots_field->value;
      memset(&roots_field->value, 0, sizeof(roots_field->value));
    } else {
      roots.kind = AB_VALUE_ARRAY;
    }
    for (root_index = 0; root_index < roots.as.array.count; root_index++)
      if (roots.as.array.items[root_index].kind == AB_VALUE_STRING &&
          ab_string_equal(&roots.as.array.items[root_index].as.text, root)) {
        status = object_set(state->engine, layer, "import_roots", &roots);
        ab_value_free(state->engine, &roots);
        return status;
      }
    status =
        array_append_string(state->engine, &roots, root->data, root->length);
    if (status == ARCHBIRD_OK)
      status = object_set(state->engine, layer, "import_roots", &roots);
    ab_value_free(state->engine, &roots);
    if (status != ARCHBIRD_OK)
      return status;
    if (state->python_import_root_count == state->python_import_root_capacity) {
      size_t next = state->python_import_root_capacity
                        ? state->python_import_root_capacity * 2
                        : 8;
      if (next < state->python_import_root_capacity ||
          next > SIZE_MAX / sizeof(*state->python_import_roots))
        return ARCHBIRD_LIMIT_EXCEEDED;
      resized = (InferredImportRoot *)ab_realloc(
          state->engine, state->python_import_roots,
          next * sizeof(*state->python_import_roots));
      if (!resized)
        return ARCHBIRD_OUT_OF_MEMORY;
      state->python_import_roots = resized;
      state->python_import_root_capacity = next;
    }
    memset(&state->python_import_roots[state->python_import_root_count], 0,
           sizeof(*state->python_import_roots));
    status = ab_string_copy(
        state->engine,
        &state->python_import_roots[state->python_import_root_count].root,
        root->data, root->length);
    if (status == ARCHBIRD_OK) {
      status = ab_string_copy(
          state->engine,
          &state->python_import_roots[state->python_import_root_count].manifest,
          manifest->data, manifest->length);
      if (status == ARCHBIRD_OK)
        state->python_import_root_count++;
      else
        ab_string_free(
            state->engine,
            &state->python_import_roots[state->python_import_root_count].root);
    }
    return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
add_discovered_packages(ResolutionState *state,
                        const AbPythonPackageMetadata *root_python) {
  size_t index;
  AbString root_manifest = {(char *)"pyproject.toml", 14};
  AbString root_import = {0};
  PythonImportRootOutcome root_outcome;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (state->has_pyproject)
    status = python_import_root_candidate(state, &root_manifest, root_python,
                                          &root_import, &root_outcome);
  if (status == ARCHBIRD_OK && root_import.length)
    status = record_python_import_root(state, &root_import, &root_manifest);
  ab_string_free(state->engine, &root_import);
  for (index = 0;
       status == ARCHBIRD_OK && index < state->manifests.npm_package_count;
       index++) {
    AbDiscoveredNpmPackage *package = &state->manifests.npm_packages[index];
    if (!package->metadata.name.length) {
      status =
          append_plain_diagnostic(state, "discovery-package-identity-missing",
                                  "warning", package->candidate->path);
      continue;
    }
    status = append_discovered_package(
        state, "npm",
        package_files_use_typescript(state, package->candidate->path)
            ? "auto-typescript"
            : "auto-javascript",
        package->candidate->path, &package->metadata.name,
        &package->metadata.version, NULL);
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < state->manifests.python_package_count;
       index++) {
    AbDiscoveredPythonPackage *package =
        &state->manifests.python_packages[index];
    PythonImportRootOutcome import_outcome;
    const AbString *alias = package->metadata.package.module.length
                                ? &package->metadata.package.module
                                : &package->metadata.package.name;
    if (!package->metadata.package.name.length) {
      if (!strcmp(package->candidate->source, "python-workspace"))
        status =
            append_plain_diagnostic(state, "discovery-package-identity-missing",
                                    "warning", package->candidate->path);
      continue;
    }
    status = python_import_root_candidate(
        state, package->candidate->path, &package->metadata.package,
        &package->import_root, &import_outcome);
    if (status == ARCHBIRD_OK &&
        import_outcome != PYTHON_IMPORT_ROOT_RESOLVED) {
      if (import_outcome == PYTHON_IMPORT_ROOT_UNRESOLVED)
        status = append_plain_diagnostic(
            state, "discovery-python-import-root-unresolved", "warning",
            package->candidate->path);
      continue;
    }
    if (status == ARCHBIRD_OK)
      status = record_python_import_root(state, &package->import_root,
                                         package->candidate->path);
    if (status == ARCHBIRD_OK)
      status = append_discovered_package(
          state, "python", "auto-python", package->candidate->path,
          &package->metadata.package.name, &package->metadata.package.version,
          alias);
  }
  return status;
}

static ArchbirdStatus
add_setup_cfg_package(ResolutionState *state,
                      const AbPythonPackageMetadata *setup_cfg) {
  AbString manifest = {(char *)"setup.cfg", 9};
  AbString import_root = {0};
  PythonImportRootOutcome outcome;
  ArchbirdStatus status = python_import_root_candidate(
      state, &manifest, setup_cfg, &import_root, &outcome);
  if (status == ARCHBIRD_OK && outcome == PYTHON_IMPORT_ROOT_RESOLVED)
    status = record_python_import_root(state, &import_root, &manifest);
  if (status == ARCHBIRD_OK && outcome == PYTHON_IMPORT_ROOT_RESOLVED)
    status = add_default_python(state, setup_cfg, "setup.cfg", 9);
  if (status == ARCHBIRD_OK && setup_cfg->name.length &&
      outcome == PYTHON_IMPORT_ROOT_UNRESOLVED)
    status = append_plain_diagnostic(
        state, "discovery-python-import-root-unresolved", "warning", &manifest);
  ab_string_free(state->engine, &import_root);
  return status;
}

static int scoped_npm_project(const AbString *identity, AbString *out) {
  size_t slash = 0;
  if (identity->length < 4 || identity->data[0] != '@')
    return 0;
  while (slash < identity->length && identity->data[slash] != '/')
    slash++;
  if (slash <= 1 || slash + 1 >= identity->length)
    return 0;
  out->data = identity->data + 1;
  out->length = slash - 1;
  return 1;
}

static int discovered_project_identity(const ResolutionState *state,
                                       AbString *out) {
  const AbValue *packages = ab_value_member(&state->effective, "packages");
  const AbString *python = NULL;
  AbString npm = {0};
  size_t named_npm = 0;
  size_t index;
  if (!packages || packages->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *package = &packages->as.array.items[index];
    const AbValue *kind = ab_value_member(package, "kind");
    const AbValue *identity = ab_value_member(package, "identity");
    if (!ab_value_string_is(kind, "npm") || !identity ||
        identity->kind != AB_VALUE_STRING)
      continue;
    named_npm++;
  }
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *package = &packages->as.array.items[index];
    const AbValue *kind = ab_value_member(package, "kind");
    const AbValue *identity = ab_value_member(package, "identity");
    AbString scope;
    if (!identity || identity->kind != AB_VALUE_STRING)
      continue;
    if (ab_value_string_is(kind, "python")) {
      if (python && !ab_string_equal(python, &identity->as.text))
        return 0;
      python = &identity->as.text;
      continue;
    }
    if (!ab_value_string_is(kind, "npm"))
      continue;
    if (!scoped_npm_project(&identity->as.text, &scope)) {
      if (named_npm != 1 || python)
        return 0;
      npm = identity->as.text;
      continue;
    }
    if (npm.length && !ab_string_equal(&npm, &scope))
      return 0;
    npm = scope;
  }
  if (python && npm.length && !ab_string_equal(python, &npm))
    return 0;
  if (python)
    *out = *python;
  else if (npm.length && named_npm)
    *out = npm;
  else
    return 0;
  return 1;
}

static ArchbirdStatus prepare_effective(
    ResolutionState *state, const uint8_t *config_json, size_t config_length,
    const AbNpmDiscoveryMetadata *npm, const AbPyprojectMetadata *pyproject,
    const AbPythonPackageMetadata *setup_cfg, const AbString *r_package,
    const AbString *r_version, const AbAutoconfMetadata *autoconf,
    const AbCmakeProjectMetadata *cmake, int has_make) {
  ArchbirdStatus status;
  size_t default_length = 0;
  const uint8_t *default_json =
      ab_default_project_configuration(&default_length);
  AbProjectConfiguration configuration = {0};
  AbString discovered_identity = {0};
  const AbString *root_identity = NULL;
  size_t overlay_index;
  if (config_length) {
    status = ab_project_configuration_decode(state->engine, config_json,
                                             config_length, &configuration);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(state->engine, &state->configured_map_overlay,
                             &configuration.map_overlay);
  } else
    status = ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(state->engine, default_json, default_length,
                                  &state->effective);
  if (status == ARCHBIRD_OK && state->has_cpp_translation_unit &&
      !state->has_c_translation_unit)
    status = prefer_cpp_headers(state);
  if (npm->name.length)
    root_identity = &npm->name;
  else if (pyproject->package.name.length)
    root_identity = &pyproject->package.name;
  else if (setup_cfg->name.length)
    root_identity = &setup_cfg->name;
  else if (r_package->length)
    root_identity = r_package;
  else if (autoconf->package.length)
    root_identity = &autoconf->package;
  else if (cmake->name.length)
    root_identity = &cmake->name;
  if (status == ARCHBIRD_OK && !configured_map_field(state, "project") &&
      !state->request.project && root_identity) {
    const char *project_data;
    size_t project_length;
    if (portable_project_name(root_identity->data, root_identity->length,
                              &project_data, &project_length)) {
      status = object_set_string(state->engine, &state->effective, "project",
                                 project_data, project_length);
      if (status == ARCHBIRD_OK)
        state->package_identity = npm->name.length                 ? 1
                                  : pyproject->package.name.length ? 2
                                  : setup_cfg->name.length         ? 6
                                  : r_package->length              ? 3
                                  : autoconf->package.length       ? 4
                                  : cmake->name.length             ? 7
                                                                   : 5;
    }
  }
  if (status == ARCHBIRD_OK && state->has_package_json)
    status = add_default_npm(state, &npm->name, &npm->version);
  if (status == ARCHBIRD_OK && state->has_pyproject)
    status =
        add_default_python(state, &pyproject->package, "pyproject.toml", 14);
  if (status == ARCHBIRD_OK)
    status = add_discovered_packages(state, &pyproject->package);
  if (status == ARCHBIRD_OK && state->has_setup_cfg &&
      !pyproject->package.name.length && setup_cfg->name.length)
    status = add_setup_cfg_package(state, setup_cfg);
  if (status == ARCHBIRD_OK && !configured_map_field(state, "project") &&
      !state->request.project && !state->package_identity &&
      discovered_project_identity(state, &discovered_identity)) {
    const char *project_data;
    size_t project_length;
    if (portable_project_name(discovered_identity.data,
                              discovered_identity.length, &project_data,
                              &project_length)) {
      status = object_set_string(state->engine, &state->effective, "project",
                                 project_data, project_length);
      if (status == ARCHBIRD_OK)
        state->package_identity = 5;
    }
  }
  if (status == ARCHBIRD_OK && state->has_description && r_package->length)
    status = add_default_r(state, r_package, r_version);
  if (status == ARCHBIRD_OK && state->has_autoconf)
    status = add_default_autoconf(state, autoconf);
  if (status == ARCHBIRD_OK && has_make)
    status = add_default_make(state);
  if (status == ARCHBIRD_OK && state->has_compile_commands)
    status = add_default_compile_commands(state);
  if (status == ARCHBIRD_OK && state->has_scip_index)
    status = add_default_scip_index(state);
  for (overlay_index = 0;
       status == ARCHBIRD_OK &&
       overlay_index < state->configured_map_overlay.as.object.count;
       overlay_index++) {
    const AbObjectField *field =
        &state->configured_map_overlay.as.object.fields[overlay_index];
    AbValue copied = {0};
    status = ab_value_copy(state->engine, &copied, &field->value);
    if (status == ARCHBIRD_OK)
      status = object_set_name(state->engine, &state->effective, &field->name,
                               &copied);
    ab_value_free(state->engine, &copied);
  }
  if (status == ARCHBIRD_OK)
    status = apply_overlays(state);
  if (status == ARCHBIRD_OK)
    status = reconcile_inferred_packages(state);
  if (status == ARCHBIRD_OK)
    status = scope_discovered_records(state);
  ab_project_configuration_free(state->engine, &configuration);
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
  if (!state->request.only || !state->request.only->as.array.count)
    return ARCHBIRD_OK;
  status = configured_map_field(state, "packages")
               ? ARCHBIRD_OK
               : scope_discovered_collection(state, "packages");
  if (status == ARCHBIRD_OK && !configured_map_field(state, "builds"))
    status = scope_discovered_collection(state, "builds");
  if (status == ARCHBIRD_OK && !configured_map_field(state, "indexes"))
    status = scope_discovered_collection(state, "indexes");
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

static int path_segment_pair(const AbString *path, const char *first,
                             const char *second) {
  size_t first_length = strlen(first);
  size_t second_length = strlen(second);
  size_t start = 0;
  int previous_is_first = 0;
  while (start < path->length) {
    size_t end = start;
    int current_is_first;
    while (end < path->length && path->data[end] != '/')
      end++;
    if (previous_is_first && end - start == second_length &&
        !memcmp(path->data + start, second, second_length))
      return 1;
    current_is_first = end - start == first_length &&
                       !memcmp(path->data + start, first, first_length);
    previous_is_first = current_is_first;
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

static ArchbirdStatus candidate_roles(ResolutionState *state, AbValue *row,
                                      const AbString *path) {
  const char *leaf = path->data;
  size_t leaf_length = path->length;
  size_t index;
  int generated_delivery = path_segment_pair(path, "public", "wasm");
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = path->length; index; index--) {
    if (path->data[index - 1] == '/') {
      leaf = path->data + index;
      leaf_length = path->length - index;
      break;
    }
  }
  if (!plan_row_has_role(row, "test") &&
      (path_segment(path, "test") || path_segment(path, "tests") ||
       path_segment(path, "__tests__") || path_segment(path, "spec") ||
       path_segment(path, "specs") || leaf_stem_is(leaf, leaf_length, "test") ||
       leaf_stem_is(leaf, leaf_length, "spec") ||
       (leaf_length >= 5 && !memcmp(leaf, "test_", 5)) ||
       (leaf_length >= 5 && !memcmp(leaf, "test-", 5)) ||
       bytes_contains(leaf, leaf_length, ".test.") ||
       bytes_contains(leaf, leaf_length, ".spec.") ||
       bytes_contains(leaf, leaf_length, "_test.") ||
       bytes_contains(leaf, leaf_length, "-test.") ||
       (leaf_length >= 5 && !memcmp(leaf + leaf_length - 5, "_test", 5))))
    status = add_role(state, row, "test-candidate");
  if (status == ARCHBIRD_OK &&
      (path_segment(path, "vendor") || path_segment(path, "vendored") ||
       path_segment(path, "third_party") || path_segment(path, "third-party")))
    status = add_role(state, row, "third-party-candidate");
  if (status == ARCHBIRD_OK &&
      (path_segment(path, "generated") || ends_with(path, ".generated.c") ||
       ends_with(path, ".generated.h") || ends_with(path, ".generated.js") ||
       ends_with(path, ".generated.ts") || generated_delivery))
    status = add_role(state, row, "generated-candidate");
  if (status == ARCHBIRD_OK &&
      (ends_with(path, ".generated.js") || ends_with(path, ".generated.ts") ||
       generated_delivery))
    status = add_role(state, row, "generated-delivery-candidate");
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

static ArchbirdStatus
append_metric_diagnostic(ResolutionState *state, const char *code,
                         const char *severity, const AbString *path,
                         const char *metric, size_t observed, size_t limit) {
  ResolutionDiagnostic *resized;
  if (state->diagnostic_count == state->diagnostic_capacity) {
    size_t next =
        state->diagnostic_capacity ? state->diagnostic_capacity * 2 : 16;
    if (next < state->diagnostic_capacity ||
        next > SIZE_MAX / sizeof(*state->diagnostics))
      return ARCHBIRD_LIMIT_EXCEEDED;
    resized = (ResolutionDiagnostic *)ab_realloc(
        state->engine, state->diagnostics, next * sizeof(*state->diagnostics));
    if (!resized)
      return ARCHBIRD_OUT_OF_MEMORY;
    state->diagnostics = resized;
    state->diagnostic_capacity = next;
  }
  memset(&state->diagnostics[state->diagnostic_count], 0,
         sizeof(*state->diagnostics));
  state->diagnostics[state->diagnostic_count].code = code;
  state->diagnostics[state->diagnostic_count].severity = severity;
  state->diagnostics[state->diagnostic_count].metric = metric;
  state->diagnostics[state->diagnostic_count].observed = observed;
  state->diagnostics[state->diagnostic_count].limit = limit;
  if (ab_string_copy(state->engine,
                     &state->diagnostics[state->diagnostic_count].path,
                     path->data, path->length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  state->diagnostic_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_diagnostic(ResolutionState *state,
                                        const char *code, const char *severity,
                                        const AbString *path, size_t bytes,
                                        size_t limit) {
  return append_metric_diagnostic(state, code, severity, path, "bytes", bytes,
                                  limit);
}

static ArchbirdStatus append_plain_diagnostic(ResolutionState *state,
                                              const char *code,
                                              const char *severity,
                                              const AbString *path) {
  return append_metric_diagnostic(state, code, severity, path, NULL, 0, 0);
}

static ArchbirdStatus manifest_diagnostic(void *user_data, const char *code,
                                          const char *severity,
                                          const AbString *path,
                                          const char *metric, size_t observed,
                                          size_t limit) {
  return append_metric_diagnostic((ResolutionState *)user_data, code, severity,
                                  path, metric, observed, limit);
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
      if (configured_map_field(state, "layers") ||
          configured_map_field(state, "limits"))
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
    if (keep)
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
      status = ab_buffer_literal(buffer, "{\"code\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->code, strlen(row->code));
    if (status == ARCHBIRD_OK && row->metric) {
      status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_json_string(buffer, row->metric, strlen(row->metric));
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, row->observed);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"limit\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, row->limit);
    }
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

static ArchbirdStatus render_manifest_requests(AbBuffer *buffer,
                                               const ResolutionState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0;
       status == ARCHBIRD_OK && index < state->manifests.candidate_count;
       index++) {
    const AbManifestCandidate *candidate = &state->manifests.candidates[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"evidence\":[");
    if (status == ARCHBIRD_OK) {
      const char *evidence =
          candidate->kind == AB_MANIFEST_CANDIDATE_NPM      ? "package.json"
          : candidate->kind == AB_MANIFEST_CANDIDATE_PYTHON ? "pyproject.toml"
          : candidate->kind == AB_MANIFEST_CANDIDATE_SETUP_CFG
              ? "setup.cfg"
              : "CMakeLists.txt";
      status = ab_buffer_json_string(buffer, evidence, strlen(evidence));
    }
    if (status == ARCHBIRD_OK && candidate->pattern) {
      status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, candidate->pattern->data,
                                       candidate->pattern->length);
    }
    if (status == ARCHBIRD_OK && candidate->witness) {
      status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, candidate->witness->data,
                                       candidate->witness->length);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"fulfilled\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_literal(buffer, candidate->supplied ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":");
    if (status == ARCHBIRD_OK) {
      const char *kind =
          candidate->kind == AB_MANIFEST_CANDIDATE_NPM         ? "npm"
          : candidate->kind == AB_MANIFEST_CANDIDATE_PYTHON    ? "python"
          : candidate->kind == AB_MANIFEST_CANDIDATE_SETUP_CFG ? "setup-cfg"
                                                               : "cmake";
      status = ab_buffer_json_string(buffer, kind, strlen(kind));
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"max_bytes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, AB_MANIFEST_DISCOVERY_MAX_BYTES);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, candidate->path->data,
                                     candidate->path->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, candidate->source,
                                     strlen(candidate->source));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus
render_manifest_request_summary(AbBuffer *buffer,
                                const ResolutionState *state) {
  static const char *const names[] = {"npm", "python", "setup-cfg", "cmake"};
  static const size_t limits[] = {
      AB_MANIFEST_DISCOVERY_NPM_LIMIT, AB_MANIFEST_DISCOVERY_PYTHON_LIMIT,
      AB_MANIFEST_DISCOVERY_ROOT_LIMIT, AB_MANIFEST_DISCOVERY_ROOT_LIMIT};
  size_t kind;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{");
  for (kind = 0;
       status == ARCHBIRD_OK && kind < AB_MANIFEST_CANDIDATE_KIND_COUNT;
       kind++) {
    if (kind)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, names[kind], strlen(names[kind]));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":{\"limit\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, limits[kind]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"matched\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, state->manifests.matches[kind]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"max_bytes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, AB_MANIFEST_DISCOVERY_MAX_BYTES);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"oversized\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, state->manifests.oversized[kind]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"requested\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, state->manifests.requested[kind]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
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
  const AbValue *packages = ab_value_member(&state->effective, "packages");
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
        buffer, &first, "/",
        state->configured_map_overlay.as.object.count ? "config-overlay"
                                                      : "discovery",
        state->configured_map_overlay.as.object.count ? "project-configuration"
                                                      : "archbird-discovery-v1",
        21);
  if (status == ARCHBIRD_OK)
    status =
        render_origin(buffer, &first, "/discovery/default_excludes",
                      request_default                            ? "cli"
                      : configured_map_field(state, "discovery") ? "config"
                                                                 : "discovery",
                      request_default ? "--no-default-excludes" : NULL,
                      request_default ? 21 : 0);
  for (index = 0;
       status == ARCHBIRD_OK && excludes && index < excludes->as.array.count;
       index++) {
    const AbValue *item = &excludes->as.array.items[index];
    status = render_indexed_origin(
        buffer, &first, "exclude", index,
        index < base_excludes
            ? configured_map_field(state, "exclude") ? "config" : "discovery"
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
        index < cli_layers                      ? "cli"
        : configured_map_field(state, "layers") ? "config"
                                                : "discovery",
        name && name->kind == AB_VALUE_STRING ? &name->as.text : NULL);
    if (status == ARCHBIRD_OK && !configured_map_field(state, "layers") &&
        ab_value_string_is(name, "auto-python")) {
      const AbValue *roots = ab_value_member(layer, "import_roots");
      size_t root_index;
      for (root_index = 0;
           status == ARCHBIRD_OK && roots && roots->kind == AB_VALUE_ARRAY &&
           root_index < roots->as.array.count;
           root_index++) {
        const AbValue *root = &roots->as.array.items[root_index];
        size_t inferred;
        for (inferred = 0; inferred < state->python_import_root_count;
             inferred++)
          if (root->kind == AB_VALUE_STRING &&
              ab_string_equal(&root->as.text,
                              &state->python_import_roots[inferred].root)) {
            char pointer[128];
            int pointer_length =
                snprintf(pointer, sizeof(pointer),
                         "/layers/%zu/import_roots/%zu", index, root_index);
            if (pointer_length < 0 || (size_t)pointer_length >= sizeof(pointer))
              status = ARCHBIRD_LIMIT_EXCEEDED;
            else
              status = render_origin(
                  buffer, &first, pointer, "manifest-candidate",
                  state->python_import_roots[inferred].manifest.data,
                  state->python_import_roots[inferred].manifest.length);
            break;
          }
      }
    }
  }
  for (index = 0;
       status == ARCHBIRD_OK && packages && index < packages->as.array.count;
       index++) {
    const AbValue *package = &packages->as.array.items[index];
    const AbValue *path = ab_value_member(package, "path");
    status = render_indexed_origin(
        buffer, &first, "packages", index,
        configured_map_field(state, "packages") ? "config" : "manifest",
        path && path->kind == AB_VALUE_STRING ? &path->as.text : NULL);
  }
  if (status == ARCHBIRD_OK)
    status = render_origin(
        buffer, &first, "/limits/max_file_bytes",
        state->request.has_max_file_bytes       ? "cli"
        : configured_map_field(state, "limits") ? "config"
                                                : "discovery",
        state->request.has_max_file_bytes ? "--max-file-bytes" : NULL,
        state->request.has_max_file_bytes ? 16 : 0);
  if (status == ARCHBIRD_OK)
    status = render_origin(
        buffer, &first, "/limits/max_index_bytes",
        state->request.has_max_index_bytes      ? "cli"
        : configured_map_field(state, "limits") ? "config"
                                                : "discovery",
        state->request.has_max_index_bytes ? "--max-index-bytes" : NULL,
        state->request.has_max_index_bytes ? 17 : 0);
  if (status == ARCHBIRD_OK) {
    const char *source = state->request.project                   ? "cli"
                         : configured_map_field(state, "project") ? "config"
                         : state->package_identity                ? "manifest"
                                                                  : "discovery";
    const char *evidence =
        state->request.project                   ? "--project"
        : configured_map_field(state, "project") ? "archbird.json"
        : state->package_identity == 1           ? "package.json"
        : state->package_identity == 2           ? "pyproject.toml"
        : state->package_identity == 3           ? "DESCRIPTION"
        : state->package_identity == 4           ? "configure.ac"
        : state->package_identity == 5           ? "workspace-manifests"
        : state->package_identity == 6           ? "setup.cfg"
        : state->package_identity == 7           ? "CMakeLists.txt"
                                                 : "stable-literal:repository";
    status = render_origin(buffer, &first, "/project", source, evidence,
                           evidence ? strlen(evidence) : 0);
  }
  if (status == ARCHBIRD_OK)
    status = render_origin(buffer, &first, "/root", "runtime",
                           "repository-root", 15);
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
  size_t index;
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
    status = ab_buffer_literal(&body, ",\"manifest_request_summary\":");
  if (status == ARCHBIRD_OK)
    status = render_manifest_request_summary(&body, state);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"manifest_requests\":");
  if (status == ARCHBIRD_OK)
    status = render_manifest_requests(&body, state);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, ",\"inventory\":[");
  for (index = 0; status == ARCHBIRD_OK && index < state->file_count; index++) {
    if (index)
      status = ab_buffer_literal(&body, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "{\"bytes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, state->files[index].bytes);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&body, state->files[index].path->data,
                                     state->files[index].path->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "]");
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
  ab_value_free(state->engine, &state->configured_map_overlay);
  ab_value_free(state->engine, &state->effective);
  ab_value_free(state->engine, &state->plan);
  for (index = 0; index < state->diagnostic_count; index++)
    ab_string_free(state->engine, &state->diagnostics[index].path);
  ab_free(state->engine, state->diagnostics);
  ab_manifest_discovery_free(&state->manifests);
  for (index = 0; index < state->python_import_root_count; index++)
    ab_string_free(state->engine, &state->python_import_roots[index].root);
  for (index = 0; index < state->python_import_root_count; index++)
    ab_string_free(state->engine, &state->python_import_roots[index].manifest);
  ab_free(state->engine, state->python_import_roots);
  memset(state, 0, sizeof(*state));
}

ArchbirdStatus
ab_discovery_resolve(ArchbirdEngine *engine, const uint8_t *config_json,
                     size_t config_length, const uint8_t *request_json,
                     size_t request_length, const uint8_t *inventory_json,
                     size_t inventory_length, uint32_t json_flags,
                     ArchbirdWriteFn write_fn, void *user_data) {
  ResolutionState state = {0};
  AbNpmDiscoveryMetadata npm = {0};
  AbString r_package = {0};
  AbString r_version = {0};
  AbPyprojectMetadata pyproject = {0};
  AbPythonPackageMetadata setup_cfg = {0};
  AbAutoconfMetadata autoconf = {0};
  AbCmakeProjectMetadata cmake = {0};
  AbBuffer effective_json;
  AbMapConfig validated = {0};
  int has_make = 0;
  size_t max_file_bytes = 0;
  size_t max_index_bytes = 0;
  ArchbirdStatus status;
  if (!engine || (!config_json && config_length) || !request_json ||
      !inventory_json || !write_fn || !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  state.engine = engine;
  ab_manifest_discovery_init(&state.manifests, engine);
  ab_ignore_set_init(&state.ignores, engine);
  ab_buffer_init(&effective_json, engine);
  status = decode_request(&state, request_json, request_length);
  if (status == ARCHBIRD_OK)
    status = decode_inventory(&state, inventory_json, inventory_length, &npm,
                              &pyproject, &setup_cfg, &r_package, &r_version,
                              &autoconf, &cmake, &has_make);
  if (status == ARCHBIRD_OK)
    status = prepare_effective(&state, config_json, config_length, &npm,
                               &pyproject, &setup_cfg, &r_package, &r_version,
                               &autoconf, &cmake, has_make);
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
  ab_npm_discovery_metadata_free(engine, &npm);
  ab_string_free(engine, &r_package);
  ab_string_free(engine, &r_version);
  ab_pyproject_metadata_free(engine, &pyproject);
  ab_python_package_metadata_free(engine, &setup_cfg);
  ab_autoconf_metadata_free(engine, &autoconf);
  ab_cmake_project_metadata_free(engine, &cmake);
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
