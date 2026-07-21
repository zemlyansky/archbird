#include "verify_membership.h"

#include "verify_internal.h"

#include <stdlib.h>
#include <string.h>

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int file_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyMembershipFile *left = (const AbVerifyMembershipFile *)left_raw;
  const AbVerifyMembershipFile *right =
      (const AbVerifyMembershipFile *)right_raw;
  return ab_string_compare(left->path, right->path);
}

static int component_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyMembershipComponent *left =
      (const AbVerifyMembershipComponent *)left_raw;
  const AbVerifyMembershipComponent *right =
      (const AbVerifyMembershipComponent *)right_raw;
  return ab_string_compare(left->name, right->name);
}

static int assignment_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyMembershipAssignment *left =
      (const AbVerifyMembershipAssignment *)left_raw;
  const AbVerifyMembershipAssignment *right =
      (const AbVerifyMembershipAssignment *)right_raw;
  int compared = ab_string_compare(left->path, right->path);
  if (compared)
    return compared;
  return left->component_index < right->component_index
             ? -1
             : left->component_index > right->component_index;
}

void ab_verify_membership_index_free(ArchbirdEngine *engine,
                                     AbVerifyMembershipIndex *index) {
  if (!index)
    return;
  ab_free(engine, index->files);
  ab_free(engine, index->components);
  ab_free(engine, index->assignments);
  memset(index, 0, sizeof(*index));
}

static void membership_unknown(ArchbirdEngine *engine,
                               AbVerifyMembershipIndex *index,
                               const char *message) {
  ab_verify_membership_index_free(engine, index);
  index->message = message;
}

static int valid_file(const AbValue *row, const AbValue **out_path) {
  const AbValue *path = ab_value_member(row, "path");
  const AbValue *layer = ab_value_member(row, "layer");
  const AbValue *language = ab_value_member(row, "language");
  if (!row || row->kind != AB_VALUE_OBJECT ||
      !ab_verify_path_is_repository(path) || !layer ||
      layer->kind != AB_VALUE_STRING || !language ||
      language->kind != AB_VALUE_STRING ||
      !lowercase_sha256(ab_value_member(row, "sha256")))
    return 0;
  *out_path = path;
  return 1;
}

static int valid_component(const AbValue *row, const AbValue **out_name,
                           const AbValue **out_files) {
  const AbValue *name = ab_value_member(row, "name");
  const AbValue *files = ab_value_member(row, "files");
  if (!row || row->kind != AB_VALUE_OBJECT || !ab_verify_nonblank(name) ||
      !files || files->kind != AB_VALUE_ARRAY)
    return 0;
  *out_name = name;
  *out_files = files;
  return 1;
}

const AbVerifyMembershipComponent *
ab_verify_membership_component(const AbVerifyMembershipIndex *index,
                               const AbString *name, size_t *out_index) {
  size_t low = 0;
  size_t high = index ? index->component_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(index->components[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      if (out_index)
        *out_index = middle;
      return &index->components[middle];
    }
  }
  return NULL;
}

