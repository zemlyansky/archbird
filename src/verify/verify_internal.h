#ifndef ARCHBIRD_VERIFY_INTERNAL_H
#define ARCHBIRD_VERIFY_INTERNAL_H

#include "json_value.h"

int ab_verify_string_is(const AbValue *value, const char *literal);
int ab_verify_nonblank(const AbValue *value);
int ab_verify_path_is_repository(const AbValue *value);

#endif
