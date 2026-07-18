#include "archbird_internal.h"

#include "config.h"
#include "file_facts.h"
#include "map_internal.h"
#include "map_references.h"
#include "project_internal.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

typedef struct ComponentRow {
  const AbConfigComponent *config;
  const AbManifestFile **files;
  size_t file_count;
  size_t symbol_count;
} ComponentRow;

typedef AbMapSymbolReference SymbolReference;
typedef AbMapNamedReference NamedReference;
typedef AbMapEdgeMention EdgeMention;
typedef AbMapCallResolution CallResolutionRow;
typedef AbMapGraph MapGraph;

enum AbMapCallBinding {
  AB_MAP_BINDING_BUILTIN = 1u << 0,
  AB_MAP_BINDING_PROJECT = 1u << 1,
  AB_MAP_BINDING_LOCAL = 1u << 2,
  AB_MAP_BINDING_UNKNOWN = 1u << 3,
  AB_MAP_BINDING_IMPORTED = 1u << 4
};

enum AbMapImportDelimiter {
  AB_MAP_IMPORT_LOCAL = 1u << 0,
  AB_MAP_IMPORT_SYSTEM = 1u << 1,
  AB_MAP_IMPORT_UNKNOWN = 1u << 2
};

#define MAP_TRY(expression)                                                    \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static int bytes_literal(const char *data, size_t length, const char *literal) {
  size_t wanted = strlen(literal);
  return length == wanted &&
         (wanted == 0 || memcmp(data, literal, wanted) == 0);
}

static int string_literal(const AbString *value, const char *literal) {
  return bytes_literal(value->data, value->length, literal);
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

static int fact_bool_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_BOOL && field->value.as.boolean;
}

static unsigned fact_call_binding(const AbFact *fact) {
  const AbString *binding = fact_string_attribute(fact, "binding");
  if (!binding)
    return 0;
  if (string_literal(binding, "builtin"))
    return AB_MAP_BINDING_BUILTIN;
  if (string_literal(binding, "project"))
    return AB_MAP_BINDING_PROJECT;
  if (string_literal(binding, "local"))
    return AB_MAP_BINDING_LOCAL;
  if (string_literal(binding, "imported"))
    return AB_MAP_BINDING_IMPORTED;
  return AB_MAP_BINDING_UNKNOWN;
}

static unsigned fact_import_delimiter(const AbFact *fact) {
  const AbString *delimiter = fact_string_attribute(fact, "delimiter");
  if (!delimiter)
    return AB_MAP_IMPORT_UNKNOWN;
  if (string_literal(delimiter, "local"))
    return AB_MAP_IMPORT_LOCAL;
  if (string_literal(delimiter, "system"))
    return AB_MAP_IMPORT_SYSTEM;
  return AB_MAP_IMPORT_UNKNOWN;
}

static const char *language_family(const AbManifestFile *file,
                                   size_t *out_length) {
  if (!file->has_language) {
    *out_length = 0;
    return "";
  }
  if (string_literal(&file->language, "c") ||
      string_literal(&file->language, "cpp")) {
    *out_length = 1;
    return "c";
  }
  if (string_literal(&file->language, "javascript") ||
      string_literal(&file->language, "typescript") ||
      string_literal(&file->language, "vue")) {
    *out_length = 10;
    return "javascript";
  }
  *out_length = file->language.length;
  return file->language.data;
}

static void symbol_leaf(const AbFact *fact, const char **out_leaf,
                        size_t *out_length) {
  size_t index = fact->name.length;
  while (index && fact->name.data[index - 1] != '.')
    index--;
  *out_leaf = fact->name.data + index;
  *out_length = fact->name.length - index;
}

const AbManifestFile *ab_map_manifest_file(const AbSourceManifest *manifest,
                                           const char *path,
                                           size_t path_length) {
  AbString wanted = {(char *)path, path_length};
  size_t low = 0;
  size_t high = manifest->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&manifest->files[middle].path, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &manifest->files[middle];
  }
  return NULL;
}

static const AbManifestFile *fact_file(const AbSourceManifest *manifest,
                                       const AbFact *fact) {
  return ab_map_manifest_file(manifest, fact->path.data, fact->path.length);
}

static int string_bytes_compare(const char *left, size_t left_length,
                                const char *right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared != 0)
    return compared < 0 ? -1 : 1;
  return (left_length > right_length) - (left_length < right_length);
}

static int symbol_reference_compare(const void *left_raw,
                                    const void *right_raw) {
  const SymbolReference *left = (const SymbolReference *)left_raw;
  const SymbolReference *right = (const SymbolReference *)right_raw;
  size_t left_family_length;
  size_t right_family_length;
  const char *left_family = language_family(left->file, &left_family_length);
  const char *right_family = language_family(right->file, &right_family_length);
  int compared = string_bytes_compare(left_family, left_family_length,
                                      right_family, right_family_length);
  if (compared != 0)
    return compared;
  compared = string_bytes_compare(left->leaf, left->leaf_length, right->leaf,
                                  right->leaf_length);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->file->path, &right->file->path);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->fact->id, &right->fact->id);
}

static int symbol_reference_key_compare(const SymbolReference *symbol,
                                        const char *family,
                                        size_t family_length, const char *leaf,
                                        size_t leaf_length) {
  size_t symbol_family_length;
  const char *symbol_family =
      language_family(symbol->file, &symbol_family_length);
  int compared = string_bytes_compare(symbol_family, symbol_family_length,
                                      family, family_length);
  if (compared != 0)
    return compared;
  return string_bytes_compare(symbol->leaf, symbol->leaf_length, leaf,
                              leaf_length);
}

static void symbol_reference_range(const SymbolReference *symbols,
                                   size_t symbol_count, const char *family,
                                   size_t family_length, const char *leaf,
                                   size_t leaf_length, size_t *out_start,
                                   size_t *out_end) {
  size_t low = 0;
  size_t high = symbol_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (symbol_reference_key_compare(&symbols[middle], family, family_length,
                                     leaf, leaf_length) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = symbol_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (symbol_reference_key_compare(&symbols[middle], family, family_length,
                                     leaf, leaf_length) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

static int named_reference_compare(const void *left_raw,
                                   const void *right_raw) {
  const NamedReference *left = (const NamedReference *)left_raw;
  const NamedReference *right = (const NamedReference *)right_raw;
  int compared = ab_string_compare(&left->file->path, &right->file->path);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(left->name, right->name);
  if (compared != 0)
    return compared;
  if (left->import_delimiter_mask < right->import_delimiter_mask)
    return -1;
  if (left->import_delimiter_mask > right->import_delimiter_mask)
    return 1;
  return 0;
}

static int edge_mention_compare(const void *left_raw, const void *right_raw) {
  const EdgeMention *left = (const EdgeMention *)left_raw;
  const EdgeMention *right = (const EdgeMention *)right_raw;
  int compared = ab_string_compare(&left->kind, &right->kind);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(left->source, right->source);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->target, &right->target);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->name, &right->name);
  if (compared != 0)
    return compared;
  if (left->has_evidence != right->has_evidence)
    return left->has_evidence ? 1 : -1;
  if (!left->has_evidence)
    return 0;
  compared = ab_string_compare(&left->evidence_basis, &right->evidence_basis);
  if (compared != 0)
    return compared;
  compared =
      ab_string_compare(&left->evidence_provider, &right->evidence_provider);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->evidence_state, &right->evidence_state);
}

static int resolution_compare(const void *left_raw, const void *right_raw) {
  const CallResolutionRow *left = (const CallResolutionRow *)left_raw;
  const CallResolutionRow *right = (const CallResolutionRow *)right_raw;
  int compared = ab_string_compare(left->source, right->source);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(left->name, right->name);
  if (compared != 0)
    return compared;
  return strcmp(left->kind, right->kind);
}

