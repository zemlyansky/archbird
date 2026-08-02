#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <stdio.h>
#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_equal(const AbValue *value, const AbString *text) {
  return value && value->kind == AB_VALUE_STRING &&
         ab_string_equal(&value->as.text, text);
}

static int exact_single_string(const AbValue *object, const char *name,
                               const AbValue **out) {
  static const char pattern_bytes[] = "*?[]{}";
  const AbValue *values = field(object, name);
  const AbValue *value;
  size_t index;
  if (!values || values->kind != AB_VALUE_ARRAY ||
      values->as.array.count != 1 ||
      values->as.array.items[0].kind != AB_VALUE_STRING ||
      !values->as.array.items[0].as.text.length)
    return 0;
  value = &values->as.array.items[0];
  for (index = 0; index < value->as.text.length; index++)
    if (memchr(pattern_bytes, value->as.text.data[index],
               sizeof(pattern_bytes) - 1))
      return 0;
  *out = value;
  return 1;
}

static int package_matches(const AbValue *package, const AbString *name) {
  const AbValue *aliases = field(package, "aliases");
  size_t index;
  if (text_equal(field(package, "name"), name) ||
      text_equal(field(package, "identity"), name))
    return 1;
  if (!aliases || aliases->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < aliases->as.array.count; index++)
    if (text_equal(&aliases->as.array.items[index], name))
      return 1;
  return 0;
}

static const AbValue *unique_npm_package(const AbValue *map,
                                         const AbString *name) {
  const AbValue *packages = field(map, "packages");
  const AbValue *matched = NULL;
  size_t index;
  size_t count = 0;
  if (!packages || packages->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *candidate = &packages->as.array.items[index];
    const AbValue *kind = field(candidate, "kind");
    const AbValue *manifest = field(candidate, "manifest");
    if (!package_matches(candidate, name))
      continue;
    count++;
    if (ab_artifact_text_is(kind, "npm") &&
        ab_artifact_repository_path(manifest))
      matched = candidate;
    else
      matched = NULL;
  }
  return count == 1 ? matched : NULL;
}

static int entrypoint_has(const AbValue *entrypoints, const AbString *route) {
  size_t index;
  if (!entrypoints || entrypoints->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < entrypoints->as.object.count; index++)
    if (ab_string_equal(&entrypoints->as.object.fields[index].name, route))
      return 1;
  return 0;
}

static int entrypoint_has_prefix(const AbValue *entrypoints, const char *prefix,
                                 size_t prefix_length) {
  size_t index;
  if (!entrypoints || entrypoints->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < entrypoints->as.object.count; index++) {
    const AbString *name = &entrypoints->as.object.fields[index].name;
    if (name->length >= prefix_length &&
        memcmp(name->data, prefix, prefix_length) == 0)
      return 1;
  }
  return 0;
}

static int entrypoint_has_child(const AbValue *entrypoints,
                                const AbString *route) {
  size_t index;
  if (!entrypoints || entrypoints->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < entrypoints->as.object.count; index++) {
    const AbString *name = &entrypoints->as.object.fields[index].name;
    if (name->length > route->length &&
        memcmp(name->data, route->data, route->length) == 0 &&
        name->data[route->length] == '/')
      return 1;
  }
  return 0;
}

static int supported_route(const AbValue *package, const AbString *route) {
  static const char bin_prefix[] = "bin:";
  static const char exports_prefix[] = "exports:";
  const AbValue *entrypoints = field(package, "entrypoints");
  if (route->length == 4 && memcmp(route->data, "main", 4) == 0)
    return 1;
  if ((route->length == 3 && memcmp(route->data, "bin", 3) == 0) ||
      (route->length == 15 && memcmp(route->data, "exports:default", 15) == 0))
    return entrypoint_has(entrypoints, route);
  if (route->length > sizeof(bin_prefix) - 1 &&
      memcmp(route->data, bin_prefix, sizeof(bin_prefix) - 1) == 0)
    return memchr(route->data + sizeof(bin_prefix) - 1, '/',
                  route->length - (sizeof(bin_prefix) - 1)) == NULL &&
           entrypoint_has_prefix(entrypoints, bin_prefix,
                                 sizeof(bin_prefix) - 1);
  if (route->length > sizeof(exports_prefix) - 1 &&
      memcmp(route->data, exports_prefix, sizeof(exports_prefix) - 1) == 0) {
    const char *name = route->data + sizeof(exports_prefix) - 1;
    size_t length = route->length - (sizeof(exports_prefix) - 1);
    int direct = (length == 1 && name[0] == '.') ||
                 (length > 2 && name[0] == '.' && name[1] == '/' &&
                  memchr(name + 2, '/', length - 2) == NULL);
    return direct &&
           entrypoint_has_prefix(entrypoints, exports_prefix,
                                 sizeof(exports_prefix) - 1) &&
           !entrypoint_has_child(entrypoints, route);
  }
  return 0;
}

