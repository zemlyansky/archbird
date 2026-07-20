#include "map_internal.h"

#include "archbird_internal.h"

#include <stdlib.h>
#include <string.h>

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
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

static const AbString *string_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_STRING ? &field->value.as.text
                                                       : NULL;
}

static ArchbirdStatus integer_attribute(AbMapState *state, const AbFact *fact,
                                        const char *name, size_t *out) {
  const AbObjectField *field = fact_attribute(fact, name);
  size_t value = 0;
  size_t index;
  if (!field || field->value.kind != AB_VALUE_INTEGER)
    return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                              ARCHBIRD_NO_OFFSET,
                              "index summary is missing an integer field");
  for (index = 0; index < field->value.as.text.length; index++) {
    unsigned char byte = (unsigned char)field->value.as.text.data[index];
    if (byte < '0' || byte > '9' || value > (SIZE_MAX - (byte - '0')) / 10)
      return archbird_error_set(state->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "index summary integer exceeds native size");
    value = value * 10 + (byte - '0');
  }
  *out = value;
  return ARCHBIRD_OK;
}

static int fact_domain(const AbFact *fact, const char *domain) {
  return string_literal(&fact->domain, domain);
}

static ArchbirdStatus copy_attribute(AbMapState *state, const AbFact *fact,
                                     const char *name, AbString *out) {
  const AbString *value = string_attribute(fact, name);
  if (!value)
    return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                              ARCHBIRD_NO_OFFSET,
                              "index summary is missing a string field");
  return ab_string_copy(state->engine, out, value->data, value->length);
}

static const AbFact *summary_fact(const AbMapState *state,
                                  const AbConfigIndex *spec) {
  size_t index;
  for (index = 0; index < ab_project_merged_fact_count(state->project);
       index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    if (fact->has_name && fact_domain(fact, "index-summaries") &&
        ab_string_equal(&fact->name, &spec->name))
      return fact;
  }
  return NULL;
}

