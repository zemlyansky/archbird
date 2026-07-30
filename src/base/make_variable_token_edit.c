#include "archbird_internal.h"
#include "makefile.h"
#include "render_internal.h"
#include "sha256.h"
#include "utf8.h"

#include <ctype.h>
#include <string.h>

static int lowercase_sha256(const char *value, size_t length) {
  size_t index;
  if (!value || length != 64)
    return 0;
  for (index = 0; index < length; index++) {
    if (!((value[index] >= '0' && value[index] <= '9') ||
          (value[index] >= 'a' && value[index] <= 'f')))
      return 0;
  }
  return 1;
}

static int valid_variable(const uint8_t *value, size_t length) {
  size_t index;
  if (!value || !length ||
      !(isalpha((unsigned char)value[0]) || value[0] == '_'))
    return 0;
  for (index = 1; index < length; index++) {
    if (!(isalnum((unsigned char)value[index]) || value[index] == '_'))
      return 0;
  }
  return 1;
}

static int valid_direct_token(const uint8_t *value, size_t length,
                              int allow_empty) {
  size_t index;
  if ((!value && length) || (!allow_empty && !length))
    return 0;
  for (index = 0; index < length; index++) {
    if (value[index] == '\0' || value[index] == '#' ||
        isspace((unsigned char)value[index]))
      return 0;
  }
  return 1;
}

static size_t line_end(const uint8_t *input, size_t length, size_t start,
                       size_t *next) {
  size_t end = start;
  while (end < length && input[end] != '\n')
    end++;
  *next = end < length ? end + 1 : end;
  if (end > start && input[end - 1] == '\r')
    end--;
  return end;
}

static size_t comment_start(const uint8_t *input, size_t start, size_t end) {
  size_t index;
  int escaped = 0;
  for (index = start; index < end; index++) {
    if (input[index] == '#' && !escaped)
      return index;
    if (input[index] == '\\')
      escaped = !escaped;
    else
      escaped = 0;
  }
  return end;
}

static int line_continues(const uint8_t *input, size_t start, size_t end,
                          size_t *content_end) {
  size_t trimmed = comment_start(input, start, end);
  size_t slashes = 0;
  while (trimmed > start && isspace((unsigned char)input[trimmed - 1]))
    trimmed--;
  while (trimmed > start && input[trimmed - 1] == '\\') {
    slashes++;
    trimmed--;
  }
  if (slashes % 2) {
    *content_end = trimmed + slashes - 1;
    return 1;
  }
  *content_end = trimmed + slashes;
  return 0;
}

typedef struct TokenSearch {
  const uint8_t *token;
  size_t length;
  size_t matches;
  size_t start;
  size_t end;
} TokenSearch;

static void match_token(const uint8_t *input, size_t start, size_t end,
                        TokenSearch *search) {
  if (search && end - start == search->length &&
      memcmp(input + start, search->token, search->length) == 0) {
    search->matches++;
    search->start = start;
    search->end = end;
  }
}

static void scan_tokens(const uint8_t *input, size_t start, size_t end,
                        TokenSearch *first, TokenSearch *second) {
  size_t index = start;
  while (index < end) {
    size_t token_start;
    while (index < end && isspace((unsigned char)input[index]))
      index++;
    if (index == end)
      break;
    token_start = index;
    while (index < end && !isspace((unsigned char)input[index])) {
      if (input[index] == '\\' && index + 1 < end)
        index += 2;
      else
        index++;
    }
    match_token(input, token_start, index, first);
    match_token(input, token_start, index, second);
  }
}

static void scan_variable_tokens(const uint8_t *input, size_t input_length,
                                 const uint8_t *variable,
                                 size_t variable_length, TokenSearch *first,
                                 TokenSearch *second) {
  size_t offset = 0;
  while (offset < input_length) {
    size_t next;
    size_t end = line_end(input, input_length, offset, &next);
    size_t name_start;
    size_t name_end;
    size_t operator_start;
    size_t operator_length;
    size_t value_start;
    int target = 0;
    int continued;
    size_t content_end;

    if (offset < end && input[offset] != '\t' &&
        ab_make_assignment((const char *)input + offset, end - offset,
                           &name_start, &name_end, &operator_start,
                           &operator_length, &value_start) &&
        name_end - name_start == variable_length &&
        memcmp(input + offset + name_start, variable, variable_length) == 0)
      target = 1;
    continued = line_continues(input, offset, end, &content_end);
    if (target)
      scan_tokens(input, offset + value_start, content_end, first, second);
    offset = next;
    while (continued && offset < input_length) {
      size_t scan_start;
      end = line_end(input, input_length, offset, &next);
      scan_start = offset;
      while (scan_start < end && isspace((unsigned char)input[scan_start]))
        scan_start++;
      continued = line_continues(input, offset, end, &content_end);
      if (target)
        scan_tokens(input, scan_start, content_end, first, second);
      offset = next;
    }
  }
}

