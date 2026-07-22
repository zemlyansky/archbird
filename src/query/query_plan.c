#include <archbird/archbird.h>

#include "../projection/projection_internal.h"
#include "json_value.h"
#include "query_internal.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "query plan: %s", message);
}

static int string_is(const AbString *value, const char *literal) {
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

static int field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *const *left = (const AbString *const *)left_raw;
  const AbString *const *right = (const AbString *const *)right_raw;
  return ab_string_compare(*left, *right);
}

static ArchbirdStatus object_init(ArchbirdEngine *engine, AbValue *out,
                                  size_t count) {
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_OBJECT;
  out->as.object.count = count;
  if (!count)
    return ARCHBIRD_OK;
  if (count > SIZE_MAX / sizeof(*out->as.object.fields))
    return ARCHBIRD_LIMIT_EXCEEDED;
  out->as.object.fields =
      (AbObjectField *)ab_calloc(engine, count, sizeof(*out->as.object.fields));
  if (!out->as.object.fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling query plan");
  return ARCHBIRD_OK;
}

static ArchbirdStatus field_name(ArchbirdEngine *engine, AbObjectField *field,
                                 const char *name) {
  return ab_string_copy(engine, &field->name, name, strlen(name));
}

static ArchbirdStatus field_copy(ArchbirdEngine *engine, AbObjectField *field,
                                 const char *name, const AbValue *value) {
  ArchbirdStatus status = field_name(engine, field, name);
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &field->value, value);
  return status;
}

static ArchbirdStatus field_string(ArchbirdEngine *engine, AbObjectField *field,
                                   const char *name, const char *value) {
  ArchbirdStatus status = field_name(engine, field, name);
  if (status == ARCHBIRD_OK) {
    field->value.kind = AB_VALUE_STRING;
    status =
        ab_string_copy(engine, &field->value.as.text, value, strlen(value));
  }
  return status;
}

static ArchbirdStatus field_text(ArchbirdEngine *engine, AbObjectField *field,
                                 const char *name, const char *value,
                                 size_t value_length) {
  ArchbirdStatus status = field_name(engine, field, name);
  if (status == ARCHBIRD_OK) {
    field->value.kind = AB_VALUE_STRING;
    status = ab_string_copy(engine, &field->value.as.text, value, value_length);
  }
  return status;
}

static ArchbirdStatus field_integer(ArchbirdEngine *engine,
                                    AbObjectField *field, const char *name,
                                    const char *value) {
  ArchbirdStatus status = field_name(engine, field, name);
  if (status == ARCHBIRD_OK) {
    field->value.kind = AB_VALUE_INTEGER;
    status =
        ab_string_copy(engine, &field->value.as.text, value, strlen(value));
  }
  return status;
}

static const AbObjectField *named_field(const AbValue *collection,
                                        const AbString *id) {
  size_t index;
  if (!collection || collection->kind != AB_VALUE_OBJECT)
    return NULL;
  for (index = 0; index < collection->as.object.count; index++)
    if (ab_string_equal(&collection->as.object.fields[index].name, id))
      return &collection->as.object.fields[index];
  return NULL;
}

