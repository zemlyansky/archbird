#include "archbird_internal.h"

#include "config.h"
#include "evidence.h"
#include "evidence_render.h"
#include "json_value.h"
#include "model.h"
#include "project_internal.h"
#include "render_internal.h"
#include "sha256.h"
#include "test_observations.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbSourceState {
  uint8_t *bytes;
  int supplied;
} AbSourceState;

typedef struct AbFactReference {
  size_t provider_index;
  AbFact *fact;
} AbFactReference;

typedef struct AbMergedFact {
  size_t provider_index;
  AbFact fact;
  const AbFact *value;
  AbFactReference primary_contributor;
  AbFactReference *contributors;
  size_t contributor_count;
} AbMergedFact;

typedef struct AbMergeConflict {
  const char *reason;
  AbSubject *subject;
  AbString *domain;
  size_t left_provider;
  AbFact *left_fact;
  size_t right_provider;
  AbFact *right_fact;
} AbMergeConflict;

typedef struct AbMergeVariation {
  AbSubject *subject;
  AbString *domain;
  size_t canonical_provider;
  AbFact *canonical_fact;
  const AbObjectField *canonical_attribute;
  size_t alternate_provider;
  AbFact *alternate_fact;
  const AbObjectField *alternate_attribute;
} AbMergeVariation;

struct ArchbirdProject {
  ArchbirdEngine *engine;
  AbSourceManifest manifest;
  AbSourceState *sources;
  size_t supplied_count;
  size_t supplied_bytes;
  AbProviderBundle *providers;
  size_t provider_count;
  size_t provider_capacity;
  uint8_t *provider_digest_index;
  uint8_t *provider_digest_occupied;
  size_t provider_digest_capacity;
  size_t provider_fact_count;
  size_t max_provider_bundles;
  size_t max_facts;
  AbMergedFact *merged_facts;
  size_t merged_fact_count;
  const AbFact **merged_facts_by_path;
  AbMergeConflict *merge_conflicts;
  size_t merge_conflict_count;
  size_t merge_conflict_capacity;
  AbMergeVariation *merge_variations;
  size_t merge_variation_count;
  size_t merge_variation_capacity;
  ArchbirdMergeSummary merge_summary;
  int providers_finalized;
  uint8_t manifest_sha256[32];
  char manifest_sha256_hex[65];
  char map_input_sha256_hex[65];
  AbValue *test_observations;
  size_t test_observation_count;
  size_t test_observation_capacity;
  AbMapConfig *config;
};

typedef struct AbDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} AbDigestWriter;

typedef struct AbDomainSelection {
  AbSubject *subject;
  AbString *name;
  size_t primary_count;
  size_t primary_provider;
  int primary_complete;
  size_t augment_count;
  size_t audit_count;
} AbDomainSelection;

static ArchbirdStatus merged_fact_add_contributor(ArchbirdEngine *engine,
                                                  AbMergedFact *merged,
                                                  size_t capacity,
                                                  AbFactReference contributor) {
  if (merged->contributors == &merged->primary_contributor) {
    AbFactReference *contributors =
        (AbFactReference *)ab_calloc(engine, capacity, sizeof(*contributors));
    if (!contributors)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing provider witnesses");
    contributors[0] = merged->primary_contributor;
    merged->contributors = contributors;
  }
  merged->contributors[merged->contributor_count++] = contributor;
  return ARCHBIRD_OK;
}

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  AbDigestWriter *writer = (AbDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

static ArchbirdStatus digest_json(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t json_length, uint8_t digest[32],
                                  char hex[65]) {
  AbDigestWriter writer;
  ArchbirdStatus status;
  archbird_sha256_init(&writer.context);
  writer.status = ARCHBIRD_OK;
  status = archbird_json_canonicalize(engine, json, json_length, 0,
                                      digest_write, &writer);
  if (status != ARCHBIRD_OK)
    return status;
  if (writer.status != ARCHBIRD_OK)
    return writer.status;
  archbird_sha256_final(&writer.context, digest);
  archbird_sha256_hex(digest, hex);
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_input_digest(ArchbirdEngine *engine,
                                       const AbSourceManifest *manifest,
                                       char out[65]) {
  ArchbirdSha256Context context;
  size_t index;
  uint8_t digest[32];
  ArchbirdStatus status;
  archbird_sha256_init(&context);
  for (index = 0; index < manifest->file_count; index++) {
    char content[65];
    archbird_sha256_hex(manifest->files[index].sha256, content);
    status = archbird_sha256_update(
        &context, (const uint8_t *)manifest->files[index].path.data,
        manifest->files[index].path.length);
    if (status == ARCHBIRD_OK)
      status = archbird_sha256_update(&context, (const uint8_t *)"\0", 1);
    if (status == ARCHBIRD_OK)
      status = archbird_sha256_update(&context, (const uint8_t *)content, 64);
    if (status == ARCHBIRD_OK)
      status = archbird_sha256_update(&context, (const uint8_t *)"\0", 1);
    if (status != ARCHBIRD_OK)
      return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                                "failed to hash map inputs");
  }
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, out);
  return ARCHBIRD_OK;
}

static int string_equals_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (length == 0 || memcmp(value->data, literal, length) == 0);
}

static uint64_t provider_digest_hash(const uint8_t digest[32]) {
  uint64_t hash = UINT64_C(14695981039346656037);
  size_t index;
  for (index = 0; index < 32; index++) {
    hash ^= digest[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static size_t provider_digest_slot(const uint8_t *digests,
                                   const uint8_t *occupied, size_t capacity,
                                   const uint8_t digest[32]) {
  size_t slot = (size_t)provider_digest_hash(digest) & (capacity - 1);
  while (occupied[slot] && memcmp(digests + slot * 32, digest, 32) != 0)
    slot = (slot + 1) & (capacity - 1);
  return slot;
}

static int provider_digest_contains(const ArchbirdProject *project,
                                    const uint8_t digest[32]) {
  size_t slot;
  if (!project->provider_digest_capacity)
    return 0;
  slot = provider_digest_slot(project->provider_digest_index,
                              project->provider_digest_occupied,
                              project->provider_digest_capacity, digest);
  return project->provider_digest_occupied[slot] != 0;
}

static void provider_digest_insert(uint8_t *digests, uint8_t *occupied,
                                   size_t capacity, const uint8_t digest[32]) {
  size_t slot = provider_digest_slot(digests, occupied, capacity, digest);
  memcpy(digests + slot * 32, digest, 32);
  occupied[slot] = 1;
}

static ArchbirdStatus reserve_provider_digests(ArchbirdEngine *engine,
                                               ArchbirdProject *project,
                                               size_t required) {
  uint8_t *digests;
  uint8_t *occupied;
  size_t capacity =
      project->provider_digest_capacity ? project->provider_digest_capacity : 8;
  size_t index;
  if (!required)
    return ARCHBIRD_OK;
  if (required <= capacity / 2 && project->provider_digest_capacity)
    return ARCHBIRD_OK;
  if (required > SIZE_MAX / 2)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "provider digest capacity overflow");
  while (capacity / 2 < required) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "provider digest capacity overflow");
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / 32)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "provider digest capacity overflow");
  digests = (uint8_t *)ab_calloc(engine, capacity, 32);
  if (!digests)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing provider digests");
  occupied = (uint8_t *)ab_calloc(engine, capacity, 1);
  if (!occupied) {
    ab_free(engine, digests);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing provider digests");
  }
  for (index = 0; index < project->provider_count; index++)
    provider_digest_insert(digests, occupied, capacity,
                           project->providers[index].sha256);
  ab_free(engine, project->provider_digest_index);
  ab_free(engine, project->provider_digest_occupied);
  project->provider_digest_index = digests;
  project->provider_digest_occupied = occupied;
  project->provider_digest_capacity = capacity;
  return ARCHBIRD_OK;
}

static int digest_compare(const void *left, const void *right) {
  return memcmp(left, right, 32);
}

