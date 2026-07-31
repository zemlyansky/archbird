#include <archbird/archbird.h>

#include "artifact_validation.h"
#include "base64.h"
#include "json_value.h"
#include "model.h"
#include "patch_source.h"
#include "plan_internal.h"
#include "project_internal.h"
#include "projection_internal.h"
#include "render_internal.h"
#include "sha256.h"
#include "utf8.h"
#include "verification_artifact.h"

#include <stdlib.h>
#include <string.h>

#define AB_PATCH_MAX_WORK_ITEMS AB_PATCH_MAX_TRANSITIONS
#define AB_PATCH_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define AB_PATCH_MAX_SOURCE_BYTES (256u * 1024u * 1024u)

typedef struct AbActEdit {
  size_t work_index;
  const AbString *item_id;
  size_t start;
  size_t end;
  const uint8_t *replacement;
  size_t replacement_length;
  uint8_t *owned_replacement;
  const AbString *make_variable;
  const AbString *make_anchor;
  const AbString *make_token;
  ArchbirdMakeVariableTokenPosition make_position;
} AbActEdit;

typedef struct AbActWork {
  AbString path;
  const uint8_t *before;
  size_t before_length;
  char before_sha256[65];
  int before_exists;
  int executable;
  int create;
  int delete_file;
  int move;
  AbString destination;
  const AbString *action_item_id;
  const uint8_t *created_bytes;
  size_t created_length;
  uint8_t *after;
  size_t after_length;
  char after_sha256[65];
} AbActWork;

typedef struct AbActContext {
  ArchbirdEngine *engine;
  const ArchbirdProject *project;
  const AbValue *map;
  const AbValue *metadata;
  AbActWork *works;
  size_t work_count;
  AbActEdit *edits;
  size_t edit_count;
  size_t total_after_bytes;
} AbActContext;

typedef struct AbActWorkOrder {
  size_t work_index;
  const AbString *output_path;
} AbActWorkOrder;

typedef struct AbDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} AbDigestWriter;

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  AbDigestWriter *writer = (AbDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static ArchbirdStatus act_error(ArchbirdEngine *engine, ArchbirdStatus code,
                                const char *message) {
  return archbird_error_set(engine, code, ARCHBIRD_NO_OFFSET, "act: %s",
                            message);
}

static ArchbirdStatus digest_bytes(const uint8_t *bytes, size_t length,
                                   char out[65]) {
  uint8_t digest[32];
  ArchbirdStatus status = archbird_sha256(bytes, length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  return status;
}

static ArchbirdStatus digest_json(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t length, char out[65]) {
  AbDigestWriter writer;
  uint8_t digest[32];
  ArchbirdStatus status;
  archbird_sha256_init(&writer.context);
  writer.status = ARCHBIRD_OK;
  status = archbird_json_canonicalize(engine, json, length, 0, digest_write,
                                      &writer);
  if (status == ARCHBIRD_OK)
    status = writer.status;
  if (status == ARCHBIRD_OK) {
    archbird_sha256_final(&writer.context, digest);
    archbird_sha256_hex(digest, out);
  }
  return status;
}

static const AbValue *object_field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static size_t project_source_index(const ArchbirdProject *project,
                                   const AbString *path) {
  const AbSourceManifest *manifest = ab_project_manifest(project);
  size_t low = 0;
  size_t high = manifest->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&manifest->files[middle].path, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return middle;
  }
  return SIZE_MAX;
}

static ArchbirdStatus source_work(AbActContext *context, const AbString *path,
                                  size_t *out_index) {
  const AbSourceManifest *manifest = ab_project_manifest(context->project);
  const AbValue *metadata;
  size_t source_index;
  size_t index;
  for (index = 0; index < context->work_count; index++)
    if (ab_string_equal(&context->works[index].path, path)) {
      *out_index = index;
      return ARCHBIRD_OK;
    }
  if (context->work_count >= AB_PATCH_MAX_WORK_ITEMS)
    return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "Plan touches too many files");
  source_index = project_source_index(context->project, path);
  if (source_index == SIZE_MAX)
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "Plan source path is outside the mapped project");
  metadata = ab_patch_source_file(context->metadata, path);
  if (!metadata)
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "Plan source path has no host metadata");
  {
    AbActWork *work = &context->works[context->work_count];
    char source_sha[65];
    archbird_sha256_hex(manifest->files[source_index].sha256, source_sha);
    if (memcmp(source_sha, object_field(metadata, "sha256")->as.text.data,
               64) != 0)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "host source metadata is stale");
    memset(work, 0, sizeof(*work));
    work->path = *path;
    work->before = ab_project_source_bytes(context->project, source_index);
    work->before_length = manifest->files[source_index].byte_length;
    memcpy(work->before_sha256, source_sha, sizeof(source_sha));
    work->before_exists = 1;
    work->executable = object_field(metadata, "executable")->as.boolean != 0;
    *out_index = context->work_count++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus absent_work(AbActContext *context, const AbString *path,
                                  size_t *out_index) {
  size_t index;
  for (index = 0; index < context->work_count; index++)
    if (ab_string_equal(&context->works[index].path, path)) {
      *out_index = index;
      return ARCHBIRD_OK;
    }
  if (!ab_patch_source_path_absent(context->metadata, path))
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "Plan destination was not proven absent");
  if (context->work_count >= AB_PATCH_MAX_WORK_ITEMS)
    return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "Plan touches too many files");
  memset(&context->works[context->work_count], 0,
         sizeof(context->works[context->work_count]));
  context->works[context->work_count].path = *path;
  *out_index = context->work_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_edit(AbActContext *context, size_t work_index,
                               const AbString *item_id, size_t start,
                               size_t end, const uint8_t *replacement,
                               size_t replacement_length,
                               uint8_t *owned_replacement) {
  AbActEdit *edit;
  if (context->edit_count >= AB_PATCH_MAX_WORK_ITEMS) {
    ab_free(context->engine, owned_replacement);
    return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "Plan contains too many exact edit sites");
  }
  edit = &context->edits[context->edit_count++];
  edit->work_index = work_index;
  edit->item_id = item_id;
  edit->start = start;
  edit->end = end;
  edit->replacement = replacement;
  edit->replacement_length = replacement_length;
  edit->owned_replacement = owned_replacement;
  return ARCHBIRD_OK;
}