static ArchbirdStatus add_index_diagnostic(AbMapState *state,
                                           const AbConfigIndex *spec,
                                           const char *severity,
                                           const char *code,
                                           const char *suffix) {
  AbBuffer message;
  ArchbirdStatus status;
  ab_buffer_init(&message, state->engine);
  status = ab_buffer_append(&message, spec->name.data, spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, ": ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, suffix);
  if (status == ARCHBIRD_OK)
    status = ab_map_add_diagnostic(state, severity, code,
                                   (const char *)message.data, &spec->path);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus decode_summary(AbMapState *state,
                                     const AbConfigIndex *spec,
                                     const AbFact *fact, AbMapIndex *out) {
  const AbString *format = string_attribute(fact, "format");
  const AbString *path = string_attribute(fact, "index_path");
  const AbString *prefix = string_attribute(fact, "path_prefix");
  const AbString *variant = string_attribute(fact, "variant");
  ArchbirdStatus status;
  if (!format || !path || !prefix || !variant ||
      !ab_string_equal(format, &spec->format) ||
      !ab_string_equal(path, &spec->path) ||
      !ab_string_equal(prefix, &spec->path_prefix) ||
      !ab_string_equal(variant, &spec->variant))
    return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                              ARCHBIRD_NO_OFFSET,
                              "index summary does not match configured index");
  status = ab_string_copy(state->engine, &out->name, spec->name.data,
                          spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->format, spec->format.data,
                            spec->format.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->path, spec->path.data,
                            spec->path.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->path_prefix,
                            spec->path_prefix.data, spec->path_prefix.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->variant, spec->variant.data,
                            spec->variant.length);
  if (status == ARCHBIRD_OK)
    status =
        copy_attribute(state, fact, "evidence_state", &out->evidence_state);
  if (status == ARCHBIRD_OK)
    status = copy_attribute(state, fact, "sha256", &out->sha256);
  if (status == ARCHBIRD_OK)
    status = copy_attribute(state, fact, "tool_name", &out->tool_name);
  if (status == ARCHBIRD_OK)
    status = copy_attribute(state, fact, "tool_version", &out->tool_version);
  out->position_encoding_fallback = spec->position_encoding_fallback;
#define INDEX_INTEGER(field)                                                   \
  if (status == ARCHBIRD_OK)                                                   \
  status = integer_attribute(state, fact, #field, &out->field)
  INDEX_INTEGER(documents_total);
  INDEX_INTEGER(documents_mapped);
  INDEX_INTEGER(documents_stale);
  INDEX_INTEGER(documents_source_unverified);
  INDEX_INTEGER(documents_source_verified);
  INDEX_INTEGER(symbols);
  INDEX_INTEGER(occurrences);
  INDEX_INTEGER(invalid_ranges);
  INDEX_INTEGER(source_mismatches);
  INDEX_INTEGER(position_encoding_fallback_documents);
  INDEX_INTEGER(references);
  INDEX_INTEGER(reference_facts);
  INDEX_INTEGER(resolved_unique);
  INDEX_INTEGER(resolved_ambiguous);
  INDEX_INTEGER(unresolved);
  INDEX_INTEGER(relationships);
  INDEX_INTEGER(relationship_edges);
  INDEX_INTEGER(edge_count);
#undef INDEX_INTEGER
  return status;
}

static ArchbirdStatus add_edges(AbMapState *state, const AbConfigIndex *spec) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < ab_project_merged_fact_count(state->project);
       index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbString *index_name;
    const AbString *source;
    const AbString *target;
    const AbManifestFile *source_file;
    const AbManifestFile *target_file;
    if (!fact->has_name || !fact_domain(fact, "index-edges"))
      continue;
    index_name = string_attribute(fact, "index");
    if (!index_name || !ab_string_equal(index_name, &spec->name))
      continue;
    source = string_attribute(fact, "source");
    target = string_attribute(fact, "target");
    if (!source || !target)
      return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                                ARCHBIRD_NO_OFFSET,
                                "index edge is missing source or target");
    source_file =
        ab_map_manifest_file(state->manifest, source->data, source->length);
    target_file =
        ab_map_manifest_file(state->manifest, target->data, target->length);
    if (!source_file || !target_file || !source_file->has_layer ||
        !target_file->has_layer)
      return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                                ARCHBIRD_NO_OFFSET,
                                "index edge targets unmapped source evidence");
    status = ab_map_graph_add_edge(
        state->engine, &state->graph, fact->kind.data, &source_file->path,
        target_file->path.data, target_file->path.length, &fact->name);
  }
  return status;
}

static ArchbirdStatus add_provider_diagnostics(AbMapState *state,
                                               const AbConfigIndex *spec) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < ab_project_merged_fact_count(state->project);
       index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbString *index_name;
    const AbString *severity;
    const AbString *code;
    const AbString *message;
    const AbString *path;
    if (!fact_domain(fact, "index-diagnostics"))
      continue;
    index_name = string_attribute(fact, "index");
    if (!index_name || !ab_string_equal(index_name, &spec->name))
      continue;
    severity = string_attribute(fact, "severity");
    code = string_attribute(fact, "code");
    message = string_attribute(fact, "message");
    path = string_attribute(fact, "diagnostic_path");
    if (!severity || !code || !message || !path)
      return archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                                ARCHBIRD_NO_OFFSET,
                                "index diagnostic has incomplete fields");
    status = ab_map_add_diagnostic(state, severity->data, code->data,
                                   message->data, path->length ? path : NULL);
  }
  return status;
}

