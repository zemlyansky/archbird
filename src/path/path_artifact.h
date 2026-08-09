#ifndef ARCHBIRD_PATH_ARTIFACT_H
#define ARCHBIRD_PATH_ARTIFACT_H

#include "base/json_value.h"

typedef struct AbPathArtifact {
  ArchbirdEngine *engine;
  AbValue root;
} AbPathArtifact;

ArchbirdStatus ab_path_artifact_validate(ArchbirdEngine *engine,
                                         const AbValue *artifact);
ArchbirdStatus ab_path_artifact_load(ArchbirdEngine *engine,
                                     const uint8_t *json, size_t json_length,
                                     AbPathArtifact *out);
void ab_path_artifact_free(AbPathArtifact *artifact);

#endif
