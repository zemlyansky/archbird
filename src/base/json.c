#include "json_internal.h"
#include "json_number.h"

#include <stdlib.h>
#include <string.h>

#define JSON_WRITE_BUFFER_SIZE 16384

typedef struct JsonMember {
  yyjson_val *key;
  yyjson_val *value;
} JsonMember;

typedef struct JsonWriter {
  ArchbirdEngine *engine;
  ArchbirdWriteFn write_fn;
  void *user_data;
  uint32_t flags;
  uint8_t output[JSON_WRITE_BUFFER_SIZE];
  size_t output_length;
} JsonWriter;

static void *yyjson_allocate(void *context, size_t size) {
  return ab_malloc((ArchbirdEngine *)context, size);
}

static void *yyjson_reallocate(void *context, void *pointer, size_t old_size,
                               size_t size) {
  (void)old_size;
  return ab_realloc((ArchbirdEngine *)context, pointer, size);
}

static void yyjson_deallocate(void *context, void *pointer) {
  ab_free((ArchbirdEngine *)context, pointer);
}

void ab_yyjson_allocator(ArchbirdEngine *engine, yyjson_alc *out_allocator) {
  out_allocator->malloc = yyjson_allocate;
  out_allocator->realloc = yyjson_reallocate;
  out_allocator->free = yyjson_deallocate;
  out_allocator->ctx = engine;
}

static int bytes_compare(const char *left, size_t left_length,
                         const char *right, size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared != 0)
    return compared;
  return (left_length > right_length) - (left_length < right_length);
}

static int member_compare(const void *left_raw, const void *right_raw) {
  const JsonMember *left = (const JsonMember *)left_raw;
  const JsonMember *right = (const JsonMember *)right_raw;
  return bytes_compare(yyjson_get_str(left->key), yyjson_get_len(left->key),
                       yyjson_get_str(right->key), yyjson_get_len(right->key));
}

static ArchbirdStatus collect_members(ArchbirdEngine *engine,
                                      yyjson_val *object,
                                      JsonMember **out_members,
                                      size_t *out_count) {
  size_t count = yyjson_obj_size(object);
  JsonMember *members = NULL;
  yyjson_obj_iter iterator;
  yyjson_val *key;
  size_t index = 0;
  *out_members = NULL;
  *out_count = 0;
  if (count > SIZE_MAX / sizeof(*members)) {
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "JSON object is too large");
  }
  if (count) {
    members = (JsonMember *)ab_malloc(engine, count * sizeof(*members));
    if (!members) {
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting JSON keys");
    }
  }
  yyjson_obj_iter_init(object, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    members[index].key = key;
    members[index].value = yyjson_obj_iter_get_val(key);
    index++;
  }
  if (count > 1)
    qsort(members, count, sizeof(*members), member_compare);
  for (index = 1; index < count; index++) {
    JsonMember *previous = &members[index - 1];
    JsonMember *current = &members[index];
    if (bytes_compare(
            yyjson_get_str(previous->key), yyjson_get_len(previous->key),
            yyjson_get_str(current->key), yyjson_get_len(current->key)) == 0) {
      ab_free(engine, members);
      return archbird_error_set(engine, ARCHBIRD_DUPLICATE_KEY,
                                ARCHBIRD_NO_OFFSET,
                                "duplicate JSON object key is not allowed");
    }
  }
  *out_members = members;
  *out_count = count;
  return ARCHBIRD_OK;
}

static int raw_number_is_real(const char *number, size_t length) {
  size_t index;
  for (index = 0; index < length; index++) {
    if (number[index] == '.' || number[index] == 'e' || number[index] == 'E') {
      return 1;
    }
  }
  return 0;
}