void ab_map_graph_free(ArchbirdEngine *engine, MapGraph *graph) {
  size_t index;
  for (index = 0; index < graph->edge_count; index++) {
    ab_string_free(engine, &graph->edges[index].kind);
    ab_string_free(engine, &graph->edges[index].target);
    ab_string_free(engine, &graph->edges[index].name);
    ab_string_free(engine, &graph->edges[index].evidence_basis);
    ab_string_free(engine, &graph->edges[index].evidence_provider);
    ab_string_free(engine, &graph->edges[index].evidence_state);
  }
  ab_free(engine, graph->edges);
  for (index = 0; index < graph->resolution_count; index++) {
    size_t candidate;
    for (candidate = 0; candidate < graph->resolutions[index].candidate_count;
         candidate++)
      ab_string_free(engine, &graph->resolutions[index].candidates[candidate]);
    ab_free(engine, graph->resolutions[index].candidates);
  }
  ab_free(engine, graph->resolutions);
  memset(graph, 0, sizeof(*graph));
}

ArchbirdStatus ab_map_graph_add_edge(ArchbirdEngine *engine, MapGraph *graph,
                                     const char *kind, const AbString *source,
                                     const char *target, size_t target_length,
                                     const AbString *name) {
  return ab_map_graph_add_edge_evidence(engine, graph, kind, source, target,
                                        target_length, name, NULL, NULL, NULL);
}

ArchbirdStatus ab_map_graph_add_edge_evidence(
    ArchbirdEngine *engine, MapGraph *graph, const char *kind,
    const AbString *source, const char *target, size_t target_length,
    const AbString *name, const char *evidence_basis,
    const AbString *evidence_provider, const AbString *evidence_state) {
  EdgeMention *resized;
  EdgeMention *edge;
  ArchbirdStatus status;
  if ((evidence_basis || evidence_provider || evidence_state) &&
      (!evidence_basis || !evidence_provider || !evidence_state))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (graph->edge_count == graph->edge_capacity) {
    size_t capacity = graph->edge_capacity ? graph->edge_capacity * 2 : 64;
    resized = (EdgeMention *)ab_realloc(engine, graph->edges,
                                        capacity * sizeof(*graph->edges));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting map edges");
    graph->edges = resized;
    graph->edge_capacity = capacity;
  }
  edge = &graph->edges[graph->edge_count++];
  memset(edge, 0, sizeof(*edge));
  edge->source = source;
  status = ab_string_copy(engine, &edge->kind, kind, strlen(kind));
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &edge->target, target, target_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &edge->name, name->data, name->length);
  if (status == ARCHBIRD_OK && evidence_basis) {
    status = ab_string_copy(engine, &edge->evidence_basis, evidence_basis,
                            strlen(evidence_basis));
    if (status == ARCHBIRD_OK)
      status =
          ab_string_copy(engine, &edge->evidence_provider,
                         evidence_provider->data, evidence_provider->length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(engine, &edge->evidence_state,
                              evidence_state->data, evidence_state->length);
    if (status == ARCHBIRD_OK)
      edge->has_evidence = 1;
  }
  if (status != ARCHBIRD_OK) {
    ab_string_free(engine, &edge->kind);
    ab_string_free(engine, &edge->target);
    ab_string_free(engine, &edge->name);
    ab_string_free(engine, &edge->evidence_basis);
    ab_string_free(engine, &edge->evidence_provider);
    ab_string_free(engine, &edge->evidence_state);
    graph->edge_count--;
  }
  return status;
}

static ArchbirdStatus
add_resolution(ArchbirdEngine *engine, MapGraph *graph, const AbString *source,
               const AbString *name, const char *kind, size_t count,
               const AbManifestFile **candidates, size_t candidate_count,
               const char *external_target, size_t external_target_length) {
  CallResolutionRow *resized;
  CallResolutionRow *row;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (graph->resolution_count == graph->resolution_capacity) {
    size_t capacity =
        graph->resolution_capacity ? graph->resolution_capacity * 2 : 64;
    resized = (CallResolutionRow *)ab_realloc(
        engine, graph->resolutions, capacity * sizeof(*graph->resolutions));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting call resolutions");
    graph->resolutions = resized;
    graph->resolution_capacity = capacity;
  }
  row = &graph->resolutions[graph->resolution_count++];
  memset(row, 0, sizeof(*row));
  row->source = source;
  row->name = name;
  row->kind = kind;
  row->count = count;
  row->candidate_count = candidate_count + (external_target ? 1 : 0);
  if (row->candidate_count) {
    row->candidates = (AbString *)ab_calloc(engine, row->candidate_count,
                                            sizeof(*row->candidates));
    if (!row->candidates)
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory storing call candidates");
  }
  for (index = 0; status == ARCHBIRD_OK && index < candidate_count; index++)
    status = ab_string_copy(engine, &row->candidates[index],
                            candidates[index]->path.data,
                            candidates[index]->path.length);
  if (status == ARCHBIRD_OK && external_target)
    status = ab_string_copy(engine, &row->candidates[candidate_count],
                            external_target, external_target_length);
  if (status != ARCHBIRD_OK) {
    for (index = 0; index < row->candidate_count; index++)
      ab_string_free(engine, &row->candidates[index]);
    ab_free(engine, row->candidates);
    memset(row, 0, sizeof(*row));
    graph->resolution_count--;
  }
  return status;
}

