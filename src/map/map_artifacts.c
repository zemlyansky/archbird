#include "map_internal.h"

#include "archbird_internal.h"
#include "lexical/tokenizer.h"
#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static int input_compare(const void *left_raw, const void *right_raw) {
  const AbMapArtifactInput *left = (const AbMapArtifactInput *)left_raw;
  const AbMapArtifactInput *right = (const AbMapArtifactInput *)right_raw;
  return ab_string_compare(&left->path, &right->path);
}

static int loader_compare(const void *left_raw, const void *right_raw) {
  const AbMapArtifactLoader *left = (const AbMapArtifactLoader *)left_raw;
  const AbMapArtifactLoader *right = (const AbMapArtifactLoader *)right_raw;
  int compared = ab_string_compare(&left->path, &right->path);
  return compared ? compared
                  : ab_string_compare(&left->pattern, &right->pattern);
}

static int build_compare(const void *left_raw, const void *right_raw) {
  const AbMapArtifactBuild *left = (const AbMapArtifactBuild *)left_raw;
  const AbMapArtifactBuild *right = (const AbMapArtifactBuild *)right_raw;
  int compared = ab_string_compare(&left->source, &right->source);
  return compared ? compared : ab_string_compare(&left->target, &right->target);
}

static int artifact_compare(const void *left_raw, const void *right_raw) {
  const AbMapArtifact *left = (const AbMapArtifact *)left_raw;
  const AbMapArtifact *right = (const AbMapArtifact *)right_raw;
  return ab_string_compare(&left->name, &right->name);
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
  if (array->count == SIZE_MAX / sizeof(*array->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many artifact evidence values");
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting artifact evidence");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus artifact_input(AbMapState *state, AbMapArtifact *artifact,
                                     const AbString *path,
                                     AbMapArtifactInput **out) {
  AbMapArtifactInput *resized;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < artifact->input_count; index++) {
    if (ab_string_equal(&artifact->inputs[index].path, path)) {
      *out = &artifact->inputs[index];
      return ARCHBIRD_OK;
    }
  }
  if (artifact->input_count == SIZE_MAX / sizeof(*artifact->inputs))
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "too many artifact inputs");
  resized = (AbMapArtifactInput *)ab_realloc(state->engine, artifact->inputs,
                                             (artifact->input_count + 1) *
                                                 sizeof(*artifact->inputs));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting artifact inputs");
  artifact->inputs = resized;
  *out = &artifact->inputs[artifact->input_count];
  memset(*out, 0, sizeof(**out));
  status =
      ab_string_copy(state->engine, &(*out)->path, path->data, path->length);
  if (status == ARCHBIRD_OK)
    artifact->input_count++;
  return status;
}

static ArchbirdStatus collect_configured_inputs(AbMapState *state,
                                                const AbConfigArtifact *config,
                                                AbMapArtifact *artifact) {
  size_t pattern_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (pattern_index = 0;
       status == ARCHBIRD_OK && pattern_index < config->inputs.count;
       pattern_index++) {
    const AbString *pattern = &config->inputs.items[pattern_index];
    size_t file_index;
    size_t matches = 0;
    for (file_index = 0; file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      AbMapArtifactInput *input;
      if (!ab_map_collection_match(&file->path, pattern))
        continue;
      matches++;
      status = artifact_input(state, artifact, &file->path, &input);
      if (status == ARCHBIRD_OK)
        status =
            append_unique(state->engine, &input->evidence, "configured", 10);
      if (status != ARCHBIRD_OK)
        break;
      {
        size_t byte_limit = ab_manifest_file_has_role(file, "index")
                                ? state->config->max_index_bytes
                                : state->config->max_file_bytes;
        if (file->byte_length > byte_limit) {
          char message[160];
          snprintf(message, sizeof(message), "%zu bytes exceeds %zu",
                   file->byte_length, byte_limit);
          status = ab_map_add_diagnostic(
              state, "error", "artifact-input-too-large", message, &file->path);
        }
      }
    }
    if (status == ARCHBIRD_OK && config->required && !matches) {
      char message[256];
      snprintf(message, sizeof(message), "artifact %.*s: %.*s",
               (int)config->name.length, config->name.data,
               (int)pattern->length, pattern->data);
      status = ab_map_add_diagnostic(state, "error", "artifact-input-unmatched",
                                     message, NULL);
    }
  }
  return status;
}

