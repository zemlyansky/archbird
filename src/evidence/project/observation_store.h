#ifndef ARCHBIRD_PROJECT_OBSERVATION_STORE_H
#define ARCHBIRD_PROJECT_OBSERVATION_STORE_H

#include "evidence/project/project_model.h"

ArchbirdStatus ab_project_observation_store_add(
    ArchbirdEngine *engine, ArchbirdProject *project,
    AbProjectObservationStore *store, const uint8_t *observations_json,
    size_t observations_length);

void ab_project_observation_store_destroy(ArchbirdEngine *engine,
                                          AbProjectObservationStore *store);

#endif
