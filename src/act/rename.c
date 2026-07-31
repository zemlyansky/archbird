#include "rename.h"

#include "act_source.h"
#include "artifact_validation.h"
#include "projection_internal.h"
#include "rename_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbActRenamePath {
  const AbString *value;
} AbActRenamePath;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static const AbValue *attribute(const AbProjectionItem *item,
                                const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         memcmp(value->data, literal, length) == 0;
}

static int value_is(const AbValue *value, const char *literal) {
  return value && value->kind == AB_VALUE_STRING &&
         string_is(&value->as.text, literal);
}

static ArchbirdStatus reject(AbActContext *context, ArchbirdStatus status,
                             const char *message) {
  return archbird_error_set(ab_act_executor_engine(context), status,
                            ARCHBIRD_NO_OFFSET, "act symbol rename: %s",
                            message);
}

static int projection_complete(const AbProjectionData *data) {
  return data && string_is(&data->state, "current") &&
         string_is(&data->shape, "set") &&
         strcmp(ab_projection_data_classification(data), "complete") == 0 &&
         data->selection.has_truncated && !data->selection.truncated &&
         (!data->selection.has_unknown || !data->selection.unknown) &&
         (!data->selection.has_unsupported || !data->selection.unsupported);
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

static const AbValue *map_file(const AbValue *map, const AbString *path) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *candidate = &files->as.array.items[index];
    const AbValue *candidate_path = field(candidate, "path");
    if (!candidate_path || candidate_path->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate_path->as.text, path))
      continue;
    if (match)
      return NULL;
    match = candidate;
  }
  return match;
}

static int path_compare(const void *left, const void *right) {
  const AbActRenamePath *a = (const AbActRenamePath *)left;
  const AbActRenamePath *b = (const AbActRenamePath *)right;
  return ab_string_compare(a->value, b->value);
}

static ArchbirdStatus validate_source_scope(AbActContext *context,
                                            const AbProjectionData *data,
                                            const AbValue *expected_paths) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  AbActRenamePath *paths = NULL;
  size_t path_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (data->item_count > AB_ACT_MAX_TRANSITIONS)
    return reject(context, ARCHBIRD_LIMIT_EXCEEDED,
                  "the occurrence projection exceeds the Act edit limit");
  if (data->item_count) {
    paths =
        (AbActRenamePath *)ab_calloc(engine, data->item_count, sizeof(*paths));
    if (!paths)
      return reject(context, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory validating rename source scope");
  }
  for (index = 0; index < data->item_count; index++) {
    const AbProjectionItem *item = &data->items[index];
    const AbValue *path = attribute(item, "path");
    if (!string_is(&item->state, "current") ||
        !ab_artifact_repository_path(path)) {
      status = reject(context, ARCHBIRD_POLICY_REJECTED,
                      "the occurrence projection contains a non-current or "
                      "unscoped item");
      break;
    }
    paths[path_count++].value = &path->as.text;
  }
  if (status == ARCHBIRD_OK && path_count > 1)
    qsort(paths, path_count, sizeof(*paths), path_compare);
  if (status == ARCHBIRD_OK && path_count > 1) {
    size_t output = 1;
    for (index = 1; index < path_count; index++)
      if (!ab_string_equal(paths[output - 1].value, paths[index].value))
        paths[output++] = paths[index];
    path_count = output;
  }
  if (status == ARCHBIRD_OK &&
      (!expected_paths || expected_paths->kind != AB_VALUE_ARRAY ||
       expected_paths->as.array.count != path_count))
    status =
        reject(context, ARCHBIRD_CONFLICT,
               "the Plan source scope differs from the current projection");
  for (index = 0; status == ARCHBIRD_OK && index < path_count; index++) {
    size_t expected_index;
    int found = 0;
    for (expected_index = 0; expected_index < expected_paths->as.array.count;
         expected_index++) {
      const AbValue *expected = &expected_paths->as.array.items[expected_index];
      if (expected->kind == AB_VALUE_STRING &&
          ab_string_equal(&expected->as.text, paths[index].value)) {
        found = 1;
        break;
      }
    }
    if (!found)
      status =
          reject(context, ARCHBIRD_CONFLICT,
                 "the Plan source scope differs from the current projection");
  }
  ab_free(engine, paths);
  return status;
}

static int string_array(const AbValue *value, int nonempty) {
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY ||
      (nonempty && !value->as.array.count))
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (value->as.array.items[index].kind != AB_VALUE_STRING ||
        !value->as.array.items[index].as.text.length)
      return 0;
  return 1;
}

