#ifndef ARCHBIRD_CONSTRAINTS_INTERNAL_H
#define ARCHBIRD_CONSTRAINTS_INTERNAL_H

#include "archbird_internal.h"
#include "json_value.h"

typedef struct AbConstraintPolicyInput {
  const AbValue *project;
  const AbValue *description;
  const AbValue *projections;
  const AbValue *constraints;
  const char *constraint_policy_sha256;
  const char *map_config_sha256;
  const char *project_configuration_sha256;
} AbConstraintPolicyInput;

ArchbirdStatus ab_constraints_evaluate(
    ArchbirdEngine *engine, const AbConstraintPolicyInput *policy,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

ArchbirdStatus ab_constraints_report(
    ArchbirdEngine *engine, const AbConstraintPolicyInput *policy,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, ArchbirdVerificationFormat format,
    size_t max_findings, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data);

ArchbirdStatus ab_constraints_freeze(
    ArchbirdEngine *engine, const AbConstraintPolicyInput *policy,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data);

#endif
