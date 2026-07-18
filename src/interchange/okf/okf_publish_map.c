#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAP_TRY(expression)                                                    \
  do {                                                                         \
    ArchbirdStatus map_status_ = (expression);                                 \
    if (map_status_ != ARCHBIRD_OK)                                            \
      return map_status_;                                                      \
  } while (0)

typedef struct MapPath {
  AbString name;
  AbString slug;
  AbString path;
} MapPath;

typedef struct MapPathSet {
  MapPath *items;
  size_t count;
} MapPathSet;

typedef struct ComponentRoute {
  AbString source;
  AbString target;
  AbString kind;
  AbString *names;
  size_t name_count;
  size_t name_capacity;
} ComponentRoute;

typedef struct ComponentRoutes {
  ComponentRoute *items;
  size_t count;
  size_t capacity;
} ComponentRoutes;

static const AbValue *row_by_name(const AbValue *rows, const AbString *name,
                                  const char *field);
static int string_array_contains(const AbValue *rows, const AbString *value);
static int bool_value(const AbValue *object, const char *name, int *out);
static ArchbirdStatus string_set_add(AbOkfPublication *pub, AbString **items,
                                     size_t *count, size_t *capacity,
                                     const AbString *value);
static void string_set_free(AbOkfPublication *pub, AbString *items,
                            size_t count);

static const AbValue EMPTY_ARRAY = {.kind = AB_VALUE_ARRAY};
static const AbString EMPTY_TEXT = {(char *)"", 0};

static const AbValue *array_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return !value ? &EMPTY_ARRAY : value->kind == AB_VALUE_ARRAY ? value : NULL;
}

static const AbString *text_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return !value                           ? &EMPTY_TEXT
         : value->kind == AB_VALUE_STRING ? &value->as.text
                                          : NULL;
}

static int path_name_compare(const void *left, const void *right) {
  return ab_string_compare(&((const MapPath *)left)->name,
                           &((const MapPath *)right)->name);
}

static int route_compare(const void *left, const void *right) {
  const ComponentRoute *a = (const ComponentRoute *)left;
  const ComponentRoute *b = (const ComponentRoute *)right;
  int compared = ab_string_compare(&a->source, &b->source);
  if (!compared)
    compared = ab_string_compare(&a->target, &b->target);
  if (!compared)
    compared = ab_string_compare(&a->kind, &b->kind);
  return compared;
}

static int string_compare(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static void path_set_free(AbOkfPublication *pub, MapPathSet *set) {
  size_t index;
  if (!set)
    return;
  for (index = 0; index < set->count; index++) {
    ab_string_free(pub->engine, &set->items[index].slug);
    ab_string_free(pub->engine, &set->items[index].path);
  }
  ab_free(pub->engine, set->items);
  memset(set, 0, sizeof(*set));
}

static const MapPath *path_find(const MapPathSet *set, const AbString *name) {
  MapPath key = {0};
  if (!set || !set->count || !name)
    return NULL;
  key.name = *name;
  return (const MapPath *)bsearch(&key, set->items, set->count,
                                  sizeof(*set->items), path_name_compare);
}

static ArchbirdStatus path_value(AbOkfPublication *pub, MapPath *row,
                                 const char *prefix) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, row->slug.data, row->slug.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, &row->path);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus path_collision_suffix(AbOkfPublication *pub,
                                            MapPath *row) {
  char digest[65];
  AbBuffer buffer;
  ArchbirdStatus status = ab_okf_pub_sha256((const uint8_t *)row->name.data,
                                            row->name.length, digest);
  ab_buffer_init(&buffer, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, row->slug.data, row->slug.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "-");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, digest, 12);
  if (status == ARCHBIRD_OK) {
    ab_string_free(pub->engine, &row->slug);
    status = ab_okf_pub_buffer_string(pub, &buffer, &row->slug);
  }
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus paths_from_names(AbOkfPublication *pub,
                                       const AbString *names, size_t count,
                                       const char *prefix, MapPathSet *out) {
  size_t index;
  size_t other;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  if (!count)
    return ARCHBIRD_OK;
  if (count > SIZE_MAX / sizeof(*out->items) ||
      count > pub->engine->options.max_values)
    return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many OKF path names");
  out->items = (MapPath *)ab_calloc(pub->engine, count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building OKF paths");
  out->count = count;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    out->items[index].name = names[index];
    status = ab_okf_pub_slug(pub, &names[index], &out->items[index].slug);
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->items, count, sizeof(*out->items), path_name_compare);
  for (index = 1; status == ARCHBIRD_OK && index < count; index++)
    if (ab_string_equal(&out->items[index - 1].name, &out->items[index].name))
      status = ab_okf_pub_error(pub, "duplicate OKF entity identity");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    size_t collision_count = 0;
    for (other = 0; other < count; other++)
      if (ab_string_equal(&out->items[index].slug, &out->items[other].slug))
        collision_count++;
    if (collision_count > 1)
      status = path_collision_suffix(pub, &out->items[index]);
  }
  for (index = 0; status == ARCHBIRD_OK && index < count; index++)
    status = path_value(pub, &out->items[index], prefix);
  if (status != ARCHBIRD_OK)
    path_set_free(pub, out);
  return status;
}

static ArchbirdStatus paths_from_rows(AbOkfPublication *pub,
                                      const AbValue *rows,
                                      const char *name_field,
                                      const char *prefix, MapPathSet *out) {
  AbString *names;
  size_t index;
  ArchbirdStatus status;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid OKF Map entity inventory");
  if (!rows->as.array.count)
    return paths_from_names(pub, NULL, 0, prefix, out);
  names =
      (AbString *)ab_calloc(pub->engine, rows->as.array.count, sizeof(*names));
  if (!names)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory reading OKF entity names");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbString *name =
        ab_okf_pub_text(&rows->as.array.items[index], name_field);
    if (!name) {
      ab_free(pub->engine, names);
      return ab_okf_pub_error(pub, "invalid named OKF Map entity");
    }
    names[index] = *name;
  }
  status = paths_from_names(pub, names, rows->as.array.count, prefix, out);
  ab_free(pub->engine, names);
  return status;
}

