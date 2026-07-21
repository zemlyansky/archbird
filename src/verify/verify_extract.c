#include "verify_runtime.h"

#include "map_internal.h"
#include "sha256.h"
#include "verify_checks.h"
#include "verify_membership.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *value) {
  return ab_string_copy(engine, out, value, strlen(value));
}

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const char *fact_shape(const AbValue *kind) {
  if (ab_verify_string_is(kind, "file_edges") ||
      ab_verify_string_is(kind, "component_edges") ||
      ab_verify_string_is(kind, "test_routes") ||
      ab_verify_string_is(kind, "literal_relation"))
    return "relation";
  if (ab_verify_string_is(kind, "python_enum") ||
      ab_verify_string_is(kind, "c_enum") ||
      ab_verify_string_is(kind, "c_designated_initializer") ||
      ab_verify_string_is(kind, "file_metrics") ||
      ab_verify_string_is(kind, "component_membership") ||
      ab_verify_string_is(kind, "provider_surface") ||
      ab_verify_string_is(kind, "literal_values"))
    return "values";
  return "set";
}

static int lowercase_sha256_value(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int valid_fact_state(const AbValue *value) {
  return ab_verify_string_is(value, "current") ||
         ab_verify_string_is(value, "stale") ||
         ab_verify_string_is(value, "unknown");
}

static int valid_fact_provenance(const AbValue *value) {
  return ab_verify_string_is(value, "derived") ||
         ab_verify_string_is(value, "asserted") ||
         ab_verify_string_is(value, "observed");
}

ArchbirdStatus ab_verify_item_init(ArchbirdEngine *engine,
                                   AbVerifyFactItem *item, const AbString *key,
                                   const AbString *label,
                                   const AbValue *value) {
  ArchbirdStatus status;
  memset(item, 0, sizeof(*item));
  status = ab_string_copy(engine, &item->key, key->data, key->length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &item->label, label->data, label->length);
  if (status == ARCHBIRD_OK) {
    if (value)
      status = ab_value_copy(engine, &item->value, value);
    else
      item->value.kind = AB_VALUE_NULL;
  }
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item->state, "current");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item->message, "");
  if (status != ARCHBIRD_OK)
    ab_verify_fact_item_free(engine, item);
  return status;
}

ArchbirdStatus ab_verify_item_add_evidence(ArchbirdEngine *engine,
                                           AbVerifyFactItem *item,
                                           const AbVerifyEvidence *source) {
  AbVerifyEvidence *rows;
  ArchbirdStatus status;
  if (item->evidence_count == item->evidence_capacity) {
    size_t capacity = item->evidence_capacity ? item->evidence_capacity * 2 : 1;
    if (capacity < item->evidence_capacity ||
        capacity > SIZE_MAX / sizeof(*rows))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too much verification evidence");
    rows = (AbVerifyEvidence *)ab_realloc(engine, item->evidence,
                                          capacity * sizeof(*rows));
    if (!rows)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification evidence");
    item->evidence = rows;
    item->evidence_capacity = capacity;
  }
  memset(&item->evidence[item->evidence_count], 0, sizeof(*item->evidence));
  status = ab_verify_evidence_init(
      engine, &item->evidence[item->evidence_count], source->provenance.data,
      &source->project, &source->path, source->line, source->sha256.data,
      source->detail.data, source->detail.length);
  if (status == ARCHBIRD_OK)
    item->evidence_count++;
  return status;
}

ArchbirdStatus ab_verify_item_set_state(ArchbirdEngine *engine,
                                        AbVerifyFactItem *item,
                                        const char *state,
                                        const char *message) {
  ArchbirdStatus status;
  if (!engine || !item || !state || !message)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_string_free(engine, &item->state);
  ab_string_free(engine, &item->message);
  status = copy_literal(engine, &item->state, state);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item->message, message);
  return status;
}

static ArchbirdStatus item_copy_attributes(ArchbirdEngine *engine,
                                           AbVerifyFactItem *item,
                                           const AbValue *attributes) {
  size_t index;
  if (!attributes || attributes->kind != AB_VALUE_OBJECT)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "provided fact attributes must be an object");
  item->attribute_count = attributes->as.object.count;
  if (item->attribute_count > SIZE_MAX / sizeof(*item->attributes))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "provided fact has too many attributes");
  if (item->attribute_count) {
    item->attributes = (AbObjectField *)ab_calloc(engine, item->attribute_count,
                                                  sizeof(*item->attributes));
    if (!item->attributes)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory copying fact attributes");
  }
  for (index = 0; index < item->attribute_count; index++) {
    ArchbirdStatus status =
        ab_string_copy(engine, &item->attributes[index].name,
                       attributes->as.object.fields[index].name.data,
                       attributes->as.object.fields[index].name.length);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &item->attributes[index].value,
                             &attributes->as.object.fields[index].value);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus item_add_string_attribute(ArchbirdEngine *engine,
                                                AbVerifyFactItem *item,
                                                const char *name,
                                                const AbString *value) {
  AbObjectField *fields;
  ArchbirdStatus status;
  if (item->attribute_count == SIZE_MAX / sizeof(*fields))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification fact attributes");
  fields = (AbObjectField *)ab_realloc(
      engine, item->attributes, (item->attribute_count + 1) * sizeof(*fields));
  if (!fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing fact attributes");
  item->attributes = fields;
  memset(&item->attributes[item->attribute_count], 0,
         sizeof(*item->attributes));
  status =
      copy_literal(engine, &item->attributes[item->attribute_count].name, name);
  if (status == ARCHBIRD_OK) {
    item->attributes[item->attribute_count].value.kind = AB_VALUE_STRING;
    status = ab_string_copy(
        engine, &item->attributes[item->attribute_count].value.as.text,
        value->data, value->length);
  }
  if (status == ARCHBIRD_OK)
    item->attribute_count++;
  return status;
}

static ArchbirdStatus
item_add_membership_components(ArchbirdEngine *engine, AbVerifyFactItem *item,
                               const AbVerifyMembershipIndex *index,
                               const AbVerifyMembershipFile *file) {
  AbObjectField *fields;
  AbObjectField *field;
  size_t assignment_index;
  ArchbirdStatus status;
  if (item->attribute_count == SIZE_MAX / sizeof(*fields))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification fact attributes");
  fields = (AbObjectField *)ab_realloc(
      engine, item->attributes, (item->attribute_count + 1) * sizeof(*fields));
  if (!fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing membership components");
  item->attributes = fields;
  field = &item->attributes[item->attribute_count];
  memset(field, 0, sizeof(*field));
  status = copy_literal(engine, &field->name, "components");
  if (status == ARCHBIRD_OK) {
    field->value.kind = AB_VALUE_ARRAY;
    field->value.as.array.count = file->assignment_count;
    if (file->assignment_count >
        SIZE_MAX / sizeof(*field->value.as.array.items))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "too many component memberships");
    else if (file->assignment_count) {
      field->value.as.array.items = (AbValue *)ab_calloc(
          engine, file->assignment_count, sizeof(*field->value.as.array.items));
      if (!field->value.as.array.items)
        status = archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory storing membership component names");
    }
  }
  for (assignment_index = 0;
       status == ARCHBIRD_OK && assignment_index < file->assignment_count;
       assignment_index++) {
    const AbVerifyMembershipAssignment *assignment =
        &index->assignments[file->assignment_start + assignment_index];
    const AbString *name = index->components[assignment->component_index].name;
    AbValue *value = &field->value.as.array.items[assignment_index];
    value->kind = AB_VALUE_STRING;
    status = ab_string_copy(engine, &value->as.text, name->data, name->length);
  }
  if (status == ARCHBIRD_OK)
    item->attribute_count++;
  else {
    ab_string_free(engine, &field->name);
    ab_value_free(engine, &field->value);
    memset(field, 0, sizeof(*field));
  }
  return status;
}

static ArchbirdStatus u64_value(ArchbirdEngine *engine, uint64_t number,
                                AbValue *out) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%" PRIu64, number);
  if (length < 0 || (size_t)length >= sizeof(text))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "failed to encode membership count");
  out->kind = AB_VALUE_INTEGER;
  return ab_string_copy(engine, &out->as.text, text, (size_t)length);
}

