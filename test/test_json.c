#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
  char *data;
  size_t length;
  size_t capacity;
  size_t write_calls;
  int fail;
} Buffer;

static int failures;

static void fail(const char *name, const char *message) {
  fprintf(stderr, "FAIL %s: %s\n", name, message);
  failures++;
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  Buffer *buffer = (Buffer *)user_data;
  char *resized;
  buffer->write_calls++;
  if (buffer->fail)
    return 1;
  if (length > SIZE_MAX - buffer->length - 1)
    return 1;
  if (buffer->length + length + 1 > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < buffer->length + length + 1) {
      if (capacity > SIZE_MAX / 2)
        return 1;
      capacity *= 2;
    }
    resized = (char *)realloc(buffer->data, capacity);
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
  buffer->write_calls = 0;
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

static void expect_text(const char *name, const Buffer *buffer,
                        const char *expected) {
  if (!buffer->data || strcmp(buffer->data, expected) != 0) {
    fprintf(stderr, "FAIL %s: output mismatch\nactual:   %s\nexpected: %s\n",
            name, buffer->data ? buffer->data : "<null>", expected);
    failures++;
  }
}

static ArchbirdStatus canonicalize(ArchbirdEngine *engine, Buffer *buffer,
                                   const char *json, uint32_t flags) {
  buffer_reset(buffer);
  return archbird_json_canonicalize(engine, (const uint8_t *)json, strlen(json),
                                    flags, buffer_write, buffer);
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  ArchbirdEngineOptions options;
  ArchbirdStatus status;
  Buffer buffer = {0};
  const char *mixed = "{\"z\":-0,\"a\":[true,null,\"x\\n\xc3\xa9\"],"
                      "\"big\":9007199254740993,\"nul\":\"x\\u0000y\"}";
  const char *compact =
      "{\"a\":[true,null,\"x\\n\xc3\xa9\"],\"big\":9007199254740993,"
      "\"nul\":\"x\\u0000y\",\"z\":0}";
  const char *pretty = "{\n"
                       "  \"a\": [\n"
                       "    true,\n"
                       "    null,\n"
                       "    \"x\\n\xc3\xa9\"\n"
                       "  ],\n"
                       "  \"big\": 9007199254740993,\n"
                       "  \"nul\": \"x\\u0000y\",\n"
                       "  \"z\": 0\n"
                       "}\n";
  static const uint8_t invalid_utf8[] = {'{',  '\"', 'a',  '\"', ':',
                                         '\"', 0xc0, 0xaf, '\"', '}'};

  archbird_engine_options_init(&options);
  {
    ArchbirdEngineOptions saved;
    size_t ordinary_limit = options.max_input_bytes;
    size_t ordinary_values = options.max_values;
    expect_status(
        "input-profile-null",
        archbird_engine_options_init_for_input(NULL, ARCHBIRD_INPUT_DEFAULT, 0),
        ARCHBIRD_INVALID_ARGUMENT);
    expect_status("input-profile-invalid",
                  archbird_engine_options_init_for_input(
                      &saved, (ArchbirdInputProfile)99, 0),
                  ARCHBIRD_INVALID_ARGUMENT);
    expect_status("ordinary-input-profile",
                  archbird_engine_options_init_for_input(
                      &saved, ARCHBIRD_INPUT_DEFAULT, ordinary_limit),
                  ARCHBIRD_OK);
    expect_status("ordinary-input-profile-limit",
                  archbird_engine_options_init_for_input(
                      &saved, ARCHBIRD_INPUT_DEFAULT, ordinary_limit + 1),
                  ARCHBIRD_LIMIT_EXCEEDED);
    expect_status(
        "saved-artifact-input-profile",
        archbird_engine_options_init_for_input(
            &saved, ARCHBIRD_INPUT_SAVED_ARTIFACT, ordinary_limit + 1),
        ARCHBIRD_OK);
    if (saved.max_input_bytes <= ordinary_limit ||
        saved.max_values <= ordinary_values ||
        saved.max_values == ordinary_limit + 1)
      fail("saved-artifact-input-profile",
           "saved-artifact limits are not fixed and distinct");
    expect_status("saved-artifact-input-profile-limit",
                  archbird_engine_options_init_for_input(
                      &saved, ARCHBIRD_INPUT_SAVED_ARTIFACT, SIZE_MAX),
                  ARCHBIRD_LIMIT_EXCEEDED);
  }
  expect_status("engine-create", archbird_engine_create(&options, &engine),
                ARCHBIRD_OK);
  if (!engine)
    return 1;

  status = canonicalize(engine, &buffer, mixed, 0);
  expect_status("compact-status", status, ARCHBIRD_OK);
  expect_text("compact-output", &buffer, compact);

  status = canonicalize(engine, &buffer, mixed,
                        ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE);
  expect_status("pretty-status", status, ARCHBIRD_OK);
  expect_text("pretty-output", &buffer, pretty);

  status = canonicalize(engine, &buffer, mixed, 0);
  expect_status("repeat-status", status, ARCHBIRD_OK);
  expect_text("repeat-output", &buffer, compact);

  status = canonicalize(
      engine, &buffer,
      "\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"", 0);
  expect_status("string-span-status", status, ARCHBIRD_OK);
  expect_text(
      "string-span-output", &buffer,
      "\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\"");
  if (buffer.write_calls != 1)
    fail("string-span-writes", "small JSON output was not written once");

  status = canonicalize(engine, &buffer, "{\"a\":1,\"\\u0061\":2}", 0);
  expect_status("escaped-duplicate", status, ARCHBIRD_DUPLICATE_KEY);
  if (archbird_engine_error_offset(engine) != ARCHBIRD_NO_OFFSET)
    fail("escaped-duplicate-offset", "expected an explicit unknown offset");
  status = canonicalize(engine, &buffer, "{\"a\":1}junk", 0);
  expect_status("trailing-content", status, ARCHBIRD_INVALID_JSON);
  if (archbird_engine_error_offset(engine) != 7)
    fail("trailing-content-offset", "expected parse byte offset 7");
  expect_status("real-number", canonicalize(engine, &buffer, "{\"a\":1.0}", 0),
                ARCHBIRD_OK);
  expect_text("real-number-output", &buffer, "{\"a\":1.0}");
  expect_status("real-exponent",
                canonicalize(engine, &buffer,
                             "[1e-9,0.000000001,1e20,1e16,1e-6,1e-7]", 0),
                ARCHBIRD_OK);
  expect_text("real-exponent-output", &buffer,
              "[1e-09,1e-09,1e+20,1e+16,1e-06,1e-07]");
  expect_status("real-number-valid-json",
                archbird_json_validate(engine, (const uint8_t *)"{\"a\":1.0}",
                                       strlen("{\"a\":1.0}")),
                ARCHBIRD_OK);
  expect_status("overflowing-real",
                canonicalize(engine, &buffer, "{\"a\":1e400}", 0),
                ARCHBIRD_UNSUPPORTED_NUMBER);
  expect_status("overflowing-real-valid-json",
                archbird_json_validate(engine, (const uint8_t *)"{\"a\":1e400}",
                                       strlen("{\"a\":1e400}")),
                ARCHBIRD_OK);
  expect_status(
      "invalid-utf8",
      archbird_json_validate(engine, invalid_utf8, sizeof(invalid_utf8)),
      ARCHBIRD_INVALID_JSON);
  expect_status(
      "huge-integer",
      canonicalize(engine, &buffer,
                   "12345678901234567890123456789012345678901234567890", 0),
      ARCHBIRD_OK);
  expect_text("huge-integer-output", &buffer,
              "12345678901234567890123456789012345678901234567890");

  buffer.fail = 1;
  status = archbird_json_canonicalize(engine, (const uint8_t *)"{}", 2, 0,
                                      buffer_write, &buffer);
  expect_status("writer-failure", status, ARCHBIRD_WRITE_FAILED);

  archbird_engine_destroy(engine);
  engine = NULL;

  archbird_engine_options_init(&options);
  options.max_depth = 3;
  expect_status("limited-engine", archbird_engine_create(&options, &engine),
                ARCHBIRD_OK);
  expect_status("depth-limit", canonicalize(engine, &buffer, "[[[[]]]]", 0),
                ARCHBIRD_LIMIT_EXCEEDED);
  archbird_engine_destroy(engine);
  engine = NULL;

  archbird_engine_options_init(&options);
  options.max_input_bytes = 2;
  expect_status("input-limited-engine",
                archbird_engine_create(&options, &engine), ARCHBIRD_OK);
  expect_status("input-limit",
                archbird_json_validate(engine, (const uint8_t *)"[0]", 3),
                ARCHBIRD_LIMIT_EXCEEDED);
  archbird_engine_destroy(engine);
  engine = NULL;

  archbird_engine_options_init(&options);
  options.max_values = 2;
  expect_status("value-limited-engine",
                archbird_engine_create(&options, &engine), ARCHBIRD_OK);
  expect_status("value-limit",
                archbird_json_validate(engine, (const uint8_t *)"[1,2]", 5),
                ARCHBIRD_LIMIT_EXCEEDED);
  archbird_engine_destroy(engine);
  engine = NULL;

  archbird_engine_options_init(&options);
  options.max_string_bytes = 2;
  expect_status("string-limited-engine",
                archbird_engine_create(&options, &engine), ARCHBIRD_OK);
  expect_status("value-string-limit",
                archbird_json_validate(engine, (const uint8_t *)"\"abc\"", 5),
                ARCHBIRD_LIMIT_EXCEEDED);
  expect_status(
      "key-string-limit",
      archbird_json_validate(engine, (const uint8_t *)"{\"abc\":0}", 9),
      ARCHBIRD_LIMIT_EXCEEDED);
  archbird_engine_destroy(engine);
  free(buffer.data);

  if (failures) {
    fprintf(stderr, "%d native JSON test(s) failed\n", failures);
    return 1;
  }
  puts("native JSON tests passed");
  return 0;
}
