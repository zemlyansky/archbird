#include "projection_internal.h"

#include "component_membership.h"
#include "path_match.h"
#include "sha256.h"

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

static ArchbirdStatus u64_value(ArchbirdEngine *engine, uint64_t number,
                                AbValue *out);

static const char *fact_shape(const AbValue *kind) {
  if (ab_projection_value_is(kind, "artifact_routes") ||
      ab_projection_value_is(kind, "file_edges") ||
      ab_projection_value_is(kind, "package_entrypoints") ||
      ab_projection_value_is(kind, "package_exports") ||
      ab_projection_value_is(kind, "component_edges") ||
      ab_projection_value_is(kind, "test_routes") ||
      ab_projection_value_is(kind, "literal_relation"))
    return "relation";
  if (ab_projection_value_is(kind, "python_enum") ||
      ab_projection_value_is(kind, "c_enum") ||
      ab_projection_value_is(kind, "c_designated_initializer") ||
      ab_projection_value_is(kind, "constant_values") ||
      ab_projection_value_is(kind, "file_metrics") ||
      ab_projection_value_is(kind, "component_membership") ||
      ab_projection_value_is(kind, "provider_surface") ||
      ab_projection_value_is(kind, "literal_values"))
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
  return ab_projection_value_is(value, "current") ||
         ab_projection_value_is(value, "stale") ||
         ab_projection_value_is(value, "unknown");
}

static int valid_fact_provenance(const AbValue *value) {
  return ab_projection_value_is(value, "derived") ||
         ab_projection_value_is(value, "asserted") ||
         ab_projection_value_is(value, "observed");
}

ArchbirdStatus ab_projection_item_init(ArchbirdEngine *engine,
                                       AbProjectionItem *item,
                                       const AbString *key,
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
    ab_projection_item_free(engine, item);
  return status;
}

ArchbirdStatus
ab_projection_item_add_evidence(ArchbirdEngine *engine, AbProjectionItem *item,
                                const AbProjectionEvidence *source) {
  AbProjectionEvidence *rows;
  ArchbirdStatus status;
  if (item->evidence_count == item->evidence_capacity) {
    size_t capacity = item->evidence_capacity ? item->evidence_capacity * 2 : 1;
    if (capacity < item->evidence_capacity ||
        capacity > SIZE_MAX / sizeof(*rows))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too much verification evidence");
    rows = (AbProjectionEvidence *)ab_realloc(engine, item->evidence,
                                              capacity * sizeof(*rows));
    if (!rows)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification evidence");
    item->evidence = rows;
    item->evidence_capacity = capacity;
  }
  memset(&item->evidence[item->evidence_count], 0, sizeof(*item->evidence));
  status = ab_projection_evidence_init(
      engine, &item->evidence[item->evidence_count], source->provenance.data,
      &source->project, &source->path, source->line, source->sha256.data,
      source->detail.data, source->detail.length);
  if (status == ARCHBIRD_OK)
    item->evidence_count++;
  return status;
}

ArchbirdStatus ab_projection_item_set_state(ArchbirdEngine *engine,
                                            AbProjectionItem *item,
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
                                           AbProjectionItem *item,
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
                                                AbProjectionItem *item,
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
  else {
    AbObjectField *field = &item->attributes[item->attribute_count];
    ab_string_free(engine, &field->name);
    ab_value_free(engine, &field->value);
    memset(field, 0, sizeof(*field));
  }
  return status;
}

static ArchbirdStatus item_add_u64_attribute(ArchbirdEngine *engine,
                                             AbProjectionItem *item,
                                             const char *name, uint64_t value) {
  AbObjectField *fields;
  AbObjectField *field;
  ArchbirdStatus status;
  if (item->attribute_count == SIZE_MAX / sizeof(*fields))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many projection item attributes");
  fields = (AbObjectField *)ab_realloc(
      engine, item->attributes, (item->attribute_count + 1) * sizeof(*fields));
  if (!fields)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing projection attributes");
  item->attributes = fields;
  field = &item->attributes[item->attribute_count];
  memset(field, 0, sizeof(*field));
  status = copy_literal(engine, &field->name, name);
  if (status == ARCHBIRD_OK)
    status = u64_value(engine, value, &field->value);
  if (status == ARCHBIRD_OK)
    item->attribute_count++;
  else {
    ab_string_free(engine, &field->name);
    ab_value_free(engine, &field->value);
  }
  return status;
}

static ArchbirdStatus item_add_symbol_location(ArchbirdEngine *engine,
                                               AbProjectionItem *item,
                                               const AbString *path,
                                               size_t source_index,
                                               size_t nested_index) {
  AbObjectField *attribute = NULL;
  AbValue *locations;
  AbValue *location;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; index < item->attribute_count; index++)
    if (string_literal(&item->attributes[index].name, "locations")) {
      attribute = &item->attributes[index];
      break;
    }
  if (!attribute) {
    AbObjectField *fields;
    if (item->attribute_count == SIZE_MAX / sizeof(*fields))
      return ARCHBIRD_LIMIT_EXCEEDED;
    fields = (AbObjectField *)ab_realloc(engine, item->attributes,
                                         (item->attribute_count + 1) *
                                             sizeof(*fields));
    if (!fields)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing symbol locations");
    item->attributes = fields;
    attribute = &fields[item->attribute_count];
    memset(attribute, 0, sizeof(*attribute));
    status = copy_literal(engine, &attribute->name, "locations");
    if (status == ARCHBIRD_OK)
      attribute->value.kind = AB_VALUE_ARRAY;
    if (status != ARCHBIRD_OK)
      return status;
    item->attribute_count++;
  }
  if (attribute->value.kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  if (attribute->value.as.array.count == SIZE_MAX ||
      attribute->value.as.array.count + 1 >
          SIZE_MAX / sizeof(*attribute->value.as.array.items))
    return ARCHBIRD_LIMIT_EXCEEDED;
  locations = (AbValue *)ab_realloc(engine, attribute->value.as.array.items,
                                    (attribute->value.as.array.count + 1) *
                                        sizeof(*locations));
  if (!locations)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory expanding symbol locations");
  attribute->value.as.array.items = locations;
  location = &locations[attribute->value.as.array.count];
  memset(location, 0, sizeof(*location));
  location->kind = AB_VALUE_OBJECT;
  location->as.object.count = 3;
  location->as.object.fields = (AbObjectField *)ab_calloc(
      engine, 3, sizeof(*location->as.object.fields));
  if (!location->as.object.fields)
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory storing a symbol location");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &location->as.object.fields[0].name,
                          "nested_index");
  if (status == ARCHBIRD_OK)
    status = u64_value(engine, (uint64_t)nested_index,
                       &location->as.object.fields[0].value);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &location->as.object.fields[1].name, "path");
  if (status == ARCHBIRD_OK) {
    location->as.object.fields[1].value.kind = AB_VALUE_STRING;
    status =
        ab_string_copy(engine, &location->as.object.fields[1].value.as.text,
                       path->data, path->length);
  }
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &location->as.object.fields[2].name,
                          "source_index");
  if (status == ARCHBIRD_OK)
    status = u64_value(engine, (uint64_t)source_index,
                       &location->as.object.fields[2].value);
  if (status == ARCHBIRD_OK)
    attribute->value.as.array.count++;
  else
    ab_value_free(engine, location);
  return status;
}

static ArchbirdStatus
item_add_membership_components(ArchbirdEngine *engine, AbProjectionItem *item,
                               const AbProjectionMembershipIndex *index,
                               const AbProjectionMembershipFile *file) {
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
    const AbProjectionMembershipAssignment *assignment =
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

static ArchbirdStatus asserted_evidence(AbProjectionContext *context,
                                        const AbObjectField *operand,
                                        AbProjectionEvidence *out) {
  AbBuffer canonical;
  AbBuffer detail;
  AbString empty = {0};
  uint8_t digest[32];
  char sha256[65];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, context->engine);
  ab_buffer_init(&detail, context->engine);
  status = ab_value_render(&canonical, &operand->value);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, "constraint operand ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&detail, operand->name.data, operand->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_projection_evidence_init(
        context->engine, out, "asserted", &empty, &empty, 0, sha256,
        (const char *)detail.data, detail.length);
  ab_buffer_free(&canonical);
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

ArchbirdStatus ab_projection_normalized_name(ArchbirdEngine *engine,
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

static ArchbirdStatus invalid_map_inventory(AbProjectionContext *context,
                                            const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s", message);
}

static int valid_string_array(const AbValue *array) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++)
    if (!ab_projection_nonblank(&array->as.array.items[index]))
      return 0;
  return 1;
}

static int valid_path_count_object(const AbValue *object) {
  size_t index;
  uint64_t count;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < object->as.object.count; index++)
    if (!ab_projection_path_is_repository(
            &(AbValue){.kind = AB_VALUE_STRING,
                       .as.text = object->as.object.fields[index].name}) ||
        !ab_value_u64(&object->as.object.fields[index].value, &count))
      return 0;
  return 1;
}

static int valid_symbol_row(const AbValue *row) {
  const AbValue *name = ab_value_member(row, "name");
  const AbValue *kind = ab_value_member(row, "kind");
  const AbValue *scope = ab_value_member(row, "scope");
  const AbValue *line = ab_value_member(row, "line");
  uint64_t line_number;
  return row && row->kind == AB_VALUE_OBJECT && name &&
         name->kind == AB_VALUE_STRING && kind &&
         kind->kind == AB_VALUE_STRING && scope &&
         scope->kind == AB_VALUE_STRING &&
         (!line || ab_value_u64(line, &line_number));
}

static int valid_projected_file_row(const AbValue *row) {
  const AbValue *symbols = ab_value_member(row, "symbols");
  size_t index;
  if (!row || row->kind != AB_VALUE_OBJECT ||
      !ab_projection_path_is_repository(ab_value_member(row, "path")) ||
      !ab_projection_nonblank(ab_value_member(row, "layer")) ||
      !lowercase_sha256_value(ab_value_member(row, "sha256")) || !symbols ||
      symbols->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < symbols->as.array.count; index++)
    if (!valid_symbol_row(&symbols->as.array.items[index]))
      return 0;
  return 1;
}

static int valid_edge_row(const AbValue *edge) {
  const AbValue *names = ab_value_member(edge, "names");
  const AbValue *providers = ab_value_member(edge, "evidence");
  size_t index;
  if (!edge || edge->kind != AB_VALUE_OBJECT ||
      !ab_projection_path_is_repository(ab_value_member(edge, "source")) ||
      !ab_projection_path_is_repository(ab_value_member(edge, "target")) ||
      !ab_projection_nonblank(ab_value_member(edge, "kind")) ||
      !valid_string_array(names) ||
      (providers && providers->kind != AB_VALUE_ARRAY))
    return 0;
  for (index = 0; providers && index < providers->as.array.count; index++) {
    const AbValue *row = &providers->as.array.items[index];
    const AbValue *state = ab_value_member(row, "state");
    if (!row || row->kind != AB_VALUE_OBJECT ||
        !ab_projection_nonblank(ab_value_member(row, "basis")) ||
        !ab_projection_nonblank(ab_value_member(row, "provider")) ||
        (!ab_projection_value_is(state, "current") &&
         !ab_projection_value_is(state, "unknown") &&
         !ab_projection_value_is(state, "stale")))
      return 0;
  }
  return 1;
}

