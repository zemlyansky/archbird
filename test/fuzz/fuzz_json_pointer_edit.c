#include "fuzz_common.h"

#include "base/sha256.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  ArchbirdJsonPointerEditOptions options;
  ArchbirdJsonPointerEditResult result;
  uint8_t digest[32];
  char sha256[65];
  if (!engine)
    return 0;
  if (archbird_sha256(data, size, digest) != ARCHBIRD_OK) {
    archbird_engine_destroy(engine);
    return 0;
  }
  archbird_sha256_hex(digest, sha256);
  archbird_json_pointer_edit_options_init(&options);
  options.source_sha256 = sha256;
  options.source_sha256_length = 64;
  options.expected_json = data;
  options.expected_json_length = size;
  options.replacement_json = (const uint8_t *)"null";
  options.replacement_json_length = 4;
  archbird_json_pointer_edit_result_init(&result);
  (void)archbird_json_pointer_edit(engine, data, size, &options, &result,
                                   fuzz_discard, NULL);
  archbird_engine_destroy(engine);
  return 0;
}
