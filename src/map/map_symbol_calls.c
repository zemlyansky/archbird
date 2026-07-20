#include "map_internal.h"

#include "archbird_internal.h"

#include <stdlib.h>
#include <string.h>

typedef AbMapSymbolReference AbSymbolCallSymbol;

typedef struct AbSymbolCallRow {
  const AbFact *call;
  const AbProviderBundle *provider;
  const AbFact *source;
  const AbManifestFile *source_file;
  const AbFact *semantic;
  const AbProviderBundle *semantic_provider;
  const AbString *semantic_path;
  const AbString *semantic_symbol;
  size_t candidate_start;
  size_t candidate_end;
  size_t candidate_count;
  const char *resolution;
} AbSymbolCallRow;

static int bytes_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int row_compare(const void *left_raw, const void *right_raw) {
  const AbSymbolCallRow *left = (const AbSymbolCallRow *)left_raw;
  const AbSymbolCallRow *right = (const AbSymbolCallRow *)right_raw;
  int compared = ab_string_compare(&left->call->path, &right->call->path);
  if (compared)
    return compared;
  if (!left->source || !right->source)
    compared = (left->source != NULL) - (right->source != NULL);
  else
    compared = ab_string_compare(&left->source->name, &right->source->name);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->call->name, &right->call->name);
  if (compared)
    return compared;
  compared = strcmp(left->resolution, right->resolution);
  if (compared)
    return compared < 0 ? -1 : 1;
  if (left->semantic_path || right->semantic_path) {
    if (!left->semantic_path)
      return -1;
    if (!right->semantic_path)
      return 1;
    compared = ab_string_compare(left->semantic_path, right->semantic_path);
    if (compared)
      return compared;
    compared = ab_string_compare(left->semantic_symbol, right->semantic_symbol);
    if (compared)
      return compared;
  }
  if (left->call->span_start != right->call->span_start)
    return left->call->span_start < right->call->span_start ? -1 : 1;
  if (left->call->span_end != right->call->span_end)
    return left->call->span_end < right->call->span_end ? -1 : 1;
  return ab_string_compare(&left->call->id, &right->call->id);
}

static int same_group(const AbSymbolCallRow *left,
                      const AbSymbolCallRow *right) {
  if (!ab_string_equal(&left->call->path, &right->call->path) ||
      (left->source != NULL) != (right->source != NULL) ||
      (left->source &&
       !ab_string_equal(&left->source->name, &right->source->name)) ||
      !ab_string_equal(&left->call->name, &right->call->name) ||
      strcmp(left->resolution, right->resolution) ||
      left->candidate_count != right->candidate_count)
    return 0;
  if (!left->semantic_path || !right->semantic_path)
    return left->semantic_path == right->semantic_path;
  return ab_string_equal(left->semantic_path, right->semantic_path) &&
         ab_string_equal(left->semantic_symbol, right->semantic_symbol);
}

