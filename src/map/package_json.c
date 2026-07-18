#include "package_json.h"

#include "archbird_internal.h"
#include "json_internal.h"

#include <stdlib.h>
#include <string.h>

static yyjson_val *member(yyjson_val *object, const char *name) {
  return object && yyjson_is_obj(object) ? yyjson_obj_get(object, name) : NULL;
}

static ArchbirdStatus copy_value(ArchbirdEngine *engine, AbString *out,
                                 yyjson_val *value) {
  if (!value || !yyjson_is_str(value))
    return ARCHBIRD_INVALID_SCHEMA;
  return ab_string_copy(engine, out, yyjson_get_str(value),
                        yyjson_get_len(value));
}

static ArchbirdStatus append_dependency(ArchbirdEngine *engine,
                                        AbMapPackage *package, const char *name,
                                        size_t name_length,
                                        yyjson_val *requirement,
                                        const char *scope) {
  AbMapDependency *resized;
  AbMapDependency *item;
  size_t index;
  ArchbirdStatus status;
  if (!yyjson_is_str(requirement))
    return ARCHBIRD_OK;
  for (index = 0; index < package->dependency_count; index++) {
    if (package->dependencies[index].name.length == name_length &&
        memcmp(package->dependencies[index].name.data, name, name_length) == 0)
      return ARCHBIRD_OK;
  }
  if (package->dependency_count == SIZE_MAX / sizeof(*package->dependencies))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "too many npm dependencies");
  resized = (AbMapDependency *)ab_realloc(engine, package->dependencies,
                                          (package->dependency_count + 1) *
                                              sizeof(*package->dependencies));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding npm dependencies");
  package->dependencies = resized;
  item = &package->dependencies[package->dependency_count];
  memset(item, 0, sizeof(*item));
  status = ab_string_copy(engine, &item->name, name, name_length);
  if (status == ARCHBIRD_OK)
    status = copy_value(engine, &item->requirement, requirement);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &item->scope, scope, strlen(scope));
  if (status == ARCHBIRD_OK) {
    package->dependency_count++;
  } else {
    ab_string_free(engine, &item->name);
    ab_string_free(engine, &item->requirement);
    ab_string_free(engine, &item->scope);
  }
  return status;
}

