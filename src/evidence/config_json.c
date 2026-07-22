#include "config.h"

#include "project_configuration.h"

#include "json_internal.h"
#include "pattern.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus config_error(ArchbirdEngine *engine,
                                   const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static yyjson_val *member(yyjson_val *object, const char *name) {
  return yyjson_obj_get(object, name);
}

static ArchbirdStatus boolean_value(ArchbirdEngine *engine, yyjson_val *value,
                                    const char *where, int default_value,
                                    int *out);
static int unsigned_value(yyjson_val *value, uint64_t *out);

static int allowed_name(const char *name, size_t length,
                        const char *const *allowed, size_t allowed_count) {
  size_t index;
  for (index = 0; index < allowed_count; index++) {
    size_t wanted = strlen(allowed[index]);
    if (wanted == length && memcmp(name, allowed[index], length) == 0)
      return 1;
  }
  return 0;
}

static ArchbirdStatus
object_shape(ArchbirdEngine *engine, yyjson_val *value, const char *where,
             const char *const *allowed, size_t allowed_count,
             const char *const *required, size_t required_count) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t index;
  if (!value || !yyjson_is_obj(value))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must be an object",
                              where);
  yyjson_obj_iter_init(value, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    if (!allowed_name(yyjson_get_str(key), yyjson_get_len(key), allowed,
                      allowed_count))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "%s contains an unknown field", where);
  }
  for (index = 0; index < required_count; index++) {
    if (!member(value, required[index]))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "%s is missing a required field", where);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus copy_string(ArchbirdEngine *engine, yyjson_val *value,
                                  const char *where, int allow_empty,
                                  AbString *out) {
  const uint8_t *bytes;
  size_t length;
  size_t index = 0;
  int non_whitespace = 0;
  if (!value || !yyjson_is_str(value) ||
      memchr(yyjson_get_str(value), '\0', yyjson_get_len(value)))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must be a non-NUL string",
                              where);
  bytes = (const uint8_t *)yyjson_get_str(value);
  length = yyjson_get_len(value);
  while (!non_whitespace && index < length) {
    uint32_t codepoint;
    uint8_t first = bytes[index++];
    if (first < 0x80) {
      codepoint = first;
    } else if (first < 0xe0) {
      codepoint =
          ((uint32_t)(first & 0x1f) << 6) | (uint32_t)(bytes[index++] & 0x3f);
    } else if (first < 0xf0) {
      codepoint = ((uint32_t)(first & 0x0f) << 12) |
                  ((uint32_t)(bytes[index] & 0x3f) << 6) |
                  (uint32_t)(bytes[index + 1] & 0x3f);
      index += 2;
    } else {
      codepoint = ((uint32_t)(first & 0x07) << 18) |
                  ((uint32_t)(bytes[index] & 0x3f) << 12) |
                  ((uint32_t)(bytes[index + 1] & 0x3f) << 6) |
                  (uint32_t)(bytes[index + 2] & 0x3f);
      index += 3;
    }
    if (!((codepoint >= 0x09 && codepoint <= 0x0d) || codepoint == 0x20 ||
          codepoint == 0x85 || codepoint == 0xa0 || codepoint == 0x1680 ||
          (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
          codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
          codepoint == 0x3000))
      non_whitespace = 1;
  }
  if (!allow_empty && !non_whitespace)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be a nonempty string", where);
  return ab_string_copy(engine, out, yyjson_get_str(value),
                        yyjson_get_len(value));
}

static ArchbirdStatus copy_default_string(ArchbirdEngine *engine,
                                          yyjson_val *value,
                                          const char *default_value,
                                          const char *where, int allow_empty,
                                          AbString *out) {
  if (!value)
    return ab_string_copy(engine, out, default_value, strlen(default_value));
  return copy_string(engine, value, where, allow_empty, out);
}

