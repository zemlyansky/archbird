#include "archbird_internal.h"
#include "pattern.h"
#include "scip_fixture.h"
#include "sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ALLOCATION_MAGIC UINT64_C(0x6172636862697264)

typedef union TestAllocationHeader TestAllocationHeader;

union TestAllocationHeader {
  max_align_t alignment;
  struct {
    uint64_t magic;
    size_t size;
    size_t call;
    TestAllocationHeader *next;
  } value;
};

typedef struct TestAllocator {
  size_t calls;
  size_t fail_at;
  size_t outstanding;
  size_t bytes;
  TestAllocationHeader *head;
  int failed;
  int corrupted;
  char error[512];
} TestAllocator;

typedef struct FixedOutput {
  uint8_t bytes[65536];
  size_t length;
} FixedOutput;

typedef struct CountingOutput {
  size_t length;
} CountingOutput;

typedef struct HeapOutput {
  uint8_t *bytes;
  size_t length;
  size_t capacity;
} HeapOutput;

static int failures;
static uint8_t *report_map;
static size_t report_map_length;
static uint8_t *report_verification;
static size_t report_verification_length;

static void fail(const char *name, const char *message) {
  fprintf(stderr, "FAIL %s: %s\n", name, message);
  failures++;
}

static int should_fail(TestAllocator *allocator) {
  allocator->calls++;
  if (allocator->fail_at && allocator->calls == allocator->fail_at) {
    allocator->failed = 1;
    return 1;
  }
  return 0;
}

static void *test_allocate(void *user_data, size_t size) {
  TestAllocator *allocator = (TestAllocator *)user_data;
  TestAllocationHeader *header;
  if (should_fail(allocator) || size > SIZE_MAX - sizeof(*header))
    return NULL;
  header = (TestAllocationHeader *)malloc(sizeof(*header) + size);
  if (!header)
    return NULL;
  header->value.magic = TEST_ALLOCATION_MAGIC;
  header->value.size = size;
  header->value.call = allocator->calls;
  header->value.next = allocator->head;
  allocator->head = header;
  allocator->outstanding++;
  allocator->bytes += size;
  return header + 1;
}

static void *test_reallocate(void *user_data, void *pointer, size_t size) {
  TestAllocator *allocator = (TestAllocator *)user_data;
  TestAllocationHeader *header;
  TestAllocationHeader *resized;
  TestAllocationHeader **link;
  size_t old_size;
  if (!pointer)
    return test_allocate(user_data, size);
  header = (TestAllocationHeader *)pointer - 1;
  if (header->value.magic != TEST_ALLOCATION_MAGIC) {
    allocator->corrupted = 1;
    return NULL;
  }
  if (should_fail(allocator) || size > SIZE_MAX - sizeof(*header))
    return NULL;
  old_size = header->value.size;
  link = &allocator->head;
  while (*link && *link != header)
    link = &(*link)->value.next;
  if (!*link) {
    allocator->corrupted = 1;
    return NULL;
  }
  resized = (TestAllocationHeader *)realloc(header, sizeof(*header) + size);
  if (!resized)
    return NULL;
  resized->value.magic = TEST_ALLOCATION_MAGIC;
  resized->value.size = size;
  resized->value.call = allocator->calls;
  *link = resized;
  allocator->bytes -= old_size;
  allocator->bytes += size;
  return resized + 1;
}

static void test_deallocate(void *user_data, void *pointer) {
  TestAllocator *allocator = (TestAllocator *)user_data;
  TestAllocationHeader *header;
  TestAllocationHeader **link;
  if (!pointer)
    return;
  header = (TestAllocationHeader *)pointer - 1;
  if (header->value.magic != TEST_ALLOCATION_MAGIC || !allocator->outstanding ||
      allocator->bytes < header->value.size) {
    allocator->corrupted = 1;
    return;
  }
  link = &allocator->head;
  while (*link && *link != header)
    link = &(*link)->value.next;
  if (!*link) {
    allocator->corrupted = 1;
    return;
  }
  *link = header->value.next;
  allocator->outstanding--;
  allocator->bytes -= header->value.size;
  header->value.magic = 0;
  free(header);
}

