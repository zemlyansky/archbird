#include "plan_report.h"

#include <string.h>

#define AB_PLAN_REPORT_EVIDENCE_LIMIT 8u

#define REPORT_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus append_size(AbBuffer *out, size_t value) {
  return ab_buffer_u64(out, (uint64_t)value);
}

static ArchbirdStatus append_markdown_text(AbBuffer *out,
                                           const AbString *value) {
  size_t index;
  int previous_space = 0;
  if (!value)
    return ARCHBIRD_OK;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    int space = byte == '\n' || byte == '\r' || byte == '\t' || byte < 0x20u;
    if (space) {
      if (!previous_space)
        REPORT_TRY(ab_buffer_literal(out, " "));
      previous_space = 1;
      continue;
    }
    previous_space = byte == ' ';
    if (byte == '\\' || byte == '`' || byte == '*' || byte == '_' ||
        byte == '[' || byte == ']' || byte == '<' || byte == '>' ||
        byte == '#' || byte == '|')
      REPORT_TRY(ab_buffer_literal(out, "\\"));
    REPORT_TRY(ab_buffer_append(out, &byte, 1));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_code(AbBuffer *out, const AbString *value) {
  size_t index;
  REPORT_TRY(ab_buffer_literal(out, "`"));
  for (index = 0; value && index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (byte == '`')
      REPORT_TRY(ab_buffer_literal(out, "\\"));
    REPORT_TRY(ab_buffer_append(out, &byte, 1));
  }
  return ab_buffer_literal(out, "`");
}

static ArchbirdStatus render_code_array(AbBuffer *out, const AbValue *array) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY || !array->as.array.count)
    return ab_buffer_literal(out, "none");
  for (index = 0; index < array->as.array.count; index++) {
    if (index)
      REPORT_TRY(ab_buffer_literal(out, ", "));
    REPORT_TRY(append_code(out, &array->as.array.items[index].as.text));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_operation_field(AbBuffer *out,
                                             const AbValue *operation,
                                             const char *name,
                                             const char *label) {
  const AbValue *value = field(operation, name);
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_buffer_literal(out, "- "));
  REPORT_TRY(ab_buffer_append(out, label, strlen(label)));
  REPORT_TRY(ab_buffer_literal(out, ": "));
  REPORT_TRY(append_code(out, &value->as.text));
  return ab_buffer_literal(out, "\n");
}

