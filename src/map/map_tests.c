#include "map_internal.h"
#include "map_references.h"

#include "archbird_internal.h"
#include "json_value.h"
#include "lexical/tokenizer.h"
#include "sha256.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct CaseRange {
  AbString selector;
  AbString callback;
  const char *evidence_kind;
  size_t line;
  size_t start;
  size_t end;
} CaseRange;

typedef struct JsSuiteRange {
  const char *name;
  size_t name_length;
  size_t opening;
  size_t closing;
} JsSuiteRange;

typedef struct ConfiguredRoute {
  const AbConfigTestCaseRoute *config;
  AbStringArray targets;
  int matched;
} ConfiguredRoute;

typedef struct RouteSymbol {
  const AbManifestFile *file;
  const AbFact *fact;
  const AbProviderBundle *provider;
  const char *leaf;
  size_t leaf_length;
} RouteSymbol;

typedef struct TestFactIndex {
  const AbFact **facts;
  const AbProviderBundle **providers;
  size_t *file_offsets;
  RouteSymbol *symbols;
  size_t symbol_count;
} TestFactIndex;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
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

static size_t fact_line(const AbFact *fact) {
  const AbObjectField *field = fact_attribute(fact, "line");
  size_t value = 0;
  size_t index;
  if (!field || field->value.kind != AB_VALUE_INTEGER)
    return 0;
  for (index = 0; index < field->value.as.text.length; index++) {
    unsigned char byte = (unsigned char)field->value.as.text.data[index];
    if (byte < '0' || byte > '9' || value > (SIZE_MAX - (byte - '0')) / 10)
      return 0;
    value = value * 10 + (byte - '0');
  }
  return value;
}

static int fact_domain(const AbFact *fact, const char *domain) {
  return string_literal(&fact->domain, domain);
}

static const AbManifestFile *fact_file(const AbMapState *state,
                                       const AbFact *fact) {
  return ab_map_manifest_file(state->manifest, fact->path.data,
                              fact->path.length);
}

static int array_contains(const AbStringArray *array, const AbString *value) {
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (ab_string_equal(&array->items[index], value))
      return 1;
  }
  return 0;
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
                              "out of memory collecting test evidence");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static void fact_leaf(const AbFact *fact, const char **out_data,
                      size_t *out_length) {
  size_t start = fact->name.length;
  while (start && fact->name.data[start - 1] != '.')
    start--;
  *out_data = fact->name.data + start;
  *out_length = fact->name.length - start;
}

static int prefix_bytes(const char *data, size_t length, const char *prefix) {
  size_t wanted = strlen(prefix);
  return wanted <= length && memcmp(data, prefix, wanted) == 0;
}

static const char *language_family(const AbString *language,
                                   size_t *out_length) {
  if (string_literal(language, "c") || string_literal(language, "cpp")) {
    *out_length = 1;
    return "c";
  }
  if (string_literal(language, "javascript") ||
      string_literal(language, "typescript") ||
      string_literal(language, "vue")) {
    *out_length = 10;
    return "javascript";
  }
  *out_length = language->length;
  return language->data;
}

static int bytes_compare(const char *left, size_t left_length,
                         const char *right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared)
    return compared < 0 ? -1 : 1;
  return (left_length > right_length) - (left_length < right_length);
}

static int route_symbol_compare(const void *left_raw, const void *right_raw) {
  const RouteSymbol *left = (const RouteSymbol *)left_raw;
  const RouteSymbol *right = (const RouteSymbol *)right_raw;
  size_t left_family_length;
  size_t right_family_length;
  const char *left_family =
      language_family(&left->file->language, &left_family_length);
  const char *right_family =
      language_family(&right->file->language, &right_family_length);
  int compared = bytes_compare(left_family, left_family_length, right_family,
                               right_family_length);
  if (compared)
    return compared;
  compared = bytes_compare(left->leaf, left->leaf_length, right->leaf,
                           right->leaf_length);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->file->path, &right->file->path);
  return compared ? compared
                  : ab_string_compare(&left->fact->id, &right->fact->id);
}

static int route_symbol_key_compare(const RouteSymbol *symbol,
                                    const AbString *language,
                                    const AbString *name) {
  size_t symbol_family_length;
  size_t wanted_family_length;
  const char *symbol_family =
      language_family(&symbol->file->language, &symbol_family_length);
  const char *wanted_family = language_family(language, &wanted_family_length);
  int compared = bytes_compare(symbol_family, symbol_family_length,
                               wanted_family, wanted_family_length);
  return compared ? compared
                  : bytes_compare(symbol->leaf, symbol->leaf_length, name->data,
                                  name->length);
}

static void test_fact_index_free(ArchbirdEngine *engine, TestFactIndex *index) {
  ab_free(engine, index->facts);
  ab_free(engine, index->providers);
  ab_free(engine, index->file_offsets);
  ab_free(engine, index->symbols);
  memset(index, 0, sizeof(*index));
}

static ArchbirdStatus test_fact_index_build(AbMapState *state,
                                            TestFactIndex *index) {
  size_t total = ab_project_merged_fact_count(state->project);
  size_t manifest_count = state->manifest->file_count;
  size_t mapped_count = 0;
  size_t symbol_count = 0;
  size_t fact_index;
  size_t file_index;
  size_t *cursor = NULL;
  memset(index, 0, sizeof(*index));
  index->file_offsets = (size_t *)ab_calloc(state->engine, manifest_count + 1,
                                            sizeof(*index->file_offsets));
  if (!index->file_offsets)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing test evidence");
  for (fact_index = 0; fact_index < total; fact_index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
    const AbManifestFile *file = fact_file(state, fact);
    if (!file)
      continue;
    file_index = (size_t)(file - state->manifest->files);
    index->file_offsets[file_index + 1]++;
    mapped_count++;
    if (fact->has_name && fact_domain(fact, "symbols") &&
        !string_literal(&fact->kind, "declaration") && file->has_language)
      symbol_count++;
  }
  for (file_index = 1; file_index <= manifest_count; file_index++)
    index->file_offsets[file_index] += index->file_offsets[file_index - 1];
  if (mapped_count) {
    index->facts = (const AbFact **)ab_malloc(
        state->engine, mapped_count * sizeof(*index->facts));
    index->providers = (const AbProviderBundle **)ab_malloc(
        state->engine, mapped_count * sizeof(*index->providers));
    cursor =
        (size_t *)ab_malloc(state->engine, manifest_count * sizeof(*cursor));
    if (!index->facts || !index->providers || !cursor) {
      ab_free(state->engine, cursor);
      test_fact_index_free(state->engine, index);
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing indexed test facts");
    }
    memcpy(cursor, index->file_offsets, manifest_count * sizeof(*cursor));
  }
  if (symbol_count) {
    index->symbols = (RouteSymbol *)ab_malloc(
        state->engine, symbol_count * sizeof(*index->symbols));
    if (!index->symbols) {
      ab_free(state->engine, cursor);
      test_fact_index_free(state->engine, index);
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing test symbol index");
    }
  }
  for (fact_index = 0; fact_index < total; fact_index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
    const AbProviderBundle *provider =
        ab_project_merged_fact_provider(state->project, fact_index);
    const AbManifestFile *file = fact_file(state, fact);
    if (!file)
      continue;
    file_index = (size_t)(file - state->manifest->files);
    index->facts[cursor[file_index]] = fact;
    index->providers[cursor[file_index]] =
        ab_project_merged_fact_provider(state->project, fact_index);
    cursor[file_index]++;
    if (fact->has_name && fact_domain(fact, "symbols") &&
        !string_literal(&fact->kind, "declaration") && file->has_language) {
      RouteSymbol *symbol = &index->symbols[index->symbol_count++];
      symbol->file = file;
      symbol->fact = fact;
      symbol->provider = provider;
      fact_leaf(fact, &symbol->leaf, &symbol->leaf_length);
    }
  }
  ab_free(state->engine, cursor);
  if (index->symbol_count > 1)
    qsort(index->symbols, index->symbol_count, sizeof(*index->symbols),
          route_symbol_compare);
  return ARCHBIRD_OK;
}

static int route_file_allowed(const AbConfigTest *spec,
                              const AbManifestFile *file) {
  return file->has_layer &&
         (spec->discovered || array_contains(&spec->route_to, &file->layer));
}

static int test_path_matches(const AbConfigTest *spec,
                             const AbManifestFile *file) {
  size_t index;
  for (index = 0; index < spec->globs.count; index++) {
    if (ab_map_collection_match(&file->path, &spec->globs.items[index]))
      return 1;
  }
  return 0;
}

static int configured_symbol_matches(const AbConfigTestCaseRoute *route,
                                     const AbFact *fact) {
  size_t index;
  if (!route->target_symbols.count || !fact->has_name ||
      !fact_domain(fact, "symbols") ||
      string_literal(&fact->kind, "declaration"))
    return 0;
  for (index = 0; index < route->target_symbols.count; index++)
    if (ab_map_glob_match(&route->target_symbols.items[index], &fact->name))
      return 1;
  return 0;
}

static int configured_file_has_symbol(const TestFactIndex *index,
                                      const AbManifestFile *file,
                                      const AbConfigTestCaseRoute *route) {
  size_t fact_index;
  for (fact_index = 0; fact_index < index->symbol_count; fact_index++)
    if (index->symbols[fact_index].file == file &&
        configured_symbol_matches(route, index->symbols[fact_index].fact))
      return 1;
  return 0;
}

