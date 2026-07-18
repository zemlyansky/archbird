#include "report_utils.h"

#include "archbird_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int report_string_compare(const void *left_raw, const void *right_raw) {
  const AbString *left = (const AbString *)left_raw;
  const AbString *right = (const AbString *)right_raw;
  return ab_string_compare(left, right);
}

void ab_report_list_init(AbReportStringList *list, ArchbirdEngine *engine) {
  memset(list, 0, sizeof(*list));
  list->engine = engine;
}

void ab_report_list_free(AbReportStringList *list) {
  size_t index;
  if (!list)
    return;
  for (index = 0; index < list->count; index++)
    ab_string_free(list->engine, &list->items[index]);
  ab_free(list->engine, list->items);
  memset(list, 0, sizeof(*list));
}

ArchbirdStatus ab_report_list_add(AbReportStringList *list, const char *data,
                                  size_t length) {
  AbString *resized;
  ArchbirdStatus status;
  if (!list || (!data && length))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (list->count == list->capacity) {
    size_t capacity = list->capacity ? list->capacity * 2 : 8;
    if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*resized))
      return archbird_error_set(list->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "report string list is too large");
    resized = (AbString *)ab_realloc(list->engine, list->items,
                                     capacity * sizeof(*resized));
    if (!resized)
      return archbird_error_set(list->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing report string list");
    memset(resized + list->capacity, 0,
           (capacity - list->capacity) * sizeof(*resized));
    list->items = resized;
    list->capacity = capacity;
  }
  status =
      ab_string_copy(list->engine, &list->items[list->count], data, length);
  if (status == ARCHBIRD_OK)
    list->count++;
  return status;
}

static ArchbirdStatus report_vformat(ArchbirdEngine *engine, AbString *out,
                                     const char *format, va_list arguments) {
  char local[512];
  char *dynamic = NULL;
  va_list copied;
  int needed;
  ArchbirdStatus status;
  va_copy(copied, arguments);
  needed = vsnprintf(local, sizeof(local), format, copied);
  va_end(copied);
  if (needed < 0)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "failed to format report text");
  if ((size_t)needed < sizeof(local))
    return ab_string_copy(engine, out, local, (size_t)needed);
  if ((size_t)needed == SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "formatted report text is too large");
  dynamic = (char *)ab_malloc(engine, (size_t)needed + 1);
  if (!dynamic)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory formatting report text");
  va_copy(copied, arguments);
  if (vsnprintf(dynamic, (size_t)needed + 1, format, copied) != needed) {
    va_end(copied);
    ab_free(engine, dynamic);
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "failed to format report text");
  }
  va_end(copied);
  status = ab_string_copy(engine, out, dynamic, (size_t)needed);
  ab_free(engine, dynamic);
  return status;
}

ArchbirdStatus ab_report_list_addf(AbReportStringList *list, const char *format,
                                   ...) {
  AbString text = {0};
  ArchbirdStatus status;
  va_list arguments;
  if (!list || !format)
    return ARCHBIRD_INVALID_ARGUMENT;
  va_start(arguments, format);
  status = report_vformat(list->engine, &text, format, arguments);
  va_end(arguments);
  if (status == ARCHBIRD_OK)
    status = ab_report_list_add(list, text.data, text.length);
  ab_string_free(list->engine, &text);
  return status;
}

void ab_report_list_sort(AbReportStringList *list) {
  if (list && list->count > 1)
    qsort(list->items, list->count, sizeof(*list->items),
          report_string_compare);
}

void ab_report_list_sort_unique(AbReportStringList *list) {
  size_t read_index;
  size_t write_index = 0;
  if (!list || !list->count)
    return;
  ab_report_list_sort(list);
  for (read_index = 0; read_index < list->count; read_index++) {
    if (write_index && ab_string_equal(&list->items[write_index - 1],
                                       &list->items[read_index])) {
      ab_string_free(list->engine, &list->items[read_index]);
      continue;
    }
    if (write_index != read_index) {
      list->items[write_index] = list->items[read_index];
      memset(&list->items[read_index], 0, sizeof(list->items[read_index]));
    }
    write_index++;
  }
  list->count = write_index;
}