static ArchbirdStatus render_projection_deltas(AbBuffer *out,
                                               const AbValue *acceptance) {
  const AbValue *rows = field(acceptance, "projection_deltas");
  size_t index;
  for (index = 0; rows && index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    REPORT_TRY(ab_buffer_literal(out, "- Allowed projection delta: add "));
    REPORT_TRY(render_code_array(out, field(row, "allowed_added")));
    REPORT_TRY(ab_buffer_literal(out, "; remove "));
    REPORT_TRY(render_code_array(out, field(row, "allowed_removed")));
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_evidence(AbBuffer *out, const AbValue *evidence) {
  size_t shown = evidence->as.array.count < AB_PLAN_REPORT_EVIDENCE_LIMIT
                     ? evidence->as.array.count
                     : AB_PLAN_REPORT_EVIDENCE_LIMIT;
  size_t index;
  for (index = 0; index < shown; index++) {
    const AbValue *row = &evidence->as.array.items[index];
    const AbValue *path = field(row, "path");
    const AbValue *line = field(row, "line");
    const AbValue *detail = field(row, "detail");
    uint64_t line_number = 0;
    REPORT_TRY(ab_buffer_literal(out, "  - "));
    if (path->as.text.length)
      REPORT_TRY(append_code(out, &path->as.text));
    else
      REPORT_TRY(ab_buffer_literal(out, "`project evidence`"));
    if (ab_value_u64(line, &line_number) && line_number) {
      REPORT_TRY(ab_buffer_literal(out, ":"));
      REPORT_TRY(ab_buffer_u64(out, line_number));
    }
    if (detail->as.text.length) {
      REPORT_TRY(ab_buffer_literal(out, " - "));
      REPORT_TRY(append_markdown_text(out, &detail->as.text));
    }
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  if (shown < evidence->as.array.count) {
    REPORT_TRY(ab_buffer_literal(out, "  - "));
    REPORT_TRY(append_size(out, evidence->as.array.count - shown));
    REPORT_TRY(ab_buffer_literal(out,
                                 " additional evidence row(s) are retained "
                                 "in Plan JSON.\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_item(AbBuffer *out, const AbValue *item,
                                  size_t index) {
  const AbValue *id = field(item, "id");
  const AbValue *statement = field(item, "statement");
  const AbValue *operation = field(item, "operation");
  const AbValue *action = field(operation, "action");
  const AbValue *executable = field(item, "executable");
  const AbValue *acceptance = field(item, "acceptance");
  const AbValue *dependencies = field(item, "depends_on");
  const AbValue *reasons = field(item, "non_executable_reasons");
  const AbValue *evidence = field(item, "evidence");
  const AbValue *unknowns = field(item, "unknowns");
  REPORT_TRY(ab_buffer_literal(out, "### "));
  REPORT_TRY(append_size(out, index + 1));
  REPORT_TRY(ab_buffer_literal(out, ". "));
  REPORT_TRY(ab_buffer_literal(out, executable->as.boolean ? "EXECUTABLE"
                                                           : "INPUT REQUIRED"));
  REPORT_TRY(ab_buffer_literal(out, " "));
  REPORT_TRY(append_code(out, &id->as.text));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  REPORT_TRY(append_markdown_text(out, &statement->as.text));
  REPORT_TRY(ab_buffer_literal(out, "\n\n- Action: "));
  REPORT_TRY(append_code(out, &action->as.text));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  REPORT_TRY(render_operation_field(out, operation, "path", "Path"));
  REPORT_TRY(
      render_operation_field(out, operation, "source_path", "Source path"));
  REPORT_TRY(
      render_operation_field(out, operation, "target_path", "Target path"));
  REPORT_TRY(render_operation_field(out, operation, "destination_path",
                                    "Destination path"));
  REPORT_TRY(render_operation_field(out, operation, "target", "Target"));
  REPORT_TRY(render_operation_field(out, operation, "symbol", "Symbol"));
  REPORT_TRY(
      render_operation_field(out, operation, "capability", "Capability"));
  REPORT_TRY(render_operation_field(out, operation, "surface", "Surface"));
  REPORT_TRY(render_operation_field(out, operation, "package", "Package"));
  REPORT_TRY(render_operation_field(out, operation, "route", "Route"));
  REPORT_TRY(render_operation_field(out, operation, "relation", "Relation"));
  REPORT_TRY(render_operation_field(out, operation, "name", "Relation name"));
  REPORT_TRY(render_operation_field(out, operation, "from", "From"));
  REPORT_TRY(render_operation_field(out, operation, "to", "To"));
  REPORT_TRY(
      render_operation_field(out, operation, "from_symbol", "From symbol"));
  REPORT_TRY(render_operation_field(out, operation, "to_symbol", "To symbol"));
  REPORT_TRY(render_operation_field(out, operation, "new_name", "New symbol"));
  REPORT_TRY(ab_buffer_literal(out, "- Depends on: "));
  REPORT_TRY(render_code_array(out, dependencies));
  REPORT_TRY(ab_buffer_literal(out, "\n- Acceptance: "));
  REPORT_TRY(render_code_array(out, field(acceptance, "constraints")));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  REPORT_TRY(render_projection_deltas(out, acceptance));
  REPORT_TRY(ab_buffer_literal(out, "- Evidence rows: "));
  REPORT_TRY(append_size(out, evidence->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "; unknowns: "));
  REPORT_TRY(append_size(out, unknowns->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  REPORT_TRY(render_evidence(out, evidence));
  if (reasons->as.array.count) {
    REPORT_TRY(ab_buffer_literal(out, "- Blocking: "));
    REPORT_TRY(render_code_array(out, reasons));
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ab_buffer_literal(out, "\n");
}

static ArchbirdStatus render_unknowns(AbBuffer *out, const AbValue *unknowns) {
  size_t index;
  if (!unknowns->as.array.count)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_buffer_literal(out, "## Unknowns\n\n"));
  for (index = 0; index < unknowns->as.array.count; index++) {
    const AbValue *unknown = &unknowns->as.array.items[index];
    const AbValue *id = field(unknown, "id");
    const AbValue *statement = field(unknown, "statement");
    REPORT_TRY(ab_buffer_literal(out, "- "));
    REPORT_TRY(append_code(out, &id->as.text));
    REPORT_TRY(ab_buffer_literal(out, ": "));
    REPORT_TRY(append_markdown_text(out, &statement->as.text));
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ab_buffer_literal(out, "\n");
}

ArchbirdStatus ab_plan_report_markdown(ArchbirdEngine *engine,
                                       const AbPlan *plan, AbBuffer *out) {
  const AbValue *project;
  const AbValue *objective;
  size_t executable = 0;
  size_t index;
  if (!engine || !plan || !plan->items || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  project = field(plan->source, "project");
  objective = field(&plan->document, "objective");
  for (index = 0; index < plan->items->as.array.count; index++)
    if (field(&plan->items->as.array.items[index], "executable")->as.boolean)
      executable++;
  REPORT_TRY(ab_buffer_literal(out, "# Change Plan: "));
  REPORT_TRY(append_markdown_text(out, &project->as.text));
  REPORT_TRY(ab_buffer_literal(out, "\n\n## Objective\n\n"));
  REPORT_TRY(append_markdown_text(out, &objective->as.text));
  REPORT_TRY(ab_buffer_literal(out, "\n\n```text\n"));
  REPORT_TRY(ab_buffer_literal(out, "items="));
  REPORT_TRY(append_size(out, plan->items->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, " executable="));
  REPORT_TRY(append_size(out, executable));
  REPORT_TRY(ab_buffer_literal(out, " input-required="));
  REPORT_TRY(append_size(out, plan->items->as.array.count - executable));
  REPORT_TRY(ab_buffer_literal(out, " unknowns="));
  REPORT_TRY(append_size(out, plan->unknowns->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, " gates="));
  REPORT_TRY(append_size(out, plan->gates->as.object.count));
  REPORT_TRY(ab_buffer_literal(out, " preserved-constraints="));
  REPORT_TRY(append_size(out, plan->preserved_constraints->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  REPORT_TRY(ab_buffer_literal(out, "```\n\n## Items\n\n"));
  if (!plan->items->as.array.count)
    REPORT_TRY(ab_buffer_literal(out, "No change items.\n\n"));
  for (index = 0; index < plan->items->as.array.count; index++)
    REPORT_TRY(render_item(out, &plan->items->as.array.items[index], index));
  REPORT_TRY(render_unknowns(out, plan->unknowns));
  REPORT_TRY(ab_buffer_literal(out, "## Result\n\nResult: items="));
  REPORT_TRY(append_size(out, plan->items->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "; executable="));
  REPORT_TRY(append_size(out, executable));
  REPORT_TRY(ab_buffer_literal(out, "; input-required="));
  REPORT_TRY(append_size(out, plan->items->as.array.count - executable));
  REPORT_TRY(ab_buffer_literal(out, "; unknowns="));
  REPORT_TRY(append_size(out, plan->unknowns->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "; gates="));
  REPORT_TRY(append_size(out, plan->gates->as.object.count));
  REPORT_TRY(ab_buffer_literal(out, "; preserved-constraints="));
  REPORT_TRY(append_size(out, plan->preserved_constraints->as.array.count));
  REPORT_TRY(ab_buffer_literal(out, "; plan=`"));
  REPORT_TRY(ab_buffer_append(out, plan->sha256, 64));
  return ab_buffer_literal(out, "`.\n");
}
