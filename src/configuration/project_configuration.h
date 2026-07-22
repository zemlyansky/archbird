#ifndef ARCHBIRD_PROJECT_CONFIGURATION_H
#define ARCHBIRD_PROJECT_CONFIGURATION_H

#include "model.h"
#include "render_internal.h"

typedef struct AbProjectConfiguration {
  AbValue normalized;
  /* Canonical schema-2 fields that affect Map construction.  Execution uses
   * the typed AbMapConfig decoded from the project configuration. */
  AbValue map_definition;
  AbValue projections;
  AbValue queries;
  AbValue constraints;
  char constraint_policy_sha256[65];
  char map_config_sha256[65];
  char sha256[65];
} AbProjectConfiguration;

const uint8_t *ab_default_project_configuration(size_t *length);

ArchbirdStatus ab_project_configuration_decode(ArchbirdEngine *engine,
                                               const uint8_t *json,
                                               size_t json_length,
                                               AbProjectConfiguration *out);

void ab_project_configuration_free(ArchbirdEngine *engine,
                                   AbProjectConfiguration *configuration);

#endif
