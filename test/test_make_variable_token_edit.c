#include <archbird/archbird.h>

#include "sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
  uint8_t *data;
  size_t length;
} Buffer;

static int failures;

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  Buffer *buffer = (Buffer *)user_data;
  uint8_t *copy = NULL;
  if (length) {
    copy = (uint8_t *)malloc(length);
    if (!copy)
      return 1;
    memcpy(copy, bytes, length);
  }
  free(buffer->data);
  buffer->data = copy;
  buffer->length = length;
  return 0;
}

static void source_sha(const char *source, char out[65]) {
  uint8_t digest[32];
  if (archbird_sha256((const uint8_t *)source, strlen(source), digest) !=
      ARCHBIRD_OK) {
    memset(out, '0', 64);
    out[64] = '\0';
    return;
  }
  archbird_sha256_hex(digest, out);
}

static ArchbirdStatus edit(ArchbirdEngine *engine, const char *source,
                           const char *sha256, const char *variable,
                           const char *expected, const char *replacement,
                           ArchbirdMakeVariableTokenEditResult *result,
                           Buffer *buffer) {
  ArchbirdMakeVariableTokenEditOptions options;
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = strlen(sha256);
  options.variable = (const uint8_t *)variable;
  options.variable_length = strlen(variable);
  options.expected_token = (const uint8_t *)expected;
  options.expected_token_length = strlen(expected);
  options.replacement_token = (const uint8_t *)replacement;
  options.replacement_token_length = strlen(replacement);
  archbird_make_variable_token_edit_result_init(result);
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  return archbird_make_variable_token_edit(engine, (const uint8_t *)source,
                                           strlen(source), &options, result,
                                           buffer_write, buffer);
}

static ArchbirdStatus insert(ArchbirdEngine *engine, const char *source,
                             const char *sha256, const char *variable,
                             const char *token, const char *anchor,
                             ArchbirdMakeVariableTokenPosition position,
                             ArchbirdMakeVariableTokenInsertResult *result,
                             Buffer *buffer) {
  ArchbirdMakeVariableTokenInsertOptions options;
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = strlen(sha256);
  options.variable = (const uint8_t *)variable;
  options.variable_length = strlen(variable);
  options.token = (const uint8_t *)token;
  options.token_length = strlen(token);
  options.anchor_token = (const uint8_t *)anchor;
  options.anchor_token_length = strlen(anchor);
  options.position = position;
  archbird_make_variable_token_insert_result_init(result);
  free(buffer->data);
  buffer->data = NULL;
  buffer->length = 0;
  return archbird_make_variable_token_insert(engine, (const uint8_t *)source,
                                             strlen(source), &options, result,
                                             buffer_write, buffer);
}

static void expect_applied(const char *name, const char *source,
                           const ArchbirdMakeVariableTokenEditResult *result,
                           const Buffer *replacement, const char *expected) {
  size_t source_length = strlen(source);
  size_t expected_length = strlen(expected);
  size_t actual_length = source_length -
                         (result->end_byte - result->start_byte) +
                         replacement->length;
  uint8_t *actual;
  if (result->start_byte > result->end_byte ||
      result->end_byte > source_length || actual_length != expected_length) {
    fprintf(stderr, "FAIL %s: invalid edit range\n", name);
    failures++;
    return;
  }
  actual = (uint8_t *)malloc(actual_length + 1);
  if (!actual) {
    fprintf(stderr, "FAIL %s: allocation failed\n", name);
    failures++;
    return;
  }
  memcpy(actual, source, result->start_byte);
  memcpy(actual + result->start_byte, replacement->data, replacement->length);
  memcpy(actual + result->start_byte + replacement->length,
         source + result->end_byte, source_length - result->end_byte);
  actual[actual_length] = '\0';
  if (memcmp(actual, expected, actual_length + 1) != 0) {
    fprintf(stderr, "FAIL %s: output mismatch\n", name);
    failures++;
  }
  free(actual);
}

