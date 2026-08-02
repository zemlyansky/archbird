#include "package_entrypoint.h"

#include "artifact_validation.h"

#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static const AbValue *string_field(const AbValue *object,
                                   const AbString *name) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return NULL;
  for (index = 0; index < object->as.object.count; index++)
    if (ab_string_equal(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index].value;
  return NULL;
}

static int text_equal(const AbValue *value, const AbString *text) {
  return value && value->kind == AB_VALUE_STRING &&
         ab_string_equal(&value->as.text, text);
}

static int package_matches(const AbValue *package, const AbString *name) {
  const AbValue *aliases = field(package, "aliases");
  size_t index;
  if (text_equal(field(package, "name"), name) ||
      text_equal(field(package, "identity"), name))
    return 1;
  if (!aliases || aliases->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < aliases->as.array.count; index++)
    if (text_equal(&aliases->as.array.items[index], name))
      return 1;
  return 0;
}

static const AbValue *unique_package(const AbValue *map, const AbString *name,
                                     const AbString *path) {
  const AbValue *packages = field(map, "packages");
  const AbValue *matched = NULL;
  size_t count = 0;
  size_t index;
  if (!packages || packages->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < packages->as.array.count; index++) {
    const AbValue *candidate = &packages->as.array.items[index];
    if (!package_matches(candidate, name))
      continue;
    count++;
    if (ab_artifact_text_is(field(candidate, "kind"), "npm") &&
        text_equal(field(candidate, "manifest"), path))
      matched = candidate;
    else
      matched = NULL;
  }
  return count == 1 ? matched : NULL;
}

static ArchbirdStatus package_error(ArchbirdEngine *engine,
                                    const char *message) {
  return archbird_error_set(engine, ARCHBIRD_POLICY_REJECTED,
                            ARCHBIRD_NO_OFFSET, "npm package entrypoint: %s",
                            message);
}

static ArchbirdStatus pointer_token(AbBuffer *pointer, const char *value,
                                    size_t length) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < length; index++) {
    if (value[index] == '~')
      status = ab_buffer_literal(pointer, "~0");
    else if (value[index] == '/')
      status = ab_buffer_literal(pointer, "~1");
    else
      status = ab_buffer_append(pointer, value + index, 1);
  }
  return status;
}

static int route_prefix(const AbString *route, const char *prefix,
                        const char **out_name, size_t *out_length) {
  size_t length = strlen(prefix);
  if (route->length <= length || memcmp(route->data, prefix, length) != 0)
    return 0;
  *out_name = route->data + length;
  *out_length = route->length - length;
  return 1;
}

static int direct_export_name(const char *name, size_t length) {
  if (length == 1 && name[0] == '.')
    return 1;
  return length > 2 && name[0] == '.' && name[1] == '/' &&
         memchr(name + 2, '/', length - 2) == NULL;
}