static ArchbirdStatus paths_from_test_groups(AbOkfPublication *pub,
                                             const AbValue *tests,
                                             MapPathSet *out) {
  AbString *names;
  size_t count = 0;
  size_t index;
  size_t other;
  ArchbirdStatus status;
  if (!tests || tests->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid OKF test inventory");
  names = tests->as.array.count
              ? (AbString *)ab_calloc(pub->engine, tests->as.array.count,
                                      sizeof(*names))
              : NULL;
  if (tests->as.array.count && !names)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory reading OKF test groups");
  for (index = 0; index < tests->as.array.count; index++) {
    const AbString *name =
        ab_okf_pub_text(&tests->as.array.items[index], "group");
    int seen = 0;
    if (!name) {
      ab_free(pub->engine, names);
      return ab_okf_pub_error(pub, "invalid OKF test group");
    }
    for (other = 0; other < count; other++)
      if (ab_string_equal(&names[other], name))
        seen = 1;
    if (!seen)
      names[count++] = *name;
  }
  status = paths_from_names(pub, names, count, "architecture/tests/", out);
  ab_free(pub->engine, names);
  return status;
}

static ArchbirdStatus md_header(AbBuffer *body, const char *const *headers,
                                size_t count) {
  size_t index;
  MAP_TRY(ab_buffer_literal(body, "| "));
  for (index = 0; index < count; index++) {
    if (index)
      MAP_TRY(ab_buffer_literal(body, " | "));
    MAP_TRY(ab_buffer_literal(body, headers[index]));
  }
  MAP_TRY(ab_buffer_literal(body, " |\n| "));
  for (index = 0; index < count; index++) {
    if (index)
      MAP_TRY(ab_buffer_literal(body, " | "));
    MAP_TRY(ab_buffer_literal(body, "---"));
  }
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus md_row_start(AbBuffer *body) {
  return ab_buffer_literal(body, "| ");
}

static ArchbirdStatus md_cell(AbBuffer *body) {
  return ab_buffer_literal(body, " | ");
}

static ArchbirdStatus md_row_end(AbBuffer *body) {
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus md_u64(AbBuffer *body, uint64_t value) {
  return ab_buffer_u64(body, value);
}

static ArchbirdStatus relation_target(AbOkfPublication *pub,
                                      AbOkfPubRelationList *relations,
                                      const char *kind,
                                      const AbString *concept_path) {
  AbBuffer target;
  ArchbirdStatus status;
  ab_buffer_init(&target, pub->engine);
  status = ab_buffer_append(&target, concept_path->data, concept_path->length);
  if (status == ARCHBIRD_OK && target.length >= 3 &&
      !memcmp(target.data + target.length - 3, ".md", 3))
    target.length -= 3;
  if (status == ARCHBIRD_OK) {
    if (target.data)
      target.data[target.length] = 0;
    status = ab_okf_pub_relation_simple(pub, relations, kind,
                                        (const char *)target.data);
  }
  ab_buffer_free(&target);
  return status;
}

static ArchbirdStatus dynamic_text(AbOkfPublication *pub, AbString *out,
                                   const char *prefix, const AbString *value,
                                   const char *suffix) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK && value)
    status = ab_buffer_append(&buffer, value->data, value->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, suffix);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus
add_project(AbOkfPublication *pub, const MapPathSet *layers,
            const MapPathSet *components, const MapPathSet *surfaces,
            const MapPathSet *packages, const MapPathSet *artifacts,
            const MapPathSet *tests, const MapPathSet *parity) {
  static const char *const evidence_headers[] = {"Field", "Value"};
  static const char *const inventory_headers[] = {
      "Files", "Symbols", "Edges", "Tests", "Cases", "Diagnostics"};
  static const char *const knowledge_headers[] = {"Knowledge", "Concept"};
  const AbString *project = ab_okf_pub_text(&pub->map, "project");
  const AbString *description = text_or_empty(&pub->map, "description");
  const AbValue *evidence =
      ab_okf_pub_member(&pub->map, "evidence", AB_VALUE_OBJECT);
  const AbValue *tool = ab_okf_pub_member(&pub->map, "tool", AB_VALUE_OBJECT);
  const AbValue *files = array_or_empty(&pub->map, "files");
  const AbValue *edges = array_or_empty(&pub->map, "edges");
  const AbValue *test_rows = array_or_empty(&pub->map, "tests");
  const AbValue *diagnostics = array_or_empty(&pub->map, "diagnostics");
  const AbString *input = ab_okf_pub_text(evidence, "input_sha256");
  const AbString *config = ab_okf_pub_text(evidence, "config_sha256");
  const AbString *producer_name = ab_okf_pub_text(tool, "name");
  const AbString *producer_version = ab_okf_pub_text(tool, "version");
  const AbString *producer_digest =
      ab_okf_pub_text(tool, "implementation_sha256");
  const MapPathSet *sets[] = {layers,    components, surfaces, packages,
                              artifacts, tests,      parity};
  const char *labels[] = {"Layer",    "Component", "Surface", "Package",
                          "Artifact", "Test",      "Parity"};
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[2] = {{"config_sha256", {0}}, {"map_input_sha256", {0}}};
  AbString title = {0};
  AbString desc = {0};
  AbString tags[] = {{(char *)"architecture", 12}, {(char *)"map", 3}};
  AbBuffer body;
  uint64_t symbols = 0;
  uint64_t cases = 0;
  size_t index;
  size_t set_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!project || !description || !files || !edges || !test_rows ||
      !diagnostics || !input || !config || !producer_name ||
      !producer_version || !producer_digest)
    return ab_okf_pub_error(pub, "invalid Map project publication fields");
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *file_symbols =
        array_or_empty(&files->as.array.items[index], "symbols");
    if (!file_symbols)
      return ab_okf_pub_error(pub, "invalid Map file symbols");
    symbols += file_symbols->as.array.count;
  }
  for (index = 0; index < test_rows->as.array.count; index++) {
    uint64_t count;
    if (!ab_okf_pub_u64(&test_rows->as.array.items[index], "count", &count))
      return ab_okf_pub_error(pub, "invalid Map test count");
    cases += count;
  }
  for (set_index = 0; set_index < sizeof(sets) / sizeof(sets[0]); set_index++)
    for (index = 0; status == ARCHBIRD_OK && index < sets[set_index]->count;
         index++)
      status = relation_target(pub, &relations, "contains",
                               &sets[set_index]->items[index].path);
  if (status == ARCHBIRD_OK) {
    AbString diagnostic_path = {(char *)"architecture/diagnostics.md", 27};
    status = relation_target(pub, &relations, "contains", &diagnostic_path);
  }
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[0].json, config);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[1].json, input);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, &title, project->data, project->length);
  if (status == ARCHBIRD_OK)
    status =
        description->length
            ? ab_okf_pub_copy(pub, &desc, description->data,
                              description->length)
            : ab_okf_pub_literal(
                  pub, &desc, "Deterministic project architecture evidence.");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, project);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK && description->length)
    status = ab_okf_pub_plain(&body, description);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "Deterministic architecture evidence.");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n# Evidence identity\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, evidence_headers, 2);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| Map input SHA-256 | ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, input);
  if (status == ARCHBIRD_OK)
    status = md_row_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| Config SHA-256 | ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, config);
  if (status == ARCHBIRD_OK)
    status = md_row_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| Producer | <code>");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&body, producer_name->data, producer_name->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, " ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&body, producer_version->data,
                              producer_version->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "</code> |\n");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| Producer implementation | ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_code(&body, producer_digest);
  if (status == ARCHBIRD_OK)
    status = md_row_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Inventory\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, inventory_headers, 6);
  if (status == ARCHBIRD_OK)
    status = md_row_start(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, files->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, symbols);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, edges->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, test_rows->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, cases);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = md_u64(&body, diagnostics->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_row_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Published concepts\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, knowledge_headers, 2);
  for (set_index = 0;
       status == ARCHBIRD_OK && set_index < sizeof(sets) / sizeof(sets[0]);
       set_index++) {
    for (index = 0; status == ARCHBIRD_OK && index < sets[set_index]->count;
         index++) {
      const MapPath *row = &sets[set_index]->items[index];
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, labels[set_index]);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, "architecture/project.md",
                                          row->path.data, &row->name);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
  }
  if (status == ARCHBIRD_OK) {
    AbString diagnostic_label = {(char *)"diagnostics", 11};
    status = ab_buffer_literal(
        &body, "| Diagnostics | [diagnostics](diagnostics.md) |\n");
    (void)diagnostic_label;
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_literal(&body, "\nThis OKF bundle is a deterministic "
                                 "publication projection. The saved "
                                 "Archbird JSON map remains authoritative.\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {
        &pub->map_source,
        "architecture/project.md",
        "Archbird Project",
        &title,
        NULL,
        &desc,
        "derived",
        "project",
        project,
        tags,
        2,
        &relations,
        extra,
        2,
        &body,
    };
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &desc);
  ab_okf_pub_fields_free(pub, extra, 2);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

/* Remaining entity projections are split below to keep the public entry small.
 */
static ArchbirdStatus add_layers(AbOkfPublication *pub,
                                 const MapPathSet *layers,
                                 const MapPathSet *components);
static ArchbirdStatus add_components(AbOkfPublication *pub,
                                     const MapPathSet *components,
                                     ComponentRoutes *routes);
static ArchbirdStatus add_surfaces(AbOkfPublication *pub,
                                   const MapPathSet *surfaces,
                                   const MapPathSet *layers);
static ArchbirdStatus add_packages(AbOkfPublication *pub,
                                   const MapPathSet *packages,
                                   const MapPathSet *layers);
static ArchbirdStatus add_artifacts(AbOkfPublication *pub,
                                    const MapPathSet *artifacts);
static ArchbirdStatus add_tests(AbOkfPublication *pub, const MapPathSet *tests,
                                const MapPathSet *components);
static ArchbirdStatus add_parity(AbOkfPublication *pub,
                                 const MapPathSet *parity);
static ArchbirdStatus add_map_diagnostics(AbOkfPublication *pub);
static ArchbirdStatus component_routes(AbOkfPublication *pub,
                                       ComponentRoutes *out);
static void component_routes_free(AbOkfPublication *pub,
                                  ComponentRoutes *routes);