static ArchbirdEngine *create_engine(TestAllocator *allocator,
                                     ArchbirdStatus *out_status) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  archbird_engine_options_init(&options);
  options.allocate = test_allocate;
  options.reallocate = test_reallocate;
  options.deallocate = test_deallocate;
  options.allocator_user_data = allocator;
  *out_status = archbird_engine_create(&options, &engine);
  return engine;
}

static int fixed_write(void *user_data, const uint8_t *bytes, size_t length) {
  FixedOutput *output = (FixedOutput *)user_data;
  if (length > sizeof(output->bytes) - output->length - 1)
    return 1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->bytes[output->length] = '\0';
  return 0;
}

static int count_write(void *user_data, const uint8_t *bytes, size_t length) {
  CountingOutput *output = (CountingOutput *)user_data;
  (void)bytes;
  if (length > SIZE_MAX - output->length)
    return 1;
  output->length += length;
  return 0;
}

static int heap_write(void *user_data, const uint8_t *bytes, size_t length) {
  HeapOutput *output = (HeapOutput *)user_data;
  uint8_t *resized;
  size_t capacity;
  if (length > SIZE_MAX - output->length)
    return 1;
  if (output->length + length > output->capacity) {
    capacity = output->capacity ? output->capacity : 4096;
    while (capacity < output->length + length) {
      if (capacity > SIZE_MAX / 2)
        return 1;
      capacity *= 2;
    }
    resized = (uint8_t *)realloc(output->bytes, capacity);
    if (!resized)
      return 1;
    output->bytes = resized;
    output->capacity = capacity;
  }
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  return 0;
}

