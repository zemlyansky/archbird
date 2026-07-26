#ifndef ARCHBIRD_PROJECTION_REPORTS_H
#define ARCHBIRD_PROJECTION_REPORTS_H

#include "projection_internal.h"

ArchbirdStatus ab_projection_report_markdown(ArchbirdEngine *engine,
                                             const AbProjectionPlan *plan,
                                             const AbProjectionResult *result,
                                             ArchbirdReportDetail detail,
                                             size_t max_chars, AbBuffer *out);

#endif