ArchbirdStatus ab_okf_pub_map(AbOkfPublication *pub) {
  const AbValue *layer_rows = array_or_empty(&pub->map, "layers");
  const AbValue *component_rows = array_or_empty(&pub->map, "components");
  const AbValue *surface_rows = array_or_empty(&pub->map, "surfaces");
  const AbValue *package_rows = array_or_empty(&pub->map, "packages");
  const AbValue *artifact_rows = array_or_empty(&pub->map, "artifacts");
  const AbValue *test_rows = array_or_empty(&pub->map, "tests");
  const AbValue *parity_rows = array_or_empty(&pub->map, "parity");
  MapPathSet layers = {0};
  MapPathSet components = {0};
  MapPathSet surfaces = {0};
  MapPathSet packages = {0};
  MapPathSet artifacts = {0};
  MapPathSet tests = {0};
  MapPathSet parity = {0};
  ComponentRoutes routes = {0};
  ArchbirdStatus status;
  if (!layer_rows || !component_rows || !surface_rows || !package_rows ||
      !artifact_rows || !test_rows || !parity_rows)
    return ab_okf_pub_error(pub, "invalid Map inventories for OKF publication");
  status =
      paths_from_rows(pub, layer_rows, "name", "architecture/layers/", &layers);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, component_rows, "name",
                             "architecture/components/", &components);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, surface_rows, "name",
                             "architecture/interfaces/", &surfaces);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, package_rows, "name",
                             "architecture/packages/", &packages);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, artifact_rows, "name",
                             "architecture/artifacts/", &artifacts);
  if (status == ARCHBIRD_OK)
    status = paths_from_test_groups(pub, test_rows, &tests);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, parity_rows, "name", "architecture/parity/",
                             &parity);
  if (status == ARCHBIRD_OK)
    status = component_routes(pub, &routes);
  if (status == ARCHBIRD_OK)
    status = add_project(pub, &layers, &components, &surfaces, &packages,
                         &artifacts, &tests, &parity);
  if (status == ARCHBIRD_OK)
    status = add_layers(pub, &layers, &components);
  if (status == ARCHBIRD_OK)
    status = add_components(pub, &components, &routes);
  if (status == ARCHBIRD_OK)
    status = add_surfaces(pub, &surfaces, &layers);
  if (status == ARCHBIRD_OK)
    status = add_packages(pub, &packages, &layers);
  if (status == ARCHBIRD_OK)
    status = add_artifacts(pub, &artifacts);
  if (status == ARCHBIRD_OK)
    status = add_tests(pub, &tests, &components);
  if (status == ARCHBIRD_OK)
    status = add_parity(pub, &parity);
  if (status == ARCHBIRD_OK)
    status = add_map_diagnostics(pub);
  component_routes_free(pub, &routes);
  path_set_free(pub, &parity);
  path_set_free(pub, &tests);
  path_set_free(pub, &artifacts);
  path_set_free(pub, &packages);
  path_set_free(pub, &surfaces);
  path_set_free(pub, &components);
  path_set_free(pub, &layers);
  return status;
}

static int component_has_target(const AbValue *component, AbString *targets,
                                size_t target_count) {
  const AbValue *files = array_or_empty(component, "files");
  size_t target;
  if (!files)
    return 0;
  for (target = 0; target < target_count; target++)
    if (string_array_contains(files, &targets[target]))
      return 1;
  return 0;
}

static ArchbirdStatus add_tests(AbOkfPublication *pub, const MapPathSet *tests,
                                const MapPathSet *components) {
  const char *summary_headers[] = {"Files", "Cases", "Distinct targets"};
  const char *file_headers[] = {"Path", "Language", "Framework", "Cases",
                                "Route targets"};
  const AbValue *rows = array_or_empty(&pub->map, "tests");
  const AbValue *component_rows = array_or_empty(&pub->map, "components");
  size_t group_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows || !component_rows)
    return ab_okf_pub_error(pub, "invalid Map test publication inputs");
  for (group_index = 0; status == ARCHBIRD_OK && group_index < tests->count;
       group_index++) {
    const MapPath *path = &tests->items[group_index];
    AbString *targets = NULL;
    size_t target_count = 0;
    size_t target_capacity = 0;
    uint64_t test_file_count = 0;
    uint64_t case_count = 0;
    size_t row_index;
    size_t component_index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"architecture", 12}, {(char *)"tests", 5}};
    AbBuffer body;
    for (row_index = 0; row_index < rows->as.array.count; row_index++) {
      const AbValue *test = &rows->as.array.items[row_index];
      const AbString *group = ab_okf_pub_text(test, "group");
      const AbValue *routes =
          ab_okf_pub_member(test, "routes", AB_VALUE_OBJECT);
      uint64_t count;
      size_t route_index;
      if (!group || !routes || !ab_okf_pub_u64(test, "count", &count))
        return ab_okf_pub_error(pub, "invalid Map test row");
      if (!ab_string_equal(group, &path->name))
        continue;
      test_file_count++;
      case_count += count;
      for (route_index = 0;
           status == ARCHBIRD_OK && route_index < routes->as.object.count;
           route_index++)
        status = string_set_add(pub, &targets, &target_count, &target_capacity,
                                &routes->as.object.fields[route_index].name);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                          "architecture/project");
    for (component_index = 0;
         status == ARCHBIRD_OK && component_index < components->count;
         component_index++) {
      const MapPath *component_path = &components->items[component_index];
      const AbValue *component =
          row_by_name(component_rows, &component_path->name, "name");
      if (component && component_has_target(component, targets, target_count))
        status = relation_target(pub, &relations, "tests_component",
                                 &component_path->path);
    }
    if (status == ARCHBIRD_OK)
      status = dynamic_text(pub, &title, "", &path->name, " tests");
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_u64(&value, test_file_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " test files with ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, case_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " cases.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " tests\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 3);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, test_file_count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, case_count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, target_count);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Test files\n\n");
    if (status == ARCHBIRD_OK && test_file_count)
      status = md_header(&body, file_headers, 5);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (row_index = 0;
         status == ARCHBIRD_OK && row_index < rows->as.array.count;
         row_index++) {
      const AbValue *test = &rows->as.array.items[row_index];
      const AbString *group = ab_okf_pub_text(test, "group");
      const AbString *test_path = ab_okf_pub_text(test, "path");
      const AbString *language = ab_okf_pub_text(test, "language");
      const AbString *framework = ab_okf_pub_text(test, "framework");
      const AbValue *routes =
          ab_okf_pub_member(test, "routes", AB_VALUE_OBJECT);
      uint64_t count;
      if (!group || !test_path || !language || !framework || !routes ||
          !ab_okf_pub_u64(test, "count", &count))
        return ab_okf_pub_error(pub, "invalid Map test file row");
      if (!ab_string_equal(group, &path->name))
        continue;
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, test_path);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, language);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, framework);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, count);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, routes->as.object.count);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Routed components\n\n");
    {
      size_t linked = 0;
      for (component_index = 0;
           status == ARCHBIRD_OK && component_index < components->count;
           component_index++) {
        const MapPath *component_path = &components->items[component_index];
        const AbValue *component =
            row_by_name(component_rows, &component_path->name, "name");
        if (!component ||
            !component_has_target(component, targets, target_count))
          continue;
        status = ab_buffer_literal(&body, "* ");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_relative_link(&body, path->path.data,
                                            component_path->path.data,
                                            &component_path->name);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&body, "\n");
        linked++;
      }
      if (status == ARCHBIRD_OK && !linked)
        status =
            ab_buffer_literal(&body, "_No configured component target._\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Route targets\n\n");
    for (row_index = 0; status == ARCHBIRD_OK && row_index < target_count;
         row_index++) {
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &targets[row_index]);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Test Route",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "test_group",
                               &path->name,
                               tags,
                               2,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
    string_set_free(pub, targets, target_count);
  }
  return status;
}