static ArchbirdStatus add_route(AbMapState *state, AbMapRouteCount **routes,
                                size_t *count, const AbString *path,
                                size_t amount) {
  AbMapRouteCount *resized;
  size_t index;
  for (index = 0; index < *count; index++) {
    if (ab_string_equal(&(*routes)[index].path, path)) {
      if ((*routes)[index].count > SIZE_MAX - amount)
        return archbird_error_set(state->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "test route count overflow");
      (*routes)[index].count += amount;
      return ARCHBIRD_OK;
    }
  }
  resized = (AbMapRouteCount *)ab_realloc(state->engine, *routes,
                                          (*count + 1) * sizeof(**routes));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting test routes");
  *routes = resized;
  memset(&(*routes)[*count], 0, sizeof(**routes));
  if (ab_string_copy(state->engine, &(*routes)[*count].path, path->data,
                     path->length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  (*routes)[*count].count = amount;
  (*count)++;
  return ARCHBIRD_OK;
}

static void route_evidence_row_clear(ArchbirdEngine *engine,
                                     AbMapRouteEvidence *row) {
  ab_string_free(engine, &row->target);
  ab_string_free(engine, &row->target_symbol);
  ab_string_free(engine, &row->fact_id);
  ab_string_free(engine, &row->relation);
  ab_string_free(engine, &row->provenance);
  ab_string_free(engine, &row->provider);
  ab_string_free(engine, &row->claim);
  ab_string_free(engine, &row->enclosing);
  ab_string_free(engine, &row->name);
  ab_string_free(engine, &row->scope);
  ab_string_free(engine, &row->observation_sha256);
  ab_string_free(engine, &row->evidence_slice_sha256);
  ab_string_free(engine, &row->producer_version);
  ab_string_free(engine, &row->producer_implementation_sha256);
  ab_string_free(engine, &row->producer_configuration_sha256);
  ab_string_free(engine, &row->runtime);
  memset(row, 0, sizeof(*row));
}

static int route_evidence_identity(const AbMapRouteEvidence *row,
                                   const AbString *target,
                                   const AbString *target_symbol,
                                   const AbFact *fact, const char *relation,
                                   const char *scope) {
  size_t relation_length = strlen(relation);
  size_t scope_length = strlen(scope);
  return ab_string_equal(&row->target, target) &&
         ((!target_symbol && !row->target_symbol.length) ||
          (target_symbol &&
           ab_string_equal(&row->target_symbol, target_symbol))) &&
         row->relation.length == relation_length &&
         !memcmp(row->relation.data, relation, relation_length) &&
         row->scope.length == scope_length &&
         !memcmp(row->scope.data, scope, scope_length) &&
         ((!fact && !row->fact_id.length) ||
          (fact && ab_string_equal(&row->fact_id, &fact->id)));
}

static ArchbirdStatus
add_route_evidence(AbMapState *state, AbMapRouteEvidence **rows, size_t *count,
                   const AbString *target, const AbString *target_symbol,
                   const AbFact *fact, const AbProviderBundle *provider,
                   const char *relation, const char *scope,
                   const char *provenance, const AbString *configured_name,
                   size_t configured_line) {
  AbMapRouteEvidence *resized;
  AbMapRouteEvidence *row;
  const AbString *enclosing =
      fact ? fact_string_attribute(fact, "enclosing") : NULL;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < *count; index++)
    if (route_evidence_identity(&(*rows)[index], target, target_symbol, fact,
                                relation, scope))
      return ARCHBIRD_OK;
  resized = (AbMapRouteEvidence *)ab_realloc(state->engine, *rows,
                                             (*count + 1) * sizeof(**rows));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting test route evidence");
  *rows = resized;
  row = &(*rows)[*count];
  memset(row, 0, sizeof(*row));
  status =
      ab_string_copy(state->engine, &row->target, target->data, target->length);
  if (status == ARCHBIRD_OK && target_symbol)
    status = ab_string_copy(state->engine, &row->target_symbol,
                            target_symbol->data, target_symbol->length);
  if (status == ARCHBIRD_OK && fact)
    status = ab_string_copy(state->engine, &row->fact_id, fact->id.data,
                            fact->id.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->relation, relation,
                            strlen(relation));
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->provenance, provenance,
                            strlen(provenance));
  if (status == ARCHBIRD_OK && provider)
    status = ab_string_copy(state->engine, &row->provider,
                            provider->producer.name.data,
                            provider->producer.name.length);
  else if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(state->engine, &row->provider, "project-config", 14);
  if (status == ARCHBIRD_OK && fact)
    status = ab_string_copy(state->engine, &row->claim, fact->claim.data,
                            fact->claim.length);
  else if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->claim, "asserted-route", 14);
  if (status == ARCHBIRD_OK && enclosing)
    status = ab_string_copy(state->engine, &row->enclosing, enclosing->data,
                            enclosing->length);
  if (status == ARCHBIRD_OK && fact && fact->has_name)
    status = ab_string_copy(state->engine, &row->name, fact->name.data,
                            fact->name.length);
  else if (status == ARCHBIRD_OK && configured_name)
    status = ab_string_copy(state->engine, &row->name, configured_name->data,
                            configured_name->length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->scope, scope, strlen(scope));
  if (status != ARCHBIRD_OK) {
    route_evidence_row_clear(state->engine, row);
    return status;
  }
  row->line = fact ? fact_line(fact) : configured_line;
  row->span_start = fact ? fact->span_start : 0;
  row->span_end = fact ? fact->span_end : 0;
  (*count)++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_evidenced_route(
    AbMapState *state, AbMapRouteCount **routes, size_t *route_count,
    AbMapRouteEvidence **evidence, size_t *evidence_count,
    const AbString *target, const AbString *target_symbol, const AbFact *fact,
    const AbProviderBundle *provider, const char *relation, const char *scope) {
  ArchbirdStatus status = add_route(state, routes, route_count, target, 1);
  return status == ARCHBIRD_OK
             ? add_route_evidence(state, evidence, evidence_count, target,
                                  target_symbol, fact, provider, relation,
                                  scope, "derived", NULL, 0)
             : status;
}

static int route_compare(const void *left_raw, const void *right_raw) {
  const AbMapRouteCount *left = (const AbMapRouteCount *)left_raw;
  const AbMapRouteCount *right = (const AbMapRouteCount *)right_raw;
  return ab_string_compare(&left->path, &right->path);
}

static int route_evidence_compare(const void *left_raw, const void *right_raw) {
  const AbMapRouteEvidence *left = (const AbMapRouteEvidence *)left_raw;
  const AbMapRouteEvidence *right = (const AbMapRouteEvidence *)right_raw;
  int compared = ab_string_compare(&left->target, &right->target);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->scope, &right->scope);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->relation, &right->relation);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->target_symbol, &right->target_symbol);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->fact_id, &right->fact_id);
  if (compared)
    return compared;
  compared =
      ab_string_compare(&left->observation_sha256, &right->observation_sha256);
  if (compared)
    return compared;
  if (left->has_hits != right->has_hits)
    return left->has_hits ? 1 : -1;
  if (left->hits != right->hits)
    return left->hits < right->hits ? -1 : 1;
  return (left->line > right->line) - (left->line < right->line);
}

static int observed_route_evidence_identity(const AbMapRouteEvidence *row,
                                            const AbString *target,
                                            const AbString *target_symbol,
                                            const AbString *observation_sha256,
                                            size_t hits) {
  return string_literal(&row->provenance, "observed") &&
         string_literal(&row->relation, "observed-symbol-hit") &&
         string_literal(&row->scope, "case") &&
         ab_string_equal(&row->target, target) &&
         ab_string_equal(&row->target_symbol, target_symbol) &&
         ab_string_equal(&row->observation_sha256, observation_sha256) &&
         row->has_hits && row->hits == hits;
}

static ArchbirdStatus copy_value_text(ArchbirdEngine *engine, AbString *out,
                                      const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING
             ? ab_string_copy(engine, out, value->as.text.data,
                              value->as.text.length)
             : ARCHBIRD_INVALID_SCHEMA;
}

static ArchbirdStatus
add_observed_route_evidence(AbMapState *state, AbMapRouteEvidence **rows,
                            size_t *count, const AbValue *document,
                            const AbValue *symbol,
                            const AbString *observation_sha256) {
  const AbValue *producer = ab_value_member(document, "producer");
  const AbValue *source = ab_value_member(document, "source");
  const AbValue *target = ab_value_member(symbol, "path");
  const AbValue *target_symbol = ab_value_member(symbol, "symbol");
  const AbValue *hits_value = ab_value_member(symbol, "hits");
  const AbValue *runtime = ab_value_member(producer, "runtime");
  uint64_t hits = 0;
  AbMapRouteEvidence *resized;
  AbMapRouteEvidence *row;
  size_t index;
  ArchbirdStatus status;
  if (!ab_value_u64(hits_value, &hits) || !hits || hits > SIZE_MAX)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index < *count; index++)
    if (observed_route_evidence_identity(&(*rows)[index], &target->as.text,
                                         &target_symbol->as.text,
                                         observation_sha256, (size_t)hits))
      return ARCHBIRD_OK;
  resized = (AbMapRouteEvidence *)ab_realloc(state->engine, *rows,
                                             (*count + 1) * sizeof(**rows));
  if (!resized)
    return archbird_error_set(
        state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory collecting observed test route evidence");
  *rows = resized;
  row = &(*rows)[*count];
  memset(row, 0, sizeof(*row));
  status = copy_value_text(state->engine, &row->target, target);
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->target_symbol, target_symbol);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->relation,
                            "observed-symbol-hit", 19);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->provenance, "observed", 8);
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->provider,
                             ab_value_member(producer, "name"));
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->claim, "symbol-hit", 10);
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->name, target_symbol);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &row->scope, "case", 4);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(state->engine, &row->observation_sha256,
                       observation_sha256->data, observation_sha256->length);
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->evidence_slice_sha256,
                             ab_value_member(source, "evidence_slice_sha256"));
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->producer_version,
                             ab_value_member(producer, "version"));
  if (status == ARCHBIRD_OK)
    status =
        copy_value_text(state->engine, &row->producer_implementation_sha256,
                        ab_value_member(producer, "implementation_sha256"));
  if (status == ARCHBIRD_OK)
    status = copy_value_text(state->engine, &row->producer_configuration_sha256,
                             ab_value_member(producer, "configuration_sha256"));
  if (status == ARCHBIRD_OK && runtime)
    status = copy_value_text(state->engine, &row->runtime, runtime);
  if (status != ARCHBIRD_OK) {
    route_evidence_row_clear(state->engine, row);
    return status;
  }
  row->hits = (size_t)hits;
  row->has_hits = 1;
  (*count)++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus call_route(AbMapState *state, const TestFactIndex *index,
                                 const AbConfigTest *spec, const AbFact *call,
                                 const AbProviderBundle *provider,
                                 const char *evidence_scope,
                                 AbMapRouteCount **routes, size_t *route_count,
                                 AbMapRouteEvidence **evidence,
                                 size_t *evidence_count) {
  const AbString *candidate = NULL;
  const AbString *candidate_symbol = NULL;
  const AbString *binding = fact_string_attribute(call, "binding");
  size_t low = 0;
  size_t high = index->symbol_count;
  size_t symbol_index;
  int ambiguous = 0;
  if (binding && !string_literal(binding, "project"))
    return ARCHBIRD_OK;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = route_symbol_key_compare(&index->symbols[middle],
                                            &spec->language, &call->name);
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  for (symbol_index = low; symbol_index < index->symbol_count; symbol_index++) {
    const RouteSymbol *symbol = &index->symbols[symbol_index];
    const AbString *scope;
    if (route_symbol_key_compare(symbol, &spec->language, &call->name) != 0)
      break;
    if (!route_file_allowed(spec, symbol->file))
      continue;
    if (string_literal(&symbol->fact->kind, "method"))
      continue;
    scope = fact_string_attribute(symbol->fact, "scope");
    if (scope && string_literal(scope, "local"))
      continue;
    if (!candidate) {
      candidate = &symbol->file->path;
      candidate_symbol = &symbol->fact->name;
    } else if (!ab_string_equal(candidate, &symbol->file->path)) {
      ambiguous = 1;
      break;
    } else if (candidate_symbol &&
               !ab_string_equal(candidate_symbol, &symbol->fact->name)) {
      candidate_symbol = NULL;
    }
  }
  return candidate && !ambiguous
             ? add_evidenced_route(
                   state, routes, route_count, evidence, evidence_count,
                   candidate, candidate_symbol, call, provider,
                   ab_string_equal(candidate, &call->path) ? "call"
                                                           : "call-candidate",
                   evidence_scope)
             : ARCHBIRD_OK;
}

