#include <archbird/archbird.h>

#include "act_internal.h"
#include "act_source.h"
#include "artifact_validation.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(ArchbirdEngine *engine, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act apply preflight: %s", message);
}

static ArchbirdStatus reject_path(ArchbirdEngine *engine, ArchbirdStatus status,
                                  const char *message, const AbValue *path) {
  return archbird_error_set(
      engine, status, ARCHBIRD_NO_OFFSET, "act apply preflight: %s: %.*s",
      message, (int)path->as.text.length, (const char *)path->as.text.data);
}

static ArchbirdStatus require_before_state(ArchbirdEngine *engine,
                                           const AbValue *metadata,
                                           const AbValue *path,
                                           const AbValue *before) {
  const AbValue *current = ab_act_source_file(metadata, &path->as.text);
  if (!current)
    return reject_path(engine, ARCHBIRD_CONFLICT, "source path is missing",
                       path);
  if (!ab_value_equal(field(current, "sha256"), field(before, "sha256")) ||
      !ab_value_equal(field(current, "executable"),
                      field(before, "executable")))
    return reject_path(engine, ARCHBIRD_CONFLICT, "source preimage has drifted",
                       path);
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_act_preflight_apply(ArchbirdEngine *engine,
                                            const uint8_t *act_json,
                                            size_t act_length,
                                            const uint8_t *source_metadata_json,
                                            size_t source_metadata_length) {
  AbAct act = {0};
  AbValue metadata = {0};
  size_t index;
  ArchbirdStatus status;
  if (!engine || !act_json || !act_length || !source_metadata_json ||
      !source_metadata_length)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_act_load(engine, act_json, act_length, &act);
  if (status == ARCHBIRD_OK &&
      !ab_artifact_text_is(field(&act.document, "state"), "accepted"))
    status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                    "only an accepted Act can be applied");
  if (status == ARCHBIRD_OK)
    status = ab_act_source_metadata_load(engine, source_metadata_json,
                                         source_metadata_length, &metadata);
  for (index = 0; status == ARCHBIRD_OK && index < act.transition_count;
       index++) {
    const AbValue *transition = act.transitions[index].record;
    const AbValue *kind = field(transition, "kind");
    const AbValue *path = field(transition, "path");
    const AbValue *source_path = field(transition, "source_path");
    const AbValue *before = field(transition, "before");
    if (ab_artifact_text_is(kind, "create")) {
      if (!ab_act_source_path_absent(&metadata, &path->as.text))
        status = reject_path(engine, ARCHBIRD_CONFLICT,
                             "create destination is no longer absent", path);
    } else if (ab_artifact_text_is(kind, "move")) {
      status = require_before_state(engine, &metadata, source_path, before);
      if (status == ARCHBIRD_OK &&
          !ab_act_source_path_absent(&metadata, &path->as.text))
        status = reject_path(engine, ARCHBIRD_CONFLICT,
                             "move destination is no longer absent", path);
    } else {
      status = require_before_state(engine, &metadata, path, before);
    }
  }
  ab_value_free(engine, &metadata);
  ab_act_free(engine, &act);
  return status;
}
