#include "json_internal.h"
#include "render_internal.h"
#include "sha256.h"
#include "utf8.h"

#include <string.h>

typedef struct JsonSourceLayout {
  yyjson_val *target;
  yyjson_val *insertion_parent;
  size_t target_start;
  size_t target_end;
  int target_found;
  size_t trailing_start;
  size_t last_prefix_start;
  size_t last_key_start;
  size_t last_key_end;
  size_t last_value_start;
  size_t member_count;
  int parent_found;
} JsonSourceLayout;

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static int is_json_whitespace(uint8_t byte) {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static void skip_whitespace(const uint8_t *input, size_t input_length,
                            size_t *offset) {
  while (*offset < input_length && is_json_whitespace(input[*offset]))
    (*offset)++;
}

static int scan_string_token(const uint8_t *input, size_t input_length,
                             size_t *offset) {
  size_t index = *offset;
  if (index >= input_length || input[index] != '"')
    return 0;
  index++;
  while (index < input_length) {
    if (input[index] == '"') {
      *offset = index + 1;
      return 1;
    }
    if (input[index] == '\\') {
      index++;
      if (index >= input_length)
        return 0;
    }
    index++;
  }
  return 0;
}

static int scan_primitive_token(const uint8_t *input, size_t input_length,
                                size_t *offset) {
  size_t index = *offset;
  while (index < input_length && input[index] != ',' && input[index] != '}' &&
         input[index] != ']' && !is_json_whitespace(input[index])) {
    index++;
  }
  if (index == *offset)
    return 0;
  *offset = index;
  return 1;
}

static ArchbirdStatus scan_error(ArchbirdEngine *engine, size_t offset) {
  return archbird_error_set(
      engine, ARCHBIRD_INVALID_JSON, offset,
      "validated JSON source could not be correlated with its parsed values");
}

static ArchbirdStatus
scan_source_value(ArchbirdEngine *engine, const uint8_t *input,
                  size_t input_length, size_t *offset, yyjson_val *value,
                  JsonSourceLayout *layout, size_t depth) {
  size_t start = *offset;
  ArchbirdStatus status;
  if (depth > engine->options.max_depth)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "JSON source scan exceeds nesting limit");

  if (yyjson_is_str(value)) {
    if (!scan_string_token(input, input_length, offset))
      return scan_error(engine, start);
  } else if (yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    size_t member_count = 0;
    size_t trailing_start = 0;
    size_t last_prefix_start = 0;
    size_t last_key_start = 0;
    size_t last_key_end = 0;
    size_t last_value_start = 0;
    if (*offset >= input_length || input[*offset] != '{')
      return scan_error(engine, *offset);
    (*offset)++;
    yyjson_obj_iter_init(value, &iterator);
    for (;;) {
      size_t probe = *offset;
      size_t prefix_start = probe;
      size_t key_start;
      size_t key_end;
      size_t value_start;
      yyjson_val *member_value;
      skip_whitespace(input, input_length, &probe);
      if (probe >= input_length)
        return scan_error(engine, probe);
      if (input[probe] == '}') {
        if (yyjson_obj_iter_next(&iterator) != NULL)
          return scan_error(engine, probe);
        if (member_count == 0)
          trailing_start = probe;
        *offset = probe + 1;
        break;
      }
      key_start = probe;
      key_end = key_start;
      if (!scan_string_token(input, input_length, &key_end))
        return scan_error(engine, key_start);
      probe = key_end;
      skip_whitespace(input, input_length, &probe);
      if (probe >= input_length || input[probe] != ':')
        return scan_error(engine, probe);
      probe++;
      skip_whitespace(input, input_length, &probe);
      value_start = probe;
      key = yyjson_obj_iter_next(&iterator);
      if (!key)
        return scan_error(engine, key_start);
      member_value = yyjson_obj_iter_get_val(key);
      status = scan_source_value(engine, input, input_length, &probe,
                                 member_value, layout, depth + 1);
      if (status != ARCHBIRD_OK)
        return status;
      member_count++;
      last_prefix_start = prefix_start;
      last_key_start = key_start;
      last_key_end = key_end;
      last_value_start = value_start;
      trailing_start = probe;
      skip_whitespace(input, input_length, &probe);
      if (probe >= input_length)
        return scan_error(engine, probe);
      if (input[probe] == ',') {
        *offset = probe + 1;
        continue;
      }
      if (input[probe] != '}')
        return scan_error(engine, probe);
      if (yyjson_obj_iter_next(&iterator) != NULL)
        return scan_error(engine, probe);
      *offset = probe + 1;
      break;
    }
    if (value == layout->insertion_parent) {
      layout->trailing_start = trailing_start;
      layout->last_prefix_start = last_prefix_start;
      layout->last_key_start = last_key_start;
      layout->last_key_end = last_key_end;
      layout->last_value_start = last_value_start;
      layout->member_count = member_count;
      layout->parent_found = 1;
    }
  } else if (yyjson_is_arr(value)) {
    yyjson_arr_iter iterator;
    yyjson_val *item;
    if (*offset >= input_length || input[*offset] != '[')
      return scan_error(engine, *offset);
    (*offset)++;
    yyjson_arr_iter_init(value, &iterator);
    for (;;) {
      size_t probe = *offset;
      skip_whitespace(input, input_length, &probe);
      if (probe >= input_length)
        return scan_error(engine, probe);
      item = yyjson_arr_iter_next(&iterator);
      if (!item) {
        if (input[probe] != ']')
          return scan_error(engine, probe);
        *offset = probe + 1;
        break;
      }
      status = scan_source_value(engine, input, input_length, &probe, item,
                                 layout, depth + 1);
      if (status != ARCHBIRD_OK)
        return status;
      skip_whitespace(input, input_length, &probe);
      if (probe >= input_length)
        return scan_error(engine, probe);
      if (input[probe] == ',') {
        *offset = probe + 1;
        continue;
      }
      if (input[probe] != ']' || yyjson_arr_iter_next(&iterator) != NULL)
        return scan_error(engine, probe);
      *offset = probe + 1;
      break;
    }
  } else if (!scan_primitive_token(input, input_length, offset)) {
    return scan_error(engine, start);
  }

  if (value == layout->target) {
    layout->target_start = start;
    layout->target_end = *offset;
    layout->target_found = 1;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus pointer_token_decode(ArchbirdEngine *engine,
                                           const uint8_t *pointer, size_t start,
                                           size_t end, AbBuffer *token) {
  size_t index;
  token->length = 0;
  for (index = start; index < end; index++) {
    uint8_t byte = pointer[index];
    if (byte == '~') {
      if (index + 1 >= end ||
          (pointer[index + 1] != '0' && pointer[index + 1] != '1')) {
        return archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, index,
            "JSON Pointer contains an invalid tilde escape");
      }
      index++;
      byte = pointer[index] == '0' ? '~' : '/';
    }
    if (token->length >= engine->options.max_string_bytes) {
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON Pointer token exceeds %zu bytes",
                                engine->options.max_string_bytes);
    }
    {
      ArchbirdStatus status = ab_buffer_append(token, &byte, 1);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static yyjson_val *object_member(yyjson_val *object, const uint8_t *name,
                                 size_t name_length) {
  yyjson_obj_iter iterator;
  yyjson_val *key;
  yyjson_obj_iter_init(object, &iterator);
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    if (yyjson_get_len(key) == name_length &&
        (name_length == 0 ||
         memcmp(yyjson_get_str(key), name, name_length) == 0)) {
      return yyjson_obj_iter_get_val(key);
    }
  }
  return NULL;
}

static int array_index(const uint8_t *token, size_t token_length,
                       size_t *out_index) {
  size_t index = 0;
  size_t value = 0;
  if (token_length == 0 || (token_length > 1 && token[0] == '0'))
    return 0;
  for (; index < token_length; index++) {
    unsigned digit;
    if (token[index] < '0' || token[index] > '9')
      return 0;
    digit = (unsigned)(token[index] - '0');
    if (value > (SIZE_MAX - digit) / 10)
      return 0;
    value = value * 10 + digit;
  }
  *out_index = value;
  return 1;
}

static ArchbirdStatus resolve_pointer(ArchbirdEngine *engine, yyjson_val *root,
                                      const uint8_t *pointer,
                                      size_t pointer_length,
                                      yyjson_val **out_target,
                                      yyjson_val **out_parent,
                                      AbBuffer *final_token) {
  yyjson_val *current = root;
  size_t start;
  if (pointer_length == 0) {
    *out_target = root;
    *out_parent = NULL;
    return ARCHBIRD_OK;
  }
  if (pointer[0] != '/') {
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, 0,
                              "JSON Pointer must be empty or start with '/'");
  }
  start = 1;
  for (;;) {
    size_t end = start;
    size_t item_index;
    int final;
    ArchbirdStatus status;
    yyjson_val *next = NULL;
    while (end < pointer_length && pointer[end] != '/')
      end++;
    final = end == pointer_length;
    status = pointer_token_decode(engine, pointer, start, end, final_token);
    if (status != ARCHBIRD_OK)
      return status;
    if (yyjson_is_obj(current)) {
      next = object_member(current, final_token->data, final_token->length);
      if (!next && final) {
        *out_target = NULL;
        *out_parent = current;
        return ARCHBIRD_OK;
      }
    } else if (yyjson_is_arr(current)) {
      if (!array_index(final_token->data, final_token->length, &item_index) ||
          item_index >= yyjson_arr_size(current)) {
        return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON Pointer does not resolve exactly");
      }
      next = yyjson_arr_get(current, item_index);
    } else {
      return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON Pointer traverses a scalar value");
    }
    if (!next) {
      return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                                ARCHBIRD_NO_OFFSET,
                                "JSON Pointer parent does not exist");
    }
    if (final) {
      *out_target = next;
      *out_parent = current;
      return ARCHBIRD_OK;
    }
    current = next;
    start = end + 1;
  }
}

