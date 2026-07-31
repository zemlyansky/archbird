#ifndef ARCHBIRD_PLAN_REPORT_H
#define ARCHBIRD_PLAN_REPORT_H

#include "plan_internal.h"
#include "render_internal.h"

ArchbirdStatus ab_plan_report_markdown(ArchbirdEngine *engine,
                                       const AbPlan *plan, AbBuffer *out);

#endif
