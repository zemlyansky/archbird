#include "model.h"

#include <stdlib.h>
#include <string.h>

ArchbirdStatus ab_string_copy(ArchbirdEngine *engine, AbString *out,
                              const char *data, size_t length) {
  char *copy;
  out->data = NULL;
  out->length = 0;
  if (length == SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "string is too large");
  copy = (char *)ab_malloc(engine, length + 1);
  if (!copy)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory copying evidence string");
  if (length)
    memcpy(copy, data, length);
  copy[length] = '\0';
  out->data = copy;
  out->length = length;
  return ARCHBIRD_OK;
}

void ab_string_free(ArchbirdEngine *engine, AbString *value) {
  if (!value)
    return;
  ab_free(engine, value->data);
  value->data = NULL;
  value->length = 0;
}

int ab_string_compare(const AbString *left, const AbString *right) {
  size_t common = left->length < right->length ? left->length : right->length;
  int compared = common ? memcmp(left->data, right->data, common) : 0;
  if (compared != 0)
    return compared;
  return (left->length > right->length) - (left->length < right->length);
}

int ab_string_equal(const AbString *left, const AbString *right) {
  return ab_string_compare(left, right) == 0;
}

int ab_manifest_file_has_role(const AbManifestFile *file, const char *role) {
  size_t index;
  size_t length;
  if (!file || !role)
    return 0;
  length = strlen(role);
  for (index = 0; index < file->roles.count; index++) {
    const AbString *candidate = &file->roles.items[index];
    if (candidate->length == length &&
        memcmp(candidate->data, role, length) == 0)
      return 1;
  }
  return 0;
}

void ab_value_free(ArchbirdEngine *engine, AbValue *value) {
  size_t index;
  if (!value)
    return;
  if (value->kind == AB_VALUE_INTEGER || value->kind == AB_VALUE_STRING) {
    ab_string_free(engine, &value->as.text);
  } else if (value->kind == AB_VALUE_ARRAY) {
    if (value->as.array.items)
      for (index = 0; index < value->as.array.count; index++)
        ab_value_free(engine, &value->as.array.items[index]);
    ab_free(engine, value->as.array.items);
  } else if (value->kind == AB_VALUE_OBJECT) {
    if (value->as.object.fields)
      for (index = 0; index < value->as.object.count; index++) {
        ab_string_free(engine, &value->as.object.fields[index].name);
        ab_value_free(engine, &value->as.object.fields[index].value);
      }
    ab_free(engine, value->as.object.fields);
  }
  memset(value, 0, sizeof(*value));
}

int ab_value_equal(const AbValue *left, const AbValue *right) {
  size_t index;
  if (left->kind != right->kind)
    return 0;
  switch (left->kind) {
  case AB_VALUE_NULL:
    return 1;
  case AB_VALUE_BOOL:
    return left->as.boolean == right->as.boolean;
  case AB_VALUE_INTEGER:
  case AB_VALUE_STRING:
    return ab_string_equal(&left->as.text, &right->as.text);
  case AB_VALUE_REAL: {
    uint64_t left_bits;
    uint64_t right_bits;
    memcpy(&left_bits, &left->as.real, sizeof(left_bits));
    memcpy(&right_bits, &right->as.real, sizeof(right_bits));
    return left_bits == right_bits;
  }
  case AB_VALUE_ARRAY:
    if (left->as.array.count != right->as.array.count)
      return 0;
    for (index = 0; index < left->as.array.count; index++) {
      if (!ab_value_equal(&left->as.array.items[index],
                          &right->as.array.items[index]))
        return 0;
    }
    return 1;
  case AB_VALUE_OBJECT:
    if (left->as.object.count != right->as.object.count)
      return 0;
    for (index = 0; index < left->as.object.count; index++) {
      if (!ab_string_equal(&left->as.object.fields[index].name,
                           &right->as.object.fields[index].name) ||
          !ab_value_equal(&left->as.object.fields[index].value,
                          &right->as.object.fields[index].value))
        return 0;
    }
    return 1;
  }
  return 0;
}