static ArchbirdStatus append_loader(AbMapState *state, AbMapArtifact *artifact,
                                    const AbString *path,
                                    const AbString *pattern, size_t matches) {
  AbMapArtifactLoader *resized;
  AbMapArtifactLoader *loader;
  ArchbirdStatus status;
  if (artifact->loader_count == SIZE_MAX / sizeof(*artifact->loaders))
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "too many artifact loaders");
  resized = (AbMapArtifactLoader *)ab_realloc(state->engine, artifact->loaders,
                                              (artifact->loader_count + 1) *
                                                  sizeof(*artifact->loaders));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting artifact loaders");
  artifact->loaders = resized;
  loader = &artifact->loaders[artifact->loader_count];
  memset(loader, 0, sizeof(*loader));
  status =
      ab_string_copy(state->engine, &loader->path, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &loader->pattern, pattern->data,
                            pattern->length);
  if (status == ARCHBIRD_OK) {
    loader->matches = matches;
    artifact->loader_count++;
  }
  return status;
}

static ArchbirdStatus collect_loaders(AbMapState *state,
                                      const AbConfigArtifact *config,
                                      AbMapArtifact *artifact) {
  size_t loader_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (loader_index = 0;
       status == ARCHBIRD_OK && loader_index < config->loader_count;
       loader_index++) {
    const AbConfigArtifactLoader *loader = &config->loaders[loader_index];
    AbPattern *pattern = NULL;
    size_t path_matches = 0;
    size_t file_index;
    status =
        ab_pattern_compile(state->engine, &loader->pattern, SIZE_MAX, &pattern);
    for (file_index = 0;
         status == ARCHBIRD_OK && file_index < state->manifest->file_count;
         file_index++) {
      const AbManifestFile *file = &state->manifest->files[file_index];
      const uint8_t *text;
      size_t glob_index;
      int selected = 0;
      size_t match_count = 0;
      for (glob_index = 0; glob_index < loader->paths.count; glob_index++) {
        if (ab_map_collection_match(&file->path,
                                    &loader->paths.items[glob_index])) {
          selected = 1;
          break;
        }
      }
      if (!selected)
        continue;
      path_matches++;
      text = ab_project_source_bytes(state->project,
                                     (size_t)(file - state->manifest->files));
      status = ab_utf8_validate(state->engine, text, file->byte_length);
      if (status != ARCHBIRD_OK) {
        const char *message = archbird_engine_error(state->engine);
        status = ab_map_add_diagnostic(
            state, "error", "read-failed",
            message && message[0] ? message : "artifact loader is not UTF-8",
            &file->path);
        archbird_error_clear(state->engine);
        continue;
      }
      status = ab_pattern_scan(state->engine, pattern, text, file->byte_length,
                               SIZE_MAX, NULL, NULL, &match_count);
      if (status == ARCHBIRD_OK && loader->required && !match_count) {
        char message[320];
        snprintf(message, sizeof(message), "artifact %.*s: %.*s",
                 (int)config->name.length, config->name.data,
                 (int)loader->pattern.length, loader->pattern.data);
        status = ab_map_add_diagnostic(state, "error",
                                       "artifact-loader-pattern-unmatched",
                                       message, &file->path);
      }
      if (status == ARCHBIRD_OK)
        status = append_loader(state, artifact, &file->path, &loader->pattern,
                               match_count);
    }
    if (status == ARCHBIRD_OK && loader->required && !path_matches) {
      char message[320];
      size_t write = 0;
      int length = snprintf(message, sizeof(message),
                            "artifact %.*s: ", (int)config->name.length,
                            config->name.data);
      if (length < 0)
        status = ARCHBIRD_CONFLICT;
      else
        write = (size_t)length;
      for (file_index = 0;
           status == ARCHBIRD_OK && file_index < loader->paths.count;
           file_index++) {
        length = snprintf(message + write, sizeof(message) - write, "%s%.*s",
                          file_index ? ", " : "",
                          (int)loader->paths.items[file_index].length,
                          loader->paths.items[file_index].data);
        if (length < 0 || (size_t)length >= sizeof(message) - write) {
          status = archbird_error_set(state->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                      ARCHBIRD_NO_OFFSET,
                                      "artifact diagnostic is too large");
          break;
        }
        write += (size_t)length;
      }
      if (status == ARCHBIRD_OK)
        status = ab_map_add_diagnostic(
            state, "error", "artifact-loader-unmatched", message, NULL);
    }
    ab_pattern_free(pattern);
  }
  return status;
}

