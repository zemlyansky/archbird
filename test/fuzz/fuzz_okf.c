#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  if (!engine)
    return 0;
  (void)archbird_okf_analyze(engine, data, size, NULL, 0, ARCHBIRD_OKF_JSON, 1,
                             0, fuzz_discard, NULL);
  archbird_engine_destroy(engine);
  return 0;
}
