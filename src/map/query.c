#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "json_value.h"
#include "map_internal.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct QueryRequest {
  const AbValue *focus;
  const AbValue *paths;
  const AbValue *symbols;
  const AbValue *components;
  const AbValue *packages;
  const AbValue *artifacts;
  const AbValue *context;
  const AbString *direction;
  const AbString *producer_policy;
  size_t depth;
  size_t test_depth;
} QueryRequest;

typedef struct QueryFile {
  const AbValue *row;
  const AbString *path;
  const AbString *layer;
  const AbString *language;
  const AbString *sha256;
  const AbValue *symbols;
  size_t original_index;
  size_t distance;
  size_t degree;
  int seed;
  int symbol_seed;
  int matched_symbol_seed;
  int related_symbol;
  int requested_seed;
  int selected;
} QueryFile;

typedef struct QueryEdge {
  const AbValue *row;
  const AbString *kind;
  const AbString *source;
  const AbString *target;
  const AbValue *names;
  size_t source_index;
  size_t target_index;
  int target_mapped;
} QueryEdge;

typedef struct QuerySymbol {
  size_t file_index;
  const AbValue *row;
  const AbString *path;
  const AbString *name;
  const AbString *kind;
  const AbString *scope;
  size_t line;
} QuerySymbol;

typedef struct QueryRelatedSymbol {
  size_t file_index;
  const AbValue *row;
  const AbString *name;
} QueryRelatedSymbol;

typedef struct QueryTestRow {
  size_t test_index;
  const AbValue *test;
  const AbValue *case_row;
  const AbString *path;
  const AbString *group;
  const AbString *selector;
  size_t line;
  const AbValue *routes;
  const AbValue *route_evidence;
  const AbValue *configured;
  const AbString *inventory_state;
  int focused;
  int evidence_v2;
} QueryTestRow;

typedef struct QueryTestMatch {
  const QueryTestRow *row;
  size_t *route;
  size_t route_count;
  const char *evidence;
  const char *classification;
  const char *provenance;
  const char *confidence;
  const char *evidence_scope;
  const char *target_role;
  size_t seed_distance;
  size_t evidence_target;
  size_t ranking_affinity;
} QueryTestMatch;

typedef struct QueryContext {
  ArchbirdEngine *engine;
  const AbValue *map;
  QueryRequest request;
  QueryFile *files;
  size_t file_count;
  QueryEdge *edges;
  size_t edge_count;
  QuerySymbol *symbols;
  size_t symbol_count;
  size_t symbol_capacity;
  QueryRelatedSymbol *related_symbols;
  size_t related_symbol_count;
  size_t related_symbol_capacity;
  QueryTestRow *test_rows;
  size_t test_row_count;
  QueryTestMatch *test_matches;
  size_t test_match_count;
  size_t test_match_capacity;
  int *test_selected;
  size_t test_count;
  int has_focused_tests;
  int *package_selected;
  size_t package_count;
  int *artifact_selected;
  size_t artifact_count;
  int *component_selected;
  size_t component_count;
  const AbValue *symbol_calls;
  const AbValue *symbol_references;
  const char *producer_compatibility;
  int exact_symbol_scope;
} QueryContext;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int valid_sha256(const AbString *value) {
  size_t index;
  if (!value || value->length != 64)
    return 0;
  for (index = 0; index < value->length; index++)
    if (!((value->data[index] >= '0' && value->data[index] <= '9') ||
          (value->data[index] >= 'a' && value->data[index] <= 'f')))
      return 0;
  return 1;
}

static int selected_symbol_identity(const QueryContext *context,
                                    const AbString *path, const AbString *name);
static const AbString *match_target_symbol(const QueryContext *context,
                                           const QueryTestMatch *match);
static ArchbirdStatus append_related_symbol_row(QueryContext *context,
                                                size_t file_index,
                                                const AbValue *row,
                                                const AbString *name);

static ArchbirdStatus query_error(QueryContext *context, const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static const AbValue *optional_array(const AbValue *object, const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  const AbValue *value = ab_value_member(object, name);
  return value ? value : &empty;
}

static ArchbirdStatus build_symbol_calls(QueryContext *context) {
  context->symbol_calls = optional_array(context->map, "symbol_calls");
  if (context->symbol_calls->kind != AB_VALUE_ARRAY)
    return query_error(context, "map.symbol_calls must be an array");
  return ARCHBIRD_OK;
}

static ArchbirdStatus build_symbol_references(QueryContext *context) {
  context->symbol_references =
      optional_array(context->map, "symbol_references");
  if (context->symbol_references->kind != AB_VALUE_ARRAY)
    return query_error(context, "map.symbol_references must be an array");
  return ARCHBIRD_OK;
}

static const AbString *required_string(QueryContext *context,
                                       const AbValue *object, const char *name,
                                       const char *where) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_STRING) {
    archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET, "%s.%s must be a string", where,
                       name);
    return NULL;
  }
  return &value->as.text;
}

static const AbValue *required_array(QueryContext *context,
                                     const AbValue *object, const char *name,
                                     const char *where) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_ARRAY) {
    archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET, "%s.%s must be an array", where,
                       name);
    return NULL;
  }
  return value;
}

static int string_array_valid(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    if (value->as.array.items[index].kind != AB_VALUE_STRING)
      return 0;
  }
  return 1;
}

static int context_string_allowed(const AbString *value,
                                  const char *const *allowed,
                                  size_t allowed_count) {
  size_t index;
  for (index = 0; index < allowed_count; index++)
    if (string_literal(value, allowed[index]))
      return 1;
  return 0;
}

static int context_enum_array_valid(const AbValue *value,
                                    const char *const *allowed,
                                    size_t allowed_count) {
  size_t index;
  if (!string_array_valid(value))
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (!context_string_allowed(&value->as.array.items[index].as.text, allowed,
                                allowed_count))
      return 0;
  return 1;
}

