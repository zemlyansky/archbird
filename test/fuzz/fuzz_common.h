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
  "\"1111111111111111111111111111111111111111111111111111111111111111\","      \
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

static const uint8_t fuzz_suite_json[] =
    "{\"schema_version\":1,\"suite\":\"fuzz\",\"projects\":{"
    "\"subject\":{\"map\":\"subject.json\"}},\"extractors\":{"
    "\"expected\":{\"kind\":\"literal_set\",\"values\":[\"A\"]},"
    "\"actual\":{\"kind\":\"literal_set\",\"values\":[\"B\"]}},"
    "\"checks\":[{\"id\":\"FUZZ-SET\",\"assert\":\"set_equal\","
    "\"expected\":\"expected\",\"actual\":\"actual\","
    "\"owner\":\"fuzz\",\"rationale\":"
    "\"Exercise native verification.\"}]}";

static const uint8_t fuzz_verification_input_json[] =
    "{\"schema_version\":1,\"artifact\":\"verification-input\","
    "\"suite_path\":\"fuzz.verify.json\",\"projects\":[{\"name\":"
    "\"subject\",\"map\":" ARCHBIRD_FUZZ_MAP_JSON ",\"sources\":[]}],"
    "\"provided_facts\":[],\"attestations\":[],\"baseline\":null}";

static const uint8_t fuzz_review_json[] =
    "{\"objective\":\"Exercise one reviewed transition.\",\"owner\":"
    "\"fuzz\",\"preserve_checks\":[],\"rationale\":"
    "\"Exercise native Act artifact boundaries.\","
    "\"selected_candidates\":[]}";

typedef struct FuzzBuffer {
  uint8_t *data;
  size_t length;
  size_t capacity;
} FuzzBuffer;

typedef struct FuzzActChain {
  FuzzBuffer verification;
  FuzzBuffer proposal;
  FuzzBuffer contract;
  FuzzBuffer result;
  char fingerprint[65];
} FuzzActChain;

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

static int fuzz_first_fingerprint(const FuzzBuffer *verification,
                                  char fingerprint[65]) {
  static const uint8_t prefix[] = "\"fingerprint\":\"";
  size_t index;
  if (!verification || !fingerprint)
    return 0;
  for (index = 0; index + sizeof(prefix) - 1 + 64 < verification->length;
       index++) {
    size_t digit;
    const uint8_t *candidate;
    if (memcmp(verification->data + index, prefix, sizeof(prefix) - 1) != 0)
      continue;
    candidate = verification->data + index + sizeof(prefix) - 1;
    for (digit = 0; digit < 64; digit++) {
      uint8_t byte = candidate[digit];
      if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
        break;
    }
    if (digit == 64 && candidate[64] == '"') {
      memcpy(fingerprint, candidate, 64);
      fingerprint[64] = '\0';
      return 1;
    }
  }
  return 0;
}

static void fuzz_act_chain_free(FuzzActChain *chain) {
  if (!chain)
    return;
  fuzz_buffer_free(&chain->result);
  fuzz_buffer_free(&chain->contract);
  fuzz_buffer_free(&chain->proposal);
  fuzz_buffer_free(&chain->verification);
  memset(chain, 0, sizeof(*chain));
}

static int fuzz_build_act_chain(ArchbirdEngine *engine, FuzzActChain *chain) {
  ArchbirdStatus status;
  if (!engine || !chain)
    return 0;
  memset(chain, 0, sizeof(*chain));
  status = archbird_verification_analyze(
      engine, fuzz_suite_json, sizeof(fuzz_suite_json) - 1,
      fuzz_verification_input_json, sizeof(fuzz_verification_input_json) - 1, 0,
      fuzz_buffer_write, &chain->verification);
  if (status == ARCHBIRD_OK &&
      !fuzz_first_fingerprint(&chain->verification, chain->fingerprint))
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK)
    status = archbird_change_proposal(
        engine, chain->verification.data, chain->verification.length,
        chain->fingerprint, 64, 0, fuzz_buffer_write, &chain->proposal);
  if (status == ARCHBIRD_OK)
    status = archbird_change_contract(
        engine, chain->proposal.data, chain->proposal.length, fuzz_review_json,
        sizeof(fuzz_review_json) - 1, 0, fuzz_buffer_write, &chain->contract);
  if (status == ARCHBIRD_OK)
    status = archbird_change_verify(
        engine, chain->proposal.data, chain->proposal.length,
        chain->contract.data, chain->contract.length, chain->verification.data,
        chain->verification.length, chain->verification.data,
        chain->verification.length, 0, fuzz_buffer_write, &chain->result);
  if (status == ARCHBIRD_OK)
    return 1;
  fuzz_act_chain_free(chain);
  return 0;
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
