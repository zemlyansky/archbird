#ifndef ARCHBIRD_GATE_H
#define ARCHBIRD_GATE_H

#include "json_value.h"

#define AB_GATE_MAX_COUNT 4096u
#define AB_GATE_MAX_ARGUMENTS 64u
#define AB_GATE_MAX_ARGUMENT_BYTES 4096u
#define AB_GATE_MAX_TIMEOUT_SECONDS 3600u
#define AB_GATE_MAX_OUTPUT_BYTES (16u * 1024u * 1024u)
#define AB_GATE_DEFAULT_OUTPUT_BYTES (1024u * 1024u)

int ab_gate_definitions_valid(const AbValue *gates);
ArchbirdStatus ab_gate_definition_sha256(ArchbirdEngine *engine,
                                         const AbValue *gate, char out[65]);

#endif
