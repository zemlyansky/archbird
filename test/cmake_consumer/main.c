#include <archbird/archbird.h>

#include <string.h>

int main(void) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  const char *implementation;

  archbird_engine_options_init(&options);
  if (options.struct_size != sizeof(options))
    return 1;
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK || !engine)
    return 2;
  implementation = archbird_implementation_sha256();
  if (!implementation || strlen(implementation) != 64) {
    archbird_engine_destroy(engine);
    return 3;
  }
  archbird_engine_destroy(engine);
  return ARCHBIRD_NATIVE_ABI_VERSION == 0u ? 0 : 4;
}
