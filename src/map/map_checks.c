#include "map_internal.h"

#include "archbird_internal.h"

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

ArchbirdStatus ab_map_run_diagnostics(AbMapState *state) {
  ArchbirdStatus status = layer_diagnostics(state);
  if (status == ARCHBIRD_OK)
    status = component_diagnostics(state);
  return status;
}