ArchbirdStatus ab_verify_membership_index_build(ArchbirdEngine *engine,
                                                const AbValue *map,
                                                AbVerifyMembershipIndex *out) {
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *components = map ? ab_value_member(map, "components") : NULL;
  size_t assignment_capacity = 0;
  size_t file_index;
  size_t component_index;
  size_t assignment_index = 0;
  if (!engine || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!files || files->kind != AB_VALUE_ARRAY || !components ||
      components->kind != AB_VALUE_ARRAY) {
    out->message = "project map has no component/file inventory";
    return ARCHBIRD_OK;
  }
  if (files->as.array.count > SIZE_MAX / sizeof(*out->files) ||
      components->as.array.count > SIZE_MAX / sizeof(*out->components))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "component membership inventory is too large");
  out->file_count = files->as.array.count;
  out->component_count = components->as.array.count;
  if (out->file_count) {
    out->files = (AbVerifyMembershipFile *)ab_calloc(engine, out->file_count,
                                                     sizeof(*out->files));
    if (!out->files)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing membership files");
  }
  if (out->component_count) {
    out->components = (AbVerifyMembershipComponent *)ab_calloc(
        engine, out->component_count, sizeof(*out->components));
    if (!out->components) {
      ab_verify_membership_index_free(engine, out);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing components");
    }
  }
  for (file_index = 0; file_index < out->file_count; file_index++) {
    const AbValue *path;
    const AbValue *row = &files->as.array.items[file_index];
    if (!valid_file(row, &path)) {
      membership_unknown(engine, out,
                         "project map has an invalid file inventory");
      return ARCHBIRD_OK;
    }
    out->files[file_index].row = row;
    out->files[file_index].path = &path->as.text;
  }
  if (out->file_count > 1)
    qsort(out->files, out->file_count, sizeof(*out->files), file_compare);
  for (file_index = 1; file_index < out->file_count; file_index++) {
    if (ab_string_equal(out->files[file_index - 1].path,
                        out->files[file_index].path)) {
      membership_unknown(engine, out,
                         "project map has duplicate file identities");
      return ARCHBIRD_OK;
    }
  }
  for (component_index = 0; component_index < out->component_count;
       component_index++) {
    const AbValue *name;
    const AbValue *members;
    const AbValue *row = &components->as.array.items[component_index];
    if (!valid_component(row, &name, &members)) {
      membership_unknown(engine, out,
                         "project map has an invalid component inventory");
      return ARCHBIRD_OK;
    }
    if (members->as.array.count > SIZE_MAX - assignment_capacity) {
      ab_verify_membership_index_free(engine, out);
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "component membership inventory is too large");
    }
    out->components[component_index].row = row;
    out->components[component_index].name = &name->as.text;
    assignment_capacity += members->as.array.count;
  }
  if (out->component_count > 1)
    qsort(out->components, out->component_count, sizeof(*out->components),
          component_compare);
  for (component_index = 1; component_index < out->component_count;
       component_index++) {
    if (ab_string_equal(out->components[component_index - 1].name,
                        out->components[component_index].name)) {
      membership_unknown(engine, out,
                         "project map has duplicate component identities");
      return ARCHBIRD_OK;
    }
  }
  if (assignment_capacity > SIZE_MAX / sizeof(*out->assignments)) {
    ab_verify_membership_index_free(engine, out);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "component membership inventory is too large");
  }
  if (assignment_capacity) {
    out->assignments = (AbVerifyMembershipAssignment *)ab_calloc(
        engine, assignment_capacity, sizeof(*out->assignments));
    if (!out->assignments) {
      ab_verify_membership_index_free(engine, out);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing component assignments");
    }
  }
  for (component_index = 0; component_index < out->component_count;
       component_index++) {
    const AbValue *members =
        ab_value_member(out->components[component_index].row, "files");
    size_t member_index;
    for (member_index = 0; member_index < members->as.array.count;
         member_index++) {
      const AbValue *path = &members->as.array.items[member_index];
      if (!ab_verify_path_is_repository(path)) {
        membership_unknown(engine, out,
                           "project map has an invalid component file path");
        return ARCHBIRD_OK;
      }
      out->assignments[assignment_index].path = &path->as.text;
      out->assignments[assignment_index].component_index = component_index;
      assignment_index++;
    }
  }
  out->assignment_count = assignment_index;
  if (out->assignment_count > 1)
    qsort(out->assignments, out->assignment_count, sizeof(*out->assignments),
          assignment_compare);
  for (assignment_index = 1; assignment_index < out->assignment_count;
       assignment_index++) {
    const AbVerifyMembershipAssignment *previous =
        &out->assignments[assignment_index - 1];
    const AbVerifyMembershipAssignment *current =
        &out->assignments[assignment_index];
    if (previous->component_index == current->component_index &&
        ab_string_equal(previous->path, current->path)) {
      membership_unknown(engine, out,
                         "project map has duplicate component assignments");
      return ARCHBIRD_OK;
    }
  }
  assignment_index = 0;
  for (file_index = 0; file_index < out->file_count; file_index++) {
    AbVerifyMembershipFile *file = &out->files[file_index];
    size_t start;
    if (assignment_index < out->assignment_count &&
        ab_string_compare(out->assignments[assignment_index].path, file->path) <
            0) {
      membership_unknown(
          engine, out,
          "project map component inventory references an unknown file");
      return ARCHBIRD_OK;
    }
    start = assignment_index;
    while (assignment_index < out->assignment_count &&
           ab_string_equal(out->assignments[assignment_index].path, file->path))
      assignment_index++;
    file->assignment_start = start;
    file->assignment_count = assignment_index - start;
    if (!file->assignment_count) {
      out->unassigned_count++;
      continue;
    }
    out->assigned_count++;
    if (file->assignment_count > 1)
      out->overlap_count++;
    while (start < assignment_index) {
      AbVerifyMembershipComponent *component =
          &out->components[out->assignments[start].component_index];
      component->file_count++;
      if (file->assignment_count == 1)
        component->exclusive_count++;
      else
        component->overlap_count++;
      start++;
    }
  }
  if (assignment_index != out->assignment_count) {
    membership_unknown(
        engine, out,
        "project map component inventory references an unknown file");
    return ARCHBIRD_OK;
  }
  for (component_index = 0; component_index < out->component_count;
       component_index++)
    if (!out->components[component_index].file_count)
      out->empty_component_count++;
  out->current = 1;
  out->message = "";
  return ARCHBIRD_OK;
}