static int valid_package_row(const AbValue *package, int exports) {
  const AbValue *aliases = ab_value_member(package, "aliases");
  const AbValue *routes =
      ab_value_member(package, exports ? "export_origins" : "entrypoints");
  size_t index;
  if (!package || package->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(package, "name")) ||
      !ab_projection_path_is_repository(ab_value_member(package, "manifest")) ||
      (aliases && !valid_string_array(aliases)) || !routes ||
      routes->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < routes->as.object.count; index++) {
    const AbObjectField *field = &routes->as.object.fields[index];
    size_t path_index;
    if (!field->name.length)
      return 0;
    if (!exports) {
      if (!ab_projection_path_is_repository(&field->value))
        return 0;
      continue;
    }
    if (field->value.kind != AB_VALUE_ARRAY)
      return 0;
    for (path_index = 0; path_index < field->value.as.array.count; path_index++)
      if (!ab_projection_path_is_repository(
              &field->value.as.array.items[path_index]))
        return 0;
  }
  return 1;
}

static int valid_artifact_row(const AbValue *artifact) {
  static const char *const fields[] = {"inputs", "loaded_by"};
  const AbValue *output = ab_value_member(artifact, "output");
  size_t role;
  if (!artifact || artifact->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(artifact, "name")) ||
      (output && !ab_projection_nonblank(output)))
    return 0;
  for (role = 0; role < sizeof(fields) / sizeof(fields[0]); role++) {
    const AbValue *rows = ab_value_member(artifact, fields[role]);
    size_t index;
    if (!rows || rows->kind != AB_VALUE_ARRAY)
      return 0;
    for (index = 0; index < rows->as.array.count; index++)
      if (rows->as.array.items[index].kind != AB_VALUE_OBJECT ||
          !ab_projection_path_is_repository(
              ab_value_member(&rows->as.array.items[index], "path")))
        return 0;
  }
  return 1;
}

static int valid_test_case_row(const AbValue *test_case) {
  const AbValue *line = ab_value_member(test_case, "line");
  const AbValue *configured = ab_value_member(test_case, "configured_routes");
  uint64_t line_number;
  size_t index;
  if (!test_case || test_case->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(test_case, "selector")) ||
      !valid_path_count_object(ab_value_member(test_case, "routes")) ||
      !valid_string_array(configured) ||
      (line && !ab_value_u64(line, &line_number)))
    return 0;
  for (index = 0; index < configured->as.array.count; index++)
    if (!ab_projection_path_is_repository(&configured->as.array.items[index]))
      return 0;
  return 1;
}

static int valid_test_row(const AbValue *test) {
  const AbValue *cases = ab_value_member(test, "cases");
  size_t index;
  if (!test || test->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(test, "group")) ||
      !ab_projection_path_is_repository(ab_value_member(test, "path")) ||
      !cases || cases->kind != AB_VALUE_ARRAY ||
      !valid_path_count_object(ab_value_member(test, "routes")))
    return 0;
  for (index = 0; index < cases->as.array.count; index++)
    if (!valid_test_case_row(&cases->as.array.items[index]))
      return 0;
  return 1;
}

static int valid_surface_row(const AbValue *surface) {
  const AbValue *names = ab_value_member(surface, "names");
  size_t name_index;
  if (!surface || surface->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(surface, "name")) ||
      !ab_projection_nonblank(ab_value_member(surface, "kind")) || !names ||
      names->kind != AB_VALUE_ARRAY)
    return 0;
  for (name_index = 0; name_index < names->as.array.count; name_index++) {
    const AbValue *row = &names->as.array.items[name_index];
    const AbValue *ignored = ab_value_member(row, "ignored");
    const AbValue *declarations = ab_value_member(row, "declarations");
    const AbValue *uses = ab_value_member(row, "uses");
    const AbValue *candidates = ab_value_member(row, "candidates");
    size_t index;
    if (!row || row->kind != AB_VALUE_OBJECT ||
        !ab_projection_nonblank(ab_value_member(row, "name")) || !ignored ||
        ignored->kind != AB_VALUE_BOOL ||
        !ab_projection_nonblank(ab_value_member(row, "declaration")) ||
        !ab_projection_nonblank(ab_value_member(row, "resolution")) ||
        !valid_string_array(ab_value_member(row, "declaration_signatures")) ||
        !valid_string_array(
            ab_value_member(row, "implementation_signatures")) ||
        !declarations || declarations->kind != AB_VALUE_ARRAY || !uses ||
        uses->kind != AB_VALUE_ARRAY || !valid_string_array(candidates))
      return 0;
    for (index = 0; index < declarations->as.array.count; index++) {
      const AbValue *entry = &declarations->as.array.items[index];
      if (!entry || entry->kind != AB_VALUE_OBJECT ||
          !ab_projection_path_is_repository(ab_value_member(entry, "path")) ||
          !ab_projection_nonblank(ab_value_member(entry, "source")))
        return 0;
    }
    for (index = 0; index < uses->as.array.count; index++) {
      const AbValue *entry = &uses->as.array.items[index];
      uint64_t count;
      if (!entry || entry->kind != AB_VALUE_OBJECT ||
          !ab_projection_path_is_repository(ab_value_member(entry, "path")) ||
          !ab_value_u64(ab_value_member(entry, "count"), &count))
        return 0;
    }
    for (index = 0; index < candidates->as.array.count; index++)
      if (!ab_projection_path_is_repository(&candidates->as.array.items[index]))
        return 0;
  }
  return 1;
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