static ArchbirdStatus exercise_json(TestAllocator *allocator) {
  static const char input[] =
      "{\"z\":[3,2,1,{\"nested\":true}],\"a\":1.25,\"text\":\"value\"}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  FixedOutput output = {{0}, 0};
  if (!engine)
    return status;
  status =
      archbird_json_canonicalize(engine, (const uint8_t *)input,
                                 sizeof(input) - 1, 0, fixed_write, &output);
  if (status == ARCHBIRD_OK &&
      (output.length != sizeof(input) - 1 ||
       memcmp(output.bytes,
              "{\"a\":1.25,\"text\":\"value\",\"z\":[3,2,1,{\"nested\":true}]}",
              output.length) != 0))
    status = ARCHBIRD_CONFLICT;
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_pattern(TestAllocator *allocator) {
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  AbPattern *pattern = NULL;
  AbString source = {"(\\p{L}+)", 8};
  size_t count = 0;
  if (!engine)
    return status;
  status = ab_pattern_compile(engine, &source, 1, &pattern);
  if (status == ARCHBIRD_OK)
    status = ab_pattern_scan(engine, pattern, (const uint8_t *)"alpha beta", 10,
                             1, NULL, NULL, &count);
  if (status == ARCHBIRD_OK && count != 2)
    status = ARCHBIRD_CONFLICT;
  if (status != ARCHBIRD_OK && status != ARCHBIRD_OUT_OF_MEMORY)
    fprintf(stderr, "pattern allocator status %d: %s\n", status,
            archbird_engine_error(engine));
  ab_pattern_free(pattern);
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_config_resolution(TestAllocator *allocator) {
  static const char request[] =
      "{\"artifact\":\"archbird-map-request\",\"schema_version\":1,"
      "\"sources\":[]}";
  static const char inventory[] =
      "{\"artifact\":\"archbird-repository-inventory\",\"documents\":[],"
      "\"files\":[{\"bytes\":5000001,\"path\":\"huge.py\"},"
      "{\"bytes\":12,\"path\":\"src/main.py\"}],\"ignore_files\":[],"
      "\"schema_version\":1}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  CountingOutput output = {0};
  if (!engine)
    return status;
  status = archbird_discovery_resolve(
      engine, NULL, 0, (const uint8_t *)request, sizeof(request) - 1,
      (const uint8_t *)inventory, sizeof(inventory) - 1, 0, count_write,
      &output);
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  if (status != ARCHBIRD_OK)
    (void)snprintf(allocator->error, sizeof(allocator->error),
                   "%s (output=%zu)", archbird_engine_error(engine),
                   output.length);
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_map(TestAllocator *allocator) {
  static const char manifest[] =
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":3,"
      "\"language\":\"c\",\"layer\":\"core\",\"path\":\"src/a.c\","
      "\"roles\":[\"source\"],\"sha256\":"
      "\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\""
      "}],\"producer\":{\"implementation_sha256\":"
      "\"1111111111111111111111111111111111111111111111111111111111111111\","
      "\"name\":\"allocator-test\",\"version\":\"1\"},\"project\":"
      "\"allocator-test\",\"schema_version\":1}";
  static const char config[] =
      "{\"schema_version\":2,\"project\":\"allocator-test\","
      "\"description\":\"Allocator ownership fixture\",\"layers\":[{"
      "\"name\":\"core\",\"role\":\"core\",\"language\":\"c\","
      "\"globs\":[\"src/**\"]}],\"components\":[{\"name\":\"core\","
      "\"paths\":[\"src/**\"]}],\"limits\":{\"compact_symbols\":7}}";
  static const char query[] =
      "{\"paths\":[\"src/a.c\"],\"direction\":\"both\",\"depth\":0,"
      "\"test_depth\":0}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  ArchbirdProject *project = NULL;
  FixedOutput map_output = {{0}, 0};
  CountingOutput output = {0};
  if (!engine)
    return status;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   sizeof(manifest) - 1, &project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, "src/a.c", 7,
                                         (const uint8_t *)"abc", 3);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)config, sizeof(config) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin(engine, project,
                                           ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_render_map(engine, project, 0, fixed_write,
                                         &map_output);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown(engine, map_output.bytes,
                                          map_output.length, 0, 0, count_write,
                                          &output);
  if (status == ARCHBIRD_OK)
    status = archbird_map_render_markdown(engine, map_output.bytes,
                                          map_output.length, 1, 0, count_write,
                                          &output);
  if (status == ARCHBIRD_OK)
    status = archbird_map_query_markdown(
        engine, map_output.bytes, map_output.length, (const uint8_t *)query,
        sizeof(query) - 1, 0, count_write, &output);
  if (status == ARCHBIRD_OK)
    status = archbird_map_freshness(engine, map_output.bytes, map_output.length,
                                    map_output.bytes, map_output.length, 0,
                                    count_write, &output);
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_scip(TestAllocator *allocator) {
  static const uint8_t source[] = "int add(void) { return 1; }\n";
  static const char config[] =
      "{\"schema_version\":2,\"project\":\"allocator-scip\",\"layers\":[{"
      "\"name\":\"core\",\"role\":\"core\",\"language\":\"c\","
      "\"globs\":[\"src/**\"]}],\"indexes\":[{\"name\":\"compiler\","
      "\"format\":\"scip\",\"path\":\"valid.scip\"}]}";
  ScipTestBuffer index;
  ArchbirdStatus status;
  ArchbirdEngine *engine;
  ArchbirdProject *project = NULL;
  CountingOutput output = {0};
  uint8_t source_digest[32];
  uint8_t index_digest[32];
  char source_sha[65];
  char index_sha[65];
  char manifest[2048];
  int manifest_length;
  scip_test_valid_index(&index);
  if (!index.ok)
    return ARCHBIRD_CONFLICT;
  (void)archbird_sha256(source, sizeof(source) - 1, source_digest);
  (void)archbird_sha256(index.bytes, index.length, index_digest);
  archbird_sha256_hex(source_digest, source_sha);
  archbird_sha256_hex(index_digest, index_sha);
  manifest_length = snprintf(
      manifest, sizeof(manifest),
      "{\"artifact\":\"archbird-source-manifest\",\"files\":[{\"bytes\":%zu,"
      "\"language\":\"c\",\"layer\":\"core\",\"path\":\"src/core.c\","
      "\"roles\":[\"source\"],\"sha256\":\"%s\"},{\"bytes\":%zu,"
      "\"path\":\"valid.scip\",\"roles\":[\"index\"],\"sha256\":\"%s\"}],"
      "\"producer\":{\"implementation_sha256\":\"111111111111111111111111"
      "1111111111111111111111111111111111111111\",\"name\":\"allocator\","
      "\"version\":\"1\"},\"project\":\"allocator-scip\",\"schema_version\":1}",
      sizeof(source) - 1, source_sha, index.length, index_sha);
  if (manifest_length < 0 || (size_t)manifest_length >= sizeof(manifest))
    return ARCHBIRD_LIMIT_EXCEEDED;
  engine = create_engine(allocator, &status);
  if (!engine)
    return status;
  status = archbird_project_create(engine, (const uint8_t *)manifest,
                                   (size_t)manifest_length, &project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, "src/core.c", 10,
                                         source, sizeof(source) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_add_source(engine, project, "valid.scip", 10,
                                         index.bytes, index.length);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_sources(engine, project);
  if (status == ARCHBIRD_OK)
    status = archbird_project_set_config(
        engine, project, (const uint8_t *)config, sizeof(config) - 1);
  if (status == ARCHBIRD_OK)
    status = archbird_project_scan_builtin_provider(
        engine, project, "semantic:scip", 13, ARCHBIRD_PROVIDER_PRIMARY);
  if (status == ARCHBIRD_OK)
    status = archbird_project_finalize_providers(engine, project);
  if (status == ARCHBIRD_OK)
    status =
        archbird_project_render_map(engine, project, 0, count_write, &output);
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  if (status != ARCHBIRD_OK)
    (void)snprintf(allocator->error, sizeof(allocator->error), "%s",
                   archbird_engine_error(engine));
  archbird_project_destroy(project);
  archbird_engine_destroy(engine);
  return status;
}

typedef enum ReportExerciseKind {
  REPORT_EXERCISE_MAP_COMPACT,
  REPORT_EXERCISE_MAP_BUDGET,
  REPORT_EXERCISE_MAP_FULL,
  REPORT_EXERCISE_MAP_OVERVIEW,
  REPORT_EXERCISE_MAP_ARCHITECTURE,
  REPORT_EXERCISE_QUERY,
  REPORT_EXERCISE_QUERY_RETRIEVAL,
  REPORT_EXERCISE_QUERY_CHANGES,
  REPORT_EXERCISE_QUERY_VERIFICATION,
  REPORT_EXERCISE_QUERY_BUDGET
} ReportExerciseKind;

static ArchbirdStatus exercise_report(TestAllocator *allocator,
                                      ReportExerciseKind kind) {
  static const char query[] =
      "{\"artifacts\":[\"browser-bundle\"],\"direction\":\"both\","
      "\"depth\":1,\"test_depth\":1}";
  static const char retrieval_query[] =
      "{\"search\":[\"browser bundle\"],\"search_limit\":4,"
      "\"direction\":\"both\",\"depth\":1,\"test_depth\":1}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  CountingOutput output = {0};
  if (!engine)
    return status;
  switch (kind) {
  case REPORT_EXERCISE_MAP_COMPACT:
    status = archbird_map_render_markdown(engine, report_map, report_map_length,
                                          0, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_MAP_BUDGET:
    status = archbird_map_render_markdown(engine, report_map, report_map_length,
                                          0, 2000, count_write, &output);
    break;
  case REPORT_EXERCISE_MAP_FULL:
    status = archbird_map_render_markdown(engine, report_map, report_map_length,
                                          1, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_MAP_OVERVIEW:
    status = archbird_map_render_markdown_view(
        engine, report_map, report_map_length, ARCHBIRD_MAP_VIEW_OVERVIEW,
        ARCHBIRD_REPORT_DETAIL_STANDARD, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_MAP_ARCHITECTURE:
    status = archbird_map_render_markdown_view(
        engine, report_map, report_map_length, ARCHBIRD_MAP_VIEW_ARCHITECTURE,
        ARCHBIRD_REPORT_DETAIL_COMPACT, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_QUERY:
    status = archbird_map_query_markdown(
        engine, report_map, report_map_length, (const uint8_t *)query,
        sizeof(query) - 1, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_QUERY_RETRIEVAL:
    status = archbird_map_query_markdown(
        engine, report_map, report_map_length, (const uint8_t *)retrieval_query,
        sizeof(retrieval_query) - 1, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_QUERY_CHANGES:
    status = archbird_map_query_markdown_view(
        engine, report_map, report_map_length, (const uint8_t *)query,
        sizeof(query) - 1, ARCHBIRD_QUERY_VIEW_CHANGES,
        ARCHBIRD_REPORT_DETAIL_STANDARD, 0, count_write, &output);
    break;
  case REPORT_EXERCISE_QUERY_VERIFICATION:
    status = archbird_map_query_markdown_view_with_verification(
        engine, report_map, report_map_length, (const uint8_t *)query,
        sizeof(query) - 1, report_verification, report_verification_length,
        ARCHBIRD_QUERY_VIEW_CHANGES, ARCHBIRD_REPORT_DETAIL_STANDARD, 0,
        count_write, &output);
    break;
  case REPORT_EXERCISE_QUERY_BUDGET:
    status = archbird_map_query_markdown(
        engine, report_map, report_map_length, (const uint8_t *)query,
        sizeof(query) - 1, 1200, count_write, &output);
    break;
  default:
    status = ARCHBIRD_INVALID_ARGUMENT;
    break;
  }
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_map_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_MAP_COMPACT);
}

static ArchbirdStatus exercise_budgeted_map_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_MAP_BUDGET);
}

static ArchbirdStatus exercise_full_map_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_MAP_FULL);
}

static ArchbirdStatus exercise_overview_map_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_MAP_OVERVIEW);
}

static ArchbirdStatus
exercise_architecture_map_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_MAP_ARCHITECTURE);
}

static ArchbirdStatus exercise_query_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_QUERY);
}

