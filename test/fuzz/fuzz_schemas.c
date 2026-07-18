#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  if (!engine)
    return 0;
  (void)archbird_source_manifest_validate(engine, data, size);
  (void)archbird_provider_facts_validate(engine, data, size);
  (void)archbird_test_symbol_observations_validate(engine, data, size);
  archbird_engine_destroy(engine);
  return 0;
}
