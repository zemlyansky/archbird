#ifndef ARCHBIRD_ARTIFACT_VALIDATION_H
#define ARCHBIRD_ARTIFACT_VALIDATION_H

#include "json_value.h"

#include <stdint.h>

#define AB_ARTIFACT_MAX_SAFE_INTEGER UINT64_C(9007199254740991)

int ab_artifact_text_is(const AbValue *value, const char *literal);
int ab_artifact_bounded_text(const AbValue *value, size_t maximum,
                             int nonempty);
int ab_artifact_sha256(const AbValue *value);
int ab_artifact_stable_id(const AbValue *value);
int ab_artifact_repository_path(const AbValue *value);
int ab_artifact_repository_literal_path(const AbValue *value);
int ab_artifact_safe_integer(const AbValue *value, uint64_t *out);
int ab_artifact_boolean(const AbValue *value);
int ab_artifact_object_exact(const AbValue *value, const char *const *fields,
                             size_t count);
ArchbirdStatus ab_artifact_json_sha256(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       char out[65]);

#endif
