#ifndef ARCHBIRD_ACT_PROJECTION_ACCEPTANCE_H
#define ARCHBIRD_ACT_PROJECTION_ACCEPTANCE_H

#include "json_value.h"
#include "render_internal.h"

ArchbirdStatus ab_act_projection_deltas_accept(ArchbirdEngine *engine,
                                               const AbValue *requirements,
                                               const AbValue *before_map,
                                               const AbValue *after_map,
                                               AbBuffer *rendered);

#endif