static const AbValue *map_input(const AbValue *map, const AbString *path) {
  const AbValue *inputs = ab_value_member(map, "inputs");
  size_t index;
  if (!inputs || inputs->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < inputs->as.array.count; index++) {
    const AbValue *input = &inputs->as.array.items[index];
    const AbValue *candidate = ab_value_member(input, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return input;
  }
  return NULL;
}

static const char *file_sha(const AbValue *map, const AbString *path) {
  const AbValue *file = map_file(map, path);
  const AbValue *input = file ? file : map_input(map, path);
  const AbValue *sha = input ? ab_value_member(input, "sha256") : NULL;
  return sha && sha->kind == AB_VALUE_STRING ? sha->as.text.data : "";
}

static ArchbirdStatus
search_domain_key(ArchbirdEngine *engine, const AbString *entity_kind,
                  const AbString *path, const AbString *name,
                  const AbString *scope, const AbString *signature,
                  const AbString *symbol_kind, uint64_t line, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "{\"kind\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, entity_kind->data, entity_kind->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"line\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, line);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"name\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, name ? name->data : "",
                                   name ? name->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, path ? path->data : "",
                                   path ? path->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"scope\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, scope ? scope->data : "",
                                   scope ? scope->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"signature\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, signature ? signature->data : "",
                                   signature ? signature->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",\"symbol_kind\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&buffer, symbol_kind ? symbol_kind->data : "",
                              symbol_kind ? symbol_kind->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_search_domain_item(
    AbProjectionContext *context, AbProjectionData *fact,
    const AbString *project, const char *entity_kind, const AbString *path,
    const AbString *name, const AbString *scope, const AbString *signature,
    const AbString *symbol_kind, uint64_t line, size_t source_index,
    size_t nested_index, const AbString *evidence_path,
    const char *evidence_sha) {
  AbString kind = {(char *)entity_kind, strlen(entity_kind)};
  AbString empty = {0};
  const AbString *label = name && name->length ? name : path;
  AbProjectionItem item = {0};
  AbProjectionEvidence evidence = {0};
  AbBuffer detail;
  ArchbirdStatus status =
      search_domain_key(context->engine, &kind, path, name, scope, signature,
                        symbol_kind, line, &item.key);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(context->engine, &item.label, label ? label->data : "",
                       label ? label->length : 0);
  item.value.kind = AB_VALUE_NULL;
  if (status == ARCHBIRD_OK)
    status = copy_literal(context->engine, &item.state, "current");
  if (status == ARCHBIRD_OK)
    status = copy_literal(context->engine, &item.message, "");
  if (status == ARCHBIRD_OK)
    status =
        item_add_string_attribute(context->engine, &item, "entity_kind", &kind);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(context->engine, &item, "name",
                                       name ? name : &empty);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(context->engine, &item, "path",
                                       path ? path : &empty);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(context->engine, &item, "scope",
                                       scope ? scope : &empty);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(context->engine, &item, "signature",
                                       signature ? signature : &empty);
  if (status == ARCHBIRD_OK)
    status = item_add_string_attribute(context->engine, &item, "symbol_kind",
                                       symbol_kind ? symbol_kind : &empty);
  if (status == ARCHBIRD_OK)
    status = item_add_u64_attribute(context->engine, &item, "line", line);
  if (status == ARCHBIRD_OK)
    status = item_add_u64_attribute(context->engine, &item, "source_index",
                                    (uint64_t)source_index);
  if (status == ARCHBIRD_OK)
    status = item_add_u64_attribute(context->engine, &item, "nested_index",
                                    (uint64_t)nested_index);
  ab_buffer_init(&detail, context->engine);
  if (status == ARCHBIRD_OK && evidence_path && evidence_path->length) {
    status = ab_buffer_literal(&detail, "searchable ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&detail, kind.data, kind.length);
    if (status == ARCHBIRD_OK)
      status = ab_projection_evidence_init(
          context->engine, &evidence, "derived", project, evidence_path, line,
          evidence_sha ? evidence_sha : "", (const char *)detail.data,
          detail.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_add_item(context->engine, fact, &item);
  ab_buffer_free(&detail);
  ab_projection_evidence_free(context->engine, &evidence);
  if (status != ARCHBIRD_OK)
    ab_projection_item_free(context->engine, &item);
  return status;
}

static ArchbirdStatus extract_search_domain(AbProjectionContext *context,
                                            const AbProjectionPlan *plan,
                                            AbProjectionData *fact) {
  const AbValue *project = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *components = map ? ab_value_member(map, "components") : NULL;
  const AbValue *packages = map ? ab_value_member(map, "packages") : NULL;
  const AbValue *artifacts = map ? ab_value_member(map, "artifacts") : NULL;
  size_t universe = 0;
  size_t index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "set",
                              "derived", project ? &project->as.text : NULL);
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY || !components ||
      components->kind != AB_VALUE_ARRAY || !packages ||
      packages->kind != AB_VALUE_ARRAY || !artifacts ||
      artifacts->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, project ? &project->as.text : NULL,
        "set", "Map has no complete searchable entity inventory");
  }
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *sha = ab_value_member(file, "sha256");
    const AbValue *symbols = ab_value_member(file, "symbols");
    size_t symbol_index;
    if (!valid_projected_file_row(file)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid searchable file row");
      break;
    }
    status =
        add_search_domain_item(context, fact, &project->as.text, "file",
                               &path->as.text, &path->as.text, NULL, NULL, NULL,
                               0, index, 0, &path->as.text, sha->as.text.data);
    universe++;
    for (symbol_index = 0;
         status == ARCHBIRD_OK && symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *name = ab_value_member(symbol, "name");
      const AbValue *kind = ab_value_member(symbol, "kind");
      const AbValue *scope = ab_value_member(symbol, "scope");
      const AbValue *signature = ab_value_member(symbol, "signature");
      uint64_t line = 0;
      if (!valid_symbol_row(symbol) ||
          !ab_value_u64(ab_value_member(symbol, "line"), &line)) {
        status = invalid_map_inventory(
            context, "Map contains an invalid searchable symbol row");
        break;
      }
      status = add_search_domain_item(
          context, fact, &project->as.text, "symbol", &path->as.text,
          &name->as.text, &scope->as.text,
          signature && signature->kind == AB_VALUE_STRING ? &signature->as.text
                                                          : NULL,
          &kind->as.text, line, index, symbol_index, &path->as.text,
          sha->as.text.data);
      universe++;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < components->as.array.count;
       index++) {
    const AbValue *component = &components->as.array.items[index];
    const AbValue *name = ab_value_member(component, "name");
    const AbValue *members = ab_value_member(component, "files");
    const AbString *witness = NULL;
    if (!ab_projection_nonblank(name) || !valid_string_array(members)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid searchable component row");
      break;
    }
    if (members->as.array.count)
      witness = &members->as.array.items[0].as.text;
    status = add_search_domain_item(context, fact, &project->as.text,
                                    "component", NULL, &name->as.text, NULL,
                                    NULL, NULL, 0, index, 0, witness,
                                    witness ? file_sha(map, witness) : "");
    universe++;
  }
  for (index = 0; status == ARCHBIRD_OK && index < packages->as.array.count;
       index++) {
    const AbValue *package = &packages->as.array.items[index];
    const AbValue *name = ab_value_member(package, "identity");
    const AbValue *manifest = ab_value_member(package, "manifest");
    if (!valid_package_row(package, 0) || !ab_projection_nonblank(name)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid searchable package row");
      break;
    }
    status = add_search_domain_item(context, fact, &project->as.text, "package",
                                    &manifest->as.text, &name->as.text, NULL,
                                    NULL, NULL, 0, index, 0, &manifest->as.text,
                                    file_sha(map, &manifest->as.text));
    universe++;
  }
  for (index = 0; status == ARCHBIRD_OK && index < artifacts->as.array.count;
       index++) {
    const AbValue *artifact = &artifacts->as.array.items[index];
    const AbValue *name = ab_value_member(artifact, "name");
    const AbValue *output = ab_value_member(artifact, "output");
    const AbValue *inputs = ab_value_member(artifact, "inputs");
    const AbString *witness = NULL;
    if (!valid_artifact_row(artifact) || !ab_projection_nonblank(output)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid searchable artifact row");
      break;
    }
    if (inputs->as.array.count) {
      const AbValue *path = ab_value_member(&inputs->as.array.items[0], "path");
      witness = path ? &path->as.text : NULL;
    }
    status = add_search_domain_item(
        context, fact, &project->as.text, "artifact", &output->as.text,
        &name->as.text, NULL, NULL, NULL, 0, index, 0, witness,
        witness ? file_sha(map, witness) : "");
    universe++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "searchable_entity", (uint64_t)universe,
        (uint64_t)universe, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus derived_evidence(ArchbirdEngine *engine,
                                       const AbString *project,
                                       const AbString *path, uint64_t line,
                                       const char *sha256, AbBuffer *detail,
                                       AbProjectionEvidence *out) {
  return ab_projection_evidence_init(engine, out, "derived", project, path,
                                     line, sha256, (const char *)detail->data,
                                     detail->length);
}

static ArchbirdStatus
add_relation_item_state(ArchbirdEngine *engine, AbProjectionData *fact,
                        const AbString *source, const AbString *kind,
                        const AbString *target,
                        const AbProjectionEvidence *evidence, const char *state,
                        const char *message) {
  AbProjectionItem item = {0};
  AbProjectionItem *existing;
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
    ab_projection_item_free(engine, &item);
    return status;
  }
  status = ab_projection_data_find_item(engine, fact, &item.key, &existing);
  if (status != ARCHBIRD_OK) {
    ab_projection_item_free(engine, &item);
    return status;
  }
  if (existing) {
    int existing_rank =
        string_literal(&existing->state, "current")
            ? 2
            : (string_literal(&existing->state, "unknown") ? 1 : 0);
    int incoming_rank = strcmp(state, "current") == 0
                            ? 2
                            : (strcmp(state, "unknown") == 0 ? 1 : 0);
    status = ab_projection_item_add_evidence(engine, existing, evidence);
    if (status == ARCHBIRD_OK && incoming_rank > existing_rank)
      status = ab_projection_item_set_state(engine, existing, state, message);
    ab_projection_item_free(engine, &item);
    return status;
  }
  status = ab_projection_item_add_evidence(engine, &item, evidence);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_add_item(engine, fact, &item);
  if (status != ARCHBIRD_OK)
    ab_projection_item_free(engine, &item);
  return status;
}

static ArchbirdStatus add_relation_item(ArchbirdEngine *engine,
                                        AbProjectionData *fact,
                                        const AbString *source,
                                        const AbString *kind,
                                        const AbString *target,
                                        const AbProjectionEvidence *evidence) {
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

ArchbirdStatus ab_projection_evidence_decode_artifact(
    ArchbirdEngine *engine, const AbValue *row, AbProjectionEvidence *out) {
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
  return ab_projection_evidence_init(
      engine, out, provenance->as.text.data, &project->as.text, &path->as.text,
      line_number, sha->as.text.data, detail->as.text.data,
      detail->as.text.length);
}

static int decode_nullable_count(const AbValue *value, uint64_t *out,
                                 int *present) {
  if (!value)
    return 0;
  if (value->kind == AB_VALUE_NULL) {
    *out = 0;
    *present = 0;
    return 1;
  }
  *present = ab_value_u64(value, out);
  return *present;
}

static ArchbirdStatus decode_projection_completeness(ArchbirdEngine *engine,
                                                     const AbValue *value,
                                                     AbProjectionData *out) {
  static const char *const allowed[] = {
      "classification", "counts", "exhaustive", "truncated", "unit",
  };
  static const char *const count_allowed[] = {
      "evaluated", "excluded", "selected", "unknown", "universe", "unsupported",
  };
  const AbValue *classification = ab_value_member(value, "classification");
  const AbValue *counts = ab_value_member(value, "counts");
  const AbValue *exhaustive = ab_value_member(value, "exhaustive");
  const AbValue *truncated = ab_value_member(value, "truncated");
  const AbValue *unit = ab_value_member(value, "unit");
  AbProjectionCompleteness *selection = &out->selection;
  uint64_t accounted;
  ArchbirdStatus status;
  if (!object_fields_allowed(value, allowed,
                             sizeof(allowed) / sizeof(allowed[0])) ||
      !classification || classification->kind != AB_VALUE_STRING || !counts ||
      !object_fields_allowed(counts, count_allowed,
                             sizeof(count_allowed) /
                                 sizeof(count_allowed[0])) ||
      !exhaustive || exhaustive->kind != AB_VALUE_BOOL ||
      !ab_projection_nonblank(unit) ||
      !decode_nullable_count(ab_value_member(counts, "universe"),
                             &selection->universe, &selection->has_universe) ||
      !decode_nullable_count(ab_value_member(counts, "selected"),
                             &selection->selected, &selection->has_selected) ||
      !decode_nullable_count(ab_value_member(counts, "evaluated"),
                             &selection->evaluated,
                             &selection->has_evaluated) ||
      !decode_nullable_count(ab_value_member(counts, "excluded"),
                             &selection->excluded, &selection->has_excluded) ||
      !decode_nullable_count(ab_value_member(counts, "unsupported"),
                             &selection->unsupported,
                             &selection->has_unsupported) ||
      !decode_nullable_count(ab_value_member(counts, "unknown"),
                             &selection->unknown, &selection->has_unknown) ||
      !truncated ||
      (truncated->kind != AB_VALUE_NULL && truncated->kind != AB_VALUE_BOOL))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid projection completeness ledger");
  selection->has_truncated = truncated->kind == AB_VALUE_BOOL;
  selection->truncated = selection->has_truncated ? truncated->as.boolean : 0;
  if (selection->has_universe && selection->has_selected &&
      selection->has_excluded &&
      (selection->selected > selection->universe ||
       selection->excluded > selection->universe ||
       selection->selected + selection->excluded < selection->selected ||
       selection->selected + selection->excluded != selection->universe))
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "inconsistent projection selection counts");
  if (selection->has_selected && selection->has_evaluated &&
      selection->has_unknown) {
    accounted = selection->evaluated + selection->unknown;
    if (accounted < selection->evaluated || accounted != selection->selected)
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "inconsistent projection evaluation counts");
  }
  ab_string_free(engine, &selection->unit);
  status = ab_string_copy(engine, &selection->unit, unit->as.text.data,
                          unit->as.text.length);
  return status;
}

ArchbirdStatus ab_projection_data_decode_artifact(ArchbirdEngine *engine,
                                                  const AbValue *value,
                                                  AbProjectionData *out) {
  static const char *const envelope_allowed[] = {
      "completeness", "items",  "message", "name",  "project",
      "provenance",   "sha256", "shape",   "state",
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
  const AbValue *completeness = ab_value_member(value, "completeness");
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
      !ab_projection_nonblank(name) || !ab_projection_nonblank(shape) ||
      !valid_fact_provenance(provenance) || !project ||
      project->kind != AB_VALUE_STRING || !valid_fact_state(state) ||
      !message || message->kind != AB_VALUE_STRING || !completeness ||
      !lowercase_sha256_value(sha) || !items || items->kind != AB_VALUE_ARRAY)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid canonical verification fact set");
  status =
      ab_projection_data_init(engine, out, &name->as.text, shape->as.text.data,
                              provenance->as.text.data, &project->as.text);
  if (status == ARCHBIRD_OK && !ab_value_string_is(state, "current")) {
    ab_string_free(engine, &out->state);
    status = ab_string_copy(engine, &out->state, state->as.text.data,
                            state->as.text.length);
  }
  if (status == ARCHBIRD_OK) {
    ab_string_free(engine, &out->message);
    status = ab_string_copy(engine, &out->message, message->as.text.data,
                            message->as.text.length);
  }
  if (status == ARCHBIRD_OK)
    status = decode_projection_completeness(engine, completeness, out);
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
    AbProjectionItem item = {0};
    size_t evidence_index;
    if (!object_fields_allowed(row, item_allowed,
                               sizeof(item_allowed) /
                                   sizeof(item_allowed[0])) ||
        !ab_projection_nonblank(key) || !ab_projection_nonblank(label) ||
        !item_value || !attributes || attributes->kind != AB_VALUE_OBJECT ||
        !valid_fact_state(item_state) || !item_message ||
        item_message->kind != AB_VALUE_STRING || !evidence ||
        evidence->kind != AB_VALUE_ARRAY) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "invalid canonical verification fact item");
      break;
    }
    AbProjectionItem *duplicate = NULL;
    status =
        ab_projection_data_find_item(engine, out, &key->as.text, &duplicate);
    if (status == ARCHBIRD_OK && duplicate)
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "duplicate canonical verification fact key");
    if (status != ARCHBIRD_OK)
      break;
    status = ab_projection_item_init(engine, &item, &key->as.text,
                                     &label->as.text, item_value);
    if (status == ARCHBIRD_OK)
      status = item_copy_attributes(engine, &item, attributes);
    if (status == ARCHBIRD_OK && !ab_value_string_is(item_state, "current"))
      status = ab_projection_item_set_state(
          engine, &item, item_state->as.text.data, item_message->as.text.data);
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
         evidence_index++) {
      AbProjectionEvidence decoded = {0};
      status = ab_projection_evidence_decode_artifact(
          engine, &evidence->as.array.items[evidence_index], &decoded);
      if (status == ARCHBIRD_OK)
        status = ab_projection_item_add_evidence(engine, &item, &decoded);
      ab_projection_evidence_free(engine, &decoded);
    }
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(engine, out, &item);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(engine, out);
  if (status == ARCHBIRD_OK) {
    const AbValue *classification =
        ab_value_member(completeness, "classification");
    const AbValue *exhaustive = ab_value_member(completeness, "exhaustive");
    const char *actual = ab_projection_data_classification(out);
    if (!ab_value_string_is(classification, actual) ||
        exhaustive->as.boolean != (strcmp(actual, "complete") == 0))
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "projection completeness classification "
                                  "does not match its ledger");
  }
  if (status == ARCHBIRD_OK && memcmp(out->sha256, sha->as.text.data, 64) != 0)
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "verification fact SHA-256 does not match content");
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(engine, out);
  return status;
}

