#include "json_internal.h"
#include "json_number.h"

#include "evidence.h"
#include "model.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct StringView {
  const char *data;
  size_t length;
} StringView;

static ArchbirdStatus schema_error(ArchbirdEngine *engine,
                                   const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static ArchbirdStatus schema_error_field(ArchbirdEngine *engine,
                                         const char *context, const char *field,
                                         const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s.%s %s", context, field, message);
}

static int bytes_equal(const char *left, size_t left_length, const char *right,
                       size_t right_length) {
  return left_length == right_length &&
         (left_length == 0 || memcmp(left, right, left_length) == 0);
}

static int bytes_compare(const char *left, size_t left_length,
                         const char *right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared != 0)
    return compared;
  return (left_length > right_length) - (left_length < right_length);
}

static int value_equals(yyjson_val *value, const char *literal) {
  return yyjson_is_str(value) &&
         bytes_equal(yyjson_get_str(value), yyjson_get_len(value), literal,
                     strlen(literal));
}

static yyjson_val *object_get(yyjson_val *object, const char *name) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t name_length = strlen(name);
  yyjson_obj_iter_init(object, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    if (bytes_equal(yyjson_get_str(key), yyjson_get_len(key), name,
                    name_length))
      return yyjson_obj_iter_get_val(key);
  }
  return NULL;
}

static int name_in(const char *name, size_t name_length,
                   const char *const *names, size_t name_count) {
  size_t index;
  for (index = 0; index < name_count; index++) {
    if (bytes_equal(name, name_length, names[index], strlen(names[index])))
      return 1;
  }
  return 0;
}

static ArchbirdStatus
validate_object_shape(ArchbirdEngine *engine, yyjson_val *value,
                      const char *context, const char *const *allowed,
                      size_t allowed_count, const char *const *required,
                      size_t required_count) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t index;
  if (!yyjson_is_obj(value)) {
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must be an object",
                              context);
  }
  yyjson_obj_iter_init(value, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    if (!name_in(yyjson_get_str(key), yyjson_get_len(key), allowed,
                 allowed_count)) {
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "%s contains unknown field '%.*s'", context,
                                (int)yyjson_get_len(key), yyjson_get_str(key));
    }
  }
  for (index = 0; index < required_count; index++) {
    if (!object_get(value, required[index])) {
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "%s is missing required field '%s'", context, required[index]);
    }
  }
  return ARCHBIRD_OK;
}

static int contains_nul(StringView value) {
  return value.length != 0 && memchr(value.data, '\0', value.length) != NULL;
}

static ArchbirdStatus require_string(ArchbirdEngine *engine, yyjson_val *value,
                                     const char *context, const char *field,
                                     int allow_empty, StringView *out) {
  if (!yyjson_is_str(value))
    return schema_error_field(engine, context, field, "must be a string");
  out->data = yyjson_get_str(value);
  out->length = yyjson_get_len(value);
  if ((!allow_empty && out->length == 0) || contains_nul(*out)) {
    return schema_error_field(engine, context, field,
                              "must be a non-NUL string");
  }
  return ARCHBIRD_OK;
}

static int ascii_id_valid(StringView value, int lowercase) {
  size_t index;
  unsigned char first;
  if (value.length == 0 || contains_nul(value))
    return 0;
  first = (unsigned char)value.data[0];
  if (lowercase) {
    if (first < 'a' || first > 'z')
      return 0;
  } else if (!isalnum(first)) {
    return 0;
  }
  for (index = 1; index < value.length; index++) {
    unsigned char byte = (unsigned char)value.data[index];
    if (lowercase) {
      if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
            byte == '_' || byte == '.' || byte == ':' || byte == '-'))
        return 0;
    } else if (!(isalnum(byte) || byte == '_' || byte == '.' || byte == ':' ||
                 byte == '-')) {
      return 0;
    }
  }
  return 1;
}

static ArchbirdStatus validate_id(ArchbirdEngine *engine, yyjson_val *value,
                                  const char *context, const char *field,
                                  int lowercase) {
  StringView string;
  ArchbirdStatus status =
      require_string(engine, value, context, field, 0, &string);
  if (status != ARCHBIRD_OK)
    return status;
  if (!ascii_id_valid(string, lowercase)) {
    return schema_error_field(engine, context, field,
                              lowercase ? "is not a lowercase vocabulary ID"
                                        : "is not a portable ID");
  }
  return ARCHBIRD_OK;
}

static int sha256_valid(yyjson_val *value) {
  size_t index;
  const char *string;
  if (!yyjson_is_str(value) || yyjson_get_len(value) != 64)
    return 0;
  string = yyjson_get_str(value);
  for (index = 0; index < 64; index++) {
    if (!((string[index] >= '0' && string[index] <= '9') ||
          (string[index] >= 'a' && string[index] <= 'f')))
      return 0;
  }
  return 1;
}

static ArchbirdStatus validate_sha256(ArchbirdEngine *engine, yyjson_val *value,
                                      const char *context, const char *field) {
  if (!sha256_valid(value))
    return schema_error_field(engine, context, field,
                              "must be 64 lowercase hexadecimal digits");
  return ARCHBIRD_OK;
}

static int path_valid(StringView path) {
  size_t start = 0;
  size_t index;
  if (path.length == 0 || contains_nul(path) || path.data[0] == '/' ||
      path.data[path.length - 1] == '/')
    return 0;
  if (path.length >= 2 && isalpha((unsigned char)path.data[0]) &&
      path.data[1] == ':')
    return 0;
  for (index = 0; index <= path.length; index++) {
    if (index < path.length && path.data[index] == '\\')
      return 0;
    if (index == path.length || path.data[index] == '/') {
      size_t segment_length = index - start;
      if (segment_length == 0 ||
          (segment_length == 1 && path.data[start] == '.') ||
          (segment_length == 2 && path.data[start] == '.' &&
           path.data[start + 1] == '.'))
        return 0;
      start = index + 1;
    }
  }
  return 1;
}

static ArchbirdStatus validate_path(ArchbirdEngine *engine, yyjson_val *value,
                                    const char *context, const char *field) {
  StringView path = {0};
  ArchbirdStatus status =
      require_string(engine, value, context, field, 0, &path);
  if (status != ARCHBIRD_OK)
    return status;
  if (!path_valid(path))
    return schema_error_field(engine, context, field,
                              "is not a canonical repository-relative path");
  return ARCHBIRD_OK;
}

static int parse_u64(yyjson_val *value, uint64_t *out) {
  const char *raw;
  size_t length;
  size_t index;
  uint64_t result = 0;
  if (!yyjson_is_raw(value))
    return 0;
  raw = yyjson_get_raw(value);
  length = yyjson_get_len(value);
  if (length == 0 || raw[0] == '-')
    return 0;
  for (index = 0; index < length; index++) {
    unsigned digit;
    if (raw[index] < '0' || raw[index] > '9')
      return 0;
    digit = (unsigned)(raw[index] - '0');
    if (result > (UINT64_MAX - digit) / UINT64_C(10))
      return 0;
    result = result * UINT64_C(10) + digit;
  }
  *out = result;
  return 1;
}

