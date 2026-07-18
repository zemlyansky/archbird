#include "map_internal.h"

#include "archbird_internal.h"

#include <stdlib.h>
#include <string.h>

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const AbManifestFile *fact_file(const AbMapState *state,
                                       const AbFact *fact) {
  return ab_map_manifest_file(state->manifest, fact->path.data,
                              fact->path.length);
}

static int fact_domain(const AbFact *fact, const char *domain) {
  return string_literal(&fact->domain, domain);
}

static ArchbirdStatus add_message(AbMapState *state, const char *code,
                                  const AbString *path, size_t part_count,
                                  const AbString *const *parts,
                                  const char *const *literals) {
  AbBuffer message;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&message, state->engine);
  for (index = 0; status == ARCHBIRD_OK && index < part_count; index++) {
    if (literals[index])
      status = ab_buffer_literal(&message, literals[index]);
    if (status == ARCHBIRD_OK && parts[index])
      status =
          ab_buffer_append(&message, parts[index]->data, parts[index]->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_map_add_diagnostic(
        state, "error", code, message.data ? (const char *)message.data : "",
        path);
  ab_buffer_free(&message);
  return status;
}

static int mapped_path(const AbMapState *state, const AbString *path) {
  const AbManifestFile *file =
      ab_map_manifest_file(state->manifest, path->data, path->length);
  return file && file->has_layer;
}

static int layer_has_files(const AbMapState *state,
                           const AbConfigLayer *layer) {
  size_t index;
  for (index = 0; index < state->manifest->file_count; index++) {
    const AbManifestFile *file = &state->manifest->files[index];
    if (file->has_layer && ab_string_equal(&file->layer, &layer->name))
      return 1;
  }
  return 0;
}

static int path_matches_any(const AbString *path, const AbStringArray *patterns,
                            int collection) {
  size_t index;
  for (index = 0; index < patterns->count; index++) {
    if ((collection ? ab_map_collection_match(path, &patterns->items[index])
                    : ab_map_path_match(path, &patterns->items[index])))
      return 1;
  }
  return 0;
}

static ArchbirdStatus layer_diagnostics(AbMapState *state) {
  size_t layer_index;
  size_t file_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (layer_index = 0;
       status == ARCHBIRD_OK && layer_index < state->config->layer_count;
       layer_index++) {
    const AbConfigLayer *layer = &state->config->layers[layer_index];
    size_t header_index;
    if (layer->required && !layer_has_files(state, layer)) {
      AbBuffer globs;
      size_t glob;
      ab_buffer_init(&globs, state->engine);
      for (glob = 0; status == ARCHBIRD_OK && glob < layer->globs.count;
           glob++) {
        if (glob)
          status = ab_buffer_literal(&globs, ", ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&globs, layer->globs.items[glob].data,
                                    layer->globs.items[glob].length);
      }
      if (status == ARCHBIRD_OK) {
        AbString globs_view = {(char *)globs.data, globs.length};
        const AbString *parts[] = {&layer->name, &globs_view};
        const char *literals[] = {"layer ", ": "};
        status = add_message(state, "unmatched-layer-glob", NULL, 2, parts,
                             literals);
      }
      ab_buffer_free(&globs);
    }
    for (header_index = 0;
         status == ARCHBIRD_OK && header_index < layer->public_headers.count;
         header_index++) {
      const AbString *header = &layer->public_headers.items[header_index];
      if (!ab_map_manifest_file(state->manifest, header->data,
                                header->length)) {
        const AbString *parts[] = {&layer->name};
        const char *literals[] = {"layer "};
        status = add_message(state, "missing-public-header", header, 1, parts,
                             literals);
      }
    }
  }
  for (file_index = 0;
       status == ARCHBIRD_OK && file_index < state->manifest->file_count;
       file_index++) {
    const AbManifestFile *file = &state->manifest->files[file_index];
    int seen = 0;
    if (!file->has_layer)
      continue;
    for (layer_index = 0;
         status == ARCHBIRD_OK && layer_index < state->config->layer_count;
         layer_index++) {
      const AbConfigLayer *layer = &state->config->layers[layer_index];
      if (!path_matches_any(&file->path, &layer->globs, 1))
        continue;
      if (seen) {
        const AbString *parts[] = {&layer->name};
        const char *literals[] = {
            "matched by more than one configured layer including "};
        status = add_message(state, "overlapping-layers", &file->path, 1, parts,
                             literals);
      }
      seen = 1;
    }
  }
  return status;
}

static ArchbirdStatus component_diagnostics(AbMapState *state) {
  size_t component_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (component_index = 0; status == ARCHBIRD_OK &&
                            component_index < state->config->component_count;
       component_index++) {
    const AbConfigComponent *component =
        &state->config->components[component_index];
    size_t file_index;
    int matched = 0;
    if (!component->required)
      continue;
    for (file_index = 0; !matched && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      matched = file->has_layer &&
                path_matches_any(&file->path, &component->paths, 1);
    }
    if (!matched) {
      AbBuffer paths;
      size_t path_index;
      ab_buffer_init(&paths, state->engine);
      for (path_index = 0;
           status == ARCHBIRD_OK && path_index < component->paths.count;
           path_index++) {
        if (path_index)
          status = ab_buffer_literal(&paths, ", ");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&paths, component->paths.items[path_index].data,
                               component->paths.items[path_index].length);
      }
      if (status == ARCHBIRD_OK) {
        AbString path_view = {(char *)paths.data, paths.length};
        const AbString *parts[] = {&component->name, &path_view};
        const char *literals[] = {"component ", ": "};
        status =
            add_message(state, "empty-component", NULL, 2, parts, literals);
      }
      ab_buffer_free(&paths);
    }
  }
  return status;
}