static ArchbirdStatus collect_symbols(ArchbirdEngine *engine,
                                      const ArchbirdProject *project,
                                      const AbSourceManifest *manifest,
                                      SymbolReference **out_items,
                                      size_t *out_count) {
  size_t total = ab_project_merged_fact_count(project);
  SymbolReference *items = NULL;
  size_t count = 0;
  size_t index;
  *out_items = NULL;
  *out_count = 0;
  if (total) {
    items = (SymbolReference *)ab_malloc(engine, total * sizeof(*items));
    if (!items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing map symbols");
  }
  for (index = 0; index < total; index++) {
    const AbFact *fact = ab_project_merged_fact(project, index);
    const AbManifestFile *file;
    if (!string_literal(&fact->domain, "symbols") || !fact->has_name)
      continue;
    file = fact_file(manifest, fact);
    if (!file || !file->has_layer)
      continue;
    items[count].file = file;
    items[count].fact = fact;
    symbol_leaf(fact, &items[count].leaf, &items[count].leaf_length);
    count++;
  }
  if (count > 1)
    qsort(items, count, sizeof(*items), symbol_reference_compare);
  *out_items = items;
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
collect_named_domain(ArchbirdEngine *engine, const ArchbirdProject *project,
                     const AbSourceManifest *manifest, const char *domain,
                     NamedReference **out_items, size_t *out_count) {
  size_t total = ab_project_merged_fact_count(project);
  NamedReference *items = NULL;
  size_t count = 0;
  size_t index;
  size_t write_index = 0;
  *out_items = NULL;
  *out_count = 0;
  if (total) {
    items = (NamedReference *)ab_malloc(engine, total * sizeof(*items));
    if (!items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing named facts");
  }
  for (index = 0; index < total; index++) {
    const AbFact *fact = ab_project_merged_fact(project, index);
    const AbManifestFile *file;
    if (!string_literal(&fact->domain, domain) || !fact->has_name ||
        !fact->name.length)
      continue;
    file = fact_file(manifest, fact);
    if (!file || !file->has_layer)
      continue;
    items[count].file = file;
    items[count].name = &fact->name;
    items[count].count = 1;
    items[count].builtin = fact_bool_attribute(fact, "stdlib");
    items[count].binding_mask = fact_call_binding(fact);
    items[count].import_delimiter_mask = fact_import_delimiter(fact);
    count++;
  }
  if (count > 1)
    qsort(items, count, sizeof(*items), named_reference_compare);
  for (index = 0; index < count; index++) {
    if (write_index > 0 &&
        ab_string_equal(&items[write_index - 1].file->path,
                        &items[index].file->path) &&
        ab_string_equal(items[write_index - 1].name, items[index].name) &&
        items[write_index - 1].import_delimiter_mask ==
            items[index].import_delimiter_mask) {
      items[write_index - 1].count++;
      items[write_index - 1].builtin =
          items[write_index - 1].builtin || items[index].builtin;
      items[write_index - 1].binding_mask |= items[index].binding_mask;
      items[write_index - 1].import_delimiter_mask |=
          items[index].import_delimiter_mask;
      continue;
    }
    if (write_index != index)
      items[write_index] = items[index];
    write_index++;
  }
  *out_items = items;
  *out_count = write_index;
  return ARCHBIRD_OK;
}

static int normalized_path(const char *input, size_t input_length, char *out,
                           size_t *out_length) {
  size_t input_index = 0;
  size_t write = 0;
  while (input_index < input_length) {
    size_t start;
    size_t length;
    while (input_index < input_length && input[input_index] == '/')
      input_index++;
    start = input_index;
    while (input_index < input_length && input[input_index] != '/')
      input_index++;
    length = input_index - start;
    if (!length || (length == 1 && input[start] == '.'))
      continue;
    if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
      if (!write)
        return 0;
      while (write && out[write - 1] != '/')
        write--;
      if (write)
        write--;
      continue;
    }
    if (write)
      out[write++] = '/';
    memcpy(out + write, input + start, length);
    write += length;
  }
  out[write] = '\0';
  *out_length = write;
  return write > 0;
}

static ArchbirdStatus candidate_file(ArchbirdEngine *engine,
                                     const AbSourceManifest *manifest,
                                     const char *path, size_t path_length,
                                     const char *const *suffixes,
                                     size_t suffix_count,
                                     const AbManifestFile **out) {
  char *candidate;
  size_t capacity = path_length + 32;
  size_t normalized_length;
  size_t index;
  size_t effective_suffix_count = suffix_count;
  const AbManifestFile *found = NULL;
  *out = NULL;
  if (path_length > SIZE_MAX - 32)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "import candidate path is too large");
  candidate = (char *)ab_malloc(engine, capacity);
  if (!candidate)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory resolving import path");
  if (!normalized_path(path, path_length, candidate, &normalized_length)) {
    ab_free(engine, candidate);
    return ARCHBIRD_OK;
  }
  for (index = 1; index < suffix_count; index++) {
    size_t suffix_length = strlen(suffixes[index]);
    if (suffixes[index][0] == '.' && normalized_length >= suffix_length &&
        memcmp(candidate + normalized_length - suffix_length, suffixes[index],
               suffix_length) == 0) {
      effective_suffix_count = 1;
      break;
    }
  }
  for (index = 0; index < effective_suffix_count; index++) {
    size_t suffix_length = strlen(suffixes[index]);
    if (normalized_length + suffix_length + 1 > capacity)
      continue;
    memcpy(candidate + normalized_length, suffixes[index], suffix_length);
    candidate[normalized_length + suffix_length] = '\0';
    found = ab_map_manifest_file(manifest, candidate,
                                 normalized_length + suffix_length);
    if (found)
      break;
  }
  ab_free(engine, candidate);
  *out = found;
  return ARCHBIRD_OK;
}

static ArchbirdStatus exact_candidate_file(ArchbirdEngine *engine,
                                           const AbSourceManifest *manifest,
                                           const char *path, size_t path_length,
                                           const AbManifestFile **out) {
  char *candidate;
  size_t normalized_length;
  *out = NULL;
  if (path_length == SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "import candidate path is too large");
  candidate = (char *)ab_malloc(engine, path_length + 1);
  if (!candidate)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory resolving exact import path");
  if (normalized_path(path, path_length, candidate, &normalized_length))
    *out = ab_map_manifest_file(manifest, candidate, normalized_length);
  ab_free(engine, candidate);
  return ARCHBIRD_OK;
}

static size_t path_directory_length(const AbString *path) {
  size_t index = path->length;
  while (index && path->data[index - 1] != '/')
    index--;
  return index ? index - 1 : 0;
}

static ArchbirdStatus
resolve_joined_import(ArchbirdEngine *engine, const AbSourceManifest *manifest,
                      const char *prefix, size_t prefix_length,
                      const AbString *imported, const AbManifestFile **out) {
  char *joined;
  size_t joined_length;
  ArchbirdStatus status;
  *out = NULL;
  if (prefix_length > SIZE_MAX - imported->length - 2)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "import candidate path is too large");
  joined_length = prefix_length + (prefix_length ? 1 : 0) + imported->length;
  joined = (char *)ab_malloc(engine, joined_length + 1);
  if (!joined)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory resolving import path");
  if (prefix_length)
    memcpy(joined, prefix, prefix_length);
  if (prefix_length)
    joined[prefix_length] = '/';
  if (imported->length)
    memcpy(joined + prefix_length + (prefix_length ? 1 : 0), imported->data,
           imported->length);
  status = exact_candidate_file(engine, manifest, joined, joined_length, out);
  ab_free(engine, joined);
  return status;
}

static const AbConfigLayer *source_config_layer(const AbMapConfig *config,
                                                const AbManifestFile *file) {
  size_t layer_index;
  if (!file->has_layer)
    return NULL;
  for (layer_index = 0; layer_index < config->layer_count; layer_index++) {
    if (ab_string_equal(&config->layers[layer_index].name, &file->layer))
      return &config->layers[layer_index];
  }
  return NULL;
}

static int absolute_c_include(const AbString *imported) {
  if (!imported->length)
    return 0;
  if (imported->data[0] == '/' || imported->data[0] == '\\')
    return 1;
  return imported->length >= 3 &&
         ((imported->data[0] >= 'A' && imported->data[0] <= 'Z') ||
          (imported->data[0] >= 'a' && imported->data[0] <= 'z')) &&
         imported->data[1] == ':' &&
         (imported->data[2] == '/' || imported->data[2] == '\\');
}

