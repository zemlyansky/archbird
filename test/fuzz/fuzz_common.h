#ifndef ARCHBIRD_FUZZ_COMMON_H
#define ARCHBIRD_FUZZ_COMMON_H

#include <archbird/archbird.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARCHBIRD_FUZZ_MAP_JSON                                                 \
  "{\"artifact\":\"map\",\"artifacts\":[],\"builds\":[],"                      \
  "\"call_resolutions\":[],\"components\":[],\"description\":\"\","            \
  "\"diagnostics\":[],\"edges\":[],\"evidence\":{"                             \
  "\"absolute_paths_included\":false,\"config_sha256\":"                       \
  "\"07f114f9f920ee168c64035e868121f22bfc888d65fde4610d6d243073e3f687\","      \
  "\"input_sha256\":"                                                          \
  "\"2222222222222222222222222222222222222222222222222222222222222222\"},"     \
  "\"files\":[{\"bytes\":6,\"call_counts\":{},\"calls\":[],"                   \
  "\"export_origins\":{},\"exports\":[],\"imported_names\":{},"                \
  "\"imports\":[],\"language\":\"c\",\"layer\":\"core\","                      \
  "\"messages\":{\"receives\":[],\"sends\":[]},"                               \
  "\"method_call_counts\":{},\"method_calls\":[],\"path\":\"src/a.c\","        \
  "\"reexport_candidates\":[],\"sha256\":"                                     \
  "\"4444444444444444444444444444444444444444444444444444444444444444\","      \
  "\"symbols\":[{\"kind\":\"function\",\"line\":1,\"name\":\"a\","             \
  "\"scope\":\"function\",\"signature\":\"a()\"}]}],\"indexes\":[],"           \
  "\"layers\":[{\"files\":1,\"language\":\"c\",\"name\":\"core\","             \
  "\"role\":\"core\",\"symbols\":1}],\"limits\":{"                             \
  "\"compact_edge_names\":12,\"compact_symbols\":10},"                         \
  "\"named_entries\":{},\"packages\":[],\"parity\":[],"                        \
  "\"project\":\"fuzz\",\"schema_version\":6,\"surfaces\":[],"                 \
  "\"tests\":[],\"tool\":{\"implementation_sha256\":"                          \
  "\"3333333333333333333333333333333333333333333333333333333333333333\","      \
  "\"name\":\"archbird\",\"version\":\"fixture\"}}"

static const uint8_t fuzz_map_json[] = ARCHBIRD_FUZZ_MAP_JSON;
static const uint8_t fuzz_query_json[] =
    "{\"depth\":1,\"direction\":\"both\",\"paths\":[\"src\"],"
    "\"test_depth\":1}";
static const uint8_t fuzz_workspace_json[] =
    "{\"description\":\"\",\"projects\":[{\"config\":"
    "\"subject/archbird.json\"}],\"schema_version\":1,"
    "\"workspace\":\"fuzz\"}";
static const uint8_t fuzz_workspace_maps_json[] =
    "[" ARCHBIRD_FUZZ_MAP_JSON "]";

static const uint8_t fuzz_project_configuration_json[] =
    "{\"project\":\"fuzz\",\"layers\":[{"
    "\"name\":\"core\",\"role\":\"core\",\"language\":\"c\","
    "\"globs\":[\"src/**/*.c\"]}],\"constraints\":{"
    "\"FUZZ-SET\":{\"assert\":\"set_equal\","
    "\"expected\":{\"literal\":[\"A\"]},"
    "\"actual\":{\"literal\":[\"B\"]},"
    "\"owner\":\"fuzz\",\"rationale\":"
    "\"Exercise native verification.\"}}}";

typedef struct FuzzBuffer {
  uint8_t *data;
  size_t length;
  size_t capacity;
} FuzzBuffer;

static int fuzz_discard(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  (void)bytes;
  (void)length;
  return 0;
}

static int fuzz_buffer_write(void *user_data, const uint8_t *bytes,
                             size_t length) {
  FuzzBuffer *buffer = (FuzzBuffer *)user_data;
  size_t capacity;
  uint8_t *resized;
  if (!buffer || length > SIZE_MAX - buffer->length)
    return 1;
  if (buffer->length + length <= buffer->capacity) {
    if (length)
      memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    return 0;
  }
  capacity = buffer->capacity ? buffer->capacity : 4096;
  while (capacity < buffer->length + length) {
    if (capacity > SIZE_MAX / 2) {
      capacity = buffer->length + length;
      break;
    }
    capacity *= 2;
  }
  resized = (uint8_t *)realloc(buffer->data, capacity);
  if (!resized)
    return 1;
  buffer->data = resized;
  buffer->capacity = capacity;
  if (length)
    memcpy(buffer->data + buffer->length, bytes, length);
  buffer->length += length;
  return 0;
}

static void fuzz_buffer_free(FuzzBuffer *buffer) {
  if (!buffer)
    return;
  free(buffer->data);
  memset(buffer, 0, sizeof(*buffer));
}

static ArchbirdEngine *fuzz_engine(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  archbird_engine_options_init(&options);
  options.max_input_bytes = 1024 * 1024;
  options.max_file_bytes = 1024 * 1024;
  options.max_source_bytes = 4 * 1024 * 1024;
  options.max_string_bytes = 1024 * 1024;
  options.max_pattern_matches = 4096;
  options.regex_match_limit = 10000;
  options.regex_depth_limit = 256;
  options.regex_heap_limit_kib = 4096;
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK)
    return NULL;
  return engine;
}

#endif
