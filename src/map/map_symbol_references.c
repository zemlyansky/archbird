#include "map_internal.h"

#include "archbird_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbSymbolReferenceRow {
  const AbFact *occurrence;
  const AbProviderBundle *provider;
  const AbFact *source;
  const AbManifestFile *source_file;
  const AbString *context;
  const AbString *container;
  const AbFact *semantic;
  const AbProviderBundle *semantic_provider;
  const AbString *semantic_path;
  const AbString *semantic_symbol;
  struct AbSymbolReferenceCandidate *candidates;
  size_t candidate_count;
  size_t candidate_capacity;
  const char *relation;
  const char *resolution;
} AbSymbolReferenceRow;

typedef struct AbSymbolReferenceCandidate {
  const AbString *path;
  const AbString *symbol;
  const AbFact *definition;
} AbSymbolReferenceCandidate;

typedef struct AbSymbolReferenceLookup {
  AbSymbolReferenceRow *row;
} AbSymbolReferenceLookup;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const AbManifestFile *manifest_file(const AbSourceManifest *manifest,
                                           const AbString *path) {
  return ab_map_manifest_file(manifest, path->data, path->length);
}

static int optional_string_compare(const AbString *left,
                                   const AbString *right) {
  if (!left || !right)
    return (left != NULL) - (right != NULL);
  return ab_string_compare(left, right);
}

static int source_compare(const AbSymbolReferenceRow *left,
                          const AbSymbolReferenceRow *right) {
  if (!left->source || !right->source)
    return (left->source != NULL) - (right->source != NULL);
  return ab_string_compare(&left->source->name, &right->source->name);
}

static int source_equal(const AbSymbolReferenceRow *left,
                        const AbSymbolReferenceRow *right) {
  return (left->source != NULL) == (right->source != NULL) &&
         (!left->source ||
          ab_string_equal(&left->source->name, &right->source->name));
}

static int candidate_compare(const void *left_raw, const void *right_raw) {
  const AbSymbolReferenceCandidate *left =
      (const AbSymbolReferenceCandidate *)left_raw;
  const AbSymbolReferenceCandidate *right =
      (const AbSymbolReferenceCandidate *)right_raw;
  int compared = ab_string_compare(left->path, right->path);
  return compared ? compared : ab_string_compare(left->symbol, right->symbol);
}

static int candidate_lists_equal(const AbSymbolReferenceRow *left,
                                 const AbSymbolReferenceRow *right) {
  size_t index;
  if (left->candidate_count != right->candidate_count)
    return 0;
  for (index = 0; index < left->candidate_count; index++)
    if (!ab_string_equal(left->candidates[index].path,
                         right->candidates[index].path) ||
        !ab_string_equal(left->candidates[index].symbol,
                         right->candidates[index].symbol))
      return 0;
  return 1;
}

static int row_compare(const void *left_raw, const void *right_raw) {
  const AbSymbolReferenceRow *left = (const AbSymbolReferenceRow *)left_raw;
  const AbSymbolReferenceRow *right = (const AbSymbolReferenceRow *)right_raw;
  int compared =
      ab_string_compare(&left->occurrence->path, &right->occurrence->path);
  if (compared)
    return compared;
  compared = source_compare(left, right);
  if (compared)
    return compared;
  compared = ab_string_compare(left->context, right->context);
  if (compared)
    return compared;
  compared = strcmp(left->relation, right->relation);
  if (compared)
    return compared < 0 ? -1 : 1;
  compared = optional_string_compare(left->container, right->container);
  if (compared)
    return compared;
  compared =
      ab_string_compare(&left->occurrence->name, &right->occurrence->name);
  if (compared)
    return compared;
  compared = strcmp(left->resolution, right->resolution);
  if (compared)
    return compared < 0 ? -1 : 1;
  compared = optional_string_compare(left->semantic_path, right->semantic_path);
  if (compared)
    return compared;
  compared =
      optional_string_compare(left->semantic_symbol, right->semantic_symbol);
  if (compared)
    return compared;
  if (left->occurrence->span_start != right->occurrence->span_start)
    return left->occurrence->span_start < right->occurrence->span_start ? -1
                                                                        : 1;
  if (left->occurrence->span_end != right->occurrence->span_end)
    return left->occurrence->span_end < right->occurrence->span_end ? -1 : 1;
  return ab_string_compare(&left->occurrence->id, &right->occurrence->id);
}