ArchbirdStatus ab_map_analyze_indexes(AbMapState *state) {
  size_t spec_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (spec_index = 0;
       status == ARCHBIRD_OK && spec_index < state->config->index_count;
       spec_index++) {
    const AbConfigIndex *spec = &state->config->indexes[spec_index];
    const AbManifestFile *file = ab_map_manifest_file(
        state->manifest, spec->path.data, spec->path.length);
    const AbFact *summary;
    AbMapIndex *resized;
    AbMapIndex *index;
    if (!file) {
      status = add_index_diagnostic(state, spec,
                                    spec->required ? "error" : "warning",
                                    "missing-index", spec->format.data);
      continue;
    }
    summary = summary_fact(state, spec);
    if (!summary) {
      status = add_index_diagnostic(
          state, spec, spec->required ? "error" : "warning",
          "index-provider-unavailable",
          "analyze through archbird.pipeline or the CLI");
      continue;
    }
    resized = (AbMapIndex *)ab_realloc(state->engine, state->indexes,
                                       (state->index_count + 1) *
                                           sizeof(*state->indexes));
    if (!resized)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting index summaries");
    state->indexes = resized;
    index = &state->indexes[state->index_count++];
    memset(index, 0, sizeof(*index));
    status = decode_summary(state, spec, summary, index);
    if (status == ARCHBIRD_OK)
      status = add_edges(state, spec);
    if (status == ARCHBIRD_OK)
      status = add_provider_diagnostics(state, spec);
  }
  if (status == ARCHBIRD_OK)
    ab_map_graph_sort(&state->graph);
  return status;
}

ArchbirdStatus ab_map_render_indexes(AbBuffer *buffer,
                                     const AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->index_count;
       index++) {
    const AbMapIndex *row = &state->indexes[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_literal(buffer, "{\"coverage\":{\"documents_mapped\":");
#define RENDER_COUNT(label, field)                                             \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_u64(buffer, row->field);                              \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_literal(buffer, label);                               \
  } while (0)
    RENDER_COUNT(",\"documents_stale\":", documents_mapped);
    RENDER_COUNT(",\"documents_total\":", documents_stale);
    RENDER_COUNT(",\"documents_source_unverified\":", documents_total);
    RENDER_COUNT(",\"documents_source_verified\":",
                 documents_source_unverified);
    RENDER_COUNT(",\"edges\":", documents_source_verified);
    RENDER_COUNT(",\"invalid_ranges\":", edge_count);
    RENDER_COUNT(",\"occurrences\":", invalid_ranges);
    RENDER_COUNT(",\"position_encoding_fallback_documents\":", occurrences);
    RENDER_COUNT(",\"references\":", position_encoding_fallback_documents);
    RENDER_COUNT(",\"reference_facts\":", references);
    RENDER_COUNT(",\"relationship_edges\":", reference_facts);
    RENDER_COUNT(",\"relationships\":", relationship_edges);
    RENDER_COUNT(",\"resolved_ambiguous\":", relationships);
    RENDER_COUNT(",\"resolved_unique\":", resolved_ambiguous);
    RENDER_COUNT(",\"source_mismatches\":", resolved_unique);
    RENDER_COUNT(",\"symbols\":", source_mismatches);
    RENDER_COUNT(",\"unresolved\":", symbols);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->unresolved);
#undef RENDER_COUNT
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"format\":");
#define RENDER_STRING(label, field)                                            \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status =                                                                 \
          ab_buffer_json_string(buffer, row->field.data, row->field.length);   \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_literal(buffer, label);                               \
  } while (0)
    RENDER_STRING(",\"evidence_state\":", format);
    RENDER_STRING(",\"name\":", evidence_state);
    RENDER_STRING(",\"path\":", name);
    RENDER_STRING(",\"path_prefix\":", path);
    RENDER_STRING(",\"position_encoding_fallback\":", path_prefix);
    if (status == ARCHBIRD_OK) {
      static const char *const encodings[] = {"", "utf8", "utf16", "utf32"};
      if (row->position_encoding_fallback <= 3)
        status = row->position_encoding_fallback
                     ? ab_buffer_json_string(
                           buffer, encodings[row->position_encoding_fallback],
                           strlen(encodings[row->position_encoding_fallback]))
                     : ab_buffer_literal(buffer, "null");
      else
        status = archbird_error_set(
            state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "index has an invalid position encoding fallback");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"sha256\":");
    RENDER_STRING(",\"tool\":{\"name\":", sha256);
    RENDER_STRING(",\"version\":", tool_name);
    RENDER_STRING("}", tool_version);
    if (status == ARCHBIRD_OK && row->variant.length) {
      status = ab_buffer_literal(buffer, ",\"variant\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, row->variant.data,
                                       row->variant.length);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
#undef RENDER_STRING
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