static ArchbirdStatus add_parity(AbOkfPublication *pub,
                                 const MapPathSet *parity) {
  const char *summary_headers[] = {"Enforced", "Shared values", "Members"};
  const char *member_headers[] = {"Member", "Values", "Missing"};
  const AbValue *rows = array_or_empty(&pub->map, "parity");
  size_t parity_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows)
    return ab_okf_pub_error(pub, "invalid Map parity inventory");
  for (parity_index = 0; status == ARCHBIRD_OK && parity_index < parity->count;
       parity_index++) {
    const MapPath *path = &parity->items[parity_index];
    const AbValue *row = row_by_name(rows, &path->name, "name");
    const AbValue *members = array_or_empty(row, "members");
    const AbValue *shared = array_or_empty(row, "shared");
    int enforce;
    size_t index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[3] = {
        {(char *)"architecture", 12}, {(char *)"parity", 6}, {0}};
    AbBuffer body;
    if (!row || !members || !shared || !bool_value(row, "enforce", &enforce))
      return ab_okf_pub_error(pub, "invalid Map parity row");
    tags[2] = enforce ? (AbString){(char *)"enforced", 8}
                      : (AbString){(char *)"report", 6};
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                        "architecture/project");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Configured parity surface across ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, members->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " members.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 3);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, enforce ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, shared->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, members->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Members\n\n");
    if (status == ARCHBIRD_OK && members->as.array.count)
      status = md_header(&body, member_headers, 3);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < members->as.array.count;
         index++) {
      const AbValue *member = &members->as.array.items[index];
      const AbString *label = ab_okf_pub_text(member, "label");
      const AbValue *values = array_or_empty(member, "values");
      const AbValue *missing = array_or_empty(member, "missing");
      size_t missing_index;
      if (!label || !values || !missing)
        return ab_okf_pub_error(pub, "invalid parity member");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, label);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, values->as.array.count);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (missing_index = 0;
           status == ARCHBIRD_OK && missing_index < missing->as.array.count;
           missing_index++) {
        const AbValue *value = &missing->as.array.items[missing_index];
        if (value->kind != AB_VALUE_STRING)
          return ab_okf_pub_error(pub, "invalid parity missing value");
        if (missing_index)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &value->as.text);
      }
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Shared values\n\n");
    for (index = 0; status == ARCHBIRD_OK && index < shared->as.array.count;
         index++) {
      const AbValue *value = &shared->as.array.items[index];
      if (value->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid parity shared value");
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &value->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Parity Surface",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "parity",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static ArchbirdStatus add_map_diagnostics(AbOkfPublication *pub) {
  const char *headers[] = {"Severity", "Code", "Path", "Message"};
  const AbValue *rows = array_or_empty(&pub->map, "diagnostics");
  AbOkfPubRelationList relations = {0};
  AbString title = {(char *)"Map diagnostics", 15};
  AbString entity = {(char *)"map", 3};
  AbString description = {0};
  AbString tags[] = {{(char *)"architecture", 12}, {(char *)"diagnostics", 11}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status;
  if (!rows)
    return ab_okf_pub_error(pub, "invalid Map diagnostics inventory");
  status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                      "architecture/project");
  if (status == ARCHBIRD_OK) {
    AbBuffer value;
    ab_buffer_init(&value, pub->engine);
    status = ab_buffer_u64(&value, rows->as.array.count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          &value, " diagnostics from the saved architecture map.");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &value, &description);
    ab_buffer_free(&value);
  }
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Map diagnostics\n\n");
  if (status == ARCHBIRD_OK && rows->as.array.count)
    status = md_header(&body, headers, 4);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbString *severity = ab_okf_pub_text(row, "severity");
    const AbString *code = ab_okf_pub_text(row, "code");
    const AbString *path = text_or_empty(row, "path");
    const AbString *message = ab_okf_pub_text(row, "message");
    if (!severity || !code || !path || !message)
      return ab_okf_pub_error(pub, "invalid Map diagnostic");
    status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, severity);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, code);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, path);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, message);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
  }
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->map_source,
                             "architecture/diagnostics.md",
                             "Archbird Diagnostic Report",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "diagnostics",
                             &entity,
                             tags,
                             2,
                             &relations,
                             NULL,
                             0,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus package_label(AbOkfPublication *pub,
                                    const AbValue *package, AbString *out) {
  const AbString *identity = text_or_empty(package, "identity");
  const AbString *name = ab_okf_pub_text(package, "name");
  const AbString *version = text_or_empty(package, "version");
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!identity || !name || !version)
    return ab_okf_pub_error(pub, "invalid package identity");
  if (!identity->length)
    identity = name;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_append(&buffer, identity->data, identity->length);
  if (status == ARCHBIRD_OK && version->length)
    status = ab_buffer_literal(&buffer, "@");
  if (status == ARCHBIRD_OK && version->length)
    status = ab_buffer_append(&buffer, version->data, version->length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_packages(AbOkfPublication *pub,
                                   const MapPathSet *packages,
                                   const MapPathSet *layers) {
  const char *summary_headers[] = {"Kind", "Layer", "Manifest", "Exports",
                                   "Dependencies"};
  const char *entry_headers[] = {"Route", "Target"};
  const char *dependency_headers[] = {"Name", "Requirement", "Scope"};
  const AbValue *rows = array_or_empty(&pub->map, "packages");
  size_t package_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows)
    return ab_okf_pub_error(pub, "invalid Map package inventory");
  for (package_index = 0;
       status == ARCHBIRD_OK && package_index < packages->count;
       package_index++) {
    const MapPath *path = &packages->items[package_index];
    const AbValue *package = row_by_name(rows, &path->name, "name");
    const AbString *kind = ab_okf_pub_text(package, "kind");
    const AbString *layer = ab_okf_pub_text(package, "layer");
    const AbString *manifest = ab_okf_pub_text(package, "manifest");
    const AbValue *entrypoints =
        ab_okf_pub_member(package, "entrypoints", AB_VALUE_OBJECT);
    const AbValue *dependencies = array_or_empty(package, "dependencies");
    const AbValue *exports = array_or_empty(package, "exports");
    const MapPath *layer_path;
    size_t index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[3] = {
        {(char *)"architecture", 12}, {(char *)"package", 7}, {0}};
    AbBuffer body;
    if (!package || !kind || !layer || !manifest || !entrypoints ||
        !dependencies || !exports)
      return ab_okf_pub_error(pub, "invalid Map package row");
    tags[2] = *kind;
    layer_path = path_find(layers, layer);
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                        "architecture/project");
    if (status == ARCHBIRD_OK && layer_path)
      status = relation_target(pub, &relations, "belongs_to_layer",
                               &layer_path->path);
    if (status == ARCHBIRD_OK)
      status = package_label(pub, package, &title);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_append(&value, kind->data, kind->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " package surface for layer ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, layer->data, layer->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, ".");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &title);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 5);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, kind);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK && layer_path)
      status = ab_okf_pub_relative_link(&body, path->path.data,
                                        layer_path->path.data, layer);
    else if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, layer);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, manifest);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, exports->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, dependencies->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Entrypoints\n\n");
    if (status == ARCHBIRD_OK && entrypoints->as.object.count)
      status = md_header(&body, entry_headers, 2);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0;
         status == ARCHBIRD_OK && index < entrypoints->as.object.count;
         index++) {
      const AbObjectField *field = &entrypoints->as.object.fields[index];
      if (field->value.kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid package entrypoint");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &field->name);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &field->value.as.text);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Dependencies\n\n");
    if (status == ARCHBIRD_OK && dependencies->as.array.count)
      status = md_header(&body, dependency_headers, 3);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0;
         status == ARCHBIRD_OK && index < dependencies->as.array.count;
         index++) {
      const AbValue *dependency = &dependencies->as.array.items[index];
      const AbString *name = ab_okf_pub_text(dependency, "name");
      const AbString *requirement = ab_okf_pub_text(dependency, "requirement");
      const AbString *scope = ab_okf_pub_text(dependency, "scope");
      if (!name || !requirement || !scope)
        return ab_okf_pub_error(pub, "invalid package dependency");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, name);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, requirement);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, scope);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Public exports\n\n");
    if (status == ARCHBIRD_OK && !exports->as.array.count)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < exports->as.array.count;
         index++) {
      const AbValue *value = &exports->as.array.items[index];
      if (value->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid package export");
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &value->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Package",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "package",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static ArchbirdStatus add_artifacts(AbOkfPublication *pub,
                                    const MapPathSet *artifacts) {
  const char *summary_headers[] = {"Output", "Inputs", "Loaders",
                                   "Build routes"};
  const char *input_headers[] = {"Path", "Evidence"};
  const char *loader_headers[] = {"Path", "Pattern", "Matches"};
  const char *build_headers[] = {"Source", "Target"};
  const AbValue *rows = array_or_empty(&pub->map, "artifacts");
  size_t artifact_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows)
    return ab_okf_pub_error(pub, "invalid Map artifact inventory");
  for (artifact_index = 0;
       status == ARCHBIRD_OK && artifact_index < artifacts->count;
       artifact_index++) {
    const MapPath *path = &artifacts->items[artifact_index];
    const AbValue *artifact = row_by_name(rows, &path->name, "name");
    const AbString *output = ab_okf_pub_text(artifact, "output");
    const AbValue *inputs = array_or_empty(artifact, "inputs");
    const AbValue *loaders = array_or_empty(artifact, "loaded_by");
    const AbValue *builds = array_or_empty(artifact, "builds");
    const AbValue *depends = array_or_empty(artifact, "depends_on");
    size_t index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"architecture", 12}, {(char *)"artifact", 8}};
    AbBuffer body;
    if (!artifact || !output || !inputs || !loaders || !builds || !depends)
      return ab_okf_pub_error(pub, "invalid Map artifact row");
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                        "architecture/project");
    for (index = 0; status == ARCHBIRD_OK && index < depends->as.array.count;
         index++) {
      const AbValue *value = &depends->as.array.items[index];
      const MapPath *target;
      if (value->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid artifact dependency");
      target = path_find(artifacts, &value->as.text);
      if (target)
        status = relation_target(pub, &relations, "depends_on_artifact",
                                 &target->path);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Generated artifact ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, output->data, output->length);
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_literal(&value, " and its source/build/loader routes.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 4);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, output);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, inputs->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, loaders->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, builds->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Inputs\n\n");
    if (status == ARCHBIRD_OK && inputs->as.array.count)
      status = md_header(&body, input_headers, 2);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < inputs->as.array.count;
         index++) {
      const AbValue *input = &inputs->as.array.items[index];
      const AbString *input_path = ab_okf_pub_text(input, "path");
      const AbValue *evidence = array_or_empty(input, "evidence");
      size_t item;
      if (!input_path || !evidence)
        return ab_okf_pub_error(pub, "invalid artifact input");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, input_path);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (item = 0; status == ARCHBIRD_OK && item < evidence->as.array.count;
           item++) {
        const AbValue *entry = &evidence->as.array.items[item];
        if (entry->kind != AB_VALUE_STRING)
          return ab_okf_pub_error(pub, "invalid artifact input evidence");
        if (item)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_plain(&body, &entry->as.text);
      }
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Loaders\n\n");
    if (status == ARCHBIRD_OK && loaders->as.array.count)
      status = md_header(&body, loader_headers, 3);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < loaders->as.array.count;
         index++) {
      const AbValue *loader = &loaders->as.array.items[index];
      const AbString *loader_path = ab_okf_pub_text(loader, "path");
      const AbString *pattern = ab_okf_pub_text(loader, "pattern");
      uint64_t matches;
      if (!loader_path || !pattern ||
          !ab_okf_pub_u64(loader, "matches", &matches))
        return ab_okf_pub_error(pub, "invalid artifact loader");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, loader_path);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, pattern);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, matches);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Build routes\n\n");
    if (status == ARCHBIRD_OK && builds->as.array.count)
      status = md_header(&body, build_headers, 2);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < builds->as.array.count;
         index++) {
      const AbValue *build = &builds->as.array.items[index];
      const AbString *source = ab_okf_pub_text(build, "source");
      const AbString *target = ab_okf_pub_text(build, "target");
      if (!source || !target)
        return ab_okf_pub_error(pub, "invalid artifact build route");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, source);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, target);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Build Artifact",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "artifact",
                               &path->name,
                               tags,
                               2,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

