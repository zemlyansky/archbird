#include "verify_runtime.h"

#include "sha256.h"

#include <string.h>

static const AbValue *suite_project(const AbVerificationContext *context,
                                    const AbString *name) {
  size_t low = 0;
  size_t high = context->suite.projects->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbObjectField *field =
        &context->suite.projects->as.object.fields[middle];
    int compared = ab_string_compare(&field->name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &field->value;
  }
  return NULL;
}

int ab_verify_source_lock_observed_sha256(const AbVerificationContext *context,
                                          const AbString *project_name,
                                          const AbString *path,
                                          char output[65]) {
  const AbValue *project =
      ab_verify_input_project(&context->input, project_name);
  const AbValue *source =
      project ? ab_verify_input_source(project, path) : NULL;
  const AbValue *text = source ? ab_value_member(source, "text") : NULL;
  const AbValue *sha = source ? ab_value_member(source, "sha256") : NULL;
  if (text && text->kind == AB_VALUE_STRING) {
    uint8_t digest[32];
    if (archbird_sha256((const uint8_t *)text->as.text.data,
                        text->as.text.length, digest) != ARCHBIRD_OK)
      return 0;
    archbird_sha256_hex(digest, output);
    return 1;
  }
  if (sha && sha->kind == AB_VALUE_STRING && sha->as.text.length == 64) {
    memcpy(output, sha->as.text.data, 64);
    output[64] = '\0';
    return 1;
  }
  output[0] = '\0';
  return 0;
}

AbVerifySourceLockState
ab_verify_source_lock_state(const AbVerificationContext *context,
                            const AbString *project_name) {
  const AbValue *project = suite_project(context, project_name);
  const AbValue *lock =
      project ? ab_value_member(project, "source_lock") : NULL;
  AbVerifySourceLockState state = AB_VERIFY_SOURCE_LOCK_CURRENT;
  size_t index;
  if (!lock)
    return AB_VERIFY_SOURCE_LOCK_NOT_DECLARED;
  for (index = 0; index < lock->as.object.count; index++) {
    const AbObjectField *field = &lock->as.object.fields[index];
    char observed[65];
    if (!ab_verify_source_lock_observed_sha256(context, project_name,
                                               &field->name, observed)) {
      if (state != AB_VERIFY_SOURCE_LOCK_MISMATCH)
        state = AB_VERIFY_SOURCE_LOCK_UNAVAILABLE;
    } else if (memcmp(observed, field->value.as.text.data, 64) != 0) {
      state = AB_VERIFY_SOURCE_LOCK_MISMATCH;
    }
  }
  return state;
}

const char *ab_verify_source_lock_state_name(AbVerifySourceLockState state) {
  switch (state) {
  case AB_VERIFY_SOURCE_LOCK_CURRENT:
    return "current";
  case AB_VERIFY_SOURCE_LOCK_MISMATCH:
    return "mismatch";
  case AB_VERIFY_SOURCE_LOCK_UNAVAILABLE:
    return "unavailable";
  case AB_VERIFY_SOURCE_LOCK_NOT_DECLARED:
  default:
    return "not_declared";
  }
}