static ArchbirdStatus extract_literal(AbProjectionContext *context,
                                      const AbObjectField *operand,
                                      AbProjectionData *fact) {
  const AbValue *spec = &operand->value;
  const AbValue *kind = ab_value_member(spec, "kind");
  const AbValue *values = ab_value_member(spec, "values");
  const AbValue *rows = ab_value_member(spec, "rows");
  AbString empty = {0};
  AbProjectionEvidence evidence = {0};
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &operand->name,
                              fact_shape(kind), "asserted", &empty);
  size_t index;
  if (status == ARCHBIRD_OK)
    status = asserted_evidence(context, operand, &evidence);
  if (status == ARCHBIRD_OK && ab_projection_value_is(kind, "literal_set")) {
    for (index = 0; status == ARCHBIRD_OK && index < values->as.array.count;
         index++) {
      AbProjectionItem item = {0};
      status = ab_projection_item_init(
          context->engine, &item, &values->as.array.items[index].as.text,
          &values->as.array.items[index].as.text, NULL);
      if (status == ARCHBIRD_OK)
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_projection_item_free(context->engine, &item);
    }
  } else if (status == ARCHBIRD_OK &&
             ab_projection_value_is(kind, "literal_values")) {
    for (index = 0; status == ARCHBIRD_OK && index < values->as.object.count;
         index++) {
      const AbObjectField *row = &values->as.object.fields[index];
      AbProjectionItem item = {0};
      status = ab_projection_item_init(context->engine, &item, &row->name,
                                       &row->name, &row->value);
      if (status == ARCHBIRD_OK)
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_projection_item_free(context->engine, &item);
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
      AbProjectionItem item = {0};
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
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_add_item(context->engine, fact, &item);
      if (status != ARCHBIRD_OK)
        ab_projection_item_free(context->engine, &item);
    }
  }
  ab_projection_evidence_free(context->engine, &evidence);
  if (status == ARCHBIRD_OK) {
    size_t universe = ab_projection_value_is(kind, "literal_relation")
                          ? rows->as.array.count
                      : ab_projection_value_is(kind, "literal_values")
                          ? values->as.object.count
                          : values->as.array.count;
    if (fact->item_count > universe) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "literal projection produced too many facts");
    } else {
      status = ab_projection_data_completeness_exact(
          context->engine, fact,
          ab_projection_value_is(kind, "literal_relation") ? "relation"
                                                           : "value",
          (uint64_t)universe, (uint64_t)fact->item_count,
          (uint64_t)(universe - fact->item_count), 0, 0);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus symbol_patterns_match(ArchbirdEngine *engine,
                                            const AbValue *patterns,
                                            const AbString *path,
                                            const AbString *name,
                                            int *matched) {
  AbString leaf = *name;
  AbBuffer qualified;
  size_t index;
  ArchbirdStatus status;
  *matched = 1;
  if (!patterns || !patterns->as.array.count)
    return ARCHBIRD_OK;
  for (index = name->length; index; index--)
    if (name->data[index - 1] == '.') {
      leaf.data += index;
      leaf.length -= index;
      break;
    }
  if (patterns_match(patterns, name) || patterns_match(patterns, &leaf))
    return ARCHBIRD_OK;
  *matched = 0;
  ab_buffer_init(&qualified, engine);
  status = ab_buffer_append(&qualified, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&qualified, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&qualified, name->data, name->length);
  if (status == ARCHBIRD_OK)
    *matched = patterns_match(
        patterns, &(AbString){(char *)qualified.data, qualified.length});
  ab_buffer_free(&qualified);
  return status;
}

static ArchbirdStatus extract_symbols(AbProjectionContext *context,
                                      const AbProjectionPlan *plan,
                                      AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *layer = ab_value_member(spec, "layer");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *kinds = ab_value_member(spec, "kinds");
  const AbValue *names = ab_value_member(spec, "names");
  const AbValue *name_patterns = ab_value_member(spec, "name_patterns");
  const AbValue *public_only = ab_value_member(spec, "public_only");
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "set",
                              "derived", &project_name->as.text);
  size_t file_index;
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
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
    if (!valid_projected_file_row(file)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid projected file/symbol row");
      break;
    }
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
      AbProjectionItem item = {0};
      AbProjectionEvidence evidence = {0};
      AbBuffer detail;
      int name_selected = 0;
      if (!valid_symbol_row(symbol)) {
        status = invalid_map_inventory(
            context, "Map contains an invalid projected symbol row");
        break;
      }
      if (line)
        (void)ab_value_u64(line, &line_number);
      if (kinds && !string_array_has(kinds, &kind->as.text))
        continue;
      if (public_only && public_only->as.boolean &&
          !ab_projection_value_is(scope, "public"))
        continue;
      status =
          symbol_patterns_match(context->engine, name_patterns, &path->as.text,
                                &name->as.text, &name_selected);
      if (status != ARCHBIRD_OK || !name_selected)
        continue;
      status = ab_projection_normalized_name(
          context->engine, spec, &name->as.text, &normalized, &selected);
      if (status != ARCHBIRD_OK || !selected) {
        ab_string_free(context->engine, &normalized);
        continue;
      }
      if (names && !string_array_has(names, &normalized)) {
        ab_string_free(context->engine, &normalized);
        continue;
      }
      status = ab_projection_item_init(context->engine, &item, &normalized,
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
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK) {
        AbProjectionItem *existing = NULL;
        status = ab_projection_data_find_item(context->engine, fact, &item.key,
                                              &existing);
        if (status == ARCHBIRD_OK && existing) {
          size_t evidence_index;
          for (evidence_index = 0;
               status == ARCHBIRD_OK && evidence_index < item.evidence_count;
               evidence_index++)
            status = ab_projection_item_add_evidence(
                context->engine, existing, &item.evidence[evidence_index]);
          if (status == ARCHBIRD_OK)
            status = item_add_symbol_location(context->engine, existing,
                                              &path->as.text, file_index,
                                              symbol_index);
          ab_projection_item_free(context->engine, &item);
        } else if (status == ARCHBIRD_OK) {
          status = item_add_symbol_location(
              context->engine, &item, &path->as.text, file_index, symbol_index);
          if (status == ARCHBIRD_OK)
            status = ab_projection_data_add_item(context->engine, fact, &item);
        }
      }
      ab_buffer_free(&detail);
      ab_projection_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      if (status != ARCHBIRD_OK)
        ab_projection_item_free(context->engine, &item);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "symbol", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_file_metrics(AbProjectionContext *context,
                                           const AbProjectionPlan *plan,
                                           AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *resolution = context->resolution;
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
      ab_projection_data_init(context->engine, fact, &plan->id, "values",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project_name->as.text, "values",
                                      "project map has no file inventory");
  }
  if (!oversized || !ab_value_u64(oversized, &oversized_count)) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "values",
        "project map has no valid oversized-file coverage");
  }
  if (oversized_count && !resolution) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "values",
        "project discovery omitted oversized files; file metrics are "
        "incomplete");
  }
  for (file_index = 0; file_index < files->as.array.count; file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *value = ab_value_member(file, "bytes");
    const AbValue *sha = ab_value_member(file, "sha256");
    uint64_t number;
    if (!ab_projection_path_is_repository(path) || !value ||
        !ab_value_u64(value, &number) || !lowercase_sha256_value(sha)) {
      ab_projection_data_free(context->engine, fact);
      return ab_projection_data_unknown(
          context->engine, fact, &plan->id, &project_name->as.text, "values",
          "project map has an invalid file metric inventory");
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
    AbProjectionItem item = {0};
    AbProjectionEvidence evidence = {0};
    AbBuffer detail;
    status = ab_projection_normalized_name(
        context->engine, spec, &path->as.text, &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status = ab_projection_item_init(context->engine, &item, &normalized,
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
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_projection_evidence_free(context->engine, &evidence);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(context->engine, &item);
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
      AbProjectionItem item = {0};
      AbProjectionEvidence evidence = {0};
      AbBuffer detail;
      if (!ab_projection_value_is(code, "discovery-file-oversized"))
        continue;
      source_oversized_count++;
      status = ab_projection_normalized_name(
          context->engine, spec, &path->as.text, &normalized, &selected);
      if (status != ARCHBIRD_OK || !selected) {
        ab_string_free(context->engine, &normalized);
        continue;
      }
      status = ab_projection_item_init(context->engine, &item, &normalized,
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
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_add_item(context->engine, fact, &item);
      ab_buffer_free(&detail);
      ab_projection_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      if (status != ARCHBIRD_OK)
        ab_projection_item_free(context->engine, &item);
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
      status = ab_projection_data_completeness_exact(
          context->engine, fact, "mapped_source_file", universe,
          (uint64_t)fact->item_count, universe - (uint64_t)fact->item_count, 0,
          0);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_inventory_paths(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *resolution = context->resolution;
  const AbValue *inventory =
      resolution ? ab_value_member(resolution, "inventory") : NULL;
  const AbValue *resolution_sha =
      resolution ? ab_value_member(resolution, "sha256") : NULL;
  size_t index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "set",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!inventory || inventory->kind != AB_VALUE_ARRAY ||
      !lowercase_sha256_value(resolution_sha)) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "set",
        "project discovery has no exhaustive repository inventory");
  }
  for (index = 0; status == ARCHBIRD_OK && index < inventory->as.array.count;
       index++) {
    const AbValue *row = &inventory->as.array.items[index];
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *bytes = ab_value_member(row, "bytes");
    uint64_t byte_count = 0;
    AbString normalized = {0};
    int selected = 0;
    AbProjectionItem item = {0};
    AbProjectionEvidence evidence = {0};
    AbBuffer detail;
    if (!ab_projection_path_is_repository(path) ||
        !ab_value_u64(bytes, &byte_count)) {
      status = archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "project discovery contains an invalid inventory row");
      break;
    }
    status = ab_projection_normalized_name(
        context->engine, spec, &path->as.text, &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status = ab_projection_item_init(context->engine, &item, &normalized,
                                     &path->as.text, NULL);
    ab_buffer_init(&detail, context->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, "repository inventory bytes=");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&detail, bytes);
    if (status == ARCHBIRD_OK)
      status = derived_evidence(context->engine, &project_name->as.text,
                                &path->as.text, 0, resolution_sha->as.text.data,
                                &detail, &evidence);
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_projection_evidence_free(context->engine, &evidence);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "repository_file",
        (uint64_t)inventory->as.array.count, (uint64_t)fact->item_count,
        (uint64_t)inventory->as.array.count - (uint64_t)fact->item_count, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_mapped_paths(AbProjectionContext *context,
                                           const AbProjectionPlan *plan,
                                           AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *files = map ? ab_value_member(map, "files") : NULL;
  const AbValue *paths = ab_value_member(spec, "paths");
  size_t index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "set",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!files || files->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "set",
        "project map has no mapped file inventory");
  }
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbValue *path = ab_value_member(file, "path");
    const AbValue *sha = ab_value_member(file, "sha256");
    AbString normalized = {0};
    int selected = 0;
    AbProjectionItem item = {0};
    AbProjectionEvidence evidence = {0};
    AbBuffer detail;
    if (!ab_projection_path_is_repository(path) ||
        !lowercase_sha256_value(sha)) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "Map contains an invalid mapped file row");
      break;
    }
    if (!path_matches(&path->as.text, paths))
      continue;
    status = ab_projection_normalized_name(
        context->engine, spec, &path->as.text, &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status = ab_projection_item_init(context->engine, &item, &normalized,
                                     &path->as.text, NULL);
    ab_buffer_init(&detail, context->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&detail, "mapped source file");
    if (status == ARCHBIRD_OK)
      status = derived_evidence(context->engine, &project_name->as.text,
                                &path->as.text, 0, sha->as.text.data, &detail,
                                &evidence);
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_projection_evidence_free(context->engine, &evidence);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "mapped_source_file",
        (uint64_t)files->as.array.count, (uint64_t)fact->item_count,
        (uint64_t)(files->as.array.count - fact->item_count), 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_component_membership(AbProjectionContext *context,
                                                   const AbProjectionPlan *plan,
                                                   AbProjectionData *fact) {
  static const AbString unassigned = {(char *)"unassigned", 10};
  static const AbString exclusive = {(char *)"exclusive", 9};
  static const AbString overlap = {(char *)"overlap", 7};
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *components = ab_value_member(spec, "components");
  AbProjectionMembershipIndex index = {0};
  size_t file_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "values",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_projection_membership_index_build(context->engine, map, &index);
  if (status != ARCHBIRD_OK) {
    ab_projection_data_free(context->engine, fact);
    return status;
  }
  if (!index.current) {
    const char *message = index.message;
    ab_projection_membership_index_free(context->engine, &index);
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "values",
        message ? message : "component membership is unavailable");
  }
  for (file_index = 0; status == ARCHBIRD_OK && file_index < index.file_count;
       file_index++) {
    const AbProjectionMembershipFile *file = &index.files[file_index];
    const AbValue *sha = ab_value_member(file->row, "sha256");
    const AbValue *layer = ab_value_member(file->row, "layer");
    const AbValue *language = ab_value_member(file->row, "language");
    const AbString *membership = !file->assignment_count       ? &unassigned
                                 : file->assignment_count == 1 ? &exclusive
                                                               : &overlap;
    AbString normalized = {0};
    int selected = 0;
    AbValue count = {0};
    AbProjectionItem item = {0};
    AbProjectionEvidence evidence = {0};
    AbBuffer detail;
    int component_selected = !components || !components->as.array.count;
    size_t component_offset;
    for (component_offset = 0;
         !component_selected && component_offset < file->assignment_count;
         component_offset++) {
      const AbProjectionMembershipAssignment *assignment =
          &index.assignments[file->assignment_start + component_offset];
      component_selected = patterns_match(
          components, index.components[assignment->component_index].name);
    }
    if (!component_selected)
      continue;
    status = ab_projection_normalized_name(context->engine, spec, file->path,
                                           &normalized, &selected);
    if (status != ARCHBIRD_OK || !selected) {
      ab_string_free(context->engine, &normalized);
      continue;
    }
    status =
        u64_value(context->engine, (uint64_t)file->assignment_count, &count);
    if (status == ARCHBIRD_OK)
      status = ab_projection_item_init(context->engine, &item, &normalized,
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
        const AbProjectionMembershipAssignment *assignment =
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
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    ab_buffer_free(&detail);
    ab_projection_evidence_free(context->engine, &evidence);
    ab_value_free(context->engine, &count);
    ab_string_free(context->engine, &normalized);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(context->engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "mapped_source_file", (uint64_t)index.file_count,
        (uint64_t)fact->item_count,
        (uint64_t)(index.file_count - fact->item_count), 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  ab_projection_membership_index_free(context->engine, &index);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus edge_evidence(AbProjectionContext *context,
                                    const AbString *project_name,
                                    const AbValue *map, const AbValue *edge,
                                    AbProjectionEvidence *out,
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
  const AbValue *kind_patterns = ab_value_member(spec, "kind_patterns");
  const AbValue *name_patterns = ab_value_member(spec, "name_patterns");
  const AbValue *from = ab_value_member(spec, "from_paths");
  const AbValue *to = ab_value_member(spec, "to_paths");
  const AbValue *kind = ab_value_member(edge, "kind");
  const AbValue *source = ab_value_member(edge, "source");
  const AbValue *target = ab_value_member(edge, "target");
  const AbValue *names = ab_value_member(edge, "names");
  size_t index;
  int name_match = !name_patterns || !name_patterns->as.array.count;
  if (name_patterns && names && names->kind == AB_VALUE_ARRAY)
    for (index = 0; !name_match && index < names->as.array.count; index++)
      name_match =
          patterns_match(name_patterns, &names->as.array.items[index].as.text);
  return kind && source && target && name_match &&
         (!kinds || string_array_has(kinds, &kind->as.text)) &&
         (!kind_patterns || patterns_match(kind_patterns, &kind->as.text)) &&
         path_matches(&source->as.text, from) &&
         path_matches(&target->as.text, to);
}

static int package_matches(const AbValue *package, const AbValue *patterns) {
  const AbValue *name = ab_value_member(package, "name");
  const AbValue *identity = ab_value_member(package, "identity");
  const AbValue *aliases = ab_value_member(package, "aliases");
  size_t index;
  if (!patterns || !patterns->as.array.count)
    return 1;
  if ((name && name->kind == AB_VALUE_STRING &&
       patterns_match(patterns, &name->as.text)) ||
      (identity && identity->kind == AB_VALUE_STRING &&
       patterns_match(patterns, &identity->as.text)))
    return 1;
  if (aliases && aliases->kind == AB_VALUE_ARRAY)
    for (index = 0; index < aliases->as.array.count; index++)
      if (aliases->as.array.items[index].kind == AB_VALUE_STRING &&
          patterns_match(patterns, &aliases->as.array.items[index].as.text))
        return 1;
  return 0;
}

static ArchbirdStatus extract_package_entrypoints(AbProjectionContext *context,
                                                  const AbProjectionPlan *plan,
                                                  AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *packages = map ? ab_value_member(map, "packages") : NULL;
  const AbValue *package_patterns = ab_value_member(spec, "packages");
  const AbValue *route_patterns = ab_value_member(spec, "routes");
  const AbValue *target_patterns = ab_value_member(spec, "target_paths");
  size_t package_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!packages || packages->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project_name->as.text, "relation",
                                      "project map has no package inventory");
  }
  for (package_index = 0;
       status == ARCHBIRD_OK && package_index < packages->as.array.count;
       package_index++) {
    const AbValue *package = &packages->as.array.items[package_index];
    const AbValue *name = ab_value_member(package, "name");
    const AbValue *manifest = ab_value_member(package, "manifest");
    const AbValue *entrypoints = ab_value_member(package, "entrypoints");
    size_t route_index;
    if (!valid_package_row(package, 0)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid package entrypoint row");
      break;
    }
    if (!package_matches(package, package_patterns))
      continue;
    for (route_index = 0;
         status == ARCHBIRD_OK && route_index < entrypoints->as.object.count;
         route_index++) {
      const AbObjectField *route = &entrypoints->as.object.fields[route_index];
      AbProjectionEvidence evidence = {0};
      AbBuffer detail;
      if ((route_patterns && !patterns_match(route_patterns, &route->name)) ||
          !path_matches(&route->value.as.text, target_patterns))
        continue;
      ab_buffer_init(&detail, context->engine);
      status = ab_buffer_literal(&detail, "package entrypoint ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&detail, route->name.data, route->name.length);
      if (status == ARCHBIRD_OK)
        status = derived_evidence(
            context->engine, &project_name->as.text, &manifest->as.text, 0,
            file_sha(map, &manifest->as.text), &detail, &evidence);
      if (status == ARCHBIRD_OK)
        status =
            add_relation_item(context->engine, fact, &name->as.text,
                              &route->name, &route->value.as.text, &evidence);
      ab_buffer_free(&detail);
      ab_projection_evidence_free(context->engine, &evidence);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "package_entrypoint", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_package_exports(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *packages = map ? ab_value_member(map, "packages") : NULL;
  const AbValue *package_patterns = ab_value_member(spec, "packages");
  const AbValue *name_patterns = ab_value_member(spec, "name_patterns");
  size_t package_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!packages || packages->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project->as.text, "relation",
                                      "project map has no package inventory");
  }
  for (package_index = 0;
       status == ARCHBIRD_OK && package_index < packages->as.array.count;
       package_index++) {
    const AbValue *package = &packages->as.array.items[package_index];
    const AbValue *name = ab_value_member(package, "name");
    const AbValue *origins = ab_value_member(package, "export_origins");
    size_t export_index;
    if (!valid_package_row(package, 1)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid package export row");
      break;
    }
    if (!package_matches(package, package_patterns))
      continue;
    for (export_index = 0;
         status == ARCHBIRD_OK && export_index < origins->as.object.count;
         export_index++) {
      const AbObjectField *exported = &origins->as.object.fields[export_index];
      size_t path_index;
      if (name_patterns && !patterns_match(name_patterns, &exported->name))
        continue;
      for (path_index = 0;
           status == ARCHBIRD_OK && path_index < exported->value.as.array.count;
           path_index++) {
        const AbValue *path = &exported->value.as.array.items[path_index];
        AbProjectionEvidence evidence = {0};
        AbBuffer detail;
        ab_buffer_init(&detail, context->engine);
        status = ab_buffer_literal(&detail, "package export ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&detail, exported->name.data,
                                    exported->name.length);
        if (status == ARCHBIRD_OK)
          status = derived_evidence(
              context->engine, &project->as.text, &path->as.text, 0,
              file_sha(map, &path->as.text), &detail, &evidence);
        if (status == ARCHBIRD_OK)
          status =
              add_relation_item(context->engine, fact, &name->as.text,
                                &exported->name, &path->as.text, &evidence);
        ab_buffer_free(&detail);
        ab_projection_evidence_free(context->engine, &evidence);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "package_export", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static int artifact_selected(const AbValue *artifact, const AbValue *patterns) {
  const AbValue *name = ab_value_member(artifact, "name");
  const AbValue *output = ab_value_member(artifact, "output");
  const AbValue *loaders = ab_value_member(artifact, "loaded_by");
  size_t index;
  if (!patterns || !patterns->as.array.count)
    return 1;
  if ((name && name->kind == AB_VALUE_STRING &&
       patterns_match(patterns, &name->as.text)) ||
      (output && output->kind == AB_VALUE_STRING &&
       path_matches(&output->as.text, patterns)))
    return 1;
  if (loaders && loaders->kind == AB_VALUE_ARRAY)
    for (index = 0; index < loaders->as.array.count; index++) {
      const AbValue *path =
          ab_value_member(&loaders->as.array.items[index], "path");
      if (path && path->kind == AB_VALUE_STRING &&
          path_matches(&path->as.text, patterns))
        return 1;
    }
  return 0;
}

static ArchbirdStatus extract_artifact_routes(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *artifacts = map ? ab_value_member(map, "artifacts") : NULL;
  const AbValue *patterns = ab_value_member(spec, "artifacts");
  static const char *const fields[] = {"inputs", "loaded_by"};
  static const char *const kinds[] = {"input", "loader"};
  size_t artifact_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!artifacts || artifacts->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project->as.text, "relation",
                                      "project map has no artifact inventory");
  }
  for (artifact_index = 0;
       status == ARCHBIRD_OK && artifact_index < artifacts->as.array.count;
       artifact_index++) {
    const AbValue *artifact = &artifacts->as.array.items[artifact_index];
    const AbValue *name = ab_value_member(artifact, "name");
    size_t role;
    if (!valid_artifact_row(artifact)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid artifact route row");
      break;
    }
    if (!artifact_selected(artifact, patterns))
      continue;
    for (role = 0; status == ARCHBIRD_OK && role < 2; role++) {
      const AbValue *rows = ab_value_member(artifact, fields[role]);
      AbString kind = {(char *)kinds[role], strlen(kinds[role])};
      size_t row_index;
      for (row_index = 0;
           status == ARCHBIRD_OK && row_index < rows->as.array.count;
           row_index++) {
        const AbValue *path =
            ab_value_member(&rows->as.array.items[row_index], "path");
        AbProjectionEvidence evidence = {0};
        AbBuffer detail;
        ab_buffer_init(&detail, context->engine);
        status = ab_buffer_literal(&detail, "artifact ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&detail, kind.data, kind.length);
        if (status == ARCHBIRD_OK)
          status = derived_evidence(
              context->engine, &project->as.text, &path->as.text, 0,
              file_sha(map, &path->as.text), &detail, &evidence);
        if (status == ARCHBIRD_OK)
          status = add_relation_item(context->engine, fact, &name->as.text,
                                     &kind, &path->as.text, &evidence);
        ab_buffer_free(&detail);
        ab_projection_evidence_free(context->engine, &evidence);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "artifact_route", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_file_edges(AbProjectionContext *context,
                                         const AbProjectionPlan *plan,
                                         AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *edges = map ? ab_value_member(map, "edges") : NULL;
  size_t index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!edges || edges->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project_name->as.text, "relation",
                                      "project map has no edge inventory");
  }
  for (index = 0; status == ARCHBIRD_OK && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *source;
    const AbValue *kind;
    const AbValue *target;
    AbProjectionEvidence evidence = {0};
    const char *evidence_state;
    if (!valid_edge_row(edge)) {
      status = invalid_map_inventory(context,
                                     "Map contains an invalid file edge row");
      break;
    }
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
    ab_projection_evidence_free(context->engine, &evidence);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "relation", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_component_edges(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *edges = map ? ab_value_member(map, "edges") : NULL;
  const AbValue *kinds = ab_value_member(spec, "kinds");
  AbProjectionMembershipIndex membership = {0};
  size_t edge_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!edges || edges->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "relation",
        "project map has no component/edge inventory");
  }
  status =
      ab_projection_membership_index_build(context->engine, map, &membership);
  if (status != ARCHBIRD_OK) {
    ab_projection_data_free(context->engine, fact);
    return status;
  }
  if (!membership.current) {
    const char *message = membership.message;
    ab_projection_membership_index_free(context->engine, &membership);
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "relation",
        message ? message : "component membership evidence is unavailable");
  }
  for (edge_index = 0;
       status == ARCHBIRD_OK && edge_index < edges->as.array.count;
       edge_index++) {
    const AbValue *edge = &edges->as.array.items[edge_index];
    const AbValue *source = ab_value_member(edge, "source");
    const AbValue *target = ab_value_member(edge, "target");
    const AbValue *kind = ab_value_member(edge, "kind");
    const AbProjectionMembershipFile *source_file;
    const AbProjectionMembershipFile *target_file;
    AbProjectionEvidence evidence = {0};
    const char *evidence_state = "current";
    size_t source_offset;
    if (!valid_edge_row(edge)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid component edge row");
      break;
    }
    if (kinds && !string_array_has(kinds, &kind->as.text))
      continue;
    source_file = ab_projection_membership_file(&membership, &source->as.text);
    target_file = ab_projection_membership_file(&membership, &target->as.text);
    if (!source_file || !target_file || !source_file->assignment_count ||
        !target_file->assignment_count)
      continue;
    status = edge_evidence(context, &project_name->as.text, map, edge,
                           &evidence, &evidence_state);
    for (source_offset = 0;
         status == ARCHBIRD_OK && source_offset < source_file->assignment_count;
         source_offset++) {
      const AbProjectionMembershipAssignment *source_assignment =
          &membership
               .assignments[source_file->assignment_start + source_offset];
      const AbString *source_name =
          membership.components[source_assignment->component_index].name;
      size_t target_offset;
      for (target_offset = 0; status == ARCHBIRD_OK &&
                              target_offset < target_file->assignment_count;
           target_offset++) {
        const AbProjectionMembershipAssignment *target_assignment =
            &membership
                 .assignments[target_file->assignment_start + target_offset];
        const AbString *target_name =
            membership.components[target_assignment->component_index].name;
        if (ab_string_equal(source_name, target_name))
          continue;
        status = add_relation_item_state(
            context->engine, fact, source_name, &kind->as.text, target_name,
            &evidence, evidence_state,
            strcmp(evidence_state, "current") == 0
                ? ""
                : (strcmp(evidence_state, "unknown") == 0
                       ? "edge freshness is unknown"
                       : "edge evidence is stale"));
      }
    }
    ab_projection_evidence_free(context->engine, &evidence);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "relation", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  ab_projection_membership_index_free(context->engine, &membership);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static int selector_matches(const AbString *selector, const AbValue *patterns) {
  return !patterns || !patterns->as.array.count ||
         patterns_match(patterns, selector);
}

static ArchbirdStatus
test_route_evidence(AbProjectionContext *context, const AbString *project,
                    const AbString *path, uint64_t line, const char *sha,
                    const AbString *selector, const AbString *target,
                    const char *kind, AbProjectionEvidence *out) {
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
    AbProjectionContext *context, const AbString *project,
    const AbString *selector, const AbString *target,
    AbProjectionEvidence *out) {
  const AbValue *map = context->map;
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
    status = ab_projection_evidence_init(
        context->engine, out, "asserted", project, &empty, 0,
        config_sha->as.text.data, (const char *)detail.data, detail.length);
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus
add_test_route_relation(AbProjectionContext *context, AbProjectionData *fact,
                        const AbString *project, const AbString *path,
                        uint64_t line, const char *sha,
                        const AbString *selector, const AbString *source,
                        const AbString *target, const char *kind) {
  AbString kind_string = {(char *)kind, strlen(kind)};
  AbProjectionEvidence source_evidence = {0};
  AbProjectionEvidence assertion_evidence = {0};
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
  ab_projection_evidence_free(context->engine, &source_evidence);
  ab_projection_evidence_free(context->engine, &assertion_evidence);
  return status;
}

static ArchbirdStatus add_case_routes(AbProjectionContext *context,
                                      AbProjectionData *fact,
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
  const AbValue *target_patterns = ab_value_member(spec, "target_paths");
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
    if (!path_matches(target, target_patterns))
      continue;
    status = add_test_route_relation(
        context, fact, project, &path->as.text, line_number, sha,
        &selector->as.text, &(AbString){(char *)source.data, source.length},
        target, kind);
  }
  for (route_index = 0; status == ARCHBIRD_OK && configured &&
                        route_index < configured->as.array.count;
       route_index++) {
    const AbString *target = &configured->as.array.items[route_index].as.text;
    if (!path_matches(target, target_patterns))
      continue;
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

static ArchbirdStatus extract_test_routes(AbProjectionContext *context,
                                          const AbProjectionPlan *plan,
                                          AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *tests = map ? ab_value_member(map, "tests") : NULL;
  const AbValue *wanted_group = ab_value_member(spec, "group");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *selectors = ab_value_member(spec, "selectors");
  size_t test_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "relation",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!tests || tests->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
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
    if (!valid_test_row(test)) {
      status = invalid_map_inventory(context,
                                     "Map contains an invalid test route row");
      break;
    }
    if (wanted_group && wanted_group->as.text.length &&
        !ab_string_equal(&wanted_group->as.text, &group->as.text))
      continue;
    if (!path_matches(&path->as.text, paths))
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
      const AbValue *target_patterns = ab_value_member(spec, "target_paths");
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
        AbProjectionEvidence evidence = {0};
        if (!path_matches(target, target_patterns))
          continue;
        status = test_route_evidence(context, &project_name->as.text,
                                     &path->as.text, 0, sha, &empty_selector,
                                     target, "static", &evidence);
        if (status == ARCHBIRD_OK)
          status =
              add_relation_item(context->engine, fact,
                                &(AbString){(char *)source.data, source.length},
                                &static_kind, target, &evidence);
        ab_projection_evidence_free(context->engine, &evidence);
      }
      ab_buffer_free(&source);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "test_route", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_test_selectors(AbProjectionContext *context,
                                             const AbProjectionPlan *plan,
                                             AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *map = context->map;
  const AbValue *tests = map ? ab_value_member(map, "tests") : NULL;
  const AbValue *wanted_group = ab_value_member(spec, "group");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *selectors = ab_value_member(spec, "selectors");
  size_t test_index;
  ArchbirdStatus status =
      ab_projection_data_init(context->engine, fact, &plan->id, "set",
                              "derived", &project_name->as.text);
  if (status != ARCHBIRD_OK)
    return status;
  if (!tests || tests->kind != AB_VALUE_ARRAY) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
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
    if (!valid_test_row(test)) {
      status = invalid_map_inventory(
          context, "Map contains an invalid test selector row");
      break;
    }
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
      AbProjectionItem item = {0};
      AbProjectionEvidence evidence = {0};
      int selected = 0;
      if (!valid_test_case_row(test_case)) {
        status = invalid_map_inventory(context,
                                       "Map contains an invalid test case row");
        break;
      }
      if (line)
        (void)ab_value_u64(line, &line_number);
      if (!selector_matches(&selector->as.text, selectors))
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
        status = ab_projection_normalized_name(
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
        AbProjectionItem *previous = NULL;
        status = ab_projection_data_find_item(context->engine, fact,
                                              &normalized, &previous);
        if (status == ARCHBIRD_OK && previous) {
          status = ab_projection_item_add_evidence(context->engine, previous,
                                                   &evidence);
        } else if (status == ARCHBIRD_OK) {
          status = ab_projection_item_init(context->engine, &item, &normalized,
                                           &normalized, NULL);
          if (status == ARCHBIRD_OK)
            status = ab_projection_item_add_evidence(context->engine, &item,
                                                     &evidence);
          if (status == ARCHBIRD_OK)
            status = ab_projection_data_add_item(context->engine, fact, &item);
        }
      }
      ab_projection_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &normalized);
      ab_buffer_free(&detail);
      ab_buffer_free(&identity);
      if (status != ARCHBIRD_OK && selected)
        ab_projection_item_free(context->engine, &item);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "test_selector", (uint64_t)fact->item_count,
        (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
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
  int declared = ab_projection_value_is(declaration, "declared");
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

static ArchbirdStatus current_path_hash(AbProjectionContext *context,
                                        const AbString *project,
                                        const AbValue *map,
                                        const AbString *path, char output[65],
                                        int *current) {
  const AbValue *file = map_file(map, path);
  const AbValue *input = file ? file : map_input(map, path);
  const AbValue *sha = input ? ab_value_member(input, "sha256") : NULL;
  (void)context;
  (void)project;
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

static ArchbirdStatus extract_provider_surface(AbProjectionContext *context,
                                               const AbProjectionPlan *plan,
                                               AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *project_name = ab_value_member(context->map, "project");
  const AbValue *wanted = ab_value_member(spec, "name");
  const AbValue *map = context->map;
  const AbValue *surfaces = map ? ab_value_member(map, "surfaces") : NULL;
  const AbValue *surface = NULL;
  const AbValue *surface_name;
  const AbValue *surface_kind;
  const AbValue *names;
  size_t index;
  ArchbirdStatus status;
  if (!surfaces || surfaces->kind != AB_VALUE_ARRAY)
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project_name->as.text, "values",
        "project Map has no provider-surface inventory");
  for (index = 0; index < surfaces->as.array.count; index++)
    if (!valid_surface_row(&surfaces->as.array.items[index]))
      return invalid_map_inventory(
          context, "Map contains an invalid provider-surface row");
  surface = find_surface(map, &wanted->as.text);
  surface_name = surface ? ab_value_member(surface, "name") : NULL;
  surface_kind = surface ? ab_value_member(surface, "kind") : NULL;
  names = surface ? ab_value_member(surface, "names") : NULL;
  if (!surface || !names || names->kind != AB_VALUE_ARRAY) {
    char message[512];
    (void)snprintf(message, sizeof(message),
                   "project %.*s: provider surface '%.*s' is absent",
                   (int)project_name->as.text.length,
                   project_name->as.text.data, (int)wanted->as.text.length,
                   wanted->as.text.data);
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project_name->as.text, "values",
                                      message);
  }
  status = ab_projection_data_init(context->engine, fact, &plan->id, "values",
                                   "derived", &project_name->as.text);
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
    AbProjectionItem item = {0};
    AbBuffer missing;
    int missing_count = 0;
    if (ignored->as.boolean)
      continue;
    status = ab_projection_normalized_name(
        context->engine, spec, &name->as.text, &normalized, &selected);
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
      status = ab_projection_item_init(context->engine, &item, &normalized,
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
      AbProjectionEvidence evidence = {0};
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
        status = ab_projection_evidence_init(
            context->engine, &evidence, "derived", &project_name->as.text,
            path->path, 0, sha, detail.data, detail.length);
      if (status == ARCHBIRD_OK)
        status =
            ab_projection_item_add_evidence(context->engine, &item, &evidence);
      ab_projection_evidence_free(context->engine, &evidence);
      ab_string_free(context->engine, &detail);
    }
    if (status == ARCHBIRD_OK && missing_count) {
      AbBuffer message;
      ab_buffer_init(&message, context->engine);
      status = ab_buffer_literal(&message, "current bytes unavailable for: ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, missing.data, missing.length);
      if (status == ARCHBIRD_OK)
        status = ab_projection_item_set_state(context->engine, &item, "unknown",
                                              (const char *)message.data);
      ab_buffer_free(&message);
    }
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    ab_buffer_free(&missing);
    ab_projection_item_free(context->engine, &item);
    ab_value_free(context->engine, &value);
    ab_string_free(context->engine, &normalized);
    surface_paths_free(context->engine, paths, path_count);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "provider_capability",
        (uint64_t)fact->item_count, (uint64_t)fact->item_count, 0, 0, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static const AbValue *map_fact_attribute(const AbValue *row, const char *name) {
  const AbValue *attributes = ab_value_member(row, "attributes");
  return attributes && attributes->kind == AB_VALUE_OBJECT
             ? ab_value_member(attributes, name)
             : NULL;
}

static ArchbirdStatus validate_map_fact_row(AbProjectionContext *context,
                                            const AbValue *map,
                                            const AbValue *row) {
  const AbValue *attributes = ab_value_member(row, "attributes");
  const AbValue *claim = ab_value_member(row, "claim");
  const AbValue *domain = ab_value_member(row, "domain");
  const AbValue *id = ab_value_member(row, "id");
  const AbValue *key = ab_value_member(row, "key");
  const AbValue *kind = ab_value_member(row, "kind");
  const AbValue *path = ab_value_member(row, "path");
  const AbValue *provider = ab_value_member(row, "provider");
  const AbValue *span = ab_value_member(row, "span");
  uint64_t start;
  uint64_t end;
  if (!attributes || attributes->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(claim) || !ab_projection_nonblank(domain) ||
      !ab_projection_nonblank(id) || !ab_projection_nonblank(key) ||
      !ab_projection_nonblank(kind) || !ab_projection_nonblank(path) ||
      !provider || provider->kind != AB_VALUE_OBJECT ||
      !ab_projection_nonblank(ab_value_member(provider, "name")) ||
      !lowercase_sha256_value(
          ab_value_member(provider, "implementation_sha256")) ||
      !span || span->kind != AB_VALUE_OBJECT ||
      !ab_value_u64(ab_value_member(span, "start"), &start) ||
      !ab_value_u64(ab_value_member(span, "end"), &end) || start > end ||
      !map_file(map, &path->as.text))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "Map contains an invalid projected fact row");
  return ARCHBIRD_OK;
}

static ArchbirdStatus map_fact_evidence(AbProjectionContext *context,
                                        const AbValue *map,
                                        const AbString *project,
                                        const AbValue *row,
                                        AbProjectionEvidence *out) {
  const AbValue *path = ab_value_member(row, "path");
  const AbValue *domain = ab_value_member(row, "domain");
  const AbValue *kind = ab_value_member(row, "kind");
  const AbValue *provider = ab_value_member(row, "provider");
  const AbValue *provider_name = ab_value_member(provider, "name");
  const AbValue *line = map_fact_attribute(row, "line");
  AbBuffer detail;
  uint64_t line_number = 0;
  ArchbirdStatus status;
  if (line && !ab_value_u64(line, &line_number))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "Map fact line is not a nonnegative integer");
  ab_buffer_init(&detail, context->engine);
  status =
      ab_buffer_append(&detail, domain->as.text.data, domain->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&detail, kind->as.text.data, kind->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " from ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, provider_name->as.text.data,
                              provider_name->as.text.length);
  if (status == ARCHBIRD_OK)
    status =
        derived_evidence(context->engine, project, &path->as.text, line_number,
                         file_sha(map, &path->as.text), &detail, out);
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus
add_projected_map_item(AbProjectionContext *context, AbProjectionData *fact,
                       const AbValue *map, const AbValue *spec,
                       const AbValue *row, const AbValue *value,
                       const char *state, const char *message,
                       uint64_t *selected, uint64_t *excluded) {
  const AbValue *name = ab_value_member(row, "name");
  const AbValue *project = ab_value_member(map, "project");
  AbString normalized = {0};
  AbProjectionItem item = {0};
  AbProjectionItem *existing;
  AbProjectionEvidence evidence = {0};
  int wanted = 0;
  ArchbirdStatus status;
  if (!ab_projection_nonblank(name))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "projected Map fact has no stable name");
  status = ab_projection_normalized_name(context->engine, spec, &name->as.text,
                                         &normalized, &wanted);
  if (status != ARCHBIRD_OK)
    return status;
  if (!wanted) {
    (*excluded)++;
    ab_string_free(context->engine, &normalized);
    return ARCHBIRD_OK;
  }
  (*selected)++;
  status = map_fact_evidence(context, map, &project->as.text, row, &evidence);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_find_item(context->engine, fact, &normalized,
                                          &existing);
  if (status == ARCHBIRD_OK && existing) {
    status =
        ab_projection_item_add_evidence(context->engine, existing, &evidence);
    if (status == ARCHBIRD_OK &&
        (!ab_value_equal(&existing->value, value) || strcmp(state, "current")))
      status = ab_projection_item_set_state(
          context->engine, existing, "unknown",
          "multiple or incomplete Map facts share one normalized identity");
  } else if (status == ARCHBIRD_OK) {
    status = ab_projection_item_init(context->engine, &item, &normalized,
                                     &normalized, value);
    if (status == ARCHBIRD_OK && strcmp(state, "current"))
      status =
          ab_projection_item_set_state(context->engine, &item, state, message);
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_item_add_evidence(context->engine, &item, &evidence);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(context->engine, fact, &item);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(context->engine, &item);
  }
  ab_projection_evidence_free(context->engine, &evidence);
  ab_string_free(context->engine, &normalized);
  return status;
}

static int fact_container_matches(const AbValue *row,
                                  const AbValue *container) {
  const AbValue *actual = map_fact_attribute(row, "container");
  return actual && actual->kind == AB_VALUE_STRING && container &&
         container->kind == AB_VALUE_STRING &&
         ab_string_equal(&actual->as.text, &container->as.text);
}

static ArchbirdStatus extract_constant_values(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *map = context->map;
  const AbValue *project = map ? ab_value_member(map, "project") : NULL;
  const AbValue *rows = map ? ab_value_member(map, "facts") : NULL;
  const AbValue *container = ab_value_member(spec, "container");
  const AbValue *paths = ab_value_member(spec, "paths");
  const AbValue *kinds = ab_value_member(spec, "kinds");
  uint64_t universe = 0;
  uint64_t selected = 0;
  uint64_t excluded = 0;
  uint64_t unsupported = 0;
  size_t index;
  ArchbirdStatus status;
  if (!project || !ab_projection_nonblank(container))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "constant_values requires a container");
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project->as.text, "values",
                                      "Map has no canonical source facts");
  status = ab_projection_data_init(context->engine, fact, &plan->id, "values",
                                   "derived", &project->as.text);
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *domain = ab_value_member(row, "domain");
    const AbValue *kind = ab_value_member(row, "kind");
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *state;
    const AbValue *value;
    const AbValue *expression;
    status = validate_map_fact_row(context, map, row);
    if (status != ARCHBIRD_OK)
      break;
    if (!ab_projection_value_is(domain, "constant-values"))
      continue;
    if (!fact_container_matches(row, container) ||
        !path_matches(&path->as.text, paths) ||
        (kinds && !string_array_has(kinds, &kind->as.text)))
      continue;
    universe++;
    state = map_fact_attribute(row, "state");
    value = map_fact_attribute(row, "value");
    expression = map_fact_attribute(row, "expression");
    if (!ab_projection_value_is(state, "current") &&
        !ab_projection_value_is(state, "unknown")) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "constant value has an invalid state");
      break;
    }
    if (!value)
      value = expression;
    if (!value) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "constant value has no value or expression");
      break;
    }
    {
      uint64_t selected_before = selected;
      status = add_projected_map_item(
          context, fact, map, spec, row, value,
          ab_projection_value_is(state, "current") ? "current" : "unknown",
          ab_projection_value_is(state, "current")
              ? ""
              : "constant expression is unresolved",
          &selected, &excluded);
      if (status == ARCHBIRD_OK && ab_projection_value_is(state, "unknown") &&
          selected > selected_before)
        unsupported++;
    }
  }
  if (status == ARCHBIRD_OK && !universe) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project->as.text, "values",
        "constant container is absent from Map facts");
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "constant_value", universe, selected, excluded,
        unsupported, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_constant_memberships(AbProjectionContext *context,
                                                   const AbProjectionPlan *plan,
                                                   AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *map = context->map;
  const AbValue *project = map ? ab_value_member(map, "project") : NULL;
  const AbValue *rows = map ? ab_value_member(map, "facts") : NULL;
  const AbValue *container = ab_value_member(spec, "container");
  const AbValue *paths = ab_value_member(spec, "paths");
  uint64_t universe = 0;
  uint64_t selected = 0;
  uint64_t excluded = 0;
  uint64_t unsupported = 0;
  int container_seen = 0;
  size_t index;
  ArchbirdStatus status;
  if (!project || !ab_projection_nonblank(container))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "constant_memberships requires a container");
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project->as.text, "set",
                                      "Map has no canonical source facts");
  status = ab_projection_data_init(context->engine, fact, &plan->id, "set",
                                   "derived", &project->as.text);
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *domain = ab_value_member(row, "domain");
    const AbValue *kind = ab_value_member(row, "kind");
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *state;
    AbValue empty = {0};
    status = validate_map_fact_row(context, map, row);
    if (status != ARCHBIRD_OK)
      break;
    if (!ab_projection_value_is(domain, "constant-memberships"))
      continue;
    if (!fact_container_matches(row, container) ||
        !path_matches(&path->as.text, paths))
      continue;
    container_seen = 1;
    if (ab_projection_value_is(kind, "class-collection"))
      continue;
    universe++;
    state = map_fact_attribute(row, "state");
    if (!ab_projection_value_is(state, "current") &&
        !ab_projection_value_is(state, "unknown")) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "constant membership has an invalid state");
      break;
    }
    {
      uint64_t selected_before = selected;
      status = add_projected_map_item(
          context, fact, map, spec, row, &empty,
          ab_projection_value_is(state, "current") ? "current" : "unknown",
          ab_projection_value_is(state, "current")
              ? ""
              : "constant membership is unresolved",
          &selected, &excluded);
      if (status == ARCHBIRD_OK && ab_projection_value_is(state, "unknown") &&
          selected > selected_before)
        unsupported++;
    }
  }
  if (status == ARCHBIRD_OK && !container_seen) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project->as.text, "set",
        "constant collection is absent from Map facts");
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "constant_member", universe, selected, excluded,
        unsupported, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