static ArchbirdStatus require_u64(ArchbirdEngine *engine, yyjson_val *value,
                                  const char *context, const char *field,
                                  uint64_t *out) {
  if (!parse_u64(value, out))
    return schema_error_field(engine, context, field,
                              "must be an unsigned 64-bit integer");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_literal(ArchbirdEngine *engine,
                                       yyjson_val *value, const char *context,
                                       const char *field,
                                       const char *expected) {
  if (!value_equals(value, expected)) {
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s.%s must equal '%s'",
                              context, field, expected);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_version_one(ArchbirdEngine *engine,
                                           yyjson_val *value,
                                           const char *context) {
  uint64_t version = 0;
  ArchbirdStatus status =
      require_u64(engine, value, context, "schema_version", &version);
  if (status != ARCHBIRD_OK)
    return status;
  if (version != 1)
    return schema_error_field(engine, context, "schema_version", "must be 1");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_producer(ArchbirdEngine *engine,
                                        yyjson_val *value, const char *context,
                                        int require_configuration) {
  static const char *const allowed_source[] = {"implementation_sha256", "name",
                                               "version"};
  static const char *const allowed_provider[] = {"configuration_sha256",
                                                 "implementation_sha256",
                                                 "name", "runtime", "version"};
  static const char *const required_source[] = {"implementation_sha256", "name",
                                                "version"};
  static const char *const required_provider[] = {
      "configuration_sha256", "implementation_sha256", "name", "version"};
  StringView ignored;
  ArchbirdStatus status = validate_object_shape(
      engine, value, context,
      require_configuration ? allowed_provider : allowed_source,
      require_configuration
          ? sizeof(allowed_provider) / sizeof(allowed_provider[0])
          : sizeof(allowed_source) / sizeof(allowed_source[0]),
      require_configuration ? required_provider : required_source,
      require_configuration
          ? sizeof(required_provider) / sizeof(required_provider[0])
          : sizeof(required_source) / sizeof(required_source[0]));
  if (status != ARCHBIRD_OK)
    return status;
  status = validate_id(engine, object_get(value, "name"), context, "name", 0);
  if (status == ARCHBIRD_OK)
    status = require_string(engine, object_get(value, "version"), context,
                            "version", 0, &ignored);
  if (status == ARCHBIRD_OK)
    status = validate_sha256(engine, object_get(value, "implementation_sha256"),
                             context, "implementation_sha256");
  if (status == ARCHBIRD_OK && require_configuration) {
    status = validate_sha256(engine, object_get(value, "configuration_sha256"),
                             context, "configuration_sha256");
  }
  if (status == ARCHBIRD_OK && object_get(value, "runtime")) {
    status = require_string(engine, object_get(value, "runtime"), context,
                            "runtime", 0, &ignored);
  }
  return status;
}

static ArchbirdStatus validate_sorted_id_array(ArchbirdEngine *engine,
                                               yyjson_val *value,
                                               const char *context,
                                               int lowercase,
                                               int require_nonempty) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  if (!yyjson_is_arr(value))
    return schema_error(engine, "expected an array");
  if (require_nonempty && yyjson_arr_size(value) == 0) {
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must not be empty",
                              context);
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView current = {0};
    ArchbirdStatus status =
        require_string(engine, item, context, "item", 0, &current);
    if (status != ARCHBIRD_OK)
      return status;
    if (!ascii_id_valid(current, lowercase)) {
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s contains an invalid ID",
                                context);
    }
    if (index && bytes_compare(previous.data, previous.length, current.data,
                               current.length) >= 0) {
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "%s must be strictly sorted with unique members", context);
    }
    previous = current;
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_source_file(ArchbirdEngine *engine,
                                           yyjson_val *value,
                                           StringView *out_path) {
  static const char *const allowed[] = {"bytes", "language", "layer",
                                        "path",  "roles",    "sha256"};
  static const char *const required[] = {"bytes", "path", "roles", "sha256"};
  uint64_t ignored;
  ArchbirdStatus status =
      validate_object_shape(engine, value, "source-manifest.files[]", allowed,
                            sizeof(allowed) / sizeof(allowed[0]), required,
                            sizeof(required) / sizeof(required[0]));
  if (status != ARCHBIRD_OK)
    return status;
  status = validate_path(engine, object_get(value, "path"),
                         "source-manifest.files[]", "path");
  if (status == ARCHBIRD_OK)
    status = validate_sha256(engine, object_get(value, "sha256"),
                             "source-manifest.files[]", "sha256");
  if (status == ARCHBIRD_OK)
    status = require_u64(engine, object_get(value, "bytes"),
                         "source-manifest.files[]", "bytes", &ignored);
  if (status == ARCHBIRD_OK) {
    status = validate_sorted_id_array(engine, object_get(value, "roles"),
                                      "source-manifest.files[].roles", 0, 1);
  }
  if (status == ARCHBIRD_OK && object_get(value, "language")) {
    status = validate_id(engine, object_get(value, "language"),
                         "source-manifest.files[]", "language", 0);
  }
  if (status == ARCHBIRD_OK && object_get(value, "layer")) {
    status = validate_id(engine, object_get(value, "layer"),
                         "source-manifest.files[]", "layer", 0);
  }
  if (status == ARCHBIRD_OK) {
    out_path->data = yyjson_get_str(object_get(value, "path"));
    out_path->length = yyjson_get_len(object_get(value, "path"));
  }
  return status;
}

static ArchbirdStatus
archbird_validate_source_manifest_root(ArchbirdEngine *engine,
                                       yyjson_val *root) {
  static const char *const allowed[] = {
      "artifact", "configuration_sha256", "files",         "producer",
      "project",  "resolution",           "schema_version"};
  static const char *const required[] = {"artifact", "files", "producer",
                                         "project", "schema_version"};
  yyjson_val *files;
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  ArchbirdStatus status =
      validate_object_shape(engine, root, "source-manifest", allowed,
                            sizeof(allowed) / sizeof(allowed[0]), required,
                            sizeof(required) / sizeof(required[0]));
  if (status != ARCHBIRD_OK)
    return status;
  status = validate_version_one(engine, object_get(root, "schema_version"),
                                "source-manifest");
  if (status == ARCHBIRD_OK) {
    status = validate_literal(engine, object_get(root, "artifact"),
                              "source-manifest", "artifact",
                              "archbird-source-manifest");
  }
  if (status == ARCHBIRD_OK) {
    status = validate_id(engine, object_get(root, "project"), "source-manifest",
                         "project", 0);
  }
  if (status == ARCHBIRD_OK) {
    status = validate_producer(engine, object_get(root, "producer"),
                               "source-manifest.producer", 0);
  }
  if (status == ARCHBIRD_OK && object_get(root, "configuration_sha256")) {
    status = validate_sha256(engine, object_get(root, "configuration_sha256"),
                             "source-manifest", "configuration_sha256");
  }
  if (status == ARCHBIRD_OK && object_get(root, "resolution")) {
    static const char *const resolution_allowed[] = {"coverage", "profile",
                                                     "sha256"};
    static const char *const resolution_required[] = {"coverage", "profile",
                                                      "sha256"};
    static const char *const profile_allowed[] = {"implementation_sha256",
                                                  "name"};
    static const char *const profile_required[] = {"implementation_sha256",
                                                   "name"};
    static const char *const coverage_fields[] = {"assets",
                                                  "ignored",
                                                  "inventory_files",
                                                  "oversized",
                                                  "pruned_directories",
                                                  "selected",
                                                  "unsupported_known"};
    yyjson_val *resolution = object_get(root, "resolution");
    yyjson_val *profile = object_get(resolution, "profile");
    yyjson_val *coverage = object_get(resolution, "coverage");
    size_t field_index;
    uint64_t ignored_value;
    status = validate_object_shape(
        engine, resolution, "source-manifest.resolution", resolution_allowed,
        sizeof(resolution_allowed) / sizeof(resolution_allowed[0]),
        resolution_required,
        sizeof(resolution_required) / sizeof(resolution_required[0]));
    if (status == ARCHBIRD_OK)
      status = validate_sha256(engine, object_get(resolution, "sha256"),
                               "source-manifest.resolution", "sha256");
    if (status == ARCHBIRD_OK)
      status = validate_object_shape(
          engine, profile, "source-manifest.resolution.profile",
          profile_allowed, sizeof(profile_allowed) / sizeof(profile_allowed[0]),
          profile_required,
          sizeof(profile_required) / sizeof(profile_required[0]));
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(profile, "name"),
                           "source-manifest.resolution.profile", "name", 0);
    if (status == ARCHBIRD_OK)
      status = validate_sha256(
          engine, object_get(profile, "implementation_sha256"),
          "source-manifest.resolution.profile", "implementation_sha256");
    if (status == ARCHBIRD_OK)
      status = validate_object_shape(
          engine, coverage, "source-manifest.resolution.coverage",
          coverage_fields, sizeof(coverage_fields) / sizeof(coverage_fields[0]),
          coverage_fields,
          sizeof(coverage_fields) / sizeof(coverage_fields[0]));
    for (field_index = 0;
         status == ARCHBIRD_OK &&
         field_index < sizeof(coverage_fields) / sizeof(coverage_fields[0]);
         field_index++)
      status = require_u64(engine,
                           object_get(coverage, coverage_fields[field_index]),
                           "source-manifest.resolution.coverage",
                           coverage_fields[field_index], &ignored_value);
  }
  if (status != ARCHBIRD_OK)
    return status;
  files = object_get(root, "files");
  if (!yyjson_is_arr(files))
    return schema_error_field(engine, "source-manifest", "files",
                              "must be an array");
  yyjson_arr_iter_init(files, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView current = {0};
    status = validate_source_file(engine, item, &current);
    if (status != ARCHBIRD_OK)
      return status;
    if (index && bytes_compare(previous.data, previous.length, current.data,
                               current.length) >= 0) {
      return schema_error_field(
          engine, "source-manifest", "files",
          "must be strictly sorted by path with unique paths");
    }
    previous = current;
    index++;
  }
  if (object_get(root, "resolution")) {
    yyjson_val *coverage =
        object_get(object_get(root, "resolution"), "coverage");
    uint64_t selected = 0;
    (void)parse_u64(object_get(coverage, "selected"), &selected);
    if (selected != yyjson_arr_size(files))
      return schema_error_field(
          engine, "source-manifest.resolution.coverage", "selected",
          "must equal the number of source-manifest files");
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_source_manifest_validate(ArchbirdEngine *engine,
                                                 const uint8_t *input,
                                                 size_t input_length) {
  yyjson_doc *document = NULL;
  ArchbirdStatus status;
  if (!engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  status =
      archbird_json_parse_document(engine, input, input_length, &document, 1);
  if (status == ARCHBIRD_OK) {
    status = archbird_validate_source_manifest_root(
        engine, yyjson_doc_get_root(document));
  }
  yyjson_doc_free(document);
  return status;
}

static ArchbirdStatus validate_subject(ArchbirdEngine *engine,
                                       yyjson_val *value) {
  static const char *const file_required[] = {"path", "project", "scope"};
  static const char *const package_required[] = {"name", "project", "scope"};
  static const char *const project_required[] = {"project", "scope"};
  static const char *const workspace_required[] = {"name", "scope"};
  yyjson_val *scope;
  const char *const *required;
  size_t required_count;
  ArchbirdStatus status;
  if (!yyjson_is_obj(value))
    return schema_error(engine, "provider-facts.subject must be an object");
  scope = object_get(value, "scope");
  if (value_equals(scope, "file")) {
    required = file_required;
    required_count = sizeof(file_required) / sizeof(file_required[0]);
  } else if (value_equals(scope, "package")) {
    required = package_required;
    required_count = sizeof(package_required) / sizeof(package_required[0]);
  } else if (value_equals(scope, "project")) {
    required = project_required;
    required_count = sizeof(project_required) / sizeof(project_required[0]);
  } else if (value_equals(scope, "workspace")) {
    required = workspace_required;
    required_count = sizeof(workspace_required) / sizeof(workspace_required[0]);
  } else {
    return schema_error_field(engine, "provider-facts.subject", "scope",
                              "must be file, package, project, or workspace");
  }
  status =
      validate_object_shape(engine, value, "provider-facts.subject", required,
                            required_count, required, required_count);
  if (status != ARCHBIRD_OK)
    return status;
  if (object_get(value, "project")) {
    status = validate_id(engine, object_get(value, "project"),
                         "provider-facts.subject", "project", 0);
  }
  if (status == ARCHBIRD_OK && object_get(value, "name")) {
    status = validate_id(engine, object_get(value, "name"),
                         "provider-facts.subject", "name", 0);
  }
  if (status == ARCHBIRD_OK && object_get(value, "path")) {
    status = validate_path(engine, object_get(value, "path"),
                           "provider-facts.subject", "path");
  }
  return status;
}

static ArchbirdStatus validate_inputs(ArchbirdEngine *engine,
                                      yyjson_val *value) {
  static const char *const manifest_fields[] = {"project",
                                                "source_manifest_sha256"};
  static const char *const source_fields[] = {"path", "project",
                                              "source_sha256"};
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous_project = {0};
  StringView previous_path = {0};
  int previous_source = 0;
  size_t index = 0;
  if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0) {
    return schema_error_field(engine, "provider-facts", "inputs",
                              "must be a nonempty array");
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    yyjson_val *manifest_sha = object_get(item, "source_manifest_sha256");
    yyjson_val *source_sha = object_get(item, "source_sha256");
    yyjson_val *path = object_get(item, "path");
    StringView current_project = {0};
    StringView current_path = {0};
    int current_source = source_sha != NULL;
    ArchbirdStatus status;
    if ((manifest_sha != NULL) == (source_sha != NULL))
      return schema_error_field(engine, "provider-facts.inputs[]", "source",
                                "must contain exactly one of "
                                "source_manifest_sha256 or source_sha256");
    if ((source_sha != NULL) != (path != NULL))
      return schema_error_field(
          engine, "provider-facts.inputs[]", "path",
          "is required exactly when source_sha256 is present");
    if (source_sha) {
      status = validate_object_shape(
          engine, item, "provider-facts.inputs[]", source_fields,
          sizeof(source_fields) / sizeof(source_fields[0]), source_fields,
          sizeof(source_fields) / sizeof(source_fields[0]));
    } else {
      status = validate_object_shape(
          engine, item, "provider-facts.inputs[]", manifest_fields,
          sizeof(manifest_fields) / sizeof(manifest_fields[0]), manifest_fields,
          sizeof(manifest_fields) / sizeof(manifest_fields[0]));
    }
    if (status == ARCHBIRD_OK) {
      status = validate_id(engine, object_get(item, "project"),
                           "provider-facts.inputs[]", "project", 0);
    }
    if (status == ARCHBIRD_OK && source_sha) {
      status = validate_path(engine, path, "provider-facts.inputs[]", "path");
    }
    if (status == ARCHBIRD_OK && source_sha) {
      status = validate_sha256(engine, source_sha, "provider-facts.inputs[]",
                               "source_sha256");
    }
    if (status == ARCHBIRD_OK && manifest_sha) {
      status = validate_sha256(engine, manifest_sha, "provider-facts.inputs[]",
                               "source_manifest_sha256");
    }
    if (status != ARCHBIRD_OK)
      return status;
    current_project.data = yyjson_get_str(object_get(item, "project"));
    current_project.length = yyjson_get_len(object_get(item, "project"));
    if (source_sha) {
      current_path.data = yyjson_get_str(path);
      current_path.length = yyjson_get_len(path);
    }
    if (index) {
      int compared =
          bytes_compare(previous_project.data, previous_project.length,
                        current_project.data, current_project.length);
      if (compared == 0 && previous_source != current_source)
        compared = previous_source ? 1 : -1;
      if (compared == 0 && current_source)
        compared = bytes_compare(previous_path.data, previous_path.length,
                                 current_path.data, current_path.length);
      if (compared >= 0) {
        return schema_error_field(
            engine, "provider-facts", "inputs",
            "must be strictly sorted by project, input kind, and path");
      }
    }
    if (index && previous_source != current_source &&
        bytes_compare(previous_project.data, previous_project.length,
                      current_project.data, current_project.length) == 0) {
      return schema_error_field(
          engine, "provider-facts", "inputs",
          "must not mix manifest and source bindings for one project");
    }
    previous_project = current_project;
    previous_path = current_path;
    previous_source = current_source;
    index++;
  }
  return ARCHBIRD_OK;
}

static int string_array_contains(yyjson_val *array, StringView wanted) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    if (bytes_equal(yyjson_get_str(item), yyjson_get_len(item), wanted.data,
                    wanted.length))
      return 1;
  }
  return 0;
}