static ArchbirdStatus
exercise_retrieval_query_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_QUERY_RETRIEVAL);
}

static ArchbirdStatus exercise_budgeted_query_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_QUERY_BUDGET);
}

static ArchbirdStatus exercise_change_query_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_QUERY_CHANGES);
}

static ArchbirdStatus
exercise_verification_query_report(TestAllocator *allocator) {
  return exercise_report(allocator, REPORT_EXERCISE_QUERY_VERIFICATION);
}

static ArchbirdStatus exercise_verify(TestAllocator *allocator) {
  static const char config[] =
      "{\"constraints\":{\"ALLOC-VERIFY\":{\"actual\":{\"literal\":[\"B\"]},"
      "\"assert\":\"set_equal\",\"expected\":{\"literal\":[\"A\"]},"
      "\"owner\":\"test\",\"rationale\":\"Exercise allocator ownership "
      "through constraints.\"}},\"layers\":[{\"globs\":[\"**/*.c\"],"
      "\"language\":\"c\",\"name\":\"core\",\"required\":false}],"
      "\"project\":\"allocator-test\",\"schema_version\":2}";
  static const char map[] =
      "{\"artifact\":\"map\",\"diagnostics\":[],\"evidence\":{"
      "\"config_sha256\":"
      "\"07aba52df9d1c027596c7ff22c340e21734e8f5065c76161dae6859605dc309d\","
      "\"input_sha256\":"
      "\"2222222222222222222222222222222222222222222222222222222222222222\"},"
      "\"project\":\"allocator-test\",\"schema_version\":7,\"tool\":{"
      "\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"archbird\",\"version\":\"fixture\"}}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  CountingOutput output = {0};
  if (!engine)
    return status;
  status = archbird_constraints_evaluate(
      engine, (const uint8_t *)config, sizeof(config) - 1, (const uint8_t *)map,
      sizeof(map) - 1, NULL, 0, NULL, 0, 0, count_write, &output);
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  archbird_engine_destroy(engine);
  return status;
}

