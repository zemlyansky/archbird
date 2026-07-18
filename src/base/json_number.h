#ifndef ARCHBIRD_JSON_NUMBER_H
#define ARCHBIRD_JSON_NUMBER_H

#include "archbird_internal.h"

#define AB_JSON_REAL_BUFFER_SIZE 64

/*
 * Parse one strict JSON real token as a finite IEEE-754 binary64 value.
 * Integer tokens are rejected so the owned JSON model preserves Python's
 * int-versus-float distinction.
 */
ArchbirdStatus ab_json_real_parse(ArchbirdEngine *engine, const char *token,
                                  size_t token_length, double *out_value);

/* Render a finite binary64 value exactly like Python's repr/json encoder. */
ArchbirdStatus ab_json_real_format(ArchbirdEngine *engine, double value,
                                   char output[AB_JSON_REAL_BUFFER_SIZE],
                                   size_t *out_length);

#endif
