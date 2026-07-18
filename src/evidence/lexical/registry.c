#include "archbird_internal.h"

#include "lexical/c/scanner.h"
#include "lexical/javascript/scanner.h"
#include "lexical/python/scanner.h"
#include "lexical/r/scanner.h"
#include "lexical/registry.h"
#include "project_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum AbLexicalProviderKind {
  AB_LEXICAL_C = 0,
  AB_LEXICAL_JAVASCRIPT = 1,
  AB_LEXICAL_PYTHON = 2,
  AB_LEXICAL_R = 3
} AbLexicalProviderKind;

typedef struct AbLexicalProvider {
  const char *id;
  AbLexicalProviderKind kind;
  const char *implementation_sha256;
} AbLexicalProvider;

static const AbLexicalProvider LEXICAL_PROVIDERS[] = {
    {"lexical:c", AB_LEXICAL_C, ARCHBIRD_LEXICAL_C_IMPLEMENTATION_SHA256},
    {"lexical:javascript", AB_LEXICAL_JAVASCRIPT,
     ARCHBIRD_LEXICAL_JAVASCRIPT_IMPLEMENTATION_SHA256},
    {"lexical:python", AB_LEXICAL_PYTHON,
     ARCHBIRD_LEXICAL_PYTHON_IMPLEMENTATION_SHA256},
    {"lexical:r", AB_LEXICAL_R, ARCHBIRD_LEXICAL_R_IMPLEMENTATION_SHA256},
};

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (length == 0 || memcmp(value->data, literal, length) == 0);
}

static int role_present(const AbManifestFile *file, const char *role) {
  size_t index;
  for (index = 0; index < file->roles.count; index++) {
    if (string_literal(&file->roles.items[index], role))
      return 1;
  }
  return 0;
}

static int same_layer(const AbManifestFile *left, const AbManifestFile *right);

static int public_header_for_layer(const AbManifestFile *header,
                                   const AbManifestFile *source) {
  static const char prefix[] = "public-header:";
  size_t index;
  if (role_present(header, "public-header") && same_layer(source, header))
    return 1;
  if (!source->has_layer)
    return 0;
  for (index = 0; index < header->roles.count; index++) {
    const AbString *role = &header->roles.items[index];
    if (role->length != sizeof(prefix) - 1 + source->layer.length ||
        memcmp(role->data, prefix, sizeof(prefix) - 1) != 0)
      continue;
    if (!source->layer.length ||
        memcmp(role->data + sizeof(prefix) - 1, source->layer.data,
               source->layer.length) == 0)
      return 1;
  }
  return 0;
}

static int c_language(const AbManifestFile *file) {
  return file->has_language && (string_literal(&file->language, "c") ||
                                string_literal(&file->language, "cpp"));
}

static int r_language(const AbManifestFile *file) {
  return file->has_language && string_literal(&file->language, "r");
}

static int js_language(const AbManifestFile *file) {
  return file->has_language && (string_literal(&file->language, "javascript") ||
                                string_literal(&file->language, "typescript") ||
                                string_literal(&file->language, "vue"));
}

static int python_language(const AbManifestFile *file) {
  return file->has_language && string_literal(&file->language, "python");
}

static int provider_matches(const AbLexicalProvider *provider,
                            const AbManifestFile *file) {
  switch (provider->kind) {
  case AB_LEXICAL_C:
    return c_language(file);
  case AB_LEXICAL_JAVASCRIPT:
    return js_language(file);
  case AB_LEXICAL_PYTHON:
    return python_language(file);
  case AB_LEXICAL_R:
    return r_language(file);
  }
  return 0;
}

static const AbLexicalProvider *provider_for_file(const AbManifestFile *file) {
  size_t index;
  for (index = 0;
       index < sizeof(LEXICAL_PROVIDERS) / sizeof(LEXICAL_PROVIDERS[0]);
       index++) {
    if (provider_matches(&LEXICAL_PROVIDERS[index], file))
      return &LEXICAL_PROVIDERS[index];
  }
  return NULL;
}

static const AbLexicalProvider *provider_by_id(const char *id,
                                               size_t id_length) {
  size_t index;
  for (index = 0;
       index < sizeof(LEXICAL_PROVIDERS) / sizeof(LEXICAL_PROVIDERS[0]);
       index++) {
    size_t length = strlen(LEXICAL_PROVIDERS[index].id);
    if (length == id_length && !memcmp(id, LEXICAL_PROVIDERS[index].id, length))
      return &LEXICAL_PROVIDERS[index];
  }
  return NULL;
}

static int manifest_file_index(const AbSourceManifest *manifest,
                               const char *path, size_t path_length,
                               size_t *out_index) {
  AbString wanted = {(char *)path, path_length};
  size_t low = 0;
  size_t high = manifest->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&manifest->files[middle].path, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      *out_index = middle;
      return 1;
    }
  }
  return 0;
}

static int same_layer(const AbManifestFile *left, const AbManifestFile *right) {
  return left->has_layer == right->has_layer &&
         (!left->has_layer || ab_string_equal(&left->layer, &right->layer));
}