void archbird_make_variable_token_edit_options_init(
    ArchbirdMakeVariableTokenEditOptions *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
}

void archbird_make_variable_token_edit_result_init(
    ArchbirdMakeVariableTokenEditResult *result) {
  if (!result)
    return;
  memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
}

void archbird_make_variable_token_insert_options_init(
    ArchbirdMakeVariableTokenInsertOptions *options) {
  if (!options)
    return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
}

void archbird_make_variable_token_insert_result_init(
    ArchbirdMakeVariableTokenInsertResult *result) {
  if (!result)
    return;
  memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
}

ArchbirdStatus archbird_make_variable_token_edit(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length,
    const ArchbirdMakeVariableTokenEditOptions *options,
    ArchbirdMakeVariableTokenEditResult *out_result, ArchbirdWriteFn write_fn,
    void *user_data) {
  uint8_t digest[32];
  char source_sha256[65];
  TokenSearch expected = {0};
  ArchbirdStatus status;

  if (!engine || !options || options->struct_size != sizeof(*options) ||
      !out_result || out_result->struct_size != sizeof(*out_result) ||
      !write_fn || (!input && input_length) ||
      !lowercase_sha256(options->source_sha256,
                        options->source_sha256_length) ||
      !valid_variable(options->variable, options->variable_length) ||
      !valid_direct_token(options->expected_token,
                          options->expected_token_length, 0) ||
      !valid_direct_token(options->replacement_token,
                          options->replacement_token_length, 1) ||
      (options->expected_token_length == options->replacement_token_length &&
       memcmp(options->expected_token, options->replacement_token,
              options->expected_token_length) == 0)) {
    if (engine)
      return archbird_error_set(engine, ARCHBIRD_INVALID_ARGUMENT,
                                ARCHBIRD_NO_OFFSET,
                                "invalid Make variable token edit arguments");
    return ARCHBIRD_INVALID_ARGUMENT;
  }
  out_result->start_byte = 0;
  out_result->end_byte = 0;
  out_result->matched_tokens = 0;
  if (input_length > engine->options.max_input_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Make source length exceeds %zu bytes",
                              engine->options.max_input_bytes);
  if (options->variable_length > engine->options.max_string_bytes ||
      options->expected_token_length > engine->options.max_string_bytes ||
      options->replacement_token_length > engine->options.max_string_bytes)
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "Make edit value exceeds %zu bytes", engine->options.max_string_bytes);
  status = ab_utf8_validate(engine, input, input_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_utf8_validate(engine, options->expected_token,
                            options->expected_token_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_utf8_validate(engine, options->replacement_token,
                            options->replacement_token_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = archbird_sha256(input, input_length, digest);
  if (status != ARCHBIRD_OK)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "failed to hash Make source");
  archbird_sha256_hex(digest, source_sha256);
  if (memcmp(source_sha256, options->source_sha256, 64) != 0)
    return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                              ARCHBIRD_NO_OFFSET,
                              "Make source SHA-256 is stale");

  expected.token = options->expected_token;
  expected.length = options->expected_token_length;
  scan_variable_tokens(input, input_length, options->variable,
                       options->variable_length, &expected, NULL);
  out_result->matched_tokens = expected.matches;
  if (expected.matches != 1)
    return archbird_error_set(
        engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "Make variable token edit expected one match but found %zu",
        expected.matches);
  out_result->start_byte = expected.start;
  out_result->end_byte = expected.end;
  if (options->replacement_token_length == 0 &&
      out_result->end_byte < input_length &&
      (input[out_result->end_byte] == ' ' ||
       input[out_result->end_byte] == '\t'))
    out_result->end_byte++;
  else if (options->replacement_token_length == 0 &&
           out_result->start_byte > 0 &&
           (input[out_result->start_byte - 1] == ' ' ||
            input[out_result->start_byte - 1] == '\t'))
    out_result->start_byte--;
  if (write_fn(user_data, options->replacement_token,
               options->replacement_token_length) != 0)
    return archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                              "Make variable token edit callback failed");
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_make_variable_token_insert(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length,
    const ArchbirdMakeVariableTokenInsertOptions *options,
    ArchbirdMakeVariableTokenInsertResult *out_result, ArchbirdWriteFn write_fn,
    void *user_data) {
  uint8_t digest[32];
  char source_sha256[65];
  TokenSearch token = {0};
  TokenSearch anchor = {0};
  AbBuffer replacement;
  ArchbirdStatus status;

  if (!engine || !options || options->struct_size != sizeof(*options) ||
      !out_result || out_result->struct_size != sizeof(*out_result) ||
      !write_fn || (!input && input_length) ||
      !lowercase_sha256(options->source_sha256,
                        options->source_sha256_length) ||
      !valid_variable(options->variable, options->variable_length) ||
      !valid_direct_token(options->token, options->token_length, 0) ||
      !valid_direct_token(options->anchor_token, options->anchor_token_length,
                          0) ||
      (options->token_length == options->anchor_token_length &&
       memcmp(options->token, options->anchor_token, options->token_length) ==
           0) ||
      (options->position != ARCHBIRD_MAKE_TOKEN_BEFORE &&
       options->position != ARCHBIRD_MAKE_TOKEN_AFTER)) {
    if (engine)
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
          "invalid Make variable token insertion arguments");
    return ARCHBIRD_INVALID_ARGUMENT;
  }
  out_result->start_byte = 0;
  out_result->end_byte = 0;
  out_result->matched_tokens = 0;
  out_result->matched_anchors = 0;
  if (input_length > engine->options.max_input_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Make source length exceeds %zu bytes",
                              engine->options.max_input_bytes);
  if (options->variable_length > engine->options.max_string_bytes ||
      options->token_length > engine->options.max_string_bytes ||
      options->anchor_token_length > engine->options.max_string_bytes)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Make insertion value exceeds %zu bytes",
                              engine->options.max_string_bytes);
  status = ab_utf8_validate(engine, input, input_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_utf8_validate(engine, options->token, options->token_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_utf8_validate(engine, options->anchor_token,
                            options->anchor_token_length);
  if (status != ARCHBIRD_OK)
    return status;
  status = archbird_sha256(input, input_length, digest);
  if (status != ARCHBIRD_OK)
    return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                              "failed to hash Make source");
  archbird_sha256_hex(digest, source_sha256);
  if (memcmp(source_sha256, options->source_sha256, 64) != 0)
    return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                              ARCHBIRD_NO_OFFSET,
                              "Make source SHA-256 is stale");
  token.token = options->token;
  token.length = options->token_length;
  anchor.token = options->anchor_token;
  anchor.length = options->anchor_token_length;
  scan_variable_tokens(input, input_length, options->variable,
                       options->variable_length, &token, &anchor);
  out_result->matched_tokens = token.matches;
  out_result->matched_anchors = anchor.matches;
  if (token.matches != 0 || anchor.matches != 1)
    return archbird_error_set(
        engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "Make variable token insertion expected zero token matches and one "
        "anchor but found %zu and %zu",
        token.matches, anchor.matches);
  out_result->start_byte = options->position == ARCHBIRD_MAKE_TOKEN_BEFORE
                               ? anchor.start
                               : anchor.end;
  out_result->end_byte = out_result->start_byte;
  ab_buffer_init(&replacement, engine);
  if (options->position == ARCHBIRD_MAKE_TOKEN_AFTER)
    status = ab_buffer_literal(&replacement, " ");
  else
    status = ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&replacement, options->token, options->token_length);
  if (status == ARCHBIRD_OK && options->position == ARCHBIRD_MAKE_TOKEN_BEFORE)
    status = ab_buffer_literal(&replacement, " ");
  if (status == ARCHBIRD_OK &&
      write_fn(user_data, replacement.data, replacement.length) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                           "Make variable token insertion callback failed");
  ab_buffer_free(&replacement);
  return status;
}
