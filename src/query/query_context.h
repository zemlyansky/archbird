#ifndef ARCHBIRD_QUERY_CONTEXT_H
#define ARCHBIRD_QUERY_CONTEXT_H

#include "archbird_internal.h"
#include "json_value.h"

ArchbirdStatus ab_query_context_validate(ArchbirdEngine *engine,
                                         const AbValue *context);

#endif
