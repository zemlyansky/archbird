#include "archbird_internal.h"
#include "makefile.h"
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

static void scan_tokens(const uint8_t *input, size_t start, size_t end,
                        const uint8_t *expected, size_t expected_length,
                        size_t *matches, size_t *match_start,
                        size_t *match_end) {
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
    if (index - token_start == expected_length &&
        memcmp(input + token_start, expected, expected_length) == 0) {
      (*matches)++;
      *match_start = token_start;
      *match_end = index;
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

ArchbirdStatus archbird_make_variable_token_edit(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length,
    const ArchbirdMakeVariableTokenEditOptions *options,
    ArchbirdMakeVariableTokenEditResult *out_result, ArchbirdWriteFn write_fn,
    void *user_data) {
  uint8_t digest[32];
  char source_sha256[65];
  size_t offset = 0;
  size_t matches = 0;
  size_t match_start = 0;
  size_t match_end = 0;
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
        name_end - name_start == options->variable_length &&
        memcmp(input + offset + name_start, options->variable,
               options->variable_length) == 0)
      target = 1;
    continued = line_continues(input, offset, end, &content_end);
    if (target)
      scan_tokens(input, offset + value_start, content_end,
                  options->expected_token, options->expected_token_length,
                  &matches, &match_start, &match_end);
    offset = next;
    while (continued && offset < input_length) {
      size_t scan_start;
      end = line_end(input, input_length, offset, &next);
      scan_start = offset;
      while (scan_start < end && isspace((unsigned char)input[scan_start]))
        scan_start++;
      continued = line_continues(input, offset, end, &content_end);
      if (target)
        scan_tokens(input, scan_start, content_end, options->expected_token,
                    options->expected_token_length, &matches, &match_start,
                    &match_end);
      offset = next;
    }
  }
  out_result->matched_tokens = matches;
  if (matches != 1)
    return archbird_error_set(
        engine, ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
        "Make variable token edit expected one match but found %zu", matches);
  out_result->start_byte = match_start;
  out_result->end_byte = match_end;
  if (write_fn(user_data, options->replacement_token,
               options->replacement_token_length) != 0)
    return archbird_error_set(engine, ARCHBIRD_WRITE_FAILED, ARCHBIRD_NO_OFFSET,
                              "Make variable token edit callback failed");
  return ARCHBIRD_OK;
}