typedef struct SurfaceSummary {
  uint64_t registered;
  uint64_t used;
  uint64_t unused;
  uint64_t unregistered_use;
  uint64_t unresolved;
  uint64_t ambiguous;
  uint64_t ignored;
} SurfaceSummary;

static int bool_value(const AbValue *object, const char *name, int *out) {
  const AbValue *value = ab_value_member(object, name);
  if (!value || value->kind != AB_VALUE_BOOL)
    return 0;
  *out = value->as.boolean;
  return 1;
}

static ArchbirdStatus surface_summary(AbOkfPublication *pub,
                                      const AbValue *surface,
                                      SurfaceSummary *summary) {
  const AbValue *names = array_or_empty(surface, "names");
  size_t index;
  if (!names)
    return ab_okf_pub_error(pub, "invalid interface name inventory");
  memset(summary, 0, sizeof(*summary));
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbString *declaration = ab_okf_pub_text(row, "declaration");
    const AbString *resolution = ab_okf_pub_text(row, "resolution");
    const AbValue *uses = array_or_empty(row, "uses");
    int ignored;
    int declared;
    if (!declaration || !resolution || !uses ||
        !bool_value(row, "ignored", &ignored))
      return ab_okf_pub_error(pub, "invalid interface surface row");
    if (ignored) {
      summary->ignored++;
      continue;
    }
    declared =
        declaration->length == 8 && !memcmp(declaration->data, "declared", 8);
    summary->registered += declared;
    summary->used += uses->as.array.count != 0;
    summary->unused += declared && !uses->as.array.count;
    summary->unregistered_use += declaration->length == 10 &&
                                 !memcmp(declaration->data, "undeclared", 10) &&
                                 uses->as.array.count != 0;
    summary->unresolved +=
        resolution->length == 10 && !memcmp(resolution->data, "unresolved", 10);
    summary->ambiguous +=
        resolution->length == 9 && !memcmp(resolution->data, "ambiguous", 9);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus surface_summary_json(AbOkfPublication *pub,
                                           const SurfaceSummary *summary,
                                           AbString *out) {
  AbOkfPubField fields[7] = {{"ambiguous", {0}},  {"ignored", {0}},
                             {"registered", {0}}, {"unregistered_use", {0}},
                             {"unresolved", {0}}, {"unused", {0}},
                             {"used", {0}}};
  AbBuffer buffer;
  size_t index;
  const uint64_t values[] = {summary->ambiguous,  summary->ignored,
                             summary->registered, summary->unregistered_use,
                             summary->unresolved, summary->unused,
                             summary->used};
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < 7; index++)
    status = ab_okf_pub_json_u64(pub, &fields[index].json, values[index]);
  ab_buffer_init(&buffer, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "{");
  for (index = 0; status == ARCHBIRD_OK && index < 7; index++) {
    if (index)
      status = ab_buffer_literal(&buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&buffer, fields[index].name,
                                     strlen(fields[index].name));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, fields[index].json.data,
                                fields[index].json.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  ab_okf_pub_fields_free(pub, fields, 7);
  return status;
}

static const AbString *file_layer(const AbValue *files, const AbString *path) {
  size_t index;
  for (index = 0; files && index < files->as.array.count; index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbString *candidate = ab_okf_pub_text(row, "path");
    if (candidate && ab_string_equal(candidate, path))
      return ab_okf_pub_text(row, "layer");
  }
  return NULL;
}

static ArchbirdStatus string_set_add(AbOkfPublication *pub, AbString **items,
                                     size_t *count, size_t *capacity,
                                     const AbString *value) {
  size_t index;
  size_t next;
  AbString *resized;
  for (index = 0; index < *count; index++)
    if (ab_string_equal(&(*items)[index], value))
      return ARCHBIRD_OK;
  if (*count == *capacity) {
    next = *capacity ? *capacity * 2 : 8;
    if (next > SIZE_MAX / sizeof(*resized) ||
        next > pub->engine->options.max_values)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET, "too many OKF names");
    resized =
        (AbString *)ab_realloc(pub->engine, *items, next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing OKF names");
    memset(resized + *capacity, 0, (next - *capacity) * sizeof(*resized));
    *items = resized;
    *capacity = next;
  }
  MAP_TRY(ab_okf_pub_copy(pub, &(*items)[*count], value->data, value->length));
  (*count)++;
  if (*count > 1)
    qsort(*items, *count, sizeof(**items), string_compare);
  return ARCHBIRD_OK;
}

static void string_set_free(AbOkfPublication *pub, AbString *items,
                            size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(pub->engine, &items[index]);
  ab_free(pub->engine, items);
}

static ArchbirdStatus add_surface_path_layer(AbOkfPublication *pub,
                                             const AbValue *files,
                                             const AbString *file_path,
                                             AbString **layers, size_t *count,
                                             size_t *capacity) {
  const AbString *layer = file_layer(files, file_path);
  return layer ? string_set_add(pub, layers, count, capacity, layer)
               : ARCHBIRD_OK;
}

static ArchbirdStatus add_surfaces(AbOkfPublication *pub,
                                   const MapPathSet *surfaces,
                                   const MapPathSet *layers) {
  const char *summary_headers[] = {
      "Kind",   "Provider configured", "registered", "used",
      "unused", "unregistered_use",    "unresolved", "ambiguous",
      "ignored"};
  const char *provider_headers[] = {"Path", "Source"};
  const char *name_headers[] = {"Name",       "Declaration", "Uses",
                                "Resolution", "Candidates",  "Ignored"};
  const AbValue *rows = array_or_empty(&pub->map, "surfaces");
  const AbValue *files = array_or_empty(&pub->map, "files");
  size_t surface_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows || !files)
    return ab_okf_pub_error(pub, "invalid Map interface inventory");
  for (surface_index = 0;
       status == ARCHBIRD_OK && surface_index < surfaces->count;
       surface_index++) {
    const MapPath *path = &surfaces->items[surface_index];
    const AbValue *surface = row_by_name(rows, &path->name, "name");
    const AbString *kind = ab_okf_pub_text(surface, "kind");
    const AbValue *providers = array_or_empty(surface, "providers");
    const AbValue *names = array_or_empty(surface, "names");
    SurfaceSummary summary;
    int provider_configured;
    AbString *touched = NULL;
    size_t touched_count = 0;
    size_t touched_capacity = 0;
    size_t name_index;
    size_t provider_index;
    size_t layer_index;
    AbOkfPubRelationList relations = {0};
    AbOkfPubField extra[1] = {{"summary", {0}}};
    AbString title = {0};
    AbString description = {0};
    AbString tags[3] = {
        {(char *)"architecture", 12}, {(char *)"interface", 9}, {0}};
    AbBuffer body;
    if (!surface || !kind || !providers || !names ||
        !bool_value(surface, "provider_configured", &provider_configured))
      return ab_okf_pub_error(pub, "invalid Map interface row");
    tags[2] = *kind;
    status = surface_summary(pub, surface, &summary);
    for (name_index = 0;
         status == ARCHBIRD_OK && name_index < names->as.array.count;
         name_index++) {
      const AbValue *name = &names->as.array.items[name_index];
      const AbValue *declarations = array_or_empty(name, "declarations");
      const AbValue *uses = array_or_empty(name, "uses");
      const AbValue *candidates = array_or_empty(name, "candidates");
      size_t item;
      if (!declarations || !uses || !candidates) {
        status = ab_okf_pub_error(pub, "invalid interface evidence paths");
        break;
      }
      for (item = 0;
           status == ARCHBIRD_OK && item < declarations->as.array.count;
           item++) {
        const AbString *file_path =
            ab_okf_pub_text(&declarations->as.array.items[item], "path");
        if (!file_path)
          status = ab_okf_pub_error(pub, "invalid interface declaration path");
        else
          status = add_surface_path_layer(pub, files, file_path, &touched,
                                          &touched_count, &touched_capacity);
      }
      for (item = 0; status == ARCHBIRD_OK && item < uses->as.array.count;
           item++) {
        const AbString *file_path =
            ab_okf_pub_text(&uses->as.array.items[item], "path");
        if (!file_path)
          status = ab_okf_pub_error(pub, "invalid interface use path");
        else
          status = add_surface_path_layer(pub, files, file_path, &touched,
                                          &touched_count, &touched_capacity);
      }
      for (item = 0; status == ARCHBIRD_OK && item < candidates->as.array.count;
           item++) {
        const AbValue *candidate = &candidates->as.array.items[item];
        if (candidate->kind != AB_VALUE_STRING)
          status = ab_okf_pub_error(pub, "invalid interface candidate path");
        else
          status =
              add_surface_path_layer(pub, files, &candidate->as.text, &touched,
                                     &touched_count, &touched_capacity);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                          "architecture/project");
    for (layer_index = 0; status == ARCHBIRD_OK && layer_index < touched_count;
         layer_index++) {
      const MapPath *target = path_find(layers, &touched[layer_index]);
      if (target)
        status =
            relation_target(pub, &relations, "touches_layer", &target->path);
    }
    if (status == ARCHBIRD_OK)
      status = surface_summary_json(pub, &summary, &extra[0].json);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Configured ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, kind->data, kind->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " provider/consumer surface with ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, names->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " names.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 9);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, kind);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, provider_configured ? "true" : "false");