ArchbirdStatus ab_act_json_package_entrypoint(AbActContext *context,
                                              const AbValue *operation,
                                              const AbString *item_id) {
  const AbValue *package_name = field(operation, "package");
  const AbValue *path = field(operation, "path");
  const AbValue *route = field(operation, "route");
  const AbValue *target = field(operation, "target");
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *map = ab_act_executor_map(context);
  const AbValue *package;
  ArchbirdSourceView source;
  AbString target_path = {0};
  AbValue document = {0};
  AbValue replacement = {0};
  const AbValue *parent = NULL;
  const AbValue *expected = NULL;
  const char *member = NULL;
  size_t member_length = 0;
  AbBuffer pointer;
  AbBuffer target_text;
  int expected_absent;
  int export_target = 0;
  ArchbirdStatus status;
  if (!engine || !package_name || !path || !route || !target)
    return ARCHBIRD_INVALID_ARGUMENT;
  package = unique_package(map, &package_name->as.text, &path->as.text);
  if (!package)
    return package_error(engine, "Plan no longer identifies one npm manifest");
  status = ab_artifact_resolve_relative_to_file(engine, &path->as.text,
                                                &target->as.text, &target_path);
  if (status != ARCHBIRD_OK)
    return package_error(engine,
                         "target is not one literal package-relative path");
  status = ab_act_executor_begin(context, item_id,
                                 "archbird.native.json.package-entrypoint@1");
  if (status != ARCHBIRD_OK) {
    ab_string_free(engine, &target_path);
    return status;
  }
  status = ab_act_executor_source(context, &path->as.text, &source);
  if (status == ARCHBIRD_OK)
    status = ab_act_executor_observe_source(context, &target_path);
  ab_string_free(engine, &target_path);
  if (status != ARCHBIRD_OK)
    return status;
  status =
      ab_json_value_decode(engine, source.bytes, source.byte_length, &document);
  if (status != ARCHBIRD_OK)
    return status;
  if (document.kind != AB_VALUE_OBJECT) {
    status = package_error(engine, "manifest root is not an object");
    goto cleanup;
  }
  ab_buffer_init(&pointer, engine);
  ab_buffer_init(&target_text, engine);
  if (route->as.text.length == 4 &&
      memcmp(route->as.text.data, "main", 4) == 0) {
    status = ab_buffer_literal(&pointer, "/main");
    expected = field(&document, "main");
  } else if (route->as.text.length == 3 &&
             memcmp(route->as.text.data, "bin", 3) == 0) {
    status = ab_buffer_literal(&pointer, "/bin");
    expected = field(&document, "bin");
  } else if (route->as.text.length == 15 &&
             memcmp(route->as.text.data, "exports:default", 15) == 0) {
    status = ab_buffer_literal(&pointer, "/exports");
    expected = field(&document, "exports");
    export_target = 1;
    if (expected && expected->kind == AB_VALUE_OBJECT) {
      status = package_error(
          engine, "default exports cannot replace an existing exports object");
      goto buffers;
    }
  } else if (route_prefix(&route->as.text, "bin:", &member, &member_length)) {
    parent = field(&document, "bin");
    if (!parent || parent->kind != AB_VALUE_OBJECT ||
        memchr(member, '/', member_length)) {
      status =
          package_error(engine, "named bin route requires one existing object");
      goto buffers;
    }
    status = ab_buffer_literal(&pointer, "/bin/");
    if (status == ARCHBIRD_OK)
      status = pointer_token(&pointer, member, member_length);
    {
      AbString member_name = {(char *)member, member_length};
      expected = string_field(parent, &member_name);
    }
  } else if (route_prefix(&route->as.text, "exports:", &member,
                          &member_length)) {
    parent = field(&document, "exports");
    if (!direct_export_name(member, member_length) || !parent ||
        parent->kind != AB_VALUE_OBJECT) {
      status = package_error(
          engine, "named export route requires one existing direct object");
      goto buffers;
    }
    status = ab_buffer_literal(&pointer, "/exports/");
    if (status == ARCHBIRD_OK)
      status = pointer_token(&pointer, member, member_length);
    {
      AbString member_name = {(char *)member, member_length};
      expected = string_field(parent, &member_name);
    }
    export_target = 1;
  } else {
    status = package_error(engine, "route form is not supported");
    goto buffers;
  }
  if (status != ARCHBIRD_OK)
    goto buffers;
  if (expected && expected->kind != AB_VALUE_STRING) {
    status =
        package_error(engine, "existing route is not a direct string target");
    goto buffers;
  }
  expected_absent = expected == NULL;
  if (export_target)
    status = ab_buffer_literal(&target_text, "./");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&target_text, target->as.text.data,
                              target->as.text.length);
  if (status != ARCHBIRD_OK)
    goto buffers;
  replacement.kind = AB_VALUE_STRING;
  replacement.as.text.data = (char *)target_text.data;
  replacement.as.text.length = target_text.length;
  {
    AbString pointer_text = {(char *)pointer.data, pointer.length};
    status = ab_act_executor_json_pointer_edit(context, item_id, &path->as.text,
                                               &pointer_text, expected_absent,
                                               expected, &replacement);
  }
buffers:
  ab_buffer_free(&target_text);
  ab_buffer_free(&pointer);
cleanup:
  ab_value_free(engine, &document);
  return status;
}