static ArchbirdStatus digest_value(ArchbirdEngine *engine, const AbValue *value,
                                   char out[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_value_render(&buffer, value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus digest_buffer(const AbBuffer *buffer, char out[65]) {
  uint8_t digest[32];
  ArchbirdStatus status = archbird_sha256(buffer->data, buffer->length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  return status;
}

static ArchbirdStatus normalized_string_union(ArchbirdEngine *engine,
                                              const AbValue *left,
                                              const AbValue *right,
                                              AbValue *out) {
  const AbString **items = NULL;
  size_t left_count = left ? left->as.array.count : 0;
  size_t right_count = right ? right->as.array.count : 0;
  size_t capacity = left_count + right_count;
  size_t index;
  size_t count = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if ((left && left->kind != AB_VALUE_ARRAY) ||
      (right && right->kind != AB_VALUE_ARRAY))
    return invalid(engine, "query selectors must be arrays of strings");
  if (capacity) {
    if (capacity > SIZE_MAX / sizeof(*items))
      return ARCHBIRD_LIMIT_EXCEEDED;
    items = (const AbString **)ab_malloc(engine, capacity * sizeof(*items));
    if (!items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory normalizing query selectors");
  }
  for (index = 0; index < left_count; index++) {
    if (left->as.array.items[index].kind != AB_VALUE_STRING) {
      status = invalid(engine, "query selectors must be arrays of strings");
      goto done;
    }
    items[count++] = &left->as.array.items[index].as.text;
  }
  for (index = 0; index < right_count; index++) {
    if (right->as.array.items[index].kind != AB_VALUE_STRING) {
      status = invalid(engine, "query selectors must be arrays of strings");
      goto done;
    }
    items[count++] = &right->as.array.items[index].as.text;
  }
  if (count > 1)
    qsort(items, count, sizeof(*items), string_pointer_compare);
  {
    size_t unique = 0;
    for (index = 0; index < count; index++)
      if (!index || !ab_string_equal(items[index - 1], items[index]))
        unique++;
    memset(out, 0, sizeof(*out));
    out->kind = AB_VALUE_ARRAY;
    out->as.array.count = unique;
    if (unique) {
      out->as.array.items =
          (AbValue *)ab_calloc(engine, unique, sizeof(*out->as.array.items));
      if (!out->as.array.items) {
        status = archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory normalizing query selectors");
        goto done;
      }
    }
    unique = 0;
    for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
      AbValue *item;
      if (index && ab_string_equal(items[index - 1], items[index]))
        continue;
      item = &out->as.array.items[unique++];
      item->kind = AB_VALUE_STRING;
      status = ab_string_copy(engine, &item->as.text, items[index]->data,
                              items[index]->length);
    }
  }
done:
  ab_free(engine, items);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus merge_objects(ArchbirdEngine *engine, const AbValue *base,
                                    const AbValue *overrides, AbValue *out) {
  size_t left = 0;
  size_t right = 0;
  size_t output = 0;
  size_t capacity;
  ArchbirdStatus status;
  if ((base && base->kind != AB_VALUE_OBJECT) ||
      (overrides && overrides->kind != AB_VALUE_OBJECT))
    return invalid(engine, "query context must be an object");
  capacity = (base ? base->as.object.count : 0) +
             (overrides ? overrides->as.object.count : 0);
  status = object_init(engine, out, capacity);
  while (status == ARCHBIRD_OK &&
         (left < (base ? base->as.object.count : 0) ||
          right < (overrides ? overrides->as.object.count : 0))) {
    const AbObjectField *field;
    if (left >= (base ? base->as.object.count : 0)) {
      field = &overrides->as.object.fields[right++];
    } else if (right >= (overrides ? overrides->as.object.count : 0)) {
      field = &base->as.object.fields[left++];
    } else {
      int compared =
          ab_string_compare(&base->as.object.fields[left].name,
                            &overrides->as.object.fields[right].name);
      if (compared < 0) {
        field = &base->as.object.fields[left++];
      } else {
        field = &overrides->as.object.fields[right++];
        if (!compared)
          left++;
      }
    }
    status = ab_string_copy(engine, &out->as.object.fields[output].name,
                            field->name.data, field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &out->as.object.fields[output].value,
                             &field->value);
    output++;
  }
  out->as.object.count = output;
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static int override_allowed(const AbString *name) {
  static const char *const allowed[] = {
      "artifacts", "components",   "context",  "depth",
      "direction", "focus",        "packages", "paths",
      "search",    "search_limit", "symbols",  "test_depth",
  };
  size_t index;
  for (index = 0; index < sizeof(allowed) / sizeof(allowed[0]); index++)
    if (string_is(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus build_options(ArchbirdEngine *engine,
                                    const AbValue *definition,
                                    const AbValue *overrides, AbValue *out) {
  static const char *const arrays[] = {"artifacts", "components", "focus",
                                       "packages",  "paths",      "search",
                                       "symbols"};
  static const struct {
    const char *name;
    const char *default_value;
    int integer;
  } scalars[] = {{"depth", "1", 1},
                 {"direction", "both", 0},
                 {"search_limit", "8", 1},
                 {"test_depth", "8", 1}};
  size_t index;
  size_t output = 0;
  ArchbirdStatus status;
  for (index = 0; index < overrides->as.object.count; index++)
    if (!override_allowed(&overrides->as.object.fields[index].name))
      return invalid(engine, "runtime overrides contain an unknown field");
  status = object_init(engine, out, 12);
  for (index = 0;
       status == ARCHBIRD_OK && index < sizeof(arrays) / sizeof(arrays[0]);
       index++) {
    AbObjectField *field = &out->as.object.fields[output++];
    status = field_name(engine, field, arrays[index]);
    if (status == ARCHBIRD_OK)
      status = normalized_string_union(
          engine, ab_value_member(definition, arrays[index]),
          ab_value_member(overrides, arrays[index]), &field->value);
  }
  if (status == ARCHBIRD_OK) {
    AbObjectField *field = &out->as.object.fields[output++];
    status = field_name(engine, field, "context");
    if (status == ARCHBIRD_OK)
      status =
          merge_objects(engine, ab_value_member(definition, "context"),
                        ab_value_member(overrides, "context"), &field->value);
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < sizeof(scalars) / sizeof(scalars[0]);
       index++) {
    const AbValue *value = ab_value_member(overrides, scalars[index].name);
    if (!value)
      value = ab_value_member(definition, scalars[index].name);
    if (value)
      status = field_copy(engine, &out->as.object.fields[output++],
                          scalars[index].name, value);
    else if (scalars[index].integer)
      status = field_integer(engine, &out->as.object.fields[output++],
                             scalars[index].name, scalars[index].default_value);
    else
      status = field_string(engine, &out->as.object.fields[output++],
                            scalars[index].name, scalars[index].default_value);
  }
  if (status == ARCHBIRD_OK && output != out->as.object.count)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK)
    qsort(out->as.object.fields, out->as.object.count,
          sizeof(*out->as.object.fields), field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus copy_selected_fields(ArchbirdEngine *engine,
                                           const AbValue *source,
                                           const char *const *names,
                                           size_t count, AbValue *out) {
  size_t index;
  ArchbirdStatus status = object_init(engine, out, count);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *value = ab_value_member(source, names[index]);
    if (!value) {
      status = invalid(engine, "normalized query option is missing");
      break;
    }
    status =
        field_copy(engine, &out->as.object.fields[index], names[index], value);
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->as.object.fields, count, sizeof(*out->as.object.fields),
          field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus build_plan_sections(ArchbirdEngine *engine,
                                          const AbValue *options,
                                          AbValue *selection,
                                          AbValue *operations) {
  static const char *const selection_fields[] = {
      "artifacts", "components", "focus", "packages", "paths", "symbols"};
  static const char *const operation_fields[] = {
      "context", "depth", "direction", "search", "search_limit", "test_depth"};
  ArchbirdStatus status = copy_selected_fields(
      engine, options, selection_fields,
      sizeof(selection_fields) / sizeof(selection_fields[0]), selection);
  if (status == ARCHBIRD_OK)
    status = copy_selected_fields(
        engine, options, operation_fields,
        sizeof(operation_fields) / sizeof(operation_fields[0]), operations);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, selection);
  return status;
}

static ArchbirdStatus render_projection_plan(AbBuffer *buffer,
                                             const AbProjectionPlan *plan) {
  TRY(ab_buffer_literal(buffer, "{\"id\":"));
  TRY(ab_buffer_json_string(buffer, plan->id.data, plan->id.length));
  TRY(ab_buffer_literal(buffer, ",\"operation\":"));
  TRY(ab_value_render(buffer, &plan->definition));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, plan->definition_sha256, 64));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus inline_projection_id(ArchbirdEngine *engine,
                                           const AbString *query_id,
                                           const char sha256[65],
                                           AbString *out);

static int projection_plan_compare(const void *left_raw,
                                   const void *right_raw) {
  const AbProjectionPlan *left = (const AbProjectionPlan *)left_raw;
  const AbProjectionPlan *right = (const AbProjectionPlan *)right_raw;
  int compared = strcmp(left->definition_sha256, right->definition_sha256);
  return compared ? compared : ab_string_compare(&left->id, &right->id);
}

static void projection_plans_free(ArchbirdEngine *engine,
                                  AbProjectionPlan *plans, size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_projection_plan_free(engine, &plans[index]);
  ab_free(engine, plans);
}

static ArchbirdStatus
projection_plan_add(ArchbirdEngine *engine, const AbValue *definition,
                    const AbString *declared_id, const AbString *query_id,
                    AbProjectionPlan *nodes, size_t capacity, size_t *count) {
  char definition_sha256[65] = {0};
  AbString node_id = {0};
  size_t previous;
  ArchbirdStatus status =
      ab_projection_definition_sha256(engine, definition, definition_sha256);
  for (previous = 0; status == ARCHBIRD_OK && previous < *count; previous++)
    if (!strcmp(nodes[previous].definition_sha256, definition_sha256))
      return ARCHBIRD_OK;
  if (status != ARCHBIRD_OK)
    return status;
  if (*count >= capacity)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many inferred query projections");
  if (declared_id)
    status = ab_string_copy(engine, &node_id, declared_id->data,
                            declared_id->length);
  else
    status =
        inline_projection_id(engine, query_id, definition_sha256, &node_id);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_compile(engine, definition, &node_id,
                                        &nodes[*count]);
  if (status == ARCHBIRD_OK)
    (*count)++;
  ab_string_free(engine, &node_id);
  return status;
}

static ArchbirdStatus inferred_definition(ArchbirdEngine *engine,
                                          const char *select,
                                          const char *first_name,
                                          const AbValue *first,
                                          const char *second_name,
                                          const AbValue *second, AbValue *out) {
  size_t count = 1 + (first ? 1u : 0u) + (second ? 1u : 0u);
  size_t output = 0;
  ArchbirdStatus status = object_init(engine, out, count);
  if (status == ARCHBIRD_OK && first)
    status =
        field_copy(engine, &out->as.object.fields[output++], first_name, first);
  if (status == ARCHBIRD_OK && second)
    status = field_copy(engine, &out->as.object.fields[output++], second_name,
                        second);
  if (status == ARCHBIRD_OK)
    status = field_string(engine, &out->as.object.fields[output++], "select",
                          select);
  if (status == ARCHBIRD_OK && output != count)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->as.object.fields, count, sizeof(*out->as.object.fields),
          field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus
inferred_projection_add(ArchbirdEngine *engine, const char *select,
                        const char *first_name, const AbValue *first,
                        const char *second_name, const AbValue *second,
                        const AbString *query_id, AbProjectionPlan *nodes,
                        size_t capacity, size_t *count) {
  AbValue definition = {0};
  ArchbirdStatus status = inferred_definition(engine, select, first_name, first,
                                              second_name, second, &definition);
  if (status == ARCHBIRD_OK)
    status = projection_plan_add(engine, &definition, NULL, query_id, nodes,
                                 capacity, count);
  ab_value_free(engine, &definition);
  return status;
}

static ArchbirdStatus inline_projection_id(ArchbirdEngine *engine,
                                           const AbString *query_id,
                                           const char sha256[65],
                                           AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "query.");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, query_id->data, query_id->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, sha256, 12);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus
prepare_projection_plans(ArchbirdEngine *engine,
                         const AbValue *configured_projections,
                         const AbValue *definition, const AbValue *selection,
                         const AbValue *operations, const AbString *query_id,
                         AbProjectionPlan **out_nodes, size_t *out_count) {
  const AbValue *references = ab_value_member(definition, "projection");
  const AbValue *paths = ab_value_member(selection, "paths");
  const AbValue *symbols = ab_value_member(selection, "symbols");
  const AbValue *components = ab_value_member(selection, "components");
  const AbValue *packages = ab_value_member(selection, "packages");
  const AbValue *artifacts = ab_value_member(selection, "artifacts");
  const AbValue *focus = ab_value_member(selection, "focus");
  const AbValue *search = ab_value_member(operations, "search");
  AbProjectionPlan *nodes = NULL;
  size_t reference_count = !references ? 0
                           : references->kind == AB_VALUE_ARRAY
                               ? references->as.array.count
                               : 1;
  size_t capacity;
  size_t node_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_nodes = NULL;
  *out_count = 0;
  if (reference_count > SIZE_MAX - 17 ||
      reference_count + 17 > SIZE_MAX / sizeof(*nodes))
    return ARCHBIRD_LIMIT_EXCEEDED;
  capacity = reference_count + 17;
  nodes = (AbProjectionPlan *)ab_calloc(engine, capacity, sizeof(*nodes));
  if (!nodes)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling query projections");
  for (index = 0; status == ARCHBIRD_OK && index < reference_count; index++) {
    const AbValue *reference = references->kind == AB_VALUE_ARRAY
                                   ? &references->as.array.items[index]
                                   : references;
    const AbValue *definition = NULL;
    const AbString *declared_id = NULL;
    const AbObjectField *named = NULL;
    if (reference->kind == AB_VALUE_STRING) {
      named = named_field(configured_projections, &reference->as.text);
      if (!named) {
        status =
            invalid(engine, "named query references an unknown projection");
        break;
      }
      definition = &named->value;
      declared_id = &named->name;
    } else if (reference->kind == AB_VALUE_OBJECT) {
      const AbValue *id = ab_value_member(reference, "id");
      definition = reference;
      if (id && id->kind == AB_VALUE_STRING)
        declared_id = &id->as.text;
    } else {
      status = invalid(engine, "query projection reference is invalid");
      break;
    }
    if (status == ARCHBIRD_OK)
      status = projection_plan_add(engine, definition, declared_id, query_id,
                                   nodes, capacity, &node_count);
  }
  if (status == ARCHBIRD_OK && paths && paths->as.array.count)
    status =
        inferred_projection_add(engine, "mapped_paths", "paths", paths, NULL,
                                NULL, query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && symbols && symbols->as.array.count)
    status = inferred_projection_add(engine, "symbols", "name_patterns",
                                     symbols, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && symbols && symbols->as.array.count)
    status = inferred_projection_add(engine, "package_exports", "name_patterns",
                                     symbols, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && components && components->as.array.count)
    status = inferred_projection_add(engine, "component_membership",
                                     "components", components, NULL, NULL,
                                     query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && packages && packages->as.array.count)
    status = inferred_projection_add(engine, "package_entrypoints", "packages",
                                     packages, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && artifacts && artifacts->as.array.count)
    status = inferred_projection_add(engine, "artifact_routes", "artifacts",
                                     artifacts, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status =
        inferred_projection_add(engine, "mapped_paths", "paths", focus, NULL,
                                NULL, query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status =
        inferred_projection_add(engine, "symbols", "name_patterns", focus, NULL,
                                NULL, query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "component_membership",
                                     "components", focus, NULL, NULL, query_id,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "package_entrypoints", "packages",
                                     focus, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "package_exports", "name_patterns",
                                     focus, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "artifact_routes", "artifacts",
                                     focus, NULL, NULL, query_id, nodes,
                                     capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status =
        inferred_projection_add(engine, "test_routes", "paths", focus, NULL,
                                NULL, query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status =
        inferred_projection_add(engine, "test_routes", "selectors", focus, NULL,
                                NULL, query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && search && search->as.array.count)
    status =
        inferred_projection_add(engine, "search_domain", NULL, NULL, NULL, NULL,
                                query_id, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && node_count > 1)
    qsort(nodes, node_count, sizeof(*nodes), projection_plan_compare);
  if (status != ARCHBIRD_OK) {
    projection_plans_free(engine, nodes, node_count);
    return status;
  }
  *out_nodes = nodes;
  *out_count = node_count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
projection_plan_values(ArchbirdEngine *engine,
                       const AbProjectionPlan *projections,
                       size_t projection_count, AbValue *out) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_ARRAY;
  out->as.array.count = projection_count;
  if (projection_count) {
    if (projection_count > SIZE_MAX / sizeof(*out->as.array.items))
      return ARCHBIRD_LIMIT_EXCEEDED;
    out->as.array.items = (AbValue *)ab_calloc(engine, projection_count,
                                               sizeof(*out->as.array.items));
    if (!out->as.array.items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory compiling query plan");
  }
  for (index = 0; status == ARCHBIRD_OK && index < projection_count; index++) {
    AbValue *row = &out->as.array.items[index];
    status = object_init(engine, row, 3);
    if (status == ARCHBIRD_OK)
      status =
          field_text(engine, &row->as.object.fields[0], "id",
                     projections[index].id.data, projections[index].id.length);
    if (status == ARCHBIRD_OK)
      status = field_copy(engine, &row->as.object.fields[1], "operation",
                          &projections[index].definition);
    if (status == ARCHBIRD_OK)
      status = field_string(engine, &row->as.object.fields[2],
                            "projection_definition_sha256",
                            projections[index].definition_sha256);
  }
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus query_plan_value(
    ArchbirdEngine *engine, const AbString *id, const char *map_config_sha256,
    const char *project_configuration_sha256, const AbValue *operations,
    const AbValue *selection, const AbProjectionPlan *projections,
    size_t projection_count, const char query_definition_sha256[65],
    const char query_plan_sha256[65], AbValue *out) {
  ArchbirdStatus status = object_init(engine, out, 8);
  if (status == ARCHBIRD_OK)
    status = field_text(engine, &out->as.object.fields[0], "id", id->data,
                        id->length);
  if (status == ARCHBIRD_OK && map_config_sha256)
    status = field_string(engine, &out->as.object.fields[1],
                          "map_config_sha256", map_config_sha256);
  else if (status == ARCHBIRD_OK) {
    status = field_name(engine, &out->as.object.fields[1], "map_config_sha256");
    out->as.object.fields[1].value.kind = AB_VALUE_NULL;
  }
  if (status == ARCHBIRD_OK)
    status =
        field_copy(engine, &out->as.object.fields[2], "operations", operations);
  if (status == ARCHBIRD_OK && project_configuration_sha256)
    status = field_string(engine, &out->as.object.fields[3],
                          "project_configuration_sha256",
                          project_configuration_sha256);
  else if (status == ARCHBIRD_OK) {
    status = field_name(engine, &out->as.object.fields[3],
                        "project_configuration_sha256");
    out->as.object.fields[3].value.kind = AB_VALUE_NULL;
  }
  if (status == ARCHBIRD_OK) {
    status = field_name(engine, &out->as.object.fields[4], "projections");
    if (status == ARCHBIRD_OK)
      status = projection_plan_values(engine, projections, projection_count,
                                      &out->as.object.fields[4].value);
  }
  if (status == ARCHBIRD_OK)
    status = field_string(engine, &out->as.object.fields[5],
                          "query_definition_sha256", query_definition_sha256);
  if (status == ARCHBIRD_OK)
    status = field_string(engine, &out->as.object.fields[6],
                          "query_plan_sha256", query_plan_sha256);
  if (status == ARCHBIRD_OK)
    status =
        field_copy(engine, &out->as.object.fields[7], "selection", selection);
  if (status == ARCHBIRD_OK)
    qsort(out->as.object.fields, out->as.object.count,
          sizeof(*out->as.object.fields), field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus ad_hoc_definition(ArchbirdEngine *engine,
                                        const AbValue *request, AbValue *out) {
  size_t index;
  size_t count = 0;
  size_t output = 0;
  ArchbirdStatus status;
  if (!request || request->kind != AB_VALUE_OBJECT)
    return invalid(engine, "query request must be an object");
  for (index = 0; index < request->as.object.count; index++) {
    const AbObjectField *field = &request->as.object.fields[index];
    if (string_is(&field->name, "change_set") ||
        string_is(&field->name, "producer_policy"))
      continue;
    if (!override_allowed(&field->name))
      return invalid(engine, "query request contains an unknown field");
    count++;
  }
  status = object_init(engine, out, count);
  for (index = 0; status == ARCHBIRD_OK && index < request->as.object.count;
       index++) {
    const AbObjectField *field = &request->as.object.fields[index];
    AbObjectField *target;
    if (string_is(&field->name, "change_set") ||
        string_is(&field->name, "producer_policy"))
      continue;
    target = &out->as.object.fields[output++];
    status = ab_string_copy(engine, &target->name, field->name.data,
                            field->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &target->value, &field->value);
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->as.object.fields, count, sizeof(*out->as.object.fields),
          field_compare);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

ArchbirdStatus ab_query_plan_compile_ad_hoc(ArchbirdEngine *engine,
                                            const AbValue *request,
                                            AbValue *out_plan) {
  const AbValue empty = {.kind = AB_VALUE_OBJECT};
  AbString id = {(char *)"ad-hoc", 6};
  AbValue definition = {0};
  ArchbirdStatus status;
  if (!engine || !request || !out_plan)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out_plan, 0, sizeof(*out_plan));
  status = ad_hoc_definition(engine, request, &definition);
  if (status == ARCHBIRD_OK)
    status = ab_query_plan_compile_definition(engine, &id, &definition, &empty,
                                              &empty, NULL, NULL, out_plan);
  ab_value_free(engine, &definition);
  return status;
}

ArchbirdStatus ab_query_plan_compile_definition(
    ArchbirdEngine *engine, const AbString *id, const AbValue *definition,
    const AbValue *overrides, const AbValue *configured_projections,
    const char *map_config_sha256, const char *project_configuration_sha256,
    AbValue *out_plan) {
  AbValue options = {0};
  AbValue plan_selection = {0};
  AbValue plan_operations = {0};
  AbValue plan = {0};
  AbProjectionPlan *projections = NULL;
  size_t projection_count = 0;
  size_t projection_index;
  char query_definition_sha256[65] = {0};
  char query_plan_sha256[65] = {0};
  AbBuffer plan_base;
  ArchbirdStatus status;
  if (!engine || !id || !stable_id(id) || !definition ||
      definition->kind != AB_VALUE_OBJECT || !overrides ||
      overrides->kind != AB_VALUE_OBJECT || !configured_projections ||
      configured_projections->kind != AB_VALUE_OBJECT || !out_plan)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out_plan, 0, sizeof(*out_plan));
  ab_buffer_init(&plan_base, engine);
  status = build_options(engine, definition, overrides, &options);
  if (status == ARCHBIRD_OK)
    status = build_plan_sections(engine, &options, &plan_selection,
                                 &plan_operations);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, definition, query_definition_sha256);
  if (status == ARCHBIRD_OK)
    status = prepare_projection_plans(
        engine, configured_projections, definition, &plan_selection,
        &plan_operations, id, &projections, &projection_count);
  if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(&plan_base, "{\"operations\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&plan_base, &plan_operations);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&plan_base, ",\"projections\":[");
    for (projection_index = 0;
         status == ARCHBIRD_OK && projection_index < projection_count;
         projection_index++) {
      if (projection_index)
        status = ab_buffer_literal(&plan_base, ",");
      if (status == ARCHBIRD_OK)
        status =
            render_projection_plan(&plan_base, &projections[projection_index]);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&plan_base, "],\"selection\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&plan_base, &plan_selection);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&plan_base, "}");
    if (status == ARCHBIRD_OK)
      status = digest_buffer(&plan_base, query_plan_sha256);
  }
  if (status == ARCHBIRD_OK)
    status = query_plan_value(
        engine, id, map_config_sha256, project_configuration_sha256,
        &plan_operations, &plan_selection, projections, projection_count,
        query_definition_sha256, query_plan_sha256, &plan);
  if (status == ARCHBIRD_OK) {
    *out_plan = plan;
    memset(&plan, 0, sizeof(plan));
  }
  ab_buffer_free(&plan_base);
  projection_plans_free(engine, projections, projection_count);
  ab_value_free(engine, &plan);
  ab_value_free(engine, &options);
  ab_value_free(engine, &plan_operations);
  ab_value_free(engine, &plan_selection);
  return status;
}