static int edit_compare(const void *left, const void *right) {
  const AbActEdit *a = (const AbActEdit *)left;
  const AbActEdit *b = (const AbActEdit *)right;
  int compared;
  if (a->work_index != b->work_index)
    return (a->work_index > b->work_index) - (a->work_index < b->work_index);
  if (a->start != b->start)
    return (a->start > b->start) - (a->start < b->start);
  if (a->end != b->end)
    return (a->end > b->end) - (a->end < b->end);
  if (a->make_variable && b->make_variable) {
    compared = ab_string_compare(a->make_variable, b->make_variable);
    if (compared)
      return compared;
    compared = ab_string_compare(a->make_anchor, b->make_anchor);
    if (compared)
      return compared;
    if (a->make_position != b->make_position)
      return (a->make_position > b->make_position) -
             (a->make_position < b->make_position);
    compared = ab_string_compare(a->make_token, b->make_token);
    if (compared)
      return compared;
  } else if (a->make_variable || b->make_variable) {
    return a->make_variable ? 1 : -1;
  }
  return ab_string_compare(a->item_id, b->item_id);
}

static int make_insertions_compose(const AbActEdit *left,
                                   const AbActEdit *right) {
  return left->make_variable && right->make_variable &&
         ab_string_equal(left->make_variable, right->make_variable) &&
         ab_string_equal(left->make_anchor, right->make_anchor) &&
         left->make_position == right->make_position &&
         !ab_string_equal(left->make_token, right->make_token);
}

static int edits_overlap(const AbActEdit *left, const AbActEdit *right) {
  if (left->start == left->end) {
    if (right->start == right->end)
      return left->start == right->start &&
             !make_insertions_compose(left, right);
    return right->start < left->start && left->start < right->end;
  }
  if (right->start == right->end)
    return left->start < right->start && right->start < left->end;
  return left->start < right->end && right->start < left->end;
}

static ArchbirdStatus add_replace_range(AbActContext *context,
                                        const AbValue *operation,
                                        const AbString *item_id) {
  const AbValue *path = object_field(operation, "path");
  const AbValue *source_sha = object_field(operation, "source_sha256");
  const AbValue *start_value = object_field(operation, "start_byte");
  const AbValue *end_value = object_field(operation, "end_byte");
  const AbValue *before = object_field(operation, "before");
  const AbValue *replacement = object_field(operation, "replacement");
  uint64_t start;
  uint64_t end;
  size_t work_index;
  AbActWork *work;
  ArchbirdStatus status = source_work(context, &path->as.text, &work_index);
  if (status != ARCHBIRD_OK)
    return status;
  work = &context->works[work_index];
  if (memcmp(work->before_sha256, source_sha->as.text.data, 64) != 0)
    return act_error(context->engine, ARCHBIRD_CONFLICT,
                     "replace_range source lock is stale");
  if (!ab_artifact_safe_integer(start_value, &start) ||
      !ab_artifact_safe_integer(end_value, &end) || start > end ||
      end > work->before_length || before->as.text.length != end - start ||
      memcmp(work->before + (size_t)start, before->as.text.data,
             before->as.text.length) != 0)
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "replace_range does not match exact source bytes");
  status = ab_utf8_validate(context->engine, work->before, work->before_length);
  if (status == ARCHBIRD_OK)
    status = ab_utf8_validate(context->engine, work->before, (size_t)start);
  if (status == ARCHBIRD_OK)
    status = ab_utf8_validate(context->engine, work->before + (size_t)start,
                              (size_t)(end - start));
  if (status != ARCHBIRD_OK)
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "replace_range must align to UTF-8 byte boundaries");
  return add_edit(context, work_index, item_id, (size_t)start, (size_t)end,
                  (const uint8_t *)replacement->as.text.data,
                  replacement->as.text.length, NULL);
}

static ArchbirdStatus locked_source_work(AbActContext *context,
                                         const AbValue *operation,
                                         size_t *out_index) {
  const AbValue *path = object_field(operation, "path");
  const AbValue *source_sha = object_field(operation, "source_sha256");
  ArchbirdStatus status = source_work(context, &path->as.text, out_index);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(context->works[*out_index].before_sha256, source_sha->as.text.data,
             64) != 0)
    return act_error(context->engine, ARCHBIRD_CONFLICT,
                     "structured edit source lock is stale");
  return ARCHBIRD_OK;
}

static ArchbirdStatus take_edit_buffer(AbActContext *context, size_t work_index,
                                       const AbString *item_id, size_t start,
                                       size_t end, AbBuffer *replacement) {
  uint8_t *owned = replacement->data;
  ArchbirdStatus status = add_edit(context, work_index, item_id, start, end,
                                   owned, replacement->length, owned);
  if (status == ARCHBIRD_OK) {
    replacement->data = NULL;
    replacement->length = 0;
    replacement->capacity = 0;
  }
  return status;
}