typedef struct MacroProjectionRow {
  const AbValue *row;
  const AbValue *path;
  const AbValue *call;
  const AbValue *text;
  const AbValue *identifier;
  uint64_t invocation;
  uint64_t argument;
} MacroProjectionRow;

static int macro_projection_row_compare(const void *left_raw,
                                        const void *right_raw) {
  const MacroProjectionRow *left = (const MacroProjectionRow *)left_raw;
  const MacroProjectionRow *right = (const MacroProjectionRow *)right_raw;
  const AbValue *left_id;
  const AbValue *right_id;
  int compared = ab_string_compare(&left->path->as.text, &right->path->as.text);
  if (!compared)
    compared = ab_string_compare(&left->call->as.text, &right->call->as.text);
  if (!compared && left->invocation != right->invocation)
    compared = left->invocation < right->invocation ? -1 : 1;
  if (!compared && left->argument != right->argument)
    compared = left->argument < right->argument ? -1 : 1;
  if (compared)
    return compared;
  left_id = ab_value_member(left->row, "id");
  right_id = ab_value_member(right->row, "id");
  return ab_string_compare(&left_id->as.text, &right_id->as.text);
}

static int macro_projection_same_invocation(const MacroProjectionRow *left,
                                            const MacroProjectionRow *right) {
  return left->invocation == right->invocation &&
         ab_string_equal(&left->path->as.text, &right->path->as.text) &&
         ab_string_equal(&left->call->as.text, &right->call->as.text);
}

