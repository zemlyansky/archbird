#include "act/dependency_redirect.h"

#include "act/act_source.h"
#include "act/dependency_redirect_internal.h"
#include "base/artifact_validation.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbActRedirectPath {
  const AbString *value;
} AbActRedirectPath;

enum {
  AB_ACT_REDIRECT_LANGUAGE_NONE = 0u,
  AB_ACT_REDIRECT_LANGUAGE_C = 1u,
  AB_ACT_REDIRECT_LANGUAGE_PYTHON = 2u,
  AB_ACT_REDIRECT_LANGUAGE_ECMASCRIPT = 3u,
};

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static const AbValue *attribute(const AbProjectionItem *item,
                                const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         memcmp(value->as.text.data, literal, length) == 0;
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         memcmp(value->data, literal, length) == 0;
}

static unsigned redirect_language(const AbValue *value) {
  if (text_is(value, "c"))
    return AB_ACT_REDIRECT_LANGUAGE_C;
  if (text_is(value, "python"))
    return AB_ACT_REDIRECT_LANGUAGE_PYTHON;
  if (text_is(value, "javascript") || text_is(value, "typescript") ||
      text_is(value, "tsx"))
    return AB_ACT_REDIRECT_LANGUAGE_ECMASCRIPT;
  return AB_ACT_REDIRECT_LANGUAGE_NONE;
}

static ArchbirdStatus reject(AbActContext *context, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(ab_act_executor_engine(context), status,
                            ARCHBIRD_NO_OFFSET, "act dependency redirect: %s",
                            message);
}

static int projection_complete(const AbProjectionData *data) {
  return data && string_is(&data->state, "current") &&
         strcmp(ab_projection_data_classification(data), "complete") == 0 &&
         data->selection.has_truncated && !data->selection.truncated &&
         (!data->selection.has_unknown || !data->selection.unknown) &&
         (!data->selection.has_unsupported || !data->selection.unsupported);
}

static const AbProjectionItem *selected_relation(const AbProjectionData *data,
                                                 const AbString *label) {
  const AbProjectionItem *match = NULL;
  size_t index;
  for (index = 0; data && index < data->item_count; index++)
    if (ab_string_equal(&data->items[index].label, label)) {
      if (match)
        return NULL;
      match = &data->items[index];
    }
  return match;
}

static const AbValue *map_file(const AbValue *map, const AbString *path) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t index;
  for (index = 0;
       files && files->kind == AB_VALUE_ARRAY && index < files->as.array.count;
       index++) {
    const AbValue *candidate = &files->as.array.items[index];
    const AbValue *candidate_path = field(candidate, "path");
    if (!candidate_path || candidate_path->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate_path->as.text, path))
      continue;
    if (match)
      return NULL;
    match = candidate;
  }
  return match;
}

static int path_compare(const void *left, const void *right) {
  const AbActRedirectPath *a = (const AbActRedirectPath *)left;
  const AbActRedirectPath *b = (const AbActRedirectPath *)right;
  return ab_string_compare(a->value, b->value);
}

static ArchbirdStatus validate_source_scope(AbActContext *context,
                                            const AbValue *map,
                                            const AbValue *relation_sites,
                                            const AbValue *expected_paths,
                                            unsigned *out_language) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  AbActRedirectPath *paths = NULL;
  size_t path_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_language = AB_ACT_REDIRECT_LANGUAGE_NONE;
  if (!relation_sites || relation_sites->kind != AB_VALUE_ARRAY ||
      relation_sites->as.array.count > AB_ACT_MAX_TRANSITIONS)
    return reject(context, ARCHBIRD_LIMIT_EXCEEDED,
                  "the relation source scope exceeds the Act edit limit");
  if (relation_sites->as.array.count) {
    paths = (AbActRedirectPath *)ab_calloc(
        engine, relation_sites->as.array.count, sizeof(*paths));
    if (!paths)
      return reject(context, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory validating relation source scope");
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < relation_sites->as.array.count;
       index++) {
    const AbValue *path = field(&relation_sites->as.array.items[index], "path");
    const AbValue *file;
    const AbValue *language;
    unsigned family;
    if (!ab_artifact_repository_path(path)) {
      status = reject(context, ARCHBIRD_POLICY_REJECTED,
                      "the relation contains an invalid source path");
      break;
    }
    file = map_file(map, &path->as.text);
    language = file ? field(file, "language") : NULL;
    family = redirect_language(language);
    if (!file || !family) {
      status = reject(context, ARCHBIRD_CONFLICT,
                      "a relation source has no supported mapped language");
      break;
    }
    if (*out_language && *out_language != family) {
      status = reject(context, ARCHBIRD_POLICY_REJECTED,
                      "one redirect cannot mix executor language families");
      break;
    }
    *out_language = family;
    paths[path_count++].value = &path->as.text;
  }
  if (status == ARCHBIRD_OK && path_count > 1)
    qsort(paths, path_count, sizeof(*paths), path_compare);
  if (status == ARCHBIRD_OK && path_count > 1) {
    size_t output = 1;
    for (index = 1; index < path_count; index++)
      if (!ab_string_equal(paths[output - 1].value, paths[index].value))
        paths[output++] = paths[index];
    path_count = output;
  }
  if (status == ARCHBIRD_OK &&
      (!expected_paths || expected_paths->kind != AB_VALUE_ARRAY ||
       expected_paths->as.array.count != path_count))
    status = reject(context, ARCHBIRD_CONFLICT,
                    "the Plan source scope differs from the current relation");
  for (index = 0; status == ARCHBIRD_OK && index < path_count; index++) {
    size_t expected_index;
    int found = 0;
    for (expected_index = 0; expected_index < expected_paths->as.array.count;
         expected_index++) {
      const AbValue *expected = &expected_paths->as.array.items[expected_index];
      if (expected->kind == AB_VALUE_STRING &&
          ab_string_equal(&expected->as.text, paths[index].value)) {
        found = 1;
        break;
      }
    }
    if (!found)
      status =
          reject(context, ARCHBIRD_CONFLICT,
                 "the Plan source scope differs from the current relation");
  }
  if (status == ARCHBIRD_OK && !path_count)
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "the dependency relation has no exact source paths");
  ab_free(engine, paths);
  return status;
}

