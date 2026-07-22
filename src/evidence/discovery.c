#include "archbird_internal.h"

#include "config.h"
#include "gitignore.h"
#include "path_match.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct DiscoveryRow {
  const AbString *path;
  AbString layer;
  AbString language;
  AbStringArray roles;
} DiscoveryRow;

struct ArchbirdDiscovery {
  ArchbirdEngine *engine;
  AbMapConfig config;
  AbStringArray paths;
  size_t path_capacity;
  AbIgnoreSet ignores;
  int ignores_ready;
};

static int repository_path_valid(const char *path, size_t length) {
  size_t segment = 0;
  size_t index;
  if (!length || path[0] == '/' || path[length - 1] == '/' ||
      (length >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':'))
    return 0;
  for (index = 0; index <= length; index++) {
    if (index < length && path[index] != '/') {
      if (path[index] == '\\' || path[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment || (index - segment == 1 && path[segment] == '.') ||
        (index - segment == 2 && path[segment] == '.' &&
         path[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus append_unique(ArchbirdEngine *engine,
                                    AbStringArray *array, const char *data,
                                    size_t length) {
  AbString candidate = {(char *)data, length};
  AbString *resized;
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (ab_string_equal(&array->items[index], &candidate))
      return ARCHBIRD_OK;
  }
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting discovery values");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_discovery_path(ArchbirdEngine *engine,
                                         ArchbirdDiscovery *discovery,
                                         const char *data, size_t length) {
  AbString candidate = {(char *)data, length};
  AbString copied = {0};
  size_t first = 0;
  size_t last = discovery->paths.count;
  size_t index;
  while (first < last) {
    size_t middle = first + (last - first) / 2;
    int comparison =
        ab_string_compare(&discovery->paths.items[middle], &candidate);
    if (comparison < 0)
      first = middle + 1;
    else
      last = middle;
  }
  index = first;
  if (index < discovery->paths.count &&
      ab_string_equal(&discovery->paths.items[index], &candidate))
    return ARCHBIRD_OK;
  if (discovery->paths.count == discovery->path_capacity) {
    AbString *resized;
    size_t capacity = discovery->path_capacity ? discovery->path_capacity : 64;
    if (discovery->path_capacity) {
      if (capacity > SIZE_MAX / 2)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "discovery path capacity overflow");
      capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*resized))
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "discovery path capacity overflow");
    resized = (AbString *)ab_realloc(engine, discovery->paths.items,
                                     capacity * sizeof(*resized));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting discovery paths");
    discovery->paths.items = resized;
    discovery->path_capacity = capacity;
  }
  if (ab_string_copy(engine, &copied, data, length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  if (index < discovery->paths.count)
    memmove(&discovery->paths.items[index + 1], &discovery->paths.items[index],
            (discovery->paths.count - index) * sizeof(*discovery->paths.items));
  discovery->paths.items[index] = copied;
  discovery->paths.count++;
  return ARCHBIRD_OK;
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

ArchbirdStatus archbird_discovery_create(ArchbirdEngine *engine,
                                         const uint8_t *config_json,
                                         size_t config_length,
                                         ArchbirdDiscovery **out_discovery) {
  ArchbirdDiscovery *discovery;
  ArchbirdStatus status;
  if (!engine || (!config_json && config_length) || !out_discovery)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_discovery = NULL;
  discovery = (ArchbirdDiscovery *)ab_calloc(engine, 1, sizeof(*discovery));
  if (!discovery)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating discovery plan");
  discovery->engine = engine;
  ab_ignore_set_init(&discovery->ignores, engine);
  status = ab_decode_map_config(engine, config_json, config_length,
                                &discovery->config);
  if (status != ARCHBIRD_OK) {
    ab_ignore_set_free(&discovery->ignores);
    ab_free(engine, discovery);
    return status;
  }
  *out_discovery = discovery;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_discovery_add_path(ArchbirdEngine *engine,
                                           ArchbirdDiscovery *discovery,
                                           const char *path,
                                           size_t path_length) {
  if (!engine || !discovery || (!path && path_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!repository_path_valid(path, path_length))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "inventory path is not canonical and repository-relative");
  return add_discovery_path(engine, discovery, path, path_length);
}

ArchbirdStatus archbird_discovery_add_ignore(
    ArchbirdEngine *engine, ArchbirdDiscovery *discovery, const char *path,
    size_t path_length, const uint8_t *bytes, size_t byte_length) {
  ArchbirdStatus status;
  if (!engine || !discovery || (!path && path_length) ||
      (!bytes && byte_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_ignore_set_add(&discovery->ignores, path, path_length, bytes,
                             byte_length);
  if (status == ARCHBIRD_OK)
    discovery->ignores_ready = 0;
  return status;
}

static int path_below(const AbString *directory, const AbString *path) {
  return path->length > directory->length &&
         path->data[directory->length] == '/' &&
         memcmp(path->data, directory->data, directory->length) == 0;
}

static int exact_input_below(const AbMapConfig *config,
                             const AbString *directory) {
  size_t index;
  for (index = 0; index < config->layer_count; index++) {
    size_t header;
    for (header = 0; header < config->layers[index].public_headers.count;
         header++) {
      if (path_below(directory,
                     &config->layers[index].public_headers.items[header]))
        return 1;
    }
  }
  for (index = 0; index < config->package_count; index++) {
    if (path_below(directory, &config->packages[index].path))
      return 1;
  }
  for (index = 0; index < config->build_count; index++) {
    if (path_below(directory, &config->builds[index].path))
      return 1;
  }
  for (index = 0; index < config->index_count; index++) {
    if (path_below(directory, &config->indexes[index].path))
      return 1;
  }
  for (index = 0; index < config->bridge_count; index++) {
    size_t provider;
    for (provider = 0; provider < config->bridges[index].provider_count;
         provider++) {
      const AbString *path = &config->bridges[index].providers[provider].path;
      if (path->length && path_below(directory, path))
        return 1;
    }
  }
  return 0;
}

static int excluded_subtree(const AbMapConfig *config,
                            const AbString *directory) {
  size_t pattern_index;
  for (pattern_index = 0; pattern_index < config->exclude.count;
       pattern_index++) {
    const AbString *original = &config->exclude.items[pattern_index];
    const char *data = original->data;
    size_t length = original->length;
    AbString base;
    size_t prefix_length;
    if (length >= 2 && data[0] == '.' && data[1] == '/') {
      data += 2;
      length -= 2;
    }
    if (length < 3 || memcmp(data + length - 3, "/**", 3) != 0)
      continue;
    base.data = (char *)data;
    base.length = length - 3;
    while (base.length && base.data[base.length - 1] == '/')
      base.length--;
    prefix_length = directory->length;
    for (;;) {
      AbString prefix = {directory->data, prefix_length};
      if (ab_map_collection_match(&prefix, &base))
        return 1;
      while (prefix_length && directory->data[prefix_length - 1] != '/')
        prefix_length--;
      if (!prefix_length)
        break;
      prefix_length--;
    }
  }
  return 0;
}

ArchbirdStatus archbird_discovery_should_descend(ArchbirdEngine *engine,
                                                 ArchbirdDiscovery *discovery,
                                                 const char *directory,
                                                 size_t directory_length,
                                                 int *out_should_descend) {
  AbString path;
  if (!engine || !discovery || (!directory && directory_length) ||
      !out_should_descend)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!repository_path_valid(directory, directory_length))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "inventory directory is not canonical and repository-relative");
  path.data = (char *)directory;
  path.length = directory_length;
  *out_should_descend = 1;
  if (!discovery->ignores_ready) {
    ArchbirdStatus status = ab_ignore_set_finalize(&discovery->ignores);
    if (status != ARCHBIRD_OK)
      return status;
    discovery->ignores_ready = 1;
  }
  if (ab_ignore_set_matches(&discovery->ignores, &path, 1)) {
    *out_should_descend = 0;
    return ARCHBIRD_OK;
  }
  if (!excluded_subtree(&discovery->config, &path) ||
      exact_input_below(&discovery->config, &path))
    return ARCHBIRD_OK;
  *out_should_descend = 0;
  return ARCHBIRD_OK;
}

static int exact(const AbString *path, const AbString *candidate) {
  return ab_string_equal(path, candidate);
}

static int matches_any(const AbString *path, const AbStringArray *patterns,
                       int collection) {
  size_t index;
  for (index = 0; index < patterns->count; index++) {
    if (collection ? ab_map_collection_match(path, &patterns->items[index])
                   : ab_map_path_match(path, &patterns->items[index]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus public_header_role(ArchbirdEngine *engine,
                                         const AbString *layer,
                                         AbStringArray *roles) {
  AbBuffer role;
  ArchbirdStatus status;
  ab_buffer_init(&role, engine);
  status = ab_buffer_literal(&role, "public-header:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&role, layer->data, layer->length);
  if (status == ARCHBIRD_OK)
    status = append_unique(engine, roles, (const char *)role.data, role.length);
  ab_buffer_free(&role);
  return status;
}

static ArchbirdStatus classify(ArchbirdEngine *engine,
                               const AbMapConfig *config, const AbString *path,
                               DiscoveryRow *row) {
  size_t index;
  int excluded = matches_any(path, &config->exclude, 0);
  int mapped = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(row, 0, sizeof(*row));
  row->path = path;
  for (index = 0; status == ARCHBIRD_OK && index < config->layer_count;
       index++) {
    const AbConfigLayer *layer = &config->layers[index];
    size_t header;
    if (!mapped && !excluded && matches_any(path, &layer->globs, 1)) {
      status = ab_string_copy(engine, &row->layer, layer->name.data,
                              layer->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_string_copy(engine, &row->language, layer->language.data,
                                layer->language.length);
      if (status == ARCHBIRD_OK)
        status = append_unique(engine, &row->roles, "source", 6);
      if (status == ARCHBIRD_OK &&
          ((layer->role.length == 6 &&
            memcmp(layer->role.data, "vendor", 6) == 0) ||
           (layer->role.length == 9 &&
            memcmp(layer->role.data, "generated", 9) == 0)))
        status = append_unique(engine, &row->roles, layer->role.data,
                               layer->role.length);
      mapped = 1;
    }
    for (header = 0;
         status == ARCHBIRD_OK && header < layer->public_headers.count;
         header++) {
      if (!exact(path, &layer->public_headers.items[header]))
        continue;
      status = public_header_role(engine, &layer->name, &row->roles);
      if (status == ARCHBIRD_OK && !row->language.length)
        status = ab_string_copy(engine, &row->language, layer->language.data,
                                layer->language.length);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->package_count;
       index++) {
    if (exact(path, &config->packages[index].path))
      status = append_unique(engine, &row->roles, "manifest", 8);
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->build_count;
       index++) {
    if (exact(path, &config->builds[index].path)) {
      status = append_unique(engine, &row->roles, "build", 5);
      if (status == ARCHBIRD_OK && config->builds[index].kind.length == 16 &&
          !memcmp(config->builds[index].kind.data, "compile_commands", 16))
        status = append_unique(engine, &row->roles, "index", 5);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->index_count;
       index++) {
    if (exact(path, &config->indexes[index].path))
      status = append_unique(engine, &row->roles, "index", 5);
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->artifact_count;
       index++) {
    const AbConfigArtifact *artifact = &config->artifacts[index];
    size_t loader;
    if (!excluded && matches_any(path, &artifact->inputs, 1))
      status = append_unique(engine, &row->roles, "artifact-input", 14);
    for (loader = 0; status == ARCHBIRD_OK && loader < artifact->loader_count;
         loader++) {
      if (!excluded && matches_any(path, &artifact->loaders[loader].paths, 1))
        status = append_unique(engine, &row->roles, "loader", 6);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->bridge_count;
       index++) {
    const AbConfigBridge *bridge = &config->bridges[index];
    size_t provider;
    for (provider = 0;
         status == ARCHBIRD_OK && provider < bridge->provider_count;
         provider++) {
      const AbConfigProvider *spec = &bridge->providers[provider];
      if (spec->path.length && exact(path, &spec->path))
        status = append_unique(engine, &row->roles, "provider", 8);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->test_count;
       index++) {
    const AbConfigTest *test = &config->tests[index];
    if (excluded || !matches_any(path, &test->globs, 1))
      continue;
    status = append_unique(engine, &row->roles, "test", 4);
    if (status == ARCHBIRD_OK && !row->language.length)
      status = ab_string_copy(engine, &row->language, test->language.data,
                              test->language.length);
  }
  for (index = 0; status == ARCHBIRD_OK && index < config->named_entry_count;
       index++) {
    if (!excluded && matches_any(path, &config->named_entries[index].globs, 1))
      status = append_unique(engine, &row->roles, "named-entry", 11);
  }
  if (row->roles.count > 1)
    qsort(row->roles.items, row->roles.count, sizeof(*row->roles.items),
          string_compare);
  return status;
}

static void row_free(ArchbirdEngine *engine, DiscoveryRow *row) {
  ab_string_free(engine, &row->layer);
  ab_string_free(engine, &row->language);
  string_array_free(engine, &row->roles);
}

ArchbirdStatus archbird_discovery_render(ArchbirdEngine *engine,
                                         ArchbirdDiscovery *discovery,
                                         uint32_t json_flags,
                                         ArchbirdWriteFn write_fn,
                                         void *user_data) {
  AbBuffer buffer;
  size_t index;
  int first = 1;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !discovery || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(
      &buffer,
      "{\"artifact\":\"archbird-discovery-plan\",\"configuration_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, discovery->config.sha256_hex, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"files\":[");
  for (index = 0; status == ARCHBIRD_OK && index < discovery->paths.count;
       index++) {
    DiscoveryRow row;
    size_t role;
    status = classify(engine, &discovery->config,
                      &discovery->paths.items[index], &row);
    if (status != ARCHBIRD_OK) {
      row_free(engine, &row);
      break;
    }
    if (!row.roles.count) {
      row_free(engine, &row);
      continue;
    }
    if (!first)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "{\"language\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, row.language.data,
                                     row.language.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",\"layer\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, row.layer.data, row.layer.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, row.path->data, row.path->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ",\"roles\":[");
    for (role = 0; status == ARCHBIRD_OK && role < row.roles.count; role++) {
      if (role)
        status = ab_buffer_literal(&buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(&buffer, row.roles.items[role].data,
                                       row.roles.items[role].length);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, "]}");
    first = 0;
    row_free(engine, &row);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "],\"max_file_bytes\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, (uint64_t)discovery->config.max_file_bytes);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"max_index_bytes\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_u64(&buffer, (uint64_t)discovery->config.max_index_bytes);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, discovery->config.project.data,
                                   discovery->config.project.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"root\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, discovery->config.root.data,
                                   discovery->config.root.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"schema_version\":1}");
  if (status == ARCHBIRD_OK) {
    if (json_flags) {
      status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                          json_flags, write_fn, user_data);
    } else if (write_fn(user_data, buffer.data, buffer.length) != 0) {
      status = engine->error_status != ARCHBIRD_OK
                   ? engine->error_status
                   : archbird_error_set(engine, ARCHBIRD_WRITE_FAILED,
                                        ARCHBIRD_NO_OFFSET,
                                        "JSON output callback failed");
    }
  }
  ab_buffer_free(&buffer);
  return status;
}

void archbird_discovery_destroy(ArchbirdDiscovery *discovery) {
  ArchbirdEngine *engine;
  if (!discovery)
    return;
  engine = discovery->engine;
  ab_map_config_free(engine, &discovery->config);
  string_array_free(engine, &discovery->paths);
  ab_ignore_set_free(&discovery->ignores);
  ab_free(engine, discovery);
}
