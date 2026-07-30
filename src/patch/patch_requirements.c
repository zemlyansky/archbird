#include <archbird/archbird.h>

#include "artifact_validation.h"
#include "patch_internal.h"
#include "patch_source.h"
#include "plan_internal.h"
#include "render_internal.h"

#include <stdlib.h>

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
  if (*count >= AB_PATCH_MAX_TRANSITIONS)
    return reject(engine, ARCHBIRD_LIMIT_EXCEEDED,
                  "Plan references too many paths");
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

static ArchbirdStatus collect_item(ArchbirdEngine *engine, const AbValue *item,
                                   PathRequirement *present,
                                   size_t *present_count,
                                   PathRequirement *absent,
                                   size_t *absent_count) {
  const AbValue *operation = field(item, "operation");
  const AbValue *action = field(operation, "action");
  const AbValue *path;
  size_t index;
  if (!field(item, "executable")->as.boolean ||
      ab_artifact_text_is(action, "manual"))
    return reject(engine, ARCHBIRD_POLICY_REJECTED,
                  "Plan contains a manual or blocked item");
  if (ab_artifact_text_is(action, "create_file"))
    return add_path(engine, absent, absent_count, field(operation, "path"));
  if (ab_artifact_text_is(action, "move_file")) {
    ArchbirdStatus status = add_path(engine, present, present_count,
                                     field(operation, "source_path"));
    if (status == ARCHBIRD_OK)
      status = add_path(engine, absent, absent_count,
                        field(operation, "destination_path"));
    return status;
  }
  if (ab_artifact_text_is(action, "rename_symbol")) {
    const AbValue *sites = field(operation, "sites");
    ArchbirdStatus status = ARCHBIRD_OK;
    for (index = 0; status == ARCHBIRD_OK && index < sites->as.array.count;
         index++) {
      path = field(&sites->as.array.items[index], "path");
      status = add_path(engine, present, present_count, path);
    }
    return status;
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

ArchbirdStatus archbird_act_source_requirements(
    ArchbirdEngine *engine, const uint8_t *plan_json, size_t plan_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbPlan plan = {0};
  PathRequirement present[AB_PATCH_MAX_TRANSITIONS];
  PathRequirement absent[AB_PATCH_MAX_TRANSITIONS];
  size_t present_count = 0;
  size_t absent_count = 0;
  size_t index;
  AbBuffer document;
  ArchbirdStatus status;
  if (!engine || !plan_json || !plan_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&document, engine);
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  for (index = 0; status == ARCHBIRD_OK && index < plan.items->as.array.count;
       index++)
    status = collect_item(engine, &plan.items->as.array.items[index], present,
                          &present_count, absent, &absent_count);
  if (status == ARCHBIRD_OK) {
    sort_unique(present, &present_count);
    sort_unique(absent, &absent_count);
    for (index = 0; index < present_count; index++)
      if (bsearch(&present[index], absent, absent_count, sizeof(*absent),
                  path_compare)) {
        status = reject(engine, ARCHBIRD_CONFLICT,
                        "one path is required present and absent");
        break;
      }
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
    status = ab_buffer_literal(&document, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, document.data, document.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&document);
  ab_plan_free(engine, &plan);
  return status;
}

ArchbirdStatus archbird_patch_source_requirements(
    ArchbirdEngine *engine, const uint8_t *patch_json, size_t patch_length,
    uint32_t json_flags, ArchbirdWriteFn write_fn, void *user_data) {
  AbPatch patch = {0};
  PathRequirement present[AB_PATCH_MAX_TRANSITIONS];
  PathRequirement absent[AB_PATCH_MAX_TRANSITIONS];
  size_t present_count = 0;
  size_t absent_count = 0;
  size_t index;
  AbBuffer document;
  ArchbirdStatus status;
  if (!engine || !patch_json || !patch_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_buffer_init(&document, engine);
  status = ab_patch_load(engine, patch_json, patch_length, &patch);
  if (status == ARCHBIRD_OK &&
      !ab_artifact_text_is(field(&patch.document, "state"), "accepted"))
    status = reject(engine, ARCHBIRD_POLICY_REJECTED,
                    "only an accepted Patch can be applied");
  for (index = 0; status == ARCHBIRD_OK && index < patch.transition_count;
       index++) {
    const AbValue *transition = patch.transitions[index].record;
    const AbValue *kind = field(transition, "kind");
    if (ab_artifact_text_is(kind, "create")) {
      status =
          add_path(engine, absent, &absent_count, field(transition, "path"));
    } else if (ab_artifact_text_is(kind, "move")) {
      status = add_path(engine, present, &present_count,
                        field(transition, "source_path"));
      if (status == ARCHBIRD_OK)
        status =
            add_path(engine, absent, &absent_count, field(transition, "path"));
    } else {
      status =
          add_path(engine, present, &present_count, field(transition, "path"));
    }
  }
  if (status == ARCHBIRD_OK) {
    sort_unique(present, &present_count);
    sort_unique(absent, &absent_count);
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
    status = ab_buffer_literal(&document, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, document.data, document.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&document);
  ab_patch_free(engine, &patch);
  return status;
}
