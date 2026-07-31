#ifndef ARCHBIRD_ACT_EXECUTOR_INTERNAL_H
#define ARCHBIRD_ACT_EXECUTOR_INTERNAL_H

#include "json_value.h"

typedef struct AbActContext AbActContext;

ArchbirdEngine *ab_act_executor_engine(AbActContext *context);
const ArchbirdProject *ab_act_executor_project(const AbActContext *context);
const AbValue *ab_act_executor_map(const AbActContext *context);

ArchbirdStatus ab_act_executor_source(AbActContext *context,
                                      const AbString *path,
                                      ArchbirdSourceView *out);
ArchbirdStatus ab_act_executor_replace_exact(
    AbActContext *context, const AbString *item_id, const AbString *path,
    size_t start, size_t end, const uint8_t *expected, size_t expected_length,
    const uint8_t *replacement, size_t replacement_length);
ArchbirdStatus ab_act_executor_insert_make_token(
    AbActContext *context, const AbString *item_id, const AbString *path,
    size_t start, const uint8_t *replacement, size_t replacement_length,
    const AbString *variable, const AbString *anchor, const AbString *token,
    ArchbirdMakeVariableTokenPosition position);

#endif