static ArchbirdStatus copy_strings(ArchbirdEngine *engine, yyjson_val *value,
                                   const char *where, int required,
                                   AbStringArray *out) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index = 0;
  if (!value) {
    if (required)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s is required", where);
    return ARCHBIRD_OK;
  }
  if (!yyjson_is_arr(value) || (required && yyjson_arr_size(value) == 0))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be a nonempty string array", where);
  out->count = yyjson_arr_size(value);
  if (out->count) {
    out->items = (AbString *)ab_calloc(engine, out->count, sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding configuration");
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    ArchbirdStatus status =
        copy_string(engine, item, where, 0, &out->items[index++]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus copy_sizes(ArchbirdEngine *engine, yyjson_val *value,
                                 const char *where, int required,
                                 size_t **out_items, size_t *out_count) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index = 0;
  *out_items = NULL;
  *out_count = 0;
  if (!value) {
    if (required)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s is required", where);
    return ARCHBIRD_OK;
  }
  if (!yyjson_is_arr(value) || (required && yyjson_arr_size(value) == 0))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "%s must be a nonempty unsigned integer array", where);
  *out_count = yyjson_arr_size(value);
  if (*out_count) {
    *out_items = (size_t *)ab_calloc(engine, *out_count, sizeof(**out_items));
    if (!*out_items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding configuration");
  }
  yyjson_arr_iter_init(value, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    uint64_t number;
    size_t prior;
    if (!unsigned_value(item, &number) || number > 31)
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "%s values must be integers from 0 through 31", where);
    for (prior = 0; prior < index; prior++) {
      if ((*out_items)[prior] == (size_t)number)
        return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "%s values must be unique", where);
    }
    (*out_items)[index++] = (size_t)number;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus copy_excludes(ArchbirdEngine *engine, yyjson_val *value,
                                    int include_defaults, AbStringArray *out) {
  static const char *const defaults[] = {".git/**",
                                         "**/.git/**",
                                         ".hg/**",
                                         "**/.hg/**",
                                         ".svn/**",
                                         "**/.svn/**",
                                         "build/**",
                                         "**/build/**",
                                         "build-*/**",
                                         "**/build-*/**",
                                         "dist/**",
                                         "**/dist/**",
                                         "out/**",
                                         "**/out/**",
                                         "target/**",
                                         "**/target/**",
                                         "node_modules/**",
                                         "**/node_modules/**",
                                         ".venv/**",
                                         "**/.venv/**",
                                         "venv/**",
                                         "__pycache__/**",
                                         "**/__pycache__/**",
                                         ".pytest_cache/**",
                                         "**/.pytest_cache/**",
                                         ".mypy_cache/**",
                                         "**/.mypy_cache/**",
                                         ".ruff_cache/**",
                                         "**/.ruff_cache/**",
                                         ".tox/**",
                                         "**/.tox/**",
                                         ".nox/**",
                                         "**/.nox/**",
                                         ".cache/**",
                                         "**/.cache/**",
                                         ".gradle/**",
                                         "**/.gradle/**",
                                         ".next/**",
                                         "**/.next/**",
                                         ".nuxt/**",
                                         "**/.nuxt/**",
                                         ".svelte-kit/**",
                                         "**/.svelte-kit/**",
                                         ".parcel-cache/**",
                                         "**/.parcel-cache/**",
                                         ".turbo/**",
                                         "**/.turbo/**",
                                         "coverage/**",
                                         "**/coverage/**",
                                         "htmlcov/**",
                                         "**/htmlcov/**"};
  const size_t available_defaults = sizeof(defaults) / sizeof(defaults[0]);
  const size_t default_count = include_defaults ? available_defaults : 0;
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t user_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (value && !yyjson_is_arr(value))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "exclude must be a string array");
  if (value)
    user_count = yyjson_arr_size(value);
  if (user_count > SIZE_MAX - default_count ||
      default_count + user_count > SIZE_MAX / sizeof(*out->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "exclude contains too many patterns");
  out->count = default_count + user_count;
  out->items = (AbString *)ab_calloc(engine, out->count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding configuration");
  for (index = 0; status == ARCHBIRD_OK && index < default_count; index++)
    status = ab_string_copy(engine, &out->items[index], defaults[index],
                            strlen(defaults[index]));
  if (status == ARCHBIRD_OK && value) {
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      status = copy_string(engine, item, "exclude", 0, &out->items[index++]);
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  return status;
}

static ArchbirdStatus parse_discovery_options(ArchbirdEngine *engine,
                                              yyjson_val *value,
                                              AbMapConfig *out) {
  static const char *const fields[] = {"default_excludes"};
  ArchbirdStatus status = ARCHBIRD_OK;
  out->default_excludes = 1;
  if (!value)
    return ARCHBIRD_OK;
  status = object_shape(engine, value, "discovery", fields, 1, NULL, 0);
  if (status == ARCHBIRD_OK)
    status =
        boolean_value(engine, member(value, "default_excludes"),
                      "discovery.default_excludes", 1, &out->default_excludes);
  return status;
}

static ArchbirdStatus copy_string_or_strings(ArchbirdEngine *engine,
                                             yyjson_val *value,
                                             const char *where,
                                             AbStringArray *out) {
  if (value && yyjson_is_str(value)) {
    out->items = (AbString *)ab_calloc(engine, 1, sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding configuration");
    out->count = 1;
    return copy_string(engine, value, where, 0, out->items);
  }
  return copy_strings(engine, value, where, 1, out);
}

static int is_ascii_alpha(uint8_t value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

static ArchbirdStatus normalize_repository_path(ArchbirdEngine *engine,
                                                AbString *path,
                                                const char *where,
                                                int pattern) {
  size_t read = 0;
  size_t write = 0;
  size_t segment_start = 0;
  if (path->length >= 2 && path->data[0] == '.' && path->data[1] == '/')
    read = 2;
  while (read < path->length) {
    char value = path->data[read++];
    path->data[write++] = value == '\\' ? '/' : value;
  }
  path->length = write;
  path->data[write] = '\0';
  if (!write || path->data[0] == '/' ||
      (write >= 3 && is_ascii_alpha((uint8_t)path->data[0]) &&
       path->data[1] == ':' && path->data[2] == '/'))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be repository-relative", where);
  for (read = 0; read <= write; read++) {
    if (read == write || path->data[read] == '/') {
      size_t segment_length = read - segment_start;
      if (segment_length == 2 && path->data[segment_start] == '.' &&
          path->data[segment_start + 1] == '.')
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "%s must not contain a parent segment", where);
      segment_start = read + 1;
    } else if (!pattern &&
               (path->data[read] == '*' || path->data[read] == '?' ||
                path->data[read] == '[')) {
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "%s must be an exact repository path", where);
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus require_canonical_directory(ArchbirdEngine *engine,
                                                  const AbString *path,
                                                  const char *where) {
  size_t index;
  size_t start = 0;
  if (!path->length || path->data[path->length - 1] == '/')
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be a canonical directory", where);
  for (index = 0; index <= path->length; index++) {
    if (index == path->length || path->data[index] == '/') {
      size_t length = index - start;
      if (!length || (length == 1 && path->data[start] == '.'))
        return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "%s must be a canonical directory", where);
      start = index + 1;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus copy_repository_path(ArchbirdEngine *engine,
                                           yyjson_val *value, const char *where,
                                           int pattern, AbString *out) {
  ArchbirdStatus status = copy_string(engine, value, where, 0, out);
  if (status == ARCHBIRD_OK)
    status = normalize_repository_path(engine, out, where, pattern);
  return status;
}

static ArchbirdStatus copy_repository_paths(ArchbirdEngine *engine,
                                            yyjson_val *value,
                                            const char *where, int required,
                                            int pattern, AbStringArray *out) {
  size_t index;
  ArchbirdStatus status = copy_strings(engine, value, where, required, out);
  for (index = 0; status == ARCHBIRD_OK && index < out->count; index++)
    status =
        normalize_repository_path(engine, &out->items[index], where, pattern);
  return status;
}

static int string_one_of(const AbString *value, const char *const *allowed,
                         size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    AbString wanted = {(char *)allowed[index], strlen(allowed[index])};
    if (ab_string_equal(value, &wanted))
      return 1;
  }
  return 0;
}

static ArchbirdStatus boolean_value(ArchbirdEngine *engine, yyjson_val *value,
                                    const char *where, int default_value,
                                    int *out) {
  if (!value) {
    *out = default_value;
    return ARCHBIRD_OK;
  }
  if (!yyjson_is_bool(value))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must be boolean", where);
  *out = yyjson_is_true(value) ? 1 : 0;
  return ARCHBIRD_OK;
}

static int unsigned_value(yyjson_val *value, uint64_t *out) {
  const char *text;
  size_t length;
  size_t index;
  uint64_t number = 0;
  if (!value || !yyjson_is_raw(value))
    return 0;
  text = yyjson_get_raw(value);
  length = yyjson_get_len(value);
  if (!length)
    return 0;
  for (index = 0; index < length; index++) {
    uint8_t digit = (uint8_t)text[index];
    if (digit < '0' || digit > '9' ||
        number > (UINT64_MAX - (uint64_t)(digit - '0')) / 10)
      return 0;
    number = number * 10 + (uint64_t)(digit - '0');
  }
  *out = number;
  return 1;
}

static ArchbirdStatus size_value(ArchbirdEngine *engine, yyjson_val *value,
                                 const char *where, size_t default_value,
                                 size_t minimum, size_t *out) {
  uint64_t number;
  if (!value) {
    *out = default_value;
    return ARCHBIRD_OK;
  }
  if (!unsigned_value(value, &number))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "%s must be a nonnegative integer", where);
  if (number < minimum || number > SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s is outside limits",
                              where);
  *out = (size_t)number;
  return ARCHBIRD_OK;
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; array->items && index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static void layer_free(ArchbirdEngine *engine, AbConfigLayer *layer) {
  size_t index;
  ab_string_free(engine, &layer->name);
  ab_string_free(engine, &layer->role);
  ab_string_free(engine, &layer->language);
  string_array_free(engine, &layer->globs);
  string_array_free(engine, &layer->public_headers);
  string_array_free(engine, &layer->import_roots);
  for (index = 0;
       layer->external_namespaces && index < layer->external_namespace_count;
       index++) {
    ab_string_free(engine, &layer->external_namespaces[index].prefix);
    ab_string_free(engine, &layer->external_namespaces[index].package);
  }
  ab_free(engine, layer->external_namespaces);
  memset(layer, 0, sizeof(*layer));
}

static void component_free(ArchbirdEngine *engine,
                           AbConfigComponent *component) {
  ab_string_free(engine, &component->name);
  ab_string_free(engine, &component->description);
  string_array_free(engine, &component->paths);
  memset(component, 0, sizeof(*component));
}

static void package_free(ArchbirdEngine *engine, AbConfigPackage *package) {
  ab_string_free(engine, &package->name);
  ab_string_free(engine, &package->kind);
  ab_string_free(engine, &package->path);
  ab_string_free(engine, &package->layer);
  string_array_free(engine, &package->entries);
  ab_string_free(engine, &package->identity);
  ab_string_free(engine, &package->version);
  string_array_free(engine, &package->aliases);
  memset(package, 0, sizeof(*package));
}

static void build_free(ArchbirdEngine *engine, AbConfigBuild *build) {
  ab_string_free(engine, &build->name);
  ab_string_free(engine, &build->kind);
  ab_string_free(engine, &build->path);
  ab_string_free(engine, &build->variant);
  memset(build, 0, sizeof(*build));
}

static void index_free(ArchbirdEngine *engine, AbConfigIndex *index) {
  ab_string_free(engine, &index->name);
  ab_string_free(engine, &index->format);
  ab_string_free(engine, &index->path);
  ab_string_free(engine, &index->path_prefix);
  ab_string_free(engine, &index->variant);
  memset(index, 0, sizeof(*index));
}

static void artifact_free(ArchbirdEngine *engine, AbConfigArtifact *artifact) {
  size_t index;
  ab_string_free(engine, &artifact->name);
  ab_string_free(engine, &artifact->output);
  string_array_free(engine, &artifact->inputs);
  for (index = 0; artifact->loaders && index < artifact->loader_count;
       index++) {
    string_array_free(engine, &artifact->loaders[index].paths);
    ab_string_free(engine, &artifact->loaders[index].pattern);
  }
  ab_free(engine, artifact->loaders);
  for (index = 0; artifact->builds && index < artifact->build_count; index++) {
    ab_string_free(engine, &artifact->builds[index].source);
    ab_string_free(engine, &artifact->builds[index].target);
  }
  ab_free(engine, artifact->builds);
  memset(artifact, 0, sizeof(*artifact));
}

static void bridge_free(ArchbirdEngine *engine, AbConfigBridge *bridge) {
  size_t index;
  ab_string_free(engine, &bridge->name);
  ab_string_free(engine, &bridge->kind);
  string_array_free(engine, &bridge->from_layers);
  string_array_free(engine, &bridge->from_paths);
  string_array_free(engine, &bridge->exclude_from_paths);
  string_array_free(engine, &bridge->to_layers);
  string_array_free(engine, &bridge->prefixes);
  string_array_free(engine, &bridge->message_keys);
  string_array_free(engine, &bridge->ignore);
  for (index = 0; bridge->providers && index < bridge->provider_count;
       index++) {
    ab_string_free(engine, &bridge->providers[index].kind);
    ab_string_free(engine, &bridge->providers[index].path);
    ab_string_free(engine, &bridge->providers[index].variable);
    ab_string_free(engine, &bridge->providers[index].pattern);
  }
  ab_free(engine, bridge->providers);
  memset(bridge, 0, sizeof(*bridge));
}

static void test_free(ArchbirdEngine *engine, AbConfigTest *test) {
  size_t index;
  ab_string_free(engine, &test->name);
  ab_string_free(engine, &test->language);
  string_array_free(engine, &test->globs);
  string_array_free(engine, &test->route_to);
  for (index = 0; test->case_routes && index < test->case_route_count;
       index++) {
    ab_string_free(engine, &test->case_routes[index].selector);
    string_array_free(engine, &test->case_routes[index].paths);
    string_array_free(engine, &test->case_routes[index].targets);
    string_array_free(engine, &test->case_routes[index].target_symbols);
  }
  ab_free(engine, test->case_routes);
  for (index = 0; test->case_extractors && index < test->case_extractor_count;
       index++) {
    AbConfigTestCaseExtractor *extractor = &test->case_extractors[index];
    ab_string_free(engine, &extractor->kind);
    ab_string_free(engine, &extractor->call);
    ab_string_free(engine, &extractor->name);
    ab_free(engine, extractor->selector_arguments);
    ab_string_free(engine, &extractor->separator);
  }
  ab_free(engine, test->case_extractors);
  for (index = 0; test->generated_files && index < test->generated_file_count;
       index++) {
    string_array_free(engine, &test->generated_files[index].globs);
    string_array_free(engine, &test->generated_files[index].sources);
  }
  ab_free(engine, test->generated_files);
  memset(test, 0, sizeof(*test));
}

static void named_entry_free(ArchbirdEngine *engine,
                             AbConfigNamedEntry *entry) {
  ab_string_free(engine, &entry->name);
  ab_string_free(engine, &entry->kind);
  string_array_free(engine, &entry->functions);
  string_array_free(engine, &entry->globs);
  memset(entry, 0, sizeof(*entry));
}

static void parity_free(ArchbirdEngine *engine, AbConfigParity *parity) {
  size_t index;
  ab_string_free(engine, &parity->name);
  for (index = 0; parity->members && index < parity->member_count; index++) {
    AbConfigParityMember *member = &parity->members[index];
    ab_string_free(engine, &member->label);
    ab_string_free(engine, &member->source);
    ab_string_free(engine, &member->layer);
    ab_string_free(engine, &member->package);
    ab_string_free(engine, &member->bridge);
    string_array_free(engine, &member->include);
    string_array_free(engine, &member->exclude);
    string_array_free(engine, &member->kinds);
  }
  ab_free(engine, parity->members);
  ab_string_free(engine, &parity->case_name);
  string_array_free(engine, &parity->strip_prefixes);
  string_array_free(engine, &parity->strip_suffixes);
  for (index = 0; parity->aliases && index < parity->alias_count; index++) {
    ab_string_free(engine, &parity->aliases[index].key);
    ab_string_free(engine, &parity->aliases[index].value);
  }
  ab_free(engine, parity->aliases);
  string_array_free(engine, &parity->ignore);
  memset(parity, 0, sizeof(*parity));
}

void ab_map_config_free(ArchbirdEngine *engine, AbMapConfig *config) {
  size_t index;
  if (!config)
    return;
  ab_string_free(engine, &config->project);
  ab_string_free(engine, &config->description);
  ab_string_free(engine, &config->root);
  string_array_free(engine, &config->exclude);
  for (index = 0; config->layers && index < config->layer_count; index++)
    layer_free(engine, &config->layers[index]);
  ab_free(engine, config->layers);
  for (index = 0; config->components && index < config->component_count;
       index++)
    component_free(engine, &config->components[index]);
  ab_free(engine, config->components);
  for (index = 0; config->packages && index < config->package_count; index++)
    package_free(engine, &config->packages[index]);
  ab_free(engine, config->packages);
  for (index = 0; config->builds && index < config->build_count; index++)
    build_free(engine, &config->builds[index]);
  ab_free(engine, config->builds);
  for (index = 0; config->indexes && index < config->index_count; index++)
    index_free(engine, &config->indexes[index]);
  ab_free(engine, config->indexes);
  for (index = 0; config->artifacts && index < config->artifact_count; index++)
    artifact_free(engine, &config->artifacts[index]);
  ab_free(engine, config->artifacts);
  for (index = 0; config->bridges && index < config->bridge_count; index++)
    bridge_free(engine, &config->bridges[index]);
  ab_free(engine, config->bridges);
  for (index = 0; config->tests && index < config->test_count; index++)
    test_free(engine, &config->tests[index]);
  ab_free(engine, config->tests);
  for (index = 0; config->named_entries && index < config->named_entry_count;
       index++)
    named_entry_free(engine, &config->named_entries[index]);
  ab_free(engine, config->named_entries);
  for (index = 0; config->parity && index < config->parity_count; index++)
    parity_free(engine, &config->parity[index]);
  ab_free(engine, config->parity);
  memset(config, 0, sizeof(*config));
}

static ArchbirdStatus parse_namespace(ArchbirdEngine *engine, yyjson_val *value,
                                      AbExternalNamespace *out) {
  static const char *const fields[] = {"package", "prefix"};
  ArchbirdStatus status = object_shape(
      engine, value, "external_call_namespaces[]", fields, 2, fields, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "prefix"),
                         "external_call_namespaces[].prefix", 0, &out->prefix);
  if (status == ARCHBIRD_OK)
    status =
        copy_string(engine, member(value, "package"),
                    "external_call_namespaces[].package", 0, &out->package);
  return status;
}

static ArchbirdStatus parse_layer(ArchbirdEngine *engine, yyjson_val *value,
                                  AbConfigLayer *out) {
  static const char *const fields[] = {"external_call_namespaces",
                                       "globs",
                                       "import_roots",
                                       "language",
                                       "name",
                                       "public_headers",
                                       "required",
                                       "role"};
  static const char *const required[] = {"globs", "language", "name"};
  static const char *const languages[] = {"c", "cpp",  "javascript", "python",
                                          "r", "text", "typescript", "vue"};
  yyjson_val *namespaces;
  size_t index;
  int language_valid = 0;
  ArchbirdStatus status =
      object_shape(engine, value, "layers[]", fields, 8, required, 3);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "layers[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "role"), "source",
                                 "layers[].role", 0, &out->role);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "language"), "layers[].language",
                         0, &out->language);
  for (index = 0; status == ARCHBIRD_OK && index < 8; index++) {
    AbString wanted = {(char *)languages[index], strlen(languages[index])};
    if (ab_string_equal(&out->language, &wanted))
      language_valid = 1;
  }
  if (status == ARCHBIRD_OK && !language_valid)
    status = config_error(engine, "layers[].language is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "globs"), "layers[].globs", 1,
                          &out->globs);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "public_headers"),
                          "layers[].public_headers", 0, &out->public_headers);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "import_roots"),
                          "layers[].import_roots", 0, &out->import_roots);
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "required"),
                           "layers[].required", 1, &out->required);
  namespaces = member(value, "external_call_namespaces");
  if (status == ARCHBIRD_OK && namespaces && !yyjson_is_arr(namespaces))
    status = config_error(engine,
                          "layers[].external_call_namespaces must be an array");
  if (status == ARCHBIRD_OK && namespaces) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    out->external_namespace_count = yyjson_arr_size(namespaces);
    if (out->external_namespace_count) {
      out->external_namespaces = (AbExternalNamespace *)ab_calloc(
          engine, out->external_namespace_count,
          sizeof(*out->external_namespaces));
      if (!out->external_namespaces)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory decoding configuration");
    }
    yyjson_arr_iter_init(namespaces, &iterator);
    index = 0;
    while (status == ARCHBIRD_OK &&
           (item = yyjson_arr_iter_next(&iterator)) != NULL)
      status =
          parse_namespace(engine, item, &out->external_namespaces[index++]);
  }
  return status;
}

