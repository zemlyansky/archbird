#ifndef ARCHBIRD_ACT_INTERNAL_H
#define ARCHBIRD_ACT_INTERNAL_H

#include "json_value.h"

typedef struct AbActTransition {
  const AbValue *record;
  uint8_t *after_bytes;
  size_t after_length;
} AbActTransition;

typedef struct AbAct {
  AbValue document;
  const AbValue *source;
  const AbValue *source_locks;
  const AbValue *after;
  const AbValue *acceptance;
  AbActTransition *transitions;
  size_t transition_count;
  char sha256[65];
} AbAct;

ArchbirdStatus ab_act_load(ArchbirdEngine *engine, const uint8_t *json,
                           size_t length, AbAct *out);
void ab_act_free(ArchbirdEngine *engine, AbAct *act);

#endif
