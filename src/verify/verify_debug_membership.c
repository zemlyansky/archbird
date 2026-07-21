#include "verify_debug_membership.h"

#include "verify_membership.h"

#include <string.h>

#define MEMBERSHIP_TRY(expression)                                             \
  do {                                                                         \
    ArchbirdStatus membership_status__ = (expression);                         \
    if (membership_status__ != ARCHBIRD_OK)                                    \
      return membership_status__;                                              \
  } while (0)

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

int ab_verify_debug_is_membership_view(const AbString *view) {
  return string_is(view, "component") || string_is(view, "unassigned") ||
         string_is(view, "overlap");
}

static const AbValue *selected_project(const AbVerificationContext *context,
                                       const AbString *name) {
  size_t index;
  if (!name)
    return NULL;
  for (index = 0; index < context->input.projects->as.array.count; index++) {
    const AbValue *row = &context->input.projects->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

ArchbirdStatus ab_verify_debug_membership_validate(
    AbVerificationContext *context, const AbString *view,
    const AbString *project, const AbString *component) {
  const AbValue *project_row;
  if (!context || !view || !ab_verify_debug_is_membership_view(view))
    return ARCHBIRD_INVALID_ARGUMENT;
  project_row = selected_project(context, project);
  if (project && !project_row)
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "verification debug project is unknown");
  if (!string_is(view, "component")) {
    if (component)
      return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "component filter requires component view");
    return ARCHBIRD_OK;
  }
  if (!component || !component->length)
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "component view requires a component name");
  if (!project_row && context->input.projects->as.array.count != 1)
    return archbird_error_set(
        context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "component view requires a project for a multi-project suite");
  if (!project_row)
    project_row = &context->input.projects->as.array.items[0];
  {
    const AbValue *map = ab_value_member(project_row, "map");
    AbVerifyMembershipIndex index = {0};
    ArchbirdStatus status =
        ab_verify_membership_index_build(context->engine, map, &index);
    if (status != ARCHBIRD_OK)
      return status;
    if (index.current &&
        !ab_verify_membership_component(&index, component, NULL))
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "verification debug component is unknown");
    ab_verify_membership_index_free(context->engine, &index);
    return status;
  }
}

static ArchbirdStatus render_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value ? value->data : "",
                               value ? value->length : 0);
}

static ArchbirdStatus render_optional_string(AbBuffer *buffer,
                                             const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING
             ? render_string(buffer, &value->as.text)
             : ab_buffer_literal(buffer, "null");
}

static int file_has_component(const AbVerifyMembershipIndex *index,
                              const AbVerifyMembershipFile *file,
                              size_t component_index) {
  size_t low = file->assignment_start;
  size_t high = low + file->assignment_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    size_t candidate = index->assignments[middle].component_index;
    if (candidate < component_index)
      low = middle + 1;
    else if (candidate > component_index)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static ArchbirdStatus
render_file_components(AbBuffer *buffer, const AbVerifyMembershipIndex *index,
                       const AbVerifyMembershipFile *file) {
  size_t offset;
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "["));
  for (offset = 0; offset < file->assignment_count; offset++) {
    const AbVerifyMembershipAssignment *assignment =
        &index->assignments[file->assignment_start + offset];
    if (offset)
      MEMBERSHIP_TRY(ab_buffer_literal(buffer, ","));
    MEMBERSHIP_TRY(render_string(
        buffer, index->components[assignment->component_index].name));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_file(AbBuffer *buffer,
                                  const AbVerifyMembershipIndex *index,
                                  const AbVerifyMembershipFile *file) {
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "{\"components\":"));
  MEMBERSHIP_TRY(render_file_components(buffer, index, file));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"language\":"));
  MEMBERSHIP_TRY(
      render_optional_string(buffer, ab_value_member(file->row, "language")));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"layer\":"));
  MEMBERSHIP_TRY(
      render_optional_string(buffer, ab_value_member(file->row, "layer")));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  MEMBERSHIP_TRY(render_string(buffer, file->path));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  MEMBERSHIP_TRY(
      render_optional_string(buffer, ab_value_member(file->row, "sha256")));
  return ab_buffer_literal(buffer, "}");
}

static int file_selected(const AbString *view,
                         const AbVerifyMembershipIndex *index,
                         const AbVerifyMembershipFile *file,
                         size_t component_index) {
  if (string_is(view, "unassigned"))
    return file->assignment_count == 0;
  if (string_is(view, "overlap"))
    return file->assignment_count > 1;
  return file_has_component(index, file, component_index);
}