static ArchbirdStatus parse_component(ArchbirdEngine *engine, yyjson_val *value,
                                      AbConfigComponent *out) {
  static const char *const fields[] = {"description", "name", "paths",
                                       "required"};
  static const char *const required[] = {"name", "paths"};
  ArchbirdStatus status =
      object_shape(engine, value, "components[]", fields, 4, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "components[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status =
        copy_default_string(engine, member(value, "description"), "",
                            "components[].description", 1, &out->description);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "paths"), "components[].paths",
                          1, &out->paths);
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "required"),
                           "components[].required", 1, &out->required);
  return status;
}

typedef ArchbirdStatus (*ConfigItemParser)(ArchbirdEngine *, yyjson_val *,
                                           void *);

static ArchbirdStatus parse_item_array(ArchbirdEngine *engine,
                                       yyjson_val *value, const char *where,
                                       size_t item_size,
                                       ConfigItemParser parser,
                                       void **out_items, size_t *out_count) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  uint8_t *items = NULL;
  size_t count;
  size_t index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_items = NULL;
  *out_count = 0;
  if (!value)
    return ARCHBIRD_OK;
  if (!yyjson_is_arr(value))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET, "%s must be an array", where);
  count = yyjson_arr_size(value);
  if (count > 0 && item_size > SIZE_MAX / count)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "configuration array is too large");
  if (count) {
    items = (uint8_t *)ab_calloc(engine, count, item_size);
    if (!items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding configuration");
  }
  yyjson_arr_iter_init(value, &iterator);
  while (status == ARCHBIRD_OK &&
         (item = yyjson_arr_iter_next(&iterator)) != NULL) {
    status = parser(engine, item, items + index * item_size);
    index++;
  }
  *out_items = items;
  *out_count = count;
  return status;
}