static const AbManifestFile *manifest_file(const AbSourceManifest *manifest,
                                           const AbString *path) {
  return ab_map_manifest_file(manifest, path->data, path->length);
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_candidate(AbBuffer *buffer, const AbString *path,
                                       const AbString *symbol,
                                       const AbFact *fact) {
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"path\":");
  const AbString *index =
      fact ? ab_map_fact_string_attribute(fact, "index") : NULL;
  const AbString *syntax_recovery =
      fact ? ab_map_fact_string_attribute(fact, "syntax_recovery") : NULL;
  const AbString *variant =
      fact ? ab_map_fact_string_attribute(fact, "variant") : NULL;
  uint64_t line;
  if (status == ARCHBIRD_OK)
    status = json_string(buffer, path);
  if (status == ARCHBIRD_OK && index && index->length) {
    status = ab_buffer_literal(buffer, ",\"index\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, index);
  }
  if (status == ARCHBIRD_OK && fact &&
      ab_map_fact_u64_attribute(fact, "line", &line)) {
    status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, line);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbol\":");
  if (status == ARCHBIRD_OK)
    status = json_string(buffer, symbol);
  if (status == ARCHBIRD_OK && syntax_recovery) {
    status = ab_buffer_literal(buffer, ",\"syntax_recovery\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, syntax_recovery);
  }
  if (status == ARCHBIRD_OK && variant && variant->length) {
    status = ab_buffer_literal(buffer, ",\"variant\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, variant);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_evidence(AbBuffer *buffer, const AbFact *fact,
                                      const AbProviderBundle *provider) {
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"claim\":");
  uint64_t line;
  uint64_t recovery_nodes;
  const AbString *syntax_recovery =
      ab_map_fact_string_attribute(fact, "syntax_recovery");
  const AbString *index = ab_map_fact_string_attribute(fact, "index");
  const AbString *variant = ab_map_fact_string_attribute(fact, "variant");
  if (status == ARCHBIRD_OK)
    status = json_string(buffer, &fact->claim);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"fact_id\":");
  if (status == ARCHBIRD_OK)
    status = json_string(buffer, &fact->id);
  if (status == ARCHBIRD_OK && index && index->length) {
    status = ab_buffer_literal(buffer, ",\"index\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, index);
  }
  if (status == ARCHBIRD_OK && ab_map_fact_u64_attribute(fact, "line", &line)) {
    status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, line);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"provider\":");
  if (status == ARCHBIRD_OK)
    status = json_string(buffer, &provider->producer.name);
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
      status = json_string(buffer, syntax_recovery);
  } else if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK && variant && variant->length) {
    status = ab_buffer_literal(buffer, ",\"variant\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, variant);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

ArchbirdStatus ab_map_render_symbol_calls(AbBuffer *buffer,
                                          const AbMapState *state) {
  size_t total = ab_project_merged_fact_count(state->project);
  AbSymbolCallSymbol *symbols = NULL;
  AbSymbolCallRow *rows = NULL;
  size_t symbol_count = 0;
  size_t row_count = 0;
  size_t index;
  int first = 1;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (total) {
    symbols = (AbSymbolCallSymbol *)ab_malloc(state->engine,
                                              total * sizeof(*symbols));
    rows = (AbSymbolCallRow *)ab_calloc(state->engine, total, sizeof(*rows));
    if (!symbols || !rows) {
      status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory collecting symbol calls");
      goto done;
    }
  }
  for (index = 0; index < total; index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbManifestFile *file;
    if (fact->has_name && bytes_literal(&fact->domain, "symbols") &&
        (file = manifest_file(state->manifest, &fact->path)) != NULL &&
        file->has_layer) {
      symbols[symbol_count].file = file;
      symbols[symbol_count].fact = fact;
      ab_map_symbol_leaf(fact, &symbols[symbol_count].leaf,
                         &symbols[symbol_count].leaf_length);
      symbol_count++;
    }
    if (fact->has_name && bytes_literal(&fact->domain, "calls")) {
      const AbFact *source = ab_map_enclosing_symbol(state->project, fact);
      const AbProviderBundle *provider =
          ab_project_merged_fact_provider(state->project, index);
      const AbManifestFile *source_file =
          manifest_file(state->manifest, &fact->path);
      if (provider && source_file) {
        rows[row_count].call = fact;
        rows[row_count].provider = provider;
        rows[row_count].source = source;
        rows[row_count].source_file = source_file;
        row_count++;
      }
    }
  }
  if (symbol_count > 1)
    qsort(symbols, symbol_count, sizeof(*symbols),
          ab_map_symbol_reference_compare);
  for (index = 0; index < row_count; index++) {
    AbSymbolCallRow *row = &rows[index];
    const AbString *binding =
        ab_map_fact_string_attribute(row->call, "binding");
    size_t candidate;
    row->semantic = ab_map_unique_semantic_target(state->project, row->call);
    row->semantic_path =
        row->semantic
            ? ab_map_fact_string_attribute(row->semantic, "target_path")
            : NULL;
    row->semantic_symbol =
        row->semantic
            ? ab_map_fact_string_attribute(row->semantic, "target_symbol")
            : NULL;
    if (binding && bytes_literal(binding, "builtin")) {
      row->resolution = "builtin";
    } else if (row->semantic && row->semantic_path && row->semantic_symbol) {
      size_t fact_index;
      for (fact_index = 0; fact_index < total; fact_index++)
        if (ab_project_merged_fact(state->project, fact_index) ==
            row->semantic) {
          row->semantic_provider =
              ab_project_merged_fact_provider(state->project, fact_index);
          break;
        }
      row->candidate_count = 1;
      row->resolution = "unique";
    } else {
      ab_map_symbol_reference_range(symbols, symbol_count, row->source_file,
                                    &row->call->name, &row->candidate_start,
                                    &row->candidate_end);
      for (candidate = row->candidate_start; candidate < row->candidate_end;
           candidate++)
        if (ab_map_symbol_reference_usable(&symbols[candidate],
                                           row->source_file, 0, 0))
          row->candidate_count++;
      row->resolution =
          row->candidate_count == 0
              ? "unresolved"
              : (row->candidate_count == 1 ? "candidate" : "ambiguous");
    }
  }
  if (row_count > 1)
    qsort(rows, row_count, sizeof(*rows), row_compare);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < row_count;) {
    const AbSymbolCallRow *row = &rows[index];
    const AbFact *semantic_definition = NULL;
    size_t group_end = index + 1;
    size_t candidate;
    if (!row->source && !row->candidate_count && !row->semantic) {
      index++;
      continue;
    }
    while (group_end < row_count && same_group(row, &rows[group_end]))
      group_end++;
    if (row->semantic) {
      size_t symbol_index;
      for (symbol_index = 0; symbol_index < symbol_count; symbol_index++) {
        if (!ab_string_equal(&symbols[symbol_index].file->path,
                             row->semantic_path) ||
            !ab_string_equal(&symbols[symbol_index].fact->name,
                             row->semantic_symbol) ||
            bytes_literal(&symbols[symbol_index].fact->kind, "declaration"))
          continue;
        if (semantic_definition) {
          semantic_definition = NULL;
          break;
        }
        semantic_definition = symbols[symbol_index].fact;
      }
    }
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"candidates\":[");
    if (status == ARCHBIRD_OK && row->semantic)
      status = render_candidate(buffer, row->semantic_path,
                                row->semantic_symbol, semantic_definition);
    if (status == ARCHBIRD_OK && !row->semantic &&
        strcmp(row->resolution, "builtin")) {
      size_t rendered = 0;
      for (candidate = row->candidate_start;
           status == ARCHBIRD_OK && candidate < row->candidate_end;
           candidate++) {
        if (!ab_map_symbol_reference_usable(&symbols[candidate],
                                            row->source_file, 0, 0))
          continue;
        if (rendered++)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = render_candidate(buffer, &symbols[candidate].file->path,
                                    &symbols[candidate].fact->name,
                                    symbols[candidate].fact);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"evidence\":[");
    for (candidate = index; status == ARCHBIRD_OK && candidate < group_end;
         candidate++) {
      const AbSymbolCallRow *occurrence = &rows[candidate];
      if (candidate != index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status =
            render_evidence(buffer, occurrence->call, occurrence->provider);
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
      status = json_string(buffer, &row->call->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"resolution\":\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, row->resolution);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\",\"source\":{\"path\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &row->call->path);
    if (status == ARCHBIRD_OK && row->source)
      status = ab_buffer_literal(buffer, ",\"symbol\":");
    if (status == ARCHBIRD_OK && row->source)
      status = json_string(buffer, &row->source->name);
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
  ab_free(state->engine, rows);
  ab_free(state->engine, symbols);
  return status;
}
