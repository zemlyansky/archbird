#include "map_reports.h"

#include "archbird_internal.h"
#include "report_utils.h"

#include <stdlib.h>
#include <string.h>

#define MAP_REPORT_WIDTH 116u
#define MAP_REPORT_DEFAULT_BUDGET 30000u

static const char *const map_resolution_kinds[] = {
    "unique", "candidate", "ambiguous", "unresolved",
    "method", "builtin",   "external"};

typedef struct MapReportFile {
  const AbValue *row;
  const AbString *path;
  const AbString *layer;
  const AbString *language;
  const AbValue *symbols;
  const AbValue *roles;
  size_t bytes;
  size_t degree;
  size_t public_symbols;
  size_t rank;
  int generated_candidate;
  int third_party_candidate;
  int test_candidate;
  int collapsed_candidate;
  AbReportStringList incoming;
} MapReportFile;

typedef struct MapReportEdge {
  const AbValue *row;
  const AbString *kind;
  const AbString *source;
  const AbString *target;
  const AbValue *names;
  size_t source_index;
  size_t target_index;
} MapReportEdge;

typedef struct MapReportContext {
  ArchbirdEngine *engine;
  const AbValue *map;
  const AbValue *layers;
  const AbValue *surfaces;
  const AbValue *components;
  const AbValue *packages;
  const AbValue *builds;
  const AbValue *indexes;
  const AbValue *artifacts;
  const AbValue *resolutions;
  const AbValue *tests;
  const AbValue *parity;
  const AbValue *named_entries;
  const AbValue *diagnostics;
  const AbValue *discovery;
  MapReportFile *files;
  size_t file_count;
  MapReportEdge *edges;
  size_t edge_count;
  MapReportFile **ranked;
  size_t compact_file_count;
  size_t generated_candidate_count;
  size_t third_party_candidate_count;
  size_t test_candidate_count;
  size_t compact_symbols;
  size_t compact_edges;
} MapReportContext;

typedef struct MapCountedName {
  const AbString *name;
  size_t count;
  size_t order;
} MapCountedName;

static ArchbirdStatus map_schema_error(ArchbirdEngine *engine,
                                       const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static const AbValue *map_optional_array(const AbValue *object,
                                         const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_ARRAY ? value : &empty;
}

static const AbValue *map_optional_object(const AbValue *object,
                                          const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_OBJECT};
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_OBJECT ? value : &empty;
}

static const AbString *map_optional_string(const AbValue *object,
                                           const char *name,
                                           const AbString *fallback) {
  const AbValue *value = ab_value_member(object, name);
  if (!value)
    return fallback;
  return value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

static const AbString *map_string_or_empty(const AbValue *object,
                                           const char *name) {
  static char empty_data[] = "";
  static const AbString empty = {empty_data, 0};
  return map_optional_string(object, name, &empty);
}

static int map_string_array_valid(const AbValue *array) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++)
    if (array->as.array.items[index].kind != AB_VALUE_STRING)
      return 0;
  return 1;
}

static int map_file_compare(const void *left_raw, const void *right_raw) {
  const MapReportFile *left = (const MapReportFile *)left_raw;
  const MapReportFile *right = (const MapReportFile *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static size_t map_find_file(const MapReportContext *context,
                            const AbString *path) {
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

static size_t map_symbol_priority(const AbString *scope) {
  if (ab_report_string_equal(scope, "public"))
    return 0;
  if (ab_report_string_equal(scope, "global") ||
      ab_report_string_equal(scope, "class"))
    return 1;
  if (ab_report_string_equal(scope, "function"))
    return 2;
  if (ab_report_string_equal(scope, "method"))
    return 3;
  if (ab_report_string_equal(scope, "local"))
    return 4;
  return 5;
}

static int map_public_scope(const AbString *scope) {
  return ab_report_string_equal(scope, "public") ||
         ab_report_string_equal(scope, "global") ||
         ab_report_string_equal(scope, "class") ||
         ab_report_string_equal(scope, "function");
}

static int map_rank_compare(const void *left_raw, const void *right_raw) {
  const MapReportFile *left = *(MapReportFile *const *)left_raw;
  const MapReportFile *right = *(MapReportFile *const *)right_raw;
  if (left->collapsed_candidate != right->collapsed_candidate)
    return left->collapsed_candidate ? 1 : -1;
  if (left->degree != right->degree)
    return left->degree > right->degree ? -1 : 1;
  if (left->incoming.count != right->incoming.count)
    return left->incoming.count > right->incoming.count ? -1 : 1;
  if (left->public_symbols != right->public_symbols)
    return left->public_symbols > right->public_symbols ? -1 : 1;
  return ab_string_compare(left->path, right->path);
}

static int map_has_role(const AbValue *roles, const char *wanted) {
  size_t index;
  if (!roles || roles->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < roles->as.array.count; index++)
    if (ab_value_string_is(&roles->as.array.items[index], wanted))
      return 1;
  return 0;
}

static void map_context_free(MapReportContext *context) {
  size_t index;
  if (!context)
    return;
  if (context->files)
    for (index = 0; index < context->file_count; index++)
      ab_report_list_free(&context->files[index].incoming);
  ab_free(context->engine, context->ranked);
  ab_free(context->engine, context->edges);
  ab_free(context->engine, context->files);
  memset(context, 0, sizeof(*context));
}

static ArchbirdStatus map_context_build(MapReportContext *context,
                                        ArchbirdEngine *engine,
                                        const AbValue *map) {
  const AbValue *files;
  const AbValue *edges;
  const AbValue *limits;
  size_t index;
  uint64_t schema;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(context, 0, sizeof(*context));
  context->engine = engine;
  context->map = map;
  if (!map || map->kind != AB_VALUE_OBJECT ||
      !ab_value_string_is(ab_value_member(map, "artifact"), "map") ||
      !ab_value_u64(ab_value_member(map, "schema_version"), &schema) ||
      schema < ARCHBIRD_MAP_SCHEMA_MIN || schema > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return map_schema_error(engine,
                            "map report input must be an Archbird map "
                            "schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  files = ab_value_member(map, "files");
  edges = ab_value_member(map, "edges");
  if (!files || files->kind != AB_VALUE_ARRAY || !edges ||
      edges->kind != AB_VALUE_ARRAY)
    return map_schema_error(engine, "map files and edges must be arrays");
  context->layers = map_optional_array(map, "layers");
  context->surfaces = map_optional_array(map, "surfaces");
  context->components = map_optional_array(map, "components");
  context->packages = map_optional_array(map, "packages");
  context->builds = map_optional_array(map, "builds");
  context->indexes = map_optional_array(map, "indexes");
  context->artifacts = map_optional_array(map, "artifacts");
  context->resolutions = map_optional_array(map, "call_resolutions");
  context->tests = map_optional_array(map, "tests");
  context->parity = map_optional_array(map, "parity");
  context->named_entries = map_optional_object(map, "named_entries");
  context->diagnostics = map_optional_array(map, "diagnostics");
  context->discovery = map_optional_object(map, "discovery");
  limits = map_optional_object(map, "limits");
  context->compact_symbols = ab_report_size(limits, "compact_symbols", 10);
  context->compact_edges = ab_report_size(limits, "compact_edge_names", 12);
  if (!context->compact_symbols || !context->compact_edges)
    return map_schema_error(engine, "map compact limits must be positive");
  context->file_count = files->as.array.count;
  if (context->file_count) {
    context->files = (MapReportFile *)ab_calloc(engine, context->file_count,
                                                sizeof(*context->files));
    context->ranked = (MapReportFile **)ab_calloc(engine, context->file_count,
                                                  sizeof(*context->ranked));
    if (!context->files || !context->ranked) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory indexing map report files");
      goto cleanup;
    }
  }
  for (index = 0; index < context->file_count; index++) {
    MapReportFile *file = &context->files[index];
    size_t symbol_index;
    file->row = &files->as.array.items[index];
    file->path = ab_report_string(file->row, "path");
    file->layer = ab_report_string(file->row, "layer");
    file->language = ab_report_string(file->row, "language");
    file->symbols = ab_report_array(file->row, "symbols");
    file->roles = map_optional_array(file->row, "roles");
    file->bytes = ab_report_size(file->row, "bytes", SIZE_MAX);
    ab_report_list_init(&file->incoming, engine);
    if (file->row->kind != AB_VALUE_OBJECT || !file->path || !file->layer ||
        !file->language || !file->symbols ||
        !map_string_array_valid(file->roles) || file->bytes == SIZE_MAX) {
      status = map_schema_error(engine, "map.files[] is malformed");
      goto cleanup;
    }
    file->generated_candidate =
        map_has_role(file->roles, "generated-candidate");
    file->third_party_candidate =
        map_has_role(file->roles, "third-party-candidate");
    file->test_candidate = map_has_role(file->roles, "test-candidate");
    file->collapsed_candidate =
        file->generated_candidate || file->third_party_candidate;
    context->generated_candidate_count += file->generated_candidate != 0;
    context->third_party_candidate_count += file->third_party_candidate != 0;
    context->test_candidate_count += file->test_candidate != 0;
    context->compact_file_count += file->collapsed_candidate == 0;
    for (symbol_index = 0; symbol_index < file->symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &file->symbols->as.array.items[symbol_index];
      const AbString *scope = map_string_or_empty(symbol, "scope");
      if (!scope) {
        status = map_schema_error(engine, "map.files[].symbols[] is malformed");
        goto cleanup;
      }
      if (map_public_scope(scope))
        file->public_symbols++;
    }
  }
  if (context->file_count > 1)
    qsort(context->files, context->file_count, sizeof(*context->files),
          map_file_compare);
  for (index = 1; index < context->file_count; index++)
    if (ab_string_equal(context->files[index - 1].path,
                        context->files[index].path)) {
      status = map_schema_error(engine, "map.files contains duplicate paths");
      goto cleanup;
    }
  context->edge_count = edges->as.array.count;
  if (context->edge_count) {
    context->edges = (MapReportEdge *)ab_calloc(engine, context->edge_count,
                                                sizeof(*context->edges));
    if (!context->edges) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory indexing map report edges");
      goto cleanup;
    }
  }
  for (index = 0; index < context->edge_count; index++) {
    MapReportEdge *edge = &context->edges[index];
    size_t name_index;
    edge->row = &edges->as.array.items[index];
    edge->kind = ab_report_string(edge->row, "kind");
    edge->source = ab_report_string(edge->row, "source");
    edge->target = ab_report_string(edge->row, "target");
    edge->names = ab_report_array(edge->row, "names");
    if (!edge->kind || !edge->source || !edge->target || !edge->names ||
        !map_string_array_valid(edge->names)) {
      status = map_schema_error(engine, "map.edges[] is malformed");
      goto cleanup;
    }
    edge->source_index = map_find_file(context, edge->source);
    edge->target_index = map_find_file(context, edge->target);
    if (edge->source_index != SIZE_MAX)
      context->files[edge->source_index].degree++;
    if (edge->target_index != SIZE_MAX) {
      MapReportFile *target = &context->files[edge->target_index];
      context->files[edge->target_index].degree++;
      for (name_index = 0; name_index < edge->names->as.array.count;
           name_index++) {
        const AbString *name = &edge->names->as.array.items[name_index].as.text;
        status =
            ab_report_list_add(&target->incoming, name->data, name->length);
        if (status != ARCHBIRD_OK)
          goto cleanup;
      }
    }
  }
  for (index = 0; index < context->file_count; index++) {
    ab_report_list_sort_unique(&context->files[index].incoming);
    context->ranked[index] = &context->files[index];
  }
  if (context->file_count > 1)
    qsort(context->ranked, context->file_count, sizeof(*context->ranked),
          map_rank_compare);
  for (index = 0; index < context->file_count; index++)
    context->ranked[index]->rank = index;
  return ARCHBIRD_OK;

cleanup:
  map_context_free(context);
  return status;
}

static int map_string_starts(const AbString *value, const char *prefix) {
  size_t length = strlen(prefix);
  return value && value->length >= length &&
         memcmp(value->data, prefix, length) == 0;
}

static int map_symbol_incoming(const MapReportFile *file,
                               const AbString *name) {
  const char *leaf = name->data;
  size_t leaf_length = name->length;
  size_t index;
  for (index = 0; index < name->length; index++)
    if (name->data[index] == '.') {
      leaf = name->data + index + 1;
      leaf_length = name->length - index - 1;
    }
  for (index = 0; index < file->incoming.count; index++)
    if (file->incoming.items[index].length == leaf_length &&
        (!leaf_length ||
         memcmp(file->incoming.items[index].data, leaf, leaf_length) == 0))
      return 1;
  return 0;
}

typedef struct MapReportSymbol {
  const AbString *name;
  const AbString *scope;
  const AbString *signature;
  size_t line;
  size_t priority;
  int incoming;
} MapReportSymbol;

static int map_symbol_compare(const void *left_raw, const void *right_raw) {
  const MapReportSymbol *left = (const MapReportSymbol *)left_raw;
  const MapReportSymbol *right = (const MapReportSymbol *)right_raw;
  int compared;
  if (left->incoming != right->incoming)
    return left->incoming ? -1 : 1;
  if (left->priority != right->priority)
    return left->priority < right->priority ? -1 : 1;
  compared = ab_string_compare(left->name, right->name);
  if (compared)
    return compared;
  if (left->line != right->line)
    return left->line < right->line ? -1 : 1;
  return 0;
}

static ArchbirdStatus map_symbol_items(const MapReportContext *context,
                                       const MapReportFile *file, int full,
                                       AbReportStringList *out) {
  size_t count = file->symbols->as.array.count;
  MapReportSymbol *symbols = NULL;
  size_t selected;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (count) {
    symbols =
        (MapReportSymbol *)ab_calloc(context->engine, count, sizeof(*symbols));
    if (!symbols)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory ranking map report symbols");
  }
  for (index = 0; index < count; index++) {
    const AbValue *row = &file->symbols->as.array.items[index];
    symbols[index].name = ab_report_string(row, "name");
    symbols[index].scope = map_string_or_empty(row, "scope");
    symbols[index].signature = map_string_or_empty(row, "signature");
    symbols[index].line = ab_report_size(row, "line", 0);
    if (!symbols[index].name || !symbols[index].scope ||
        !symbols[index].signature) {
      status = map_schema_error(context->engine,
                                "map.files[].symbols[] is malformed");
      goto cleanup;
    }
    symbols[index].priority = map_symbol_priority(symbols[index].scope);
    symbols[index].incoming = map_symbol_incoming(file, symbols[index].name);
  }
  if (count > 1)
    qsort(symbols, count, sizeof(*symbols), map_symbol_compare);
  selected = full || count < context->compact_symbols
                 ? count
                 : context->compact_symbols;
  for (index = 0; status == ARCHBIRD_OK && index < selected; index++) {
    if (full && symbols[index].signature->length &&
        !ab_string_equal(symbols[index].signature, symbols[index].name))
      status = ab_report_list_addf(
          out, "%.*s@%zu=%.*s", (int)symbols[index].name->length,
          symbols[index].name->data, symbols[index].line,
          (int)symbols[index].signature->length,
          symbols[index].signature->data);
    else
      status =
          ab_report_list_addf(out, "%.*s@%zu", (int)symbols[index].name->length,
                              symbols[index].name->data, symbols[index].line);
  }
  if (status == ARCHBIRD_OK && count > selected)
    status = ab_report_list_addf(out, "…+%zu", count - selected);
cleanup:
  ab_free(context->engine, symbols);
  return status;
}

static ArchbirdStatus map_add_array_strings(AbReportStringList *list,
                                            const AbValue *array) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_OK;
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *item = &array->as.array.items[index];
    ArchbirdStatus status;
    if (item->kind != AB_VALUE_STRING)
      return map_schema_error(list->engine,
                              "report string array contains non-string");
    status = ab_report_list_add(list, item->as.text.data, item->as.text.length);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_limited_values(ArchbirdEngine *engine,
                                         const AbValue *array, size_t limit,
                                         AbReportStringList *out) {
  AbReportStringList values;
  size_t index;
  ArchbirdStatus status;
  ab_report_list_init(&values, engine);
  status = map_add_array_strings(&values, array);
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(&values);
  for (index = 0;
       status == ARCHBIRD_OK && index < values.count && index < limit; index++)
    status = ab_report_list_add(out, values.items[index].data,
                                values.items[index].length);
  if (status == ARCHBIRD_OK && values.count > limit)
    status = ab_report_list_addf(out, "…+%zu", values.count - limit);
  ab_report_list_free(&values);
  return status;
}

static ArchbirdStatus map_tool_label(AbBuffer *buffer, const AbValue *tool) {
  static char default_name_data[] = "archbird";
  static const AbString default_name = {default_name_data, 8};
  const AbString *name = map_optional_string(tool, "name", &default_name);
  const AbString *version = ab_report_string(tool, "version");
  const AbString *digest = ab_report_string(tool, "implementation_sha256");
  if (!name || !version || !digest || digest->length < 16)
    return map_schema_error(buffer->engine, "map tool identity is malformed");
  return ab_report_appendf(buffer, "%.*s %.*s `%.*s`", (int)name->length,
                           name->data, (int)version->length, version->data, 16,
                           digest->data);
}

static size_t map_surface_summary(const AbValue *surface, const char *field) {
  const AbValue *summary = map_optional_object(surface, "summary");
  const AbValue *names;
  size_t index;
  size_t count = 0;
  if (summary->as.object.count)
    return ab_report_size(summary, field, 0);
  names = map_optional_array(surface, "names");
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbString *declaration = ab_report_string(row, "declaration");
    const AbString *resolution = ab_report_string(row, "resolution");
    const AbValue *uses = map_optional_array(row, "uses");
    int ignored = ab_report_bool(row, "ignored", 0);
    if (strcmp(field, "ignored") == 0) {
      if (ignored)
        count++;
      continue;
    }
    if (ignored)
      continue;
    if (strcmp(field, "registered") == 0 &&
        ab_report_string_equal(declaration, "declared"))
      count++;
    else if (strcmp(field, "used") == 0 && uses->as.array.count)
      count++;
    else if (strcmp(field, "unused") == 0 &&
             ab_report_string_equal(declaration, "declared") &&
             !uses->as.array.count)
      count++;
    else if (strcmp(field, "unregistered_use") == 0 &&
             ab_report_string_equal(declaration, "undeclared") &&
             uses->as.array.count)
      count++;
    else if (resolution && ((strcmp(field, "resolved") == 0 &&
                             ab_report_string_equal(resolution, "unique")) ||
                            ab_report_string_equal(resolution, field)))
      count++;
  }
  return count;
}

static int map_counted_compare(const void *left_raw, const void *right_raw) {
  const MapCountedName *left = (const MapCountedName *)left_raw;
  const MapCountedName *right = (const MapCountedName *)right_raw;
  if (left->count != right->count)
    return left->count > right->count ? -1 : 1;
  return ab_string_compare(left->name, right->name);
}

static int map_counted_name_compare(const void *left_raw,
                                    const void *right_raw) {
  const MapCountedName *left = (const MapCountedName *)left_raw;
  const MapCountedName *right = (const MapCountedName *)right_raw;
  return ab_string_compare(left->name, right->name);
}

static int map_hub_compare(const void *left_raw, const void *right_raw) {
  const MapCountedName *left = (const MapCountedName *)left_raw;
  const MapCountedName *right = (const MapCountedName *)right_raw;
  if (left->count != right->count)
    return left->count > right->count ? -1 : 1;
  if (left->order != right->order)
    return left->order < right->order ? -1 : 1;
  return 0;
}

static int map_list_contains(const AbReportStringList *list,
                             const AbString *value) {
  size_t index;
  for (index = 0; index < list->count; index++)
    if (ab_string_equal(&list->items[index], value))
      return 1;
  return 0;
}

#define MAP_REPORT_TRY(expression)                                             \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus map_render_identity(const MapReportContext *context,
                                          AbBuffer *out) {
  const AbString *project = ab_report_string(context->map, "project");
  const AbString *description = ab_report_string(context->map, "description");
  const AbValue *tool = ab_report_object(context->map, "tool");
  const AbValue *evidence = ab_report_object(context->map, "evidence");
  const AbString *config = ab_report_string(evidence, "config_sha256");
  const AbString *inputs = ab_report_string(evidence, "input_sha256");
  size_t symbol_count = 0;
  size_t errors = 0;
  size_t warnings = 0;
  size_t index;
  if (!project || !tool || !evidence || !config || !inputs ||
      config->length < 16 || inputs->length < 16)
    return map_schema_error(context->engine,
                            "map report identity/evidence is malformed");
  for (index = 0; index < context->file_count; index++)
    symbol_count += context->files[index].symbols->as.array.count;
  for (index = 0; index < context->diagnostics->as.array.count; index++) {
    const AbString *severity = ab_report_string(
        &context->diagnostics->as.array.items[index], "severity");
    if (ab_report_string_equal(severity, "error"))
      errors++;
    else if (ab_report_string_equal(severity, "warning"))
      warnings++;
  }
  MAP_REPORT_TRY(ab_report_linef(out, "# %.*s deterministic architecture map",
                                 (int)project->length, project->data));
  MAP_REPORT_TRY(ab_report_blank(out));
  if (description && description->length)
    MAP_REPORT_TRY(ab_report_line(out, description->data, description->length));
  else
    MAP_REPORT_TRY(ab_report_literal_line(
        out, "Source-derived repository structure and connections."));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_buffer_literal(out, "Evidence: tool "));
  MAP_REPORT_TRY(map_tool_label(out, tool));
  MAP_REPORT_TRY(ab_report_linef(
      out,
      "; config `%.*s`; inputs `%.*s`; %zu mapped source files; %zu "
      "symbols; %zu errors; %zu warnings.",
      16, config->data, 16, inputs->data, context->file_count, symbol_count,
      errors, warnings));
  MAP_REPORT_TRY(ab_report_blank(out));
  if (context->discovery->as.object.count) {
    const AbValue *coverage = ab_report_object(context->discovery, "coverage");
    const AbValue *profile = ab_report_object(context->discovery, "profile");
    const AbString *profile_name = ab_report_string(profile, "name");
    const AbString *profile_digest =
        ab_report_string(profile, "implementation_sha256");
    const AbString *resolution_digest =
        ab_report_string(context->discovery, "sha256");
    size_t assets = ab_report_size(coverage, "assets", SIZE_MAX);
    size_t ignored = ab_report_size(coverage, "ignored", SIZE_MAX);
    size_t inventory = ab_report_size(coverage, "inventory_files", SIZE_MAX);
    size_t oversized = ab_report_size(coverage, "oversized", SIZE_MAX);
    size_t pruned = ab_report_size(coverage, "pruned_directories", SIZE_MAX);
    size_t selected = ab_report_size(coverage, "selected", SIZE_MAX);
    size_t unsupported =
        ab_report_size(coverage, "unsupported_known", SIZE_MAX);
    if (!profile_name || !profile_digest || profile_digest->length < 16 ||
        !resolution_digest || resolution_digest->length < 16 ||
        assets == SIZE_MAX || ignored == SIZE_MAX || inventory == SIZE_MAX ||
        oversized == SIZE_MAX || pruned == SIZE_MAX || selected == SIZE_MAX ||
        unsupported == SIZE_MAX)
      return map_schema_error(context->engine,
                              "map discovery evidence is malformed");
    MAP_REPORT_TRY(ab_report_linef(
        out,
        "Discovery: %.*s `%.*s`; resolution `%.*s`; inventory=%zu; "
        "selected=%zu; mapped=%zu; unsupported-known=%zu; assets=%zu; "
        "ignored=%zu; oversized=%zu; pruned-directories=%zu.",
        (int)profile_name->length, profile_name->data, 16, profile_digest->data,
        16, resolution_digest->data, inventory, selected, context->file_count,
        unsupported, assets, ignored, oversized, pruned));
    MAP_REPORT_TRY(ab_report_blank(out));
    if (unsupported) {
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "Coverage warning: %zu known source files use unsupported language "
          "extensions and are not represented by source facts.",
          unsupported));
      MAP_REPORT_TRY(ab_report_blank(out));
    }
  }
  MAP_REPORT_TRY(ab_report_literal_line(
      out,
      "Static evidence only: dynamic dispatch and ambiguous calls are not "
      "guessed. Paths are repository-relative; timestamps, absolute paths, "
      "git state, generated host probes, and model output are absent."));
  return ab_report_blank(out);
}

