#ifndef ARCHBIRD_REPORT_UTILS_H
#define ARCHBIRD_REPORT_UTILS_H

#include "json_value.h"

typedef struct AbReportStringList {
  ArchbirdEngine *engine;
  AbString *items;
  size_t count;
  size_t capacity;
} AbReportStringList;

void ab_report_list_init(AbReportStringList *list, ArchbirdEngine *engine);
void ab_report_list_free(AbReportStringList *list);
ArchbirdStatus ab_report_list_add(AbReportStringList *list, const char *data,
                                  size_t length);
ArchbirdStatus ab_report_list_addf(AbReportStringList *list, const char *format,
                                   ...);
void ab_report_list_sort(AbReportStringList *list);
void ab_report_list_sort_unique(AbReportStringList *list);

ArchbirdStatus ab_report_appendf(AbBuffer *buffer, const char *format, ...);
ArchbirdStatus ab_report_linef(AbBuffer *buffer, const char *format, ...);
ArchbirdStatus ab_report_line(AbBuffer *buffer, const char *data,
                              size_t length);
ArchbirdStatus ab_report_literal_line(AbBuffer *buffer, const char *literal);
ArchbirdStatus ab_report_blank(AbBuffer *buffer);
ArchbirdStatus ab_report_chunks(AbBuffer *buffer,
                                const AbReportStringList *items,
                                const char *prefix, size_t width);

size_t ab_report_codepoints(const uint8_t *data, size_t length);
const AbValue *ab_report_array(const AbValue *object, const char *name);
const AbValue *ab_report_object(const AbValue *object, const char *name);
const AbString *ab_report_string(const AbValue *object, const char *name);
int ab_report_bool(const AbValue *object, const char *name, int fallback);
size_t ab_report_size(const AbValue *object, const char *name, size_t fallback);
int ab_report_string_equal(const AbString *value, const char *literal);

#endif