static ArchbirdStatus
resolve_c_import(ArchbirdEngine *engine, const AbSourceManifest *manifest,
                 const AbMapConfig *config, const AbManifestFile *file,
                 const AbString *imported, unsigned delimiter_mask,
                 const AbManifestFile **out) {
  size_t base_length = path_directory_length(&file->path);
  const AbConfigLayer *layer;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out = NULL;
  if (!imported->length || absolute_c_include(imported))
    return ARCHBIRD_OK;
  if (!delimiter_mask)
    delimiter_mask = AB_MAP_IMPORT_UNKNOWN;
  if (delimiter_mask & (AB_MAP_IMPORT_LOCAL | AB_MAP_IMPORT_UNKNOWN)) {
    status = resolve_joined_import(engine, manifest, file->path.data,
                                   base_length, imported, out);
    if (status != ARCHBIRD_OK || *out)
      return status;
  }
  layer = source_config_layer(config, file);
  if (layer && (string_literal(&layer->language, "c") ||
                string_literal(&layer->language, "cpp"))) {
    size_t root_index;
    for (root_index = 0; root_index < layer->import_roots.count; root_index++) {
      const AbString *root = &layer->import_roots.items[root_index];
      status = resolve_joined_import(engine, manifest, root->data, root->length,
                                     imported, out);
      if (status != ARCHBIRD_OK || *out)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
resolve_import(ArchbirdEngine *engine, const AbSourceManifest *manifest,
               const AbMapConfig *config, const AbManifestFile *file,
               const AbString *imported, unsigned delimiter_mask,
               const AbManifestFile **out) {
  size_t family_length;
  const char *family = language_family(file, &family_length);
  if (bytes_literal(family, family_length, "c"))
    return resolve_c_import(engine, manifest, config, file, imported,
                            delimiter_mask, out);
  return ab_map_resolve_import(engine, manifest, config, file, imported, out);
}

ArchbirdStatus
ab_map_resolve_import(ArchbirdEngine *engine, const AbSourceManifest *manifest,
                      const AbMapConfig *config, const AbManifestFile *file,
                      const AbString *imported, const AbManifestFile **out) {
  size_t family_length;
  const char *family = language_family(file, &family_length);
  size_t base_length = path_directory_length(&file->path);
  char *joined;
  size_t joined_length;
  const AbManifestFile *target = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out = NULL;
  if (bytes_literal(family, family_length, "javascript")) {
    static const char *const suffixes[] = {
        "",           ".js",        ".mjs",      ".cjs",
        ".ts",        ".tsx",       ".vue",      "/index.js",
        "/index.mjs", "/index.cjs", "/index.ts", "/index.tsx"};
    size_t clean_length = imported->length;
    size_t index;
    if (!imported->length || imported->data[0] != '.')
      return ARCHBIRD_OK;
    for (index = 0; index < imported->length; index++) {
      if (imported->data[index] == '?' || imported->data[index] == '#') {
        clean_length = index;
        break;
      }
    }
    joined_length = base_length + (base_length ? 1 : 0) + clean_length;
    joined = (char *)ab_malloc(engine, joined_length + 1);
    if (!joined)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory resolving JavaScript import");
    if (base_length)
      memcpy(joined, file->path.data, base_length);
    if (base_length)
      joined[base_length] = '/';
    memcpy(joined + base_length + (base_length ? 1 : 0), imported->data,
           clean_length);
    status = candidate_file(engine, manifest, joined, joined_length, suffixes,
                            sizeof(suffixes) / sizeof(suffixes[0]), &target);
    ab_free(engine, joined);
    *out = target;
    return status;
  }
  if (!bytes_literal(family, family_length, "python"))
    return ARCHBIRD_OK;
  if (imported->length && imported->data[0] == '.') {
    static const char *const suffixes[] = {"", ".py", "/__init__.py"};
    size_t dots = 0;
    size_t parent_length = base_length;
    size_t index;
    while (dots < imported->length && imported->data[dots] == '.')
      dots++;
    for (index = 1; index < dots; index++) {
      while (parent_length && file->path.data[parent_length - 1] != '/')
        parent_length--;
      if (parent_length)
        parent_length--;
    }
    joined_length = parent_length + (parent_length && dots < imported->length) +
                    (imported->length - dots);
    joined = (char *)ab_malloc(engine, joined_length + 1);
    if (!joined)
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory resolving relative Python import");
    if (parent_length)
      memcpy(joined, file->path.data, parent_length);
    index = parent_length;
    if (parent_length && dots < imported->length)
      joined[index++] = '/';
    while (dots < imported->length) {
      joined[index++] =
          imported->data[dots] == '.' ? '/' : imported->data[dots];
      dots++;
    }
    status = candidate_file(engine, manifest, joined, joined_length, suffixes,
                            sizeof(suffixes) / sizeof(suffixes[0]), &target);
    ab_free(engine, joined);
    *out = target;
    return status;
  }
  {
    static const char *const suffixes[] = {"", ".py", "/__init__.py"};
    size_t layer_index;
    for (layer_index = 0; layer_index < config->layer_count; layer_index++) {
      const AbConfigLayer *layer = &config->layers[layer_index];
      size_t root_index;
      if (!string_literal(&layer->language, "python"))
        continue;
      for (root_index = 0; root_index < layer->import_roots.count;
           root_index++) {
        const AbString *root = &layer->import_roots.items[root_index];
        size_t index;
        joined_length =
            root->length + (root->length ? 1 : 0) + imported->length;
        joined = (char *)ab_malloc(engine, joined_length + 1);
        if (!joined)
          return archbird_error_set(
              engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
              "out of memory resolving Python import root");
        memcpy(joined, root->data, root->length);
        index = root->length;
        if (root->length)
          joined[index++] = '/';
        for (base_length = 0; base_length < imported->length; base_length++)
          joined[index++] = imported->data[base_length] == '.'
                                ? '/'
                                : imported->data[base_length];
        status =
            candidate_file(engine, manifest, joined, joined_length, suffixes,
                           sizeof(suffixes) / sizeof(suffixes[0]), &target);
        ab_free(engine, joined);
        if (status != ARCHBIRD_OK || target) {
          *out = target;
          return status;
        }
      }
    }
  }
  return ARCHBIRD_OK;
}

static int name_in_list(const AbString *name, const char *const *items,
                        size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (string_literal(name, items[index]))
      return 1;
  }
  return 0;
}

static int python_builtin_call(const AbString *name) {
  static const char *const names[] = {
      "__import__", "abs",          "all",         "any",        "ascii",
      "bin",        "bool",         "breakpoint",  "bytearray",  "bytes",
      "callable",   "chr",          "classmethod", "compile",    "complex",
      "delattr",    "dict",         "dir",         "divmod",     "enumerate",
      "eval",       "exec",         "filter",      "float",      "format",
      "frozenset",  "getattr",      "globals",     "hasattr",    "hash",
      "help",       "hex",          "id",          "input",      "int",
      "isinstance", "issubclass",   "iter",        "len",        "list",
      "locals",     "map",          "max",         "memoryview", "min",
      "next",       "object",       "oct",         "open",       "ord",
      "pow",        "print",        "property",    "range",      "repr",
      "reversed",   "round",        "set",         "setattr",    "slice",
      "sorted",     "staticmethod", "str",         "sum",        "super",
      "tuple",      "type",         "vars",        "zip"};
  return name_in_list(name, names, sizeof(names) / sizeof(names[0]));
}

static int python_stdlib(const char *name, size_t length) {
  static const char *const names[] = {"__future__",
                                      "abc",
                                      "argparse",
                                      "array",
                                      "ast",
                                      "asyncio",
                                      "atexit",
                                      "base64",
                                      "binascii",
                                      "bisect",
                                      "builtins",
                                      "bz2",
                                      "calendar",
                                      "cmath",
                                      "collections",
                                      "concurrent",
                                      "configparser",
                                      "contextlib",
                                      "contextvars",
                                      "copy",
                                      "csv",
                                      "ctypes",
                                      "dataclasses",
                                      "datetime",
                                      "decimal",
                                      "difflib",
                                      "dis",
                                      "email",
                                      "enum",
                                      "errno",
                                      "fcntl",
                                      "fnmatch",
                                      "fractions",
                                      "functools",
                                      "gc",
                                      "getopt",
                                      "getpass",
                                      "glob",
                                      "graphlib",
                                      "gzip",
                                      "hashlib",
                                      "heapq",
                                      "hmac",
                                      "html",
                                      "http",
                                      "importlib",
                                      "inspect",
                                      "io",
                                      "ipaddress",
                                      "itertools",
                                      "json",
                                      "keyword",
                                      "linecache",
                                      "locale",
                                      "logging",
                                      "lzma",
                                      "marshal",
                                      "math",
                                      "mimetypes",
                                      "mmap",
                                      "multiprocessing",
                                      "numbers",
                                      "operator",
                                      "os",
                                      "pathlib",
                                      "pickle",
                                      "pkgutil",
                                      "platform",
                                      "pprint",
                                      "queue",
                                      "random",
                                      "re",
                                      "readline",
                                      "resource",
                                      "runpy",
                                      "secrets",
                                      "select",
                                      "selectors",
                                      "shlex",
                                      "shutil",
                                      "signal",
                                      "socket",
                                      "sqlite3",
                                      "ssl",
                                      "stat",
                                      "statistics",
                                      "string",
                                      "struct",
                                      "subprocess",
                                      "sys",
                                      "sysconfig",
                                      "tempfile",
                                      "textwrap",
                                      "threading",
                                      "time",
                                      "token",
                                      "tokenize",
                                      "traceback",
                                      "types",
                                      "typing",
                                      "unicodedata",
                                      "unittest",
                                      "urllib",
                                      "uuid",
                                      "venv",
                                      "warnings",
                                      "weakref",
                                      "xml",
                                      "zipfile",
                                      "zlib"};
  size_t index;
  for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
    if (bytes_literal(name, length, names[index]))
      return 1;
  }
  return 0;
}

static int node_builtin(const char *name, size_t length) {
  static const char *const names[] = {
      "assert", "buffer",         "child_process",
      "crypto", "events",         "fs",
      "http",   "https",          "os",
      "path",   "stream",         "url",
      "util",   "worker_threads", "zlib"};
  size_t index;
  if (length > 5 && memcmp(name, "node:", 5) == 0) {
    name += 5;
    length -= 5;
  }
  for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
    if (bytes_literal(name, length, names[index]))
      return 1;
  }
  return 0;
}