int main(void) {
  static const char multiline[] = "OTHER = _core_add\n"
                                  "WASM_EXPORTS := _core_first \\\n"
                                  "\t_core_add \\\n"
                                  "\t_core_last # preserve this comment\n";
  static const char comma_list[] =
      "WASM_EXPORTS = _core_first,_core_add,_core_last\n";
  static const char duplicate[] = "WASM_EXPORTS = _core_add\n"
                                  "WASM_EXPORTS += _core_add\n";
  ArchbirdEngineOptions engine_options;
  ArchbirdEngine *engine = NULL;
  ArchbirdMakeVariableTokenEditResult result;
  ArchbirdMakeVariableTokenInsertResult insert_result;
  Buffer buffer = {0};
  char sha256[65];
  ArchbirdStatus status;

  archbird_engine_options_init(&engine_options);
  if (archbird_engine_create(&engine_options, &engine) != ARCHBIRD_OK)
    return 1;

  source_sha(multiline, sha256);
  status = edit(engine, multiline, sha256, "WASM_EXPORTS", "_core_add",
                "_core_sum", &result, &buffer);
  if (status != ARCHBIRD_OK || result.matched_tokens != 1) {
    fprintf(stderr, "FAIL multiline: %s\n", archbird_engine_error(engine));
    failures++;
  } else {
    expect_applied("multiline", multiline, &result, &buffer,
                   "OTHER = _core_add\n"
                   "WASM_EXPORTS := _core_first \\\n"
                   "\t_core_sum \\\n"
                   "\t_core_last # preserve this comment\n");
  }

  status = edit(engine, multiline, sha256, "WASM_EXPORTS", "_core_first", "",
                &result, &buffer);
  if (status != ARCHBIRD_OK || result.matched_tokens != 1) {
    fprintf(stderr, "FAIL removal: %s\n", archbird_engine_error(engine));
    failures++;
  } else {
    expect_applied("removal", multiline, &result, &buffer,
                   "OTHER = _core_add\n"
                   "WASM_EXPORTS := \\\n"
                   "\t_core_add \\\n"
                   "\t_core_last # preserve this comment\n");
  }

  {
    static const char trailing[] = "WASM_EXPORTS = _core_add\n";
    source_sha(trailing, sha256);
    status = edit(engine, trailing, sha256, "WASM_EXPORTS", "_core_add", "",
                  &result, &buffer);
    if (status != ARCHBIRD_OK || result.matched_tokens != 1) {
      fprintf(stderr, "FAIL trailing removal: %s\n",
              archbird_engine_error(engine));
      failures++;
    } else {
      expect_applied("trailing removal", trailing, &result, &buffer,
                     "WASM_EXPORTS =\n");
    }
  }

  source_sha(duplicate, sha256);
  status = edit(engine, duplicate, sha256, "WASM_EXPORTS", "_core_add",
                "_core_sum", &result, &buffer);
  if (status != ARCHBIRD_POLICY_REJECTED || result.matched_tokens != 2) {
    fprintf(stderr, "FAIL duplicate cardinality\n");
    failures++;
  }

  status =
      edit(engine, duplicate,
           "0000000000000000000000000000000000000000000000000000000000000000",
           "WASM_EXPORTS", "_core_add", "_core_sum", &result, &buffer);
  if (status != ARCHBIRD_POLICY_REJECTED) {
    fprintf(stderr, "FAIL stale source lock\n");
    failures++;
  }

  status = edit(engine, duplicate, sha256, "OTHER", "_core_add", "_core_sum",
                &result, &buffer);
  if (status != ARCHBIRD_POLICY_REJECTED || result.matched_tokens != 0) {
    fprintf(stderr, "FAIL wrong variable cardinality\n");
    failures++;
  }

  source_sha(comma_list, sha256);
  status = edit(engine, comma_list, sha256, "WASM_EXPORTS", "_core_add",
                "_core_sum", &result, &buffer);
  if (status != ARCHBIRD_OK || result.matched_tokens != 1) {
    fprintf(stderr, "FAIL comma replacement: %s\n",
            archbird_engine_error(engine));
    failures++;
  } else {
    expect_applied("comma replacement", comma_list, &result, &buffer,
                   "WASM_EXPORTS = _core_first,_core_sum,_core_last\n");
  }

  status = edit(engine, comma_list, sha256, "WASM_EXPORTS", "_core_add", "",
                &result, &buffer);
  if (status != ARCHBIRD_OK || result.matched_tokens != 1) {
    fprintf(stderr, "FAIL comma removal: %s\n", archbird_engine_error(engine));
    failures++;
  } else {
    expect_applied("comma removal", comma_list, &result, &buffer,
                   "WASM_EXPORTS = _core_first,_core_last\n");
  }

  source_sha(multiline, sha256);
  status =
      insert(engine, multiline, sha256, "WASM_EXPORTS", "_core_sum",
             "_core_add", ARCHBIRD_MAKE_TOKEN_AFTER, &insert_result, &buffer);
  if (status != ARCHBIRD_OK || insert_result.matched_tokens != 0 ||
      insert_result.matched_anchors != 1 ||
      insert_result.start_byte != insert_result.end_byte) {
    fprintf(stderr, "FAIL insertion: %s\n", archbird_engine_error(engine));
    failures++;
  } else {
    ArchbirdMakeVariableTokenEditResult applied_range;
    archbird_make_variable_token_edit_result_init(&applied_range);
    applied_range.start_byte = insert_result.start_byte;
    applied_range.end_byte = insert_result.end_byte;
    expect_applied("insertion", multiline, &applied_range, &buffer,
                   "OTHER = _core_add\n"
                   "WASM_EXPORTS := _core_first \\\n"
                   "\t_core_add _core_sum \\\n"
                   "\t_core_last # preserve this comment\n");
  }

  source_sha(comma_list, sha256);
  status =
      insert(engine, comma_list, sha256, "WASM_EXPORTS", "_core_sum",
             "_core_add", ARCHBIRD_MAKE_TOKEN_AFTER, &insert_result, &buffer);
  if (status != ARCHBIRD_OK || insert_result.matched_tokens != 0 ||
      insert_result.matched_anchors != 1) {
    fprintf(stderr, "FAIL comma insertion: %s\n",
            archbird_engine_error(engine));
    failures++;
  } else {
    ArchbirdMakeVariableTokenEditResult applied_range;
    archbird_make_variable_token_edit_result_init(&applied_range);
    applied_range.start_byte = insert_result.start_byte;
    applied_range.end_byte = insert_result.end_byte;
    expect_applied("comma insertion", comma_list, &applied_range, &buffer,
                   "WASM_EXPORTS = "
                   "_core_first,_core_add,_core_sum,_core_last\n");
  }

  source_sha(multiline, sha256);
  status = insert(engine, multiline, sha256, "WASM_EXPORTS", "_core_sum",
                  "_core_missing", ARCHBIRD_MAKE_TOKEN_BEFORE, &insert_result,
                  &buffer);
  if (status != ARCHBIRD_POLICY_REJECTED ||
      insert_result.matched_anchors != 0) {
    fprintf(stderr, "FAIL missing insertion anchor\n");
    failures++;
  }

  source_sha(duplicate, sha256);
  status =
      insert(engine, duplicate, sha256, "WASM_EXPORTS", "_core_sum",
             "_core_add", ARCHBIRD_MAKE_TOKEN_AFTER, &insert_result, &buffer);
  if (status != ARCHBIRD_POLICY_REJECTED ||
      insert_result.matched_anchors != 2) {
    fprintf(stderr, "FAIL duplicate insertion anchor\n");
    failures++;
  }

  free(buffer.data);
  archbird_engine_destroy(engine);
  return failures ? 1 : 0;
}
