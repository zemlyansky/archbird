#include "map_internal.h"

#include "archbird_internal.h"
#include "pattern.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NamedCount {
  const AbManifestFile *file;
  const AbString *name;
  size_t count;
} NamedCount;

typedef struct BridgeNamedFact {
  const AbManifestFile *file;
  const AbFact *fact;
  const char *name;
  size_t name_length;
} BridgeNamedFact;

typedef struct BridgeFactIndex {
  BridgeNamedFact *symbols;
  size_t symbol_count;
  BridgeNamedFact *exports;
  size_t export_count;
} BridgeFactIndex;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int array_contains(const AbStringArray *array, const AbString *value) {
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (ab_string_equal(&array->items[index], value))
      return 1;
  }
  return 0;
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

static const AbManifestFile *fact_file(const AbMapState *state,
                                       const AbFact *fact) {
  return ab_map_manifest_file(state->manifest, fact->path.data,
                              fact->path.length);
}

static int fact_domain(const AbFact *fact, const char *domain) {
  size_t length = strlen(domain);
  return fact->domain.length == length &&
         memcmp(fact->domain.data, domain, length) == 0;
}

static void fact_leaf(const AbFact *fact, const char **out_data,
                      size_t *out_length) {
  size_t start = fact->name.length;
  while (start && fact->name.data[start - 1] != '.')
    start--;
  *out_data = fact->name.data + start;
  *out_length = fact->name.length - start;
}

static int bytes_compare(const char *left, size_t left_length,
                         const char *right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared)
    return compared < 0 ? -1 : 1;
  return (left_length > right_length) - (left_length < right_length);
}

static int named_fact_compare(const void *left_raw, const void *right_raw) {
  const BridgeNamedFact *left = (const BridgeNamedFact *)left_raw;
  const BridgeNamedFact *right = (const BridgeNamedFact *)right_raw;
  int compared = bytes_compare(left->name, left->name_length, right->name,
                               right->name_length);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->file->path, &right->file->path);
  return compared ? compared
                  : ab_string_compare(&left->fact->id, &right->fact->id);
}

static void bridge_fact_index_free(ArchbirdEngine *engine,
                                   BridgeFactIndex *index) {
  ab_free(engine, index->symbols);
  ab_free(engine, index->exports);
  memset(index, 0, sizeof(*index));
}

static ArchbirdStatus bridge_fact_index_build(AbMapState *state,
                                              BridgeFactIndex *index) {
  size_t total = ab_project_merged_fact_count(state->project);
  size_t fact_index;
  memset(index, 0, sizeof(*index));
  if (total) {
    index->symbols = (BridgeNamedFact *)ab_malloc(
        state->engine, total * sizeof(*index->symbols));
    index->exports = (BridgeNamedFact *)ab_malloc(
        state->engine, total * sizeof(*index->exports));
    if (!index->symbols || !index->exports) {
      bridge_fact_index_free(state->engine, index);
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing bridge evidence");
    }
  }
  for (fact_index = 0; fact_index < total; fact_index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
    const AbManifestFile *file;
    BridgeNamedFact *row;
    if (!fact->has_name || !fact->name.length)
      continue;
    file = fact_file(state, fact);
    if (!file || !file->has_layer)
      continue;
    if (fact_domain(fact, "symbols")) {
      row = &index->symbols[index->symbol_count++];
      fact_leaf(fact, &row->name, &row->name_length);
    } else if (fact_domain(fact, "exports")) {
      row = &index->exports[index->export_count++];
      row->name = fact->name.data;
      row->name_length = fact->name.length;
    } else {
      continue;
    }
    row->file = file;
    row->fact = fact;
  }
  if (index->symbol_count > 1)
    qsort(index->symbols, index->symbol_count, sizeof(*index->symbols),
          named_fact_compare);
  if (index->export_count > 1)
    qsort(index->exports, index->export_count, sizeof(*index->exports),
          named_fact_compare);
  return ARCHBIRD_OK;
}

static void named_fact_range(const BridgeNamedFact *items, size_t count,
                             const AbString *name, size_t *out_start,
                             size_t *out_end) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = bytes_compare(items[middle].name, items[middle].name_length,
                                 name->data, name->length);
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  while (
      low < count && items[low].name_length == name->length &&
      (!name->length || memcmp(items[low].name, name->data, name->length) == 0))
    low++;
  *out_end = low;
}

static int source_matches(const AbConfigBridge *bridge,
                          const AbManifestFile *file) {
  size_t index;
  if (!file->has_layer || !array_contains(&bridge->from_layers, &file->layer))
    return 0;
  if (bridge->from_paths.count) {
    int matched = 0;
    for (index = 0; index < bridge->from_paths.count; index++) {
      if (ab_map_collection_match(&file->path,
                                  &bridge->from_paths.items[index])) {
        matched = 1;
        break;
      }
    }
    if (!matched)
      return 0;
  }
  for (index = 0; index < bridge->exclude_from_paths.count; index++) {
    if (ab_map_collection_match(&file->path,
                                &bridge->exclude_from_paths.items[index]))
      return 0;
  }
  return 1;
}

