#include "projection_internal.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "projection plan: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int stable_id(const AbString *value) {
  size_t index;
  if (!value || !value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
          (byte >= '0' && byte <= '9') ||
          (index &&
           (byte == '_' || byte == '.' || byte == ':' || byte == '-'))))
      return 0;
  }
  return 1;
}

static int name_allowed(const AbString *name, const char *const *allowed,
                        size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_is(name, allowed[index]))
      return 1;
  return 0;
}

static int string_array(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING)
      return 0;
  return 1;
}

static int select_supported(const AbValue *select) {
  static const char *const supported[] = {
      "artifact_routes",      "component_edges",     "component_membership",
      "constant_memberships", "constant_values",     "file_edges",
      "file_metrics",         "inventory_paths",     "macro_members",
      "mapped_paths",         "package_entrypoints", "package_exports",
      "provider_surface",     "search_domain",       "symbols",
      "test_routes",          "test_selectors",
  };
  size_t index;
  if (!select || select->kind != AB_VALUE_STRING)
    return 0;
  for (index = 0; index < sizeof(supported) / sizeof(supported[0]); index++)
    if (string_is(&select->as.text, supported[index]))
      return 1;
  return 0;
}

static int select_is(const AbValue *select, const char *literal) {
  return select && select->kind == AB_VALUE_STRING &&
         string_is(&select->as.text, literal);
}

