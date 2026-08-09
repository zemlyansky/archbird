#ifndef ARCHBIRD_PLAN_REPORT_H
#define ARCHBIRD_PLAN_REPORT_H

#include "base/render_internal.h"
#include "plan/plan_internal.h"

ArchbirdStatus ab_plan_report_markdown(ArchbirdEngine *engine,
                                       const AbPlan *plan, AbBuffer *out);

#endif