static ArchbirdStatus exercise_verify_authoring(TestAllocator *allocator) {
  static const char map[] =
      "{\"artifact\":\"map\",\"diagnostics\":[],\"evidence\":{"
      "\"config_sha256\":"
      "\"07aba52df9d1c027596c7ff22c340e21734e8f5065c76161dae6859605dc309d\","
      "\"input_sha256\":"
      "\"2222222222222222222222222222222222222222222222222222222222222222\"},"
      "\"project\":\"allocator-test\",\"schema_version\":7,\"tool\":{"
      "\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"archbird\",\"version\":\"fixture\"}}";
  static const char config[] =
      "{\"constraints\":{\"ALLOC-AUTHOR\":{\"actual\":{\"literal\":[\"B\"]},"
      "\"assert\":\"set_equal\",\"expected\":{\"literal\":[\"A\"]},"
      "\"owner\":\"test\",\"rationale\":\"Exercise allocator ownership "
      "through authoring.\"}},\"layers\":[{\"globs\":[\"**/*.c\"],"
      "\"language\":\"c\",\"name\":\"core\",\"required\":false}],"
      "\"project\":\"allocator-test\",\"schema_version\":2}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  FixedOutput output = {{0}, 0};
  if (!engine)
    return status;
  status = archbird_project_configuration_compile(
      engine, (const uint8_t *)config, sizeof(config) - 1, 0, fixed_write,
      &output);
  if (status == ARCHBIRD_OK) {
    output.length = 0;
    status = archbird_constraints_freeze(
        engine, (const uint8_t *)config, sizeof(config) - 1,
        (const uint8_t *)map, sizeof(map) - 1, NULL, 0, NULL, 0, "test",
        sizeof("test") - 1, "Reviewed allocator baseline.",
        sizeof("Reviewed allocator baseline.") - 1, 0, fixed_write, &output);
  }
  if (status == ARCHBIRD_OK && !output.length)
    status = ARCHBIRD_CONFLICT;
  if (status != ARCHBIRD_OK)
    (void)snprintf(allocator->error, sizeof(allocator->error), "%s",
                   archbird_engine_error(engine));
  archbird_engine_destroy(engine);
  return status;
}