static AbManifestFile *find_manifest_file(ArchbirdProject *project,
                                          const char *path, size_t path_length,
                                          size_t *out_index) {
  AbString wanted = {(char *)path, path_length};
  size_t low = 0;
  size_t high = project->manifest.file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&project->manifest.files[middle].path, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      if (out_index)
        *out_index = middle;
      return &project->manifest.files[middle];
    }
  }
  return NULL;
}

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
  status = ab_decode_source_manifest(engine, manifest_json, manifest_length,
                                     &project->manifest);
  if (status != ARCHBIRD_OK) {
    archbird_project_destroy(project);
    return status;
  }
  if (project->manifest.file_count) {
    project->sources = (AbSourceState *)ab_calloc(
        engine, project->manifest.file_count, sizeof(*project->sources));
    if (!project->sources) {
      archbird_project_destroy(project);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing source project");
    }
  }
  project->max_provider_bundles = engine->options.max_provider_bundles;
  project->max_facts = engine->options.max_facts;
  status = digest_json(engine, manifest_json, manifest_length,
                       project->manifest_sha256, project->manifest_sha256_hex);
  if (status == ARCHBIRD_OK)
    status = map_input_digest(engine, &project->manifest,
                              project->map_input_sha256_hex);
  if (status != ARCHBIRD_OK) {
    archbird_project_destroy(project);
    return status;
  }
  *out_project = project;
  return ARCHBIRD_OK;
}

void archbird_project_destroy(ArchbirdProject *project) {
  ArchbirdEngine *engine;
  size_t index;
  if (!project)
    return;
  engine = project->engine;
  for (index = 0; index < project->manifest.file_count; index++)
    ab_free(engine, project->sources ? project->sources[index].bytes : NULL);
  ab_free(engine, project->sources);
  if (project->merged_facts)
    for (index = 0; index < project->merged_fact_count; index++) {
      if (project->merged_facts[index].value ==
          &project->merged_facts[index].fact)
        ab_fact_free(engine, &project->merged_facts[index].fact);
      if (project->merged_facts[index].contributors !=
          &project->merged_facts[index].primary_contributor)
        ab_free(engine, project->merged_facts[index].contributors);
    }
  ab_free(engine, project->merged_facts);
  ab_free(engine, project->merged_facts_by_path);
  for (index = 0; index < project->provider_count; index++)
    ab_provider_bundle_free(engine, &project->providers[index]);
  ab_free(engine, project->providers);
  ab_free(engine, project->provider_digest_index);
  ab_free(engine, project->provider_digest_occupied);
  ab_free(engine, project->merge_conflicts);
  ab_free(engine, project->merge_variations);
  for (index = 0; index < project->test_observation_count; index++)
    ab_value_free(engine, &project->test_observations[index]);
  ab_free(engine, project->test_observations);
  if (project->config) {
    ab_map_config_free(engine, project->config);
    ab_free(engine, project->config);
  }
  ab_source_manifest_free(engine, &project->manifest);
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
      !ab_string_equal(&config->project, &project->manifest.project))
    status = archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "configuration project does not match source manifest");
  if (status == ARCHBIRD_OK) {
    size_t index;
    for (index = 0; index < project->manifest.file_count; index++) {
      const AbManifestFile *file = &project->manifest.files[index];
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
  file = find_manifest_file(project, path, path_length, &index);
  if (!file)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source path is absent from the manifest");
  source = &project->sources[index];
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
  project->supplied_count++;
  project->supplied_bytes += byte_length;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_finalize_sources(ArchbirdEngine *engine,
                                                 ArchbirdProject *project) {
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->supplied_count != project->manifest.file_count)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "source project is missing %zu manifest file(s)",
                              project->manifest.file_count -
                                  project->supplied_count);
  return ARCHBIRD_OK;
}

size_t archbird_project_source_count(const ArchbirdProject *project) {
  return project ? project->manifest.file_count : 0;
}

ArchbirdStatus archbird_project_source(const ArchbirdProject *project,
                                       size_t index,
                                       ArchbirdSourceView *out_source) {
  const AbManifestFile *file;
  const AbSourceState *source;
  if (!project || !out_source ||
      out_source->struct_size != sizeof(*out_source) ||
      index >= project->manifest.file_count)
    return ARCHBIRD_INVALID_ARGUMENT;
  file = &project->manifest.files[index];
  source = &project->sources[index];
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
  return project ? project->manifest_sha256_hex : NULL;
}

