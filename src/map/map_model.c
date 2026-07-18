#include "map_internal.h"

#include "archbird_internal.h"

#include <stdlib.h>
#include <string.h>

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static void pairs_free(ArchbirdEngine *engine, AbStringPair *pairs,
                       size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &pairs[index].key);
    ab_string_free(engine, &pairs[index].value);
  }
  ab_free(engine, pairs);
}

static ArchbirdStatus add_diagnostic(AbMapState *state, const char *severity,
                                     const char *code, const char *message,
                                     const AbString *path, int has_span,
                                     size_t span_start, size_t span_end) {
  AbMapDiagnostic *resized;
  AbMapDiagnostic *diagnostic;
  ArchbirdStatus status;
  if (!state || !state->engine || !severity || !code || !message)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (state->diagnostic_count == state->diagnostic_capacity) {
    size_t capacity =
        state->diagnostic_capacity ? state->diagnostic_capacity * 2 : 16;
    if (capacity < state->diagnostic_capacity ||
        capacity > SIZE_MAX / sizeof(*state->diagnostics))
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET, "too many map diagnostics");
    resized =
        (AbMapDiagnostic *)ab_realloc(state->engine, state->diagnostics,
                                      capacity * sizeof(*state->diagnostics));
    if (!resized)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing map diagnostics");
    state->diagnostics = resized;
    state->diagnostic_capacity = capacity;
  }
  diagnostic = &state->diagnostics[state->diagnostic_count];
  memset(diagnostic, 0, sizeof(*diagnostic));
  status = ab_string_copy(state->engine, &diagnostic->severity, severity,
                          strlen(severity));
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(state->engine, &diagnostic->code, code, strlen(code));
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &diagnostic->message, message,
                            strlen(message));
  if (status == ARCHBIRD_OK && path)
    status = ab_string_copy(state->engine, &diagnostic->path, path->data,
                            path->length);
  if (status == ARCHBIRD_OK && has_span) {
    diagnostic->span_start = span_start;
    diagnostic->span_end = span_end;
    diagnostic->has_span = 1;
  }
  if (status != ARCHBIRD_OK) {
    ab_string_free(state->engine, &diagnostic->severity);
    ab_string_free(state->engine, &diagnostic->code);
    ab_string_free(state->engine, &diagnostic->message);
    ab_string_free(state->engine, &diagnostic->path);
    return status;
  }
  state->diagnostic_count++;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_map_add_diagnostic(AbMapState *state, const char *severity,
                                     const char *code, const char *message,
                                     const AbString *path) {
  return add_diagnostic(state, severity, code, message, path, 0, 0, 0);
}

ArchbirdStatus ab_map_add_diagnostic_span(AbMapState *state,
                                          const char *severity,
                                          const char *code, const char *message,
                                          const AbString *path,
                                          size_t span_start, size_t span_end) {
  if (!path || span_end < span_start)
    return ARCHBIRD_INVALID_ARGUMENT;
  return add_diagnostic(state, severity, code, message, path, 1, span_start,
                        span_end);
}

static int diagnostic_compare(const void *left_raw, const void *right_raw) {
  const AbMapDiagnostic *left = (const AbMapDiagnostic *)left_raw;
  const AbMapDiagnostic *right = (const AbMapDiagnostic *)right_raw;
  int compared = ab_string_compare(&left->severity, &right->severity);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->code, &right->code);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->message, &right->message);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->path, &right->path);
  if (compared)
    return compared;
  if (left->has_span != right->has_span)
    return left->has_span ? 1 : -1;
  if (!left->has_span)
    return 0;
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  return left->span_end < right->span_end ? -1
                                          : left->span_end > right->span_end;
}

void ab_map_diagnostics_sort_unique(AbMapState *state) {
  size_t read;
  size_t write = 0;
  if (!state || state->diagnostic_count < 2)
    return;
  qsort(state->diagnostics, state->diagnostic_count,
        sizeof(*state->diagnostics), diagnostic_compare);
  for (read = 0; read < state->diagnostic_count; read++) {
    if (write && diagnostic_compare(&state->diagnostics[write - 1],
                                    &state->diagnostics[read]) == 0) {
      ab_string_free(state->engine, &state->diagnostics[read].severity);
      ab_string_free(state->engine, &state->diagnostics[read].code);
      ab_string_free(state->engine, &state->diagnostics[read].message);
      ab_string_free(state->engine, &state->diagnostics[read].path);
      continue;
    }
    if (write != read) {
      state->diagnostics[write] = state->diagnostics[read];
      memset(&state->diagnostics[read], 0, sizeof(state->diagnostics[read]));
    }
    write++;
  }
  state->diagnostic_count = write;
}