static int canonical_name(const AbConfigBridge *bridge, const AbString *raw,
                          AbString *out) {
  size_t candidate;
  const char *starts[2] = {raw->data, raw->data};
  size_t lengths[2] = {raw->length, raw->length};
  size_t candidate_count = 1;
  if (raw->length && raw->data[0] == '_') {
    starts[1] = raw->data + 1;
    lengths[1] = raw->length - 1;
    candidate_count = 2;
  }
  if (!bridge->prefixes.count) {
    *out = *raw;
    return 1;
  }
  for (candidate = 0; candidate < candidate_count; candidate++) {
    size_t prefix;
    for (prefix = 0; prefix < bridge->prefixes.count; prefix++) {
      const AbString *wanted = &bridge->prefixes.items[prefix];
      if (wanted->length <= lengths[candidate] &&
          memcmp(starts[candidate], wanted->data, wanted->length) == 0) {
        out->data = (char *)starts[candidate];
        out->length = lengths[candidate];
        return 1;
      }
    }
  }
  return 0;
}

static int named_count_compare(const void *left_raw, const void *right_raw) {
  const NamedCount *left = (const NamedCount *)left_raw;
  const NamedCount *right = (const NamedCount *)right_raw;
  int compared = ab_string_compare(&left->file->path, &right->file->path);
  return compared ? compared : ab_string_compare(left->name, right->name);
}