static yyjson_val *find_capability(yyjson_val *capabilities,
                                   StringView domain) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  yyjson_arr_iter_init(capabilities, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    yyjson_val *value = object_get(item, "domain");
    int compared = bytes_compare(yyjson_get_str(value), yyjson_get_len(value),
                                 domain.data, domain.length);
    if (compared == 0)
      return item;
    if (compared > 0)
      break;
  }
  return NULL;
}

static ArchbirdStatus validate_capabilities(ArchbirdEngine *engine,
                                            yyjson_val *value) {
  static const char *const allowed[] = {"boundary", "claims", "coverage",
                                        "domain"};
  static const char *const required[] = {"claims", "coverage", "domain"};
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  if (!yyjson_is_arr(value)) {
    return schema_error_field(engine, "provider-facts", "capabilities",
                              "must be an array");
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView current = {0};
    StringView ignored;
    yyjson_val *coverage;
    ArchbirdStatus status =
        validate_object_shape(engine, item, "provider-facts.capabilities[]",
                              allowed, sizeof(allowed) / sizeof(allowed[0]),
                              required, sizeof(required) / sizeof(required[0]));
    if (status == ARCHBIRD_OK) {
      status = validate_id(engine, object_get(item, "domain"),
                           "provider-facts.capabilities[]", "domain", 1);
    }
    coverage = object_get(item, "coverage");
    if (status == ARCHBIRD_OK && !(value_equals(coverage, "complete") ||
                                   value_equals(coverage, "bounded") ||
                                   value_equals(coverage, "partial") ||
                                   value_equals(coverage, "none"))) {
      status = schema_error_field(engine, "provider-facts.capabilities[]",
                                  "coverage", "has an invalid value");
    }
    if (status == ARCHBIRD_OK) {
      status = validate_sorted_id_array(engine, object_get(item, "claims"),
                                        "provider-facts.capabilities[].claims",
                                        1, 1);
    }
    if (status == ARCHBIRD_OK && object_get(item, "boundary")) {
      status = require_string(engine, object_get(item, "boundary"),
                              "provider-facts.capabilities[]", "boundary", 0,
                              &ignored);
    }
    if (status != ARCHBIRD_OK)
      return status;
    current.data = yyjson_get_str(object_get(item, "domain"));
    current.length = yyjson_get_len(object_get(item, "domain"));
    if (index && bytes_compare(previous.data, previous.length, current.data,
                               current.length) >= 0) {
      return schema_error_field(
          engine, "provider-facts", "capabilities",
          "must be strictly sorted by domain with unique domains");
    }
    previous = current;
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_span(ArchbirdEngine *engine, yyjson_val *value,
                                    const char *context, uint64_t *out_start,
                                    uint64_t *out_end) {
  static const char *const fields[] = {"end", "start"};
  ArchbirdStatus status = validate_object_shape(
      engine, value, context, fields, sizeof(fields) / sizeof(fields[0]),
      fields, sizeof(fields) / sizeof(fields[0]));
  if (status == ARCHBIRD_OK)
    status = require_u64(engine, object_get(value, "start"), context, "start",
                         out_start);
  if (status == ARCHBIRD_OK)
    status =
        require_u64(engine, object_get(value, "end"), context, "end", out_end);
  if (status == ARCHBIRD_OK && *out_start > *out_end)
    return schema_error(engine, "provider fact span start must not exceed end");
  return status;
}

static int scalar_valid(yyjson_val *value) {
  if (yyjson_is_null(value) || yyjson_is_bool(value) || yyjson_is_str(value))
    return 1;
  if (yyjson_is_raw(value)) {
    const char *raw = yyjson_get_raw(value);
    size_t length = yyjson_get_len(value);
    size_t index = raw[0] == '-' ? 1 : 0;
    if (index == length)
      return 0;
    for (; index < length; index++) {
      if (raw[index] < '0' || raw[index] > '9')
        return 0;
    }
    return 1;
  }
  return 0;
}

static ArchbirdStatus validate_fact_value(ArchbirdEngine *engine,
                                          yyjson_val *value) {
  if (scalar_valid(value))
    return ARCHBIRD_OK;
  if (yyjson_is_arr(value)) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      if (!scalar_valid(item))
        return schema_error(engine,
                            "fact attribute arrays must contain scalars");
    }
    return ARCHBIRD_OK;
  }
  if (yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    yyjson_obj_iter_init(value, &iterator);
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
      if (!scalar_valid(yyjson_obj_iter_get_val(key))) {
        return schema_error(
            engine, "fact attribute objects must contain scalar values");
      }
    }
    return ARCHBIRD_OK;
  }
  return schema_error(engine, "fact attribute has an unsupported value shape");
}

