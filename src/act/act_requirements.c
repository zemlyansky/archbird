#include <archbird/archbird.h>

#include "act/act_internal.h"
#include "act/act_source.h"
#include "act/act_submission.h"
#include "base/artifact_validation.h"
#include "base/render_internal.h"
#include "plan/plan_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct PathRequirement {
  const AbString *path;
} PathRequirement;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus reject(ArchbirdEngine *engine, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act source requirements: %s", message);
}

static int path_compare(const void *left, const void *right) {
  const PathRequirement *a = (const PathRequirement *)left;
  const PathRequirement *b = (const PathRequirement *)right;
  return ab_string_compare(a->path, b->path);
}

static ArchbirdStatus add_path(ArchbirdEngine *engine, PathRequirement *paths,
                               size_t *count, const AbValue *value) {
  if (*count >= AB_ACT_MAX_SOURCE_PATHS)
    return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                  "Plan references too many paths");
  paths[(*count)++].path = &value->as.text;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_string_path(ArchbirdEngine *engine,
                                      PathRequirement *paths, size_t *count,
                                      const AbString *value) {
  if (*count >= AB_ACT_MAX_SOURCE_PATHS)
    return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                  "Plan references too many paths");
  paths[(*count)++].path = value;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_observed_path(ArchbirdEngine *engine,
                                        PathRequirement *paths, size_t *count,
                                        const AbValue *value) {
  if (*count >= AB_ACT_MAX_OBSERVED_PATHS)
    return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                  "Act references too many paths");
  paths[(*count)++].path = &value->as.text;
  return ARCHBIRD_OK;
}

static void sort_unique(PathRequirement *paths, size_t *count) {
  size_t input;
  size_t output = 0;
  if (*count > 1)
    qsort(paths, *count, sizeof(*paths), path_compare);
  for (input = 0; input < *count; input++)
    if (!output || !ab_string_equal(paths[output - 1].path, paths[input].path))
      paths[output++] = paths[input];
  *count = output;
}

static void remove_refined(PathRequirement *observe, size_t *observe_count,
                           const PathRequirement *present, size_t present_count,
                           const PathRequirement *absent, size_t absent_count) {
  size_t input;
  size_t output = 0;
  for (input = 0; input < *observe_count; input++)
    if (!bsearch(&observe[input], present, present_count, sizeof(*present),
                 path_compare) &&
        !bsearch(&observe[input], absent, absent_count, sizeof(*absent),
                 path_compare))
      observe[output++] = observe[input];
  *observe_count = output;
}

