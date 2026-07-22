#include "project_configuration.h"

#include "../projection/projection_internal.h"
#include "date.h"
#include "json_value.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static const char default_project_configuration[] =
    "{\"layers\":["
    "{\"globs\":[\"**/*.c\",\"**/"
    "*.h\"],\"language\":\"c\",\"name\":\"auto-c\",\"required\":false},"
    "{\"globs\":[\"**/*.cc\",\"**/*.cpp\",\"**/*.cxx\",\"**/*.hh\",\"**/"
    "*.hpp\",\"**/"
    "*.hxx\"],\"language\":\"cpp\",\"name\":\"auto-cpp\",\"required\":false},"
    "{\"globs\":[\"**/*.py\",\"**/*.pyi\",\"**/"
    "*.pyw\"],\"language\":\"python\",\"name\":\"auto-python\",\"required\":"
    "false},"
    "{\"globs\":[\"**/*.js\",\"**/*.mjs\",\"**/*.cjs\",\"**/"
    "*.jsx\"],\"language\":\"javascript\",\"name\":\"auto-javascript\","
    "\"required\":false},"
    "{\"globs\":[\"**/*.ts\",\"**/*.mts\",\"**/*.cts\",\"**/"
    "*.tsx\"],\"language\":\"typescript\",\"name\":\"auto-typescript\","
    "\"required\":false},"
    "{\"globs\":[\"**/"
    "*.vue\"],\"language\":\"vue\",\"name\":\"auto-vue\",\"required\":false},"
    "{\"globs\":[\"**/*.R\",\"**/"
    "*.r\",\"NAMESPACE\"],\"language\":\"r\",\"name\":\"auto-r\","
    "\"required\":false}"
    "],\"project\":\"repository\",\"schema_version\":2}";

static int field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int string_equal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
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