static ArchbirdStatus validate_attributes(ArchbirdEngine *engine,
                                          yyjson_val *value) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  if (!yyjson_is_obj(value))
    return schema_error(engine, "provider fact attributes must be an object");
  yyjson_obj_iter_init(value, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    StringView name = {yyjson_get_str(key), yyjson_get_len(key)};
    ArchbirdStatus status;
    if (!ascii_id_valid(name, 1))
      return schema_error(engine,
                          "fact attribute names must be vocabulary IDs");
    status = validate_fact_value(engine, yyjson_obj_iter_get_val(key));
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int project_is_bound(yyjson_val *inputs, StringView project) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  yyjson_arr_iter_init(inputs, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    yyjson_val *value = object_get(item, "project");
    int compared = bytes_compare(yyjson_get_str(value), yyjson_get_len(value),
                                 project.data, project.length);
    if (compared == 0)
      return 1;
    if (compared > 0)
      return 0;
  }
  return 0;
}

static ArchbirdStatus validate_facts(ArchbirdEngine *engine, yyjson_val *value,
                                     yyjson_val *subject, yyjson_val *inputs,
                                     yyjson_val *capabilities) {
  static const char *const allowed[] = {
      "attributes", "claim", "correlation", "domain",  "id",  "key",
      "kind",       "name",  "path",        "project", "span"};
  static const char *const required[] = {"claim", "domain", "id",      "key",
                                         "kind",  "path",   "project", "span"};
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  if (!yyjson_is_arr(value))
    return schema_error_field(engine, "provider-facts", "facts",
                              "must be an array");
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView id;
    StringView domain;
    StringView claim;
    StringView project;
    StringView ignored;
    uint64_t start;
    uint64_t end;
    yyjson_val *capability;
    ArchbirdStatus status =
        validate_object_shape(engine, item, "provider-facts.facts[]", allowed,
                              sizeof(allowed) / sizeof(allowed[0]), required,
                              sizeof(required) / sizeof(required[0]));
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "id"),
                           "provider-facts.facts[]", "id", 0);
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "domain"),
                           "provider-facts.facts[]", "domain", 1);
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "kind"),
                           "provider-facts.facts[]", "kind", 1);
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "claim"),
                           "provider-facts.facts[]", "claim", 1);
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "project"),
                           "provider-facts.facts[]", "project", 0);
    if (status == ARCHBIRD_OK)
      status = validate_path(engine, object_get(item, "path"),
                             "provider-facts.facts[]", "path");
    if (status == ARCHBIRD_OK)
      status = require_string(engine, object_get(item, "key"),
                              "provider-facts.facts[]", "key", 0, &ignored);
    if (status == ARCHBIRD_OK && object_get(item, "name")) {
      status = require_string(engine, object_get(item, "name"),
                              "provider-facts.facts[]", "name", 1, &ignored);
    }
    if (status == ARCHBIRD_OK && object_get(item, "correlation") &&
        !value_equals(object_get(item, "correlation"), "span") &&
        !value_equals(object_get(item, "correlation"), "key")) {
      status =
          schema_error(engine, "provider fact correlation must be span or key");
    }
    if (status == ARCHBIRD_OK)
      status = validate_span(engine, object_get(item, "span"),
                             "provider-facts.facts[].span", &start, &end);
    if (status == ARCHBIRD_OK && object_get(item, "attributes"))
      status = validate_attributes(engine, object_get(item, "attributes"));
    if (status != ARCHBIRD_OK)
      return status;

    id.data = yyjson_get_str(object_get(item, "id"));
    id.length = yyjson_get_len(object_get(item, "id"));
    if (index && bytes_compare(previous.data, previous.length, id.data,
                               id.length) >= 0) {
      return schema_error_field(
          engine, "provider-facts", "facts",
          "must be strictly sorted by id with unique ids");
    }
    previous = id;
    index++;

    project.data = yyjson_get_str(object_get(item, "project"));
    project.length = yyjson_get_len(object_get(item, "project"));
    if (!project_is_bound(inputs, project)) {
      return schema_error(engine,
                          "provider fact project has no bound source input");
    }
    if (value_equals(object_get(subject, "scope"), "file")) {
      yyjson_val *subject_project = object_get(subject, "project");
      yyjson_val *subject_path = object_get(subject, "path");
      if (!bytes_equal(project.data, project.length,
                       yyjson_get_str(subject_project),
                       yyjson_get_len(subject_project)) ||
          !bytes_equal(yyjson_get_str(object_get(item, "path")),
                       yyjson_get_len(object_get(item, "path")),
                       yyjson_get_str(subject_path),
                       yyjson_get_len(subject_path))) {
        return schema_error(engine,
                            "file-scoped provider fact is outside its subject");
      }
    }
    domain.data = yyjson_get_str(object_get(item, "domain"));
    domain.length = yyjson_get_len(object_get(item, "domain"));
    claim.data = yyjson_get_str(object_get(item, "claim"));
    claim.length = yyjson_get_len(object_get(item, "claim"));
    capability = find_capability(capabilities, domain);
    if (!capability)
      return schema_error(engine,
                          "provider fact domain has no declared capability");
    if (value_equals(object_get(capability, "coverage"), "none")) {
      return schema_error(engine,
                          "coverage none cannot contain facts in that domain");
    }
    if (!string_array_contains(object_get(capability, "claims"), claim)) {
      return schema_error(engine,
                          "provider fact claim is absent from its capability");
    }
  }
  return ARCHBIRD_OK;
}