ArchbirdStatus ab_report_appendf(AbBuffer *buffer, const char *format, ...) {
  AbString text = {0};
  ArchbirdStatus status;
  va_list arguments;
  if (!buffer || !format)
    return ARCHBIRD_INVALID_ARGUMENT;
  va_start(arguments, format);
  status = report_vformat(buffer->engine, &text, format, arguments);
  va_end(arguments);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, text.data, text.length);
  ab_string_free(buffer->engine, &text);
  return status;
}

ArchbirdStatus ab_report_linef(AbBuffer *buffer, const char *format, ...) {
  AbString text = {0};
  ArchbirdStatus status;
  va_list arguments;
  if (!buffer || !format)
    return ARCHBIRD_INVALID_ARGUMENT;
  va_start(arguments, format);
  status = report_vformat(buffer->engine, &text, format, arguments);
  va_end(arguments);
  if (status == ARCHBIRD_OK)
    status = ab_report_line(buffer, text.data, text.length);
  ab_string_free(buffer->engine, &text);
  return status;
}

ArchbirdStatus ab_report_line(AbBuffer *buffer, const char *data,
                              size_t length) {
  ArchbirdStatus status = ab_buffer_append(buffer, data, length);
  return status == ARCHBIRD_OK ? ab_buffer_literal(buffer, "\n") : status;
}

ArchbirdStatus ab_report_literal_line(AbBuffer *buffer, const char *literal) {
  return ab_report_line(buffer, literal, strlen(literal));
}

ArchbirdStatus ab_report_blank(AbBuffer *buffer) {
  return ab_buffer_literal(buffer, "\n");
}

size_t ab_report_codepoints(const uint8_t *data, size_t length) {
  size_t index;
  size_t result = 0;
  for (index = 0; index < length; index++)
    if ((data[index] & 0xc0u) != 0x80u)
      result++;
  return result;
}

ArchbirdStatus ab_report_chunks(AbBuffer *buffer,
                                const AbReportStringList *items,
                                const char *prefix, size_t width) {
  size_t prefix_bytes = strlen(prefix);
  size_t prefix_width =
      ab_report_codepoints((const uint8_t *)prefix, prefix_bytes);
  size_t current_width = prefix_width;
  size_t index;
  ArchbirdStatus status = ab_buffer_append(buffer, prefix, prefix_bytes);
  if (status != ARCHBIRD_OK)
    return status;
  if (!items || !items->count) {
    while (buffer->length && buffer->data[buffer->length - 1] == ' ')
      buffer->length--;
    if (buffer->data)
      buffer->data[buffer->length] = '\0';
    return ab_buffer_literal(buffer, "\n");
  }
  for (index = 0; index < items->count; index++) {
    const AbString *item = &items->items[index];
    size_t item_width =
        ab_report_codepoints((const uint8_t *)item->data, item->length);
    size_t separator = index ? 1 : 0;
    if (index && current_width + separator + item_width > width &&
        current_width != prefix_width) {
      size_t spaces;
      status = ab_buffer_literal(buffer, "\n");
      for (spaces = 0; status == ARCHBIRD_OK && spaces < prefix_width; spaces++)
        status = ab_buffer_literal(buffer, " ");
      current_width = prefix_width;
      separator = 0;
    }
    if (status == ARCHBIRD_OK && separator)
      status = ab_buffer_literal(buffer, " ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, item->data, item->length);
    if (status != ARCHBIRD_OK)
      return status;
    current_width += separator + item_width;
  }
  return ab_buffer_literal(buffer, "\n");
}

const AbValue *ab_report_array(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_ARRAY ? value : NULL;
}

const AbValue *ab_report_object(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_OBJECT ? value : NULL;
}

const AbString *ab_report_string(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

int ab_report_bool(const AbValue *object, const char *name, int fallback) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_BOOL ? value->as.boolean : fallback;
}

size_t ab_report_size(const AbValue *object, const char *name,
                      size_t fallback) {
  const AbValue *value = ab_value_member(object, name);
  uint64_t number;
  if (!ab_value_u64(value, &number) || number > SIZE_MAX)
    return fallback;
  return (size_t)number;
}

int ab_report_string_equal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}