static ArchbirdStatus append_pair(ArchbirdEngine *engine, AbStringPair **items,
                                  size_t *count, const char *key,
                                  size_t key_length, const char *value,
                                  size_t value_length, int setdefault) {
  AbStringPair *resized;
  AbStringPair *item;
  size_t index;
  ArchbirdStatus status;
  for (index = 0; index < *count; index++) {
    if ((*items)[index].key.length == key_length &&
        memcmp((*items)[index].key.data, key, key_length) == 0) {
      AbString replacement = {0};
      ArchbirdStatus replacement_status;
      if (setdefault)
        return ARCHBIRD_OK;
      replacement_status =
          ab_string_copy(engine, &replacement, value, value_length);
      if (replacement_status == ARCHBIRD_OK) {
        ab_string_free(engine, &(*items)[index].value);
        (*items)[index].value = replacement;
      }
      return replacement_status;
    }
  }
  if (*count == SIZE_MAX / sizeof(**items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "too many npm map entries");
  resized = (AbStringPair *)ab_realloc(engine, *items,
                                       (*count + 1) * sizeof(**items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding npm map entries");
  *items = resized;
  item = &(*items)[*count];
  memset(item, 0, sizeof(*item));
  status = ab_string_copy(engine, &item->key, key, key_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &item->value, value, value_length);
  if (status == ARCHBIRD_OK) {
    (*count)++;
  } else {
    ab_string_free(engine, &item->key);
    ab_string_free(engine, &item->value);
  }
  return status;
}

static ArchbirdStatus append_unique_string(ArchbirdEngine *engine,
                                           AbStringArray *items,
                                           const char *value,
                                           size_t value_length) {
  AbString *resized;
  size_t index;
  for (index = 0; index < items->count; index++) {
    if (items->items[index].length == value_length &&
        (!value_length ||
         memcmp(items->items[index].data, value, value_length) == 0))
      return ARCHBIRD_OK;
  }
  if (items->count == SIZE_MAX / sizeof(*items->items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many npm runtime entry targets");
  resized = (AbString *)ab_realloc(engine, items->items,
                                   (items->count + 1) * sizeof(*items->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding npm runtime entries");
  items->items = resized;
  memset(&items->items[items->count], 0, sizeof(*items->items));
  if (ab_string_copy(engine, &items->items[items->count], value,
                     value_length) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  items->count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_runtime_target(ArchbirdEngine *engine,
                                            AbMapPackage *package,
                                            yyjson_val *value) {
  const char *target;
  size_t target_length;
  if (!yyjson_is_str(value))
    return ARCHBIRD_OK;
  target = yyjson_get_str(value);
  target_length = yyjson_get_len(value);
  if (target_length >= 2 && target[0] == '.' && target[1] == '/') {
    target += 2;
    target_length -= 2;
  }
  return append_unique_string(engine, &package->npm_runtime_entries, target,
                              target_length);
}

static int pair_compare(const void *left_raw, const void *right_raw) {
  const AbStringPair *left = (const AbStringPair *)left_raw;
  const AbStringPair *right = (const AbStringPair *)right_raw;
  return ab_string_compare(&left->key, &right->key);
}

static int dependency_compare(const void *left_raw, const void *right_raw) {
  const AbMapDependency *left = (const AbMapDependency *)left_raw;
  const AbMapDependency *right = (const AbMapDependency *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static ArchbirdStatus dependencies(ArchbirdEngine *engine,
                                   AbMapPackage *package, yyjson_val *root,
                                   const char *section, const char *scope) {
  yyjson_val *object = member(root, section);
  yyjson_obj_iter iterator;
  yyjson_val *key;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!object || !yyjson_is_obj(object))
    return ARCHBIRD_OK;
  yyjson_obj_iter_init(object, &iterator);
  while (status == ARCHBIRD_OK &&
         (key = yyjson_obj_iter_next(&iterator)) != NULL) {
    status = append_dependency(engine, package, yyjson_get_str(key),
                               yyjson_get_len(key),
                               yyjson_obj_iter_get_val(key), scope);
  }
  return status;
}

static ArchbirdStatus prefixed_pair(ArchbirdEngine *engine,
                                    AbStringPair **items, size_t *count,
                                    const char *prefix, const char *key,
                                    size_t key_length, yyjson_val *value,
                                    int strip_dot_slash) {
  size_t prefix_length = strlen(prefix);
  char *joined;
  const char *target;
  size_t target_length;
  ArchbirdStatus status;
  if (!yyjson_is_str(value))
    return ARCHBIRD_OK;
  if (prefix_length > SIZE_MAX - key_length)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "npm route is too large");
  joined = (char *)ab_malloc(engine, prefix_length + key_length);
  if (!joined)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory decoding npm route");
  memcpy(joined, prefix, prefix_length);
  memcpy(joined + prefix_length, key, key_length);
  target = yyjson_get_str(value);
  target_length = yyjson_get_len(value);
  if (strip_dot_slash && target_length >= 2 && target[0] == '.' &&
      target[1] == '/') {
    target += 2;
    target_length -= 2;
  }
  status = append_pair(engine, items, count, joined, prefix_length + key_length,
                       target, target_length, 0);
  ab_free(engine, joined);
  return status;
}

static int npm_types_condition(const char *key, size_t key_length) {
  return (key_length == 5 && memcmp(key, "types", 5) == 0) ||
         (key_length > 6 && memcmp(key, "types@", 6) == 0);
}

static ArchbirdStatus flatten_exports(ArchbirdEngine *engine,
                                      AbMapPackage *package, yyjson_val *value,
                                      const char *prefix, size_t prefix_length,
                                      int type_condition) {
  ArchbirdStatus status = ARCHBIRD_OK;
  if (yyjson_is_str(value)) {
    const char *route = prefix_length ? prefix : "default";
    size_t route_length = prefix_length ? prefix_length : 7;
    status =
        prefixed_pair(engine, &package->entrypoints, &package->entrypoint_count,
                      "exports:", route, route_length, value, 1);
    if (status == ARCHBIRD_OK && !type_condition)
      status = append_runtime_target(engine, package, value);
    return status;
  }
  if (yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    yyjson_obj_iter_init(value, &iterator);
    while (status == ARCHBIRD_OK &&
           (key = yyjson_obj_iter_next(&iterator)) != NULL) {
      size_t key_length = yyjson_get_len(key);
      size_t next_length = prefix_length + (prefix_length ? 1 : 0) + key_length;
      char *next;
      if (next_length < prefix_length)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "npm export route is too large");
      next = (char *)ab_malloc(engine, next_length ? next_length : 1);
      if (!next)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory decoding npm exports");
      if (prefix_length)
        memcpy(next, prefix, prefix_length);
      if (prefix_length)
        next[prefix_length] = '/';
      memcpy(next + prefix_length + (prefix_length ? 1 : 0),
             yyjson_get_str(key), key_length);
      status = flatten_exports(
          engine, package, yyjson_obj_iter_get_val(key), next, next_length,
          type_condition ||
              npm_types_condition(yyjson_get_str(key), key_length));
      ab_free(engine, next);
    }
  }
  return status;
}

ArchbirdStatus ab_decode_npm_manifest(ArchbirdEngine *engine,
                                      const uint8_t *json, size_t json_length,
                                      AbMapPackage *package) {
  yyjson_doc *document = NULL;
  yyjson_val *root;
  yyjson_val *value;
  ArchbirdStatus status;
  if (!engine || !package)
    return ARCHBIRD_INVALID_ARGUMENT;
  status =
      archbird_json_parse_document(engine, json, json_length, &document, 1);
  if (status != ARCHBIRD_OK)
    return status;
  root = yyjson_doc_get_root(document);
  if (!yyjson_is_obj(root)) {
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "npm package manifest must be an object");
    goto done;
  }
  value = member(root, "name");
  if (!package->identity.length && value && yyjson_is_str(value))
    status = copy_value(engine, &package->identity, value);
  value = member(root, "version");
  if (status == ARCHBIRD_OK && !package->version.length && value &&
      yyjson_is_str(value))
    status = copy_value(engine, &package->version, value);
  if (status == ARCHBIRD_OK)
    status = dependencies(engine, package, root, "dependencies", "runtime");
  if (status == ARCHBIRD_OK)
    status =
        dependencies(engine, package, root, "optionalDependencies", "optional");
  if (status == ARCHBIRD_OK)
    status = dependencies(engine, package, root, "peerDependencies", "peer");
  if (status == ARCHBIRD_OK)
    status = dependencies(engine, package, root, "devDependencies", "dev");
  value = member(root, "main");
  if (status == ARCHBIRD_OK && value && yyjson_is_str(value))
    status = prefixed_pair(engine, &package->entrypoints,
                           &package->entrypoint_count, "", "main", 4, value, 1);
  value = member(root, "exports");
  if (status == ARCHBIRD_OK && value &&
      (yyjson_is_str(value) || yyjson_is_obj(value))) {
    package->npm_has_exports = 1;
    status = flatten_exports(engine, package, value, NULL, 0, 0);
  }
  value = member(root, "bin");
  if (status == ARCHBIRD_OK && value && yyjson_is_str(value))
    status = prefixed_pair(engine, &package->entrypoints,
                           &package->entrypoint_count, "", "bin", 3, value, 1);
  if (status == ARCHBIRD_OK && value && yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    yyjson_obj_iter_init(value, &iterator);
    while (status == ARCHBIRD_OK &&
           (key = yyjson_obj_iter_next(&iterator)) != NULL)
      status =
          prefixed_pair(engine, &package->entrypoints,
                        &package->entrypoint_count, "bin:", yyjson_get_str(key),
                        yyjson_get_len(key), yyjson_obj_iter_get_val(key), 1);
  }
  value = member(root, "scripts");
  if (status == ARCHBIRD_OK && value && yyjson_is_obj(value)) {
    yyjson_obj_iter iterator;
    yyjson_val *key;
    yyjson_obj_iter_init(value, &iterator);
    while (status == ARCHBIRD_OK &&
           (key = yyjson_obj_iter_next(&iterator)) != NULL) {
      yyjson_val *script = yyjson_obj_iter_get_val(key);
      if (yyjson_is_str(script))
        status = append_pair(engine, &package->scripts, &package->script_count,
                             yyjson_get_str(key), yyjson_get_len(key),
                             yyjson_get_str(script), yyjson_get_len(script), 0);
    }
  }
  if (status == ARCHBIRD_OK && package->dependency_count > 1)
    qsort(package->dependencies, package->dependency_count,
          sizeof(*package->dependencies), dependency_compare);
  if (status == ARCHBIRD_OK && package->entrypoint_count > 1)
    qsort(package->entrypoints, package->entrypoint_count,
          sizeof(*package->entrypoints), pair_compare);
  if (status == ARCHBIRD_OK && package->script_count > 1)
    qsort(package->scripts, package->script_count, sizeof(*package->scripts),
          pair_compare);
done:
  yyjson_doc_free(document);
  return status;
}
