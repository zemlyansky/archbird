#include "plan_compile_internal.h"

#include "artifact_validation.h"
#include "utf8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AB_PLAN_SYMBOL_MAX_REASONS 8u
#define AB_PLAN_SYMBOL_REASON_LENGTH 512u

typedef struct AbPlanReasons {
  char storage[AB_PLAN_SYMBOL_MAX_REASONS][AB_PLAN_SYMBOL_REASON_LENGTH];
  const char *values[AB_PLAN_SYMBOL_MAX_REASONS];
  size_t count;
} AbPlanReasons;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, ArchbirdStatus status,
                              const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "plan compilation: %s", message);
}

static ArchbirdStatus literal(AbBuffer *buffer, const char *value) {
  return ab_buffer_append(buffer, value, strlen(value));
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value ? value->data : "",
                               value ? value->length : 0);
}

static ArchbirdStatus json_cstring(AbBuffer *buffer, const char *value) {
  return ab_buffer_json_string(buffer, value, strlen(value));
}

static int string_is(const AbString *value, const char *literal_value) {
  size_t length = strlen(literal_value);
  return value && value->length == length &&
         memcmp(value->data, literal_value, length) == 0;
}

static int portable_identifier(const AbString *value) {
  size_t index;
  if (!value || !value->length || value->length > 256)
    return 0;
  for (index = 0; index < value->length; index++) {
    unsigned char byte = (unsigned char)value->data[index];
    if (index == 0) {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            byte == '_'))
        return 0;
    } else if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                 (byte >= '0' && byte <= '9') || byte == '_')) {
      return 0;
    }
  }
  return 1;
}

static int repository_path_without_pattern(const AbValue *value) {
  static const char pattern_bytes[] = "*?[]{}";
  size_t index;
  if (!ab_artifact_repository_path(value))
    return 0;
  for (index = 0; index < value->as.text.length; index++)
    if (memchr(pattern_bytes, value->as.text.data[index],
               sizeof(pattern_bytes) - 1))
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

static const AbValue *unique_definition_file(const AbValue *map,
                                             const AbString *name,
                                             const AbString *language,
                                             const AbString *excluded_path) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t file_index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (file_index = 0; file_index < files->as.array.count; file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = field(file, "path");
    const AbValue *file_language = field(file, "language");
    const AbValue *symbols = field(file, "symbols");
    size_t symbol_index;
    if (!repository_path_without_pattern(path) || !file_language ||
        file_language->kind != AB_VALUE_STRING ||
        !ab_string_equal(&file_language->as.text, language) ||
        (excluded_path && ab_string_equal(&path->as.text, excluded_path)) ||
        !symbols || symbols->kind != AB_VALUE_ARRAY)
      continue;
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *symbol_name = field(symbol, "name");
      if (!symbol_name || symbol_name->kind != AB_VALUE_STRING ||
          !ab_string_equal(&symbol_name->as.text, name) ||
          !ab_artifact_text_is(field(symbol, "kind"), "function") ||
          field(symbol, "syntax_recovery"))
        continue;
      if (match)
        return NULL;
      match = file;
    }
  }
  return match;
}

static int declaration_objective_scope(const AbValue *map, const AbString *path,
                                       const AbString *symbol,
                                       const AbValue **out_implementation_file,
                                       const char **out_reason) {
  const AbValue *target = map_file(map, path);
  const AbValue *language;
  const AbValue *symbols;
  size_t index;
  *out_implementation_file = NULL;
  *out_reason = NULL;
  if (!target) {
    *out_reason = "The declaration destination is not one exact mapped file.";
    return 0;
  }
  language = field(target, "language");
  if (!language || language->kind != AB_VALUE_STRING) {
    *out_reason = "The declaration destination has no mapped language.";
    return 0;
  }
  symbols = field(target, "symbols");
  if (!symbols || symbols->kind != AB_VALUE_ARRAY) {
    *out_reason =
        "The declaration destination has no complete mapped symbol ledger.";
    return 0;
  }
  for (index = 0; index < symbols->as.array.count; index++) {
    const AbValue *name = field(&symbols->as.array.items[index], "name");
    if (name && name->kind == AB_VALUE_STRING &&
        ab_string_equal(&name->as.text, symbol)) {
      *out_reason = "The declaration destination already contains the symbol.";
      return 0;
    }
  }
  *out_implementation_file =
      unique_definition_file(map, symbol, &language->as.text, path);
  if (!*out_implementation_file) {
    *out_reason =
        "No unique exact mapped definition establishes the declaration scope.";
    return 0;
  }
  return 1;
}