static ArchbirdStatus canonicalize(ArchbirdEngine *engine, const uint8_t *json,
                                   size_t json_length, AbBuffer *out) {
  ab_buffer_init(out, engine);
  return archbird_json_canonicalize(engine, json, json_length, 0, buffer_write,
                                    out);
}

static int lowercase_sha256(const char *value, size_t length) {
  size_t index;
  if (!value || length != 64)
    return 0;
  for (index = 0; index < length; index++) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f'))) {
      return 0;
    }
  }
  return 1;
}

static ArchbirdStatus emit_edit(ArchbirdEngine *engine,
                                const AbBuffer *replacement,
                                ArchbirdWriteFn write_fn, void *user_data) {
  if (write_fn(user_data, replacement->data, replacement->length) != 0) {
    return archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                              "JSON Pointer edit output callback failed");
  }
  return ARCHBIRD_OK;
}

void archbird_json_pointer_edit_options_init(
    ArchbirdJsonPointerEditOptions *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
}

void archbird_json_pointer_edit_result_init(
    ArchbirdJsonPointerEditResult *result) {
  if (!result)
    return;
  memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
}

ArchbirdStatus
archbird_json_pointer_edit(ArchbirdEngine *engine, const uint8_t *input,
                           size_t input_length,
                           const ArchbirdJsonPointerEditOptions *options,
                           ArchbirdJsonPointerEditResult *out_result,
                           ArchbirdWriteFn write_fn, void *user_data) {
  uint8_t digest[32];
  char source_sha256[65];
  yyjson_doc *document = NULL;
  yyjson_val *root;
  yyjson_val *target = NULL;
  yyjson_val *parent = NULL;
  JsonSourceLayout layout;
  AbBuffer token;
  AbBuffer expected;
  AbBuffer actual;
  AbBuffer replacement_value;
  AbBuffer replacement;
  size_t offset = 0;
  ArchbirdStatus status;

  memset(&expected, 0, sizeof(expected));
  memset(&actual, 0, sizeof(actual));
  memset(&replacement_value, 0, sizeof(replacement_value));
  memset(&replacement, 0, sizeof(replacement));
  if (!engine || !options || options->struct_size != sizeof(*options) ||
      !out_result || out_result->struct_size != sizeof(*out_result) ||
      !write_fn || (!input && input_length) ||
      (!options->pointer && options->pointer_length) ||
      (!options->replacement_json && options->replacement_json_length) ||
      (!options->expected_json && options->expected_json_length) ||
      !lowercase_sha256(options->source_sha256,
                        options->source_sha256_length) ||
      (options->expected_absent != 0 && options->expected_absent != 1) ||
      (options->expected_absent &&
       (options->expected_json || options->expected_json_length)) ||
      (!options->expected_absent && options->expected_json_length == 0) ||
      options->replacement_json_length == 0) {
    if (engine) {
      return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                ARCHBIRD_NO_OFFSET,
                                "invalid JSON Pointer edit arguments");
    }
    return ARCHBIRD_INVALID_ARGUMENT;
  }
  out_result->start_byte = 0;
  out_result->end_byte = 0;
  out_result->matched_values = 0;
  out_result->kind = ARCHBIRD_JSON_POINTER_REPLACE;
  if (input_length > engine->options.max_input_bytes) {
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "JSON source length exceeds %zu bytes",
                              engine->options.max_input_bytes);
  }
  if (options->pointer_length > engine->options.max_string_bytes) {
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "JSON Pointer exceeds %zu bytes", engine->options.max_string_bytes);
  }
  status = ab_utf8_validate(engine, options->pointer, options->pointer_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = archbird_sha256(input, input_length, digest);
  if (status != ARCHBIRD_OK)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "failed to hash JSON source");
  archbird_sha256_hex(digest, source_sha256);
  if (memcmp(source_sha256, options->source_sha256, 64) != 0) {
    return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                              ARCHBIRD_NO_OFFSET,
                              "JSON source SHA-256 is stale");
  }
  status = canonicalize(engine, options->replacement_json,
                        options->replacement_json_length, &replacement_value);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  if (!options->expected_absent) {
    status = canonicalize(engine, options->expected_json,
                          options->expected_json_length, &expected);
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  status =
      archbird_json_parse_document(engine, input, input_length, &document, 0);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  root = yyjson_doc_get_root(document);
  ab_buffer_init(&token, engine);
  status = resolve_pointer(engine, root, options->pointer,
                           options->pointer_length, &target, &parent, &token);
  if (status != ARCHBIRD_OK) {
    ab_buffer_free(&token);
    goto cleanup;
  }
  if (options->expected_absent && target) {
    status =
        archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
                           "JSON Pointer target was expected to be absent");
    ab_buffer_free(&token);
    goto cleanup;
  }
  if (!options->expected_absent && !target) {
    status =
        archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
                           "JSON Pointer target was expected to exist");
    ab_buffer_free(&token);
    goto cleanup;
  }
  if (!target && (!parent || !yyjson_is_obj(parent))) {
    status = archbird_error_set(
        engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "absent JSON Pointer target requires an existing object parent");
    ab_buffer_free(&token);
    goto cleanup;
  }

  memset(&layout, 0, sizeof(layout));
  layout.target = target;
  layout.insertion_parent = target ? NULL : parent;
  skip_whitespace(input, input_length, &offset);
  status =
      scan_source_value(engine, input, input_length, &offset, root, &layout, 1);
  if (status == ARCHBIRD_OK) {
    skip_whitespace(input, input_length, &offset);
    if (offset != input_length)
      status = scan_error(engine, offset);
  }
  if (status != ARCHBIRD_OK) {
    ab_buffer_free(&token);
    goto cleanup;
  }

  if (target) {
    if (!layout.target_found) {
      status = scan_error(engine, ARCHBIRD_NO_OFFSET);
      ab_buffer_free(&token);
      goto cleanup;
    }
    status = canonicalize(engine, input + layout.target_start,
                          layout.target_end - layout.target_start, &actual);
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&token);
      goto cleanup;
    }
    if (actual.length != expected.length ||
        memcmp(actual.data, expected.data, actual.length) != 0) {
      status = archbird_error_set(
          engine, ARCHBIRD_POLICY_REJECTED, layout.target_start,
          "JSON Pointer expected value does not match current source");
      ab_buffer_free(&token);
      goto cleanup;
    }
    status = emit_edit(engine, &replacement_value, write_fn, user_data);
    if (status == ARCHBIRD_OK) {
      out_result->start_byte = layout.target_start;
      out_result->end_byte = layout.target_end;
      out_result->matched_values = 1;
      out_result->kind = ARCHBIRD_JSON_POINTER_REPLACE;
    }
  } else {
    if (!layout.parent_found) {
      status = scan_error(engine, ARCHBIRD_NO_OFFSET);
      ab_buffer_free(&token);
      goto cleanup;
    }
    ab_buffer_init(&replacement, engine);
    if (layout.member_count) {
      status = ab_buffer_literal(&replacement, ",");
      if (status == ARCHBIRD_OK) {
        status =
            ab_buffer_append(&replacement, input + layout.last_prefix_start,
                             layout.last_key_start - layout.last_prefix_start);
      }
    } else {
      status = ARCHBIRD_OK;
    }
    if (status == ARCHBIRD_OK) {
      status = ab_buffer_json_string(&replacement, (const char *)token.data,
                                     token.length);
    }
    if (status == ARCHBIRD_OK) {
      if (layout.member_count) {
        status =
            ab_buffer_append(&replacement, input + layout.last_key_end,
                             layout.last_value_start - layout.last_key_end);
      } else {
        status = ab_buffer_literal(&replacement, ":");
      }
    }
    if (status == ARCHBIRD_OK) {
      status = ab_buffer_append(&replacement, replacement_value.data,
                                replacement_value.length);
    }
    if (status == ARCHBIRD_OK)
      status = emit_edit(engine, &replacement, write_fn, user_data);
    if (status == ARCHBIRD_OK) {
      out_result->start_byte = layout.trailing_start;
      out_result->end_byte = layout.trailing_start;
      out_result->matched_values = 0;
      out_result->kind = ARCHBIRD_JSON_POINTER_INSERT;
    }
    ab_buffer_free(&replacement);
  }
  ab_buffer_free(&token);

cleanup:
  yyjson_doc_free(document);
  ab_buffer_free(&expected);
  ab_buffer_free(&actual);
  ab_buffer_free(&replacement_value);
  return status;
}
