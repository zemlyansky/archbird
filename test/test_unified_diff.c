#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Output {
  uint8_t *bytes;
  size_t length;
  size_t capacity;
} Output;

typedef struct Allocator {
  size_t allocations;
  size_t deallocations;
} Allocator;

static int output_write(void *user_data, const uint8_t *bytes, size_t length) {
  Output *output = (Output *)user_data;
  uint8_t *resized;
  size_t required;
  size_t capacity;
  if (length > SIZE_MAX - output->length - 1)
    return 1;
  required = output->length + length + 1;
  if (required > output->capacity) {
    capacity = output->capacity ? output->capacity : 256;
    while (capacity < required)
      capacity *= 2;
    resized = (uint8_t *)realloc(output->bytes, capacity);
    if (!resized)
      return 1;
    output->bytes = resized;
    output->capacity = capacity;
  }
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static void output_free(Output *output) {
  free(output->bytes);
  memset(output, 0, sizeof(*output));
}

static void *tracked_allocate(void *user_data, size_t size) {
  Allocator *allocator = (Allocator *)user_data;
  allocator->allocations++;
  return malloc(size);
}

static void *tracked_reallocate(void *user_data, void *pointer, size_t size) {
  Allocator *allocator = (Allocator *)user_data;
  if (!pointer)
    allocator->allocations++;
  return realloc(pointer, size);
}

static void tracked_deallocate(void *user_data, void *pointer) {
  Allocator *allocator = (Allocator *)user_data;
  allocator->deallocations++;
  free(pointer);
}

static int render(ArchbirdEngine *engine, const uint8_t *before,
                  size_t before_length, const uint8_t *after,
                  size_t after_length, const char *before_path,
                  const char *after_path,
                  const ArchbirdUnifiedDiffOptions *options, Output *output) {
  ArchbirdStatus status = archbird_unified_diff(
      engine, before, before_length, after, after_length, before_path,
      before_path ? strlen(before_path) : 0, after_path,
      after_path ? strlen(after_path) : 0, options, output_write, output);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "unified diff failed: %s\n", archbird_engine_error(engine));
    return 0;
  }
  return 1;
}

static int expect_equal(const Output *output, const char *expected,
                        const char *name) {
  size_t length = strlen(expected);
  if (output->length == length && memcmp(output->bytes, expected, length) == 0)
    return 1;
  fprintf(stderr, "%s differed\nexpected:\n%s\nactual:\n%.*s\n", name, expected,
          (int)output->length, output->bytes);
  return 0;
}

static int expect_contains(const Output *output, const char *expected,
                           const char *name) {
  if (output->bytes && strstr((const char *)output->bytes, expected))
    return 1;
  fprintf(stderr, "%s did not contain %s\nactual:\n%.*s\n", name, expected,
          (int)output->length, output->bytes);
  return 0;
}