static yyjson_val *find_fact(yyjson_val *facts, StringView id) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  yyjson_arr_iter_init(facts, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    yyjson_val *value = object_get(item, "id");
    int compared = bytes_compare(yyjson_get_str(value), yyjson_get_len(value),
                                 id.data, id.length);
    if (compared == 0)
      return item;
    if (compared > 0)
      break;
  }
  return NULL;
}

static ArchbirdStatus validate_targets(ArchbirdEngine *engine,
                                       yyjson_val *value, size_t *out_count) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  if (!yyjson_is_arr(value))
    return schema_error(engine, "resolution targets must be an array");
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView current = {0};
    ArchbirdStatus status =
        require_string(engine, item, "provider-facts.resolutions[].targets",
                       "item", 0, &current);
    if (status != ARCHBIRD_OK)
      return status;
    if (index && bytes_compare(previous.data, previous.length, current.data,
                               current.length) >= 0) {
      return schema_error(engine,
                          "resolution targets must be sorted and unique");
    }
    previous = current;
    index++;
  }
  *out_count = index;
  return ARCHBIRD_OK;
}

static int targetless_state(yyjson_val *state) {
  return value_equals(state, "unresolved") || value_equals(state, "builtin") ||
         value_equals(state, "unknown") ||
         value_equals(state, "not-applicable");
}

static ArchbirdStatus validate_resolutions(ArchbirdEngine *engine,
                                           yyjson_val *value,
                                           yyjson_val *facts) {
  static const char *const allowed[] = {"fact_id", "reason", "state",
                                        "targets"};
  static const char *const required[] = {"fact_id", "state", "targets"};
  static const char *const states[] = {
      "unique",  "ambiguous", "unresolved", "external",
      "builtin", "conflict",  "unknown",    "not-applicable"};
  yyjson_arr_iter iterator;
  yyjson_val *item;
  StringView previous = {0};
  size_t index = 0;
  if (!yyjson_is_arr(value))
    return schema_error_field(engine, "provider-facts", "resolutions",
                              "must be an array");
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView id;
    StringView ignored;
    yyjson_val *state;
    size_t target_count = 0;
    size_t state_index;
    int state_valid = 0;
    ArchbirdStatus status =
        validate_object_shape(engine, item, "provider-facts.resolutions[]",
                              allowed, sizeof(allowed) / sizeof(allowed[0]),
                              required, sizeof(required) / sizeof(required[0]));
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "fact_id"),
                           "provider-facts.resolutions[]", "fact_id", 0);
    state = object_get(item, "state");
    for (state_index = 0; state_index < sizeof(states) / sizeof(states[0]);
         state_index++) {
      if (value_equals(state, states[state_index])) {
        state_valid = 1;
        break;
      }
    }
    if (status == ARCHBIRD_OK && !state_valid)
      status = schema_error(engine, "provider resolution state is invalid");
    if (status == ARCHBIRD_OK)
      status =
          validate_targets(engine, object_get(item, "targets"), &target_count);
    if (status == ARCHBIRD_OK && object_get(item, "reason")) {
      status =
          require_string(engine, object_get(item, "reason"),
                         "provider-facts.resolutions[]", "reason", 0, &ignored);
    }
    if (status != ARCHBIRD_OK)
      return status;
    id.data = yyjson_get_str(object_get(item, "fact_id"));
    id.length = yyjson_get_len(object_get(item, "fact_id"));
    if (index && bytes_compare(previous.data, previous.length, id.data,
                               id.length) >= 0) {
      return schema_error_field(
          engine, "provider-facts", "resolutions",
          "must be strictly sorted by fact_id with unique facts");
    }
    previous = id;
    index++;
    if (!find_fact(facts, id))
      return schema_error(engine, "resolution names an unknown fact_id");
    if ((value_equals(state, "unique") && target_count != 1) ||
        (value_equals(state, "ambiguous") && target_count < 2) ||
        (value_equals(state, "external") && target_count == 0) ||
        (value_equals(state, "conflict") && target_count < 2) ||
        (targetless_state(state) && target_count != 0)) {
      return schema_error(engine,
                          "resolution state and target cardinality disagree");
    }
  }
  return ARCHBIRD_OK;
}

static StringView optional_string_member(yyjson_val *object, const char *name) {
  yyjson_val *value = object_get(object, name);
  StringView result = {"", 0};
  if (value) {
    result.data = yyjson_get_str(value);
    result.length = yyjson_get_len(value);
  }
  return result;
}

static uint64_t optional_span_member(yyjson_val *object, const char *name) {
  yyjson_val *span = object_get(object, "span");
  yyjson_val *value;
  uint64_t result = 0;
  if (!span)
    return 0;
  value = object_get(span, name);
  (void)parse_u64(value, &result);
  return result;
}

static int compare_diagnostics(yyjson_val *left, yyjson_val *right) {
  static const char *const string_fields[] = {"code", "project", "path",
                                              "severity", "message"};
  size_t index;
  for (index = 0; index < 3; index++) {
    StringView left_value = optional_string_member(left, string_fields[index]);
    StringView right_value =
        optional_string_member(right, string_fields[index]);
    int compared = bytes_compare(left_value.data, left_value.length,
                                 right_value.data, right_value.length);
    if (compared != 0)
      return compared;
  }
  {
    uint64_t left_start = optional_span_member(left, "start");
    uint64_t right_start = optional_span_member(right, "start");
    if (left_start != right_start)
      return left_start < right_start ? -1 : 1;
  }
  {
    uint64_t left_end = optional_span_member(left, "end");
    uint64_t right_end = optional_span_member(right, "end");
    if (left_end != right_end)
      return left_end < right_end ? -1 : 1;
  }
  for (index = 3; index < sizeof(string_fields) / sizeof(string_fields[0]);
       index++) {
    StringView left_value = optional_string_member(left, string_fields[index]);
    StringView right_value =
        optional_string_member(right, string_fields[index]);
    int compared = bytes_compare(left_value.data, left_value.length,
                                 right_value.data, right_value.length);
    if (compared != 0)
      return compared;
  }
  return 0;
}