static ArchbirdStatus validate_value(ArchbirdEngine *engine, yyjson_val *value,
                                     size_t depth, int reject_real_numbers) {
  if (depth > engine->options.max_depth) {
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "JSON nesting depth exceeds %zu", engine->options.max_depth);
  }
  if (yyjson_is_str(value)) {
    if (yyjson_get_len(value) > engine->options.max_string_bytes) {
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON string length exceeds %zu bytes",
                                engine->options.max_string_bytes);
    }
    return ARCHBIRD_OK;
  }
  if (yyjson_is_raw(value)) {
    if (reject_real_numbers &&
        raw_number_is_real(yyjson_get_raw(value), yyjson_get_len(value))) {
      return archbird_error_set(
          engine, ARCHBIRD_UNSUPPORTED_NUMBER, ARCHBIRD_NO_OFFSET,
          "JSON real-number parity is not implemented in native ABI 0");
    }
    return ARCHBIRD_OK;
  }
  if (yyjson_is_arr(value)) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    yyjson_arr_iter_init(value, &iterator);
    while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
      ArchbirdStatus status =
          validate_value(engine, item, depth + 1, reject_real_numbers);
      if (status != ARCHBIRD_OK)
        return status;
    }
    return ARCHBIRD_OK;
  }
  if (yyjson_is_obj(value)) {
    JsonMember *members;
    size_t count;
    size_t index;
    ArchbirdStatus status = collect_members(engine, value, &members, &count);
    if (status != ARCHBIRD_OK)
      return status;
    for (index = 0; index < count; index++) {
      if (yyjson_get_len(members[index].key) >
          engine->options.max_string_bytes) {
        ab_free(engine, members);
        return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON key length exceeds %zu bytes",
                                  engine->options.max_string_bytes);
      }
      status = validate_value(engine, members[index].value, depth + 1,
                              reject_real_numbers);
      if (status != ARCHBIRD_OK) {
        ab_free(engine, members);
        return status;
      }
    }
    ab_free(engine, members);
    return ARCHBIRD_OK;
  }
  if (yyjson_is_null(value) || yyjson_is_bool(value))
    return ARCHBIRD_OK;
  return archbird_error_set(engine, ARCHBIRD_INVALID_JSON, ARCHBIRD_NO_OFFSET,
                            "unsupported JSON value type");
}