static ArchbirdStatus append_package_item(
    ArchbirdEngine *engine, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbPlanFindingGroups *groups,
    const AbValue *package_name, const AbValue *manifest, const AbValue *route,
    const AbValue *target, const char *reason) {
  AbBuffer operation;
  char statement[1024];
  int length;
  ArchbirdStatus status;
  const char *reasons[] = {reason};
  size_t finding_count = 0;
  size_t group_index;
  for (group_index = 0; group_index < groups->count; group_index++)
    finding_count += groups->groups[group_index].count;
  ab_buffer_init(&operation, engine);
  if (reason) {
    status = ab_buffer_literal(&operation,
                               "{\"action\":\"manual\",\"candidate_paths\":[");
    if (status == ARCHBIRD_OK && manifest)
      status = ab_value_render(&operation, manifest);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          &operation,
          "],\"instructions\":\"Review the package manifest and establish "
          "the required entrypoint without removing unrelated routes.\"}");
  } else {
    status =
        ab_buffer_literal(&operation, "{\"action\":\"set_package_entrypoint\","
                                      "\"package\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, package_name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, manifest);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, ",\"route\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, route);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, ",\"target\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, target);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&operation, "}");
  }
  length = snprintf(
      statement, sizeof(statement), "%s package %.*s entrypoint %.*s%s%.*s.",
      reason ? "Review" : "Set", (int)package_name->as.text.length,
      package_name->as.text.data, (int)route->as.text.length,
      route->as.text.data, target ? " to " : "",
      target ? (int)target->as.text.length : 0,
      target ? target->as.text.data : "");
  if (status == ARCHBIRD_OK &&
      (length < 0 || (size_t)length >= sizeof(statement)))
    status = archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "plan compilation: generated package entrypoint statement is too "
        "long");
  if (status == ARCHBIRD_OK) {
    AbPlanItemSpec spec = {
        .constraint = constraint,
        .findings = groups->rows,
        .finding_count = finding_count,
        .statement = statement,
        .provenance = "derived",
        .operation = &operation,
        .executable = reason == NULL,
        .reasons = reason ? reasons : NULL,
        .reason_count = reason ? 1 : 0,
    };
    status = ab_plan_item_builder_append(builder, &spec);
  }
  ab_buffer_free(&operation);
  return status;
}

ArchbirdStatus ab_plan_compile_package_constraint(
    ArchbirdEngine *engine, const AbValue *map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbProjectionData *actual, int *out_handled) {
  const AbValue *findings = field(constraint, "findings");
  const AbValue *package_name = NULL;
  const AbValue *route = NULL;
  const AbValue *target = NULL;
  const AbValue *package = NULL;
  const AbValue *manifest = NULL;
  const char *reason = NULL;
  AbPlanFindingGroups groups = {0};
  ArchbirdStatus status;
  *out_handled = 0;
  if (!definition ||
      !ab_artifact_text_is(field(definition, "select"), "package_entrypoints"))
    return ARCHBIRD_OK;
  if (!findings || findings->kind != AB_VALUE_ARRAY ||
      !findings->as.array.count)
    return ARCHBIRD_OK;
  if (!exact_single_string(definition, "packages", &package_name) ||
      !exact_single_string(definition, "routes", &route) ||
      !exact_single_string(definition, "target_paths", &target))
    return ARCHBIRD_OK;
  status = ab_plan_finding_groups_collect(engine, findings, &groups);
  if (status != ARCHBIRD_OK)
    return status;
  package = unique_npm_package(map, &package_name->as.text);
  manifest = package ? field(package, "manifest") : NULL;
  if (!actual || actual->state.length != 7 ||
      memcmp(actual->state.data, "current", 7) != 0 ||
      strcmp(ab_projection_data_classification(actual), "complete") != 0 ||
      !actual->selection.has_truncated || actual->selection.truncated ||
      actual->item_count != 0)
    reason =
        "Package entrypoint evidence is not a complete current missing route.";
  else if (!package)
    reason = "The package selector does not identify one npm manifest.";
  else if (!ab_artifact_repository_literal_path(target))
    reason = "The required package entrypoint target is not one literal "
             "package-relative path.";
  else if (!supported_route(package, &route->as.text))
    reason = "The required npm route has nested or conditional structure that "
             "needs review.";
  status = append_package_item(engine, builder, constraint, &groups,
                               package_name, manifest, route, target, reason);
  if (status == ARCHBIRD_OK)
    *out_handled = 1;
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