static const AbMapBuildRoute *find_build(const AbMapState *state,
                                         const AbString *source,
                                         const AbString *target) {
  size_t index;
  for (index = 0; index < state->build_count; index++) {
    if (ab_string_equal(&state->builds[index].source, source) &&
        ab_string_equal(&state->builds[index].name, target))
      return &state->builds[index];
  }
  return NULL;
}

static ArchbirdStatus append_build(AbMapState *state, AbMapArtifact *artifact,
                                   const AbConfigArtifactBuild *config) {
  AbMapArtifactBuild *resized;
  AbMapArtifactBuild *build;
  ArchbirdStatus status;
  if (artifact->build_count == SIZE_MAX / sizeof(*artifact->builds))
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many artifact build routes");
  resized = (AbMapArtifactBuild *)ab_realloc(state->engine, artifact->builds,
                                             (artifact->build_count + 1) *
                                                 sizeof(*artifact->builds));
  if (!resized)
    return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting artifact build routes");
  artifact->builds = resized;
  build = &artifact->builds[artifact->build_count];
  memset(build, 0, sizeof(*build));
  status = ab_string_copy(state->engine, &build->source, config->source.data,
                          config->source.length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(state->engine, &build->target, config->target.data,
                            config->target.length);
  if (status == ARCHBIRD_OK)
    artifact->build_count++;
  return status;
}

static ArchbirdStatus collect_builds(AbMapState *state,
                                     const AbConfigArtifact *config,
                                     AbMapArtifact *artifact) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < config->build_count;
       index++) {
    const AbConfigArtifactBuild *configured = &config->builds[index];
    const AbMapBuildRoute *route =
        find_build(state, &configured->source, &configured->target);
    if (!route) {
      if (config->required) {
        char message[320];
        snprintf(message, sizeof(message), "artifact %.*s: %.*s:%.*s",
                 (int)config->name.length, config->name.data,
                 (int)configured->source.length, configured->source.data,
                 (int)configured->target.length, configured->target.data);
        status =
            ab_map_add_diagnostic(state, "error", "artifact-build-unresolved",
                                  message, &configured->source);
      }
      continue;
    }
    status = append_build(state, artifact, configured);
    if (status == ARCHBIRD_OK) {
      size_t collection;
      for (collection = 0; collection < 2; collection++) {
        const AbStringArray *candidates =
            collection ? &route->paths : &route->deps;
        size_t candidate;
        for (candidate = 0;
             status == ARCHBIRD_OK && candidate < candidates->count;
             candidate++) {
          const AbString *raw = &candidates->items[candidate];
          AbString normalized = *raw;
          const AbManifestFile *file;
          AbMapArtifactInput *input;
          char evidence[384];
          int evidence_length;
          if (normalized.length >= 2 && normalized.data[0] == '.' &&
              normalized.data[1] == '/') {
            normalized.data += 2;
            normalized.length -= 2;
          }
          file = ab_map_manifest_file(state->manifest, normalized.data,
                                      normalized.length);
          if (!file || !file->has_layer)
            continue;
          status = artifact_input(state, artifact, &normalized, &input);
          evidence_length =
              snprintf(evidence, sizeof(evidence), "build:%.*s:%.*s",
                       (int)configured->source.length, configured->source.data,
                       (int)configured->target.length, configured->target.data);
          if (status == ARCHBIRD_OK && evidence_length >= 0 &&
              (size_t)evidence_length < sizeof(evidence))
            status = append_unique(state->engine, &input->evidence, evidence,
                                   (size_t)evidence_length);
        }
      }
    }
  }
  return status;
}