static ArchbirdStatus parse_package(ArchbirdEngine *engine, yyjson_val *value,
                                    void *out_raw) {
  static const char *const fields[] = {"aliases", "entries", "identity",
                                       "kind",    "layer",   "name",
                                       "path",    "version"};
  static const char *const required[] = {"kind", "layer", "name", "path"};
  static const char *const kinds[] = {"generic", "npm", "python", "r"};
  AbConfigPackage *out = (AbConfigPackage *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "packages[]", fields, 8, required, 4);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "packages[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"), "packages[].kind", 0,
                         &out->kind);
  if (status == ARCHBIRD_OK && !string_one_of(&out->kind, kinds, 4))
    status = config_error(engine, "packages[].kind is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "path"), "packages[].path", 0,
                         &out->path);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "layer"), "packages[].layer", 0,
                         &out->layer);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "entries"),
                          "packages[].entries", 0, &out->entries);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "identity"), "",
                                 "packages[].identity", 1, &out->identity);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "version"), "",
                                 "packages[].version", 1, &out->version);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "aliases"),
                          "packages[].aliases", 0, &out->aliases);
  return status;
}

static ArchbirdStatus parse_build(ArchbirdEngine *engine, yyjson_val *value,
                                  void *out_raw) {
  static const char *const fields[] = {"kind", "name", "path", "variant"};
  static const char *const required[] = {"kind", "name", "path"};
  static const char *const kinds[] = {"autoconf", "compile_commands", "make",
                                      "npm"};
  AbConfigBuild *out = (AbConfigBuild *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "builds[]", fields, 4, required, 3);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "builds[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"), "builds[].kind", 0,
                         &out->kind);
  if (status == ARCHBIRD_OK && !string_one_of(&out->kind, kinds, 4))
    status = config_error(engine, "builds[].kind is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "path"), "builds[].path", 0,
                         &out->path);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "variant"), "",
                                 "builds[].variant", 1, &out->variant);
  return status;
}

static ArchbirdStatus parse_index(ArchbirdEngine *engine, yyjson_val *value,
                                  void *out_raw) {
  static const char *const fields[] = {
      "format",   "name",   "path", "path_prefix", "position_encoding_fallback",
      "required", "variant"};
  static const char *const required[] = {"format", "name", "path"};
  AbConfigIndex *out = (AbConfigIndex *)out_raw;
  yyjson_val *fallback;
  ArchbirdStatus status =
      object_shape(engine, value, "indexes[]", fields, 7, required, 3);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "indexes[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "format"), "indexes[].format", 0,
                         &out->format);
  if (status == ARCHBIRD_OK &&
      !string_one_of(&out->format, (const char *const[]){"scip"}, 1))
    status = config_error(engine, "indexes[].format is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_repository_path(engine, member(value, "path"),
                                  "indexes[].path", 0, &out->path);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "path_prefix"), "",
                                 "indexes[].path_prefix", 1, &out->path_prefix);
  if (status == ARCHBIRD_OK && out->path_prefix.length)
    status = normalize_repository_path(engine, &out->path_prefix,
                                       "indexes[].path_prefix", 0);
  if (status == ARCHBIRD_OK && out->path_prefix.length)
    status = require_canonical_directory(engine, &out->path_prefix,
                                         "indexes[].path_prefix");
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "variant"), "",
                                 "indexes[].variant", 1, &out->variant);
  fallback = member(value, "position_encoding_fallback");
  if (status == ARCHBIRD_OK && fallback) {
    const char *text;
    size_t length;
    if (!yyjson_is_str(fallback))
      status = config_error(
          engine, "indexes[].position_encoding_fallback must be a string");
    else {
      text = yyjson_get_str(fallback);
      length = yyjson_get_len(fallback);
      if (length == 4 && memcmp(text, "utf8", 4) == 0)
        out->position_encoding_fallback = 1;
      else if (length == 5 && memcmp(text, "utf16", 5) == 0)
        out->position_encoding_fallback = 2;
      else if (length == 5 && memcmp(text, "utf32", 5) == 0)
        out->position_encoding_fallback = 3;
      else
        status = config_error(
            engine,
            "indexes[].position_encoding_fallback must be utf8, utf16, or "
            "utf32");
    }
  }
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "required"),
                           "indexes[].required", 1, &out->required);
  return status;
}

static ArchbirdStatus parse_artifact_loader(ArchbirdEngine *engine,
                                            yyjson_val *value, void *out_raw) {
  static const char *const fields[] = {"paths", "pattern", "required"};
  static const char *const required[] = {"paths", "pattern"};
  AbConfigArtifactLoader *out = (AbConfigArtifactLoader *)out_raw;
  ArchbirdStatus status = object_shape(engine, value, "artifacts[].loaded_by[]",
                                       fields, 3, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_repository_paths(engine, member(value, "paths"),
                                   "artifacts[].loaded_by[].paths", 1, 1,
                                   &out->paths);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "pattern"),
                         "artifacts[].loaded_by[].pattern", 0, &out->pattern);
  if (status == ARCHBIRD_OK) {
    AbPattern *pattern = NULL;
    status = ab_pattern_compile(engine, &out->pattern, SIZE_MAX, &pattern);
    ab_pattern_free(pattern);
  }
  if (status == ARCHBIRD_OK)
    status =
        boolean_value(engine, member(value, "required"),
                      "artifacts[].loaded_by[].required", 1, &out->required);
  return status;
}

static ArchbirdStatus parse_artifact_build(ArchbirdEngine *engine,
                                           yyjson_val *value, void *out_raw) {
  static const char *const fields[] = {"source", "target"};
  AbConfigArtifactBuild *out = (AbConfigArtifactBuild *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "artifacts[].builds[]", fields, 2, fields, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "source"),
                         "artifacts[].builds[].source", 0, &out->source);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "target"),
                         "artifacts[].builds[].target", 0, &out->target);
  return status;
}

static ArchbirdStatus parse_artifact(ArchbirdEngine *engine, yyjson_val *value,
                                     void *out_raw) {
  static const char *const fields[] = {"builds", "inputs", "loaded_by",
                                       "name",   "output", "required"};
  static const char *const required[] = {"name", "output"};
  AbConfigArtifact *out = (AbConfigArtifact *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "artifacts[]", fields, 6, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "artifacts[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_repository_path(engine, member(value, "output"),
                                  "artifacts[].output", 0, &out->output);
  if (status == ARCHBIRD_OK)
    status = copy_repository_paths(engine, member(value, "inputs"),
                                   "artifacts[].inputs", 0, 1, &out->inputs);
  if (status == ARCHBIRD_OK)
    status = parse_item_array(engine, member(value, "loaded_by"),
                              "artifacts[].loaded_by", sizeof(*out->loaders),
                              parse_artifact_loader, (void **)&out->loaders,
                              &out->loader_count);
  if (status == ARCHBIRD_OK)
    status =
        parse_item_array(engine, member(value, "builds"), "artifacts[].builds",
                         sizeof(*out->builds), parse_artifact_build,
                         (void **)&out->builds, &out->build_count);
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "required"),
                           "artifacts[].required", 1, &out->required);
  return status;
}