static ArchbirdStatus validate_diagnostics(ArchbirdEngine *engine,
                                           yyjson_val *value) {
  static const char *const allowed[] = {"code",    "message",  "path",
                                        "project", "severity", "span"};
  static const char *const required[] = {"code", "message", "severity"};
  yyjson_arr_iter iterator;
  yyjson_val *item;
  yyjson_val *previous = NULL;
  if (!yyjson_is_arr(value))
    return schema_error_field(engine, "provider-facts", "diagnostics",
                              "must be an array");
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    StringView ignored;
    uint64_t start;
    uint64_t end;
    yyjson_val *severity;
    ArchbirdStatus status =
        validate_object_shape(engine, item, "provider-facts.diagnostics[]",
                              allowed, sizeof(allowed) / sizeof(allowed[0]),
                              required, sizeof(required) / sizeof(required[0]));
    severity = object_get(item, "severity");
    if (status == ARCHBIRD_OK && !(value_equals(severity, "error") ||
                                   value_equals(severity, "warning") ||
                                   value_equals(severity, "note"))) {
      status = schema_error(engine, "provider diagnostic severity is invalid");
    }
    if (status == ARCHBIRD_OK)
      status = validate_id(engine, object_get(item, "code"),
                           "provider-facts.diagnostics[]", "code", 1);
    if (status == ARCHBIRD_OK)
      status = require_string(engine, object_get(item, "message"),
                              "provider-facts.diagnostics[]", "message", 0,
                              &ignored);
    if (status == ARCHBIRD_OK && object_get(item, "project")) {
      status = validate_id(engine, object_get(item, "project"),
                           "provider-facts.diagnostics[]", "project", 0);
    }
    if (status == ARCHBIRD_OK && object_get(item, "path")) {
      status = validate_path(engine, object_get(item, "path"),
                             "provider-facts.diagnostics[]", "path");
    }
    if (status == ARCHBIRD_OK && object_get(item, "span")) {
      if (!object_get(item, "path")) {
        return schema_error(engine,
                            "provider diagnostic span requires a source path");
      }
      status = validate_span(engine, object_get(item, "span"),
                             "provider-facts.diagnostics[].span", &start, &end);
    }
    if (status == ARCHBIRD_OK && object_get(item, "path") &&
        !object_get(item, "project")) {
      return schema_error(engine,
                          "provider diagnostic path requires a project");
    }
    if (status != ARCHBIRD_OK)
      return status;
    if (previous && compare_diagnostics(previous, item) >= 0) {
      return schema_error_field(engine, "provider-facts", "diagnostics",
                                "must be canonically sorted with unique rows");
    }
    previous = item;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
archbird_validate_provider_facts_root(ArchbirdEngine *engine,
                                      yyjson_val *root) {
  static const char *const fields[] = {
      "artifact", "capabilities", "diagnostics", "facts",          "inputs",
      "producer", "provenance",   "resolutions", "schema_version", "subject"};
  yyjson_val *subject;
  yyjson_val *inputs;
  yyjson_val *capabilities;
  yyjson_val *facts;
  ArchbirdStatus status =
      validate_object_shape(engine, root, "provider-facts", fields,
                            sizeof(fields) / sizeof(fields[0]), fields,
                            sizeof(fields) / sizeof(fields[0]));
  if (status == ARCHBIRD_OK)
    status = validate_version_one(engine, object_get(root, "schema_version"),
                                  "provider-facts");
  if (status == ARCHBIRD_OK)
    status =
        validate_literal(engine, object_get(root, "artifact"), "provider-facts",
                         "artifact", "archbird-provider-facts");
  if (status == ARCHBIRD_OK)
    status = validate_literal(engine, object_get(root, "provenance"),
                              "provider-facts", "provenance", "derived");
  subject = object_get(root, "subject");
  inputs = object_get(root, "inputs");
  capabilities = object_get(root, "capabilities");
  facts = object_get(root, "facts");
  if (status == ARCHBIRD_OK)
    status = validate_subject(engine, subject);
  if (status == ARCHBIRD_OK)
    status = validate_producer(engine, object_get(root, "producer"),
                               "provider-facts.producer", 1);
  if (status == ARCHBIRD_OK)
    status = validate_inputs(engine, inputs);
  if (status == ARCHBIRD_OK && object_get(subject, "project")) {
    StringView project = {yyjson_get_str(object_get(subject, "project")),
                          yyjson_get_len(object_get(subject, "project"))};
    if (!project_is_bound(inputs, project)) {
      status =
          schema_error(engine, "provider subject project has no bound input");
    }
  }
  if (status == ARCHBIRD_OK)
    status = validate_capabilities(engine, capabilities);
  if (status == ARCHBIRD_OK)
    status = validate_facts(engine, facts, subject, inputs, capabilities);
  if (status == ARCHBIRD_OK)
    status =
        validate_resolutions(engine, object_get(root, "resolutions"), facts);
  if (status == ARCHBIRD_OK)
    status = validate_diagnostics(engine, object_get(root, "diagnostics"));
  return status;
}

ArchbirdStatus archbird_provider_facts_validate(ArchbirdEngine *engine,
                                                const uint8_t *input,
                                                size_t input_length) {
  yyjson_doc *document = NULL;
  ArchbirdStatus status;
  if (!engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  status =
      archbird_json_parse_document(engine, input, input_length, &document, 1);
  if (status == ARCHBIRD_OK) {
    status = archbird_validate_provider_facts_root(
        engine, yyjson_doc_get_root(document));
  }
  yyjson_doc_free(document);
  return status;
}

static int decode_hex_digit(char byte) {
  if (byte >= '0' && byte <= '9')
    return byte - '0';
  return byte - 'a' + 10;
}

static void decode_sha256(yyjson_val *value, uint8_t digest[32]) {
  const char *hex = yyjson_get_str(value);
  size_t index;
  for (index = 0; index < 32; index++) {
    digest[index] = (uint8_t)((decode_hex_digit(hex[index * 2]) << 4) |
                              decode_hex_digit(hex[index * 2 + 1]));
  }
}

static ArchbirdStatus decode_string(ArchbirdEngine *engine, yyjson_val *value,
                                    AbString *out) {
  return ab_string_copy(engine, out, yyjson_get_str(value),
                        yyjson_get_len(value));
}

static ArchbirdStatus decode_string_array(ArchbirdEngine *engine,
                                          yyjson_val *value,
                                          AbStringArray *out) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index = 0;
  out->count = yyjson_arr_size(value);
  if (out->count > SIZE_MAX / sizeof(*out->items))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "evidence string array is too large");
  if (out->count) {
    out->items = (AbString *)ab_calloc(engine, out->count, sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding string array");
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    ArchbirdStatus status = decode_string(engine, item, &out->items[index++]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus decode_producer(ArchbirdEngine *engine, yyjson_val *value,
                                      AbProducer *out) {
  yyjson_val *configuration = object_get(value, "configuration_sha256");
  yyjson_val *runtime = object_get(value, "runtime");
  ArchbirdStatus status =
      decode_string(engine, object_get(value, "name"), &out->name);
  if (status == ARCHBIRD_OK)
    status = decode_string(engine, object_get(value, "version"), &out->version);
  if (status != ARCHBIRD_OK)
    return status;
  decode_sha256(object_get(value, "implementation_sha256"),
                out->implementation_sha256);
  if (configuration) {
    decode_sha256(configuration, out->configuration_sha256);
    out->has_configuration_sha256 = 1;
  }
  if (runtime) {
    status = decode_string(engine, runtime, &out->runtime);
    if (status != ARCHBIRD_OK)
      return status;
    out->has_runtime = 1;
  }
  return ARCHBIRD_OK;
}

static int object_field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus decode_value(ArchbirdEngine *engine, yyjson_val *value,
                                   AbValue *out) {
  if (yyjson_is_null(value)) {
    out->kind = AB_VALUE_NULL;
    return ARCHBIRD_OK;
  }
  if (yyjson_is_bool(value)) {
    out->kind = AB_VALUE_BOOL;
    out->as.boolean = yyjson_is_true(value);
    return ARCHBIRD_OK;
  }
  if (yyjson_is_raw(value)) {
    const char *number = yyjson_get_raw(value);
    size_t length = yyjson_get_len(value);
    size_t index;
    for (index = 0; index < length; index++) {
      if (number[index] == '.' || number[index] == 'e' ||
          number[index] == 'E') {
        out->kind = AB_VALUE_REAL;
        return ab_json_real_parse(engine, number, length, &out->as.real);
      }
    }
    out->kind = AB_VALUE_INTEGER;
    return ab_string_copy(engine, &out->as.text, number, length);
  }
  if (yyjson_is_str(value)) {
    out->kind = AB_VALUE_STRING;
    return decode_string(engine, value, &out->as.text);
  }
  if (yyjson_is_arr(value)) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    size_t index = 0;
    out->kind = AB_VALUE_ARRAY;
    out->as.array.count = yyjson_arr_size(value);
    if (out->as.array.count > SIZE_MAX / sizeof(*out->as.array.items))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "evidence value array is too large");
    if (out->as.array.count) {
      out->as.array.items = (AbValue *)ab_calloc(engine, out->as.array.count,
                                                 sizeof(*out->as.array.items));
      if (!out->as.array.items)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory decoding evidence array");
    }
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      ArchbirdStatus status =
          decode_value(engine, item, &out->as.array.items[index++]);
      if (status != ARCHBIRD_OK)
        return status;
    }
    return ARCHBIRD_OK;
  }
  if (yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    size_t index = 0;
    out->kind = AB_VALUE_OBJECT;
    out->as.object.count = yyjson_obj_size(value);
    if (out->as.object.count > SIZE_MAX / sizeof(*out->as.object.fields))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "evidence value object is too large");
    if (out->as.object.count) {
      out->as.object.fields = (AbObjectField *)ab_calloc(
          engine, out->as.object.count, sizeof(*out->as.object.fields));
      if (!out->as.object.fields)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory decoding evidence object");
    }
    yyjson_obj_iter_init(value, &iterator);
    while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
      AbObjectField *field = &out->as.object.fields[index++];
      ArchbirdStatus status = ab_string_copy(
          engine, &field->name, yyjson_get_str(key), yyjson_get_len(key));
      if (status == ARCHBIRD_OK)
        status =
            decode_value(engine, yyjson_obj_iter_get_val(key), &field->value);
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (out->as.object.count > 1) {
      qsort(out->as.object.fields, out->as.object.count,
            sizeof(*out->as.object.fields), object_field_compare);
    }
    return ARCHBIRD_OK;
  }
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "cannot decode unsupported evidence value");
}