static int same_group(const AbSymbolReferenceRow *left,
                      const AbSymbolReferenceRow *right) {
  if (!ab_string_equal(&left->occurrence->path, &right->occurrence->path) ||
      !source_equal(left, right) ||
      !ab_string_equal(left->context, right->context) ||
      strcmp(left->relation, right->relation) ||
      optional_string_compare(left->container, right->container) ||
      !ab_string_equal(&left->occurrence->name, &right->occurrence->name) ||
      strcmp(left->resolution, right->resolution) ||
      !candidate_lists_equal(left, right))
    return 0;
  if (!left->semantic_path || !right->semantic_path)
    return left->semantic_path == right->semantic_path;
  return ab_string_equal(left->semantic_path, right->semantic_path) &&
         ab_string_equal(left->semantic_symbol, right->semantic_symbol);
}

static ArchbirdStatus add_candidate(ArchbirdEngine *engine,
                                    AbSymbolReferenceRow *row,
                                    const AbString *path,
                                    const AbString *symbol,
                                    const AbFact *definition, int *added) {
  size_t index;
  AbSymbolReferenceCandidate *resized;
  *added = 0;
  for (index = 0; index < row->candidate_count; index++) {
    if (ab_string_equal(row->candidates[index].path, path) &&
        ab_string_equal(row->candidates[index].symbol, symbol)) {
      if (ab_map_symbol_definition_compare(
              definition, row->candidates[index].definition) < 0) {
        row->candidates[index].definition = definition;
        *added = 1;
      }
      return ARCHBIRD_OK;
    }
  }
  if (row->candidate_count == row->candidate_capacity) {
    size_t next = row->candidate_capacity ? row->candidate_capacity * 2 : 4;
    if (next > engine->options.max_values)
      next = engine->options.max_values;
    if (next <= row->candidate_capacity)
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "symbol reference candidate limit exceeded");
    resized = (AbSymbolReferenceCandidate *)ab_realloc(engine, row->candidates,
                                                       next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory collecting symbol reference candidates");
    row->candidates = resized;
    row->candidate_capacity = next;
  }
  row->candidates[row->candidate_count].path = path;
  row->candidates[row->candidate_count].symbol = symbol;
  row->candidates[row->candidate_count].definition = definition;
  row->candidate_count++;
  *added = 1;
  return ARCHBIRD_OK;
}

static int same_source_scope(const AbSymbolReferenceRow *left,
                             const AbSymbolReferenceRow *right) {
  return ab_string_equal(&left->occurrence->path, &right->occurrence->path) &&
         left->source && right->source && source_equal(left, right);
}

static int lookup_compare(const void *left_raw, const void *right_raw) {
  const AbSymbolReferenceRow *left =
      ((const AbSymbolReferenceLookup *)left_raw)->row;
  const AbSymbolReferenceRow *right =
      ((const AbSymbolReferenceLookup *)right_raw)->row;
  int compared =
      ab_string_compare(&left->occurrence->path, &right->occurrence->path);
  if (compared)
    return compared;
  compared = source_compare(left, right);
  if (compared)
    return compared;
  compared =
      ab_string_compare(&left->occurrence->name, &right->occurrence->name);
  if (compared)
    return compared;
  if (left->occurrence->span_start != right->occurrence->span_start)
    return left->occurrence->span_start < right->occurrence->span_start ? -1
                                                                        : 1;
  if (left->occurrence->span_end != right->occurrence->span_end)
    return left->occurrence->span_end < right->occurrence->span_end ? -1 : 1;
  return ab_string_compare(&left->occurrence->id, &right->occurrence->id);
}

static int lookup_key_compare(const AbSymbolReferenceLookup *lookup,
                              const AbSymbolReferenceRow *source) {
  int compared = ab_string_compare(&lookup->row->occurrence->path,
                                   &source->occurrence->path);
  if (compared)
    return compared;
  compared = source_compare(lookup->row, source);
  if (compared)
    return compared;
  return ab_string_compare(&lookup->row->occurrence->name, source->container);
}

