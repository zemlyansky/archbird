#include "archbird_internal.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status = archbird_engine_create(NULL, &engine);
  if (status != ARCHBIRD_OK || !engine) {
    fputs("build identity test failed: cannot create engine\n", stderr);
    return 1;
  }
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_CONFLICT ||
      strstr(archbird_engine_error(engine), "ARCHBIRD_IMPLEMENTATION_SHA256") ==
          NULL ||
      strstr(archbird_engine_error(engine),
             "64 lowercase hexadecimal characters") == NULL) {
    fprintf(stderr, "build identity test failed: status=%d error=%s\n",
            (int)status, archbird_engine_error(engine));
    archbird_engine_destroy(engine);
    return 1;
  }
  archbird_engine_destroy(engine);
  puts("invalid build identity rejected");
  return 0;
}