static const AbExternalNamespace *external_namespace(const AbMapConfig *config,
                                                     const AbManifestFile *file,
                                                     const AbString *name) {
  const AbConfigLayer *layer =
      file->has_layer ? ab_map_config_layer(config, &file->layer) : NULL;
  const AbExternalNamespace *selected = NULL;
  size_t index;
  if (!layer)
    return NULL;
  for (index = 0; index < layer->external_namespace_count; index++) {
    const AbExternalNamespace *candidate = &layer->external_namespaces[index];
    if (candidate->prefix.length <= name->length &&
        memcmp(candidate->prefix.data, name->data, candidate->prefix.length) ==
            0 &&
        (!selected || candidate->prefix.length > selected->prefix.length))
      selected = candidate;
  }
  return selected;
}

static ArchbirdStatus package_target(ArchbirdEngine *engine,
                                     const AbString *package, char **out,
                                     size_t *out_length) {
  size_t length = 8 + package->length;
  char *target = (char *)ab_malloc(engine, length + 1);
  if (!target)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating package target");
  memcpy(target, "package:", 8);
  memcpy(target + 8, package->data, package->length);
  target[length] = '\0';
  *out = target;
  *out_length = length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
external_import_target(ArchbirdEngine *engine, const AbManifestFile *file,
                       const AbString *imported, const AbMapPackage *packages,
                       size_t package_count, char **out, size_t *out_length,
                       int *out_builtin, int *out_local) {
  size_t family_length;
  const char *family = language_family(file, &family_length);
  const char *start = imported->data;
  size_t length = imported->length;
  size_t name_length;
  AbString package;
  const AbMapPackage *matched = NULL;
  const char *manager;
  size_t matches = 0;
  size_t package_index;
  *out = NULL;
  *out_length = 0;
  *out_builtin = 0;
  *out_local = 0;
  if (bytes_literal(family, family_length, "python")) {
    while (length && *start == '.') {
      start++;
      length--;
    }
    name_length = 0;
    while (name_length < length && start[name_length] != '.')
      name_length++;
    if (!name_length)
      return ARCHBIRD_OK;
    if (python_stdlib(start, name_length)) {
      *out_builtin = 1;
      return ARCHBIRD_OK;
    }
  } else if (bytes_literal(family, family_length, "javascript")) {
    if (length > 5 && memcmp(start, "node:", 5) == 0) {
      *out_builtin = 1;
      return ARCHBIRD_OK;
    }
    if (length && start[0] == '@') {
      size_t slash = 0;
      while (slash < length && start[slash] != '/')
        slash++;
      if (slash < length) {
        slash++;
        while (slash < length && start[slash] != '/')
          slash++;
      }
      name_length = slash;
    } else {
      name_length = 0;
      while (name_length < length && start[name_length] != '/')
        name_length++;
    }
    if (!name_length)
      return ARCHBIRD_OK;
    if (node_builtin(start, name_length)) {
      *out_builtin = 1;
      return ARCHBIRD_OK;
    }
  } else {
    return ARCHBIRD_OK;
  }
  package.data = (char *)start;
  package.length = name_length;
  manager = ab_map_language_manager(&file->language);
  for (package_index = 0; package_index < package_count; package_index++) {
    const AbMapPackage *candidate = &packages[package_index];
    if (strcmp(ab_map_package_manager(&candidate->kind), manager) != 0 ||
        !ab_map_package_alias_matches(candidate, manager, &package))
      continue;
    matched = candidate;
    matches++;
  }
  if (matches == 1) {
    package = matched->name;
    *out_local = 1;
  }
  return package_target(engine, &package, out, out_length);
}

static ArchbirdStatus definition_candidates(
    ArchbirdEngine *engine, const SymbolReference *symbols, size_t symbol_count,
    const AbManifestFile *source, const AbString *name, int free_call,
    const AbManifestFile ***out_candidates, size_t *out_count) {
  const AbManifestFile **candidates = NULL;
  size_t count = 0;
  size_t index;
  size_t start;
  size_t end;
  size_t family_length;
  const char *family = language_family(source, &family_length);
  *out_candidates = NULL;
  *out_count = 0;
  symbol_reference_range(symbols, symbol_count, family, family_length,
                         name->data, name->length, &start, &end);
  if (end > start) {
    candidates = (const AbManifestFile **)ab_malloc(
        engine, (end - start) * sizeof(*candidates));
    if (!candidates)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory resolving call candidates");
  }
  for (index = start; index < end; index++) {
    const SymbolReference *symbol = &symbols[index];
    const AbString *scope;
    if (!bytes_literal(symbol->fact->kind.data, symbol->fact->kind.length,
                       "declaration")) {
      /* A receiver method and a free call merely sharing a leaf name are not
       * evidence of a call target. Precise providers may eventually resolve a
       * receiver occurrence, but the conservative file graph must not turn
       * that lexical coincidence into a unique cross-file edge. Classes and
       * module/nested functions remain callable free-name candidates. */
      if (free_call && bytes_literal(symbol->fact->kind.data,
                                     symbol->fact->kind.length, "method"))
        continue;
      scope = fact_string_attribute(symbol->fact, "scope");
      if (scope && string_literal(scope, "local") && symbol->file != source)
        continue;
      if (count && candidates[count - 1] == symbol->file)
        continue;
      candidates[count++] = symbol->file;
    }
  }
  *out_candidates = candidates;
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus build_graph(ArchbirdEngine *engine,
                                  const ArchbirdProject *project,
                                  const AbSourceManifest *manifest,
                                  const AbMapConfig *config,
                                  const AbMapPackage *packages,
                                  size_t package_count, MapGraph *graph) {
  SymbolReference *symbols = NULL;
  size_t symbol_count = 0;
  NamedReference *calls = NULL;
  size_t call_count = 0;
  NamedReference *methods = NULL;
  size_t method_count = 0;
  NamedReference *imports = NULL;
  size_t import_count = 0;
  size_t index;
  ArchbirdStatus status =
      collect_symbols(engine, project, manifest, &symbols, &symbol_count);
  if (status == ARCHBIRD_OK)
    status = collect_named_domain(engine, project, manifest, "calls", &calls,
                                  &call_count);
  if (status == ARCHBIRD_OK)
    status = collect_named_domain(engine, project, manifest, "method-calls",
                                  &methods, &method_count);
  if (status == ARCHBIRD_OK)
    status = collect_named_domain(engine, project, manifest, "imports",
                                  &imports, &import_count);
  for (index = 0; status == ARCHBIRD_OK && index < call_count; index++) {
    const AbManifestFile **candidates = NULL;
    size_t candidate_count = 0;
    const AbExternalNamespace *external = NULL;
    const char *kind;
    char *external_target = NULL;
    size_t external_target_length = 0;
    size_t family_length;
    const char *family = language_family(calls[index].file, &family_length);
    int python = bytes_literal(family, family_length, "python");
    int builtin_name = python && python_builtin_call(calls[index].name);
    unsigned binding = calls[index].binding_mask;
    status = definition_candidates(engine, symbols, symbol_count,
                                   calls[index].file, calls[index].name, 1,
                                   &candidates, &candidate_count);
    if (status != ARCHBIRD_OK)
      break;
    if (binding == AB_MAP_BINDING_BUILTIN) {
      kind = "builtin";
    } else if (binding & (AB_MAP_BINDING_BUILTIN | AB_MAP_BINDING_LOCAL |
                          AB_MAP_BINDING_UNKNOWN)) {
      /* Aggregated occurrences with local, unknown, or mixed bindings cannot
       * justify one file-level target. Keep candidates as inspection evidence
       * but never emit a unique edge. */
      kind = "unresolved";
    } else if (builtin_name && binding == 0 && candidate_count) {
      /* The portable lexical provider does not claim Python binding. A
       * same-leaf project symbol is therefore insufficient to displace a
       * builtin; preserve uncertainty rather than guessing either way. */
      kind = "unresolved";
    } else if (!candidate_count && builtin_name) {
      kind = "builtin";
    } else if (!candidate_count &&
               (external = external_namespace(config, calls[index].file,
                                              calls[index].name)) != NULL) {
      kind = "external";
      status = package_target(engine, &external->package, &external_target,
                              &external_target_length);
    } else if (binding & AB_MAP_BINDING_IMPORTED) {
      kind = candidate_count == 1
                 ? "candidate"
                 : (candidate_count > 1 ? "ambiguous" : "unresolved");
    } else if (candidate_count == 1) {
      /* A same-leaf definition in another file is bounded navigation
       * evidence, not a proven call target.  Exact cross-file relations come
       * from provider reference-targets or proven import/name-use facts. */
      kind = candidates[0] == calls[index].file ? "unique" : "candidate";
    } else if (candidate_count > 1) {
      kind = "ambiguous";
    } else {
      kind = "unresolved";
    }
    if (status == ARCHBIRD_OK)
      status = add_resolution(engine, graph, &calls[index].file->path,
                              calls[index].name, kind, calls[index].count,
                              candidates, candidate_count, external_target,
                              external_target_length);
    if (status == ARCHBIRD_OK &&
        ((!strcmp(kind, "unique") && candidate_count == 1 &&
          candidates[0] != calls[index].file) ||
         external_target)) {
      const char *target =
          external_target ? external_target : candidates[0]->path.data;
      size_t target_length =
          external_target ? external_target_length : candidates[0]->path.length;
      status = ab_map_graph_add_edge(
          engine, graph, external_target ? "external-call" : "call",
          &calls[index].file->path, target, target_length, calls[index].name);
    }
    ab_free(engine, external_target);
    ab_free(engine, candidates);
  }
  for (index = 0; status == ARCHBIRD_OK && index < method_count; index++)
    status = add_resolution(engine, graph, &methods[index].file->path,
                            methods[index].name, "method", methods[index].count,
                            NULL, 0, NULL, 0);
  for (index = 0; status == ARCHBIRD_OK && index < import_count; index++) {
    const AbManifestFile *target = NULL;
    status = resolve_import(engine, manifest, config, imports[index].file,
                            imports[index].name,
                            imports[index].import_delimiter_mask, &target);
    if (status != ARCHBIRD_OK)
      break;
    if (target && target != imports[index].file) {
      status = ab_map_graph_add_edge(
          engine, graph, "import", &imports[index].file->path,
          target->path.data, target->path.length, imports[index].name);
    } else if (!imports[index].name->length ||
               imports[index].name->data[0] != '.') {
      char *external_target = NULL;
      size_t external_length = 0;
      int builtin = 0;
      int package_local = 0;
      if (imports[index].builtin) {
        builtin = 1;
      } else {
        status = external_import_target(
            engine, imports[index].file, imports[index].name, packages,
            package_count, &external_target, &external_length, &builtin,
            &package_local);
      }
      if (status == ARCHBIRD_OK && external_target)
        status = ab_map_graph_add_edge(
            engine, graph, package_local ? "package-local" : "external",
            &imports[index].file->path, external_target, external_length,
            imports[index].name);
      ab_free(engine, external_target);
      (void)builtin;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < symbol_count; index++) {
    const AbManifestFile **candidates = NULL;
    size_t candidate_count = 0;
    AbString leaf;
    if (!string_literal(&symbols[index].fact->kind, "declaration"))
      continue;
    leaf.data = (char *)symbols[index].leaf;
    leaf.length = symbols[index].leaf_length;
    status = definition_candidates(engine, symbols, symbol_count,
                                   symbols[index].file, &leaf, 0, &candidates,
                                   &candidate_count);
    if (status == ARCHBIRD_OK && candidate_count == 1 &&
        candidates[0] != symbols[index].file)
      status = ab_map_graph_add_edge(
          engine, graph, "declaration", &symbols[index].file->path,
          candidates[0]->path.data, candidates[0]->path.length, &leaf);
    ab_free(engine, candidates);
  }
  if (status == ARCHBIRD_OK && graph->edge_count > 1)
    qsort(graph->edges, graph->edge_count, sizeof(*graph->edges),
          edge_mention_compare);
  if (status == ARCHBIRD_OK && graph->resolution_count > 1)
    qsort(graph->resolutions, graph->resolution_count,
          sizeof(*graph->resolutions), resolution_compare);
  ab_free(engine, imports);
  ab_free(engine, methods);
  ab_free(engine, calls);
  ab_free(engine, symbols);
  if (status != ARCHBIRD_OK)
    ab_map_graph_free(engine, graph);
  return status;
}

void ab_map_graph_sort(AbMapGraph *graph) {
  if (!graph)
    return;
  if (graph->edge_count > 1)
    qsort(graph->edges, graph->edge_count, sizeof(*graph->edges),
          edge_mention_compare);
  if (graph->resolution_count > 1)
    qsort(graph->resolutions, graph->resolution_count,
          sizeof(*graph->resolutions), resolution_compare);
}

static int same_edge_key(const EdgeMention *left, const EdgeMention *right) {
  return ab_string_equal(&left->kind, &right->kind) &&
         ab_string_equal(left->source, right->source) &&
         ab_string_equal(&left->target, &right->target);
}

static int same_edge_evidence(const EdgeMention *left,
                              const EdgeMention *right) {
  return left->has_evidence && right->has_evidence &&
         ab_string_equal(&left->evidence_basis, &right->evidence_basis) &&
         ab_string_equal(&left->evidence_provider, &right->evidence_provider) &&
         ab_string_equal(&left->evidence_state, &right->evidence_state);
}

static ArchbirdStatus render_edges(AbBuffer *buffer, const MapGraph *graph) {
  size_t index = 0;
  int first_edge = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  while (status == ARCHBIRD_OK && index < graph->edge_count) {
    size_t end = index + 1;
    size_t name_index;
    size_t evidence_index;
    const AbString *previous_name = NULL;
    int has_evidence = graph->edges[index].has_evidence;
    while (end < graph->edge_count &&
           same_edge_key(&graph->edges[index], &graph->edges[end])) {
      has_evidence = has_evidence || graph->edges[end].has_evidence;
      end++;
    }
    if (!first_edge)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{");
    if (status == ARCHBIRD_OK && has_evidence)
      status = ab_buffer_literal(buffer, "\"evidence\":[");
    if (has_evidence) {
      int first_evidence = 1;
      for (evidence_index = index;
           status == ARCHBIRD_OK && evidence_index < end; evidence_index++) {
        const EdgeMention *evidence = &graph->edges[evidence_index];
        size_t previous;
        int duplicate = 0;
        if (!evidence->has_evidence)
          continue;
        for (previous = index; previous < evidence_index; previous++)
          if (same_edge_evidence(evidence, &graph->edges[previous])) {
            duplicate = 1;
            break;
          }
        if (duplicate)
          continue;
        if (!first_evidence)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "{\"basis\":");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &evidence->evidence_basis);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ",\"provider\":");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &evidence->evidence_provider);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ",\"state\":");
        if (status == ARCHBIRD_OK)
          status = json_string(buffer, &evidence->evidence_state);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "}");
        first_evidence = 0;
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "],");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\"kind\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &graph->edges[index].kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"names\":[");
    for (name_index = index; status == ARCHBIRD_OK && name_index < end;
         name_index++) {
      const AbString *name = &graph->edges[name_index].name;
      if (previous_name && ab_string_equal(previous_name, name))
        continue;
      if (previous_name)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, name);
      previous_name = name;
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"source\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, graph->edges[index].source);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &graph->edges[index].target);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first_edge = 0;
    index = end;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_resolutions(AbBuffer *buffer,
                                         const MapGraph *graph) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < graph->resolution_count;
       index++) {
    const CallResolutionRow *row = &graph->resolutions[index];
    size_t candidate;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"candidates\":[");
    for (candidate = 0;
         status == ARCHBIRD_OK && candidate < row->candidate_count;
         candidate++) {
      if (candidate)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &row->candidates[candidate]);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"kind\":\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, row->kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\",\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, row->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, row->source);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int layer_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbConfigLayer *left = *(const AbConfigLayer *const *)left_raw;
  const AbConfigLayer *right = *(const AbConfigLayer *const *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int component_pointer_compare(const void *left_raw,
                                     const void *right_raw) {
  const ComponentRow *left = (const ComponentRow *)left_raw;
  const ComponentRow *right = (const ComponentRow *)right_raw;
  return ab_string_compare(&left->config->name, &right->config->name);
}

static ArchbirdStatus render_layers(AbBuffer *buffer, ArchbirdEngine *engine,
                                    const ArchbirdProject *project,
                                    const AbMapConfig *config,
                                    const AbSourceManifest *manifest) {
  const AbConfigLayer **layers = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (config->layer_count) {
    layers = (const AbConfigLayer **)ab_malloc(engine, config->layer_count *
                                                           sizeof(*layers));
    if (!layers)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory rendering map layers");
  }
  for (index = 0; index < config->layer_count; index++)
    layers[index] = &config->layers[index];
  if (config->layer_count > 1)
    qsort(layers, config->layer_count, sizeof(*layers), layer_pointer_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < config->layer_count;
       index++) {
    size_t file_index;
    size_t file_count = 0;
    size_t symbol_count = 0;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < manifest->file_count;
         file_index++) {
      size_t count = 0;
      const AbManifestFile *file = &manifest->files[file_index];
      if (!file->has_layer ||
          !ab_string_equal(&file->layer, &layers[index]->name))
        continue;
      file_count++;
      status = ab_file_symbol_count(engine, project, file, &count);
      symbol_count += count;
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"files\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, file_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"language\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &layers[index]->language);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &layers[index]->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"role\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &layers[index]->role);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"symbols\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, symbol_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(engine, layers);
  return status;
}

static void components_free(ArchbirdEngine *engine, ComponentRow *rows,
                            size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_free(engine, rows[index].files);
  ab_free(engine, rows);
}

static int component_contains(const ComponentRow *row, const AbString *path) {
  size_t low = 0;
  size_t high = row->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&row->files[middle]->path, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *left = *(const AbString *const *)left_raw;
  const AbString *right = *(const AbString *const *)right_raw;
  return ab_string_compare(left, right);
}

static ArchbirdStatus
render_component_outgoing(AbBuffer *buffer, ArchbirdEngine *engine,
                          const ComponentRow *rows, size_t row_count,
                          size_t source_index, const MapGraph *graph) {
  const AbString **names = NULL;
  size_t target_index;
  int first_target = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{");
  if (graph->edge_count) {
    names = (const AbString **)ab_malloc(engine,
                                         graph->edge_count * sizeof(*names));
    if (!names)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting component routes");
  }
  for (target_index = 0; status == ARCHBIRD_OK && target_index < row_count;
       target_index++) {
    size_t edge_index;
    size_t name_count = 0;
    size_t name_index;
    const AbString *previous = NULL;
    if (target_index == source_index)
      continue;
    for (edge_index = 0; edge_index < graph->edge_count; edge_index++) {
      const EdgeMention *edge = &graph->edges[edge_index];
      if (component_contains(&rows[source_index], edge->source) &&
          component_contains(&rows[target_index], &edge->target))
        names[name_count++] = &edge->name;
    }
    if (!name_count)
      continue;
    if (name_count > 1)
      qsort(names, name_count, sizeof(*names), string_pointer_compare);
    if (!first_target)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &rows[target_index].config->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":[");
    for (name_index = 0; status == ARCHBIRD_OK && name_index < name_count;
         name_index++) {
      if (previous && ab_string_equal(previous, names[name_index]))
        continue;
      if (previous)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, names[name_index]);
      previous = names[name_index];
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "]");
    first_target = 0;
  }
  ab_free(engine, names);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus collect_components(ArchbirdEngine *engine,
                                         const ArchbirdProject *project,
                                         const AbMapConfig *config,
                                         const AbSourceManifest *manifest,
                                         ComponentRow **out_rows) {
  ComponentRow *rows = NULL;
  size_t component_index;
  *out_rows = NULL;
  if (config->component_count) {
    rows = (ComponentRow *)ab_calloc(engine, config->component_count,
                                     sizeof(*rows));
    if (!rows)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting map components");
  }
  for (component_index = 0; component_index < config->component_count;
       component_index++) {
    size_t file_index;
    rows[component_index].config = &config->components[component_index];
    if (manifest->file_count) {
      rows[component_index].files = (const AbManifestFile **)ab_malloc(
          engine, manifest->file_count * sizeof(*rows[component_index].files));
      if (!rows[component_index].files) {
        components_free(engine, rows, config->component_count);
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory collecting component files");
      }
    }
    for (file_index = 0; file_index < manifest->file_count; file_index++) {
      const AbManifestFile *file = &manifest->files[file_index];
      size_t pattern_index;
      int matched = 0;
      if (!file->has_layer)
        continue;
      for (pattern_index = 0;
           pattern_index < rows[component_index].config->paths.count;
           pattern_index++) {
        if (ab_map_collection_match(
                &file->path,
                &rows[component_index].config->paths.items[pattern_index])) {
          matched = 1;
          break;
        }
      }
      if (matched) {
        size_t symbols = 0;
        ArchbirdStatus status =
            ab_file_symbol_count(engine, project, file, &symbols);
        if (status != ARCHBIRD_OK) {
          components_free(engine, rows, config->component_count);
          return status;
        }
        rows[component_index].files[rows[component_index].file_count++] = file;
        rows[component_index].symbol_count += symbols;
      }
    }
  }
  if (config->component_count > 1)
    qsort(rows, config->component_count, sizeof(*rows),
          component_pointer_compare);
  *out_rows = rows;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
render_components(AbBuffer *buffer, ArchbirdEngine *engine,
                  const ArchbirdProject *project, const AbMapConfig *config,
                  const AbSourceManifest *manifest, const MapGraph *graph) {
  ComponentRow *rows = NULL;
  size_t index;
  ArchbirdStatus status =
      collect_components(engine, project, config, manifest, &rows);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < config->component_count;
       index++) {
    size_t file_index;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"description\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &rows[index].config->description);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"files\":[");
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < rows[index].file_count;
         file_index++) {
      if (file_index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &rows[index].files[file_index]->path);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &rows[index].config->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"outgoing\":");
    if (status == ARCHBIRD_OK)
      status = render_component_outgoing(buffer, engine, rows,
                                         config->component_count, index, graph);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"symbol_count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, rows[index].symbol_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  components_free(engine, rows, config->component_count);
  return status;
}