static ArchbirdStatus collect_named_counts(AbMapState *state,
                                           NamedCount **out_rows,
                                           size_t *out_count) {
  size_t total = ab_project_merged_fact_count(state->project);
  NamedCount *rows = NULL;
  size_t count = 0;
  size_t index;
  size_t write = 0;
  *out_rows = NULL;
  *out_count = 0;
  if (total) {
    rows = (NamedCount *)ab_malloc(state->engine, total * sizeof(*rows));
    if (!rows)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting bridge calls");
  }
  for (index = 0; index < total; index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbManifestFile *file;
    if (!fact->has_name || !fact->name.length ||
        (!fact_domain(fact, "calls") && !fact_domain(fact, "method-calls")))
      continue;
    file = fact_file(state, fact);
    if (!file || !file->has_layer)
      continue;
    rows[count].file = file;
    rows[count].name = &fact->name;
    rows[count].count = 1;
    count++;
  }
  if (count > 1)
    qsort(rows, count, sizeof(*rows), named_count_compare);
  for (index = 0; index < count; index++) {
    if (write && rows[write - 1].file == rows[index].file &&
        ab_string_equal(rows[write - 1].name, rows[index].name)) {
      rows[write - 1].count++;
    } else {
      if (write != index)
        rows[write] = rows[index];
      write++;
    }
  }
  *out_rows = rows;
  *out_count = write;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_unique(ArchbirdEngine *engine,
                                    AbStringArray *array, const char *data,
                                    size_t length) {
  AbString *resized;
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (array->items[index].length == length &&
        (!length || memcmp(array->items[index].data, data, length) == 0))
      return ARCHBIRD_OK;
  }
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting bridge evidence");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus bridge_kind(ArchbirdEngine *engine,
                                  const AbConfigBridge *bridge, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "bridge:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, bridge->name.data, bridge->name.length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus
add_buffer_diagnostic(AbMapState *state, const char *severity, const char *code,
                      const AbBuffer *message, const AbString *path) {
  return ab_map_add_diagnostic(state, severity, code,
                               message->data ? (const char *)message->data : "",
                               path);
}

static ArchbirdStatus definition_paths(AbMapState *state,
                                       const BridgeFactIndex *index,
                                       const AbConfigBridge *bridge,
                                       const AbString *name, int exports,
                                       AbStringArray *out) {
  const BridgeNamedFact *items = exports ? index->exports : index->symbols;
  size_t item_count = exports ? index->export_count : index->symbol_count;
  size_t start;
  size_t end;
  size_t item_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  named_fact_range(items, item_count, name, &start, &end);
  for (item_index = start; status == ARCHBIRD_OK && item_index < end;
       item_index++) {
    const AbFact *fact = items[item_index].fact;
    const AbManifestFile *file = items[item_index].file;
    const AbString *scope;
    if (!array_contains(&bridge->to_layers, &file->layer))
      continue;
    if (exports) {
      /* The export index is already keyed by the exact public name. */
    } else {
      if (string_literal(&fact->kind, "declaration"))
        continue;
      scope = fact_string_attribute(fact, "scope");
      if (scope && string_literal(scope, "local"))
        continue;
    }
    status =
        append_unique(state->engine, out, file->path.data, file->path.length);
  }
  return status;
}

static ArchbirdStatus add_call_bridges(AbMapState *state,
                                       const BridgeFactIndex *fact_index,
                                       const AbConfigBridge *bridge,
                                       const NamedCount *calls,
                                       size_t call_count) {
  size_t index;
  int exports = string_literal(&bridge->kind, "binding");
  AbString kind = {0};
  AbStringArray unresolved = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  status = bridge_kind(state->engine, bridge, &kind);
  for (index = 0; status == ARCHBIRD_OK && index < call_count; index++) {
    AbString name;
    AbStringArray targets = {0};
    size_t target;
    if (!source_matches(bridge, calls[index].file) ||
        !canonical_name(bridge, calls[index].name, &name) ||
        array_contains(&bridge->ignore, &name))
      continue;
    status =
        definition_paths(state, fact_index, bridge, &name, exports, &targets);
    if (status == ARCHBIRD_OK && targets.count == 1)
      status = ab_map_graph_add_edge(
          state->engine, &state->graph, kind.data, &calls[index].file->path,
          targets.items[0].data, targets.items[0].length, &name);
    if (status == ARCHBIRD_OK &&
        ((!exports && targets.count != 1) || (exports && targets.count > 1)))
      status =
          append_unique(state->engine, &unresolved, name.data, name.length);
    for (target = 0; target < targets.count; target++)
      ab_string_free(state->engine, &targets.items[target]);
    ab_free(state->engine, targets.items);
  }
  if (status == ARCHBIRD_OK && unresolved.count) {
    AbBuffer message;
    size_t limit = unresolved.count < 20 ? unresolved.count : 20;
    size_t item;
    qsort(unresolved.items, unresolved.count, sizeof(*unresolved.items),
          string_compare);
    ab_buffer_init(&message, state->engine);
    status = ab_buffer_append(&message, bridge->name.data, bridge->name.length);
    if (status == ARCHBIRD_OK && exports)
      status = ab_buffer_literal(&message, ": ambiguous exported properties: ");
    if (status == ARCHBIRD_OK && !exports) {
      status = ab_buffer_literal(&message, ": ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&message, unresolved.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(
            &message, " ABI names had zero or ambiguous targets: ");
    }
    for (item = 0; status == ARCHBIRD_OK && item < limit; item++) {
      if (item)
        status = ab_buffer_literal(&message, ", ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, unresolved.items[item].data,
                                  unresolved.items[item].length);
    }
    if (status == ARCHBIRD_OK && !exports && unresolved.count > limit)
      status = ab_buffer_literal(&message, " â¦");
    if (status == ARCHBIRD_OK)
      status = add_buffer_diagnostic(
          state, "warning", exports ? "binding-ambiguous" : "bridge-unresolved",
          &message, NULL);
    ab_buffer_free(&message);
  }
  for (index = 0; index < unresolved.count; index++)
    ab_string_free(state->engine, &unresolved.items[index]);
  ab_free(state->engine, unresolved.items);
  ab_string_free(state->engine, &kind);
  return status;
}

static size_t pair_key_length(const AbString *value) {
  size_t index;
  for (index = 0; index < value->length; index++) {
    if (value->data[index] == ':')
      return index;
  }
  return value->length;
}

static int message_allowed(const AbConfigBridge *bridge,
                           const AbString *message) {
  size_t key_length;
  size_t index;
  if (!bridge->message_keys.count)
    return 1;
  key_length = pair_key_length(message);
  for (index = 0; index < bridge->message_keys.count; index++) {
    if (bridge->message_keys.items[index].length == key_length &&
        memcmp(bridge->message_keys.items[index].data, message->data,
               key_length) == 0)
      return 1;
  }
  return 0;
}

static int message_matches(const AbString *sent, const AbString *received) {
  size_t sent_key = pair_key_length(sent);
  size_t received_key = pair_key_length(received);
  const char *sent_value = sent->data + sent_key + (sent_key < sent->length);
  const char *received_value =
      received->data + received_key + (received_key < received->length);
  size_t sent_length = sent->length - (size_t)(sent_value - sent->data);
  size_t received_length =
      received->length - (size_t)(received_value - received->data);
  return ab_string_equal(sent, received) ||
         (received_key == 1 && received->data[0] == '*' &&
          sent_length == received_length &&
          memcmp(sent_value, received_value, sent_length) == 0);
}

static ArchbirdStatus message_direction(AbMapState *state,
                                        const AbConfigBridge *bridge,
                                        const AbStringArray *from_layers,
                                        const AbStringArray *to_layers) {
  size_t sent_index;
  AbString kind = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  status = bridge_kind(state->engine, bridge, &kind);
  for (sent_index = 0;
       status == ARCHBIRD_OK &&
       sent_index < ab_project_merged_fact_count(state->project);
       sent_index++) {
    const AbFact *sent = ab_project_merged_fact(state->project, sent_index);
    const AbManifestFile *source;
    size_t receive_index;
    if (!sent->has_name || !fact_domain(sent, "messages") ||
        !string_literal(&sent->kind, "send") ||
        !message_allowed(bridge, &sent->name))
      continue;
    source = fact_file(state, sent);
    if (!source || !source->has_layer ||
        !array_contains(from_layers, &source->layer))
      continue;
    for (receive_index = 0;
         status == ARCHBIRD_OK &&
         receive_index < ab_project_merged_fact_count(state->project);
         receive_index++) {
      const AbFact *received =
          ab_project_merged_fact(state->project, receive_index);
      const AbManifestFile *target;
      if (!received->has_name || !fact_domain(received, "messages") ||
          !string_literal(&received->kind, "receive") ||
          !message_matches(&sent->name, &received->name))
        continue;
      target = fact_file(state, received);
      if (!target || target == source || !target->has_layer ||
          !array_contains(to_layers, &target->layer))
        continue;
      status = ab_map_graph_add_edge(state->engine, &state->graph, kind.data,
                                     &source->path, target->path.data,
                                     target->path.length, &sent->name);
    }
  }
  ab_string_free(state->engine, &kind);
  return status;
}

static int declaration_compare(const void *left_raw, const void *right_raw) {
  const AbMapSurfaceDeclaration *left =
      (const AbMapSurfaceDeclaration *)left_raw;
  const AbMapSurfaceDeclaration *right =
      (const AbMapSurfaceDeclaration *)right_raw;
  int compared = ab_string_compare(&left->path, &right->path);
  return compared ? compared : ab_string_compare(&left->source, &right->source);
}

static int use_compare(const void *left_raw, const void *right_raw) {
  const AbMapSurfaceUse *left = (const AbMapSurfaceUse *)left_raw;
  const AbMapSurfaceUse *right = (const AbMapSurfaceUse *)right_raw;
  return ab_string_compare(&left->path, &right->path);
}

static int surface_name_compare(const void *left_raw, const void *right_raw) {
  const AbMapSurfaceName *left = (const AbMapSurfaceName *)left_raw;
  const AbMapSurfaceName *right = (const AbMapSurfaceName *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int surface_compare(const void *left_raw, const void *right_raw) {
  const AbMapSurface *left = (const AbMapSurface *)left_raw;
  const AbMapSurface *right = (const AbMapSurface *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus surface_name(AbMapState *state, AbMapSurface *surface,
                                   const AbString *name,
                                   AbMapSurfaceName **out) {
  AbMapSurfaceName *resized;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < surface->name_count; index++) {
    if (ab_string_equal(&surface->names[index].name, name)) {
      *out = &surface->names[index];
      return ARCHBIRD_OK;
    }
  }
  resized = (AbMapSurfaceName *)ab_realloc(state->engine, surface->names,
                                           (surface->name_count + 1) *
                                               sizeof(*surface->names));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting bridge surface names");
  surface->names = resized;
  *out = &surface->names[surface->name_count];
  memset(*out, 0, sizeof(**out));
  status =
      ab_string_copy(state->engine, &(*out)->name, name->data, name->length);
  if (status == ARCHBIRD_OK)
    surface->name_count++;
  return status;
}

static ArchbirdStatus append_declaration(AbMapState *state,
                                         AbMapSurfaceDeclaration **rows,
                                         size_t *count, const AbString *path,
                                         const char *source,
                                         size_t source_length) {
  AbMapSurfaceDeclaration *resized;
  AbMapSurfaceDeclaration *row;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < *count; index++) {
    if (ab_string_equal(&(*rows)[index].path, path) &&
        (*rows)[index].source.length == source_length &&
        memcmp((*rows)[index].source.data, source, source_length) == 0)
      return ARCHBIRD_OK;
  }
  resized = (AbMapSurfaceDeclaration *)ab_realloc(
      state->engine, *rows, (*count + 1) * sizeof(**rows));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting surface declarations");
  *rows = resized;
  row = &(*rows)[*count];
  memset(row, 0, sizeof(*row));
  status = ab_string_copy(state->engine, &row->path, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->source, source, source_length);
  if (status == ARCHBIRD_OK)
    (*count)++;
  return status;
}

static ArchbirdStatus append_use(AbMapState *state, AbMapSurfaceName *name,
                                 const AbString *path, size_t count) {
  AbMapSurfaceUse *resized;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < name->use_count; index++) {
    if (ab_string_equal(&name->uses[index].path, path)) {
      if (name->uses[index].count > SIZE_MAX - count)
        return archbird_error_set(state->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "surface use count overflow");
      name->uses[index].count += count;
      return ARCHBIRD_OK;
    }
  }
  resized = (AbMapSurfaceUse *)ab_realloc(
      state->engine, name->uses, (name->use_count + 1) * sizeof(*name->uses));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting surface uses");
  name->uses = resized;
  memset(&name->uses[name->use_count], 0, sizeof(*name->uses));
  status = ab_string_copy(state->engine, &name->uses[name->use_count].path,
                          path->data, path->length);
  if (status == ARCHBIRD_OK) {
    name->uses[name->use_count].count = count;
    name->use_count++;
  }
  return status;
}

static ArchbirdStatus provider_source(AbMapState *state,
                                      const AbConfigProvider *provider,
                                      const uint8_t **out_text,
                                      size_t *out_length, int *out_present) {
  const AbManifestFile *file = ab_map_manifest_file(
      state->manifest, provider->path.data, provider->path.length);
  *out_present = file != NULL;
  if (!file) {
    *out_text = NULL;
    *out_length = 0;
    return ARCHBIRD_OK;
  }
  *out_text = ab_project_source_bytes(state->project,
                                      (size_t)(file - state->manifest->files));
  *out_length = file->byte_length;
  return ARCHBIRD_OK;
}

typedef struct ProviderCapture {
  AbMapState *state;
  const AbConfigBridge *bridge;
  AbMapSurface *surface;
  const AbString *path;
  const char *source;
  size_t source_length;
  const uint8_t *subject;
  size_t names;
} ProviderCapture;

static ArchbirdStatus capture_provider_name(void *user_data,
                                            const AbPatternMatch *match) {
  ProviderCapture *capture = (ProviderCapture *)user_data;
  AbString raw;
  AbString canonical;
  AbMapSurfaceName *name;
  ArchbirdStatus status;
  if (!match->capture_present)
    return ARCHBIRD_OK;
  raw.data = (char *)capture->subject + match->capture_start;
  raw.length = match->capture_end - match->capture_start;
  if (!canonical_name(capture->bridge, &raw, &canonical))
    return ARCHBIRD_OK;
  status = surface_name(capture->state, capture->surface, &canonical, &name);
  if (status == ARCHBIRD_OK)
    status = append_declaration(capture->state, &name->declarations,
                                &name->declaration_count, capture->path,
                                capture->source, capture->source_length);
  if (status == ARCHBIRD_OK)
    capture->names++;
  return status;
}

static ArchbirdStatus exports_provider(AbMapState *state,
                                       const AbConfigBridge *bridge,
                                       const AbConfigProvider *provider,
                                       AbMapSurface *surface) {
  size_t file_index;
  size_t matched = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (file_index = 0;
       status == ARCHBIRD_OK && file_index < state->manifest->file_count;
       file_index++) {
    const AbManifestFile *file = &state->manifest->files[file_index];
    size_t fact_index;
    if (!file->has_layer || !array_contains(&bridge->to_layers, &file->layer) ||
        (provider->path.length &&
         !ab_map_collection_match(&file->path, &provider->path)))
      continue;
    matched++;
    status =
        append_declaration(state, &surface->providers, &surface->provider_count,
                           &file->path, "exports", 7);
    for (fact_index = 0;
         status == ARCHBIRD_OK &&
         fact_index < ab_project_merged_fact_count(state->project);
         fact_index++) {
      const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
      AbString canonical;
      AbMapSurfaceName *name;
      if (!fact->has_name || !fact_domain(fact, "exports") ||
          !ab_string_equal(&fact->path, &file->path) ||
          !canonical_name(bridge, &fact->name, &canonical))
        continue;
      status = surface_name(state, surface, &canonical, &name);
      if (status == ARCHBIRD_OK)
        status = append_declaration(state, &name->declarations,
                                    &name->declaration_count, &file->path,
                                    "exports", 7);
    }
  }
  if (status == ARCHBIRD_OK && !matched) {
    char message[320];
    snprintf(message, sizeof(message),
             "bridge %.*s: exports provider matched no mapped files",
             (int)bridge->name.length, bridge->name.data);
    status = ab_map_add_diagnostic(
        state, "error", "provider-source-unmatched", message,
        provider->path.length ? &provider->path : NULL);
  }
  return status;
}

static ArchbirdStatus pattern_provider(AbMapState *state,
                                       const AbConfigBridge *bridge,
                                       const AbConfigProvider *provider,
                                       AbMapSurface *surface) {
  const uint8_t *text = NULL;
  size_t text_length = 0;
  AbString expanded = {0};
  const uint8_t *subject;
  size_t subject_length;
  AbString source = {0};
  AbPattern *pattern = NULL;
  ProviderCapture capture;
  size_t matches = 0;
  int found = 0;
  int present = 0;
  static const uint8_t empty[] = "";
  ArchbirdStatus status =
      provider_source(state, provider, &text, &text_length, &present);
  if (status == ARCHBIRD_OK && !present) {
    status =
        ab_map_add_diagnostic(state, "error", "provider-source-missing",
                              "provider source is absent", &provider->path);
    return status;
  }
  if (!text)
    text = empty;
  status = ab_utf8_validate(state->engine, text, text_length);
  if (status != ARCHBIRD_OK) {
    const char *message = archbird_engine_error(state->engine);
    status = ab_map_add_diagnostic(
        state, "error", "read-failed",
        message && message[0] ? message : "provider source is not UTF-8",
        &provider->path);
    archbird_error_clear(state->engine);
    return status;
  }
  if (string_literal(&provider->kind, "make_variable")) {
    status = ab_make_variable_value(state->engine, text, text_length,
                                    &provider->variable, &expanded, &found);
    if (status == ARCHBIRD_OK && !found) {
      char message[320];
      snprintf(message, sizeof(message),
               "bridge %.*s: Make variable '%.*s' is absent",
               (int)bridge->name.length, bridge->name.data,
               (int)provider->variable.length, provider->variable.data);
      status =
          ab_map_add_diagnostic(state, "error", "provider-variable-missing",
                                message, &provider->path);
      ab_string_free(state->engine, &expanded);
      return status;
    }
    subject = (const uint8_t *)expanded.data;
    subject_length = expanded.length;
    {
      size_t length = 14 + provider->variable.length;
      char *value = (char *)ab_malloc(state->engine, length + 1);
      if (!value) {
        ab_string_free(state->engine, &expanded);
        return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory naming Make provider");
      }
      memcpy(value, "make-variable:", 14);
      memcpy(value + 14, provider->variable.data, provider->variable.length);
      value[length] = '\0';
      source.data = value;
      source.length = length;
    }
  } else {
    subject = text;
    subject_length = text_length;
    status = ab_string_copy(state->engine, &source, "file-pattern", 12);
  }
  if (status == ARCHBIRD_OK)
    status =
        append_declaration(state, &surface->providers, &surface->provider_count,
                           &provider->path, source.data, source.length);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_compile(state->engine, &provider->pattern, 1, &pattern);
  memset(&capture, 0, sizeof(capture));
  capture.state = state;
  capture.bridge = bridge;
  capture.surface = surface;
  capture.path = &provider->path;
  capture.source = source.data;
  capture.source_length = source.length;
  capture.subject = subject;
  if (status == ARCHBIRD_OK)
    status = ab_pattern_scan(state->engine, pattern, subject, subject_length, 1,
                             capture_provider_name, &capture, &matches);
  if (status == ARCHBIRD_OK && !capture.names) {
    char message[320];
    snprintf(message, sizeof(message),
             "bridge %.*s: provider extracted no names",
             (int)bridge->name.length, bridge->name.data);
    status = ab_map_add_diagnostic(state, "error", "provider-empty", message,
                                   &provider->path);
  }
  ab_pattern_free(pattern);
  ab_string_free(state->engine, &source);
  ab_string_free(state->engine, &expanded);
  return status;
}

static ArchbirdStatus surface_candidates(AbMapState *state,
                                         const BridgeFactIndex *index,
                                         const AbConfigBridge *bridge,
                                         AbMapSurfaceName *name) {
  size_t fact_index;
  size_t start = 0;
  size_t end = index->export_count;
  size_t declaration_index;
  int abi = string_literal(&bridge->kind, "abi");
  ArchbirdStatus status = ARCHBIRD_OK;
  if (abi) {
    named_fact_range(index->symbols, index->symbol_count, &name->name, &start,
                     &end);
  }
  for (fact_index = start; status == ARCHBIRD_OK && fact_index < end;
       fact_index++) {
    const BridgeNamedFact *indexed =
        abi ? &index->symbols[fact_index] : &index->exports[fact_index];
    const AbFact *fact = indexed->fact;
    const AbManifestFile *file = indexed->file;
    int matched = 0;
    if (!array_contains(&bridge->to_layers, &file->layer))
      continue;
    if (abi) {
      const AbString *scope;
      if (string_literal(&fact->kind, "declaration"))
        continue;
      scope = fact_string_attribute(fact, "scope");
      matched = !(scope && string_literal(scope, "local"));
    } else {
      AbString canonical;
      matched = canonical_name(bridge, &fact->name, &canonical) &&
                ab_string_equal(&canonical, &name->name);
    }
    if (matched)
      status = append_unique(state->engine, &name->candidates, file->path.data,
                             file->path.length);
  }
  if (!abi) {
    for (declaration_index = 0;
         status == ARCHBIRD_OK && declaration_index < name->declaration_count;
         declaration_index++) {
      const AbMapSurfaceDeclaration *declaration =
          &name->declarations[declaration_index];
      if (string_literal(&declaration->source, "file-pattern"))
        status =
            append_unique(state->engine, &name->candidates,
                          declaration->path.data, declaration->path.length);
    }
  }
  if (status == ARCHBIRD_OK && name->candidates.count > 1)
    qsort(name->candidates.items, name->candidates.count,
          sizeof(*name->candidates.items), string_compare);
  return status;
}

static ArchbirdStatus surface_signatures(AbMapState *state,
                                         const BridgeFactIndex *index,
                                         const AbConfigBridge *bridge,
                                         AbMapSurfaceName *name) {
  size_t start;
  size_t end;
  size_t fact_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  named_fact_range(index->symbols, index->symbol_count, &name->name, &start,
                   &end);
  for (fact_index = start; status == ARCHBIRD_OK && fact_index < end;
       fact_index++) {
    const AbFact *fact = index->symbols[fact_index].fact;
    const AbManifestFile *file = index->symbols[fact_index].file;
    const AbString *signature;
    int declaration;
    if (!array_contains(&bridge->to_layers, &file->layer))
      continue;
    signature = fact_string_attribute(fact, "signature");
    if (!signature || !signature->length)
      continue;
    declaration = string_literal(&fact->kind, "declaration");
    if (declaration) {
      status = append_unique(state->engine, &name->declaration_signatures,
                             signature->data, signature->length);
    } else if (array_contains(&name->candidates, &file->path)) {
      status = append_unique(state->engine, &name->implementation_signatures,
                             signature->data, signature->length);
    }
  }
  if (status == ARCHBIRD_OK && name->declaration_signatures.count > 1)
    qsort(name->declaration_signatures.items,
          name->declaration_signatures.count,
          sizeof(*name->declaration_signatures.items), string_compare);
  if (status == ARCHBIRD_OK && name->implementation_signatures.count > 1)
    qsort(name->implementation_signatures.items,
          name->implementation_signatures.count,
          sizeof(*name->implementation_signatures.items), string_compare);
  return status;
}

static ArchbirdStatus finish_surface_name(AbMapState *state,
                                          const BridgeFactIndex *index,
                                          const AbConfigBridge *bridge,
                                          AbMapSurfaceName *name) {
  const char *declaration = name->declaration_count  ? "declared"
                            : bridge->provider_count ? "undeclared"
                                                     : "unknown";
  const char *resolution;
  ArchbirdStatus status = surface_candidates(state, index, bridge, name);
  if (status == ARCHBIRD_OK)
    status = surface_signatures(state, index, bridge, name);
  resolution = name->candidates.count == 1 ? "unique"
               : name->candidates.count    ? "ambiguous"
                                           : "unresolved";
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &name->declaration, declaration,
                            strlen(declaration));
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &name->resolution, resolution,
                            strlen(resolution));
  name->ignored = array_contains(&bridge->ignore, &name->name);
  if (name->declaration_count > 1)
    qsort(name->declarations, name->declaration_count,
          sizeof(*name->declarations), declaration_compare);
  if (name->use_count > 1)
    qsort(name->uses, name->use_count, sizeof(*name->uses), use_compare);
  return status;
}

