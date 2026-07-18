#include "json_value.h"

#include "json_internal.h"
#include "json_number.h"

#include <stdlib.h>
#include <string.h>

static int object_field_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus decode_node(ArchbirdEngine *engine, yyjson_val *node,
                                  AbValue *out) {
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  if (yyjson_is_null(node)) {
    out->kind = AB_VALUE_NULL;
  } else if (yyjson_is_bool(node)) {
    out->kind = AB_VALUE_BOOL;
    out->as.boolean = yyjson_is_true(node);
  } else if (yyjson_is_raw(node)) {
    const char *number = yyjson_get_raw(node);
    size_t length = yyjson_get_len(node);
    size_t index;
    int real = 0;
    for (index = 0; index < length; index++) {
      if (number[index] == '.' || number[index] == 'e' ||
          number[index] == 'E') {
        real = 1;
        break;
      }
    }
    if (real) {
      out->kind = AB_VALUE_REAL;
      status = ab_json_real_parse(engine, number, length, &out->as.real);
    } else {
      out->kind = AB_VALUE_INTEGER;
      status = ab_string_copy(engine, &out->as.text, number, length);
    }
  } else if (yyjson_is_str(node)) {
    out->kind = AB_VALUE_STRING;
    status = ab_string_copy(engine, &out->as.text, yyjson_get_str(node),
                            yyjson_get_len(node));
  } else if (yyjson_is_arr(node)) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    size_t index = 0;
    out->kind = AB_VALUE_ARRAY;
    out->as.array.count = yyjson_arr_size(node);
    if (out->as.array.count > SIZE_MAX / sizeof(*out->as.array.items))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON value array is too large");
    if (status == ARCHBIRD_OK && out->as.array.count) {
      out->as.array.items = (AbValue *)ab_calloc(engine, out->as.array.count,
                                                 sizeof(*out->as.array.items));
      if (!out->as.array.items)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory decoding JSON array");
    }
    yyjson_arr_iter_init(node, &iterator);
    while (status == ARCHBIRD_OK &&
           (item = yyjson_arr_iter_next(&iterator)) != NULL)
      status = decode_node(engine, item, &out->as.array.items[index++]);
  } else if (yyjson_is_obj(node)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    size_t index = 0;
    out->kind = AB_VALUE_OBJECT;
    out->as.object.count = yyjson_obj_size(node);
    if (out->as.object.count > SIZE_MAX / sizeof(*out->as.object.fields))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON value object is too large");
    if (status == ARCHBIRD_OK && out->as.object.count) {
      out->as.object.fields = (AbObjectField *)ab_calloc(
          engine, out->as.object.count, sizeof(*out->as.object.fields));
      if (!out->as.object.fields)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory decoding JSON object");
    }
    yyjson_obj_iter_init(node, &iterator);
    while (status == ARCHBIRD_OK &&
           (key = yyjson_obj_iter_next(&iterator)) != NULL) {
      AbObjectField *field = &out->as.object.fields[index++];
      status = ab_string_copy(engine, &field->name, yyjson_get_str(key),
                              yyjson_get_len(key));
      if (status == ARCHBIRD_OK)
        status =
            decode_node(engine, yyjson_obj_iter_get_val(key), &field->value);
    }
    if (status == ARCHBIRD_OK && out->as.object.count > 1)
      qsort(out->as.object.fields, out->as.object.count,
            sizeof(*out->as.object.fields), object_field_compare);
  } else {
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "unsupported JSON value kind");
  }
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

ArchbirdStatus ab_json_value_decode(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbValue *out) {
  yyjson_doc *document = NULL;
  ArchbirdStatus status;
  if (!engine || (!json && json_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status =
      archbird_json_parse_document(engine, json, json_length, &document, 0);
  if (status == ARCHBIRD_OK)
    status = decode_node(engine, yyjson_doc_get_root(document), out);
  yyjson_doc_free(document);
  return status;
}

const AbValue *ab_value_member(const AbValue *object, const char *name) {
  AbString wanted;
  size_t low = 0;
  size_t high;
  if (!object || object->kind != AB_VALUE_OBJECT || !name)
    return NULL;
  wanted.data = (char *)name;
  wanted.length = strlen(name);
  high = object->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&object->as.object.fields[middle].name, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &object->as.object.fields[middle].value;
  }
  return NULL;
}

int ab_value_u64(const AbValue *value, uint64_t *out) {
  uint64_t result = 0;
  size_t index;
  if (!value || value->kind != AB_VALUE_INTEGER || !out ||
      !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte < '0' || byte > '9' ||
        result > (UINT64_MAX - (uint64_t)(byte - '0')) / 10)
      return 0;
    result = result * 10 + (uint64_t)(byte - '0');
  }
  *out = result;
  return 1;
}

int ab_value_string_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         (!length || memcmp(value->as.text.data, literal, length) == 0);
}

ArchbirdStatus ab_value_render(AbBuffer *buffer, const AbValue *value) {
  size_t index;
  ArchbirdStatus status;
  if (!buffer || !value)
    return ARCHBIRD_INVALID_ARGUMENT;
  switch (value->kind) {
  case AB_VALUE_NULL:
    return ab_buffer_literal(buffer, "null");
  case AB_VALUE_BOOL:
    return ab_buffer_literal(buffer, value->as.boolean ? "true" : "false");
  case AB_VALUE_INTEGER:
    return ab_buffer_append(buffer, value->as.text.data, value->as.text.length);
  case AB_VALUE_REAL: {
    char number[AB_JSON_REAL_BUFFER_SIZE];
    size_t length;
    ArchbirdStatus number_status =
        ab_json_real_format(buffer->engine, value->as.real, number, &length);
    return number_status == ARCHBIRD_OK
               ? ab_buffer_append(buffer, number, length)
               : number_status;
  }
  case AB_VALUE_STRING:
    return ab_buffer_json_string(buffer, value->as.text.data,
                                 value->as.text.length);
  case AB_VALUE_ARRAY:
    status = ab_buffer_literal(buffer, "[");
    for (index = 0; status == ARCHBIRD_OK && index < value->as.array.count;
         index++) {
      if (index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(buffer, &value->as.array.items[index]);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "]");
    return status;
  case AB_VALUE_OBJECT:
    status = ab_buffer_literal(buffer, "{");
    for (index = 0; status == ARCHBIRD_OK && index < value->as.object.count;
         index++) {
      const AbObjectField *field = &value->as.object.fields[index];
      if (index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_json_string(buffer, field->name.data, field->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ":");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(buffer, &field->value);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
    return status;
  }
  return archbird_error_set(buffer->engine, ARCHBIRD_CONFLICT,
                            ARCHBIRD_NO_OFFSET,
                            "cannot render unknown JSON value kind");
}