#define SUMMARY_CELL(member)                                                   \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = md_cell(&body);                                                 \
    if (status == ARCHBIRD_OK)                                                 \
      status = md_u64(&body, summary.member);                                  \
  } while (0)
    SUMMARY_CELL(registered);
    SUMMARY_CELL(used);
    SUMMARY_CELL(unused);
    SUMMARY_CELL(unregistered_use);
    SUMMARY_CELL(unresolved);
    SUMMARY_CELL(ambiguous);
    SUMMARY_CELL(ignored);
#undef SUMMARY_CELL
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Provider declarations\n\n");
    if (status == ARCHBIRD_OK && providers->as.array.count)
      status = md_header(&body, provider_headers, 2);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (provider_index = 0;
         status == ARCHBIRD_OK && provider_index < providers->as.array.count;
         provider_index++) {
      const AbValue *provider = &providers->as.array.items[provider_index];
      const AbString *provider_path = ab_okf_pub_text(provider, "path");
      const AbString *source = ab_okf_pub_text(provider, "source");
      if (!provider_path || !source)
        return ab_okf_pub_error(pub, "invalid interface provider row");
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, provider_path);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_plain(&body, source);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Complete configured surface\n\n");
    if (status == ARCHBIRD_OK && names->as.array.count)
      status = md_header(&body, name_headers, 6);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (name_index = 0;
         status == ARCHBIRD_OK && name_index < names->as.array.count;
         name_index++) {
      const AbValue *name = &names->as.array.items[name_index];
      const AbString *identity = ab_okf_pub_text(name, "name");
      const AbString *declaration = ab_okf_pub_text(name, "declaration");
      const AbString *resolution = ab_okf_pub_text(name, "resolution");
      const AbValue *uses = array_or_empty(name, "uses");
      const AbValue *candidates = array_or_empty(name, "candidates");
      uint64_t uses_count = 0;
      size_t use_index;
      int ignored;
      if (!identity || !declaration || !resolution || !uses || !candidates ||
          !bool_value(name, "ignored", &ignored))
        return ab_okf_pub_error(pub, "invalid interface name row");
      for (use_index = 0; use_index < uses->as.array.count; use_index++) {
        uint64_t count;
        if (!ab_okf_pub_u64(&uses->as.array.items[use_index], "count", &count))
          return ab_okf_pub_error(pub, "invalid interface use count");
        uses_count += count;
      }
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, identity);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, declaration);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, uses_count);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, resolution);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (use_index = 0;
           status == ARCHBIRD_OK && use_index < candidates->as.array.count;
           use_index++) {
        const AbValue *candidate = &candidates->as.array.items[use_index];
        if (candidate->kind != AB_VALUE_STRING)
          return ab_okf_pub_error(pub, "invalid interface candidate");
        if (use_index)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &candidate->as.text);
      }
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, ignored ? "true" : "false");
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Interface",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "interface",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               extra,
                               1,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_fields_free(pub, extra, 1);
    ab_okf_pub_relations_free(pub, &relations);
    string_set_free(pub, touched, touched_count);
  }
  return status;
}

static const AbValue *row_by_name(const AbValue *rows, const AbString *name,
                                  const char *field) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY || !name)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbString *candidate =
        ab_okf_pub_text(&rows->as.array.items[index], field);
    if (candidate && ab_string_equal(candidate, name))
      return &rows->as.array.items[index];
  }
  return NULL;
}

static int string_array_contains(const AbValue *rows, const AbString *value) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY || !value)
    return 0;
  for (index = 0; index < rows->as.array.count; index++)
    if (rows->as.array.items[index].kind == AB_VALUE_STRING &&
        ab_string_equal(&rows->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

static ArchbirdStatus route_add_name(AbOkfPublication *pub,
                                     ComponentRoute *route,
                                     const AbString *name) {
  size_t index;
  size_t capacity;
  AbString *resized;
  for (index = 0; index < route->name_count; index++)
    if (ab_string_equal(&route->names[index], name))
      return ARCHBIRD_OK;
  if (route->name_count == route->name_capacity) {
    capacity = route->name_capacity ? route->name_capacity * 2 : 4;
    if (capacity > SIZE_MAX / sizeof(*resized) ||
        capacity > pub->engine->options.max_values)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many component route names");
    resized = (AbString *)ab_realloc(pub->engine, route->names,
                                     capacity * sizeof(*resized));
    if (!resized)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing component routes");
    memset(resized + route->name_capacity, 0,
           (capacity - route->name_capacity) * sizeof(*resized));
    route->names = resized;
    route->name_capacity = capacity;
  }
  MAP_TRY(ab_okf_pub_copy(pub, &route->names[route->name_count], name->data,
                          name->length));
  route->name_count++;
  if (route->name_count > 1)
    qsort(route->names, route->name_count, sizeof(*route->names),
          string_compare);
  return ARCHBIRD_OK;
}

static ArchbirdStatus routes_reserve(AbOkfPublication *pub,
                                     ComponentRoutes *routes, size_t count) {
  size_t capacity = routes->capacity ? routes->capacity : 16;
  ComponentRoute *resized;
  if (count <= routes->capacity)
    return ARCHBIRD_OK;
  while (capacity < count) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many component routes");
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*resized) ||
      capacity > pub->engine->options.max_values)
    return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many component routes");
  resized = (ComponentRoute *)ab_realloc(pub->engine, routes->items,
                                         capacity * sizeof(*resized));
  if (!resized)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory growing component routes");
  memset(resized + routes->capacity, 0,
         (capacity - routes->capacity) * sizeof(*resized));
  routes->items = resized;
  routes->capacity = capacity;
  return ARCHBIRD_OK;
}

