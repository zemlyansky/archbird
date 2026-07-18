#include "syntax/registry.h"

#include "archbird_internal.h"
#include "json_value.h"
#include "project_internal.h"

#ifdef ARCHBIRD_HAVE_TREE_SITTER_C
#include "syntax/tree_sitter/c/scanner.h"
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_CPP
#include "syntax/tree_sitter/cpp/scanner.h"
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_JAVASCRIPT
#include "syntax/tree_sitter/javascript/scanner.h"
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_PYTHON
#include "syntax/tree_sitter/python/scanner.h"
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_R
#include "syntax/tree_sitter/r/scanner.h"
#endif
#if defined(ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT) ||                           \
    defined(ARCHBIRD_HAVE_TREE_SITTER_TSX)
#include "syntax/tree_sitter/typescript/scanner.h"
#endif

#include <stdlib.h>
#include <string.h>

typedef enum AbSyntaxKind {
  AB_SYNTAX_NONE = 0,
  AB_SYNTAX_C,
  AB_SYNTAX_CPP,
  AB_SYNTAX_PYTHON,
  AB_SYNTAX_JAVASCRIPT,
  AB_SYNTAX_TYPESCRIPT,
  AB_SYNTAX_TSX,
  AB_SYNTAX_R
} AbSyntaxKind;

typedef struct AbSyntaxProvider {
  const char *id;
  const char *language;
  const char *implementation_sha256;
  AbSyntaxKind kind;
} AbSyntaxProvider;

static const AbSyntaxProvider SYNTAX_PROVIDERS[] = {
#ifdef ARCHBIRD_HAVE_TREE_SITTER_C
    {"syntax:tree-sitter:c", "c", ARCHBIRD_TREE_SITTER_C_IMPLEMENTATION_SHA256,
     AB_SYNTAX_C},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_CPP
    {"syntax:tree-sitter:cpp", "cpp",
     ARCHBIRD_TREE_SITTER_CPP_IMPLEMENTATION_SHA256, AB_SYNTAX_CPP},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_PYTHON
    {"syntax:tree-sitter:python", "python",
     ARCHBIRD_TREE_SITTER_PYTHON_IMPLEMENTATION_SHA256, AB_SYNTAX_PYTHON},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_JAVASCRIPT
    {"syntax:tree-sitter:javascript", "javascript",
     ARCHBIRD_TREE_SITTER_JAVASCRIPT_IMPLEMENTATION_SHA256,
     AB_SYNTAX_JAVASCRIPT},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
    {"syntax:tree-sitter:typescript", "typescript",
     ARCHBIRD_TREE_SITTER_TYPESCRIPT_IMPLEMENTATION_SHA256,
     AB_SYNTAX_TYPESCRIPT},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
    {"syntax:tree-sitter:tsx", "typescript",
     ARCHBIRD_TREE_SITTER_TSX_IMPLEMENTATION_SHA256, AB_SYNTAX_TSX},
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_R
    {"syntax:tree-sitter:r", "r", ARCHBIRD_TREE_SITTER_R_IMPLEMENTATION_SHA256,
     AB_SYNTAX_R},
#endif
    {NULL, NULL, NULL, AB_SYNTAX_NONE}};

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (length == 0 || memcmp(value->data, literal, length) == 0);
}

static int path_suffix(const AbString *path, const char *suffix) {
  size_t length = strlen(suffix);
  return path->length >= length &&
         memcmp(path->data + path->length - length, suffix, length) == 0;
}

static int provider_matches(const AbSyntaxProvider *provider,
                            const AbManifestFile *file) {
  if (!file->has_language ||
      !string_literal(&file->language, provider->language))
    return 0;
  if (provider->kind == AB_SYNTAX_TYPESCRIPT)
    return !path_suffix(&file->path, ".tsx");
  if (provider->kind == AB_SYNTAX_TSX)
    return path_suffix(&file->path, ".tsx");
  return 1;
}

static const AbSyntaxProvider *provider_for_file(const AbManifestFile *file) {
  size_t index;
  for (index = 0; SYNTAX_PROVIDERS[index].id; index++)
    if (provider_matches(&SYNTAX_PROVIDERS[index], file))
      return &SYNTAX_PROVIDERS[index];
  return NULL;
}

