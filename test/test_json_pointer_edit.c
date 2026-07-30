#include <archbird/archbird.h>

#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
  uint8_t *data;
  size_t length;
  size_t capacity;
  int fail;
} Buffer;

static int failures;

static void fail(const char *name, const char *message) {
  fprintf(stderr, "FAIL %s: %s\n", name, message);
  failures++;
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  Buffer *buffer = (Buffer *)user_data;
  uint8_t *resized;
  size_t required;
  if (buffer->fail)
    return 1;
  if (length > SIZE_MAX - buffer->length - 1)
    return 1;
  required = buffer->length + length + 1;
  if (required > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 128;
    while (capacity < required)
      capacity *= 2;
    resized = (uint8_t *)realloc(buffer->data, capacity);
    if (!resized)
      return 1;
    buffer->data = resized;
    buffer->capacity = capacity;
  }
  memcpy(buffer->data + buffer->length, bytes, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return 0;
}

static void buffer_reset(Buffer *buffer) {
  buffer->length = 0;
  buffer->fail = 0;
  if (buffer->data)
    buffer->data[0] = '\0';
}

static void expect_status(const char *name, ArchbirdStatus actual,
                          ArchbirdStatus expected) {
  if (actual != expected) {
    char message[128];
    (void)snprintf(message, sizeof(message), "status %d, expected %d", actual,
                   expected);
    fail(name, message);
  }
}

static void source_sha(const uint8_t *source, size_t source_length,
                       char output[65]) {
  uint8_t digest[32];
  if (archbird_sha256(source, source_length, digest) != ARCHBIRD_OK) {
    memset(output, '0', 64);
    output[64] = '\0';
    return;
  }
  archbird_sha256_hex(digest, output);
}

static ArchbirdStatus edit(ArchbirdEngine *engine, Buffer *buffer,
                           const char *source, const char *pointer,
                           int expected_absent, const char *expected,
                           const char *replacement,
                           ArchbirdJsonPointerEditResult *result) {
  ArchbirdJsonPointerEditOptions options;
  char sha256[65];
  source_sha((const uint8_t *)source, strlen(source), sha256);
  archbird_json_pointer_edit_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = 64;
  options.pointer = (const uint8_t *)pointer;
  options.pointer_length = strlen(pointer);
  options.expected_absent = expected_absent;
  options.expected_json = (const uint8_t *)expected;
  options.expected_json_length = expected ? strlen(expected) : 0;
  options.replacement_json = (const uint8_t *)replacement;
  options.replacement_json_length = strlen(replacement);
  archbird_json_pointer_edit_result_init(result);
  buffer_reset(buffer);
  return archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                    strlen(source), &options, result,
                                    buffer_write, buffer);
}

static void expect_output(const char *name, const Buffer *buffer,
                          const char *expected) {
  if (!buffer->data || strcmp((const char *)buffer->data, expected) != 0) {
    fprintf(stderr, "FAIL %s: output mismatch\nactual: %s\nexpected: %s\n",
            name, buffer->data ? (const char *)buffer->data : "<null>",
            expected);
    failures++;
  }
}

static void expect_applied(const char *name, const char *source,
                           const Buffer *replacement,
                           const ArchbirdJsonPointerEditResult *result,
                           const char *expected) {
  size_t source_length = strlen(source);
  size_t expected_length = strlen(expected);
  size_t actual_length;
  uint8_t *actual;
  if (result->start_byte > result->end_byte ||
      result->end_byte > source_length) {
    fail(name, "invalid result range");
    return;
  }
  actual_length = source_length - (result->end_byte - result->start_byte) +
                  replacement->length;
  if (actual_length != expected_length) {
    fail(name, "applied length mismatch");
    return;
  }
  actual = (uint8_t *)malloc(actual_length + 1);
  if (!actual) {
    fail(name, "allocation failed");
    return;
  }
  memcpy(actual, source, result->start_byte);
  memcpy(actual + result->start_byte, replacement->data, replacement->length);
  memcpy(actual + result->start_byte + replacement->length,
         source + result->end_byte, source_length - result->end_byte);
  actual[actual_length] = '\0';
  if (memcmp(actual, expected, actual_length + 1) != 0) {
    fprintf(stderr, "FAIL %s: applied mismatch\nactual: %s\nexpected: %s\n",
            name, actual, expected);
    failures++;
  }
  free(actual);
}