static ArchbirdStatus
classify_make_insertion(AbActContext *context, size_t work_index,
                        const AbValue *variable, const AbValue *anchor,
                        const AbValue *token,
                        ArchbirdMakeVariableTokenPosition position) {
  AbActEdit *edit = &context->edits[context->edit_count - 1];
  size_t index;
  for (index = 0; index + 1 < context->edit_count; index++) {
    const AbActEdit *prior = &context->edits[index];
    if (prior->work_index == work_index && prior->make_variable &&
        ab_string_equal(prior->make_variable, &variable->as.text) &&
        ab_string_equal(prior->make_token, &token->as.text))
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "Plan inserts one Make variable token more than once");
  }
  edit->make_variable = &variable->as.text;
  edit->make_anchor = &anchor->as.text;
  edit->make_token = &token->as.text;
  edit->make_position = position;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_json_pointer_edit(AbActContext *context,
                                            const AbValue *operation,
                                            const AbString *item_id) {
  const AbValue *expected_absent = object_field(operation, "expected_absent");
  const AbValue *expected = object_field(operation, "expected");
  const AbValue *replacement_value = object_field(operation, "replacement");
  const AbValue *pointer = object_field(operation, "pointer");
  ArchbirdJsonPointerEditOptions options;
  ArchbirdJsonPointerEditResult result;
  AbBuffer expected_json;
  AbBuffer replacement_json;
  AbBuffer replacement;
  size_t work_index = 0;
  ArchbirdStatus status = locked_source_work(context, operation, &work_index);
  ab_buffer_init(&expected_json, context->engine);
  ab_buffer_init(&replacement_json, context->engine);
  ab_buffer_init(&replacement, context->engine);
  if (status == ARCHBIRD_OK && expected)
    status = ab_value_render(&expected_json, expected);
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&replacement_json, replacement_value);
  archbird_json_pointer_edit_options_init(&options);
  options.source_sha256 = context->works[work_index].before_sha256;
  options.source_sha256_length = 64;
  options.pointer = (const uint8_t *)pointer->as.text.data;
  options.pointer_length = pointer->as.text.length;
  options.expected_absent = expected_absent->as.boolean != 0;
  options.expected_json = expected ? expected_json.data : NULL;
  options.expected_json_length = expected ? expected_json.length : 0;
  options.replacement_json = replacement_json.data;
  options.replacement_json_length = replacement_json.length;
  archbird_json_pointer_edit_result_init(&result);
  if (status == ARCHBIRD_OK)
    status = archbird_json_pointer_edit(
        context->engine, context->works[work_index].before,
        context->works[work_index].before_length, &options, &result,
        buffer_write, &replacement);
  if (status == ARCHBIRD_OK)
    status = take_edit_buffer(context, work_index, item_id, result.start_byte,
                              result.end_byte, &replacement);
  ab_buffer_free(&replacement);
  ab_buffer_free(&replacement_json);
  ab_buffer_free(&expected_json);
  return status;
}

static ArchbirdStatus add_make_token_edit(AbActContext *context,
                                          const AbValue *operation,
                                          const AbString *item_id) {
  ArchbirdMakeVariableTokenEditOptions options;
  ArchbirdMakeVariableTokenEditResult result;
  const AbValue *variable = object_field(operation, "variable");
  const AbValue *expected = object_field(operation, "expected_token");
  const AbValue *replacement_token =
      object_field(operation, "replacement_token");
  AbBuffer replacement;
  size_t work_index = 0;
  ArchbirdStatus status = locked_source_work(context, operation, &work_index);
  ab_buffer_init(&replacement, context->engine);
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = context->works[work_index].before_sha256;
  options.source_sha256_length = 64;
  options.variable = (const uint8_t *)variable->as.text.data;
  options.variable_length = variable->as.text.length;
  options.expected_token = (const uint8_t *)expected->as.text.data;
  options.expected_token_length = expected->as.text.length;
  options.replacement_token = (const uint8_t *)replacement_token->as.text.data;
  options.replacement_token_length = replacement_token->as.text.length;
  archbird_make_variable_token_edit_result_init(&result);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_edit(
        context->engine, context->works[work_index].before,
        context->works[work_index].before_length, &options, &result,
        buffer_write, &replacement);
  if (status == ARCHBIRD_OK)
    status = take_edit_buffer(context, work_index, item_id, result.start_byte,
                              result.end_byte, &replacement);
  ab_buffer_free(&replacement);
  return status;
}

static ArchbirdStatus add_make_token_insert(AbActContext *context,
                                            const AbValue *operation,
                                            const AbString *item_id) {
  ArchbirdMakeVariableTokenInsertOptions options;
  ArchbirdMakeVariableTokenInsertResult result;
  const AbValue *variable = object_field(operation, "variable");
  const AbValue *token = object_field(operation, "token");
  const AbValue *anchor = object_field(operation, "anchor_token");
  const AbValue *position = object_field(operation, "position");
  AbBuffer replacement;
  size_t work_index = 0;
  ArchbirdStatus status = locked_source_work(context, operation, &work_index);
  ab_buffer_init(&replacement, context->engine);
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = context->works[work_index].before_sha256;
  options.source_sha256_length = 64;
  options.variable = (const uint8_t *)variable->as.text.data;
  options.variable_length = variable->as.text.length;
  options.token = (const uint8_t *)token->as.text.data;
  options.token_length = token->as.text.length;
  options.anchor_token = (const uint8_t *)anchor->as.text.data;
  options.anchor_token_length = anchor->as.text.length;
  options.position = ab_artifact_text_is(position, "before")
                         ? ARCHBIRD_MAKE_TOKEN_BEFORE
                         : ARCHBIRD_MAKE_TOKEN_AFTER;
  archbird_make_variable_token_insert_result_init(&result);
  if (status == ARCHBIRD_OK)
    status = archbird_make_variable_token_insert(
        context->engine, context->works[work_index].before,
        context->works[work_index].before_length, &options, &result,
        buffer_write, &replacement);
  if (status == ARCHBIRD_OK)
    status = take_edit_buffer(context, work_index, item_id, result.start_byte,
                              result.end_byte, &replacement);
  if (status == ARCHBIRD_OK)
    status = classify_make_insertion(context, work_index, variable, anchor,
                                     token, options.position);
  ab_buffer_free(&replacement);
  return status;
}

