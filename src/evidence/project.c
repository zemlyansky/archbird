#include "base/archbird_internal.h"

#include "base/sha256.h"
#include "evidence/config.h"
#include "evidence/project/observation_store.h"
#include "evidence/project/project_model.h"
#include "evidence/project/provider_merge.h"
#include "evidence/project/provider_store.h"
#include "evidence/project/source_store.h"
#include "evidence/project_internal.h"

#include <string.h>

ArchbirdStatus archbird_project_create(ArchbirdEngine *engine,
                                       const uint8_t *manifest_json,
                                       size_t manifest_length,
                                       ArchbirdProject **out_project) {
  ArchbirdProject *project;
  ArchbirdStatus status;
  if (!engine || !out_project)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_project = NULL;
  project = (ArchbirdProject *)ab_calloc(engine, 1, sizeof(*project));
  if (!project)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating source project");
  project->engine = engine;
  project->providers.max_bundles = engine->options.max_provider_bundles;
  project->providers.max_facts = engine->options.max_facts;
  status = ab_project_source_store_init(engine, manifest_json, manifest_length,
                                        &project->sources);
  if (status != ARCHBIRD_OK) {
    archbird_project_destroy(project);
    return status;
  }
  *out_project = project;
  return ARCHBIRD_OK;
}

void archbird_project_destroy(ArchbirdProject *project) {
  ArchbirdEngine *engine;
  if (!project)
    return;
  engine = project->engine;
  ab_project_merge_result_destroy(engine, &project->merge);
  ab_project_provider_store_destroy(engine, &project->providers);
  ab_project_observation_store_destroy(engine, &project->observations);
  if (project->config) {
    ab_map_config_free(engine, project->config);
    ab_free(engine, project->config);
  }
  ab_project_source_store_destroy(engine, &project->sources);
  ab_free(engine, project);
}

ArchbirdStatus archbird_project_set_config(ArchbirdEngine *engine,
                                           ArchbirdProject *project,
                                           const uint8_t *config_json,
                                           size_t config_length) {
  AbMapConfig *config;
  ArchbirdStatus status;
  if (!engine || !project || (!config_json && config_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->config)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "project configuration was already supplied");
  config = (AbMapConfig *)ab_calloc(engine, 1, sizeof(*config));
  if (!config)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing project configuration");
  status = ab_decode_map_config(engine, config_json, config_length, config);
  if (status == ARCHBIRD_OK &&
      !ab_string_equal(&config->project, &project->sources.manifest.project))
    status = archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "configuration project does not match source manifest");
  if (status == ARCHBIRD_OK) {
    size_t index;
    for (index = 0; index < project->sources.manifest.file_count; index++) {
      const AbManifestFile *file = &project->sources.manifest.files[index];
      const AbConfigLayer *layer;
      if (!file->has_layer)
        continue;
      layer = ab_map_config_layer(config, &file->layer);
      if (!layer) {
        status = archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "manifest file names a layer absent from configuration");
        break;
      }
      if (file->has_language &&
          !ab_string_equal(&file->language, &layer->language)) {
        status = archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "manifest file language disagrees with configured layer");
        break;
      }
    }
  }
  if (status != ARCHBIRD_OK) {
    ab_map_config_free(engine, config);
    ab_free(engine, config);
    return status;
  }
  project->config = config;
  return ARCHBIRD_OK;
}

const char *archbird_project_config_sha256(const ArchbirdProject *project) {
  return project && project->config ? project->config->sha256_hex : NULL;
}