static ArchbirdStatus
validate_fact_binding(ArchbirdEngine *engine, ArchbirdProject *project,
                      const AbString *fact_project, const AbString *path,
                      size_t span_start, size_t span_end, const char *context) {
  AbManifestFile *file;
  if (!ab_string_equal(fact_project, &project->manifest.project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s belongs to another project", context);
  file = find_manifest_file(project, path->data, path->length, NULL);
  if (!file)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s path is absent from the manifest", context);
  if (span_start > span_end || span_end > file->byte_length)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "%s span %zu..%zu is outside manifest path %.*s (%zu bytes)", context,
        span_start, span_end, (int)file->path.length, file->path.data,
        file->byte_length);
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_provider_binding(ArchbirdEngine *engine,
                                                ArchbirdProject *project,
                                                AbProviderBundle *bundle) {
  size_t index;
  if (!bundle->input_count)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "project ingestion requires source inputs");
  for (index = 0; index < bundle->input_count; index++) {
    AbProviderInput *input = &bundle->inputs[index];
    if (!ab_string_equal(&input->project, &project->manifest.project))
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "provider input project does not match project");
    if (input->has_source_manifest_sha256) {
      if (memcmp(input->source_manifest_sha256, project->manifest_sha256, 32) !=
          0)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "provider source manifest is stale or unrelated");
    } else {
      AbManifestFile *file = find_manifest_file(project, input->path.data,
                                                input->path.length, NULL);
      if (!file)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "provider source input path is absent from the manifest");
      if (memcmp(input->source_sha256, file->sha256, 32) != 0)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "provider source input is stale or unrelated");
    }
  }
  if (string_equals_literal(&bundle->subject.scope, "workspace"))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "workspace provider bundles require a workspace ingestion API");
  if (!bundle->subject.has_project ||
      !ab_string_equal(&bundle->subject.project, &project->manifest.project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider subject does not match project");
  for (index = 0; index < bundle->fact_count; index++) {
    AbFact *fact = &bundle->facts[index];
    ArchbirdStatus status = validate_fact_binding(
        engine, project, &fact->project, &fact->path, fact->span_start,
        fact->span_end, "provider fact");
    if (status != ARCHBIRD_OK)
      return status;
  }
  for (index = 0; index < bundle->diagnostic_count; index++) {
    AbDiagnostic *diagnostic = &bundle->diagnostics[index];
    if (diagnostic->has_path) {
      ArchbirdStatus status = validate_fact_binding(
          engine, project, &diagnostic->project, &diagnostic->path,
          diagnostic->has_span ? diagnostic->span_start : 0,
          diagnostic->has_span ? diagnostic->span_end : 0,
          "provider diagnostic");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_add_provider_facts(ArchbirdEngine *engine,
                                                   ArchbirdProject *project,
                                                   ArchbirdProviderMode mode,
                                                   const uint8_t *provider_json,
                                                   size_t provider_length) {
  AbProviderBundle bundle;
  ArchbirdStatus status;
  memset(&bundle, 0, sizeof(bundle));
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_decode_provider_bundle(engine, provider_json, provider_length,
                                     &bundle);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_project_take_provider_bundle(engine, project, mode, &bundle);
  if (status != ARCHBIRD_OK)
    ab_provider_bundle_free(engine, &bundle);
  return status;
}

ArchbirdStatus archbird_project_add_test_symbol_observations(
    ArchbirdEngine *engine, ArchbirdProject *project,
    const uint8_t *observations_json, size_t observations_length) {
  AbValue document = {0};
  AbValue *resized;
  ArchbirdStatus status;
  if (!engine || !project || (!observations_json && observations_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_decode_test_symbol_observations(
      engine, project, observations_json, observations_length, &document);
  if (status != ARCHBIRD_OK)
    return status;
  if (project->test_observation_count == project->test_observation_capacity) {
    size_t next = project->test_observation_capacity
                      ? project->test_observation_capacity * 2
                      : 4;
    if (next > engine->options.max_values)
      next = engine->options.max_values;
    if (next <= project->test_observation_capacity) {
      ab_value_free(engine, &document);
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "test observation artifact limit exceeded");
    }
    resized = (AbValue *)ab_realloc(engine, project->test_observations,
                                    next * sizeof(*resized));
    if (!resized) {
      ab_value_free(engine, &document);
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing test-symbol observations");
    }
    project->test_observations = resized;
    project->test_observation_capacity = next;
  }
  project->test_observations[project->test_observation_count++] = document;
  return ARCHBIRD_OK;
}

const AbSourceManifest *ab_project_manifest(const ArchbirdProject *project) {
  return project ? &project->manifest : NULL;
}

const AbMapConfig *ab_project_config(const ArchbirdProject *project) {
  return project ? project->config : NULL;
}

const uint8_t *ab_project_source_bytes(const ArchbirdProject *project,
                                       size_t index) {
  if (!project || index >= project->manifest.file_count ||
      !project->sources[index].supplied)
    return NULL;
  return project->sources[index].bytes;
}

const uint8_t *
ab_project_manifest_sha256_bytes(const ArchbirdProject *project) {
  return project ? project->manifest_sha256 : NULL;
}

const char *archbird_project_map_input_sha256(const ArchbirdProject *project) {
  return project ? project->map_input_sha256_hex : NULL;
}

size_t ab_project_test_observation_count(const ArchbirdProject *project) {
  return project ? project->test_observation_count : 0;
}

const AbValue *ab_project_test_observation(const ArchbirdProject *project,
                                           size_t index) {
  return project && index < project->test_observation_count
             ? &project->test_observations[index]
             : NULL;
}

ArchbirdStatus ab_project_take_provider_bundle(ArchbirdEngine *engine,
                                               ArchbirdProject *project,
                                               ArchbirdProviderMode mode,
                                               AbProviderBundle *bundle) {
  return ab_project_take_provider_bundles(engine, project, mode, bundle, 1);
}

ArchbirdStatus ab_project_take_provider_bundles(ArchbirdEngine *engine,
                                                ArchbirdProject *project,
                                                ArchbirdProviderMode mode,
                                                AbProviderBundle *bundles,
                                                size_t bundle_count) {
  AbProviderBundle *resized;
  uint8_t *batch_digests = NULL;
  size_t added_facts = 0;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !project || (!bundles && bundle_count) ||
      (mode != ARCHBIRD_PROVIDER_PRIMARY && mode != ARCHBIRD_PROVIDER_AUGMENT &&
       mode != ARCHBIRD_PROVIDER_AUDIT))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->providers_finalized)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge is already finalized");
  if (project->supplied_count != project->manifest.file_count)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "source project must be complete before provider ingestion");
  if (bundle_count > project->max_provider_bundles - project->provider_count)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider bundle limit exceeded");
  for (index = 0; index < bundle_count; index++) {
    AbProviderBundle *bundle = &bundles[index];
    status = validate_provider_binding(engine, project, bundle);
    if (status != ARCHBIRD_OK)
      return status;
    if (bundle->fact_count > project->max_facts - added_facts ||
        added_facts + bundle->fact_count >
            project->max_facts - project->provider_fact_count)
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "provider fact limit exceeded");
    added_facts += bundle->fact_count;
    status = ab_provider_bundle_digest(engine, bundle);
    if (status != ARCHBIRD_OK)
      return status;
    if (provider_digest_contains(project, bundle->sha256))
      return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                "provider bundle was supplied more than once");
  }
  if (bundle_count > 1) {
    if (bundle_count > SIZE_MAX / 32)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "provider batch digest capacity overflow");
    batch_digests = (uint8_t *)ab_malloc(engine, bundle_count * 32);
    if (!batch_digests)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory validating provider batch");
    for (index = 0; index < bundle_count; index++)
      memcpy(batch_digests + index * 32, bundles[index].sha256, 32);
    qsort(batch_digests, bundle_count, 32, digest_compare);
    for (index = 1; index < bundle_count; index++) {
      if (memcmp(batch_digests + (index - 1) * 32, batch_digests + index * 32,
                 32) == 0) {
        ab_free(engine, batch_digests);
        return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                  "provider batch contains a duplicate bundle");
      }
    }
  }
  if (bundle_count > project->provider_capacity - project->provider_count) {
    size_t required = project->provider_count + bundle_count;
    size_t capacity =
        project->provider_capacity ? project->provider_capacity : 4;
    while (capacity < required) {
      if (capacity > project->max_provider_bundles / 2) {
        capacity = project->max_provider_bundles;
        break;
      }
      capacity *= 2;
    }
    resized = (AbProviderBundle *)ab_realloc(
        engine, project->providers, capacity * sizeof(*project->providers));
    if (!resized)
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory storing provider bundle");
    if (!resized) {
      ab_free(engine, batch_digests);
      return status;
    }
    project->providers = resized;
    project->provider_capacity = capacity;
  }
  status = reserve_provider_digests(engine, project,
                                    project->provider_count + bundle_count);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, batch_digests);
    return status;
  }
  for (index = 0; index < bundle_count; index++) {
    provider_digest_insert(
        project->provider_digest_index, project->provider_digest_occupied,
        project->provider_digest_capacity, bundles[index].sha256);
    bundles[index].mode = mode;
    project->providers[project->provider_count++] = bundles[index];
    memset(&bundles[index], 0, sizeof(bundles[index]));
  }
  ab_free(engine, batch_digests);
  project->provider_fact_count += added_facts;
  return ARCHBIRD_OK;
}

int ab_project_providers_finalized(const ArchbirdProject *project) {
  return project ? project->providers_finalized : 0;
}

size_t ab_project_merged_fact_count(const ArchbirdProject *project) {
  return project ? project->merged_fact_count : 0;
}

const AbFact *ab_project_merged_fact(const ArchbirdProject *project,
                                     size_t index) {
  if (!project || index >= project->merged_fact_count)
    return NULL;
  return project->merged_facts[index].value;
}

const AbProviderBundle *
ab_project_merged_fact_provider(const ArchbirdProject *project, size_t index) {
  size_t provider_index;
  if (!project || index >= project->merged_fact_count)
    return NULL;
  provider_index = project->merged_facts[index].provider_index;
  return provider_index < project->provider_count
             ? &project->providers[provider_index]
             : NULL;
}

static int fact_path_index_compare(const void *left_raw,
                                   const void *right_raw) {
  const AbFact *left = *(const AbFact *const *)left_raw;
  const AbFact *right = *(const AbFact *const *)right_raw;
  int compared = ab_string_compare(&left->path, &right->path);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->domain, &right->domain);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left->kind, &right->kind);
  if (compared != 0)
    return compared;
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  return ab_string_compare(&left->id, &right->id);
}

static int fact_path_domain_compare(const AbFact *fact, const AbString *path,
                                    const char *domain, size_t domain_length) {
  AbString wanted_domain = {(char *)domain, domain_length};
  int compared = ab_string_compare(&fact->path, path);
  if (compared != 0)
    return compared;
  return ab_string_compare(&fact->domain, &wanted_domain);
}