static ArchbirdStatus connect_artifacts(AbMapState *state) {
  size_t consumer;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (consumer = 0; status == ARCHBIRD_OK && consumer < state->artifact_count;
       consumer++) {
    size_t provider;
    for (provider = 0; provider < state->artifact_count; provider++) {
      size_t loader;
      if (provider == consumer)
        continue;
      for (loader = 0; loader < state->artifacts[provider].loader_count;
           loader++) {
        size_t input;
        for (input = 0; input < state->artifacts[consumer].input_count;
             input++) {
          if (ab_string_equal(&state->artifacts[provider].loaders[loader].path,
                              &state->artifacts[consumer].inputs[input].path)) {
            status = append_unique(state->engine,
                                   &state->artifacts[consumer].depends_on,
                                   state->artifacts[provider].name.data,
                                   state->artifacts[provider].name.length);
            break;
          }
        }
        if (status != ARCHBIRD_OK)
          break;
      }
    }
    if (state->artifacts[consumer].depends_on.count > 1)
      qsort(state->artifacts[consumer].depends_on.items,
            state->artifacts[consumer].depends_on.count,
            sizeof(*state->artifacts[consumer].depends_on.items),
            string_compare);
  }
  return status;
}

ArchbirdStatus ab_map_analyze_artifacts(AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  state->artifact_count = state->config->artifact_count;
  if (state->artifact_count) {
    state->artifacts = (AbMapArtifact *)ab_calloc(
        state->engine, state->artifact_count, sizeof(*state->artifacts));
    if (!state->artifacts)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting artifacts");
  }
  for (index = 0; status == ARCHBIRD_OK && index < state->artifact_count;
       index++) {
    const AbConfigArtifact *config = &state->config->artifacts[index];
    AbMapArtifact *artifact = &state->artifacts[index];
    status = ab_string_copy(state->engine, &artifact->name, config->name.data,
                            config->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(state->engine, &artifact->output,
                              config->output.data, config->output.length);
    if (status == ARCHBIRD_OK)
      status = collect_configured_inputs(state, config, artifact);
    if (status == ARCHBIRD_OK)
      status = collect_loaders(state, config, artifact);
    if (status == ARCHBIRD_OK)
      status = collect_builds(state, config, artifact);
    if (status == ARCHBIRD_OK && artifact->input_count > 1)
      qsort(artifact->inputs, artifact->input_count, sizeof(*artifact->inputs),
            input_compare);
    if (status == ARCHBIRD_OK && artifact->loader_count > 1)
      qsort(artifact->loaders, artifact->loader_count,
            sizeof(*artifact->loaders), loader_compare);
    if (status == ARCHBIRD_OK && artifact->build_count > 1)
      qsort(artifact->builds, artifact->build_count, sizeof(*artifact->builds),
            build_compare);
    if (status == ARCHBIRD_OK) {
      size_t input;
      for (input = 0; input < artifact->input_count; input++) {
        if (artifact->inputs[input].evidence.count > 1)
          qsort(artifact->inputs[input].evidence.items,
                artifact->inputs[input].evidence.count,
                sizeof(*artifact->inputs[input].evidence.items),
                string_compare);
      }
    }
  }
  if (status == ARCHBIRD_OK && state->artifact_count > 1)
    qsort(state->artifacts, state->artifact_count, sizeof(*state->artifacts),
          artifact_compare);
  if (status == ARCHBIRD_OK)
    status = connect_artifacts(state);
  return status;
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_strings(AbBuffer *buffer,
                                     const AbStringArray *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &values->items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

ArchbirdStatus ab_map_render_artifacts(AbBuffer *buffer,
                                       const AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < state->artifact_count;
       index++) {
    const AbMapArtifact *artifact = &state->artifacts[index];
    size_t nested;
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"builds\":[");
    for (nested = 0; status == ARCHBIRD_OK && nested < artifact->build_count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"source\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &artifact->builds[nested].source);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"target\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &artifact->builds[nested].target);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"depends_on\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &artifact->depends_on);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"inputs\":[");
    for (nested = 0; status == ARCHBIRD_OK && nested < artifact->input_count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"evidence\":");
      if (status == ARCHBIRD_OK)
        status = render_strings(buffer, &artifact->inputs[nested].evidence);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"path\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &artifact->inputs[nested].path);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"loaded_by\":[");
    for (nested = 0; status == ARCHBIRD_OK && nested < artifact->loader_count;
         nested++) {
      if (nested)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"matches\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(buffer, artifact->loaders[nested].matches);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"path\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &artifact->loaders[nested].path);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"pattern\":");
      if (status == ARCHBIRD_OK)
        status = json_string(buffer, &artifact->loaders[nested].pattern);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"name\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &artifact->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"output\":");
    if (status == ARCHBIRD_OK)
      status = json_string(buffer, &artifact->output);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