static ArchbirdStatus decode_attributes(ArchbirdEngine *engine,
                                        yyjson_val *value, AbFact *out) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t index = 0;
  if (!value)
    return ARCHBIRD_OK;
  out->attribute_count = yyjson_obj_size(value);
  if (out->attribute_count > SIZE_MAX / sizeof(*out->attributes))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "fact attribute object is too large");
  if (out->attribute_count) {
    out->attributes = (AbObjectField *)ab_calloc(engine, out->attribute_count,
                                                 sizeof(*out->attributes));
    if (!out->attributes)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding fact attributes");
  }
  yyjson_obj_iter_init(value, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    AbObjectField *field = &out->attributes[index++];
    ArchbirdStatus status = ab_string_copy(
        engine, &field->name, yyjson_get_str(key), yyjson_get_len(key));
    if (status == ARCHBIRD_OK)
      status =
          decode_value(engine, yyjson_obj_iter_get_val(key), &field->value);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (out->attribute_count > 1) {
    qsort(out->attributes, out->attribute_count, sizeof(*out->attributes),
          object_field_compare);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus decode_span(ArchbirdEngine *engine, yyjson_val *value,
                                  size_t *out_start, size_t *out_end) {
  uint64_t start;
  uint64_t end;
  if (!parse_u64(object_get(value, "start"), &start) ||
      !parse_u64(object_get(value, "end"), &end) || start > SIZE_MAX ||
      end > SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "evidence span exceeds this platform");
  *out_start = (size_t)start;
  *out_end = (size_t)end;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_decode_source_manifest(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbSourceManifest *out) {
  yyjson_doc *document = NULL;
  yyjson_val *root;
  yyjson_val *files;
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index = 0;
  size_t total_bytes = 0;
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  status =
      archbird_json_parse_document(engine, json, json_length, &document, 1);
  if (status != ARCHBIRD_OK)
    return status;
  root = yyjson_doc_get_root(document);
  status = archbird_validate_source_manifest_root(engine, root);
  if (status != ARCHBIRD_OK)
    goto done;
  status = decode_string(engine, object_get(root, "project"), &out->project);
  if (status == ARCHBIRD_OK)
    status =
        decode_producer(engine, object_get(root, "producer"), &out->producer);
  if (status != ARCHBIRD_OK)
    goto done;
  if (object_get(root, "configuration_sha256")) {
    decode_sha256(object_get(root, "configuration_sha256"),
                  out->configuration_sha256);
    out->has_configuration_sha256 = 1;
  }
  if (object_get(root, "resolution")) {
    yyjson_val *resolution = object_get(root, "resolution");
    yyjson_val *profile = object_get(resolution, "profile");
    yyjson_val *coverage = object_get(resolution, "coverage");
    status = decode_string(engine, object_get(profile, "name"),
                           &out->resolution.profile_name);
    if (status != ARCHBIRD_OK)
      goto done;
    decode_sha256(object_get(profile, "implementation_sha256"),
                  out->resolution.profile_implementation_sha256);
    decode_sha256(object_get(resolution, "sha256"), out->resolution.sha256);
#define DECODE_COVERAGE(field)                                                 \
  (void)parse_u64(object_get(coverage, #field), &out->resolution.coverage.field)
    DECODE_COVERAGE(assets);
    DECODE_COVERAGE(ignored);
    DECODE_COVERAGE(inventory_files);
    DECODE_COVERAGE(oversized);
    DECODE_COVERAGE(pruned_directories);
    DECODE_COVERAGE(selected);
    DECODE_COVERAGE(unsupported_known);
#undef DECODE_COVERAGE
    out->has_resolution = 1;
  }
  files = object_get(root, "files");
  out->file_count = yyjson_arr_size(files);
  if (out->has_resolution &&
      out->resolution.coverage.selected != out->file_count) {
    status = schema_error_field(
        engine, "source-manifest.resolution.coverage", "selected",
        "must equal the number of source-manifest files");
    goto done;
  }
  if (out->file_count > engine->options.max_files ||
      out->file_count > SIZE_MAX / sizeof(*out->files)) {
    status =
        archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
                           "source manifest file limit exceeded");
    goto done;
  }
  if (out->file_count) {
    out->files = (AbManifestFile *)ab_calloc(engine, out->file_count,
                                             sizeof(*out->files));
    if (!out->files) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory decoding source manifest");
      goto done;
    }
  }
  yyjson_arr_iter_init(files, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    AbManifestFile *file = &out->files[index++];
    uint64_t byte_length;
    size_t file_limit;
    status = decode_string(engine, object_get(item, "path"), &file->path);
    if (status != ARCHBIRD_OK ||
        !parse_u64(object_get(item, "bytes"), &byte_length) ||
        byte_length > SIZE_MAX) {
      if (status == ARCHBIRD_OK)
        status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "source length exceeds this platform");
      goto done;
    }
    file->byte_length = (size_t)byte_length;
    status =
        decode_string_array(engine, object_get(item, "roles"), &file->roles);
    if (status != ARCHBIRD_OK)
      goto done;
    file_limit = ab_manifest_file_has_role(file, "index")
                     ? engine->options.max_index_bytes
                     : engine->options.max_file_bytes;
    if (file->byte_length > file_limit ||
        file->byte_length > engine->options.max_source_bytes - total_bytes) {
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "source manifest byte limits exceeded");
      goto done;
    }
    total_bytes += file->byte_length;
    decode_sha256(object_get(item, "sha256"), file->sha256);
    if (object_get(item, "language")) {
      status =
          decode_string(engine, object_get(item, "language"), &file->language);
      if (status != ARCHBIRD_OK)
        goto done;
      file->has_language = 1;
    }
    if (object_get(item, "layer")) {
      status = decode_string(engine, object_get(item, "layer"), &file->layer);
      if (status != ARCHBIRD_OK)
        goto done;
      file->has_layer = 1;
    }
  }
done:
  yyjson_doc_free(document);
  if (status != ARCHBIRD_OK)
    ab_source_manifest_free(engine, out);
  return status;
}

static ArchbirdStatus decode_subject(ArchbirdEngine *engine, yyjson_val *value,
                                     AbSubject *out) {
  ArchbirdStatus status =
      decode_string(engine, object_get(value, "scope"), &out->scope);
  if (status == ARCHBIRD_OK && object_get(value, "project")) {
    status = decode_string(engine, object_get(value, "project"), &out->project);
    if (status == ARCHBIRD_OK)
      out->has_project = 1;
  }
  if (status == ARCHBIRD_OK && object_get(value, "path")) {
    status = decode_string(engine, object_get(value, "path"), &out->path);
    if (status == ARCHBIRD_OK)
      out->has_path = 1;
  }
  if (status == ARCHBIRD_OK && object_get(value, "name")) {
    status = decode_string(engine, object_get(value, "name"), &out->name);
    if (status == ARCHBIRD_OK)
      out->has_name = 1;
  }
  return status;
}

static ArchbirdStatus decode_fact(ArchbirdEngine *engine, yyjson_val *value,
                                  AbFact *out) {
  ArchbirdStatus status =
      decode_string(engine, object_get(value, "id"), &out->id);
#define DECODE_FACT_STRING(field)                                              \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = decode_string(engine, object_get(value, #field), &out->field);  \
  } while (0)
  DECODE_FACT_STRING(domain);
  DECODE_FACT_STRING(kind);
  DECODE_FACT_STRING(claim);
  DECODE_FACT_STRING(project);
  DECODE_FACT_STRING(path);
  DECODE_FACT_STRING(key);
#undef DECODE_FACT_STRING
  if (status == ARCHBIRD_OK && object_get(value, "correlation"))
    out->correlate_by_span =
        value_equals(object_get(value, "correlation"), "span");
  if (status == ARCHBIRD_OK) {
    status = decode_span(engine, object_get(value, "span"), &out->span_start,
                         &out->span_end);
  }
  if (status == ARCHBIRD_OK && object_get(value, "name")) {
    status = decode_string(engine, object_get(value, "name"), &out->name);
    if (status == ARCHBIRD_OK)
      out->has_name = 1;
  }
  if (status == ARCHBIRD_OK)
    status = decode_attributes(engine, object_get(value, "attributes"), out);
  return status;
}