int main(void) {
  static const char basic_expected[] = "diff --git a/demo.txt b/demo.txt\n"
                                       "--- a/demo.txt\n"
                                       "+++ b/demo.txt\n"
                                       "@@ -1,3 +1,3 @@\n"
                                       " one\n"
                                       "-two\n"
                                       "+TWO\n"
                                       " three\n";
  static const uint8_t invalid_utf8[] = {0xff, 0x00};
  ArchbirdEngineOptions engine_options;
  ArchbirdUnifiedDiffOptions options;
  ArchbirdEngine *engine = NULL;
  Allocator allocator = {0};
  Output output = {0};
  Output repeated = {0};
  int ok = 1;

  archbird_engine_options_init(&engine_options);
  engine_options.allocate = tracked_allocate;
  engine_options.reallocate = tracked_reallocate;
  engine_options.deallocate = tracked_deallocate;
  engine_options.allocator_user_data = &allocator;
  if (archbird_engine_create(&engine_options, &engine) != ARCHBIRD_OK)
    return 1;
  archbird_unified_diff_options_init(&options);

  ok = ok &&
       render(engine, (const uint8_t *)"one\ntwo\nthree\n", 14,
              (const uint8_t *)"one\nTWO\nthree\n", 14, "demo.txt", "demo.txt",
              &options, &output) &&
       expect_equal(&output, basic_expected, "basic exact diff");
  output_free(&output);

  ok = ok &&
       render(engine, (const uint8_t *)"A\nX\nA\nY\n", 8,
              (const uint8_t *)"A\nY\nA\nX\n", 8, "repeated.txt",
              "repeated.txt", &options, &output) &&
       render(engine, (const uint8_t *)"A\nX\nA\nY\n", 8,
              (const uint8_t *)"A\nY\nA\nX\n", 8, "repeated.txt",
              "repeated.txt", &options, &repeated) &&
       output.length == repeated.length &&
       memcmp(output.bytes, repeated.bytes, output.length) == 0 &&
       expect_contains(&output, "@@ -1,4 +1,4 @@\n", "repeated-line tie diff");
  output_free(&repeated);
  output_free(&output);

  ok = ok &&
       render(engine, (const uint8_t *)"caf\xc3\xa9\r\nold\r\n", 12,
              (const uint8_t *)"caf\xc3\xa9\r\nnew\r\n", 12, "utf8.txt",
              "utf8.txt", &options, &output) &&
       expect_contains(&output, " caf\xc3\xa9\r\n-old\r\n+new\r\n",
                       "UTF-8 CRLF diff");
  output_free(&output);

  ok = ok &&
       render(engine, (const uint8_t *)"old", 3, (const uint8_t *)"new", 3,
              "nonewline.txt", "nonewline.txt", &options, &output) &&
       expect_contains(&output,
                       "-old\n\\ No newline at end of file\n"
                       "+new\n\\ No newline at end of file\n",
                       "missing final newline diff");
  output_free(&output);

  ok = ok &&
       render(engine, invalid_utf8, sizeof(invalid_utf8),
              (const uint8_t *)"text\n", 5, "binary.dat", "binary.dat",
              &options, &output) &&
       expect_contains(&output,
                       "Binary files a/binary.dat and b/binary.dat differ\n",
                       "binary diff");
  output_free(&output);

  options.metadata = (const uint8_t *)"new file mode 100644\n";
  options.metadata_length = strlen((const char *)options.metadata);
  ok = ok &&
       render(engine, NULL, 0, (const uint8_t *)"created\n", 8, NULL,
              "created.txt", &options, &output) &&
       expect_contains(&output,
                       "diff --git a/created.txt b/created.txt\n"
                       "new file mode 100644\n"
                       "--- /dev/null\n"
                       "+++ b/created.txt\n"
                       "@@ -0,0 +1 @@\n"
                       "+created\n",
                       "file creation diff");
  output_free(&output);

  options.metadata = (const uint8_t *)"deleted file mode 100644\n";
  options.metadata_length = strlen((const char *)options.metadata);
  ok = ok &&
       render(engine, (const uint8_t *)"deleted\n", 8, NULL, 0, "deleted.txt",
              NULL, &options, &output) &&
       expect_contains(&output,
                       "deleted file mode 100644\n"
                       "--- a/deleted.txt\n"
                       "+++ /dev/null\n"
                       "@@ -1 +0,0 @@\n"
                       "-deleted\n",
                       "file deletion diff");
  output_free(&output);

  archbird_unified_diff_options_init(&options);
  options.max_work_bytes = 1;
  ok = ok &&
       render(engine, (const uint8_t *)"same\nold\nsame\n", 14,
              (const uint8_t *)"same\nnew\nsame\n", 14, "fallback.txt",
              "fallback.txt", &options, &output) &&
       expect_contains(&output,
                       "@@ -1,3 +1,3 @@\n"
                       "-same\n-old\n-same\n"
                       "+same\n+new\n+same\n",
                       "bounded fallback diff");
  output_free(&output);

  archbird_engine_destroy(engine);
  if (allocator.allocations == 0 ||
      allocator.allocations != allocator.deallocations) {
    fprintf(stderr, "allocator imbalance: allocations=%zu deallocations=%zu\n",
            allocator.allocations, allocator.deallocations);
    ok = 0;
  }
  return ok ? 0 : 1;
}
