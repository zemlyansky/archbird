#include "verify_runtime.h"

#include <stdlib.h>
#include <string.h>

static ArchbirdStatus copy_bytes(ArchbirdEngine *engine, AbString *out,
                                 const char *value, size_t length) {
  return ab_string_copy(engine, out, value ? value : "", value ? length : 0);
}

static void diagnostic_free(ArchbirdEngine *engine, AbVerifyDiagnostic *row) {
  ab_string_free(engine, &row->severity);
  ab_string_free(engine, &row->code);
  ab_string_free(engine, &row->message);
  ab_string_free(engine, &row->path);
  memset(row, 0, sizeof(*row));
}

ArchbirdStatus ab_verify_add_diagnostic(AbVerificationContext *context,
                                        const char *severity, const char *code,
                                        const char *message,
                                        size_t message_length, const char *path,
                                        size_t path_length) {
  AbVerifyDiagnostic *resized;
  AbVerifyDiagnostic *row;
  ArchbirdStatus status;
  if (!context || !context->engine || !severity || !code || !message)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (context->diagnostic_count == context->diagnostic_capacity) {
    size_t capacity =
        context->diagnostic_capacity ? context->diagnostic_capacity * 2 : 4;
    if (capacity < context->diagnostic_capacity ||
        capacity > SIZE_MAX / sizeof(*context->diagnostics))
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many verification diagnostics");
    resized = (AbVerifyDiagnostic *)ab_realloc(
        context->engine, context->diagnostics,
        capacity * sizeof(*context->diagnostics));
    if (!resized)
      return archbird_error_set(
          context->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing verification diagnostics");
    context->diagnostics = resized;
    context->diagnostic_capacity = capacity;
  }
  row = &context->diagnostics[context->diagnostic_count];
  memset(row, 0, sizeof(*row));
  status =
      copy_bytes(context->engine, &row->severity, severity, strlen(severity));
  if (status == ARCHBIRD_OK)
    status = copy_bytes(context->engine, &row->code, code, strlen(code));
  if (status == ARCHBIRD_OK)
    status =
        copy_bytes(context->engine, &row->message, message, message_length);
  if (status == ARCHBIRD_OK)
    status = copy_bytes(context->engine, &row->path, path, path_length);
  if (status == ARCHBIRD_OK)
    context->diagnostic_count++;
  else
    diagnostic_free(context->engine, row);
  return status;
}

static const AbValue *named_project(const AbValue *rows, const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *row_name = ab_value_member(row, "name");
    if (row_name && row_name->kind == AB_VALUE_STRING &&
        ab_string_equal(&row_name->as.text, name))
      return row;
  }
  return NULL;
}

static int map_has_errors(const AbValue *map) {
  const AbValue *rows = ab_value_member(map, "diagnostics");
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++)
    if (ab_verify_string_is(
            ab_value_member(&rows->as.array.items[index], "severity"), "error"))
      return 1;
  return 0;
}

ArchbirdStatus
ab_verify_collect_project_diagnostics(AbVerificationContext *context) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->suite.projects)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < context->suite.projects->as.object.count;
       index++) {
    const AbObjectField *project =
        &context->suite.projects->as.object.fields[index];
    const AbValue *input =
        named_project(context->input.projects, &project->name);
    const AbValue *map = input ? ab_value_member(input, "map") : NULL;
    if (map && map_has_errors(map)) {
      AbBuffer message;
      ab_buffer_init(&message, context->engine);
      status = ab_buffer_literal(&message, "project ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, project->name.data,
                                  project->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " map has validation errors");
      if (status == ARCHBIRD_OK)
        status = ab_verify_add_diagnostic(
            context, "error", "project-map-errors", (const char *)message.data,
            message.length, "", 0);
      ab_buffer_free(&message);
    }
  }
  return status;
}

static int diagnostic_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyDiagnostic *left = (const AbVerifyDiagnostic *)left_raw;
  const AbVerifyDiagnostic *right = (const AbVerifyDiagnostic *)right_raw;
  int compared = ab_string_compare(&left->severity, &right->severity);
  if (!compared)
    compared = ab_string_compare(&left->code, &right->code);
  if (!compared)
    compared = ab_string_compare(&left->message, &right->message);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  return compared;
}

void ab_verify_diagnostics_finish(AbVerificationContext *context) {
  size_t read_index;
  size_t write_index;
  if (!context || context->diagnostic_count < 2)
    return;
  qsort(context->diagnostics, context->diagnostic_count,
        sizeof(*context->diagnostics), diagnostic_compare);
  write_index = 1;
  for (read_index = 1; read_index < context->diagnostic_count; read_index++) {
    if (!diagnostic_compare(&context->diagnostics[write_index - 1],
                            &context->diagnostics[read_index])) {
      diagnostic_free(context->engine, &context->diagnostics[read_index]);
      continue;
    }
    if (write_index != read_index) {
      context->diagnostics[write_index] = context->diagnostics[read_index];
      memset(&context->diagnostics[read_index], 0,
             sizeof(context->diagnostics[read_index]));
    }
    write_index++;
  }
  context->diagnostic_count = write_index;
}

void ab_verify_diagnostics_free(AbVerificationContext *context) {
  size_t index;
  if (!context)
    return;
  for (index = 0; index < context->diagnostic_count; index++)
    diagnostic_free(context->engine, &context->diagnostics[index]);
  ab_free(context->engine, context->diagnostics);
  context->diagnostics = NULL;
  context->diagnostic_count = 0;
  context->diagnostic_capacity = 0;
}