ArchbirdStatus ab_value_copy(ArchbirdEngine *engine, AbValue *out,
                             const AbValue *source) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !out || !source)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->kind = source->kind;
  switch (source->kind) {
  case AB_VALUE_NULL:
    break;
  case AB_VALUE_BOOL:
    out->as.boolean = source->as.boolean;
    break;
  case AB_VALUE_REAL:
    out->as.real = source->as.real;
    break;
  case AB_VALUE_INTEGER:
  case AB_VALUE_STRING:
    status = ab_string_copy(engine, &out->as.text, source->as.text.data,
                            source->as.text.length);
    break;
  case AB_VALUE_ARRAY:
    out->as.array.count = source->as.array.count;
    if (out->as.array.count > SIZE_MAX / sizeof(*out->as.array.items))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON value array is too large to copy");
    if (status == ARCHBIRD_OK && out->as.array.count) {
      out->as.array.items = (AbValue *)ab_calloc(engine, out->as.array.count,
                                                 sizeof(*out->as.array.items));
      if (!out->as.array.items)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory copying JSON array");
    }
    for (index = 0; status == ARCHBIRD_OK && index < out->as.array.count;
         index++)
      status = ab_value_copy(engine, &out->as.array.items[index],
                             &source->as.array.items[index]);
    break;
  case AB_VALUE_OBJECT:
    out->as.object.count = source->as.object.count;
    if (out->as.object.count > SIZE_MAX / sizeof(*out->as.object.fields))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "JSON value object is too large to copy");
    if (status == ARCHBIRD_OK && out->as.object.count) {
      out->as.object.fields = (AbObjectField *)ab_calloc(
          engine, out->as.object.count, sizeof(*out->as.object.fields));
      if (!out->as.object.fields)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory copying JSON object");
    }
    for (index = 0; status == ARCHBIRD_OK && index < out->as.object.count;
         index++) {
      status = ab_string_copy(engine, &out->as.object.fields[index].name,
                              source->as.object.fields[index].name.data,
                              source->as.object.fields[index].name.length);
      if (status == ARCHBIRD_OK)
        status = ab_value_copy(engine, &out->as.object.fields[index].value,
                               &source->as.object.fields[index].value);
    }
    break;
  default:
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "unknown JSON value kind while copying");
    break;
  }
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

static void ab_string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  if (array->items)
    for (index = 0; index < array->count; index++)
      ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static int ab_string_arrays_equal(const AbStringArray *left,
                                  const AbStringArray *right) {
  size_t index;
  if (left->count != right->count)
    return 0;
  for (index = 0; index < left->count; index++) {
    if (!ab_string_equal(&left->items[index], &right->items[index]))
      return 0;
  }
  return 1;
}

static ArchbirdStatus ab_string_array_copy(ArchbirdEngine *engine,
                                           AbStringArray *out,
                                           const AbStringArray *source) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  if (source->count > SIZE_MAX / sizeof(*out->items))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "string array is too large to copy");
  if (source->count) {
    out->items =
        (AbString *)ab_calloc(engine, source->count, sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory copying string array");
  }
  out->count = source->count;
  for (index = 0; status == ARCHBIRD_OK && index < source->count; index++)
    status =
        ab_string_copy(engine, &out->items[index], source->items[index].data,
                       source->items[index].length);
  if (status != ARCHBIRD_OK)
    ab_string_array_free(engine, out);
  return status;
}

void ab_fact_free(ArchbirdEngine *engine, AbFact *fact) {
  size_t attribute_index;
  if (!fact)
    return;
  ab_string_free(engine, &fact->id);
  ab_string_free(engine, &fact->domain);
  ab_string_free(engine, &fact->kind);
  ab_string_free(engine, &fact->claim);
  ab_string_free(engine, &fact->project);
  ab_string_free(engine, &fact->path);
  ab_string_free(engine, &fact->key);
  ab_string_free(engine, &fact->name);
  if (fact->attributes)
    for (attribute_index = 0; attribute_index < fact->attribute_count;
         attribute_index++) {
      ab_string_free(engine, &fact->attributes[attribute_index].name);
      ab_value_free(engine, &fact->attributes[attribute_index].value);
    }
  ab_free(engine, fact->attributes);
  ab_string_free(engine, &fact->resolution.state);
  ab_string_array_free(engine, &fact->resolution.targets);
  ab_string_free(engine, &fact->resolution.reason);
  memset(fact, 0, sizeof(*fact));
}

ArchbirdStatus ab_fact_copy(ArchbirdEngine *engine, AbFact *out,
                            const AbFact *source) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