static AbString symbol_leaf(const AbString *symbol) {
  size_t start = symbol ? symbol->length : 0;
  while (start && symbol->data[start - 1] != '.' &&
         symbol->data[start - 1] != ':' && symbol->data[start - 1] != '/')
    start--;
  return (AbString){symbol->data + start, symbol->length - start};
}

static const AbValue *item_attribute(const AbProjectionItem *item,
                                     const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int projection_complete(const AbProjectionData *data,
                               const char *shape) {
  return data && string_is(&data->state, "current") &&
         string_is(&data->shape, shape) &&
         strcmp(ab_projection_data_classification(data), "complete") == 0 &&
         data->selection.has_truncated && !data->selection.truncated &&
         (!data->selection.has_unknown || !data->selection.unknown) &&
         (!data->selection.has_unsupported || !data->selection.unsupported);
}

static ArchbirdStatus reason_add(ArchbirdEngine *engine, AbPlanReasons *reasons,
                                 const char *format, ...) {
  va_list arguments;
  int length;
  if (reasons->count >= AB_PLAN_SYMBOL_MAX_REASONS)
    return invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                   "too many symbol planning reasons");
  va_start(arguments, format);
  length =
      vsnprintf(reasons->storage[reasons->count],
                sizeof(reasons->storage[reasons->count]), format, arguments);
  va_end(arguments);
  if (length < 0 || (size_t)length >= sizeof(reasons->storage[reasons->count]))
    return invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                   "symbol planning reason is too long");
  reasons->values[reasons->count] = reasons->storage[reasons->count];
  reasons->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
