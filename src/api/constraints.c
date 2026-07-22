#include "archbird/archbird.h"

#include "constraints_internal.h"
#include "project_configuration.h"

static AbConstraintPolicyInput
policy_input(const AbProjectConfiguration *configuration) {
  AbConstraintPolicyInput input = {
      .project = ab_value_member(&configuration->normalized, "project"),
      .description = ab_value_member(&configuration->normalized, "description"),
      .projections = &configuration->projections,
      .constraints = &configuration->constraints,
      .constraint_policy_sha256 = configuration->constraint_policy_sha256,
      .map_config_sha256 = configuration->map_config_sha256,
      .project_configuration_sha256 = configuration->sha256,
  };
  return input;
}

ArchbirdStatus archbird_constraints_evaluate(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbProjectConfiguration configuration = {0};
  AbConstraintPolicyInput policy;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_project_configuration_decode(engine, config_json, config_length,
                                           &configuration);
  policy = policy_input(&configuration);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_evaluate(engine, &policy, map_json, map_length,
                                     resolution_json, resolution_length,
                                     request_json, request_length, json_flags,
                                     write_fn, user_data);
  ab_project_configuration_free(engine, &configuration);
  return status;
}

ArchbirdStatus archbird_constraints_report(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, ArchbirdVerificationFormat format,
    size_t max_findings, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  AbProjectConfiguration configuration = {0};
  AbConstraintPolicyInput policy;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      format < ARCHBIRD_VERIFICATION_MARKDOWN ||
      format > ARCHBIRD_VERIFICATION_JUNIT ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_project_configuration_decode(engine, config_json, config_length,
                                           &configuration);
  policy = policy_input(&configuration);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_report(
        engine, &policy, map_json, map_length, resolution_json,
        resolution_length, request_json, request_length, format, max_findings,
        json_flags, write_fn, user_data);
  ab_project_configuration_free(engine, &configuration);
  return status;
}

ArchbirdStatus archbird_constraints_freeze(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbProjectConfiguration configuration = {0};
  AbConstraintPolicyInput policy;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !owner || !owner_length ||
      !rationale || !rationale_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_project_configuration_decode(engine, config_json, config_length,
                                           &configuration);
  policy = policy_input(&configuration);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_freeze(
        engine, &policy, map_json, map_length, resolution_json,
        resolution_length, request_json, request_length, owner, owner_length,
        rationale, rationale_length, json_flags, write_fn, user_data);
  ab_project_configuration_free(engine, &configuration);
  return status;
}