static ArchbirdStatus collect_item(
    ArchbirdEngine *engine, const AbValue *item, AbActSubmissions *submissions,
    PathRequirement *present, size_t *present_count, PathRequirement *absent,
    size_t *absent_count, PathRequirement *observe, size_t *observe_count,
    AbString *resolved_paths, size_t *resolved_count) {
  const AbValue *operation = field(item, "operation");
  const AbValue *action = field(operation, "action");
  const AbValue *path;
  size_t index;
  if (!field(item, "executable")->as.boolean) {
    AbActSubmission *submission;
    path = ab_act_submission_path(operation);
    submission =
        ab_act_submission_take(submissions, &field(item, "id")->as.text);
    if (path && submission)
      return ab_artifact_text_is(action, "create_file")
                 ? add_path(engine, absent, absent_count, path)
             : ab_artifact_text_is(action, "add_dependency") ||
                     ab_artifact_text_is(action, "remove_dependency")
                 ? add_path(engine, present, present_count, path)
                 : add_path(engine, observe, observe_count, path);
    return reject(engine, ARCHBIRD_POLICY_REJECTED,
                  "Plan contains a manual or blocked item");
  }
  if (ab_artifact_text_is(action, "manual"))
    return reject(engine, ARCHBIRD_POLICY_REJECTED,
                  "Plan contains a manual or blocked item");
  if (ab_artifact_text_is(action, "set_package_entrypoint")) {
    ArchbirdStatus status =
        add_path(engine, present, present_count, field(operation, "path"));
    if (status == ARCHBIRD_OK) {
      if (*resolved_count >= AB_ACT_MAX_TRANSITIONS)
        return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                      "Plan references too many resolved paths");
      status = ab_artifact_resolve_relative_to_file(
          engine, &field(operation, "path")->as.text,
          &field(operation, "target")->as.text,
          &resolved_paths[*resolved_count]);
    }
    if (status == ARCHBIRD_OK) {
      status = add_string_path(engine, present, present_count,
                               &resolved_paths[*resolved_count]);
      (*resolved_count)++;
    }
    return status;
  }
  if (ab_artifact_text_is(action, "move_file")) {
    ArchbirdStatus status = add_path(engine, present, present_count,
                                     field(operation, "source_path"));
    if (status == ARCHBIRD_OK)
      status = add_path(engine, absent, absent_count,
                        field(operation, "destination_path"));
    return status;
  }
  if (ab_artifact_text_is(action, "rename_symbol")) {
    const AbValue *paths = field(operation, "source_paths");
    ArchbirdStatus status = ARCHBIRD_OK;
    for (index = 0; status == ARCHBIRD_OK && index < paths->as.array.count;
         index++) {
      status = add_path(engine, present, present_count,
                        &paths->as.array.items[index]);
    }
    return status;
  }
  if (ab_artifact_text_is(action, "redirect_dependency")) {
    const AbValue *paths = field(operation, "source_paths");
    ArchbirdStatus status = ARCHBIRD_OK;
    for (index = 0; status == ARCHBIRD_OK && index < paths->as.array.count;
         index++)
      status = add_path(engine, present, present_count,
                        &paths->as.array.items[index]);
    return status;
  }
  if (ab_artifact_text_is(action, "declare_symbol")) {
    const AbValue *paths = field(operation, "source_paths");
    ArchbirdStatus status = ARCHBIRD_OK;
    for (index = 0; status == ARCHBIRD_OK && index < paths->as.array.count;
         index++)
      status = add_path(engine, present, present_count,
                        &paths->as.array.items[index]);
    return status;
  }
  if (ab_artifact_text_is(action, "add_provider_capability") ||
      ab_artifact_text_is(action, "remove_provider_capability") ||
      ab_artifact_text_is(action, "rename_provider_capability")) {
    const AbValue *provider = field(operation, "provider");
    const AbValue *paths = field(operation, "source_paths");
    ArchbirdStatus status = ARCHBIRD_OK;
    if (paths) {
      for (index = 0; status == ARCHBIRD_OK && index < paths->as.array.count;
           index++)
        status = add_path(engine, present, present_count,
                          &paths->as.array.items[index]);
      return status;
    }
    path = field(provider, "path");
    if (!path)
      return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                    "Plan provider operator has no source path");
    return add_path(engine, present, present_count, path);
  }
  path = field(operation, "path");
  if (!path)
    return reject(engine, ARCHBIRD_INVALID_SCHEMA,
                  "Plan operator has no source path");
  return add_path(engine, present, present_count, path);
}