static ArchbirdStatus build_surface(AbMapState *state,
                                    const BridgeFactIndex *fact_index,
                                    const AbConfigBridge *bridge,
                                    const NamedCount *calls, size_t call_count,
                                    AbMapSurface *surface) {
  size_t provider_index;
  size_t call_index;
  size_t name_index;
  ArchbirdStatus status = ab_string_copy(
      state->engine, &surface->name, bridge->name.data, bridge->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &surface->kind, bridge->kind.data,
                            bridge->kind.length);
  surface->provider_configured = bridge->provider_count != 0;
  for (provider_index = 0;
       status == ARCHBIRD_OK && provider_index < bridge->provider_count;
       provider_index++) {
    const AbConfigProvider *provider = &bridge->providers[provider_index];
    if (string_literal(&provider->kind, "exports"))
      status = exports_provider(state, bridge, provider, surface);
    else
      status = pattern_provider(state, bridge, provider, surface);
  }
  for (call_index = 0; status == ARCHBIRD_OK && call_index < call_count;
       call_index++) {
    AbString canonical;
    AbMapSurfaceName *name;
    if (!source_matches(bridge, calls[call_index].file) ||
        !canonical_name(bridge, calls[call_index].name, &canonical))
      continue;
    status = surface_name(state, surface, &canonical, &name);
    if (status == ARCHBIRD_OK)
      status = append_use(state, name, &calls[call_index].file->path,
                          calls[call_index].count);
  }
  if (status == ARCHBIRD_OK && surface->provider_count > 1)
    qsort(surface->providers, surface->provider_count,
          sizeof(*surface->providers), declaration_compare);
  if (status == ARCHBIRD_OK && surface->name_count > 1)
    qsort(surface->names, surface->name_count, sizeof(*surface->names),
          surface_name_compare);
  for (name_index = 0;
       status == ARCHBIRD_OK && name_index < surface->name_count; name_index++)
    status = finish_surface_name(state, fact_index, bridge,
                                 &surface->names[name_index]);
  return status;
}

