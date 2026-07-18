#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  if (!engine)
    return 0;
  (void)archbird_json_validate(engine, data, size);
  (void)archbird_json_canonicalize(engine, data, size, 0, fuzz_discard, NULL);
  (void)archbird_json_canonicalize(
      engine, data, size, ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE,
      fuzz_discard, NULL);
  archbird_engine_destroy(engine);
  return 0;
}