void ab_map_package_clear(ArchbirdEngine *engine, AbMapPackage *package) {
  size_t index;
  ab_string_free(engine, &package->name);
  ab_string_free(engine, &package->kind);
  ab_string_free(engine, &package->layer);
  ab_string_free(engine, &package->manifest);
  ab_string_free(engine, &package->identity);
  ab_string_free(engine, &package->version);
  string_array_free(engine, &package->aliases);
  for (index = 0; index < package->dependency_count; index++) {
    ab_string_free(engine, &package->dependencies[index].name);
    ab_string_free(engine, &package->dependencies[index].requirement);
    ab_string_free(engine, &package->dependencies[index].scope);
  }
  ab_free(engine, package->dependencies);
  pairs_free(engine, package->entrypoints, package->entrypoint_count);
  string_array_free(engine, &package->npm_runtime_entries);
  string_array_free(engine, &package->exports);
  for (index = 0; index < package->export_origin_count; index++) {
    ab_string_free(engine, &package->export_origins[index].name);
    ab_string_free(engine, &package->export_origins[index].target_symbol);
    string_array_free(engine, &package->export_origins[index].paths);
  }
  ab_free(engine, package->export_origins);
  for (index = 0; index < package->entrypoint_surface_count; index++) {
    AbMapEntrypointSurface *surface = &package->entrypoint_surfaces[index];
    size_t origin;
    ab_string_free(engine, &surface->path);
    string_array_free(engine, &surface->exports);
    for (origin = 0; origin < surface->export_origin_count; origin++) {
      ab_string_free(engine, &surface->export_origins[origin].name);
      ab_string_free(engine, &surface->export_origins[origin].target_symbol);
      string_array_free(engine, &surface->export_origins[origin].paths);
    }
    ab_free(engine, surface->export_origins);
  }
  ab_free(engine, package->entrypoint_surfaces);
  pairs_free(engine, package->scripts, package->script_count);
  memset(package, 0, sizeof(*package));
}

static void build_free(ArchbirdEngine *engine, AbMapBuildRoute *build) {
  ab_string_free(engine, &build->source);
  ab_string_free(engine, &build->name);
  string_array_free(engine, &build->deps);
  string_array_free(engine, &build->paths);
  ab_string_free(engine, &build->command);
  string_array_free(engine, &build->conditions);
  memset(build, 0, sizeof(*build));
}

static void artifact_free(ArchbirdEngine *engine, AbMapArtifact *artifact) {
  size_t index;
  ab_string_free(engine, &artifact->name);
  ab_string_free(engine, &artifact->output);
  for (index = 0; index < artifact->input_count; index++) {
    ab_string_free(engine, &artifact->inputs[index].path);
    string_array_free(engine, &artifact->inputs[index].evidence);
  }
  ab_free(engine, artifact->inputs);
  for (index = 0; index < artifact->loader_count; index++) {
    ab_string_free(engine, &artifact->loaders[index].path);
    ab_string_free(engine, &artifact->loaders[index].pattern);
  }
  ab_free(engine, artifact->loaders);
  for (index = 0; index < artifact->build_count; index++) {
    ab_string_free(engine, &artifact->builds[index].source);
    ab_string_free(engine, &artifact->builds[index].target);
  }
  ab_free(engine, artifact->builds);
  string_array_free(engine, &artifact->depends_on);
  memset(artifact, 0, sizeof(*artifact));
}

static void surface_declarations_free(ArchbirdEngine *engine,
                                      AbMapSurfaceDeclaration *rows,
                                      size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &rows[index].path);
    ab_string_free(engine, &rows[index].source);
  }
  ab_free(engine, rows);
}

static void surface_free(ArchbirdEngine *engine, AbMapSurface *surface) {
  size_t index;
  ab_string_free(engine, &surface->name);
  ab_string_free(engine, &surface->kind);
  surface_declarations_free(engine, surface->providers,
                            surface->provider_count);
  for (index = 0; index < surface->name_count; index++) {
    size_t use;
    AbMapSurfaceName *name = &surface->names[index];
    ab_string_free(engine, &name->name);
    ab_string_free(engine, &name->declaration);
    surface_declarations_free(engine, name->declarations,
                              name->declaration_count);
    for (use = 0; use < name->use_count; use++)
      ab_string_free(engine, &name->uses[use].path);
    ab_free(engine, name->uses);
    string_array_free(engine, &name->candidates);
    ab_string_free(engine, &name->resolution);
    string_array_free(engine, &name->declaration_signatures);
    string_array_free(engine, &name->implementation_signatures);
  }
  ab_free(engine, surface->names);
  memset(surface, 0, sizeof(*surface));
}

static void route_counts_free(ArchbirdEngine *engine, AbMapRouteCount *routes,
                              size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(engine, &routes[index].path);
  ab_free(engine, routes);
}

static void route_evidence_free(ArchbirdEngine *engine,
                                AbMapRouteEvidence *rows, size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &rows[index].target);
    ab_string_free(engine, &rows[index].target_symbol);
    ab_string_free(engine, &rows[index].fact_id);
    ab_string_free(engine, &rows[index].relation);
    ab_string_free(engine, &rows[index].provenance);
    ab_string_free(engine, &rows[index].provider);
    ab_string_free(engine, &rows[index].claim);
    ab_string_free(engine, &rows[index].enclosing);
    ab_string_free(engine, &rows[index].name);
    ab_string_free(engine, &rows[index].scope);
    ab_string_free(engine, &rows[index].observation_sha256);
    ab_string_free(engine, &rows[index].evidence_slice_sha256);
    ab_string_free(engine, &rows[index].producer_version);
    ab_string_free(engine, &rows[index].producer_implementation_sha256);
    ab_string_free(engine, &rows[index].producer_configuration_sha256);
    ab_string_free(engine, &rows[index].runtime);
  }
  ab_free(engine, rows);
}