ArchbirdStatus archbird_project_add_source(ArchbirdEngine *engine,
                                           ArchbirdProject *project,
                                           const char *path, size_t path_length,
                                           const uint8_t *bytes,
                                           size_t byte_length) {
  AbManifestFile *file;
  AbSourceState *source;
  size_t index;
  uint8_t digest[32];
  uint8_t *copy = NULL;
  ArchbirdStatus status;
  if (!engine || !project || !path || path_length == 0 ||
      (!bytes && byte_length != 0))
    return ARCHBIRD_INVALID_ARGUMENT;
  file = ab_project_source_store_find(&project->sources, path, path_length,
                                      &index);
  if (!file)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source path is absent from the manifest");
  source = &project->sources.entries[index];
  if (source->supplied)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source path was supplied more than once");
  if (byte_length != file->byte_length)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source byte length does not match manifest");
  status = archbird_sha256(bytes, byte_length, digest);
  if (status != ARCHBIRD_OK)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "failed to hash source bytes");
  if (memcmp(digest, file->sha256, sizeof(digest)) != 0)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source SHA-256 does not match manifest");
  if (byte_length) {
    copy = (uint8_t *)ab_malloc(engine, byte_length);
    if (!copy)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory copying source bytes");
    memcpy(copy, bytes, byte_length);
  }
  source->bytes = copy;
  source->supplied = 1;
  project->sources.supplied_count++;
  project->sources.supplied_bytes += byte_length;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_finalize_sources(ArchbirdEngine *engine,
                                                 ArchbirdProject *project) {
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->sources.supplied_count != project->sources.manifest.file_count)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source project is missing %zu manifest file(s)",
                              project->sources.manifest.file_count -
                                  project->sources.supplied_count);
  return ARCHBIRD_OK;
}

size_t archbird_project_source_count(const ArchbirdProject *project) {
  return project ? project->sources.manifest.file_count : 0;
}

ArchbirdStatus archbird_project_source(const ArchbirdProject *project,
                                       size_t index,
                                       ArchbirdSourceView *out_source) {
  const AbManifestFile *file;
  const AbSourceState *source;
  if (!project || !out_source ||
      out_source->struct_size != sizeof(*out_source) ||
      index >= project->sources.manifest.file_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  file = &project->sources.manifest.files[index];
  source = &project->sources.entries[index];
  out_source->path = file->path.data;
  out_source->path_length = file->path.length;
  out_source->bytes = source->bytes;
  out_source->byte_length = file->byte_length;
  out_source->language = file->has_language ? file->language.data : NULL;
  out_source->language_length = file->has_language ? file->language.length : 0;
  out_source->layer = file->has_layer ? file->layer.data : NULL;
  out_source->layer_length = file->has_layer ? file->layer.length : 0;
  return source->supplied ? ARCHBIRD_OK : ARCHBIRD_CONFLICT;
}

const char *archbird_project_manifest_sha256(const ArchbirdProject *project) {
  return project ? project->sources.manifest_sha256_hex : NULL;
}

ArchbirdStatus archbird_project_add_test_symbol_observations(
    ArchbirdEngine *engine, ArchbirdProject *project,
    const uint8_t *observations_json, size_t observations_length) {
  if (!engine || !project || (!observations_json && observations_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  return ab_project_observation_store_add(
      engine, project, &project->observations, observations_json,
      observations_length);
}

const AbSourceManifest *ab_project_manifest(const ArchbirdProject *project) {
  return project ? &project->sources.manifest : NULL;
}

const AbMapConfig *ab_project_config(const ArchbirdProject *project) {
  return project ? project->config : NULL;
}

const uint8_t *ab_project_source_bytes(const ArchbirdProject *project,
                                       size_t index) {
  if (!project || index >= project->sources.manifest.file_count ||
      !project->sources.entries[index].supplied)
    return NULL;
  return project->sources.entries[index].bytes;
}

const uint8_t *
ab_project_manifest_sha256_bytes(const ArchbirdProject *project) {
  return project ? project->sources.manifest_sha256 : NULL;
}

const char *archbird_project_map_input_sha256(const ArchbirdProject *project) {
  return project ? project->sources.map_input_sha256_hex : NULL;
}

size_t ab_project_test_observation_count(const ArchbirdProject *project) {
  return project ? project->observations.count : 0;
}

const AbValue *ab_project_test_observation(const ArchbirdProject *project,
                                           size_t index) {
  return project && index < project->observations.count
             ? &project->observations.documents[index]
             : NULL;
}