static void lookup_range(const AbSymbolReferenceLookup *lookups,
                         size_t lookup_count,
                         const AbSymbolReferenceRow *source, size_t *out_start,
                         size_t *out_end) {
  size_t low = 0;
  size_t high = lookup_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (lookup_key_compare(&lookups[middle], source) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = lookup_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (lookup_key_compare(&lookups[middle], source) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_candidate(AbBuffer *buffer, const AbString *path,
                                       const AbString *symbol,
                                       const AbFact *definition) {
  uint64_t line;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"path\":");
  const AbString *index =
      definition ? ab_map_fact_string_attribute(definition, "index") : NULL;
  const AbString *syntax_recovery =
      definition ? ab_map_fact_string_attribute(definition, "syntax_recovery")
                 : NULL;
  const AbString *variant =
      definition ? ab_map_fact_string_attribute(definition, "variant") : NULL;
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, path);
  if (status == ARCHBIRD_OK && index && index->length) {
    status = ab_buffer_literal(buffer, ",\"index\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, index);
  }
  if (status == ARCHBIRD_OK && definition &&
      ab_map_fact_u64_attribute(definition, "line", &line)) {
    status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, line);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbol\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, symbol);
  if (status == ARCHBIRD_OK && syntax_recovery) {
    status = ab_buffer_literal(buffer, ",\"syntax_recovery\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, syntax_recovery);
  }
  if (status == ARCHBIRD_OK && variant && variant->length) {
    status = ab_buffer_literal(buffer, ",\"variant\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, variant);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_evidence(AbBuffer *buffer, const AbFact *fact,
                                      const AbProviderBundle *provider) {
  uint64_t line;
  uint64_t recovery_nodes;
  const AbString *syntax_recovery =
      ab_map_fact_string_attribute(fact, "syntax_recovery");
  const AbString *index = ab_map_fact_string_attribute(fact, "index");
  const AbString *variant = ab_map_fact_string_attribute(fact, "variant");
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"claim\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, &fact->claim);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"fact_id\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, &fact->id);
  if (status == ARCHBIRD_OK && index && index->length) {
    status = ab_buffer_literal(buffer, ",\"index\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, index);
  }
  if (status == ARCHBIRD_OK && ab_map_fact_u64_attribute(fact, "line", &line)) {
    status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, line);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"provider\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, &provider->producer.name);
  if (status == ARCHBIRD_OK &&
      ab_map_fact_u64_attribute(fact, "recovery_nodes", &recovery_nodes)) {
    status = ab_buffer_literal(buffer, ",\"recovery_nodes\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, recovery_nodes);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"span\":{\"end\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, fact->span_end);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"start\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, fact->span_start);
  if (status == ARCHBIRD_OK && syntax_recovery) {
    status = ab_buffer_literal(buffer, "},\"syntax_recovery\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, syntax_recovery);
  } else if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK && variant && variant->length) {
    status = ab_buffer_literal(buffer, ",\"variant\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, variant);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

ArchbirdStatus ab_map_render_symbol_references(AbBuffer *buffer,
                                               const AbMapState *state) {
  static const AbString declaration_context = {(char *)"declaration", 11};
  size_t total = ab_project_merged_fact_count(state->project);
  AbMapSymbolReference *symbols = NULL;
  AbSymbolReferenceRow *rows = NULL;
  AbSymbolReferenceLookup *lookups = NULL;
  size_t *queue = NULL;
  uint8_t *queued = NULL;
  size_t symbol_count = 0;
  size_t row_count = 0;
  size_t index;
  int first = 1;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (total) {
    symbols = (AbMapSymbolReference *)ab_malloc(state->engine,
                                                total * sizeof(*symbols));
    rows =
        (AbSymbolReferenceRow *)ab_calloc(state->engine, total, sizeof(*rows));
    if (!symbols || !rows) {
      status = archbird_error_set(
          state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory collecting symbol value references");
      goto done;
    }
  }
  for (index = 0; index < total; index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbManifestFile *file;
    if (fact->has_name && string_literal(&fact->domain, "symbols") &&
        (file = manifest_file(state->manifest, &fact->path)) != NULL &&
        file->has_layer) {
      symbols[symbol_count].file = file;
      symbols[symbol_count].fact = fact;
      ab_map_symbol_leaf(fact, &symbols[symbol_count].leaf,
                         &symbols[symbol_count].leaf_length);
      symbol_count++;
    }
    if (fact->has_name && string_literal(&fact->domain, "symbol-references") &&
        string_literal(&fact->kind, "value")) {
      const AbFact *source = ab_map_enclosing_symbol(state->project, fact);
      const AbProviderBundle *provider =
          ab_project_merged_fact_provider(state->project, index);
      const AbManifestFile *source_file =
          manifest_file(state->manifest, &fact->path);
      const AbString *context = ab_map_fact_string_attribute(fact, "context");
      if (provider && source_file && context) {
        rows[row_count].occurrence = fact;
        rows[row_count].provider = provider;
        rows[row_count].source = source;
        rows[row_count].source_file = source_file;
        rows[row_count].context = context;
        rows[row_count].container =
            ab_map_fact_string_attribute(fact, "container");
        rows[row_count].relation = "value";
        row_count++;
      }
    } else if (fact->has_name && string_literal(&fact->domain, "symbols") &&
               string_literal(&fact->kind, "declaration") &&
               (file = manifest_file(state->manifest, &fact->path)) != NULL &&
               file->has_layer) {
      size_t family_length = 0;
      const char *family = ab_map_symbol_language_family(file, &family_length);
      if (family_length == 1 && family[0] == 'c') {
        rows[row_count].occurrence = fact;
        rows[row_count].provider =
            ab_project_merged_fact_provider(state->project, index);
        rows[row_count].source = fact;
        rows[row_count].source_file = file;
        rows[row_count].context = &declaration_context;
        rows[row_count].relation = "declaration-definition";
        row_count++;
      }
    }
  }
  if (symbol_count > 1)
    qsort(symbols, symbol_count, sizeof(*symbols),
          ab_map_symbol_reference_compare);
  for (index = 0; index < row_count; index++) {
    AbSymbolReferenceRow *row = &rows[index];
    size_t candidate_start = 0;
    size_t candidate_end = 0;
    size_t candidate;
    row->semantic =
        string_literal(&row->occurrence->kind, "declaration")
            ? NULL
            : ab_map_unique_semantic_target(state->project, row->occurrence);
    row->semantic_path =
        row->semantic
            ? ab_map_fact_string_attribute(row->semantic, "target_path")
            : NULL;
    row->semantic_symbol =
        row->semantic
            ? ab_map_fact_string_attribute(row->semantic, "target_symbol")
            : NULL;
    if (row->semantic && row->semantic_path && row->semantic_symbol) {
      size_t fact_index;
      for (fact_index = 0; fact_index < total; fact_index++)
        if (ab_project_merged_fact(state->project, fact_index) ==
            row->semantic) {
          row->semantic_provider =
              ab_project_merged_fact_provider(state->project, fact_index);
          break;
        }
      row->candidate_count = 1;
      row->candidate_capacity = 1;
      row->candidates = (AbSymbolReferenceCandidate *)ab_malloc(
          state->engine, sizeof(*row->candidates));
      if (!row->candidates) {
        status = archbird_error_set(
            state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory collecting semantic symbol reference");
        goto done;
      }
      row->candidates[0].path = row->semantic_path;
      row->candidates[0].symbol = row->semantic_symbol;
      row->candidates[0].definition = NULL;
      row->resolution = "unique";
      continue;
    }
    ab_map_symbol_reference_range(symbols, symbol_count, row->source_file,
                                  &row->occurrence->name, &candidate_start,
                                  &candidate_end);
    for (candidate = candidate_start; candidate < candidate_end; candidate++)
      if (ab_map_symbol_reference_usable(&symbols[candidate], row->source_file,
                                         0, 0)) {
        int added;
        status = add_candidate(
            state->engine, row, &symbols[candidate].file->path,
            &symbols[candidate].fact->name, symbols[candidate].fact, &added);
        if (status != ARCHBIRD_OK)
          goto done;
      }
    row->resolution =
        row->candidate_count == 0
            ? "unresolved"
            : (row->candidate_count == 1
                   ? (string_literal(&row->occurrence->kind, "declaration")
                          ? "unique"
                          : "candidate")
                   : "ambiguous");
  }
  if (row_count) {
    size_t head = 0;
    size_t tail = 0;
    size_t queued_count = 0;
    lookups = (AbSymbolReferenceLookup *)ab_malloc(
        state->engine, row_count * sizeof(*lookups));
    queue = (size_t *)ab_malloc(state->engine, row_count * sizeof(*queue));
    queued = (uint8_t *)ab_calloc(state->engine, row_count, sizeof(*queued));
    if (!lookups || !queue || !queued) {
      status = archbird_error_set(
          state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory indexing symbol reference value flow");
      goto done;
    }
    for (index = 0; index < row_count; index++) {
      lookups[index].row = &rows[index];
      if (rows[index].candidate_count) {
        queue[tail] = index;
        tail = (tail + 1) % row_count;
        queued[index] = 1;
        queued_count++;
      }
    }
    if (row_count > 1)
      qsort(lookups, row_count, sizeof(*lookups), lookup_compare);
    while (queued_count) {
      size_t source_index = queue[head];
      AbSymbolReferenceRow *source_row = &rows[source_index];
      size_t target_start;
      size_t target_end;
      size_t target;
      head = (head + 1) % row_count;
      queued[source_index] = 0;
      queued_count--;
      if (!source_row->source || !source_row->container ||
          !source_row->candidate_count)
        continue;
      lookup_range(lookups, row_count, source_row, &target_start, &target_end);
      for (target = target_start; target < target_end; target++) {
        AbSymbolReferenceRow *target_row = lookups[target].row;
        size_t target_index = (size_t)(target_row - rows);
        size_t candidate;
        int changed = 0;
        if (target_row->semantic || !same_source_scope(target_row, source_row))
          continue;
        for (candidate = 0; candidate < source_row->candidate_count;
             candidate++) {
          int added;
          status = add_candidate(
              state->engine, target_row, source_row->candidates[candidate].path,
              source_row->candidates[candidate].symbol,
              source_row->candidates[candidate].definition, &added);
          if (status != ARCHBIRD_OK)
            goto done;
          changed |= added;
        }
        if (changed && !queued[target_index]) {
          queue[tail] = target_index;
          tail = (tail + 1) % row_count;
          queued[target_index] = 1;
          queued_count++;
        }
      }
    }
    for (index = 0; index < row_count; index++) {
      AbSymbolReferenceRow *row = &rows[index];
      if (row->candidate_count > 1)
        qsort(row->candidates, row->candidate_count, sizeof(*row->candidates),
              candidate_compare);
      if (!row->semantic)
        row->resolution =
            row->candidate_count == 0
                ? "unresolved"
                : (row->candidate_count == 1
                       ? (string_literal(&row->occurrence->kind, "declaration")
                              ? "unique"
                              : "candidate")
                       : "ambiguous");
    }
  }
  if (row_count > 1)
    qsort(rows, row_count, sizeof(*rows), row_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < row_count;) {
    const AbSymbolReferenceRow *row = &rows[index];
    size_t group_end = index + 1;
    size_t candidate;
    if (!row->candidate_count) {
      index++;
      continue;
    }
    while (group_end < row_count && same_group(row, &rows[group_end]))
      group_end++;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"candidates\":[");
    for (candidate = 0;
         status == ARCHBIRD_OK && candidate < row->candidate_count;
         candidate++) {
      if (candidate)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = render_candidate(buffer, row->candidates[candidate].path,
                                  row->candidates[candidate].symbol,
                                  row->candidates[candidate].definition);
    }
    if (status == ARCHBIRD_OK && row->container) {
      status = ab_buffer_literal(buffer, "],\"container\":");
      if (status == ARCHBIRD_OK)
        status = render_string(buffer, row->container);
    } else if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(buffer, "]");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"context\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, row->context);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":[");
    for (candidate = index; status == ARCHBIRD_OK && candidate < group_end;
         candidate++) {
      const AbSymbolReferenceRow *occurrence = &rows[candidate];
      if (candidate != index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = render_evidence(buffer, occurrence->occurrence,
                                 occurrence->provider);
      if (status == ARCHBIRD_OK && occurrence->semantic &&
          occurrence->semantic_provider) {
        status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = render_evidence(buffer, occurrence->semantic,
                                   occurrence->semantic_provider);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"name\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->occurrence->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"relation\":\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, row->relation);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\",\"resolution\":\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, row->resolution);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\",\"source\":{\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, &row->occurrence->path);
    if (status == ARCHBIRD_OK && row->source)
      status = ab_buffer_literal(buffer, ",\"symbol\":");
    if (status == ARCHBIRD_OK && row->source)
      status = render_string(buffer, &row->source->name);
    if (status == ARCHBIRD_OK && !row->source)
      status = ab_buffer_literal(buffer, ",\"scope\":\"");
    if (status == ARCHBIRD_OK && !row->source)
      status = ab_buffer_literal(
          buffer,
          ab_manifest_file_has_role(row->source_file, "test") ||
                  ab_manifest_file_has_role(row->source_file, "test-candidate")
              ? "test-file"
              : "file");
    if (status == ARCHBIRD_OK && !row->source)
      status = ab_buffer_literal(buffer, "\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}}");
    first = 0;
    index = group_end;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
done:
  ab_free(state->engine, queued);
  ab_free(state->engine, queue);
  ab_free(state->engine, lookups);
  if (rows)
    for (index = 0; index < row_count; index++)
      ab_free(state->engine, rows[index].candidates);
  ab_free(state->engine, rows);
  ab_free(state->engine, symbols);
  return status;
}