static const AbValue *projection_attribute(const AbProjectionItem *item,
                                           const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int projection_item_current(const AbProjectionItem *item) {
  return item->state.length == 7 && memcmp(item->state.data, "current", 7) == 0;
}

static int string_array_set_equal(const AbValue *left, const AbValue *right) {
  size_t left_index;
  size_t right_index;
  if (!left || left->kind != AB_VALUE_ARRAY)
    return right && right->kind == AB_VALUE_ARRAY && right->as.array.count == 0;
  if (!right || right->kind != AB_VALUE_ARRAY ||
      left->as.array.count != right->as.array.count)
    return 0;
  for (left_index = 0; left_index < left->as.array.count; left_index++) {
    int found = 0;
    for (right_index = 0; right_index < right->as.array.count; right_index++)
      if (ab_value_equal(&left->as.array.items[left_index],
                         &right->as.array.items[right_index])) {
        found = 1;
        break;
      }
    if (!found)
      return 0;
  }
  return 1;
}

static int symbol_leaf(const AbValue *symbol, AbString *out) {
  size_t start;
  size_t index;
  if (!symbol || symbol->kind != AB_VALUE_STRING || !symbol->as.text.length)
    return 0;
  start = symbol->as.text.length;
  while (start && (((unsigned char)symbol->as.text.data[start - 1] >= 'A' &&
                    (unsigned char)symbol->as.text.data[start - 1] <= 'Z') ||
                   ((unsigned char)symbol->as.text.data[start - 1] >= 'a' &&
                    (unsigned char)symbol->as.text.data[start - 1] <= 'z') ||
                   ((unsigned char)symbol->as.text.data[start - 1] >= '0' &&
                    (unsigned char)symbol->as.text.data[start - 1] <= '9') ||
                   symbol->as.text.data[start - 1] == '_'))
    start--;
  if (start == symbol->as.text.length)
    return 0;
  *out =
      (AbString){symbol->as.text.data + start, symbol->as.text.length - start};
  if (!((out->data[0] >= 'A' && out->data[0] <= 'Z') ||
        (out->data[0] >= 'a' && out->data[0] <= 'z') || out->data[0] == '_'))
    return 0;
  for (index = 1; index < out->length; index++)
    if (!((out->data[index] >= 'A' && out->data[index] <= 'Z') ||
          (out->data[index] >= 'a' && out->data[index] <= 'z') ||
          (out->data[index] >= '0' && out->data[index] <= '9') ||
          out->data[index] == '_'))
      return 0;
  return 1;
}

static int rename_site_matches(const AbValue *site,
                               const AbProjectionItem *item,
                               const AbString *leaf) {
  const AbValue *path = object_field(site, "path");
  const AbValue *sha = object_field(site, "source_sha256");
  const AbValue *before = object_field(site, "before");
  const AbValue *role = object_field(site, "role");
  const AbValue *actual_path = projection_attribute(item, "path");
  const AbValue *actual_sha = projection_attribute(item, "source_sha256");
  const AbValue *actual_role = projection_attribute(item, "role");
  uint64_t start;
  uint64_t end;
  uint64_t actual_start;
  uint64_t actual_end;
  return projection_item_current(item) &&
         before->as.text.length == leaf->length &&
         memcmp(before->as.text.data, leaf->data, leaf->length) == 0 &&
         actual_path && actual_path->kind == AB_VALUE_STRING &&
         ab_value_equal(path, actual_path) && actual_sha &&
         actual_sha->kind == AB_VALUE_STRING &&
         ab_value_equal(sha, actual_sha) && actual_role &&
         actual_role->kind == AB_VALUE_STRING &&
         ab_value_equal(role, actual_role) &&
         ab_artifact_safe_integer(object_field(site, "start_byte"), &start) &&
         ab_artifact_safe_integer(object_field(site, "end_byte"), &end) &&
         ab_artifact_safe_integer(projection_attribute(item, "start_byte"),
                                  &actual_start) &&
         ab_artifact_safe_integer(projection_attribute(item, "end_byte"),
                                  &actual_end) &&
         start == actual_start && end == actual_end &&
         string_array_set_equal(object_field(site, "fact_ids"),
                                projection_attribute(item, "fact_ids")) &&
         string_array_set_equal(object_field(site, "providers"),
                                projection_attribute(item, "providers"));
}

static int rename_coverage_matches(const AbValue *coverage,
                                   const AbProjectionData *data,
                                   size_t current_count) {
  const char *classification = ab_projection_data_classification(data);
  const AbValue *expected_classification =
      object_field(coverage, "classification");
  const AbValue *expected_exhaustive = object_field(coverage, "exhaustive");
  uint64_t selected;
  uint64_t unknown;
  uint64_t unsupported;
  if (!expected_classification ||
      expected_classification->kind != AB_VALUE_STRING ||
      expected_classification->as.text.length != strlen(classification) ||
      memcmp(expected_classification->as.text.data, classification,
             strlen(classification)) != 0 ||
      !expected_exhaustive || expected_exhaustive->kind != AB_VALUE_BOOL ||
      expected_exhaustive->as.boolean !=
          (strcmp(classification, "complete") == 0) ||
      !ab_artifact_safe_integer(object_field(coverage, "selected"),
                                &selected) ||
      !ab_artifact_safe_integer(object_field(coverage, "unknown"), &unknown) ||
      !ab_artifact_safe_integer(object_field(coverage, "unsupported"),
                                &unsupported))
    return 0;
  return selected == current_count && data->selection.has_unknown &&
         unknown == data->selection.unknown &&
         data->selection.has_unsupported &&
         unsupported == data->selection.unsupported;
}

static ArchbirdStatus add_rename_symbol(AbActContext *context,
                                        const AbValue *operation,
                                        const AbString *item_id) {
  const AbValue *definition = object_field(operation, "projection");
  const AbValue *sites = object_field(operation, "sites");
  const AbValue *new_name = object_field(operation, "new_name");
  const AbValue *coverage = object_field(operation, "coverage");
  AbProjectionPlan projection = {0};
  AbProjectionResult result = {0};
  AbString projection_id = {(char *)"act-symbol-occurrences", 22};
  AbString leaf = {0};
  size_t current_count = 0;
  size_t site_index;
  size_t item_index;
  ArchbirdStatus status;
  if (!symbol_leaf(object_field(operation, "symbol"), &leaf))
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "rename_symbol has no stable source leaf");
  status = ab_projection_plan_compile(context->engine, definition,
                                      &projection_id, &projection);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(context->engine, &projection,
                                         context->map, NULL, &result);
  if (status == ARCHBIRD_OK &&
      memcmp(result.result_sha256,
             object_field(operation, "projection_result_sha256")->as.text.data,
             64) != 0)
    status = act_error(context->engine, ARCHBIRD_CONFLICT,
                       "rename_symbol ProjectionResult is stale");
  for (item_index = 0;
       status == ARCHBIRD_OK && item_index < result.data.item_count;
       item_index++)
    if (projection_item_current(&result.data.items[item_index]))
      current_count++;
  if (status == ARCHBIRD_OK &&
      (current_count != sites->as.array.count ||
       !rename_coverage_matches(coverage, &result.data, current_count) ||
       strcmp(ab_projection_data_classification(&result.data), "complete") ||
       !object_field(coverage, "exhaustive")->as.boolean))
    status = act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                       "rename_symbol projection is not exhaustive");
  for (site_index = 0;
       status == ARCHBIRD_OK && site_index < sites->as.array.count;
       site_index++) {
    int found = 0;
    for (item_index = 0; item_index < result.data.item_count; item_index++)
      if (rename_site_matches(&sites->as.array.items[site_index],
                              &result.data.items[item_index], &leaf)) {
        found = 1;
        break;
      }
    if (!found)
      status = act_error(context->engine, ARCHBIRD_CONFLICT,
                         "rename_symbol sites differ from the current Map");
  }
  for (site_index = 0;
       status == ARCHBIRD_OK && site_index < sites->as.array.count;
       site_index++) {
    const AbValue *site = &sites->as.array.items[site_index];
    const AbValue *path = object_field(site, "path");
    const AbValue *source_sha = object_field(site, "source_sha256");
    const AbValue *before = object_field(site, "before");
    uint64_t start;
    uint64_t end;
    size_t work_index;
    AbActWork *work;
    status = source_work(context, &path->as.text, &work_index);
    if (status != ARCHBIRD_OK)
      break;
    work = &context->works[work_index];
    if (!ab_artifact_safe_integer(object_field(site, "start_byte"), &start) ||
        !ab_artifact_safe_integer(object_field(site, "end_byte"), &end) ||
        end > work->before_length ||
        memcmp(work->before_sha256, source_sha->as.text.data, 64) != 0 ||
        before->as.text.length != end - start ||
        memcmp(work->before + (size_t)start, before->as.text.data,
               before->as.text.length) != 0) {
      status = act_error(context->engine, ARCHBIRD_CONFLICT,
                         "rename_symbol source site is stale");
      break;
    }
    status = add_edit(context, work_index, item_id, (size_t)start, (size_t)end,
                      (const uint8_t *)new_name->as.text.data,
                      new_name->as.text.length, NULL);
  }
  ab_projection_result_free(context->engine, &result);
  ab_projection_plan_free(context->engine, &projection);
  return status;
}