ArchbirdStatus ab_map_analyze_bridges(AbMapState *state) {
  BridgeFactIndex fact_index;
  NamedCount *calls = NULL;
  size_t call_count = 0;
  size_t bridge_index;
  ArchbirdStatus status = bridge_fact_index_build(state, &fact_index);
  if (status == ARCHBIRD_OK)
    status = collect_named_counts(state, &calls, &call_count);
  for (bridge_index = 0;
       status == ARCHBIRD_OK && bridge_index < state->config->bridge_count;
       bridge_index++) {
    const AbConfigBridge *bridge = &state->config->bridges[bridge_index];
    if (string_literal(&bridge->kind, "abi") ||
        string_literal(&bridge->kind, "binding")) {
      status = add_call_bridges(state, &fact_index, bridge, calls, call_count);
    } else {
      status = message_direction(state, bridge, &bridge->from_layers,
                                 &bridge->to_layers);
      if (status == ARCHBIRD_OK && bridge->bidirectional)
        status = message_direction(state, bridge, &bridge->to_layers,
                                   &bridge->from_layers);
    }
  }
  for (bridge_index = 0;
       status == ARCHBIRD_OK && bridge_index < state->config->bridge_count;
       bridge_index++) {
    const AbConfigBridge *bridge = &state->config->bridges[bridge_index];
    AbMapSurface *resized;
    AbMapSurface *surface;
    if (!string_literal(&bridge->kind, "abi") &&
        !string_literal(&bridge->kind, "binding"))
      continue;
    resized = (AbMapSurface *)ab_realloc(state->engine, state->surfaces,
                                         (state->surface_count + 1) *
                                             sizeof(*state->surfaces));
    if (!resized) {
      status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory collecting bridge surfaces");
      break;
    }
    state->surfaces = resized;
    surface = &state->surfaces[state->surface_count++];
    memset(surface, 0, sizeof(*surface));
    status =
        build_surface(state, &fact_index, bridge, calls, call_count, surface);
  }
  if (status == ARCHBIRD_OK && state->surface_count > 1)
    qsort(state->surfaces, state->surface_count, sizeof(*state->surfaces),
          surface_compare);
  if (status == ARCHBIRD_OK)
    ab_map_graph_sort(&state->graph);
  ab_free(state->engine, calls);
  bridge_fact_index_free(state->engine, &fact_index);
  return status;
}