static int first_fingerprint(const FixedOutput *verification,
                             const char **out) {
  static const char prefix[] = "\"fingerprint\":\"";
  size_t index;
  for (index = 0; index + sizeof(prefix) - 1 + 64 <= verification->length;
       index++) {
    size_t digit;
    const char *candidate;
    if (memcmp(verification->bytes + index, prefix, sizeof(prefix) - 1) != 0)
      continue;
    candidate = (const char *)verification->bytes + index + sizeof(prefix) - 1;
    for (digit = 0; digit < 64; digit++) {
      unsigned char byte = (unsigned char)candidate[digit];
      if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
        break;
    }
    if (digit == 64 && candidate[64] == '"') {
      *out = candidate;
      return 1;
    }
  }
  return 0;
}

static ArchbirdStatus exercise_act(TestAllocator *allocator) {
  static const char config[] =
      "{\"constraints\":{\"ALLOC-ACT\":{\"actual\":{\"literal\":[\"B\"]},"
      "\"assert\":\"set_equal\",\"expected\":{\"literal\":[\"A\"]},"
      "\"owner\":\"test\",\"rationale\":\"Exercise allocator ownership "
      "through Act.\"}},\"layers\":[{\"globs\":[\"**/*.c\"],"
      "\"language\":\"c\",\"name\":\"core\",\"required\":false}],"
      "\"project\":\"allocator-test\",\"schema_version\":2}";
  static const char map[] =
      "{\"artifact\":\"map\",\"diagnostics\":[],\"evidence\":{"
      "\"config_sha256\":"
      "\"07aba52df9d1c027596c7ff22c340e21734e8f5065c76161dae6859605dc309d\","
      "\"input_sha256\":"
      "\"2222222222222222222222222222222222222222222222222222222222222222\"},"
      "\"project\":\"allocator-test\",\"schema_version\":7,\"tool\":{"
      "\"implementation_sha256\":"
      "\"3333333333333333333333333333333333333333333333333333333333333333\","
      "\"name\":\"archbird\",\"version\":\"fixture\"}}";
  static const char review[] =
      "{\"objective\":\"Exercise the reviewed allocator transition.\","
      "\"owner\":\"test\",\"rationale\":\"Allocation failures must not leak "
      "Act state.\",\"preserve_constraints\":[],\"selected_candidates\":[]}";
  ArchbirdStatus status;
  ArchbirdEngine *engine = create_engine(allocator, &status);
  FixedOutput verification = {{0}, 0};
  FixedOutput proposal = {{0}, 0};
  FixedOutput contract = {{0}, 0};
  FixedOutput result = {{0}, 0};
  const char *fingerprint = NULL;
  if (!engine)
    return status;
  status = archbird_constraints_evaluate(
      engine, (const uint8_t *)config, sizeof(config) - 1, (const uint8_t *)map,
      sizeof(map) - 1, NULL, 0, NULL, 0, 0, fixed_write, &verification);
  if (status == ARCHBIRD_OK && !first_fingerprint(&verification, &fingerprint))
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK)
    status = archbird_change_proposal(engine, verification.bytes,
                                      verification.length, fingerprint, 64, 0,
                                      fixed_write, &proposal);
  if (status == ARCHBIRD_OK)
    status = archbird_change_contract(
        engine, proposal.bytes, proposal.length, (const uint8_t *)review,
        sizeof(review) - 1, 0, fixed_write, &contract);
  if (status == ARCHBIRD_OK)
    status = archbird_change_verify(
        engine, proposal.bytes, proposal.length, contract.bytes,
        contract.length, verification.bytes, verification.length,
        verification.bytes, verification.length, 0, fixed_write, &result);
  if (status == ARCHBIRD_OK && !result.length)
    status = ARCHBIRD_CONFLICT;
  if (status != ARCHBIRD_OK)
    (void)snprintf(allocator->error, sizeof(allocator->error), "%s",
                   archbird_engine_error(engine));
  archbird_engine_destroy(engine);
  return status;
}