static ArchbirdStatus collect_operation(AbActContext *context,
                                        const AbValue *item) {
  const AbValue *item_id = object_field(item, "id");
  const AbValue *operation = object_field(item, "operation");
  const AbValue *action = object_field(operation, "action");
  const AbValue *executable = object_field(item, "executable");
  const AbValue *path;
  const AbValue *source_sha;
  size_t work_index;
  ArchbirdStatus status;
  if (!executable->as.boolean || ab_artifact_text_is(action, "manual"))
    return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                     "Plan contains a manual or blocked item");
  if (ab_artifact_text_is(action, "replace_range"))
    return add_replace_range(context, operation, &item_id->as.text);
  if (ab_artifact_text_is(action, "edit_json_pointer"))
    return add_json_pointer_edit(context, operation, &item_id->as.text);
  if (ab_artifact_text_is(action, "edit_make_variable_token"))
    return add_make_token_edit(context, operation, &item_id->as.text);
  if (ab_artifact_text_is(action, "insert_make_variable_token"))
    return add_make_token_insert(context, operation, &item_id->as.text);
  if (ab_artifact_text_is(action, "rename_symbol"))
    return add_rename_symbol(context, operation, &item_id->as.text);
  if (ab_artifact_text_is(action, "create_file")) {
    const AbValue *content = object_field(operation, "content");
    path = object_field(operation, "path");
    status = absent_work(context, &path->as.text, &work_index);
    if (status != ARCHBIRD_OK)
      return status;
    if (context->works[work_index].create ||
        context->works[work_index].before_exists)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "create_file destination is claimed more than once");
    context->works[work_index].create = 1;
    context->works[work_index].action_item_id = &item_id->as.text;
    context->works[work_index].created_bytes =
        (const uint8_t *)content->as.text.data;
    context->works[work_index].created_length = content->as.text.length;
    context->works[work_index].executable = 0;
    return ARCHBIRD_OK;
  }
  if (ab_artifact_text_is(action, "delete_file")) {
    path = object_field(operation, "path");
    source_sha = object_field(operation, "source_sha256");
    status = source_work(context, &path->as.text, &work_index);
    if (status != ARCHBIRD_OK)
      return status;
    if (memcmp(context->works[work_index].before_sha256,
               source_sha->as.text.data, 64) != 0)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "delete_file source lock is stale");
    if (context->works[work_index].delete_file ||
        context->works[work_index].move)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "Plan consumes one source file more than once");
    context->works[work_index].delete_file = 1;
    context->works[work_index].action_item_id = &item_id->as.text;
    return ARCHBIRD_OK;
  }
  if (ab_artifact_text_is(action, "move_file")) {
    const AbValue *destination = object_field(operation, "destination_path");
    path = object_field(operation, "source_path");
    source_sha = object_field(operation, "source_sha256");
    status = source_work(context, &path->as.text, &work_index);
    if (status != ARCHBIRD_OK)
      return status;
    if (memcmp(context->works[work_index].before_sha256,
               source_sha->as.text.data, 64) != 0)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "move_file source lock is stale");
    if (!ab_patch_source_path_absent(context->metadata, &destination->as.text))
      return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                       "move_file destination was not proven absent");
    if (context->works[work_index].delete_file ||
        context->works[work_index].move)
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "Plan consumes one source file more than once");
    context->works[work_index].move = 1;
    context->works[work_index].destination = destination->as.text;
    context->works[work_index].action_item_id = &item_id->as.text;
    return ARCHBIRD_OK;
  }
  return act_error(context->engine, ARCHBIRD_POLICY_REJECTED,
                   "Plan operator is not native yet");
}