evaluate_occurrences(ArchbirdEngine *engine, const AbValue *map,
                     const AbString *symbol, const AbValue *seed_paths,
                     AbProjectionPlan *plan, AbProjectionResult *result) {
  AbBuffer definition_json;
  AbValue definition = {0};
  const AbValue *id;
  ArchbirdStatus status;
  ab_buffer_init(&definition_json, engine);
  status = literal(&definition_json,
                   "{\"id\":\"plan-symbol-occurrences\",\"names\":[");
  if (status == ARCHBIRD_OK)
    status = json_string(&definition_json, symbol);
  if (status == ARCHBIRD_OK)
    status = literal(&definition_json, "]");
  if (status == ARCHBIRD_OK && seed_paths) {
    status = literal(&definition_json, ",\"paths\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&definition_json, seed_paths);
  }
  if (status == ARCHBIRD_OK)
    status = literal(&definition_json, ",\"select\":\"symbol_occurrences\"}");
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, definition_json.data,
                                  definition_json.length, &definition);
  id = status == ARCHBIRD_OK ? field(&definition, "id") : NULL;
  if (status == ARCHBIRD_OK)
    status =
        ab_projection_plan_compile(engine, &definition, &id->as.text, plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(engine, plan, map, NULL, result);
  ab_value_free(engine, &definition);
  ab_buffer_free(&definition_json);
  return status;
}

typedef struct AbPlanPathRef {
  const AbValue *value;
} AbPlanPathRef;

static int path_ref_compare(const void *left, const void *right) {
  const AbPlanPathRef *a = (const AbPlanPathRef *)left;
  const AbPlanPathRef *b = (const AbPlanPathRef *)right;
  return ab_string_compare(&a->value->as.text, &b->value->as.text);
}

static ArchbirdStatus render_rename_operation(
    ArchbirdEngine *engine, const AbString *symbol, const AbString *new_name,
    const AbProjectionPlan *projection, const AbProjectionResult *occurrences,
    AbBuffer *operation, AbPlanReasons *reasons, size_t *out_path_count) {
  const AbProjectionData *fact = &occurrences->data;
  AbPlanPathRef *paths = NULL;
  size_t index;
  size_t path_count = 0;
  size_t invalid_count = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(operation, engine);
  if (fact->item_count) {
    paths =
        (AbPlanPathRef *)ab_calloc(engine, fact->item_count, sizeof(*paths));
    if (!paths)
      status = invalid(engine, ARCHBIRD_OUT_OF_MEMORY,
                       "out of memory collecting rename source scope");
  }
  for (index = 0; status == ARCHBIRD_OK && index < fact->item_count; index++) {
    const AbProjectionItem *item = &fact->items[index];
    const AbValue *path = item_attribute(item, "path");
    if (!string_is(&item->state, "current") ||
        !ab_artifact_repository_path(path)) {
      invalid_count++;
      continue;
    }
    paths[path_count++].value = path;
  }
  if (path_count > 1)
    qsort(paths, path_count, sizeof(*paths), path_ref_compare);
  if (path_count > 1) {
    size_t output = 1;
    for (index = 1; index < path_count; index++)
      if (!ab_value_equal(paths[output - 1].value, paths[index].value))
        paths[output++] = paths[index];
    path_count = output;
  }
  if (status == ARCHBIRD_OK)
    status = literal(operation, "{\"action\":\"rename_symbol\",\"new_name\":");
  if (status == ARCHBIRD_OK)
    status = json_string(operation, new_name);
  if (status == ARCHBIRD_OK)
    status = literal(operation, ",\"projection\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(operation, &projection->definition);
  if (status == ARCHBIRD_OK)
    status = literal(operation, ",\"projection_content_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(operation, fact->sha256, 64);
  if (status == ARCHBIRD_OK)
    status = literal(operation, ",\"projection_id\":");
  if (status == ARCHBIRD_OK)
    status = json_string(operation, &projection->id);
  if (status == ARCHBIRD_OK)
    status = literal(operation, ",\"source_paths\":[");
  for (index = 0; status == ARCHBIRD_OK && index < path_count; index++) {
    if (index)
      status = literal(operation, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(operation, paths[index].value);
  }
  if (status == ARCHBIRD_OK)
    status = literal(operation, "],\"symbol\":");
  if (status == ARCHBIRD_OK)
    status = json_string(operation, symbol);
  if (status == ARCHBIRD_OK)
    status = literal(operation, "}");
  if (status == ARCHBIRD_OK && invalid_count)
    status = reason_add(engine, reasons,
                        "%zu symbol occurrence(s) have no current repository "
                        "source scope.",
                        invalid_count);
  if (status == ARCHBIRD_OK && !projection_complete(fact, "set"))
    status = reason_add(
        engine, reasons,
        "Symbol occurrence evidence is not complete and exhaustive.");
  if (status == ARCHBIRD_OK && !path_count)
    status = reason_add(engine, reasons,
                        "No source paths are available for the rename.");
  *out_path_count = path_count;
  ab_free(engine, paths);
  return status;
}

static ArchbirdStatus manual_operation(ArchbirdEngine *engine,
                                       const AbString *path,
                                       const char *instructions,
                                       AbBuffer *operation) {
  ArchbirdStatus status;
  ab_buffer_init(operation, engine);
  status = literal(operation, "{\"action\":\"manual\",\"candidate_paths\":[");
  if (status == ARCHBIRD_OK && path)
    status = json_string(operation, path);
  if (status == ARCHBIRD_OK)
    status = literal(operation, "],\"instructions\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(operation, instructions);
  if (status == ARCHBIRD_OK)
    status = literal(operation, "}");
  return status;
}

static int evidence_has_path(const AbProjectionItem *item,
                             const AbString *path) {
  size_t index;
  for (index = 0; item && index < item->evidence_count; index++)
    if (ab_string_equal(&item->evidence[index].path, path))
      return 1;
  return 0;
}

static const AbProjectionItem *projection_item(const AbProjectionData *data,
                                               const AbString *key) {
  const AbProjectionItem *match = NULL;
  size_t index;
  for (index = 0; data && index < data->item_count; index++)
    if (ab_string_equal(&data->items[index].key, key)) {
      if (match)
        return NULL;
      match = &data->items[index];
    }
  return match;
}

static ArchbirdStatus
append_removal(ArchbirdEngine *engine, const ArchbirdProject *project,
               const AbValue *map, const AbProjectionData *actual,
               AbPlanItemBuilder *builder, const AbValue *constraint,
               const AbPlanFindingGroup *group) {
  const AbValue *finding = group->representative;
  const AbValue *map_files = field(map, "files");
  const AbValue *key = field(finding, "key");
  const AbProjectionItem *projected =
      key && key->kind == AB_VALUE_STRING
          ? projection_item(actual, &key->as.text)
          : NULL;
  const AbValue *candidate_file = NULL;
  const AbValue *candidate_symbol = NULL;
  const AbValue *candidate_path = NULL;
  AbProjectionPlan occurrence_plan = {0};
  AbProjectionResult occurrences = {0};
  AbPlanSourceLock lock = {0};
  AbPlanReasons reasons = {0};
  AbBuffer operation;
  AbPlanItemSpec spec;
  char statement[1024];
  size_t file_index;
  size_t occurrence_count = 0;
  int lock_ready = 0;
  int occurrences_ready = 0;
  uint64_t start = 0;
  uint64_t end = 0;
  int statement_length;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!key || key->kind != AB_VALUE_STRING)
    status =
        reason_add(engine, &reasons,
                   "Verification does not identify one unexpected symbol.");
  else if (!projected || !string_is(&projected->state, "current") ||
           !projected->evidence_count)
    status = reason_add(engine, &reasons,
                        "The unexpected symbol does not correlate to one "
                        "current projected item.");
  for (file_index = 0; status == ARCHBIRD_OK && map_files &&
                       file_index < map_files->as.array.count;
       file_index++) {
    const AbValue *file = &map_files->as.array.items[file_index];
    const AbValue *path = field(file, "path");
    const AbValue *symbols = field(file, "symbols");
    size_t symbol_index;
    if (!path || path->kind != AB_VALUE_STRING || !symbols ||
        symbols->kind != AB_VALUE_ARRAY ||
        (projected && !evidence_has_path(projected, &path->as.text)))
      continue;
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      if (!key || key->kind != AB_VALUE_STRING ||
          !ab_value_equal(field(symbol, "name"), key))
        continue;
      if (candidate_symbol) {
        status = reason_add(engine, &reasons,
                            "The unexpected symbol has multiple declaration "
                            "candidates in the Map.");
        break;
      }
      candidate_file = file;
      candidate_symbol = symbol;
      candidate_path = path;
    }
  }
  if (status == ARCHBIRD_OK && !candidate_symbol)
    status = reason_add(
        engine, &reasons,
        "The unexpected symbol has no unique source declaration in the Map.");
  if (status == ARCHBIRD_OK && key && key->kind == AB_VALUE_STRING) {
    status = evaluate_occurrences(engine, map, &key->as.text, NULL,
                                  &occurrence_plan, &occurrences);
    if (status == ARCHBIRD_OK)
      occurrences_ready = 1;
  }
  if (status == ARCHBIRD_OK && occurrences_ready &&
      !projection_complete(&occurrences.data, "set"))
    status =
        reason_add(engine, &reasons,
                   "Symbol relation evidence is not complete and exhaustive.");
  if (status == ARCHBIRD_OK && occurrences_ready) {
    size_t index;
    for (index = 0; index < occurrences.data.item_count; index++) {
      const AbProjectionItem *item = &occurrences.data.items[index];
      const AbValue *path = item_attribute(item, "path");
      const AbValue *role = item_attribute(item, "role");
      if (string_is(&item->state, "current") && candidate_path &&
          ab_value_equal(path, candidate_path) &&
          ab_artifact_text_is(role, "declaration"))
        occurrence_count++;
      else
        occurrence_count += 2;
    }
    if (occurrence_count != 1)
      status = reason_add(
          engine, &reasons,
          "Known imports, exports, bindings, references, or additional "
          "declarations require a reviewed rewrite before removal.");
  }
  if (status == ARCHBIRD_OK && candidate_symbol) {
    const AbValue *extent = field(candidate_symbol, "extent");
    const AbValue *syntax_recovery = field(candidate_symbol, "syntax_recovery");
    const AbValue *kind = field(candidate_symbol, "kind");
    if (!extent || extent->kind != AB_VALUE_OBJECT ||
        !ab_artifact_safe_integer(field(extent, "start"), &start) ||
        !ab_artifact_safe_integer(field(extent, "end"), &end) || start >= end)
      status = reason_add(engine, &reasons,
                          "The declaration has no exact removable extent.");
    if (status == ARCHBIRD_OK && syntax_recovery)
      status = reason_add(
          engine, &reasons,
          "The declaration was recovered from invalid or incomplete syntax.");
    if (status == ARCHBIRD_OK &&
        (memchr(key->as.text.data, '.', key->as.text.length) ||
         ab_artifact_text_is(kind, "method")))
      status = reason_add(engine, &reasons,
                          "Nested declarations and methods require a "
                          "contextual syntax rewrite.");
  }
  if (status == ARCHBIRD_OK && candidate_path &&
      candidate_path->kind == AB_VALUE_STRING) {
    status = ab_plan_source_lock(engine, project, map, &candidate_path->as.text,
                                 &lock);
    if (status == ARCHBIRD_OK)
      lock_ready = 1;
  }
  if (status == ARCHBIRD_OK && !reasons.count && !lock_ready)
    return invalid(engine, ARCHBIRD_CONFLICT,
                   "declaration source lock is unavailable");
  if (status == ARCHBIRD_OK && !reasons.count && lock_ready &&
      (end > lock.source.byte_length ||
       ab_utf8_validate(engine, lock.source.bytes + start,
                        (size_t)(end - start)) != ARCHBIRD_OK))
    return invalid(engine, ARCHBIRD_CONFLICT,
                   "declaration extent is not valid source text");
  ab_buffer_init(&operation, engine);
  if (status == ARCHBIRD_OK && !reasons.count && lock_ready) {
    status = literal(&operation, "{\"action\":\"replace_range\",\"before\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(
          &operation, (const char *)lock.source.bytes + (size_t)start,
          (size_t)(end - start));
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"end_byte\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&operation, end);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, candidate_path);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"replacement\":\"\",\"source_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, lock.sha256);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"start_byte\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&operation, start);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, "}");
  } else if (status == ARCHBIRD_OK) {
    status = manual_operation(
        engine,
        candidate_path && candidate_path->kind == AB_VALUE_STRING
            ? &candidate_path->as.text
            : NULL,
        "Remove the exact declaration only after all consumers and unresolved "
        "relation evidence have been addressed.",
        &operation);
  }
  statement_length = snprintf(
      statement, sizeof(statement), "Remove symbol %.*s%s%.*s.",
      key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
      key && key->kind == AB_VALUE_STRING ? key->as.text.data : "",
      candidate_path ? " from " : "",
      candidate_path ? (int)candidate_path->as.text.length : 0,
      candidate_path ? candidate_path->as.text.data : "");
  if (status == ARCHBIRD_OK &&
      (statement_length < 0 || (size_t)statement_length >= sizeof(statement)))
    status = invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "symbol removal statement is too long");
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = group->rows;
  spec.finding_count = group->count;
  spec.statement = statement;
  spec.provenance = "derived";
  spec.operation = &operation;
  spec.executable = reasons.count == 0;
  spec.reasons = reasons.values;
  spec.reason_count = reasons.count;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  ab_buffer_free(&operation);
  ab_projection_result_free(engine, &occurrences);
  ab_projection_plan_free(engine, &occurrence_plan);
  (void)candidate_file;
  return status;
}