static ArchbirdStatus parse_provider(ArchbirdEngine *engine, yyjson_val *value,
                                     void *out_raw) {
  static const char *const fields[] = {"kind", "path", "pattern", "variable"};
  static const char *const required[] = {"kind"};
  static const char *const kinds[] = {"exports", "file_pattern",
                                      "make_variable"};
  AbConfigProvider *out = (AbConfigProvider *)out_raw;
  ArchbirdStatus status = object_shape(engine, value, "bridges[].providers[]",
                                       fields, 4, required, 1);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"),
                         "bridges[].providers[].kind", 0, &out->kind);
  if (status == ARCHBIRD_OK && !string_one_of(&out->kind, kinds, 3))
    status = config_error(engine, "bridges[].providers[].kind is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "path"), "",
                                 "bridges[].providers[].path", 1, &out->path);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "variable"), "",
                                 "bridges[].providers[].variable", 1,
                                 &out->variable);
  if (status == ARCHBIRD_OK)
    status =
        copy_default_string(engine, member(value, "pattern"), "",
                            "bridges[].providers[].pattern", 1, &out->pattern);
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"exports"}, 1) &&
      (out->variable.length || out->pattern.length))
    status =
        config_error(engine, "exports provider accepts only an optional path");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"make_variable"}, 1) &&
      (!out->path.length || !out->variable.length || !out->pattern.length))
    status = config_error(
        engine, "make_variable provider requires path, variable, and pattern");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"file_pattern"}, 1) &&
      (!out->path.length || !out->pattern.length))
    status =
        config_error(engine, "file_pattern provider requires path and pattern");
  if (status == ARCHBIRD_OK && out->pattern.length) {
    AbPattern *pattern = NULL;
    status = ab_pattern_compile(engine, &out->pattern, 1, &pattern);
    ab_pattern_free(pattern);
  }
  return status;
}

static ArchbirdStatus parse_bridge(ArchbirdEngine *engine, yyjson_val *value,
                                   void *out_raw) {
  static const char *const fields[] = {"bidirectional",
                                       "exclude_from_paths",
                                       "from",
                                       "from_paths",
                                       "ignore",
                                       "kind",
                                       "message_keys",
                                       "name",
                                       "prefixes",
                                       "providers",
                                       "to"};
  static const char *const required[] = {"from", "kind", "name", "to"};
  static const char *const kinds[] = {"abi", "binding", "message"};
  AbConfigBridge *out = (AbConfigBridge *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "bridges[]", fields, 11, required, 4);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "bridges[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"), "bridges[].kind", 0,
                         &out->kind);
  if (status == ARCHBIRD_OK && !string_one_of(&out->kind, kinds, 3))
    status = config_error(engine, "bridges[].kind is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_string_or_strings(engine, member(value, "from"),
                                    "bridges[].from", &out->from_layers);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "from_paths"),
                          "bridges[].from_paths", 0, &out->from_paths);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "exclude_from_paths"),
                          "bridges[].exclude_from_paths", 0,
                          &out->exclude_from_paths);
  if (status == ARCHBIRD_OK)
    status = copy_string_or_strings(engine, member(value, "to"), "bridges[].to",
                                    &out->to_layers);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "prefixes"),
                          "bridges[].prefixes", 0, &out->prefixes);
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"abi"}, 1) &&
      !out->prefixes.count)
    status = config_error(engine, "ABI bridges require prefixes");
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "bidirectional"),
                           "bridges[].bidirectional", 0, &out->bidirectional);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "message_keys"),
                          "bridges[].message_keys", 0, &out->message_keys);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "ignore"), "bridges[].ignore",
                          0, &out->ignore);
  if (status == ARCHBIRD_OK)
    status = parse_item_array(engine, member(value, "providers"),
                              "bridges[].providers", sizeof(*out->providers),
                              parse_provider, (void **)&out->providers,
                              &out->provider_count);
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"message"}, 1) &&
      out->provider_count)
    status = config_error(engine, "message bridges do not have providers");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"message"}, 1) &&
      (out->from_paths.count || out->exclude_from_paths.count))
    status = config_error(engine,
                          "message bridges do not accept source path filters");
  return status;
}

static ArchbirdStatus parse_test_case_route(ArchbirdEngine *engine,
                                            yyjson_val *value, void *out_raw) {
  static const char *const fields[] = {"paths", "selector", "target_symbols",
                                       "to"};
  static const char *const required[] = {"selector", "to"};
  AbConfigTestCaseRoute *out = (AbConfigTestCaseRoute *)out_raw;
  ArchbirdStatus status = object_shape(engine, value, "tests[].case_routes[]",
                                       fields, 4, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "selector"),
                         "tests[].case_routes[].selector", 0, &out->selector);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "paths"),
                          "tests[].case_routes[].paths", 0, &out->paths);
  if (status == ARCHBIRD_OK)
    status = copy_string_or_strings(engine, member(value, "to"),
                                    "tests[].case_routes[].to", &out->targets);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "target_symbols"),
                          "tests[].case_routes[].target_symbols", 0,
                          &out->target_symbols);
  return status;
}

static ArchbirdStatus parse_test_case_extractor(ArchbirdEngine *engine,
                                                yyjson_val *value,
                                                void *out_raw) {
  static const char *const fields[] = {
      "call",     "kind", "name", "selector_argument", "selector_arguments",
      "separator"};
  static const char *const required[] = {"kind"};
  AbConfigTestCaseExtractor *out = (AbConfigTestCaseExtractor *)out_raw;
  ArchbirdStatus status = object_shape(
      engine, value, "tests[].case_extractors[]", fields, 6, required, 1);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"),
                         "tests[].case_extractors[].kind", 0, &out->kind);
  if (status == ARCHBIRD_OK &&
      !string_one_of(&out->kind,
                     (const char *const[]){"c_macro", "named_dispatch"}, 2))
    status =
        config_error(engine, "tests[].case_extractors[].kind is unsupported");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->kind, (const char *const[]){"c_macro"}, 1)) {
    status = copy_string(engine, member(value, "call"),
                         "tests[].case_extractors[].call", 0, &out->call);
    if (status == ARCHBIRD_OK)
      status =
          copy_sizes(engine, member(value, "selector_arguments"),
                     "tests[].case_extractors[].selector_arguments", 1,
                     &out->selector_arguments, &out->selector_argument_count);
    if (status == ARCHBIRD_OK)
      status = copy_default_string(engine, member(value, "separator"), ".",
                                   "tests[].case_extractors[].separator", 1,
                                   &out->separator);
    if (status == ARCHBIRD_OK &&
        (member(value, "name") || member(value, "selector_argument")))
      status = config_error(
          engine, "c_macro case extractors do not accept dispatch fields");
  } else if (status == ARCHBIRD_OK) {
    status = copy_string(engine, member(value, "name"),
                         "tests[].case_extractors[].name", 0, &out->name);
    if (status == ARCHBIRD_OK)
      status = size_value(engine, member(value, "selector_argument"),
                          "tests[].case_extractors[].selector_argument", 0, 0,
                          &out->selector_argument);
    if (status == ARCHBIRD_OK && out->selector_argument > 31)
      status = config_error(
          engine, "named_dispatch selector_argument must be at most 31");
    if (status == ARCHBIRD_OK &&
        (member(value, "call") || member(value, "selector_arguments") ||
         member(value, "separator")))
      status = config_error(
          engine, "named_dispatch case extractors do not accept macro fields");
  }
  return status;
}