static int repository_path(const AbString *value) {
  size_t index;
  size_t segment = 0;
  if (!value || !value->length || value->data[0] == '/' ||
      value->data[0] == '\\' || value->data[value->length - 1] == '/' ||
      value->data[value->length - 1] == '\\' ||
      (value->length >= 2 &&
       ((value->data[0] >= 'A' && value->data[0] <= 'Z') ||
        (value->data[0] >= 'a' && value->data[0] <= 'z')) &&
       value->data[1] == ':'))
    return 0;
  for (index = 0; index <= value->length; index++) {
    if (index < value->length && value->data[index] != '/' &&
        value->data[index] != '\\' && value->data[index] != '\0')
      continue;
    if (index < value->length && value->data[index] != '/')
      return 0;
    if (index == segment ||
        (index - segment == 1 && value->data[segment] == '.') ||
        (index - segment == 2 && value->data[segment] == '.' &&
         value->data[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static int nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int name_in(const AbString *name, const char *const *allowed,
                   size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_equal(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "project configuration: %s", message);
}

static ArchbirdStatus object_allocate(ArchbirdEngine *engine, AbValue *out,
                                      size_t count) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_OBJECT;
  if (count > SIZE_MAX / sizeof(*out->as.object.fields))
    return ARCHBIRD_LIMIT_EXCEEDED;
  if (count) {
    out->as.object.fields = (AbObjectField *)ab_calloc(
        engine, count, sizeof(*out->as.object.fields));
    if (!out->as.object.fields)
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory normalizing project configuration");
  }
  out->as.object.count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus field_copy(ArchbirdEngine *engine, AbObjectField *out,
                                 const AbString *name, const AbValue *value) {
  ArchbirdStatus status =
      ab_string_copy(engine, &out->name, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &out->value, value);
  return status;
}

static ArchbirdStatus field_literal(ArchbirdEngine *engine, AbObjectField *out,
                                    const char *name, const AbValue *value) {
  AbString key = {(char *)name, strlen(name)};
  return field_copy(engine, out, &key, value);
}

static ArchbirdStatus integer_value(ArchbirdEngine *engine, AbValue *out,
                                    const char *text) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_INTEGER;
  return ab_string_copy(engine, &out->as.text, text, strlen(text));
}

static ArchbirdStatus copy_object_without_id(ArchbirdEngine *engine,
                                             const AbValue *source,
                                             AbValue *out) {
  size_t index;
  size_t output = 0;
  size_t count;
  ArchbirdStatus status;
  if (!source || source->kind != AB_VALUE_OBJECT)
    return invalid(engine, "named collection entries must be objects");
  count = source->as.object.count -
          (ab_value_member(source, "id") != NULL ? 1u : 0u);
  status = object_allocate(engine, out, count);
  for (index = 0; status == ARCHBIRD_OK && index < source->as.object.count;
       index++) {
    const AbObjectField *field = &source->as.object.fields[index];
    if (string_equal(&field->name, "id"))
      continue;
    status = field_copy(engine, &out->as.object.fields[output++], &field->name,
                        &field->value);
  }
  if (status == ARCHBIRD_OK && out->as.object.count > 1)
    qsort(out->as.object.fields, out->as.object.count,
          sizeof(*out->as.object.fields), field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus validate_row_id(ArchbirdEngine *engine,
                                      const AbValue *row,
                                      const AbString *expected,
                                      const AbString **out) {
  const AbValue *id = ab_value_member(row, "id");
  if (expected) {
    if (!stable_id(expected))
      return invalid(engine, "named collection key is not a stable identifier");
    if (id && (id->kind != AB_VALUE_STRING ||
               !ab_string_equal(expected, &id->as.text)))
      return invalid(engine,
                     "named collection entry id does not match its key");
    *out = expected;
    return ARCHBIRD_OK;
  }
  if (!id || id->kind != AB_VALUE_STRING || !stable_id(&id->as.text))
    return invalid(engine,
                   "array collection entry requires a stable non-empty id");
  *out = &id->as.text;
  return ARCHBIRD_OK;
}

static ArchbirdStatus normalize_collection(ArchbirdEngine *engine,
                                           const AbValue *source,
                                           const char *name, AbValue *out) {
  size_t count =
      source ? (source->kind == AB_VALUE_OBJECT  ? source->as.object.count
                : source->kind == AB_VALUE_ARRAY ? source->as.array.count
                                                 : 0)
             : 0;
  size_t index;
  ArchbirdStatus status;
  if (source && source->kind != AB_VALUE_OBJECT &&
      source->kind != AB_VALUE_ARRAY)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "project configuration: %s must be an object or array", name);
  status = object_allocate(engine, out, count);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *row;
    const AbString *key = NULL;
    if (source->kind == AB_VALUE_OBJECT) {
      const AbObjectField *field = &source->as.object.fields[index];
      row = &field->value;
      status = validate_row_id(engine, row, &field->name, &key);
    } else {
      row = &source->as.array.items[index];
      status = validate_row_id(engine, row, NULL, &key);
    }
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(engine, &out->as.object.fields[index].name,
                              key->data, key->length);
    if (status == ARCHBIRD_OK)
      status = copy_object_without_id(engine, row,
                                      &out->as.object.fields[index].value);
  }
  if (status == ARCHBIRD_OK && count > 1) {
    qsort(out->as.object.fields, count, sizeof(*out->as.object.fields),
          field_compare);
    for (index = 1; index < count; index++)
      if (ab_string_equal(&out->as.object.fields[index - 1].name,
                          &out->as.object.fields[index].name)) {
        status = invalid(engine, "named collection contains duplicate ids");
        break;
      }
  }
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus collection_error(ArchbirdEngine *engine,
                                       const char *collection,
                                       const AbString *id,
                                       const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "project configuration: %s.%.*s %s", collection,
                            (int)id->length, id->data, message);
}

static int value_string_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         string_equal(&value->as.text, literal);
}

static int string_value_in(const AbValue *value, const char *const *values,
                           size_t count) {
  size_t index;
  if (!nonblank(value))
    return 0;
  for (index = 0; index < count; index++)
    if (value_string_is(value, values[index]))
      return 1;
  return 0;
}

static int strings_value(const AbValue *value, int nonempty) {
  size_t index;
  if (!value)
    return !nonempty;
  if (value->kind != AB_VALUE_ARRAY || (nonempty && !value->as.array.count))
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (!nonblank(&value->as.array.items[index]))
      return 0;
  return 1;
}

static int string_or_strings_value(const AbValue *value) {
  return nonblank(value) || strings_value(value, 0);
}

static int booleans_valid(const AbValue *object, const char *const *fields,
                          size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    const AbValue *value = ab_value_member(object, fields[index]);
    if (value && value->kind != AB_VALUE_BOOL)
      return 0;
  }
  return 1;
}

static int object_fields_allowed(const AbValue *object,
                                 const char *const *allowed, size_t count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < object->as.object.count; index++)
    if (!name_in(&object->as.object.fields[index].name, allowed, count))
      return 0;
  return 1;
}

static int collection_has(const AbValue *collection, const AbString *id) {
  size_t index;
  if (!collection || collection->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < collection->as.object.count; index++)
    if (ab_string_equal(&collection->as.object.fields[index].name, id))
      return 1;
  return 0;
}

static ArchbirdStatus validate_projection_definition(ArchbirdEngine *engine,
                                                     const AbString *id,
                                                     const AbValue *definition,
                                                     const char *collection) {
  const AbValue *declared_id = ab_value_member(definition, "id");
  const AbString *plan_id = declared_id && declared_id->kind == AB_VALUE_STRING
                                ? &declared_id->as.text
                                : id;
  AbProjectionPlan plan = {0};
  ArchbirdStatus status;
  (void)collection;
  status = ab_projection_plan_compile(engine, definition, plan_id, &plan);
  ab_projection_plan_free(engine, &plan);
  return status;
}

static ArchbirdStatus validate_projection_reference(ArchbirdEngine *engine,
                                                    const AbValue *value,
                                                    const AbValue *projections,
                                                    const AbString *owner,
                                                    const char *collection) {
  if (value && value->kind == AB_VALUE_STRING) {
    if (!collection_has(projections, &value->as.text))
      return collection_error(engine, collection, owner,
                              "references an unknown projection");
    return ARCHBIRD_OK;
  }
  if (value && value->kind == AB_VALUE_OBJECT)
    return validate_projection_definition(engine, owner, value, collection);
  return collection_error(engine, collection, owner,
                          "has an invalid projection reference");
}

static ArchbirdStatus validate_query_projection_reference(
    ArchbirdEngine *engine, const AbValue *value, const AbValue *projections,
    const AbString *owner) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return validate_projection_reference(engine, value, projections, owner,
                                         "queries");
  if (!value->as.array.count)
    return collection_error(engine, "queries", owner,
                            "has an empty projection list");
  for (index = 0; index < value->as.array.count; index++) {
    ArchbirdStatus status = validate_projection_reference(
        engine, &value->as.array.items[index], projections, owner, "queries");
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_query_definitions(ArchbirdEngine *engine,
                                                 const AbValue *queries,
                                                 const AbValue *projections) {
  static const char *const allowed[] = {
      "artifacts", "components",   "context",  "depth",     "description",
      "direction", "focus",        "packages", "paths",     "projection",
      "search",    "search_limit", "symbols",  "test_depth"};
  static const char *const arrays[] = {"artifacts", "components", "focus",
                                       "packages",  "paths",      "search",
                                       "symbols"};
  size_t index;
  for (index = 0; index < queries->as.object.count; index++) {
    const AbObjectField *query = &queries->as.object.fields[index];
    const AbValue *value;
    uint64_t number;
    size_t field;
    int has_selector = 0;
    if (!object_fields_allowed(&query->value, allowed,
                               sizeof(allowed) / sizeof(allowed[0])))
      return collection_error(engine, "queries", &query->name,
                              "contains an unknown field");
    for (field = 0; field < sizeof(arrays) / sizeof(arrays[0]); field++) {
      const AbValue *selector = ab_value_member(&query->value, arrays[field]);
      if (selector && !strings_value(selector, 0))
        return collection_error(engine, "queries", &query->name,
                                "contains an invalid string array");
      if (selector && selector->as.array.count)
        has_selector = 1;
    }
    value = ab_value_member(&query->value, "projection");
    if (value) {
      ArchbirdStatus status = validate_query_projection_reference(
          engine, value, projections, &query->name);
      if (status != ARCHBIRD_OK)
        return status;
      has_selector = 1;
    }
    if (!has_selector)
      return collection_error(engine, "queries", &query->name,
                              "requires a projection or focus selector");
    value = ab_value_member(&query->value, "direction");
    if (value &&
        !string_value_in(
            value, (const char *const[]){"both", "downstream", "upstream"}, 3))
      return collection_error(engine, "queries", &query->name,
                              "has an invalid direction");
    for (field = 0; field < 3; field++) {
      const char *name =
          (const char *const[]){"depth", "search_limit", "test_depth"}[field];
      value = ab_value_member(&query->value, name);
      if (value &&
          (!ab_value_u64(value, &number) ||
           (!strcmp(name, "search_limit") && (!number || number > 100))))
        return collection_error(
            engine, "queries", &query->name,
            !strcmp(name, "search_limit")
                ? "search_limit must be from 1 to 100"
                : "contains an invalid nonnegative integer");
    }
    value = ab_value_member(&query->value, "context");
    if (value && value->kind != AB_VALUE_OBJECT)
      return collection_error(engine, "queries", &query->name,
                              "context must be an object");
    value = ab_value_member(&query->value, "description");
    if (value && value->kind != AB_VALUE_STRING)
      return collection_error(engine, "queries", &query->name,
                              "description must be a string");
  }
  return ARCHBIRD_OK;
}

static int relation_rows_value(const AbValue *value) {
  static const char *const fields[] = {"from",   "kind",   "name",
                                       "source", "target", "to"};
  size_t index;
  size_t field;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *row = &value->as.array.items[index];
    if (!object_fields_allowed(row, fields, sizeof(fields) / sizeof(fields[0])))
      return 0;
    if ((!nonblank(ab_value_member(row, "source")) &&
         !nonblank(ab_value_member(row, "from"))) ||
        (!nonblank(ab_value_member(row, "target")) &&
         !nonblank(ab_value_member(row, "to"))))
      return 0;
    for (field = 0; field < sizeof(fields) / sizeof(fields[0]); field++) {
      const AbValue *member = ab_value_member(row, fields[field]);
      if (member && member->kind != AB_VALUE_STRING)
        return 0;
    }
  }
  return 1;
}

static ArchbirdStatus validate_operand(ArchbirdEngine *engine,
                                       const AbValue *operand,
                                       const AbValue *projections,
                                       const AbString *constraint) {
  static const char *const fields[] = {"literal", "map", "observation",
                                       "projection", "source_lock"};
  const AbValue *projection;
  const AbValue *literal;
  const AbValue *map;
  const AbValue *observation;
  const AbValue *source_lock;
  size_t index;
  if (!object_fields_allowed(operand, fields, 5))
    return collection_error(engine, "constraints", constraint,
                            "has an invalid operand");
  projection = ab_value_member(operand, "projection");
  literal = ab_value_member(operand, "literal");
  map = ab_value_member(operand, "map");
  observation = ab_value_member(operand, "observation");
  source_lock = ab_value_member(operand, "source_lock");
  if (!!projection + !!literal + !!observation != 1 ||
      (map && (!projection || map->kind != AB_VALUE_STRING ||
               !stable_id(&map->as.text))) ||
      (source_lock && (!projection || source_lock->kind != AB_VALUE_OBJECT ||
                       !source_lock->as.object.count)) ||
      operand->as.object.count !=
          (size_t)(1 + (map != NULL) + (source_lock != NULL)))
    return collection_error(engine, "constraints", constraint,
                            "has an invalid operand");
  for (index = 0; source_lock && index < source_lock->as.object.count;
       index++) {
    const AbObjectField *entry = &source_lock->as.object.fields[index];
    if (!repository_path(&entry->name) || !lowercase_sha256(&entry->value))
      return collection_error(engine, "constraints", constraint,
                              "has an invalid source_lock");
  }
  if (projection)
    return validate_projection_reference(engine, projection, projections,
                                         constraint, "constraints");
  if (literal &&
      (literal->kind == AB_VALUE_ARRAY || literal->kind == AB_VALUE_OBJECT))
    return ARCHBIRD_OK;
  if (observation && observation->kind == AB_VALUE_STRING &&
      stable_id(&observation->as.text))
    return ARCHBIRD_OK;
  return collection_error(engine, "constraints", constraint,
                          "has an invalid operand value");
}

static ArchbirdStatus validate_constraint_waivers(ArchbirdEngine *engine,
                                                  const AbString *constraint,
                                                  const AbValue *waivers) {
  static const char *const fields[] = {
      "comparison", "expires_on", "fingerprint", "id",
      "key",        "owner",      "rationale",   "until_inputs"};
  size_t index;
  if (!waivers)
    return ARCHBIRD_OK;
  if (waivers->kind != AB_VALUE_ARRAY || !waivers->as.array.count)
    return collection_error(engine, "constraints", constraint,
                            "waivers must be a non-empty array");
  for (index = 0; index < waivers->as.array.count; index++) {
    const AbValue *waiver = &waivers->as.array.items[index];
    const AbValue *id = ab_value_member(waiver, "id");
    const AbValue *fingerprint = ab_value_member(waiver, "fingerprint");
    const AbValue *comparison = ab_value_member(waiver, "comparison");
    const AbValue *key = ab_value_member(waiver, "key");
    const AbValue *expires = ab_value_member(waiver, "expires_on");
    const AbValue *until_inputs = ab_value_member(waiver, "until_inputs");
    size_t previous;
    size_t boundary;
    if (!object_fields_allowed(waiver, fields,
                               sizeof(fields) / sizeof(fields[0])) ||
        !id || id->kind != AB_VALUE_STRING || !stable_id(&id->as.text) ||
        !nonblank(ab_value_member(waiver, "owner")) ||
        !nonblank(ab_value_member(waiver, "rationale")) ||
        (!!fingerprint == !!comparison) || (!!comparison != !!key) ||
        (fingerprint && !lowercase_sha256(fingerprint)) ||
        (comparison && (comparison->kind != AB_VALUE_STRING ||
                        !stable_id(&comparison->as.text))) ||
        (key && key->kind != AB_VALUE_STRING) || (!expires && !until_inputs) ||
        (expires && (expires->kind != AB_VALUE_STRING ||
                     !ab_iso_date_valid(&expires->as.text))) ||
        (until_inputs && (until_inputs->kind != AB_VALUE_OBJECT ||
                          !until_inputs->as.object.count)))
      return collection_error(engine, "constraints", constraint,
                              "contains an invalid waiver");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old_id =
          ab_value_member(&waivers->as.array.items[previous], "id");
      if (ab_string_equal(&old_id->as.text, &id->as.text))
        return collection_error(engine, "constraints", constraint,
                                "contains duplicate waiver IDs");
    }
    for (boundary = 0; until_inputs && boundary < until_inputs->as.object.count;
         boundary++) {
      const AbObjectField *field = &until_inputs->as.object.fields[boundary];
      if (!stable_id(&field->name) || !lowercase_sha256(&field->value))
        return collection_error(engine, "constraints", constraint,
                                "contains an invalid waiver input boundary");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_constraint_definitions(ArchbirdEngine *engine,
                                const AbValue *constraints,
                                const AbValue *projections) {
  static const char *const allowed[] = {"actual",
                                        "allow_empty",
                                        "assert",
                                        "bridge",
                                        "configured_only",
                                        "declared",
                                        "edge_kind",
                                        "edges",
                                        "exclude",
                                        "expected",
                                        "exact",
                                        "forbid_ambiguous",
                                        "forbid_unregistered",
                                        "forbid_unresolved",
                                        "from_paths",
                                        "group",
                                        "include",
                                        "kind",
                                        "kind_patterns",
                                        "kinds",
                                        "layer",
                                        "mapping",
                                        "max",
                                        "min",
                                        "name",
                                        "name_patterns",
                                        "owner",
                                        "package",
                                        "paths",
                                        "public_only",
                                        "rationale",
                                        "reference_route",
                                        "required_routes",
                                        "requirement",
                                        "route",
                                        "selectors",
                                        "severity",
                                        "source",
                                        "strip_prefix",
                                        "strip_suffix",
                                        "surface",
                                        "symbols",
                                        "tags",
                                        "target",
                                        "target_paths",
                                        "to_paths",
                                        "values",
                                        "waivers"};
  static const char *const typed[] = {"allowed_component_edges",
                                      "allowed_file_edges",
                                      "component_cycles",
                                      "component_membership",
                                      "forbidden_component_edges",
                                      "forbidden_file_edges",
                                      "forbidden_paths",
                                      "forbidden_symbols",
                                      "max_file_bytes",
                                      "provider_surface",
                                      "required_bridge",
                                      "required_component_edges",
                                      "required_file_edge",
                                      "required_file_edges",
                                      "required_package_entrypoint",
                                      "required_paths",
                                      "required_symbols",
                                      "required_test_route",
                                      "symbol_cardinality",
                                      "test_routes"};
  static const char *const assertions[] = {"acyclic",
                                           "allowed_edges",
                                           "cardinality",
                                           "disjoint",
                                           "forbidden_edges",
                                           "mapped_set_equal",
                                           "mapped_values_equal",
                                           "min_test_routes",
                                           "numeric_bounds",
                                           "observations_equal",
                                           "required_edges",
                                           "required_subset",
                                           "required_values",
                                           "set_equal",
                                           "subset",
                                           "values_equal"};
  static const char *const string_arrays[] = {
      "exclude",   "from_paths",    "include", "kind_patterns",
      "kinds",     "name_patterns", "paths",   "required_routes",
      "selectors", "symbols",       "tags",    "target_paths",
      "to_paths"};
  static const char *const booleans[] = {
      "allow_empty",         "configured_only",   "forbid_ambiguous",
      "forbid_unregistered", "forbid_unresolved", "public_only"};
  size_t index;
  for (index = 0; index < constraints->as.object.count; index++) {
    const AbObjectField *constraint = &constraints->as.object.fields[index];
    const AbValue *kind = ab_value_member(&constraint->value, "kind");
    const AbValue *assertion = ab_value_member(&constraint->value, "assert");
    const AbValue *value;
    uint64_t number;
    int has_min;
    int has_max;
    int has_exact;
    if (!object_fields_allowed(&constraint->value, allowed,
                               sizeof(allowed) / sizeof(allowed[0])))
      return collection_error(engine, "constraints", &constraint->name,
                              "contains an unknown field");
    if (!nonblank(ab_value_member(&constraint->value, "owner")) ||
        !nonblank(ab_value_member(&constraint->value, "rationale")))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires owner and rationale");
    {
      size_t field;
      for (field = 0; field < sizeof(string_arrays) / sizeof(string_arrays[0]);
           field++)
        if (ab_value_member(&constraint->value, string_arrays[field]) &&
            !strings_value(
                ab_value_member(&constraint->value, string_arrays[field]), 0))
          return collection_error(engine, "constraints", &constraint->name,
                                  "contains an invalid string array");
    }
    if (!booleans_valid(&constraint->value, booleans,
                        sizeof(booleans) / sizeof(booleans[0])))
      return collection_error(engine, "constraints", &constraint->name,
                              "contains a non-boolean option");
    value = ab_value_member(&constraint->value, "requirement");
    if (value && !string_or_strings_value(value))
      return collection_error(engine, "constraints", &constraint->name,
                              "has an invalid requirement");
    value = ab_value_member(&constraint->value, "mapping");
    if (value && value->kind != AB_VALUE_OBJECT)
      return collection_error(engine, "constraints", &constraint->name,
                              "mapping must be an object");
    {
      ArchbirdStatus waiver_status = validate_constraint_waivers(
          engine, &constraint->name,
          ab_value_member(&constraint->value, "waivers"));
      if (waiver_status != ARCHBIRD_OK)
        return waiver_status;
    }
    value = ab_value_member(&constraint->value, "reference_route");
    if (value && !nonblank(value))
      return collection_error(engine, "constraints", &constraint->name,
                              "reference_route must be a non-empty string");
    if (!!kind == !!assertion)
      return collection_error(engine, "constraints", &constraint->name,
                              "requires exactly one kind or assert");
    if (kind && !string_value_in(kind, typed, sizeof(typed) / sizeof(typed[0])))
      return collection_error(engine, "constraints", &constraint->name,
                              "has an unsupported typed kind");
    if (assertion &&
        !string_value_in(assertion, assertions,
                         sizeof(assertions) / sizeof(assertions[0])))
      return collection_error(engine, "constraints", &constraint->name,
                              "has an unsupported assertion");
    value = ab_value_member(&constraint->value, "severity");
    if (value &&
        !string_value_in(value,
                         (const char *const[]){"error", "note", "warning"}, 3))
      return collection_error(engine, "constraints", &constraint->name,
                              "has an invalid severity");
    has_min = ab_value_member(&constraint->value, "min") != NULL;
    has_max = ab_value_member(&constraint->value, "max") != NULL;
    has_exact = ab_value_member(&constraint->value, "exact") != NULL;
    if ((has_min &&
         !ab_value_u64(ab_value_member(&constraint->value, "min"), &number)) ||
        (has_max &&
         !ab_value_u64(ab_value_member(&constraint->value, "max"), &number)) ||
        (has_exact &&
         !ab_value_u64(ab_value_member(&constraint->value, "exact"), &number)))
      return collection_error(engine, "constraints", &constraint->name,
                              "contains an invalid nonnegative bound");
    if (has_exact && (has_min || has_max))
      return collection_error(engine, "constraints", &constraint->name,
                              "cannot combine exact with min or max");
    if (has_min && has_max) {
      uint64_t minimum;
      uint64_t maximum;
      (void)ab_value_u64(ab_value_member(&constraint->value, "min"), &minimum);
      (void)ab_value_u64(ab_value_member(&constraint->value, "max"), &maximum);
      if (minimum > maximum)
        return collection_error(engine, "constraints", &constraint->name,
                                "requires min <= max");
    }
    if (assertion) {
      const AbValue *actual = ab_value_member(&constraint->value, "actual");
      const AbValue *expected = ab_value_member(&constraint->value, "expected");
      ArchbirdStatus status;
      if (!actual)
        return collection_error(engine, "constraints", &constraint->name,
                                "primitive assertion requires actual");
      status = validate_operand(engine, actual, projections, &constraint->name);
      if (status != ARCHBIRD_OK)
        return status;
      if (expected) {
        status =
            validate_operand(engine, expected, projections, &constraint->name);
        if (status != ARCHBIRD_OK)
          return status;
      }
      if (value_string_is(assertion, "observations_equal") &&
          (!expected || !ab_value_member(actual, "observation") ||
           !ab_value_member(expected, "observation")))
        return collection_error(
            engine, "constraints", &constraint->name,
            "observations_equal requires actual and expected observations");
      if ((value_string_is(assertion, "cardinality") ||
           value_string_is(assertion, "numeric_bounds")) &&
          !has_min && !has_max && !has_exact)
        return collection_error(
            engine, "constraints", &constraint->name,
            "bounded assertion requires min, max, or exact");
      if (!expected && !value_string_is(assertion, "acyclic") &&
          !value_string_is(assertion, "cardinality") &&
          !value_string_is(assertion, "min_test_routes") &&
          !value_string_is(assertion, "numeric_bounds"))
        return collection_error(engine, "constraints", &constraint->name,
                                "assertion requires expected");
    }
    if (kind &&
        (value_string_is(kind, "required_paths") ||
         value_string_is(kind, "forbidden_paths")) &&
        !strings_value(ab_value_member(&constraint->value, "paths"), 1))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires non-empty paths");
    if (kind &&
        (value_string_is(kind, "required_symbols") ||
         value_string_is(kind, "forbidden_symbols")) &&
        !strings_value(ab_value_member(&constraint->value, "symbols"), 1))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires non-empty symbols");
    if (kind && value_string_is(kind, "max_file_bytes") && !has_max)
      return collection_error(engine, "constraints", &constraint->name,
                              "requires max");
    if (kind && value_string_is(kind, "required_file_edge") &&
        (!nonblank(ab_value_member(&constraint->value, "edge_kind")) ||
         !nonblank(ab_value_member(&constraint->value, "source")) ||
         !nonblank(ab_value_member(&constraint->value, "target"))))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires edge_kind, source, and target");
    if (kind && value_string_is(kind, "required_bridge") &&
        !nonblank(ab_value_member(&constraint->value, "bridge")))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires bridge");
    if (kind && value_string_is(kind, "required_package_entrypoint") &&
        (!nonblank(ab_value_member(&constraint->value, "package")) ||
         !nonblank(ab_value_member(&constraint->value, "route"))))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires package and route");
    if (kind && value_string_is(kind, "required_test_route") &&
        (!nonblank(ab_value_member(&constraint->value, "group")) ||
         !nonblank(ab_value_member(&constraint->value, "target"))))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires group and target");
    if (kind && value_string_is(kind, "provider_surface") &&
        !nonblank(ab_value_member(&constraint->value, "surface")))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires surface");
    if (kind && value_string_is(kind, "provider_surface") &&
        !strings_value(ab_value_member(&constraint->value, "declared"), 0))
      return collection_error(engine, "constraints", &constraint->name,
                              "contains an invalid declared surface");
    if (kind &&
        (value_string_is(kind, "allowed_component_edges") ||
         value_string_is(kind, "forbidden_component_edges") ||
         value_string_is(kind, "required_component_edges") ||
         value_string_is(kind, "allowed_file_edges") ||
         value_string_is(kind, "forbidden_file_edges") ||
         value_string_is(kind, "required_file_edges")) &&
        !relation_rows_value(ab_value_member(&constraint->value, "edges")))
      return collection_error(engine, "constraints", &constraint->name,
                              "requires valid edge rows");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_project_consumers(ArchbirdEngine *engine,
                                                 const AbValue *projections,
                                                 const AbValue *queries,
                                                 const AbValue *constraints) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < projections->as.object.count;
       index++) {
    const AbObjectField *projection = &projections->as.object.fields[index];
    status = validate_projection_definition(engine, &projection->name,
                                            &projection->value, "projections");
  }
  if (status == ARCHBIRD_OK)
    status = validate_query_definitions(engine, queries, projections);
  if (status == ARCHBIRD_OK)
    status = validate_constraint_definitions(engine, constraints, projections);
  return status;
}

static ArchbirdStatus
normalized_configuration(ArchbirdEngine *engine, const AbValue *source,
                         const AbValue *projections, const AbValue *queries,
                         const AbValue *constraints, AbValue *out) {
  size_t index;
  size_t output = 0;
  ArchbirdStatus status = object_allocate(engine, out, source->as.object.count);
  for (index = 0; status == ARCHBIRD_OK && index < source->as.object.count;
       index++) {
    const AbObjectField *field = &source->as.object.fields[index];
    const AbValue *value = &field->value;
    if (string_equal(&field->name, "projections"))
      value = projections;
    else if (string_equal(&field->name, "queries"))
      value = queries;
    else if (string_equal(&field->name, "constraints"))
      value = constraints;
    status = field_copy(engine, &out->as.object.fields[output++], &field->name,
                        value);
  }
  if (status == ARCHBIRD_OK && out->as.object.count > 1)
    qsort(out->as.object.fields, out->as.object.count,
          sizeof(*out->as.object.fields), field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus normalize_map_definition(ArchbirdEngine *engine,
                                               const AbValue *source,
                                               AbValue *out) {
  static const char *const map_fields[] = {
      "artifacts",     "bridges",  "builds",  "components", "description",
      "discovery",     "exclude",  "indexes", "layers",     "limits",
      "named_entries", "packages", "parity",  "project",    "tests"};
  AbValue defaults = {0};
  AbValue version = {0};
  const AbValue *layers = ab_value_member(source, "layers");
  size_t index;
  size_t count = 1;
  size_t output = 0;
  ArchbirdStatus status;
  if (!layers) {
    size_t length;
    const uint8_t *json = ab_default_project_configuration(&length);
    status = ab_json_value_decode(engine, json, length, &defaults);
    if (status != ARCHBIRD_OK)
      return status;
    layers = ab_value_member(&defaults, "layers");
  }
  for (index = 0; index < sizeof(map_fields) / sizeof(map_fields[0]); index++)
    if (ab_value_member(source, map_fields[index]) ||
        !strcmp(map_fields[index], "layers"))
      count++;
  status = object_allocate(engine, out, count);
  for (index = 0; status == ARCHBIRD_OK &&
                  index < sizeof(map_fields) / sizeof(map_fields[0]);
       index++) {
    const char *name = map_fields[index];
    const AbValue *value =
        !strcmp(name, "layers") ? layers : ab_value_member(source, name);
    if (!value)
      continue;
    status =
        field_literal(engine, &out->as.object.fields[output++], name, value);
  }
  if (status == ARCHBIRD_OK)
    status = integer_value(engine, &version, "2");
  if (status == ARCHBIRD_OK)
    status = field_literal(engine, &out->as.object.fields[output++],
                           "schema_version", &version);
  if (status == ARCHBIRD_OK && out->as.object.count > 1)
    qsort(out->as.object.fields, out->as.object.count,
          sizeof(*out->as.object.fields), field_compare);
  ab_value_free(engine, &version);
  ab_value_free(engine, &defaults);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus digest_value(ArchbirdEngine *engine, const AbValue *value,
                                   char out[65]) {
  AbBuffer buffer;
  ArchbirdSha256Context context;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK) {
    archbird_sha256_init(&context);
    status = archbird_sha256_update(&context, buffer.data, buffer.length);
  }
  if (status == ARCHBIRD_OK) {
    archbird_sha256_final(&context, digest);
    archbird_sha256_hex(digest, out);
  }
  ab_buffer_free(&buffer);
  return status;
}

const uint8_t *ab_default_project_configuration(size_t *length) {
  if (length)
    *length = sizeof(default_project_configuration) - 1;
  return (const uint8_t *)default_project_configuration;
}

ArchbirdStatus ab_project_configuration_decode(ArchbirdEngine *engine,
                                               const uint8_t *json,
                                               size_t json_length,
                                               AbProjectConfiguration *out) {
  static const char *const fields[] = {
      "artifacts",   "bridges",        "builds",    "components",
      "constraints", "description",    "discovery", "exclude",
      "indexes",     "layers",         "limits",    "named_entries",
      "packages",    "parity",         "project",   "projections",
      "queries",     "schema_version", "tests"};
  AbValue document = {0};
  const AbValue *schema;
  uint64_t version = 0;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !json || !json_length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, json_length, &document);
  if (status != ARCHBIRD_OK)
    return status;
  schema = ab_value_member(&document, "schema_version");
  if (document.kind != AB_VALUE_OBJECT || !ab_value_u64(schema, &version) ||
      version != 2) {
    status = invalid(engine, "schema_version must equal 2");
    goto done;
  }
  for (index = 0; index < document.as.object.count; index++)
    if (!name_in(&document.as.object.fields[index].name, fields,
                 sizeof(fields) / sizeof(fields[0]))) {
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "project configuration: unknown field '%.*s'",
          (int)document.as.object.fields[index].name.length,
          document.as.object.fields[index].name.data);
      goto done;
    }
  if (!nonblank(ab_value_member(&document, "project"))) {
    status = invalid(engine, "project must be a non-empty string");
    goto done;
  }
  {
    const AbValue *layers = ab_value_member(&document, "layers");
    if (!layers || layers->kind != AB_VALUE_ARRAY || !layers->as.array.count) {
      status = invalid(engine, "layers must be a non-empty array");
      goto done;
    }
  }
  status =
      normalize_collection(engine, ab_value_member(&document, "projections"),
                           "projections", &out->projections);
  if (status == ARCHBIRD_OK)
    status = normalize_collection(engine, ab_value_member(&document, "queries"),
                                  "queries", &out->queries);
  if (status == ARCHBIRD_OK)
    status =
        normalize_collection(engine, ab_value_member(&document, "constraints"),
                             "constraints", &out->constraints);
  if (status == ARCHBIRD_OK)
    status = validate_project_consumers(engine, &out->projections,
                                        &out->queries, &out->constraints);
  if (status == ARCHBIRD_OK)
    status = normalized_configuration(engine, &document, &out->projections,
                                      &out->queries, &out->constraints,
                                      &out->normalized);
  if (status == ARCHBIRD_OK)
    status = normalize_map_definition(engine, &document, &out->map_definition);
  if (status == ARCHBIRD_OK)
    status =
        digest_value(engine, &out->constraints, out->constraint_policy_sha256);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, &out->map_definition, out->map_config_sha256);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, &out->normalized, out->sha256);
done:
  ab_value_free(engine, &document);
  if (status != ARCHBIRD_OK)
    ab_project_configuration_free(engine, out);
  return status;
}

void ab_project_configuration_free(ArchbirdEngine *engine,
                                   AbProjectConfiguration *configuration) {
  if (!configuration)
    return;
  ab_value_free(engine, &configuration->normalized);
  ab_value_free(engine, &configuration->map_definition);
  ab_value_free(engine, &configuration->projections);
  ab_value_free(engine, &configuration->queries);
  ab_value_free(engine, &configuration->constraints);
  memset(configuration, 0, sizeof(*configuration));
}
