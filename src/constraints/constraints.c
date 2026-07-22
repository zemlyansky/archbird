#include <archbird/archbird.h>

#include "../configuration/project_configuration.h"
#include "../projection/projection_internal.h"
#include "archbird_internal.h"
#include "date.h"
#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"
#include "verify_checks.h"
#include "verify_reports.h"
#include "verify_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

typedef struct ConstraintProjectionBinding {
  AbString name;
  AbString map_id;
  AbValue source_lock;
  AbProjectionPlan plan;
} ConstraintProjectionBinding;

typedef struct ConstraintExecution {
  ArchbirdEngine *engine;
  AbProjectConfiguration configuration;
  AbValue map;
  AbValue resolution;
  AbValue request;
  AbValue operand_definitions;
  AbValue mappings;
  AbValue constraint_plans;
  AbValue policy;
  AbVerificationContext context;
  ConstraintProjectionBinding *projection_bindings;
  size_t projection_binding_count;
  size_t operand_capacity;
  size_t mapping_capacity;
} ConstraintExecution;

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "constraints: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int object_field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int fact_compare(const void *left_raw, const void *right_raw) {
  const AbProjectionData *left = (const AbProjectionData *)left_raw;
  const AbProjectionData *right = (const AbProjectionData *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int value_string_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         string_is(&value->as.text, literal);
}

static int nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    char byte = value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
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

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    char byte = value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static ArchbirdStatus object_init(ArchbirdEngine *engine, AbValue *out,
                                  size_t capacity) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_OBJECT;
  if (!capacity)
    return ARCHBIRD_OK;
  if (capacity > SIZE_MAX / sizeof(*out->as.object.fields))
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->as.object.fields = (AbObjectField *)ab_calloc(
      engine, capacity, sizeof(*out->as.object.fields));
  if (!out->as.object.fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling constraints");
  return ARCHBIRD_OK;
}

static ArchbirdStatus array_init(ArchbirdEngine *engine, AbValue *out,
                                 size_t capacity) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_ARRAY;
  if (!capacity)
    return ARCHBIRD_OK;
  if (capacity > SIZE_MAX / sizeof(*out->as.array.items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->as.array.items =
      (AbValue *)ab_calloc(engine, capacity, sizeof(*out->as.array.items));
  if (!out->as.array.items)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling constraints");
  return ARCHBIRD_OK;
}

static ArchbirdStatus singleton_string_array(ArchbirdEngine *engine,
                                             const AbValue *value,
                                             AbValue *out) {
  if (!nonblank(value))
    return invalid(engine, "constraint filter must be a non-empty string");
  TRY(array_init(engine, out, 1));
  out->as.array.items[0].kind = AB_VALUE_STRING;
  TRY(ab_string_copy(engine, &out->as.array.items[0].as.text,
                     value->as.text.data, value->as.text.length));
  out->as.array.count = 1;
  return ARCHBIRD_OK;
}

static ArchbirdStatus object_add_copy(ArchbirdEngine *engine, AbValue *object,
                                      const char *name, const AbValue *value) {
  AbObjectField field = {0};
  ArchbirdStatus status =
      ab_string_copy(engine, &field.name, name, strlen(name));
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &field.value, value);
  if (status == ARCHBIRD_OK) {
    object->as.object.fields[object->as.object.count] = field;
    memset(&field, 0, sizeof(field));
    object->as.object.count++;
  }
  ab_value_free(engine, &field.value);
  ab_string_free(engine, &field.name);
  return status;
}

static ArchbirdStatus object_add_string(ArchbirdEngine *engine, AbValue *object,
                                        const char *name, const char *value,
                                        size_t value_length) {
  AbObjectField field = {0};
  ArchbirdStatus status =
      ab_string_copy(engine, &field.name, name, strlen(name));
  if (status == ARCHBIRD_OK) {
    field.value.kind = AB_VALUE_STRING;
    status = ab_string_copy(engine, &field.value.as.text, value, value_length);
  }
  if (status == ARCHBIRD_OK) {
    object->as.object.fields[object->as.object.count] = field;
    memset(&field, 0, sizeof(field));
    object->as.object.count++;
  }
  ab_value_free(engine, &field.value);
  ab_string_free(engine, &field.name);
  return status;
}

static ArchbirdStatus object_add_u64(ArchbirdEngine *engine, AbValue *object,
                                     const char *name, uint64_t value) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
  AbObjectField field = {0};
  ArchbirdStatus status;
  if (length < 0 || (size_t)length >= sizeof(text))
    return ARCHBIRD_CONFLICT;
  status = ab_string_copy(engine, &field.name, name, strlen(name));
  if (status == ARCHBIRD_OK) {
    field.value.kind = AB_VALUE_INTEGER;
    status = ab_string_copy(engine, &field.value.as.text, text, (size_t)length);
  }
  if (status == ARCHBIRD_OK) {
    object->as.object.fields[object->as.object.count] = field;
    memset(&field, 0, sizeof(field));
    object->as.object.count++;
  }
  ab_value_free(engine, &field.value);
  ab_string_free(engine, &field.name);
  return status;
}

static ArchbirdStatus object_add_named_copy(ArchbirdEngine *engine,
                                            AbValue *object,
                                            const AbString *name,
                                            const AbValue *value) {
  AbObjectField field = {0};
  ArchbirdStatus status =
      ab_string_copy(engine, &field.name, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &field.value, value);
  if (status == ARCHBIRD_OK) {
    object->as.object.fields[object->as.object.count] = field;
    memset(&field, 0, sizeof(field));
    object->as.object.count++;
  }
  ab_value_free(engine, &field.value);
  ab_string_free(engine, &field.name);
  return status;
}

static ArchbirdStatus value_digest(ArchbirdEngine *engine, const AbValue *value,
                                   char output[65]) {
  AbBuffer canonical;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, engine);
  status = ab_value_render(&canonical, value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus named_digest(ArchbirdEngine *engine, const char *prefix,
                                   const AbValue *value, AbString *out) {
  char digest[65];
  char name[72];
  size_t prefix_length = strlen(prefix);
  TRY(value_digest(engine, value, digest));
  if (prefix_length + 64 >= sizeof(name))
    return ARCHBIRD_CONFLICT;
  memcpy(name, prefix, prefix_length);
  memcpy(name + prefix_length, digest, 64);
  return ab_string_copy(engine, out, name, prefix_length + 64);
}

static ArchbirdStatus projection_operand_name(ConstraintExecution *execution,
                                              const AbString *map_id,
                                              const AbValue *definition,
                                              const AbValue *source_lock,
                                              AbString *out) {
  AbBuffer canonical;
  uint8_t digest[32];
  char hex[65];
  char name[67];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, execution->engine);
  status =
      ab_buffer_append(&canonical, "archbird-constraint-projection-v1\0", 34);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&canonical, map_id->data, map_id->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&canonical, "\0", 1);
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&canonical, definition);
  if (status == ARCHBIRD_OK && source_lock)
    status = ab_buffer_append(&canonical, "\0", 1);
  if (status == ARCHBIRD_OK && source_lock)
    status = ab_value_render(&canonical, source_lock);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK) {
    archbird_sha256_hex(digest, hex);
    memcpy(name, "p.", 2);
    memcpy(name + 2, hex, 64);
    status = ab_string_copy(execution->engine, out, name, 66);
  }
  ab_buffer_free(&canonical);
  return status;
}

static const AbValue *map_input_row(const AbValue *map, const AbString *path) {
  const AbValue *collections[] = {ab_value_member(map, "inputs"),
                                  ab_value_member(map, "files")};
  size_t collection;
  for (collection = 0; collection < 2; collection++) {
    const AbValue *rows = collections[collection];
    size_t index;
    if (!rows || rows->kind != AB_VALUE_ARRAY)
      continue;
    for (index = 0; index < rows->as.array.count; index++) {
      const AbValue *row = &rows->as.array.items[index];
      const AbValue *candidate = ab_value_member(row, "path");
      if (candidate && candidate->kind == AB_VALUE_STRING &&
          ab_string_equal(&candidate->as.text, path))
        return row;
    }
  }
  return NULL;
}