int main(void) {
  ArchbirdEngineOptions engine_options;
  ArchbirdEngine *engine = NULL;
  ArchbirdJsonPointerEditResult result;
  Buffer buffer = {0};
  ArchbirdStatus status;

  archbird_engine_options_init(&engine_options);
  expect_status("engine", archbird_engine_create(&engine_options, &engine),
                ARCHBIRD_OK);
  if (!engine)
    return 1;

  {
    const char *source =
        "{\n  \"exports\": {\n    \".\": \"./old.js\",\n    \"./x\": "
        "{\"types\":\"./x.d.ts\"}\n  },\n  \"name\": \"demo\"\n}\n";
    status = edit(engine, &buffer, source, "/exports/.", 0, "\"./old.js\"",
                  "\"./dist/index.js\"", &result);
    expect_status("nested-replace", status, ARCHBIRD_OK);
    if (result.kind != ARCHBIRD_JSON_POINTER_REPLACE ||
        result.matched_values != 1)
      fail("nested-replace-result", "expected one replaced value");
    expect_output("nested-replace-output", &buffer, "\"./dist/index.js\"");
    expect_applied(
        "nested-replace-applied", source, &buffer, &result,
        "{\n  \"exports\": {\n    \".\": \"./dist/index.js\",\n    "
        "\"./x\": {\"types\":\"./x.d.ts\"}\n  },\n  \"name\": \"demo\"\n}\n");
  }
  {
    const char *source =
        "{\n  \"scripts\": {\n    \"test\": \"node test.js\"\n  },\n  "
        "\"name\": \"demo\"\n}\n";
    status = edit(engine, &buffer, source, "/scripts/build", 1, NULL,
                  "\"node build.js\"", &result);
    expect_status("nested-insert", status, ARCHBIRD_OK);
    if (result.kind != ARCHBIRD_JSON_POINTER_INSERT ||
        result.matched_values != 0 || result.start_byte != result.end_byte)
      fail("nested-insert-result", "expected one absent-member insertion");
    expect_output("nested-insert-output", &buffer,
                  ",\n    \"build\": \"node build.js\"");
    expect_applied(
        "nested-insert-applied", source, &buffer, &result,
        "{\n  \"scripts\": {\n    \"test\": \"node test.js\",\n    "
        "\"build\": \"node build.js\"\n  },\n  \"name\": \"demo\"\n}\n");
  }
  {
    const char *source = "{\"a/b\":{\"~key\":1},\"items\":[\"a\",\"b\"]}";
    status = edit(engine, &buffer, source, "/a~1b/~0key", 0, "1.0",
                  "{\"z\":2,\"a\":1}", &result);
    expect_status("escaped-pointer-exact-number", status,
                  ARCHBIRD_POLICY_REJECTED);
    status = edit(engine, &buffer, source, "/a~1b/~0key", 0, "1",
                  "{\"z\":2,\"a\":1}", &result);
    expect_status("escaped-pointer-replace", status, ARCHBIRD_OK);
    expect_output("escaped-pointer-canonical-output", &buffer,
                  "{\"a\":1,\"z\":2}");
    status =
        edit(engine, &buffer, source, "/items/1", 0, "\"b\"", "\"c\"", &result);
    expect_status("array-element-replace", status, ARCHBIRD_OK);
    expect_output("array-element-output", &buffer, "\"c\"");
  }
  {
    const char *source = " \n{\"a\":1}\n";
    status =
        edit(engine, &buffer, source, "", 0, "{\"a\":1}", "[1,2]", &result);
    expect_status("root-replace", status, ARCHBIRD_OK);
    expect_output("root-replace-output", &buffer, "[1,2]");
    expect_applied("root-replace-applied", source, &buffer, &result,
                   " \n[1,2]\n");
  }
  {
    const char *source = "{\"value\":{\"a\":1,\"b\":2}}";
    status = edit(engine, &buffer, source, "/value", 0, "{\"b\":2,\"a\":1}",
                  "{\"done\":true}", &result);
    expect_status("canonical-object-precondition", status, ARCHBIRD_OK);
  }
  {
    const char *source = "{\"present\":null}";
    status =
        edit(engine, &buffer, source, "/present", 1, NULL, "true", &result);
    expect_status("absent-but-present", status, ARCHBIRD_POLICY_REJECTED);
    status =
        edit(engine, &buffer, source, "/missing", 0, "null", "true", &result);
    expect_status("present-but-missing", status, ARCHBIRD_POLICY_REJECTED);
    status = edit(engine, &buffer, source, "/missing/child", 1, NULL, "true",
                  &result);
    expect_status("missing-parent", status, ARCHBIRD_POLICY_REJECTED);
    status = edit(engine, &buffer, source, "/present/child", 1, NULL, "true",
                  &result);
    expect_status("scalar-parent", status, ARCHBIRD_POLICY_REJECTED);
    status =
        edit(engine, &buffer, source, "/missing", 1, NULL, "true", &result);
    expect_status("root-object-insert", status, ARCHBIRD_OK);
    expect_output("root-object-insert-output", &buffer, ",\"missing\":true");
  }
  {
    const char *source = "{\"a\":1,\"\\u0061\":2}";
    status = edit(engine, &buffer, source, "/a", 0, "1", "2", &result);
    expect_status("duplicate-key", status, ARCHBIRD_DUPLICATE_KEY);
  }
  {
    const char *source = "{\"a\":1}";
    ArchbirdJsonPointerEditOptions options;
    char sha256[65];
    source_sha((const uint8_t *)source, strlen(source), sha256);
    archbird_json_pointer_edit_options_init(&options);
    options.source_sha256 = sha256;
    options.source_sha256_length = 64;
    options.pointer = (const uint8_t *)"/a";
    options.pointer_length = 2;
    options.expected_json = (const uint8_t *)"1";
    options.expected_json_length = 1;
    options.replacement_json = (const uint8_t *)"2";
    options.replacement_json_length = 1;
    archbird_json_pointer_edit_result_init(&result);
    options.source_sha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    status = archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                        strlen(source), &options, &result,
                                        buffer_write, &buffer);
    expect_status("stale-source", status, ARCHBIRD_POLICY_REJECTED);
    options.source_sha256 = sha256;
    options.pointer = (const uint8_t *)"/bad~2token";
    options.pointer_length = 11;
    status = archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                        strlen(source), &options, &result,
                                        buffer_write, &buffer);
    expect_status("invalid-pointer", status, ARCHBIRD_INVALID_SCHEMA);
    options.pointer = (const uint8_t *)"/a";
    options.pointer_length = 2;
    options.replacement_json = (const uint8_t *)"{";
    status = archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                        strlen(source), &options, &result,
                                        buffer_write, &buffer);
    expect_status("invalid-replacement", status, ARCHBIRD_INVALID_JSON);
    options.replacement_json = (const uint8_t *)"2";
    buffer_reset(&buffer);
    buffer.fail = 1;
    status = archbird_json_pointer_edit(engine, (const uint8_t *)source,
                                        strlen(source), &options, &result,
                                        buffer_write, &buffer);
    expect_status("writer-failure", status, ARCHBIRD_WRITE_FAILED);
  }

  archbird_engine_destroy(engine);
  free(buffer.data);
  if (failures) {
    fprintf(stderr, "%d JSON Pointer edit test(s) failed\n", failures);
    return 1;
  }
  return 0;
}
