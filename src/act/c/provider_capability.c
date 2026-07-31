#include "c/provider_capability.h"

#include "artifact_validation.h"
#include "c/declaration.h"
#include "c/napi_export.h"
#include "project_internal.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(AbActContext *context, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(ab_act_executor_engine(context), status,
                            ARCHBIRD_NO_OFFSET, "act C provider executor: %s",
                            message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         memcmp(value->data, literal, length) == 0;
}

static const AbValue *find_named_row(const AbValue *rows,
                                     const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

static const AbConfigProvider *
find_configured_provider(const AbMapConfig *config, const AbString *surface,
                         const AbString *kind,
                         const AbString *definition_sha256) {
  const AbConfigProvider *matched = NULL;
  size_t bridge_index;
  if (!config)
    return NULL;
  for (bridge_index = 0; bridge_index < config->bridge_count; bridge_index++) {
    const AbConfigBridge *bridge = &config->bridges[bridge_index];
    size_t provider_index;
    if (!ab_string_equal(&bridge->name, surface))
      continue;
    for (provider_index = 0; provider_index < bridge->provider_count;
         provider_index++) {
      const AbConfigProvider *provider = &bridge->providers[provider_index];
      char candidate_sha256[65];
      if (!ab_string_equal(&provider->kind, kind) ||
          ab_config_provider_definition_sha256(provider, candidate_sha256) !=
              ARCHBIRD_OK ||
          definition_sha256->length != 64 ||
          memcmp(candidate_sha256, definition_sha256->data, 64) != 0)
        continue;
      if (matched)
        return NULL;
      matched = provider;
    }
  }
  return matched;
}

static const char *mapped_source(const AbString *kind) {
  if (string_is(kind, "file_pattern"))
    return "file-pattern";
  if (string_is(kind, "exports"))
    return "exports";
  return NULL;
}

static int mapped_provider_equal(const AbValue *value,
                                 const AbActCProviderCapability *provider) {
  const AbValue *definition_sha256 = field(value, "definition_sha256");
  const AbValue *path = field(value, "path");
  const char *source = mapped_source(provider->kind);
  return source && ab_artifact_sha256(definition_sha256) && path &&
         path->kind == AB_VALUE_STRING &&
         ab_artifact_text_is(field(value, "source"), source) &&
         ab_string_equal(&definition_sha256->as.text,
                         provider->definition_sha256) &&
         ab_string_equal(&path->as.text, provider->path);
}

static int
mapped_provider_definition_equal(const AbValue *value,
                                 const AbActCProviderCapability *provider) {
  const AbValue *definition_sha256 = field(value, "definition_sha256");
  const char *source = mapped_source(provider->kind);
  return source && ab_artifact_sha256(definition_sha256) &&
         ab_artifact_text_is(field(value, "source"), source) &&
         ab_string_equal(&definition_sha256->as.text,
                         provider->definition_sha256);
}

static int row_has_provider(const AbValue *row,
                            const AbActCProviderCapability *provider) {
  const AbValue *declarations = field(row, "declarations");
  size_t index;
  if (!declarations || declarations->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < declarations->as.array.count; index++)
    if (mapped_provider_equal(&declarations->as.array.items[index], provider))
      return 1;
  return 0;
}

static int file_target_is_groundable(const AbValue *row) {
  const AbValue *candidates = field(row, "candidates");
  const AbValue *uses = field(row, "uses");
  int declared = ab_artifact_text_is(field(row, "declaration"), "declared");
  return row && ab_artifact_text_is(field(row, "resolution"), "unique") &&
         candidates && candidates->kind == AB_VALUE_ARRAY &&
         candidates->as.array.count == 1 &&
         (declared ||
          (ab_artifact_text_is(field(row, "declaration"), "undeclared") &&
           uses && uses->kind == AB_VALUE_ARRAY && uses->as.array.count));
}

static int export_target_is_groundable(const AbValue *row) {
  const AbValue *uses = field(row, "uses");
  return row && ab_artifact_text_is(field(row, "declaration"), "undeclared") &&
         uses && uses->kind == AB_VALUE_ARRAY && uses->as.array.count;
}

static ArchbirdStatus load_provider(AbActContext *context,
                                    const AbValue *operation,
                                    AbActCProviderCapability *out) {
  const AbValue *provider = field(operation, "provider");
  const AbValue *definition_sha256 = field(provider, "definition_sha256");
  const AbValue *kind = field(provider, "kind");
  const AbValue *path = field(provider, "path");
  const AbValue *surface = field(operation, "surface");
  const AbValue *capability = field(operation, "capability");
  const AbValue *mapped_providers;
  size_t index;
  size_t matched = 0;
  size_t definition_matches = 0;
  memset(out, 0, sizeof(*out));
  if ((!ab_artifact_text_is(kind, "file_pattern") &&
       !ab_artifact_text_is(kind, "exports")) ||
      !ab_artifact_sha256(definition_sha256) ||
      !ab_artifact_repository_path(path) || !surface ||
      surface->kind != AB_VALUE_STRING || !capability ||
      capability->kind != AB_VALUE_STRING)
    return reject(context, ARCHBIRD_INVALID_SCHEMA,
                  "the operation has no valid C provider identity");
  out->definition_sha256 = &definition_sha256->as.text;
  out->kind = &kind->as.text;
  out->surface = &surface->as.text;
  out->path = &path->as.text;
  out->capability = &capability->as.text;
  out->configuration = find_configured_provider(
      ab_project_config(ab_act_executor_project(context)), out->surface,
      out->kind, out->definition_sha256);
  out->mapped_surface = find_named_row(
      field(ab_act_executor_map(context), "surfaces"), out->surface);
  if (!out->configuration || !out->mapped_surface)
    return reject(context, ARCHBIRD_CONFLICT,
                  "the configured provider differs from the current Map");
  mapped_providers = field(out->mapped_surface, "providers");
  for (index = 0;
       mapped_providers && mapped_providers->kind == AB_VALUE_ARRAY &&
       index < mapped_providers->as.array.count;
       index++) {
    if (mapped_provider_equal(&mapped_providers->as.array.items[index], out))
      matched++;
    if (mapped_provider_definition_equal(
            &mapped_providers->as.array.items[index], out))
      definition_matches++;
  }
  if (matched != 1 || definition_matches != 1)
    return reject(context, ARCHBIRD_CONFLICT,
                  "the provider does not map to one unique source file");
  out->target =
      find_named_row(field(out->mapped_surface, "names"), out->capability);
  if (!out->target || row_has_provider(out->target, out))
    return reject(context, ARCHBIRD_CONFLICT,
                  "the current Map does not require this provider capability");
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_c_provider_capability(AbActContext *context,
                                            const AbValue *operation,
                                            const AbString *item_id) {
  const AbValue *action = field(operation, "action");
  AbActCProviderCapability provider;
  ArchbirdStatus status;
  if (!ab_artifact_text_is(action, "add_provider_capability"))
    return reject(context, ARCHBIRD_INVALID_SCHEMA,
                  "C providers support only capability addition");
  status = load_provider(context, operation, &provider);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_is(provider.kind, "file_pattern")) {
    if (!file_target_is_groundable(provider.target))
      return reject(context, ARCHBIRD_CONFLICT,
                    "the file provider target is not exactly groundable");
    return ab_act_c_file_provider_capability(context, &provider, operation,
                                             item_id);
  }
  if (!export_target_is_groundable(provider.target))
    return reject(context, ARCHBIRD_CONFLICT,
                  "the export provider target is not exactly groundable");
  return ab_act_c_napi_export_provider_capability(context, &provider, operation,
                                                  item_id);
}
