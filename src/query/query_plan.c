#include <archbird/archbird.h>

#include "../configuration/project_configuration.h"
#include "../projection/projection_internal.h"
#include "json_value.h"
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

static AbObjectField *mutable_member(AbValue *object, const char *name) {
  size_t index;
  for (index = 0; index < object->as.object.count; index++)
    if (string_is(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index];
  return NULL;
}

static ArchbirdStatus append_selector(ArchbirdEngine *engine, AbValue *array,
                                      const char *data, size_t length) {
  AbValue *items;
  AbValue *item;
  if (array->as.array.count == SIZE_MAX ||
      array->as.array.count + 1 > SIZE_MAX / sizeof(*items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  items = (AbValue *)ab_realloc(engine, array->as.array.items,
                                (array->as.array.count + 1) * sizeof(*items));
  if (!items)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory expanding query projection seeds");
  array->as.array.items = items;
  item = &items[array->as.array.count];
  memset(item, 0, sizeof(*item));
  item->kind = AB_VALUE_STRING;
  TRY(ab_string_copy(engine, &item->as.text, data, length));
  array->as.array.count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_symbol_selector(ArchbirdEngine *engine,
                                             AbValue *array,
                                             const AbString *path,
                                             const AbString *symbol) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_append(&buffer, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, symbol->data, symbol->length);
  if (status == ARCHBIRD_OK)
    status = append_selector(engine, array, (const char *)buffer.data,
                             buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus
add_projection_seeds(ArchbirdEngine *engine, AbValue *options,
                     const AbValue *definition,
                     const AbProjectionEvaluation *evaluation) {
  AbValue paths = {.kind = AB_VALUE_ARRAY};
  AbValue symbols = {.kind = AB_VALUE_ARRAY};
  const AbValue *select = ab_value_member(definition, "select");
  int symbol_projection = ab_value_string_is(select, "symbols");
  int path_keys = ab_value_string_is(select, "mapped_paths") ||
                  ab_value_string_is(select, "inventory_paths");
  int relation_targets = ab_value_string_is(select, "artifact_routes") ||
                         ab_value_string_is(select, "package_entrypoints") ||
                         ab_value_string_is(select, "package_exports") ||
                         ab_value_string_is(select, "test_routes");
  size_t item_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (item_index = 0;
       status == ARCHBIRD_OK && item_index < evaluation->fact.item_count;
       item_index++) {
    const AbVerifyFactItem *item = &evaluation->fact.items[item_index];
    size_t evidence_index;
    if (relation_targets) {
      size_t attribute_index;
      for (attribute_index = 0; attribute_index < item->attribute_count;
           attribute_index++) {
        const AbObjectField *attribute = &item->attributes[attribute_index];
        if (string_is(&attribute->name, "target") &&
            attribute->value.kind == AB_VALUE_STRING) {
          status =
              append_selector(engine, &paths, attribute->value.as.text.data,
                              attribute->value.as.text.length);
          break;
        }
      }
      continue;
    }
    if (!item->evidence_count && path_keys)
      status =
          append_selector(engine, &paths, item->key.data, item->key.length);
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < item->evidence_count;
         evidence_index++) {
      const AbString *path = &item->evidence[evidence_index].path;
      if (!path->length)
        continue;
      status = symbol_projection
                   ? append_symbol_selector(engine, &symbols, path, &item->key)
                   : append_selector(engine, &paths, path->data, path->length);
    }
  }
  if (status == ARCHBIRD_OK) {
    AbObjectField *field = mutable_member(options, "paths");
    AbValue merged = {0};
    status = normalized_string_union(engine, &field->value, &paths, &merged);
    if (status == ARCHBIRD_OK) {
      ab_value_free(engine, &field->value);
      field->value = merged;
    }
  }
  if (status == ARCHBIRD_OK) {
    AbObjectField *field = mutable_member(options, "symbols");
    AbValue merged = {0};
    status = normalized_string_union(engine, &field->value, &symbols, &merged);
    if (status == ARCHBIRD_OK) {
      ab_value_free(engine, &field->value);
      field->value = merged;
    }
  }
  ab_value_free(engine, &paths);
  ab_value_free(engine, &symbols);
  return status;
}

static ArchbirdStatus
render_projection_definition_row(AbBuffer *buffer, const AbString *id,
                                 const AbProjectionEvaluation *evaluation) {
  TRY(ab_buffer_literal(buffer, "{\"id\":"));
  TRY(ab_buffer_json_string(buffer, id->data, id->length));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, evaluation->definition_sha256, 64));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus
render_projection_result_row(AbBuffer *buffer, const AbString *id,
                             const AbProjectionEvaluation *evaluation) {
  TRY(ab_buffer_literal(buffer, "{\"id\":"));
  TRY(ab_buffer_json_string(buffer, id->data, id->length));
  TRY(ab_buffer_literal(buffer, ",\"projection_definition_sha256\":"));
  TRY(ab_buffer_json_string(buffer, evaluation->definition_sha256, 64));
  TRY(ab_buffer_literal(buffer, ",\"projection_result_sha256\":"));
  TRY(ab_buffer_json_string(buffer, evaluation->result_sha256, 64));
  return ab_buffer_literal(buffer, "}");
}

typedef struct QueryProjectionNode {
  AbValue definition;
  AbString id;
  AbProjectionEvaluation evaluation;
} QueryProjectionNode;

static ArchbirdStatus inline_projection_id(ArchbirdEngine *engine,
                                           const AbString *query_id,
                                           const char sha256[65],
                                           AbString *out);

static int projection_node_compare(const void *left_raw,
                                   const void *right_raw) {
  const QueryProjectionNode *left = (const QueryProjectionNode *)left_raw;
  const QueryProjectionNode *right = (const QueryProjectionNode *)right_raw;
  int compared = strcmp(left->evaluation.definition_sha256,
                        right->evaluation.definition_sha256);
  return compared ? compared : ab_string_compare(&left->id, &right->id);
}

static void projection_nodes_free(ArchbirdEngine *engine,
                                  QueryProjectionNode *nodes, size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_value_free(engine, &nodes[index].definition);
    ab_string_free(engine, &nodes[index].id);
    ab_projection_evaluation_free(engine, &nodes[index].evaluation);
  }
  ab_free(engine, nodes);
}

static ArchbirdStatus
projection_node_add(ArchbirdEngine *engine, const AbValue *definition,
                    const AbString *declared_id, const AbString *query_id,
                    const AbValue *map, const AbValue *resolution,
                    QueryProjectionNode *nodes, size_t capacity,
                    size_t *count) {
  char definition_sha256[65] = {0};
  AbString node_id = {0};
  size_t previous;
  ArchbirdStatus status =
      ab_projection_definition_sha256(engine, definition, definition_sha256);
  for (previous = 0; status == ARCHBIRD_OK && previous < *count; previous++)
    if (!strcmp(nodes[previous].evaluation.definition_sha256,
                definition_sha256))
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
    status = ab_projection_evaluate_fact(engine, definition, map, resolution,
                                         &node_id, &nodes[*count].evaluation);
  if (status == ARCHBIRD_OK && strcmp(ab_verify_fact_selection_classification(
                                          &nodes[*count].evaluation.fact),
                                      "complete"))
    status = invalid(engine, "query seed projection is incomplete");
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &nodes[*count].definition, definition);
  if (status == ARCHBIRD_OK) {
    nodes[*count].id = node_id;
    memset(&node_id, 0, sizeof(node_id));
    (*count)++;
  } else {
    ab_projection_evaluation_free(engine, &nodes[*count].evaluation);
    ab_value_free(engine, &nodes[*count].definition);
  }
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

static ArchbirdStatus inferred_projection_add(
    ArchbirdEngine *engine, const char *select, const char *first_name,
    const AbValue *first, const char *second_name, const AbValue *second,
    const AbString *query_id, const AbValue *map, const AbValue *resolution,
    QueryProjectionNode *nodes, size_t capacity, size_t *count) {
  AbValue definition = {0};
  ArchbirdStatus status = inferred_definition(engine, select, first_name, first,
                                              second_name, second, &definition);
  if (status == ARCHBIRD_OK)
    status = projection_node_add(engine, &definition, NULL, query_id, map,
                                 resolution, nodes, capacity, count);
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
prepare_projection_nodes(ArchbirdEngine *engine,
                         const AbProjectConfiguration *configuration,
                         const AbValue *query, const AbString *query_id,
                         const AbValue *map, const AbValue *resolution,
                         QueryProjectionNode **out_nodes, size_t *out_count) {
  const AbValue *references = ab_value_member(query, "projection");
  const AbValue *paths = ab_value_member(query, "paths");
  const AbValue *symbols = ab_value_member(query, "symbols");
  const AbValue *components = ab_value_member(query, "components");
  const AbValue *packages = ab_value_member(query, "packages");
  const AbValue *artifacts = ab_value_member(query, "artifacts");
  const AbValue *focus = ab_value_member(query, "focus");
  QueryProjectionNode *nodes = NULL;
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
  if (reference_count > SIZE_MAX - 16 ||
      reference_count + 16 > SIZE_MAX / sizeof(*nodes))
    return ARCHBIRD_LIMIT_EXCEEDED;
  capacity = reference_count + 16;
  nodes = (QueryProjectionNode *)ab_calloc(engine, capacity, sizeof(*nodes));
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
      named = named_field(&configuration->projections, &reference->as.text);
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
      status =
          projection_node_add(engine, definition, declared_id, query_id, map,
                              resolution, nodes, capacity, &node_count);
  }
  if (status == ARCHBIRD_OK && paths && paths->as.array.count)
    status = inferred_projection_add(engine, "mapped_paths", "paths", paths,
                                     NULL, NULL, query_id, map, resolution,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && symbols && symbols->as.array.count)
    status = inferred_projection_add(engine, "symbols", "name_patterns",
                                     symbols, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && symbols && symbols->as.array.count)
    status = inferred_projection_add(engine, "package_exports", "name_patterns",
                                     symbols, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && components && components->as.array.count)
    status = inferred_projection_add(
        engine, "component_membership", "components", components, NULL, NULL,
        query_id, map, resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && packages && packages->as.array.count)
    status = inferred_projection_add(engine, "package_entrypoints", "packages",
                                     packages, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && artifacts && artifacts->as.array.count)
    status = inferred_projection_add(engine, "artifact_routes", "artifacts",
                                     artifacts, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "mapped_paths", "paths", focus,
                                     NULL, NULL, query_id, map, resolution,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "symbols", "name_patterns", focus,
                                     NULL, NULL, query_id, map, resolution,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(
        engine, "component_membership", "components", focus, NULL, NULL,
        query_id, map, resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "package_entrypoints", "packages",
                                     focus, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "package_exports", "name_patterns",
                                     focus, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "artifact_routes", "artifacts",
                                     focus, NULL, NULL, query_id, map,
                                     resolution, nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "test_routes", "paths", focus,
                                     NULL, NULL, query_id, map, resolution,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && focus && focus->as.array.count)
    status = inferred_projection_add(engine, "test_routes", "selectors", focus,
                                     NULL, NULL, query_id, map, resolution,
                                     nodes, capacity, &node_count);
  if (status == ARCHBIRD_OK && node_count > 1)
    qsort(nodes, node_count, sizeof(*nodes), projection_node_compare);
  if (status != ARCHBIRD_OK) {
    projection_nodes_free(engine, nodes, node_count);
    return status;
  }
  *out_nodes = nodes;
  *out_count = node_count;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_query_plan_compile(
    ArchbirdEngine *engine, const uint8_t *config_json, size_t config_length,
    const uint8_t *map_json, size_t map_length, const uint8_t *resolution_json,
    size_t resolution_length, const char *query_id, size_t query_id_length,
    const uint8_t *overrides_json, size_t overrides_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbProjectConfiguration configuration = {0};
  AbValue map = {0};
  AbValue resolution = {0};
  AbValue overrides = {.kind = AB_VALUE_OBJECT};
  AbValue options = {0};
  AbValue plan_options = {0};
  QueryProjectionNode *projections = NULL;
  size_t projection_count = 0;
  size_t projection_index;
  const AbObjectField *query;
  const AbValue *definition = NULL;
  const AbValue *map_config = NULL;
  const AbValue empty = {.kind = AB_VALUE_OBJECT};
  AbString id;
  int named;
  char query_definition_sha256[65] = {0};
  char query_plan_sha256[65] = {0};
  AbBuffer plan_base;
  AbBuffer rendered;
  ArchbirdStatus status;
  if (!engine || (!config_json && config_length) || !map_json || !map_length ||
      (!query_id && query_id_length) ||
      (!resolution_json && resolution_length) ||
      (!overrides_json && overrides_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  named = query_id_length != 0;
  id.data = (char *)(named ? query_id : "ad-hoc");
  id.length = named ? query_id_length : 6;
  if (!stable_id(&id) || (named && !config_length))
    return invalid(engine, "query id is not a stable identifier");
  ab_buffer_init(&plan_base, engine);
  ab_buffer_init(&rendered, engine);
  status = named ? ab_project_configuration_decode(
                       engine, config_json, config_length, &configuration)
                 : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, map_json, map_length, &map);
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_json_value_decode(engine, resolution_json, resolution_length,
                                  &resolution);
  if (status == ARCHBIRD_OK && overrides_length)
    status = ab_json_value_decode(engine, overrides_json, overrides_length,
                                  &overrides);
  if (status == ARCHBIRD_OK)
    status = ab_projection_map_validate(engine, &map, "query plan Map");
  if (status == ARCHBIRD_OK) {
    const AbValue *evidence = ab_value_member(&map, "evidence");
    map_config = evidence ? ab_value_member(evidence, "config_sha256") : NULL;
  }
  if (status == ARCHBIRD_OK && named) {
    if (!map_config || map_config->kind != AB_VALUE_STRING ||
        map_config->as.text.length != 64 ||
        memcmp(map_config->as.text.data, configuration.map_config_sha256, 64) !=
            0)
      status = invalid(
          engine,
          "project configuration Map definition does not match saved Map");
  }
  if (status == ARCHBIRD_OK && resolution_length)
    status = ab_projection_resolution_validate(engine, &resolution, &map,
                                               "query plan resolution");
  if (status == ARCHBIRD_OK && overrides.kind != AB_VALUE_OBJECT)
    status = invalid(engine, "runtime overrides must be an object");
  query = status == ARCHBIRD_OK && named
              ? named_field(&configuration.queries, &id)
              : NULL;
  if (status == ARCHBIRD_OK && named && !query)
    status = invalid(engine, "unknown named query");
  if (status == ARCHBIRD_OK)
    definition = named ? &query->value : &overrides;
  if (status == ARCHBIRD_OK)
    status = build_options(engine, definition, named ? &overrides : &empty,
                           &options);
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &plan_options, &options);
  if (status == ARCHBIRD_OK)
    status = digest_value(engine, definition, query_definition_sha256);
  if (status == ARCHBIRD_OK)
    status =
        prepare_projection_nodes(engine, &configuration, definition, &id, &map,
                                 resolution_length ? &resolution : NULL,
                                 &projections, &projection_count);
  if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(&plan_base, "{\"operations\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&plan_base, &plan_options);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&plan_base, ",\"projection_definitions\":[");
    for (projection_index = 0;
         status == ARCHBIRD_OK && projection_index < projection_count;
         projection_index++) {
      if (projection_index)
        status = ab_buffer_literal(&plan_base, ",");
      if (status == ARCHBIRD_OK)
        status = render_projection_definition_row(
            &plan_base, &projections[projection_index].id,
            &projections[projection_index].evaluation);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&plan_base, "]}");
    if (status == ARCHBIRD_OK)
      status = digest_buffer(&plan_base, query_plan_sha256);
  }
  for (projection_index = 0;
       status == ARCHBIRD_OK && projection_index < projection_count;
       projection_index++)
    status = add_projection_seeds(engine, &options,
                                  &projections[projection_index].definition,
                                  &projections[projection_index].evaluation);
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_literal(&rendered, "{\"artifact\":\"query-plan\",\"plan\":{");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, "\"id\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&rendered, id.data, id.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"map_config_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&rendered, map_config);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"project_configuration_sha256\":");
  if (status == ARCHBIRD_OK && named)
    status = ab_buffer_json_string(&rendered, configuration.sha256, 64);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, "null");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"projection_definitions\":[");
  for (projection_index = 0;
       status == ARCHBIRD_OK && projection_index < projection_count;
       projection_index++) {
    if (projection_index)
      status = ab_buffer_literal(&rendered, ",");
    if (status == ARCHBIRD_OK)
      status = render_projection_definition_row(
          &rendered, &projections[projection_index].id,
          &projections[projection_index].evaluation);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, "]");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"query_definition_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&rendered, query_definition_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"query_plan_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&rendered, query_plan_sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, "},\"projection_results\":[");
  for (projection_index = 0;
       status == ARCHBIRD_OK && projection_index < projection_count;
       projection_index++) {
    if (projection_index)
      status = ab_buffer_literal(&rendered, ",");
    if (status == ARCHBIRD_OK)
      status = render_projection_result_row(
          &rendered, &projections[projection_index].id,
          &projections[projection_index].evaluation);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, "],\"request\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&rendered, &options);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&rendered, ",\"schema_version\":1}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(engine, rendered.data, rendered.length,
                                        json_flags, write_fn, user_data);
  ab_buffer_free(&plan_base);
  ab_buffer_free(&rendered);
  projection_nodes_free(engine, projections, projection_count);
  ab_value_free(engine, &options);
  ab_value_free(engine, &plan_options);
  if (overrides_length)
    ab_value_free(engine, &overrides);
  ab_value_free(engine, &resolution);
  ab_value_free(engine, &map);
  ab_project_configuration_free(engine, &configuration);
  return status;
}