static ArchbirdStatus source_lock_current(const AbValue *map,
                                          const AbValue *source_lock,
                                          int *current, AbBuffer *message) {
  size_t index;
  *current = 1;
  for (index = 0; index < source_lock->as.object.count; index++) {
    const AbObjectField *expected = &source_lock->as.object.fields[index];
    const AbValue *row = map_input_row(map, &expected->name);
    const AbValue *actual = row ? ab_value_member(row, "sha256") : NULL;
    if (actual && actual->kind == AB_VALUE_STRING &&
        ab_string_equal(&actual->as.text, &expected->value.as.text))
      continue;
    *current = 0;
    TRY(ab_buffer_literal(message, row ? "source lock mismatch: "
                                       : "source lock input missing: "));
    return ab_buffer_append(message, expected->name.data,
                            expected->name.length);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus prepared_add_source_lock(ConstraintExecution *execution,
                                               AbObjectField *prepared,
                                               const AbValue *source_lock) {
  AbValue expanded = {0};
  size_t index;
  ArchbirdStatus status = object_init(execution->engine, &expanded,
                                      prepared->value.as.object.count + 1);
  for (index = 0;
       status == ARCHBIRD_OK && index < prepared->value.as.object.count;
       index++) {
    const AbObjectField *field = &prepared->value.as.object.fields[index];
    status = object_add_named_copy(execution->engine, &expanded, &field->name,
                                   &field->value);
  }
  if (status == ARCHBIRD_OK)
    status = object_add_copy(execution->engine, &expanded, "source_lock",
                             source_lock);
  if (status == ARCHBIRD_OK) {
    qsort(expanded.as.object.fields, expanded.as.object.count,
          sizeof(*expanded.as.object.fields), object_field_compare);
    ab_value_free(execution->engine, &prepared->value);
    prepared->value = expanded;
    memset(&expanded, 0, sizeof(expanded));
  }
  ab_value_free(execution->engine, &expanded);
  return status;
}

static const AbObjectField *named_field(const AbValue *object,
                                        const AbString *name) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return NULL;
  for (index = 0; index < object->as.object.count; index++)
    if (ab_string_equal(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index];
  return NULL;
}

static ArchbirdStatus constraint_map(ConstraintExecution *execution,
                                     const AbString *map_id,
                                     const AbValue **map,
                                     const AbValue **resolution) {
  static const AbString current = {(char *)"current", 7};
  const AbValue *maps;
  const AbObjectField *field;
  if (!map_id || ab_string_equal(map_id, &current)) {
    *map = &execution->map;
    *resolution = execution->resolution.kind == AB_VALUE_OBJECT
                      ? &execution->resolution
                      : NULL;
    return ARCHBIRD_OK;
  }
  maps = ab_value_member(&execution->request, "maps");
  field =
      maps && maps->kind == AB_VALUE_OBJECT ? named_field(maps, map_id) : NULL;
  if (!field) {
    if (map_id->length > INT_MAX)
      return invalid(execution->engine,
                     "constraint references an unsupplied Map with an "
                     "oversized ID");
    return archbird_error_set(
        execution->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "constraints: constraint references unsupplied Map \"%.*s\"; supply "
        "--map-input %.*s=PATH",
        (int)map_id->length, map_id->data, (int)map_id->length, map_id->data);
  }
  *map = ab_value_member(&field->value, "map");
  *resolution = ab_value_member(&field->value, "resolution");
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_projection_operand(ConstraintExecution *execution,
                                             const AbValue *definition,
                                             const AbString *map_id,
                                             const AbValue *source_lock,
                                             const AbString **name) {
  static const AbString current = {(char *)"current", 7};
  AbString generated = {0};
  const AbObjectField *previous;
  AbProjectionPlan plan = {0};
  AbObjectField *stored = NULL;
  ConstraintProjectionBinding *binding = NULL;
  ArchbirdStatus status =
      projection_operand_name(execution, map_id ? map_id : &current, definition,
                              source_lock, &generated);
  if (status != ARCHBIRD_OK)
    return status;
  previous = named_field(&execution->operand_definitions, &generated);
  if (previous) {
    *name = &previous->name;
    ab_string_free(execution->engine, &generated);
    return ARCHBIRD_OK;
  }
  if (execution->operand_definitions.as.object.count >=
          execution->operand_capacity ||
      execution->projection_binding_count >= execution->operand_capacity) {
    ab_string_free(execution->engine, &generated);
    return ARCHBIRD_LIMIT_EXCEEDED;
  }
  status = ab_projection_plan_compile(execution->engine, definition, &generated,
                                      &plan);
  if (status == ARCHBIRD_OK) {
    stored = &execution->operand_definitions.as.object
                  .fields[execution->operand_definitions.as.object.count];
    status = ab_string_copy(execution->engine, &stored->name, generated.data,
                            generated.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_value_copy(execution->engine, &stored->value, &plan.definition);
  }
  if (status == ARCHBIRD_OK && source_lock)
    status = prepared_add_source_lock(execution, stored, source_lock);
  if (status == ARCHBIRD_OK) {
    binding =
        &execution->projection_bindings[execution->projection_binding_count];
    status = ab_string_copy(execution->engine, &binding->name, generated.data,
                            generated.length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(execution->engine, &binding->map_id,
                            map_id ? map_id->data : current.data,
                            map_id ? map_id->length : current.length);
  if (status == ARCHBIRD_OK && source_lock)
    status =
        ab_value_copy(execution->engine, &binding->source_lock, source_lock);
  if (status == ARCHBIRD_OK) {
    binding->plan = plan;
    memset(&plan, 0, sizeof(plan));
  }
  if (status != ARCHBIRD_OK) {
    if (stored) {
      ab_string_free(execution->engine, &stored->name);
      ab_value_free(execution->engine, &stored->value);
    }
    if (binding) {
      ab_string_free(execution->engine, &binding->name);
      ab_string_free(execution->engine, &binding->map_id);
      ab_value_free(execution->engine, &binding->source_lock);
      ab_projection_plan_free(execution->engine, &binding->plan);
    }
    ab_projection_plan_free(execution->engine, &plan);
    ab_string_free(execution->engine, &generated);
    return status;
  }
  execution->operand_definitions.as.object.count++;
  execution->projection_binding_count++;
  *name = &stored->name;
  ab_projection_plan_free(execution->engine, &plan);
  ab_string_free(execution->engine, &generated);
  return status;
}

static ArchbirdStatus literal_spec(ArchbirdEngine *engine,
                                   const AbValue *literal, int relation_hint,
                                   AbValue *out) {
  const char *kind;
  const char *field;
  size_t index;
  if (literal->kind == AB_VALUE_OBJECT) {
    kind = "literal_values";
    field = "values";
  } else if (literal->kind == AB_VALUE_ARRAY) {
    int relation = relation_hint;
    if (literal->as.array.count) {
      relation = literal->as.array.items[0].kind == AB_VALUE_OBJECT;
      for (index = 0; index < literal->as.array.count; index++)
        if ((literal->as.array.items[index].kind == AB_VALUE_OBJECT) !=
            relation)
          return invalid(engine, "literal array mixes incompatible values");
    }
    kind = relation ? "literal_relation" : "literal_set";
    field = relation ? "rows" : "values";
  } else {
    return invalid(engine, "literal operand must be a string array, value "
                           "object, or relation array");
  }
  TRY(object_init(engine, out, 2));
  TRY(object_add_string(engine, out, "kind", kind, strlen(kind)));
  return object_add_copy(engine, out, field, literal);
}

static ArchbirdStatus add_literal_operand(ConstraintExecution *execution,
                                          const AbValue *literal,
                                          int relation_hint,
                                          const AbString **name) {
  AbValue spec = {0};
  AbString generated = {0};
  const AbObjectField *previous;
  AbObjectField *stored;
  ArchbirdStatus status =
      literal_spec(execution->engine, literal, relation_hint, &spec);
  if (status == ARCHBIRD_OK)
    status = named_digest(execution->engine, "l.", &spec, &generated);
  if (status != ARCHBIRD_OK) {
    ab_value_free(execution->engine, &spec);
    return status;
  }
  previous = named_field(&execution->operand_definitions, &generated);
  if (previous) {
    *name = &previous->name;
    ab_string_free(execution->engine, &generated);
    ab_value_free(execution->engine, &spec);
    return ARCHBIRD_OK;
  }
  if (execution->operand_definitions.as.object.count >=
      execution->operand_capacity) {
    status = ARCHBIRD_LIMIT_EXCEEDED;
    goto done;
  }
  stored = &execution->operand_definitions.as.object
                .fields[execution->operand_definitions.as.object.count];
  status = ab_string_copy(execution->engine, &stored->name, generated.data,
                          generated.length);
  if (status == ARCHBIRD_OK) {
    stored->value = spec;
    memset(&spec, 0, sizeof(spec));
  }
  if (status == ARCHBIRD_OK) {
    execution->operand_definitions.as.object.count++;
    *name = &stored->name;
  } else if (stored) {
    ab_string_free(execution->engine, &stored->name);
    ab_value_free(execution->engine, &stored->value);
  }
done:
  ab_string_free(execution->engine, &generated);
  ab_value_free(execution->engine, &spec);
  return status;
}

static ArchbirdStatus
projection_from_fields(ConstraintExecution *execution, const char *select,
                       const AbValue *definition, const char *const *fields,
                       size_t field_count, const AbValue *forced_include,
                       int default_edge_kinds, AbValue *out) {
  static const char *const edge_kinds[] = {
      "attribute-call",
      "call",
      "decorator",
      "import",
      "imported-call",
      "imported-reference",
      "member-call",
      "scip-definition",
      "scip-implementation",
      "scip-related-reference",
      "scip-type-definition",
      "semantic-call",
      "semantic-construct",
      "semantic-decorator",
      "semantic-import",
      "semantic-reference",
      "semantic-type-reference",
  };
  size_t index;
  int has_kinds = 0;
  /* Typed lowerings may append derived filters after copying authored fields.
   */
  TRY(object_init(execution->engine, out, field_count + 8));
  TRY(object_add_string(execution->engine, out, "select", select,
                        strlen(select)));
  for (index = 0; index < field_count; index++) {
    const AbValue *value = ab_value_member(definition, fields[index]);
    if (!value)
      continue;
    if (!strcmp(fields[index], "kinds"))
      has_kinds = 1;
    TRY(object_add_copy(execution->engine, out, fields[index], value));
  }
  if (forced_include)
    TRY(object_add_copy(execution->engine, out, "include", forced_include));
  if (default_edge_kinds && !has_kinds) {
    AbValue kinds = {0};
    TRY(array_init(execution->engine, &kinds,
                   sizeof(edge_kinds) / sizeof(edge_kinds[0])));
    for (index = 0; index < sizeof(edge_kinds) / sizeof(edge_kinds[0]);
         index++) {
      AbValue *item = &kinds.as.array.items[kinds.as.array.count];
      item->kind = AB_VALUE_STRING;
      TRY(ab_string_copy(execution->engine, &item->as.text, edge_kinds[index],
                         strlen(edge_kinds[index])));
      kinds.as.array.count++;
    }
    TRY(object_add_copy(execution->engine, out, "kinds", &kinds));
    ab_value_free(execution->engine, &kinds);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus check_base(ConstraintExecution *execution,
                                 const AbString *id, const AbValue *definition,
                                 AbValue *out) {
  static const char *const metadata[] = {"requirement", "tags"};
  const AbValue *owner = ab_value_member(definition, "owner");
  const AbValue *rationale = ab_value_member(definition, "rationale");
  const AbValue *severity = ab_value_member(definition, "severity");
  size_t index;
  if (!nonblank(owner) || !nonblank(rationale))
    return invalid(execution->engine,
                   "constraint owner and rationale are required");
  TRY(object_init(execution->engine, out, 20));
  TRY(object_add_string(execution->engine, out, "id", id->data, id->length));
  TRY(object_add_copy(execution->engine, out, "owner", owner));
  TRY(object_add_copy(execution->engine, out, "rationale", rationale));
  if (severity)
    TRY(object_add_copy(execution->engine, out, "severity", severity));
  else
    TRY(object_add_string(execution->engine, out, "severity", "error", 5));
  for (index = 0; index < sizeof(metadata) / sizeof(metadata[0]); index++) {
    const AbValue *value = ab_value_member(definition, metadata[index]);
    if (value)
      TRY(object_add_copy(execution->engine, out, metadata[index], value));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_fact_name(ConstraintExecution *execution,
                                    AbValue *check, const char *role,
                                    const AbString *name) {
  return object_add_string(execution->engine, check, role, name->data,
                           name->length);
}

static ArchbirdStatus copy_numeric_options(ConstraintExecution *execution,
                                           const AbValue *definition,
                                           AbValue *check) {
  static const char *const fields[] = {
      "allow_empty", "exact",           "max",
      "min",         "reference_route", "required_routes"};
  size_t index;
  for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
    const AbValue *value = ab_value_member(definition, fields[index]);
    if (value)
      TRY(object_add_copy(execution->engine, check, fields[index], value));
  }
  return ARCHBIRD_OK;
}

static const AbValue *projection_definition(ConstraintExecution *execution,
                                            const AbValue *operand) {
  const AbValue *reference = ab_value_member(operand, "projection");
  if (!reference)
    return NULL;
  if (reference->kind == AB_VALUE_OBJECT)
    return reference;
  if (reference->kind == AB_VALUE_STRING) {
    const AbObjectField *field =
        named_field(&execution->configuration.projections, &reference->as.text);
    return field ? &field->value : NULL;
  }
  return NULL;
}

static ArchbirdStatus compile_operand(ConstraintExecution *execution,
                                      const AbValue *operand, int relation_hint,
                                      const AbString **name) {
  const AbValue *projection;
  const AbValue *literal;
  const AbValue *map;
  const AbValue *observation;
  const AbValue *source_lock;
  if (!operand || operand->kind != AB_VALUE_OBJECT)
    return invalid(execution->engine, "constraint operand must be an object");
  projection = projection_definition(execution, operand);
  literal = ab_value_member(operand, "literal");
  map = ab_value_member(operand, "map");
  observation = ab_value_member(operand, "observation");
  source_lock = ab_value_member(operand, "source_lock");
  if (!!projection + !!literal + !!observation != 1)
    return invalid(execution->engine,
                   "operand requires exactly one projection, literal, or "
                   "observation");
  if (observation) {
    *name = &observation->as.text;
    return ARCHBIRD_OK;
  }
  return projection
             ? add_projection_operand(execution, projection,
                                      map ? &map->as.text : NULL, source_lock,
                                      name)
             : add_literal_operand(execution, literal, relation_hint, name);
}

static ArchbirdStatus compile_typed(ConstraintExecution *execution,
                                    const AbString *id,
                                    const AbValue *definition, AbValue *check) {
  static const char *const normalized[] = {"exclude", "include", "strip_prefix",
                                           "strip_suffix"};
  static const char *const symbols[] = {
      "exclude", "include",     "kinds",        "layer",
      "paths",   "public_only", "strip_prefix", "strip_suffix"};
  static const char *const file_edges[] = {
      "from_paths", "kind_patterns", "kinds", "name_patterns", "to_paths"};
  static const char *const component_edges[] = {"kinds"};
  static const char *const test_routes[] = {"configured_only", "group",
                                            "selectors", "target_paths"};
  const AbValue *kind = ab_value_member(definition, "kind");
  const AbString *actual = NULL;
  const AbString *expected = NULL;
  AbValue projection = {0};
  const AbValue *values;
  const char *assertion = NULL;
  ArchbirdStatus status;
  if (!nonblank(kind))
    return invalid(execution->engine, "typed constraint kind is required");
  status = check_base(execution, id, definition, check);
  if (status != ARCHBIRD_OK)
    return status;
  if (value_string_is(kind, "component_membership")) {
    status =
        projection_from_fields(execution, "component_membership", definition,
                               normalized, 4, NULL, 0, &projection);
    assertion = "numeric_bounds";
    if (status == ARCHBIRD_OK && !ab_value_member(definition, "min") &&
        !ab_value_member(definition, "max") &&
        !ab_value_member(definition, "exact"))
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "component_cycles")) {
    static const char *const fields[] = {"kinds"};
    status = projection_from_fields(execution, "component_edges", definition,
                                    fields, 1, NULL, 1, &projection);
    assertion = "acyclic";
  } else if (value_string_is(kind, "max_file_bytes")) {
    static const char *const fields[] = {"exclude", "include", "strip_prefix",
                                         "strip_suffix"};
    const AbValue *maximum = ab_value_member(definition, "max");
    if (!maximum)
      return invalid(execution->engine, "max_file_bytes requires max");
    status = projection_from_fields(execution, "file_metrics", definition,
                                    fields, 4, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_string(execution->engine, &projection, "metric",
                                 "bytes", 5);
    assertion = "numeric_bounds";
  } else if (value_string_is(kind, "required_paths") ||
             value_string_is(kind, "forbidden_paths")) {
    values = ab_value_member(definition, "paths");
    if (!values || values->kind != AB_VALUE_ARRAY || !values->as.array.count)
      return invalid(execution->engine,
                     "path constraint requires non-empty paths");
    status = projection_from_fields(
        execution,
        value_string_is(kind, "forbidden_paths") ? "inventory_paths"
                                                 : "mapped_paths",
        definition, normalized, 4,
        value_string_is(kind, "forbidden_paths") ? values : NULL, 0,
        &projection);
    if (value_string_is(kind, "forbidden_paths")) {
      assertion = "cardinality";
      if (status == ARCHBIRD_OK)
        status = object_add_u64(execution->engine, check, "exact", 0);
    } else {
      assertion = "required_subset";
      if (status == ARCHBIRD_OK)
        status = add_literal_operand(execution, values, 0, &expected);
    }
  } else if (value_string_is(kind, "required_symbols") ||
             value_string_is(kind, "forbidden_symbols")) {
    values = ab_value_member(definition, "symbols");
    if (!values || values->kind != AB_VALUE_ARRAY || !values->as.array.count)
      return invalid(execution->engine,
                     "symbol constraint requires non-empty symbols");
    status = projection_from_fields(execution, "symbols", definition, symbols,
                                    8, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "names", values);
    assertion = value_string_is(kind, "required_symbols") ? "required_subset"
                                                          : "disjoint";
    if (status == ARCHBIRD_OK)
      status = add_literal_operand(execution, values, 0, &expected);
  } else if (value_string_is(kind, "symbol_cardinality")) {
    status = projection_from_fields(execution, "symbols", definition, symbols,
                                    8, NULL, 0, &projection);
    assertion = "cardinality";
    if (!ab_value_member(definition, "min") &&
        !ab_value_member(definition, "max") &&
        !ab_value_member(definition, "exact"))
      return invalid(execution->engine,
                     "symbol_cardinality requires min, max, or exact");
  } else if (value_string_is(kind, "test_routes")) {
    status = projection_from_fields(execution, "test_routes", definition,
                                    test_routes, 4, NULL, 0, &projection);
    assertion = "min_test_routes";
    if (!ab_value_member(definition, "min") && status == ARCHBIRD_OK)
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "required_file_edge")) {
    const AbValue *edge_kind = ab_value_member(definition, "edge_kind");
    const AbValue *source = ab_value_member(definition, "source");
    const AbValue *target = ab_value_member(definition, "target");
    const AbValue *name = ab_value_member(definition, "name");
    AbValue kinds = {0};
    AbValue sources = {0};
    AbValue targets = {0};
    AbValue names = {0};
    status = singleton_string_array(execution->engine, edge_kind, &kinds);
    if (status == ARCHBIRD_OK)
      status = singleton_string_array(execution->engine, source, &sources);
    if (status == ARCHBIRD_OK)
      status = singleton_string_array(execution->engine, target, &targets);
    if (status == ARCHBIRD_OK && name)
      status = singleton_string_array(execution->engine, name, &names);
    if (status == ARCHBIRD_OK)
      status = projection_from_fields(execution, "file_edges", definition, NULL,
                                      0, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "kind_patterns",
                               &kinds);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "from_paths",
                               &sources);
    if (status == ARCHBIRD_OK)
      status =
          object_add_copy(execution->engine, &projection, "to_paths", &targets);
    if (status == ARCHBIRD_OK && name)
      status = object_add_copy(execution->engine, &projection, "name_patterns",
                               &names);
    ab_value_free(execution->engine, &names);
    ab_value_free(execution->engine, &targets);
    ab_value_free(execution->engine, &sources);
    ab_value_free(execution->engine, &kinds);
    assertion = "cardinality";
    if (status == ARCHBIRD_OK)
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "required_bridge")) {
    const AbValue *bridge = ab_value_member(definition, "bridge");
    AbValue kinds = {0};
    AbValue combined = {0};
    AbBuffer pattern;
    ab_buffer_init(&pattern, execution->engine);
    status = nonblank(bridge) ? ab_buffer_literal(&pattern, "bridge:")
                              : invalid(execution->engine,
                                        "required_bridge requires bridge");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&pattern, bridge->as.text.data,
                                bridge->as.text.length);
    if (status == ARCHBIRD_OK) {
      combined.kind = AB_VALUE_STRING;
      status = ab_string_copy(execution->engine, &combined.as.text,
                              (const char *)pattern.data, pattern.length);
    }
    if (status == ARCHBIRD_OK)
      status = singleton_string_array(execution->engine, &combined, &kinds);
    if (status == ARCHBIRD_OK)
      status = projection_from_fields(execution, "file_edges", definition, NULL,
                                      0, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "kind_patterns",
                               &kinds);
    ab_value_free(execution->engine, &kinds);
    ab_value_free(execution->engine, &combined);
    ab_buffer_free(&pattern);
    assertion = "cardinality";
    if (status == ARCHBIRD_OK)
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "required_package_entrypoint")) {
    const AbValue *package = ab_value_member(definition, "package");
    const AbValue *route = ab_value_member(definition, "route");
    const AbValue *target = ab_value_member(definition, "target");
    AbValue packages = {0};
    AbValue routes = {0};
    AbValue targets = {0};
    status = singleton_string_array(execution->engine, package, &packages);
    if (status == ARCHBIRD_OK)
      status = singleton_string_array(execution->engine, route, &routes);
    if (status == ARCHBIRD_OK && target)
      status = singleton_string_array(execution->engine, target, &targets);
    if (status == ARCHBIRD_OK)
      status =
          projection_from_fields(execution, "package_entrypoints", definition,
                                 NULL, 0, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "packages",
                               &packages);
    if (status == ARCHBIRD_OK)
      status =
          object_add_copy(execution->engine, &projection, "routes", &routes);
    if (status == ARCHBIRD_OK && target)
      status = object_add_copy(execution->engine, &projection, "target_paths",
                               &targets);
    ab_value_free(execution->engine, &targets);
    ab_value_free(execution->engine, &routes);
    ab_value_free(execution->engine, &packages);
    assertion = "cardinality";
    if (status == ARCHBIRD_OK)
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "required_test_route")) {
    const AbValue *target = ab_value_member(definition, "target");
    AbValue targets = {0};
    status = singleton_string_array(execution->engine, target, &targets);
    if (status == ARCHBIRD_OK)
      status = projection_from_fields(execution, "test_routes", definition,
                                      test_routes, 3, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "target_paths",
                               &targets);
    ab_value_free(execution->engine, &targets);
    assertion = "cardinality";
    if (status == ARCHBIRD_OK)
      status = object_add_u64(execution->engine, check, "min", 1);
  } else if (value_string_is(kind, "provider_surface")) {
    static const char *const fields[] = {"exclude", "include", "strip_prefix",
                                         "strip_suffix"};
    const AbValue *surface = ab_value_member(definition, "surface");
    if (!nonblank(surface))
      return invalid(execution->engine, "provider_surface requires surface");
    status = projection_from_fields(execution, "provider_surface", definition,
                                    fields, 4, NULL, 0, &projection);
    if (status == ARCHBIRD_OK)
      status = object_add_copy(execution->engine, &projection, "name", surface);
    assertion = "provider_surface";
    if (status == ARCHBIRD_OK) {
      static const char *const options[] = {"declared", "forbid_ambiguous",
                                            "forbid_unregistered",
                                            "forbid_unresolved"};
      size_t option;
      for (option = 0; status == ARCHBIRD_OK &&
                       option < sizeof(options) / sizeof(options[0]);
           option++) {
        const AbValue *value = ab_value_member(definition, options[option]);
        if (value)
          status =
              object_add_copy(execution->engine, check, options[option], value);
      }
    }
  } else if (value_string_is(kind, "allowed_component_edges") ||
             value_string_is(kind, "forbidden_component_edges") ||
             value_string_is(kind, "required_component_edges") ||
             value_string_is(kind, "allowed_file_edges") ||
             value_string_is(kind, "forbidden_file_edges") ||
             value_string_is(kind, "required_file_edges")) {
    int component = value_string_is(kind, "allowed_component_edges") ||
                    value_string_is(kind, "forbidden_component_edges") ||
                    value_string_is(kind, "required_component_edges");
    values = ab_value_member(definition, "edges");
    if (!values || values->kind != AB_VALUE_ARRAY)
      return invalid(execution->engine, "edge constraint requires edges");
    status = projection_from_fields(
        execution, component ? "component_edges" : "file_edges", definition,
        component ? component_edges : file_edges, component ? 1u : 5u, NULL, 1,
        &projection);
    if (status == ARCHBIRD_OK)
      status = add_literal_operand(execution, values, 1, &expected);
    assertion =
        string_is(&kind->as.text,
                  component ? "allowed_component_edges" : "allowed_file_edges")
            ? "allowed_edges"
        : string_is(&kind->as.text, component ? "forbidden_component_edges"
                                              : "forbidden_file_edges")
            ? "forbidden_edges"
            : "required_edges";
  } else {
    return invalid(execution->engine, "unsupported typed constraint kind");
  }
  if (status == ARCHBIRD_OK && projection.kind == AB_VALUE_OBJECT &&
      projection.as.object.count > 1)
    qsort(projection.as.object.fields, projection.as.object.count,
          sizeof(*projection.as.object.fields), object_field_compare);
  if (status == ARCHBIRD_OK)
    status =
        add_projection_operand(execution, &projection, NULL, NULL, &actual);
  if (status == ARCHBIRD_OK)
    status = object_add_string(execution->engine, check, "assert", assertion,
                               strlen(assertion));
  if (status == ARCHBIRD_OK)
    status = add_fact_name(execution, check, "actual", actual);
  if (status == ARCHBIRD_OK && expected)
    status = add_fact_name(execution, check, "expected", expected);
  if (status == ARCHBIRD_OK)
    status = copy_numeric_options(execution, definition, check);
  ab_value_free(execution->engine, &projection);
  return status;
}

static ArchbirdStatus add_mapping(ConstraintExecution *execution,
                                  const AbValue *mapping,
                                  const AbString **name) {
  AbValue normalized = {0};
  AbString generated = {0};
  AbObjectField *stored;
  const AbObjectField *previous;
  ArchbirdStatus status = object_init(execution->engine, &normalized, 1);
  if (status == ARCHBIRD_OK)
    status = object_add_copy(execution->engine, &normalized,
                             "actual_to_expected", mapping);
  if (status == ARCHBIRD_OK)
    status = named_digest(execution->engine, "m.", &normalized, &generated);
  if (status != ARCHBIRD_OK)
    goto done;
  previous = named_field(&execution->mappings, &generated);
  if (previous) {
    *name = &previous->name;
    goto done;
  }
  if (execution->mappings.as.object.count >= execution->mapping_capacity) {
    status = ARCHBIRD_LIMIT_EXCEEDED;
    goto done;
  }
  stored = &execution->mappings.as.object
                .fields[execution->mappings.as.object.count];
  status = ab_string_copy(execution->engine, &stored->name, generated.data,
                          generated.length);
  if (status == ARCHBIRD_OK) {
    stored->value = normalized;
    memset(&normalized, 0, sizeof(normalized));
    execution->mappings.as.object.count++;
    *name = &stored->name;
  }
done:
  ab_string_free(execution->engine, &generated);
  ab_value_free(execution->engine, &normalized);
  return status;
}

static ArchbirdStatus compile_primitive(ConstraintExecution *execution,
                                        const AbString *id,
                                        const AbValue *definition,
                                        AbValue *check) {
  const AbValue *assertion = ab_value_member(definition, "assert");
  const AbValue *actual_operand = ab_value_member(definition, "actual");
  const AbValue *expected_operand = ab_value_member(definition, "expected");
  const AbValue *mapping = ab_value_member(definition, "mapping");
  const AbString *actual = NULL;
  const AbString *expected = NULL;
  const AbString *mapping_name = NULL;
  int relation = value_string_is(assertion, "required_edges") ||
                 value_string_is(assertion, "forbidden_edges") ||
                 value_string_is(assertion, "allowed_edges") ||
                 value_string_is(assertion, "acyclic") ||
                 value_string_is(assertion, "min_test_routes");
  ArchbirdStatus status;
  if (!nonblank(assertion) || !actual_operand)
    return invalid(execution->engine,
                   "primitive constraint requires assert and actual");
  status = check_base(execution, id, definition, check);
  if (status == ARCHBIRD_OK)
    status = compile_operand(execution, actual_operand, relation, &actual);
  if (status == ARCHBIRD_OK && expected_operand)
    status = compile_operand(execution, expected_operand, relation, &expected);
  if (status == ARCHBIRD_OK)
    status = object_add_copy(execution->engine, check, "assert", assertion);
  if (status == ARCHBIRD_OK)
    status = add_fact_name(execution, check, "actual", actual);
  if (status == ARCHBIRD_OK && expected)
    status = add_fact_name(execution, check, "expected", expected);
  if (status == ARCHBIRD_OK && mapping)
    status = add_mapping(execution, mapping, &mapping_name);
  if (status == ARCHBIRD_OK && mapping_name)
    status = add_fact_name(execution, check, "mapping", mapping_name);
  if (status == ARCHBIRD_OK)
    status = copy_numeric_options(execution, definition, check);
  return status;
}

static int requested(ConstraintExecution *execution, const AbString *id) {
  const AbValue *ids = ab_value_member(&execution->request, "ids");
  size_t index;
  if (!ids)
    return 1;
  for (index = 0; index < ids->as.array.count; index++)
    if (ab_string_equal(&ids->as.array.items[index].as.text, id))
      return 1;
  return 0;
}

static ArchbirdStatus validate_map_artifact(ConstraintExecution *execution,
                                            const AbValue *map,
                                            const char *where) {
  const AbValue *schema = ab_value_member(map, "schema_version");
  const AbValue *evidence = ab_value_member(map, "evidence");
  const AbValue *tool = ab_value_member(map, "tool");
  uint64_t version = 0;
  if (!map || map->kind != AB_VALUE_OBJECT ||
      !value_string_is(ab_value_member(map, "artifact"), "map") ||
      !ab_value_u64(schema, &version) || version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT ||
      !nonblank(ab_value_member(map, "project")) || !evidence ||
      evidence->kind != AB_VALUE_OBJECT ||
      !lowercase_sha256(ab_value_member(evidence, "config_sha256")) ||
      !lowercase_sha256(ab_value_member(evidence, "input_sha256")) || !tool ||
      tool->kind != AB_VALUE_OBJECT ||
      !lowercase_sha256(ab_value_member(tool, "implementation_sha256")))
    return archbird_error_set(
        execution->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "constraints: %s is not a canonical saved Map", where);
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_request_maps(ConstraintExecution *execution) {
  const AbValue *maps = ab_value_member(&execution->request, "maps");
  size_t index;
  ArchbirdStatus status;
  if (!maps)
    return ARCHBIRD_OK;
  if (maps->kind != AB_VALUE_OBJECT)
    return invalid(execution->engine, "request.maps must be an object");
  for (index = 0; index < maps->as.object.count; index++) {
    const AbObjectField *field = &maps->as.object.fields[index];
    const AbValue *map;
    const AbValue *resolution;
    size_t member;
    if (!stable_id(&field->name) || string_is(&field->name, "current") ||
        field->value.kind != AB_VALUE_OBJECT)
      return invalid(execution->engine,
                     "request.maps keys must be stable IDs other than current");
    for (member = 0; member < field->value.as.object.count; member++) {
      const AbString *name = &field->value.as.object.fields[member].name;
      if (!string_is(name, "map") && !string_is(name, "resolution"))
        return invalid(execution->engine,
                       "request.maps entries accept only map and resolution");
    }
    map = ab_value_member(&field->value, "map");
    resolution = ab_value_member(&field->value, "resolution");
    status = validate_map_artifact(execution, map, "request Map");
    if (status != ARCHBIRD_OK)
      return status;
    if (resolution) {
      if (resolution->kind != AB_VALUE_OBJECT)
        return invalid(execution->engine,
                       "request Map resolution must be an object");
      status = ab_projection_resolution_validate(
          execution->engine, resolution, map, "constraint request resolution");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_request(ConstraintExecution *execution,
                                       size_t *selected) {
  static const char *const allowed[] = {"baseline", "ids", "maps",
                                        "observations", "policy_date"};
  const AbValue *ids;
  size_t index;
  *selected = execution->configuration.constraints.as.object.count;
  if (execution->request.kind == AB_VALUE_NULL)
    return ARCHBIRD_OK;
  if (execution->request.kind != AB_VALUE_OBJECT)
    return invalid(execution->engine, "request must be an object");
  for (index = 0; index < execution->request.as.object.count; index++) {
    const AbString *name = &execution->request.as.object.fields[index].name;
    if (!string_is(name, allowed[0]) && !string_is(name, allowed[1]) &&
        !string_is(name, allowed[2]) && !string_is(name, allowed[3]) &&
        !string_is(name, allowed[4]))
      return invalid(execution->engine,
                     "request accepts only baseline, ids, maps, observations, "
                     "and policy_date");
  }
  {
    const AbValue *policy_date =
        ab_value_member(&execution->request, "policy_date");
    if (policy_date && (policy_date->kind != AB_VALUE_STRING ||
                        !ab_iso_date_valid(&policy_date->as.text)))
      return invalid(execution->engine,
                     "request.policy_date must be a valid YYYY-MM-DD date");
  }
  {
    const AbValue *observations =
        ab_value_member(&execution->request, "observations");
    if (observations && observations->kind != AB_VALUE_OBJECT)
      return invalid(execution->engine,
                     "request.observations must be an object");
  }
  ids = ab_value_member(&execution->request, "ids");
  if (!ids)
    return ARCHBIRD_OK;
  if (ids->kind != AB_VALUE_ARRAY || !ids->as.array.count)
    return invalid(execution->engine, "request.ids must be a non-empty array");
  *selected = ids->as.array.count;
  for (index = 0; index < ids->as.array.count; index++) {
    const AbValue *id = &ids->as.array.items[index];
    size_t previous;
    if (!nonblank(id) || !ab_value_member(&execution->configuration.constraints,
                                          id->as.text.data))
      return invalid(execution->engine, "request names an unknown constraint");
    for (previous = 0; previous < index; previous++)
      if (ab_string_equal(&ids->as.array.items[previous].as.text, &id->as.text))
        return invalid(execution->engine,
                       "request contains a duplicate constraint id");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus executable_digest(ConstraintExecution *execution,
                                        const AbValue *check, char output[65]) {
  static const char *const metadata[] = {"id",          "owner",    "rationale",
                                         "requirement", "severity", "tags"};
  AbValue plan = {0};
  size_t index;
  ArchbirdStatus status =
      object_init(execution->engine, &plan, check->as.object.count);
  for (index = 0; status == ARCHBIRD_OK && index < check->as.object.count;
       index++) {
    const AbObjectField *field = &check->as.object.fields[index];
    size_t ignored;
    int skip = 0;
    for (ignored = 0; ignored < sizeof(metadata) / sizeof(metadata[0]);
         ignored++)
      if (string_is(&field->name, metadata[ignored])) {
        skip = 1;
        break;
      }
    if (!skip)
      status = object_add_named_copy(execution->engine, &plan, &field->name,
                                     &field->value);
  }
  if (status == ARCHBIRD_OK)
    status = value_digest(execution->engine, &plan, output);
  ab_value_free(execution->engine, &plan);
  return status;
}

static ArchbirdStatus authored_digest(ConstraintExecution *execution,
                                      const AbValue *definition,
                                      char output[65]) {
  AbValue authored = {0};
  size_t index;
  ArchbirdStatus status =
      object_init(execution->engine, &authored, definition->as.object.count);
  for (index = 0; status == ARCHBIRD_OK && index < definition->as.object.count;
       index++) {
    const AbObjectField *field = &definition->as.object.fields[index];
    status = object_add_named_copy(execution->engine, &authored, &field->name,
                                   &field->value);
  }
  if (status == ARCHBIRD_OK)
    status = value_digest(execution->engine, &authored, output);
  ab_value_free(execution->engine, &authored);
  return status;
}

static ArchbirdStatus compile_constraints(ConstraintExecution *execution,
                                          size_t selected) {
  AbValue identities = {0};
  AbValue requested_ids = {0};
  size_t configured = execution->configuration.constraints.as.object.count;
  size_t index;
  ArchbirdStatus status = array_init(execution->engine, &identities, selected);
  if (status == ARCHBIRD_OK)
    status = array_init(execution->engine, &requested_ids, selected);
  for (index = 0; status == ARCHBIRD_OK && index < configured; index++) {
    const AbObjectField *constraint =
        &execution->configuration.constraints.as.object.fields[index];
    AbValue check = {0};
    AbValue identity = {0};
    AbValue requested_id = {0};
    char definition_sha[65];
    char plan_sha[65];
    const AbValue *kind;
    if (!requested(execution, &constraint->name))
      continue;
    kind = ab_value_member(&constraint->value, "kind");
    status = kind ? compile_typed(execution, &constraint->name,
                                  &constraint->value, &check)
                  : compile_primitive(execution, &constraint->name,
                                      &constraint->value, &check);
    if (status == ARCHBIRD_OK && check.as.object.count > 1)
      qsort(check.as.object.fields, check.as.object.count,
            sizeof(*check.as.object.fields), object_field_compare);
    if (status == ARCHBIRD_OK)
      status = authored_digest(execution, &constraint->value, definition_sha);
    if (status == ARCHBIRD_OK)
      status = executable_digest(execution, &check, plan_sha);
    if (status == ARCHBIRD_OK)
      status = object_init(execution->engine, &identity, 4);
    if (status == ARCHBIRD_OK)
      status =
          object_add_string(execution->engine, &identity, "id",
                            constraint->name.data, constraint->name.length);
    if (status == ARCHBIRD_OK)
      status =
          object_add_string(execution->engine, &identity,
                            "constraint_definition_sha256", definition_sha, 64);
    if (status == ARCHBIRD_OK)
      status = object_add_string(execution->engine, &identity,
                                 "constraint_plan_sha256", plan_sha, 64);
    if (status == ARCHBIRD_OK)
      qsort(identity.as.object.fields, identity.as.object.count,
            sizeof(*identity.as.object.fields), object_field_compare);
    if (status == ARCHBIRD_OK && ab_value_member(&execution->request, "ids")) {
      requested_id.kind = AB_VALUE_STRING;
      status = ab_string_copy(execution->engine, &requested_id.as.text,
                              constraint->name.data, constraint->name.length);
    }
    if (status == ARCHBIRD_OK) {
      execution->constraint_plans.as.array
          .items[execution->constraint_plans.as.array.count++] = check;
      memset(&check, 0, sizeof(check));
      identities.as.array.items[identities.as.array.count++] = identity;
      memset(&identity, 0, sizeof(identity));
      if (requested_id.kind != AB_VALUE_NULL) {
        requested_ids.as.array.items[requested_ids.as.array.count++] =
            requested_id;
        memset(&requested_id, 0, sizeof(requested_id));
      }
    }
    ab_value_free(execution->engine, &requested_id);
    ab_value_free(execution->engine, &identity);
    ab_value_free(execution->engine, &check);
  }
  if (status == ARCHBIRD_OK &&
      execution->constraint_plans.as.array.count != selected)
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK &&
      execution->operand_definitions.as.object.count > 1)
    qsort(execution->operand_definitions.as.object.fields,
          execution->operand_definitions.as.object.count,
          sizeof(*execution->operand_definitions.as.object.fields),
          object_field_compare);
  if (status == ARCHBIRD_OK && execution->mappings.as.object.count > 1)
    qsort(execution->mappings.as.object.fields,
          execution->mappings.as.object.count,
          sizeof(*execution->mappings.as.object.fields), object_field_compare);
  if (status == ARCHBIRD_OK)
    status = object_init(execution->engine, &execution->policy, 10);
  if (status == ARCHBIRD_OK)
    status = object_add_u64(execution->engine, &execution->policy,
                            "configured_count", configured);
  if (status == ARCHBIRD_OK)
    status = object_add_string(
        execution->engine, &execution->policy, "constraint_policy_sha256",
        execution->configuration.constraint_policy_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = object_add_copy(execution->engine, &execution->policy,
                             "constraints", &identities);
  if (status == ARCHBIRD_OK)
    status = object_add_u64(execution->engine, &execution->policy,
                            "evaluated_count", selected);
  if (status == ARCHBIRD_OK)
    status = object_add_string(
        execution->engine, &execution->policy, "kind",
        ab_value_member(&execution->request, "ids") ? "selected" : "all",
        ab_value_member(&execution->request, "ids") ? 8 : 3);
  if (status == ARCHBIRD_OK)
    status = object_add_u64(execution->engine, &execution->policy,
                            "omitted_count", configured - selected);
  if (status == ARCHBIRD_OK &&
      ab_value_member(&execution->request, "policy_date"))
    status =
        object_add_copy(execution->engine, &execution->policy, "policy_date",
                        ab_value_member(&execution->request, "policy_date"));
  if (status == ARCHBIRD_OK)
    status = object_add_copy(
        execution->engine, &execution->policy, "project",
        ab_value_member(&execution->configuration.normalized, "project"));
  if (status == ARCHBIRD_OK)
    status = object_add_string(execution->engine, &execution->policy,
                               "project_configuration_sha256",
                               execution->configuration.sha256, 64);
  if (status == ARCHBIRD_OK)
    status = object_add_copy(execution->engine, &execution->policy,
                             "requested_ids", &requested_ids);
  if (status == ARCHBIRD_OK && execution->policy.as.object.count > 1)
    qsort(execution->policy.as.object.fields, execution->policy.as.object.count,
          sizeof(*execution->policy.as.object.fields), object_field_compare);
  ab_value_free(execution->engine, &requested_ids);
  ab_value_free(execution->engine, &identities);
  return status;
}

static ArchbirdStatus
evaluate_projection_binding(ConstraintExecution *execution,
                            ConstraintProjectionBinding *binding) {
  const AbValue *map = NULL;
  const AbValue *resolution = NULL;
  AbProjectionResult result = {0};
  ArchbirdStatus status =
      constraint_map(execution, &binding->map_id, &map, &resolution);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(execution->engine, &binding->plan, map,
                                         resolution, &result);
  if (status == ARCHBIRD_OK && binding->source_lock.kind == AB_VALUE_OBJECT) {
    AbBuffer message;
    int current = 0;
    ab_buffer_init(&message, execution->engine);
    status =
        source_lock_current(map, &binding->source_lock, &current, &message);
    if (status == ARCHBIRD_OK && !current) {
      const AbValue *project = ab_value_member(map, "project");
      char shape[32];
      size_t shape_length = result.data.shape.length;
      if (shape_length >= sizeof(shape))
        status = ARCHBIRD_CONFLICT;
      else {
        memcpy(shape, result.data.shape.data, shape_length);
        shape[shape_length] = '\0';
        status = ab_buffer_append(&message, "\0", 1);
        if (status == ARCHBIRD_OK) {
          ab_projection_data_free(execution->engine, &result.data);
          status = ab_projection_data_unknown(
              execution->engine, &result.data, &binding->name,
              project ? &project->as.text : NULL, shape,
              message.data ? (const char *)message.data
                           : "source lock is not current");
        }
      }
    }
    ab_buffer_free(&message);
  }
  if (status == ARCHBIRD_OK) {
    execution->context.facts[execution->context.fact_count++] = result.data;
    memset(&result.data, 0, sizeof(result.data));
  }
  ab_projection_result_free(execution->engine, &result);
  return status;
}

static ArchbirdStatus evaluate_operands(ConstraintExecution *execution) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t count = execution->operand_definitions.as.object.count;
  if (count) {
    execution->context.facts = (AbProjectionData *)ab_calloc(
        execution->engine, count, sizeof(*execution->context.facts));
    if (!execution->context.facts)
      return archbird_error_set(execution->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory evaluating constraint operands");
  }
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbObjectField *operand =
        &execution->operand_definitions.as.object.fields[index];
    if (operand->name.length >= 2 && operand->name.data[0] == 'l' &&
        operand->name.data[1] == '.') {
      status = ab_projection_extract_literal(
          execution->engine, operand,
          &execution->context.facts[execution->context.fact_count]);
      if (status == ARCHBIRD_OK)
        execution->context.fact_count++;
    }
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < execution->projection_binding_count;
       index++)
    status = evaluate_projection_binding(
        execution, &execution->projection_bindings[index]);
  if (status == ARCHBIRD_OK && execution->context.fact_count != count)
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK && execution->context.fact_count > 1)
    qsort(execution->context.facts, execution->context.fact_count,
          sizeof(*execution->context.facts), fact_compare);
  return status;
}

static ArchbirdStatus
validate_observation_operands(ConstraintExecution *execution) {
  size_t index;
  for (index = 0; index < execution->constraint_plans.as.array.count; index++) {
    const AbValue *constraint =
        &execution->constraint_plans.as.array.items[index];
    const AbValue *assertion = ab_value_member(constraint, "assert");
    const AbValue *actual;
    const AbValue *expected;
    if (!value_string_is(assertion, "observations_equal"))
      continue;
    actual = ab_value_member(constraint, "actual");
    expected = ab_value_member(constraint, "expected");
    if (!actual || actual->kind != AB_VALUE_STRING || !expected ||
        expected->kind != AB_VALUE_STRING ||
        !ab_constraints_observation_find(&execution->context,
                                         &actual->as.text) ||
        !ab_constraints_observation_find(&execution->context,
                                         &expected->as.text))
      return invalid(execution->engine,
                     "constraint references an unsupplied observation");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
execution_prepare(ConstraintExecution *execution, const uint8_t *config_json,
                  size_t config_length, const uint8_t *map_json,
                  size_t map_length, const uint8_t *resolution_json,
                  size_t resolution_length, const uint8_t *request_json,
                  size_t request_length) {
  const AbValue *project;
  size_t selected = 0;
  ArchbirdStatus status;
  status = ab_project_configuration_decode(
      execution->engine, config_json, config_length, &execution->configuration);
  if (status == ARCHBIRD_OK && request_length)
    status = ab_json_value_decode(execution->engine, request_json,
                                  request_length, &execution->request);
  if (status != ARCHBIRD_OK)
    return status;
  status = validate_request(execution, &selected);
  if (status != ARCHBIRD_OK)
    return status;
  if (!selected)
    return invalid(execution->engine,
                   "project configuration defines no constraints");
  execution->operand_capacity = selected * 2;
  execution->mapping_capacity = selected;
  status = object_init(execution->engine, &execution->operand_definitions,
                       execution->operand_capacity);
  if (status == ARCHBIRD_OK)
    status = object_init(execution->engine, &execution->mappings,
                         execution->mapping_capacity);
  if (status == ARCHBIRD_OK)
    status =
        array_init(execution->engine, &execution->constraint_plans, selected);
  if (status == ARCHBIRD_OK) {
    execution->projection_bindings = (ConstraintProjectionBinding *)ab_calloc(
        execution->engine, execution->operand_capacity,
        sizeof(*execution->projection_bindings));
    if (!execution->projection_bindings)
      status = archbird_error_set(
          execution->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory compiling constraint projection bindings");
  }
  if (status != ARCHBIRD_OK)
    return status;
  execution->context.engine = execution->engine;
  status = compile_constraints(execution, selected);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(execution->engine, map_json, map_length,
                                  &execution->map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(execution->engine, resolution_json,
                                  resolution_length, &execution->resolution);
  if (status == ARCHBIRD_OK)
    status = validate_map_artifact(execution, &execution->map, "current Map");
  if (status == ARCHBIRD_OK) {
    const AbValue *map_evidence = ab_value_member(&execution->map, "evidence");
    const AbValue *map_config =
        map_evidence ? ab_value_member(map_evidence, "config_sha256") : NULL;
    if (!map_config || map_config->as.text.length != 64 ||
        memcmp(map_config->as.text.data,
               execution->configuration.map_config_sha256, 64) != 0)
      status = invalid(
          execution->engine,
          "project configuration Map definition does not match current Map");
  }
  if (status == ARCHBIRD_OK && execution->resolution.kind == AB_VALUE_OBJECT)
    status = ab_projection_resolution_validate(
        execution->engine, &execution->resolution, &execution->map,
        "constraint resolution");
  if (status == ARCHBIRD_OK)
    status = validate_request_maps(execution);
  execution->context.current_map = &execution->map;
  execution->context.current_resolution =
      execution->resolution.kind == AB_VALUE_OBJECT ? &execution->resolution
                                                    : NULL;
  if (status == ARCHBIRD_OK)
    status = evaluate_operands(execution);
  if (status != ARCHBIRD_OK)
    return status;
  project = ab_value_member(&execution->configuration.normalized, "project");
  execution->context.project_configuration =
      &execution->configuration.normalized;
  execution->context.project = project;
  execution->context.description =
      ab_value_member(&execution->configuration.normalized, "description");
  execution->context.operand_definitions = &execution->operand_definitions;
  execution->context.mappings = &execution->mappings;
  execution->context.constraint_plans = &execution->constraint_plans;
  execution->context.constraint_policy = &execution->policy;
  execution->context.policy_date =
      ab_value_member(&execution->request, "policy_date");
  execution->context.baseline_input =
      ab_value_member(&execution->request, "baseline");
  execution->context.additional_maps =
      ab_value_member(&execution->request, "maps");
  memcpy(execution->context.constraint_policy_sha256,
         execution->configuration.constraint_policy_sha256, 65);
  status = ab_constraints_observations_load(
      &execution->context,
      ab_value_member(&execution->request, "observations"));
  return status == ARCHBIRD_OK ? validate_observation_operands(execution)
                               : status;
}

static ArchbirdStatus result_identities(ConstraintExecution *execution) {
  AbValue *identities =
      (AbValue *)ab_value_member(&execution->policy, "constraints");
  size_t index;
  if (!identities || identities->kind != AB_VALUE_ARRAY ||
      identities->as.array.count != execution->context.check_count)
    return ARCHBIRD_CONFLICT;
  for (index = 0; index < execution->context.check_count; index++) {
    AbBuffer rendered;
    ArchbirdSha256Context digest_context;
    uint8_t digest[32];
    char sha[65];
    AbValue *identity = &identities->as.array.items[index];
    AbValue completed = {0};
    size_t field_index;
    ArchbirdStatus status;
    ab_buffer_init(&rendered, execution->engine);
    status =
        ab_verify_check_render(&rendered, &execution->context.checks[index]);
    if (status == ARCHBIRD_OK) {
      const AbValue *definition =
          ab_value_member(identity, "constraint_definition_sha256");
      const AbValue *plan = ab_value_member(identity, "constraint_plan_sha256");
      static const uint8_t separator = 0;
      archbird_sha256_init(&digest_context);
      status = archbird_sha256_update(
          &digest_context, (const uint8_t *)"archbird-constraint-result-v1",
          29);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(&digest_context, &separator, 1);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(
            &digest_context, (const uint8_t *)definition->as.text.data,
            definition->as.text.length);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(&digest_context, &separator, 1);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(&digest_context,
                                        (const uint8_t *)plan->as.text.data,
                                        plan->as.text.length);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(&digest_context, &separator, 1);
      if (status == ARCHBIRD_OK)
        status = archbird_sha256_update(&digest_context, rendered.data,
                                        rendered.length);
      if (status == ARCHBIRD_OK)
        archbird_sha256_final(&digest_context, digest);
    }
    if (status == ARCHBIRD_OK) {
      archbird_sha256_hex(digest, sha);
      status = object_init(execution->engine, &completed,
                           identity->as.object.count + 1);
    }
    for (field_index = 0;
         status == ARCHBIRD_OK && field_index < identity->as.object.count;
         field_index++) {
      const AbObjectField *field = &identity->as.object.fields[field_index];
      status = object_add_named_copy(execution->engine, &completed,
                                     &field->name, &field->value);
    }
    if (status == ARCHBIRD_OK)
      status = object_add_string(execution->engine, &completed,
                                 "constraint_result_sha256", sha, 64);
    if (status == ARCHBIRD_OK) {
      ab_value_free(execution->engine, identity);
      *identity = completed;
      memset(&completed, 0, sizeof(completed));
      qsort(identity->as.object.fields, identity->as.object.count,
            sizeof(*identity->as.object.fields), object_field_compare);
    }
    ab_value_free(execution->engine, &completed);
    ab_buffer_free(&rendered);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static const AbValue *nested_member(const AbValue *object, const char *first,
                                    const char *second) {
  const AbValue *value = ab_value_member(object, first);
  return value && value->kind == AB_VALUE_OBJECT
             ? ab_value_member(value, second)
             : NULL;
}

static const AbValue *waiver_input_digest(AbVerificationContext *context,
                                          const AbString *map_id) {
  static const AbString current = {(char *)"current", 7};
  const AbValue *map = NULL;
  if (ab_string_equal(map_id, &current)) {
    map = context->current_map;
  } else {
    const AbObjectField *field = named_field(context->additional_maps, map_id);
    if (field)
      map = ab_value_member(&field->value, "map");
  }
  return map ? nested_member(map, "evidence", "input_sha256") : NULL;
}

static ArchbirdStatus waiver_note_input(AbBuffer *note,
                                        const AbString *map_id) {
  TRY(ab_buffer_literal(note,
                        "review boundary changed or is unavailable for Map "));
  return ab_buffer_append(note, map_id->data, map_id->length);
}

static ArchbirdStatus waiver_active(AbVerificationContext *context,
                                    const AbValue *waiver, int *active,
                                    AbBuffer *note) {
  const AbValue *expires = ab_value_member(waiver, "expires_on");
  const AbValue *until_inputs = ab_value_member(waiver, "until_inputs");
  size_t index;
  *active = 1;
  if (expires) {
    if (!context->policy_date)
      return invalid(context->engine,
                     "date-bound waiver requires request.policy_date");
    if (ab_iso_date_compare(&context->policy_date->as.text, &expires->as.text) >
        0) {
      *active = 0;
      TRY(ab_buffer_literal(note, "expired on "));
      TRY(ab_buffer_append(note, expires->as.text.data,
                           expires->as.text.length));
      TRY(ab_buffer_literal(note, " at policy date "));
      return ab_buffer_append(note, context->policy_date->as.text.data,
                              context->policy_date->as.text.length);
    }
  }
  for (index = 0;
       *active && until_inputs && index < until_inputs->as.object.count;
       index++) {
    const AbObjectField *boundary = &until_inputs->as.object.fields[index];
    const AbValue *actual = waiver_input_digest(context, &boundary->name);
    if (!actual ||
        !ab_string_equal(&actual->as.text, &boundary->value.as.text)) {
      *active = 0;
      return waiver_note_input(note, &boundary->name);
    }
  }
  return ARCHBIRD_OK;
}

static int waiver_matches(const AbValue *waiver,
                          const AbVerifyFinding *finding) {
  const AbValue *fingerprint = ab_value_member(waiver, "fingerprint");
  if (fingerprint)
    return ab_string_equal(&fingerprint->as.text, &finding->fingerprint);
  return ab_string_equal(&ab_value_member(waiver, "comparison")->as.text,
                         &finding->comparison) &&
         ab_string_equal(&ab_value_member(waiver, "key")->as.text,
                         &finding->key);
}

static ArchbirdStatus waiver_diagnostic(AbVerificationContext *context,
                                        const char *severity, const char *code,
                                        const AbValue *waiver,
                                        const char *suffix,
                                        size_t suffix_length) {
  const AbValue *id = ab_value_member(waiver, "id");
  AbBuffer message;
  ArchbirdStatus status;
  ab_buffer_init(&message, context->engine);
  status = ab_buffer_literal(&message, "waiver ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, id->as.text.data, id->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, ": ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, suffix, suffix_length);
  if (status == ARCHBIRD_OK)
    status = ab_verify_add_diagnostic(context, severity, code,
                                      (const char *)message.data,
                                      message.length, "", 0);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus apply_constraint_waivers(AbVerificationContext *context) {
  const AbValue *constraints =
      ab_value_member(context->project_configuration, "constraints");
  size_t check_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (check_index = 0;
       status == ARCHBIRD_OK && check_index < context->check_count;
       check_index++) {
    AbVerifyCheckResult *result = &context->checks[check_index];
    const AbValue *check_id = ab_value_member(result->spec, "id");
    const AbObjectField *definition =
        named_field(constraints, &check_id->as.text);
    const AbValue *waivers =
        definition ? ab_value_member(&definition->value, "waivers") : NULL;
    unsigned char *used = NULL;
    size_t waiver_count = waivers ? waivers->as.array.count : 0;
    size_t finding_index;
    size_t waiver_index;
    if (waiver_count) {
      used = (unsigned char *)ab_calloc(context->engine, waiver_count, 1);
      if (!used)
        return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory applying constraint waivers");
    }
    for (waiver_index = 0; status == ARCHBIRD_OK && waiver_index < waiver_count;
         waiver_index++)
      if (ab_value_member(&waivers->as.array.items[waiver_index],
                          "expires_on") &&
          !context->policy_date)
        status = invalid(context->engine,
                         "date-bound waiver requires request.policy_date");
    for (finding_index = 0;
         status == ARCHBIRD_OK && finding_index < result->finding_count;
         finding_index++) {
      AbVerifyFinding *finding = &result->findings[finding_index];
      size_t first = 0;
      size_t matches = 0;
      for (waiver_index = 0; waiver_index < waiver_count; waiver_index++)
        if (waiver_matches(&waivers->as.array.items[waiver_index], finding)) {
          if (!matches)
            first = waiver_index;
          used[waiver_index] = 1;
          matches++;
        }
      if (matches > 1) {
        static const char text[] = "matches the same finding as another waiver";
        status = waiver_diagnostic(context, "error", "ambiguous-waiver",
                                   &waivers->as.array.items[first], text,
                                   sizeof(text) - 1);
      } else if (matches == 1) {
        const AbValue *waiver = &waivers->as.array.items[first];
        const AbValue *id = ab_value_member(waiver, "id");
        AbBuffer note;
        int active = 0;
        ab_buffer_init(&note, context->engine);
        status = waiver_active(context, waiver, &active, &note);
        if (status == ARCHBIRD_OK) {
          ab_string_free(context->engine, &finding->waiver);
          status = ab_string_copy(context->engine, &finding->waiver,
                                  id->as.text.data, id->as.text.length);
        }
        if (status == ARCHBIRD_OK && active) {
          ab_string_free(context->engine, &finding->disposition);
          status = ab_string_copy(context->engine, &finding->disposition,
                                  "waived", 6);
        } else if (status == ARCHBIRD_OK) {
          ab_string_free(context->engine, &finding->waiver_note);
          status = ab_string_copy(context->engine, &finding->waiver_note,
                                  (const char *)note.data, note.length);
          if (status == ARCHBIRD_OK)
            status =
                waiver_diagnostic(context, "warning", "waiver-inactive", waiver,
                                  (const char *)note.data, note.length);
        }
        ab_buffer_free(&note);
      }
    }
    for (waiver_index = 0; status == ARCHBIRD_OK && waiver_index < waiver_count;
         waiver_index++)
      if (!used[waiver_index]) {
        static const char text[] = "matches no current finding";
        status = waiver_diagnostic(context, "warning", "unused-waiver",
                                   &waivers->as.array.items[waiver_index], text,
                                   sizeof(text) - 1);
      }
    if (status == ARCHBIRD_OK)
      status = ab_verify_check_refresh_status(context->engine, result);
    ab_free(context->engine, used);
  }
  return status;
}

static ArchbirdStatus render_required_value(ConstraintExecution *execution,
                                            AbBuffer *buffer,
                                            const AbValue *value,
                                            const char *message) {
  return value ? ab_value_render(buffer, value)
               : invalid(execution->engine, message);
}

static ArchbirdStatus render_evaluation_row(ConstraintExecution *execution,
                                            AbBuffer *buffer,
                                            const AbString *id,
                                            const AbValue *map,
                                            const AbValue *resolution) {
  const AbValue *map_config = nested_member(map, "evidence", "config_sha256");
  const AbValue *map_input = nested_member(map, "evidence", "input_sha256");
  const AbValue *implementation =
      nested_member(map, "tool", "implementation_sha256");
  const AbValue *project = ab_value_member(map, "project");
  const AbValue *resolution_sha =
      resolution ? ab_value_member(resolution, "sha256") : NULL;
  TRY(ab_buffer_literal(buffer, "{\"id\":"));
  TRY(ab_buffer_json_string(buffer, id->data, id->length));
  TRY(ab_buffer_literal(buffer, ",\"map_config_sha256\":"));
  TRY(render_required_value(execution, buffer, map_config,
                            "Map is missing config_sha256"));
  TRY(ab_buffer_literal(buffer, ",\"map_input_sha256\":"));
  TRY(render_required_value(execution, buffer, map_input,
                            "Map is missing input_sha256"));
  TRY(ab_buffer_literal(buffer, ",\"map_producer_implementation_sha256\":"));
  TRY(render_required_value(execution, buffer, implementation,
                            "Map is missing producer implementation_sha256"));
  TRY(ab_buffer_literal(buffer, ",\"project\":"));
  TRY(render_required_value(execution, buffer, project,
                            "Map is missing project identity"));
  TRY(ab_buffer_literal(buffer, ",\"resolution_sha256\":"));
  if (resolution_sha)
    TRY(ab_value_render(buffer, resolution_sha));
  else
    TRY(ab_buffer_literal(buffer, "null"));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_evaluations(ConstraintExecution *execution,
                                         AbBuffer *buffer) {
  static const AbString current = {(char *)"current", 7};
  const AbValue *maps = ab_value_member(&execution->request, "maps");
  size_t index;
  int emitted = 0;
  TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; maps && index < maps->as.object.count; index++) {
    const AbObjectField *field = &maps->as.object.fields[index];
    if (ab_string_compare(&field->name, &current) >= 0)
      break;
    if (emitted++)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(render_evaluation_row(execution, buffer, &field->name,
                              ab_value_member(&field->value, "map"),
                              ab_value_member(&field->value, "resolution")));
  }
  if (emitted++)
    TRY(ab_buffer_literal(buffer, ","));
  TRY(render_evaluation_row(execution, buffer, &current, &execution->map,
                            execution->resolution.kind == AB_VALUE_OBJECT
                                ? &execution->resolution
                                : NULL));
  for (; maps && index < maps->as.object.count; index++) {
    const AbObjectField *field = &maps->as.object.fields[index];
    TRY(ab_buffer_literal(buffer, ","));
    TRY(render_evaluation_row(execution, buffer, &field->name,
                              ab_value_member(&field->value, "map"),
                              ab_value_member(&field->value, "resolution")));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_constraint_result(ConstraintExecution *execution,
                                               AbBuffer *buffer) {
  AbBuffer base;
  uint8_t digest[32];
  char result_sha256[65];
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&base, execution->engine);
  status = ab_buffer_literal(
      &base, "{\"artifact\":\"verification\",\"constraints\":[");
  for (index = 0;
       status == ARCHBIRD_OK && index < execution->context.check_count;
       index++) {
    if (index)
      status = ab_buffer_literal(&base, ",");
    if (status == ARCHBIRD_OK)
      status = ab_verify_check_render(&base, &execution->context.checks[index]);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, "],\"diagnostics\":");
  if (status == ARCHBIRD_OK)
    status = ab_verify_render_diagnostics(&execution->context, &base);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"evaluations\":");
  if (status == ARCHBIRD_OK)
    status = render_evaluations(execution, &base);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"mappings\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&base, &execution->mappings);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"operand_definitions\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&base, &execution->operand_definitions);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"observations\":");
  if (status == ARCHBIRD_OK)
    status = ab_constraints_observations_render(&execution->context, &base);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"operands\":[");
  for (index = 0;
       status == ARCHBIRD_OK && index < execution->context.fact_count;
       index++) {
    if (index)
      status = ab_buffer_literal(&base, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_data_render(&base, &execution->context.facts[index], 1);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, "],\"policy\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&base, &execution->policy);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&base, ",\"schema_version\":2,\"summary\":");
  if (status == ARCHBIRD_OK)
    status = ab_constraints_render_summary(&execution->context, &base);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &base,
        ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
        "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(base.data, base.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, result_sha256);
  if (status == ARCHBIRD_OK &&
      (!base.length || base.data[base.length - 1] != '}'))
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, base.data, base.length - 1);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"verification_result_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, result_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  ab_buffer_free(&base);
  return status;
}

static void execution_free(ConstraintExecution *execution) {
  size_t index;
  if (!execution)
    return;
  ab_verification_context_free(&execution->context);
  for (index = 0; index < execution->projection_binding_count; index++) {
    ConstraintProjectionBinding *binding =
        &execution->projection_bindings[index];
    ab_projection_plan_free(execution->engine, &binding->plan);
    ab_value_free(execution->engine, &binding->source_lock);
    ab_string_free(execution->engine, &binding->map_id);
    ab_string_free(execution->engine, &binding->name);
  }
  ab_free(execution->engine, execution->projection_bindings);
  ab_value_free(execution->engine, &execution->policy);
  ab_value_free(execution->engine, &execution->constraint_plans);
  ab_value_free(execution->engine, &execution->mappings);
  ab_value_free(execution->engine, &execution->operand_definitions);
  ab_value_free(execution->engine, &execution->request);
  ab_value_free(execution->engine, &execution->resolution);
  ab_value_free(execution->engine, &execution->map);
  ab_project_configuration_free(execution->engine, &execution->configuration);
}

ArchbirdStatus archbird_constraints_evaluate(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  ConstraintExecution execution = {0};
  AbBuffer result;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  execution.engine = engine;
  ab_buffer_init(&result, engine);
  status = execution_prepare(&execution, config_json, config_length, map_json,
                             map_length, resolution_json, resolution_length,
                             request_json, request_length);
  if (status == ARCHBIRD_OK)
    status = ab_verify_collect_project_diagnostics(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evaluate_checks(&execution.context);
  if (status == ARCHBIRD_OK)
    status = apply_constraint_waivers(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_apply_baseline(&execution.context);
  if (status == ARCHBIRD_OK)
    status = result_identities(&execution);
  if (status == ARCHBIRD_OK)
    ab_verify_diagnostics_finish(&execution.context);
  if (status == ARCHBIRD_OK)
    status = render_constraint_result(&execution, &result);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, result.data, result.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&result);
  execution_free(&execution);
  return status;
}

ArchbirdStatus archbird_constraints_report(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, ArchbirdVerificationFormat format,
    size_t max_findings, uint32_t json_flags, ArchbirdWriteFn write_fn,
    void *user_data) {
  ConstraintExecution execution = {0};
  AbBuffer result;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !write_fn ||
      format < ARCHBIRD_VERIFICATION_MARKDOWN ||
      format > ARCHBIRD_VERIFICATION_JUNIT ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  execution.engine = engine;
  ab_buffer_init(&result, engine);
  status = execution_prepare(&execution, config_json, config_length, map_json,
                             map_length, resolution_json, resolution_length,
                             request_json, request_length);
  if (status == ARCHBIRD_OK)
    status = ab_verify_collect_project_diagnostics(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evaluate_checks(&execution.context);
  if (status == ARCHBIRD_OK)
    status = apply_constraint_waivers(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_apply_baseline(&execution.context);
  if (status == ARCHBIRD_OK)
    status = result_identities(&execution);
  if (status == ARCHBIRD_OK)
    ab_verify_diagnostics_finish(&execution.context);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_MARKDOWN)
    status = ab_constraints_render_markdown(&execution.context, &result,
                                            max_findings);
  else if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_SARIF)
    status = ab_constraints_render_sarif(&execution.context, &result);
  else if (status == ARCHBIRD_OK)
    status = ab_constraints_render_junit(&execution.context, &result);
  if (status == ARCHBIRD_OK && format == ARCHBIRD_VERIFICATION_SARIF)
    status = archbird_json_canonicalize(engine, result.data, result.length,
                                        json_flags, write_fn, user_data);
  else if (status == ARCHBIRD_OK &&
           write_fn(user_data, result.data, result.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "constraint report callback failed");
  ab_buffer_free(&result);
  execution_free(&execution);
  return status;
}

ArchbirdStatus archbird_constraints_freeze(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const uint8_t *request_json,
    size_t request_length, const char *owner, size_t owner_length,
    const char *rationale, size_t rationale_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  ConstraintExecution execution = {0};
  AbBuffer result;
  ArchbirdStatus status;
  if (!engine || !config_json || !config_length || !map_json || !map_length ||
      (!resolution_json && resolution_length) ||
      (!request_json && request_length) || !owner || !owner_length ||
      !rationale || !rationale_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  execution.engine = engine;
  ab_buffer_init(&result, engine);
  status = execution_prepare(&execution, config_json, config_length, map_json,
                             map_length, resolution_json, resolution_length,
                             request_json, request_length);
  if (status == ARCHBIRD_OK && ab_value_member(&execution.request, "ids"))
    status =
        invalid(engine, "freezing requires the complete constraint policy");
  if (status == ARCHBIRD_OK)
    status = ab_verify_collect_project_diagnostics(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evaluate_checks(&execution.context);
  if (status == ARCHBIRD_OK)
    status = apply_constraint_waivers(&execution.context);
  if (status == ARCHBIRD_OK)
    status = ab_constraints_apply_baseline(&execution.context);
  if (status == ARCHBIRD_OK)
    status = result_identities(&execution);
  if (status == ARCHBIRD_OK)
    ab_verify_diagnostics_finish(&execution.context);
  if (status == ARCHBIRD_OK)
    status =
        ab_constraints_render_baseline(&execution.context, owner, owner_length,
                                       rationale, rationale_length, &result);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, result.data, result.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&result);
  execution_free(&execution);
  return status;
}

#undef TRY