static int file_in_component(const AbProjectionMembershipIndex *membership,
                             const AbString *path, const AbString *component) {
  const AbProjectionMembershipFile *file =
      ab_projection_membership_file(membership, path);
  size_t offset;
  if (!file || !file->assignment_count)
    return 0;
  for (offset = 0; offset < file->assignment_count; offset++) {
    const AbProjectionMembershipAssignment *assignment =
        &membership->assignments[file->assignment_start + offset];
    if (ab_string_equal(
            membership->components[assignment->component_index].name,
            component))
      return 1;
  }
  return 0;
}

int ab_act_dependency_redirect_target_matches(
    const AbActDependencyRedirect *redirect, const AbString *candidate_path) {
  if (text_is(field(redirect->projection, "select"), "file_edges"))
    return ab_string_equal(candidate_path, redirect->relation_target);
  return file_in_component(&redirect->membership, candidate_path,
                           redirect->relation_target);
}

ArchbirdStatus ab_act_dependency_redirect(AbActContext *context,
                                          const AbValue *operation,
                                          const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *map = ab_act_executor_map(context);
  const AbValue *definition = field(operation, "projection");
  const AbValue *projection_id = field(operation, "projection_id");
  const AbValue *expected_sha = field(operation, "projection_content_sha256");
  const AbValue *relation_label = field(operation, "relation");
  const AbValue *source_paths = field(operation, "source_paths");
  unsigned language = AB_ACT_REDIRECT_LANGUAGE_NONE;
  AbProjectionPlan plan = {0};
  AbProjectionResult result = {0};
  AbActDependencyRedirect redirect = {0};
  ArchbirdStatus status;
  redirect.map = map;
  redirect.projection = definition;
  redirect.from_symbol = &field(operation, "from_symbol")->as.text;
  redirect.to_symbol = &field(operation, "to_symbol")->as.text;
  status = ab_projection_plan_compile(engine, definition,
                                      &projection_id->as.text, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(engine, &plan, map, NULL, &result);
  if (status == ARCHBIRD_OK &&
      memcmp(result.data.sha256, expected_sha->as.text.data, 64) != 0)
    status = reject(context, ARCHBIRD_CONFLICT,
                    "edge projection content differs from the Plan");
  if (status == ARCHBIRD_OK && !projection_complete(&result.data))
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "edge projection is not complete, current, and exhaustive");
  if (status == ARCHBIRD_OK) {
    redirect.relation =
        selected_relation(&result.data, &relation_label->as.text);
    if (!redirect.relation || !string_is(&redirect.relation->state, "current"))
      status = reject(context, ARCHBIRD_POLICY_REJECTED,
                      "Plan relation is absent, ambiguous, or not current");
  }
  redirect.relation_sites = attribute(redirect.relation, "sites");
  {
    const AbValue *target = attribute(redirect.relation, "target");
    if (status == ARCHBIRD_OK &&
        (!redirect.relation_sites ||
         redirect.relation_sites->kind != AB_VALUE_ARRAY || !target ||
         target->kind != AB_VALUE_STRING))
      status = reject(context, ARCHBIRD_POLICY_REJECTED,
                      "edge projection lacks typed source sites or target");
    if (target && target->kind == AB_VALUE_STRING)
      redirect.relation_target = &target->as.text;
  }
  if (status == ARCHBIRD_OK &&
      text_is(field(definition, "select"), "component_edges"))
    status =
        ab_projection_membership_index_build(engine, map, &redirect.membership);
  if (status == ARCHBIRD_OK &&
      text_is(field(definition, "select"), "component_edges") &&
      !redirect.membership.current)
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "component membership is not current");
  if (status == ARCHBIRD_OK)
    status = validate_source_scope(context, map, redirect.relation_sites,
                                   source_paths, &language);
  if (status == ARCHBIRD_OK && language == AB_ACT_REDIRECT_LANGUAGE_C)
    status = ab_act_c_dependency_redirect(context, &redirect, item_id);
  else if (status == ARCHBIRD_OK && language == AB_ACT_REDIRECT_LANGUAGE_PYTHON)
    status = ab_act_python_dependency_redirect(context, &redirect, item_id);
  else if (status == ARCHBIRD_OK &&
           language == AB_ACT_REDIRECT_LANGUAGE_ECMASCRIPT)
    status = ab_act_ecmascript_dependency_redirect(context, &redirect, item_id);
  else if (status == ARCHBIRD_OK)
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "no language executor supports the mapped source language");
  ab_projection_membership_index_free(engine, &redirect.membership);
  ab_projection_result_free(engine, &result);
  ab_projection_plan_free(engine, &plan);
  return status;
}