#define COPY_STRING(field)                                                     \
  do {                                                                         \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_string_copy(engine, &out->field, source->field.data,         \
                              source->field.length);                           \
  } while (0)
  if (!engine || !out || !source)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  COPY_STRING(id);
  COPY_STRING(domain);
  COPY_STRING(kind);
  COPY_STRING(claim);
  COPY_STRING(project);
  COPY_STRING(path);
  COPY_STRING(key);
  out->span_start = source->span_start;
  out->span_end = source->span_end;
  out->correlate_by_span = source->correlate_by_span;
  out->has_name = source->has_name;
  if (source->has_name)
    COPY_STRING(name);
  if (status == ARCHBIRD_OK && source->attribute_count) {
    if (source->attribute_count > SIZE_MAX / sizeof(*out->attributes))
      status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "fact attributes are too large to copy");
    else {
      out->attributes = (AbObjectField *)ab_calloc(
          engine, source->attribute_count, sizeof(*out->attributes));
      if (!out->attributes)
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory copying fact attributes");
    }
  }
  out->attribute_count =
      status == ARCHBIRD_OK ? source->attribute_count : (size_t)0;
  for (index = 0; status == ARCHBIRD_OK && index < source->attribute_count;
       index++) {
    status = ab_string_copy(engine, &out->attributes[index].name,
                            source->attributes[index].name.data,
                            source->attributes[index].name.length);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &out->attributes[index].value,
                             &source->attributes[index].value);
  }
  out->has_resolution = source->has_resolution;
  if (status == ARCHBIRD_OK && source->has_resolution) {
    status = ab_string_copy(engine, &out->resolution.state,
                            source->resolution.state.data,
                            source->resolution.state.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_array_copy(engine, &out->resolution.targets,
                                    &source->resolution.targets);
    out->resolution.has_reason = source->resolution.has_reason;
    if (status == ARCHBIRD_OK && source->resolution.has_reason)
      status = ab_string_copy(engine, &out->resolution.reason,
                              source->resolution.reason.data,
                              source->resolution.reason.length);
  }
  if (status != ARCHBIRD_OK)
    ab_fact_free(engine, out);
#undef COPY_STRING
  return status;
}

static ArchbirdStatus merge_fact_attributes(ArchbirdEngine *engine,
                                            AbFact *target,
                                            const AbFact *source) {
  AbObjectField *merged;
  size_t target_index = 0;
  size_t source_index = 0;
  size_t output_index = 0;
  size_t capacity;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (source->attribute_count > SIZE_MAX - target->attribute_count)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "merged fact attributes are too large");
  capacity = target->attribute_count + source->attribute_count;
  if (!capacity)
    return ARCHBIRD_OK;
  merged = (AbObjectField *)ab_calloc(engine, capacity, sizeof(*merged));
  if (!merged)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory merging fact attributes");
  while (status == ARCHBIRD_OK && (target_index < target->attribute_count ||
                                   source_index < source->attribute_count)) {
    const AbObjectField *selected;
    if (target_index >= target->attribute_count) {
      selected = &source->attributes[source_index++];
    } else if (source_index >= source->attribute_count) {
      selected = &target->attributes[target_index++];
    } else {
      int compared = ab_string_compare(&target->attributes[target_index].name,
                                       &source->attributes[source_index].name);
      if (compared < 0) {
        selected = &target->attributes[target_index++];
      } else if (compared > 0) {
        selected = &source->attributes[source_index++];
      } else {
        selected = &target->attributes[target_index++];
        source_index++;
      }
    }
    status = ab_string_copy(engine, &merged[output_index].name,
                            selected->name.data, selected->name.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_value_copy(engine, &merged[output_index].value, &selected->value);
    if (status == ARCHBIRD_OK)
      output_index++;
  }
  if (status == ARCHBIRD_OK) {
    size_t index;
    for (index = 0; index < target->attribute_count; index++) {
      ab_string_free(engine, &target->attributes[index].name);
      ab_value_free(engine, &target->attributes[index].value);
    }
    ab_free(engine, target->attributes);
    target->attributes = merged;
    target->attribute_count = output_index;
    return ARCHBIRD_OK;
  }
  while (output_index) {
    output_index--;
    ab_string_free(engine, &merged[output_index].name);
    ab_value_free(engine, &merged[output_index].value);
  }
  ab_free(engine, merged);
  return status;
}

int ab_fact_attribute_is_presentation(const AbString *name) {
  static const char signature[] = "signature";
  return name && name->length == sizeof(signature) - 1 &&
         memcmp(name->data, signature, sizeof(signature) - 1) == 0;
}

static int qualified_name_suffix(const AbString *qualified,
                                 const AbString *leaf) {
  size_t offset;
  if (!qualified || !leaf || qualified->length <= leaf->length)
    return 0;
  offset = qualified->length - leaf->length;
  return offset > 0 && qualified->data[offset - 1] == '.' &&
         memcmp(qualified->data + offset, leaf->data, leaf->length) == 0;
}

