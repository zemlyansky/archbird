#include "projection/projection_internal.h"

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

static int string_value_compare(const void *left_raw, const void *right_raw) {
  const AbValue *left = (const AbValue *)left_raw;
  const AbValue *right = (const AbValue *)right_raw;
  return ab_string_compare(&left->as.text, &right->as.text);
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

typedef struct ProjectionFieldName {
  const char *name;
  uint64_t bit;
} ProjectionFieldName;

static const ProjectionFieldName projection_fields[] = {
    {"artifacts", AB_PROJECTION_FIELD_ARTIFACTS},
    {"builds", AB_PROJECTION_FIELD_BUILDS},
    {"call", AB_PROJECTION_FIELD_CALL},
    {"components", AB_PROJECTION_FIELD_COMPONENTS},
    {"configured_only", AB_PROJECTION_FIELD_CONFIGURED_ONLY},
    {"container", AB_PROJECTION_FIELD_CONTAINER},
    {"exclude", AB_PROJECTION_FIELD_EXCLUDE},
    {"from_paths", AB_PROJECTION_FIELD_FROM_PATHS},
    {"group", AB_PROJECTION_FIELD_GROUP},
    {"group_by", AB_PROJECTION_FIELD_GROUP_BY},
    {"include", AB_PROJECTION_FIELD_INCLUDE},
    {"kind_patterns", AB_PROJECTION_FIELD_KIND_PATTERNS},
    {"kinds", AB_PROJECTION_FIELD_KINDS},
    {"layer", AB_PROJECTION_FIELD_LAYER},
    {"level", AB_PROJECTION_FIELD_LEVEL},
    {"metric", AB_PROJECTION_FIELD_METRIC},
    {"name", AB_PROJECTION_FIELD_NAME},
    {"name_patterns", AB_PROJECTION_FIELD_NAME_PATTERNS},
    {"names", AB_PROJECTION_FIELD_NAMES},
    {"overlays", AB_PROJECTION_FIELD_OVERLAYS},
    {"packages", AB_PROJECTION_FIELD_PACKAGES},
    {"paths", AB_PROJECTION_FIELD_PATHS},
    {"public_only", AB_PROJECTION_FIELD_PUBLIC_ONLY},
    {"relations", AB_PROJECTION_FIELD_RELATIONS},
    {"resolutions", AB_PROJECTION_FIELD_RESOLUTIONS},
    {"routes", AB_PROJECTION_FIELD_ROUTES},
    {"selector", AB_PROJECTION_FIELD_SELECTOR},
    {"selector_argument", AB_PROJECTION_FIELD_SELECTOR_ARGUMENT},
    {"selectors", AB_PROJECTION_FIELD_SELECTORS},
    {"strip_prefix", AB_PROJECTION_FIELD_STRIP_PREFIX},
    {"strip_suffix", AB_PROJECTION_FIELD_STRIP_SUFFIX},
    {"target_paths", AB_PROJECTION_FIELD_TARGET_PATHS},
    {"to_paths", AB_PROJECTION_FIELD_TO_PATHS},
    {"values_from_argument", AB_PROJECTION_FIELD_VALUES_FROM_ARGUMENT},
};

static uint64_t projection_field_bit(const AbString *name) {
  size_t index;
  for (index = 0;
       index < sizeof(projection_fields) / sizeof(projection_fields[0]);
       index++)
    if (string_is(name, projection_fields[index].name))
      return projection_fields[index].bit;
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

static int string_array_unique(const AbValue *value) {
  size_t left;
  size_t right;
  if (!string_array(value))
    return 0;
  for (left = 0; left < value->as.array.count; left++)
    for (right = left + 1; right < value->as.array.count; right++)
      if (ab_string_equal(&value->as.array.items[left].as.text,
                          &value->as.array.items[right].as.text))
        return 0;
  return 1;
}

static int projection_field_allowed(const AbProjectionDescriptor *descriptor,
                                    const AbString *name) {
  uint64_t bit;
  if (string_is(name, "id") || string_is(name, "select"))
    return 1;
  bit = projection_field_bit(name);
  return bit && (descriptor->allowed_fields & bit) != 0;
}

static ArchbirdStatus validate_definition(ArchbirdEngine *engine,
                                          const AbValue *definition,
                                          const AbString *id) {
  static const char *const arrays[] = {
      "artifacts",     "builds",       "components",    "exclude",
      "from_paths",    "include",      "kind_patterns", "kinds",
      "name_patterns", "names",        "overlays",      "packages",
      "paths",         "relations",    "resolutions",   "routes",
      "selectors",     "target_paths", "to_paths",
  };
  static const char *const strings[] = {
      "call",     "container",    "group",        "group_by",
      "layer",    "level",        "metric",       "name",
      "selector", "strip_prefix", "strip_suffix",
  };
  const AbProjectionDescriptor *descriptor;
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
  descriptor = select && select->kind == AB_VALUE_STRING
                   ? ab_projection_descriptor_find(&select->as.text)
                   : NULL;
  if (!descriptor)
    return invalid(engine, "definition has an unsupported select operator");
  for (index = 0; index < definition->as.object.count; index++)
    if (!projection_field_allowed(descriptor,
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
  for (index = 0;
       index < sizeof(projection_fields) / sizeof(projection_fields[0]);
       index++)
    if ((descriptor->required_fields & projection_fields[index].bit) != 0 &&
        !ab_value_member(definition, projection_fields[index].name))
      return invalid(engine, descriptor->required_message);
  if (descriptor->kind == AB_PROJECTION_KIND_FILE_METRICS &&
      !ab_value_string_is(ab_value_member(definition, "metric"), "bytes"))
    return invalid(engine, "file_metrics requires metric bytes");
  if (descriptor->kind == AB_PROJECTION_KIND_GRAPH) {
    const AbValue *level = ab_value_member(definition, "level");
    const AbValue *group = ab_value_member(definition, "group_by");
    const AbValue *relations = ab_value_member(definition, "relations");
    const AbValue *overlays = ab_value_member(definition, "overlays");
    size_t item;
    if (!level || (!ab_value_string_is(level, "component") &&
                   !ab_value_string_is(level, "file") &&
                   !ab_value_string_is(level, "symbol")))
      return invalid(engine, "graph requires level component, file, or symbol");
    if (group && !ab_value_string_is(group, "component") &&
        !ab_value_string_is(group, "directory") &&
        !ab_value_string_is(group, "language") &&
        !ab_value_string_is(group, "layer"))
      return invalid(
          engine,
          "graph group_by must be component, directory, language, or layer");
    if (ab_value_string_is(level, "component") && group)
      return invalid(engine, "component graph level cannot also be grouped");
    if ((relations && !string_array_unique(relations)) ||
        (overlays && !string_array_unique(overlays)))
      return invalid(
          engine, "graph relations and overlays must not contain duplicates");
    for (item = 0; relations && item < relations->as.array.count; item++) {
      const AbValue *value = &relations->as.array.items[item];
      if (!ab_value_string_is(value, "builds") &&
          !ab_value_string_is(value, "bridges") &&
          !ab_value_string_is(value, "calls") &&
          !ab_value_string_is(value, "declarations") &&
          !ab_value_string_is(value, "imports") &&
          !ab_value_string_is(value, "packages") &&
          !ab_value_string_is(value, "references") &&
          !ab_value_string_is(value, "tests"))
        return invalid(engine, "graph relations contains an unsupported kind");
      if (ab_value_string_is(level, "symbol") &&
          !ab_value_string_is(value, "calls") &&
          !ab_value_string_is(value, "references"))
        return invalid(engine,
                       "symbol graph level supports calls and references");
    }
    for (item = 0; overlays && item < overlays->as.array.count; item++) {
      const AbValue *value = &overlays->as.array.items[item];
      if (!ab_value_string_is(value, "diagnostics") &&
          !ab_value_string_is(value, "evidence-quality"))
        return invalid(engine, "graph overlays contains an unsupported kind");
    }
  }
  if (descriptor->kind == AB_PROJECTION_KIND_PROVIDER_SURFACE &&
      !ab_projection_nonblank(ab_value_member(definition, "name")))
    return invalid(engine, "provider_surface requires a name");
  if (descriptor->kind == AB_PROJECTION_KIND_SYMBOL_OCCURRENCES) {
    const AbValue *names = ab_value_member(definition, "names");
    if (!names || !string_array_unique(names) || names->as.array.count != 1 ||
        !ab_projection_nonblank(&names->as.array.items[0]))
      return invalid(
          engine,
          "symbol_occurrences requires exactly one non-empty symbol name");
  }
  if ((descriptor->kind == AB_PROJECTION_KIND_CONSTANT_VALUES ||
       descriptor->kind == AB_PROJECTION_KIND_CONSTANT_MEMBERSHIPS) &&
      !ab_projection_nonblank(ab_value_member(definition, "container")))
    return invalid(engine, "constant projection requires a container");
  if (descriptor->kind == AB_PROJECTION_KIND_MACRO_MEMBERS &&
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
    if (status == ARCHBIRD_OK && target->value.kind == AB_VALUE_ARRAY &&
        target->value.as.array.count > 1)
      qsort(target->value.as.array.items, target->value.as.array.count,
            sizeof(*target->value.as.array.items), string_value_compare);
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