static int symbol_exists(const AbMapState *state, const AbString *layer,
                         const AbString *name, const AbString **path) {
  size_t index;
  for (index = 0; index < ab_project_merged_fact_count(state->project);
       index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbManifestFile *file;
    if (!fact->has_name || !fact_domain(fact, "symbols") ||
        !ab_string_equal(&fact->name, name))
      continue;
    file = fact_file(state, fact);
    if (file && file->has_layer && ab_string_equal(&file->layer, layer)) {
      if (path)
        *path = &file->path;
      return 1;
    }
  }
  return 0;
}

static int edge_matches(const AbMapEdgeMention *edge,
                        const AbConfigEdgeCheck *check) {
  return ab_map_glob_match(&check->kind, &edge->kind) &&
         ab_map_path_match(edge->source, &check->source) &&
         ab_map_path_match(&edge->target, &check->target) &&
         (!check->name.length || ab_map_glob_match(&check->name, &edge->name));
}

static const AbMapPackage *find_package(const AbMapState *state,
                                        const AbString *name) {
  size_t index;
  for (index = 0; index < state->package_count; index++) {
    if (ab_string_equal(&state->packages[index].name, name))
      return &state->packages[index];
  }
  return NULL;
}

static const AbMapSurface *find_surface(const AbMapState *state,
                                        const AbString *name) {
  size_t index;
  for (index = 0; index < state->surface_count; index++) {
    if (ab_string_equal(&state->surfaces[index].name, name))
      return &state->surfaces[index];
  }
  return NULL;
}

static ArchbirdStatus
surface_state_diagnostic(AbMapState *state, const AbConfigSurfaceCheck *check,
                         const AbMapSurface *surface, const char *wanted,
                         const char *code) {
  AbStringArray names = {0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!surface)
    return ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < surface->name_count;
       index++) {
    const AbMapSurfaceName *name = &surface->names[index];
    int matched = 0;
    if (name->ignored)
      continue;
    if (strcmp(wanted, "undeclared-use") == 0)
      matched =
          string_literal(&name->declaration, "undeclared") && name->use_count;
    else
      matched = string_literal(&name->resolution, wanted);
    if (matched) {
      AbString *resized = (AbString *)ab_realloc(
          state->engine, names.items, (names.count + 1) * sizeof(*names.items));
      if (!resized)
        status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory collecting surface checks");
      else {
        names.items = resized;
        memset(&names.items[names.count], 0, sizeof(*names.items));
        status = ab_string_copy(state->engine, &names.items[names.count],
                                name->name.data, name->name.length);
        if (status == ARCHBIRD_OK)
          names.count++;
      }
    }
  }
  if (status == ARCHBIRD_OK && names.count) {
    AbBuffer joined;
    ab_buffer_init(&joined, state->engine);
    for (index = 0; status == ARCHBIRD_OK && index < names.count; index++) {
      if (index)
        status = ab_buffer_literal(&joined, ", ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&joined, names.items[index].data,
                                  names.items[index].length);
    }
    if (status == ARCHBIRD_OK) {
      AbString joined_view = {(char *)joined.data, joined.length};
      const AbString *parts[] = {&check->bridge, &joined_view};
      const char *literals[] = {"bridge ", ": "};
      status = add_message(state, code, NULL, 2, parts, literals);
    }
    ab_buffer_free(&joined);
  }
  for (index = 0; index < names.count; index++)
    ab_string_free(state->engine, &names.items[index]);
  ab_free(state->engine, names.items);
  return status;
}

