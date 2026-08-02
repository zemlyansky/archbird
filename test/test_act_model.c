#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define SHA_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define HELLO_SHA                                                              \
  "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03"

static int failures;

static char *replace_once(const char *input, const char *needle,
                          const char *replacement) {
  const char *found = strstr(input, needle);
  size_t input_length = strlen(input);
  size_t needle_length = strlen(needle);
  size_t replacement_length = strlen(replacement);
  char *result;
  if (!found)
    return NULL;
  result =
      (char *)malloc(input_length - needle_length + replacement_length + 1);
  if (!result)
    return NULL;
  memcpy(result, input, (size_t)(found - input));
  memcpy(result + (found - input), replacement, replacement_length);
  strcpy(result + (found - input) + replacement_length, found + needle_length);
  return result;
}

static void expect_status(ArchbirdEngine *engine, const char *name,
                          const char *json, ArchbirdStatus expected) {
  ArchbirdStatus actual =
      archbird_act_validate(engine, (const uint8_t *)json, strlen(json));
  if (actual != expected) {
    fprintf(stderr, "FAIL %s: status %d, expected %d: %s\n", name, actual,
            expected, archbird_engine_error(engine));
    failures++;
  }
}

int main(void) {
  ArchbirdEngine *engine = NULL;
  char *read_without_lock;
  char *unused_lock;
  char *valid_lock;
  char *transition_lock_read;
  char *transition_lock;
  const char *valid =
      "{\"acceptance\":{\"constraints\":[],\"status\":\"not_evaluated\","
      "\"verification_sha256\":null},\"after\":null,\"artifact\":\"act\","
      "\"executors\":[{\"capability\":\"archbird.native.create-file@1\","
      "\"deterministic\":true,\"implementation_sha256\":\"" SHA_C
      "\",\"item_ids\":[\"item:create\"],\"matches\":1,\"reads\":[],"
      "\"skipped\":0,\"unsupported\":0,\"writes\":[\"hello.txt\"]}],"
      "\"plan_sha256\":\"" SHA_A
      "\",\"provenance\":\"derived\",\"schema_version\":3,\"seal\":null,"
      "\"source\":{\"map\":{\"configuration_sha256\":\"" SHA_A
      "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"source_locks\":[],\"state\":\"materialized\",\"tool\":{"
      "\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"transitions\":[{"
      "\"after\":{\"byte_length\":6,\"content_base64\":\"aGVsbG8K\","
      "\"executable\":false,\"sha256\":\"" HELLO_SHA
      "\"},\"before\":null,\"item_ids\":[\"item:create\"],"
      "\"kind\":\"create\",\"path\":\"hello.txt\",\"source_path\":null}]}";
  const char *bad_content =
      "{\"acceptance\":{\"constraints\":[],\"status\":\"not_evaluated\","
      "\"verification_sha256\":null},\"after\":null,\"artifact\":\"act\","
      "\"executors\":[{\"capability\":\"archbird.native.create-file@1\","
      "\"deterministic\":true,\"implementation_sha256\":\"" SHA_C
      "\",\"item_ids\":[\"item:create\"],\"matches\":1,\"reads\":[],"
      "\"skipped\":0,\"unsupported\":0,\"writes\":[\"hello.txt\"]}],"
      "\"plan_sha256\":\"" SHA_A
      "\",\"provenance\":\"derived\",\"schema_version\":3,\"seal\":null,"
      "\"source\":{\"map\":{\"configuration_sha256\":\"" SHA_A
      "\",\"input_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"},\"project\":\"demo\",\"verification\":{\"policy_sha256\":\"" SHA_B
      "\",\"producer_implementation_sha256\":\"" SHA_C "\",\"sha256\":\"" SHA_A
      "\"}},\"source_locks\":[],\"state\":\"materialized\",\"tool\":{"
      "\"implementation_sha256\":\"" SHA_C
      "\",\"name\":\"archbird\",\"version\":\"test\"},\"transitions\":[{"
      "\"after\":{\"byte_length\":6,\"content_base64\":\"aGVsbG8A\","
      "\"executable\":false,\"sha256\":\"" HELLO_SHA
      "\"},\"before\":null,\"item_ids\":[\"item:create\"],"
      "\"kind\":\"create\",\"path\":\"hello.txt\",\"source_path\":null}]}";
  if (archbird_engine_create(NULL, &engine) != ARCHBIRD_OK)
    return 2;
  expect_status(engine, "valid-materialized", valid, ARCHBIRD_OK);
  read_without_lock = replace_once(valid, "\"matches\":1,\"reads\":[]",
                                   "\"matches\":1,\"reads\":[\"config.json\"]");
  unused_lock = replace_once(
      valid, "\"source_locks\":[]",
      "\"source_locks\":[{\"executable\":false,\"path\":\"config.json\","
      "\"sha256\":\"" SHA_B "\"}]");
  valid_lock =
      read_without_lock
          ? replace_once(read_without_lock, "\"source_locks\":[]",
                         "\"source_locks\":[{\"executable\":false,"
                         "\"path\":\"config.json\",\"sha256\":\"" SHA_B "\"}]")
          : NULL;
  transition_lock_read =
      replace_once(valid, "\"matches\":1,\"reads\":[]",
                   "\"matches\":1,\"reads\":[\"hello.txt\"]");
  transition_lock =
      transition_lock_read
          ? replace_once(transition_lock_read, "\"source_locks\":[]",
                         "\"source_locks\":[{\"executable\":false,"
                         "\"path\":\"hello.txt\",\"sha256\":\"" SHA_B "\"}]")
          : NULL;
  if (!read_without_lock || !unused_lock || !valid_lock ||
      !transition_lock_read || !transition_lock) {
    fprintf(stderr, "FAIL could not construct source-lock fixtures\n");
    failures++;
  } else {
    expect_status(engine, "read-without-lock", read_without_lock,
                  ARCHBIRD_INVALID_SCHEMA);
    expect_status(engine, "unused-lock", unused_lock, ARCHBIRD_INVALID_SCHEMA);
    expect_status(engine, "valid-read-lock", valid_lock, ARCHBIRD_OK);
    expect_status(engine, "lock-duplicates-transition", transition_lock,
                  ARCHBIRD_INVALID_SCHEMA);
  }
  expect_status(engine, "tampered-content", bad_content,
                ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "not-object", "[]", ARCHBIRD_INVALID_SCHEMA);
  expect_status(engine, "duplicate-key",
                "{\"artifact\":\"act\",\"artifact\":\"act\"}",
                ARCHBIRD_DUPLICATE_KEY);
  free(transition_lock);
  free(transition_lock_read);
  free(valid_lock);
  free(unused_lock);
  free(read_without_lock);
  archbird_engine_destroy(engine);
  if (failures)
    fprintf(stderr, "%d Act model test(s) failed\n", failures);
  return failures ? 1 : 0;
}
