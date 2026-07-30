#ifndef ARCHBIRD_PATCH_INTERNAL_H
#define ARCHBIRD_PATCH_INTERNAL_H

#include "json_value.h"

typedef struct AbPatchTransition {
  const AbValue *record;
  uint8_t *after_bytes;
  size_t after_length;
} AbPatchTransition;

typedef struct AbPatch {
  AbValue document;
  const AbValue *source;
  const AbValue *after;
  const AbValue *acceptance;
  AbPatchTransition *transitions;
  size_t transition_count;
  char sha256[65];
} AbPatch;

ArchbirdStatus ab_patch_load(ArchbirdEngine *engine, const uint8_t *json,
                             size_t length, AbPatch *out);
void ab_patch_free(ArchbirdEngine *engine, AbPatch *patch);

#endif