void ab_project_merged_fact_range(const ArchbirdProject *project,
                                  const AbString *path, const char *domain,
                                  size_t *out_start, size_t *out_end) {
  size_t domain_length = strlen(domain);
  size_t low = 0;
  size_t high = project ? project->merged_fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_compare(project->merged_facts_by_path[middle], path,
                                 domain, domain_length) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = project ? project->merged_fact_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (fact_path_domain_compare(project->merged_facts_by_path[middle], path,
                                 domain, domain_length) <= 0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

const AbFact *ab_project_merged_fact_by_path(const ArchbirdProject *project,
                                             size_t index) {
  if (!project || index >= project->merged_fact_count)
    return NULL;
  return project->merged_facts_by_path[index];
}

size_t archbird_project_provider_count(const ArchbirdProject *project) {
  return project ? project->provider_count : 0;
}

const AbProviderBundle *
ab_project_provider_bundle(const ArchbirdProject *project, size_t index) {
  if (!project || index >= project->provider_count)
    return NULL;
  return &project->providers[index];
}

size_t archbird_project_provider_fact_count(const ArchbirdProject *project) {
  return project ? project->provider_fact_count : 0;
}

ArchbirdStatus archbird_project_render_provider_facts(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    size_t provider_index, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || !project || provider_index >= project->provider_count ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_provider_bundle_render_compact(
      engine, &project->providers[provider_index], &buffer);
  if (status != ARCHBIRD_OK)
    return status;
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  return status;
}

static int provider_compare(const void *left_raw, const void *right_raw) {
  const AbProviderBundle *left = (const AbProviderBundle *)left_raw;
  const AbProviderBundle *right = (const AbProviderBundle *)right_raw;
  return memcmp(left->sha256, right->sha256, 32);
}

static int domain_compare(const void *left_raw, const void *right_raw) {
  const AbDomainSelection *left = (const AbDomainSelection *)left_raw;
  const AbDomainSelection *right = (const AbDomainSelection *)right_raw;
  const AbString *left_fields[] = {
      &left->subject->scope,
      &left->subject->project,
      &left->subject->path,
      &left->subject->name,
  };
  const AbString *right_fields[] = {
      &right->subject->scope,
      &right->subject->project,
      &right->subject->path,
      &right->subject->name,
  };
  size_t index;
  for (index = 0; index < sizeof(left_fields) / sizeof(left_fields[0]);
       index++) {
    int compared = ab_string_compare(left_fields[index], right_fields[index]);
    if (compared != 0)
      return compared;
  }
  return ab_string_compare(left->name, right->name);
}

static int subject_equal(const AbSubject *left, const AbSubject *right) {
  return ab_string_equal(&left->scope, &right->scope) &&
         left->has_project == right->has_project &&
         (!left->has_project ||
          ab_string_equal(&left->project, &right->project)) &&
         left->has_path == right->has_path &&
         (!left->has_path || ab_string_equal(&left->path, &right->path)) &&
         left->has_name == right->has_name &&
         (!left->has_name || ab_string_equal(&left->name, &right->name));
}

static AbDomainSelection *find_domain(AbDomainSelection *domains,
                                      size_t domain_count,
                                      const AbSubject *subject,
                                      const AbString *name) {
  size_t low = 0;
  size_t high = domain_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    AbDomainSelection wanted;
    int compared;
    memset(&wanted, 0, sizeof(wanted));
    wanted.subject = (AbSubject *)subject;
    wanted.name = (AbString *)name;
    compared = domain_compare(&domains[middle], &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &domains[middle];
  }
  return NULL;
}

static int fact_correlation_compare(const AbFact *left, const AbFact *right) {
  const AbString *left_fields[] = {&left->project, &left->path, &left->domain,
                                   &left->kind};
  const AbString *right_fields[] = {&right->project, &right->path,
                                    &right->domain, &right->kind};
  size_t index;
  for (index = 0; index < sizeof(left_fields) / sizeof(left_fields[0]);
       index++) {
    int compared = ab_string_compare(left_fields[index], right_fields[index]);
    if (compared != 0)
      return compared;
  }
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  /* Providers must opt into span correlation. This keeps occurrence-shaped
     evidence composable while preventing compact relations and synthetic
     summaries that share a representative span from collapsing. */
  if (left->correlate_by_span != right->correlate_by_span)
    return left->correlate_by_span ? 1 : -1;
  if (left->correlate_by_span && left->span_start < left->span_end)
    return 0;
  return ab_string_compare(&left->key, &right->key);
}

static int fact_claim_rank(const AbString *claim) {
  static const char semantic[] = "semantic-";
  if (claim->length >= sizeof(semantic) - 1 &&
      memcmp(claim->data, semantic, sizeof(semantic) - 1) == 0)
    return 3;
  if (string_equals_literal(claim, "syntax-structure"))
    return 2;
  if (string_equals_literal(claim, "lexical-occurrence"))
    return 1;
  return 0;
}

static int fact_witness_compare(const AbFactReference *left,
                                const AbFactReference *right,
                                const AbProviderBundle *providers) {
  const AbProviderBundle *left_provider = &providers[left->provider_index];
  const AbProviderBundle *right_provider = &providers[right->provider_index];
  int left_rank = fact_claim_rank(&left->fact->claim);
  int right_rank = fact_claim_rank(&right->fact->claim);
  int compared;
  if (left_rank != right_rank)
    return left_rank > right_rank ? -1 : 1;
  compared = ab_string_compare(&left->fact->claim, &right->fact->claim);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left_provider->producer.name,
                               &right_provider->producer.name);
  if (compared != 0)
    return compared;
  compared = ab_string_compare(&left_provider->producer.version,
                               &right_provider->producer.version);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->producer.configuration_sha256,
                    right_provider->producer.configuration_sha256, 32);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->producer.implementation_sha256,
                    right_provider->producer.implementation_sha256, 32);
  if (compared != 0)
    return compared;
  compared = memcmp(left_provider->sha256, right_provider->sha256, 32);
  if (compared != 0)
    return compared;
  return ab_string_compare(&left->fact->id, &right->fact->id);
}

static int fact_reference_compare(const AbFactReference *left,
                                  const AbFactReference *right,
                                  AbProviderBundle *providers) {
  int compared = domain_compare(
      &(AbDomainSelection){.subject = &providers[left->provider_index].subject,
                           .name = &left->fact->domain},
      &(AbDomainSelection){.subject = &providers[right->provider_index].subject,
                           .name = &right->fact->domain});
  ArchbirdProviderMode left_mode;
  ArchbirdProviderMode right_mode;
  if (compared != 0)
    return compared;
  compared = fact_correlation_compare(left->fact, right->fact);
  if (compared != 0)
    return compared;
  left_mode = providers[left->provider_index].mode;
  right_mode = providers[right->provider_index].mode;
  if (left_mode != right_mode)
    return (left_mode > right_mode) - (left_mode < right_mode);
  return fact_witness_compare(left, right, providers);
}

static ArchbirdStatus sort_fact_references(ArchbirdEngine *engine,
                                           ArchbirdProject *project,
                                           AbFactReference *facts,
                                           size_t count) {
  AbFactReference *scratch;
  AbFactReference *source = facts;
  AbFactReference *target;
  size_t width;
  int swapped = 0;
  if (count < 2)
    return ARCHBIRD_OK;
  scratch = (AbFactReference *)ab_malloc(engine, count * sizeof(*scratch));
  if (!scratch)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory sorting provider facts");
  target = scratch;
  for (width = 1; width < count;) {
    size_t start;
    for (start = 0; start < count; start += width * 2) {
      size_t left = start;
      size_t left_end = start + width < count ? start + width : count;
      size_t right = left_end;
      size_t right_end = right + width < count ? right + width : count;
      size_t output = start;
      while (left < left_end && right < right_end) {
        if (fact_reference_compare(&source[left], &source[right],
                                   project->providers) <= 0)
          target[output++] = source[left++];
        else
          target[output++] = source[right++];
      }
      while (left < left_end)
        target[output++] = source[left++];
      while (right < right_end)
        target[output++] = source[right++];
    }
    {
      AbFactReference *temporary = source;
      source = target;
      target = temporary;
    }
    swapped = !swapped;
    if (width > count / 2)
      break;
    width *= 2;
  }
  if (swapped)
    memcpy(facts, source, count * sizeof(*facts));
  ab_free(engine, scratch);
  return ARCHBIRD_OK;
}

static int string_arrays_equal(const AbStringArray *left,
                               const AbStringArray *right) {
  size_t index;
  if (left->count != right->count)
    return 0;
  for (index = 0; index < left->count; index++) {
    if (!ab_string_equal(&left->items[index], &right->items[index]))
      return 0;
  }
  return 1;
}