static int rename_value(const AbValue *renames, const AbString *symbol,
                        const AbString **out, size_t *out_index) {
  size_t index;
  if (!renames || renames->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < renames->as.object.count; index++) {
    const AbObjectField *row = &renames->as.object.fields[index];
    if (!ab_string_equal(&row->name, symbol))
      continue;
    if (row->value.kind != AB_VALUE_STRING)
      return -1;
    *out = &row->value.as.text;
    *out_index = index;
    return 1;
  }
  return 0;
}

static ArchbirdStatus
append_rename(ArchbirdEngine *engine, const ArchbirdProject *project,
              const AbValue *map, AbPlanItemBuilder *builder,
              const AbValue *constraint, const AbValue *definition,
              const AbPlanFindingGroup *source_group,
              const AbPlanFindingGroup *target_group, const AbString *new_name,
              int asserted) {
  const AbValue *source_finding = source_group->representative;
  const AbValue *symbol_value = field(source_finding, "key");
  const AbString *symbol = &symbol_value->as.text;
  AbString leaf = symbol_leaf(symbol);
  AbProjectionPlan plan = {0};
  AbProjectionResult result = {0};
  AbPlanReasons reasons = {0};
  AbBuffer operation;
  const AbValue **findings = NULL;
  AbPlanItemSpec spec;
  char statement[1024];
  size_t path_count = 0;
  int statement_length;
  ArchbirdStatus status;
  const AbValue *seed_paths = field(definition, "paths");
  if (!portable_identifier(&leaf) || !portable_identifier(new_name) ||
      ab_string_equal(&leaf, new_name))
    return invalid(
        engine, ARCHBIRD_INVALID_SCHEMA,
        "rename directives require distinct portable identifier leaves");
  status =
      evaluate_occurrences(engine, map, symbol, seed_paths, &plan, &result);
  if (status == ARCHBIRD_OK)
    status = render_rename_operation(engine, symbol, new_name, &plan, &result,
                                     &operation, &reasons, &path_count);
  else
    ab_buffer_init(&operation, engine);
  if (status == ARCHBIRD_OK && !asserted)
    status = reason_add(
        engine, &reasons,
        "The extra and missing symbols suggest a rename, but automatic "
        "execution requires asserted rename intent.");
  if (status == ARCHBIRD_OK && !path_count) {
    ab_buffer_free(&operation);
    status = manual_operation(
        engine, NULL,
        "Establish complete exact symbol occurrences before renaming.",
        &operation);
  }
  if (status == ARCHBIRD_OK) {
    size_t index;
    size_t finding_count = source_group->count + target_group->count;
    findings =
        (const AbValue **)ab_calloc(engine, finding_count, sizeof(*findings));
    if (!findings)
      status = invalid(engine, ARCHBIRD_OUT_OF_MEMORY,
                       "out of memory combining rename findings");
    for (index = 0; findings && index < source_group->count; index++)
      findings[index] = source_group->rows[index];
    for (index = 0; findings && index < target_group->count; index++)
      findings[source_group->count + index] = target_group->rows[index];
  }
  statement_length = snprintf(
      statement, sizeof(statement), "%s rename from %.*s to %.*s.",
      asserted ? "Apply the reviewed" : "Review the inferred",
      (int)symbol->length, symbol->data, (int)new_name->length, new_name->data);
  if (status == ARCHBIRD_OK &&
      (statement_length < 0 || (size_t)statement_length >= sizeof(statement)))
    status = invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "rename statement is too long");
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = findings;
  spec.finding_count = source_group->count + target_group->count;
  spec.statement = statement;
  spec.provenance = asserted ? "asserted" : "derived";
  spec.operation = &operation;
  spec.executable = asserted && reasons.count == 0 && path_count != 0;
  spec.reasons = reasons.values;
  spec.reason_count = reasons.count;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  ab_free(engine, findings);
  ab_buffer_free(&operation);
  ab_projection_result_free(engine, &result);
  ab_projection_plan_free(engine, &plan);
  (void)project;
  return status;
}