static ArchbirdStatus render_diagnostics(AbBuffer *buffer,
                                         const AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->diagnostic_count;
       index++) {
    const AbMapDiagnostic *diagnostic = &state->diagnostics[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"code\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &diagnostic->code);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"message\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &diagnostic->message);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &diagnostic->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"severity\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &diagnostic->severity);
    if (status == ARCHBIRD_OK && diagnostic->has_span) {
      status = ab_buffer_literal(buffer, ",\"span\":{\"end\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, diagnostic->span_end);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"start\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, diagnostic->span_start);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus add_provider_diagnostics(AbMapState *state) {
  size_t provider_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (provider_index = 0;
       status == ARCHBIRD_OK &&
       provider_index < archbird_project_provider_count(state->project);
       provider_index++) {
    const AbProviderBundle *provider =
        ab_project_provider_bundle(state->project, provider_index);
    size_t diagnostic_index;
    for (diagnostic_index = 0;
         status == ARCHBIRD_OK && diagnostic_index < provider->diagnostic_count;
         diagnostic_index++) {
      const AbDiagnostic *diagnostic = &provider->diagnostics[diagnostic_index];
      if (diagnostic->has_span)
        status = ab_map_add_diagnostic_span(
            state, diagnostic->severity.data, diagnostic->code.data,
            diagnostic->message.data, &diagnostic->path, diagnostic->span_start,
            diagnostic->span_end);
      else
        status = ab_map_add_diagnostic(
            state, diagnostic->severity.data, diagnostic->code.data,
            diagnostic->message.data,
            diagnostic->has_path ? &diagnostic->path : NULL);
    }
  }
  return status;
}

ArchbirdStatus archbird_project_render_map(ArchbirdEngine *engine,
                                           const ArchbirdProject *project,
                                           uint32_t json_flags,
                                           ArchbirdWriteFn write_fn,
                                           void *user_data) {
  const AbSourceManifest *manifest;
  const AbMapConfig *config;
  AbBuffer buffer;
  AbMapState state = {0};
  char input_digest[65];
  size_t index;
  size_t rendered_files = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !project || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  if (!ab_project_providers_finalized(project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge must be finalized first");
  config = ab_project_config(project);
  if (!config)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "project configuration must be supplied first");
  manifest = ab_project_manifest(project);
  state.engine = engine;
  state.project = project;
  state.manifest = manifest;
  state.config = config;
  memcpy(input_digest, archbird_project_map_input_sha256(project), 65);
  ab_buffer_init(&buffer, engine);
  MAP_TRY(ab_map_analyze_packages(&state));
  MAP_TRY(build_graph(engine, project, manifest, config, state.packages,
                      state.package_count, &state.graph));
  MAP_TRY(ab_map_add_reference_edges(&state));
  MAP_TRY(ab_map_analyze_indexes(&state));
  MAP_TRY(ab_map_analyze_builds(&state));
  MAP_TRY(ab_map_analyze_artifacts(&state));
  MAP_TRY(ab_map_analyze_bridges(&state));
  MAP_TRY(ab_map_analyze_tests(&state));
  MAP_TRY(ab_map_analyze_named_entries(&state));
  MAP_TRY(ab_map_analyze_parity(&state));
  MAP_TRY(ab_map_run_legacy_checks(&state));
  MAP_TRY(add_provider_diagnostics(&state));
  ab_map_diagnostics_sort_unique(&state);
  MAP_TRY(ab_buffer_literal(&buffer, "{\"artifact\":\"map\",\"artifacts\":"));
  MAP_TRY(ab_map_render_artifacts(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"builds\":"));
  MAP_TRY(ab_map_render_builds(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"call_resolutions\":"));
  MAP_TRY(render_resolutions(&buffer, &state.graph));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"components\":"));
  MAP_TRY(render_components(&buffer, engine, project, config, manifest,
                            &state.graph));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"description\":"));
  MAP_TRY(json_string(&buffer, &config->description));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"diagnostics\":"));
  MAP_TRY(render_diagnostics(&buffer, &state));
  if (manifest->has_resolution) {
    const AbDiscoveryCoverage *coverage = &manifest->resolution.coverage;
    char resolution_sha256[65];
    char profile_sha256[65];
    archbird_sha256_hex(manifest->resolution.sha256, resolution_sha256);
    archbird_sha256_hex(manifest->resolution.profile_implementation_sha256,
                        profile_sha256);
    MAP_TRY(ab_buffer_literal(&buffer,
                              ",\"discovery\":{\"coverage\":{\"assets\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->assets));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"ignored\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->ignored));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"inventory_files\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->inventory_files));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"oversized\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->oversized));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"pruned_directories\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->pruned_directories));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"selected\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->selected));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"unsupported_known\":"));
    MAP_TRY(ab_buffer_u64(&buffer, coverage->unsupported_known));
    MAP_TRY(ab_buffer_literal(&buffer,
                              "},\"profile\":{\"implementation_sha256\":"));
    MAP_TRY(ab_buffer_json_string(&buffer, profile_sha256, 64));
    MAP_TRY(ab_buffer_literal(&buffer, ",\"name\":"));
    MAP_TRY(json_string(&buffer, &manifest->resolution.profile_name));
    MAP_TRY(ab_buffer_literal(&buffer, "},\"sha256\":"));
    MAP_TRY(ab_buffer_json_string(&buffer, resolution_sha256, 64));
    MAP_TRY(ab_buffer_literal(&buffer, "}"));
  }
  MAP_TRY(ab_buffer_literal(&buffer, ",\"edges\":"));
  MAP_TRY(render_edges(&buffer, &state.graph));
  MAP_TRY(ab_buffer_literal(&buffer,
                            ",\"evidence\":{\"absolute_paths_included\":false,"
                            "\"config_sha256\":"));
  MAP_TRY(ab_buffer_json_string(&buffer, config->sha256_hex, 64));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"input_sha256\":"));
  MAP_TRY(ab_buffer_json_string(&buffer, input_digest, 64));
  if (manifest->has_configuration_sha256 &&
      memcmp(manifest->configuration_sha256, config->sha256,
             sizeof(config->sha256)) != 0) {
    char resolution_digest[65];
    archbird_sha256_hex(manifest->configuration_sha256, resolution_digest);
    MAP_TRY(ab_buffer_literal(&buffer, ",\"resolution_sha256\":"));
    MAP_TRY(ab_buffer_json_string(&buffer, resolution_digest, 64));
  }
  MAP_TRY(ab_buffer_literal(&buffer, "},\"files\":["));
  for (index = 0; index < manifest->file_count; index++) {
    if (!manifest->files[index].has_layer)
      continue;
    if (rendered_files)
      MAP_TRY(ab_buffer_literal(&buffer, ","));
    MAP_TRY(ab_render_map_file_facts_row(&buffer, engine, project,
                                         &manifest->files[index]));
    rendered_files++;
  }
  MAP_TRY(ab_buffer_literal(&buffer, "],\"indexes\":"));
  MAP_TRY(ab_map_render_indexes(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"layers\":"));
  MAP_TRY(render_layers(&buffer, engine, project, config, manifest));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"limits\":{\"compact_edge_names\":"));
  MAP_TRY(ab_buffer_u64(&buffer, config->compact_edge_names));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"compact_symbols\":"));
  MAP_TRY(ab_buffer_u64(&buffer, config->compact_symbols));
  MAP_TRY(ab_buffer_literal(&buffer, "},\"named_entries\":"));
  MAP_TRY(ab_map_render_named_entries(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"packages\":"));
  MAP_TRY(ab_map_render_packages(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"parity\":"));
  MAP_TRY(ab_map_render_parity(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"project\":"));
  MAP_TRY(json_string(&buffer, &config->project));
  MAP_TRY(ab_buffer_literal(
      &buffer, ",\"schema_version\":" ARCHBIRD_MAP_SCHEMA_CURRENT_TEXT
               ",\"surfaces\":"));
  MAP_TRY(ab_map_render_surfaces(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"symbol_calls\":"));
  MAP_TRY(ab_map_render_symbol_calls(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"symbol_references\":"));
  MAP_TRY(ab_map_render_symbol_references(&buffer, &state));
  MAP_TRY(ab_buffer_literal(&buffer, ",\"tests\":"));
  MAP_TRY(ab_map_render_tests(&buffer, &state));
  MAP_TRY(ab_buffer_literal(
      &buffer,
      ",\"tool\":{"
      "\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}"));
  if (json_flags == 0) {
    if (write_fn(user_data, buffer.data, buffer.length) != 0)
      status =
          archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                             "JSON output callback failed");
  } else {
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  }
done:
  ab_map_state_free(&state);
  ab_buffer_free(&buffer);
  return status;
}

#undef MAP_TRY