static ArchbirdStatus render_paths(AbBuffer *buffer,
                                   const PathRequirement *paths, size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, paths[index].path->data,
                                     paths[index].path->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

ArchbirdStatus archbird_plan_source_requirements(
    ArchbirdEngine *engine, const uint8_t *plan_json, size_t plan_length,
    const uint8_t *executor_submissions_json,
    size_t executor_submissions_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbPlan plan = {0};
  AbActSubmissions submissions = {0};
  PathRequirement present[AB_ACT_MAX_SOURCE_PATHS];
  PathRequirement absent[AB_ACT_MAX_SOURCE_PATHS];
  PathRequirement observe[AB_ACT_MAX_SOURCE_PATHS];
  AbString resolved_paths[AB_ACT_MAX_TRANSITIONS];
  size_t present_count = 0;
  size_t absent_count = 0;
  size_t observe_count = 0;
  size_t resolved_count = 0;
  size_t index;
  AbBuffer document;
  ArchbirdStatus status;
  if (!engine || !plan_json || !plan_length ||
      (!executor_submissions_json && executor_submissions_length) ||
      !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&document, engine);
  memset(resolved_paths, 0, sizeof(resolved_paths));
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_act_submissions_load(engine, executor_submissions_json,
                                     executor_submissions_length, &submissions);
  for (index = 0; status == ARCHBIRD_OK && index < plan.items->as.array.count;
       index++)
    status =
        collect_item(engine, &plan.items->as.array.items[index], &submissions,
                     present, &present_count, absent, &absent_count, observe,
                     &observe_count, resolved_paths, &resolved_count);
  if (status == ARCHBIRD_OK)
    status = ab_act_submissions_require_consumed(engine, &submissions);
  if (status == ARCHBIRD_OK) {
    sort_unique(present, &present_count);
    sort_unique(absent, &absent_count);
    sort_unique(observe, &observe_count);
    for (index = 0; index < present_count; index++)
      if (bsearch(&present[index], absent, absent_count, sizeof(*absent),
                  path_compare)) {
        status = reject(engine, ARCHBIRD_CONFLICT,
                        "one path has incompatible source requirements");
        break;
      }
    if (status == ARCHBIRD_OK)
      remove_refined(observe, &observe_count, present, present_count, absent,
                     absent_count);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "{\"absent\":");
  if (status == ARCHBIRD_OK)
    status = render_paths(&document, absent, absent_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, ",\"files\":");
  if (status == ARCHBIRD_OK)
    status = render_paths(&document, present, present_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, ",\"observe\":");
  if (status == ARCHBIRD_OK)
    status = render_paths(&document, observe, observe_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, document.data, document.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&document);
  ab_act_submissions_free(engine, &submissions);
  ab_plan_free(engine, &plan);
  for (index = 0; index < resolved_count; index++)
    ab_string_free(engine, &resolved_paths[index]);
  return status;
}

ArchbirdStatus archbird_act_source_requirements(
    ArchbirdEngine *engine, const uint8_t *act_json, size_t act_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbAct act = {0};
  PathRequirement paths[AB_ACT_MAX_OBSERVED_PATHS];
  size_t path_count = 0;
  size_t index;
  AbBuffer document;
  ArchbirdStatus status;
  if (!engine || !act_json || !act_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&document, engine);
  status = ab_act_load(engine, act_json, act_length, &act);
  if (status == ARCHBIRD_OK &&
      !ab_artifact_text_is(field(&act.document, "state"), "accepted"))
    status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                    "only an accepted Act can be applied");
  for (index = 0;
       status == ARCHBIRD_OK && index < act.source_locks->as.array.count;
       index++)
    status = add_observed_path(
        engine, paths, &path_count,
        field(&act.source_locks->as.array.items[index], "path"));
  for (index = 0; status == ARCHBIRD_OK && index < act.transition_count;
       index++) {
    const AbValue *transition = act.transitions[index].record;
    const AbValue *kind = field(transition, "kind");
    status = add_observed_path(engine, paths, &path_count,
                               field(transition, "path"));
    if (status == ARCHBIRD_OK && ab_artifact_text_is(kind, "move"))
      status = add_observed_path(engine, paths, &path_count,
                                 field(transition, "source_path"));
  }
  if (status == ARCHBIRD_OK) {
    sort_unique(paths, &path_count);
    status = ab_buffer_literal(&document, "{\"paths\":");
  }
  if (status == ARCHBIRD_OK)
    status = render_paths(&document, paths, path_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, document.data, document.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&document);
  ab_act_free(engine, &act);
  return status;
}