static int attributes_relation(const AbFact *left, const AbFact *right,
                               int *enriched, int *varied) {
  size_t left_index = 0;
  size_t right_index = 0;
  while (left_index < left->attribute_count &&
         right_index < right->attribute_count) {
    const AbObjectField *left_field = &left->attributes[left_index];
    const AbObjectField *right_field = &right->attributes[right_index];
    int compared = ab_string_compare(&left_field->name, &right_field->name);
    if (compared < 0) {
      *enriched = 1;
      left_index++;
    } else if (compared > 0) {
      *enriched = 1;
      right_index++;
    } else {
      if (!ab_value_equal(&left_field->value, &right_field->value)) {
        if (!ab_fact_attribute_is_presentation(&left_field->name))
          return 0;
        *varied = 1;
      }
      left_index++;
      right_index++;
    }
  }
  if (left_index < left->attribute_count ||
      right_index < right->attribute_count)
    *enriched = 1;
  return 1;
}

static int fact_relation(const AbFact *left, const AbFact *right, int *varied) {
  int enriched = 0;
  *varied = 0;
  if (!ab_fact_names_compatible(left, right))
    return -1;
  if (left->has_name != right->has_name ||
      (left->has_name && !ab_string_equal(&left->name, &right->name)))
    enriched = 1;
  if (!attributes_relation(left, right, &enriched, varied))
    return -1;
  if (left->has_resolution && right->has_resolution) {
    if (!ab_string_equal(&left->resolution.state, &right->resolution.state) ||
        !string_arrays_equal(&left->resolution.targets,
                             &right->resolution.targets))
      return -1;
    if (left->resolution.has_reason && right->resolution.has_reason &&
        !ab_string_equal(&left->resolution.reason, &right->resolution.reason))
      return -1;
    if (left->resolution.has_reason != right->resolution.has_reason)
      enriched = 1;
  } else if (left->has_resolution != right->has_resolution) {
    enriched = 1;
  }
  if (!ab_string_equal(&left->claim, &right->claim))
    enriched = 1;
  return enriched;
}

static ArchbirdStatus collect_domains(ArchbirdEngine *engine,
                                      ArchbirdProject *project,
                                      AbDomainSelection **out_domains,
                                      size_t *out_count) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  size_t capacity = 0;
  size_t provider_index;
  *out_domains = NULL;
  *out_count = 0;
  for (provider_index = 0; provider_index < project->provider_count;
       provider_index++) {
    AbProviderBundle *provider = &project->providers[provider_index];
    size_t capability_index;
    for (capability_index = 0; capability_index < provider->capability_count;
         capability_index++) {
      AbDomainSelection *domain;
      if (domain_count == capacity) {
        size_t next = capacity ? capacity * 2 : 8;
        AbDomainSelection *resized;
        if (next < capacity || next > SIZE_MAX / sizeof(*domains)) {
          ab_free(engine, domains);
          return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider domain limit exceeded");
        }
        resized = (AbDomainSelection *)ab_realloc(engine, domains,
                                                  next * sizeof(*domains));
        if (!resized) {
          ab_free(engine, domains);
          return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory selecting domains");
        }
        domains = resized;
        capacity = next;
      }
      domain = &domains[domain_count++];
      memset(domain, 0, sizeof(*domain));
      domain->subject = &provider->subject;
      domain->name = &provider->capabilities[capability_index].domain;
      domain->primary_provider = SIZE_MAX;
      if (provider->mode == ARCHBIRD_PROVIDER_PRIMARY) {
        domain->primary_count++;
        domain->primary_provider = provider_index;
        domain->primary_complete = string_equals_literal(
            &provider->capabilities[capability_index].coverage, "complete");
      } else if (provider->mode == ARCHBIRD_PROVIDER_AUGMENT) {
        domain->augment_count++;
      } else {
        domain->audit_count++;
      }
    }
  }
  if (domain_count > 1) {
    size_t read_index;
    size_t write_index = 0;
    qsort(domains, domain_count, sizeof(*domains), domain_compare);
    for (read_index = 0; read_index < domain_count; read_index++) {
      AbDomainSelection *current = &domains[read_index];
      if (write_index > 0 &&
          domain_compare(&domains[write_index - 1], current) == 0) {
        AbDomainSelection *selected = &domains[write_index - 1];
        selected->primary_count += current->primary_count;
        selected->augment_count += current->augment_count;
        selected->audit_count += current->audit_count;
        if (current->primary_count) {
          selected->primary_provider = current->primary_provider;
          selected->primary_complete = current->primary_complete;
        }
      } else {
        if (write_index != read_index)
          domains[write_index] = *current;
        write_index++;
      }
    }
    domain_count = write_index;
  }
  if (domain_count > project->max_facts) {
    ab_free(engine, domains);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider domain limit exceeded");
  }
  *out_domains = domains;
  *out_count = domain_count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus collect_fact_references(ArchbirdEngine *engine,
                                              ArchbirdProject *project,
                                              AbFactReference **out_facts) {
  AbFactReference *facts;
  size_t count = 0;
  size_t provider_index;
  *out_facts = NULL;
  if (!project->provider_fact_count)
    return ARCHBIRD_OK;
  facts = (AbFactReference *)ab_malloc(engine, project->provider_fact_count *
                                                   sizeof(*facts));
  if (!facts)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing provider facts");
  for (provider_index = 0; provider_index < project->provider_count;
       provider_index++) {
    size_t fact_index;
    for (fact_index = 0;
         fact_index < project->providers[provider_index].fact_count;
         fact_index++) {
      facts[count].provider_index = provider_index;
      facts[count].fact = &project->providers[provider_index].facts[fact_index];
      count++;
    }
  }
  {
    ArchbirdStatus status = sort_fact_references(engine, project, facts, count);
    if (status != ARCHBIRD_OK) {
      ab_free(engine, facts);
      return status;
    }
  }
  *out_facts = facts;
  return ARCHBIRD_OK;
}

static void record_conflict(ArchbirdProject *project, const char *reason,
                            AbSubject *subject, AbString *domain,
                            size_t left_provider, AbFact *left_fact,
                            size_t right_provider, AbFact *right_fact) {
  AbMergeConflict *conflict;
  if (project->merge_conflict_count >= project->merge_conflict_capacity)
    return;
  conflict = &project->merge_conflicts[project->merge_conflict_count++];
  conflict->reason = reason;
  conflict->subject = subject;
  conflict->domain = domain;
  conflict->left_provider = left_provider;
  conflict->left_fact = left_fact;
  conflict->right_provider = right_provider;
  conflict->right_fact = right_fact;
}