static ArchbirdStatus parse_test_generated_file(ArchbirdEngine *engine,
                                                yyjson_val *value,
                                                void *out_raw) {
  static const char *const fields[] = {"globs", "sources"};
  static const char *const required[] = {"globs", "sources"};
  AbConfigTestGeneratedFile *out = (AbConfigTestGeneratedFile *)out_raw;
  ArchbirdStatus status = object_shape(
      engine, value, "tests[].generated_files[]", fields, 2, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_repository_paths(engine, member(value, "globs"),
                                   "tests[].generated_files[].globs", 1, 1,
                                   &out->globs);
  if (status == ARCHBIRD_OK)
    status = copy_repository_paths(engine, member(value, "sources"),
                                   "tests[].generated_files[].sources", 1, 0,
                                   &out->sources);
  return status;
}

static ArchbirdStatus parse_test(ArchbirdEngine *engine, yyjson_val *value,
                                 void *out_raw) {
  static const char *const fields[] = {
      "case_extractors", "case_routes", "generated_files", "globs",
      "language",        "name",        "required",        "route_to"};
  static const char *const required[] = {"globs", "language", "name",
                                         "route_to"};
  static const char *const languages[] = {"c", "cpp",  "javascript", "python",
                                          "r", "text", "typescript", "vue"};
  AbConfigTest *out = (AbConfigTest *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "tests[]", fields, 8, required, 4);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "tests[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "language"), "tests[].language",
                         0, &out->language);
  if (status == ARCHBIRD_OK && !string_one_of(&out->language, languages, 8))
    status = config_error(engine, "tests[].language is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "globs"), "tests[].globs", 1,
                          &out->globs);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "route_to"), "tests[].route_to",
                          1, &out->route_to);
  if (status == ARCHBIRD_OK)
    status = parse_item_array(engine, member(value, "case_routes"),
                              "tests[].case_routes", sizeof(*out->case_routes),
                              parse_test_case_route, (void **)&out->case_routes,
                              &out->case_route_count);
  if (status == ARCHBIRD_OK)
    status = parse_item_array(
        engine, member(value, "case_extractors"), "tests[].case_extractors",
        sizeof(*out->case_extractors), parse_test_case_extractor,
        (void **)&out->case_extractors, &out->case_extractor_count);
  if (status == ARCHBIRD_OK)
    status = parse_item_array(
        engine, member(value, "generated_files"), "tests[].generated_files",
        sizeof(*out->generated_files), parse_test_generated_file,
        (void **)&out->generated_files, &out->generated_file_count);
  if (status == ARCHBIRD_OK) {
    size_t extractor;
    for (extractor = 0; extractor < out->case_extractor_count; extractor++) {
      const AbConfigTestCaseExtractor *row = &out->case_extractors[extractor];
      if (string_one_of(&row->kind, (const char *const[]){"c_macro"}, 1) &&
          !string_one_of(&out->language, (const char *const[]){"c", "cpp"}, 2))
        status = config_error(engine, "c_macro case extractors require C/C++");
      if (string_one_of(&row->kind, (const char *const[]){"named_dispatch"},
                        1) &&
          !string_one_of(&out->language,
                         (const char *const[]){"c", "cpp", "python"}, 3))
        status = config_error(
            engine, "named_dispatch case extractors require C/C++ or Python");
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  if (status == ARCHBIRD_OK) {
    size_t left;
    for (left = 0; left < out->case_extractor_count; left++) {
      size_t right;
      for (right = left + 1; right < out->case_extractor_count; right++) {
        const AbConfigTestCaseExtractor *a = &out->case_extractors[left];
        const AbConfigTestCaseExtractor *b = &out->case_extractors[right];
        const AbString *a_name = a->call.length ? &a->call : &a->name;
        const AbString *b_name = b->call.length ? &b->call : &b->name;
        if (ab_string_equal(&a->kind, &b->kind) &&
            ab_string_equal(a_name, b_name)) {
          status = config_error(
              engine, "tests[].case_extractors identities must be unique");
          break;
        }
      }
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "required"),
                           "tests[].required", 1, &out->required);
  return status;
}

static ArchbirdStatus parse_named_entry(ArchbirdEngine *engine,
                                        yyjson_val *value, void *out_raw) {
  static const char *const fields[] = {"argument", "functions", "globs", "kind",
                                       "name"};
  static const char *const required[] = {"functions", "globs", "kind", "name"};
  AbConfigNamedEntry *out = (AbConfigNamedEntry *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "named_entries[]", fields, 5, required, 4);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "named_entries[].name",
                         0, &out->name);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "kind"), "named_entries[].kind",
                         0, &out->kind);
  if (status == ARCHBIRD_OK &&
      !string_one_of(&out->kind, (const char *const[]){"call_string_arg"}, 1))
    status = config_error(engine, "named_entries[].kind is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "functions"),
                          "named_entries[].functions", 1, &out->functions);
  if (status == ARCHBIRD_OK)
    status = size_value(engine, member(value, "argument"),
                        "named_entries[].argument", 0, 0, &out->argument);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "globs"),
                          "named_entries[].globs", 1, &out->globs);
  return status;
}

static ArchbirdStatus parse_parity_member(ArchbirdEngine *engine,
                                          yyjson_val *value, void *out_raw) {
  static const char *const fields[] = {"bridge",  "exclude", "include",
                                       "kinds",   "label",   "layer",
                                       "package", "source"};
  static const char *const required[] = {"label", "source"};
  static const char *const sources[] = {"bridge", "package", "symbols"};
  AbConfigParityMember *out = (AbConfigParityMember *)out_raw;
  ArchbirdStatus status =
      object_shape(engine, value, "parity[].members[]", fields, 8, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "label"),
                         "parity[].members[].label", 0, &out->label);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "source"),
                         "parity[].members[].source", 0, &out->source);
  if (status == ARCHBIRD_OK && !string_one_of(&out->source, sources, 3))
    status = config_error(engine, "parity[].members[].source is unsupported");
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "layer"), "",
                                 "parity[].members[].layer", 1, &out->layer);
  if (status == ARCHBIRD_OK)
    status =
        copy_default_string(engine, member(value, "package"), "",
                            "parity[].members[].package", 1, &out->package);
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "bridge"), "",
                                 "parity[].members[].bridge", 1, &out->bridge);
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->source, (const char *const[]){"symbols"}, 1) &&
      !out->layer.length)
    status = config_error(engine, "symbol parity member requires a layer");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->source, (const char *const[]){"package"}, 1) &&
      !out->package.length)
    status = config_error(engine, "package parity member requires a package");
  if (status == ARCHBIRD_OK &&
      string_one_of(&out->source, (const char *const[]){"bridge"}, 1) &&
      !out->bridge.length)
    status = config_error(engine, "bridge parity member requires a bridge");
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "include"),
                          "parity[].members[].include", 0, &out->include);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "exclude"),
                          "parity[].members[].exclude", 0, &out->exclude);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "kinds"),
                          "parity[].members[].kinds", 0, &out->kinds);
  return status;
}

static int string_pair_compare(const void *left_raw, const void *right_raw) {
  const AbStringPair *left = (const AbStringPair *)left_raw;
  const AbStringPair *right = (const AbStringPair *)right_raw;
  int compared = ab_string_compare(&left->key, &right->key);
  return compared ? compared : ab_string_compare(&left->value, &right->value);
}