static ArchbirdStatus materialize_work(AbActContext *context,
                                       size_t work_index) {
  AbActWork *work = &context->works[work_index];
  size_t first = 0;
  size_t last = 0;
  size_t after_length;
  size_t source_cursor = 0;
  size_t output_cursor = 0;
  size_t index;
  if (work->create) {
    work->after_length = work->created_length;
    if (work->after_length) {
      work->after = (uint8_t *)ab_malloc(context->engine, work->after_length);
      if (!work->after)
        return act_error(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                         "out of memory materializing created file");
      memcpy(work->after, work->created_bytes, work->after_length);
    }
    return digest_bytes(work->after, work->after_length, work->after_sha256);
  }
  while (first < context->edit_count &&
         context->edits[first].work_index < work_index)
    first++;
  last = first;
  while (last < context->edit_count &&
         context->edits[last].work_index == work_index)
    last++;
  if (work->delete_file && first != last)
    return act_error(context->engine, ARCHBIRD_CONFLICT,
                     "a file cannot be edited and deleted in one Plan");
  if (work->delete_file)
    return ARCHBIRD_OK;
  after_length = work->before_length;
  for (index = first; index < last; index++) {
    const AbActEdit *edit = &context->edits[index];
    if (index > first && edits_overlap(&context->edits[index - 1], edit))
      return act_error(context->engine, ARCHBIRD_CONFLICT,
                       "Plan contains overlapping exact edits");
    if (edit->end - edit->start > after_length ||
        edit->replacement_length >
            AB_PATCH_MAX_FILE_BYTES -
                (after_length - (edit->end - edit->start)))
      return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                       "materialized file exceeds the Act limit");
    after_length =
        after_length - (edit->end - edit->start) + edit->replacement_length;
  }
  if (after_length > AB_PATCH_MAX_FILE_BYTES)
    return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "materialized file exceeds the Act limit");
  if (after_length) {
    work->after = (uint8_t *)ab_malloc(context->engine, after_length);
    if (!work->after)
      return act_error(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                       "out of memory materializing source edits");
  }
  for (index = first; index < last; index++) {
    const AbActEdit *edit = &context->edits[index];
    size_t unchanged = edit->start - source_cursor;
    if (unchanged)
      memcpy(work->after + output_cursor, work->before + source_cursor,
             unchanged);
    output_cursor += unchanged;
    if (edit->replacement_length)
      memcpy(work->after + output_cursor, edit->replacement,
             edit->replacement_length);
    output_cursor += edit->replacement_length;
    source_cursor = edit->end;
  }
  if (source_cursor < work->before_length) {
    size_t tail = work->before_length - source_cursor;
    memcpy(work->after + output_cursor, work->before + source_cursor, tail);
    output_cursor += tail;
  }
  work->after_length = after_length;
  if (output_cursor != after_length)
    return act_error(context->engine, ARCHBIRD_CONFLICT,
                     "materialized byte count is inconsistent");
  return digest_bytes(work->after, work->after_length, work->after_sha256);
}

static int work_order_compare(const void *left, const void *right) {
  const AbActWorkOrder *a = (const AbActWorkOrder *)left;
  const AbActWorkOrder *b = (const AbActWorkOrder *)right;
  return ab_string_compare(a->output_path, b->output_path);
}

static ArchbirdStatus
render_string_array(AbBuffer *buffer, const AbString **values, size_t count) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, values[index]->data,
                                     values[index]->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static int string_pointer_compare(const void *left, const void *right) {
  const AbString *const *a = (const AbString *const *)left;
  const AbString *const *b = (const AbString *const *)right;
  return ab_string_compare(*a, *b);
}

static ArchbirdStatus collect_constraint_ids(AbActContext *context,
                                             const AbPlan *plan,
                                             const AbString **values,
                                             size_t *out_count) {
  size_t count = 0;
  size_t item_index;
  size_t value_index;
  for (value_index = 0;
       value_index < plan->preserved_constraints->as.array.count;
       value_index++) {
    if (count == AB_PATCH_MAX_WORK_ITEMS)
      return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                       "Plan acceptance names too many constraints");
    values[count++] =
        &plan->preserved_constraints->as.array.items[value_index].as.text;
  }
  for (item_index = 0; item_index < plan->items->as.array.count; item_index++) {
    const AbValue *acceptance =
        object_field(&plan->items->as.array.items[item_index], "acceptance");
    const AbValue *constraints = object_field(acceptance, "constraints");
    for (value_index = 0; value_index < constraints->as.array.count;
         value_index++) {
      if (count == AB_PATCH_MAX_WORK_ITEMS)
        return act_error(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                         "Plan acceptance names too many constraints");
      values[count++] = &constraints->as.array.items[value_index].as.text;
    }
  }
  if (count > 1)
    qsort(values, count, sizeof(*values), string_pointer_compare);
  {
    size_t read;
    size_t write = 0;
    for (read = 0; read < count; read++)
      if (!write || !ab_string_equal(values[write - 1], values[read]))
        values[write++] = values[read];
    count = write;
  }
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_constraint_rows(AbBuffer *buffer,
                                             const AbString **values,
                                             size_t count,
                                             const char *status_text) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"id\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, values[index]->data,
                                     values[index]->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"status\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, status_text, strlen(status_text));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

static size_t work_item_ids(const AbActContext *context, size_t work_index,
                            const AbString **out) {
  const AbActWork *work = &context->works[work_index];
  size_t count = 0;
  size_t index;
  if (work->action_item_id)
    out[count++] = work->action_item_id;
  for (index = 0; index < context->edit_count; index++)
    if (context->edits[index].work_index == work_index)
      out[count++] = context->edits[index].item_id;
  if (count > 1)
    qsort(out, count, sizeof(*out), string_pointer_compare);
  {
    size_t output = 0;
    for (index = 0; index < count; index++)
      if (!output || !ab_string_equal(out[output - 1], out[index]))
        out[output++] = out[index];
    return output;
  }
}

static ArchbirdStatus render_file_state(AbBuffer *buffer, const char *sha256,
                                        size_t length, int executable,
                                        const uint8_t *content,
                                        int include_content) {
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"byte_length\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(buffer, (uint64_t)length);
  if (status == ARCHBIRD_OK && include_content)
    status = ab_buffer_literal(buffer, ",\"content_base64\":\"");
  if (status == ARCHBIRD_OK && include_content)
    status = ab_base64_encode(buffer, content, length);
  if (status == ARCHBIRD_OK && include_content)
    status = ab_buffer_literal(buffer, "\"");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"executable\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, executable ? "true" : "false");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_transition(AbBuffer *buffer,
                                        const AbActContext *context,
                                        size_t work_index) {
  const AbActWork *work = &context->works[work_index];
  const AbString *ids[AB_PATCH_MAX_WORK_ITEMS];
  const AbString *path = work->move ? &work->destination : &work->path;
  const char *kind = work->create        ? "create"
                     : work->delete_file ? "delete"
                     : work->move        ? "move"
                                         : "modify";
  size_t id_count = work_item_ids(context, work_index, ids);
  ArchbirdStatus status = ab_buffer_literal(buffer, "{\"after\":");
  if (status == ARCHBIRD_OK) {
    if (work->delete_file)
      status = ab_buffer_literal(buffer, "null");
    else
      status = render_file_state(buffer, work->after_sha256, work->after_length,
                                 work->executable, work->after, 1);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"before\":");
  if (status == ARCHBIRD_OK) {
    if (work->create)
      status = ab_buffer_literal(buffer, "null");
    else
      status =
          render_file_state(buffer, work->before_sha256, work->before_length,
                            work->executable, NULL, 0);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"item_ids\":");
  if (status == ARCHBIRD_OK)
    status = render_string_array(buffer, ids, id_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"kind\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, kind, strlen(kind));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(buffer, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, ",\"source_path\":");
  if (status == ARCHBIRD_OK) {
    if (work->move)
      status =
          ab_buffer_json_string(buffer, work->path.data, work->path.length);
    else
      status = ab_buffer_literal(buffer, "null");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "}");
  return status;
}