static int projection_field_allowed(const AbValue *select,
                                    const AbString *name) {
  static const char *const normalized[] = {"exclude", "include", "strip_prefix",
                                           "strip_suffix"};
  static const char *const normalized_operators[] = {
      "component_membership", "constant_memberships", "constant_values",
      "file_metrics",         "inventory_paths",      "macro_members",
      "mapped_paths",         "provider_surface",     "symbols",
      "test_selectors",
  };
  size_t index;
  if (string_is(name, "id") || string_is(name, "select"))
    return 1;
  for (index = 0;
       index < sizeof(normalized_operators) / sizeof(normalized_operators[0]);
       index++)
    if (select_is(select, normalized_operators[index]) &&
        name_allowed(name, normalized,
                     sizeof(normalized) / sizeof(normalized[0])))
      return 1;
  if (select_is(select, "artifact_routes")) {
    static const char *const fields[] = {"artifacts"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "component_edges")) {
    static const char *const fields[] = {"kinds"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "component_membership")) {
    static const char *const fields[] = {"components"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "constant_memberships")) {
    static const char *const fields[] = {"container", "paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "constant_values")) {
    static const char *const fields[] = {"container", "kinds", "paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "file_edges")) {
    static const char *const fields[] = {"from_paths", "kind_patterns", "kinds",
                                         "name_patterns", "to_paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "file_metrics")) {
    static const char *const fields[] = {"metric"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "macro_members")) {
    static const char *const fields[] = {"call", "paths", "selector",
                                         "selector_argument",
                                         "values_from_argument"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "mapped_paths")) {
    static const char *const fields[] = {"paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "package_entrypoints")) {
    static const char *const fields[] = {"packages", "routes", "target_paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "package_exports")) {
    static const char *const fields[] = {"name_patterns", "packages"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "provider_surface")) {
    static const char *const fields[] = {"name"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "symbols")) {
    static const char *const fields[] = {"kinds", "layer", "name_patterns",
                                         "names", "paths", "public_only"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "test_routes")) {
    static const char *const fields[] = {"configured_only", "group", "paths",
                                         "selectors", "target_paths"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  if (select_is(select, "test_selectors")) {
    static const char *const fields[] = {"group", "paths", "selectors"};
    return name_allowed(name, fields, sizeof(fields) / sizeof(fields[0]));
  }
  return 0;
}

static ArchbirdStatus validate_definition(ArchbirdEngine *engine,
                                          const AbValue *definition,
                                          const AbString *id) {
  static const char *const arrays[] = {
      "artifacts", "components",    "exclude",  "from_paths",
      "include",   "kind_patterns", "kinds",    "name_patterns",
      "names",     "packages",      "paths",    "routes",
      "selectors", "target_paths",  "to_paths",
  };
  static const char *const strings[] = {
      "call", "container", "group",        "layer",        "metric",
      "name", "selector",  "strip_prefix", "strip_suffix",
  };
  const AbValue *declared_id;
  const AbValue *select;
  uint64_t argument;
  size_t index;
  if (!definition || definition->kind != AB_VALUE_OBJECT || !stable_id(id))
    return ARCHBIRD_INVALID_ARGUMENT;
  declared_id = ab_value_member(definition, "id");
  if (declared_id && (declared_id->kind != AB_VALUE_STRING ||
                      !ab_string_equal(&declared_id->as.text, id)))
    return invalid(engine, "definition id does not match the plan id");
  select = ab_value_member(definition, "select");
  if (!select_supported(select))
    return invalid(engine, "definition has an unsupported select operator");
  for (index = 0; index < definition->as.object.count; index++)
    if (!projection_field_allowed(select,
                                  &definition->as.object.fields[index].name))
      return invalid(engine,
                     "definition contains a field unsupported by its select "
                     "operator");
  for (index = 0; index < sizeof(arrays) / sizeof(arrays[0]); index++) {
    const AbValue *value = ab_value_member(definition, arrays[index]);
    if (value && !string_array(value))
      return invalid(engine, "projection pattern fields must be string arrays");
  }
  for (index = 0; index < sizeof(strings) / sizeof(strings[0]); index++) {
    const AbValue *value = ab_value_member(definition, strings[index]);
    if (value && value->kind != AB_VALUE_STRING)
      return invalid(engine, "projection string field has the wrong type");
  }
  if ((ab_value_member(definition, "configured_only") &&
       ab_value_member(definition, "configured_only")->kind != AB_VALUE_BOOL) ||
      (ab_value_member(definition, "public_only") &&
       ab_value_member(definition, "public_only")->kind != AB_VALUE_BOOL))
    return invalid(engine, "projection boolean field has the wrong type");
  if (string_is(&select->as.text, "file_metrics") &&
      !ab_value_string_is(ab_value_member(definition, "metric"), "bytes"))
    return invalid(engine, "file_metrics requires metric bytes");
  if (string_is(&select->as.text, "provider_surface") &&
      !ab_projection_nonblank(ab_value_member(definition, "name")))
    return invalid(engine, "provider_surface requires a name");
  if ((string_is(&select->as.text, "constant_values") ||
       string_is(&select->as.text, "constant_memberships")) &&
      !ab_projection_nonblank(ab_value_member(definition, "container")))
    return invalid(engine, "constant projection requires a container");
  if (string_is(&select->as.text, "macro_members") &&
      (!ab_projection_nonblank(ab_value_member(definition, "call")) ||
       !ab_projection_nonblank(ab_value_member(definition, "selector"))))
    return invalid(engine, "macro_members requires call and selector");
  if (ab_value_member(definition, "selector_argument") &&
      (!ab_value_u64(ab_value_member(definition, "selector_argument"),
                     &argument) ||
       argument > 31))
    return invalid(engine, "selector_argument must be from 0 to 31");
  if (ab_value_member(definition, "values_from_argument") &&
      (!ab_value_u64(ab_value_member(definition, "values_from_argument"),
                     &argument) ||
       argument > 31))
    return invalid(engine, "values_from_argument must be from 0 to 31");
  return ARCHBIRD_OK;
}

static ArchbirdStatus normalize_definition(ArchbirdEngine *engine,
                                           const AbValue *definition,
                                           AbValue *out) {
  size_t index;
  size_t count = definition->as.object.count;
  size_t output = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_value_member(definition, "id"))
    count--;
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_OBJECT;
  out->as.object.count = count;
  out->as.object.fields =
      (AbObjectField *)ab_calloc(engine, count, sizeof(*out->as.object.fields));
  if (count && !out->as.object.fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory normalizing projection plan");
  for (index = 0; status == ARCHBIRD_OK && index < definition->as.object.count;
       index++) {
    const AbObjectField *source = &definition->as.object.fields[index];
    AbObjectField *target;
    if (string_is(&source->name, "id"))
      continue;
    target = &out->as.object.fields[output++];
    status = ab_string_copy(engine, &target->name, source->name.data,
                            source->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &target->value, &source->value);
  }
  if (status == ARCHBIRD_OK && output != count)
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->as.object.fields, count, sizeof(*out->as.object.fields),
          field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

ArchbirdStatus ab_projection_plan_compile(ArchbirdEngine *engine,
                                          const AbValue *definition,
                                          const AbString *id,
                                          AbProjectionPlan *out) {
  ArchbirdStatus status;
  if (!engine || !definition || !id || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = validate_definition(engine, definition, id);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &out->id, id->data, id->length);
  if (status == ARCHBIRD_OK)
    status = normalize_definition(engine, definition, &out->definition);
  if (status == ARCHBIRD_OK)
    status = ab_projection_definition_sha256(engine, &out->definition,
                                             out->definition_sha256);
  if (status != ARCHBIRD_OK)
    ab_projection_plan_free(engine, out);
  return status;
}

void ab_projection_plan_free(ArchbirdEngine *engine, AbProjectionPlan *plan) {
  if (!plan)
    return;
  ab_string_free(engine, &plan->id);
  ab_value_free(engine, &plan->definition);
  memset(plan, 0, sizeof(*plan));
}
