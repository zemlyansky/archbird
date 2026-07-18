#ifndef ARCHBIRD_TEST_OBSERVATIONS_H
#define ARCHBIRD_TEST_OBSERVATIONS_H

#include "project_internal.h"

ArchbirdStatus ab_decode_test_symbol_observations(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    const uint8_t *input, size_t input_length, AbValue *out);

#endif