static ArchbirdStatus validate_context_counts(QueryContext *context,
                                              const AbValue *value,
                                              const char *field) {
  static const char *const allowed[] = {"files", "symbol_calls",
                                        "symbol_references", "test_matches"};
  size_t index;
  uint64_t count;
  if (!value)
    return ARCHBIRD_OK;
  if (value->kind != AB_VALUE_OBJECT)
    return query_error(context, "query context counts must be objects");
  for (index = 0; index < value->as.object.count; index++) {
    const AbObjectField *row = &value->as.object.fields[index];
    if (!context_string_allowed(&row->name, allowed,
                                sizeof(allowed) / sizeof(allowed[0])) ||
        !ab_value_u64(&row->value, &count) || count > SIZE_MAX)
      return archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "query.context.%s values must be nonnegative integers for known "
          "fact kinds",
          field);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_context_request(QueryContext *context,
                                               const AbValue *value) {
  static const AbValue empty = {.kind = AB_VALUE_OBJECT};
  static const char *const profiles[] = {"exact", "change", "architecture",
                                         "audit"};
  static const char *const provenances[] = {"derived", "asserted", "observed"};
  static const char *const confidences[] = {"exact", "candidate",
                                            "conservative", "unresolved"};
  static const char *const route_modes[] = {"collapse", "expand", "exclude"};
  size_t index;
  uint64_t distance;
  context->request.context = value ? value : &empty;
  if (context->request.context->kind != AB_VALUE_OBJECT)
    return query_error(context, "query.context must be an object");
  for (index = 0; index < context->request.context->as.object.count; index++) {
    const AbObjectField *field =
        &context->request.context->as.object.fields[index];
    if (string_literal(&field->name, "profile")) {
      if (field->value.kind != AB_VALUE_STRING ||
          !context_string_allowed(&field->value.as.text, profiles,
                                  sizeof(profiles) / sizeof(profiles[0])))
        return query_error(
            context,
            "query.context.profile must be exact, change, architecture, or "
            "audit");
    } else if (string_literal(&field->name, "provenance")) {
      if (!context_enum_array_valid(&field->value, provenances,
                                    sizeof(provenances) /
                                        sizeof(provenances[0])))
        return query_error(
            context, "query.context.provenance contains an invalid value");
    } else if (string_literal(&field->name, "confidence")) {
      if (!context_enum_array_valid(&field->value, confidences,
                                    sizeof(confidences) /
                                        sizeof(confidences[0])))
        return query_error(
            context, "query.context.confidence contains an invalid value");
    } else if (string_literal(&field->name, "max_seed_distance")) {
      if (!ab_value_u64(&field->value, &distance) || distance > SIZE_MAX)
        return query_error(
            context,
            "query.context.max_seed_distance must be a nonnegative integer");
    } else if (string_literal(&field->name, "candidate") ||
               string_literal(&field->name, "conservative")) {
      if (field->value.kind != AB_VALUE_STRING ||
          !context_string_allowed(&field->value.as.text, route_modes,
                                  sizeof(route_modes) / sizeof(route_modes[0])))
        return query_error(
            context,
            "query context candidate/conservative policy must be collapse, "
            "expand, or exclude");
    } else if (string_literal(&field->name, "quotas")) {
      ArchbirdStatus status =
          validate_context_counts(context, &field->value, "quotas");
      if (status != ARCHBIRD_OK)
        return status;
    } else if (string_literal(&field->name, "offsets")) {
      ArchbirdStatus status =
          validate_context_counts(context, &field->value, "offsets");
      if (status != ARCHBIRD_OK)
        return status;
    } else {
      return query_error(context, "query.context contains an unknown field");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus decode_request(QueryContext *context,
                                     const AbValue *request) {
  const AbValue *direction;
  const AbValue *depth;
  const AbValue *test_depth;
  uint64_t number;
  static const AbString both = {(char *)"both", 4};
  static const AbString compatible = {(char *)"compatible", 10};
  const AbValue *producer_policy;
  if (!request || request->kind != AB_VALUE_OBJECT)
    return query_error(context, "query request must be an object");
  context->request.focus = optional_array(request, "focus");
  context->request.paths = optional_array(request, "paths");
  context->request.symbols = optional_array(request, "symbols");
  context->request.components = optional_array(request, "components");
  context->request.packages = optional_array(request, "packages");
  context->request.artifacts = optional_array(request, "artifacts");
  if (!string_array_valid(context->request.focus) ||
      !string_array_valid(context->request.paths) ||
      !string_array_valid(context->request.symbols) ||
      !string_array_valid(context->request.components) ||
      !string_array_valid(context->request.packages) ||
      !string_array_valid(context->request.artifacts))
    return query_error(context, "query selectors must be arrays of strings");
  if (validate_context_request(context, ab_value_member(request, "context")) !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  direction = ab_value_member(request, "direction");
  context->request.direction = direction && direction->kind == AB_VALUE_STRING
                                   ? &direction->as.text
                                   : &both;
  if (!string_literal(context->request.direction, "upstream") &&
      !string_literal(context->request.direction, "downstream") &&
      !string_literal(context->request.direction, "both"))
    return query_error(
        context, "query.direction must be one of both, downstream, upstream");
  producer_policy = ab_value_member(request, "producer_policy");
  if (producer_policy && producer_policy->kind != AB_VALUE_STRING)
    return query_error(context, "query.producer_policy must be a string");
  context->request.producer_policy =
      producer_policy && producer_policy->kind == AB_VALUE_STRING
          ? &producer_policy->as.text
          : &compatible;
  if (!string_literal(context->request.producer_policy, "compatible") &&
      !string_literal(context->request.producer_policy, "current"))
    return query_error(context,
                       "query.producer_policy must be compatible or current");
  context->request.depth = 1;
  depth = ab_value_member(request, "depth");
  if (depth) {
    if (!ab_value_u64(depth, &number) || number > SIZE_MAX)
      return query_error(context, "query.depth must be an integer >= 0");
    context->request.depth = (size_t)number;
  }
  context->request.test_depth = 8;
  test_depth = ab_value_member(request, "test_depth");
  if (test_depth) {
    if (!ab_value_u64(test_depth, &number) || number > SIZE_MAX)
      return query_error(context, "query.test_depth must be an integer >= 0");
    context->request.test_depth = (size_t)number;
  }
  if (!context->request.focus->as.array.count &&
      !context->request.paths->as.array.count &&
      !context->request.symbols->as.array.count &&
      !context->request.components->as.array.count &&
      !context->request.packages->as.array.count &&
      !context->request.artifacts->as.array.count)
    return query_error(context, "query requires at least one focus selector");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_producer_policy(QueryContext *context) {
  const AbValue *tool = ab_value_member(context->map, "tool");
  const AbValue *digest = tool && tool->kind == AB_VALUE_OBJECT
                              ? ab_value_member(tool, "implementation_sha256")
                              : NULL;
  const AbString *value =
      digest && digest->kind == AB_VALUE_STRING ? &digest->as.text : NULL;
  int current = valid_sha256(value) &&
                string_literal(value, ARCHBIRD_IMPLEMENTATION_SHA256);
  context->producer_compatibility =
      current ? "current" : (valid_sha256(value) ? "different" : "unknown");
  if (!string_literal(context->request.producer_policy, "current"))
    return ARCHBIRD_OK;
  if (!valid_sha256(value))
    return archbird_error_set(
        context->engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "saved Map core implementation digest is missing or invalid");
  if (!current)
    return archbird_error_set(
        context->engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "saved Map core %.*s does not match active core %s", (int)value->length,
        value->data, ARCHBIRD_IMPLEMENTATION_SHA256);
  return ARCHBIRD_OK;
}

static int file_compare(const void *left_raw, const void *right_raw) {
  const QueryFile *left = (const QueryFile *)left_raw;
  const QueryFile *right = (const QueryFile *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static size_t find_file(const QueryContext *context, const AbString *path) {
  size_t low = 0;
  size_t high = context->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(context->files[middle].path, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return middle;
  }
  return SIZE_MAX;
}

static ArchbirdStatus build_files(QueryContext *context) {
  const AbValue *files = required_array(context, context->map, "files", "map");
  size_t index;
  if (!files)
    return ARCHBIRD_INVALID_SCHEMA;
  context->file_count = files->as.array.count;
  if (context->file_count) {
    context->files = (QueryFile *)ab_calloc(
        context->engine, context->file_count, sizeof(*context->files));
    if (!context->files)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing query files");
  }
  for (index = 0; index < context->file_count; index++) {
    const AbValue *row = &files->as.array.items[index];
    QueryFile *file = &context->files[index];
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.files[] must be objects");
    file->row = row;
    file->path = required_string(context, row, "path", "map.files[]");
    file->layer = required_string(context, row, "layer", "map.files[]");
    file->language = required_string(context, row, "language", "map.files[]");
    file->sha256 = required_string(context, row, "sha256", "map.files[]");
    file->symbols = required_array(context, row, "symbols", "map.files[]");
    file->original_index = index;
    file->distance = SIZE_MAX;
    if (!file->path || !file->layer || !file->language || !file->sha256 ||
        !file->symbols)
      return ARCHBIRD_INVALID_SCHEMA;
  }
  if (context->file_count > 1)
    qsort(context->files, context->file_count, sizeof(*context->files),
          file_compare);
  for (index = 1; index < context->file_count; index++) {
    if (ab_string_equal(context->files[index - 1].path,
                        context->files[index].path))
      return query_error(context, "map.files contains duplicate paths");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus build_edges(QueryContext *context) {
  const AbValue *edges = required_array(context, context->map, "edges", "map");
  size_t index;
  if (!edges)
    return ARCHBIRD_INVALID_SCHEMA;
  context->edge_count = edges->as.array.count;
  if (context->edge_count) {
    context->edges = (QueryEdge *)ab_calloc(
        context->engine, context->edge_count, sizeof(*context->edges));
    if (!context->edges)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing query edges");
  }
  for (index = 0; index < context->edge_count; index++) {
    QueryEdge *edge = &context->edges[index];
    const AbValue *row = &edges->as.array.items[index];
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.edges[] must be objects");
    edge->row = row;
    edge->kind = required_string(context, row, "kind", "map.edges[]");
    edge->source = required_string(context, row, "source", "map.edges[]");
    edge->target = required_string(context, row, "target", "map.edges[]");
    edge->names = required_array(context, row, "names", "map.edges[]");
    if (!edge->kind || !edge->source || !edge->target || !edge->names ||
        !string_array_valid(edge->names))
      return ARCHBIRD_INVALID_SCHEMA;
    edge->source_index = find_file(context, edge->source);
    edge->target_index = find_file(context, edge->target);
    edge->target_mapped = edge->target_index != SIZE_MAX;
    if (edge->source_index != SIZE_MAX && edge->target_mapped) {
      context->files[edge->source_index].degree++;
      context->files[edge->target_index].degree++;
    }
  }
  return ARCHBIRD_OK;
}

static void query_context_free(QueryContext *context) {
  ab_free(context->engine, context->files);
  ab_free(context->engine, context->edges);
  ab_free(context->engine, context->symbols);
  ab_free(context->engine, context->related_symbols);
  ab_free(context->engine, context->test_rows);
  if (context->test_matches) {
    size_t index;
    for (index = 0; index < context->test_match_count; index++)
      ab_free(context->engine, context->test_matches[index].route);
  }
  ab_free(context->engine, context->test_matches);
  ab_free(context->engine, context->test_selected);
  ab_free(context->engine, context->package_selected);
  ab_free(context->engine, context->artifact_selected);
  ab_free(context->engine, context->component_selected);
  memset(context, 0, sizeof(*context));
}

static int match_value(const AbString *value, const AbString *pattern) {
  return ab_string_equal(value, pattern) || ab_map_glob_match(pattern, value);
}

static int array_matches(const AbValue *patterns, const AbString *value) {
  size_t index;
  for (index = 0; index < patterns->as.array.count; index++) {
    if (match_value(value, &patterns->as.array.items[index].as.text))
      return 1;
  }
  return 0;
}

static int path_matches(const AbString *path, const AbString *raw_pattern) {
  AbString pattern = *raw_pattern;
  size_t index;
  int wildcard = 0;
  if (pattern.length >= 2 && pattern.data[0] == '.' && pattern.data[1] == '/') {
    pattern.data += 2;
    pattern.length -= 2;
  }
  while (pattern.length && pattern.data[pattern.length - 1] == '/')
    pattern.length--;
  if (!pattern.length)
    return 0;
  if (match_value(path, &pattern))
    return 1;
  for (index = 0; index < pattern.length; index++) {
    if (pattern.data[index] == '*' || pattern.data[index] == '?' ||
        pattern.data[index] == '[') {
      wildcard = 1;
      break;
    }
  }
  return !wildcard && path->length > pattern.length &&
         path->data[pattern.length] == '/' &&
         memcmp(path->data, pattern.data, pattern.length) == 0;
}

static int path_array_matches(const AbValue *patterns, const AbString *path) {
  size_t index;
  for (index = 0; index < patterns->as.array.count; index++) {
    if (path_matches(path, &patterns->as.array.items[index].as.text))
      return 1;
  }
  return 0;
}

static void mark_seed(QueryContext *context, const AbString *path) {
  size_t index = find_file(context, path);
  if (index != SIZE_MAX)
    context->files[index].seed = 1;
}

static int qualified_matches(QueryContext *context, const AbString *path,
                             const AbString *name, const AbValue *patterns) {
  AbBuffer qualified;
  AbString value;
  size_t index;
  int matched = 0;
  ab_buffer_init(&qualified, context->engine);
  if (ab_buffer_append(&qualified, path->data, path->length) != ARCHBIRD_OK ||
      ab_buffer_literal(&qualified, ":") != ARCHBIRD_OK ||
      ab_buffer_append(&qualified, name->data, name->length) != ARCHBIRD_OK) {
    ab_buffer_free(&qualified);
    return -1;
  }
  value.data = (char *)qualified.data;
  value.length = qualified.length;
  for (index = 0; index < patterns->as.array.count; index++) {
    if (match_value(&value, &patterns->as.array.items[index].as.text)) {
      matched = 1;
      break;
    }
  }
  ab_buffer_free(&qualified);
  return matched;
}

static int symbol_compare(const void *left_raw, const void *right_raw) {
  const QuerySymbol *left = (const QuerySymbol *)left_raw;
  const QuerySymbol *right = (const QuerySymbol *)right_raw;
  int compared = ab_string_compare(left->path, right->path);
  if (compared)
    return compared;
  compared = ab_string_compare(left->name, right->name);
  if (compared)
    return compared;
  compared = ab_string_compare(left->kind, right->kind);
  if (compared)
    return compared;
  compared = ab_string_compare(left->scope, right->scope);
  return compared ? compared
                  : (left->line > right->line) - (left->line < right->line);
}

static ArchbirdStatus append_symbol(QueryContext *context, size_t file_index,
                                    const AbValue *row, const AbString *name,
                                    const AbString *kind, const AbString *scope,
                                    size_t line) {
  QuerySymbol *resized;
  QuerySymbol *symbol;
  if (context->symbol_count == context->symbol_capacity) {
    size_t capacity =
        context->symbol_capacity ? context->symbol_capacity * 2 : 32;
    if (capacity < context->symbol_capacity ||
        capacity > SIZE_MAX / sizeof(*context->symbols))
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "too many matched query symbols");
    resized = (QuerySymbol *)ab_realloc(context->engine, context->symbols,
                                        capacity * sizeof(*context->symbols));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting query symbols");
    context->symbols = resized;
    context->symbol_capacity = capacity;
  }
  symbol = &context->symbols[context->symbol_count++];
  symbol->file_index = file_index;
  symbol->row = row;
  symbol->path = context->files[file_index].path;
  symbol->name = name;
  symbol->kind = kind;
  symbol->scope = scope;
  symbol->line = line;
  return ARCHBIRD_OK;
}

static ArchbirdStatus match_symbols(QueryContext *context,
                                    const AbValue *patterns) {
  size_t file_index;
  for (file_index = 0; file_index < context->file_count; file_index++) {
    const QueryFile *file = &context->files[file_index];
    size_t symbol_index;
    for (symbol_index = 0; symbol_index < file->symbols->as.array.count;
         symbol_index++) {
      const AbValue *row = &file->symbols->as.array.items[symbol_index];
      const AbString *name;
      const AbString *kind;
      const AbString *scope;
      AbString base;
      size_t base_start;
      uint64_t line;
      int qualified;
      if (row->kind != AB_VALUE_OBJECT)
        return query_error(context, "map.files[].symbols[] must be objects");
      name = required_string(context, row, "name", "map.files[].symbols[]");
      kind = required_string(context, row, "kind", "map.files[].symbols[]");
      scope = required_string(context, row, "scope", "map.files[].symbols[]");
      if (!name || !kind || !scope ||
          !ab_value_u64(ab_value_member(row, "line"), &line) || line > SIZE_MAX)
        return query_error(context,
                           "map.files[].symbols[].line must be an integer");
      base_start = name->length;
      while (base_start && name->data[base_start - 1] != '.')
        base_start--;
      base.data = name->data + base_start;
      base.length = name->length - base_start;
      qualified = qualified_matches(context, file->path, name, patterns);
      if (qualified < 0)
        return ARCHBIRD_OUT_OF_MEMORY;
      if (!array_matches(patterns, name) && !array_matches(patterns, &base) &&
          !qualified)
        continue;
      context->files[file_index].symbol_seed = 1;
      context->files[file_index].matched_symbol_seed = 1;
      if (append_symbol(context, file_index, row, name, kind, scope,
                        (size_t)line) != ARCHBIRD_OK)
        return ARCHBIRD_OUT_OF_MEMORY;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_files(QueryContext *context,
                                   const AbValue *patterns) {
  size_t index;
  for (index = 0; index < context->file_count; index++) {
    if (path_array_matches(patterns, context->files[index].path))
      context->files[index].seed = 1;
  }
  return ARCHBIRD_OK;
}

static const AbValue *member_array_checked(QueryContext *context,
                                           const AbValue *object,
                                           const char *name,
                                           const char *where) {
  const AbValue *value = ab_value_member(object, name);
  if (!value)
    return optional_array(object, name);
  if (value->kind != AB_VALUE_ARRAY) {
    archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                       ARCHBIRD_NO_OFFSET, "%s.%s must be an array", where,
                       name);
    return NULL;
  }
  return value;
}

static void seed_string_array(QueryContext *context, const AbValue *values) {
  size_t index;
  for (index = 0; index < values->as.array.count; index++) {
    if (values->as.array.items[index].kind == AB_VALUE_STRING)
      mark_seed(context, &values->as.array.items[index].as.text);
  }
}

static ArchbirdStatus select_components(QueryContext *context,
                                        const AbValue *patterns) {
  const AbValue *components =
      required_array(context, context->map, "components", "map");
  size_t index;
  if (!components)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!context->component_selected && components->as.array.count) {
    context->component_selected = (int *)ab_calloc(
        context->engine, components->as.array.count, sizeof(int));
    if (!context->component_selected)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory selecting query components");
    context->component_count = components->as.array.count;
  }
  for (index = 0; index < components->as.array.count; index++) {
    const AbValue *row = &components->as.array.items[index];
    const AbString *name;
    const AbValue *files;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.components[] must be objects");
    name = required_string(context, row, "name", "map.components[]");
    files = required_array(context, row, "files", "map.components[]");
    if (!name || !files || !string_array_valid(files))
      return ARCHBIRD_INVALID_SCHEMA;
    if (array_matches(patterns, name)) {
      context->component_selected[index] = 1;
      seed_string_array(context, files);
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_packages(QueryContext *context,
                                      const AbValue *patterns,
                                      const AbValue *export_patterns) {
  const AbValue *packages =
      required_array(context, context->map, "packages", "map");
  size_t index;
  if (!packages)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!context->package_selected && packages->as.array.count) {
    context->package_selected = (int *)ab_calloc(
        context->engine, packages->as.array.count, sizeof(int));
    if (!context->package_selected)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory selecting query packages");
    context->package_count = packages->as.array.count;
  }
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *row = &packages->as.array.items[index];
    const AbString *name;
    const AbValue *identity;
    const AbValue *aliases;
    const AbValue *entrypoints;
    const AbValue *origins;
    size_t alias_index;
    int matched = 0;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.packages[] must be objects");
    name = required_string(context, row, "name", "map.packages[]");
    identity = ab_value_member(row, "identity");
    aliases = member_array_checked(context, row, "aliases", "map.packages[]");
    entrypoints = ab_value_member(row, "entrypoints");
    origins = ab_value_member(row, "export_origins");
    if (!name || !aliases || !string_array_valid(aliases) || !identity ||
        identity->kind != AB_VALUE_STRING || !entrypoints ||
        entrypoints->kind != AB_VALUE_OBJECT || !origins ||
        origins->kind != AB_VALUE_OBJECT)
      return query_error(context, "map package shape is invalid");
    matched = array_matches(patterns, name) ||
              (identity->as.text.length &&
               array_matches(patterns, &identity->as.text));
    for (alias_index = 0; !matched && alias_index < aliases->as.array.count;
         alias_index++)
      matched = array_matches(patterns,
                              &aliases->as.array.items[alias_index].as.text);
    if (matched) {
      size_t field;
      context->package_selected[index] = 1;
      for (field = 0; field < entrypoints->as.object.count; field++) {
        const AbValue *target = &entrypoints->as.object.fields[field].value;
        if (target->kind != AB_VALUE_STRING)
          return query_error(context,
                             "map package entrypoint must be a string");
        mark_seed(context, &target->as.text);
      }
    }
    for (alias_index = 0; alias_index < origins->as.object.count;
         alias_index++) {
      const AbObjectField *origin = &origins->as.object.fields[alias_index];
      if (!array_matches(export_patterns, &origin->name))
        continue;
      if (!string_array_valid(&origin->value))
        return query_error(context,
                           "map package export origins must be string arrays");
      context->package_selected[index] = 1;
      seed_string_array(context, &origin->value);
    }
  }
  return ARCHBIRD_OK;
}

static int artifact_matches(QueryContext *context, const AbValue *row,
                            const AbValue *patterns) {
  const AbString *name =
      required_string(context, row, "name", "map.artifacts[]");
  const AbString *output =
      required_string(context, row, "output", "map.artifacts[]");
  const AbValue *loaders =
      required_array(context, row, "loaded_by", "map.artifacts[]");
  size_t index;
  if (!name || !output || !loaders)
    return -1;
  if (array_matches(patterns, name) || path_array_matches(patterns, output))
    return 1;
  for (index = 0; index < loaders->as.array.count; index++) {
    const AbValue *loader = &loaders->as.array.items[index];
    const AbString *path;
    if (loader->kind != AB_VALUE_OBJECT)
      return -1;
    path =
        required_string(context, loader, "path", "map.artifacts[].loaded_by[]");
    if (!path)
      return -1;
    if (path_array_matches(patterns, path))
      return 1;
  }
  return 0;
}

static ArchbirdStatus select_artifacts(QueryContext *context,
                                       const AbValue *patterns) {
  const AbValue *artifacts =
      required_array(context, context->map, "artifacts", "map");
  size_t index;
  if (!artifacts)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!context->artifact_selected && artifacts->as.array.count) {
    context->artifact_selected = (int *)ab_calloc(
        context->engine, artifacts->as.array.count, sizeof(int));
    if (!context->artifact_selected)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory selecting query artifacts");
    context->artifact_count = artifacts->as.array.count;
  }
  for (index = 0; index < artifacts->as.array.count; index++) {
    const AbValue *row = &artifacts->as.array.items[index];
    const AbValue *inputs;
    size_t input_index;
    int matched;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.artifacts[] must be objects");
    matched = artifact_matches(context, row, patterns);
    if (matched < 0)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!matched)
      continue;
    context->artifact_selected[index] = 1;
    inputs = required_array(context, row, "inputs", "map.artifacts[]");
    if (!inputs)
      return ARCHBIRD_INVALID_SCHEMA;
    for (input_index = 0; input_index < inputs->as.array.count; input_index++) {
      const AbValue *input = &inputs->as.array.items[input_index];
      const AbString *path;
      if (input->kind != AB_VALUE_OBJECT)
        return query_error(context, "map.artifacts[].inputs[] must be objects");
      path =
          required_string(context, input, "path", "map.artifacts[].inputs[]");
      if (!path)
        return ARCHBIRD_INVALID_SCHEMA;
      mark_seed(context, path);
    }
    {
      const AbValue *loaders = ab_value_member(row, "loaded_by");
      for (input_index = 0; input_index < loaders->as.array.count;
           input_index++) {
        const AbString *path =
            required_string(context, &loaders->as.array.items[input_index],
                            "path", "map.artifacts[].loaded_by[]");
        if (!path)
          return ARCHBIRD_INVALID_SCHEMA;
        mark_seed(context, path);
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_initial(QueryContext *context) {
  size_t index;
  ArchbirdStatus status;
  status = select_files(context, context->request.focus);
  if (status == ARCHBIRD_OK)
    status = select_files(context, context->request.paths);
  if (status == ARCHBIRD_OK)
    status = match_symbols(context, context->request.focus);
  if (status == ARCHBIRD_OK)
    status = match_symbols(context, context->request.symbols);
  if (status == ARCHBIRD_OK)
    status = select_components(context, context->request.focus);
  if (status == ARCHBIRD_OK)
    status = select_components(context, context->request.components);
  if (status == ARCHBIRD_OK)
    status = select_packages(context, context->request.focus,
                             context->request.focus);
  if (status == ARCHBIRD_OK)
    status = select_packages(context, context->request.packages,
                             context->request.symbols);
  if (status == ARCHBIRD_OK)
    status = select_artifacts(context, context->request.focus);
  if (status == ARCHBIRD_OK)
    status = select_artifacts(context, context->request.artifacts);
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; index < context->file_count; index++)
    context->files[index].requested_seed = context->files[index].seed;
  if (context->symbol_count) {
    context->exact_symbol_scope = 1;
    for (index = 0; index < context->file_count; index++)
      if (context->files[index].requested_seed) {
        context->exact_symbol_scope = 0;
        break;
      }
  }
  if (context->symbol_count > 1) {
    size_t read;
    size_t write = 0;
    qsort(context->symbols, context->symbol_count, sizeof(*context->symbols),
          symbol_compare);
    for (read = 0; read < context->symbol_count; read++) {
      if (write && symbol_compare(&context->symbols[write - 1],
                                  &context->symbols[read]) == 0)
        continue;
      if (write != read)
        context->symbols[write] = context->symbols[read];
      write++;
    }
    context->symbol_count = write;
  }
  return ARCHBIRD_OK;
}

static int container_symbol_kind(const AbString *kind) {
  return string_literal(kind, "class") || string_literal(kind, "enum") ||
         string_literal(kind, "interface") ||
         string_literal(kind, "namespace") || string_literal(kind, "struct") ||
         string_literal(kind, "union");
}

static int qualified_member_of(const AbString *candidate,
                               const AbString *container) {
  return candidate->length > container->length &&
         candidate->data[container->length] == '.' &&
         !memcmp(candidate->data, container->data, container->length);
}

static ArchbirdStatus select_contained_symbols(QueryContext *context) {
  size_t selected_index;
  for (selected_index = 0; selected_index < context->symbol_count;
       selected_index++) {
    const QuerySymbol *selected = &context->symbols[selected_index];
    const AbValue *symbols;
    size_t row_index;
    if (!container_symbol_kind(selected->kind))
      continue;
    symbols = context->files[selected->file_index].symbols;
    for (row_index = 0; row_index < symbols->as.array.count; row_index++) {
      const AbValue *row = &symbols->as.array.items[row_index];
      const AbValue *name =
          row->kind == AB_VALUE_OBJECT ? ab_value_member(row, "name") : NULL;
      if (!name || name->kind != AB_VALUE_STRING)
        return query_error(context,
                           "map.files[].symbols[].name must be a string");
      if (qualified_member_of(&name->as.text, selected->name)) {
        ArchbirdStatus status = append_related_symbol_row(
            context, selected->file_index, row, &name->as.text);
        if (status != ARCHBIRD_OK)
          return status;
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus build_test_rows(QueryContext *context) {
  const AbValue *tests = required_array(context, context->map, "tests", "map");
  size_t total = 0;
  size_t test_index;
  size_t write = 0;
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  if (!tests)
    return ARCHBIRD_INVALID_SCHEMA;
  context->test_count = tests->as.array.count;
  if (context->test_count) {
    context->test_selected = (int *)ab_calloc(
        context->engine, context->test_count, sizeof(*context->test_selected));
    if (!context->test_selected)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory selecting query tests");
  }
  for (test_index = 0; test_index < tests->as.array.count; test_index++) {
    const AbValue *test = &tests->as.array.items[test_index];
    const AbValue *cases;
    if (test->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.tests[] must be objects");
    cases = required_array(context, test, "cases", "map.tests[]");
    if (!cases)
      return ARCHBIRD_INVALID_SCHEMA;
    total += cases->as.array.count ? cases->as.array.count : 1;
  }
  if (total) {
    context->test_rows = (QueryTestRow *)ab_calloc(context->engine, total,
                                                   sizeof(*context->test_rows));
    if (!context->test_rows)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing query test rows");
  }
  for (test_index = 0; test_index < tests->as.array.count; test_index++) {
    const AbValue *test = &tests->as.array.items[test_index];
    const AbString *path =
        required_string(context, test, "path", "map.tests[]");
    const AbString *group =
        required_string(context, test, "group", "map.tests[]");
    const AbValue *inventory_state = ab_value_member(test, "inventory_state");
    const AbValue *cases = ab_value_member(test, "cases");
    size_t case_count = cases->as.array.count ? cases->as.array.count : 1;
    size_t case_index;
    if (!path || !group)
      return ARCHBIRD_INVALID_SCHEMA;
    if (inventory_state && inventory_state->kind != AB_VALUE_STRING)
      return query_error(context, "map test inventory state must be a string");
    for (case_index = 0; case_index < case_count; case_index++) {
      QueryTestRow *row = &context->test_rows[write++];
      const AbValue *case_row =
          cases->as.array.count ? &cases->as.array.items[case_index] : NULL;
      const AbValue *line = case_row ? ab_value_member(case_row, "line") : NULL;
      const AbValue *selector =
          case_row ? ab_value_member(case_row, "selector") : NULL;
      uint64_t line_number = 0;
      int qualified;
      row->test_index = test_index;
      row->test = test;
      row->case_row = case_row;
      row->path = path;
      row->group = group;
      row->inventory_state = inventory_state ? &inventory_state->as.text : NULL;
      row->selector = case_row && selector && selector->kind == AB_VALUE_STRING
                          ? &selector->as.text
                          : path;
      if (case_row && (!line || !ab_value_u64(line, &line_number) ||
                       line_number > SIZE_MAX))
        return query_error(context, "map test case line must be an integer");
      row->line = (size_t)line_number;
      row->routes = case_row ? ab_value_member(case_row, "routes")
                             : ab_value_member(test, "routes");
      row->route_evidence = case_row
                                ? ab_value_member(case_row, "route_evidence")
                                : ab_value_member(test, "route_evidence");
      row->evidence_v2 = row->route_evidence != NULL;
      if (!row->route_evidence)
        row->route_evidence = &empty;
      row->configured =
          case_row ? ab_value_member(case_row, "configured_routes") : &empty;
      if (!row->routes || row->routes->kind != AB_VALUE_OBJECT ||
          row->route_evidence->kind != AB_VALUE_ARRAY ||
          !string_array_valid(row->configured))
        return query_error(context, "map test route shape is invalid");
      qualified = qualified_matches(context, path, row->selector,
                                    context->request.focus);
      if (qualified < 0)
        return ARCHBIRD_OUT_OF_MEMORY;
      row->focused = path_array_matches(context->request.focus, path) ||
                     path_array_matches(context->request.paths, path) ||
                     array_matches(context->request.focus, row->selector) ||
                     qualified;
      if (row->focused) {
        size_t route;
        size_t evidence_index;
        context->has_focused_tests = 1;
        context->test_selected[test_index] = 1;
        for (route = 0; route < row->routes->as.object.count; route++)
          mark_seed(context, &row->routes->as.object.fields[route].name);
        for (evidence_index = 0;
             evidence_index < row->route_evidence->as.array.count;
             evidence_index++) {
          const AbValue *evidence =
              &row->route_evidence->as.array.items[evidence_index];
          const AbValue *target = evidence->kind == AB_VALUE_OBJECT
                                      ? ab_value_member(evidence, "target")
                                      : NULL;
          if (!target || target->kind != AB_VALUE_STRING)
            return query_error(context,
                               "map test route evidence shape is invalid");
          mark_seed(context, &target->as.text);
        }
      }
    }
  }
  context->test_row_count = write;
  return ARCHBIRD_OK;
}

static int configured_contains(const QueryTestRow *row,
                               const AbString *target) {
  size_t index;
  for (index = 0; index < row->configured->as.array.count; index++) {
    if (ab_string_equal(&row->configured->as.array.items[index].as.text,
                        target))
      return 1;
  }
  return 0;
}

static int qualified_name_extends(const AbString *longer,
                                  const AbString *shorter) {
  size_t offset;
  if (longer->length <= shorter->length)
    return 0;
  offset = longer->length - shorter->length;
  return offset && longer->data[offset - 1] == '.' &&
         memcmp(longer->data + offset, shorter->data, shorter->length) == 0;
}

static int target_symbol_is_selected(const QueryContext *context,
                                     size_t target_index,
                                     const AbString *target_symbol,
                                     int *requires_symbol) {
  const AbString *unique_leaf_name = NULL;
  const AbValue *file_symbols = context->files[target_index].symbols;
  size_t index;
  int leaf_selected = 0;
  int leaf_ambiguous = 0;
  *requires_symbol = 0;
  for (index = 0; index < context->symbol_count; index++) {
    const QuerySymbol *symbol = &context->symbols[index];
    if (symbol->file_index != target_index)
      continue;
    *requires_symbol = 1;
    if (target_symbol && target_symbol->length) {
      size_t split = symbol->name->length;
      while (split && symbol->name->data[split - 1] != '.')
        split--;
      if (ab_string_equal(symbol->name, target_symbol))
        return 1;
      if (symbol->name->length - split == target_symbol->length &&
          memcmp(symbol->name->data + split, target_symbol->data,
                 target_symbol->length) == 0)
        leaf_selected = 1;
    }
  }
  if (!leaf_selected || !file_symbols || file_symbols->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < file_symbols->as.array.count; index++) {
    const AbValue *row = &file_symbols->as.array.items[index];
    const AbValue *name =
        row->kind == AB_VALUE_OBJECT ? ab_value_member(row, "name") : NULL;
    size_t split;
    if (!name || name->kind != AB_VALUE_STRING)
      continue;
    split = name->as.text.length;
    while (split && name->as.text.data[split - 1] != '.')
      split--;
    if (name->as.text.length - split != target_symbol->length ||
        memcmp(name->as.text.data + split, target_symbol->data,
               target_symbol->length) != 0)
      continue;
    if (!unique_leaf_name) {
      unique_leaf_name = &name->as.text;
    } else if (!ab_string_equal(unique_leaf_name, &name->as.text)) {
      if (qualified_name_extends(&name->as.text, unique_leaf_name))
        unique_leaf_name = &name->as.text;
      else if (!qualified_name_extends(unique_leaf_name, &name->as.text))
        leaf_ambiguous = 1;
    }
  }
  return unique_leaf_name && !leaf_ambiguous;
}

static int candidate_relation(const AbString *relation) {
  static const char suffix[] = "-candidate";
  return relation->length >= sizeof(suffix) - 1 &&
         !memcmp(relation->data + relation->length - (sizeof(suffix) - 1),
                 suffix, sizeof(suffix) - 1);
}

static int observed_route_evidence(const QueryContext *context,
                                   const QueryTestRow *row,
                                   size_t target_index) {
  size_t index;
  for (index = 0; index < row->route_evidence->as.array.count; index++) {
    const AbValue *evidence = &row->route_evidence->as.array.items[index];
    const AbValue *candidate;
    const AbValue *target_symbol;
    const AbValue *provenance;
    const AbValue *relation;
    const AbValue *scope;
    int requires_symbol;
    int selected_symbol;
    if (evidence->kind != AB_VALUE_OBJECT)
      return -1;
    candidate = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    provenance = ab_value_member(evidence, "provenance");
    relation = ab_value_member(evidence, "relation");
    scope = ab_value_member(evidence, "scope");
    if (!candidate || candidate->kind != AB_VALUE_STRING || !target_symbol ||
        target_symbol->kind != AB_VALUE_STRING || !provenance ||
        provenance->kind != AB_VALUE_STRING || !relation ||
        relation->kind != AB_VALUE_STRING || !scope ||
        scope->kind != AB_VALUE_STRING)
      return -1;
    if (!ab_string_equal(&candidate->as.text,
                         context->files[target_index].path) ||
        !string_literal(&provenance->as.text, "observed") ||
        !string_literal(&relation->as.text, "observed-symbol-hit") ||
        !string_literal(&scope->as.text, "case"))
      continue;
    selected_symbol = target_symbol_is_selected(
        context, target_index, &target_symbol->as.text, &requires_symbol);
    if (!requires_symbol || selected_symbol)
      return 1;
  }
  return 0;
}

static int direct_route_evidence(const QueryContext *context,
                                 const QueryTestRow *row, size_t target_index) {
  size_t index;
  for (index = 0; index < row->route_evidence->as.array.count; index++) {
    const AbValue *evidence = &row->route_evidence->as.array.items[index];
    const AbValue *candidate;
    const AbValue *target_symbol;
    const AbValue *provenance;
    const AbValue *relation;
    const AbValue *scope;
    int requires_symbol;
    int selected_symbol;
    if (evidence->kind != AB_VALUE_OBJECT)
      return -1;
    candidate = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    provenance = ab_value_member(evidence, "provenance");
    relation = ab_value_member(evidence, "relation");
    scope = ab_value_member(evidence, "scope");
    if (!candidate || candidate->kind != AB_VALUE_STRING || !provenance ||
        provenance->kind != AB_VALUE_STRING || !relation ||
        relation->kind != AB_VALUE_STRING || !scope ||
        scope->kind != AB_VALUE_STRING ||
        (target_symbol && target_symbol->kind != AB_VALUE_STRING))
      return -1;
    if (!ab_string_equal(&candidate->as.text,
                         context->files[target_index].path) ||
        !string_literal(&provenance->as.text, "derived") ||
        !string_literal(&scope->as.text, "case"))
      continue;
    if (candidate_relation(&relation->as.text))
      continue;
    selected_symbol = target_symbol_is_selected(
        context, target_index, target_symbol ? &target_symbol->as.text : NULL,
        &requires_symbol);
    if (!requires_symbol || selected_symbol)
      return 1;
  }
  return 0;
}

static int candidate_route_evidence(const QueryContext *context,
                                    const QueryTestRow *row,
                                    size_t target_index) {
  size_t index;
  if (!context->files[target_index].requested_seed &&
      !(context->exact_symbol_scope &&
        context->files[target_index].symbol_seed))
    return 0;
  for (index = 0; index < row->route_evidence->as.array.count; index++) {
    const AbValue *evidence = &row->route_evidence->as.array.items[index];
    const AbValue *candidate;
    const AbValue *target_symbol;
    const AbValue *provenance;
    const AbValue *relation;
    const AbValue *scope;
    int requires_symbol;
    int selected_symbol;
    if (evidence->kind != AB_VALUE_OBJECT)
      return -1;
    candidate = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    provenance = ab_value_member(evidence, "provenance");
    relation = ab_value_member(evidence, "relation");
    scope = ab_value_member(evidence, "scope");
    if (!candidate || candidate->kind != AB_VALUE_STRING || !provenance ||
        provenance->kind != AB_VALUE_STRING || !relation ||
        relation->kind != AB_VALUE_STRING || !scope ||
        scope->kind != AB_VALUE_STRING ||
        (target_symbol && target_symbol->kind != AB_VALUE_STRING))
      return -1;
    if (!ab_string_equal(&candidate->as.text,
                         context->files[target_index].path) ||
        !string_literal(&provenance->as.text, "derived") ||
        !string_literal(&scope->as.text, "case") ||
        !candidate_relation(&relation->as.text))
      continue;
    selected_symbol = target_symbol_is_selected(
        context, target_index, target_symbol ? &target_symbol->as.text : NULL,
        &requires_symbol);
    if (requires_symbol && selected_symbol)
      return 1;
  }
  return 0;
}

static int asserted_route_evidence(const QueryContext *context,
                                   const QueryTestRow *row,
                                   size_t target_index) {
  size_t index;
  if (!context->files[target_index].requested_seed &&
      !(context->exact_symbol_scope &&
        context->files[target_index].symbol_seed))
    return 0;
  for (index = 0; index < row->route_evidence->as.array.count; index++) {
    const AbValue *evidence = &row->route_evidence->as.array.items[index];
    const AbValue *candidate;
    const AbValue *target_symbol;
    const AbValue *provenance;
    const AbValue *scope;
    int requires_symbol;
    int selected_symbol;
    if (evidence->kind != AB_VALUE_OBJECT)
      return -1;
    candidate = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    provenance = ab_value_member(evidence, "provenance");
    scope = ab_value_member(evidence, "scope");
    if (!candidate || candidate->kind != AB_VALUE_STRING || !provenance ||
        provenance->kind != AB_VALUE_STRING || !scope ||
        scope->kind != AB_VALUE_STRING ||
        (target_symbol && target_symbol->kind != AB_VALUE_STRING))
      return -1;
    if (!ab_string_equal(&candidate->as.text,
                         context->files[target_index].path) ||
        !string_literal(&provenance->as.text, "asserted") ||
        !string_literal(&scope->as.text, "case"))
      continue;
    selected_symbol = target_symbol_is_selected(
        context, target_index, target_symbol ? &target_symbol->as.text : NULL,
        &requires_symbol);
    if (!requires_symbol || selected_symbol)
      return 1;
  }
  return 0;
}

static ArchbirdStatus
append_test_match(QueryContext *context, const QueryTestRow *row, size_t target,
                  const size_t *next_hop, size_t seed_distance,
                  const char *evidence, const char *classification) {
  QueryTestMatch *resized;
  QueryTestMatch *match;
  size_t count = 0;
  size_t current = target;
  if (context->test_match_count == context->test_match_capacity) {
    size_t capacity =
        context->test_match_capacity ? context->test_match_capacity * 2 : 32;
    resized =
        (QueryTestMatch *)ab_realloc(context->engine, context->test_matches,
                                     capacity * sizeof(*context->test_matches));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting query test matches");
    context->test_matches = resized;
    context->test_match_capacity = capacity;
  }
  match = &context->test_matches[context->test_match_count++];
  memset(match, 0, sizeof(*match));
  match->row = row;
  match->evidence = evidence;
  match->classification = classification;
  if (classification && !strcmp(classification, "observed")) {
    match->provenance = "observed";
    match->confidence = "exact";
  } else if (classification && !strcmp(classification, "asserted")) {
    match->provenance = "asserted";
    match->confidence = "exact";
  } else if (classification && !strcmp(classification, "direct")) {
    match->provenance = "derived";
    match->confidence = "exact";
  } else if (classification && !strcmp(classification, "candidate")) {
    match->provenance = "derived";
    match->confidence = "candidate";
  } else if (classification && !strcmp(classification, "conservative")) {
    match->provenance = "derived";
    match->confidence = "conservative";
  } else if (classification && !strcmp(classification, "unresolved")) {
    match->provenance = "derived";
    match->confidence = "unresolved";
  } else if (!strcmp(evidence, "configured")) {
    match->provenance = "asserted";
    match->confidence = context->exact_symbol_scope ? "conservative" : "exact";
  } else if (!strcmp(evidence, "unresolved")) {
    match->provenance = "derived";
    match->confidence = "unresolved";
  } else {
    match->provenance = "derived";
    match->confidence = "conservative";
  }
  if (target == SIZE_MAX) {
    match->target_role = "unresolved";
  } else if (context->has_focused_tests && context->files[target].seed &&
             !context->files[target].requested_seed) {
    match->target_role = "focused-test-route";
  } else if (context->files[target].requested_seed) {
    match->target_role = "requested-seed";
  } else if (context->files[target].matched_symbol_seed) {
    match->target_role = "requested-symbol";
  } else if (context->files[target].related_symbol) {
    match->target_role = "symbol-neighborhood";
  } else if (context->files[target].selected) {
    match->target_role = "file-neighborhood";
  } else {
    match->target_role = "transitive-context";
  }
  match->seed_distance = seed_distance;
  match->evidence_target = target;
  if (target != SIZE_MAX) {
    while (current != SIZE_MAX) {
      count++;
      current = next_hop[current];
    }
    match->route =
        (size_t *)ab_malloc(context->engine, count * sizeof(*match->route));
    if (!match->route)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing query test route");
    current = target;
    while (current != SIZE_MAX) {
      match->route[match->route_count++] = current;
      current = next_hop[current];
    }
  }
  context->test_selected[row->test_index] = 1;
  return ARCHBIRD_OK;
}

static unsigned char ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

static int bytes_equal_fold(const char *left, size_t left_length,
                            const char *right, size_t right_length) {
  size_t index;
  if (left_length != right_length)
    return 0;
  for (index = 0; index < left_length; index++)
    if (ascii_lower((unsigned char)left[index]) !=
        ascii_lower((unsigned char)right[index]))
      return 0;
  return 1;
}

static void path_leaf_stem(const AbString *path, const char **out,
                           size_t *out_length) {
  size_t start = path->length;
  size_t end;
  while (start && path->data[start - 1] != '/')
    start--;
  end = start;
  while (end < path->length && path->data[end] != '.')
    end++;
  *out = path->data + start;
  *out_length = end - start;
}

static void symbol_leaf(const AbString *symbol, const char **out,
                        size_t *out_length) {
  size_t start = symbol->length;
  while (start && symbol->data[start - 1] != '.')
    start--;
  *out = symbol->data + start;
  *out_length = symbol->length - start;
}

static int path_has_segment_fold(const AbString *path, const char *value,
                                 size_t value_length) {
  size_t start = 0;
  while (start < path->length) {
    size_t end = start;
    size_t stem_end;
    while (end < path->length && path->data[end] != '/')
      end++;
    stem_end = start;
    while (stem_end < end && path->data[stem_end] != '.')
      stem_end++;
    if (bytes_equal_fold(path->data + start, stem_end - start, value,
                         value_length))
      return 1;
    start = end + (end < path->length);
  }
  return 0;
}

static size_t normalized_alnum(const char *data, size_t length, char *out,
                               size_t capacity) {
  size_t read;
  size_t write = 0;
  for (read = 0; read < length && write < capacity; read++) {
    unsigned char value = (unsigned char)data[read];
    if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9'))
      out[write++] = (char)ascii_lower(value);
  }
  return write;
}

static int byte_sequence(const char *shorter, size_t shorter_length,
                         const char *longer, size_t longer_length) {
  size_t short_index = 0;
  size_t long_index;
  if (!shorter_length || shorter_length > longer_length)
    return 0;
  for (long_index = 0;
       long_index < longer_length && short_index < shorter_length; long_index++)
    if (shorter[short_index] == longer[long_index])
      short_index++;
  return short_index == shorter_length;
}

static size_t token_symbol_affinity(const char *token, size_t token_length,
                                    const char *symbol, size_t symbol_length) {
  size_t index;
  if (token_length < 3 || !symbol_length)
    return 0;
  if (token_length == symbol_length && !memcmp(token, symbol, token_length))
    return 8;
  for (index = 0; index + token_length <= symbol_length; index++)
    if (!memcmp(symbol + index, token, token_length))
      return token_length * 2 >= symbol_length ? 4 : 2;
  if (token_length > 3 && token[token_length - 1] == 's')
    for (index = 0; index + token_length - 1 <= symbol_length; index++)
      if (!memcmp(symbol + index, token, token_length - 1))
        return (token_length - 1) * 2 >= symbol_length ? 4 : 2;
  return token_length > 3 && token[0] == symbol[0] &&
                 token_length * 2 <= symbol_length &&
                 byte_sequence(token, token_length, symbol, symbol_length)
             ? 6
             : 0;
}

static size_t text_symbol_affinity(const AbString *text, const char *symbol,
                                   size_t symbol_length) {
  size_t start = 0;
  size_t best = 0;
  while (start < text->length) {
    char token[128];
    size_t end = start;
    size_t token_length;
    size_t affinity;
    while (end < text->length) {
      unsigned char value = (unsigned char)text->data[end];
      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9')))
        break;
      end++;
    }
    token_length =
        normalized_alnum(text->data + start, end - start, token, sizeof(token));
    affinity =
        token_symbol_affinity(token, token_length, symbol, symbol_length);
    if (affinity > best)
      best = affinity;
    start = end + (end < text->length);
  }
  return best;
}

static size_t requested_symbol_affinity(const QueryContext *context,
                                        const QueryTestRow *row) {
  size_t index;
  size_t score = 0;
  for (index = 0; index < context->symbol_count; index++) {
    const QuerySymbol *selected = &context->symbols[index];
    const char *leaf;
    size_t leaf_length;
    char normalized[256];
    size_t normalized_length;
    symbol_leaf(selected->name, &leaf, &leaf_length);
    normalized_length =
        normalized_alnum(leaf, leaf_length, normalized, sizeof(normalized));
    if (!normalized_length)
      continue;
    score += text_symbol_affinity(row->selector, normalized, normalized_length);
    score += text_symbol_affinity(row->path, normalized, normalized_length) / 2;
  }
  return score;
}

static size_t test_match_affinity(const QueryContext *context,
                                  const QueryTestMatch *match) {
  const AbString *target_symbol = match_target_symbol(context, match);
  const char *test_stem;
  const char *target_stem;
  const char *symbol_name;
  size_t test_stem_length;
  size_t target_stem_length;
  size_t symbol_length;
  size_t score = requested_symbol_affinity(context, match->row);
  if (match->evidence_target == SIZE_MAX)
    return 0;
  path_leaf_stem(match->row->path, &test_stem, &test_stem_length);
  path_leaf_stem(context->files[match->evidence_target].path, &target_stem,
                 &target_stem_length);
  if (target_symbol) {
    symbol_leaf(target_symbol, &symbol_name, &symbol_length);
    if (bytes_equal_fold(test_stem, test_stem_length, symbol_name,
                         symbol_length))
      score += 4;
  }
  if (target_stem_length &&
      path_has_segment_fold(match->row->path, target_stem, target_stem_length))
    score += 2;
  return score;
}

static const char *test_match_evidence_scope(const QueryContext *context,
                                             const QueryTestMatch *match) {
  size_t index;
  if (match->evidence_target == SIZE_MAX)
    return "unresolved";
  for (index = 0; index < match->row->route_evidence->as.array.count; index++) {
    const AbValue *evidence =
        &match->row->route_evidence->as.array.items[index];
    const AbValue *target;
    const AbValue *target_symbol;
    const AbValue *scope;
    int requires_symbol;
    int selected_symbol;
    if (evidence->kind != AB_VALUE_OBJECT)
      continue;
    target = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    scope = ab_value_member(evidence, "scope");
    if (!target || target->kind != AB_VALUE_STRING || !scope ||
        scope->kind != AB_VALUE_STRING ||
        (target_symbol && target_symbol->kind != AB_VALUE_STRING))
      continue;
    if (!ab_string_equal(&target->as.text,
                         context->files[match->evidence_target].path))
      continue;
    selected_symbol = target_symbol_is_selected(
        context, match->evidence_target,
        target_symbol ? &target_symbol->as.text : NULL, &requires_symbol);
    if (requires_symbol && !selected_symbol)
      continue;
    if (string_literal(&scope->as.text, "case"))
      return "case";
  }
  return "file";
}

static int test_match_priority(const QueryTestMatch *match) {
  if (!strcmp(match->provenance, "observed"))
    return 0;
  if (!strcmp(match->provenance, "asserted"))
    return 1;
  if (!strcmp(match->confidence, "exact"))
    return 2;
  if (!strcmp(match->confidence, "candidate"))
    return 3;
  if (!strcmp(match->confidence, "conservative"))
    return 4;
  return 5;
}

static int test_scope_priority(const char *scope) {
  if (!strcmp(scope, "case"))
    return 0;
  if (!strcmp(scope, "file"))
    return 1;
  return 2;
}

static int test_target_role_priority(const char *role) {
  if (!strcmp(role, "requested-symbol"))
    return 0;
  if (!strcmp(role, "requested-seed"))
    return 1;
  if (!strcmp(role, "focused-test-route"))
    return 2;
  if (!strcmp(role, "symbol-neighborhood"))
    return 3;
  if (!strcmp(role, "file-neighborhood"))
    return 4;
  if (!strcmp(role, "transitive-context"))
    return 5;
  return 6;
}

static int test_match_compare(const void *left_raw, const void *right_raw) {
  const QueryTestMatch *left = (const QueryTestMatch *)left_raw;
  const QueryTestMatch *right = (const QueryTestMatch *)right_raw;
  int compared = test_match_priority(left) - test_match_priority(right);
  if (compared)
    return compared;
  compared = test_scope_priority(left->evidence_scope) -
             test_scope_priority(right->evidence_scope);
  if (compared)
    return compared;
  compared = test_target_role_priority(left->target_role) -
             test_target_role_priority(right->target_role);
  if (compared)
    return compared;
  if (left->seed_distance != right->seed_distance)
    return left->seed_distance > right->seed_distance ? 1 : -1;
  if (left->ranking_affinity != right->ranking_affinity)
    return left->ranking_affinity < right->ranking_affinity ? 1 : -1;
  if (left->route_count != right->route_count)
    return left->route_count > right->route_count ? 1 : -1;
  compared = ab_string_compare(left->row->path, right->row->path);
  if (compared)
    return compared;
  compared = ab_string_compare(left->row->group, right->row->group);
  if (compared)
    return compared;
  compared = ab_string_compare(left->row->selector, right->row->selector);
  if (compared)
    return compared;
  return (left->row->line > right->row->line) -
         (left->row->line < right->row->line);
}

static ArchbirdStatus find_test_matches(QueryContext *context) {
  size_t *distance = NULL;
  size_t *next_hop = NULL;
  size_t *queue = NULL;
  int *in_queue = NULL;
  size_t head = 0;
  size_t tail = 0;
  size_t queued = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (context->file_count) {
    distance = (size_t *)ab_malloc(context->engine,
                                   context->file_count * sizeof(*distance));
    next_hop = (size_t *)ab_malloc(context->engine,
                                   context->file_count * sizeof(*next_hop));
    queue = (size_t *)ab_malloc(context->engine,
                                context->file_count * sizeof(*queue));
    in_queue = (int *)ab_calloc(context->engine, context->file_count,
                                sizeof(*in_queue));
    if (!distance || !next_hop || !queue || !in_queue) {
      status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory routing query tests");
      goto done;
    }
  }
  for (index = 0; index < context->file_count; index++) {
    size_t initial_distance = SIZE_MAX;
    distance[index] = SIZE_MAX;
    next_hop[index] = SIZE_MAX;
    if (context->has_focused_tests && context->files[index].seed)
      initial_distance = 0;
    else if (!context->has_focused_tests &&
             (context->files[index].requested_seed ||
              (context->exact_symbol_scope &&
               context->files[index].matched_symbol_seed)))
      initial_distance = 0;
    else if (!context->has_focused_tests && context->exact_symbol_scope &&
             context->files[index].related_symbol)
      initial_distance = context->files[index].distance;
    if (initial_distance <= context->request.test_depth) {
      distance[index] = initial_distance;
      queue[tail] = index;
      tail = (tail + 1) % context->file_count;
      queued++;
      in_queue[index] = 1;
    }
  }
  while (queued) {
    size_t current = queue[head];
    size_t source;
    head = (head + 1) % context->file_count;
    queued--;
    in_queue[current] = 0;
    if (distance[current] >= context->request.test_depth)
      continue;
    for (source = 0; source < context->file_count; source++) {
      size_t edge_index;
      size_t candidate_distance = distance[current] + 1;
      int connected = 0;
      for (edge_index = 0; edge_index < context->edge_count; edge_index++) {
        const QueryEdge *edge = &context->edges[edge_index];
        if (edge->target_mapped && edge->source_index == source &&
            edge->target_index == current) {
          connected = 1;
          break;
        }
      }
      if (!connected || candidate_distance >= distance[source])
        continue;
      distance[source] = candidate_distance;
      next_hop[source] = current;
      if (!in_queue[source]) {
        queue[tail] = source;
        tail = (tail + 1) % context->file_count;
        queued++;
        in_queue[source] = 1;
      }
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < context->test_row_count;
       index++) {
    const QueryTestRow *row = &context->test_rows[index];
    size_t route_index;
    size_t best = SIZE_MAX;
    int best_priority = 3;
    size_t best_distance = SIZE_MAX;
    if (context->has_focused_tests && !row->focused)
      continue;
    for (route_index = 0; route_index < row->routes->as.object.count;
         route_index++) {
      const AbString *target = &row->routes->as.object.fields[route_index].name;
      size_t file = find_file(context, target);
      int priority;
      if (file == SIZE_MAX || distance[file] == SIZE_MAX)
        continue;
      priority = configured_contains(row, target) ? 1 : 2;
      if (priority < best_priority ||
          (priority == best_priority && distance[file] < best_distance) ||
          (priority == best_priority && distance[file] == best_distance &&
           (best == SIZE_MAX ||
            ab_string_compare(context->files[file].path,
                              context->files[best].path) < 0))) {
        best = file;
        best_priority = priority;
        best_distance = distance[file];
      }
    }
    for (route_index = 0; status == ARCHBIRD_OK &&
                          route_index < row->route_evidence->as.array.count;
         route_index++) {
      const AbValue *evidence =
          &row->route_evidence->as.array.items[route_index];
      const AbValue *provenance = evidence->kind == AB_VALUE_OBJECT
                                      ? ab_value_member(evidence, "provenance")
                                      : NULL;
      const AbValue *target = evidence->kind == AB_VALUE_OBJECT
                                  ? ab_value_member(evidence, "target")
                                  : NULL;
      size_t file;
      int observed;
      if (!provenance || provenance->kind != AB_VALUE_STRING || !target ||
          target->kind != AB_VALUE_STRING) {
        status =
            query_error(context, "map test route evidence shape is invalid");
        break;
      }
      if (!string_literal(&provenance->as.text, "observed"))
        continue;
      file = find_file(context, &target->as.text);
      if (file == SIZE_MAX || distance[file] == SIZE_MAX)
        continue;
      observed = observed_route_evidence(context, row, file);
      if (observed < 0) {
        status =
            query_error(context, "map test route evidence shape is invalid");
        break;
      }
      if (!observed)
        continue;
      if (best_priority > 0 || distance[file] < best_distance ||
          (distance[file] == best_distance &&
           (best == SIZE_MAX ||
            ab_string_compare(context->files[file].path,
                              context->files[best].path) < 0))) {
        best = file;
        best_priority = 0;
        best_distance = distance[file];
      }
    }
    if (best != SIZE_MAX) {
      if (row->evidence_v2) {
        int observed = observed_route_evidence(context, row, best);
        int direct = direct_route_evidence(context, row, best);
        int candidate = candidate_route_evidence(context, row, best);
        int asserted = asserted_route_evidence(context, row, best);
        if (observed < 0 || direct < 0 || candidate < 0 || asserted < 0)
          status =
              query_error(context, "map test route evidence shape is invalid");
        else
          status = append_test_match(
              context, row, best, next_hop, best_distance,
              observed ? "observed"
                       : (best_priority == 1 ? "configured" : "static"),
              observed ? "observed"
                       : (best_priority == 1 && asserted
                              ? "asserted"
                              : ((best_distance == 0 ||
                                  context->files[best].related_symbol) &&
                                         direct
                                     ? "direct"
                                     : ((best_distance == 0 ||
                                         context->files[best].related_symbol) &&
                                                candidate
                                            ? "candidate"
                                            : "conservative"))));
      } else {
        status = append_test_match(context, row, best, next_hop, best_distance,
                                   best_priority == 1 ? "configured" : "static",
                                   context->exact_symbol_scope ? "conservative"
                                                               : NULL);
      }
    } else if (row->focused) {
      status = append_test_match(context, row, SIZE_MAX, next_hop, SIZE_MAX,
                                 "unresolved",
                                 row->evidence_v2 ? "unresolved" : NULL);
    }
  }
  if (status == ARCHBIRD_OK)
    for (index = 0; index < context->test_match_count; index++) {
      QueryTestMatch *match = &context->test_matches[index];
      match->evidence_scope = test_match_evidence_scope(context, match);
      match->ranking_affinity = test_match_affinity(context, match);
    }
  if (status == ARCHBIRD_OK && context->test_match_count > 1)
    qsort(context->test_matches, context->test_match_count,
          sizeof(*context->test_matches), test_match_compare);
done:
  ab_free(context->engine, queue);
  ab_free(context->engine, in_queue);
  ab_free(context->engine, next_hop);
  ab_free(context->engine, distance);
  return status;
}

static int related_symbol_row_exists(const QueryContext *context,
                                     size_t file_index, const AbValue *row) {
  size_t index;
  for (index = 0; index < context->symbol_count; index++)
    if (context->symbols[index].file_index == file_index &&
        context->symbols[index].row == row)
      return 1;
  for (index = 0; index < context->related_symbol_count; index++)
    if (context->related_symbols[index].file_index == file_index &&
        context->related_symbols[index].row == row)
      return 1;
  return 0;
}

static ArchbirdStatus append_related_symbol_row(QueryContext *context,
                                                size_t file_index,
                                                const AbValue *row,
                                                const AbString *name) {
  QueryRelatedSymbol *resized;
  QueryRelatedSymbol *symbol;
  if (related_symbol_row_exists(context, file_index, row))
    return ARCHBIRD_OK;
  if (context->related_symbol_count == context->related_symbol_capacity) {
    size_t capacity = context->related_symbol_capacity
                          ? context->related_symbol_capacity * 2
                          : 16;
    if (capacity < context->related_symbol_capacity ||
        capacity > SIZE_MAX / sizeof(*context->related_symbols))
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "too many related query symbols");
    resized = (QueryRelatedSymbol *)ab_realloc(
        context->engine, context->related_symbols,
        capacity * sizeof(*context->related_symbols));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting related symbols");
    context->related_symbols = resized;
    context->related_symbol_capacity = capacity;
  }
  symbol = &context->related_symbols[context->related_symbol_count++];
  symbol->file_index = file_index;
  symbol->row = row;
  symbol->name = name;
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_related_symbol(QueryContext *context,
                                            const AbString *path,
                                            const AbString *name, size_t *queue,
                                            size_t *tail) {
  size_t file_index = find_file(context, path);
  const AbValue *symbols;
  size_t index;
  size_t matched = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (file_index == SIZE_MAX)
    return ARCHBIRD_OK;
  symbols = context->files[file_index].symbols;
  for (index = 0; status == ARCHBIRD_OK && index < symbols->as.array.count;
       index++) {
    const AbValue *row = &symbols->as.array.items[index];
    const AbValue *candidate_name;
    const AbValue *kind;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.files[].symbols[] must be objects");
    candidate_name = ab_value_member(row, "name");
    kind = ab_value_member(row, "kind");
    if (!candidate_name || candidate_name->kind != AB_VALUE_STRING || !kind ||
        kind->kind != AB_VALUE_STRING)
      return query_error(context, "map.files[].symbols[] identity is invalid");
    if (!ab_string_equal(&candidate_name->as.text, name) ||
        string_literal(&kind->as.text, "declaration"))
      continue;
    matched++;
    status = append_related_symbol_row(context, file_index, row,
                                       &candidate_name->as.text);
  }
  if (status != ARCHBIRD_OK || !matched)
    return status;
  context->files[file_index].symbol_seed = 1;
  context->files[file_index].related_symbol = 1;
  if (!context->files[file_index].selected) {
    context->files[file_index].selected = 1;
    context->files[file_index].distance = 1;
    queue[(*tail)++] = file_index;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_related_file(QueryContext *context,
                                          const AbString *path, size_t *queue,
                                          size_t *tail) {
  size_t file_index = find_file(context, path);
  if (file_index == SIZE_MAX || context->files[file_index].selected)
    return ARCHBIRD_OK;
  context->files[file_index].selected = 1;
  context->files[file_index].distance = 1;
  queue[(*tail)++] = file_index;
  return ARCHBIRD_OK;
}

static ArchbirdStatus select_symbol_relations(QueryContext *context,
                                              const AbValue *relations,
                                              size_t *queue, size_t *tail) {
  int downstream = string_literal(context->request.direction, "downstream") ||
                   string_literal(context->request.direction, "both");
  int upstream = string_literal(context->request.direction, "upstream") ||
                 string_literal(context->request.direction, "both");
  size_t index;
  for (index = 0; index < relations->as.array.count; index++) {
    const AbValue *row = &relations->as.array.items[index];
    const AbValue *source;
    const AbValue *source_path;
    const AbValue *source_symbol;
    const AbValue *candidates;
    const AbValue *resolution;
    size_t candidate;
    int source_selected;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map symbol relation must be an object");
    source = ab_value_member(row, "source");
    candidates = ab_value_member(row, "candidates");
    resolution = ab_value_member(row, "resolution");
    if (!source || source->kind != AB_VALUE_OBJECT || !candidates ||
        candidates->kind != AB_VALUE_ARRAY || !resolution ||
        resolution->kind != AB_VALUE_STRING)
      return query_error(context, "map symbol relation shape is invalid");
    source_path = ab_value_member(source, "path");
    source_symbol = ab_value_member(source, "symbol");
    if (!source_path || source_path->kind != AB_VALUE_STRING ||
        (source_symbol && source_symbol->kind != AB_VALUE_STRING))
      return query_error(context, "map symbol relation source is invalid");
    if (!string_literal(&resolution->as.text, "unique") &&
        !string_literal(&resolution->as.text, "candidate") &&
        !string_literal(&resolution->as.text, "ambiguous"))
      continue;
    source_selected = source_symbol &&
                      selected_symbol_identity(context, &source_path->as.text,
                                               &source_symbol->as.text);
    for (candidate = 0; candidate < candidates->as.array.count; candidate++) {
      const AbValue *target = &candidates->as.array.items[candidate];
      const AbValue *target_path;
      const AbValue *target_symbol;
      int target_selected;
      ArchbirdStatus status;
      if (target->kind != AB_VALUE_OBJECT)
        return query_error(context,
                           "map symbol relation candidate must be an object");
      target_path = ab_value_member(target, "path");
      target_symbol = ab_value_member(target, "symbol");
      if (!target_path || target_path->kind != AB_VALUE_STRING ||
          !target_symbol || target_symbol->kind != AB_VALUE_STRING)
        return query_error(context, "map symbol relation candidate is invalid");
      target_selected = selected_symbol_identity(context, &target_path->as.text,
                                                 &target_symbol->as.text);
      if (upstream && target_selected) {
        status =
            source_symbol
                ? select_related_symbol(context, &source_path->as.text,
                                        &source_symbol->as.text, queue, tail)
                : select_related_file(context, &source_path->as.text, queue,
                                      tail);
        if (status != ARCHBIRD_OK)
          return status;
      }
      if (downstream && source_selected) {
        status = select_related_symbol(context, &target_path->as.text,
                                       &target_symbol->as.text, queue, tail);
        if (status != ARCHBIRD_OK)
          return status;
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus traverse(QueryContext *context) {
  size_t *queue = NULL;
  size_t head = 0;
  size_t tail = 0;
  size_t index;
  int downstream = string_literal(context->request.direction, "downstream") ||
                   string_literal(context->request.direction, "both");
  int upstream = string_literal(context->request.direction, "upstream") ||
                 string_literal(context->request.direction, "both");
  if (context->file_count) {
    queue = (size_t *)ab_malloc(context->engine,
                                context->file_count * sizeof(*queue));
    if (!queue)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory traversing query graph");
  }
  for (index = 0; index < context->file_count; index++) {
    if (!context->files[index].seed) {
      if (context->exact_symbol_scope && context->files[index].symbol_seed) {
        context->files[index].distance = 0;
        context->files[index].selected = 1;
        continue;
      } else {
        continue;
      }
    }
    context->files[index].distance = 0;
    context->files[index].selected = 1;
    queue[tail++] = index;
  }
  if (context->exact_symbol_scope && context->request.depth) {
    ArchbirdStatus status =
        select_symbol_relations(context, context->symbol_calls, queue, &tail);
    if (status == ARCHBIRD_OK)
      status = select_symbol_relations(context, context->symbol_references,
                                       queue, &tail);
    if (status != ARCHBIRD_OK) {
      ab_free(context->engine, queue);
      return status;
    }
  }
  while (head < tail) {
    size_t current = queue[head++];
    size_t edge_index;
    if (context->files[current].distance >= context->request.depth)
      continue;
    for (edge_index = 0; edge_index < context->edge_count; edge_index++) {
      const QueryEdge *edge = &context->edges[edge_index];
      size_t target = SIZE_MAX;
      if (!edge->target_mapped || edge->source_index == SIZE_MAX)
        continue;
      if (downstream && edge->source_index == current)
        target = edge->target_index;
      if (upstream && edge->target_index == current)
        target = edge->source_index;
      if (target == SIZE_MAX || context->files[target].selected)
        continue;
      context->files[target].selected = 1;
      context->files[target].distance = context->files[current].distance + 1;
      queue[tail++] = target;
    }
  }
  ab_free(context->engine, queue);
  return ARCHBIRD_OK;
}

static int any_initial_match(const QueryContext *context) {
  size_t index;
  for (index = 0; index < context->file_count; index++) {
    if (context->files[index].seed)
      return 1;
  }
  for (index = 0; index < context->artifact_count; index++) {
    if (context->artifact_selected[index])
      return 1;
  }
  if (context->symbol_count)
    return 1;
  return 0;
}

typedef struct QueryOrder {
  const QueryFile *file;
} QueryOrder;

static int order_compare(const void *left_raw, const void *right_raw) {
  const QueryFile *left = ((const QueryOrder *)left_raw)->file;
  const QueryFile *right = ((const QueryOrder *)right_raw)->file;
  if (left->distance != right->distance)
    return (left->distance > right->distance) -
           (left->distance < right->distance);
  if (left->degree != right->degree)
    return (left->degree < right->degree) - (left->degree > right->degree);
  return ab_string_compare(left->path, right->path);
}

static ArchbirdStatus selected_order(QueryContext *context, QueryOrder **out,
                                     size_t *out_count) {
  QueryOrder *rows = NULL;
  size_t count = 0;
  size_t index;
  *out = NULL;
  *out_count = 0;
  if (context->file_count) {
    rows = (QueryOrder *)ab_malloc(context->engine,
                                   context->file_count * sizeof(*rows));
    if (!rows)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory ordering query files");
  }
  for (index = 0; index < context->file_count; index++) {
    if (context->files[index].selected)
      rows[count++].file = &context->files[index];
  }
  if (count > 1)
    qsort(rows, count, sizeof(*rows), order_compare);
  *out = rows;
  *out_count = count;
  return ARCHBIRD_OK;
}

static int selected_path(const QueryContext *context, const AbString *path) {
  size_t index = find_file(context, path);
  return index != SIZE_MAX && context->files[index].selected;
}

static ArchbirdStatus finalize_artifacts(QueryContext *context) {
  const AbValue *artifacts = ab_value_member(context->map, "artifacts");
  size_t index;
  int changed = 1;
  for (index = 0; index < artifacts->as.array.count; index++) {
    const AbValue *row = &artifacts->as.array.items[index];
    const AbValue *inputs = ab_value_member(row, "inputs");
    const AbValue *loaders = ab_value_member(row, "loaded_by");
    size_t item;
    for (item = 0;
         !context->artifact_selected[index] && item < inputs->as.array.count;
         item++) {
      const AbString *path =
          required_string(context, &inputs->as.array.items[item], "path",
                          "map.artifacts[].inputs[]");
      if (!path)
        return ARCHBIRD_INVALID_SCHEMA;
      context->artifact_selected[index] = selected_path(context, path);
    }
    for (item = 0;
         !context->artifact_selected[index] && item < loaders->as.array.count;
         item++) {
      const AbString *path =
          required_string(context, &loaders->as.array.items[item], "path",
                          "map.artifacts[].loaded_by[]");
      if (!path)
        return ARCHBIRD_INVALID_SCHEMA;
      context->artifact_selected[index] = selected_path(context, path);
    }
  }
  while (changed) {
    changed = 0;
    for (index = 0; index < artifacts->as.array.count; index++) {
      const AbValue *row = &artifacts->as.array.items[index];
      const AbValue *depends = ab_value_member(row, "depends_on");
      const AbString *name =
          required_string(context, row, "name", "map.artifacts[]");
      size_t dependency;
      int connected = context->artifact_selected[index];
      if (!name || !string_array_valid(depends))
        return ARCHBIRD_INVALID_SCHEMA;
      for (dependency = 0; !connected && dependency < depends->as.array.count;
           dependency++) {
        size_t candidate;
        const AbString *wanted = &depends->as.array.items[dependency].as.text;
        for (candidate = 0; candidate < artifacts->as.array.count;
             candidate++) {
          const AbString *candidate_name =
              required_string(context, &artifacts->as.array.items[candidate],
                              "name", "map.artifacts[]");
          if (!candidate_name)
            return ARCHBIRD_INVALID_SCHEMA;
          if (context->artifact_selected[candidate] &&
              ab_string_equal(candidate_name, wanted)) {
            connected = 1;
            break;
          }
        }
      }
      if (!connected)
        continue;
      if (!context->artifact_selected[index]) {
        context->artifact_selected[index] = 1;
        changed = 1;
      }
      for (dependency = 0; dependency < depends->as.array.count; dependency++) {
        size_t candidate;
        const AbString *wanted = &depends->as.array.items[dependency].as.text;
        for (candidate = 0; candidate < artifacts->as.array.count;
             candidate++) {
          const AbString *candidate_name =
              required_string(context, &artifacts->as.array.items[candidate],
                              "name", "map.artifacts[]");
          if (!candidate_name)
            return ARCHBIRD_INVALID_SCHEMA;
          if (ab_string_equal(candidate_name, wanted) &&
              !context->artifact_selected[candidate]) {
            context->artifact_selected[candidate] = 1;
            changed = 1;
          }
        }
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus finalize_components_packages(QueryContext *context) {
  const AbValue *components = ab_value_member(context->map, "components");
  const AbValue *packages = ab_value_member(context->map, "packages");
  size_t index;
  for (index = 0; index < components->as.array.count; index++) {
    const AbValue *files =
        ab_value_member(&components->as.array.items[index], "files");
    size_t file;
    for (file = 0;
         !context->component_selected[index] && file < files->as.array.count;
         file++)
      context->component_selected[index] =
          selected_path(context, &files->as.array.items[file].as.text);
  }
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *entrypoints =
        ab_value_member(&packages->as.array.items[index], "entrypoints");
    size_t field;
    for (field = 0; !context->package_selected[index] &&
                    field < entrypoints->as.object.count;
         field++) {
      const AbValue *target = &entrypoints->as.object.fields[field].value;
      context->package_selected[index] =
          target->kind == AB_VALUE_STRING &&
          selected_path(context, &target->as.text);
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_named_value(AbBuffer *buffer, const char *name,
                                         const AbValue *value, int *first) {
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!*first)
    status = ab_buffer_literal(buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, name, strlen(name));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, value);
  *first = 0;
  return status;
}

static ArchbirdStatus render_query_file_symbols(AbBuffer *buffer,
                                                const QueryContext *context,
                                                const QueryFile *file) {
  size_t index;
  size_t file_index = (size_t)(file - context->files);
  int first = 1;
  ArchbirdStatus status;
  if (!context->exact_symbol_scope || !file->symbol_seed)
    return ab_value_render(buffer, file->symbols);
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->symbol_count;
       index++) {
    if (context->symbols[index].file_index != file_index)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, context->symbols[index].row);
    first = 0;
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < context->related_symbol_count;
       index++) {
    if (context->related_symbols[index].file_index != file_index)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, context->related_symbols[index].row);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_file_rows(AbBuffer *buffer,
                                       const QueryContext *context,
                                       const QueryOrder *order,
                                       size_t order_count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < order_count; index++) {
    const QueryFile *file = order[index].file;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"distance\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, file->distance);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"language\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, file->language);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"layer\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, file->layer);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, file->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"sha256\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, file->sha256);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"symbols\":");
    if (status == ARCHBIRD_OK)
      status = render_query_file_symbols(buffer, context, file);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_matched_symbols(AbBuffer *buffer,
                                             const QueryContext *context) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->symbol_count;
       index++) {
    const QuerySymbol *symbol = &context->symbols[index];
    const AbValue *syntax_recovery =
        ab_value_member(symbol->row, "syntax_recovery");
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"kind\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->kind);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, symbol->line);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"scope\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->scope);
    if (status == ARCHBIRD_OK && syntax_recovery &&
        syntax_recovery->kind == AB_VALUE_STRING) {
      status = ab_buffer_literal(buffer, ",\"syntax_recovery\":");
      if (status == ARCHBIRD_OK)
        status = render_string(buffer, &syntax_recovery->as.text);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int selected_symbol_identity(const QueryContext *context,
                                    const AbString *path,
                                    const AbString *name) {
  size_t index;
  for (index = 0; index < context->symbol_count; index++)
    if (ab_string_equal(context->symbols[index].path, path) &&
        ab_string_equal(context->symbols[index].name, name))
      return 1;
  return 0;
}

static ArchbirdStatus render_selected_symbol_calls(AbBuffer *buffer,
                                                   QueryContext *context) {
  size_t index;
  int first = 1;
  int downstream = string_literal(context->request.direction, "downstream") ||
                   string_literal(context->request.direction, "both");
  int upstream = string_literal(context->request.direction, "upstream") ||
                 string_literal(context->request.direction, "both");
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0;
       status == ARCHBIRD_OK && index < context->symbol_calls->as.array.count;
       index++) {
    const AbValue *row = &context->symbol_calls->as.array.items[index];
    const AbValue *source;
    const AbValue *source_path;
    const AbValue *source_symbol;
    const AbValue *candidates;
    size_t candidate;
    int selected = 0;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.symbol_calls[] must be objects");
    source = ab_value_member(row, "source");
    candidates = ab_value_member(row, "candidates");
    if (!source || source->kind != AB_VALUE_OBJECT || !candidates ||
        candidates->kind != AB_VALUE_ARRAY)
      return query_error(context, "map.symbol_calls[] shape is invalid");
    source_path = ab_value_member(source, "path");
    source_symbol = ab_value_member(source, "symbol");
    if (!source_path || source_path->kind != AB_VALUE_STRING ||
        (source_symbol && source_symbol->kind != AB_VALUE_STRING))
      return query_error(context, "map.symbol_calls[].source is invalid");
    if (downstream)
      selected = context->exact_symbol_scope
                     ? source_symbol && selected_symbol_identity(
                                            context, &source_path->as.text,
                                            &source_symbol->as.text)
                     : selected_path(context, &source_path->as.text);
    for (candidate = 0;
         upstream && !selected && candidate < candidates->as.array.count;
         candidate++) {
      const AbValue *target = &candidates->as.array.items[candidate];
      const AbValue *target_path;
      const AbValue *target_symbol;
      if (target->kind != AB_VALUE_OBJECT)
        return query_error(context,
                           "map.symbol_calls[].candidates[] is invalid");
      target_path = ab_value_member(target, "path");
      target_symbol = ab_value_member(target, "symbol");
      if (!target_path || target_path->kind != AB_VALUE_STRING ||
          !target_symbol || target_symbol->kind != AB_VALUE_STRING)
        return query_error(context,
                           "map.symbol_calls[] candidate identity is invalid");
      selected = context->exact_symbol_scope
                     ? selected_symbol_identity(context, &target_path->as.text,
                                                &target_symbol->as.text)
                     : selected_path(context, &target_path->as.text);
    }
    if (!selected)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, row);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_selected_symbol_references(AbBuffer *buffer,
                                                        QueryContext *context) {
  size_t index;
  int first = 1;
  int downstream = string_literal(context->request.direction, "downstream") ||
                   string_literal(context->request.direction, "both");
  int upstream = string_literal(context->request.direction, "upstream") ||
                 string_literal(context->request.direction, "both");
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK &&
                  index < context->symbol_references->as.array.count;
       index++) {
    const AbValue *row = &context->symbol_references->as.array.items[index];
    const AbValue *source;
    const AbValue *source_path;
    const AbValue *source_symbol;
    const AbValue *candidates;
    size_t candidate;
    int selected = 0;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.symbol_references[] must be objects");
    source = ab_value_member(row, "source");
    candidates = ab_value_member(row, "candidates");
    if (!source || source->kind != AB_VALUE_OBJECT || !candidates ||
        candidates->kind != AB_VALUE_ARRAY)
      return query_error(context, "map.symbol_references[] shape is invalid");
    source_path = ab_value_member(source, "path");
    source_symbol = ab_value_member(source, "symbol");
    if (!source_path || source_path->kind != AB_VALUE_STRING ||
        (source_symbol && source_symbol->kind != AB_VALUE_STRING))
      return query_error(context, "map.symbol_references[].source is invalid");
    if (downstream)
      selected = context->exact_symbol_scope
                     ? source_symbol && selected_symbol_identity(
                                            context, &source_path->as.text,
                                            &source_symbol->as.text)
                     : selected_path(context, &source_path->as.text);
    for (candidate = 0;
         upstream && !selected && candidate < candidates->as.array.count;
         candidate++) {
      const AbValue *target = &candidates->as.array.items[candidate];
      const AbValue *target_path;
      const AbValue *target_symbol;
      if (target->kind != AB_VALUE_OBJECT)
        return query_error(context,
                           "map.symbol_references[].candidates[] is invalid");
      target_path = ab_value_member(target, "path");
      target_symbol = ab_value_member(target, "symbol");
      if (!target_path || target_path->kind != AB_VALUE_STRING ||
          !target_symbol || target_symbol->kind != AB_VALUE_STRING)
        return query_error(
            context, "map.symbol_references[] candidate identity is invalid");
      selected = context->exact_symbol_scope
                     ? selected_symbol_identity(context, &target_path->as.text,
                                                &target_symbol->as.text)
                     : selected_path(context, &target_path->as.text);
    }
    if (!selected)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, row);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_selected_edges(AbBuffer *buffer,
                                            const QueryContext *context) {
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->edge_count;
       index++) {
    const QueryEdge *edge = &context->edges[index];
    if (edge->source_index == SIZE_MAX ||
        !context->files[edge->source_index].selected ||
        (edge->target_mapped && !context->files[edge->target_index].selected))
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, edge->row);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_selected_resolutions(AbBuffer *buffer,
                                                  QueryContext *context) {
  const AbValue *rows =
      required_array(context, context->map, "call_resolutions", "map");
  size_t index;
  int first = 1;
  ArchbirdStatus status =
      rows ? ab_buffer_literal(buffer, "[") : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbString *source;
    if (row->kind != AB_VALUE_OBJECT)
      return query_error(context, "map.call_resolutions[] must be objects");
    source = required_string(context, row, "source", "map.call_resolutions[]");
    if (!source)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!selected_path(context, source))
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, row);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_component_names(AbBuffer *buffer,
                                             QueryContext *context) {
  const AbValue *rows = ab_value_member(context->map, "components");
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbString *name;
    if (!context->component_selected[index])
      continue;
    name = required_string(context, &rows->as.array.items[index], "name",
                           "map.components[]");
    if (!name)
      return ARCHBIRD_INVALID_SCHEMA;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, name);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_packages(AbBuffer *buffer, QueryContext *context) {
  const AbValue *rows = ab_value_member(context->map, "packages");
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row;
    const char *names[] = {"entrypoints", "export_origins", "identity",
                           "kind",        "name",           "version"};
    size_t field;
    int first_field = 1;
    if (!context->package_selected[index])
      continue;
    row = &rows->as.array.items[index];
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{");
    for (field = 0;
         status == ARCHBIRD_OK && field < sizeof(names) / sizeof(names[0]);
         field++) {
      const AbValue *value = ab_value_member(row, names[field]);
      if (!value)
        return query_error(context, "map package projection is incomplete");
      status = render_named_value(buffer, names[field], value, &first_field);
    }
    if (status == ARCHBIRD_OK) {
      const AbValue *surfaces = ab_value_member(row, "entrypoint_surfaces");
      if (surfaces)
        status = render_named_value(buffer, "entrypoint_surfaces", surfaces,
                                    &first_field);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int selected_artifact_build(const QueryContext *context,
                                   const AbString *source,
                                   const AbString *target) {
  const AbValue *artifacts = ab_value_member(context->map, "artifacts");
  size_t artifact;
  for (artifact = 0; artifact < artifacts->as.array.count; artifact++) {
    const AbValue *builds;
    size_t build;
    if (!context->artifact_selected[artifact])
      continue;
    builds = ab_value_member(&artifacts->as.array.items[artifact], "builds");
    for (build = 0; build < builds->as.array.count; build++) {
      const AbValue *row = &builds->as.array.items[build];
      const AbValue *row_source = ab_value_member(row, "source");
      const AbValue *row_target = ab_value_member(row, "target");
      if (row_source && row_target && row_source->kind == AB_VALUE_STRING &&
          row_target->kind == AB_VALUE_STRING &&
          ab_string_equal(&row_source->as.text, source) &&
          ab_string_equal(&row_target->as.text, target))
        return 1;
    }
  }
  return 0;
}

static ArchbirdStatus render_builds(AbBuffer *buffer, QueryContext *context) {
  const AbValue *rows = required_array(context, context->map, "builds", "map");
  size_t index;
  int first = 1;
  ArchbirdStatus status =
      rows ? ab_buffer_literal(buffer, "[") : ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbString *source =
        required_string(context, row, "source", "map.builds[]");
    const AbString *name =
        required_string(context, row, "name", "map.builds[]");
    const AbValue *paths =
        required_array(context, row, "paths", "map.builds[]");
    const char *fields[] = {"conditions", "deps", "name", "paths", "source"};
    size_t path_index;
    size_t field;
    int selected;
    int first_field = 1;
    if (!source || !name || !paths || !string_array_valid(paths))
      return ARCHBIRD_INVALID_SCHEMA;
    selected = selected_path(context, source) ||
               selected_artifact_build(context, source, name);
    for (path_index = 0; !selected && path_index < paths->as.array.count;
         path_index++)
      selected =
          selected_path(context, &paths->as.array.items[path_index].as.text);
    if (!selected)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{");
    for (field = 0;
         status == ARCHBIRD_OK && field < sizeof(fields) / sizeof(fields[0]);
         field++) {
      const AbValue *value = ab_value_member(row, fields[field]);
      if (!value)
        return query_error(context, "map build projection is incomplete");
      status = render_named_value(buffer, fields[field], value, &first_field);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_artifacts(AbBuffer *buffer,
                                       const QueryContext *context) {
  const AbValue *rows = ab_value_member(context->map, "artifacts");
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    if (!context->artifact_selected[index])
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, &rows->as.array.items[index]);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int surface_name_selected(QueryContext *context, const AbValue *row) {
  const char *fields[] = {"declarations", "uses"};
  size_t field;
  const AbValue *candidates = ab_value_member(row, "candidates");
  size_t index;
  if (!string_array_valid(candidates))
    return -1;
  for (index = 0; index < candidates->as.array.count; index++) {
    if (selected_path(context, &candidates->as.array.items[index].as.text))
      return 1;
  }
  for (field = 0; field < sizeof(fields) / sizeof(fields[0]); field++) {
    const AbValue *items = ab_value_member(row, fields[field]);
    if (!items || items->kind != AB_VALUE_ARRAY)
      return -1;
    for (index = 0; index < items->as.array.count; index++) {
      const AbValue *path =
          ab_value_member(&items->as.array.items[index], "path");
      if (!path || path->kind != AB_VALUE_STRING)
        return -1;
      if (selected_path(context, &path->as.text))
        return 1;
    }
  }
  return 0;
}

typedef struct SurfaceSummary {
  size_t ambiguous;
  size_t declaration_unknown;
  size_t ignored;
  size_t registered;
  size_t resolved;
  size_t unregistered_use;
  size_t unresolved;
  size_t unused;
  size_t used;
} SurfaceSummary;

static ArchbirdStatus add_surface_summary(QueryContext *context,
                                          const AbValue *row,
                                          SurfaceSummary *summary) {
  const AbValue *ignored = ab_value_member(row, "ignored");
  const AbValue *declaration = ab_value_member(row, "declaration");
  const AbValue *resolution = ab_value_member(row, "resolution");
  const AbValue *uses = ab_value_member(row, "uses");
  int used;
  if (!ignored || ignored->kind != AB_VALUE_BOOL || !declaration ||
      declaration->kind != AB_VALUE_STRING || !resolution ||
      resolution->kind != AB_VALUE_STRING || !uses ||
      uses->kind != AB_VALUE_ARRAY)
    return query_error(context, "map surface name shape is invalid");
  if (ignored->as.boolean) {
    summary->ignored++;
    return ARCHBIRD_OK;
  }
  used = uses->as.array.count != 0;
  if (string_literal(&declaration->as.text, "declared")) {
    summary->registered++;
    if (!used)
      summary->unused++;
  } else if (string_literal(&declaration->as.text, "undeclared") && used) {
    summary->unregistered_use++;
  } else if (string_literal(&declaration->as.text, "unknown")) {
    summary->declaration_unknown++;
  }
  if (used)
    summary->used++;
  if (string_literal(&resolution->as.text, "unique"))
    summary->resolved++;
  else if (string_literal(&resolution->as.text, "ambiguous"))
    summary->ambiguous++;
  else if (string_literal(&resolution->as.text, "unresolved"))
    summary->unresolved++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_surface_summary(AbBuffer *buffer,
                                             const SurfaceSummary *summary) {
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"ambiguous\":");
#define QUERY_SUMMARY_FIELD(label, field)                                      \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_u64(buffer, summary->field);                          \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_literal(buffer, label);                               \
  } while (0)
  QUERY_SUMMARY_FIELD(",\"declaration_unknown\":", ambiguous);
  QUERY_SUMMARY_FIELD(",\"ignored\":", declaration_unknown);
  QUERY_SUMMARY_FIELD(",\"registered\":", ignored);
  QUERY_SUMMARY_FIELD(",\"resolved\":", registered);
  QUERY_SUMMARY_FIELD(",\"unregistered_use\":", resolved);
  QUERY_SUMMARY_FIELD(",\"unresolved\":", unregistered_use);
  QUERY_SUMMARY_FIELD(",\"unused\":", unresolved);
  QUERY_SUMMARY_FIELD(",\"used\":", unused);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, summary->used);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
#undef QUERY_SUMMARY_FIELD
  return status;
}

static ArchbirdStatus render_surfaces(AbBuffer *buffer, QueryContext *context) {
  const AbValue *surfaces =
      required_array(context, context->map, "surfaces", "map");
  size_t surface_index;
  int first_surface = 1;
  ArchbirdStatus status =
      surfaces ? ab_buffer_literal(buffer, "[") : ARCHBIRD_INVALID_SCHEMA;
  for (surface_index = 0;
       status == ARCHBIRD_OK && surface_index < surfaces->as.array.count;
       surface_index++) {
    const AbValue *surface = &surfaces->as.array.items[surface_index];
    const AbValue *names = ab_value_member(surface, "names");
    const char *copy_fields[] = {"kind", "name"};
    SurfaceSummary summary = {0};
    size_t name_index;
    size_t selected_count = 0;
    size_t field;
    int first_field = 1;
    if (!names || names->kind != AB_VALUE_ARRAY)
      return query_error(context, "map.surfaces[].names must be an array");
    for (name_index = 0; name_index < names->as.array.count; name_index++) {
      int selected =
          surface_name_selected(context, &names->as.array.items[name_index]);
      if (selected < 0)
        return query_error(context, "map surface evidence shape is invalid");
      if (selected)
        selected_count++;
    }
    if (!selected_count)
      continue;
    if (!first_surface)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{");
    for (field = 0; status == ARCHBIRD_OK &&
                    field < sizeof(copy_fields) / sizeof(copy_fields[0]);
         field++) {
      const AbValue *value = ab_value_member(surface, copy_fields[field]);
      if (!value)
        return query_error(context, "map surface metadata is incomplete");
      status =
          render_named_value(buffer, copy_fields[field], value, &first_field);
    }
    if (status == ARCHBIRD_OK) {
      if (!first_field)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "\"names\":[");
      first_field = 0;
    }
    {
      int first_name = 1;
      for (name_index = 0;
           status == ARCHBIRD_OK && name_index < names->as.array.count;
           name_index++) {
        const AbValue *name = &names->as.array.items[name_index];
        int selected = surface_name_selected(context, name);
        if (!selected)
          continue;
        if (!first_name)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_value_render(buffer, name);
        if (status == ARCHBIRD_OK)
          status = add_surface_summary(context, name, &summary);
        first_name = 0;
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "]");
    if (status == ARCHBIRD_OK) {
      const AbValue *configured =
          ab_value_member(surface, "provider_configured");
      const AbValue *providers = ab_value_member(surface, "providers");
      status = render_named_value(buffer, "provider_configured", configured,
                                  &first_field);
      if (status == ARCHBIRD_OK)
        status =
            render_named_value(buffer, "providers", providers, &first_field);
    }
    if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(buffer, ",\"summary\":");
      if (status == ARCHBIRD_OK)
        status = render_surface_summary(buffer, &summary);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first_surface = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_tests(AbBuffer *buffer, QueryContext *context) {
  const AbValue *tests = ab_value_member(context->map, "tests");
  const char *fields[] = {"cases", "count", "framework",
                          "group", "path",  "routes"};
  const char *optional_fields[] = {"count_unit", "generated", "generated_from",
                                   "route_evidence"};
  size_t test_index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (test_index = 0;
       status == ARCHBIRD_OK && test_index < tests->as.array.count;
       test_index++) {
    const AbValue *test;
    size_t field;
    int first_field = 1;
    if (!context->test_selected[test_index])
      continue;
    test = &tests->as.array.items[test_index];
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{");
    for (field = 0;
         status == ARCHBIRD_OK && field < sizeof(fields) / sizeof(fields[0]);
         field++) {
      const AbValue *value = ab_value_member(test, fields[field]);
      if (!value)
        return query_error(context, "map test projection is incomplete");
      status = render_named_value(buffer, fields[field], value, &first_field);
    }
    for (field = 0;
         status == ARCHBIRD_OK &&
         field < sizeof(optional_fields) / sizeof(optional_fields[0]);
         field++) {
      const AbValue *value = ab_value_member(test, optional_fields[field]);
      if (value)
        status = render_named_value(buffer, optional_fields[field], value,
                                    &first_field);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_match_route_evidence(AbBuffer *buffer,
                                                  const QueryContext *context,
                                                  const QueryTestMatch *match) {
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  if (match->evidence_target == SIZE_MAX)
    return status == ARCHBIRD_OK ? ab_buffer_literal(buffer, "]") : status;
  if (context->symbol_count &&
      !context->files[match->evidence_target].requested_seed &&
      !(context->exact_symbol_scope &&
        context->files[match->evidence_target].symbol_seed))
    return status == ARCHBIRD_OK ? ab_buffer_literal(buffer, "]") : status;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < match->row->route_evidence->as.array.count;
       index++) {
    const AbValue *evidence =
        &match->row->route_evidence->as.array.items[index];
    const AbValue *target = evidence->kind == AB_VALUE_OBJECT
                                ? ab_value_member(evidence, "target")
                                : NULL;
    const AbValue *target_symbol =
        evidence->kind == AB_VALUE_OBJECT
            ? ab_value_member(evidence, "target_symbol")
            : NULL;
    int requires_symbol = 0;
    int selected_symbol = 0;
    if (!target || target->kind != AB_VALUE_STRING)
      return query_error((QueryContext *)context,
                         "map test route evidence shape is invalid");
    if (!ab_string_equal(&target->as.text,
                         context->files[match->evidence_target].path))
      continue;
    if (target_symbol && target_symbol->kind != AB_VALUE_STRING)
      return query_error((QueryContext *)context,
                         "map test route target_symbol is invalid");
    selected_symbol = target_symbol_is_selected(
        context, match->evidence_target,
        target_symbol ? &target_symbol->as.text : NULL, &requires_symbol);
    if (requires_symbol && !selected_symbol)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, evidence);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int match_evidence_kind(const QueryTestMatch *match,
                               const AbValue *evidence) {
  const AbValue *provenance = ab_value_member(evidence, "provenance");
  const AbValue *relation = ab_value_member(evidence, "relation");
  if (!provenance || provenance->kind != AB_VALUE_STRING)
    return 0;
  if (!string_literal(&provenance->as.text, match->provenance))
    return 0;
  if (!strcmp(match->provenance, "observed"))
    return relation && relation->kind == AB_VALUE_STRING &&
           string_literal(&relation->as.text, "observed-symbol-hit");
  if (!strcmp(match->provenance, "derived")) {
    if (!relation || relation->kind != AB_VALUE_STRING)
      return 0;
    if (!strcmp(match->confidence, "candidate"))
      return candidate_relation(&relation->as.text);
    if (!strcmp(match->confidence, "exact"))
      return !candidate_relation(&relation->as.text);
  }
  return !strcmp(match->provenance, "asserted");
}

static const AbString *match_target_symbol(const QueryContext *context,
                                           const QueryTestMatch *match) {
  const AbString *selected = NULL;
  size_t index;
  if (match->evidence_target == SIZE_MAX ||
      !strcmp(match->confidence, "conservative") ||
      !strcmp(match->confidence, "unresolved"))
    return NULL;
  for (index = 0; index < match->row->route_evidence->as.array.count; index++) {
    const AbValue *evidence =
        &match->row->route_evidence->as.array.items[index];
    const AbValue *target;
    const AbValue *target_symbol;
    int requires_symbol;
    int symbol_selected;
    if (evidence->kind != AB_VALUE_OBJECT ||
        !match_evidence_kind(match, evidence))
      continue;
    target = ab_value_member(evidence, "target");
    target_symbol = ab_value_member(evidence, "target_symbol");
    if (!target || target->kind != AB_VALUE_STRING || !target_symbol ||
        target_symbol->kind != AB_VALUE_STRING ||
        !target_symbol->as.text.length)
      continue;
    if (!ab_string_equal(&target->as.text,
                         context->files[match->evidence_target].path))
      continue;
    symbol_selected =
        target_symbol_is_selected(context, match->evidence_target,
                                  &target_symbol->as.text, &requires_symbol);
    if (requires_symbol && !symbol_selected)
      continue;
    if (!selected) {
      selected = &target_symbol->as.text;
    } else if (!ab_string_equal(selected, &target_symbol->as.text)) {
      return NULL;
    }
  }
  return selected;
}

static ArchbirdStatus render_match_target(AbBuffer *buffer,
                                          const QueryContext *context,
                                          const QueryTestMatch *match) {
  const AbString *symbol;
  ArchbirdStatus status;
  if (match->evidence_target == SIZE_MAX)
    return ab_buffer_literal(buffer, "null");
  symbol = match_target_symbol(context, match);
  status = ab_buffer_literal(buffer, "{\"path\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, context->files[match->evidence_target].path);
  if (status == ARCHBIRD_OK && symbol) {
    status = ab_buffer_literal(buffer, ",\"symbol\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_test_matches(AbBuffer *buffer,
                                          const QueryContext *context) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->test_match_count;
       index++) {
    const QueryTestMatch *match = &context->test_matches[index];
    size_t route;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK && match->classification) {
      status = ab_buffer_literal(buffer, "{\"classification\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, match->classification,
                                       strlen(match->classification));
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"confidence\":");
    } else if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(buffer, "{\"confidence\":");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, match->confidence,
                                     strlen(match->confidence));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"configured_targets\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, match->row->configured);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, match->evidence,
                                     strlen(match->evidence));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"evidence_scope\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, match->evidence_scope,
                                     strlen(match->evidence_scope));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"group\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, match->row->group);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, match->row->line);
    if (status == ARCHBIRD_OK && match->row->inventory_state) {
      status = ab_buffer_literal(buffer, ",\"inventory_state\":");
      if (status == ARCHBIRD_OK)
        status = render_string(buffer, match->row->inventory_state);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, match->row->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"provenance\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, match->provenance,
                                     strlen(match->provenance));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"ranking_affinity\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, match->ranking_affinity);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"route\":[");
    for (route = 0; status == ARCHBIRD_OK && route < match->route_count;
         route++) {
      if (route)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status =
            render_string(buffer, context->files[match->route[route]].path);
    }
    if (status == ARCHBIRD_OK && match->classification) {
      status = ab_buffer_literal(buffer, "],\"route_evidence\":");
      if (status == ARCHBIRD_OK)
        status = render_match_route_evidence(buffer, context, match);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"seed_distance\":");
    } else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"seed_distance\":");
    if (status == ARCHBIRD_OK) {
      if (match->seed_distance == SIZE_MAX)
        status = ab_buffer_literal(buffer, "null");
      else
        status = ab_buffer_u64(buffer, match->seed_distance);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"selector\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, match->row->selector);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = render_match_target(buffer, context, match);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"target_role\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, match->target_role,
                                     strlen(match->target_role));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_focus_labels(AbBuffer *buffer,
                                          const QueryContext *context) {
  const AbValue *groups[] = {
      context->request.focus,    context->request.paths,
      context->request.symbols,  context->request.components,
      context->request.packages, context->request.artifacts,
  };
  const char *prefixes[] = {
      "", "path:", "symbol:", "component:", "package:", "artifact:"};
  size_t group;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (group = 0;
       status == ARCHBIRD_OK && group < sizeof(groups) / sizeof(groups[0]);
       group++) {
    size_t index;
    for (index = 0;
         status == ARCHBIRD_OK && index < groups[group]->as.array.count;
         index++) {
      const AbString *value = &groups[group]->as.array.items[index].as.text;
      if (!first)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK && prefixes[group][0]) {
        AbBuffer label;
        ab_buffer_init(&label, context->engine);
        status = ab_buffer_literal(&label, prefixes[group]);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&label, value->data, value->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_json_string(buffer, (const char *)label.data,
                                         label.length);
        ab_buffer_free(&label);
      } else if (status == ARCHBIRD_OK) {
        status = render_string(buffer, value);
      }
      first = 0;
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_seed_identities(AbBuffer *buffer,
                                             const QueryContext *context) {
  size_t index;
  int first = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context->file_count;
       index++) {
    if (!context->files[index].seed)
      continue;
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"kind\":\"file\",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, context->files[index].path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  for (index = 0; status == ARCHBIRD_OK && index < context->symbol_count;
       index++) {
    const QuerySymbol *symbol = &context->symbols[index];
    if (!first)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"kind\":\"symbol\",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, symbol->line);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"symbol\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, symbol->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static ArchbirdStatus render_query_metadata(AbBuffer *buffer,
                                            const QueryContext *context) {
  size_t index;
  int first_seed = 1;
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"depth\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, context->request.depth);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"context\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, context->request.context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"direction\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, context->request.direction);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"focus\":");
  if (status == ARCHBIRD_OK)
    status = render_focus_labels(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"producer_compatibility\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, context->producer_compatibility,
                                   strlen(context->producer_compatibility));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"producer_policy\":");
  if (status == ARCHBIRD_OK)
    status = render_string(buffer, context->request.producer_policy);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"seeds\":[");
  for (index = 0; status == ARCHBIRD_OK && index < context->file_count;
       index++) {
    if (!context->files[index].seed)
      continue;
    if (!first_seed)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, context->files[index].path);
    first_seed = 0;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"scope\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(
        buffer, context->exact_symbol_scope ? "symbol" : "file",
        context->exact_symbol_scope ? 6 : 4);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"seed_identities\":");
  if (status == ARCHBIRD_OK)
    status = render_seed_identities(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbol_evidence_state\":");
  if (status == ARCHBIRD_OK) {
    const char *evidence_state = ab_value_member(context->map, "symbol_calls")
                                     ? "current"
                                     : "unavailable";
    status =
        ab_buffer_json_string(buffer, evidence_state, strlen(evidence_state));
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"test_depth\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, context->request.test_depth);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_query_document(QueryContext *context,
                                            AbBuffer *buffer,
                                            const QueryOrder *order,
                                            size_t order_count) {
  const AbValue *evidence = ab_value_member(context->map, "evidence");
  const AbValue *project = ab_value_member(context->map, "project");
  const AbValue *source_tool = ab_value_member(context->map, "tool");
  const AbValue *indexes = ab_value_member(context->map, "indexes");
  const AbValue *discovery = ab_value_member(context->map, "discovery");
  ArchbirdStatus status;
  if (!evidence || evidence->kind != AB_VALUE_OBJECT || !project ||
      project->kind != AB_VALUE_STRING || !source_tool ||
      source_tool->kind != AB_VALUE_OBJECT || !indexes ||
      indexes->kind != AB_VALUE_ARRAY)
    return query_error(context, "map identity fields are invalid");
  status = ab_buffer_literal(buffer, "{\"artifact\":\"query\",\"artifacts\":");
  if (status == ARCHBIRD_OK)
    status = render_artifacts(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"builds\":");
  if (status == ARCHBIRD_OK)
    status = render_builds(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"call_resolutions\":");
  if (status == ARCHBIRD_OK)
    status = render_selected_resolutions(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"components\":");
  if (status == ARCHBIRD_OK)
    status = render_component_names(buffer, context);
  if (status == ARCHBIRD_OK && discovery) {
    status = ab_buffer_literal(buffer, ",\"discovery\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, discovery);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"edges\":");
  if (status == ARCHBIRD_OK)
    status = render_selected_edges(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"evidence\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, evidence);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"files\":");
  if (status == ARCHBIRD_OK)
    status = render_file_rows(buffer, context, order, order_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"indexes\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, indexes);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"matched_symbols\":");
  if (status == ARCHBIRD_OK)
    status = render_matched_symbols(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"packages\":");
  if (status == ARCHBIRD_OK)
    status = render_packages(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"query\":");
  if (status == ARCHBIRD_OK)
    status = render_query_metadata(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer, ",\"schema_version\":" ARCHBIRD_MAP_SCHEMA_CURRENT_TEXT
                ",\"source_tool\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, source_tool);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"surfaces\":");
  if (status == ARCHBIRD_OK)
    status = render_surfaces(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbol_calls\":");
  if (status == ARCHBIRD_OK)
    status = render_selected_symbol_calls(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"symbol_references\":");
  if (status == ARCHBIRD_OK)
    status = render_selected_symbol_references(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"test_matches\":");
  if (status == ARCHBIRD_OK)
    status = render_test_matches(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"tests\":");
  if (status == ARCHBIRD_OK)
    status = render_tests(buffer, context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        buffer,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  return status;
}

ArchbirdStatus archbird_map_query(ArchbirdEngine *engine,
                                  const uint8_t *map_json, size_t map_length,
                                  const uint8_t *query_json,
                                  size_t query_length, uint32_t json_flags,
                                  ArchbirdWriteFn write_fn, void *user_data) {
  AbValue map = {0};
  AbValue request = {0};
  QueryContext context = {0};
  QueryOrder *order = NULL;
  size_t order_count = 0;
  AbBuffer buffer;
  ArchbirdStatus status;
  uint64_t schema;
  if (!engine || (!map_json && map_length) || (!query_json && query_length) ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  context.engine = engine;
  ab_buffer_init(&buffer, engine);
  status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, query_json, query_length, &request);
  context.map = &map;
  if (status == ARCHBIRD_OK &&
      (map.kind != AB_VALUE_OBJECT ||
       !ab_value_string_is(ab_value_member(&map, "artifact"), "map") ||
       !ab_value_u64(ab_value_member(&map, "schema_version"), &schema) ||
       schema < ARCHBIRD_MAP_SCHEMA_MIN ||
       schema > ARCHBIRD_MAP_SCHEMA_CURRENT))
    status =
        query_error(&context, "query input must be an Archbird map "
                              "schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  if (status == ARCHBIRD_OK)
    status = decode_request(&context, &request);
  if (status == ARCHBIRD_OK)
    status = validate_producer_policy(&context);
  if (status == ARCHBIRD_OK)
    status = build_files(&context);
  if (status == ARCHBIRD_OK)
    status = build_edges(&context);
  if (status == ARCHBIRD_OK)
    status = build_symbol_calls(&context);
  if (status == ARCHBIRD_OK)
    status = build_symbol_references(&context);
  if (status == ARCHBIRD_OK)
    status = select_initial(&context);
  if (status == ARCHBIRD_OK)
    status = select_contained_symbols(&context);
  if (status == ARCHBIRD_OK)
    status = build_test_rows(&context);
  if (status == ARCHBIRD_OK && !any_initial_match(&context) &&
      !context.has_focused_tests)
    status =
        query_error(&context, "query selectors matched no indexed evidence");
  if (status == ARCHBIRD_OK)
    status = traverse(&context);
  if (status == ARCHBIRD_OK)
    status = find_test_matches(&context);
  if (status == ARCHBIRD_OK)
    status = finalize_artifacts(&context);
  if (status == ARCHBIRD_OK)
    status = finalize_components_packages(&context);
  if (status == ARCHBIRD_OK)
    status = selected_order(&context, &order, &order_count);
  if (status == ARCHBIRD_OK)
    status = render_query_document(&context, &buffer, order, order_count);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_free(context.engine, order);
  query_context_free(&context);
  ab_value_free(engine, &request);
  ab_value_free(engine, &map);
  ab_buffer_free(&buffer);
  return status;
}
