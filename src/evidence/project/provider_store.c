#include "evidence/project/provider_store.h"

#include "evidence/evidence_render.h"
#include "evidence/project/source_store.h"
#include "evidence/project_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
  if (!project->providers.digest_capacity)
    return 0;
  slot = provider_digest_slot(project->providers.digest_index,
                              project->providers.digest_occupied,
                              project->providers.digest_capacity, digest);
  return project->providers.digest_occupied[slot] != 0;
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
  size_t capacity = project->providers.digest_capacity
                        ? project->providers.digest_capacity
                        : 8;
  size_t index;
  if (!required)
    return ARCHBIRD_OK;
  if (required <= capacity / 2 && project->providers.digest_capacity)
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
  for (index = 0; index < project->providers.count; index++)
    provider_digest_insert(digests, occupied, capacity,
                           project->providers.bundles[index].sha256);
  ab_free(engine, project->providers.digest_index);
  ab_free(engine, project->providers.digest_occupied);
  project->providers.digest_index = digests;
  project->providers.digest_occupied = occupied;
  project->providers.digest_capacity = capacity;
  return ARCHBIRD_OK;
}

static int digest_compare(const void *left, const void *right) {
  return memcmp(left, right, 32);
}

static ArchbirdStatus
validate_fact_binding(ArchbirdEngine *engine, ArchbirdProject *project,
                      const AbString *fact_project, const AbString *path,
                      size_t span_start, size_t span_end, const char *context) {
  AbManifestFile *file;
  if (!ab_string_equal(fact_project, &project->sources.manifest.project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "%s belongs to another project", context);
  file = ab_project_source_store_find(&project->sources, path->data,
                                      path->length, NULL);
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
    if (!ab_string_equal(&input->project, &project->sources.manifest.project))
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "provider input project does not match project");
    if (input->has_source_manifest_sha256) {
      if (memcmp(input->source_manifest_sha256,
                 project->sources.manifest_sha256, 32) != 0)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "provider source manifest is stale or unrelated");
    } else {
      AbManifestFile *file = ab_project_source_store_find(
          &project->sources, input->path.data, input->path.length, NULL);
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
  if (ab_project_string_equals_literal(&bundle->subject.scope, "workspace"))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "workspace provider bundles require a workspace ingestion API");
  if (!bundle->subject.has_project ||
      !ab_string_equal(&bundle->subject.project,
                       &project->sources.manifest.project))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider subject does not match project");
  for (index = 0; index < bundle->fact_count; index++) {
    AbFact *fact = &bundle->facts[index];
    uint64_t extent_start = 0;
    uint64_t extent_end = 0;
    int extent_state;
    ArchbirdStatus status = validate_fact_binding(
        engine, project, &fact->project, &fact->path, fact->span_start,
        fact->span_end, "provider fact");
    if (status != ARCHBIRD_OK)
      return status;
    extent_state = ab_fact_declaration_extent_rank(fact);
    if (extent_state < 0)
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "provider %.*s has an invalid declaration extent for %.*s at "
          "anchor %zu..%zu",
          (int)bundle->producer.name.length, bundle->producer.name.data,
          (int)fact->path.length, fact->path.data, fact->span_start,
          fact->span_end);
    if (extent_state > 0) {
      AbManifestFile *file = ab_project_source_store_find(
          &project->sources, fact->path.data, fact->path.length, NULL);
      if (ab_fact_declaration_extent(fact, &extent_start, &extent_end) != 1 ||
          !file || extent_end > (uint64_t)file->byte_length)
        return archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "provider %.*s declaration extent %" PRIu64 "..%" PRIu64
            " is outside %.*s (%zu bytes)",
            (int)bundle->producer.name.length, bundle->producer.name.data,
            extent_start, extent_end, (int)fact->path.length, fact->path.data,
            file ? file->byte_length : (size_t)0);
    }
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
  if (project->merge.finalized)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "provider merge is already finalized");
  if (project->sources.supplied_count != project->sources.manifest.file_count)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "source project must be complete before provider ingestion");
  if (bundle_count > project->providers.max_bundles - project->providers.count)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provider bundle limit exceeded");
  for (index = 0; index < bundle_count; index++) {
    AbProviderBundle *bundle = &bundles[index];
    status = validate_provider_binding(engine, project, bundle);
    if (status != ARCHBIRD_OK)
      return status;
    if (bundle->fact_count > project->providers.max_facts - added_facts ||
        added_facts + bundle->fact_count >
            project->providers.max_facts - project->providers.fact_count)
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
  if (bundle_count > project->providers.capacity - project->providers.count) {
    size_t required = project->providers.count + bundle_count;
    size_t capacity =
        project->providers.capacity ? project->providers.capacity : 4;
    while (capacity < required) {
      if (capacity > project->providers.max_bundles / 2) {
        capacity = project->providers.max_bundles;
        break;
      }
      capacity *= 2;
    }
    resized = (AbProviderBundle *)ab_realloc(
        engine, project->providers.bundles,
        capacity * sizeof(*project->providers.bundles));
    if (!resized)
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory storing provider bundle");
    if (!resized) {
      ab_free(engine, batch_digests);
      return status;
    }
    project->providers.bundles = resized;
    project->providers.capacity = capacity;
  }
  status = reserve_provider_digests(engine, project,
                                    project->providers.count + bundle_count);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, batch_digests);
    return status;
  }
  for (index = 0; index < bundle_count; index++) {
    provider_digest_insert(
        project->providers.digest_index, project->providers.digest_occupied,
        project->providers.digest_capacity, bundles[index].sha256);
    bundles[index].mode = mode;
    project->providers.bundles[project->providers.count++] = bundles[index];
    memset(&bundles[index], 0, sizeof(bundles[index]));
  }
  ab_free(engine, batch_digests);
  project->providers.fact_count += added_facts;
  return ARCHBIRD_OK;
}

size_t archbird_project_provider_count(const ArchbirdProject *project) {
  return project ? project->providers.count : 0;
}

const AbProviderBundle *
ab_project_provider_bundle(const ArchbirdProject *project, size_t index) {
  if (!project || index >= project->providers.count)
    return NULL;
  return &project->providers.bundles[index];
}

size_t archbird_project_provider_fact_count(const ArchbirdProject *project) {
  return project ? project->providers.fact_count : 0;
}

ArchbirdStatus archbird_project_render_provider_facts(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    size_t provider_index, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || !project || provider_index >= project->providers.count ||
      !write_fn || !ab_json_flags_valid(json_flags))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_provider_bundle_render_compact(
      engine, &project->providers.bundles[provider_index], &buffer);
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

void ab_project_provider_store_destroy(ArchbirdEngine *engine,
                                       AbProjectProviderStore *store) {
  size_t index;
  if (!store)
    return;
  for (index = 0; index < store->count; index++)
    ab_provider_bundle_free(engine, &store->bundles[index]);
  ab_free(engine, store->bundles);
  ab_free(engine, store->digest_index);
  ab_free(engine, store->digest_occupied);
  memset(store, 0, sizeof(*store));
}

void ab_project_provider_store_canonicalize(AbProjectProviderStore *store) {
  if (store && store->count > 1)
    qsort(store->bundles, store->count, sizeof(*store->bundles),
          provider_compare);
}