typedef ArchbirdStatus (*ExerciseFn)(TestAllocator *allocator);

static void run_failure_sweep(const char *name, ExerciseFn exercise) {
  TestAllocator baseline = {0};
  ArchbirdStatus baseline_status = exercise(&baseline);
  size_t total_calls = baseline.calls;
  size_t fail_at;
  if (baseline_status != ARCHBIRD_OK || baseline.corrupted ||
      baseline.outstanding || baseline.bytes) {
    char message[192];
    (void)snprintf(message, sizeof(message),
                   "baseline returned status %d with %zu allocations and %zu "
                   "bytes outstanding%s",
                   (int)baseline_status, baseline.outstanding, baseline.bytes,
                   baseline.corrupted ? " (corrupted)" : "");
    fail(name, message);
    return;
  }
  for (fail_at = 1; fail_at <= total_calls; fail_at++) {
    TestAllocator allocator = {0};
    ArchbirdStatus status;
    allocator.fail_at = fail_at;
    status = exercise(&allocator);
    if (allocator.corrupted || allocator.outstanding || allocator.bytes) {
      char message[192];
      (void)snprintf(
          message, sizeof(message),
          "allocation %zu returned status %d with %zu allocations "
          "and %zu bytes outstanding; oldest head call=%zu size=%zu%s",
          fail_at, (int)status, allocator.outstanding, allocator.bytes,
          allocator.head ? allocator.head->value.call : 0,
          allocator.head ? allocator.head->value.size : 0,
          allocator.corrupted ? " (corrupted)" : "");
      fail(name, message);
      return;
    }
    if (status != ARCHBIRD_OK && status != ARCHBIRD_OUT_OF_MEMORY) {
      char message[128];
      (void)snprintf(message, sizeof(message),
                     "failure at allocation %zu returned status %d", fail_at,
                     status);
      if (allocator.error[0]) {
        size_t used = strlen(message);
        (void)snprintf(message + used, sizeof(message) - used, ": %.80s",
                       allocator.error);
      }
      fail(name, message);
      return;
    }
    if (!allocator.failed) {
      fail(name, "baseline allocation was not reached during failure replay");
      return;
    }
  }
}

static void test_invalid_options(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  TestAllocator allocator = {0};
  archbird_engine_options_init(&options);
  options.allocate = test_allocate;
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_INVALID_ARGUMENT ||
      engine)
    fail("partial-callbacks", "partial allocator callback set was accepted");
  archbird_engine_options_init(&options);
  options.allocator_user_data = &allocator;
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_INVALID_ARGUMENT ||
      engine)
    fail("orphan-user-data",
         "allocator user data without callbacks was accepted");
}

static int load_fixture(const char *path, uint8_t **bytes, size_t *size) {
  FILE *file = fopen(path, "rb");
  long length;
  if (!file)
    return 0;
  if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
      fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  if ((unsigned long)length >= SIZE_MAX) {
    fclose(file);
    return 0;
  }
  *bytes = (uint8_t *)malloc((size_t)length + 1);
  if (!*bytes || fread(*bytes, 1, (size_t)length, file) != (size_t)length) {
    free(*bytes);
    *bytes = NULL;
    fclose(file);
    return 0;
  }
  (*bytes)[length] = '\0';
  fclose(file);
  *size = (size_t)length;
  return 1;
}