static void test_free(ArchbirdEngine *engine, AbMapTest *test) {
  size_t index;
  ab_string_free(engine, &test->path);
  ab_string_free(engine, &test->group);
  ab_string_free(engine, &test->language);
  ab_string_free(engine, &test->framework);
  string_array_free(engine, &test->generated_from);
  string_array_free(engine, &test->selectors);
  route_counts_free(engine, test->routes, test->route_count);
  route_evidence_free(engine, test->route_evidence, test->route_evidence_count);
  for (index = 0; index < test->case_count; index++) {
    ab_string_free(engine, &test->cases[index].selector);
    ab_string_free(engine, &test->cases[index].evidence_kind);
    route_counts_free(engine, test->cases[index].routes,
                      test->cases[index].route_count);
    route_evidence_free(engine, test->cases[index].route_evidence,
                        test->cases[index].route_evidence_count);
    string_array_free(engine, &test->cases[index].configured_routes);
  }
  ab_free(engine, test->cases);
  memset(test, 0, sizeof(*test));
}

static void named_entry_free(ArchbirdEngine *engine, AbMapNamedEntry *entry) {
  size_t index;
  ab_string_free(engine, &entry->name);
  for (index = 0; index < entry->path_count; index++) {
    ab_string_free(engine, &entry->paths[index].path);
    string_array_free(engine, &entry->paths[index].names);
  }
  ab_free(engine, entry->paths);
  memset(entry, 0, sizeof(*entry));
}

static void parity_free(ArchbirdEngine *engine, AbMapParity *parity) {
  size_t member_index;
  ab_string_free(engine, &parity->name);
  string_array_free(engine, &parity->shared);
  for (member_index = 0; member_index < parity->member_count; member_index++) {
    AbMapParityMember *member = &parity->members[member_index];
    size_t evidence_index;
    ab_string_free(engine, &member->label);
    string_array_free(engine, &member->values);
    string_array_free(engine, &member->missing);
    for (evidence_index = 0; evidence_index < member->evidence_count;
         evidence_index++) {
      ab_string_free(engine, &member->evidence[evidence_index].name);
      string_array_free(engine, &member->evidence[evidence_index].locations);
    }
    ab_free(engine, member->evidence);
  }
  ab_free(engine, parity->members);
  memset(parity, 0, sizeof(*parity));
}

static void index_free(ArchbirdEngine *engine, AbMapIndex *index) {
  ab_string_free(engine, &index->name);
  ab_string_free(engine, &index->format);
  ab_string_free(engine, &index->path);
  ab_string_free(engine, &index->path_prefix);
  ab_string_free(engine, &index->evidence_state);
  ab_string_free(engine, &index->sha256);
  ab_string_free(engine, &index->tool_name);
  ab_string_free(engine, &index->tool_version);
  memset(index, 0, sizeof(*index));
}

void ab_map_state_free(AbMapState *state) {
  size_t index;
  if (!state)
    return;
  ab_map_graph_free(state->engine, &state->graph);
  for (index = 0; index < state->package_count; index++)
    ab_map_package_clear(state->engine, &state->packages[index]);
  ab_free(state->engine, state->packages);
  for (index = 0; index < state->build_count; index++)
    build_free(state->engine, &state->builds[index]);
  ab_free(state->engine, state->builds);
  for (index = 0; index < state->artifact_count; index++)
    artifact_free(state->engine, &state->artifacts[index]);
  ab_free(state->engine, state->artifacts);
  for (index = 0; index < state->surface_count; index++)
    surface_free(state->engine, &state->surfaces[index]);
  ab_free(state->engine, state->surfaces);
  for (index = 0; index < state->test_count; index++)
    test_free(state->engine, &state->tests[index]);
  ab_free(state->engine, state->tests);
  for (index = 0; index < state->named_entry_count; index++)
    named_entry_free(state->engine, &state->named_entries[index]);
  ab_free(state->engine, state->named_entries);
  for (index = 0; index < state->parity_count; index++)
    parity_free(state->engine, &state->parity[index]);
  ab_free(state->engine, state->parity);
  for (index = 0; index < state->index_count; index++)
    index_free(state->engine, &state->indexes[index]);
  ab_free(state->engine, state->indexes);
  for (index = 0; index < state->diagnostic_count; index++) {
    ab_string_free(state->engine, &state->diagnostics[index].severity);
    ab_string_free(state->engine, &state->diagnostics[index].code);
    ab_string_free(state->engine, &state->diagnostics[index].message);
    ab_string_free(state->engine, &state->diagnostics[index].path);
  }
  ab_free(state->engine, state->diagnostics);
  memset(state, 0, sizeof(*state));
}
