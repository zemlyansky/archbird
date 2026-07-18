#ifndef ARCHBIRD_INTERCHANGE_VERIFY_REPORTS_H
#define ARCHBIRD_INTERCHANGE_VERIFY_REPORTS_H

#include "verify_runtime.h"

ArchbirdStatus ab_verify_render_markdown(AbVerificationContext *context,
                                         AbBuffer *buffer, size_t max_findings);
ArchbirdStatus ab_verify_render_sarif(AbVerificationContext *context,
                                      AbBuffer *buffer);
ArchbirdStatus ab_verify_render_junit(AbVerificationContext *context,
                                      AbBuffer *buffer);

#endif