static ArchbirdStatus extract_macro_members(AbProjectionContext *context,
                                            const AbProjectionPlan *plan,
                                            AbProjectionData *fact) {
  const AbValue *spec = &plan->definition;
  const AbValue *map = context->map;
  const AbValue *project = map ? ab_value_member(map, "project") : NULL;
  const AbValue *rows = map ? ab_value_member(map, "facts") : NULL;
  const AbValue *call = ab_value_member(spec, "call");
  const AbValue *selector = ab_value_member(spec, "selector");
  const AbValue *paths = ab_value_member(spec, "paths");
  uint64_t selector_argument = 0;
  uint64_t values_from_argument = 1;
  uint64_t universe = 0;
  uint64_t selected = 0;
  uint64_t excluded = 0;
  uint64_t unsupported = 0;
  size_t selector_matches = 0;
  MacroProjectionRow *macro_rows = NULL;
  size_t macro_count = 0;
  size_t row_index;
  ArchbirdStatus status;
  if (!project || !ab_projection_nonblank(call) ||
      !ab_projection_nonblank(selector) ||
      (ab_value_member(spec, "selector_argument") &&
       !ab_value_u64(ab_value_member(spec, "selector_argument"),
                     &selector_argument)) ||
      (ab_value_member(spec, "values_from_argument") &&
       !ab_value_u64(ab_value_member(spec, "values_from_argument"),
                     &values_from_argument)))
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "macro_members requires call, selector, and "
                              "valid argument indexes");
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_projection_data_unknown(context->engine, fact, &plan->id,
                                      &project->as.text, "set",
                                      "Map has no canonical source facts");
  status = ab_projection_data_init(context->engine, fact, &plan->id, "set",
                                   "derived", &project->as.text);
  if (status == ARCHBIRD_OK && rows->as.array.count) {
    if (rows->as.array.count > SIZE_MAX / sizeof(*macro_rows))
      status = archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "Map fact inventory is too large");
    else
      macro_rows = (MacroProjectionRow *)ab_malloc(
          context->engine, rows->as.array.count * sizeof(*macro_rows));
    if (status == ARCHBIRD_OK && !macro_rows)
      status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory indexing macro facts");
  }
  for (row_index = 0; status == ARCHBIRD_OK && row_index < rows->as.array.count;
       row_index++) {
    const AbValue *row = &rows->as.array.items[row_index];
    const AbValue *domain = ab_value_member(row, "domain");
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *row_call;
    const AbValue *row_invocation;
    const AbValue *argument;
    const AbValue *text;
    uint64_t invocation;
    uint64_t ordinal;
    status = validate_map_fact_row(context, map, row);
    if (status != ARCHBIRD_OK)
      break;
    if (!ab_projection_value_is(domain, "macro-invocations"))
      continue;
    row_call = map_fact_attribute(row, "call");
    row_invocation = map_fact_attribute(row, "invocation");
    argument = map_fact_attribute(row, "argument");
    text = map_fact_attribute(row, "text");
    if (!row_call || row_call->kind != AB_VALUE_STRING ||
        !ab_value_u64(row_invocation, &invocation) ||
        !ab_value_u64(argument, &ordinal) || !text ||
        text->kind != AB_VALUE_STRING) {
      status = archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "Map contains an invalid macro fact");
      break;
    }
    if (!ab_string_equal(&row_call->as.text, &call->as.text) ||
        !path_matches(&path->as.text, paths))
      continue;
    macro_rows[macro_count].row = row;
    macro_rows[macro_count].path = path;
    macro_rows[macro_count].call = row_call;
    macro_rows[macro_count].text = text;
    macro_rows[macro_count].identifier = map_fact_attribute(row, "identifier");
    macro_rows[macro_count].invocation = invocation;
    macro_rows[macro_count].argument = ordinal;
    macro_count++;
  }
  if (status == ARCHBIRD_OK && macro_count > 1)
    qsort(macro_rows, macro_count, sizeof(*macro_rows),
          macro_projection_row_compare);
  for (row_index = 0; status == ARCHBIRD_OK && row_index < macro_count;) {
    size_t end = row_index + 1;
    size_t value_index;
    int selector_found = 0;
    while (end < macro_count && macro_projection_same_invocation(
                                    &macro_rows[row_index], &macro_rows[end]))
      end++;
    for (value_index = row_index; value_index < end; value_index++)
      if (macro_rows[value_index].argument == selector_argument &&
          ab_string_equal(&macro_rows[value_index].text->as.text,
                          &selector->as.text)) {
        selector_found = 1;
        break;
      }
    if (selector_found) {
      selector_matches++;
      for (value_index = row_index; status == ARCHBIRD_OK && value_index < end;
           value_index++) {
        MacroProjectionRow *value_row = &macro_rows[value_index];
        AbValue empty = {0};
        if (value_row->argument < values_from_argument)
          continue;
        universe++;
        if (!value_row->identifier ||
            value_row->identifier->kind != AB_VALUE_STRING) {
          uint64_t selected_before = selected;
          status = add_projected_map_item(
              context, fact, map, spec, value_row->row, &empty, "unknown",
              "macro member argument is not one static identifier", &selected,
              &excluded);
          if (status == ARCHBIRD_OK && selected > selected_before)
            unsupported++;
        } else {
          status = add_projected_map_item(context, fact, map, spec,
                                          value_row->row, &empty, "current", "",
                                          &selected, &excluded);
        }
      }
    }
    row_index = end;
  }
  ab_free(context->engine, macro_rows);
  if (status == ARCHBIRD_OK && !selector_matches) {
    ab_projection_data_free(context->engine, fact);
    return ab_projection_data_unknown(
        context->engine, fact, &plan->id, &project->as.text, "set",
        "macro selector is absent from Map facts");
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, fact, "macro_member", universe, selected, excluded,
        unsupported, 0);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, fact);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, fact);
  return status;
}