static ArchbirdStatus append_manual_finding(ArchbirdEngine *engine,
                                            AbPlanItemBuilder *builder,
                                            const AbValue *constraint,
                                            const AbPlanFindingGroup *group,
                                            const char *reason) {
  const AbValue *finding = group->representative;
  const AbValue *key = field(finding, "key");
  const char *reasons[] = {reason};
  AbBuffer operation;
  AbPlanItemSpec spec;
  char statement[1024];
  int length;
  ArchbirdStatus status = manual_operation(
      engine, NULL,
      "Provide reviewed implementation or transformation semantics for this "
      "symbol obligation.",
      &operation);
  length = snprintf(
      statement, sizeof(statement), "Resolve symbol obligation %.*s.",
      key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
      key && key->kind == AB_VALUE_STRING ? key->as.text.data : "");
  if (status == ARCHBIRD_OK &&
      (length < 0 || (size_t)length >= sizeof(statement)))
    status = invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "symbol obligation statement is too long");
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = group->rows;
  spec.finding_count = group->count;
  spec.statement = statement;
  spec.provenance = "derived";
  spec.operation = &operation;
  spec.executable = 0;
  spec.reasons = reasons;
  spec.reason_count = 1;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  ab_buffer_free(&operation);
  return status;
}

static ArchbirdStatus append_required_symbol(ArchbirdEngine *engine,
                                             const AbValue *map,
                                             AbPlanItemBuilder *builder,
                                             const AbValue *constraint,
                                             const AbValue *definition,
                                             const AbPlanFindingGroup *group) {
  const AbValue *finding = group->representative;
  const AbValue *key = field(finding, "key");
  const AbValue *paths = field(definition, "paths");
  const AbValue *path =
      paths && paths->kind == AB_VALUE_ARRAY && paths->as.array.count == 1
          ? &paths->as.array.items[0]
          : NULL;
  const char *reason = NULL;
  const char *reasons[1];
  AbBuffer operation;
  AbPlanItemSpec spec;
  char statement[1024];
  int supported = 0;
  int length;
  ArchbirdStatus status = ARCHBIRD_OK;
  const AbValue *implementation_path = NULL;
  const AbValue *implementation_file = NULL;
  if (!key || key->kind != AB_VALUE_STRING ||
      !portable_identifier(&key->as.text))
    reason = "The required symbol is not one portable identifier.";
  else if (!repository_path_without_pattern(path))
    reason = "The required symbol projection does not name one exact file.";
  else
    supported = declaration_objective_scope(map, &path->as.text, &key->as.text,
                                            &implementation_file, &reason);
  if (supported) {
    implementation_path = field(implementation_file, "path");
    if (!repository_path_without_pattern(implementation_path)) {
      supported = 0;
      reason = "The required declaration has no exact mapped definition path.";
    }
  }
  ab_buffer_init(&operation, engine);
  if (supported) {
    const AbValue *first = path;
    const AbValue *second = implementation_path;
    if (ab_string_compare(&first->as.text, &second->as.text) > 0) {
      first = implementation_path;
      second = path;
    }
    status = literal(&operation, "{\"action\":\"declare_symbol\",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, path);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"symbol\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, key);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"source_paths\":[");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, first);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, second);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, "]}");
  } else {
    status = manual_operation(
        engine, path && path->kind == AB_VALUE_STRING ? &path->as.text : NULL,
        "Provide the reviewed declaration and exact destination.", &operation);
  }
  length = snprintf(
      statement, sizeof(statement),
      "%s required symbol declaration %.*s%s%.*s.",
      supported ? "Declare" : "Review",
      key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
      key && key->kind == AB_VALUE_STRING ? key->as.text.data : "",
      path && path->kind == AB_VALUE_STRING ? " in " : "",
      path && path->kind == AB_VALUE_STRING ? (int)path->as.text.length : 0,
      path && path->kind == AB_VALUE_STRING ? path->as.text.data : "");
  if (status == ARCHBIRD_OK &&
      (length < 0 || (size_t)length >= sizeof(statement)))
    status = invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "required declaration statement is too long");
  reasons[0] = reason;
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = group->rows;
  spec.finding_count = group->count;
  spec.statement = statement;
  spec.provenance = "derived";
  spec.operation = &operation;
  spec.executable = supported;
  spec.reasons = supported ? NULL : reasons;
  spec.reason_count = supported ? 0 : 1;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  ab_buffer_free(&operation);
  return status;
}