ArchbirdStatus ab_map_run_legacy_checks(AbMapState *state) {
  const AbConfigChecks *checks = &state->config->checks;
  size_t index;
  ArchbirdStatus status = layer_diagnostics(state);
  if (status == ARCHBIRD_OK)
    status = component_diagnostics(state);
  for (index = 0; status == ARCHBIRD_OK && index < checks->files.count;
       index++) {
    if (!mapped_path(state, &checks->files.items[index]))
      status = ab_map_add_diagnostic(state, "error", "required-file-unmapped",
                                     "required mapped file is absent",
                                     &checks->files.items[index]);
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->symbol_count;
       index++) {
    const AbConfigSymbolCheck *check = &checks->symbols[index];
    if (!symbol_exists(state, &check->layer, &check->name, NULL)) {
      const AbString *parts[] = {&check->layer, &check->name};
      const char *literals[] = {"layer ", ": "};
      status = add_message(state, "required-symbol-missing", NULL, 2, parts,
                           literals);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->edge_count;
       index++) {
    const AbConfigEdgeCheck *check = &checks->edges[index];
    size_t edge_index;
    int matched = 0;
    for (edge_index = 0; !matched && edge_index < state->graph.edge_count;
         edge_index++)
      matched = edge_matches(&state->graph.edges[edge_index], check);
    if (!matched) {
      AbBuffer message;
      ab_buffer_init(&message, state->engine);
      status = ab_buffer_append(&message, check->kind.data, check->kind.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, check->source.data,
                                  check->source.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " -> ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, check->target.data,
                                  check->target.length);
      if (status == ARCHBIRD_OK && check->name.length) {
        status = ab_buffer_literal(&message, " name=");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&message, check->name.data, check->name.length);
      }
      if (status == ARCHBIRD_OK)
        status = ab_map_add_diagnostic(state, "error", "required-edge-missing",
                                       (const char *)message.data, NULL);
      ab_buffer_free(&message);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->bridges.count;
       index++) {
    size_t edge_index;
    int matched = 0;
    AbBuffer kind;
    ab_buffer_init(&kind, state->engine);
    status = ab_buffer_literal(&kind, "bridge:");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&kind, checks->bridges.items[index].data,
                                checks->bridges.items[index].length);
    for (edge_index = 0; status == ARCHBIRD_OK && !matched &&
                         edge_index < state->graph.edge_count;
         edge_index++)
      matched = state->graph.edges[edge_index].kind.length == kind.length &&
                memcmp(state->graph.edges[edge_index].kind.data, kind.data,
                       kind.length) == 0;
    if (status == ARCHBIRD_OK && !matched) {
      const AbString *parts[] = {&checks->bridges.items[index]};
      const char *literals[] = {"bridge "};
      status =
          add_message(state, "required-bridge-empty", NULL, 1, parts, literals);
    }
    ab_buffer_free(&kind);
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->entrypoint_count;
       index++) {
    const AbConfigEntrypointCheck *check = &checks->entrypoints[index];
    const AbMapPackage *package = find_package(state, &check->package);
    const AbString *target = NULL;
    size_t route_index;
    if (package) {
      for (route_index = 0; route_index < package->entrypoint_count;
           route_index++) {
        if (ab_string_equal(&package->entrypoints[route_index].key,
                            &check->route)) {
          target = &package->entrypoints[route_index].value;
          break;
        }
      }
    }
    if (!target ||
        (check->target.length && !ab_map_path_match(target, &check->target))) {
      AbBuffer message;
      ab_buffer_init(&message, state->engine);
      status = ab_buffer_literal(&message, "package ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, check->package.data,
                                  check->package.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " route ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&message, check->route.data, check->route.length);
      if (status == ARCHBIRD_OK && check->target.length) {
        status = ab_buffer_literal(&message, " -> ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, check->target.data,
                                    check->target.length);
      }
      if (status == ARCHBIRD_OK)
        status = ab_map_add_diagnostic(
            state, "error", "required-entrypoint-missing",
            (const char *)message.data, package ? &package->manifest : NULL);
      ab_buffer_free(&message);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->test_route_count;
       index++) {
    const AbConfigTestRouteCheck *check = &checks->test_routes[index];
    size_t test_index;
    int matched = 0;
    for (test_index = 0; !matched && test_index < state->test_count;
         test_index++) {
      const AbMapTest *test = &state->tests[test_index];
      size_t route_index;
      if (!ab_string_equal(&test->group, &check->group))
        continue;
      for (route_index = 0; !matched && route_index < test->route_count;
           route_index++)
        matched =
            ab_map_path_match(&test->routes[route_index].path, &check->target);
    }
    if (!matched) {
      const AbString *parts[] = {&check->group, &check->target};
      const char *literals[] = {"test group ", " -> "};
      status = add_message(state, "required-test-route-missing", NULL, 2, parts,
                           literals);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->surface_count;
       index++) {
    const AbConfigSurfaceCheck *check = &checks->surfaces[index];
    const AbMapSurface *surface = find_surface(state, &check->bridge);
    size_t name_index;
    for (name_index = 0;
         status == ARCHBIRD_OK && name_index < check->declared.count;
         name_index++) {
      size_t row_index;
      int declared = 0;
      if (surface) {
        for (row_index = 0; row_index < surface->name_count; row_index++) {
          declared = ab_string_equal(&surface->names[row_index].name,
                                     &check->declared.items[name_index]) &&
                     string_literal(&surface->names[row_index].declaration,
                                    "declared");
          if (declared)
            break;
        }
      }
      if (!declared) {
        const AbString *parts[] = {&check->bridge,
                                   &check->declared.items[name_index]};
        const char *literals[] = {"bridge ", ": provider does not declare "};
        status = add_message(state, "required-surface-name-missing", NULL, 2,
                             parts, literals);
      }
    }
    if (status == ARCHBIRD_OK && check->forbid_unregistered)
      status = surface_state_diagnostic(state, check, surface, "undeclared-use",
                                        "surface-unregistered-use");
    if (status == ARCHBIRD_OK && check->forbid_unresolved)
      status = surface_state_diagnostic(state, check, surface, "unresolved",
                                        "surface-unresolved");
    if (status == ARCHBIRD_OK && check->forbid_ambiguous)
      status = surface_state_diagnostic(state, check, surface, "ambiguous",
                                        "surface-ambiguous");
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->forbid_paths.count;
       index++) {
    size_t file_index;
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      if (ab_map_collection_match(&file->path,
                                  &checks->forbid_paths.items[index])) {
        const AbString *parts[] = {&checks->forbid_paths.items[index]};
        const char *literals[] = {"forbidden path matched "};
        status = add_message(state, "forbidden-path-present", &file->path, 1,
                             parts, literals);
      }
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < checks->forbid_symbol_count;
       index++) {
    const AbConfigSymbolCheck *check = &checks->forbid_symbols[index];
    size_t fact_index;
    for (fact_index = 0;
         status == ARCHBIRD_OK &&
         fact_index < ab_project_merged_fact_count(state->project);
         fact_index++) {
      const AbFact *fact = ab_project_merged_fact(state->project, fact_index);
      const AbManifestFile *file;
      if (!fact->has_name || !fact_domain(fact, "symbols") ||
          !ab_string_equal(&fact->name, &check->name))
        continue;
      file = fact_file(state, fact);
      if (!file || !file->has_layer ||
          !ab_string_equal(&file->layer, &check->layer))
        continue;
      {
        const AbString *parts[] = {&check->layer, &check->name};
        const char *literals[] = {"layer ", ": "};
        status = add_message(state, "forbidden-symbol-present", &file->path, 2,
                             parts, literals);
      }
    }
  }
  return status;
}
