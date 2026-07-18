#ifndef ARCHBIRD_JSON_VALUE_H
#define ARCHBIRD_JSON_VALUE_H

#include "model.h"
#include "render_internal.h"

ArchbirdStatus ab_json_value_decode(ArchbirdEngine *engine, const uint8_t *json,
                                    size_t json_length, AbValue *out);

const AbValue *ab_value_member(const AbValue *object, const char *name);
int ab_value_u64(const AbValue *value, uint64_t *out);
int ab_value_string_is(const AbValue *value, const char *literal);
ArchbirdStatus ab_value_render(AbBuffer *buffer, const AbValue *value);

#endif