ArchbirdStatus ab_plan_compile_symbol_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbVerificationArtifact *verification, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbProjectionData *actual, const AbValue *renames,
    uint8_t *rename_used, int *out_handled) {
  const AbValue *select = field(definition, "select");
  const AbValue *assertion = field(constraint, "assert");
  const AbValue *operands = field(constraint, "operands");
  const AbValue *mapping = field(operands, "mapping");
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  const AbValue *expected_name = field(operands, "expected");
  const AbProjectionData *expected = NULL;
  const AbPlanFindingGroup *extra = NULL;
  const AbPlanFindingGroup *missing = NULL;
  size_t extra_count = 0;
  size_t missing_count = 0;
  size_t index;
  ArchbirdStatus status;
  *out_handled = 0;
  if (!ab_artifact_text_is(select, "symbols") ||
      (!ab_artifact_text_is(assertion, "disjoint") &&
       !ab_artifact_text_is(assertion, "required_subset") &&
       !ab_artifact_text_is(assertion, "set_equal") &&
       !ab_artifact_text_is(assertion, "subset")))
    return ARCHBIRD_OK;
  *out_handled = 1;
  status = ab_plan_finding_groups_collect(engine, findings, &groups);
  if (status != ARCHBIRD_OK)
    return status;
  if (mapping && mapping->kind == AB_VALUE_STRING && mapping->as.text.length) {
    for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++)
      status = append_manual_finding(engine, builder, constraint,
                                     &groups.groups[index],
                                     "Mapped symbol identities do not identify "
                                     "one removable declaration.");
    ab_plan_finding_groups_free(engine, &groups);
    return status;
  }
  if (expected_name && expected_name->kind == AB_VALUE_STRING)
    ab_verification_artifact_fact_value(verification, &expected_name->as.text,
                                        &expected);
  if (!projection_complete(actual, "set") ||
      !projection_complete(expected, "set")) {
    for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++)
      status = append_manual_finding(engine, builder, constraint,
                                     &groups.groups[index],
                                     "Symbol operands are not current, "
                                     "complete, exhaustive, and untruncated.");
    ab_plan_finding_groups_free(engine, &groups);
    return status;
  }
  if (ab_artifact_text_is(assertion, "required_subset")) {
    for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
      const AbValue *row = groups.groups[index].representative;
      if (ab_plan_finding_current(row) &&
          ab_artifact_text_is(field(row, "comparison"), "missing"))
        status = append_required_symbol(engine, map, builder, constraint,
                                        definition, &groups.groups[index]);
      else
        status = append_manual_finding(
            engine, builder, constraint, &groups.groups[index],
            "The required-symbol finding is not one current missing symbol.");
    }
    ab_plan_finding_groups_free(engine, &groups);
    return status;
  }
  for (index = 0; index < groups.count; index++) {
    const AbValue *row = groups.groups[index].representative;
    const AbValue *comparison = field(row, "comparison");
    if (ab_plan_finding_current(row) &&
        ab_artifact_text_is(comparison, "extra")) {
      extra = &groups.groups[index];
      extra_count++;
    } else if (ab_plan_finding_current(row) &&
               ab_artifact_text_is(comparison, "missing")) {
      missing = &groups.groups[index];
      missing_count++;
    }
  }
  if (extra_count == 1 && missing_count == 1) {
    const AbValue *old_value = field(extra->representative, "key");
    const AbValue *new_value = field(missing->representative, "key");
    const AbString *requested_name = NULL;
    size_t rename_index = 0;
    int requested = old_value && old_value->kind == AB_VALUE_STRING
                        ? rename_value(renames, &old_value->as.text,
                                       &requested_name, &rename_index)
                        : 0;
    if (requested < 0 ||
        (requested &&
         (!new_value || new_value->kind != AB_VALUE_STRING ||
          !ab_string_equal(requested_name, &new_value->as.text)))) {
      ab_plan_finding_groups_free(engine, &groups);
      return invalid(
          engine, ARCHBIRD_INVALID_SCHEMA,
          "asserted rename does not match one current extra/missing pair");
    }
    status = append_rename(
        engine, project, map, builder, constraint, definition, extra, missing,
        requested ? requested_name : &new_value->as.text, requested != 0);
    if (status == ARCHBIRD_OK && requested)
      rename_used[rename_index] = 1;
    ab_plan_finding_groups_free(engine, &groups);
    return status;
  }
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
    const AbValue *row = groups.groups[index].representative;
    const AbValue *comparison = field(row, "comparison");
    if (ab_plan_finding_current(row) &&
        (ab_artifact_text_is(comparison, "extra") ||
         ab_artifact_text_is(comparison, "overlap")))
      status = append_removal(engine, project, map, actual, builder, constraint,
                              &groups.groups[index]);
    else
      status = append_manual_finding(
          engine, builder, constraint, &groups.groups[index],
          ab_plan_finding_current(row)
              ? "The constraint identifies a symbol obligation but does not "
                "define its implementation."
              : "Finding evidence is not current executable evidence.");
  }
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
