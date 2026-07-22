#include "archbird/archbird.h"

#include "archbird_internal.h"
#include "json_internal.h"
#include "json_value.h"
#include "project_configuration.h"

#include <string.h>

ArchbirdStatus archbird_project_configuration_compile(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbProjectConfiguration configuration = {0};
  AbBuffer buffer;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&buffer, engine);
  status = ab_project_configuration_decode(engine, config_json, config_length,
                                           &configuration);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer,
                               "{\"artifact\":\"project-configuration-plan\","
                               "\"constraints\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&buffer, &configuration.constraints);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"constraint_policy_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer,
                                   configuration.constraint_policy_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"map_definition\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&buffer, &configuration.map_definition);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"map_config_sha256\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, configuration.map_config_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(
        &buffer, ab_value_member(&configuration.normalized, "project"));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"project_configuration_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, configuration.sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"projections\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&buffer, &configuration.projections);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"queries\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&buffer, &configuration.queries);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"schema_version\":1}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, buffer.data, buffer.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&buffer);
  ab_project_configuration_free(engine, &configuration);
  return status;
}