static ArchbirdStatus parse_aliases(ArchbirdEngine *engine, yyjson_val *value,
                                    AbStringPair **out_items,
                                    size_t *out_count) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  AbStringPair *items = NULL;
  size_t count = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_items = NULL;
  *out_count = 0;
  if (!value)
    return ARCHBIRD_OK;
  if (!yyjson_is_obj(value))
    return config_error(engine, "parity[].aliases must be an object");
  count = yyjson_obj_size(value);
  if (count) {
    items = (AbStringPair *)ab_calloc(engine, count, sizeof(*items));
    if (!items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding parity aliases");
  }
  yyjson_obj_iter_init(value, &iterator);
  count = 0;
  while (status == ARCHBIRD_OK &&
         (key = yyjson_obj_iter_next(&iterator)) != NULL) {
    yyjson_val *item = yyjson_obj_iter_get_val(key);
    if (!yyjson_is_str(item)) {
      status = config_error(engine, "parity[].aliases values must be strings");
      break;
    }
    status = ab_string_copy(engine, &items[count].key, yyjson_get_str(key),
                            yyjson_get_len(key));
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(engine, &items[count].value, yyjson_get_str(item),
                              yyjson_get_len(item));
    count++;
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(items, count, sizeof(*items), string_pair_compare);
  *out_items = items;
  *out_count = count;
  return status;
}

static ArchbirdStatus parse_parity(ArchbirdEngine *engine, yyjson_val *value,
                                   void *out_raw) {
  static const char *const fields[] = {
      "aliases", "case", "enforce",        "ignore",
      "members", "name", "strip_prefixes", "strip_suffixes"};
  static const char *const required[] = {"members", "name"};
  static const char *const cases[] = {"identity", "lower", "snake"};
  AbConfigParity *out = (AbConfigParity *)out_raw;
  size_t left;
  size_t right;
  ArchbirdStatus status =
      object_shape(engine, value, "parity[]", fields, 8, required, 2);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, member(value, "name"), "parity[].name", 0,
                         &out->name);
  if (status == ARCHBIRD_OK)
    status =
        parse_item_array(engine, member(value, "members"), "parity[].members",
                         sizeof(*out->members), parse_parity_member,
                         (void **)&out->members, &out->member_count);
  if (status == ARCHBIRD_OK && out->member_count < 2)
    status =
        config_error(engine, "parity[].members requires at least two rows");
  for (left = 0; status == ARCHBIRD_OK && left < out->member_count; left++) {
    for (right = left + 1; right < out->member_count; right++) {
      if (ab_string_equal(&out->members[left].label,
                          &out->members[right].label))
        status = config_error(engine, "parity[].members labels must be unique");
    }
  }
  if (status == ARCHBIRD_OK)
    status = copy_default_string(engine, member(value, "case"), "identity",
                                 "parity[].case", 0, &out->case_name);
  if (status == ARCHBIRD_OK && !string_one_of(&out->case_name, cases, 3))
    status =
        config_error(engine, "parity[].case must be identity, lower, or snake");
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "strip_prefixes"),
                          "parity[].strip_prefixes", 0, &out->strip_prefixes);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "strip_suffixes"),
                          "parity[].strip_suffixes", 0, &out->strip_suffixes);
  if (status == ARCHBIRD_OK)
    status = parse_aliases(engine, member(value, "aliases"), &out->aliases,
                           &out->alias_count);
  if (status == ARCHBIRD_OK)
    status = copy_strings(engine, member(value, "ignore"), "parity[].ignore", 0,
                          &out->ignore);
  if (status == ARCHBIRD_OK)
    status = boolean_value(engine, member(value, "enforce"), "parity[].enforce",
                           0, &out->enforce);
  return status;
}

static ArchbirdStatus validate_optional_sections(ArchbirdEngine *engine,
                                                 yyjson_val *root) {
  static const char *const arrays[] = {"artifacts",  "bridges", "builds",
                                       "components", "indexes", "named_entries",
                                       "packages",   "parity",  "tests"};
  size_t index;
  for (index = 0; index < sizeof(arrays) / sizeof(arrays[0]); index++) {
    yyjson_val *value = member(root, arrays[index]);
    if (value && !yyjson_is_arr(value))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "configuration section must be an array");
  }
  if (member(root, "limits") && !yyjson_is_obj(member(root, "limits")))
    return config_error(engine, "limits must be an object");
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_limits(ArchbirdEngine *engine, yyjson_val *value,
                                   AbMapConfig *out) {
  static const char *const fields[] = {"compact_edge_names", "compact_symbols",
                                       "max_file_bytes", "max_index_bytes"};
  ArchbirdStatus status;
  out->max_file_bytes = 4000000;
  out->max_index_bytes = 536870912;
  out->compact_symbols = 10;
  out->compact_edge_names = 12;
  if (!value)
    return ARCHBIRD_OK;
  status = object_shape(engine, value, "limits", fields, 4, NULL, 0);
  if (status == ARCHBIRD_OK)
    status =
        size_value(engine, member(value, "max_file_bytes"),
                   "limits.max_file_bytes", 4000000, 1, &out->max_file_bytes);
  if (status == ARCHBIRD_OK)
    status = size_value(engine, member(value, "max_index_bytes"),
                        "limits.max_index_bytes", 536870912, 1,
                        &out->max_index_bytes);
  if (status == ARCHBIRD_OK)
    status = size_value(engine, member(value, "compact_symbols"),
                        "limits.compact_symbols", 10, 1, &out->compact_symbols);
  if (status == ARCHBIRD_OK)
    status = size_value(engine, member(value, "compact_edge_names"),
                        "limits.compact_edge_names", 12, 1,
                        &out->compact_edge_names);
  return status;
}

static ArchbirdStatus ensure_unique_names(ArchbirdEngine *engine,
                                          AbMapConfig *config) {
  struct NamedRows {
    void *items;
    size_t count;
    size_t item_size;
    size_t name_offset;
    const char *label;
  } rows[] = {
      {config->layers, config->layer_count, sizeof(*config->layers),
       offsetof(AbConfigLayer, name), "layers"},
      {config->packages, config->package_count, sizeof(*config->packages),
       offsetof(AbConfigPackage, name), "packages"},
      {config->builds, config->build_count, sizeof(*config->builds),
       offsetof(AbConfigBuild, name), "builds"},
      {config->indexes, config->index_count, sizeof(*config->indexes),
       offsetof(AbConfigIndex, name), "indexes"},
      {config->artifacts, config->artifact_count, sizeof(*config->artifacts),
       offsetof(AbConfigArtifact, name), "artifacts"},
      {config->bridges, config->bridge_count, sizeof(*config->bridges),
       offsetof(AbConfigBridge, name), "bridges"},
      {config->tests, config->test_count, sizeof(*config->tests),
       offsetof(AbConfigTest, name), "tests"},
      {config->components, config->component_count, sizeof(*config->components),
       offsetof(AbConfigComponent, name), "components"},
      {config->named_entries, config->named_entry_count,
       sizeof(*config->named_entries), offsetof(AbConfigNamedEntry, name),
       "named_entries"},
      {config->parity, config->parity_count, sizeof(*config->parity),
       offsetof(AbConfigParity, name), "parity"}};
  size_t row;
  size_t left;
  size_t right;
  for (row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
    for (left = 0; left < rows[row].count; left++) {
      const AbString *left_name =
          (const AbString *)((const uint8_t *)rows[row].items +
                             left * rows[row].item_size +
                             rows[row].name_offset);
      for (right = left + 1; right < rows[row].count; right++) {
        const AbString *right_name =
            (const AbString *)((const uint8_t *)rows[row].items +
                               right * rows[row].item_size +
                               rows[row].name_offset);
        if (ab_string_equal(left_name, right_name))
          return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "%s have duplicate names", rows[row].label);
      }
    }
  }
  for (left = 0; left < config->layer_count; left++) {
    size_t prefix_left;
    size_t prefix_right;
    for (prefix_left = 0;
         prefix_left < config->layers[left].external_namespace_count;
         prefix_left++) {
      for (prefix_right = prefix_left + 1;
           prefix_right < config->layers[left].external_namespace_count;
           prefix_right++) {
        if (ab_string_equal(
                &config->layers[left].external_namespaces[prefix_left].prefix,
                &config->layers[left].external_namespaces[prefix_right].prefix))
          return config_error(
              engine, "external call namespaces have duplicate prefixes");
      }
    }
  }
  return ARCHBIRD_OK;
}

static int name_exists(const void *items, size_t count, size_t item_size,
                       size_t name_offset, const AbString *name) {
  size_t index;
  for (index = 0; index < count; index++) {
    const AbString *candidate =
        (const AbString *)((const uint8_t *)items + index * item_size +
                           name_offset);
    if (ab_string_equal(candidate, name))
      return 1;
  }
  return 0;
}

static int layer_exists(const AbMapConfig *config, const AbString *name) {
  return name_exists(config->layers, config->layer_count,
                     sizeof(*config->layers), offsetof(AbConfigLayer, name),
                     name);
}

static int package_exists(const AbMapConfig *config, const AbString *name) {
  return name_exists(config->packages, config->package_count,
                     sizeof(*config->packages), offsetof(AbConfigPackage, name),
                     name);
}

static int bridge_exists(const AbMapConfig *config, const AbString *name,
                         int surface_only) {
  size_t index;
  for (index = 0; index < config->bridge_count; index++) {
    if (ab_string_equal(&config->bridges[index].name, name)) {
      return !surface_only ||
             string_one_of(&config->bridges[index].kind,
                           (const char *const[]){"abi", "binding"}, 2);
    }
  }
  return 0;
}