static int hex_nibble(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

static void implementation_digest(const char *hex, uint8_t digest[32]) {
  size_t index;
  for (index = 0; index < 32; index++) {
    int high = hex_nibble(hex[index * 2]);
    int low = hex_nibble(hex[index * 2 + 1]);
    digest[index] = (uint8_t)((high << 4) | low);
  }
}

static ArchbirdStatus
scan_file(ArchbirdEngine *engine, ArchbirdProject *project,
          const AbSourceManifest *manifest, size_t file_index,
          const AbLexicalProvider *provider, const uint8_t manifest_digest[32],
          AbProviderBundle *out_bundle) {
  const AbManifestFile *file = &manifest->files[file_index];
  uint8_t provider_digest[32];
  implementation_digest(provider->implementation_sha256, provider_digest);
  if (provider->kind == AB_LEXICAL_C) {
    AbNameSet public_names = {0};
    size_t header_index;
    ArchbirdStatus status;
    for (header_index = 0; header_index < manifest->file_count;
         header_index++) {
      const AbManifestFile *header = &manifest->files[header_index];
      if (!c_language(header) || !public_header_for_layer(header, file))
        continue;
      status = ab_c_collect_public_names(
          engine, ab_project_source_bytes(project, header_index),
          header->byte_length, &public_names);
      if (status != ARCHBIRD_OK) {
        ab_name_set_free(&public_names);
        return status;
      }
    }
    status = ab_scan_c_file(engine, manifest, file,
                            ab_project_source_bytes(project, file_index),
                            file->byte_length, manifest_digest, &public_names,
                            provider_digest, out_bundle);
    ab_name_set_free(&public_names);
    return status;
  }
  if (provider->kind == AB_LEXICAL_JAVASCRIPT)
    return ab_scan_js_file(engine, manifest, file,
                           ab_project_source_bytes(project, file_index),
                           file->byte_length, provider_digest, out_bundle);
  if (provider->kind == AB_LEXICAL_PYTHON)
    return ab_scan_python_file(engine, manifest, file,
                               ab_project_source_bytes(project, file_index),
                               file->byte_length, provider_digest, out_bundle);
  return ab_scan_r_file(engine, manifest, file,
                        ab_project_source_bytes(project, file_index),
                        file->byte_length, provider_digest, out_bundle);
}

static ArchbirdStatus scan_providers(ArchbirdEngine *engine,
                                     ArchbirdProject *project,
                                     const AbLexicalProvider *selected,
                                     ArchbirdProviderMode mode) {
  const AbSourceManifest *manifest;
  const uint8_t *manifest_digest;
  AbProviderBundle *bundles = NULL;
  size_t bundle_count = 0;
  size_t file_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  manifest = ab_project_manifest(project);
  manifest_digest = ab_project_manifest_sha256_bytes(project);
  for (file_index = 0; file_index < manifest->file_count; file_index++) {
    const AbLexicalProvider *provider =
        provider_for_file(&manifest->files[file_index]);
    if (provider && (!selected || provider == selected))
      bundle_count++;
  }
  if (!bundle_count)
    goto done;
  bundles =
      (AbProviderBundle *)ab_calloc(engine, bundle_count, sizeof(*bundles));
  if (!bundles) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory creating lexical providers");
    goto done;
  }
  bundle_count = 0;
  for (file_index = 0; file_index < manifest->file_count; file_index++) {
    const AbManifestFile *file = &manifest->files[file_index];
    const AbLexicalProvider *provider = provider_for_file(file);
    if (!provider || (selected && provider != selected))
      continue;
    status = scan_file(engine, project, manifest, file_index, provider,
                       manifest_digest, &bundles[bundle_count]);
    if (status != ARCHBIRD_OK)
      goto done;
    bundle_count++;
  }
  status = ab_project_take_provider_bundles(engine, project, mode, bundles,
                                            bundle_count);
done:
  if (bundles) {
    for (file_index = 0; file_index < bundle_count; file_index++)
      ab_provider_bundle_free(engine, &bundles[file_index]);
  }
  ab_free(engine, bundles);
  return status;
}

int ab_lexical_provider_known(const char *provider_id,
                              size_t provider_id_length) {
  return provider_id && provider_by_id(provider_id, provider_id_length) != NULL;
}

ArchbirdStatus ab_scan_lexical_providers(ArchbirdEngine *engine,
                                         ArchbirdProject *project,
                                         ArchbirdProviderMode mode) {
  return scan_providers(engine, project, NULL, mode);
}

ArchbirdStatus ab_scan_lexical_provider(ArchbirdEngine *engine,
                                        ArchbirdProject *project,
                                        const char *provider_id,
                                        size_t provider_id_length,
                                        ArchbirdProviderMode mode) {
  const AbLexicalProvider *provider;
  if (!engine || !project || !provider_id)
    return ARCHBIRD_INVALID_ARGUMENT;
  provider = provider_by_id(provider_id, provider_id_length);
  if (!provider)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "unknown lexical provider ID");
  return scan_providers(engine, project, provider, mode);
}

ArchbirdStatus
ab_scan_lexical_provider_file(ArchbirdEngine *engine, ArchbirdProject *project,
                              const char *provider_id,
                              size_t provider_id_length, const char *path,
                              size_t path_length, ArchbirdProviderMode mode) {
  const AbSourceManifest *manifest;
  const AbLexicalProvider *provider;
  AbProviderBundle bundle = {0};
  size_t file_index;
  ArchbirdStatus status;
  if (!engine || !project || !provider_id || !path || !path_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  provider = provider_by_id(provider_id, provider_id_length);
  if (!provider)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "unknown lexical provider ID");
  manifest = ab_project_manifest(project);
  if (!manifest_file_index(manifest, path, path_length, &file_index))
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "provider file is absent from the manifest");
  if (!provider_matches(provider, &manifest->files[file_index]))
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "provider does not support the selected file");
  status = scan_file(engine, project, manifest, file_index, provider,
                     ab_project_manifest_sha256_bytes(project), &bundle);
  if (status == ARCHBIRD_OK)
    status = ab_project_take_provider_bundle(engine, project, mode, &bundle);
  ab_provider_bundle_free(engine, &bundle);
  return status;
}
