#include <archbird/archbird.h>

#include "patch_internal.h"

ArchbirdStatus archbird_patch_validate(ArchbirdEngine *engine,
                                       const uint8_t *patch_json,
                                       size_t patch_length) {
  AbPatch patch;
  ArchbirdStatus status;
  if (!engine || !patch_json || !patch_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_patch_load(engine, patch_json, patch_length, &patch);
  if (status == ARCHBIRD_OK)
    ab_patch_free(engine, &patch);
  return status;
}
