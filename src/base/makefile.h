#ifndef ARCHBIRD_BASE_MAKEFILE_H
#define ARCHBIRD_BASE_MAKEFILE_H

#include <stddef.h>

int ab_make_assignment(const char *line, size_t length, size_t *name_start,
                       size_t *name_end, size_t *operator_start,
                       size_t *operator_length, size_t *value_start);

#endif
