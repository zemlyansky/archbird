#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ArchbirdEngine *engine = fuzz_engine();
  if (!engine)
    return 0;
#ifdef ARCHBIRD_FUZZ_WORKSPACE_MAPS
  (void)archbird_workspace_analyze(engine, fuzz_workspace_json,
                                   sizeof(fuzz_workspace_json) - 1, data, size,
                                   0, fuzz_discard, NULL);
#else
  (void)archbird_workspace_plan(engine, data, size, 0, fuzz_discard, NULL);
  (void)archbird_workspace_analyze(engine, data, size, fuzz_workspace_maps_json,
                                   sizeof(fuzz_workspace_maps_json) - 1, 0,
                                   fuzz_discard, NULL);
#endif
  archbird_engine_destroy(engine);
  return 0;
}