static ArchbirdStatus asserted_evidence(AbVerificationContext *context,
                                        const AbString *extractor,
                                        AbVerifyEvidence *out) {
  AbBuffer detail;
  AbString empty = {0};
  ArchbirdStatus status;
  ab_buffer_init(&detail, context->engine);
  status = ab_buffer_literal(&detail, "extractor ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, extractor->data, extractor->length);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evidence_init(context->engine, out, "asserted", &empty,
                                     &context->input.suite_path->as.text, 0,
                                     context->suite.sha256,
                                     (const char *)detail.data, detail.length);
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus relation_key(ArchbirdEngine *engine,
                                   const AbString *source, const AbString *kind,
                                   const AbString *target, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "{\"kind\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, kind->data, kind->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"source\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, source->data, source->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"target\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, target->data, target->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus relation_label(ArchbirdEngine *engine,
                                     const AbString *source,
                                     const AbString *kind,
                                     const AbString *target, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_append(&buffer, source->data, source->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, " -[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, kind->data, kind->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]-> ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, target->data, target->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static int string_starts(const AbString *value, const AbString *prefix) {
  return prefix->length <= value->length &&
         (!prefix->length ||
          memcmp(value->data, prefix->data, prefix->length) == 0);
}

static int string_ends(const AbString *value, const AbString *suffix) {
  return suffix->length <= value->length &&
         (!suffix->length ||
          memcmp(value->data + value->length - suffix->length, suffix->data,
                 suffix->length) == 0);
}

static int patterns_match(const AbValue *patterns, const AbString *value) {
  size_t index;
  if (!patterns || !patterns->as.array.count)
    return 0;
  for (index = 0; index < patterns->as.array.count; index++)
    if (ab_map_glob_match(&patterns->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

ArchbirdStatus ab_verify_normalized_name(ArchbirdEngine *engine,
                                         const AbValue *spec,
                                         const AbString *raw, AbString *out,
                                         int *selected) {
  const AbValue *prefix_value = ab_value_member(spec, "strip_prefix");
  const AbValue *suffix_value = ab_value_member(spec, "strip_suffix");
  const AbValue *include = ab_value_member(spec, "include");
  const AbValue *exclude = ab_value_member(spec, "exclude");
  const AbString empty = {0};
  const AbString *prefix = prefix_value ? &prefix_value->as.text : &empty;
  const AbString *suffix = suffix_value ? &suffix_value->as.text : &empty;
  AbString view = *raw;
  *selected = 0;
  memset(out, 0, sizeof(*out));
  if (prefix->length && string_starts(&view, prefix)) {
    view.data += prefix->length;
    view.length -= prefix->length;
  }
  if (suffix->length && string_ends(&view, suffix))
    view.length -= suffix->length;
  if (include && include->as.array.count && !patterns_match(include, &view))
    return ARCHBIRD_OK;
  if (exclude && patterns_match(exclude, &view))
    return ARCHBIRD_OK;
  *selected = 1;
  return ab_string_copy(engine, out, view.data, view.length);
}

static int path_matches(const AbString *path, const AbValue *patterns) {
  size_t index;
  if (!patterns || !patterns->as.array.count)
    return 1;
  for (index = 0; index < patterns->as.array.count; index++) {
    AbString pattern = patterns->as.array.items[index].as.text;
    int wildcard = 0;
    size_t cursor;
    while (pattern.length >= 2 && pattern.data[0] == '.' &&
           pattern.data[1] == '/') {
      pattern.data += 2;
      pattern.length -= 2;
    }
    while (pattern.length && pattern.data[pattern.length - 1] == '/')
      pattern.length--;
    for (cursor = 0; cursor < pattern.length; cursor++)
      if (pattern.data[cursor] == '*' || pattern.data[cursor] == '?' ||
          pattern.data[cursor] == '[') {
        wildcard = 1;
        break;
      }
    if ((wildcard && ab_map_glob_match(&pattern, path)) ||
        (!wildcard && (ab_string_equal(&pattern, path) ||
                       (path->length > pattern.length &&
                        memcmp(path->data, pattern.data, pattern.length) == 0 &&
                        path->data[pattern.length] == '/'))))
      return 1;
  }
  return 0;
}

static int string_array_has(const AbValue *array, const AbString *value) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++)
    if (array->as.array.items[index].kind == AB_VALUE_STRING &&
        ab_string_equal(&array->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

static const AbValue *project_map(AbVerificationContext *context,
                                  const AbValue *spec) {
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *project =
      project_name
          ? ab_verify_input_project(&context->input, &project_name->as.text)
          : NULL;
  return project ? ab_value_member(project, "map") : NULL;
}

static const AbValue *project_resolution(AbVerificationContext *context,
                                         const AbValue *spec) {
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *project =
      project_name
          ? ab_verify_input_project(&context->input, &project_name->as.text)
          : NULL;
  return project ? ab_value_member(project, "resolution") : NULL;
}

static const AbValue *map_file(const AbValue *map, const AbString *path) {
  const AbValue *files = ab_value_member(map, "files");
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbValue *candidate = ab_value_member(file, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return file;
  }
  return NULL;
}

static const char *file_sha(const AbValue *map, const AbString *path) {
  const AbValue *file = map_file(map, path);
  const AbValue *sha = file ? ab_value_member(file, "sha256") : NULL;
  return sha && sha->kind == AB_VALUE_STRING ? sha->as.text.data : "";
}

static ArchbirdStatus derived_evidence(ArchbirdEngine *engine,
                                       const AbString *project,
                                       const AbString *path, uint64_t line,
                                       const char *sha256, AbBuffer *detail,
                                       AbVerifyEvidence *out) {
  return ab_verify_evidence_init(engine, out, "derived", project, path, line,
                                 sha256, (const char *)detail->data,
                                 detail->length);
}

static AbVerifyFactItem *find_item(AbVerifyFactSet *fact, const AbString *key) {
  size_t index;
  for (index = 0; index < fact->item_count; index++)
    if (ab_string_equal(&fact->items[index].key, key))
      return &fact->items[index];
  return NULL;
}

static ArchbirdStatus add_relation_item_state(
    ArchbirdEngine *engine, AbVerifyFactSet *fact, const AbString *source,
    const AbString *kind, const AbString *target,
    const AbVerifyEvidence *evidence, const char *state, const char *message) {
  AbVerifyFactItem item = {0};
  AbVerifyFactItem *existing;
  ArchbirdStatus status = relation_key(engine, source, kind, target, &item.key);
  if (status == ARCHBIRD_OK)
    status = relation_label(engine, source, kind, target, &item.label);
  item.value.kind = AB_VALUE_NULL;
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item.state, state);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item.message, message);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(engine, &item, "kind", kind);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(engine, &item, "source", source);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(engine, &item, "target", target);
  if (status != ARCHBIRD_OK) {
    ab_verify_fact_item_free(engine, &item);
    return status;
  }
  existing = find_item(fact, &item.key);
  if (existing) {
    int existing_rank =
        string_literal(&existing->state, "current")
            ? 2
            : (string_literal(&existing->state, "unknown") ? 1 : 0);
    int incoming_rank = strcmp(state, "current") == 0
                            ? 2
                            : (strcmp(state, "unknown") == 0 ? 1 : 0);
    status = ab_verify_item_add_evidence(engine, existing, evidence);
    if (status == ARCHBIRD_OK && incoming_rank > existing_rank)
      status = ab_verify_item_set_state(engine, existing, state, message);
    ab_verify_fact_item_free(engine, &item);
    return status;
  }
  status = ab_verify_item_add_evidence(engine, &item, evidence);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_add_item(engine, fact, &item);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_item_free(engine, &item);
  return status;
}

static ArchbirdStatus
add_relation_item(ArchbirdEngine *engine, AbVerifyFactSet *fact,
                  const AbString *source, const AbString *kind,
                  const AbString *target, const AbVerifyEvidence *evidence) {
  return add_relation_item_state(engine, fact, source, kind, target, evidence,
                                 "current", "");
}

static int object_fields_allowed(const AbValue *object,
                                 const char *const *allowed, size_t count) {
  size_t field_index;
  size_t name_index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (field_index = 0; field_index < object->as.object.count; field_index++) {
    const AbString *name = &object->as.object.fields[field_index].name;
    int found = 0;
    for (name_index = 0; name_index < count; name_index++) {
      size_t length = strlen(allowed[name_index]);
      if (name->length == length &&
          (!length || memcmp(name->data, allowed[name_index], length) == 0)) {
        found = 1;
        break;
      }
    }
    if (!found)
      return 0;
  }
  return 1;
}

ArchbirdStatus ab_verify_evidence_decode_artifact(ArchbirdEngine *engine,
                                                  const AbValue *row,
                                                  AbVerifyEvidence *out) {
  static const char *const allowed[] = {
      "detail", "line", "path", "project", "provenance", "sha256",
  };
  const AbValue *provenance = ab_value_member(row, "provenance");
  const AbValue *project = ab_value_member(row, "project");
  const AbValue *path = ab_value_member(row, "path");
  const AbValue *line = ab_value_member(row, "line");
  const AbValue *sha = ab_value_member(row, "sha256");
  const AbValue *detail = ab_value_member(row, "detail");
  uint64_t line_number;
  if (!object_fields_allowed(row, allowed,
                             sizeof(allowed) / sizeof(allowed[0])) ||
      !valid_fact_provenance(provenance) || !project ||
      project->kind != AB_VALUE_STRING || !path ||
      path->kind != AB_VALUE_STRING || !ab_value_u64(line, &line_number) ||
      !sha || sha->kind != AB_VALUE_STRING ||
      (sha->as.text.length && !lowercase_sha256_value(sha)) || !detail ||
      detail->kind != AB_VALUE_STRING)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid verification fact evidence row");
  return ab_verify_evidence_init(engine, out, provenance->as.text.data,
                                 &project->as.text, &path->as.text, line_number,
                                 sha->as.text.data, detail->as.text.data,
                                 detail->as.text.length);
}

ArchbirdStatus ab_verify_fact_decode_artifact(ArchbirdEngine *engine,
                                              const AbValue *value,
                                              AbVerifyFactSet *out) {
  static const char *const envelope_allowed[] = {
      "items",      "message", "name",  "project",
      "provenance", "sha256",  "shape", "state",
  };
  static const char *const item_allowed[] = {
      "attributes", "evidence", "key", "label", "message", "state", "value",
  };
  const AbValue *name = ab_value_member(value, "name");
  const AbValue *shape = ab_value_member(value, "shape");
  const AbValue *provenance = ab_value_member(value, "provenance");
  const AbValue *project = ab_value_member(value, "project");
  const AbValue *state = ab_value_member(value, "state");
  const AbValue *message = ab_value_member(value, "message");
  const AbValue *sha = ab_value_member(value, "sha256");
  const AbValue *items = ab_value_member(value, "items");
  ArchbirdStatus status;
  size_t index;
  if (!engine || !value || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!object_fields_allowed(value, envelope_allowed,
                             sizeof(envelope_allowed) /
                                 sizeof(envelope_allowed[0])) ||
      !ab_verify_nonblank(name) || !ab_verify_nonblank(shape) ||
      !valid_fact_provenance(provenance) || !project ||
      project->kind != AB_VALUE_STRING || !valid_fact_state(state) ||
      !message || message->kind != AB_VALUE_STRING ||
      !lowercase_sha256_value(sha) || !items || items->kind != AB_VALUE_ARRAY)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid canonical verification fact set");
  status = ab_verify_fact_init(engine, out, &name->as.text, shape->as.text.data,
                               provenance->as.text.data, &project->as.text);
  if (status == ARCHBIRD_OK && !ab_value_string_is(state, "current")) {
    ab_string_free(engine, &out->state);
    ab_string_free(engine, &out->message);
    status = ab_string_copy(engine, &out->state, state->as.text.data,
                            state->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(engine, &out->message, message->as.text.data,
                              message->as.text.length);
  }
  for (index = 0; status == ARCHBIRD_OK && index < items->as.array.count;
       index++) {
    const AbValue *row = &items->as.array.items[index];
    const AbValue *key = ab_value_member(row, "key");
    const AbValue *label = ab_value_member(row, "label");
    const AbValue *item_value = ab_value_member(row, "value");
    const AbValue *attributes = ab_value_member(row, "attributes");
    const AbValue *item_state = ab_value_member(row, "state");
    const AbValue *item_message = ab_value_member(row, "message");
    const AbValue *evidence = ab_value_member(row, "evidence");
    AbVerifyFactItem item = {0};
    size_t previous;
    size_t evidence_index;
    if (!object_fields_allowed(row, item_allowed,
                               sizeof(item_allowed) /
                                   sizeof(item_allowed[0])) ||
        !ab_verify_nonblank(key) || !ab_verify_nonblank(label) || !item_value ||
        !attributes || attributes->kind != AB_VALUE_OBJECT ||
        !valid_fact_state(item_state) || !item_message ||
        item_message->kind != AB_VALUE_STRING || !evidence ||
        evidence->kind != AB_VALUE_ARRAY) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "invalid canonical verification fact item");
      break;
    }
    for (previous = 0; previous < out->item_count; previous++) {
      if (ab_string_equal(&out->items[previous].key, &key->as.text)) {
        status = archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "duplicate canonical verification fact key");
        break;
      }
    }
    if (status != ARCHBIRD_OK)
      break;
    status = ab_verify_item_init(engine, &item, &key->as.text, &label->as.text,
                                 item_value);
    if (status == ARCHBIRD_OK)
      status = item_copy_attributes(engine, &item, attributes);
    if (status == ARCHBIRD_OK && !ab_value_string_is(item_state, "current"))
      status = ab_verify_item_set_state(engine, &item, item_state->as.text.data,
                                        item_message->as.text.data);
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
         evidence_index++) {
      AbVerifyEvidence decoded = {0};
      status = ab_verify_evidence_decode_artifact(
          engine, &evidence->as.array.items[evidence_index], &decoded);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(engine, &item, &decoded);
      ab_verify_evidence_free(engine, &decoded);
    }
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_add_item(engine, out, &item);
    if (status != ARCHBIRD_OK)
      ab_verify_fact_item_free(engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(engine, out);
  if (status == ARCHBIRD_OK && memcmp(out->sha256, sha->as.text.data, 64) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "verification fact SHA-256 does not match content");
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(engine, out);
  return status;
}

static ArchbirdStatus current_source_sha(AbVerificationContext *context,
                                         const AbString *project_name,
                                         const AbString *path,
                                         char output[65]) {
  const AbValue *project =
      ab_verify_input_project(&context->input, project_name);
  const AbValue *source =
      project ? ab_verify_input_source(project, path) : NULL;
  const AbValue *text = source ? ab_value_member(source, "text") : NULL;
  uint8_t digest[32];
  ArchbirdStatus status;
  if (!text || text->kind != AB_VALUE_STRING)
    return archbird_error_set(
        context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "provided fact evidence path has no supplied source");
  status = archbird_sha256((const uint8_t *)text->as.text.data,
                           text->as.text.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  return status;
}

static ArchbirdStatus decode_provided_evidence(AbVerificationContext *context,
                                               const AbValue *row,
                                               const AbString *expected_project,
                                               AbVerifyEvidence *out) {
  static const char *const allowed[] = {
      "detail", "line", "path", "project", "provenance", "sha256",
  };
  const AbValue *provenance = ab_value_member(row, "provenance");
  const AbValue *project = ab_value_member(row, "project");
  const AbValue *path = ab_value_member(row, "path");
  const AbValue *line = ab_value_member(row, "line");
  const AbValue *sha = ab_value_member(row, "sha256");
  const AbValue *detail = ab_value_member(row, "detail");
  uint64_t line_number;
  char current[65];
  ArchbirdStatus status;
  if (!object_fields_allowed(row, allowed,
                             sizeof(allowed) / sizeof(allowed[0])) ||
      !ab_verify_string_is(provenance, "derived") || !project ||
      project->kind != AB_VALUE_STRING ||
      !ab_string_equal(&project->as.text, expected_project) ||
      !ab_verify_path_is_repository(path) ||
      !ab_value_u64(line, &line_number) || !lowercase_sha256_value(sha) ||
      !detail || detail->kind != AB_VALUE_STRING)
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid provided fact evidence row");
  status =
      current_source_sha(context, expected_project, &path->as.text, current);
  if (status == ARCHBIRD_OK && memcmp(current, sha->as.text.data, 64) != 0)
    status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "provided fact evidence is stale");
  if (status == ARCHBIRD_OK)
    status = ab_verify_evidence_init(
        context->engine, out, "derived", expected_project, &path->as.text,
        line_number, current, detail->as.text.data, detail->as.text.length);
  return status;
}

static ArchbirdStatus extract_provided(AbVerificationContext *context,
                                       const AbObjectField *extractor,
                                       const AbValue *provided,
                                       AbVerifyFactSet *fact) {
  static const char *const envelope_allowed[] = {
      "items", "message", "name", "producer", "project", "shape", "state",
  };
  static const char *const producer_allowed[] = {
      "implementation_sha256",
      "name",
      "runtime",
      "version",
  };
  static const char *const item_allowed[] = {
      "attributes", "evidence", "key", "label", "message", "state", "value",
  };
  const AbValue *kind = ab_value_member(&extractor->value, "kind");
  const AbValue *name = ab_value_member(provided, "name");
  const AbValue *project = ab_value_member(provided, "project");
  const AbValue *shape = ab_value_member(provided, "shape");
  const AbValue *state = ab_value_member(provided, "state");
  const AbValue *message = ab_value_member(provided, "message");
  const AbValue *producer = ab_value_member(provided, "producer");
  const AbValue *items = ab_value_member(provided, "items");
  const AbValue *producer_name =
      producer ? ab_value_member(producer, "name") : NULL;
  const AbValue *producer_version =
      producer ? ab_value_member(producer, "version") : NULL;
  const AbValue *producer_digest =
      producer ? ab_value_member(producer, "implementation_sha256") : NULL;
  size_t index;
  ArchbirdStatus status;
  if (!object_fields_allowed(provided, envelope_allowed,
                             sizeof(envelope_allowed) /
                                 sizeof(envelope_allowed[0])) ||
      !name || name->kind != AB_VALUE_STRING ||
      !ab_string_equal(&name->as.text, &extractor->name) || !project ||
      project->kind != AB_VALUE_STRING ||
      !ab_string_equal(
          &project->as.text,
          &ab_value_member(&extractor->value, "project")->as.text) ||
      !shape || !ab_verify_string_is(shape, fact_shape(kind)) ||
      !valid_fact_state(state) || !message ||
      message->kind != AB_VALUE_STRING || !items ||
      items->kind != AB_VALUE_ARRAY ||
      !object_fields_allowed(producer, producer_allowed,
                             sizeof(producer_allowed) /
                                 sizeof(producer_allowed[0])) ||
      !ab_verify_nonblank(producer_name) ||
      !ab_verify_nonblank(producer_version) ||
      !lowercase_sha256_value(producer_digest) ||
      (ab_value_member(producer, "runtime") &&
       !ab_verify_nonblank(ab_value_member(producer, "runtime"))))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid provided verification fact set");
  status = ab_verify_fact_init(context->engine, fact, &extractor->name,
                               fact_shape(kind), "derived", &project->as.text);
  if (status == ARCHBIRD_OK && !ab_verify_string_is(state, "current")) {
    ab_string_free(context->engine, &fact->state);
    ab_string_free(context->engine, &fact->message);
    status = ab_string_copy(context->engine, &fact->state, state->as.text.data,
                            state->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(context->engine, &fact->message,
                              message->as.text.data, message->as.text.length);
  }
  for (index = 0; status == ARCHBIRD_OK && index < items->as.array.count;
       index++) {
    const AbValue *row = &items->as.array.items[index];
    const AbValue *key = ab_value_member(row, "key");
    const AbValue *label = ab_value_member(row, "label");
    const AbValue *value = ab_value_member(row, "value");
    const AbValue *attributes = ab_value_member(row, "attributes");
    const AbValue *item_state = ab_value_member(row, "state");
    const AbValue *item_message = ab_value_member(row, "message");
    const AbValue *evidence = ab_value_member(row, "evidence");
    AbVerifyFactItem item = {0};
    size_t evidence_index;
    if (!object_fields_allowed(row, item_allowed,
                               sizeof(item_allowed) /
                                   sizeof(item_allowed[0])) ||
        !key || key->kind != AB_VALUE_STRING || !label ||
        label->kind != AB_VALUE_STRING || !value || !attributes ||
        !valid_fact_state(item_state) || !item_message ||
        item_message->kind != AB_VALUE_STRING || !evidence ||
        evidence->kind != AB_VALUE_ARRAY || !evidence->as.array.count) {
      status =
          archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                             ARCHBIRD_NO_OFFSET, "invalid provided fact item");
      break;
    }
    status = ab_verify_item_init(context->engine, &item, &key->as.text,
                                 &label->as.text, value);
    if (status == ARCHBIRD_OK)
      status = item_copy_attributes(context->engine, &item, attributes);
    if (status == ARCHBIRD_OK && !ab_verify_string_is(item_state, "current"))
      status = ab_verify_item_set_state(context->engine, &item,
                                        item_state->as.text.data,
                                        item_message->as.text.data);
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
         evidence_index++) {
      AbVerifyEvidence decoded = {0};
      status = decode_provided_evidence(
          context, &evidence->as.array.items[evidence_index], &project->as.text,
          &decoded);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &decoded);
      ab_verify_evidence_free(context->engine, &decoded);
    }
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_add_item(context->engine, fact, &item);
    if (status != ARCHBIRD_OK)
      ab_verify_fact_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_literal(AbVerificationContext *context,
                                      const AbObjectField *extractor,
                                      AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *kind = ab_value_member(spec, "kind");
  const AbValue *values = ab_value_member(spec, "values");
  const AbValue *rows = ab_value_member(spec, "rows");
  AbString empty = {0};
  AbVerifyEvidence evidence = {0};
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name,
                          fact_shape(kind), "asserted", &empty);
  size_t index;
  if (status == ARCHBIRD_OK)
    status = asserted_evidence(context, &extractor->name, &evidence);
  if (status == ARCHBIRD_OK && ab_verify_string_is(kind, "literal_set")) {
    for (index = 0; status == ARCHBIRD_OK && index < values->as.array.count;
         index++) {
      AbVerifyFactItem item = {0};
      status = ab_verify_item_init(
          context->engine, &item, &values->as.array.items[index].as.text,
          &values->as.array.items[index].as.text, NULL);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_fact_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_verify_fact_item_free(context->engine, &item);
    }
  } else if (status == ARCHBIRD_OK &&
             ab_verify_string_is(kind, "literal_values")) {
    for (index = 0; status == ARCHBIRD_OK && index < values->as.object.count;
         index++) {
      const AbObjectField *row = &values->as.object.fields[index];
      AbVerifyFactItem item = {0};
      status = ab_verify_item_init(context->engine, &item, &row->name,
                                   &row->name, &row->value);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_fact_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_verify_fact_item_free(context->engine, &item);
    }
  } else if (status == ARCHBIRD_OK) {
    static const AbString star = {(char *)"*", 1};
    for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
         index++) {
      const AbValue *row = &rows->as.array.items[index];
      const AbValue *source = ab_value_member(row, "source");
      const AbValue *target = ab_value_member(row, "target");
      const AbValue *kind_value = ab_value_member(row, "kind");
      const AbString *relation_kind = kind_value ? &kind_value->as.text : &star;
      AbVerifyFactItem item = {0};
      status = relation_key(context->engine, &source->as.text, relation_kind,
                            &target->as.text, &item.key);
      if (status == ARCHBIRD_OK)
        status = relation_label(context->engine, &source->as.text,
                                relation_kind, &target->as.text, &item.label);
      item.value.kind = AB_VALUE_NULL;
      if (status == ARCHBIRD_OK)
        status = copy_literal(context->engine, &item.state, "current");
      if (status == ARCHBIRD_OK)
        status = copy_literal(context->engine, &item.message, "");
      if (status == ARCHBIRD_OK)
        status = item_add_string_attribute(context->engine, &item, "kind",
                                           relation_kind);
      if (status == ARCHBIRD_OK)
        status = item_add_string_attribute(context->engine, &item, "source",
                                           &source->as.text);
      if (status == ARCHBIRD_OK)
        status = item_add_string_attribute(context->engine, &item, "target",
                                           &target->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_fact_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_verify_fact_item_free(context->engine, &item);
    }
  }
  ab_verify_evidence_free(context->engine, &evidence);
  if (status == ARCHBIRD_OK) {
    size_t universe =
        ab_verify_string_is(kind, "literal_relation") ? rows->as.array.count
        : ab_verify_string_is(kind, "literal_values") ? values->as.object.count
                                                      : values->as.array.count;
    if (fact->item_count > universe) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "literal extractor produced too many facts");
    } else {
      status = ab_verify_fact_selection_exact(
          context->engine, fact,
          ab_verify_string_is(kind, "literal_relation") ? "relation" : "value",
          (uint64_t)universe, (uint64_t)fact->item_count,
          (uint64_t)(universe - fact->item_count), 0, 0);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_symbols(AbVerificationContext *context,
                                      const AbObjectField *extractor,
                                      AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *layer = ab_value_member(spec, "layer");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *kinds = ab_value_member(spec, "kinds");
  const AbValue *public_only = ab_value_member(spec, "public_only");
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "set",
                          "derived", &project_name->as.text);
  size_t file_index;
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "set",
                                  "project map has no file inventory");
  }
  for (file_index = 0;
       status == ARCHBIRD_OK && file_index < files->as.array.count;
       file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *file_layer = ab_value_member(file, "layer");
    const AbValue *sha = ab_value_member(file, "sha256");
    const AbValue *symbols = ab_value_member(file, "symbols");
    size_t symbol_index;
    if (!path || path->kind != AB_VALUE_STRING || !file_layer ||
        file_layer->kind != AB_VALUE_STRING || !sha ||
        sha->kind != AB_VALUE_STRING || !symbols ||
        symbols->kind != AB_VALUE_ARRAY)
      continue;
    if (layer && !ab_string_equal(&layer->as.text, &file_layer->as.text))
      continue;
    if (!path_matches(&path->as.text, paths))
      continue;
    for (symbol_index = 0;
         status == ARCHBIRD_OK && symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *name = ab_value_member(symbol, "name");
      const AbValue *kind = ab_value_member(symbol, "kind");
      const AbValue *scope = ab_value_member(symbol, "scope");
      const AbValue *line = ab_value_member(symbol, "line");
      uint64_t line_number = 0;
      AbString normalized = {0};
      int selected = 0;
      AbVerifyFactItem item = {0};
      AbVerifyEvidence evidence = {0};
      AbBuffer detail;
      if (!name || name->kind != AB_VALUE_STRING || !kind ||
          kind->kind != AB_VALUE_STRING || !scope ||
          scope->kind != AB_VALUE_STRING ||
          (line && !ab_value_u64(line, &line_number)))
        continue;
      if (kinds && !string_array_has(kinds, &kind->as.text))
        continue;
      if (public_only && public_only->as.boolean &&
          !ab_verify_string_is(scope, "public"))
        continue;
      status = ab_verify_normalized_name(context->engine, spec, &name->as.text,
                                         &normalized, &selected);
      if (status != ARCHBIRD_OK || !selected) {
        ab_string_free(context->engine, &normalized);
        continue;
      }
      status = ab_verify_item_init(context->engine, &item, &normalized,
                                   &normalized, NULL);
      ab_buffer_init(&detail, context->engine);
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&detail, kind->as.text.data, kind->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, " ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&detail, name->as.text.data, name->as.text.length);
      if (status == ARCHBIRD_OK)
        status = derived_evidence(context->engine, &project_name->as.text,
                                  &path->as.text, line_number,
                                  sha->as.text.data, &detail, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_fact_add_item(context->engine, fact, &item);
      ab_buffer_free(&detail);
      ab_verify_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      if (status != ARCHBIRD_OK)
        ab_verify_fact_item_free(context->engine, &item);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_file_metrics(AbVerificationContext *context,
                                           const AbObjectField *extractor,
                                           AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *resolution = project_resolution(context, spec);
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *discovery = map ? ab_value_member(map, "discovery") : NULL;
  const AbValue *coverage =
      discovery ? ab_value_member(discovery, "coverage") : NULL;
  const AbValue *oversized =
      coverage ? ab_value_member(coverage, "oversized") : NULL;
  const AbValue *metric = ab_value_member(spec, "metric");
  size_t file_index;
  uint64_t source_oversized_count = 0;
  uint64_t oversized_count = 0;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "values",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "values",
                                  "project map has no file inventory");
  }
  if (!oversized || !ab_value_u64(oversized, &oversized_count)) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(
        context->engine, fact, &extractor->name, &project_name->as.text,
        "values", "project map has no valid oversized-file coverage");
  }
  if (oversized_count && !resolution) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(
        context->engine, fact, &extractor->name, &project_name->as.text,
        "values",
        "project discovery omitted oversized files; file metrics are "
        "incomplete");
  }
  for (file_index = 0; file_index < files->as.array.count; file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *value = ab_value_member(file, "bytes");
    const AbValue *sha = ab_value_member(file, "sha256");
    uint64_t number;
    if (!ab_verify_path_is_repository(path) || !value ||
        !ab_value_u64(value, &number) || !lowercase_sha256_value(sha)) {
      ab_verify_fact_free(context->engine, fact);
      return ab_verify_fact_unknown(
          context->engine, fact, &extractor->name, &project_name->as.text,
          "values", "project map has an invalid file metric inventory");
    }
  }
  for (file_index = 0;
       status == ARCHBIRD_OK && file_index < files->as.array.count;
       file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *value = ab_value_member(file, "bytes");
    const AbValue *sha = ab_value_member(file, "sha256");
    const AbValue *layer = ab_value_member(file, "layer");
    const AbValue *language = ab_value_member(file, "language");
    AbString normalized = {0};
    int selected = 0;
    AbVerifyFactItem item = {0};
    AbVerifyEvidence evidence = {0};
    AbBuffer detail;
    status = ab_verify_normalized_name(context->engine, spec, &path->as.text,
                                       &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status = ab_verify_item_init(context->engine, &item, &normalized,
                                 &path->as.text, value);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "metric",
                                         &metric->as.text);
    if (status == ARCHBIRD_OK && layer && layer->kind == AB_VALUE_STRING)
      status = item_add_string_attribute(context->engine, &item, "layer",
                                         &layer->as.text);
    if (status == ARCHBIRD_OK && language && language->kind == AB_VALUE_STRING)
      status = item_add_string_attribute(context->engine, &item, "language",
                                         &language->as.text);
    ab_buffer_init(&detail, context->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&detail, metric->as.text.data,
                                metric->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, "=");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&detail, value);
    if (status == ARCHBIRD_OK)
      status = derived_evidence(context->engine, &project_name->as.text,
                                &path->as.text, 0, sha->as.text.data, &detail,
                                &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_verify_evidence_free(context->engine, &evidence);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_verify_fact_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK && oversized_count) {
    const AbValue *diagnostics = ab_value_member(resolution, "diagnostics");
    const AbValue *resolution_sha = ab_value_member(resolution, "sha256");
    size_t diagnostic_index;
    for (diagnostic_index = 0; status == ARCHBIRD_OK &&
                               diagnostic_index < diagnostics->as.array.count;
         diagnostic_index++) {
      const AbValue *diagnostic =
          &diagnostics->as.array.items[diagnostic_index];
      const AbValue *code = ab_value_member(diagnostic, "code");
      const AbValue *path = ab_value_member(diagnostic, "path");
      const AbValue *value = ab_value_member(diagnostic, "bytes");
      AbString normalized = {0};
      int selected = 0;
      AbVerifyFactItem item = {0};
      AbVerifyEvidence evidence = {0};
      AbBuffer detail;
      if (!ab_verify_string_is(code, "discovery-file-oversized"))
        continue;
      source_oversized_count++;
      status = ab_verify_normalized_name(context->engine, spec, &path->as.text,
                                         &normalized, &selected);
      if (status != ARCHBIRD_OK || !selected) {
        ab_string_free(context->engine, &normalized);
        continue;
      }
      status = ab_verify_item_init(context->engine, &item, &normalized,
                                   &path->as.text, value);
      if (status == ARCHBIRD_OK)
        status = item_add_string_attribute(context->engine, &item, "metric",
                                           &metric->as.text);
      ab_buffer_init(&detail, context->engine);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, "discovery-file-oversized ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&detail, metric->as.text.data,
                                  metric->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, "=");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(&detail, value);
      if (status == ARCHBIRD_OK)
        status = derived_evidence(
            context->engine, &project_name->as.text, &path->as.text, 0,
            resolution_sha->as.text.data, &detail, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_verify_fact_add_item(context->engine, fact, &item);
      ab_buffer_free(&detail);
      ab_verify_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      if (status != ARCHBIRD_OK)
        ab_verify_fact_item_free(context->engine, &item);
    }
  }
  if (status == ARCHBIRD_OK) {
    uint64_t universe = (uint64_t)files->as.array.count;
    if (source_oversized_count > UINT64_MAX - universe) {
      status = archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "too many mapped source file metrics");
    } else {
      universe += source_oversized_count;
      status = ab_verify_fact_selection_exact(
          context->engine, fact, "mapped_source_file", universe,
          (uint64_t)fact->item_count, universe - (uint64_t)fact->item_count, 0,
          0);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus
extract_component_membership(AbVerificationContext *context,
                             const AbObjectField *extractor,
                             AbVerifyFactSet *fact) {
  static const AbString unassigned = {(char *)"unassigned", 10};
  static const AbString exclusive = {(char *)"exclusive", 9};
  static const AbString overlap = {(char *)"overlap", 7};
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  AbVerifyMembershipIndex index = {0};
  size_t file_index;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "values",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_verify_membership_index_build(context->engine, map, &index);
  if (status != ARCHBIRD_OK) {
    ab_verify_fact_free(context->engine, fact);
    return status;
  }
  if (!index.current) {
    const char *message = index.message;
    ab_verify_membership_index_free(context->engine, &index);
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(
        context->engine, fact, &extractor->name, &project_name->as.text,
        "values", message ? message : "component membership is unavailable");
  }
  for (file_index = 0; status == ARCHBIRD_OK && file_index < index.file_count;
       file_index++) {
    const AbVerifyMembershipFile *file = &index.files[file_index];
    const AbValue *sha = ab_value_member(file->row, "sha256");
    const AbValue *layer = ab_value_member(file->row, "layer");
    const AbValue *language = ab_value_member(file->row, "language");
    const AbString *membership = !file->assignment_count       ? &unassigned
                                 : file->assignment_count == 1 ? &exclusive
                                                               : &overlap;
    AbString normalized = {0};
    int selected = 0;
    AbValue count = {0};
    AbVerifyFactItem item = {0};
    AbVerifyEvidence evidence = {0};
    AbBuffer detail;
    status = ab_verify_normalized_name(context->engine, spec, file->path,
                                       &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status =
        u64_value(context->engine, (uint64_t)file->assignment_count, &count);
    if (status == ARCHBIRD_OK)
      status = ab_verify_item_init(context->engine, &item, &normalized,
                                   file->path, &count);
    if (status == ARCHBIRD_OK)
      status =
          item_add_membership_components(context->engine, &item, &index, file);
    if (status == ARCHBIRD_OK && language && language->kind == AB_VALUE_STRING)
      status = item_add_string_attribute(context->engine, &item, "language",
                                         &language->as.text);
    if (status == ARCHBIRD_OK && layer && layer->kind == AB_VALUE_STRING)
      status = item_add_string_attribute(context->engine, &item, "layer",
                                         &layer->as.text);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "membership",
                                         membership);
    ab_buffer_init(&detail, context->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, "components=[");
    if (status == ARCHBIRD_OK) {
      size_t assignment_offset;
      for (assignment_offset = 0;
           status == ARCHBIRD_OK && assignment_offset < file->assignment_count;
           assignment_offset++) {
        const AbVerifyMembershipAssignment *assignment =
            &index.assignments[file->assignment_start + assignment_offset];
        const AbString *name =
            index.components[assignment->component_index].name;
        if (assignment_offset)
          status = ab_buffer_literal(&detail, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_json_string(&detail, name->data, name->length);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, "]");
    if (status == ARCHBIRD_OK)
      status =
          derived_evidence(context->engine, &project_name->as.text, file->path,
                           0, sha->as.text.data, &detail, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_verify_evidence_free(context->engine, &evidence);
    ab_value_free(context->engine, &count);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_verify_fact_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_selection_exact(
        context->engine, fact, "mapped_source_file", (uint64_t)index.file_count,
        (uint64_t)fact->item_count,
        (uint64_t)(index.file_count - fact->item_count), 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  ab_verify_membership_index_free(context->engine, &index);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus edge_evidence(AbVerificationContext *context,
                                    const AbString *project_name,
                                    const AbValue *map, const AbValue *edge,
                                    AbVerifyEvidence *out,
                                    const char **out_state) {
  const AbValue *source = ab_value_member(edge, "source");
  const AbValue *target = ab_value_member(edge, "target");
  const AbValue *kind = ab_value_member(edge, "kind");
  const AbValue *names = ab_value_member(edge, "names");
  const AbValue *providers = ab_value_member(edge, "evidence");
  AbBuffer detail;
  size_t index;
  int has_current = 0;
  int has_unknown = 0;
  ArchbirdStatus status;
  *out_state = "current";
  ab_buffer_init(&detail, context->engine);
  status = ab_buffer_append(&detail, kind->as.text.data, kind->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " -> ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&detail, target->as.text.data, target->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, "; names=");
  for (index = 0;
       status == ARCHBIRD_OK && names && index < names->as.array.count;
       index++) {
    if (index)
      status = ab_buffer_literal(&detail, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&detail, names->as.array.items[index].as.text.data,
                           names->as.array.items[index].as.text.length);
  }
  if (status == ARCHBIRD_OK && providers) {
    if (providers->kind != AB_VALUE_ARRAY)
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "Map edge evidence must be an array");
    else
      status = ab_buffer_literal(&detail, "; evidence=");
  }
  for (index = 0;
       status == ARCHBIRD_OK && providers && index < providers->as.array.count;
       index++) {
    const AbValue *row = &providers->as.array.items[index];
    const AbValue *basis = ab_value_member(row, "basis");
    const AbValue *provider = ab_value_member(row, "provider");
    const AbValue *state = ab_value_member(row, "state");
    if (row->kind != AB_VALUE_OBJECT || !basis || !provider || !state ||
        basis->kind != AB_VALUE_STRING || provider->kind != AB_VALUE_STRING ||
        state->kind != AB_VALUE_STRING ||
        (!ab_value_string_is(state, "current") &&
         !ab_value_string_is(state, "unknown") &&
         !ab_value_string_is(state, "stale"))) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "Map edge evidence is invalid");
      break;
    }
    if (index)
      status = ab_buffer_literal(&detail, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&detail, basis->as.text.data, basis->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&detail, provider->as.text.data,
                                provider->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, ":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&detail, state->as.text.data, state->as.text.length);
    has_current = has_current || ab_value_string_is(state, "current");
    has_unknown = has_unknown || ab_value_string_is(state, "unknown");
  }
  if (providers && !has_current)
    *out_state = has_unknown ? "unknown" : "stale";
  if (status == ARCHBIRD_OK)
    status = derived_evidence(context->engine, project_name, &source->as.text,
                              0, file_sha(map, &source->as.text), &detail, out);
  ab_buffer_free(&detail);
  return status;
}

static int edge_selected(const AbValue *spec, const AbValue *edge) {
  const AbValue *kinds = ab_value_member(spec, "kinds");
  const AbValue *from = ab_value_member(spec, "from_paths");
  const AbValue *to = ab_value_member(spec, "to_paths");
  const AbValue *kind = ab_value_member(edge, "kind");
  const AbValue *source = ab_value_member(edge, "source");
  const AbValue *target = ab_value_member(edge, "target");
  return kind && source && target &&
         (!kinds || string_array_has(kinds, &kind->as.text)) &&
         path_matches(&source->as.text, from) &&
         path_matches(&target->as.text, to);
}

static ArchbirdStatus extract_file_edges(AbVerificationContext *context,
                                         const AbObjectField *extractor,
                                         AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *edges = map ? ab_value_member(map, "edges") : NULL;
  size_t index;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "relation",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!edges || edges->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "relation",
                                  "project map has no edge inventory");
  }
  for (index = 0; status == ARCHBIRD_OK && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *source;
    const AbValue *kind;
    const AbValue *target;
    AbVerifyEvidence evidence = {0};
    const char *evidence_state;
    if (!edge_selected(spec, edge))
      continue;
    source = ab_value_member(edge, "source");
    kind = ab_value_member(edge, "kind");
    target = ab_value_member(edge, "target");
    status = edge_evidence(context, &project_name->as.text, map, edge,
                           &evidence, &evidence_state);
    if (status == ARCHBIRD_OK)
      status = add_relation_item_state(
          context->engine, fact, &source->as.text, &kind->as.text,
          &target->as.text, &evidence, evidence_state,
          strcmp(evidence_state, "current") == 0
              ? ""
              : (strcmp(evidence_state, "unknown") == 0
                     ? "edge freshness is unknown"
                     : "edge evidence is stale"));
    ab_verify_evidence_free(context->engine, &evidence);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static int component_has_file(const AbValue *component, const AbString *path) {
  const AbValue *files = ab_value_member(component, "files");
  return files && string_array_has(files, path);
}

static ArchbirdStatus extract_component_edges(AbVerificationContext *context,
                                              const AbObjectField *extractor,
                                              AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *edges = map ? ab_value_member(map, "edges") : NULL;
  const AbValue *components = map ? ab_value_member(map, "components") : NULL;
  const AbValue *kinds = ab_value_member(spec, "kinds");
  size_t edge_index;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "relation",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!edges || edges->kind != AB_VALUE_ARRAY || !components ||
      components->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(
        context->engine, fact, &extractor->name, &project_name->as.text,
        "relation", "project map has no component/edge inventory");
  }
  for (edge_index = 0;
       status == ARCHBIRD_OK && edge_index < edges->as.array.count;
       edge_index++) {
    const AbValue *edge = &edges->as.array.items[edge_index];
    const AbValue *source = ab_value_member(edge, "source");
    const AbValue *target = ab_value_member(edge, "target");
    const AbValue *kind = ab_value_member(edge, "kind");
    size_t source_index;
    if (!source || !target || !kind ||
        (kinds && !string_array_has(kinds, &kind->as.text)))
      continue;
    for (source_index = 0;
         status == ARCHBIRD_OK && source_index < components->as.array.count;
         source_index++) {
      const AbValue *source_component =
          &components->as.array.items[source_index];
      const AbValue *source_name = ab_value_member(source_component, "name");
      size_t target_index;
      if (!source_name ||
          !component_has_file(source_component, &source->as.text))
        continue;
      for (target_index = 0;
           status == ARCHBIRD_OK && target_index < components->as.array.count;
           target_index++) {
        const AbValue *target_component =
            &components->as.array.items[target_index];
        const AbValue *target_name = ab_value_member(target_component, "name");
        AbVerifyEvidence evidence = {0};
        const char *evidence_state;
        if (!target_name ||
            !component_has_file(target_component, &target->as.text) ||
            ab_string_equal(&source_name->as.text, &target_name->as.text))
          continue;
        status = edge_evidence(context, &project_name->as.text, map, edge,
                               &evidence, &evidence_state);
        if (status == ARCHBIRD_OK)
          status = add_relation_item_state(
              context->engine, fact, &source_name->as.text, &kind->as.text,
              &target_name->as.text, &evidence, evidence_state,
              strcmp(evidence_state, "current") == 0
                  ? ""
                  : (strcmp(evidence_state, "unknown") == 0
                         ? "edge freshness is unknown"
                         : "edge evidence is stale"));
        ab_verify_evidence_free(context->engine, &evidence);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static int selector_matches(const AbString *selector, const AbValue *patterns) {
  return !patterns || !patterns->as.array.count ||
         patterns_match(patterns, selector);
}

static ArchbirdStatus
test_route_evidence(AbVerificationContext *context, const AbString *project,
                    const AbString *path, uint64_t line, const char *sha,
                    const AbString *selector, const AbString *target,
                    const char *kind, AbVerifyEvidence *out) {
  AbBuffer detail;
  ArchbirdStatus status;
  ab_buffer_init(&detail, context->engine);
  status = ab_buffer_literal(
      &detail, !strcmp(kind, "configured") ? "static test case" : "test route");
  if (status == ARCHBIRD_OK && selector && selector->length) {
    status = ab_buffer_literal(&detail, " ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&detail, selector->data, selector->length);
  }
  if (!strcmp(kind, "configured")) {
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, " selected by configured route");
  } else {
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, " -> ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&detail, target->data, target->length);
    if (status == ARCHBIRD_OK && selector && selector->length) {
      status = ab_buffer_literal(&detail, " (");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, kind);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, ")");
    }
  }
  if (status == ARCHBIRD_OK)
    status = derived_evidence(context->engine, project, path, line, sha,
                              &detail, out);
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus configured_route_assertion_evidence(
    AbVerificationContext *context, const AbString *project,
    const AbString *selector, const AbString *target, AbVerifyEvidence *out) {
  const AbValue *input = ab_verify_input_project(&context->input, project);
  const AbValue *map = input ? ab_value_member(input, "map") : NULL;
  const AbValue *map_evidence = map ? ab_value_member(map, "evidence") : NULL;
  const AbValue *config_sha =
      map_evidence ? ab_value_member(map_evidence, "config_sha256") : NULL;
  AbString empty = {0};
  AbBuffer detail;
  ArchbirdStatus status;
  if (!config_sha || config_sha->kind != AB_VALUE_STRING)
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "configured test route has no config digest");
  ab_buffer_init(&detail, context->engine);
  status = ab_buffer_literal(&detail, "configured structural test route ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, selector->data, selector->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " -> ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, target->data, target->length);
  if (status == ARCHBIRD_OK)
    status = ab_verify_evidence_init(context->engine, out, "asserted", project,
                                     &empty, 0, config_sha->as.text.data,
                                     (const char *)detail.data, detail.length);
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus
add_test_route_relation(AbVerificationContext *context, AbVerifyFactSet *fact,
                        const AbString *project, const AbString *path,
                        uint64_t line, const char *sha,
                        const AbString *selector, const AbString *source,
                        const AbString *target, const char *kind) {
  AbString kind_string = {(char *)kind, strlen(kind)};
  AbVerifyEvidence source_evidence = {0};
  AbVerifyEvidence assertion_evidence = {0};
  ArchbirdStatus status =
      test_route_evidence(context, project, path, line, sha, selector, target,
                          kind, &source_evidence);
  if (status == ARCHBIRD_OK)
    status = add_relation_item(context->engine, fact, source, &kind_string,
                               target, &source_evidence);
  if (status == ARCHBIRD_OK && !strcmp(kind, "configured"))
    status = configured_route_assertion_evidence(context, project, selector,
                                                 target, &assertion_evidence);
  if (status == ARCHBIRD_OK && !strcmp(kind, "configured"))
    status = add_relation_item(context->engine, fact, source, &kind_string,
                               target, &assertion_evidence);
  ab_verify_evidence_free(context->engine, &source_evidence);
  ab_verify_evidence_free(context->engine, &assertion_evidence);
  return status;
}

static ArchbirdStatus add_case_routes(AbVerificationContext *context,
                                      AbVerifyFactSet *fact,
                                      const AbString *project,
                                      const AbValue *spec, const AbValue *test,
                                      const AbValue *test_case,
                                      const char *sha) {
  const AbValue *group = ab_value_member(test, "group");
  const AbValue *path = ab_value_member(test, "path");
  const AbValue *selector = ab_value_member(test_case, "selector");
  const AbValue *routes = ab_value_member(test_case, "routes");
  const AbValue *configured = ab_value_member(test_case, "configured_routes");
  const AbValue *patterns = ab_value_member(spec, "selectors");
  const AbValue *configured_only = ab_value_member(spec, "configured_only");
  const AbValue *line = ab_value_member(test_case, "line");
  AbBuffer source;
  uint64_t line_number = 0;
  size_t route_index;
  int only_configured = configured_only && configured_only->as.boolean;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!selector || !routes || routes->kind != AB_VALUE_OBJECT ||
      !selector_matches(&selector->as.text, patterns) ||
      (line && !ab_value_u64(line, &line_number)))
    return ARCHBIRD_OK;
  ab_buffer_init(&source, context->engine);
  status =
      ab_buffer_append(&source, group->as.text.data, group->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&source, ":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&source, path->as.text.data, path->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&source, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&source, selector->as.text.data,
                              selector->as.text.length);
  for (route_index = 0; status == ARCHBIRD_OK && !only_configured &&
                        route_index < routes->as.object.count;
       route_index++) {
    const AbString *target = &routes->as.object.fields[route_index].name;
    const char *kind =
        string_array_has(configured, target) ? "configured" : "static";
    status = add_test_route_relation(
        context, fact, project, &path->as.text, line_number, sha,
        &selector->as.text, &(AbString){(char *)source.data, source.length},
        target, kind);
  }
  for (route_index = 0; status == ARCHBIRD_OK && configured &&
                        route_index < configured->as.array.count;
       route_index++) {
    const AbString *target = &configured->as.array.items[route_index].as.text;
    if (!only_configured && routes) {
      /* Configured routes already emitted through the route object. */
      size_t route_field;
      int present = 0;
      for (route_field = 0; route_field < routes->as.object.count;
           route_field++)
        if (ab_string_equal(&routes->as.object.fields[route_field].name,
                            target))
          present = 1;
      if (present)
        continue;
    }
    status = add_test_route_relation(
        context, fact, project, &path->as.text, line_number, sha,
        &selector->as.text, &(AbString){(char *)source.data, source.length},
        target, "configured");
  }
  ab_buffer_free(&source);
  return status;
}

static ArchbirdStatus extract_test_routes(AbVerificationContext *context,
                                          const AbObjectField *extractor,
                                          AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *tests = map ? ab_value_member(map, "tests") : NULL;
  const AbValue *wanted_group = ab_value_member(spec, "group");
  const AbValue *selectors = ab_value_member(spec, "selectors");
  size_t test_index;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "relation",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!tests || tests->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "relation",
                                  "project map has no test inventory");
  }
  for (test_index = 0;
       status == ARCHBIRD_OK && test_index < tests->as.array.count;
       test_index++) {
    const AbValue *test = &tests->as.array.items[test_index];
    const AbValue *group = ab_value_member(test, "group");
    const AbValue *path = ab_value_member(test, "path");
    const AbValue *cases = ab_value_member(test, "cases");
    const AbValue *routes = ab_value_member(test, "routes");
    const char *sha;
    size_t case_index;
    if (!group || !path || !cases || cases->kind != AB_VALUE_ARRAY || !routes ||
        routes->kind != AB_VALUE_OBJECT)
      continue;
    if (wanted_group && wanted_group->as.text.length &&
        !ab_string_equal(&wanted_group->as.text, &group->as.text))
      continue;
    sha = file_sha(map, &path->as.text);
    if (cases->as.array.count) {
      for (case_index = 0;
           status == ARCHBIRD_OK && case_index < cases->as.array.count;
           case_index++)
        status = add_case_routes(context, fact, &project_name->as.text, spec,
                                 test, &cases->as.array.items[case_index], sha);
    } else if (!selectors || !selectors->as.array.count) {
      AbBuffer source;
      size_t route_index;
      ab_buffer_init(&source, context->engine);
      status =
          ab_buffer_append(&source, group->as.text.data, group->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&source, ":");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&source, path->as.text.data, path->as.text.length);
      for (route_index = 0;
           status == ARCHBIRD_OK && route_index < routes->as.object.count;
           route_index++) {
        const AbString *target = &routes->as.object.fields[route_index].name;
        AbString static_kind = {(char *)"static", 6};
        AbString empty_selector = {0};
        AbVerifyEvidence evidence = {0};
        status = test_route_evidence(context, &project_name->as.text,
                                     &path->as.text, 0, sha, &empty_selector,
                                     target, "static", &evidence);
        if (status == ARCHBIRD_OK)
          status =
              add_relation_item(context->engine, fact,
                                &(AbString){(char *)source.data, source.length},
                                &static_kind, target, &evidence);
        ab_verify_evidence_free(context->engine, &evidence);
      }
      ab_buffer_free(&source);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_test_selectors(AbVerificationContext *context,
                                             const AbObjectField *extractor,
                                             AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *map = project_map(context, spec);
  const AbValue *tests = map ? ab_value_member(map, "tests") : NULL;
  const AbValue *wanted_group = ab_value_member(spec, "group");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *selectors = ab_value_member(spec, "selectors");
  size_t test_index;
  ArchbirdStatus status =
      ab_verify_fact_init(context->engine, fact, &extractor->name, "set",
                          "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!tests || tests->kind != AB_VALUE_ARRAY) {
    ab_verify_fact_free(context->engine, fact);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "set",
                                  "project map has no test inventory");
  }
  for (test_index = 0;
       status == ARCHBIRD_OK && test_index < tests->as.array.count;
       test_index++) {
    const AbValue *test = &tests->as.array.items[test_index];
    const AbValue *group = ab_value_member(test, "group");
    const AbValue *path = ab_value_member(test, "path");
    const AbValue *cases = ab_value_member(test, "cases");
    const char *sha;
    size_t case_index;
    if (!group || group->kind != AB_VALUE_STRING || !path ||
        path->kind != AB_VALUE_STRING || !cases ||
        cases->kind != AB_VALUE_ARRAY)
      continue;
    if (wanted_group && wanted_group->as.text.length &&
        !ab_string_equal(&wanted_group->as.text, &group->as.text))
      continue;
    if (!path_matches(&path->as.text, paths))
      continue;
    sha = file_sha(map, &path->as.text);
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *test_case = &cases->as.array.items[case_index];
      const AbValue *selector = ab_value_member(test_case, "selector");
      const AbValue *line = ab_value_member(test_case, "line");
      uint64_t line_number = 0;
      AbBuffer identity;
      AbBuffer detail;
      AbString normalized = {0};
      AbVerifyFactItem item = {0};
      AbVerifyEvidence evidence = {0};
      int selected = 0;
      if (!selector || selector->kind != AB_VALUE_STRING ||
          (line && !ab_value_u64(line, &line_number)) ||
          !selector_matches(&selector->as.text, selectors))
        continue;
      ab_buffer_init(&identity, context->engine);
      ab_buffer_init(&detail, context->engine);
      status =
          ab_buffer_append(&identity, path->as.text.data, path->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&identity, "::");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&identity, selector->as.text.data,
                                  selector->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_verify_normalized_name(
            context->engine, spec,
            &(AbString){(char *)identity.data, identity.length}, &normalized,
            &selected);
      if (status == ARCHBIRD_OK && selected)
        status = ab_buffer_literal(&detail, "test case ");
      if (status == ARCHBIRD_OK && selected)
        status = ab_buffer_append(&detail, selector->as.text.data,
                                  selector->as.text.length);
      if (status == ARCHBIRD_OK && selected)
        status = derived_evidence(context->engine, &project_name->as.text,
                                  &path->as.text, line_number, sha, &detail,
                                  &evidence);
      if (status == ARCHBIRD_OK && selected) {
        size_t previous;
        for (previous = 0; previous < fact->item_count; previous++)
          if (ab_string_equal(&fact->items[previous].key, &normalized))
            break;
        if (previous < fact->item_count) {
          status = ab_verify_item_add_evidence(
              context->engine, &fact->items[previous], &evidence);
        } else {
          status = ab_verify_item_init(context->engine, &item, &normalized,
                                       &normalized, NULL);
          if (status == ARCHBIRD_OK)
            status =
                ab_verify_item_add_evidence(context->engine, &item, &evidence);
          if (status == ARCHBIRD_OK)
            status = ab_verify_fact_add_item(context->engine, fact, &item);
        }
      }
      ab_verify_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      ab_buffer_free(&detail);
      ab_buffer_free(&identity);
      if (status != ARCHBIRD_OK && selected)
        ab_verify_fact_item_free(context->engine, &item);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

typedef struct SurfaceEvidencePath {
  const AbString *path;
  const char *role;
  AbString detail;
} SurfaceEvidencePath;

static void surface_paths_free(ArchbirdEngine *engine,
                               SurfaceEvidencePath *rows, size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(engine, &rows[index].detail);
  ab_free(engine, rows);
}

static ArchbirdStatus surface_path_add(ArchbirdEngine *engine,
                                       SurfaceEvidencePath **rows,
                                       size_t *count, const AbString *path,
                                       const char *role, const char *detail,
                                       size_t detail_length) {
  SurfaceEvidencePath *resized;
  size_t index;
  for (index = 0; index < *count; index++) {
    if (ab_string_equal((*rows)[index].path, path) &&
        strcmp((*rows)[index].role, role) == 0) {
      ab_string_free(engine, &(*rows)[index].detail);
      return ab_string_copy(engine, &(*rows)[index].detail, detail,
                            detail_length);
    }
  }
  if (*count == SIZE_MAX / sizeof(**rows))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many provider-surface evidence paths");
  resized = (SurfaceEvidencePath *)ab_realloc(engine, *rows,
                                              (*count + 1) * sizeof(**rows));
  if (!resized)
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory storing provider-surface evidence");
  *rows = resized;
  memset(&(*rows)[*count], 0, sizeof(**rows));
  (*rows)[*count].path = path;
  (*rows)[*count].role = role;
  if (ab_string_copy(engine, &(*rows)[*count].detail, detail, detail_length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  (*count)++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus object_field_name(ArchbirdEngine *engine,
                                        AbObjectField *field,
                                        const char *name) {
  return ab_string_copy(engine, &field->name, name, strlen(name));
}

static ArchbirdStatus surface_value(ArchbirdEngine *engine, const AbValue *row,
                                    AbValue *out) {
  const AbValue *name = ab_value_member(row, "name");
  const AbValue *declaration = ab_value_member(row, "declaration");
  const AbValue *resolution = ab_value_member(row, "resolution");
  const AbValue *declaration_signatures =
      ab_value_member(row, "declaration_signatures");
  const AbValue *implementation_signatures =
      ab_value_member(row, "implementation_signatures");
  const AbValue *uses = ab_value_member(row, "uses");
  int declared = ab_verify_string_is(declaration, "declared");
  int used = uses && uses->kind == AB_VALUE_ARRAY && uses->as.array.count != 0;
  static const char *const names[] = {
      "declaration",
      "declaration_signatures",
      "implementation_signatures",
      "name",
      "registered",
      "resolution",
      "unregistered_use",
      "unused",
      "used",
  };
  const AbValue *copies[] = {
      declaration,
      declaration_signatures,
      implementation_signatures,
      name,
      NULL,
      resolution,
      NULL,
      NULL,
      NULL,
  };
  int booleans[] = {
      0, 0, 0, 0, declared, 0, !declared && used, declared && !used, used};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  out->kind = AB_VALUE_OBJECT;
  out->as.object.count = sizeof(names) / sizeof(names[0]);
  out->as.object.fields = (AbObjectField *)ab_calloc(
      engine, out->as.object.count, sizeof(*out->as.object.fields));
  if (!out->as.object.fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory constructing provider value");
  for (index = 0; status == ARCHBIRD_OK && index < out->as.object.count;
       index++) {
    status =
        object_field_name(engine, &out->as.object.fields[index], names[index]);
    if (status == ARCHBIRD_OK) {
      if (copies[index])
        status = ab_value_copy(engine, &out->as.object.fields[index].value,
                               copies[index]);
      else {
        out->as.object.fields[index].value.kind = AB_VALUE_BOOL;
        out->as.object.fields[index].value.as.boolean = booleans[index];
      }
    }
  }
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static ArchbirdStatus current_path_hash(AbVerificationContext *context,
                                        const AbString *project,
                                        const AbValue *map,
                                        const AbString *path, char output[65],
                                        int *current) {
  const AbValue *input_project =
      ab_verify_input_project(&context->input, project);
  const AbValue *source =
      input_project ? ab_verify_input_source(input_project, path) : NULL;
  const AbValue *text = source ? ab_value_member(source, "text") : NULL;
  const AbValue *observed_sha =
      source ? ab_value_member(source, "sha256") : NULL;
  const AbValue *file = map_file(map, path);
  const AbValue *sha = file ? ab_value_member(file, "sha256") : NULL;
  if (text && text->kind == AB_VALUE_STRING) {
    uint8_t digest[32];
    ArchbirdStatus status = archbird_sha256((const uint8_t *)text->as.text.data,
                                            text->as.text.length, digest);
    if (status == ARCHBIRD_OK)
      archbird_sha256_hex(digest, output);
    *current = status == ARCHBIRD_OK;
    return status;
  }
  if (observed_sha && observed_sha->kind == AB_VALUE_STRING &&
      observed_sha->as.text.length == 64) {
    memcpy(output, observed_sha->as.text.data, 64);
    output[64] = '\0';
    *current = 1;
    return ARCHBIRD_OK;
  }
  if (sha && sha->kind == AB_VALUE_STRING && sha->as.text.length == 64) {
    memcpy(output, sha->as.text.data, 64);
    output[64] = '\0';
    *current = 1;
  } else {
    output[0] = '\0';
    *current = 0;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus provider_detail(ArchbirdEngine *engine,
                                      const AbString *surface, const char *role,
                                      const AbString *name,
                                      const AbString *extra, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "surface ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, surface->data, surface->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, " ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, role);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, " ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, name->data, name->length);
  if (status == ARCHBIRD_OK && extra && extra->length) {
    status = ab_buffer_literal(&buffer, " ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&buffer, extra->data, extra->length);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static const AbValue *find_surface(const AbValue *map, const AbString *name) {
  const AbValue *surfaces = ab_value_member(map, "surfaces");
  size_t index;
  if (!surfaces || surfaces->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < surfaces->as.array.count; index++) {
    const AbValue *row = &surfaces->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

static ArchbirdStatus extract_provider_surface(AbVerificationContext *context,
                                               const AbObjectField *extractor,
                                               AbVerifyFactSet *fact) {
  const AbValue *spec = &extractor->value;
  const AbValue *project_name = ab_value_member(spec, "project");
  const AbValue *wanted = ab_value_member(spec, "name");
  const AbValue *map = project_map(context, spec);
  const AbValue *surface = map ? find_surface(map, &wanted->as.text) : NULL;
  const AbValue *surface_name =
      surface ? ab_value_member(surface, "name") : NULL;
  const AbValue *surface_kind =
      surface ? ab_value_member(surface, "kind") : NULL;
  const AbValue *names = surface ? ab_value_member(surface, "names") : NULL;
  size_t index;
  ArchbirdStatus status;
  if (!surface || !names || names->kind != AB_VALUE_ARRAY) {
    char message[512];
    (void)snprintf(message, sizeof(message),
                   "project %.*s: provider surface '%.*s' is absent",
                   (int)project_name->as.text.length,
                   project_name->as.text.data, (int)wanted->as.text.length,
                   wanted->as.text.data);
    return ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                                  &project_name->as.text, "values", message);
  }
  status = ab_verify_fact_init(context->engine, fact, &extractor->name,
                               "values", "derived", &project_name->as.text);
  for (index = 0; status == ARCHBIRD_OK && index < names->as.array.count;
       index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *name = ab_value_member(row, "name");
    const AbValue *ignored = ab_value_member(row, "ignored");
    const AbValue *declarations = ab_value_member(row, "declarations");
    const AbValue *uses = ab_value_member(row, "uses");
    const AbValue *candidates = ab_value_member(row, "candidates");
    const AbValue *declaration = ab_value_member(row, "declaration");
    const AbValue *resolution = ab_value_member(row, "resolution");
    SurfaceEvidencePath *paths = NULL;
    size_t path_count = 0;
    size_t path_index;
    AbString normalized = {0};
    int selected = 0;
    AbValue value = {0};
    AbVerifyFactItem item = {0};
    AbBuffer missing;
    int missing_count = 0;
    if (!name || !ignored || ignored->kind != AB_VALUE_BOOL ||
        ignored->as.boolean)
      continue;
    status = ab_verify_normalized_name(context->engine, spec, &name->as.text,
                                       &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    for (path_index = 0; status == ARCHBIRD_OK && declarations &&
                         path_index < declarations->as.array.count;
         path_index++) {
      const AbValue *entry = &declarations->as.array.items[path_index];
      const AbValue *path = ab_value_member(entry, "path");
      const AbValue *source = ab_value_member(entry, "source");
      status = surface_path_add(context->engine, &paths, &path_count,
                                &path->as.text, "declaration",
                                source->as.text.data, source->as.text.length);
    }
    for (path_index = 0;
         status == ARCHBIRD_OK && uses && path_index < uses->as.array.count;
         path_index++) {
      const AbValue *entry = &uses->as.array.items[path_index];
      const AbValue *path = ab_value_member(entry, "path");
      const AbValue *count = ab_value_member(entry, "count");
      AbBuffer detail;
      ab_buffer_init(&detail, context->engine);
      status = ab_buffer_literal(&detail, "count=");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&detail, count->as.text.data,
                                  count->as.text.length);
      if (status == ARCHBIRD_OK)
        status = surface_path_add(context->engine, &paths, &path_count,
                                  &path->as.text, "use",
                                  (const char *)detail.data, detail.length);
      ab_buffer_free(&detail);
    }
    for (path_index = 0; status == ARCHBIRD_OK && candidates &&
                         path_index < candidates->as.array.count;
         path_index++)
      status = surface_path_add(context->engine, &paths, &path_count,
                                &candidates->as.array.items[path_index].as.text,
                                "implementation", "", 0);
    if (status == ARCHBIRD_OK)
      status = surface_value(context->engine, row, &value);
    if (status == ARCHBIRD_OK)
      status = ab_verify_item_init(context->engine, &item, &normalized,
                                   &normalized, &value);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "declaration",
                                         &declaration->as.text);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "resolution",
                                         &resolution->as.text);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "surface",
                                         &surface_name->as.text);
    if (status == ARCHBIRD_OK)
      status = item_add_string_attribute(context->engine, &item, "surface_kind",
                                         &surface_kind->as.text);
    if (status == ARCHBIRD_OK) {
      AbString used = {
          (char *)((uses && uses->as.array.count) ? "true" : "false"),
          (uses && uses->as.array.count) ? 4 : 5};
      status = item_add_string_attribute(context->engine, &item, "used", &used);
    }
    ab_buffer_init(&missing, context->engine);
    for (path_index = 0; status == ARCHBIRD_OK && path_index < path_count;
         path_index++) {
      SurfaceEvidencePath *path = &paths[path_index];
      char sha[65];
      int current = 0;
      AbString detail = {0};
      AbVerifyEvidence evidence = {0};
      status = current_path_hash(context, &project_name->as.text, map,
                                 path->path, sha, &current);
      if (!current) {
        if (missing_count++)
          status = ab_buffer_literal(&missing, ", ");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&missing, path->path->data, path->path->length);
        continue;
      }
      if (status == ARCHBIRD_OK)
        status =
            provider_detail(context->engine, &surface_name->as.text, path->role,
                            &name->as.text, &path->detail, &detail);
      if (status == ARCHBIRD_OK)
        status = ab_verify_evidence_init(context->engine, &evidence, "derived",
                                         &project_name->as.text, path->path, 0,
                                         sha, detail.data, detail.length);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
      ab_verify_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &detail);
    }
    if (status == ARCHBIRD_OK && missing_count) {
      AbBuffer message;
      ab_buffer_init(&message, context->engine);
      status = ab_buffer_literal(&message, "current bytes unavailable for: ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, missing.data, missing.length);
      if (status == ARCHBIRD_OK)
        status = ab_verify_item_set_state(context->engine, &item, "unknown",
                                          (const char *)message.data);
      ab_buffer_free(&message);
    }
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_add_item(context->engine, fact, &item);
    ab_buffer_free(&missing);
    ab_verify_fact_item_free(context->engine, &item);
    ab_value_free(context->engine, &value);
    ab_string_free(context->engine, &normalized);
    surface_paths_free(context->engine, paths, path_count);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_map_fact(AbVerificationContext *context,
                                       const AbObjectField *extractor,
                                       AbVerifyFactSet *fact) {
  const AbValue *kind = ab_value_member(&extractor->value, "kind");
  if (ab_verify_string_is(kind, "symbols"))
    return extract_symbols(context, extractor, fact);
  if (ab_verify_string_is(kind, "file_edges"))
    return extract_file_edges(context, extractor, fact);
  if (ab_verify_string_is(kind, "file_metrics"))
    return extract_file_metrics(context, extractor, fact);
  if (ab_verify_string_is(kind, "component_membership"))
    return extract_component_membership(context, extractor, fact);
  if (ab_verify_string_is(kind, "component_edges"))
    return extract_component_edges(context, extractor, fact);
  if (ab_verify_string_is(kind, "test_routes"))
    return extract_test_routes(context, extractor, fact);
  if (ab_verify_string_is(kind, "test_selectors"))
    return extract_test_selectors(context, extractor, fact);
  if (ab_verify_string_is(kind, "provider_surface"))
    return extract_provider_surface(context, extractor, fact);
  return ARCHBIRD_CONFLICT;
}

static ArchbirdStatus extract_unknown(AbVerificationContext *context,
                                      const AbObjectField *extractor,
                                      AbVerifyFactSet *fact) {
  const AbValue *project_name = ab_value_member(&extractor->value, "project");
  AbString empty = {0};
  return ab_verify_fact_unknown(
      context->engine, fact, &extractor->name,
      project_name ? &project_name->as.text : &empty,
      fact_shape(ab_value_member(&extractor->value, "kind")),
      "native extractor is not implemented yet");
}

static ArchbirdStatus apply_source_lock(AbVerificationContext *context,
                                        AbVerifyFactSet *fact) {
  AbVerifySourceLockState lock_state;
  const char *state;
  const char *message;
  ArchbirdStatus status;
  if (!string_literal(&fact->provenance, "derived") || !fact->project.length ||
      !string_literal(&fact->state, "current"))
    return ARCHBIRD_OK;
  lock_state = ab_verify_source_lock_state(context, &fact->project);
  if (lock_state == AB_VERIFY_SOURCE_LOCK_NOT_DECLARED ||
      lock_state == AB_VERIFY_SOURCE_LOCK_CURRENT)
    return ARCHBIRD_OK;
  if (lock_state == AB_VERIFY_SOURCE_LOCK_MISMATCH) {
    state = "stale";
    message = "project source lock does not match current source bytes";
  } else {
    state = "unknown";
    message =
        "project source lock cannot be checked from supplied source bytes";
  }
  ab_string_free(context->engine, &fact->state);
  ab_string_free(context->engine, &fact->message);
  status = copy_literal(context->engine, &fact->state, state);
  if (status == ARCHBIRD_OK)
    status = copy_literal(context->engine, &fact->message, message);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(context->engine, fact);
  return status;
}

ArchbirdStatus ab_verify_extract_all(AbVerificationContext *context) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->engine || !context->suite.extractors)
    return ARCHBIRD_INVALID_ARGUMENT;
  context->fact_count = context->suite.extractors->as.object.count;
  if (context->fact_count > SIZE_MAX / sizeof(*context->facts))
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification extractors");
  if (context->fact_count) {
    context->facts = (AbVerifyFactSet *)ab_calloc(
        context->engine, context->fact_count, sizeof(*context->facts));
    if (!context->facts)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification facts");
  }
  for (index = 0; status == ARCHBIRD_OK && index < context->fact_count;
       index++) {
    const AbObjectField *extractor =
        &context->suite.extractors->as.object.fields[index];
    const AbValue *kind = ab_value_member(&extractor->value, "kind");
    const AbValue *provided =
        ab_verify_input_provided_fact(&context->input, &extractor->name);
    if (provided)
      status = extract_provided(context, extractor, provided,
                                &context->facts[index]);
    else if (ab_verify_string_is(kind, "literal_set") ||
             ab_verify_string_is(kind, "literal_values") ||
             ab_verify_string_is(kind, "literal_relation"))
      status = extract_literal(context, extractor, &context->facts[index]);
    else if (ab_verify_string_is(kind, "symbols") ||
             ab_verify_string_is(kind, "file_edges") ||
             ab_verify_string_is(kind, "file_metrics") ||
             ab_verify_string_is(kind, "component_membership") ||
             ab_verify_string_is(kind, "component_edges") ||
             ab_verify_string_is(kind, "test_routes") ||
             ab_verify_string_is(kind, "test_selectors") ||
             ab_verify_string_is(kind, "provider_surface"))
      status = extract_map_fact(context, extractor, &context->facts[index]);
    else if (ab_verify_string_is(kind, "c_enum") ||
             ab_verify_string_is(kind, "c_designated_initializer") ||
             ab_verify_string_is(kind, "c_macro_set"))
      status = ab_verify_extract_c(context, extractor, &context->facts[index]);
    else
      status = extract_unknown(context, extractor, &context->facts[index]);
    if (status == ARCHBIRD_OK)
      status = apply_source_lock(context, &context->facts[index]);
  }
  return status;
}

void ab_verification_context_free(AbVerificationContext *context) {
  size_t index;
  if (!context)
    return;
  for (index = 0; context->facts && index < context->fact_count; index++)
    ab_verify_fact_free(context->engine, &context->facts[index]);
  ab_free(context->engine, context->facts);
  context->facts = NULL;
  context->fact_count = 0;
  for (index = 0; context->checks && index < context->check_count; index++)
    ab_verify_check_result_free(context->engine, &context->checks[index]);
  ab_free(context->engine, context->checks);
  context->checks = NULL;
  context->check_count = 0;
  ab_verify_diagnostics_free(context);
  ab_verify_baseline_free(context->engine, &context->baseline);
  ab_verify_attestations_free(context);
}