int ab_fact_names_compatible(const AbFact *left, const AbFact *right) {
  static const char symbols[] = "symbols";
  if (!left || !right)
    return 0;
  if (!left->has_name || !right->has_name ||
      ab_string_equal(&left->name, &right->name))
    return 1;
  if (left->domain.length != sizeof(symbols) - 1 ||
      memcmp(left->domain.data, symbols, sizeof(symbols) - 1) != 0 ||
      !ab_string_equal(&left->domain, &right->domain))
    return 0;
  return qualified_name_suffix(&left->name, &right->name) ||
         qualified_name_suffix(&right->name, &left->name);
}

static ArchbirdStatus merge_fact_compatible_in_place(ArchbirdEngine *engine,
                                                     AbFact *target,
                                                     const AbFact *source) {
  size_t target_index = 0;
  size_t source_index = 0;
  ArchbirdStatus status;
  if (!engine || !target || !source)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_fact_names_compatible(target, source))
    return ARCHBIRD_CONFLICT;
  if (!target->has_name && source->has_name) {
    status = ab_string_copy(engine, &target->name, source->name.data,
                            source->name.length);
    if (status != ARCHBIRD_OK)
      return status;
    target->has_name = 1;
  } else if (target->has_name && source->has_name &&
             source->name.length > target->name.length) {
    AbString more_qualified = {0};
    status = ab_string_copy(engine, &more_qualified, source->name.data,
                            source->name.length);
    if (status != ARCHBIRD_OK)
      return status;
    ab_string_free(engine, &target->name);
    target->name = more_qualified;
  }
  while (target_index < target->attribute_count &&
         source_index < source->attribute_count) {
    int compared = ab_string_compare(&target->attributes[target_index].name,
                                     &source->attributes[source_index].name);
    if (compared < 0) {
      target_index++;
    } else if (compared > 0) {
      source_index++;
    } else {
      if (!ab_value_equal(&target->attributes[target_index].value,
                          &source->attributes[source_index].value) &&
          !ab_fact_attribute_is_presentation(
              &target->attributes[target_index].name))
        return ARCHBIRD_CONFLICT;
      target_index++;
      source_index++;
    }
  }
  status = merge_fact_attributes(engine, target, source);
  if (status != ARCHBIRD_OK)
    return status;
  if (target->has_resolution && source->has_resolution) {
    if (!ab_string_equal(&target->resolution.state,
                         &source->resolution.state) ||
        !ab_string_arrays_equal(&target->resolution.targets,
                                &source->resolution.targets))
      return ARCHBIRD_CONFLICT;
    if (target->resolution.has_reason && source->resolution.has_reason &&
        !ab_string_equal(&target->resolution.reason,
                         &source->resolution.reason))
      return ARCHBIRD_CONFLICT;
    if (!target->resolution.has_reason && source->resolution.has_reason) {
      status = ab_string_copy(engine, &target->resolution.reason,
                              source->resolution.reason.data,
                              source->resolution.reason.length);
      if (status != ARCHBIRD_OK)
        return status;
      target->resolution.has_reason = 1;
    }
  } else if (!target->has_resolution && source->has_resolution) {
    status = ab_string_copy(engine, &target->resolution.state,
                            source->resolution.state.data,
                            source->resolution.state.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_array_copy(engine, &target->resolution.targets,
                                    &source->resolution.targets);
    target->resolution.has_reason = source->resolution.has_reason;
    if (status == ARCHBIRD_OK && source->resolution.has_reason)
      status = ab_string_copy(engine, &target->resolution.reason,
                              source->resolution.reason.data,
                              source->resolution.reason.length);
    if (status != ARCHBIRD_OK) {
      ab_string_free(engine, &target->resolution.state);
      ab_string_array_free(engine, &target->resolution.targets);
      ab_string_free(engine, &target->resolution.reason);
      memset(&target->resolution, 0, sizeof(target->resolution));
      return status;
    }
    target->has_resolution = 1;
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_fact_merge_compatible(ArchbirdEngine *engine, AbFact *target,
                                        const AbFact *source) {
  AbFact merged;
  ArchbirdStatus status;
  if (!engine || !target || !source)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&merged, 0, sizeof(merged));
  status = ab_fact_copy(engine, &merged, target);
  if (status != ARCHBIRD_OK)
    return status;
  status = merge_fact_compatible_in_place(engine, &merged, source);
  if (status != ARCHBIRD_OK) {
    ab_fact_free(engine, &merged);
    return status;
  }
  ab_fact_free(engine, target);
  *target = merged;
  return ARCHBIRD_OK;
}

int ab_fact_equal(const AbFact *left, const AbFact *right) {
  size_t index;
  if (!ab_string_equal(&left->id, &right->id) ||
      !ab_string_equal(&left->domain, &right->domain) ||
      !ab_string_equal(&left->kind, &right->kind) ||
      !ab_string_equal(&left->claim, &right->claim) ||
      !ab_string_equal(&left->project, &right->project) ||
      !ab_string_equal(&left->path, &right->path) ||
      left->span_start != right->span_start ||
      left->span_end != right->span_end ||
      left->correlate_by_span != right->correlate_by_span ||
      !ab_string_equal(&left->key, &right->key) ||
      left->has_name != right->has_name ||
      (left->has_name && !ab_string_equal(&left->name, &right->name)) ||
      left->attribute_count != right->attribute_count ||
      left->has_resolution != right->has_resolution)
    return 0;
  for (index = 0; index < left->attribute_count; index++) {
    if (!ab_string_equal(&left->attributes[index].name,
                         &right->attributes[index].name) ||
        !ab_value_equal(&left->attributes[index].value,
                        &right->attributes[index].value))
      return 0;
  }
  if (left->has_resolution &&
      (!ab_string_equal(&left->resolution.state, &right->resolution.state) ||
       !ab_string_arrays_equal(&left->resolution.targets,
                               &right->resolution.targets) ||
       left->resolution.has_reason != right->resolution.has_reason ||
       (left->resolution.has_reason &&
        !ab_string_equal(&left->resolution.reason, &right->resolution.reason))))
    return 0;
  return 1;
}

static void ab_producer_free(ArchbirdEngine *engine, AbProducer *producer) {
  ab_string_free(engine, &producer->name);
  ab_string_free(engine, &producer->version);
  ab_string_free(engine, &producer->runtime);
  memset(producer, 0, sizeof(*producer));
}

void ab_source_manifest_free(ArchbirdEngine *engine,
                             AbSourceManifest *manifest) {
  size_t index;
  if (!manifest)
    return;
  ab_string_free(engine, &manifest->project);
  ab_producer_free(engine, &manifest->producer);
  ab_string_free(engine, &manifest->resolution.profile_name);
  if (manifest->files)
    for (index = 0; index < manifest->file_count; index++) {
      AbManifestFile *file = &manifest->files[index];
      ab_string_free(engine, &file->path);
      ab_string_free(engine, &file->language);
      ab_string_free(engine, &file->layer);
      ab_string_array_free(engine, &file->roles);
    }
  ab_free(engine, manifest->files);
  memset(manifest, 0, sizeof(*manifest));
}

void ab_provider_bundle_free(ArchbirdEngine *engine, AbProviderBundle *bundle) {
  size_t index;
  if (!bundle)
    return;
  ab_string_free(engine, &bundle->subject.scope);
  ab_string_free(engine, &bundle->subject.project);
  ab_string_free(engine, &bundle->subject.path);
  ab_string_free(engine, &bundle->subject.name);
  ab_producer_free(engine, &bundle->producer);
  if (bundle->inputs)
    for (index = 0; index < bundle->input_count; index++) {
      ab_string_free(engine, &bundle->inputs[index].project);
      ab_string_free(engine, &bundle->inputs[index].path);
    }
  ab_free(engine, bundle->inputs);
  if (bundle->capabilities)
    for (index = 0; index < bundle->capability_count; index++) {
      AbCapability *capability = &bundle->capabilities[index];
      ab_string_free(engine, &capability->domain);
      ab_string_free(engine, &capability->coverage);
      ab_string_array_free(engine, &capability->claims);
      ab_string_free(engine, &capability->boundary);
    }
  ab_free(engine, bundle->capabilities);
  if (bundle->facts)
    for (index = 0; index < bundle->fact_count; index++)
      ab_fact_free(engine, &bundle->facts[index]);
  ab_free(engine, bundle->facts);
  if (bundle->diagnostics)
    for (index = 0; index < bundle->diagnostic_count; index++) {
      AbDiagnostic *diagnostic = &bundle->diagnostics[index];
      ab_string_free(engine, &diagnostic->severity);
      ab_string_free(engine, &diagnostic->code);
      ab_string_free(engine, &diagnostic->message);
      ab_string_free(engine, &diagnostic->project);
      ab_string_free(engine, &diagnostic->path);
    }
  ab_free(engine, bundle->diagnostics);
  memset(bundle, 0, sizeof(*bundle));
}