static ArchbirdStatus
record_variations(ArchbirdEngine *engine, ArchbirdProject *project,
                  AbSubject *subject, AbString *domain,
                  size_t canonical_provider, AbFact *canonical_fact,
                  size_t alternate_provider, AbFact *alternate_fact) {
  size_t canonical_index = 0;
  size_t alternate_index = 0;
  while (canonical_index < canonical_fact->attribute_count &&
         alternate_index < alternate_fact->attribute_count) {
    const AbObjectField *canonical =
        &canonical_fact->attributes[canonical_index];
    const AbObjectField *alternate =
        &alternate_fact->attributes[alternate_index];
    int compared = ab_string_compare(&canonical->name, &alternate->name);
    if (compared < 0) {
      canonical_index++;
    } else if (compared > 0) {
      alternate_index++;
    } else {
      if (ab_fact_attribute_is_presentation(&canonical->name) &&
          !ab_value_equal(&canonical->value, &alternate->value)) {
        AbMergeVariation *variation;
        if (project->merge_variation_count >= project->merge_variation_capacity)
          return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider variation ledger is too large");
        variation =
            &project->merge_variations[project->merge_variation_count++];
        variation->subject = subject;
        variation->domain = domain;
        variation->canonical_provider = canonical_provider;
        variation->canonical_fact = canonical_fact;
        variation->canonical_attribute = canonical;
        variation->alternate_provider = alternate_provider;
        variation->alternate_fact = alternate_fact;
        variation->alternate_attribute = alternate;
      }
      canonical_index++;
      alternate_index++;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_finalize_providers(ArchbirdEngine *engine,
                                                   ArchbirdProject *project) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  AbFactReference *facts = NULL;
  size_t cursor = 0;
  size_t domain_index;
  ArchbirdStatus status;
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (project->providers_finalized) {
    return project->merge_summary.conflicts
               ? archbird_error_set(engine, ARCHBIRD_CONFLICT,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider merge contains %zu conflict(s)",
                                    project->merge_summary.conflicts)
               : ARCHBIRD_OK;
  }
  if (project->supplied_count != project->manifest.file_count)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "source project must be complete before provider merge");
  if (project->provider_count > 1)
    qsort(project->providers, project->provider_count,
          sizeof(*project->providers), provider_compare);
  status = collect_domains(engine, project, &domains, &domain_count);
  if (status == ARCHBIRD_OK)
    status = collect_fact_references(engine, project, &facts);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, domains);
    ab_free(engine, facts);
    return status;
  }
  memset(&project->merge_summary, 0, sizeof(project->merge_summary));
  project->merge_summary.struct_size = sizeof(project->merge_summary);
  project->merge_summary.providers = project->provider_count;
  project->merge_summary.selections = domain_count;
  if (domain_count > SIZE_MAX - project->provider_fact_count) {
    ab_free(engine, domains);
    ab_free(engine, facts);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider conflict ledger is too large");
  }
  project->merge_conflict_capacity =
      domain_count + project->provider_fact_count;
  if (project->merge_conflict_capacity) {
    project->merge_conflicts =
        (AbMergeConflict *)ab_calloc(engine, project->merge_conflict_capacity,
                                     sizeof(*project->merge_conflicts));
    if (!project->merge_conflicts) {
      ab_free(engine, domains);
      ab_free(engine, facts);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing provider conflicts");
    }
  }
  project->merge_variation_capacity = project->provider_fact_count;
  if (project->merge_variation_capacity) {
    project->merge_variations =
        (AbMergeVariation *)ab_calloc(engine, project->merge_variation_capacity,
                                      sizeof(*project->merge_variations));
    if (!project->merge_variations) {
      ab_free(engine, domains);
      ab_free(engine, facts);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing provider variations");
    }
  }
  for (domain_index = 0; domain_index < domain_count; domain_index++) {
    if (domains[domain_index].primary_count > 1)
      record_conflict(project, "primary-cardinality",
                      domains[domain_index].subject, domains[domain_index].name,
                      SIZE_MAX, NULL, SIZE_MAX, NULL);
  }
  if (project->provider_fact_count) {
    project->merged_facts = (AbMergedFact *)ab_calloc(
        engine, project->provider_fact_count, sizeof(*project->merged_facts));
    if (!project->merged_facts) {
      ab_free(engine, domains);
      ab_free(engine, facts);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing merged facts");
    }
  }
  while (cursor < project->provider_fact_count) {
    size_t end = cursor + 1;
    size_t selected = SIZE_MAX;
    size_t index;
    AbProviderBundle *group_provider =
        &project->providers[facts[cursor].provider_index];
    AbDomainSelection *domain =
        find_domain(domains, domain_count, &group_provider->subject,
                    &facts[cursor].fact->domain);
    while (
        end < project->provider_fact_count &&
        subject_equal(&group_provider->subject,
                      &project->providers[facts[end].provider_index].subject) &&
        fact_correlation_compare(facts[cursor].fact, facts[end].fact) == 0)
      end++;
    if (!domain || domain->primary_count > 1) {
      cursor = end;
      continue;
    }
    for (index = cursor; index < end; index++) {
      if (project->providers[facts[index].provider_index].mode ==
          ARCHBIRD_PROVIDER_PRIMARY) {
        if (selected == SIZE_MAX)
          selected = index;
        else
          record_conflict(project, "duplicate-primary-occurrence",
                          domain->subject, domain->name,
                          facts[selected].provider_index, facts[selected].fact,
                          facts[index].provider_index, facts[index].fact);
      }
    }
    /* A complete primary capability owns its closed inventory. Bounded and
       partial primaries do not prove absence, so independently anchored
       augment facts remain part of the common view. */
    if (selected == SIZE_MAX &&
        (domain->primary_count == 0 || !domain->primary_complete)) {
      for (index = cursor; index < end; index++) {
        if (project->providers[facts[index].provider_index].mode ==
            ARCHBIRD_PROVIDER_AUGMENT) {
          selected = index;
          break;
        }
      }
    }
    if (selected != SIZE_MAX) {
      AbMergedFact *merged =
          &project->merged_facts[project->merged_fact_count++];
      merged->provider_index = facts[selected].provider_index;
      merged->value = facts[selected].fact;
      if (end - cursor > SIZE_MAX / sizeof(*merged->contributors)) {
        status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "provider witness set is too large");
        goto merge_failed;
      }
      merged->primary_contributor = facts[selected];
      merged->contributors = &merged->primary_contributor;
      merged->contributor_count = 1;
      project->merge_summary.contributed++;
      for (index = cursor; index < end; index++) {
        ArchbirdProviderMode mode =
            project->providers[facts[index].provider_index].mode;
        int relation;
        int varied;
        if (index == selected || mode == ARCHBIRD_PROVIDER_PRIMARY)
          continue;
        relation =
            fact_relation(facts[selected].fact, facts[index].fact, &varied);
        if (varied) {
          status = record_variations(
              engine, project, domain->subject, domain->name,
              facts[selected].provider_index, facts[selected].fact,
              facts[index].provider_index, facts[index].fact);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
        }
        if (mode == ARCHBIRD_PROVIDER_AUDIT) {
          if (relation < 0)
            project->merge_summary.audit_differences++;
          else
            project->merge_summary.audit_matches++;
        } else if (relation < 0) {
          record_conflict(project, "augment-mismatch", domain->subject,
                          domain->name, facts[selected].provider_index,
                          facts[selected].fact, facts[index].provider_index,
                          facts[index].fact);
        } else if (relation > 0) {
          if (merged->value != &merged->fact) {
            status = ab_fact_copy(engine, &merged->fact, merged->value);
            if (status != ARCHBIRD_OK)
              goto merge_failed;
            merged->value = &merged->fact;
          }
          status = ab_fact_merge_compatible(engine, &merged->fact,
                                            facts[index].fact);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          status = merged_fact_add_contributor(engine, merged, end - cursor,
                                               facts[index]);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          project->merge_summary.enriched++;
        } else {
          status = merged_fact_add_contributor(engine, merged, end - cursor,
                                               facts[index]);
          if (status != ARCHBIRD_OK)
            goto merge_failed;
          project->merge_summary.deduplicated++;
        }
      }
    } else {
      for (index = cursor; index < end; index++) {
        if (project->providers[facts[index].provider_index].mode ==
            ARCHBIRD_PROVIDER_AUDIT)
          project->merge_summary.audit_differences++;
      }
    }
    cursor = end;
  }
  project->merge_summary.selected_facts = project->merged_fact_count;
  project->merge_summary.conflicts = project->merge_conflict_count;
  project->merge_summary.variations = project->merge_variation_count;
  if (project->merged_fact_count) {
    size_t fact_index;
    project->merged_facts_by_path = (const AbFact **)ab_malloc(
        engine,
        project->merged_fact_count * sizeof(*project->merged_facts_by_path));
    if (!project->merged_facts_by_path) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory indexing merged facts by path");
      goto merge_failed;
    }
    for (fact_index = 0; fact_index < project->merged_fact_count; fact_index++)
      project->merged_facts_by_path[fact_index] =
          project->merged_facts[fact_index].value;
    if (project->merged_fact_count > 1)
      qsort(project->merged_facts_by_path, project->merged_fact_count,
            sizeof(*project->merged_facts_by_path), fact_path_index_compare);
  }
  project->providers_finalized = 1;
  ab_free(engine, domains);
  ab_free(engine, facts);
  if (project->merge_summary.conflicts)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge contains %zu conflict(s)",
                              project->merge_summary.conflicts);
  return ARCHBIRD_OK;

