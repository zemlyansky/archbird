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

static int file_state_matches(const AbValue *metadata, const AbValue *path,
                              const AbValue *state) {
  const AbValue *current = ab_act_source_file(metadata, &path->as.text);
  if (!current)
    return 0;
  return ab_value_equal(field(current, "sha256"), field(state, "sha256")) &&
         ab_value_equal(field(current, "executable"),
                        field(state, "executable"));
}

static int path_is_observed(const AbValue *metadata, const AbValue *path) {
  return ab_act_source_file(metadata, &path->as.text) ||
         ab_act_source_path_absent(metadata, &path->as.text);
}

static int transition_before_matches(const AbValue *metadata,
                                     const AbValue *transition) {
  const AbValue *kind = field(transition, "kind");
  const AbValue *path = field(transition, "path");
  const AbValue *source_path = field(transition, "source_path");
  const AbValue *before = field(transition, "before");
  if (ab_artifact_text_is(kind, "create"))
    return ab_act_source_path_absent(metadata, &path->as.text);
  if (ab_artifact_text_is(kind, "move"))
    return file_state_matches(metadata, source_path, before) &&
           ab_act_source_path_absent(metadata, &path->as.text);
  return file_state_matches(metadata, path, before);
}

static int transition_after_matches(const AbValue *metadata,
                                    const AbValue *transition) {
  const AbValue *kind = field(transition, "kind");
  const AbValue *path = field(transition, "path");
  const AbValue *source_path = field(transition, "source_path");
  const AbValue *after = field(transition, "after");
  if (ab_artifact_text_is(kind, "delete"))
    return ab_act_source_path_absent(metadata, &path->as.text);
  if (ab_artifact_text_is(kind, "move"))
    return ab_act_source_path_absent(metadata, &source_path->as.text) &&
           file_state_matches(metadata, path, after);
  return file_state_matches(metadata, path, after);
}

ArchbirdStatus archbird_act_preflight_apply(ArchbirdEngine *engine,
                                            const uint8_t *act_json,
                                            size_t act_length,
                                            const uint8_t *source_metadata_json,
                                            size_t source_metadata_length,
                                            ArchbirdActApplyState *out_state) {
  AbAct act = {0};
  AbValue metadata = {0};
  size_t index;
  int before_all = 1;
  int after_all = 1;
  ArchbirdStatus status;
  if (!engine || !act_json || !act_length || !source_metadata_json ||
      !source_metadata_length || !out_state)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out_state = ARCHBIRD_ACT_APPLY_READY;
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
    int before_matches;
    int after_matches;
    if (!path_is_observed(&metadata, path) ||
        (ab_artifact_text_is(kind, "move") &&
         !path_is_observed(&metadata, source_path))) {
      status = reject_path(engine, ARCHBIRD_CONFLICT,
                           "required source state was not observed", path);
      continue;
    }
    before_matches = transition_before_matches(&metadata, transition);
    after_matches = transition_after_matches(&metadata, transition);
    if (!before_matches && !after_matches) {
      status = reject_path(engine, ARCHBIRD_CONFLICT,
                           "source state matches neither the Act before-state "
                           "nor after-state",
                           path);
      continue;
    }
    before_all = before_all && before_matches;
    after_all = after_all && after_matches;
  }
  if (status == ARCHBIRD_OK) {
    if (after_all)
      *out_state = ARCHBIRD_ACT_APPLY_ALREADY_SATISFIED;
    else if (before_all)
      *out_state = ARCHBIRD_ACT_APPLY_READY;
    else
      status =
          reject(engine, ARCHBIRD_CONFLICT, "Act is only partially applied");
  }
  ab_value_free(engine, &metadata);
  ab_act_free(engine, &act);
  return status;
}
