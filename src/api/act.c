#include <archbird/archbird.h>

#include "act/act_internal.h"

ArchbirdStatus archbird_act_validate(ArchbirdEngine *engine,
                                     const uint8_t *act_json,
                                     size_t act_length) {
  AbAct act;
  ArchbirdStatus status;
  if (!engine || !act_json || !act_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_act_load(engine, act_json, act_length, &act);
  if (status == ARCHBIRD_OK)
    ab_act_free(engine, &act);
  return status;
}