static int build_path_exists(const AbMapConfig *config, const AbString *path) {
  size_t index;
  for (index = 0; index < config->build_count; index++) {
    if (ab_string_equal(&config->builds[index].path, path))
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_references(ArchbirdEngine *engine,
                                          const AbMapConfig *config) {
  size_t index;
  size_t nested;
  for (index = 0; index < config->package_count; index++) {
    if (!layer_exists(config, &config->packages[index].layer))
      return config_error(engine, "package refers to an unknown layer");
  }
  for (index = 0; index < config->artifact_count; index++) {
    for (nested = 0; nested < config->artifacts[index].build_count; nested++) {
      if (!build_path_exists(config,
                             &config->artifacts[index].builds[nested].source))
        return config_error(engine,
                            "artifact refers to an unconfigured build source");
    }
  }
  for (index = 0; index < config->bridge_count; index++) {
    const AbConfigBridge *bridge = &config->bridges[index];
    for (nested = 0; nested < bridge->from_layers.count; nested++) {
      if (!layer_exists(config, &bridge->from_layers.items[nested]))
        return config_error(engine, "bridge refers to an unknown source layer");
    }
    for (nested = 0; nested < bridge->to_layers.count; nested++) {
      if (!layer_exists(config, &bridge->to_layers.items[nested]))
        return config_error(engine, "bridge refers to an unknown target layer");
    }
  }
  for (index = 0; index < config->test_count; index++) {
    for (nested = 0; nested < config->tests[index].route_to.count; nested++) {
      if (!layer_exists(config, &config->tests[index].route_to.items[nested]))
        return config_error(engine, "test refers to an unknown route layer");
    }
  }
  for (index = 0; index < config->parity_count; index++) {
    for (nested = 0; nested < config->parity[index].member_count; nested++) {
      const AbConfigParityMember *member =
          &config->parity[index].members[nested];
      if (member->layer.length && !layer_exists(config, &member->layer))
        return config_error(engine, "parity member refers to an unknown layer");
      if (member->package.length && !package_exists(config, &member->package))
        return config_error(engine,
                            "parity member refers to an unknown package");
      if (member->bridge.length && !bridge_exists(config, &member->bridge, 0))
        return config_error(engine,
                            "parity member refers to an unknown bridge");
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_decode_map_config(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbMapConfig *out) {
  AbProjectConfiguration project_configuration = {0};
  if (!engine || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  {
    static const char *const fields[] = {
        "artifacts",   "bridges",        "builds",    "components",
        "constraints", "description",    "discovery", "exclude",
        "indexes",     "layers",         "limits",    "named_entries",
        "packages",    "parity",         "project",   "projections",
        "queries",     "schema_version", "tests"};
    static const char *const required[] = {"layers", "project",
                                           "schema_version"};
    yyjson_doc *document = NULL;
    yyjson_val *root;
    yyjson_val *layers;
    yyjson_val *components;
    yyjson_arr_iter iterator;
    yyjson_val *item;
    size_t index;
    uint64_t schema_version = 0;
    ArchbirdStatus status;
    if (!engine || !out)
      return ARCHBIRD_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    status = ab_project_configuration_decode(engine, json, json_length,
                                             &project_configuration);
    if (status == ARCHBIRD_OK)
      status =
          archbird_json_parse_document(engine, json, json_length, &document, 1);
    if (status != ARCHBIRD_OK)
      goto done;
    root = yyjson_doc_get_root(document);
    status = object_shape(engine, root, "project configuration", fields, 19,
                          required, 3);
    if (status == ARCHBIRD_OK &&
        (!unsigned_value(member(root, "schema_version"), &schema_version) ||
         schema_version != 2))
      status = config_error(engine, "schema_version must equal 2");
    if (status == ARCHBIRD_OK)
      status = validate_optional_sections(engine, root);
    if (status == ARCHBIRD_OK)
      status = copy_string(engine, member(root, "project"), "project", 0,
                           &out->project);
    if (status == ARCHBIRD_OK)
      status = copy_default_string(engine, member(root, "description"), "",
                                   "description", 1, &out->description);
    if (status == ARCHBIRD_OK)
      status = copy_default_string(engine, NULL, ".", "root", 0, &out->root);
    if (status == ARCHBIRD_OK)
      status = parse_discovery_options(engine, member(root, "discovery"), out);
    if (status == ARCHBIRD_OK)
      status = copy_excludes(engine, member(root, "exclude"),
                             out->default_excludes, &out->exclude);
    layers = member(root, "layers");
    if (status == ARCHBIRD_OK &&
        (!yyjson_is_arr(layers) || yyjson_arr_size(layers) == 0))
      status = config_error(engine, "layers must be a nonempty array");
    if (status == ARCHBIRD_OK) {
      out->layer_count = yyjson_arr_size(layers);
      out->layers = (AbConfigLayer *)ab_calloc(engine, out->layer_count,
                                               sizeof(*out->layers));
      if (!out->layers)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory decoding configuration");
    }
    if (status == ARCHBIRD_OK) {
      yyjson_arr_iter_init(layers, &iterator);
      index = 0;
      while (status == ARCHBIRD_OK &&
             (item = yyjson_arr_iter_next(&iterator)) != NULL)
        status = parse_layer(engine, item, &out->layers[index++]);
    }
    components = member(root, "components");
    if (status == ARCHBIRD_OK && components) {
      out->component_count = yyjson_arr_size(components);
      if (out->component_count) {
        out->components = (AbConfigComponent *)ab_calloc(
            engine, out->component_count, sizeof(*out->components));
        if (!out->components)
          status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                      ARCHBIRD_NO_OFFSET,
                                      "out of memory decoding configuration");
      }
    }
    if (status == ARCHBIRD_OK && components) {
      yyjson_arr_iter_init(components, &iterator);
      index = 0;
      while (status == ARCHBIRD_OK &&
             (item = yyjson_arr_iter_next(&iterator)) != NULL)
        status = parse_component(engine, item, &out->components[index++]);
    }
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "packages"), "packages",
                                sizeof(*out->packages), parse_package,
                                (void **)&out->packages, &out->package_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "builds"), "builds",
                                sizeof(*out->builds), parse_build,
                                (void **)&out->builds, &out->build_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "indexes"), "indexes",
                                sizeof(*out->indexes), parse_index,
                                (void **)&out->indexes, &out->index_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "artifacts"), "artifacts",
                                sizeof(*out->artifacts), parse_artifact,
                                (void **)&out->artifacts, &out->artifact_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "bridges"), "bridges",
                                sizeof(*out->bridges), parse_bridge,
                                (void **)&out->bridges, &out->bridge_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "tests"), "tests",
                                sizeof(*out->tests), parse_test,
                                (void **)&out->tests, &out->test_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "named_entries"),
                                "named_entries", sizeof(*out->named_entries),
                                parse_named_entry, (void **)&out->named_entries,
                                &out->named_entry_count);
    if (status == ARCHBIRD_OK)
      status = parse_item_array(engine, member(root, "parity"), "parity",
                                sizeof(*out->parity), parse_parity,
                                (void **)&out->parity, &out->parity_count);
    if (status == ARCHBIRD_OK)
      status = parse_limits(engine, member(root, "limits"), out);
    if (status == ARCHBIRD_OK)
      status = ensure_unique_names(engine, out);
    if (status == ARCHBIRD_OK)
      status = validate_references(engine, out);
    if (status == ARCHBIRD_OK) {
      size_t digest_index;
      for (digest_index = 0; digest_index < 32; digest_index++) {
        const char high =
            project_configuration.map_config_sha256[digest_index * 2];
        const char low =
            project_configuration.map_config_sha256[digest_index * 2 + 1];
        const uint8_t high_value =
            (uint8_t)(high <= '9' ? high - '0' : high - 'a' + 10);
        const uint8_t low_value =
            (uint8_t)(low <= '9' ? low - '0' : low - 'a' + 10);
        out->sha256[digest_index] = (uint8_t)((high_value << 4) | low_value);
      }
      if (status == ARCHBIRD_OK)
        memcpy(out->sha256_hex, project_configuration.map_config_sha256, 65);
    }
  done:
    yyjson_doc_free(document);
    ab_project_configuration_free(engine, &project_configuration);
    if (status != ARCHBIRD_OK)
      ab_map_config_free(engine, out);
    return status;
  }
}

const AbConfigLayer *ab_map_config_layer(const AbMapConfig *config,
                                         const AbString *name) {
  size_t index;
  if (!config || !name)
    return NULL;
  for (index = 0; index < config->layer_count; index++) {
    if (ab_string_equal(&config->layers[index].name, name))
      return &config->layers[index];
  }
  return NULL;
}
