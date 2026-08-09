#ifndef ARCHBIRD_QUERY_CONTEXT_H
#define ARCHBIRD_QUERY_CONTEXT_H

#include "base/archbird_internal.h"
#include "base/json_value.h"

ArchbirdStatus ab_query_context_validate(ArchbirdEngine *engine,
                                         const AbValue *context);

#endif