static int generate_report_verification(void) {
  static const char config[] =
      "{\"constraints\":{\"ALLOC-OVERLAY\":{\"actual\":{\"literal\":[\"x\"]},"
      "\"assert\":\"set_equal\",\"expected\":{\"literal\":[\"y\"]},"
      "\"owner\":\"test\",\"rationale\":\"Exercise query overlay "
      "allocation ownership.\"}},\"layers\":[{\"globs\":[\"**/*.c\"],"
      "\"language\":\"c\",\"name\":\"core\",\"required\":false}],"
      "\"project\":\"sample\",\"schema_version\":2}";
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  HeapOutput plan = {0};
  HeapOutput output = {0};
  const char *plan_digest;
  char *map_digest;
  ArchbirdStatus status;
  archbird_engine_options_init(&options);
  status = archbird_engine_create(&options, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_project_configuration_compile(
        engine, (const uint8_t *)config, sizeof(config) - 1, 0, heap_write,
        &plan);
  if (status == ARCHBIRD_OK) {
    uint8_t *terminated = (uint8_t *)realloc(plan.bytes, plan.length + 1);
    if (!terminated)
      status = ARCHBIRD_OUT_OF_MEMORY;
    else {
      plan.bytes = terminated;
      plan.capacity = plan.length + 1;
      plan.bytes[plan.length] = '\0';
      plan_digest =
          strstr((const char *)plan.bytes, "\"map_config_sha256\":\"");
      map_digest = strstr((char *)report_map, "\"config_sha256\":\"");
      if (!plan_digest || !map_digest)
        status = ARCHBIRD_INVALID_SCHEMA;
      else {
        plan_digest += sizeof("\"map_config_sha256\":\"") - 1;
        map_digest += sizeof("\"config_sha256\":\"") - 1;
        memcpy(map_digest, plan_digest, 64);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = archbird_constraints_evaluate(
        engine, (const uint8_t *)config, sizeof(config) - 1, report_map,
        report_map_length, NULL, 0, NULL, 0, 0, heap_write, &output);
  archbird_engine_destroy(engine);
  free(plan.bytes);
  if (status != ARCHBIRD_OK) {
    free(output.bytes);
    return 0;
  }
  report_verification = output.bytes;
  report_verification_length = output.length;
  return 1;
}

int main(void) {
  if (!load_fixture(ARCHBIRD_ALLOCATOR_REPORT_MAP, &report_map,
                    &report_map_length) ||
      !generate_report_verification()) {
    fail("report-fixtures", "cannot load report allocator fixtures");
    free(report_map);
    free(report_verification);
    return 1;
  }
  test_invalid_options();
  run_failure_sweep("json-every-n", exercise_json);
  run_failure_sweep("pattern-every-n", exercise_pattern);
  run_failure_sweep("config-resolution-every-n", exercise_config_resolution);
  run_failure_sweep("map-every-n", exercise_map);
  run_failure_sweep("scip-every-n", exercise_scip);
  run_failure_sweep("map-report-every-n", exercise_map_report);
  run_failure_sweep("map-report-budget-every-n", exercise_budgeted_map_report);
  run_failure_sweep("map-report-full-every-n", exercise_full_map_report);
  run_failure_sweep("map-report-overview-every-n",
                    exercise_overview_map_report);
  run_failure_sweep("map-report-architecture-every-n",
                    exercise_architecture_map_report);
  run_failure_sweep("query-report-every-n", exercise_query_report);
  run_failure_sweep("query-retrieval-report-every-n",
                    exercise_retrieval_query_report);
  run_failure_sweep("query-change-report-every-n",
                    exercise_change_query_report);
  run_failure_sweep("query-verification-report-every-n",
                    exercise_verification_query_report);
  run_failure_sweep("query-report-budget-every-n",
                    exercise_budgeted_query_report);
  run_failure_sweep("verify-every-n", exercise_verify);
  run_failure_sweep("verify-authoring-every-n", exercise_verify_authoring);
  run_failure_sweep("act-every-n", exercise_act);
  free(report_map);
  free(report_verification);
  report_map = NULL;
  report_map_length = 0;
  report_verification = NULL;
  report_verification_length = 0;
  if (failures)
    return 1;
  puts("native allocator tests passed: JSON, PCRE2, config resolution, "
       "SCIP, Map/reports, Verify/authoring, and Act every-N sweeps");
  return 0;
}