merge_failed:
  if (project->merged_facts)
    for (domain_index = 0; domain_index < project->merged_fact_count;
         domain_index++) {
      if (project->merged_facts[domain_index].value ==
          &project->merged_facts[domain_index].fact)
        ab_fact_free(engine, &project->merged_facts[domain_index].fact);
      if (project->merged_facts[domain_index].contributors !=
          &project->merged_facts[domain_index].primary_contributor)
        ab_free(engine, project->merged_facts[domain_index].contributors);
    }
  ab_free(engine, project->merged_facts);
  ab_free(engine, project->merged_facts_by_path);
  ab_free(engine, project->merge_conflicts);
  ab_free(engine, project->merge_variations);
  project->merged_facts = NULL;
  project->merged_facts_by_path = NULL;
  project->merge_conflicts = NULL;
  project->merge_variations = NULL;
  project->merged_fact_count = 0;
  project->merge_conflict_count = 0;
  project->merge_conflict_capacity = 0;
  project->merge_variation_count = 0;
  project->merge_variation_capacity = 0;
  memset(&project->merge_summary, 0, sizeof(project->merge_summary));
  ab_free(engine, domains);
  ab_free(engine, facts);
  return status;
}

ArchbirdStatus
archbird_project_merge_summary(const ArchbirdProject *project,
                               ArchbirdMergeSummary *out_summary) {
  if (!project || !out_summary ||
      out_summary->struct_size != sizeof(*out_summary))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!project->providers_finalized)
    return ARCHBIRD_CONFLICT;
  *out_summary = project->merge_summary;
  return ARCHBIRD_OK;
}

static int provider_has_selection(const AbProviderBundle *provider,
                                  const AbSubject *subject,
                                  const AbString *domain) {
  size_t index;
  if (!subject_equal(&provider->subject, subject))
    return 0;
  for (index = 0; index < provider->capability_count; index++) {
    int compared =
        ab_string_compare(&provider->capabilities[index].domain, domain);
    if (compared == 0)
      return 1;
    if (compared > 0)
      return 0;
  }
  return 0;
}

static const char *provider_mode_name(ArchbirdProviderMode mode) {
  if (mode == ARCHBIRD_PROVIDER_PRIMARY)
    return "primary";
  if (mode == ARCHBIRD_PROVIDER_AUGMENT)
    return "augment";
  return "audit";
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus json_sha(AbBuffer *buffer, const char *hex) {
  return ab_buffer_json_string(buffer, hex, 64);
}

static ArchbirdStatus render_subject(AbBuffer *buffer,
                                     const AbSubject *subject) {
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (subject->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"name\":"));
    RENDER_TRY(json_string(buffer, &subject->name));
    if (subject->has_project)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  if (subject->has_path) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"path\":"));
    RENDER_TRY(json_string(buffer, &subject->path));
    if (subject->has_project)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  if (subject->has_project) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"project\":"));
    RENDER_TRY(json_string(buffer, &subject->project));
    RENDER_TRY(ab_buffer_literal(buffer, ","));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"scope\":"));
  RENDER_TRY(json_string(buffer, &subject->scope));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_provider(AbBuffer *buffer,
                                      const AbProviderBundle *provider) {
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"domains\":["));
  for (index = 0; index < provider->capability_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_string(buffer, &provider->capabilities[index].domain));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"facts\":"));
  RENDER_TRY(ab_buffer_u64(buffer, provider->fact_count));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"mode\":"));
  RENDER_TRY(ab_buffer_json_string(buffer, provider_mode_name(provider->mode),
                                   strlen(provider_mode_name(provider->mode))));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"producer\":{"));
  RENDER_TRY(ab_buffer_literal(buffer, "\"configuration_sha256\":"));
  {
    char hex[65];
    archbird_sha256_hex(provider->producer.configuration_sha256, hex);
    RENDER_TRY(json_sha(buffer, hex));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"implementation_sha256\":"));
  {
    char hex[65];
    archbird_sha256_hex(provider->producer.implementation_sha256, hex);
    RENDER_TRY(json_sha(buffer, hex));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
  RENDER_TRY(json_string(buffer, &provider->producer.name));
  if (provider->producer.has_runtime) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"runtime\":"));
    RENDER_TRY(json_string(buffer, &provider->producer.runtime));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"version\":"));
  RENDER_TRY(json_string(buffer, &provider->producer.version));
  RENDER_TRY(ab_buffer_literal(buffer, "},\"sha256\":"));
  RENDER_TRY(json_sha(buffer, provider->sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, &provider->subject));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_domain(AbBuffer *buffer,
                                    const ArchbirdProject *project,
                                    const AbDomainSelection *domain) {
  size_t index;
  int first;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"audits\":["));
  first = 1;
  for (index = 0; index < project->provider_count; index++) {
    const AbProviderBundle *provider = &project->providers[index];
    if (provider->mode != ARCHBIRD_PROVIDER_AUDIT ||
        !provider_has_selection(provider, domain->subject, domain->name))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_sha(buffer, provider->sha256_hex));
    first = 0;
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"augments\":["));
  first = 1;
  for (index = 0; index < project->provider_count; index++) {
    const AbProviderBundle *provider = &project->providers[index];
    if (provider->mode != ARCHBIRD_PROVIDER_AUGMENT ||
        !provider_has_selection(provider, domain->subject, domain->name))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(json_sha(buffer, provider->sha256_hex));
    first = 0;
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"domain\":"));
  RENDER_TRY(json_string(buffer, domain->name));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"primary\":"));
  if (domain->primary_count == 1)
    RENDER_TRY(json_sha(
        buffer, project->providers[domain->primary_provider].sha256_hex));
  else
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"primary_count\":"));
  RENDER_TRY(ab_buffer_u64(buffer, domain->primary_count));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, domain->subject));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_conflict_fact(AbBuffer *buffer,
                                           const AbFact *fact) {
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  if (!fact)
    return ab_buffer_literal(buffer, "null");
  RENDER_TRY(ab_buffer_literal(buffer, "{"));
  if (fact->attribute_count) {
    RENDER_TRY(ab_buffer_literal(buffer, "\"attributes\":{"));
    for (index = 0; index < fact->attribute_count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(json_string(buffer, &fact->attributes[index].name));
      RENDER_TRY(ab_buffer_literal(buffer, ":"));
      RENDER_TRY(ab_value_render(buffer, &fact->attributes[index].value));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "},"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "\"claim\":"));
  RENDER_TRY(json_string(buffer, &fact->claim));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"correlation\":\""));
  RENDER_TRY(
      ab_buffer_literal(buffer, fact->correlate_by_span ? "span" : "key"));
  RENDER_TRY(ab_buffer_literal(buffer, "\",\"id\":"));
  RENDER_TRY(json_string(buffer, &fact->id));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &fact->kind));
  if (fact->has_name) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    RENDER_TRY(json_string(buffer, &fact->name));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &fact->path));
  if (fact->has_resolution) {
    RENDER_TRY(ab_buffer_literal(buffer, ",\"resolution\":{"));
    if (fact->resolution.has_reason) {
      RENDER_TRY(ab_buffer_literal(buffer, "\"reason\":"));
      RENDER_TRY(json_string(buffer, &fact->resolution.reason));
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "\"state\":"));
    RENDER_TRY(json_string(buffer, &fact->resolution.state));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"targets\":["));
    for (index = 0; index < fact->resolution.targets.count; index++) {
      if (index)
        RENDER_TRY(ab_buffer_literal(buffer, ","));
      RENDER_TRY(json_string(buffer, &fact->resolution.targets.items[index]));
    }
    RENDER_TRY(ab_buffer_literal(buffer, "]}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, ",\"span\":{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_conflict(AbBuffer *buffer,
                                      const ArchbirdProject *project,
                                      const AbMergeConflict *conflict) {
  AbFact *witness =
      conflict->left_fact ? conflict->left_fact : conflict->right_fact;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"domain\":"));
  RENDER_TRY(json_string(buffer, conflict->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"left_provider\":"));
  if (conflict->left_provider == SIZE_MAX)
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  else
    RENDER_TRY(json_sha(
        buffer, project->providers[conflict->left_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"left_fact\":"));
  RENDER_TRY(render_conflict_fact(buffer, conflict->left_fact));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"reason\":"));
  RENDER_TRY(ab_buffer_json_string(buffer, conflict->reason,
                                   strlen(conflict->reason)));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"right_provider\":"));
  if (conflict->right_provider == SIZE_MAX)
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  else
    RENDER_TRY(json_sha(
        buffer, project->providers[conflict->right_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"right_fact\":"));
  RENDER_TRY(render_conflict_fact(buffer, conflict->right_fact));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, conflict->subject));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"witness\":"));
  if (!witness) {
    RENDER_TRY(ab_buffer_literal(buffer, "null"));
  } else {
    RENDER_TRY(ab_buffer_literal(buffer, "{\"end\":"));
    RENDER_TRY(ab_buffer_u64(buffer, witness->span_end));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
    RENDER_TRY(json_string(buffer, &witness->key));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    RENDER_TRY(json_string(buffer, &witness->kind));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    RENDER_TRY(json_string(buffer, &witness->path));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
    RENDER_TRY(ab_buffer_u64(buffer, witness->span_start));
    RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_occurrence(AbBuffer *buffer,
                                        const ArchbirdProject *project,
                                        const AbMergedFact *merged) {
  const AbFact *fact = merged->value;
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"canonical\":{"));
  RENDER_TRY(ab_buffer_literal(buffer, "\"claim\":"));
  RENDER_TRY(json_string(buffer, &fact->claim));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"fact_id\":"));
  RENDER_TRY(json_string(buffer, &fact->id));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"provider\":"));
  RENDER_TRY(
      json_sha(buffer, project->providers[merged->provider_index].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, "},\"contributors\":["));
  for (index = 0; index < merged->contributor_count; index++) {
    const AbFactReference *contributor = &merged->contributors[index];
    if (index)
      RENDER_TRY(ab_buffer_literal(buffer, ","));
    RENDER_TRY(ab_buffer_literal(buffer, "{\"claim\":"));
    RENDER_TRY(json_string(buffer, &contributor->fact->claim));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"fact_id\":"));
    RENDER_TRY(json_string(buffer, &contributor->fact->id));
    RENDER_TRY(ab_buffer_literal(buffer, ",\"provider\":"));
    RENDER_TRY(json_sha(
        buffer, project->providers[contributor->provider_index].sha256_hex));
    RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  RENDER_TRY(ab_buffer_literal(buffer, "],\"domain\":"));
  RENDER_TRY(json_string(buffer, &fact->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &fact->kind));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &fact->path));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_variation(AbBuffer *buffer,
                                       const ArchbirdProject *project,
                                       const AbMergeVariation *variation) {
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      return status;                                                           \
  } while (0)
  RENDER_TRY(ab_buffer_literal(buffer, "{\"alternate_provider\":"));
  RENDER_TRY(json_sha(
      buffer, project->providers[variation->alternate_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"alternate_value\":"));
  RENDER_TRY(ab_value_render(buffer, &variation->alternate_attribute->value));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"attribute\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_attribute->name));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"canonical_provider\":"));
  RENDER_TRY(json_sha(
      buffer, project->providers[variation->canonical_provider].sha256_hex));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"canonical_value\":"));
  RENDER_TRY(ab_value_render(buffer, &variation->canonical_attribute->value));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"domain\":"));
  RENDER_TRY(json_string(buffer, variation->domain));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"subject\":"));
  RENDER_TRY(render_subject(buffer, variation->subject));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"witness\":{\"end\":"));
  RENDER_TRY(ab_buffer_u64(buffer, variation->canonical_fact->span_end));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->key));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->kind));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  RENDER_TRY(json_string(buffer, &variation->canonical_fact->path));
  RENDER_TRY(ab_buffer_literal(buffer, ",\"start\":"));
  RENDER_TRY(ab_buffer_u64(buffer, variation->canonical_fact->span_start));
  RENDER_TRY(ab_buffer_literal(buffer, "}}"));
