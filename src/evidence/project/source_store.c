#include "evidence/project/source_store.h"

#include "base/sha256.h"

#include <string.h>

typedef struct AbDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} AbDigestWriter;

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

ArchbirdStatus ab_project_source_store_init(ArchbirdEngine *engine,
                                            const uint8_t *manifest_json,
                                            size_t manifest_length,
                                            AbProjectSourceStore *store) {
  ArchbirdStatus status;
  memset(store, 0, sizeof(*store));
  status = ab_decode_source_manifest(engine, manifest_json, manifest_length,
                                     &store->manifest);
  if (status != ARCHBIRD_OK)
    goto failed;
  if (store->manifest.file_count) {
    store->entries = (AbSourceState *)ab_calloc(
        engine, store->manifest.file_count, sizeof(*store->entries));
    if (!store->entries) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory indexing source project");
      goto failed;
    }
  }
  status = digest_json(engine, manifest_json, manifest_length,
                       store->manifest_sha256, store->manifest_sha256_hex);
  if (status == ARCHBIRD_OK)
    status =
        map_input_digest(engine, &store->manifest, store->map_input_sha256_hex);
  if (status == ARCHBIRD_OK)
    return ARCHBIRD_OK;

failed:
  ab_project_source_store_destroy(engine, store);
  return status;
}

void ab_project_source_store_destroy(ArchbirdEngine *engine,
                                     AbProjectSourceStore *store) {
  size_t index;
  if (!store)
    return;
  for (index = 0; index < store->manifest.file_count; index++)
    ab_free(engine, store->entries ? store->entries[index].bytes : NULL);
  ab_free(engine, store->entries);
  ab_source_manifest_free(engine, &store->manifest);
  memset(store, 0, sizeof(*store));
}

AbManifestFile *ab_project_source_store_find(AbProjectSourceStore *store,
                                             const char *path,
                                             size_t path_length,
                                             size_t *out_index) {
  AbString wanted = {(char *)path, path_length};
  size_t low = 0;
  size_t high = store->manifest.file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&store->manifest.files[middle].path, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      if (out_index)
        *out_index = middle;
      return &store->manifest.files[middle];
    }
  }
  return NULL;
}
