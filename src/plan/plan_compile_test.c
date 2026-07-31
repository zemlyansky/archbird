#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <stdio.h>
#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int exact_path(const AbValue *definition, const AbValue **out) {
  const AbValue *paths = field(definition, "target_paths");
  if (!paths || paths->kind != AB_VALUE_ARRAY || paths->as.array.count != 1 ||
      !ab_artifact_repository_path(&paths->as.array.items[0]))
    return 0;
  *out = &paths->as.array.items[0];
  return 1;
}

static const AbValue *unique_test_path(const AbValue *map,
                                       const AbValue *group) {
  const AbValue *tests = field(map, "tests");
  const AbValue *candidate = NULL;
  size_t index;
  if (!tests || tests->kind != AB_VALUE_ARRAY || !group ||
      group->kind != AB_VALUE_STRING)
    return NULL;
  for (index = 0; index < tests->as.array.count; index++) {
    const AbValue *test = &tests->as.array.items[index];
    const AbValue *test_group = field(test, "group");
    const AbValue *generated = field(test, "generated");
    const AbValue *path = field(test, "path");
    if (!test_group || test_group->kind != AB_VALUE_STRING ||
        !ab_string_equal(&test_group->as.text, &group->as.text))
      continue;
    if (!ab_artifact_repository_path(path))
      return NULL;
    if (!generated || generated->kind != AB_VALUE_BOOL || generated->as.boolean)
      return NULL;
    if (candidate && !ab_value_equal(candidate, path))
      return NULL;
    candidate = path;
  }
  return candidate;
}

static ArchbirdStatus render_string_array(AbBuffer *out,
                                          const AbValue *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(out, "[");
  for (index = 0;
       status == ARCHBIRD_OK && values && values->kind == AB_VALUE_ARRAY &&
       index < values->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(out, &values->as.array.items[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "]");
  return status;
}

ArchbirdStatus ab_plan_compile_test_constraint(
    ArchbirdEngine *engine, const AbValue *map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition, int *out_handled) {
  const AbValue *findings = field(constraint, "findings");
  const AbValue *group = field(definition, "group");
  const AbValue *selectors = field(definition, "selectors");
  const AbValue *target = NULL;
  const AbValue *test_path;
  AbPlanFindingGroups groups = {0};
  size_t group_index;
  ArchbirdStatus status;
  *out_handled = 0;
  if (!definition ||
      !ab_artifact_text_is(field(definition, "select"), "test_routes") ||
      !findings || findings->kind != AB_VALUE_ARRAY ||
      !findings->as.array.count || !exact_path(definition, &target) ||
      !ab_artifact_bounded_text(group, 65536u, 1))
    return ARCHBIRD_OK;
  status = ab_plan_finding_groups_collect(engine, findings, &groups);
  test_path = unique_test_path(map, group);
  for (group_index = 0; status == ARCHBIRD_OK && group_index < groups.count;
       group_index++) {
    const AbPlanFindingGroup *finding_group = &groups.groups[group_index];
    const AbValue *finding = finding_group->representative;
    const char *reason =
        "The required test route is exact, but its test implementation or "
        "registration is not derivable from architecture evidence.";
    const char *reasons[] = {reason};
    AbBuffer operation;
    AbPlanItemSpec spec;
    char statement[1024];
    int length;
    if (!ab_plan_finding_current(finding)) {
      reason = "Finding evidence is not current executable evidence.";
      reasons[0] = reason;
    }
    ab_buffer_init(&operation, engine);
    status = ab_buffer_literal(&operation,
                               "{\"action\":\"add_test_route\",\"group\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, group);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, ",\"selectors\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(&operation, selectors);
    if (status == ARCHBIRD_OK && test_path)
      status = ab_buffer_literal(&operation, ",\"path\":");
    if (status == ARCHBIRD_OK && test_path)
      status = ab_value_render(&operation, test_path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, target);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, "}");
    length = snprintf(statement, sizeof(statement),
                      "Add a %.*s test route for %.*s.",
                      (int)group->as.text.length, group->as.text.data,
                      (int)target->as.text.length, target->as.text.data);
    if (status == ARCHBIRD_OK &&
        (length < 0 || (size_t)length >= sizeof(statement)))
      status = archbird_error_set(
          engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
          "plan compilation: generated test-route statement is too long");
    memset(&spec, 0, sizeof(spec));
    spec.constraint = constraint;
    spec.findings = finding_group->rows;
    spec.finding_count = finding_group->count;
    spec.statement = statement;
    spec.provenance = "derived";
    spec.operation = &operation;
    spec.executable = 0;
    spec.reasons = reasons;
    spec.reason_count = 1;
    if (status == ARCHBIRD_OK)
      status = ab_plan_item_builder_append(builder, &spec);
    ab_buffer_free(&operation);
  }
  if (status == ARCHBIRD_OK)
    *out_handled = 1;
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