#undef RENDER_TRY
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_project_render_merge_ledger(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbDomainSelection *domains = NULL;
  size_t domain_count = 0;
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)
  if (!engine || !project || !write_fn || !project->providers_finalized ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = collect_domains(engine, (ArchbirdProject *)project, &domains,
                           &domain_count);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&buffer, engine);
  RENDER_TRY(ab_buffer_literal(
      &buffer, "{\"artifact\":\"archbird-provider-merge\",\"conflicts\":["));
  for (index = 0; index < project->merge_conflict_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_conflict(&buffer, project, &project->merge_conflicts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"occurrences\":["));
  for (index = 0; index < project->merged_fact_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_occurrence(&buffer, project, &project->merged_facts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"selections\":["));
  for (index = 0; index < domain_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_domain(&buffer, project, &domains[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"project\":"));
  RENDER_TRY(json_string(&buffer, &project->manifest.project));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":["));
  for (index = 0; index < project->provider_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_provider(&buffer, &project->providers[index]));
  }
  RENDER_TRY(ab_buffer_literal(
      &buffer, "],\"schema_version\":5,\"source_manifest_sha256\":"));
  RENDER_TRY(json_sha(&buffer, project->manifest_sha256_hex));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"summary\":{"));
  RENDER_TRY(ab_buffer_literal(&buffer, "\"audit_differences\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.audit_differences));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"audit_matches\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.audit_matches));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.conflicts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"contributed\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.contributed));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"deduplicated\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.deduplicated));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"selections\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.selections));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"enriched\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.enriched));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.providers));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"selected_facts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.selected_facts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"variations\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.variations));
  RENDER_TRY(ab_buffer_literal(&buffer, "},\"variations\":["));
  for (index = 0; index < project->merge_variation_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_variation(&buffer, project, &project->merge_variations[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "]}"));
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
done:
  ab_buffer_free(&buffer);
  ab_free(engine, domains);
#undef RENDER_TRY
  return status;
}

static int provider_has_conflict(const ArchbirdProject *project,
                                 size_t provider_index) {
  size_t index;
  for (index = 0; index < project->merge_conflict_count; index++) {
    const AbMergeConflict *conflict = &project->merge_conflicts[index];
    if (conflict->left_provider == provider_index ||
        conflict->right_provider == provider_index)
      return 1;
    if ((conflict->left_provider == SIZE_MAX ||
         conflict->right_provider == SIZE_MAX) &&
        project->providers[provider_index].mode == ARCHBIRD_PROVIDER_PRIMARY &&
        provider_has_selection(&project->providers[provider_index],
                               conflict->subject, conflict->domain))
      return 1;
  }
  return 0;
}

ArchbirdStatus archbird_project_render_merge_conflicts(
    ArchbirdEngine *engine, const ArchbirdProject *project, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbBuffer buffer;
  size_t index;
  size_t provider_count = 0;
  int first;
  ArchbirdStatus status;
#define RENDER_TRY(expression)                                                 \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto done;                                                               \
  } while (0)
  if (!engine || !project || !write_fn || !project->providers_finalized ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  RENDER_TRY(ab_buffer_literal(
      &buffer,
      "{\"artifact\":\"archbird-provider-merge-conflicts\",\"conflicts\":["));
  for (index = 0; index < project->merge_conflict_count; index++) {
    if (index)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(
        render_conflict(&buffer, project, &project->merge_conflicts[index]));
  }
  RENDER_TRY(ab_buffer_literal(&buffer, "],\"project\":"));
  RENDER_TRY(json_string(&buffer, &project->manifest.project));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers\":["));
  first = 1;
  for (index = 0; index < project->provider_count; index++) {
    if (!provider_has_conflict(project, index))
      continue;
    if (!first)
      RENDER_TRY(ab_buffer_literal(&buffer, ","));
    RENDER_TRY(render_provider(&buffer, &project->providers[index]));
    first = 0;
    provider_count++;
  }
  RENDER_TRY(ab_buffer_literal(
      &buffer, "],\"schema_version\":1,\"source_manifest_sha256\":"));
  RENDER_TRY(json_sha(&buffer, project->manifest_sha256_hex));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"summary\":{\"conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.conflicts));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers_in_conflicts\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, provider_count));
  RENDER_TRY(ab_buffer_literal(&buffer, ",\"providers_total\":"));
  RENDER_TRY(ab_buffer_u64(&buffer, project->merge_summary.providers));
  RENDER_TRY(ab_buffer_literal(&buffer, "}}"));
  status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                      json_flags, write_fn, user_data);
done:
  ab_buffer_free(&buffer);
#undef RENDER_TRY
  return status;
}
