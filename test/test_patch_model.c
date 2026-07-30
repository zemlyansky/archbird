#include <archbird/archbird.h>

#include <stdio.h>
#include <string.h>

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define HELLO_SHA                                                              \
  "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"

static int failures;

static void expect_status(ArchbirdEngine *engine, const char *name,
                          const char *json, ArchbirdStatus expected) {
  ArchbirdStatus actual =
      archbird_patch_validate(engine, (const uint8_t *)json, strlen(json));
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d, expected %d: %s\n", name, actual,
            expected, archbird_engine_error(engine));
    failures++;
  }
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  const char *valid =
      "{\"acceptance\":{\"constraints\":[],\"status\":\"not_evaluated\","
      "\"verification_sha256\":null},\"after\":null,\"artifact\":\"patch\","
      "\"executors\":[{\"capability\":\"archbird.native.create-file@1\","
      "\"deterministic\":true,\"implementation_sha256\":\"" SHA_C
      "\",\"item_ids\":[\"item:create\"],\"matches\":1,\"reads\":[],"
      "\"skipped\":0,\"unsupported\":0,\"writes\":[\"hello.txt\"]}],"
      "\"plan_sha256\":\"" SHA_A
      "\",\"provenance\":\"derived\",\"schema_version\":1,\"seal\":null,"
      "\"source\":{\"map\":{\"configuration_sha256\":\"" SHA_A
      "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"state\":\"materialized\",\"tool\":{"
      "\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"transitions\":[{"
      "\"after\":{\"byte_length\":6,\"content_base64\":\"aGVsbG8K\","
      "\"executable\":false,\"sha256\":\"" HELLO_SHA
      "\"},\"before\":null,\"item_ids\":[\"item:create\"],"
      "\"kind\":\"create\",\"path\":\"hello.txt\",\"source_path\":null}]}";
  const char *bad_content =
      "{\"acceptance\":{\"constraints\":[],\"status\":\"not_evaluated\","
      "\"verification_sha256\":null},\"after\":null,\"artifact\":\"patch\","
      "\"executors\":[{\"capability\":\"archbird.native.create-file@1\","
      "\"deterministic\":true,\"implementation_sha256\":\"" SHA_C
      "\",\"item_ids\":[\"item:create\"],\"matches\":1,\"reads\":[],"
      "\"skipped\":0,\"unsupported\":0,\"writes\":[\"hello.txt\"]}],"
      "\"plan_sha256\":\"" SHA_A
      "\",\"provenance\":\"derived\",\"schema_version\":1,\"seal\":null,"
      "\"source\":{\"map\":{\"configuration_sha256\":\"" SHA_A
      "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"state\":\"materialized\",\"tool\":{"
      "\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"transitions\":[{"
      "\"after\":{\"byte_length\":6,\"content_base64\":\"aGVsbG8A\","
      "\"executable\":false,\"sha256\":\"" HELLO_SHA
      "\"},\"before\":null,\"item_ids\":[\"item:create\"],"
      "\"kind\":\"create\",\"path\":\"hello.txt\",\"source_path\":null}]}";
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  expect_status(engine, "valid-materialized", valid, ARCHBIRD_OK);
  expect_status(engine, "tampered-content", bad_content,
                ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "not-object", "[]", ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "duplicate-key",
                "{\"artifact\":\"patch\",\"artifact\":\"patch\"}",
                ARCHBIRD_DUPLICATE_KEY);
  archbird_engine_destroy(engine);
  if (failures)
    fprintf(stderr, "%d Patch model test(s) failed\n", failures);
  return failures ? 1 : 0;
}