static ArchbirdStatus render_patch(AbActContext *context, const AbPlan *plan,
                                   const AbActWorkOrder *order,
                                   uint32_t json_flags,
                                   ArchbirdWriteFn write_fn, void *user_data) {
  AbBuffer document;
  const AbString *item_ids[AB_PATCH_MAX_WORK_ITEMS];
  const AbString *reads[AB_PATCH_MAX_WORK_ITEMS];
  const AbString *writes[AB_PATCH_MAX_WORK_ITEMS];
  const AbString *constraint_ids[AB_PATCH_MAX_WORK_ITEMS];
  size_t item_count = 0;
  size_t read_count = 0;
  size_t write_count = 0;
  size_t constraint_count = 0;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&document, context->engine);
  status =
      collect_constraint_ids(context, plan, constraint_ids, &constraint_count);
  for (index = 0; index < context->work_count; index++) {
    size_t work_index = order[index].work_index;
    const AbActWork *work = &context->works[work_index];
    const AbString *ids[AB_PATCH_MAX_WORK_ITEMS];
    size_t count = work_item_ids(context, work_index, ids);
    size_t id_index;
    for (id_index = 0; id_index < count; id_index++)
      item_ids[item_count++] = ids[id_index];
    if (work->before_exists)
      reads[read_count++] = &work->path;
    writes[write_count++] = work->move ? &work->destination : &work->path;
  }
  if (item_count > 1)
    qsort(item_ids, item_count, sizeof(*item_ids), string_pointer_compare);
  if (read_count > 1)
    qsort(reads, read_count, sizeof(*reads), string_pointer_compare);
  if (write_count > 1)
    qsort(writes, write_count, sizeof(*writes), string_pointer_compare);
#define UNIQUE_POINTERS(values, count)                                         \
  do {                                                                         \
    size_t _input;                                                             \
    size_t _output = 0;                                                        \
    for (_input = 0; _input < (count); _input++)                               \
      if (!_output ||                                                          \
          !ab_string_equal((values)[_output - 1], (values)[_input]))           \
        (values)[_output++] = (values)[_input];                                \
    (count) = _output;                                                         \
  } while (0)
  UNIQUE_POINTERS(item_ids, item_count);
  UNIQUE_POINTERS(reads, read_count);
  UNIQUE_POINTERS(writes, write_count);
#undef UNIQUE_POINTERS
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "{\"acceptance\":{\"constraints\":");
  if (status == ARCHBIRD_OK)
    status = render_constraint_rows(&document, constraint_ids, constraint_count,
                                    "not_evaluated");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &document,
        ",\"status\":\"not_evaluated\",\"verification_sha256\":null},"
        "\"after\":null,\"artifact\":\"patch\",\"executors\":[");
  if (status == ARCHBIRD_OK && item_count) {
    status = ab_buffer_literal(
        &document, "{\"capability\":\"archbird.native.plan-operators@1\","
                   "\"deterministic\":true,\"implementation_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&document,
                                     archbird_implementation_sha256(), 64);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&document, ",\"item_ids\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(&document, item_ids, item_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&document, ",\"matches\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&document, (uint64_t)item_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&document, ",\"reads\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(&document, reads, read_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(
          &document, ",\"skipped\":0,\"unsupported\":0,\"writes\":");
    if (status == ARCHBIRD_OK)
      status = render_string_array(&document, writes, write_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&document, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "],\"plan_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&document, plan->sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &document,
        ",\"provenance\":\"derived\",\"schema_version\":2,\"seal\":null,"
        "\"source\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&document, plan->source);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &document,
        ",\"state\":\"materialized\",\"tool\":{\"implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&document, archbird_implementation_sha256(), 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &document, ",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION
                   "\"},\"transitions\":[");
  for (index = 0; status == ARCHBIRD_OK && index < context->work_count;
       index++) {
    if (index)
      status = ab_buffer_literal(&document, ",");
    if (status == ARCHBIRD_OK)
      status = render_transition(&document, context, order[index].work_index);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&document, "]}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(context->engine, document.data,
                                        document.length, json_flags, write_fn,
                                        user_data);
  ab_buffer_free(&document);
  return status;
}