static ArchbirdStatus ground_occurrence(AbActContext *context,
                                        const AbProjectionItem *item,
                                        const AbString *leaf,
                                        const AbString *new_name,
                                        const AbString *item_id) {
  const AbValue *path = attribute(item, "path");
  const AbValue *sha = attribute(item, "source_sha256");
  const AbValue *start_value = attribute(item, "start_byte");
  const AbValue *end_value = attribute(item, "end_byte");
  const AbValue *role = attribute(item, "role");
  const AbValue *fact_ids = attribute(item, "fact_ids");
  const AbValue *providers = attribute(item, "providers");
  const AbValue *file;
  const AbValue *file_sha;
  const AbValue *language;
  AbActRenameEvidence evidence;
  ArchbirdSourceView source;
  uint64_t start;
  uint64_t end;
  const char *reason = NULL;
  int supported = 0;
  ArchbirdStatus status;
  if (!path || path->kind != AB_VALUE_STRING || !ab_artifact_sha256(sha) ||
      !ab_artifact_safe_integer(start_value, &start) ||
      !ab_artifact_safe_integer(end_value, &end) || start >= end || !role ||
      role->kind != AB_VALUE_STRING || !string_array(fact_ids, 1) ||
      (providers && !string_array(providers, 0)))
    return reject(context, ARCHBIRD_POLICY_REJECTED,
                  "an occurrence has no exact typed source evidence");
  file = map_file(ab_act_executor_map(context), &path->as.text);
  file_sha = file ? field(file, "sha256") : NULL;
  if (!file || !ab_artifact_sha256(file_sha) || !ab_value_equal(file_sha, sha))
    return reject(context, ARCHBIRD_CONFLICT,
                  "an occurrence does not match one current mapped file");
  evidence.file = file;
  evidence.providers = providers;
  evidence.role = &role->as.text;
  language = field(file, "language");
  if (value_is(language, "python"))
    supported = ab_act_python_rename_evidence_supported(&evidence, &reason);
  else if (value_is(language, "javascript") ||
           value_is(language, "typescript") || value_is(language, "tsx"))
    supported = ab_act_ecmascript_rename_evidence_supported(&evidence, &reason);
  else
    reason = "no language executor supports the mapped source language";
  if (!supported)
    return reject(context, ARCHBIRD_POLICY_REJECTED,
                  reason ? reason
                         : "no language executor supports the occurrence");
  if (value_is(language, "python"))
    status = ab_act_executor_begin(context, item_id,
                                   "archbird.native.python.rename-symbol@1");
  else
    status = ab_act_executor_begin(
        context, item_id, "archbird.native.ecmascript.rename-symbol@1");
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_source(context, &path->as.text, &source);
  if (status == ARCHBIRD_OK &&
      (end > source.byte_length || end - start != leaf->length ||
       memcmp(source.bytes + (size_t)start, leaf->data, leaf->length) != 0))
    status = reject(context, ARCHBIRD_CONFLICT,
                    "an occurrence source span does not contain the old name");
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_replace_exact(
        context, item_id, &path->as.text, (size_t)start, (size_t)end,
        source.bytes + (size_t)start, (size_t)(end - start),
        (const uint8_t *)new_name->data, new_name->length);
  return status;
}

ArchbirdStatus ab_act_rename_symbol(AbActContext *context,
                                    const AbValue *operation,
                                    const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *definition = field(operation, "projection");
  const AbValue *projection_id = field(operation, "projection_id");
  const AbValue *expected_sha = field(operation, "projection_content_sha256");
  const AbValue *source_paths = field(operation, "source_paths");
  const AbValue *new_name = field(operation, "new_name");
  AbProjectionPlan projection = {0};
  AbProjectionResult result = {0};
  AbString leaf = {0};
  size_t index;
  ArchbirdStatus status;
  if (!symbol_leaf(field(operation, "symbol"), &leaf))
    return reject(context, ARCHBIRD_POLICY_REJECTED,
                  "the symbol has no stable source leaf");
  status = ab_projection_plan_compile(engine, definition,
                                      &projection_id->as.text, &projection);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        engine, &projection, ab_act_executor_map(context), NULL, &result);
  if (status == ARCHBIRD_OK &&
      memcmp(result.data.sha256, expected_sha->as.text.data, 64) != 0)
    status = reject(context, ARCHBIRD_CONFLICT,
                    "the occurrence projection differs from the Plan");
  if (status == ARCHBIRD_OK && !projection_complete(&result.data))
    status = reject(context, ARCHBIRD_POLICY_REJECTED,
                    "the occurrence projection is not complete and exhaustive");
  if (status == ARCHBIRD_OK)
    status = validate_source_scope(context, &result.data, source_paths);
  for (index = 0; status == ARCHBIRD_OK && index < result.data.item_count;
       index++)
    status = ground_occurrence(context, &result.data.items[index], &leaf,
                               &new_name->as.text, item_id);
  ab_projection_result_free(engine, &result);
  ab_projection_plan_free(engine, &projection);
  return status;
}