static ArchbirdStatus route_add(AbOkfPublication *pub, ComponentRoutes *routes,
                                const AbString *source, const AbString *target,
                                const AbString *kind, const AbValue *names) {
  size_t index;
  ComponentRoute *route = NULL;
  ArchbirdStatus status;
  for (index = 0; index < routes->count; index++) {
    ComponentRoute *candidate = &routes->items[index];
    if (ab_string_equal(&candidate->source, source) &&
        ab_string_equal(&candidate->target, target) &&
        ab_string_equal(&candidate->kind, kind)) {
      route = candidate;
      break;
    }
  }
  if (!route) {
    status = routes_reserve(pub, routes, routes->count + 1);
    if (status != ARCHBIRD_OK)
      return status;
    route = &routes->items[routes->count];
    status = ab_okf_pub_copy(pub, &route->source, source->data, source->length);
    if (status == ARCHBIRD_OK)
      status =
          ab_okf_pub_copy(pub, &route->target, target->data, target->length);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &route->kind, kind->data, kind->length);
    if (status != ARCHBIRD_OK)
      return status;
    routes->count++;
  }
  if (!names || names->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid component edge names");
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *name = &names->as.array.items[index];
    if (name->kind != AB_VALUE_STRING)
      return ab_okf_pub_error(pub, "invalid component edge name");
    status = route_add_name(pub, route, &name->as.text);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (routes->count > 1)
    qsort(routes->items, routes->count, sizeof(*routes->items), route_compare);
  return ARCHBIRD_OK;
}

static ArchbirdStatus component_routes(AbOkfPublication *pub,
                                       ComponentRoutes *out) {
  const AbValue *components = array_or_empty(&pub->map, "components");
  const AbValue *edges = array_or_empty(&pub->map, "edges");
  size_t edge_index;
  size_t source_index;
  size_t target_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!components || !edges)
    return ab_okf_pub_error(pub, "invalid component route inventories");
  for (edge_index = 0;
       status == ARCHBIRD_OK && edge_index < edges->as.array.count;
       edge_index++) {
    const AbValue *edge = &edges->as.array.items[edge_index];
    const AbString *source_path = ab_okf_pub_text(edge, "source");
    const AbString *target_path = ab_okf_pub_text(edge, "target");
    const AbString *kind = ab_okf_pub_text(edge, "kind");
    const AbValue *names = array_or_empty(edge, "names");
    if (!source_path || !target_path || !kind || !names)
      return ab_okf_pub_error(pub, "invalid Map edge for component routes");
    for (source_index = 0; source_index < components->as.array.count;
         source_index++) {
      const AbValue *source_component =
          &components->as.array.items[source_index];
      const AbString *source_name = ab_okf_pub_text(source_component, "name");
      const AbValue *source_files = array_or_empty(source_component, "files");
      if (!source_name || !source_files)
        return ab_okf_pub_error(pub, "invalid Map component membership");
      if (!string_array_contains(source_files, source_path))
        continue;
      for (target_index = 0; target_index < components->as.array.count;
           target_index++) {
        const AbValue *target_component =
            &components->as.array.items[target_index];
        const AbString *target_name = ab_okf_pub_text(target_component, "name");
        const AbValue *target_files = array_or_empty(target_component, "files");
        if (!target_name || !target_files)
          return ab_okf_pub_error(pub, "invalid Map component membership");
        if (ab_string_equal(source_name, target_name) ||
            !string_array_contains(target_files, target_path))
          continue;
        status = route_add(pub, out, source_name, target_name, kind, names);
        if (status != ARCHBIRD_OK)
          break;
      }
    }
  }
  return status;
}

static void component_routes_free(AbOkfPublication *pub,
                                  ComponentRoutes *routes) {
  size_t index;
  size_t name;
  for (index = 0; index < routes->count; index++) {
    ComponentRoute *row = &routes->items[index];
    ab_string_free(pub->engine, &row->source);
    ab_string_free(pub->engine, &row->target);
    ab_string_free(pub->engine, &row->kind);
    for (name = 0; name < row->name_count; name++)
      ab_string_free(pub->engine, &row->names[name]);
    ab_free(pub->engine, row->names);
  }
  ab_free(pub->engine, routes->items);
  memset(routes, 0, sizeof(*routes));
}

static int component_intersects_files(const AbValue *component,
                                      const AbValue *files,
                                      const AbString *layer) {
  const AbValue *members = array_or_empty(component, "files");
  size_t file_index;
  if (!members)
    return 0;
  for (file_index = 0; file_index < files->as.array.count; file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbString *path = ab_okf_pub_text(file, "path");
    const AbString *file_layer = ab_okf_pub_text(file, "layer");
    if (path && file_layer && ab_string_equal(file_layer, layer) &&
        string_array_contains(members, path))
      return 1;
  }
  return 0;
}