static ArchbirdStatus render_counts(AbBuffer *buffer,
                                    const AbVerifyMembershipIndex *index) {
  if (!index->current)
    return ab_buffer_literal(
        buffer, "{\"assigned\":null,\"assignments\":null,\"components\":null,"
                "\"empty_components\":null,\"mapped_files\":null,"
                "\"overlapping\":null,\"unassigned\":null}");
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "{\"assigned\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->assigned_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"assignments\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->assignment_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"components\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->component_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"empty_components\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->empty_component_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"mapped_files\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->file_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"overlapping\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->overlap_count));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, ",\"unassigned\":"));
  MEMBERSHIP_TRY(ab_buffer_u64(buffer, (uint64_t)index->unassigned_count));
  return ab_buffer_literal(buffer, "}");
}

static void membership_state(const AbVerificationContext *context,
                             const AbString *project,
                             const AbVerifyMembershipIndex *index,
                             const char **out_state, const char **out_message) {
  AbVerifySourceLockState lock_state;
  if (!index->current) {
    *out_state = "unknown";
    *out_message = index->message ? index->message : "";
    return;
  }
  lock_state = ab_verify_source_lock_state(context, project);
  if (lock_state == AB_VERIFY_SOURCE_LOCK_MISMATCH) {
    *out_state = "stale";
    *out_message = "project source lock does not match current source bytes";
  } else if (lock_state == AB_VERIFY_SOURCE_LOCK_UNAVAILABLE) {
    *out_state = "unknown";
    *out_message =
        "project source lock cannot be checked from supplied source bytes";
  } else {
    *out_state = "current";
    *out_message = "";
  }
}

static ArchbirdStatus
render_project_json(AbBuffer *buffer, const AbVerificationContext *context,
                    const AbValue *project_row, const AbString *view,
                    const AbString *component, size_t limit) {
  const AbValue *name = ab_value_member(project_row, "name");
  const AbValue *map = ab_value_member(project_row, "map");
  AbVerifyMembershipIndex index = {0};
  const AbVerifyMembershipComponent *selected = NULL;
  size_t component_index = 0;
  size_t file_index;
  size_t matched = 0;
  size_t rendered = 0;
  const char *state;
  const char *message;
  ArchbirdStatus status =
      ab_verify_membership_index_build(context->engine, map, &index);
  if (status != ARCHBIRD_OK)
    return status;
  membership_state(context, &name->as.text, &index, &state, &message);
  if (index.current && component)
    selected =
        ab_verify_membership_component(&index, component, &component_index);
  status = ab_buffer_literal(buffer, "{\"component\":");
  if (status == ARCHBIRD_OK && selected) {
    status = ab_buffer_literal(buffer, "{\"exclusive_files\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->exclusive_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"files\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->file_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = render_string(buffer, selected->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"overlapping_files\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->overlap_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  } else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "null");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"counts\":");
  if (status == ARCHBIRD_OK)
    status = render_counts(buffer, &index);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"files\":[");
  for (file_index = 0;
       status == ARCHBIRD_OK && index.current && file_index < index.file_count;
       file_index++) {
    const AbVerifyMembershipFile *file = &index.files[file_index];
    if (!file_selected(view, &index, file, component_index))
      continue;
    matched++;
    if (rendered >= limit)
      continue;
    if (rendered++)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = render_file(buffer, &index, file);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "],\"message\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, message, strlen(message));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(buffer, name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"selection\":");
  if (status == ARCHBIRD_OK && index.current) {
    status = ab_buffer_literal(buffer, "{\"matched\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)matched);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"rendered\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)rendered);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"truncated\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, matched > rendered ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  } else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "null");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"state\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, state, strlen(state));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  ab_verify_membership_index_free(context->engine, &index);
  return status;
}

ArchbirdStatus ab_verify_debug_membership_render_json(
    AbBuffer *buffer, const AbVerificationContext *context,
    const AbString *view, const AbString *project, const AbString *component,
    size_t limit) {
  size_t index;
  size_t emitted = 0;
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->input.projects->as.array.count; index++) {
    const AbValue *row = &context->input.projects->as.array.items[index];
    const AbValue *name = ab_value_member(row, "name");
    if (project && !ab_string_equal(project, &name->as.text))
      continue;
    if (emitted++)
      MEMBERSHIP_TRY(ab_buffer_literal(buffer, ","));
    MEMBERSHIP_TRY(
        render_project_json(buffer, context, row, view, component, limit));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus append_field(AbBuffer *buffer, const char *name,
                                   const AbString *value) {
  if (!value || !value->length)
    return ARCHBIRD_OK;
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, " "));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, name));
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "="));
  return ab_buffer_append(buffer, value->data, value->length);
}

