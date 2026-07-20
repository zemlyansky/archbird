#ifndef ARCHBIRD_MAP_REPORTS_H
#define ARCHBIRD_MAP_REPORTS_H

#include "json_value.h"

ArchbirdStatus ab_map_report_markdown(ArchbirdEngine *engine,
                                      const AbValue *map, int full,
                                      size_t max_chars, AbBuffer *out);

ArchbirdStatus ab_map_report_markdown_view(ArchbirdEngine *engine,
                                           const AbValue *map,
                                           ArchbirdMapView view,
                                           ArchbirdReportDetail detail,
                                           size_t max_chars, AbBuffer *out);

ArchbirdStatus ab_query_report_markdown(ArchbirdEngine *engine,
                                        const AbValue *map,
                                        const AbValue *query, size_t max_chars,
                                        AbBuffer *out);

ArchbirdStatus ab_query_report_markdown_view(ArchbirdEngine *engine,
                                             const AbValue *map,
                                             const AbValue *query,
                                             ArchbirdQueryView view,
                                             ArchbirdReportDetail detail,
                                             size_t max_chars, AbBuffer *out);

#endif