static const AbSyntaxProvider *provider_by_id(const char *id,
                                              size_t id_length) {
  size_t index;
  for (index = 0; SYNTAX_PROVIDERS[index].id; index++) {
    size_t length = strlen(SYNTAX_PROVIDERS[index].id);
    if (length == id_length &&
        memcmp(id, SYNTAX_PROVIDERS[index].id, length) == 0)
      return &SYNTAX_PROVIDERS[index];
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

static void implementation_digest(const char *hex, uint8_t digest[32]);

#if defined(ARCHBIRD_HAVE_TREE_SITTER_C) &&                                    \
    defined(ARCHBIRD_HAVE_TREE_SITTER_CPP)
typedef struct AbSyntaxRecovery {
  uint64_t errors;
  uint64_t missing;
} AbSyntaxRecovery;

static AbSyntaxRecovery syntax_recovery(const AbProviderBundle *bundle) {
  AbSyntaxRecovery recovery = {0, 0};
  size_t fact_index;
  for (fact_index = 0; fact_index < bundle->fact_count; fact_index++) {
    const AbFact *fact = &bundle->facts[fact_index];
    size_t attribute_index;
    if (!string_literal(&fact->domain, "syntax-summaries"))
      continue;
    for (attribute_index = 0; attribute_index < fact->attribute_count;
         attribute_index++) {
      const AbObjectField *attribute = &fact->attributes[attribute_index];
      uint64_t value;
      if (!ab_value_u64(&attribute->value, &value))
        continue;
      if (string_literal(&attribute->name, "error_nodes"))
        recovery.errors = value;
      else if (string_literal(&attribute->name, "missing_nodes"))
        recovery.missing = value;
    }
  }
  return recovery;
}

static int recovery_better(AbSyntaxRecovery candidate,
                           AbSyntaxRecovery baseline) {
  uint64_t candidate_total = candidate.errors > UINT64_MAX - candidate.missing
                                 ? UINT64_MAX
                                 : candidate.errors + candidate.missing;
  uint64_t baseline_total = baseline.errors > UINT64_MAX - baseline.missing
                                ? UINT64_MAX
                                : baseline.errors + baseline.missing;
  if (candidate_total != baseline_total)
    return candidate_total < baseline_total;
  if (candidate.errors != baseline.errors)
    return candidate.errors < baseline.errors;
  return candidate.missing < baseline.missing;
}

static int discovery_c_header(const AbManifestFile *file) {
  return file->has_layer && string_literal(&file->layer, "auto-c") &&
         path_suffix(&file->path, ".h");
}

static ArchbirdStatus scan_discovered_c_header(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t manifest_digest[32], const uint8_t c_digest[32],
    AbProviderBundle *out_bundle) {
  AbProviderBundle c_bundle = {0};
  AbProviderBundle cpp_bundle = {0};
  uint8_t cpp_digest[32];
  ArchbirdStatus status =
      ab_scan_tree_sitter_c_file(engine, manifest, file, source, source_length,
                                 manifest_digest, c_digest, &c_bundle);
  implementation_digest(ARCHBIRD_TREE_SITTER_CPP_IMPLEMENTATION_SHA256,
                        cpp_digest);
  if (status == ARCHBIRD_OK)
    status = ab_scan_tree_sitter_cpp_file(engine, manifest, file, source,
                                          source_length, manifest_digest,
                                          cpp_digest, &cpp_bundle);
  if (status == ARCHBIRD_OK && recovery_better(syntax_recovery(&cpp_bundle),
                                               syntax_recovery(&c_bundle))) {
    *out_bundle = cpp_bundle;
    memset(&cpp_bundle, 0, sizeof(cpp_bundle));
  } else if (status == ARCHBIRD_OK) {
    *out_bundle = c_bundle;
    memset(&c_bundle, 0, sizeof(c_bundle));
  }
  ab_provider_bundle_free(engine, &c_bundle);
  ab_provider_bundle_free(engine, &cpp_bundle);
  return status;
}
#endif

static int hex_nibble(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
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
          const AbSyntaxProvider *provider, const uint8_t manifest_digest[32],
          AbProviderBundle *out_bundle) {
  const AbManifestFile *file = &manifest->files[file_index];
  const uint8_t *source = ab_project_source_bytes(project, file_index);
  uint8_t provider_digest[32];
  (void)file;
  (void)source;
  (void)manifest_digest;
  (void)out_bundle;
  implementation_digest(provider->implementation_sha256, provider_digest);
  switch (provider->kind) {
#ifdef ARCHBIRD_HAVE_TREE_SITTER_C
  case AB_SYNTAX_C:
#ifdef ARCHBIRD_HAVE_TREE_SITTER_CPP
    if (discovery_c_header(file))
      return scan_discovered_c_header(engine, manifest, file, source,
                                      file->byte_length, manifest_digest,
                                      provider_digest, out_bundle);
#endif
    return ab_scan_tree_sitter_c_file(engine, manifest, file, source,
                                      file->byte_length, manifest_digest,
                                      provider_digest, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_CPP
  case AB_SYNTAX_CPP:
    return ab_scan_tree_sitter_cpp_file(engine, manifest, file, source,
                                        file->byte_length, manifest_digest,
                                        provider_digest, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_PYTHON
  case AB_SYNTAX_PYTHON:
    return ab_scan_tree_sitter_python_file(engine, manifest, file, source,
                                           file->byte_length, manifest_digest,
                                           provider_digest, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_JAVASCRIPT
  case AB_SYNTAX_JAVASCRIPT:
    return ab_scan_tree_sitter_javascript_file(
        engine, manifest, file, source, file->byte_length, manifest_digest,
        provider_digest, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TYPESCRIPT
  case AB_SYNTAX_TYPESCRIPT:
    return ab_scan_tree_sitter_typescript_file(
        engine, manifest, file, source, file->byte_length, manifest_digest,
        provider_digest, 0, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_TSX
  case AB_SYNTAX_TSX:
    return ab_scan_tree_sitter_typescript_file(
        engine, manifest, file, source, file->byte_length, manifest_digest,
        provider_digest, 1, out_bundle);
#endif
#ifdef ARCHBIRD_HAVE_TREE_SITTER_R
  case AB_SYNTAX_R:
    return ab_scan_tree_sitter_r_file(engine, manifest, file, source,
                                      file->byte_length, manifest_digest,
                                      provider_digest, out_bundle);
#endif
  case AB_SYNTAX_NONE:
  default:
    break;
  }
  return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                            "syntax provider was built without its scanner");
}

static ArchbirdStatus scan_providers(ArchbirdEngine *engine,
                                     ArchbirdProject *project,
                                     const AbSyntaxProvider *selected,
                                     ArchbirdProviderMode mode) {
  const AbSourceManifest *manifest = ab_project_manifest(project);
  const uint8_t *manifest_digest = ab_project_manifest_sha256_bytes(project);
  AbProviderBundle *bundles = NULL;
  size_t bundle_count = 0;
  size_t file_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (file_index = 0; file_index < manifest->file_count; file_index++) {
    const AbSyntaxProvider *provider =
        provider_for_file(&manifest->files[file_index]);
    if (provider && (!selected || provider == selected))
      bundle_count++;
  }
  if (!bundle_count)
    return ARCHBIRD_OK;
  bundles =
      (AbProviderBundle *)ab_calloc(engine, bundle_count, sizeof(*bundles));
  if (!bundles)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory creating syntax providers");
  bundle_count = 0;
  for (file_index = 0; file_index < manifest->file_count; file_index++) {
    const AbSyntaxProvider *provider =
        provider_for_file(&manifest->files[file_index]);
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
  for (file_index = 0; file_index < bundle_count; file_index++)
    ab_provider_bundle_free(engine, &bundles[file_index]);
  ab_free(engine, bundles);
  return status;
}

int ab_syntax_provider_known(const char *provider_id,
                             size_t provider_id_length) {
  return provider_id && provider_by_id(provider_id, provider_id_length) != NULL;
}

ArchbirdStatus ab_scan_syntax_providers(ArchbirdEngine *engine,
                                        ArchbirdProject *project,
                                        ArchbirdProviderMode mode) {
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  return scan_providers(engine, project, NULL, mode);
}

ArchbirdStatus ab_scan_syntax_provider(ArchbirdEngine *engine,
                                       ArchbirdProject *project,
                                       const char *provider_id,
                                       size_t provider_id_length,
                                       ArchbirdProviderMode mode) {
  const AbSyntaxProvider *provider;
  if (!engine || !project || !provider_id)
    return ARCHBIRD_INVALID_ARGUMENT;
  provider = provider_by_id(provider_id, provider_id_length);
  if (!provider)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "unknown or unavailable syntax provider ID");
  return scan_providers(engine, project, provider, mode);
}

ArchbirdStatus
ab_scan_syntax_provider_file(ArchbirdEngine *engine, ArchbirdProject *project,
                             const char *provider_id, size_t provider_id_length,
                             const char *path, size_t path_length,
                             ArchbirdProviderMode mode) {
  const AbSourceManifest *manifest;
  const AbSyntaxProvider *provider;
  AbProviderBundle bundle = {0};
  size_t file_index;
  ArchbirdStatus status;
  if (!engine || !project || !provider_id || !path || !path_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  provider = provider_by_id(provider_id, provider_id_length);
  if (!provider)
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "unknown or unavailable syntax provider ID");
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