static ArchbirdStatus
render_project_markdown(AbBuffer *buffer, const AbVerificationContext *context,
                        const AbValue *project_row, const AbString *view,
                        const AbString *component, size_t limit) {
  const AbValue *name = ab_value_member(project_row, "name");
  const AbValue *map = ab_value_member(project_row, "map");
  AbVerifyMembershipIndex index = {0};
  const AbVerifyMembershipComponent *selected = NULL;
  size_t component_index = 0;
  size_t file_index;
  size_t matched = 0;
  size_t rendered = 0;
  const char *state;
  const char *message;
  ArchbirdStatus status =
      ab_verify_membership_index_build(context->engine, map, &index);
  if (status != ARCHBIRD_OK)
    return status;
  membership_state(context, &name->as.text, &index, &state, &message);
  if (index.current && component)
    selected =
        ab_verify_membership_component(&index, component, &component_index);
  status = ab_buffer_literal(buffer, "### ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(buffer, name->as.text.data, name->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "state=");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, state);
  if (status == ARCHBIRD_OK && message[0])
    status = ab_buffer_literal(buffer, " message=");
  if (status == ARCHBIRD_OK && message[0])
    status = ab_buffer_literal(buffer, message);
  if (status == ARCHBIRD_OK && index.current) {
    status = ab_buffer_literal(buffer, " mapped_files=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.file_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " assigned=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.assigned_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " unassigned=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.unassigned_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " overlapping=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.overlap_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " components=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.component_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " empty_components=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)index.empty_component_count);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "\n");
  if (status == ARCHBIRD_OK && selected) {
    status = ab_buffer_literal(buffer, "component=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, selected->name->data,
                                selected->name->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " files=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->file_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " exclusive=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->exclusive_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " overlapping=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)selected->overlap_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\n");
  }
  for (file_index = 0;
       status == ARCHBIRD_OK && index.current && file_index < index.file_count;
       file_index++) {
    const AbVerifyMembershipFile *file = &index.files[file_index];
    size_t offset;
    if (!file_selected(view, &index, file, component_index))
      continue;
    matched++;
    if (rendered >= limit)
      continue;
    rendered++;
    status = ab_buffer_literal(buffer, "- path=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(buffer, file->path->data, file->path->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " components=");
    if (status == ARCHBIRD_OK && !file->assignment_count)
      status = ab_buffer_literal(buffer, "(none)");
    for (offset = 0; status == ARCHBIRD_OK && offset < file->assignment_count;
         offset++) {
      const AbVerifyMembershipAssignment *assignment =
          &index.assignments[file->assignment_start + offset];
      const AbString *component_name =
          index.components[assignment->component_index].name;
      if (offset)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(buffer, component_name->data,
                                  component_name->length);
    }
    if (status == ARCHBIRD_OK)
      status = append_field(buffer, "layer",
                            &ab_value_member(file->row, "layer")->as.text);
    if (status == ARCHBIRD_OK)
      status = append_field(buffer, "language",
                            &ab_value_member(file->row, "language")->as.text);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "\n");
  }
  if (status == ARCHBIRD_OK && index.current) {
    status = ab_buffer_literal(buffer, "selection matched=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)matched);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " rendered=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(buffer, (uint64_t)rendered);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, " truncated=");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_literal(buffer, matched > rendered ? "true\n" : "false\n");
  }
  ab_verify_membership_index_free(context->engine, &index);
  return status;
}

ArchbirdStatus ab_verify_debug_membership_render_markdown(
    AbBuffer *buffer, const AbVerificationContext *context,
    const AbString *view, const AbString *project, const AbString *component,
    size_t limit) {
  size_t index;
  MEMBERSHIP_TRY(ab_buffer_literal(buffer, "## Component membership\n\n"));
  for (index = 0; index < context->input.projects->as.array.count; index++) {
    const AbValue *row = &context->input.projects->as.array.items[index];
    const AbValue *name = ab_value_member(row, "name");
    if (project && !ab_string_equal(project, &name->as.text))
      continue;
    MEMBERSHIP_TRY(
        render_project_markdown(buffer, context, row, view, component, limit));
    MEMBERSHIP_TRY(ab_buffer_literal(buffer, "\n"));
  }
  return ARCHBIRD_OK;
}

#undef MEMBERSHIP_TRY
