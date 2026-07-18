#include "archbird_internal.h"

#include "lexical/registry.h"
#include "semantic/registry.h"
#include "syntax/registry.h"

ArchbirdStatus archbird_project_scan_builtin(ArchbirdEngine *engine,
                                             ArchbirdProject *project,
                                             ArchbirdProviderMode mode) {
  ArchbirdStatus status;
  if (!engine || !project)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status == ARCHBIRD_OK)
    status = ab_scan_lexical_providers(
        engine, project,
        mode == ARCHBIRD_PROVIDER_PRIMARY ? ARCHBIRD_PROVIDER_AUGMENT : mode);
  if (status == ARCHBIRD_OK)
    status = ab_scan_syntax_providers(engine, project, mode);
  if (status == ARCHBIRD_OK)
    status = ab_scan_semantic_providers(engine, project, mode);
  return status;
}

ArchbirdStatus archbird_project_scan_builtin_provider(
    ArchbirdEngine *engine, ArchbirdProject *project, const char *provider_id,
    size_t provider_id_length, ArchbirdProviderMode mode) {
  ArchbirdStatus status;
  if (!engine || !project || !provider_id)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  if (ab_lexical_provider_known(provider_id, provider_id_length))
    return ab_scan_lexical_provider(engine, project, provider_id,
                                    provider_id_length, mode);
  if (ab_syntax_provider_known(provider_id, provider_id_length))
    return ab_scan_syntax_provider(engine, project, provider_id,
                                   provider_id_length, mode);
  if (ab_semantic_provider_known(provider_id, provider_id_length))
    return ab_scan_semantic_provider(engine, project, provider_id,
                                     provider_id_length, mode);
  return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                            ARCHBIRD_NO_OFFSET,
                            "unknown or unavailable built-in provider ID");
}

ArchbirdStatus archbird_project_scan_builtin_provider_file(
    ArchbirdEngine *engine, ArchbirdProject *project, const char *provider_id,
    size_t provider_id_length, const char *path, size_t path_length,
    ArchbirdProviderMode mode) {
  ArchbirdStatus status;
  if (!engine || !project || !provider_id || !path || !path_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  if (ab_lexical_provider_known(provider_id, provider_id_length))
    return ab_scan_lexical_provider_file(engine, project, provider_id,
                                         provider_id_length, path, path_length,
                                         mode);
  if (ab_syntax_provider_known(provider_id, provider_id_length))
    return ab_scan_syntax_provider_file(engine, project, provider_id,
                                        provider_id_length, path, path_length,
                                        mode);
  if (ab_semantic_provider_known(provider_id, provider_id_length))
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "semantic provider is project-scoped and cannot scan one file");
  return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                            ARCHBIRD_NO_OFFSET,
                            "unknown or unavailable built-in provider ID");
}
