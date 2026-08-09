#ifndef ARCHBIRD_SOURCE_REPORT_H
#define ARCHBIRD_SOURCE_REPORT_H

#include "base/json_value.h"

typedef ArchbirdStatus (*AbSourceReportLookupFn)(void *user_data,
                                                 const AbString *path,
                                                 const uint8_t **out_bytes,
                                                 size_t *out_length);

ArchbirdStatus ab_source_report_markdown(ArchbirdEngine *engine,
                                         const AbValue *artifact,
                                         ArchbirdReportDetail detail,
                                         size_t max_chars,
                                         AbSourceReportLookupFn source_lookup,
                                         void *source_user_data, AbBuffer *out);

#endif