static ArchbirdStatus map_render_layers(const MapReportContext *context,
                                        AbBuffer *out) {
  size_t index;
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Layer inventory"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->layers->as.array.count; index++) {
    const AbValue *layer = &context->layers->as.array.items[index];
    const AbString *name = ab_report_string(layer, "name");
    const AbString *role = ab_report_string(layer, "role");
    const AbString *language = ab_report_string(layer, "language");
    size_t file_count = 0;
    size_t layer_symbols = 0;
    size_t bytes = 0;
    size_t file_index;
    if (!name || !role || !language)
      return map_schema_error(context->engine, "map.layers[] is malformed");
    for (file_index = 0; file_index < context->file_count; file_index++) {
      const MapReportFile *file = &context->files[file_index];
      if (!ab_string_equal(file->layer, name))
        continue;
      file_count++;
      layer_symbols += file->symbols->as.array.count;
      bytes += file->bytes;
    }
    MAP_REPORT_TRY(ab_report_linef(
        out, "%.*s role=%.*s language=%.*s files=%zu symbols=%zu bytes=%zu",
        (int)name->length, name->data, (int)role->length, role->data,
        (int)language->length, language->data, file_count, layer_symbols,
        bytes));
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_header(const MapReportContext *context,
                                        AbBuffer *out) {
  MAP_REPORT_TRY(map_render_identity(context, out));
  return map_render_layers(context, out);
}

static ArchbirdStatus
map_render_surface_categories(const MapReportContext *context,
                              const AbValue *surface, AbBuffer *out) {
  static const char *const labels[] = {"unused", "unregistered", "unresolved",
                                       "ambiguous"};
  const AbValue *names = map_optional_array(surface, "names");
  size_t label_index;
  for (label_index = 0; label_index < 4; label_index++) {
    AbReportStringList values;
    size_t index;
    ArchbirdStatus status = ARCHBIRD_OK;
    ab_report_list_init(&values, context->engine);
    for (index = 0; index < names->as.array.count; index++) {
      const AbValue *row = &names->as.array.items[index];
      const AbString *name = ab_report_string(row, "name");
      const AbString *declaration = ab_report_string(row, "declaration");
      const AbString *resolution = ab_report_string(row, "resolution");
      const AbValue *uses = map_optional_array(row, "uses");
      int include = 0;
      if (!name || !declaration || !resolution) {
        status = map_schema_error(context->engine,
                                  "map.surfaces[].names[] is malformed");
        break;
      }
      if (ab_report_bool(row, "ignored", 0))
        continue;
      if (label_index == 0)
        include = ab_report_string_equal(declaration, "declared") &&
                  !uses->as.array.count;
      else if (label_index == 1)
        include = ab_report_string_equal(declaration, "undeclared") &&
                  uses->as.array.count;
      else
        include = ab_report_string_equal(resolution, labels[label_index]);
      if (include) {
        status = ab_report_list_add(&values, name->data, name->length);
        if (status != ARCHBIRD_OK)
          break;
      }
    }
    if (status == ARCHBIRD_OK)
      ab_report_list_sort_unique(&values);
    if (status == ARCHBIRD_OK && values.count) {
      AbReportStringList limited;
      size_t shown = values.count < context->compact_edges
                         ? values.count
                         : context->compact_edges;
      ab_report_list_init(&limited, context->engine);
      for (index = 0; status == ARCHBIRD_OK && index < shown; index++)
        status = ab_report_list_add(&limited, values.items[index].data,
                                    values.items[index].length);
      if (status == ARCHBIRD_OK && values.count > shown)
        status = ab_report_list_addf(&limited, "…+%zu", values.count - shown);
      if (status == ARCHBIRD_OK) {
        AbBuffer prefix;
        ab_buffer_init(&prefix, context->engine);
        status = ab_report_appendf(&prefix, "  %s: ", labels[label_index]);
        if (status == ARCHBIRD_OK)
          status = ab_report_chunks(out, &limited, (const char *)prefix.data,
                                    MAP_REPORT_WIDTH);
        ab_buffer_free(&prefix);
      }
      ab_report_list_free(&limited);
    }
    ab_report_list_free(&values);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_surface_full(const MapReportContext *context,
                                              const AbValue *surface,
                                              AbBuffer *out) {
  const AbValue *names = map_optional_array(surface, "names");
  size_t index;
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbString *name = ab_report_string(row, "name");
    const AbString *declaration = ab_report_string(row, "declaration");
    const AbString *resolution = ab_report_string(row, "resolution");
    const AbValue *uses = map_optional_array(row, "uses");
    AbReportStringList evidence;
    AbBuffer prefix;
    size_t use_count = 0;
    size_t item_index;
    ArchbirdStatus status = ARCHBIRD_OK;
    if (!name || !declaration || !resolution)
      return map_schema_error(context->engine,
                              "map.surfaces[].names[] is malformed");
    for (item_index = 0; item_index < uses->as.array.count; item_index++)
      use_count +=
          ab_report_size(&uses->as.array.items[item_index], "count", 0);
    ab_report_list_init(&evidence, context->engine);
    ab_buffer_init(&prefix, context->engine);
    {
      const AbValue *declarations = map_optional_array(row, "declarations");
      const AbValue *candidates = map_optional_array(row, "candidates");
      const AbValue *declaration_signatures =
          map_optional_array(row, "declaration_signatures");
      const AbValue *implementation_signatures =
          map_optional_array(row, "implementation_signatures");
      for (item_index = 0;
           status == ARCHBIRD_OK && item_index < declarations->as.array.count;
           item_index++) {
        const AbValue *item = &declarations->as.array.items[item_index];
        const AbString *path = ab_report_string(item, "path");
        const AbString *source = ab_report_string(item, "source");
        if (!path || !source)
          status = map_schema_error(context->engine,
                                    "surface declaration is malformed");
        else
          status = ab_report_list_addf(&evidence, "decl:%.*s:%.*s",
                                       (int)path->length, path->data,
                                       (int)source->length, source->data);
      }
      for (item_index = 0;
           status == ARCHBIRD_OK && item_index < uses->as.array.count;
           item_index++) {
        const AbValue *item = &uses->as.array.items[item_index];
        const AbString *path = ab_report_string(item, "path");
        if (!path)
          status =
              map_schema_error(context->engine, "surface use is malformed");
        else
          status =
              ab_report_list_addf(&evidence, "use:%.*s(%zu)", (int)path->length,
                                  path->data, ab_report_size(item, "count", 0));
      }
      for (item_index = 0;
           status == ARCHBIRD_OK && item_index < candidates->as.array.count;
           item_index++) {
        const AbValue *item = &candidates->as.array.items[item_index];
        if (item->kind != AB_VALUE_STRING)
          status = map_schema_error(context->engine,
                                    "surface candidate is malformed");
        else
          status = ab_report_list_addf(&evidence, "candidate:%.*s",
                                       (int)item->as.text.length,
                                       item->as.text.data);
      }
      for (item_index = 0; status == ARCHBIRD_OK &&
                           item_index < declaration_signatures->as.array.count;
           item_index++) {
        const AbValue *item =
            &declaration_signatures->as.array.items[item_index];
        if (item->kind != AB_VALUE_STRING)
          status = map_schema_error(context->engine,
                                    "surface signature is malformed");
        else
          status = ab_report_list_addf(&evidence, "declaration-signature:%.*s",
                                       (int)item->as.text.length,
                                       item->as.text.data);
      }
      for (item_index = 0;
           status == ARCHBIRD_OK &&
           item_index < implementation_signatures->as.array.count;
           item_index++) {
        const AbValue *item =
            &implementation_signatures->as.array.items[item_index];
        if (item->kind != AB_VALUE_STRING)
          status = map_schema_error(context->engine,
                                    "surface signature is malformed");
        else
          status = ab_report_list_addf(
              &evidence, "implementation-signature:%.*s",
              (int)item->as.text.length, item->as.text.data);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(
          &prefix, "  %.*s %.*s %s resolution=%.*s%s: ", (int)name->length,
          name->data, (int)declaration->length, declaration->data,
          uses->as.array.count ? "used=" : "unused", (int)resolution->length,
          resolution->data,
          ab_report_bool(row, "ignored", 0) ? " ignored" : "");
    /* The used count is embedded after `used=` rather than as another token. */
    if (status == ARCHBIRD_OK && uses->as.array.count) {
      ab_buffer_free(&prefix);
      ab_buffer_init(&prefix, context->engine);
      status = ab_report_appendf(
          &prefix,
          "  %.*s %.*s used=%zu resolution=%.*s%s: ", (int)name->length,
          name->data, (int)declaration->length, declaration->data, use_count,
          (int)resolution->length, resolution->data,
          ab_report_bool(row, "ignored", 0) ? " ignored" : "");
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &evidence, (const char *)prefix.data,
                                MAP_REPORT_WIDTH);
    ab_buffer_free(&prefix);
    ab_report_list_free(&evidence);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_surfaces(const MapReportContext *context,
                                          int full, AbBuffer *out) {
  size_t index;
  if (!context->surfaces->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Provider surfaces"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->surfaces->as.array.count; index++) {
    const AbValue *surface = &context->surfaces->as.array.items[index];
    const AbString *name = ab_report_string(surface, "name");
    const AbString *kind = ab_report_string(surface, "kind");
    const AbValue *providers = map_optional_array(surface, "providers");
    const AbValue *names = map_optional_array(surface, "names");
    int configured = ab_report_bool(surface, "provider_configured", 0);
    size_t provider_index;
    if (!name || !kind)
      return map_schema_error(context->engine, "map.surfaces[] is malformed");
    if (configured)
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "%.*s kind=%.*s providers=%zu names=%zu registered=%zu used=%zu "
          "unused=%zu unregistered=%zu resolved=%zu unresolved=%zu "
          "ambiguous=%zu ignored=%zu",
          (int)name->length, name->data, (int)kind->length, kind->data,
          providers->as.array.count, names->as.array.count,
          map_surface_summary(surface, "registered"),
          map_surface_summary(surface, "used"),
          map_surface_summary(surface, "unused"),
          map_surface_summary(surface, "unregistered_use"),
          map_surface_summary(surface, "resolved"),
          map_surface_summary(surface, "unresolved"),
          map_surface_summary(surface, "ambiguous"),
          map_surface_summary(surface, "ignored")));
    else
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "%.*s kind=%.*s providers=none (declaration status unknown) "
          "names=%zu registered=%zu used=%zu unused=%zu unregistered=%zu "
          "resolved=%zu unresolved=%zu ambiguous=%zu ignored=%zu",
          (int)name->length, name->data, (int)kind->length, kind->data,
          names->as.array.count, map_surface_summary(surface, "registered"),
          map_surface_summary(surface, "used"),
          map_surface_summary(surface, "unused"),
          map_surface_summary(surface, "unregistered_use"),
          map_surface_summary(surface, "resolved"),
          map_surface_summary(surface, "unresolved"),
          map_surface_summary(surface, "ambiguous"),
          map_surface_summary(surface, "ignored")));
    if (providers->as.array.count) {
      AbReportStringList rows;
      ArchbirdStatus status = ARCHBIRD_OK;
      ab_report_list_init(&rows, context->engine);
      for (provider_index = 0;
           status == ARCHBIRD_OK && provider_index < providers->as.array.count;
           provider_index++) {
        const AbValue *provider = &providers->as.array.items[provider_index];
        const AbString *path = ab_report_string(provider, "path");
        const AbString *source = ab_report_string(provider, "source");
        if (!path || !source)
          status = map_schema_error(context->engine,
                                    "surface provider is malformed");
        else
          status = ab_report_list_addf(&rows, "%.*s:%.*s", (int)path->length,
                                       path->data, (int)source->length,
                                       source->data);
      }
      if (status == ARCHBIRD_OK)
        status =
            ab_report_chunks(out, &rows, "  providers: ", MAP_REPORT_WIDTH);
      ab_report_list_free(&rows);
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (full)
      MAP_REPORT_TRY(map_render_surface_full(context, surface, out));
    else
      MAP_REPORT_TRY(map_render_surface_categories(context, surface, out));
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_components(const MapReportContext *context,
                                            int full, AbBuffer *out) {
  size_t index;
  if (!context->components->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Architecture components"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->components->as.array.count; index++) {
    const AbValue *component = &context->components->as.array.items[index];
    const AbString *name = ab_report_string(component, "name");
    const AbString *description = map_string_or_empty(component, "description");
    const AbValue *files = map_optional_array(component, "files");
    const AbValue *outgoing = map_optional_object(component, "outgoing");
    size_t symbols = ab_report_size(component, "symbol_count", SIZE_MAX);
    size_t field_index;
    AbReportStringList routes;
    ArchbirdStatus status = ARCHBIRD_OK;
    if (!name || !description || symbols == SIZE_MAX)
      return map_schema_error(context->engine, "map.components[] is malformed");
    if (description->length)
      MAP_REPORT_TRY(ab_report_linef(
          out, "%.*s: files=%zu symbols=%zu; %.*s", (int)name->length,
          name->data, files->as.array.count, symbols, (int)description->length,
          description->data));
    else
      MAP_REPORT_TRY(ab_report_linef(out, "%.*s: files=%zu symbols=%zu",
                                     (int)name->length, name->data,
                                     files->as.array.count, symbols));
    ab_report_list_init(&routes, context->engine);
    for (field_index = 0;
         status == ARCHBIRD_OK && field_index < outgoing->as.object.count;
         field_index++) {
      const AbObjectField *field = &outgoing->as.object.fields[field_index];
      if (field->value.kind != AB_VALUE_ARRAY)
        status = map_schema_error(context->engine,
                                  "component outgoing routes are malformed");
      else
        status =
            ab_report_list_addf(&routes, "%.*s(%zu)", (int)field->name.length,
                                field->name.data, field->value.as.array.count);
    }
    if (status == ARCHBIRD_OK && routes.count)
      status = ab_report_chunks(out, &routes, "  routes: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&routes);
    if (status != ARCHBIRD_OK)
      return status;
    if (full) {
      AbReportStringList paths;
      ab_report_list_init(&paths, context->engine);
      status = map_add_array_strings(&paths, files);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &paths, "  files: ", MAP_REPORT_WIDTH);
      ab_report_list_free(&paths);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_package(const MapReportContext *context,
                                         const AbValue *package, int full,
                                         AbBuffer *out) {
  const AbString *name = ab_report_string(package, "name");
  const AbString *kind = ab_report_string(package, "kind");
  const AbString *identity = map_string_or_empty(package, "identity");
  const AbString *version = map_string_or_empty(package, "version");
  const AbString *manifest = ab_report_string(package, "manifest");
  const AbValue *entries = map_optional_object(package, "entrypoints");
  const AbValue *dependencies = map_optional_array(package, "dependencies");
  const AbValue *exports = map_optional_array(package, "exports");
  AbReportStringList rows;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  size_t limit = full ? SIZE_MAX : 30;
  if (!name || !kind || !identity || !version || !manifest)
    return map_schema_error(context->engine, "map.packages[] is malformed");
  if (version->length)
    MAP_REPORT_TRY(ab_report_linef(
        out, "package %.*s kind=%.*s identity=%.*s@%.*s manifest=%.*s",
        (int)name->length, name->data, (int)kind->length, kind->data,
        (int)identity->length, identity->data, (int)version->length,
        version->data, (int)manifest->length, manifest->data));
  else
    MAP_REPORT_TRY(ab_report_linef(
        out, "package %.*s kind=%.*s identity=%.*s manifest=%.*s",
        (int)name->length, name->data, (int)kind->length, kind->data,
        (int)identity->length, identity->data, (int)manifest->length,
        manifest->data));
  ab_report_list_init(&rows, context->engine);
  for (index = 0; status == ARCHBIRD_OK && index < entries->as.object.count;
       index++) {
    const AbObjectField *field = &entries->as.object.fields[index];
    if (field->value.kind != AB_VALUE_STRING)
      status =
          map_schema_error(context->engine, "package entrypoint is malformed");
    else
      status = ab_report_list_addf(
          &rows, "%.*s->%.*s", (int)field->name.length, field->name.data,
          (int)field->value.as.text.length, field->value.as.text.data);
  }
  if (status == ARCHBIRD_OK && rows.count > limit) {
    while (rows.count > limit) {
      ab_string_free(context->engine, &rows.items[rows.count - 1]);
      rows.count--;
    }
    status =
        ab_report_list_addf(&rows, "…+%zu", entries->as.object.count - limit);
  }
  if (status == ARCHBIRD_OK)
    status = ab_report_chunks(out, &rows, "  entries: ", MAP_REPORT_WIDTH);
  ab_report_list_free(&rows);
  if (status != ARCHBIRD_OK)
    return status;
  if (dependencies->as.array.count) {
    ab_report_list_init(&rows, context->engine);
    for (index = 0;
         status == ARCHBIRD_OK && index < dependencies->as.array.count;
         index++) {
      const AbValue *dependency = &dependencies->as.array.items[index];
      const AbString *dependency_name = ab_report_string(dependency, "name");
      const AbString *scope = ab_report_string(dependency, "scope");
      if (!dependency_name || !scope)
        status = map_schema_error(context->engine,
                                  "package dependency is malformed");
      else
        status = ab_report_list_addf(
            &rows, "%.*s[%.*s]", (int)dependency_name->length,
            dependency_name->data, (int)scope->length, scope->data);
    }
    if (status == ARCHBIRD_OK && rows.count > limit) {
      size_t omitted = rows.count - limit;
      while (rows.count > limit) {
        ab_string_free(context->engine, &rows.items[rows.count - 1]);
        rows.count--;
      }
      status = ab_report_list_addf(&rows, "…+%zu", omitted);
    }
    if (status == ARCHBIRD_OK)
      status =
          ab_report_chunks(out, &rows, "  dependencies: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&rows);
    if (status != ARCHBIRD_OK)
      return status;
  }
  ab_report_list_init(&rows, context->engine);
  status =
      map_limited_values(context->engine, exports, full ? SIZE_MAX : 40, &rows);
  if (status == ARCHBIRD_OK)
    status = ab_report_chunks(out, &rows, "  exports: ", MAP_REPORT_WIDTH);
  ab_report_list_free(&rows);
  return status;
}

static ArchbirdStatus map_render_build_full(const MapReportContext *context,
                                            const AbValue *route,
                                            AbBuffer *out) {
  const AbString *source = ab_report_string(route, "source");
  const AbString *name = ab_report_string(route, "name");
  const AbString *command = map_string_or_empty(route, "command");
  const AbString *variant = map_string_or_empty(route, "variant");
  const AbValue *deps = map_optional_array(route, "deps");
  const AbValue *paths = map_optional_array(route, "paths");
  const AbValue *conditions = map_optional_array(route, "conditions");
  AbReportStringList details;
  AbBuffer text;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  if (!source || !name || !command || !variant)
    return map_schema_error(context->engine, "map.builds[] is malformed");
  ab_report_list_init(&details, context->engine);
  ab_buffer_init(&text, context->engine);
  if (deps->as.array.count) {
    status = ab_buffer_literal(&text, "deps=");
    for (index = 0; status == ARCHBIRD_OK && index < deps->as.array.count;
         index++) {
      const AbValue *item = &deps->as.array.items[index];
      if (item->kind != AB_VALUE_STRING)
        status = map_schema_error(context->engine,
                                  "build dependencies are malformed");
      else {
        if (index)
          status = ab_buffer_literal(&text, ",");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&text, item->as.text.data, item->as.text.length);
      }
    }
    if (status == ARCHBIRD_OK)
      status =
          ab_report_list_add(&details, (const char *)text.data, text.length);
    text.length = 0;
    if (text.data)
      text.data[0] = '\0';
  }
  if (status == ARCHBIRD_OK && paths->as.array.count) {
    status = ab_buffer_literal(&text, "paths=");
    for (index = 0; status == ARCHBIRD_OK && index < paths->as.array.count;
         index++) {
      const AbValue *item = &paths->as.array.items[index];
      if (item->kind != AB_VALUE_STRING)
        status = map_schema_error(context->engine, "build paths are malformed");
      else {
        if (index)
          status = ab_buffer_literal(&text, ",");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&text, item->as.text.data, item->as.text.length);
      }
    }
    if (status == ARCHBIRD_OK)
      status =
          ab_report_list_add(&details, (const char *)text.data, text.length);
    text.length = 0;
    if (text.data)
      text.data[0] = '\0';
  }
  if (status == ARCHBIRD_OK && command->length)
    status = ab_report_list_addf(&details, "command=%.*s", (int)command->length,
                                 command->data);
  if (status == ARCHBIRD_OK && variant->length)
    status = ab_report_list_addf(&details, "variant=%.*s", (int)variant->length,
                                 variant->data);
  if (status == ARCHBIRD_OK && conditions->as.array.count) {
    status = ab_buffer_literal(&text, "conditions=");
    for (index = 0; status == ARCHBIRD_OK && index < conditions->as.array.count;
         index++) {
      const AbValue *item = &conditions->as.array.items[index];
      if (item->kind != AB_VALUE_STRING)
        status =
            map_schema_error(context->engine, "build conditions are malformed");
      else {
        if (index)
          status = ab_buffer_literal(&text, " || ");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&text, item->as.text.data, item->as.text.length);
      }
    }
    if (status == ARCHBIRD_OK)
      status =
          ab_report_list_add(&details, (const char *)text.data, text.length);
  }
  if (status == ARCHBIRD_OK) {
    status = ab_report_appendf(out, "%.*s:%.*s: ", (int)source->length,
                               source->data, (int)name->length, name->data);
    if (status == ARCHBIRD_OK && !details.count)
      status = ab_buffer_literal(out, "target");
    for (index = 0; status == ARCHBIRD_OK && index < details.count; index++) {
      if (index)
        status = ab_buffer_literal(out, "; ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(out, details.items[index].data,
                                  details.items[index].length);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "\n");
  }
  ab_buffer_free(&text);
  ab_report_list_free(&details);
  return status;
}

static ArchbirdStatus map_render_entrypoints(const MapReportContext *context,
                                             int full, AbBuffer *out) {
  size_t index;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(
      ab_report_literal_line(out, "## Entrypoints and build routes"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  if (!context->packages->as.array.count && !context->builds->as.array.count)
    MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
  for (index = 0; index < context->packages->as.array.count; index++)
    MAP_REPORT_TRY(map_render_package(
        context, &context->packages->as.array.items[index], full, out));
  if (full) {
    for (index = 0; index < context->builds->as.array.count; index++)
      MAP_REPORT_TRY(map_render_build_full(
          context, &context->builds->as.array.items[index], out));
  } else {
    AbReportStringList sources;
    ArchbirdStatus status = ARCHBIRD_OK;
    ab_report_list_init(&sources, context->engine);
    for (index = 0;
         status == ARCHBIRD_OK && index < context->builds->as.array.count;
         index++) {
      const AbString *source =
          ab_report_string(&context->builds->as.array.items[index], "source");
      if (!source)
        status = map_schema_error(context->engine, "map.builds[] is malformed");
      else
        status = ab_report_list_add(&sources, source->data, source->length);
    }
    if (status == ARCHBIRD_OK)
      ab_report_list_sort_unique(&sources);
    for (index = 0; status == ARCHBIRD_OK && index < sources.count; index++) {
      AbReportStringList routes;
      AbBuffer prefix;
      size_t route_index;
      ab_report_list_init(&routes, context->engine);
      ab_buffer_init(&prefix, context->engine);
      for (route_index = 0; status == ARCHBIRD_OK &&
                            route_index < context->builds->as.array.count;
           route_index++) {
        const AbValue *route = &context->builds->as.array.items[route_index];
        const AbString *source = ab_report_string(route, "source");
        const AbString *name = ab_report_string(route, "name");
        const AbString *variant = map_string_or_empty(route, "variant");
        if (!source || !name || !variant) {
          status =
              map_schema_error(context->engine, "map.builds[] is malformed");
          break;
        }
        if (ab_string_equal(source, &sources.items[index]))
          status =
              variant->length
                  ? ab_report_list_addf(
                        &routes,
                        "%.*s[variant=%.*s,deps=%zu,paths=%zu,conditions=%zu]",
                        (int)name->length, name->data, (int)variant->length,
                        variant->data,
                        map_optional_array(route, "deps")->as.array.count,
                        map_optional_array(route, "paths")->as.array.count,
                        map_optional_array(route, "conditions")->as.array.count)
                  : ab_report_list_addf(
                        &routes, "%.*s[deps=%zu,paths=%zu,conditions=%zu]",
                        (int)name->length, name->data,
                        map_optional_array(route, "deps")->as.array.count,
                        map_optional_array(route, "paths")->as.array.count,
                        map_optional_array(route, "conditions")
                            ->as.array.count);
      }
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(&prefix,
                                   "%.*s: ", (int)sources.items[index].length,
                                   sources.items[index].data);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &routes, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&routes);
    }
    ab_report_list_free(&sources);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_indexes(const MapReportContext *context,
                                         AbBuffer *out) {
  size_t index;
  if (!context->indexes->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Precision indexes"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->indexes->as.array.count; index++) {
    const AbValue *row = &context->indexes->as.array.items[index];
    const AbString *name = ab_report_string(row, "name");
    const AbString *format = ab_report_string(row, "format");
    const AbString *prefix = map_string_or_empty(row, "path_prefix");
    const AbString *variant = map_string_or_empty(row, "variant");
    const AbValue *tool = ab_report_object(row, "tool");
    const AbValue *coverage = ab_report_object(row, "coverage");
    const AbString *tool_name = map_string_or_empty(tool, "name");
    const AbString *tool_version = map_string_or_empty(tool, "version");
    if (!name || !format || !prefix || !variant || !tool_name ||
        !tool_version || !coverage)
      return map_schema_error(context->engine, "map.indexes[] is malformed");
    if (variant->length)
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "%.*s: format=%.*s variant=%.*s tool=%.*s@%.*s%s%.*s "
          "documents=%zu/%zu references=%zu unique=%zu ambiguous=%zu "
          "unresolved=%zu edges=%zu",
          (int)name->length, name->data, (int)format->length, format->data,
          (int)variant->length, variant->data, (int)tool_name->length,
          tool_name->data, (int)tool_version->length, tool_version->data,
          prefix->length ? " path_prefix=" : "", (int)prefix->length,
          prefix->data, ab_report_size(coverage, "documents_mapped", 0),
          ab_report_size(coverage, "documents_total", 0),
          ab_report_size(coverage, "references", 0),
          ab_report_size(coverage, "resolved_unique", 0),
          ab_report_size(coverage, "resolved_ambiguous", 0),
          ab_report_size(coverage, "unresolved", 0),
          ab_report_size(coverage, "edges", 0)));
    else if (prefix->length)
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "%.*s: format=%.*s tool=%.*s@%.*s path_prefix=%.*s "
          "documents=%zu/%zu references=%zu unique=%zu ambiguous=%zu "
          "unresolved=%zu edges=%zu",
          (int)name->length, name->data, (int)format->length, format->data,
          (int)tool_name->length, tool_name->data, (int)tool_version->length,
          tool_version->data, (int)prefix->length, prefix->data,
          ab_report_size(coverage, "documents_mapped", 0),
          ab_report_size(coverage, "documents_total", 0),
          ab_report_size(coverage, "references", 0),
          ab_report_size(coverage, "resolved_unique", 0),
          ab_report_size(coverage, "resolved_ambiguous", 0),
          ab_report_size(coverage, "unresolved", 0),
          ab_report_size(coverage, "edges", 0)));
    else
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "%.*s: format=%.*s tool=%.*s@%.*s documents=%zu/%zu "
          "references=%zu unique=%zu ambiguous=%zu unresolved=%zu edges=%zu",
          (int)name->length, name->data, (int)format->length, format->data,
          (int)tool_name->length, tool_name->data, (int)tool_version->length,
          tool_version->data, ab_report_size(coverage, "documents_mapped", 0),
          ab_report_size(coverage, "documents_total", 0),
          ab_report_size(coverage, "references", 0),
          ab_report_size(coverage, "resolved_unique", 0),
          ab_report_size(coverage, "resolved_ambiguous", 0),
          ab_report_size(coverage, "unresolved", 0),
          ab_report_size(coverage, "edges", 0)));
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_artifact_inputs(const MapReportContext *context,
                                          const AbValue *artifact,
                                          AbReportStringList *rows) {
  const AbValue *inputs = map_optional_array(artifact, "inputs");
  size_t index;
  for (index = 0; index < inputs->as.array.count; index++) {
    const AbValue *input = &inputs->as.array.items[index];
    const AbString *path = ab_report_string(input, "path");
    const AbValue *evidence = map_optional_array(input, "evidence");
    AbBuffer text;
    ArchbirdStatus status;
    size_t evidence_index;
    if (!path)
      return map_schema_error(context->engine, "artifact input is malformed");
    ab_buffer_init(&text, context->engine);
    status = ab_report_appendf(&text, "%.*s[", (int)path->length, path->data);
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
         evidence_index++) {
      const AbValue *item = &evidence->as.array.items[evidence_index];
      if (item->kind != AB_VALUE_STRING)
        status = map_schema_error(context->engine,
                                  "artifact input evidence is malformed");
      else {
        if (evidence_index)
          status = ab_buffer_literal(&text, ",");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&text, item->as.text.data, item->as.text.length);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&text, "]");
    if (status == ARCHBIRD_OK)
      status = ab_report_list_add(rows, (const char *)text.data, text.length);
    ab_buffer_free(&text);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_artifacts(const MapReportContext *context,
                                           AbBuffer *out) {
  size_t index;
  if (!context->artifacts->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Executable artifacts"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(
      out,
      "Configured artifact records are shown; unconfigured repository outputs "
      "are outside this inventory."));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->artifacts->as.array.count; index++) {
    const AbValue *artifact = &context->artifacts->as.array.items[index];
    const AbString *name = ab_report_string(artifact, "name");
    const AbString *output = ab_report_string(artifact, "output");
    const AbValue *depends = map_optional_array(artifact, "depends_on");
    const AbValue *loaders = map_optional_array(artifact, "loaded_by");
    const AbValue *builds = map_optional_array(artifact, "builds");
    AbReportStringList rows;
    ArchbirdStatus status;
    size_t item_index;
    if (!name || !output)
      return map_schema_error(context->engine, "map.artifacts[] is malformed");
    MAP_REPORT_TRY(ab_report_linef(out, "%.*s: output=%.*s", (int)name->length,
                                   name->data, (int)output->length,
                                   output->data));
    if (depends->as.array.count) {
      ab_report_list_init(&rows, context->engine);
      status = map_add_array_strings(&rows, depends);
      if (status == ARCHBIRD_OK)
        status =
            ab_report_chunks(out, &rows, "  depends-on: ", MAP_REPORT_WIDTH);
      ab_report_list_free(&rows);
      if (status != ARCHBIRD_OK)
        return status;
    }
    ab_report_list_init(&rows, context->engine);
    status = map_artifact_inputs(context, artifact, &rows);
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &rows, "  inputs: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&rows);
    if (status != ARCHBIRD_OK)
      return status;
    ab_report_list_init(&rows, context->engine);
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < loaders->as.array.count;
         item_index++) {
      const AbValue *loader = &loaders->as.array.items[item_index];
      const AbString *path = ab_report_string(loader, "path");
      const AbString *pattern = ab_report_string(loader, "pattern");
      if (!path || !pattern)
        status =
            map_schema_error(context->engine, "artifact loader is malformed");
      else
        status = ab_report_list_addf(&rows, "%.*s[matches=%zu; pattern=%.*s]",
                                     (int)path->length, path->data,
                                     ab_report_size(loader, "matches", 0),
                                     (int)pattern->length, pattern->data);
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &rows, "  loaded-by: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&rows);
    if (status != ARCHBIRD_OK)
      return status;
    ab_report_list_init(&rows, context->engine);
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < builds->as.array.count;
         item_index++) {
      const AbValue *build = &builds->as.array.items[item_index];
      const AbString *source = ab_report_string(build, "source");
      const AbString *target = ab_report_string(build, "target");
      if (!source || !target)
        status =
            map_schema_error(context->engine, "artifact build is malformed");
      else
        status = ab_report_list_addf(&rows, "%.*s:%.*s", (int)source->length,
                                     source->data, (int)target->length,
                                     target->data);
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &rows, "  builds: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&rows);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_bridges(const MapReportContext *context,
                                         int full, AbBuffer *out) {
  AbReportStringList bridge_names;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&bridge_names, context->engine);
  for (index = 0; index < context->edge_count; index++) {
    const MapReportEdge *edge = &context->edges[index];
    if (map_string_starts(edge->kind, "bridge:")) {
      status = ab_report_list_add(&bridge_names, edge->kind->data + 7,
                                  edge->kind->length - 7);
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(&bridge_names);
  if (status != ARCHBIRD_OK) {
    ab_report_list_free(&bridge_names);
    return status;
  }
  status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "## Cross-layer bridges");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```text");
  if (!bridge_names.count)
    if (status == ARCHBIRD_OK)
      status = ab_report_literal_line(out, "none");
  if (status == ARCHBIRD_OK && full) {
    for (index = 0; index < context->edge_count; index++) {
      const MapReportEdge *edge = &context->edges[index];
      AbReportStringList names;
      AbBuffer prefix;
      if (!map_string_starts(edge->kind, "bridge:"))
        continue;
      ab_report_list_init(&names, context->engine);
      ab_buffer_init(&prefix, context->engine);
      status = map_add_array_strings(&names, edge->names);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(
            &prefix, "%.*s %.*s -> %.*s: ", (int)(edge->kind->length - 7),
            edge->kind->data + 7, (int)edge->source->length, edge->source->data,
            (int)edge->target->length, edge->target->data);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &names, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&names);
      if (status != ARCHBIRD_OK)
        break;
    }
  } else if (status == ARCHBIRD_OK) {
    for (index = 0; index < bridge_names.count && status == ARCHBIRD_OK;
         index++) {
      const AbString *bridge = &bridge_names.items[index];
      AbReportStringList sources;
      AbReportStringList targets;
      AbReportStringList routes;
      AbBuffer prefix;
      size_t edge_index;
      ab_report_list_init(&sources, context->engine);
      ab_report_list_init(&targets, context->engine);
      ab_report_list_init(&routes, context->engine);
      ab_buffer_init(&prefix, context->engine);
      for (edge_index = 0;
           status == ARCHBIRD_OK && edge_index < context->edge_count;
           edge_index++) {
        const MapReportEdge *edge = &context->edges[edge_index];
        if (edge->kind->length != bridge->length + 7 ||
            memcmp(edge->kind->data, "bridge:", 7) != 0 ||
            memcmp(edge->kind->data + 7, bridge->data, bridge->length) != 0)
          continue;
        status = ab_report_list_add(&sources, edge->source->data,
                                    edge->source->length);
        if (status == ARCHBIRD_OK)
          status = ab_report_list_add(&targets, edge->target->data,
                                      edge->target->length);
      }
      if (status == ARCHBIRD_OK) {
        ab_report_list_sort_unique(&sources);
        ab_report_list_sort_unique(&targets);
      }
      for (edge_index = 0; status == ARCHBIRD_OK && edge_index < targets.count;
           edge_index++) {
        AbReportStringList names;
        size_t scan;
        ab_report_list_init(&names, context->engine);
        for (scan = 0; status == ARCHBIRD_OK && scan < context->edge_count;
             scan++) {
          const MapReportEdge *edge = &context->edges[scan];
          if (edge->kind->length != bridge->length + 7 ||
              memcmp(edge->kind->data, "bridge:", 7) != 0 ||
              memcmp(edge->kind->data + 7, bridge->data, bridge->length) != 0 ||
              !ab_string_equal(edge->target, &targets.items[edge_index]))
            continue;
          status = map_add_array_strings(&names, edge->names);
        }
        if (status == ARCHBIRD_OK)
          ab_report_list_sort_unique(&names);
        if (status == ARCHBIRD_OK)
          status = ab_report_list_addf(
              &routes, "%.*s(%zu)", (int)targets.items[edge_index].length,
              targets.items[edge_index].data, names.count);
        ab_report_list_free(&names);
      }
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(&prefix,
                                   "%.*s sources=%zu: ", (int)bridge->length,
                                   bridge->data, sources.count);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &routes, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&routes);
      ab_report_list_free(&targets);
      ab_report_list_free(&sources);
    }
  }
  ab_report_list_free(&bridge_names);
  if (status != ARCHBIRD_OK)
    return status;
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_package_edges(const MapReportContext *context,
                                               const char *kind,
                                               const char *heading,
                                               AbBuffer *out) {
  AbReportStringList targets;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&targets, context->engine);
  for (index = 0; index < context->edge_count; index++) {
    const MapReportEdge *edge = &context->edges[index];
    int include = strcmp(kind, "external") == 0
                      ? ab_report_string_equal(edge->kind, "external") ||
                            ab_report_string_equal(edge->kind, "external-call")
                      : ab_report_string_equal(edge->kind, kind);
    const char *target;
    size_t target_length;
    if (!include)
      continue;
    target = edge->target->data;
    target_length = edge->target->length;
    if (target_length >= 8 && memcmp(target, "package:", 8) == 0) {
      target += 8;
      target_length -= 8;
    }
    status = ab_report_list_add(&targets, target, target_length);
    if (status != ARCHBIRD_OK)
      break;
  }
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(&targets);
  if (status != ARCHBIRD_OK) {
    ab_report_list_free(&targets);
    return status;
  }
  if (!targets.count) {
    ab_report_list_free(&targets);
    return ARCHBIRD_OK;
  }
  status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, heading);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```text");
  {
    AbReportStringList rows;
    ab_report_list_init(&rows, context->engine);
    for (index = 0; status == ARCHBIRD_OK && index < targets.count; index++) {
      AbReportStringList sources;
      size_t edge_index;
      ab_report_list_init(&sources, context->engine);
      for (edge_index = 0;
           status == ARCHBIRD_OK && edge_index < context->edge_count;
           edge_index++) {
        const MapReportEdge *edge = &context->edges[edge_index];
        int include =
            strcmp(kind, "external") == 0
                ? ab_report_string_equal(edge->kind, "external") ||
                      ab_report_string_equal(edge->kind, "external-call")
                : ab_report_string_equal(edge->kind, kind);
        const char *target = edge->target->data;
        size_t target_length = edge->target->length;
        if (!include)
          continue;
        if (target_length >= 8 && memcmp(target, "package:", 8) == 0) {
          target += 8;
          target_length -= 8;
        }
        if (target_length == targets.items[index].length &&
            (!target_length ||
             memcmp(target, targets.items[index].data, target_length) == 0))
          status = ab_report_list_add(&sources, edge->source->data,
                                      edge->source->length);
      }
      if (status == ARCHBIRD_OK)
        ab_report_list_sort_unique(&sources);
      if (status == ARCHBIRD_OK)
        status = ab_report_list_addf(&rows, "%.*s(%zu files)",
                                     (int)targets.items[index].length,
                                     targets.items[index].data, sources.count);
      ab_report_list_free(&sources);
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &rows, "packages: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&rows);
  }
  ab_report_list_free(&targets);
  if (status != ARCHBIRD_OK)
    return status;
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus
map_render_resolution_kind(const MapReportContext *context, const char *kind,
                           int full, AbBuffer *out) {
  MapCountedName *ranked = NULL;
  size_t index;
  size_t match_count = 0;
  size_t ranked_count = 0;
  size_t selected;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; index < context->resolutions->as.array.count; index++) {
    const AbValue *row = &context->resolutions->as.array.items[index];
    const AbString *row_kind = ab_report_string(row, "kind");
    const AbString *name = ab_report_string(row, "name");
    if (!row_kind || !name) {
      status = map_schema_error(context->engine,
                                "map.call_resolutions[] is malformed");
      goto cleanup;
    }
    if (ab_report_string_equal(row_kind, kind))
      match_count++;
  }
  if (match_count) {
    ranked = (MapCountedName *)ab_calloc(context->engine, match_count,
                                         sizeof(*ranked));
    if (!ranked) {
      status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory ranking call resolutions");
      goto cleanup;
    }
  }
  for (index = 0; index < context->resolutions->as.array.count; index++) {
    const AbValue *row = &context->resolutions->as.array.items[index];
    const AbString *row_kind = ab_report_string(row, "kind");
    if (ab_report_string_equal(row_kind, kind)) {
      ranked[ranked_count].name = ab_report_string(row, "name");
      ranked[ranked_count].count = ab_report_size(row, "count", 0);
      ranked_count++;
    }
  }
  if (ranked_count > 1)
    qsort(ranked, ranked_count, sizeof(*ranked), map_counted_name_compare);
  match_count = ranked_count;
  ranked_count = 0;
  for (index = 0; index < match_count; index++) {
    if (ranked_count &&
        ab_string_equal(ranked[ranked_count - 1].name, ranked[index].name)) {
      ranked[ranked_count - 1].count += ranked[index].count;
    } else {
      if (ranked_count != index)
        ranked[ranked_count] = ranked[index];
      ranked_count++;
    }
  }
  if (ranked_count > 1)
    qsort(ranked, ranked_count, sizeof(*ranked), map_counted_compare);
  selected = full || ranked_count < context->compact_edges
                 ? ranked_count
                 : context->compact_edges;
  {
    AbReportStringList rows;
    AbBuffer prefix;
    ab_report_list_init(&rows, context->engine);
    ab_buffer_init(&prefix, context->engine);
    for (index = 0; status == ARCHBIRD_OK && index < selected; index++)
      status = ab_report_list_addf(
          &rows, "%.*s(%zu)", (int)ranked[index].name->length,
          ranked[index].name->data, ranked[index].count);
    if (status == ARCHBIRD_OK && ranked_count > selected)
      status = ab_report_list_addf(&rows, "…+%zu", ranked_count - selected);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(&prefix, "%s: ", kind);
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &rows, (const char *)prefix.data,
                                MAP_REPORT_WIDTH);
    ab_buffer_free(&prefix);
    ab_report_list_free(&rows);
  }
cleanup:
  ab_free(context->engine, ranked);
  return status;
}

static ArchbirdStatus map_render_resolutions(const MapReportContext *context,
                                             int full, AbBuffer *out) {
  size_t counts[sizeof(map_resolution_kinds) /
                sizeof(map_resolution_kinds[0])] = {0};
  size_t total = 0;
  size_t index;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Call resolution coverage"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->resolutions->as.array.count; index++) {
    const AbValue *row = &context->resolutions->as.array.items[index];
    const AbString *kind = ab_report_string(row, "kind");
    size_t count = ab_report_size(row, "count", 0);
    size_t kind_index;
    if (!kind)
      return map_schema_error(context->engine,
                              "map.call_resolutions[] is malformed");
    total += count;
    for (kind_index = 0; kind_index < sizeof(map_resolution_kinds) /
                                          sizeof(map_resolution_kinds[0]);
         kind_index++)
      if (ab_report_string_equal(kind, map_resolution_kinds[kind_index]))
        counts[kind_index] += count;
  }
  MAP_REPORT_TRY(ab_report_appendf(out, "lexical occurrences=%zu", total));
  for (index = 0;
       index < sizeof(map_resolution_kinds) / sizeof(map_resolution_kinds[0]);
       index++)
    MAP_REPORT_TRY(ab_report_appendf(
        out, " %s=%zu", map_resolution_kinds[index], counts[index]));
  MAP_REPORT_TRY(ab_buffer_literal(out, "\n"));
  if (full) {
    for (index = 0;
         index < sizeof(map_resolution_kinds) / sizeof(map_resolution_kinds[0]);
         index++)
      MAP_REPORT_TRY(map_render_resolution_kind(
          context, map_resolution_kinds[index], 1, out));
  } else {
    MAP_REPORT_TRY(map_render_resolution_kind(context, "ambiguous", 0, out));
    MAP_REPORT_TRY(map_render_resolution_kind(context, "candidate", 0, out));
    MAP_REPORT_TRY(map_render_resolution_kind(context, "unresolved", 0, out));
    MAP_REPORT_TRY(map_render_resolution_kind(context, "external", 0, out));
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_named_entries(const MapReportContext *context,
                                               AbBuffer *out) {
  size_t group_index;
  if (!context->named_entries->as.object.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Named entrypoints"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (group_index = 0; group_index < context->named_entries->as.object.count;
       group_index++) {
    const AbObjectField *group =
        &context->named_entries->as.object.fields[group_index];
    size_t path_index;
    if (group->value.kind != AB_VALUE_OBJECT)
      return map_schema_error(context->engine,
                              "map.named_entries is malformed");
    if (!group->value.as.object.count)
      MAP_REPORT_TRY(ab_report_linef(out, "%.*s: none", (int)group->name.length,
                                     group->name.data));
    for (path_index = 0; path_index < group->value.as.object.count;
         path_index++) {
      const AbObjectField *path = &group->value.as.object.fields[path_index];
      AbReportStringList names;
      AbBuffer prefix;
      ArchbirdStatus status;
      ab_report_list_init(&names, context->engine);
      ab_buffer_init(&prefix, context->engine);
      if (path->value.kind != AB_VALUE_ARRAY)
        status = map_schema_error(context->engine,
                                  "map.named_entries values are malformed");
      else
        status = map_add_array_strings(&names, &path->value);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(
            &prefix, "%.*s %.*s: ", (int)group->name.length, group->name.data,
            (int)path->name.length, path->name.data);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &names, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&names);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_parity(const MapReportContext *context,
                                        int full, AbBuffer *out) {
  size_t parity_index;
  if (!context->parity->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Parity surfaces"));
  MAP_REPORT_TRY(ab_report_blank(out));
  for (parity_index = 0; parity_index < context->parity->as.array.count;
       parity_index++) {
    const AbValue *parity = &context->parity->as.array.items[parity_index];
    const AbString *name = ab_report_string(parity, "name");
    const AbValue *shared = map_optional_array(parity, "shared");
    const AbValue *members = map_optional_array(parity, "members");
    size_t member_index;
    if (!name)
      return map_schema_error(context->engine, "map.parity[] is malformed");
    MAP_REPORT_TRY(
        ab_report_linef(out, "### %.*s", (int)name->length, name->data));
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
    MAP_REPORT_TRY(
        ab_report_appendf(out, "shared=%zu", shared->as.array.count));
    for (member_index = 0; member_index < members->as.array.count;
         member_index++) {
      const AbValue *member = &members->as.array.items[member_index];
      const AbString *label = ab_report_string(member, "label");
      const AbValue *values = map_optional_array(member, "values");
      if (!label)
        return map_schema_error(context->engine,
                                "map parity member is malformed");
      MAP_REPORT_TRY(ab_report_appendf(out, " %.*s=%zu", (int)label->length,
                                       label->data, values->as.array.count));
    }
    MAP_REPORT_TRY(ab_report_linef(
        out, " enforce=%s",
        ab_report_bool(parity, "enforce", 0) ? "true" : "false"));
    for (member_index = 0; member_index < members->as.array.count;
         member_index++) {
      const AbValue *member = &members->as.array.items[member_index];
      const AbString *label = ab_report_string(member, "label");
      const AbValue *missing = map_optional_array(member, "missing");
      AbReportStringList values;
      AbBuffer prefix;
      ArchbirdStatus status;
      ab_report_list_init(&values, context->engine);
      ab_buffer_init(&prefix, context->engine);
      status = map_limited_values(context->engine, missing,
                                  full ? SIZE_MAX : 60, &values);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(&prefix,
                                   "%.*s missing(%zu): ", (int)label->length,
                                   label->data, missing->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &values, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&values);
      if (status != ARCHBIRD_OK)
        return status;
    }
    MAP_REPORT_TRY(ab_report_literal_line(out, "```"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_directory_key(ArchbirdEngine *engine,
                                        const AbString *path, AbString *out) {
  size_t last_slash = SIZE_MAX;
  size_t first_slash = SIZE_MAX;
  size_t second_slash = SIZE_MAX;
  size_t index;
  for (index = 0; index < path->length; index++)
    if (path->data[index] == '/')
      last_slash = index;
  if (last_slash == SIZE_MAX)
    return ab_string_copy(engine, out, ".", 1);
  for (index = 0; index < last_slash; index++) {
    if (path->data[index] != '/')
      continue;
    if (first_slash == SIZE_MAX)
      first_slash = index;
    else {
      second_slash = index;
      break;
    }
  }
  return ab_string_copy(engine, out, path->data,
                        second_slash == SIZE_MAX ? last_slash : second_slash);
}

typedef struct MapDirectoryGroup {
  AbString key;
  size_t files;
  size_t symbols;
  size_t incoming;
  size_t outgoing;
} MapDirectoryGroup;

static int map_directory_key_compare(const void *left_raw,
                                     const void *right_raw) {
  const MapDirectoryGroup *left = (const MapDirectoryGroup *)left_raw;
  const MapDirectoryGroup *right = (const MapDirectoryGroup *)right_raw;
  return ab_string_compare(&left->key, &right->key);
}

static int map_directory_rank_compare(const void *left_raw,
                                      const void *right_raw) {
  const MapDirectoryGroup *left = *(MapDirectoryGroup *const *)left_raw;
  const MapDirectoryGroup *right = *(MapDirectoryGroup *const *)right_raw;
  if (left->files != right->files)
    return left->files > right->files ? -1 : 1;
  return ab_string_compare(&left->key, &right->key);
}

static size_t map_find_directory(const MapDirectoryGroup *groups, size_t count,
                                 const AbString *key) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&groups[middle].key, key);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return middle;
  }
  return SIZE_MAX;
}

static ArchbirdStatus map_render_directories(const MapReportContext *context,
                                             AbBuffer *out) {
  MapDirectoryGroup *groups = NULL;
  MapDirectoryGroup **ranked = NULL;
  AbString *file_keys = NULL;
  size_t group_count = 0;
  size_t index;
  size_t limit;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (context->file_count) {
    groups = (MapDirectoryGroup *)ab_calloc(
        context->engine, context->file_count, sizeof(*groups));
    ranked = (MapDirectoryGroup **)ab_calloc(
        context->engine, context->file_count, sizeof(*ranked));
    file_keys = (AbString *)ab_calloc(context->engine, context->file_count,
                                      sizeof(*file_keys));
    if (!groups || !ranked || !file_keys) {
      status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory building directory bird view");
      goto cleanup;
    }
  }
  for (index = 0; index < context->file_count; index++) {
    size_t group_index;
    if (context->files[index].third_party_candidate)
      status = ab_string_copy(context->engine, &file_keys[index],
                              "[third-party candidates]", 24);
    else if (context->files[index].generated_candidate)
      status = ab_string_copy(context->engine, &file_keys[index],
                              "[generated candidates]", 22);
    else
      status = map_directory_key(context->engine, context->files[index].path,
                                 &file_keys[index]);
    if (status != ARCHBIRD_OK)
      goto cleanup;
    for (group_index = 0; group_index < group_count; group_index++)
      if (ab_string_equal(&groups[group_index].key, &file_keys[index]))
        break;
    if (group_index == group_count) {
      status = ab_string_copy(context->engine, &groups[group_count].key,
                              file_keys[index].data, file_keys[index].length);
      if (status != ARCHBIRD_OK)
        goto cleanup;
      group_count++;
    }
  }
  if (group_count > 1)
    qsort(groups, group_count, sizeof(*groups), map_directory_key_compare);
  for (index = 0; index < context->file_count; index++) {
    size_t group_index =
        map_find_directory(groups, group_count, &file_keys[index]);
    if (group_index == SIZE_MAX) {
      status = map_schema_error(context->engine,
                                "directory index lost a mapped file");
      goto cleanup;
    }
    groups[group_index].files++;
    groups[group_index].symbols +=
        context->files[index].symbols->as.array.count;
  }
  for (index = 0; index < context->edge_count; index++) {
    const MapReportEdge *edge = &context->edges[index];
    size_t source_group;
    size_t target_group;
    if (edge->source_index == SIZE_MAX || edge->target_index == SIZE_MAX)
      continue;
    source_group =
        map_find_directory(groups, group_count, &file_keys[edge->source_index]);
    target_group =
        map_find_directory(groups, group_count, &file_keys[edge->target_index]);
    if (source_group == target_group)
      continue;
    groups[source_group].outgoing++;
    groups[target_group].incoming++;
  }
  for (index = 0; index < group_count; index++)
    ranked[index] = &groups[index];
  if (group_count > 1)
    qsort(ranked, group_count, sizeof(*ranked), map_directory_rank_compare);
  limit = context->compact_edges * 4;
  if (limit < 12)
    limit = 12;
  for (index = 0; index < group_count && index < limit; index++) {
    MapDirectoryGroup *group = ranked[index];
    AbReportStringList names;
    MapCountedName *hubs = NULL;
    size_t edge_index;
    size_t hub_index;
    size_t shown;
    ab_report_list_init(&names, context->engine);
    for (edge_index = 0;
         status == ARCHBIRD_OK && edge_index < context->edge_count;
         edge_index++) {
      const MapReportEdge *edge = &context->edges[edge_index];
      size_t source_group;
      size_t target_group;
      if (edge->source_index == SIZE_MAX || edge->target_index == SIZE_MAX)
        continue;
      source_group = map_find_directory(groups, group_count,
                                        &file_keys[edge->source_index]);
      target_group = map_find_directory(groups, group_count,
                                        &file_keys[edge->target_index]);
      if (source_group == target_group || &groups[target_group] != group)
        continue;
      {
        size_t name_index;
        for (name_index = 0;
             status == ARCHBIRD_OK && name_index < edge->names->as.array.count;
             name_index++) {
          const AbString *name =
              &edge->names->as.array.items[name_index].as.text;
          if (!map_list_contains(&names, name))
            status = ab_report_list_add(&names, name->data, name->length);
        }
      }
    }
    if (status == ARCHBIRD_OK && names.count) {
      hubs = (MapCountedName *)ab_calloc(context->engine, names.count,
                                         sizeof(*hubs));
      if (!hubs)
        status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory ranking directory hubs");
    }
    for (hub_index = 0; status == ARCHBIRD_OK && hub_index < names.count;
         hub_index++) {
      hubs[hub_index].name = &names.items[hub_index];
      hubs[hub_index].order = hub_index;
      for (edge_index = 0; edge_index < context->edge_count; edge_index++) {
        const MapReportEdge *edge = &context->edges[edge_index];
        size_t source_group;
        size_t target_group;
        size_t name_index;
        if (edge->source_index == SIZE_MAX || edge->target_index == SIZE_MAX)
          continue;
        source_group = map_find_directory(groups, group_count,
                                          &file_keys[edge->source_index]);
        target_group = map_find_directory(groups, group_count,
                                          &file_keys[edge->target_index]);
        if (source_group == target_group || &groups[target_group] != group)
          continue;
        for (name_index = 0; name_index < edge->names->as.array.count;
             name_index++)
          if (ab_string_equal(&edge->names->as.array.items[name_index].as.text,
                              hubs[hub_index].name))
            hubs[hub_index].count++;
      }
    }
    if (status == ARCHBIRD_OK && names.count > 1)
      qsort(hubs, names.count, sizeof(*hubs), map_hub_compare);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(
          out, "%.*s: files=%zu symbols=%zu in=%zu out=%zu",
          (int)group->key.length, group->key.data, group->files, group->symbols,
          group->incoming, group->outgoing);
    shown = names.count < 5 ? names.count : 5;
    if (status == ARCHBIRD_OK && shown)
      status = ab_buffer_literal(out, " hubs=");
    for (hub_index = 0; status == ARCHBIRD_OK && hub_index < shown;
         hub_index++) {
      if (hub_index)
        status = ab_buffer_literal(out, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(out, hubs[hub_index].name->data,
                                  hubs[hub_index].name->length);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "\n");
    ab_free(context->engine, hubs);
    ab_report_list_free(&names);
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  if (group_count > limit)
    status = ab_report_linef(out, "… %zu lower-ranked directory groups omitted",
                             group_count - limit);

cleanup:
  if (groups)
    for (index = 0; index < group_count; index++)
      ab_string_free(context->engine, &groups[index].key);
  if (file_keys)
    for (index = 0; index < context->file_count; index++)
      ab_string_free(context->engine, &file_keys[index]);
  ab_free(context->engine, file_keys);
  ab_free(context->engine, ranked);
  ab_free(context->engine, groups);
  return status;
}

static ArchbirdStatus map_render_file_edges(const MapReportContext *context,
                                            const MapReportFile *file, int full,
                                            AbBuffer *out) {
  size_t index;
  if (full) {
    for (index = 0; index < context->edge_count; index++) {
      const MapReportEdge *edge = &context->edges[index];
      AbReportStringList names;
      AbBuffer prefix;
      ArchbirdStatus status;
      if (!ab_string_equal(edge->source, file->path) ||
          map_string_starts(edge->kind, "bridge:"))
        continue;
      ab_report_list_init(&names, context->engine);
      ab_buffer_init(&prefix, context->engine);
      status = map_add_array_strings(&names, edge->names);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(
            &prefix, "  %.*s -> %.*s: ", (int)edge->kind->length,
            edge->kind->data, (int)edge->target->length, edge->target->data);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &names, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&names);
      if (status != ARCHBIRD_OK)
        return status;
    }
    return ARCHBIRD_OK;
  }
  {
    AbReportStringList kinds;
    ArchbirdStatus status = ARCHBIRD_OK;
    ab_report_list_init(&kinds, context->engine);
    for (index = 0; index < context->edge_count; index++) {
      const MapReportEdge *edge = &context->edges[index];
      if (!ab_string_equal(edge->source, file->path) ||
          map_string_starts(edge->kind, "bridge:") ||
          ab_report_string_equal(edge->kind, "external"))
        continue;
      status = ab_report_list_add(&kinds, edge->kind->data, edge->kind->length);
      if (status != ARCHBIRD_OK)
        break;
    }
    if (status == ARCHBIRD_OK)
      ab_report_list_sort_unique(&kinds);
    for (index = 0; status == ARCHBIRD_OK && index < kinds.count; index++) {
      AbReportStringList routes;
      AbBuffer prefix;
      size_t edge_index;
      ab_report_list_init(&routes, context->engine);
      ab_buffer_init(&prefix, context->engine);
      for (edge_index = 0;
           status == ARCHBIRD_OK && edge_index < context->edge_count;
           edge_index++) {
        const MapReportEdge *edge = &context->edges[edge_index];
        if (ab_string_equal(edge->source, file->path) &&
            ab_string_equal(edge->kind, &kinds.items[index]))
          status = ab_report_list_addf(
              &routes, "%.*s(%zu)", (int)edge->target->length,
              edge->target->data, edge->names->as.array.count);
      }
      if (status == ARCHBIRD_OK)
        ab_report_list_sort(&routes);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(&prefix,
                                   "  %.*s: ", (int)kinds.items[index].length,
                                   kinds.items[index].data);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &routes, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&routes);
    }
    ab_report_list_free(&kinds);
    return status;
  }
}

static ArchbirdStatus map_render_symbols(const MapReportContext *context,
                                         int full, size_t visible_count,
                                         AbBuffer *out) {
  size_t layer_index;
  size_t selected = full ? context->file_count
                    : visible_count < context->compact_file_count
                        ? visible_count
                        : context->compact_file_count;
  if (!full) {
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_literal_line(out, "## Directory bird view"));
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
    if (!context->file_count)
      MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
    else
      MAP_REPORT_TRY(map_render_directories(context, out));
    MAP_REPORT_TRY(ab_report_literal_line(out, "```"));
    if (context->generated_candidate_count ||
        context->third_party_candidate_count) {
      MAP_REPORT_TRY(ab_report_blank(out));
      MAP_REPORT_TRY(ab_report_linef(
          out,
          "Compact candidate collapse: generated=%zu, third-party=%zu. "
          "Canonical Map JSON and focused queries retain every file.",
          context->generated_candidate_count,
          context->third_party_candidate_count));
    }
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_literal_line(
        out, "Expand a directory with `archbird query --map <map.json> --path "
             "<directory>` or the equivalent `--config` query."));
    MAP_REPORT_TRY(ab_report_blank(out));
  }
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Symbol and connection map"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_linef(
      out,
      "Rendered detail for %zu of %zu mapped files; the canonical Map "
      "contains all %zu.",
      selected, context->file_count, context->file_count));
  MAP_REPORT_TRY(ab_report_blank(out));
  for (layer_index = 0; layer_index < context->layers->as.array.count;
       layer_index++) {
    const AbValue *layer = &context->layers->as.array.items[layer_index];
    const AbString *name = ab_report_string(layer, "name");
    size_t file_index;
    size_t emitted = 0;
    if (!name)
      return map_schema_error(context->engine, "map.layers[] is malformed");
    MAP_REPORT_TRY(
        ab_report_linef(out, "### %.*s", (int)name->length, name->data));
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
    for (file_index = 0; file_index < context->file_count; file_index++) {
      const MapReportFile *file = &context->files[file_index];
      AbReportStringList symbols;
      AbBuffer prefix;
      ArchbirdStatus status;
      if (!ab_string_equal(file->layer, name) ||
          (!full && file->rank >= selected))
        continue;
      emitted++;
      ab_report_list_init(&symbols, context->engine);
      ab_buffer_init(&prefix, context->engine);
      status = map_symbol_items(context, file, full, &symbols);
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(
            &prefix, "%.*s symbols(%zu): ", (int)file->path->length,
            file->path->data, file->symbols->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_report_chunks(out, &symbols, (const char *)prefix.data,
                                  MAP_REPORT_WIDTH);
      ab_buffer_free(&prefix);
      ab_report_list_free(&symbols);
      if (status != ARCHBIRD_OK)
        return status;
      MAP_REPORT_TRY(map_render_file_edges(context, file, full, out));
    }
    if (!emitted)
      MAP_REPORT_TRY(ab_report_literal_line(
          out, "none selected within compact output budget"));
    MAP_REPORT_TRY(ab_report_literal_line(out, "```"));
  }
  if (!full && selected < context->file_count) {
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_report_linef(
        out,
        "… %zu lower-ranked files omitted; use `query` for exact local "
        "context or `--full` for complete Markdown.",
        context->file_count - selected));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_join_strings(ArchbirdEngine *engine,
                                       const AbReportStringList *values,
                                       const char *separator, AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(&buffer, separator);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, values->items[index].data,
                                values->items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus map_render_test_full(const MapReportContext *context,
                                           const AbValue *test, AbBuffer *out) {
  const AbString *path = ab_report_string(test, "path");
  const AbString *group = ab_report_string(test, "group");
  const AbString *framework = ab_report_string(test, "framework");
  const AbString *count_unit = ab_report_string(test, "count_unit");
  const AbValue *selectors = map_optional_array(test, "selectors");
  const AbValue *routes = map_optional_object(test, "routes");
  const AbValue *generated_from = map_optional_array(test, "generated_from");
  AbReportStringList details;
  AbReportStringList route_rows;
  AbString selector_text = {0};
  AbString route_text = {0};
  AbString generated_text = {0};
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  if (!path || !group || !framework)
    return map_schema_error(context->engine, "map.tests[] is malformed");
  ab_report_list_init(&details, context->engine);
  ab_report_list_init(&route_rows, context->engine);
  if (selectors->as.array.count) {
    AbReportStringList selector_rows;
    ab_report_list_init(&selector_rows, context->engine);
    status = map_add_array_strings(&selector_rows, selectors);
    if (status == ARCHBIRD_OK)
      status = map_join_strings(context->engine, &selector_rows, ",",
                                &selector_text);
    ab_report_list_free(&selector_rows);
  }
  if (status == ARCHBIRD_OK && !count_unit && selector_text.length)
    status = ab_report_list_addf(
        &details, "%.*s/%.*s; tests=%zu; selectors=%.*s", (int)group->length,
        group->data, (int)framework->length, framework->data,
        ab_report_size(test, "count", 0), (int)selector_text.length,
        selector_text.data);
  else if (status == ARCHBIRD_OK && !count_unit)
    status = ab_report_list_addf(&details, "%.*s/%.*s; tests=%zu",
                                 (int)group->length, group->data,
                                 (int)framework->length, framework->data,
                                 ab_report_size(test, "count", 0));
  else if (status == ARCHBIRD_OK && selector_text.length)
    status = ab_report_list_addf(
        &details,
        "%.*s/%.*s; static_occurrences=%zu; count_unit=%.*s; "
        "suite_selectors=%.*s",
        (int)group->length, group->data, (int)framework->length,
        framework->data, ab_report_size(test, "count", 0),
        (int)(count_unit ? count_unit->length : 7),
        count_unit ? count_unit->data : "unknown", (int)selector_text.length,
        selector_text.data);
  else if (status == ARCHBIRD_OK)
    status = ab_report_list_addf(
        &details, "%.*s/%.*s; static_occurrences=%zu; count_unit=%.*s",
        (int)group->length, group->data, (int)framework->length,
        framework->data, ab_report_size(test, "count", 0),
        (int)(count_unit ? count_unit->length : 7),
        count_unit ? count_unit->data : "unknown");
  if (status == ARCHBIRD_OK && count_unit &&
      ab_report_bool(test, "generated", 0)) {
    AbReportStringList sources;
    ab_report_list_init(&sources, context->engine);
    status = map_add_array_strings(&sources, generated_from);
    if (status == ARCHBIRD_OK)
      status =
          map_join_strings(context->engine, &sources, ",", &generated_text);
    ab_report_list_free(&sources);
    if (status == ARCHBIRD_OK)
      status = ab_report_list_addf(
          &details, "source_kind=generated; generated_from=%.*s",
          (int)(generated_text.length ? generated_text.length : 1),
          generated_text.length ? generated_text.data : "-");
  } else if (status == ARCHBIRD_OK && count_unit) {
    status = ab_report_list_addf(&details, "source_kind=source");
  }
  for (index = 0; status == ARCHBIRD_OK && index < routes->as.object.count;
       index++) {
    const AbObjectField *field = &routes->as.object.fields[index];
    uint64_t count;
    if (!ab_value_u64(&field->value, &count) || count > SIZE_MAX)
      status =
          map_schema_error(context->engine, "test route count is malformed");
    else
      status =
          ab_report_list_addf(&route_rows, "%.*s(%zu)", (int)field->name.length,
                              field->name.data, (size_t)count);
  }
  if (status == ARCHBIRD_OK)
    status = map_join_strings(context->engine, &route_rows, ",", &route_text);
  if (status == ARCHBIRD_OK && count_unit)
    status =
        ab_report_list_addf(&details, "structural_routes=%.*s",
                            (int)(route_text.length ? route_text.length : 1),
                            route_text.length ? route_text.data : "-");
  else if (status == ARCHBIRD_OK)
    status = ab_report_list_addf(
        &details, "-> %.*s", (int)(route_text.length ? route_text.length : 1),
        route_text.length ? route_text.data : "-");
  if (status == ARCHBIRD_OK) {
    AbBuffer prefix;
    ab_buffer_init(&prefix, context->engine);
    status =
        ab_report_appendf(&prefix, "%.*s: ", (int)path->length, path->data);
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &details, (const char *)prefix.data,
                                MAP_REPORT_WIDTH);
    ab_buffer_free(&prefix);
  }
  ab_string_free(context->engine, &route_text);
  ab_string_free(context->engine, &selector_text);
  ab_string_free(context->engine, &generated_text);
  ab_report_list_free(&route_rows);
  ab_report_list_free(&details);
  return status;
}

static ArchbirdStatus map_render_test_group(const MapReportContext *context,
                                            const AbString *group,
                                            AbBuffer *out) {
  AbReportStringList route_names;
  AbReportStringList selectors;
  AbReportStringList case_selectors;
  MapCountedName *ranked = NULL;
  size_t source_file_count = 0;
  size_t generated_file_count = 0;
  size_t source_occurrences = 0;
  size_t generated_occurrences = 0;
  size_t dispatch_entries = 0;
  int evidence_v2 = 0;
  size_t index;
  size_t shown;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&route_names, context->engine);
  ab_report_list_init(&selectors, context->engine);
  ab_report_list_init(&case_selectors, context->engine);
  for (index = 0; index < context->tests->as.array.count; index++) {
    const AbValue *test = &context->tests->as.array.items[index];
    const AbString *test_group = ab_report_string(test, "group");
    const AbValue *routes = map_optional_object(test, "routes");
    const AbValue *cases = map_optional_array(test, "cases");
    int generated = ab_report_bool(test, "generated", 0);
    size_t field_index;
    if (!test_group) {
      status = map_schema_error(context->engine, "map.tests[] is malformed");
      goto cleanup;
    }
    if (!ab_string_equal(test_group, group))
      continue;
    if (ab_report_string(test, "count_unit"))
      evidence_v2 = 1;
    if (generated) {
      generated_file_count++;
      generated_occurrences += ab_report_size(test, "count", 0);
    } else {
      source_file_count++;
      source_occurrences += ab_report_size(test, "count", 0);
    }
    for (field_index = 0; field_index < cases->as.array.count; field_index++) {
      const AbValue *test_case = &cases->as.array.items[field_index];
      const AbString *selector = ab_report_string(test_case, "selector");
      const AbString *evidence_kind =
          ab_report_string(test_case, "evidence_kind");
      if (!selector) {
        status =
            map_schema_error(context->engine, "map test case is malformed");
        goto cleanup;
      }
      if (!generated) {
        status = ab_report_list_add(&case_selectors, selector->data,
                                    selector->length);
        if (status != ARCHBIRD_OK)
          goto cleanup;
      }
      if (evidence_kind &&
          ab_report_string_equal(evidence_kind, "named_dispatch_entry"))
        dispatch_entries++;
    }
    status = map_add_array_strings(&selectors,
                                   map_optional_array(test, "selectors"));
    if (status != ARCHBIRD_OK)
      goto cleanup;
    for (field_index = 0; field_index < routes->as.object.count;
         field_index++) {
      const AbObjectField *field = &routes->as.object.fields[field_index];
      status = ab_report_list_add(&route_names, field->name.data,
                                  field->name.length);
      if (status != ARCHBIRD_OK)
        goto cleanup;
    }
  }
  ab_report_list_sort_unique(&route_names);
  ab_report_list_sort_unique(&selectors);
  ab_report_list_sort_unique(&case_selectors);
  if (route_names.count) {
    ranked = (MapCountedName *)ab_calloc(context->engine, route_names.count,
                                         sizeof(*ranked));
    if (!ranked) {
      status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory ranking test routes");
      goto cleanup;
    }
  }
  for (index = 0; index < route_names.count; index++) {
    size_t test_index;
    ranked[index].name = &route_names.items[index];
    for (test_index = 0; test_index < context->tests->as.array.count;
         test_index++) {
      const AbValue *test = &context->tests->as.array.items[test_index];
      const AbString *test_group = ab_report_string(test, "group");
      const AbValue *routes = map_optional_object(test, "routes");
      size_t field_index;
      if (!test_group || !ab_string_equal(test_group, group))
        continue;
      for (field_index = 0; field_index < routes->as.object.count;
           field_index++) {
        const AbObjectField *field = &routes->as.object.fields[field_index];
        uint64_t count;
        if (!ab_string_equal(&field->name, ranked[index].name))
          continue;
        if (!ab_value_u64(&field->value, &count) || count > SIZE_MAX) {
          status = map_schema_error(context->engine,
                                    "test route count is malformed");
          goto cleanup;
        }
        ranked[index].count += (size_t)count;
      }
    }
  }
  if (route_names.count > 1)
    qsort(ranked, route_names.count, sizeof(*ranked), map_counted_compare);
  shown = route_names.count < context->compact_edges * 2
              ? route_names.count
              : context->compact_edges * 2;
  {
    AbReportStringList route_rows;
    AbReportStringList details;
    AbReportStringList selector_rows;
    AbString route_text = {0};
    AbString selector_text = {0};
    AbBuffer prefix;
    size_t selector_shown = selectors.count < context->compact_edges * 2
                                ? selectors.count
                                : context->compact_edges * 2;
    ab_report_list_init(&route_rows, context->engine);
    ab_report_list_init(&details, context->engine);
    ab_report_list_init(&selector_rows, context->engine);
    ab_buffer_init(&prefix, context->engine);
    for (index = 0; status == ARCHBIRD_OK && index < shown; index++)
      status = ab_report_list_addf(
          &route_rows, "%.*s(%zu)", (int)ranked[index].name->length,
          ranked[index].name->data, ranked[index].count);
    if (status == ARCHBIRD_OK && route_names.count > shown)
      status =
          ab_report_list_addf(&route_rows, "…+%zu", route_names.count - shown);
    if (status == ARCHBIRD_OK)
      status = map_join_strings(context->engine, &route_rows, ",", &route_text);
    for (index = 0; status == ARCHBIRD_OK && index < selector_shown; index++)
      status = ab_report_list_add(&selector_rows, selectors.items[index].data,
                                  selectors.items[index].length);
    if (status == ARCHBIRD_OK && selectors.count > selector_shown)
      status = ab_report_list_addf(&selector_rows, "…+%zu",
                                   selectors.count - selector_shown);
    if (status == ARCHBIRD_OK && selector_rows.count)
      status = map_join_strings(context->engine, &selector_rows, ",",
                                &selector_text);
    if (status == ARCHBIRD_OK && !evidence_v2)
      status = ab_report_list_addf(&details, "files=%zu",
                                   source_file_count + generated_file_count);
    if (status == ARCHBIRD_OK && !evidence_v2)
      status = ab_report_list_addf(&details, "tests=%zu",
                                   source_occurrences + generated_occurrences);
    if (status == ARCHBIRD_OK && evidence_v2)
      status =
          ab_report_list_addf(&details, "source_files=%zu", source_file_count);
    if (status == ARCHBIRD_OK && evidence_v2)
      status = ab_report_list_addf(&details, "generated_files=%zu",
                                   generated_file_count);
    if (status == ARCHBIRD_OK && evidence_v2)
      status = ab_report_list_addf(&details, "source_static_occurrences=%zu",
                                   source_occurrences);
    if (status == ARCHBIRD_OK && evidence_v2)
      status = ab_report_list_addf(&details, "generated_static_occurrences=%zu",
                                   generated_occurrences);
    if (status == ARCHBIRD_OK && evidence_v2)
      status = ab_report_list_addf(&details, "unique_source_case_selectors=%zu",
                                   case_selectors.count);
    if (status == ARCHBIRD_OK && evidence_v2)
      status = ab_report_list_addf(&details, "named_dispatch_entries=%zu",
                                   dispatch_entries);
    if (status == ARCHBIRD_OK && evidence_v2)
      status =
          ab_report_list_addf(&details, "structural_routes=%.*s",
                              (int)(route_text.length ? route_text.length : 1),
                              route_text.length ? route_text.data : "-");
    else if (status == ARCHBIRD_OK)
      status =
          ab_report_list_addf(&details, "routes=%.*s",
                              (int)(route_text.length ? route_text.length : 1),
                              route_text.length ? route_text.data : "-");
    if (status == ARCHBIRD_OK && selector_text.length)
      status =
          ab_report_list_addf(&details, "selectors=%.*s",
                              (int)selector_text.length, selector_text.data);
    if (status == ARCHBIRD_OK)
      status =
          ab_report_appendf(&prefix, "%.*s: ", (int)group->length, group->data);
    if (status == ARCHBIRD_OK)
      status = ab_report_chunks(out, &details, (const char *)prefix.data,
                                MAP_REPORT_WIDTH);
    ab_buffer_free(&prefix);
    ab_string_free(context->engine, &selector_text);
    ab_string_free(context->engine, &route_text);
    ab_report_list_free(&selector_rows);
    ab_report_list_free(&details);
    ab_report_list_free(&route_rows);
  }

cleanup:
  ab_free(context->engine, ranked);
  ab_report_list_free(&case_selectors);
  ab_report_list_free(&selectors);
  ab_report_list_free(&route_names);
  return status;
}

static ArchbirdStatus map_render_tests(const MapReportContext *context,
                                       int full, AbBuffer *out) {
  size_t index;
  int evidence_v2 = 0;
  for (index = 0; index < context->tests->as.array.count; index++)
    if (ab_report_string(&context->tests->as.array.items[index],
                         "count_unit")) {
      evidence_v2 = 1;
      break;
    }
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Test routing"));
  MAP_REPORT_TRY(ab_report_blank(out));
  if (evidence_v2) {
    MAP_REPORT_TRY(ab_report_literal_line(
        out,
        "Static case occurrences and configured structural routes are shown; "
        "they do not claim runtime collection or execution."));
    MAP_REPORT_TRY(ab_report_blank(out));
  }
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  if (!context->tests->as.array.count)
    MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
  else if (full)
    for (index = 0; index < context->tests->as.array.count; index++)
      MAP_REPORT_TRY(map_render_test_full(
          context, &context->tests->as.array.items[index], out));
  else {
    AbReportStringList groups;
    ArchbirdStatus status = ARCHBIRD_OK;
    ab_report_list_init(&groups, context->engine);
    for (index = 0; index < context->tests->as.array.count; index++) {
      const AbString *group =
          ab_report_string(&context->tests->as.array.items[index], "group");
      if (!group) {
        status = map_schema_error(context->engine, "map.tests[] is malformed");
        break;
      }
      status = ab_report_list_add(&groups, group->data, group->length);
      if (status != ARCHBIRD_OK)
        break;
    }
    if (status == ARCHBIRD_OK)
      ab_report_list_sort_unique(&groups);
    for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++)
      status = map_render_test_group(context, &groups.items[index], out);
    ab_report_list_free(&groups);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_diagnostics(const MapReportContext *context,
                                             AbBuffer *out) {
  size_t index;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Diagnostics"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  if (!context->diagnostics->as.array.count)
    MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
  for (index = 0; index < context->diagnostics->as.array.count;) {
    const AbValue *diagnostic = &context->diagnostics->as.array.items[index];
    const AbString *severity = ab_report_string(diagnostic, "severity");
    const AbString *code = ab_report_string(diagnostic, "code");
    const AbString *message = ab_report_string(diagnostic, "message");
    const AbString *path = map_string_or_empty(diagnostic, "path");
    size_t group_end = index + 1;
    if (!severity || !code || !message || !path)
      return map_schema_error(context->engine,
                              "map.diagnostics[] is malformed");
    while (group_end < context->diagnostics->as.array.count) {
      const AbValue *next = &context->diagnostics->as.array.items[group_end];
      const AbString *next_severity = ab_report_string(next, "severity");
      const AbString *next_code = ab_report_string(next, "code");
      const AbString *next_message = ab_report_string(next, "message");
      const AbString *next_path = map_string_or_empty(next, "path");
      if (!next_severity || !next_code || !next_message || !next_path ||
          !ab_string_equal(severity, next_severity) ||
          !ab_string_equal(code, next_code) ||
          !ab_string_equal(message, next_message) ||
          !ab_string_equal(path, next_path))
        break;
      group_end++;
    }
    if (path->length && group_end - index > 1)
      MAP_REPORT_TRY(ab_report_linef(
          out, "%.*s %.*s %.*s: %.*s [evidence=%zu spans]",
          (int)severity->length, severity->data, (int)code->length, code->data,
          (int)path->length, path->data, (int)message->length, message->data,
          group_end - index));
    else if (path->length)
      MAP_REPORT_TRY(ab_report_linef(
          out, "%.*s %.*s %.*s: %.*s", (int)severity->length, severity->data,
          (int)code->length, code->data, (int)path->length, path->data,
          (int)message->length, message->data));
    else if (group_end - index > 1)
      MAP_REPORT_TRY(ab_report_linef(
          out, "%.*s %.*s: %.*s [evidence=%zu spans]", (int)severity->length,
          severity->data, (int)code->length, code->data, (int)message->length,
          message->data, group_end - index));
    else
      MAP_REPORT_TRY(ab_report_linef(
          out, "%.*s %.*s: %.*s", (int)severity->length, severity->data,
          (int)code->length, code->data, (int)message->length, message->data));
    index = group_end;
  }
  MAP_REPORT_TRY(ab_report_literal_line(out, "```"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Determinism contract"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(
      out,
      "Files, symbols, call resolutions, provider surfaces, edges, "
      "entrypoints, artifacts, components, parity values, tests, and "
      "diagnostics are sorted. The input digest covers mapped sources, public "
      "headers, manifests, build files, tests, artifact inputs, and loaders "
      "by relative path and content hash. Re-running the same tool "
      "implementation digest with identical config and source bytes produces "
      "byte-identical output."));
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_brief_identity(const MapReportContext *context,
                                                ArchbirdReportDetail detail,
                                                AbBuffer *out) {
  const AbString *project = ab_report_string(context->map, "project");
  const AbString *description = ab_report_string(context->map, "description");
  const AbValue *tool = ab_report_object(context->map, "tool");
  const AbValue *evidence = ab_report_object(context->map, "evidence");
  const AbString *inputs = ab_report_string(evidence, "input_sha256");
  size_t symbols = 0;
  size_t tests = 0;
  size_t warnings = 0;
  size_t errors = 0;
  size_t index;
  if (!project || !tool || !evidence || !inputs || inputs->length < 16)
    return map_schema_error(context->engine,
                            "map report identity/evidence is malformed");
  for (index = 0; index < context->file_count; index++)
    symbols += context->files[index].symbols->as.array.count;
  for (index = 0; index < context->tests->as.array.count; index++)
    tests += ab_report_size(&context->tests->as.array.items[index], "count", 0);
  for (index = 0; index < context->diagnostics->as.array.count; index++) {
    const AbString *severity = ab_report_string(
        &context->diagnostics->as.array.items[index], "severity");
    errors += ab_report_string_equal(severity, "error");
    warnings += ab_report_string_equal(severity, "warning");
  }
  MAP_REPORT_TRY(ab_report_linef(out, "# %.*s architecture",
                                 (int)project->length, project->data));
  MAP_REPORT_TRY(ab_report_blank(out));
  if (description && description->length) {
    MAP_REPORT_TRY(ab_report_line(out, description->data, description->length));
    MAP_REPORT_TRY(ab_report_blank(out));
  }
  MAP_REPORT_TRY(ab_report_linef(
      out,
      "%zu source file%s · %zu symbol%s · %zu package%s · %zu component%s · "
      "%zu static test occurrence%s · %zu error%s · %zu warning%s",
      context->file_count, context->file_count == 1 ? "" : "s", symbols,
      symbols == 1 ? "" : "s", context->packages->as.array.count,
      context->packages->as.array.count == 1 ? "" : "s",
      context->components->as.array.count,
      context->components->as.array.count == 1 ? "" : "s", tests,
      tests == 1 ? "" : "s", errors, errors == 1 ? "" : "s", warnings,
      warnings == 1 ? "" : "s"));
  if (detail != ARCHBIRD_REPORT_DETAIL_COMPACT) {
    MAP_REPORT_TRY(ab_report_blank(out));
    MAP_REPORT_TRY(ab_buffer_literal(out, "Map `"));
    MAP_REPORT_TRY(ab_buffer_append(out, inputs->data, 16));
    MAP_REPORT_TRY(ab_buffer_literal(out, "` · "));
    MAP_REPORT_TRY(map_tool_label(out, tool));
    MAP_REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
map_render_populated_layers(const MapReportContext *context, AbBuffer *out) {
  size_t layer_index;
  int emitted = 0;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Languages"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (layer_index = 0; layer_index < context->layers->as.array.count;
       layer_index++) {
    const AbValue *layer = &context->layers->as.array.items[layer_index];
    const AbString *name = ab_report_string(layer, "name");
    const AbString *language = ab_report_string(layer, "language");
    size_t files = 0;
    size_t symbols = 0;
    size_t file_index;
    if (!name || !language)
      return map_schema_error(context->engine, "map.layers[] is malformed");
    for (file_index = 0; file_index < context->file_count; file_index++)
      if (ab_string_equal(context->files[file_index].layer, name)) {
        files++;
        symbols += context->files[file_index].symbols->as.array.count;
      }
    if (!files)
      continue;
    emitted = 1;
    MAP_REPORT_TRY(ab_report_linef(out, "%.*s files=%zu symbols=%zu layer=%.*s",
                                   (int)language->length, language->data, files,
                                   symbols, (int)name->length, name->data));
  }
  if (!emitted)
    MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_key_files(const MapReportContext *context,
                                           ArchbirdReportDetail detail,
                                           size_t standard_limit,
                                           int include_edges, AbBuffer *out) {
  size_t limit =
      detail == ARCHBIRD_REPORT_DETAIL_COMPACT
          ? (standard_limit + 1) / 2
          : (detail == ARCHBIRD_REPORT_DETAIL_FULL ? context->file_count
                                                   : standard_limit);
  size_t index;
  size_t emitted = 0;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Key files and symbols"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->file_count && emitted < limit; index++) {
    const MapReportFile *file = context->ranked[index];
    AbReportStringList symbols;
    AbBuffer prefix;
    ArchbirdStatus status;
    if (file->collapsed_candidate)
      continue;
    ab_report_list_init(&symbols, context->engine);
    ab_buffer_init(&prefix, context->engine);
    status = map_symbol_items(context, file,
                              detail == ARCHBIRD_REPORT_DETAIL_FULL, &symbols);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(
          &prefix, "%.*s symbols(%zu): ", (int)file->path->length,
          file->path->data, file->symbols->as.array.count);
    if (status == ARCHBIRD_OK)
      status = symbols.count
                   ? ab_report_chunks(out, &symbols, (const char *)prefix.data,
                                      MAP_REPORT_WIDTH)
                   : ab_report_linef(out, "%.*s symbols(0)",
                                     (int)file->path->length, file->path->data);
    ab_buffer_free(&prefix);
    ab_report_list_free(&symbols);
    if (status != ARCHBIRD_OK)
      return status;
    if (include_edges)
      MAP_REPORT_TRY(map_render_file_edges(
          context, file, detail == ARCHBIRD_REPORT_DETAIL_FULL, out));
    emitted++;
  }
  if (!emitted)
    MAP_REPORT_TRY(ab_report_literal_line(out, "none"));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```"));
  if (emitted < context->compact_file_count)
    MAP_REPORT_TRY(ab_report_linef(
        out,
        "%zu lower-ranked files are available through `query` or a fuller "
        "detail level.",
        context->compact_file_count - emitted));
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_test_summary(const MapReportContext *context,
                                              AbBuffer *out) {
  AbReportStringList groups;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context->tests->as.array.count)
    return ARCHBIRD_OK;
  ab_report_list_init(&groups, context->engine);
  for (index = 0;
       status == ARCHBIRD_OK && index < context->tests->as.array.count;
       index++) {
    const AbValue *test = &context->tests->as.array.items[index];
    const AbString *group = ab_report_string(test, "group");
    if (!group)
      status = map_schema_error(context->engine, "map.tests[] is malformed");
    else
      status = ab_report_list_add(&groups, group->data, group->length);
  }
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(&groups);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "## Tests");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```text");
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
    size_t test_index;
    size_t files = 0;
    size_t occurrences = 0;
    size_t selectors = 0;
    for (test_index = 0; test_index < context->tests->as.array.count;
         test_index++) {
      const AbValue *test = &context->tests->as.array.items[test_index];
      const AbString *group = ab_report_string(test, "group");
      const AbValue *cases = map_optional_array(test, "cases");
      if (!group || !ab_string_equal(group, &groups.items[index]))
        continue;
      files++;
      occurrences += ab_report_size(test, "count", 0);
      selectors += cases->as.array.count;
    }
    status = ab_report_linef(out,
                             "%.*s files=%zu static_occurrences=%zu "
                             "case_selectors=%zu",
                             (int)groups.items[index].length,
                             groups.items[index].data, files, occurrences,
                             selectors);
  }
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```");
  ab_report_list_free(&groups);
  return status;
}

static ArchbirdStatus
map_render_entrypoint_summary(const MapReportContext *context,
                              ArchbirdReportDetail detail, AbBuffer *out) {
  size_t index;
  size_t limit = detail == ARCHBIRD_REPORT_DETAIL_COMPACT ? 4 : 8;
  if (!context->packages->as.array.count && !context->builds->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Entrypoints"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```text"));
  for (index = 0; index < context->packages->as.array.count; index++) {
    const AbValue *package = &context->packages->as.array.items[index];
    const AbString *identity = ab_report_string(package, "identity");
    const AbString *version = map_string_or_empty(package, "version");
    const AbValue *entrypoints = map_optional_object(package, "entrypoints");
    const AbValue *exports = map_optional_array(package, "exports");
    AbReportStringList paths;
    AbReportStringList names;
    size_t field;
    ArchbirdStatus status = ARCHBIRD_OK;
    if (!identity || !version || !map_string_array_valid(exports))
      return map_schema_error(context->engine, "map.packages[] is malformed");
    ab_report_list_init(&paths, context->engine);
    ab_report_list_init(&names, context->engine);
    for (field = 0;
         status == ARCHBIRD_OK && field < entrypoints->as.object.count;
         field++) {
      const AbValue *value = &entrypoints->as.object.fields[field].value;
      if (value->kind != AB_VALUE_STRING) {
        status = map_schema_error(context->engine,
                                  "map package entrypoint is malformed");
        break;
      }
      status = ab_report_list_add(&paths, value->as.text.data,
                                  value->as.text.length);
    }
    if (status == ARCHBIRD_OK)
      ab_report_list_sort_unique(&paths);
    if (status == ARCHBIRD_OK)
      status = map_limited_values(context->engine, exports, limit, &names);
    if (status == ARCHBIRD_OK)
      status =
          ab_report_appendf(out, "%.*s", (int)identity->length, identity->data);
    if (status == ARCHBIRD_OK && version->length)
      status =
          ab_report_appendf(out, "@%.*s", (int)version->length, version->data);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(out, " entries=%zu exports=%zu\n", paths.count,
                                 exports->as.array.count);
    if (status == ARCHBIRD_OK && paths.count)
      status = ab_report_chunks(out, &paths, "  paths: ", MAP_REPORT_WIDTH);
    if (status == ARCHBIRD_OK && names.count)
      status = ab_report_chunks(out, &names, "  public: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&names);
    ab_report_list_free(&paths);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (context->builds->as.array.count) {
    AbReportStringList names;
    ArchbirdStatus status = ARCHBIRD_OK;
    ab_report_list_init(&names, context->engine);
    for (index = 0; status == ARCHBIRD_OK &&
                    index < context->builds->as.array.count && index < limit;
         index++) {
      const AbString *name =
          ab_report_string(&context->builds->as.array.items[index], "name");
      if (!name) {
        status = map_schema_error(context->engine, "map.builds[] is malformed");
        break;
      }
      status = ab_report_list_add(&names, name->data, name->length);
    }
    if (status == ARCHBIRD_OK && context->builds->as.array.count > limit)
      status = ab_report_list_addf(&names, "…+%zu",
                                   context->builds->as.array.count - limit);
    if (status == ARCHBIRD_OK)
      status =
          ab_report_chunks(out, &names, "build routes: ", MAP_REPORT_WIDTH);
    ab_report_list_free(&names);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_findings(const MapReportContext *context,
                                          AbBuffer *out) {
  size_t index;
  if (!context->diagnostics->as.array.count)
    return ARCHBIRD_OK;
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Findings"));
  MAP_REPORT_TRY(ab_report_blank(out));
  for (index = 0; index < context->diagnostics->as.array.count;) {
    const AbValue *row = &context->diagnostics->as.array.items[index];
    const AbString *severity = ab_report_string(row, "severity");
    const AbString *code = ab_report_string(row, "code");
    const AbString *message = ab_report_string(row, "message");
    const AbString *path = map_string_or_empty(row, "path");
    size_t end = index + 1;
    if (!severity || !code || !message || !path)
      return map_schema_error(context->engine,
                              "map.diagnostics[] is malformed");
    while (end < context->diagnostics->as.array.count) {
      const AbValue *next = &context->diagnostics->as.array.items[end];
      const AbString *next_severity = ab_report_string(next, "severity");
      const AbString *next_code = ab_report_string(next, "code");
      const AbString *next_message = ab_report_string(next, "message");
      const AbString *next_path = map_string_or_empty(next, "path");
      if (!next_severity || !next_code || !next_message || !next_path ||
          !ab_string_equal(severity, next_severity) ||
          !ab_string_equal(code, next_code) ||
          !ab_string_equal(message, next_message) ||
          !ab_string_equal(path, next_path))
        break;
      end++;
    }
    MAP_REPORT_TRY(ab_report_appendf(out, "- %.*s `%.*s`",
                                     (int)severity->length, severity->data,
                                     (int)code->length, code->data));
    if (path->length)
      MAP_REPORT_TRY(
          ab_report_appendf(out, " `%.*s`", (int)path->length, path->data));
    MAP_REPORT_TRY(
        ab_report_appendf(out, ": %.*s", (int)message->length, message->data));
    if (end - index > 1)
      MAP_REPORT_TRY(
          ab_report_appendf(out, " (%zu evidence spans)", end - index));
    MAP_REPORT_TRY(ab_buffer_literal(out, "\n"));
    index = end;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_render_explore(AbBuffer *out) {
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "## Explore"));
  MAP_REPORT_TRY(ab_report_blank(out));
  MAP_REPORT_TRY(ab_report_literal_line(out, "```bash"));
  MAP_REPORT_TRY(ab_report_literal_line(out, "archbird query --symbol <name>"));
  MAP_REPORT_TRY(ab_report_literal_line(out, "archbird impact --path <path>"));
  MAP_REPORT_TRY(ab_report_literal_line(out, "archbird serve"));
  return ab_report_literal_line(out, "```");
}

static ArchbirdStatus map_render_overview(const MapReportContext *context,
                                          ArchbirdReportDetail detail,
                                          AbBuffer *out) {
  MAP_REPORT_TRY(map_render_brief_identity(context, detail, out));
  if (context->packages->as.array.count || context->builds->as.array.count)
    MAP_REPORT_TRY(map_render_entrypoint_summary(context, detail, out));
  MAP_REPORT_TRY(map_render_key_files(context, detail, 12, 0, out));
  MAP_REPORT_TRY(map_render_package_edges(context, "external",
                                          "## External dependencies", out));
  MAP_REPORT_TRY(map_render_test_summary(context, out));
  MAP_REPORT_TRY(map_render_findings(context, out));
  return map_render_explore(out);
}

static ArchbirdStatus
map_render_architecture_view(const MapReportContext *context,
                             ArchbirdReportDetail detail, AbBuffer *out) {
  int full = detail == ARCHBIRD_REPORT_DETAIL_FULL;
  MAP_REPORT_TRY(map_render_brief_identity(context, detail, out));
  MAP_REPORT_TRY(map_render_populated_layers(context, out));
  if (context->components->as.array.count)
    MAP_REPORT_TRY(map_render_components(context, full, out));
  if (context->surfaces->as.array.count)
    MAP_REPORT_TRY(map_render_surfaces(context, full, out));
  if (context->packages->as.array.count || context->builds->as.array.count)
    MAP_REPORT_TRY(map_render_entrypoints(context, full, out));
  MAP_REPORT_TRY(map_render_bridges(context, full, out));
  MAP_REPORT_TRY(map_render_package_edges(context, "external",
                                          "## External dependencies", out));
  MAP_REPORT_TRY(map_render_package_edges(context, "package-local",
                                          "## Local dependencies", out));
  MAP_REPORT_TRY(map_render_key_files(context, detail, 24, 1, out));
  MAP_REPORT_TRY(map_render_test_summary(context, out));
  MAP_REPORT_TRY(map_render_findings(context, out));
  return map_render_explore(out);
}

static ArchbirdStatus map_render_once(const MapReportContext *context, int full,
                                      size_t visible_count, AbBuffer *out) {
  MAP_REPORT_TRY(map_render_header(context, out));
  MAP_REPORT_TRY(map_render_surfaces(context, full, out));
  MAP_REPORT_TRY(map_render_components(context, full, out));
  MAP_REPORT_TRY(map_render_entrypoints(context, full, out));
  MAP_REPORT_TRY(map_render_indexes(context, out));
  MAP_REPORT_TRY(map_render_artifacts(context, out));
  MAP_REPORT_TRY(map_render_bridges(context, full, out));
  MAP_REPORT_TRY(map_render_package_edges(
      context, "external", "## External package dependencies", out));
  MAP_REPORT_TRY(map_render_package_edges(context, "package-local",
                                          "## Local package references", out));
  MAP_REPORT_TRY(map_render_resolutions(context, full, out));
  MAP_REPORT_TRY(map_render_named_entries(context, out));
  MAP_REPORT_TRY(map_render_parity(context, full, out));
  MAP_REPORT_TRY(map_render_symbols(context, full, visible_count, out));
  MAP_REPORT_TRY(map_render_tests(context, full, out));
  return map_render_diagnostics(context, out);
}

typedef enum MapBudgetSectionId {
  MAP_BUDGET_LAYERS = 0,
  MAP_BUDGET_SURFACES,
  MAP_BUDGET_COMPONENTS,
  MAP_BUDGET_ENTRYPOINTS,
  MAP_BUDGET_INDEXES,
  MAP_BUDGET_ARTIFACTS,
  MAP_BUDGET_BRIDGES,
  MAP_BUDGET_EXTERNAL_PACKAGES,
  MAP_BUDGET_LOCAL_PACKAGES,
  MAP_BUDGET_RESOLUTIONS,
  MAP_BUDGET_NAMED_ENTRIES,
  MAP_BUDGET_PARITY,
  MAP_BUDGET_SYMBOLS,
  MAP_BUDGET_TESTS,
  MAP_BUDGET_DIAGNOSTICS,
  MAP_BUDGET_SECTION_COUNT
} MapBudgetSectionId;

typedef struct MapBudgetSection {
  const char *label;
  AbBuffer content;
  int selected;
} MapBudgetSection;

static void map_budget_sections_init(MapBudgetSection *sections,
                                     ArchbirdEngine *engine) {
  static const char *const labels[MAP_BUDGET_SECTION_COUNT] = {
      "Layer inventory",
      "Provider surfaces",
      "Architecture components",
      "Entrypoints and build routes",
      "Precision indexes",
      "Executable artifacts",
      "Cross-layer bridges",
      "External package dependencies",
      "Local package references",
      "Call resolution coverage",
      "Named entrypoints",
      "Parity surfaces",
      "Directory, symbol, and connection map",
      "Test routing",
      "Diagnostics and determinism contract"};
  size_t index;
  memset(sections, 0, MAP_BUDGET_SECTION_COUNT * sizeof(*sections));
  for (index = 0; index < MAP_BUDGET_SECTION_COUNT; index++) {
    sections[index].label = labels[index];
    ab_buffer_init(&sections[index].content, engine);
  }
}

static void map_budget_sections_free(MapBudgetSection *sections) {
  size_t index;
  for (index = 0; index < MAP_BUDGET_SECTION_COUNT; index++)
    ab_buffer_free(&sections[index].content);
}

static ArchbirdStatus
map_budget_sections_render(const MapReportContext *context,
                           MapBudgetSection *sections) {
  MAP_REPORT_TRY(
      map_render_layers(context, &sections[MAP_BUDGET_LAYERS].content));
  MAP_REPORT_TRY(
      map_render_surfaces(context, 0, &sections[MAP_BUDGET_SURFACES].content));
  MAP_REPORT_TRY(map_render_components(
      context, 0, &sections[MAP_BUDGET_COMPONENTS].content));
  MAP_REPORT_TRY(map_render_entrypoints(
      context, 0, &sections[MAP_BUDGET_ENTRYPOINTS].content));
  MAP_REPORT_TRY(
      map_render_indexes(context, &sections[MAP_BUDGET_INDEXES].content));
  MAP_REPORT_TRY(
      map_render_artifacts(context, &sections[MAP_BUDGET_ARTIFACTS].content));
  MAP_REPORT_TRY(
      map_render_bridges(context, 0, &sections[MAP_BUDGET_BRIDGES].content));
  MAP_REPORT_TRY(map_render_package_edges(
      context, "external", "## External package dependencies",
      &sections[MAP_BUDGET_EXTERNAL_PACKAGES].content));
  MAP_REPORT_TRY(map_render_package_edges(
      context, "package-local", "## Local package references",
      &sections[MAP_BUDGET_LOCAL_PACKAGES].content));
  MAP_REPORT_TRY(map_render_resolutions(
      context, 0, &sections[MAP_BUDGET_RESOLUTIONS].content));
  MAP_REPORT_TRY(map_render_named_entries(
      context, &sections[MAP_BUDGET_NAMED_ENTRIES].content));
  MAP_REPORT_TRY(
      map_render_parity(context, 0, &sections[MAP_BUDGET_PARITY].content));
  MAP_REPORT_TRY(
      map_render_symbols(context, 0, 0, &sections[MAP_BUDGET_SYMBOLS].content));
  MAP_REPORT_TRY(
      map_render_tests(context, 0, &sections[MAP_BUDGET_TESTS].content));
  return map_render_diagnostics(context,
                                &sections[MAP_BUDGET_DIAGNOSTICS].content);
}

static ArchbirdStatus
map_budget_projection_summary(const MapReportContext *context,
                              const MapBudgetSection *sections, size_t budget,
                              size_t visible_count, AbBuffer *out) {
  AbReportStringList omitted;
  size_t section_count = 0;
  size_t selected_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&omitted, context->engine);
  for (index = 0; index < MAP_BUDGET_SECTION_COUNT; index++) {
    if (!sections[index].content.length)
      continue;
    section_count++;
    if (sections[index].selected)
      selected_count++;
    else {
      status = ab_report_list_add(&omitted, sections[index].label,
                                  strlen(sections[index].label));
      if (status != ARCHBIRD_OK)
        goto cleanup;
    }
  }
  status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "## Compact projection");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "Budget=%zu characters; sections=%zu/%zu; ranked-file blocks=%zu/%zu.",
        budget, selected_count, section_count, visible_count,
        context->compact_file_count);
  if (status == ARCHBIRD_OK && omitted.count)
    status = ab_report_chunks(out, &omitted,
                              "Omitted complete sections: ", MAP_REPORT_WIDTH);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(
        out,
        "The canonical Map IR remains complete. Use `query` for exact local "
        "context, increase `--max-chars`, or use `--full` for complete "
        "Markdown.");

cleanup:
  ab_report_list_free(&omitted);
  return status;
}

static ArchbirdStatus map_budget_render_candidate(
    const MapReportContext *context, const AbBuffer *identity,
    const MapBudgetSection *sections, const AbBuffer *symbols_override,
    size_t budget, size_t visible_count, AbBuffer *out) {
  size_t index;
  ArchbirdStatus status =
      ab_buffer_append(out, identity->data, identity->length);
  for (index = 0; status == ARCHBIRD_OK && index < MAP_BUDGET_SECTION_COUNT;
       index++) {
    const AbBuffer *content = index == MAP_BUDGET_SYMBOLS && symbols_override
                                  ? symbols_override
                                  : &sections[index].content;
    if (sections[index].selected)
      status = ab_buffer_append(out, content->data, content->length);
  }
  if (status == ARCHBIRD_OK)
    status = map_budget_projection_summary(context, sections, budget,
                                           visible_count, out);
  return status;
}

static ArchbirdStatus map_budget_candidate_length(
    const MapReportContext *context, const AbBuffer *identity,
    const MapBudgetSection *sections, const AbBuffer *symbols_override,
    size_t budget, size_t visible_count, size_t *out_length) {
  AbBuffer candidate;
  ArchbirdStatus status;
  ab_buffer_init(&candidate, context->engine);
  status =
      map_budget_render_candidate(context, identity, sections, symbols_override,
                                  budget, visible_count, &candidate);
  if (status == ARCHBIRD_OK)
    *out_length = ab_report_codepoints(candidate.data, candidate.length);
  ab_buffer_free(&candidate);
  return status;
}

static ArchbirdStatus map_render_budgeted(const MapReportContext *context,
                                          size_t budget, AbBuffer *out) {
  static const MapBudgetSectionId selection_order[] = {
      MAP_BUDGET_DIAGNOSTICS,    MAP_BUDGET_LAYERS,
      MAP_BUDGET_COMPONENTS,     MAP_BUDGET_SURFACES,
      MAP_BUDGET_SYMBOLS,        MAP_BUDGET_ENTRYPOINTS,
      MAP_BUDGET_BRIDGES,        MAP_BUDGET_RESOLUTIONS,
      MAP_BUDGET_TESTS,          MAP_BUDGET_EXTERNAL_PACKAGES,
      MAP_BUDGET_LOCAL_PACKAGES, MAP_BUDGET_PARITY,
      MAP_BUDGET_NAMED_ENTRIES,  MAP_BUDGET_INDEXES,
      MAP_BUDGET_ARTIFACTS};
  MapBudgetSection sections[MAP_BUDGET_SECTION_COUNT];
  AbBuffer identity;
  AbBuffer symbols;
  size_t order_index;
  size_t minimum_length = 0;
  size_t visible_count = 0;
  ArchbirdStatus status;
  map_budget_sections_init(sections, context->engine);
  ab_buffer_init(&identity, context->engine);
  ab_buffer_init(&symbols, context->engine);
  status = map_render_identity(context, &identity);
  if (status == ARCHBIRD_OK)
    status = map_budget_sections_render(context, sections);
  if (status == ARCHBIRD_OK)
    status = map_budget_candidate_length(context, &identity, sections, NULL,
                                         budget, 0, &minimum_length);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  if (minimum_length > budget) {
    status = archbird_error_set(
        context->engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "map.max_chars: %zu is too small; minimum navigable projection is %zu",
        budget, minimum_length);
    goto cleanup;
  }
  for (order_index = 0;
       order_index < sizeof(selection_order) / sizeof(selection_order[0]);
       order_index++) {
    MapBudgetSectionId section = selection_order[order_index];
    size_t length = 0;
    if (!sections[section].content.length)
      continue;
    sections[section].selected = 1;
    status = map_budget_candidate_length(context, &identity, sections, NULL,
                                         budget, 0, &length);
    if (status != ARCHBIRD_OK)
      goto cleanup;
    if (length > budget)
      sections[section].selected = 0;
  }
  if (sections[MAP_BUDGET_SYMBOLS].selected && context->compact_file_count) {
    size_t low = 1;
    size_t high = context->compact_file_count;
    while (low <= high) {
      size_t middle = low + (high - low) / 2;
      size_t length = 0;
      ab_buffer_free(&symbols);
      ab_buffer_init(&symbols, context->engine);
      status = map_render_symbols(context, 0, middle, &symbols);
      if (status == ARCHBIRD_OK)
        status = map_budget_candidate_length(context, &identity, sections,
                                             &symbols, budget, middle, &length);
      if (status != ARCHBIRD_OK)
        goto cleanup;
      if (length <= budget) {
        visible_count = middle;
        low = middle + 1;
      } else {
        if (!middle)
          break;
        high = middle - 1;
      }
    }
    ab_buffer_free(&symbols);
    ab_buffer_init(&symbols, context->engine);
    status = map_render_symbols(context, 0, visible_count, &symbols);
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  status = map_budget_render_candidate(context, &identity, sections,
                                       sections[MAP_BUDGET_SYMBOLS].selected &&
                                               context->compact_file_count
                                           ? &symbols
                                           : NULL,
                                       budget, visible_count, out);

cleanup:
  ab_buffer_free(&symbols);
  ab_buffer_free(&identity);
  map_budget_sections_free(sections);
  return status;
}

ArchbirdStatus ab_map_report_markdown(ArchbirdEngine *engine,
                                      const AbValue *map, int full,
                                      size_t max_chars, AbBuffer *out) {
  MapReportContext context;
  size_t low;
  size_t high;
  size_t best_count = SIZE_MAX;
  size_t budget;
  ArchbirdStatus status;
  if (!engine || !map || !out || (full != 0 && full != 1))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (full && max_chars)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "map.max_chars: cannot be combined with full output");
  status = map_context_build(&context, engine, map);
  if (status != ARCHBIRD_OK)
    return status;
  if (full) {
    status = map_render_once(&context, 1, context.file_count, out);
    map_context_free(&context);
    return status;
  }
  budget = max_chars ? max_chars : MAP_REPORT_DEFAULT_BUDGET;
  /* Explicit budgets and large maps use the section-aware renderer directly.
     Re-rendering every non-symbol section for each ranked-file binary-search
     probe is quadratic in practice for large call/test inventories, even
     though only the symbol block changes between probes. */
  if (max_chars || context.compact_file_count > 512) {
    status = map_render_budgeted(&context, budget, out);
    map_context_free(&context);
    return status;
  }
  low = 0;
  high = context.compact_file_count;
  while (low <= high) {
    size_t middle = low + (high - low) / 2;
    AbBuffer candidate;
    size_t length;
    ab_buffer_init(&candidate, engine);
    status = map_render_once(&context, 0, middle, &candidate);
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&candidate);
      map_context_free(&context);
      return status;
    }
    length = ab_report_codepoints(candidate.data, candidate.length);
    ab_buffer_free(&candidate);
    if (length <= budget) {
      best_count = middle;
      low = middle + 1;
    } else {
      if (!middle)
        break;
      high = middle - 1;
    }
  }
  if (best_count == SIZE_MAX) {
    status = map_render_budgeted(&context, budget, out);
  } else {
    status = map_render_once(&context, 0, best_count, out);
  }
  map_context_free(&context);
  return status;
}

ArchbirdStatus ab_map_report_markdown_view(ArchbirdEngine *engine,
                                           const AbValue *map,
                                           ArchbirdMapView view,
                                           ArchbirdReportDetail detail,
                                           size_t max_chars, AbBuffer *out) {
  MapReportContext context;
  ArchbirdStatus status;
  if (!engine || !map || !out || view < ARCHBIRD_MAP_VIEW_OVERVIEW ||
      view > ARCHBIRD_MAP_VIEW_AUDIT ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (detail == ARCHBIRD_REPORT_DETAIL_FULL && max_chars)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "map.max_chars: cannot be combined with full detail");
  if (view == ARCHBIRD_MAP_VIEW_AUDIT)
    return ab_map_report_markdown(
        engine, map, detail == ARCHBIRD_REPORT_DETAIL_FULL,
        max_chars ? max_chars
                  : (detail == ARCHBIRD_REPORT_DETAIL_COMPACT ? 12000 : 0),
        out);
  status = map_context_build(&context, engine, map);
  if (status != ARCHBIRD_OK)
    return status;
  status = view == ARCHBIRD_MAP_VIEW_OVERVIEW
               ? map_render_overview(&context, detail, out)
               : map_render_architecture_view(&context, detail, out);
  if (status == ARCHBIRD_OK && max_chars &&
      ab_report_codepoints(out->data, out->length) > max_chars)
    status = archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "map.max_chars: selected %s/%s projection requires %zu characters; "
        "choose compact detail or increase the limit",
        view == ARCHBIRD_MAP_VIEW_OVERVIEW ? "overview" : "architecture",
        detail == ARCHBIRD_REPORT_DETAIL_COMPACT ? "compact" : "standard",
        ab_report_codepoints(out->data, out->length));
  map_context_free(&context);
  return status;
}