static AbFact *decoded_fact_find(AbProviderBundle *bundle, yyjson_val *id) {
  AbString wanted = {(char *)yyjson_get_str(id), yyjson_get_len(id)};
  size_t low = 0;
  size_t high = bundle->fact_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&bundle->facts[middle].id, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &bundle->facts[middle];
  }
  return NULL;
}

static ArchbirdStatus decode_resolution(ArchbirdEngine *engine,
                                        yyjson_val *value,
                                        AbProviderBundle *bundle) {
  AbFact *fact = decoded_fact_find(bundle, object_get(value, "fact_id"));
  ArchbirdStatus status = decode_string(engine, object_get(value, "state"),
                                        &fact->resolution.state);
  if (status == ARCHBIRD_OK) {
    status = decode_string_array(engine, object_get(value, "targets"),
                                 &fact->resolution.targets);
  }
  if (status == ARCHBIRD_OK && object_get(value, "reason")) {
    status = decode_string(engine, object_get(value, "reason"),
                           &fact->resolution.reason);
    if (status == ARCHBIRD_OK)
      fact->resolution.has_reason = 1;
  }
  if (status == ARCHBIRD_OK)
    fact->has_resolution = 1;
  return status;
}

ArchbirdStatus ab_decode_provider_bundle(ArchbirdEngine *engine,
                                         const uint8_t *json,
                                         size_t json_length,
                                         AbProviderBundle *out) {
  yyjson_doc *document = NULL;
  yyjson_val *root;
  yyjson_val *array;
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index;
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  status =
      archbird_json_parse_document(engine, json, json_length, &document, 1);
  if (status != ARCHBIRD_OK)
    return status;
  root = yyjson_doc_get_root(document);
  status = archbird_validate_provider_facts_root(engine, root);
  if (status != ARCHBIRD_OK)
    goto done;
  status = decode_subject(engine, object_get(root, "subject"), &out->subject);
  if (status == ARCHBIRD_OK)
    status =
        decode_producer(engine, object_get(root, "producer"), &out->producer);
  if (status != ARCHBIRD_OK)
    goto done;

  array = object_get(root, "inputs");
  out->input_count = yyjson_arr_size(array);
  if (out->input_count) {
    out->inputs = (AbProviderInput *)ab_calloc(engine, out->input_count,
                                               sizeof(*out->inputs));
    if (!out->inputs) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory decoding provider inputs");
      goto done;
    }
  }
  index = 0;
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    status = decode_string(engine, object_get(item, "project"),
                           &out->inputs[index].project);
    if (status != ARCHBIRD_OK)
      goto done;
    if (object_get(item, "source_manifest_sha256")) {
      decode_sha256(object_get(item, "source_manifest_sha256"),
                    out->inputs[index].source_manifest_sha256);
      out->inputs[index].has_source_manifest_sha256 = 1;
    } else {
      status = decode_string(engine, object_get(item, "path"),
                             &out->inputs[index].path);
      if (status != ARCHBIRD_OK)
        goto done;
      out->inputs[index].has_path = 1;
      decode_sha256(object_get(item, "source_sha256"),
                    out->inputs[index].source_sha256);
      out->inputs[index].has_source_sha256 = 1;
    }
    index++;
  }

  array = object_get(root, "capabilities");
  out->capability_count = yyjson_arr_size(array);
  if (out->capability_count) {
    out->capabilities = (AbCapability *)ab_calloc(engine, out->capability_count,
                                                  sizeof(*out->capabilities));
    if (!out->capabilities) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory decoding provider capabilities");
      goto done;
    }
  }
  index = 0;
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    AbCapability *capability = &out->capabilities[index++];
    status =
        decode_string(engine, object_get(item, "domain"), &capability->domain);
    if (status == ARCHBIRD_OK) {
      status = decode_string(engine, object_get(item, "coverage"),
                             &capability->coverage);
    }
    if (status == ARCHBIRD_OK) {
      status = decode_string_array(engine, object_get(item, "claims"),
                                   &capability->claims);
    }
    if (status == ARCHBIRD_OK && object_get(item, "boundary")) {
      status = decode_string(engine, object_get(item, "boundary"),
                             &capability->boundary);
      if (status == ARCHBIRD_OK)
        capability->has_boundary = 1;
    }
    if (status != ARCHBIRD_OK)
      goto done;
  }

  array = object_get(root, "facts");
  out->fact_count = yyjson_arr_size(array);
  if (out->fact_count > engine->options.max_facts ||
      out->fact_count > SIZE_MAX / sizeof(*out->facts)) {
    status =
        archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
                           "provider fact limit exceeded");
    goto done;
  }
  if (out->fact_count) {
    out->facts =
        (AbFact *)ab_calloc(engine, out->fact_count, sizeof(*out->facts));
    if (!out->facts) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory decoding provider facts");
      goto done;
    }
  }
  index = 0;
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    status = decode_fact(engine, item, &out->facts[index++]);
    if (status != ARCHBIRD_OK)
      goto done;
  }
  array = object_get(root, "resolutions");
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    status = decode_resolution(engine, item, out);
    if (status != ARCHBIRD_OK)
      goto done;
  }

  array = object_get(root, "diagnostics");
  out->diagnostic_count = yyjson_arr_size(array);
  if (out->diagnostic_count) {
    out->diagnostics = (AbDiagnostic *)ab_calloc(engine, out->diagnostic_count,
                                                 sizeof(*out->diagnostics));
    if (!out->diagnostics) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory decoding diagnostics");
      goto done;
    }
  }
  index = 0;
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    AbDiagnostic *diagnostic = &out->diagnostics[index++];
    status = decode_string(engine, object_get(item, "severity"),
                           &diagnostic->severity);
    if (status == ARCHBIRD_OK)
      status =
          decode_string(engine, object_get(item, "code"), &diagnostic->code);
    if (status == ARCHBIRD_OK) {
      status = decode_string(engine, object_get(item, "message"),
                             &diagnostic->message);
    }
    if (status == ARCHBIRD_OK && object_get(item, "project")) {
      status = decode_string(engine, object_get(item, "project"),
                             &diagnostic->project);
      if (status == ARCHBIRD_OK)
        diagnostic->has_project = 1;
    }
    if (status == ARCHBIRD_OK && object_get(item, "path")) {
      status =
          decode_string(engine, object_get(item, "path"), &diagnostic->path);
      if (status == ARCHBIRD_OK)
        diagnostic->has_path = 1;
    }
    if (status == ARCHBIRD_OK && object_get(item, "span")) {
      status = decode_span(engine, object_get(item, "span"),
                           &diagnostic->span_start, &diagnostic->span_end);
      if (status == ARCHBIRD_OK)
        diagnostic->has_span = 1;
    }
    if (status != ARCHBIRD_OK)
      goto done;
  }
done:
  yyjson_doc_free(document);
  if (status != ARCHBIRD_OK)
    ab_provider_bundle_free(engine, out);
  return status;
}