static ArchbirdStatus extract_map_fact(AbProjectionContext *context,
                                       const AbProjectionPlan *plan,
                                       AbProjectionData *fact) {
  const AbValue *select = ab_value_member(&plan->definition, "select");
  if (ab_projection_value_is(select, "symbols"))
    return extract_symbols(context, plan, fact);
  if (ab_projection_value_is(select, "file_edges"))
    return extract_file_edges(context, plan, fact);
  if (ab_projection_value_is(select, "file_metrics"))
    return extract_file_metrics(context, plan, fact);
  if (ab_projection_value_is(select, "mapped_paths"))
    return extract_mapped_paths(context, plan, fact);
  if (ab_projection_value_is(select, "inventory_paths"))
    return extract_inventory_paths(context, plan, fact);
  if (ab_projection_value_is(select, "component_membership"))
    return extract_component_membership(context, plan, fact);
  if (ab_projection_value_is(select, "component_edges"))
    return extract_component_edges(context, plan, fact);
  if (ab_projection_value_is(select, "artifact_routes"))
    return extract_artifact_routes(context, plan, fact);
  if (ab_projection_value_is(select, "package_entrypoints"))
    return extract_package_entrypoints(context, plan, fact);
  if (ab_projection_value_is(select, "package_exports"))
    return extract_package_exports(context, plan, fact);
  if (ab_projection_value_is(select, "test_routes"))
    return extract_test_routes(context, plan, fact);
  if (ab_projection_value_is(select, "test_selectors"))
    return extract_test_selectors(context, plan, fact);
  if (ab_projection_value_is(select, "provider_surface"))
    return extract_provider_surface(context, plan, fact);
  if (ab_projection_value_is(select, "search_domain"))
    return extract_search_domain(context, plan, fact);
  if (ab_projection_value_is(select, "constant_values"))
    return extract_constant_values(context, plan, fact);
  if (ab_projection_value_is(select, "constant_memberships"))
    return extract_constant_memberships(context, plan, fact);
  if (ab_projection_value_is(select, "macro_members"))
    return extract_macro_members(context, plan, fact);
  return ARCHBIRD_CONFLICT;
}

ArchbirdStatus ab_projection_extract_map(ArchbirdEngine *engine,
                                         const AbValue *map,
                                         const AbValue *resolution,
                                         const AbProjectionPlan *plan,
                                         AbProjectionData *out) {
  AbProjectionContext context = {0};
  if (!engine || !map || !plan || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  context.engine = engine;
  context.map = map;
  context.resolution = resolution;
  return extract_map_fact(&context, plan, out);
}

ArchbirdStatus ab_projection_extract_literal(ArchbirdEngine *engine,
                                             const AbObjectField *operand,
                                             AbProjectionData *out) {
  AbProjectionContext context = {0};
  if (!engine || !operand || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  context.engine = engine;
  return extract_literal(&context, operand, out);
}