static ArchbirdStatus render_string_array(AbBuffer *buffer,
                                          const AbStringArray *array) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < array->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, array->items[index].data,
                                     array->items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_declarations(AbBuffer *buffer,
                                          const AbMapSurfaceDeclaration *rows,
                                          size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, rows[index].path.data,
                                     rows[index].path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"source\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, rows[index].source.data,
                                     rows[index].source.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_surface_summary(AbBuffer *buffer,
                                             const AbMapSurface *surface) {
  size_t index;
  size_t registered = 0, used = 0, unused = 0, unregistered = 0;
  size_t unknown = 0, resolved = 0, unresolved = 0, ambiguous = 0;
  size_t ignored = 0;
  ArchbirdStatus status;
  for (index = 0; index < surface->name_count; index++) {
    const AbMapSurfaceName *name = &surface->names[index];
    if (name->ignored) {
      ignored++;
      continue;
    }
    if (string_literal(&name->declaration, "declared")) {
      registered++;
      if (!name->use_count)
        unused++;
    } else if (string_literal(&name->declaration, "undeclared") &&
               name->use_count) {
      unregistered++;
    } else if (string_literal(&name->declaration, "unknown")) {
      unknown++;
    }
    if (name->use_count)
      used++;
    if (string_literal(&name->resolution, "unique"))
      resolved++;
    else if (string_literal(&name->resolution, "ambiguous"))
      ambiguous++;
    else if (string_literal(&name->resolution, "unresolved"))
      unresolved++;
  }
  status = ab_buffer_literal(buffer, "{\"ambiguous\":");
#define SUMMARY_FIELD(label, value)                                            \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_u64(buffer, (value));                                 \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_literal(buffer, (label));                             \
  } while (0)
  SUMMARY_FIELD(",\"declaration_unknown\":", ambiguous);
  SUMMARY_FIELD(",\"ignored\":", unknown);
  SUMMARY_FIELD(",\"registered\":", ignored);
  SUMMARY_FIELD(",\"resolved\":", registered);
  SUMMARY_FIELD(",\"unregistered_use\":", resolved);
  SUMMARY_FIELD(",\"unresolved\":", unregistered);
  SUMMARY_FIELD(",\"unused\":", unresolved);
  SUMMARY_FIELD(",\"used\":", unused);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, used);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