static ArchbirdStatus validate_source_binding(ArchbirdEngine *engine,
                                              const ArchbirdProject *project,
                                              const AbPlan *plan,
                                              const uint8_t *map_json,
                                              size_t map_length, AbValue *map) {
  const AbSourceManifest *manifest = ab_project_manifest(project);
  const AbValue *plan_map = object_field(plan->source, "map");
  const AbValue *map_project;
  const AbValue *evidence;
  const AbValue *tool;
  char map_sha[65];
  ArchbirdStatus status =
      ab_json_value_decode(engine, map_json, map_length, map);
  if (status != ARCHBIRD_OK)
    return status;
  map_project = object_field(map, "project");
  evidence = object_field(map, "evidence");
  tool = object_field(map, "tool");
  if (!ab_artifact_text_is(object_field(map, "artifact"), "map") ||
      !map_project || map_project->kind != AB_VALUE_STRING ||
      !ab_string_equal(&map_project->as.text, &manifest->project) ||
      !ab_string_equal(&object_field(plan->source, "project")->as.text,
                       &manifest->project) ||
      !evidence || !tool)
    return act_error(engine, ARCHBIRD_CONFLICT,
                     "Plan, Map, and source project identities differ");
  status = digest_json(engine, map_json, map_length, map_sha);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(map_sha, object_field(plan_map, "sha256")->as.text.data, 64) !=
          0 ||
      !ab_value_equal(object_field(plan_map, "input_sha256"),
                      object_field(evidence, "input_sha256")) ||
      !ab_value_equal(object_field(plan_map, "configuration_sha256"),
                      object_field(evidence, "config_sha256")) ||
      !ab_value_equal(object_field(plan_map, "producer_implementation_sha256"),
                      object_field(tool, "implementation_sha256")) ||
      memcmp(archbird_project_map_input_sha256(project),
             object_field(plan_map, "input_sha256")->as.text.data, 64) != 0)
    return act_error(engine, ARCHBIRD_CONFLICT,
                     "Plan source Map is stale or unrelated");
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_verification_binding(ArchbirdEngine *engine, const AbPlan *plan,
                              const AbVerificationArtifact *verification) {
  const AbValue *source = object_field(plan->source, "verification");
  const AbValue *result =
      object_field(&verification->root, "verification_result_sha256");
  if (!source || !result ||
      !ab_value_equal(object_field(source, "sha256"), result) ||
      !ab_value_equal(
          object_field(source, "policy_sha256"),
          object_field(verification->policy, "constraint_policy_sha256")) ||
      !ab_value_equal(
          object_field(source, "producer_implementation_sha256"),
          object_field(verification->tool, "implementation_sha256")) ||
      !ab_value_equal(object_field(plan->source, "project"),
                      object_field(verification->evaluation, "project")))
    return act_error(engine, ARCHBIRD_CONFLICT,
                     "Plan source Verification is stale or unrelated");
  return ARCHBIRD_OK;
}

ArchbirdStatus archbird_act_materialize_patch(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    const uint8_t *plan_json, size_t plan_length, const uint8_t *map_json,
    size_t map_length, const uint8_t *verification_json,
    size_t verification_length, const uint8_t *source_metadata_json,
    size_t source_metadata_length, uint32_t json_flags,
    ArchbirdWriteFn write_fn, void *user_data) {
  AbPlan plan;
  AbValue map = {0};
  AbValue metadata = {0};
  AbVerificationArtifact verification = {0};
  AbActContext context;
  AbActWorkOrder *order = NULL;
  ArchbirdStatus status;
  size_t index;
  if (!engine || !project || !plan_json || !plan_length || !map_json ||
      !map_length || !verification_json || !verification_length ||
      !source_metadata_json || !source_metadata_length || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&plan, 0, sizeof(plan));
  memset(&context, 0, sizeof(context));
  context.engine = engine;
  context.project = project;
  status = ab_plan_load(engine, plan_json, plan_length, &plan);
  if (status == ARCHBIRD_OK)
    status = validate_source_binding(engine, project, &plan, map_json,
                                     map_length, &map);
  if (status == ARCHBIRD_OK)
    status = ab_verification_artifact_load(engine, verification_json,
                                           verification_length, &verification);
  if (status == ARCHBIRD_OK)
    status = validate_verification_binding(engine, &plan, &verification);
  if (status == ARCHBIRD_OK)
    status = ab_patch_source_metadata_load(engine, source_metadata_json,
                                           source_metadata_length, &metadata);
  if (status == ARCHBIRD_OK) {
    context.map = &map;
    context.metadata = &metadata;
    context.works = (AbActWork *)ab_calloc(engine, AB_PATCH_MAX_WORK_ITEMS,
                                           sizeof(*context.works));
    context.edits = (AbActEdit *)ab_calloc(engine, AB_PATCH_MAX_WORK_ITEMS,
                                           sizeof(*context.edits));
    if (!context.works || !context.edits)
      status = act_error(engine, ARCHBIRD_OUT_OF_MEMORY,
                         "out of memory preparing Plan operators");
  }
  for (index = 0; status == ARCHBIRD_OK && index < plan.items->as.array.count;
       index++)
    status = collect_operation(&context, &plan.items->as.array.items[index]);
  if (status == ARCHBIRD_OK && context.edit_count > 1)
    qsort(context.edits, context.edit_count, sizeof(*context.edits),
          edit_compare);
  for (index = 0; status == ARCHBIRD_OK && index < context.work_count; index++)
    status = materialize_work(&context, index);
  if (status == ARCHBIRD_OK && context.work_count) {
    order =
        (AbActWorkOrder *)ab_calloc(engine, context.work_count, sizeof(*order));
    if (!order)
      status = act_error(engine, ARCHBIRD_OUT_OF_MEMORY,
                         "out of memory ordering Patch transitions");
  }
  for (index = 0; status == ARCHBIRD_OK && index < context.work_count;
       index++) {
    order[index].work_index = index;
    order[index].output_path = context.works[index].move
                                   ? &context.works[index].destination
                                   : &context.works[index].path;
  }
  if (status == ARCHBIRD_OK && context.work_count > 1)
    qsort(order, context.work_count, sizeof(*order), work_order_compare);
  for (index = 1; status == ARCHBIRD_OK && index < context.work_count;
       index++) {
    const AbString *previous = order[index - 1].output_path;
    const AbString *current = order[index].output_path;
    if (ab_string_equal(previous, current))
      status = act_error(engine, ARCHBIRD_CONFLICT,
                         "Plan produces one destination more than once");
  }
  for (index = 0; status == ARCHBIRD_OK && index < context.work_count;
       index++) {
    if (!context.works[index].delete_file) {
      if (context.works[index].after_length >
          AB_PATCH_MAX_SOURCE_BYTES - context.total_after_bytes)
        status = act_error(engine, ARCHBIRD_LIMIT_EXCEEDED,
                           "Patch after-source bytes exceed the Act limit");
      else
        context.total_after_bytes += context.works[index].after_length;
    }
  }
  if (status == ARCHBIRD_OK)
    status =
        render_patch(&context, &plan, order, json_flags, write_fn, user_data);
  for (index = 0; index < context.edit_count; index++)
    ab_free(engine, context.edits[index].owned_replacement);
  for (index = 0; index < context.work_count; index++)
    ab_free(engine, context.works[index].after);
  ab_free(engine, context.edits);
  ab_free(engine, context.works);
  ab_free(engine, order);
  ab_verification_artifact_free(&verification);
  ab_value_free(engine, &metadata);
  ab_value_free(engine, &map);
  ab_plan_free(engine, &plan);
  return status;
}