ArchbirdStatus archbird_json_parse_document(ArchbirdEngine *engine,
                                            const uint8_t *input,
                                            size_t input_length,
                                            yyjson_doc **out_document,
                                            int reject_real_numbers) {
  yyjson_read_err error;
  yyjson_alc allocator;
  yyjson_doc *document;
  yyjson_val *root;
  size_t value_count;
  *out_document = NULL;
  archbird_error_clear(engine);
  if (!input && input_length != 0) {
    return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET, "JSON input pointer is null");
  }
  if (input_length == 0) {
    return archbird_error_set(engine, ARCHBIRD_INVALID_JSON, ARCHBIRD_NO_OFFSET,
                              "JSON input is empty");
  }
  if (input_length > engine->options.max_input_bytes) {
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "JSON input length exceeds %zu bytes", engine->options.max_input_bytes);
  }
  ab_yyjson_allocator(engine, &allocator);
  document = yyjson_read_opts((char *)input, input_length,
                              YYJSON_READ_NUMBER_AS_RAW, &allocator, &error);
  if (!document) {
    if (error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory parsing JSON");
    return archbird_error_set(engine, ARCHBIRD_INVALID_JSON, error.pos,
                              "invalid JSON at byte %zu: %s", error.pos,
                              error.msg ? error.msg : "parse failure");
  }
  value_count = yyjson_doc_get_val_count(document);
  if (value_count > engine->options.max_values) {
    yyjson_doc_free(document);
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "JSON value count exceeds %zu", engine->options.max_values);
  }
  root = yyjson_doc_get_root(document);
  {
    ArchbirdStatus status =
        validate_value(engine, root, 1, reject_real_numbers);
    if (status != ARCHBIRD_OK) {
      yyjson_doc_free(document);
      return status;
    }
  }
  *out_document = document;
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_json_validate(ArchbirdEngine *engine,
                                      const uint8_t *input,
                                      size_t input_length) {
  yyjson_doc *document;
  ArchbirdStatus status;
  if (!engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  status =
      archbird_json_parse_document(engine, input, input_length, &document, 0);
  yyjson_doc_free(document);
  return status;
}

static ArchbirdStatus flush_output(JsonWriter *writer) {
  int callback_status;
  if (writer->output_length == 0)
    return ARCHBIRD_OK;
  callback_status = writer->write_fn(writer->user_data, writer->output,
                                     writer->output_length);
  if (callback_status != 0) {
    /* Internal writers report their precise allocator/limit failure through
       the shared engine. Preserve it instead of relabeling it as an external
       callback failure. A normal public callback does not mutate the engine
       and therefore still receives ARCHBIRD_WRITE_FAILED. */
    if (writer->engine->error_status != ARCHBIRD_OK)
      return writer->engine->error_status;
    return archbird_error_set(writer->engine, ARCHBIRD_WRITE_FAILED,
                              ARCHBIRD_NO_OFFSET,
                              "JSON output callback failed");
  }
  writer->output_length = 0;
  return ARCHBIRD_OK;
}

static ArchbirdStatus write_bytes(JsonWriter *writer, const void *bytes,
                                  size_t length) {
  ArchbirdStatus status;
  if (length == 0)
    return ARCHBIRD_OK;
  if (length > sizeof(writer->output)) {
    status = flush_output(writer);
    if (status != ARCHBIRD_OK)
      return status;
    if (writer->write_fn(writer->user_data, (const uint8_t *)bytes, length) !=
        0) {
      if (writer->engine->error_status != ARCHBIRD_OK)
        return writer->engine->error_status;
      return archbird_error_set(writer->engine, ARCHBIRD_WRITE_FAILED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON output callback failed");
    }
    return ARCHBIRD_OK;
  }
  if (length > sizeof(writer->output) - writer->output_length) {
    status = flush_output(writer);
    if (status != ARCHBIRD_OK)
      return status;
  }
  memcpy(writer->output + writer->output_length, bytes, length);
  writer->output_length += length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus write_literal(JsonWriter *writer, const char *literal) {
  return write_bytes(writer, literal, strlen(literal));
}

static ArchbirdStatus write_indent(JsonWriter *writer, size_t depth) {
  static const char spaces[] = "                                ";
  size_t remaining = depth * 2;
  while (remaining) {
    size_t chunk =
        remaining < sizeof(spaces) - 1 ? remaining : sizeof(spaces) - 1;
    ArchbirdStatus status = write_bytes(writer, spaces, chunk);
    if (status != ARCHBIRD_OK)
      return status;
    remaining -= chunk;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus write_string(JsonWriter *writer, const char *string,
                                   size_t length) {
  static const char hex[] = "0123456789abcdef";
  size_t index;
  size_t span_start = 0;
  ArchbirdStatus status = write_literal(writer, "\"");
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; index < length; index++) {
    unsigned char byte = (unsigned char)string[index];
    const char *escape = NULL;
    switch (byte) {
    case '\"':
      escape = "\\\"";
      break;
    case '\\':
      escape = "\\\\";
      break;
    case '\b':
      escape = "\\b";
      break;
    case '\f':
      escape = "\\f";
      break;
    case '\n':
      escape = "\\n";
      break;
    case '\r':
      escape = "\\r";
      break;
    case '\t':
      escape = "\\t";
      break;
    default:
      break;
    }
    if (!escape && byte >= 0x20)
      continue;
    if (index > span_start) {
      status = write_bytes(writer, &string[span_start], index - span_start);
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (escape) {
      status = write_literal(writer, escape);
    } else {
      char encoded[6] = {'\\', 'u', '0', '0', hex[byte >> 4], hex[byte & 0xf]};
      status = write_bytes(writer, encoded, sizeof(encoded));
    }
    if (status != ARCHBIRD_OK)
      return status;
    span_start = index + 1;
  }
  if (span_start < length) {
    status = write_bytes(writer, &string[span_start], length - span_start);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return write_literal(writer, "\"");
}

static ArchbirdStatus write_value(JsonWriter *writer, yyjson_val *value,
                                  size_t depth);

static ArchbirdStatus write_array(JsonWriter *writer, yyjson_val *array,
                                  size_t depth) {
  yyjson_arr_iter iterator;
  yyjson_val *item;
  size_t index = 0;
  size_t count = yyjson_arr_size(array);
  int pretty = (writer->flags & ARCHBIRD_JSON_PRETTY) != 0;
  ArchbirdStatus status = write_literal(writer, "[");
  if (status != ARCHBIRD_OK || count == 0) {
    return status == ARCHBIRD_OK ? write_literal(writer, "]") : status;
  }
  yyjson_arr_iter_init(array, &iterator);
  while ((item = yyjson_arr_iter_next(&iterator)) != NULL) {
    if (index++) {
      status = write_literal(writer, pretty ? ",\n" : ",");
    } else if (pretty) {
      status = write_literal(writer, "\n");
    }
    if (status == ARCHBIRD_OK && pretty)
      status = write_indent(writer, depth + 1);
    if (status == ARCHBIRD_OK)
      status = write_value(writer, item, depth + 1);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (pretty) {
    status = write_literal(writer, "\n");
    if (status == ARCHBIRD_OK)
      status = write_indent(writer, depth);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return write_literal(writer, "]");
}

static ArchbirdStatus write_object(JsonWriter *writer, yyjson_val *object,
                                   size_t depth) {
  JsonMember *members;
  size_t count;
  size_t index;
  int pretty = (writer->flags & ARCHBIRD_JSON_PRETTY) != 0;
  ArchbirdStatus status =
      collect_members(writer->engine, object, &members, &count);
  if (status != ARCHBIRD_OK)
    return status;
  status = write_literal(writer, "{");
  if (status != ARCHBIRD_OK || count == 0) {
    ab_free(writer->engine, members);
    return status == ARCHBIRD_OK ? write_literal(writer, "}") : status;
  }
  for (index = 0; index < count; index++) {
    if (index) {
      status = write_literal(writer, pretty ? ",\n" : ",");
    } else if (pretty) {
      status = write_literal(writer, "\n");
    }
    if (status == ARCHBIRD_OK && pretty)
      status = write_indent(writer, depth + 1);
    if (status == ARCHBIRD_OK) {
      status = write_string(writer, yyjson_get_str(members[index].key),
                            yyjson_get_len(members[index].key));
    }
    if (status == ARCHBIRD_OK)
      status = write_literal(writer, pretty ? ": " : ":");
    if (status == ARCHBIRD_OK) {
      status = write_value(writer, members[index].value, depth + 1);
    }
    if (status != ARCHBIRD_OK) {
      ab_free(writer->engine, members);
      return status;
    }
  }
  ab_free(writer->engine, members);
  if (pretty) {
    status = write_literal(writer, "\n");
    if (status == ARCHBIRD_OK)
      status = write_indent(writer, depth);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return write_literal(writer, "}");
}

static ArchbirdStatus write_value(JsonWriter *writer, yyjson_val *value,
                                  size_t depth) {
  if (yyjson_is_null(value))
    return write_literal(writer, "null");
  if (yyjson_is_true(value))
    return write_literal(writer, "true");
  if (yyjson_is_false(value))
    return write_literal(writer, "false");
  if (yyjson_is_str(value)) {
    return write_string(writer, yyjson_get_str(value), yyjson_get_len(value));
  }
  if (yyjson_is_raw(value)) {
    const char *number = yyjson_get_raw(value);
    size_t length = yyjson_get_len(value);
    if (raw_number_is_real(number, length)) {
      double real;
      char rendered[AB_JSON_REAL_BUFFER_SIZE];
      size_t rendered_length = 0;
      ArchbirdStatus status =
          ab_json_real_parse(writer->engine, number, length, &real);
      if (status == ARCHBIRD_OK)
        status = ab_json_real_format(writer->engine, real, rendered,
                                     &rendered_length);
      return status == ARCHBIRD_OK
                 ? write_bytes(writer, rendered, rendered_length)
                 : status;
    }
    if (length == 2 && number[0] == '-' && number[1] == '0') {
      return write_literal(writer, "0");
    }
    return write_bytes(writer, number, length);
  }
  if (yyjson_is_arr(value))
    return write_array(writer, value, depth);
  if (yyjson_is_obj(value))
    return write_object(writer, value, depth);
  return archbird_error_set(writer->engine, ARCHBIRD_INVALID_JSON,
                            ARCHBIRD_NO_OFFSET, "unsupported JSON value type");
}

ArchbirdStatus archbird_json_canonicalize(ArchbirdEngine *engine,
                                          const uint8_t *input,
                                          size_t input_length, uint32_t flags,
                                          ArchbirdWriteFn write_fn,
                                          void *user_data) {
  yyjson_doc *document;
  JsonWriter writer;
  ArchbirdStatus status;
  if (!engine || !write_fn ||
      (flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE))) {
    if (engine) {
      return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                ARCHBIRD_NO_OFFSET,
                                "invalid JSON canonicalization arguments");
    }
    return ARCHBIRD_INVALID_ARGUMENT;
  }
  status =
      archbird_json_parse_document(engine, input, input_length, &document, 0);
  if (status != ARCHBIRD_OK)
    return status;
  writer.engine = engine;
  writer.write_fn = write_fn;
  writer.user_data = user_data;
  writer.flags = flags;
  writer.output_length = 0;
  status = write_value(&writer, yyjson_doc_get_root(document), 0);
  if (status == ARCHBIRD_OK && (flags & ARCHBIRD_JSON_TRAILING_NEWLINE)) {
    status = write_literal(&writer, "\n");
  }
  if (status == ARCHBIRD_OK)
    status = flush_output(&writer);
  yyjson_doc_free(document);
  return status;
}

#undef JSON_WRITE_BUFFER_SIZE