#undef SUMMARY_FIELD
  return status;
}

ArchbirdStatus ab_map_render_surfaces(AbBuffer *buffer,
                                      const AbMapState *state) {
  size_t surface_index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (surface_index = 0;
       status == ARCHBIRD_OK && surface_index < state->surface_count;
       surface_index++) {
    const AbMapSurface *surface = &state->surfaces[surface_index];
    size_t name_index;
    if (surface_index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"kind\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, surface->kind.data,
                                     surface->kind.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, surface->name.data,
                                     surface->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"names\":[");
    for (name_index = 0;
         status == ARCHBIRD_OK && name_index < surface->name_count;
         name_index++) {
      const AbMapSurfaceName *name = &surface->names[name_index];
      size_t use_index;
      if (name_index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"candidates\":");
      if (status == ARCHBIRD_OK)
        status = render_string_array(buffer, &name->candidates);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"declaration\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, name->declaration.data,
                                       name->declaration.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"declaration_signatures\":");
      if (status == ARCHBIRD_OK)
        status = render_string_array(buffer, &name->declaration_signatures);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"declarations\":");
      if (status == ARCHBIRD_OK)
        status = render_declarations(buffer, name->declarations,
                                     name->declaration_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"ignored\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, name->ignored ? "true" : "false");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"implementation_signatures\":");
      if (status == ARCHBIRD_OK)
        status = render_string_array(buffer, &name->implementation_signatures);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"name\":");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_json_string(buffer, name->name.data, name->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"registered\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(
            buffer,
            string_literal(&name->declaration, "declared") ? "true" : "false");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"resolution\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, name->resolution.data,
                                       name->resolution.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"unregistered_use\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(
            buffer,
            string_literal(&name->declaration, "undeclared") && name->use_count
                ? "true"
                : "false");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"unused\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(
            buffer,
            string_literal(&name->declaration, "declared") && !name->use_count
                ? "true"
                : "false");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"used\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, name->use_count ? "true" : "false");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"uses\":[");
      for (use_index = 0; status == ARCHBIRD_OK && use_index < name->use_count;
           use_index++) {
        if (use_index)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "{\"count\":");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_u64(buffer, name->uses[use_index].count);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ",\"path\":");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_json_string(buffer, name->uses[use_index].path.data,
                                    name->uses[use_index].path.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, "}");
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "]}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"provider_configured\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          buffer, surface->provider_configured ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"providers\":");
    if (status == ARCHBIRD_OK)
      status = render_declarations(buffer, surface->providers,
                                   surface->provider_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"summary\":");
    if (status == ARCHBIRD_OK)
      status = render_surface_summary(buffer, surface);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