static ArchbirdStatus add_layers(AbOkfPublication *pub,
                                 const MapPathSet *layers,
                                 const MapPathSet *components) {
  static const char *const summary_headers[] = {"Role", "Language", "Files",
                                                "Symbols", "Bytes"};
  static const char *const file_headers[] = {"Path", "Language", "Symbols",
                                             "SHA-256"};
  const AbValue *layer_rows = array_or_empty(&pub->map, "layers");
  const AbValue *component_rows = array_or_empty(&pub->map, "components");
  const AbValue *files = array_or_empty(&pub->map, "files");
  size_t layer_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!layer_rows || !component_rows || !files)
    return ab_okf_pub_error(pub, "invalid Map layer publication inputs");
  for (layer_index = 0; status == ARCHBIRD_OK && layer_index < layers->count;
       layer_index++) {
    const MapPath *path = &layers->items[layer_index];
    const AbValue *layer = row_by_name(layer_rows, &path->name, "name");
    const AbString *role = ab_okf_pub_text(layer, "role");
    const AbString *language = ab_okf_pub_text(layer, "language");
    uint64_t file_count = 0;
    uint64_t symbol_count = 0;
    uint64_t bytes = 0;
    size_t file_index;
    size_t component_index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[3] = {
        {(char *)"architecture", 12}, {(char *)"layer", 5}, {0}};
    AbBuffer body;
    if (!layer || !role || !language)
      return ab_okf_pub_error(pub, "invalid Map layer row");
    tags[2] = *role;
    for (file_index = 0; file_index < files->as.array.count; file_index++) {
      const AbValue *file = &files->as.array.items[file_index];
      const AbString *file_layer = ab_okf_pub_text(file, "layer");
      const AbValue *symbols = array_or_empty(file, "symbols");
      uint64_t file_bytes;
      if (!file_layer || !symbols ||
          !ab_okf_pub_u64(file, "bytes", &file_bytes))
        return ab_okf_pub_error(pub, "invalid Map file in layer");
      if (ab_string_equal(file_layer, &path->name)) {
        file_count++;
        symbol_count += symbols->as.array.count;
        bytes += file_bytes;
      }
    }
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                        "architecture/project");
    for (component_index = 0;
         status == ARCHBIRD_OK && component_index < components->count;
         component_index++) {
      const MapPath *component_path = &components->items[component_index];
      const AbValue *component =
          row_by_name(component_rows, &component_path->name, "name");
      if (component &&
          component_intersects_files(component, files, &path->name))
        status = relation_target(pub, &relations, "contains_component",
                                 &component_path->path);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_append(&value, role->data, role->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, language->data, language->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " layer with ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, file_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " mapped files.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " layer\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 5);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, role);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, language);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, file_count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, symbol_count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, bytes);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Components\n\n");
    {
      size_t linked = 0;
      for (component_index = 0;
           status == ARCHBIRD_OK && component_index < components->count;
           component_index++) {
        const MapPath *component_path = &components->items[component_index];
        const AbValue *component =
            row_by_name(component_rows, &component_path->name, "name");
        if (!component ||
            !component_intersects_files(component, files, &path->name))
          continue;
        status = ab_buffer_literal(&body, "* ");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_relative_link(&body, path->path.data,
                                            component_path->path.data,
                                            &component_path->name);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&body, "\n");
        linked++;
      }
      if (status == ARCHBIRD_OK && !linked)
        status = ab_buffer_literal(
            &body, "_No configured component intersects this layer._\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Files\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, file_headers, 4);
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < files->as.array.count;
         file_index++) {
      const AbValue *file = &files->as.array.items[file_index];
      const AbString *file_layer = ab_okf_pub_text(file, "layer");
      const AbString *file_path = ab_okf_pub_text(file, "path");
      const AbString *file_language = ab_okf_pub_text(file, "language");
      const AbString *sha = ab_okf_pub_text(file, "sha256");
      const AbValue *symbols = array_or_empty(file, "symbols");
      if (!file_layer || !file_path || !file_language || !sha || !symbols)
        return ab_okf_pub_error(pub, "invalid Map layer file row");
      if (!ab_string_equal(file_layer, &path->name))
        continue;
      status = md_row_start(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, file_path);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, file_language);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = md_u64(&body, symbols->as.array.count);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, sha);
      if (status == ARCHBIRD_OK)
        status = md_row_end(&body);
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Layer",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "layer",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static ArchbirdStatus relation_component_edge(AbOkfPublication *pub,
                                              AbOkfPubRelationList *relations,
                                              const ComponentRoute *route,
                                              const MapPath *target) {
  AbOkfPubField fields[4] = {
      {"edge_kind", {0}}, {"kind", {0}}, {"names", {0}}, {"target", {0}}};
  AbBuffer names;
  AbBuffer target_id;
  size_t index;
  ArchbirdStatus status =
      ab_okf_pub_json_text(pub, &fields[0].json, &route->kind);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_literal(pub, &fields[1].json, "component_edge");
  ab_buffer_init(&names, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&names, "[");
  for (index = 0; status == ARCHBIRD_OK && index < route->name_count; index++) {
    if (index)
      status = ab_buffer_literal(&names, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&names, route->names[index].data,
                                     route->names[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&names, "]");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &names, &fields[2].json);
  ab_buffer_free(&names);
  ab_buffer_init(&target_id, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&target_id, target->path.data,
                              target->path.length - 3);
  if (status == ARCHBIRD_OK) {
    AbString value = {(char *)target_id.data, target_id.length};
    status = ab_okf_pub_json_text(pub, &fields[3].json, &value);
  }
  ab_buffer_free(&target_id);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation(pub, relations, fields, 4);
  ab_okf_pub_fields_free(pub, fields, 4);
  return status;
}

static ArchbirdStatus append_route_names(AbBuffer *body,
                                         const ComponentRoute *route) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < route->name_count; index++) {
    if (index)
      status = ab_buffer_literal(body, ", ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(body, &route->names[index]);
  }
  return status;
}

static ArchbirdStatus add_components(AbOkfPublication *pub,
                                     const MapPathSet *components,
                                     ComponentRoutes *routes) {
  static const char *const inventory_headers[] = {"Files", "Symbols"};
  static const char *const route_headers[] = {"Target", "Edge kind", "Names"};
  static const char *const incoming_headers[] = {"Source", "Edge kind",
                                                 "Names"};
  const AbValue *rows = array_or_empty(&pub->map, "components");
  size_t component_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows)
    return ab_okf_pub_error(pub, "invalid Map component inventory");
  for (component_index = 0;
       status == ARCHBIRD_OK && component_index < components->count;
       component_index++) {
    const MapPath *path = &components->items[component_index];
    const AbValue *component = row_by_name(rows, &path->name, "name");
    const AbString *raw_description = text_or_empty(component, "description");
    const AbValue *files = array_or_empty(component, "files");
    uint64_t symbol_count;
    size_t route_index;
    size_t file_index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"architecture", 12}, {(char *)"component", 9}};
    AbBuffer body;
    if (!component || !raw_description || !files ||
        !ab_okf_pub_u64(component, "symbol_count", &symbol_count))
      return ab_okf_pub_error(pub, "invalid Map component row");
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of",
                                        "architecture/project");
    for (route_index = 0; status == ARCHBIRD_OK && route_index < routes->count;
         route_index++) {
      const ComponentRoute *route = &routes->items[route_index];
      const MapPath *target;
      if (!ab_string_equal(&route->source, &path->name))
        continue;
      target = path_find(components, &route->target);
      if (target)
        status = relation_component_edge(pub, &relations, route, target);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK)
      status =
          raw_description->length
              ? ab_okf_pub_copy(pub, &description, raw_description->data,
                                raw_description->length)
              : dynamic_text(pub, &description,
                             "Architecture component containing ", NULL, "");
    if (status == ARCHBIRD_OK && !raw_description->length) {
      AbBuffer value;
      ab_string_free(pub->engine, &description);
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Architecture component containing ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, files->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " files.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK && raw_description->length)
      status = ab_okf_pub_plain(&body, raw_description);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "Configured architecture component.");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n# Inventory\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, inventory_headers, 2);
    if (status == ARCHBIRD_OK)
      status = md_row_start(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, files->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = md_u64(&body, symbol_count);
    if (status == ARCHBIRD_OK)
      status = md_row_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Outgoing component relations\n\n");
    {
      size_t outgoing = 0;
      for (route_index = 0; route_index < routes->count; route_index++)
        if (ab_string_equal(&routes->items[route_index].source, &path->name))
          outgoing++;
      if (status == ARCHBIRD_OK && outgoing)
        status = md_header(&body, route_headers, 3);
      else if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "_None._\n");
      for (route_index = 0;
           status == ARCHBIRD_OK && route_index < routes->count;
           route_index++) {
        const ComponentRoute *route = &routes->items[route_index];
        const MapPath *target;
        if (!ab_string_equal(&route->source, &path->name))
          continue;
        target = path_find(components, &route->target);
        status = md_row_start(&body);
        if (status == ARCHBIRD_OK && target)
          status = ab_okf_pub_relative_link(&body, path->path.data,
                                            target->path.data, &target->name);
        else if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &route->target);
        if (status == ARCHBIRD_OK)
          status = md_cell(&body);
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &route->kind);
        if (status == ARCHBIRD_OK)
          status = md_cell(&body);
        if (status == ARCHBIRD_OK)
          status = append_route_names(&body, route);
        if (status == ARCHBIRD_OK)
          status = md_row_end(&body);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Incoming component relations\n\n");
    {
      size_t incoming = 0;
      for (route_index = 0; route_index < routes->count; route_index++)
        if (ab_string_equal(&routes->items[route_index].target, &path->name))
          incoming++;
      if (status == ARCHBIRD_OK && incoming)
        status = md_header(&body, incoming_headers, 3);
      else if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "_None._\n");
      for (route_index = 0;
           status == ARCHBIRD_OK && route_index < routes->count;
           route_index++) {
        const ComponentRoute *route = &routes->items[route_index];
        const MapPath *source;
        if (!ab_string_equal(&route->target, &path->name))
          continue;
        source = path_find(components, &route->source);
        status = md_row_start(&body);
        if (status == ARCHBIRD_OK && source)
          status = ab_okf_pub_relative_link(&body, path->path.data,
                                            source->path.data, &source->name);
        else if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &route->source);
        if (status == ARCHBIRD_OK)
          status = md_cell(&body);
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &route->kind);
        if (status == ARCHBIRD_OK)
          status = md_cell(&body);
        if (status == ARCHBIRD_OK)
          status = append_route_names(&body, route);
        if (status == ARCHBIRD_OK)
          status = md_row_end(&body);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Files\n\n");
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < files->as.array.count;
         file_index++) {
      const AbValue *file = &files->as.array.items[file_index];
      if (file->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid component file path");
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &file->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->map_source,
                               path->path.data,
                               "Archbird Component",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "component",
                               &path->name,
                               tags,
                               2,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}
