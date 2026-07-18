#ifndef ARCHBIRD_JSON_INTERNAL_H
#define ARCHBIRD_JSON_INTERNAL_H

#include "archbird_internal.h"

#include "yyjson.h"

ArchbirdStatus archbird_json_parse_document(ArchbirdEngine *engine,
                                            const uint8_t *input,
                                            size_t input_length,
                                            yyjson_doc **out_document,
                                            int reject_real_numbers);

void ab_yyjson_allocator(ArchbirdEngine *engine, yyjson_alc *out_allocator);

#endif
