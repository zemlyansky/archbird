#include "archbird/archbird.h"

#include "archbird_internal.h"
#include "json_internal.h"
#include "json_value.h"
#include "project_configuration.h"
#include "query_internal.h"

#include <string.h>

static int stable_id(const char *value, size_t length) {
  size_t index;
  if (!value || !length)
    return 0;
  for (index = 0; index < length; index++) {
    unsigned char byte = (unsigned char)value[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
}

static const AbObjectField *named_field(const AbValue *object,
                                        const AbString *name) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return NULL;
  for (index = 0; index < object->as.object.count; index++)
    if (ab_string_equal(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index];
  return NULL;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "query plan: %s", message);
}

ArchbirdStatus archbird_query_plan_compile(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const char *query_id, size_t query_id_length, const uint8_t *overrides_json,
    size_t overrides_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  const AbValue empty = {.kind = AB_VALUE_OBJECT};
  AbProjectConfiguration configuration = {0};
  AbValue overrides = {.kind = AB_VALUE_OBJECT};
  AbValue plan = {0};
  AbString id = {(char *)(query_id_length ? query_id : "ad-hoc"),
                 query_id_length ? query_id_length : 6};
  const AbObjectField *query = NULL;
  const AbValue *definition = NULL;
  const AbValue *configured_projections = &empty;
  const char *map_config_sha256 = NULL;
  const char *project_configuration_sha256 = NULL;
  AbBuffer rendered;
  ArchbirdStatus status;
  int named = query_id_length != 0;
  if (!engine || (!config_json && config_length) ||
      (!query_id && query_id_length) || (!overrides_json && overrides_length) ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!stable_id(id.data, id.length) || (named && !config_length))
    return invalid(engine, "query id is not a stable identifier");
  ab_buffer_init(&rendered, engine);
  status = named ? ab_project_configuration_decode(
                       engine, config_json, config_length, &configuration)
                 : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK && overrides_length)
    status = ab_json_value_decode(engine, overrides_json, overrides_length,
                                  &overrides);
  if (status == ARCHBIRD_OK && overrides.kind != AB_VALUE_OBJECT)
    status = invalid(engine, "runtime overrides must be an object");
  if (status == ARCHBIRD_OK && named) {
    query = named_field(&configuration.queries, &id);
    if (!query)
      status = invalid(engine, "unknown named query");
    else {
      definition = &query->value;
      configured_projections = &configuration.projections;
      map_config_sha256 = configuration.map_config_sha256;
      project_configuration_sha256 = configuration.sha256;
    }
  } else if (status == ARCHBIRD_OK) {
    definition = &overrides;
  }
  if (status == ARCHBIRD_OK)
    status = ab_query_plan_compile_definition(
        engine, &id, definition, named ? &overrides : &empty,
        configured_projections, map_config_sha256, project_configuration_sha256,
        &plan);
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_literal(&rendered, "{\"artifact\":\"query-plan\",\"plan\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&rendered, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"schema_version\":2}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&rendered);
  ab_value_free(engine, &plan);
  if (overrides_length)
    ab_value_free(engine, &overrides);
  ab_project_configuration_free(engine, &configuration);
  return status;
}