static ArchbirdStatus
imported_name_routes(AbMapState *state, const AbConfigTest *spec,
                     const AbFact *fact, const AbProviderBundle *provider,
                     const char *evidence_scope, AbMapRouteCount **routes,
                     size_t *route_count, AbMapRouteEvidence **evidence,
                     size_t *evidence_count) {
  const AbString *module = fact_string_attribute(fact, "module");
  const char *manager = ab_map_language_manager(&spec->language);
  AbString external;
  const AbMapPackage *matched = NULL;
  size_t matches = 0;
  size_t package_index;
  if (!module || !manager[0] || (module->length && module->data[0] == '.'))
    return ARCHBIRD_OK;
  external = ab_map_external_import_name(&spec->language, module);
  for (package_index = 0; package_index < state->package_count;
       package_index++) {
    const AbMapPackage *package = &state->packages[package_index];
    if ((!spec->discovered &&
         !array_contains(&spec->route_to, &package->layer)) ||
        strcmp(ab_map_package_manager(&package->kind), manager) != 0 ||
        !ab_map_package_alias_matches(package, manager, &external))
      continue;
    matched = package;
    matches++;
  }
  if (matches == 1) {
    size_t origin_index;
    for (origin_index = 0; origin_index < matched->export_origin_count;
         origin_index++) {
      const AbMapExportOrigin *origin = &matched->export_origins[origin_index];
      size_t path_index;
      if (!ab_string_equal(&origin->name, &fact->name))
        continue;
      for (path_index = 0; path_index < origin->paths.count; path_index++) {
        const AbManifestFile *target = ab_map_manifest_file(
            state->manifest, origin->paths.items[path_index].data,
            origin->paths.items[path_index].length);
        if (target && route_file_allowed(spec, target)) {
          ArchbirdStatus status = add_evidenced_route(
              state, routes, route_count, evidence, evidence_count,
              &target->path, &fact->name, fact, provider, "imported-name",
              evidence_scope);
          if (status != ARCHBIRD_OK)
            return status;
        }
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus imported_name_use_routes(
    AbMapState *state, const AbConfigTest *spec, const AbManifestFile *source,
    const AbFact *fact, const AbProviderBundle *provider,
    const char *evidence_scope, AbMapRouteCount **routes, size_t *route_count,
    AbMapRouteEvidence **evidence, size_t *evidence_count) {
  AbMapReferenceResolution resolution;
  ArchbirdStatus status;
  status =
      ab_map_resolve_imported_name_reference(state, source, fact, &resolution);
  if (status != ARCHBIRD_OK || !resolution.target ||
      !route_file_allowed(spec, resolution.target))
    return status;
  return add_evidenced_route(state, routes, route_count, evidence,
                             evidence_count, &resolution.target->path,
                             resolution.target_symbol, fact, provider,
                             resolution.relation, evidence_scope);
}

static ArchbirdStatus imported_attribute_call_routes(
    AbMapState *state, const TestFactIndex *index, const AbConfigTest *spec,
    const AbManifestFile *source, const AbFact *fact,
    const AbProviderBundle *provider, const char *evidence_scope,
    AbMapRouteCount **routes, size_t *route_count,
    AbMapRouteEvidence **evidence, size_t *evidence_count) {
  AbMapReferenceResolution resolution;
  const AbString *target_symbol;
  ArchbirdStatus status;
  (void)index;
  status = ab_map_resolve_imported_reference(state, source, fact, &resolution);
  if (status != ARCHBIRD_OK || !resolution.target ||
      !route_file_allowed(spec, resolution.target))
    return status;
  target_symbol = resolution.target_fact
                      ? &resolution.target_fact->name
                      : (resolution.target_symbol
                             ? resolution.target_symbol
                             : fact_string_attribute(fact, "imported"));
  status =
      add_evidenced_route(state, routes, route_count, evidence, evidence_count,
                          &resolution.target->path, target_symbol, fact,
                          provider, resolution.relation, evidence_scope);
  if (status == ARCHBIRD_OK && resolution.callable_fact)
    status = add_route_evidence(
        state, evidence, evidence_count, &resolution.target->path,
        &resolution.callable_fact->name, fact, provider,
        "decorator-callable-candidate", evidence_scope, "derived", NULL, 0);
  return status;
}

static ArchbirdStatus method_name_candidate_routes(
    AbMapState *state, const TestFactIndex *index, const AbConfigTest *spec,
    const AbFact *fact, const AbProviderBundle *provider,
    const char *evidence_scope, AbMapRouteCount **routes, size_t *route_count,
    AbMapRouteEvidence **evidence, size_t *evidence_count) {
  enum { MAX_METHOD_NAME_CANDIDATES = 16 };
  const RouteSymbol *candidates[MAX_METHOD_NAME_CANDIDATES];
  size_t candidate_count = 0;
  size_t low = 0;
  size_t high = index->symbol_count;
  size_t symbol_index;
  size_t candidate_index;
  if (!string_literal(&spec->language, "javascript") &&
      !string_literal(&spec->language, "typescript") &&
      !string_literal(&spec->language, "vue"))
    return ARCHBIRD_OK;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = route_symbol_key_compare(&index->symbols[middle],
                                            &spec->language, &fact->name);
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  for (symbol_index = low; symbol_index < index->symbol_count; symbol_index++) {
    const RouteSymbol *symbol = &index->symbols[symbol_index];
    const AbString *scope;
    if (route_symbol_key_compare(symbol, &spec->language, &fact->name) != 0)
      break;
    if (!route_file_allowed(spec, symbol->file) ||
        !string_literal(&symbol->fact->kind, "method"))
      continue;
    scope = fact_string_attribute(symbol->fact, "scope");
    if (scope && string_literal(scope, "local"))
      continue;
    for (candidate_index = 0; candidate_index < candidate_count;
         candidate_index++)
      if (candidates[candidate_index]->file == symbol->file &&
          ab_string_equal(&candidates[candidate_index]->fact->name,
                          &symbol->fact->name))
        break;
    if (candidate_index < candidate_count)
      continue;
    if (candidate_count == MAX_METHOD_NAME_CANDIDATES)
      return ARCHBIRD_OK;
    candidates[candidate_count++] = symbol;
  }
  for (candidate_index = 0; candidate_index < candidate_count;
       candidate_index++) {
    const RouteSymbol *symbol = candidates[candidate_index];
    ArchbirdStatus status = add_evidenced_route(
        state, routes, route_count, evidence, evidence_count,
        &symbol->file->path, &symbol->fact->name, fact, provider,
        "method-name-candidate", evidence_scope);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
resolve_routes(AbMapState *state, const TestFactIndex *index,
               const AbConfigTest *spec, const AbManifestFile *file, int ranged,
               size_t start, size_t end, AbMapRouteCount **routes,
               size_t *route_count, AbMapRouteEvidence **evidence,
               size_t *evidence_count) {
  size_t file_index = (size_t)(file - state->manifest->files);
  size_t fact_index;
  AbStringArray seen_imports = {0};
  AbStringArray seen_imported_names = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  for (fact_index = index->file_offsets[file_index];
       status == ARCHBIRD_OK &&
       fact_index < index->file_offsets[file_index + 1];
       fact_index++) {
    const AbFact *fact = index->facts[fact_index];
    const AbProviderBundle *provider = index->providers[fact_index];
    const char *evidence_scope = ranged ? "case" : "file";
    if ((ranged && (fact->span_start < start || fact->span_start >= end)) ||
        !fact->has_name)
      continue;
    if (fact_domain(fact, "calls")) {
      status = call_route(state, index, spec, fact, provider, evidence_scope,
                          routes, route_count, evidence, evidence_count);
    } else if (fact_domain(fact, "method-calls")) {
      status = method_name_candidate_routes(state, index, spec, fact, provider,
                                            evidence_scope, routes, route_count,
                                            evidence, evidence_count);
    } else if (fact_domain(fact, "imports")) {
      const AbManifestFile *target = NULL;
      if (!array_contains(&seen_imports, &fact->name)) {
        status = append_unique(state->engine, &seen_imports, fact->name.data,
                               fact->name.length);
        if (status == ARCHBIRD_OK)
          status =
              ab_map_resolve_import(state->engine, state->manifest,
                                    state->config, file, &fact->name, &target);
        if (status == ARCHBIRD_OK && target && route_file_allowed(spec, target))
          status = add_evidenced_route(
              state, routes, route_count, evidence, evidence_count,
              &target->path, NULL, fact, provider, "import", evidence_scope);
      }
    } else if (fact_domain(fact, "imported-names")) {
      if (!array_contains(&seen_imported_names, &fact->key)) {
        status = append_unique(state->engine, &seen_imported_names,
                               fact->key.data, fact->key.length);
        if (status == ARCHBIRD_OK)
          status = imported_name_routes(state, spec, fact, provider,
                                        evidence_scope, routes, route_count,
                                        evidence, evidence_count);
      }
    } else if (fact_domain(fact, "name-uses")) {
      if (string_literal(&fact->kind, "imported-attribute-call") ||
          string_literal(&fact->kind, "inferred-receiver-call") ||
          string_literal(&fact->kind, "decorator-reference"))
        status = imported_attribute_call_routes(
            state, index, spec, file, fact, provider, evidence_scope, routes,
            route_count, evidence, evidence_count);
      else
        status = imported_name_use_routes(state, spec, file, fact, provider,
                                          evidence_scope, routes, route_count,
                                          evidence, evidence_count);
    } else if (fact_domain(fact, "reference-targets")) {
      AbMapReferenceResolution resolution;
      status = ab_map_resolve_provider_reference(state, fact, &resolution);
      if (status == ARCHBIRD_OK && resolution.exact && resolution.target &&
          route_file_allowed(spec, resolution.target))
        status = add_evidenced_route(state, routes, route_count, evidence,
                                     evidence_count, &resolution.target->path,
                                     resolution.target_symbol, fact, provider,
                                     resolution.relation, evidence_scope);
    }
  }
  if (status == ARCHBIRD_OK && *route_count > 1)
    qsort(*routes, *route_count, sizeof(**routes), route_compare);
  if (status == ARCHBIRD_OK && *evidence_count > 1)
    qsort(*evidence, *evidence_count, sizeof(**evidence),
          route_evidence_compare);
  string_array_free(state->engine, &seen_imports);
  string_array_free(state->engine, &seen_imported_names);
  return status;
}

static ArchbirdStatus append_case(AbMapState *state, CaseRange **cases,
                                  size_t *count, const char *selector,
                                  size_t selector_length, size_t line,
                                  size_t start, size_t end,
                                  const char *evidence_kind) {
  CaseRange *resized = (CaseRange *)ab_realloc(state->engine, *cases,
                                               (*count + 1) * sizeof(**cases));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting test cases");
  *cases = resized;
  memset(&(*cases)[*count], 0, sizeof(**cases));
  if (ab_string_copy(state->engine, &(*cases)[*count].selector, selector,
                     selector_length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  (*cases)[*count].line = line;
  (*cases)[*count].start = start;
  (*cases)[*count].end = end;
  (*cases)[*count].evidence_kind = evidence_kind;
  (*count)++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
append_callback_case(AbMapState *state, CaseRange **cases, size_t *count,
                     const char *selector, size_t selector_length,
                     const char *callback, size_t callback_length, size_t line,
                     size_t start, size_t end, const char *evidence_kind) {
  ArchbirdStatus status =
      append_case(state, cases, count, selector, selector_length, line, start,
                  end, evidence_kind);
  if (status != ARCHBIRD_OK || !callback_length)
    return status;
  status = ab_string_copy(state->engine, &(*cases)[*count - 1].callback,
                          callback, callback_length);
  if (status != ARCHBIRD_OK) {
    ab_string_free(state->engine, &(*cases)[*count - 1].selector);
    (*count)--;
  }
  return status;
}

static size_t matching_token(const AbTokenList *tokens, size_t opening,
                             const char *left, const char *right) {
  size_t depth = 0;
  size_t index;
  for (index = opening; index < tokens->count; index++) {
    if (ab_token_equals(tokens, index, left))
      depth++;
    else if (ab_token_equals(tokens, index, right) && depth && --depth == 0)
      return index;
  }
  return SIZE_MAX;
}

static void token_inner(const AbTokenList *tokens, size_t index,
                        const char **data, size_t *length) {
  const AbToken *token = &tokens->items[index];
  *data = (const char *)tokens->source + token->start;
  *length = token->end - token->start;
  if (*length >= 2 && ((*data)[0] == '\'' || (*data)[0] == '"') &&
      (*data)[*length - 1] == (*data)[0]) {
    (*data)++;
    *length -= 2;
  }
}

static int c_macro_argument_token(const AbTokenList *tokens, size_t opening,
                                  size_t closing, size_t wanted,
                                  size_t *out_token) {
  size_t argument = 0;
  size_t start = opening + 1;
  size_t index;
  size_t parentheses = 0;
  size_t brackets = 0;
  size_t braces = 0;
  for (index = opening + 1; index <= closing; index++) {
    int boundary = index == closing;
    if (!boundary) {
      if (ab_token_equals(tokens, index, "("))
        parentheses++;
      else if (ab_token_equals(tokens, index, ")") && parentheses)
        parentheses--;
      else if (ab_token_equals(tokens, index, "["))
        brackets++;
      else if (ab_token_equals(tokens, index, "]") && brackets)
        brackets--;
      else if (ab_token_equals(tokens, index, "{"))
        braces++;
      else if (ab_token_equals(tokens, index, "}") && braces)
        braces--;
      else if (!parentheses && !brackets && !braces &&
               ab_token_equals(tokens, index, ","))
        boundary = 1;
    }
    if (!boundary)
      continue;
    if (argument == wanted) {
      if (index != start + 1)
        return 0;
      if (tokens->items[start].kind != AB_TOKEN_IDENTIFIER &&
          tokens->items[start].kind != AB_TOKEN_NUMBER &&
          tokens->items[start].kind != AB_TOKEN_STRING)
        return 0;
      *out_token = start;
      return 1;
    }
    argument++;
    start = index + 1;
  }
  return 0;
}

static const AbConfigTestCaseExtractor *
c_macro_extractor(const AbConfigTest *spec, const AbTokenList *tokens,
                  size_t index, AbConfigTestCaseExtractor *legacy,
                  size_t legacy_arguments[2]) {
  size_t extractor_index;
  for (extractor_index = 0; extractor_index < spec->case_extractor_count;
       extractor_index++) {
    const AbConfigTestCaseExtractor *extractor =
        &spec->case_extractors[extractor_index];
    if (tokens->items[index].end - tokens->items[index].start ==
            extractor->call.length &&
        (!extractor->call.length ||
         memcmp(tokens->source + tokens->items[index].start,
                extractor->call.data, extractor->call.length) == 0))
      return extractor;
  }
  if (spec->case_extractor_count)
    return NULL;
  memset(legacy, 0, sizeof(*legacy));
  legacy->kind.data = "c_macro";
  legacy->kind.length = 7;
  legacy->separator.data = ".";
  legacy->separator.length = 1;
  legacy->selector_arguments = legacy_arguments;
  legacy_arguments[0] = 0;
  legacy_arguments[1] = 1;
  legacy->selector_argument_count =
      ab_token_equals(tokens, index, "TEST") ? 2 : 1;
  if (!ab_token_equals(tokens, index, "TEST") &&
      !ab_token_equals(tokens, index, "TEST_CASE")) {
    size_t cursor;
    int contains_test = 0;
    int macro_style = 1;
    if (!spec->discovered || tokens->items[index].kind != AB_TOKEN_IDENTIFIER)
      return NULL;
    for (cursor = tokens->items[index].start; cursor < tokens->items[index].end;
         cursor++) {
      unsigned char value = tokens->source[cursor];
      if ((value >= 'a' && value <= 'z') ||
          !((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
            value == '_')) {
        macro_style = 0;
        break;
      }
    }
    for (cursor = tokens->items[index].start;
         macro_style && cursor + 3 < tokens->items[index].end; cursor++) {
      unsigned char first = (unsigned char)tokens->source[cursor];
      unsigned char second = (unsigned char)tokens->source[cursor + 1];
      unsigned char third = (unsigned char)tokens->source[cursor + 2];
      unsigned char fourth = (unsigned char)tokens->source[cursor + 3];
      if (tolower(first) == 't' && tolower(second) == 'e' &&
          tolower(third) == 's' && tolower(fourth) == 't') {
        contains_test = 1;
        break;
      }
    }
    if (!contains_test)
      return NULL;
    legacy->selector_argument_count = 1;
  }
  return legacy;
}

static int string_contains_fold(const char *data, size_t length,
                                const char *needle) {
  size_t needle_length = strlen(needle);
  size_t start;
  if (!needle_length || needle_length > length)
    return 0;
  for (start = 0; start + needle_length <= length; start++) {
    size_t index;
    for (index = 0; index < needle_length; index++)
      if (tolower((unsigned char)data[start + index]) !=
          tolower((unsigned char)needle[index]))
        break;
    if (index == needle_length)
      return 1;
  }
  return 0;
}

static size_t test_registry_group_length(const char *name, size_t length) {
  static const char *const suffixes[] = {"_testcases", "testcases", "_tests",
                                         "tests",      "_cases",    "cases"};
  size_t suffix_index;
  for (suffix_index = 0; suffix_index < sizeof(suffixes) / sizeof(suffixes[0]);
       suffix_index++) {
    const char *suffix = suffixes[suffix_index];
    size_t suffix_length = strlen(suffix);
    size_t index;
    if (suffix_length > length)
      continue;
    for (index = 0; index < suffix_length; index++)
      if (tolower((unsigned char)name[length - suffix_length + index]) !=
          tolower((unsigned char)suffix[index]))
        break;
    if (index == suffix_length) {
      length -= suffix_length;
      while (length && name[length - 1] == '_')
        length--;
      return length;
    }
  }
  return 0;
}

static const RouteSymbol *unique_route_symbol(const TestFactIndex *index,
                                              const AbConfigTest *spec,
                                              const char *name,
                                              size_t name_length) {
  const RouteSymbol *matched = NULL;
  size_t symbol_index;
  for (symbol_index = 0; symbol_index < index->symbol_count; symbol_index++) {
    const RouteSymbol *symbol = &index->symbols[symbol_index];
    if (!route_file_allowed(spec, symbol->file) ||
        symbol->leaf_length != name_length ||
        memcmp(symbol->leaf, name, name_length) != 0)
      continue;
    if (matched)
      return NULL;
    matched = symbol;
  }
  return matched;
}

static size_t dispatch_initializer(const AbTokenList *tokens, size_t name);

static size_t registry_initializer(const AbTokenList *tokens, size_t name) {
  size_t index;
  size_t limit = name + 64 < tokens->count ? name + 64 : tokens->count;
  for (index = name + 1; index < limit; index++) {
    if (ab_token_equals(tokens, index, ";"))
      return SIZE_MAX;
    if (tokens->items[index].kind == AB_TOKEN_IDENTIFIER)
      return SIZE_MAX;
    if (ab_token_equals(tokens, index, "="))
      return index + 1 < tokens->count &&
                     ab_token_equals(tokens, index + 1, "{")
                 ? index + 1
                 : SIZE_MAX;
  }
  return SIZE_MAX;
}

static const RouteSymbol *
inferred_registry_callback(AbMapState *state, const TestFactIndex *index,
                           const AbConfigTest *spec, const char *group,
                           size_t group_length, const char *selector,
                           size_t selector_length) {
  AbBuffer name;
  const RouteSymbol *matched = NULL;
  size_t pattern;
  ab_buffer_init(&name, state->engine);
  for (pattern = 0; pattern < 3 && !matched; pattern++) {
    ArchbirdStatus status = ARCHBIRD_OK;
    name.length = 0;
    if (pattern == 0 && group_length) {
      status = ab_buffer_append(&name, group, group_length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&name, "_");
    } else if (pattern == 2) {
      status = ab_buffer_literal(&name, "test_");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&name, selector, selector_length);
    if (status == ARCHBIRD_OK && pattern != 2)
      status = ab_buffer_literal(&name, "_test");
    if (status != ARCHBIRD_OK)
      break;
    matched =
        unique_route_symbol(index, spec, (const char *)name.data, name.length);
  }
  ab_buffer_free(&name);
  return matched;
}

static ArchbirdStatus c_registry_cases(AbMapState *state,
                                       const TestFactIndex *index,
                                       const AbConfigTest *spec,
                                       const uint8_t *source,
                                       size_t source_length, CaseRange **cases,
                                       size_t *case_count, size_t *registered) {
  AbTokenList tokens;
  size_t name_index;
  ArchbirdStatus status = ab_tokenize(state->engine, source, source_length,
                                      AB_LEX_C_PREPROCESSOR, &tokens);
  if (status != ARCHBIRD_OK)
    return status;
  for (name_index = 0; status == ARCHBIRD_OK && name_index < tokens.count;
       name_index++) {
    const AbToken *name_token = &tokens.items[name_index];
    const char *name;
    size_t name_length;
    size_t group_length;
    size_t opening;
    size_t closing;
    size_t cursor;
    size_t depth = 0;
    if (name_token->kind != AB_TOKEN_IDENTIFIER)
      continue;
    name = (const char *)tokens.source + name_token->start;
    name_length = name_token->end - name_token->start;
    if (!string_contains_fold(name, name_length, "test") &&
        !string_contains_fold(name, name_length, "case"))
      continue;
    group_length = test_registry_group_length(name, name_length);
    opening = registry_initializer(&tokens, name_index);
    if (opening == SIZE_MAX)
      continue;
    closing = matching_token(&tokens, opening, "{", "}");
    if (closing == SIZE_MAX)
      continue;
    for (cursor = opening + 1; status == ARCHBIRD_OK && cursor < closing;
         cursor++) {
      size_t entry_close;
      size_t selector_token;
      size_t callback_token = SIZE_MAX;
      const char *selector;
      size_t selector_length;
      const char *callback = NULL;
      size_t callback_length = 0;
      AbBuffer qualified;
      if (ab_token_equals(&tokens, cursor, "{")) {
        if (depth++) {
          continue;
        }
        entry_close = matching_token(&tokens, cursor, "{", "}");
        if (entry_close == SIZE_MAX)
          continue;
        if (!c_macro_argument_token(&tokens, cursor, entry_close, 0,
                                    &selector_token)) {
          cursor = entry_close;
          depth = 0;
          continue;
        }
        (void)c_macro_argument_token(&tokens, cursor, entry_close, 1,
                                     &callback_token);
      } else if (!depth && cursor + 1 < closing &&
                 tokens.items[cursor].kind == AB_TOKEN_IDENTIFIER &&
                 ab_token_equals(&tokens, cursor + 1, "(")) {
        entry_close = matching_token(&tokens, cursor + 1, "(", ")");
        if (entry_close == SIZE_MAX ||
            !c_macro_argument_token(&tokens, cursor + 1, entry_close, 0,
                                    &selector_token))
          continue;
      } else {
        if (ab_token_equals(&tokens, cursor, "}") && depth)
          depth--;
        continue;
      }
      token_inner(&tokens, selector_token, &selector, &selector_length);
      if (callback_token != SIZE_MAX &&
          tokens.items[callback_token].kind == AB_TOKEN_IDENTIFIER) {
        callback =
            (const char *)tokens.source + tokens.items[callback_token].start;
        callback_length = tokens.items[callback_token].end -
                          tokens.items[callback_token].start;
      } else {
        const RouteSymbol *inferred = inferred_registry_callback(
            state, index, spec, name, group_length, selector, selector_length);
        if (inferred) {
          callback = inferred->leaf;
          callback_length = inferred->leaf_length;
        }
      }
      ab_buffer_init(&qualified, state->engine);
      if (group_length)
        status = ab_buffer_append(&qualified, name, group_length);
      if (status == ARCHBIRD_OK && group_length)
        status = ab_buffer_literal(&qualified, "/");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&qualified, selector, selector_length);
      if (status == ARCHBIRD_OK)
        status = append_callback_case(
            state, cases, case_count, (const char *)qualified.data,
            qualified.length, callback, callback_length,
            tokens.items[selector_token].line, tokens.items[cursor].start,
            tokens.items[entry_close].end, "test_registration_candidate");
      ab_buffer_free(&qualified);
      if (status == ARCHBIRD_OK)
        (*registered)++;
      cursor = entry_close;
      depth = 0;
    }
    name_index = closing;
  }
  ab_token_list_free(&tokens);
  return status;
}

static int token_equals_string(const AbTokenList *tokens, size_t index,
                               const AbString *value) {
  const AbToken *token = &tokens->items[index];
  return token->end - token->start == value->length &&
         (!value->length || memcmp(tokens->source + token->start, value->data,
                                   value->length) == 0);
}

static size_t dispatch_initializer(const AbTokenList *tokens, size_t name) {
  size_t index;
  size_t limit = name + 64 < tokens->count ? name + 64 : tokens->count;
  for (index = name + 1; index < limit; index++) {
    if (ab_token_equals(tokens, index, ";"))
      return SIZE_MAX;
    if (ab_token_equals(tokens, index, "="))
      return index + 1 < tokens->count &&
                     ab_token_equals(tokens, index + 1, "{")
                 ? index + 1
                 : SIZE_MAX;
  }
  return SIZE_MAX;
}

static size_t python_mapping_entry_end(const AbTokenList *tokens, size_t start,
                                       size_t closing) {
  size_t index;
  size_t parentheses = 0;
  size_t brackets = 0;
  size_t braces = 0;
  for (index = start; index < closing; index++) {
    if (ab_token_equals(tokens, index, "("))
      parentheses++;
    else if (ab_token_equals(tokens, index, ")") && parentheses)
      parentheses--;
    else if (ab_token_equals(tokens, index, "["))
      brackets++;
    else if (ab_token_equals(tokens, index, "]") && brackets)
      brackets--;
    else if (ab_token_equals(tokens, index, "{"))
      braces++;
    else if (ab_token_equals(tokens, index, "}") && braces)
      braces--;
    else if (!parentheses && !brackets && !braces &&
             ab_token_equals(tokens, index, ","))
      return index ? index - 1 : index;
  }
  return closing ? closing - 1 : closing;
}

static ArchbirdStatus
named_dispatch_cases(AbMapState *state, const AbConfigTest *spec,
                     const uint8_t *source, size_t source_length,
                     CaseRange **cases, size_t *case_count,
                     size_t *registered) {
  uint32_t flags = string_literal(&spec->language, "python")
                       ? AB_LEX_PYTHON
                       : AB_LEX_C_PREPROCESSOR;
  int python = string_literal(&spec->language, "python");
  AbTokenList tokens;
  size_t extractor_index;
  ArchbirdStatus status =
      ab_tokenize(state->engine, source, source_length, flags, &tokens);
  if (status != ARCHBIRD_OK)
    return status;
  for (extractor_index = 0;
       status == ARCHBIRD_OK && extractor_index < spec->case_extractor_count;
       extractor_index++) {
    const AbConfigTestCaseExtractor *extractor =
        &spec->case_extractors[extractor_index];
    size_t name_index;
    if (!string_literal(&extractor->kind, "named_dispatch"))
      continue;
    for (name_index = 0; status == ARCHBIRD_OK && name_index < tokens.count;
         name_index++) {
      size_t opening;
      size_t closing;
      size_t index;
      if (!token_equals_string(&tokens, name_index, &extractor->name))
        continue;
      opening = dispatch_initializer(&tokens, name_index);
      if (opening == SIZE_MAX)
        continue;
      closing = matching_token(&tokens, opening, "{", "}");
      if (closing == SIZE_MAX)
        continue;
      if (python) {
        size_t braces = 0;
        size_t parentheses = 0;
        size_t brackets = 0;
        for (index = opening + 1; status == ARCHBIRD_OK && index < closing;
             index++) {
          const char *selector;
          size_t selector_length;
          size_t end;
          if (!braces && !parentheses && !brackets &&
              tokens.items[index].kind == AB_TOKEN_STRING &&
              index + 1 < closing && ab_token_equals(&tokens, index + 1, ":")) {
            token_inner(&tokens, index, &selector, &selector_length);
            end = python_mapping_entry_end(&tokens, index + 2, closing);
            status =
                append_case(state, cases, case_count, selector, selector_length,
                            tokens.items[index].line, tokens.items[index].start,
                            tokens.items[end].end, "named_dispatch_entry");
            if (status == ARCHBIRD_OK)
              (*registered)++;
          }
          if (ab_token_equals(&tokens, index, "{"))
            braces++;
          else if (ab_token_equals(&tokens, index, "}") && braces)
            braces--;
          else if (ab_token_equals(&tokens, index, "("))
            parentheses++;
          else if (ab_token_equals(&tokens, index, ")") && parentheses)
            parentheses--;
          else if (ab_token_equals(&tokens, index, "["))
            brackets++;
          else if (ab_token_equals(&tokens, index, "]") && brackets)
            brackets--;
        }
      } else {
        size_t depth = 0;
        for (index = opening + 1; status == ARCHBIRD_OK && index < closing;
             index++) {
          if (ab_token_equals(&tokens, index, "{")) {
            if (depth == 0) {
              size_t entry_close = matching_token(&tokens, index, "{", "}");
              size_t token_index;
              if (entry_close != SIZE_MAX &&
                  c_macro_argument_token(&tokens, index, entry_close,
                                         extractor->selector_argument,
                                         &token_index)) {
                const char *selector;
                size_t selector_length;
                token_inner(&tokens, token_index, &selector, &selector_length);
                status = append_case(
                    state, cases, case_count, selector, selector_length,
                    tokens.items[token_index].line, tokens.items[index].start,
                    tokens.items[entry_close].end, "named_dispatch_entry");
                if (status == ARCHBIRD_OK)
                  (*registered)++;
              }
              if (entry_close != SIZE_MAX)
                index = entry_close;
            } else {
              depth++;
            }
          } else if (ab_token_equals(&tokens, index, "}") && depth) {
            depth--;
          }
        }
      }
      name_index = closing;
    }
  }
  ab_token_list_free(&tokens);
  return status;
}

static int has_named_dispatch(const AbConfigTest *spec) {
  size_t index;
  for (index = 0; index < spec->case_extractor_count; index++)
    if (string_literal(&spec->case_extractors[index].kind, "named_dispatch"))
      return 1;
  return 0;
}

static void c_main_body(const AbTokenList *tokens, size_t *out_opening,
                        size_t *out_closing) {
  size_t index;
  *out_opening = SIZE_MAX;
  *out_closing = SIZE_MAX;
  for (index = 0; index + 1 < tokens->count; index++) {
    size_t parameters;
    size_t brace;
    size_t closing;
    if (tokens->items[index].kind != AB_TOKEN_IDENTIFIER ||
        !ab_token_equals(tokens, index, "main") ||
        !ab_token_equals(tokens, index + 1, "("))
      continue;
    parameters = matching_token(tokens, index + 1, "(", ")");
    if (parameters == SIZE_MAX)
      continue;
    for (brace = parameters + 1; brace < tokens->count; brace++) {
      if (ab_token_equals(tokens, brace, "{"))
        break;
      if (ab_token_equals(tokens, brace, ";"))
        break;
    }
    if (brace == tokens->count || !ab_token_equals(tokens, brace, "{"))
      continue;
    closing = matching_token(tokens, brace, "{", "}");
    if (closing != SIZE_MAX) {
      *out_opening = brace;
      *out_closing = closing;
      return;
    }
  }
}

static int c_parameter_list_is_test_like(const AbTokenList *tokens,
                                         size_t opening, size_t closing) {
  return closing == opening + 1 ||
         (closing == opening + 2 &&
          ab_token_equals(tokens, opening + 1, "void"));
}

static int c_main_calls_function(const AbTokenList *tokens, size_t main_opening,
                                 size_t main_closing,
                                 const AbToken *function_name) {
  size_t index;
  size_t name_length = function_name->end - function_name->start;
  for (index = main_opening + 1; index + 1 < main_closing; index++) {
    const AbToken *candidate = &tokens->items[index];
    if (candidate->kind == AB_TOKEN_IDENTIFIER &&
        candidate->end - candidate->start == name_length &&
        memcmp(tokens->source + candidate->start,
               tokens->source + function_name->start, name_length) == 0 &&
        ab_token_equals(tokens, index + 1, "("))
      return 1;
  }
  return 0;
}

static ArchbirdStatus token_cases(AbMapState *state, const AbConfigTest *spec,
                                  const uint8_t *source, size_t source_length,
                                  CaseRange **cases, size_t *case_count,
                                  AbStringArray *selectors,
                                  size_t *registered) {
  uint32_t flags = string_literal(&spec->language, "c") ||
                           string_literal(&spec->language, "cpp")
                       ? AB_LEX_C_PREPROCESSOR
                   : string_literal(&spec->language, "r") ? AB_LEX_R
                                                          : AB_LEX_JAVASCRIPT;
  AbTokenList tokens;
  JsSuiteRange *suites = NULL;
  size_t suite_count = 0;
  size_t main_opening = SIZE_MAX;
  size_t main_closing = SIZE_MAX;
  size_t index;
  ArchbirdStatus status =
      ab_tokenize(state->engine, source, source_length, flags, &tokens);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_literal(&spec->language, "c") ||
      string_literal(&spec->language, "cpp"))
    c_main_body(&tokens, &main_opening, &main_closing);
  if (string_literal(&spec->language, "javascript") ||
      string_literal(&spec->language, "typescript") ||
      string_literal(&spec->language, "vue")) {
    for (index = 0; status == ARCHBIRD_OK && index + 2 < tokens.count;
         index++) {
      JsSuiteRange *resized;
      size_t closing;
      const char *name;
      size_t name_length;
      if (!ab_token_equals(&tokens, index, "describe") &&
          !ab_token_equals(&tokens, index, "context") &&
          !ab_token_equals(&tokens, index, "suite"))
        continue;
      if (!ab_token_equals(&tokens, index + 1, "(") ||
          tokens.items[index + 2].kind != AB_TOKEN_STRING)
        continue;
      closing = matching_token(&tokens, index + 1, "(", ")");
      if (closing == SIZE_MAX)
        continue;
      resized = (JsSuiteRange *)ab_realloc(state->engine, suites,
                                           (suite_count + 1) * sizeof(*suites));
      if (!resized) {
        status = archbird_error_set(
            state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory collecting JavaScript test suites");
        break;
      }
      suites = resized;
      token_inner(&tokens, index + 2, &name, &name_length);
      suites[suite_count++] = (JsSuiteRange){name, name_length, index, closing};
    }
  }
  *registered = 0;
  for (index = 0; status == ARCHBIRD_OK && index + 2 < tokens.count; index++) {
    if (string_literal(&spec->language, "c") ||
        string_literal(&spec->language, "cpp")) {
      AbConfigTestCaseExtractor legacy;
      size_t legacy_arguments[2];
      const AbConfigTestCaseExtractor *extractor;
      size_t close, brace, end;
      size_t argument_index;
      int selector_valid = 1;
      AbBuffer selector;
      extractor =
          c_macro_extractor(spec, &tokens, index, &legacy, legacy_arguments);
      if (!extractor && !spec->case_extractor_count &&
          tokens.items[index].kind == AB_TOKEN_IDENTIFIER &&
          prefix_bytes((const char *)tokens.source + tokens.items[index].start,
                       tokens.items[index].end - tokens.items[index].start,
                       "test_") &&
          ab_token_equals(&tokens, index + 1, "(")) {
        const char *selector =
            (const char *)tokens.source + tokens.items[index].start;
        size_t selector_length =
            tokens.items[index].end - tokens.items[index].start;
        size_t close = matching_token(&tokens, index + 1, "(", ")");
        size_t brace;
        size_t end;
        if (close == SIZE_MAX)
          continue;
        if ((main_opening != SIZE_MAX &&
             !c_main_calls_function(&tokens, main_opening, main_closing,
                                    &tokens.items[index])) ||
            (main_opening == SIZE_MAX &&
             !c_parameter_list_is_test_like(&tokens, index + 1, close)))
          continue;
        for (brace = close + 1; brace < tokens.count; brace++) {
          if (ab_token_equals(&tokens, brace, "{"))
            break;
          if (ab_token_equals(&tokens, brace, ";"))
            break;
        }
        if (brace == tokens.count || ab_token_equals(&tokens, brace, ";"))
          continue;
        end = matching_token(&tokens, brace, "{", "}");
        if (end == SIZE_MAX)
          continue;
        status =
            append_case(state, cases, case_count, selector, selector_length,
                        tokens.items[index].line, tokens.items[index].start,
                        tokens.items[end].end, "test_function_candidate");
        if (status == ARCHBIRD_OK)
          (*registered)++;
        continue;
      }
      if (!extractor || !ab_token_equals(&tokens, index + 1, "("))
        continue;
      close = matching_token(&tokens, index + 1, "(", ")");
      if (close == SIZE_MAX)
        continue;
      ab_buffer_init(&selector, state->engine);
      for (argument_index = 0;
           status == ARCHBIRD_OK &&
           argument_index < extractor->selector_argument_count;
           argument_index++) {
        size_t token_index;
        const char *part;
        size_t part_length;
        if (!c_macro_argument_token(
                &tokens, index + 1, close,
                extractor->selector_arguments[argument_index], &token_index)) {
          selector_valid = 0;
          break;
        }
        token_inner(&tokens, token_index, &part, &part_length);
        if (argument_index)
          status = ab_buffer_append(&selector, extractor->separator.data,
                                    extractor->separator.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&selector, part, part_length);
        if (status == ARCHBIRD_OK && argument_index == 0)
          status = append_unique(state->engine, selectors, part, part_length);
      }
      if (!selector_valid) {
        ab_buffer_free(&selector);
        continue;
      }
      if (status != ARCHBIRD_OK) {
        ab_buffer_free(&selector);
        continue;
      }
      for (brace = close + 1; brace < tokens.count; brace++) {
        if (ab_token_equals(&tokens, brace, "{"))
          break;
        if (ab_token_equals(&tokens, brace, ";") ||
            ab_token_equals(&tokens, brace, ",") ||
            ab_token_equals(&tokens, brace, "}"))
          break;
      }
      if (brace == tokens.count || !ab_token_equals(&tokens, brace, "{")) {
        ab_buffer_free(&selector);
        continue;
      }
      end = matching_token(&tokens, brace, "{", "}");
      if (end == SIZE_MAX) {
        ab_buffer_free(&selector);
        continue;
      }
      if (status == ARCHBIRD_OK)
        status =
            append_case(state, cases, case_count, (const char *)selector.data,
                        selector.length, tokens.items[index].line,
                        tokens.items[index].start, tokens.items[end].end,
                        "test_definition");
      if (status == ARCHBIRD_OK)
        (*registered)++;
      ab_buffer_free(&selector);
    } else {
      const char *name;
      size_t name_length;
      size_t close;
      int js = string_literal(&spec->language, "javascript") ||
               string_literal(&spec->language, "typescript") ||
               string_literal(&spec->language, "vue");
      int r = string_literal(&spec->language, "r");
      if ((!js || (!ab_token_equals(&tokens, index, "test") &&
                   !ab_token_equals(&tokens, index, "it") &&
                   !ab_token_equals(&tokens, index, "testIf"))) &&
          (!r || !ab_token_equals(&tokens, index, "test_that")))
        continue;
      if (!ab_token_equals(&tokens, index + 1, "(") ||
          tokens.items[index + 2].kind != AB_TOKEN_STRING)
        continue;
      token_inner(&tokens, index + 2, &name, &name_length);
      close = matching_token(&tokens, index + 1, "(", ")");
      if (close == SIZE_MAX)
        continue;
      (*registered)++;
      if (js) {
        const char *colon = (const char *)memchr(name, ':', name_length);
        if (colon)
          status = append_unique(state->engine, selectors, name,
                                 (size_t)(colon - name));
      }
      if (status == ARCHBIRD_OK) {
        if (js && suite_count) {
          AbBuffer qualified;
          size_t suite_index;
          ab_buffer_init(&qualified, state->engine);
          for (suite_index = 0;
               status == ARCHBIRD_OK && suite_index < suite_count;
               suite_index++) {
            const JsSuiteRange *suite = &suites[suite_index];
            if (!(suite->opening < index && index < suite->closing))
              continue;
            if (qualified.length)
              status = ab_buffer_literal(&qualified, " ");
            if (status == ARCHBIRD_OK)
              status =
                  ab_buffer_append(&qualified, suite->name, suite->name_length);
          }
          if (status == ARCHBIRD_OK && qualified.length)
            status = ab_buffer_literal(&qualified, " ");
          if (status == ARCHBIRD_OK)
            status = ab_buffer_append(&qualified, name, name_length);
          if (status == ARCHBIRD_OK)
            status =
                append_case(state, cases, case_count,
                            (const char *)qualified.data, qualified.length,
                            tokens.items[index].line, tokens.items[index].start,
                            tokens.items[close].end, "test_definition");
          ab_buffer_free(&qualified);
        } else {
          status =
              append_case(state, cases, case_count, name, name_length,
                          tokens.items[index].line, tokens.items[index].start,
                          tokens.items[close].end, "test_definition");
        }
      }
    }
  }
  ab_free(state->engine, suites);
  ab_token_list_free(&tokens);
  return status;
}

static size_t line_start(const uint8_t *source, size_t offset) {
  while (offset && source[offset - 1] != '\n')
    offset--;
  return offset;
}

static size_t indent_column(const uint8_t *source, size_t length, size_t start,
                            size_t *content) {
  size_t column = 0;
  while (start < length && (source[start] == ' ' || source[start] == '\t')) {
    if (source[start] == '\t')
      column = (column / 8 + 1) * 8;
    else
      column++;
    start++;
  }
  *content = start;
  return column;
}

static size_t python_case_end(const uint8_t *source, size_t length,
                              size_t start) {
  size_t content;
  size_t indent = indent_column(source, length, start, &content);
  size_t cursor = start;
  int depth = 0;
  int quote = 0;
  int escaped = 0;
  int header_done = 0;
  while (cursor < length) {
    unsigned char byte = source[cursor];
    if (quote) {
      if (escaped)
        escaped = 0;
      else if (byte == '\\')
        escaped = 1;
      else if (byte == quote)
        quote = 0;
    } else if (byte == '\'' || byte == '"') {
      quote = byte;
    } else if (byte == '(' || byte == '[' || byte == '{') {
      depth++;
    } else if ((byte == ')' || byte == ']' || byte == '}') && depth) {
      depth--;
    } else if (byte == ':' && depth == 0) {
      header_done = 1;
    } else if (byte == '\n' && header_done) {
      cursor++;
      break;
    }
    cursor++;
  }
  while (cursor < length) {
    size_t next = cursor;
    size_t line_content;
    size_t line_indent;
    while (next < length && source[next] != '\n')
      next++;
    line_indent = indent_column(source, length, cursor, &line_content);
    if (line_content < next && source[line_content] != '#' &&
        line_indent <= indent)
      return cursor;
    cursor = next < length ? next + 1 : length;
  }
  return length;
}

static ArchbirdStatus
python_cases(AbMapState *state, const TestFactIndex *index,
             const AbManifestFile *file, const uint8_t *source,
             size_t source_length, CaseRange **cases, size_t *case_count,
             AbStringArray *selectors, size_t *registered) {
  size_t file_index = (size_t)(file - state->manifest->files);
  size_t fact_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *registered = 0;
  for (fact_index = index->file_offsets[file_index];
       status == ARCHBIRD_OK &&
       fact_index < index->file_offsets[file_index + 1];
       fact_index++) {
    const AbFact *fact = index->facts[fact_index];
    const char *leaf;
    size_t leaf_length;
    if (!fact->has_name || !fact_domain(fact, "symbols"))
      continue;
    fact_leaf(fact, &leaf, &leaf_length);
    if (!string_literal(&fact->kind, "declaration") &&
        prefix_bytes(leaf, leaf_length, "test_")) {
      size_t start = fact->span_start;
      size_t decorator_index;
      for (decorator_index = index->file_offsets[file_index];
           decorator_index < index->file_offsets[file_index + 1];
           decorator_index++) {
        const AbFact *decorator = index->facts[decorator_index];
        const AbString *decorated;
        if (!fact_domain(decorator, "decorators"))
          continue;
        decorated = fact_string_attribute(decorator, "decorated");
        if (decorated && ab_string_equal(decorated, &fact->name) &&
            decorator->span_start < start)
          start = decorator->span_start;
      }
      start = line_start(source, start);
      status = append_case(state, cases, case_count, fact->name.data,
                           fact->name.length, fact_line(fact), start,
                           python_case_end(source, source_length, start),
                           "test_definition");
      (*registered)++;
    }
    if (status == ARCHBIRD_OK && string_literal(&fact->kind, "class") &&
        prefix_bytes(fact->name.data, fact->name.length, "Test")) {
      const char *dot =
          (const char *)memchr(fact->name.data, '.', fact->name.length);
      status = append_unique(state->engine, selectors, fact->name.data,
                             dot ? (size_t)(dot - fact->name.data)
                                 : fact->name.length);
    }
  }
  return status;
}

static int case_compare(const void *left_raw, const void *right_raw) {
  const CaseRange *left = (const CaseRange *)left_raw;
  const CaseRange *right = (const CaseRange *)right_raw;
  int compared = ab_string_compare(&left->selector, &right->selector);
  return compared ? compared
                  : (left->line > right->line) - (left->line < right->line);
}

static ArchbirdStatus add_message(AbMapState *state, const char *severity,
                                  const char *code, const char *prefix,
                                  const AbString *first, const char *middle,
                                  const AbString *second) {
  AbBuffer message;
  ArchbirdStatus status;
  ab_buffer_init(&message, state->engine);
  status = ab_buffer_literal(&message, prefix);
  if (status == ARCHBIRD_OK && first)
    status = ab_buffer_append(&message, first->data, first->length);
  if (status == ARCHBIRD_OK && middle)
    status = ab_buffer_literal(&message, middle);
  if (status == ARCHBIRD_OK && second)
    status = ab_buffer_append(&message, second->data, second->length);
  if (status == ARCHBIRD_OK)
    status = ab_map_add_diagnostic(
        state, severity, code, message.data ? (const char *)message.data : "",
        NULL);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus configured_routes(AbMapState *state,
                                        const TestFactIndex *fact_index,
                                        const AbConfigTest *spec,
                                        ConfiguredRoute **out) {
  ConfiguredRoute *rows = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out = NULL;
  if (spec->case_route_count) {
    rows = (ConfiguredRoute *)ab_calloc(state->engine, spec->case_route_count,
                                        sizeof(*rows));
    if (!rows)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting configured routes");
  }
  for (index = 0; status == ARCHBIRD_OK && index < spec->case_route_count;
       index++) {
    size_t file_index;
    size_t pattern_index;
    int symbol_matched = !spec->case_routes[index].target_symbols.count;
    rows[index].config = &spec->case_routes[index];
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      if (!file->has_layer)
        continue;
      for (pattern_index = 0;
           pattern_index < spec->case_routes[index].targets.count;
           pattern_index++) {
        if (ab_map_collection_match(
                &file->path,
                &spec->case_routes[index].targets.items[pattern_index])) {
          status = append_unique(state->engine, &rows[index].targets,
                                 file->path.data, file->path.length);
          if (configured_file_has_symbol(fact_index, file,
                                         &spec->case_routes[index]))
            symbol_matched = 1;
          break;
        }
      }
    }
    if (status == ARCHBIRD_OK && rows[index].targets.count > 1)
      qsort(rows[index].targets.items, rows[index].targets.count,
            sizeof(*rows[index].targets.items), string_compare);
    if (status == ARCHBIRD_OK && !rows[index].targets.count) {
      AbBuffer names;
      size_t target;
      ab_buffer_init(&names, state->engine);
      for (target = 0; status == ARCHBIRD_OK &&
                       target < spec->case_routes[index].targets.count;
           target++) {
        if (target)
          status = ab_buffer_literal(&names, ", ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(
              &names, spec->case_routes[index].targets.items[target].data,
              spec->case_routes[index].targets.items[target].length);
      }
      if (status == ARCHBIRD_OK) {
        AbBuffer message;
        ab_buffer_init(&message, state->engine);
        status = ab_buffer_literal(&message, "test group ");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&message, spec->name.data, spec->name.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, " selector ");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&message, spec->case_routes[index].selector.data,
                               spec->case_routes[index].selector.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, ": ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, names.data, names.length);
        if (status == ARCHBIRD_OK)
          status = ab_map_add_diagnostic(state, "error",
                                         "test-case-route-target-unmatched",
                                         (const char *)message.data, NULL);
        ab_buffer_free(&message);
      }
      ab_buffer_free(&names);
    }
    if (status == ARCHBIRD_OK && rows[index].targets.count && !symbol_matched)
      status = add_message(state, "error", "test-case-route-symbol-unmatched",
                           "test route target symbol unmatched: ",
                           &spec->case_routes[index].selector, NULL, NULL);
  }
  if (status != ARCHBIRD_OK) {
    for (index = 0; index < spec->case_route_count; index++)
      string_array_free(state->engine, &rows[index].targets);
    ab_free(state->engine, rows);
    return status;
  }
  *out = rows;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
analyze_test_file(AbMapState *state, const TestFactIndex *index,
                  const AbConfigTest *spec, const AbManifestFile *file,
                  ConfiguredRoute *configured, AbMapTest *out) {
  const uint8_t *source = ab_project_source_bytes(
      state->project, (size_t)(file - state->manifest->files));
  static const uint8_t empty[] = "";
  CaseRange *ranges = NULL;
  size_t range_count = 0;
  size_t registered = 0;
  size_t range_index;
  ArchbirdStatus status;
  size_t generated_index;
  const AbConfigTestGeneratedFile *generated = NULL;
  if (!source)
    source = empty;
  for (generated_index = 0; generated_index < spec->generated_file_count;
       generated_index++) {
    const AbConfigTestGeneratedFile *candidate =
        &spec->generated_files[generated_index];
    size_t glob_index;
    int matched = 0;
    for (glob_index = 0; !matched && glob_index < candidate->globs.count;
         glob_index++)
      matched = ab_map_collection_match(&file->path,
                                        &candidate->globs.items[glob_index]);
    if (!matched)
      continue;
    if (generated)
      return archbird_error_set(
          state->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "test file %.*s matches multiple generated_files entries",
          (int)file->path.length, file->path.data);
    generated = candidate;
  }
  status = ab_string_copy(state->engine, &out->path, file->path.data,
                          file->path.length);
  if (status == ARCHBIRD_OK && generated) {
    out->generated = 1;
    for (generated_index = 0;
         status == ARCHBIRD_OK && generated_index < generated->sources.count;
         generated_index++)
      status = append_unique(state->engine, &out->generated_from,
                             generated->sources.items[generated_index].data,
                             generated->sources.items[generated_index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->group, spec->name.data,
                            spec->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &out->language, spec->language.data,
                            spec->language.length);
  out->candidate = spec->discovered;
  if (status == ARCHBIRD_OK && string_literal(&spec->language, "python")) {
    status = ab_string_copy(state->engine, &out->framework, "pytest", 6);
    if (status == ARCHBIRD_OK)
      status =
          python_cases(state, index, file, source, file->byte_length, &ranges,
                       &range_count, &out->selectors, &registered);
    if (status == ARCHBIRD_OK && has_named_dispatch(spec))
      status = named_dispatch_cases(state, spec, source, file->byte_length,
                                    &ranges, &range_count, &registered);
  } else if (status == ARCHBIRD_OK &&
             (string_literal(&spec->language, "c") ||
              string_literal(&spec->language, "cpp"))) {
    status = ab_string_copy(state->engine, &out->framework, "C harness", 9);
    if (status == ARCHBIRD_OK)
      status = token_cases(state, spec, source, file->byte_length, &ranges,
                           &range_count, &out->selectors, &registered);
    if (status == ARCHBIRD_OK && spec->discovered)
      status = c_registry_cases(state, index, spec, source, file->byte_length,
                                &ranges, &range_count, &registered);
    if (status == ARCHBIRD_OK && has_named_dispatch(spec))
      status = named_dispatch_cases(state, spec, source, file->byte_length,
                                    &ranges, &range_count, &registered);
    if (status == ARCHBIRD_OK && registered == 0) {
      size_t file_index = (size_t)(file - state->manifest->files);
      size_t fact_index;
      for (fact_index = index->file_offsets[file_index];
           fact_index < index->file_offsets[file_index + 1]; fact_index++) {
        const AbFact *fact = index->facts[fact_index];
        const char *leaf;
        size_t leaf_length;
        if (!fact->has_name || !fact_domain(fact, "symbols") ||
            string_literal(&fact->kind, "declaration"))
          continue;
        fact_leaf(fact, &leaf, &leaf_length);
        if (prefix_bytes(leaf, leaf_length, "test_"))
          registered++;
      }
    }
  } else if (status == ARCHBIRD_OK &&
             (string_literal(&spec->language, "javascript") ||
              string_literal(&spec->language, "typescript") ||
              string_literal(&spec->language, "vue"))) {
    status = ab_string_copy(state->engine, &out->framework, "Node/browser", 12);
    if (status == ARCHBIRD_OK)
      status = token_cases(state, spec, source, file->byte_length, &ranges,
                           &range_count, &out->selectors, &registered);
  } else if (status == ARCHBIRD_OK && string_literal(&spec->language, "r")) {
    status = ab_string_copy(state->engine, &out->framework, "testthat", 8);
    if (status == ARCHBIRD_OK)
      status = token_cases(state, spec, source, file->byte_length, &ranges,
                           &range_count, &out->selectors, &registered);
  } else if (status == ARCHBIRD_OK) {
    status = ab_string_copy(state->engine, &out->framework, "unknown", 7);
  }
  out->count = registered;
  if (status == ARCHBIRD_OK && out->selectors.count > 1)
    qsort(out->selectors.items, out->selectors.count,
          sizeof(*out->selectors.items), string_compare);
  if (status == ARCHBIRD_OK)
    status = resolve_routes(state, index, spec, file, 0, 0, 0, &out->routes,
                            &out->route_count, &out->route_evidence,
                            &out->route_evidence_count);
  if (status == ARCHBIRD_OK && range_count > 1)
    qsort(ranges, range_count, sizeof(*ranges), case_compare);
  if (status == ARCHBIRD_OK && range_count) {
    out->cases = (AbMapTestCase *)ab_calloc(state->engine, range_count,
                                            sizeof(*out->cases));
    if (!out->cases)
      status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory collecting test case routes");
  }
  for (range_index = 0; status == ARCHBIRD_OK && range_index < range_count;
       range_index++) {
    AbMapTestCase *test_case = &out->cases[out->case_count++];
    size_t configured_index;
    status = ab_string_copy(state->engine, &test_case->selector,
                            ranges[range_index].selector.data,
                            ranges[range_index].selector.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &test_case->evidence_kind,
                              ranges[range_index].evidence_kind,
                              strlen(ranges[range_index].evidence_kind));
    test_case->line = ranges[range_index].line;
    if (status == ARCHBIRD_OK)
      status = resolve_routes(
          state, index, spec, file, 1, ranges[range_index].start,
          ranges[range_index].end, &test_case->routes, &test_case->route_count,
          &test_case->route_evidence, &test_case->route_evidence_count);
    if (status == ARCHBIRD_OK && ranges[range_index].callback.length) {
      const RouteSymbol *callback =
          unique_route_symbol(index, spec, ranges[range_index].callback.data,
                              ranges[range_index].callback.length);
      if (callback)
        status = add_evidenced_route(
            state, &test_case->routes, &test_case->route_count,
            &test_case->route_evidence, &test_case->route_evidence_count,
            &callback->file->path, &callback->fact->name, callback->fact,
            callback->provider, "test-registration-candidate", "case");
      if (status == ARCHBIRD_OK && callback)
        status = add_evidenced_route(
            state, &out->routes, &out->route_count, &out->route_evidence,
            &out->route_evidence_count, &callback->file->path,
            &callback->fact->name, callback->fact, callback->provider,
            "test-registration-candidate", "case");
    }
    for (configured_index = 0;
         status == ARCHBIRD_OK && configured_index < spec->case_route_count;
         configured_index++) {
      const AbConfigTestCaseRoute *route = configured[configured_index].config;
      size_t path_index;
      int path_matched = !route->paths.count;
      if (!ab_map_glob_match(&route->selector, &test_case->selector))
        continue;
      for (path_index = 0; !path_matched && path_index < route->paths.count;
           path_index++)
        path_matched = ab_map_collection_match(&file->path,
                                               &route->paths.items[path_index]);
      if (!path_matched)
        continue;
      configured[configured_index].matched = 1;
      for (path_index = 0;
           status == ARCHBIRD_OK &&
           path_index < configured[configured_index].targets.count;
           path_index++) {
        const AbString *target =
            &configured[configured_index].targets.items[path_index];
        status = append_unique(state->engine, &test_case->configured_routes,
                               target->data, target->length);
        if (status == ARCHBIRD_OK)
          status = add_route(state, &test_case->routes, &test_case->route_count,
                             target, 1);
        if (status == ARCHBIRD_OK)
          status = add_route(state, &out->routes, &out->route_count, target, 1);
        if (status == ARCHBIRD_OK && !route->target_symbols.count) {
          status =
              add_route_evidence(state, &test_case->route_evidence,
                                 &test_case->route_evidence_count, target, NULL,
                                 NULL, NULL, "configured", "case", "asserted",
                                 &test_case->selector, test_case->line);
          if (status == ARCHBIRD_OK)
            status = add_route_evidence(
                state, &out->route_evidence, &out->route_evidence_count, target,
                NULL, NULL, NULL, "configured", "case", "asserted",
                &test_case->selector, test_case->line);
        } else if (status == ARCHBIRD_OK) {
          const AbManifestFile *target_file = ab_map_manifest_file(
              state->manifest, target->data, target->length);
          size_t symbol_index;
          for (symbol_index = 0;
               status == ARCHBIRD_OK && symbol_index < index->symbol_count;
               symbol_index++) {
            const RouteSymbol *symbol = &index->symbols[symbol_index];
            if (symbol->file != target_file ||
                !configured_symbol_matches(route, symbol->fact))
              continue;
            status = add_route_evidence(state, &test_case->route_evidence,
                                        &test_case->route_evidence_count,
                                        target, &symbol->fact->name, NULL, NULL,
                                        "configured", "case", "asserted",
                                        &test_case->selector, test_case->line);
            if (status == ARCHBIRD_OK)
              status = add_route_evidence(
                  state, &out->route_evidence, &out->route_evidence_count,
                  target, &symbol->fact->name, NULL, NULL, "configured", "case",
                  "asserted", &test_case->selector, test_case->line);
          }
        }
      }
    }
    if (status == ARCHBIRD_OK && test_case->configured_routes.count > 1)
      qsort(test_case->configured_routes.items,
            test_case->configured_routes.count,
            sizeof(*test_case->configured_routes.items), string_compare);
    if (status == ARCHBIRD_OK && test_case->route_count > 1)
      qsort(test_case->routes, test_case->route_count,
            sizeof(*test_case->routes), route_compare);
    if (status == ARCHBIRD_OK && test_case->route_evidence_count > 1)
      qsort(test_case->route_evidence, test_case->route_evidence_count,
            sizeof(*test_case->route_evidence), route_evidence_compare);
  }
  if (status == ARCHBIRD_OK && out->route_count > 1)
    qsort(out->routes, out->route_count, sizeof(*out->routes), route_compare);
  if (status == ARCHBIRD_OK && out->route_evidence_count > 1)
    qsort(out->route_evidence, out->route_evidence_count,
          sizeof(*out->route_evidence), route_evidence_compare);
  for (range_index = 0; range_index < range_count; range_index++)
    ab_string_free(state->engine, &ranges[range_index].selector);
  for (range_index = 0; range_index < range_count; range_index++)
    ab_string_free(state->engine, &ranges[range_index].callback);
  ab_free(state->engine, ranges);
  return status;
}

static int test_compare(const void *left_raw, const void *right_raw) {
  const AbMapTest *left = (const AbMapTest *)left_raw;
  const AbMapTest *right = (const AbMapTest *)right_raw;
  int compared = ab_string_compare(&left->path, &right->path);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->group, &right->group);
  return compared ? compared
                  : ab_string_compare(&left->framework, &right->framework);
}

static int observed_symbol_is_mapped(const TestFactIndex *index,
                                     const AbString *path,
                                     const AbString *symbol) {
  size_t index_value;
  for (index_value = 0; index_value < index->symbol_count; index_value++)
    if (ab_string_equal(&index->symbols[index_value].file->path, path) &&
        ab_string_equal(&index->symbols[index_value].fact->name, symbol))
      return 1;
  return 0;
}

static size_t
observed_test_case_matches(AbMapState *state, const AbString *group,
                           const AbString *path, const AbString *selector,
                           AbMapTest **out_test, AbMapTestCase **out_case) {
  size_t test_index;
  size_t matches = 0;
  *out_test = NULL;
  *out_case = NULL;
  for (test_index = 0; test_index < state->test_count; test_index++) {
    AbMapTest *test = &state->tests[test_index];
    size_t case_index;
    if (!ab_string_equal(&test->group, group) ||
        !ab_string_equal(&test->path, path))
      continue;
    for (case_index = 0; case_index < test->case_count; case_index++)
      if (ab_string_equal(&test->cases[case_index].selector, selector)) {
        matches++;
        *out_test = test;
        *out_case = &test->cases[case_index];
      }
  }
  return matches;
}

static ArchbirdStatus
observation_digest(AbMapState *state, const AbValue *document, AbString *out) {
  AbBuffer canonical;
  uint8_t digest[32];
  char hex[65];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, state->engine);
  status = ab_value_render(&canonical, document);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  ab_buffer_free(&canonical);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_hex(digest, hex);
  return ab_string_copy(state->engine, out, hex, 64);
}

static ArchbirdStatus add_observed_test_routes(AbMapState *state,
                                               const TestFactIndex *index) {
  size_t document_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (document_index = 0;
       status == ARCHBIRD_OK &&
       document_index < ab_project_test_observation_count(state->project);
       document_index++) {
    const AbValue *document =
        ab_project_test_observation(state->project, document_index);
    const AbValue *cases = ab_value_member(document, "cases");
    AbString digest = {0};
    size_t case_index;
    status = observation_digest(state, document, &digest);
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *observed_case = &cases->as.array.items[case_index];
      const AbString *group = &ab_value_member(observed_case, "group")->as.text;
      const AbString *path = &ab_value_member(observed_case, "path")->as.text;
      const AbString *selector =
          &ab_value_member(observed_case, "selector")->as.text;
      const AbValue *symbols = ab_value_member(observed_case, "symbols");
      AbMapTest *test;
      AbMapTestCase *test_case;
      size_t matches = observed_test_case_matches(state, group, path, selector,
                                                  &test, &test_case);
      size_t symbol_index;
      if (!matches) {
        status = ab_map_add_diagnostic(
            state, "warning", "observed-test-case-unmapped",
            "observed test selector is not present in the mapped test "
            "inventory",
            path);
        continue;
      }
      if (matches > 1) {
        status = ab_map_add_diagnostic(
            state, "warning", "observed-test-case-ambiguous",
            "observed test identity matches more than one mapped test case",
            path);
        continue;
      }
      for (symbol_index = 0;
           status == ARCHBIRD_OK && symbol_index < symbols->as.array.count;
           symbol_index++) {
        const AbValue *symbol = &symbols->as.array.items[symbol_index];
        const AbString *target = &ab_value_member(symbol, "path")->as.text;
        const AbString *target_symbol =
            &ab_value_member(symbol, "symbol")->as.text;
        if (!observed_symbol_is_mapped(index, target, target_symbol)) {
          status = ab_map_add_diagnostic(
              state, "warning", "observed-symbol-unmapped",
              "observed symbol identity is not present in the current Map",
              target);
          continue;
        }
        status = add_observed_route_evidence(state, &test_case->route_evidence,
                                             &test_case->route_evidence_count,
                                             document, symbol, &digest);
        if (status == ARCHBIRD_OK)
          status = add_observed_route_evidence(state, &test->route_evidence,
                                               &test->route_evidence_count,
                                               document, symbol, &digest);
      }
    }
    ab_string_free(state->engine, &digest);
  }
  if (status != ARCHBIRD_OK)
    return status;
  for (document_index = 0; document_index < state->test_count;
       document_index++) {
    AbMapTest *test = &state->tests[document_index];
    size_t case_index;
    if (test->route_evidence_count > 1)
      qsort(test->route_evidence, test->route_evidence_count,
            sizeof(*test->route_evidence), route_evidence_compare);
    for (case_index = 0; case_index < test->case_count; case_index++)
      if (test->cases[case_index].route_evidence_count > 1)
        qsort(test->cases[case_index].route_evidence,
              test->cases[case_index].route_evidence_count,
              sizeof(*test->cases[case_index].route_evidence),
              route_evidence_compare);
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_map_analyze_tests(AbMapState *state) {
  TestFactIndex index;
  size_t spec_index;
  ArchbirdStatus status = test_fact_index_build(state, &index);
  for (spec_index = 0;
       status == ARCHBIRD_OK && spec_index < state->config->test_count;
       spec_index++) {
    const AbConfigTest *spec = &state->config->tests[spec_index];
    ConfiguredRoute *configured = NULL;
    size_t file_index;
    size_t matched_files = 0;
    status = configured_routes(state, &index, spec, &configured);
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      AbMapTest *resized;
      AbMapTest *test;
      if (!test_path_matches(spec, file))
        continue;
      matched_files++;
      resized = (AbMapTest *)ab_realloc(state->engine, state->tests,
                                        (state->test_count + 1) *
                                            sizeof(*state->tests));
      if (!resized) {
        status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory collecting test files");
        break;
      }
      state->tests = resized;
      test = &state->tests[state->test_count++];
      memset(test, 0, sizeof(*test));
      status = analyze_test_file(state, &index, spec, file, configured, test);
    }
    if (status == ARCHBIRD_OK && spec->required && !matched_files) {
      AbBuffer globs;
      size_t glob;
      ab_buffer_init(&globs, state->engine);
      for (glob = 0; status == ARCHBIRD_OK && glob < spec->globs.count;
           glob++) {
        if (glob)
          status = ab_buffer_literal(&globs, ", ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&globs, spec->globs.items[glob].data,
                                    spec->globs.items[glob].length);
      }
      if (status == ARCHBIRD_OK)
        status = add_message(state, "error", "unmatched-test-glob",
                             "test group ", &spec->name, ": ",
                             &(AbString){(char *)globs.data, globs.length});
      ab_buffer_free(&globs);
    }
    if (status == ARCHBIRD_OK) {
      size_t route_index;
      for (route_index = 0; route_index < spec->case_route_count;
           route_index++) {
        if (!configured[route_index].matched) {
          status =
              add_message(state, "error", "test-case-route-selector-unmatched",
                          "test group ", &spec->name, ": ",
                          &spec->case_routes[route_index].selector);
          if (status != ARCHBIRD_OK)
            break;
        }
      }
    }
    if (configured) {
      size_t route_index;
      for (route_index = 0; route_index < spec->case_route_count; route_index++)
        string_array_free(state->engine, &configured[route_index].targets);
      ab_free(state->engine, configured);
    }
  }
  if (status == ARCHBIRD_OK) {
    size_t file_index;
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      AbConfigTest spec;
      AbMapTest *resized;
      AbMapTest *test;
      if (!file->has_language ||
          !ab_manifest_file_has_role(file, "test-candidate"))
        continue;
      memset(&spec, 0, sizeof(spec));
      spec.name.data = "discovery";
      spec.name.length = 9;
      spec.language = file->language;
      spec.discovered = 1;
      resized = (AbMapTest *)ab_realloc(state->engine, state->tests,
                                        (state->test_count + 1) *
                                            sizeof(*state->tests));
      if (!resized) {
        status = archbird_error_set(
            state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory collecting discovered test candidates");
        break;
      }
      state->tests = resized;
      test = &state->tests[state->test_count++];
      memset(test, 0, sizeof(*test));
      status = analyze_test_file(state, &index, &spec, file, NULL, test);
    }
  }
  if (status == ARCHBIRD_OK)
    status = add_observed_test_routes(state, &index);
  if (status == ARCHBIRD_OK && state->test_count > 1)
    qsort(state->tests, state->test_count, sizeof(*state->tests), test_compare);
  test_fact_index_free(state->engine, &index);
  return status;
}

static ArchbirdStatus
render_routes(AbBuffer *buffer, const AbMapRouteCount *routes, size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, routes[index].path.data,
                                     routes[index].path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, routes[index].count);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_route_evidence(AbBuffer *buffer,
                                            const AbMapRouteEvidence *rows,
                                            size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbMapRouteEvidence *row = &rows[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"claim\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, row->claim.data, row->claim.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"enclosing\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->enclosing.data,
                                     row->enclosing.length);
    if (status == ARCHBIRD_OK && row->evidence_slice_sha256.length)
      status = ab_buffer_literal(buffer, ",\"evidence_slice_sha256\":");
    if (status == ARCHBIRD_OK && row->evidence_slice_sha256.length)
      status = ab_buffer_json_string(buffer, row->evidence_slice_sha256.data,
                                     row->evidence_slice_sha256.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"fact_id\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, row->fact_id.data, row->fact_id.length);
    if (status == ARCHBIRD_OK && row->has_hits)
      status = ab_buffer_literal(buffer, ",\"hits\":");
    if (status == ARCHBIRD_OK && row->has_hits)
      status = ab_buffer_u64(buffer, row->hits);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->line);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->name.data, row->name.length);
    if (status == ARCHBIRD_OK && row->observation_sha256.length)
      status = ab_buffer_literal(buffer, ",\"observation_sha256\":");
    if (status == ARCHBIRD_OK && row->observation_sha256.length)
      status = ab_buffer_json_string(buffer, row->observation_sha256.data,
                                     row->observation_sha256.length);
    if (status == ARCHBIRD_OK && row->producer_configuration_sha256.length)
      status = ab_buffer_literal(buffer, ",\"producer_configuration_sha256\":");
    if (status == ARCHBIRD_OK && row->producer_configuration_sha256.length)
      status =
          ab_buffer_json_string(buffer, row->producer_configuration_sha256.data,
                                row->producer_configuration_sha256.length);
    if (status == ARCHBIRD_OK && row->producer_implementation_sha256.length)
      status =
          ab_buffer_literal(buffer, ",\"producer_implementation_sha256\":");
    if (status == ARCHBIRD_OK && row->producer_implementation_sha256.length)
      status = ab_buffer_json_string(
          buffer, row->producer_implementation_sha256.data,
          row->producer_implementation_sha256.length);
    if (status == ARCHBIRD_OK && row->producer_version.length)
      status = ab_buffer_literal(buffer, ",\"producer_version\":");
    if (status == ARCHBIRD_OK && row->producer_version.length)
      status = ab_buffer_json_string(buffer, row->producer_version.data,
                                     row->producer_version.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"provenance\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->provenance.data,
                                     row->provenance.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"provider\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->provider.data,
                                     row->provider.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"relation\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->relation.data,
                                     row->relation.length);
    if (status == ARCHBIRD_OK && row->runtime.length)
      status = ab_buffer_literal(buffer, ",\"runtime\":");
    if (status == ARCHBIRD_OK && row->runtime.length)
      status =
          ab_buffer_json_string(buffer, row->runtime.data, row->runtime.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"scope\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, row->scope.data, row->scope.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"span\":{\"end\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->span_end);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"start\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, row->span_start);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "},\"target\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, row->target.data, row->target.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"target_symbol\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, row->target_symbol.data,
                                     row->target_symbol.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_strings(AbBuffer *buffer,
                                     const AbStringArray *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, values->items[index].data,
                                     values->items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

ArchbirdStatus ab_map_render_tests(AbBuffer *buffer, const AbMapState *state) {
  size_t test_index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (test_index = 0; status == ARCHBIRD_OK && test_index < state->test_count;
       test_index++) {
    const AbMapTest *test = &state->tests[test_index];
    size_t case_index;
    if (test_index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"cases\":[");
    for (case_index = 0; status == ARCHBIRD_OK && case_index < test->case_count;
         case_index++) {
      const AbMapTestCase *test_case = &test->cases[case_index];
      if (case_index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"configured_routes\":");
      if (status == ARCHBIRD_OK)
        status = render_strings(buffer, &test_case->configured_routes);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"evidence_kind\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, test_case->evidence_kind.data,
                                       test_case->evidence_kind.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"line\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, test_case->line);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"routes\":");
      if (status == ARCHBIRD_OK)
        status =
            render_routes(buffer, test_case->routes, test_case->route_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"route_evidence\":");
      if (status == ARCHBIRD_OK)
        status = render_route_evidence(buffer, test_case->route_evidence,
                                       test_case->route_evidence_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"selector\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, test_case->selector.data,
                                       test_case->selector.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"count\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, test->count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer,
                                 ",\"count_unit\":\"static_case_occurrence\"");
    if (status == ARCHBIRD_OK && test->candidate)
      status = ab_buffer_literal(buffer, ",\"inventory_source\":\"discovery\","
                                         "\"inventory_state\":\"candidate\"");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"framework\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, test->framework.data,
                                     test->framework.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"generated\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, test->generated ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"generated_from\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &test->generated_from);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"group\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, test->group.data, test->group.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"language\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, test->language.data,
                                     test->language.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, test->path.data, test->path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"routes\":");
    if (status == ARCHBIRD_OK)
      status = render_routes(buffer, test->routes, test->route_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"route_evidence\":");
    if (status == ARCHBIRD_OK)
      status = render_route_evidence(buffer, test->route_evidence,
                                     test->route_evidence_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"selectors\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &test->selectors);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
