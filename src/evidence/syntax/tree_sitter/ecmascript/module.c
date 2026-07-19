#include "syntax/tree_sitter/ecmascript/module.h"

#include "syntax/tree_sitter/ecmascript/scanner.h"

#include "render_internal.h"

#include <stdint.h>
#include <string.h>

typedef struct JsSlice {
  const uint8_t *data;
  size_t length;
  size_t start;
  size_t end;
} JsSlice;

typedef struct JsProperty {
  JsSlice name;
  JsSlice origin_name;
  const char *evidence_kind;
  int has_origin_name;
} JsProperty;

typedef struct JsUnknown {
  const char *reason;
  size_t start;
  size_t end;
} JsUnknown;

typedef struct JsObject {
  JsProperty *properties;
  size_t property_count;
  size_t property_capacity;
  JsSlice *reexports;
  size_t reexport_count;
  size_t reexport_capacity;
  JsUnknown *unknowns;
  size_t unknown_count;
  size_t unknown_capacity;
  int module_value;
} JsObject;

typedef struct JsBinding {
  JsSlice name;
  size_t object;
} JsBinding;

typedef struct JsModule {
  AbTreeSitterScan *scan;
  JsObject *objects;
  size_t object_count;
  size_t object_capacity;
  JsBinding *bindings;
  size_t binding_count;
  size_t binding_capacity;
  size_t root;
  size_t exports_alias;
  int module_builtin;
  int object_builtin;
  int require_builtin;
  int saw_commonjs;
  int root_dynamic;
  size_t root_dynamic_start;
  size_t root_dynamic_end;
  int root_value_export;
  JsSlice root_value_origin;
  int root_value_has_origin;
  size_t root_value_start;
  size_t root_value_end;
  const char *root_value_evidence_kind;
} JsModule;

static int slice_equal(const JsSlice *left, const JsSlice *right) {
  return left->length == right->length &&
         (!left->length || memcmp(left->data, right->data, left->length) == 0);
}

static int slice_literal(const JsSlice *slice, const char *literal) {
  size_t length = strlen(literal);
  return slice->length == length &&
         (!length || memcmp(slice->data, literal, length) == 0);
}

static int node_slice(AbTreeSitterScan *scan, TSNode node, JsSlice *out) {
  memset(out, 0, sizeof(*out));
  if (!ab_tree_sitter_node_slice(scan, node, &out->start, &out->end) ||
      out->start == out->end)
    return 0;
  out->data = scan->source + out->start;
  out->length = out->end - out->start;
  return 1;
}

static int identifier(AbTreeSitterScan *scan, TSNode node, JsSlice *out) {
  const char *type = ts_node_is_null(node) ? "" : ts_node_type(node);
  if (strcmp(type, "identifier") != 0 &&
      strcmp(type, "property_identifier") != 0 &&
      strcmp(type, "shorthand_property_identifier") != 0 &&
      strcmp(type, "shorthand_property_identifier_pattern") != 0 &&
      strcmp(type, "type_identifier") != 0)
    return 0;
  return node_slice(scan, node, out) &&
         memchr(out->data, '\\', out->length) == NULL;
}

static TSNode unwrap(TSNode node) {
  return ab_tree_sitter_unwrap_ecmascript_expression(node);
}

static int static_name(AbTreeSitterScan *scan, TSNode node, JsSlice *out) {
  JsSlice raw;
  const char *type;
  node = unwrap(node);
  type = ts_node_is_null(node) ? "" : ts_node_type(node);
  if (identifier(scan, node, out))
    return 1;
  if (strcmp(type, "default") == 0) {
    static const uint8_t name[] = "default";
    if (!node_slice(scan, node, out))
      return 0;
    out->data = name;
    out->length = sizeof(name) - 1;
    return 1;
  }
  if (strcmp(type, "computed_property_name") == 0 &&
      ts_node_named_child_count(node) == 1) {
    TSNode child = unwrap(ts_node_named_child(node, 0));
    if (!ab_tree_sitter_node_type(child, "string") &&
        !ab_tree_sitter_node_type(child, "number"))
      return 0;
    return static_name(scan, child, out);
  }
  if (strcmp(type, "string") != 0 && strcmp(type, "number") != 0)
    return 0;
  if (!node_slice(scan, node, &raw))
    return 0;
  if (strcmp(type, "string") == 0) {
    if (raw.length < 2 ||
        !((raw.data[0] == '\'' && raw.data[raw.length - 1] == '\'') ||
          (raw.data[0] == '"' && raw.data[raw.length - 1] == '"')))
      return 0;
    raw.data++;
    raw.length -= 2;
    raw.start++;
    raw.end--;
    if (!raw.length || memchr(raw.data, '\\', raw.length))
      return 0;
  }
  *out = raw;
  return 1;
}

static int string_value(AbTreeSitterScan *scan, TSNode node, JsSlice *out) {
  node = unwrap(node);
  return ab_tree_sitter_node_type(node, "string") &&
         static_name(scan, node, out);
}

static ArchbirdStatus grow(JsModule *module, void **items, size_t *capacity,
                           size_t required, size_t item_size,
                           const char *label) {
  size_t next = *capacity ? *capacity : 8;
  void *resized;
  while (next < required && next <= SIZE_MAX / 2)
    next *= 2;
  if (next < required || next > module->scan->engine->options.max_values ||
      next > SIZE_MAX / item_size)
    return archbird_error_set(module->scan->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "%s limit exceeded", label);
  resized = ab_realloc(module->scan->engine, *items, next * item_size);
  if (!resized)
    return archbird_error_set(module->scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET, "out of memory collecting %s",
                              label);
  *items = resized;
  *capacity = next;
  return ARCHBIRD_OK;
}

static ArchbirdStatus new_object(JsModule *module, size_t *out) {
  ArchbirdStatus status;
  if (module->object_count == module->object_capacity) {
    status = grow(module, (void **)&module->objects, &module->object_capacity,
                  module->object_count + 1, sizeof(*module->objects),
                  "CommonJS objects");
    if (status != ARCHBIRD_OK)
      return status;
  }
  memset(&module->objects[module->object_count], 0, sizeof(*module->objects));
  *out = module->object_count++;
  return ARCHBIRD_OK;
}

static JsBinding *find_binding(JsModule *module, const JsSlice *name) {
  size_t index;
  for (index = module->binding_count; index > 0; index--)
    if (slice_equal(&module->bindings[index - 1].name, name))
      return &module->bindings[index - 1];
  return NULL;
}

static ArchbirdStatus set_binding(JsModule *module, const JsSlice *name,
                                  size_t object) {
  JsBinding *row = find_binding(module, name);
  ArchbirdStatus status;
  if (row) {
    row->object = object;
    return ARCHBIRD_OK;
  }
  if (module->binding_count == module->binding_capacity) {
    status = grow(module, (void **)&module->bindings, &module->binding_capacity,
                  module->binding_count + 1, sizeof(*module->bindings),
                  "CommonJS aliases");
    if (status != ARCHBIRD_OK)
      return status;
  }
  row = &module->bindings[module->binding_count++];
  row->name = *name;
  row->object = object;
  return ARCHBIRD_OK;
}

static JsProperty *find_property(JsObject *object, const JsSlice *name) {
  size_t index;
  for (index = 0; index < object->property_count; index++)
    if (slice_equal(&object->properties[index].name, name))
      return &object->properties[index];
  return NULL;
}

static ArchbirdStatus set_property(JsModule *module, size_t object_index,
                                   const JsSlice *name,
                                   const JsSlice *origin_name,
                                   const char *evidence_kind) {
  JsObject *object;
  JsProperty *row;
  ArchbirdStatus status;
  if (object_index >= module->object_count)
    return ARCHBIRD_OK;
  object = &module->objects[object_index];
  row = find_property(object, name);
  if (!row) {
    if (object->property_count == object->property_capacity) {
      status = grow(module, (void **)&object->properties,
                    &object->property_capacity, object->property_count + 1,
                    sizeof(*object->properties), "CommonJS properties");
      if (status != ARCHBIRD_OK)
        return status;
    }
    row = &object->properties[object->property_count++];
  }
  memset(row, 0, sizeof(*row));
  row->name = *name;
  row->evidence_kind = evidence_kind;
  if (origin_name && origin_name->length) {
    row->origin_name = *origin_name;
    row->has_origin_name = 1;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_reexport(JsModule *module, size_t object_index,
                                   const JsSlice *origin) {
  JsObject *object;
  size_t index;
  ArchbirdStatus status;
  if (object_index >= module->object_count)
    return ARCHBIRD_OK;
  object = &module->objects[object_index];
  for (index = 0; index < object->reexport_count; index++)
    if (slice_equal(&object->reexports[index], origin))
      return ARCHBIRD_OK;
  if (object->reexport_count == object->reexport_capacity) {
    status = grow(module, (void **)&object->reexports,
                  &object->reexport_capacity, object->reexport_count + 1,
                  sizeof(*object->reexports), "CommonJS re-exports");
    if (status != ARCHBIRD_OK)
      return status;
  }
  object->reexports[object->reexport_count++] = *origin;
  return ARCHBIRD_OK;
}

static ArchbirdStatus mark_unknown(JsModule *module, size_t object_index,
                                   const char *reason, TSNode node) {
  JsObject *object;
  JsUnknown *row;
  ArchbirdStatus status;
  size_t start = ts_node_is_null(node) ? 0 : ts_node_start_byte(node);
  size_t end = ts_node_is_null(node) ? start : ts_node_end_byte(node);
  size_t index;
  if (object_index >= module->object_count)
    return ARCHBIRD_OK;
  object = &module->objects[object_index];
  for (index = 0; index < object->unknown_count; index++)
    if (object->unknowns[index].start == start &&
        object->unknowns[index].end == end &&
        strcmp(object->unknowns[index].reason, reason) == 0)
      return ARCHBIRD_OK;
  if (object->unknown_count == object->unknown_capacity) {
    status = grow(module, (void **)&object->unknowns, &object->unknown_capacity,
                  object->unknown_count + 1, sizeof(*object->unknowns),
                  "CommonJS unknown frontiers");
    if (status != ARCHBIRD_OK)
      return status;
  }
  row = &object->unknowns[object->unknown_count++];
  row->reason = reason;
  row->start = start;
  row->end = end;
  return ARCHBIRD_OK;
}

static int static_member(JsModule *module, TSNode node, TSNode *base,
                         JsSlice *key) {
  TSNode property;
  node = unwrap(node);
  if (ab_tree_sitter_node_type(node, "member_expression")) {
    *base = ab_tree_sitter_child(node, "object");
    property = ab_tree_sitter_child(node, "property");
  } else if (ab_tree_sitter_node_type(node, "subscript_expression")) {
    *base = ab_tree_sitter_child(node, "object");
    property = ab_tree_sitter_child(node, "index");
    if (!ab_tree_sitter_node_type(unwrap(property), "string") &&
        !ab_tree_sitter_node_type(unwrap(property), "number"))
      return 0;
  } else {
    return 0;
  }
  return !ts_node_is_null(*base) && static_name(module->scan, property, key);
}

static int module_exports(JsModule *module, TSNode node) {
  TSNode base;
  JsSlice key;
  JsSlice name;
  return module->module_builtin && static_member(module, node, &base, &key) &&
         identifier(module->scan, unwrap(base), &name) &&
         slice_literal(&name, "module") && slice_literal(&key, "exports");
}

static size_t object_for(JsModule *module, TSNode node);
static ArchbirdStatus process_expression(JsModule *module, TSNode node,
                                         size_t *out_object);
static ArchbirdStatus add_named_fact(AbTreeSitterScan *scan, const char *domain,
                                     const char *kind, const JsSlice *name,
                                     TSNode anchor, AbFact **out);
static ArchbirdStatus add_export_fact(AbTreeSitterScan *scan,
                                      const JsSlice *name, TSNode anchor,
                                      const JsSlice *origin,
                                      const JsSlice *origin_name,
                                      const char *evidence_kind);

static size_t object_for(JsModule *module, TSNode node) {
  JsSlice name;
  JsBinding *row;
  node = unwrap(node);
  if (module_exports(module, node))
    return module->root;
  if (!identifier(module->scan, node, &name))
    return SIZE_MAX;
  if (slice_literal(&name, "exports"))
    return module->exports_alias;
  row = find_binding(module, &name);
  return row ? row->object : SIZE_MAX;
}

static ArchbirdStatus copy_object(JsModule *module, size_t target,
                                  size_t source, const char *evidence_kind) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (target == source || target >= module->object_count ||
      source >= module->object_count)
    return ARCHBIRD_OK;
  for (index = 0;
       status == ARCHBIRD_OK && index < module->objects[source].property_count;
       index++) {
    const JsProperty *row = &module->objects[source].properties[index];
    status = set_property(module, target, &row->name,
                          row->has_origin_name ? &row->origin_name : NULL,
                          evidence_kind);
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < module->objects[source].reexport_count;
       index++)
    status =
        add_reexport(module, target, &module->objects[source].reexports[index]);
  if (status == ARCHBIRD_OK && module->objects[source].unknown_count)
    status = mark_unknown(module, target, "dynamic-object-copy", (TSNode){0});
  return status;
}

static ArchbirdStatus object_literal(JsModule *module, TSNode node,
                                     size_t *out_object) {
  uint32_t count = ts_node_named_child_count(node);
  uint32_t index;
  ArchbirdStatus status = new_object(module, out_object);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode item = ts_node_named_child(node, index);
    JsSlice key;
    JsSlice origin;
    if (ab_tree_sitter_node_type(item, "spread_element")) {
      size_t source = SIZE_MAX;
      TSNode value = ts_node_named_child_count(item) == 1
                         ? ts_node_named_child(item, 0)
                         : (TSNode){0};
      status = process_expression(module, value, &source);
      if (status == ARCHBIRD_OK && source != SIZE_MAX)
        status = copy_object(module, *out_object, source, "object-spread");
      else if (status == ARCHBIRD_OK)
        status =
            mark_unknown(module, *out_object, "dynamic-object-spread", item);
    } else if (ab_tree_sitter_node_type(item, "pair")) {
      TSNode key_node = ab_tree_sitter_child(item, "key");
      TSNode value = ab_tree_sitter_child(item, "value");
      if (!static_name(module->scan, key_node, &key))
        status =
            mark_unknown(module, *out_object, "dynamic-property-key", key_node);
      else {
        JsSlice *origin_ptr =
            identifier(module->scan, unwrap(value), &origin) ? &origin : NULL;
        status = set_property(module, *out_object, &key, origin_ptr,
                              "object-literal");
      }
    } else if (ab_tree_sitter_node_type(item, "method_definition")) {
      TSNode name = ab_tree_sitter_child(item, "name");
      if (static_name(module->scan, name, &key))
        status = set_property(module, *out_object, &key, &key, "object-method");
      else
        status =
            mark_unknown(module, *out_object, "dynamic-property-key", item);
    } else if (identifier(module->scan, item, &key)) {
      status =
          set_property(module, *out_object, &key, &key, "object-shorthand");
    } else {
      status =
          mark_unknown(module, *out_object, "unsupported-object-member", item);
    }
  }
  return status;
}

static int call_named(JsModule *module, TSNode function, const char *object,
                      const char *method) {
  TSNode base;
  JsSlice key;
  JsSlice name;
  return static_member(module, function, &base, &key) &&
         identifier(module->scan, unwrap(base), &name) &&
         slice_literal(&name, object) && slice_literal(&key, method);
}

static TSNode argument(TSNode arguments, uint32_t index) {
  return index < ts_node_named_child_count(arguments)
             ? ts_node_named_child(arguments, index)
             : (TSNode){0};
}

static ArchbirdStatus require_object(JsModule *module, TSNode call,
                                     TSNode arguments, size_t *out_object) {
  JsSlice origin;
  AbFact *fact = NULL;
  ArchbirdStatus status;
  if (!module->require_builtin || ts_node_named_child_count(arguments) != 1 ||
      !string_value(module->scan, argument(arguments, 0), &origin)) {
    *out_object = SIZE_MAX;
    return ARCHBIRD_OK;
  }
  status = new_object(module, out_object);
  if (status == ARCHBIRD_OK)
    status = add_reexport(module, *out_object, &origin);
  if (status == ARCHBIRD_OK)
    status = add_named_fact(module->scan, "imports", "require", &origin,
                            argument(arguments, 0), &fact);
  (void)call;
  return status;
}

static ArchbirdStatus object_call(JsModule *module, TSNode call,
                                  TSNode function, TSNode arguments,
                                  size_t *out_object) {
  uint32_t count = ts_node_named_child_count(arguments);
  uint32_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t target;
  *out_object = SIZE_MAX;
  if (!module->object_builtin)
    return ARCHBIRD_OK;
  if (call_named(module, function, "Object", "defineProperty")) {
    JsSlice key;
    target = count ? object_for(module, argument(arguments, 0)) : SIZE_MAX;
    if (target == SIZE_MAX)
      return ARCHBIRD_OK;
    *out_object = target;
    if (count >= 3 && static_name(module->scan, argument(arguments, 1), &key))
      return set_property(module, target, &key, NULL, "define-property");
    return mark_unknown(module, target, "dynamic-define-property", call);
  }
  if (call_named(module, function, "Object", "defineProperties")) {
    TSNode descriptors =
        count >= 2 ? unwrap(argument(arguments, 1)) : (TSNode){0};
    target = count ? object_for(module, argument(arguments, 0)) : SIZE_MAX;
    if (target == SIZE_MAX)
      return ARCHBIRD_OK;
    *out_object = target;
    if (count == 2 &&
        ab_tree_sitter_node_type(descriptors, "call_expression")) {
      TSNode descriptor_function =
          unwrap(ab_tree_sitter_child(descriptors, "function"));
      TSNode descriptor_arguments =
          ab_tree_sitter_child(descriptors, "arguments");
      if (call_named(module, descriptor_function, "Object",
                     "getOwnPropertyDescriptors") &&
          ts_node_named_child_count(descriptor_arguments) == 1) {
        size_t source = SIZE_MAX;
        status = process_expression(module, argument(descriptor_arguments, 0),
                                    &source);
        if (status != ARCHBIRD_OK)
          return status;
        if (source != SIZE_MAX)
          return copy_object(module, target, source, "descriptor-forward");
        return mark_unknown(module, target,
                            "unresolved-property-descriptor-source", call);
      }
    }
    if (!ab_tree_sitter_node_type(descriptors, "object"))
      return mark_unknown(module, target, "dynamic-define-properties", call);
    for (index = 0; status == ARCHBIRD_OK &&
                    index < ts_node_named_child_count(descriptors);
         index++) {
      TSNode row = ts_node_named_child(descriptors, index);
      TSNode key_node = ab_tree_sitter_node_type(row, "pair")
                            ? ab_tree_sitter_child(row, "key")
                            : row;
      JsSlice key;
      if (static_name(module->scan, key_node, &key))
        status = set_property(module, target, &key, NULL, "define-properties");
      else
        status = mark_unknown(module, target, "dynamic-property-key", row);
    }
    return status;
  }
  if (call_named(module, function, "Object", "assign")) {
    target = count ? object_for(module, argument(arguments, 0)) : SIZE_MAX;
    if (target == SIZE_MAX)
      return ARCHBIRD_OK;
    *out_object = target;
    for (index = 1; status == ARCHBIRD_OK && index < count; index++) {
      size_t source = SIZE_MAX;
      status = process_expression(module, argument(arguments, index), &source);
      if (status == ARCHBIRD_OK && source != SIZE_MAX)
        status = copy_object(module, target, source, "object-assign");
      else if (status == ARCHBIRD_OK)
        status = mark_unknown(module, target, "dynamic-object-assign",
                              argument(arguments, index));
    }
  }
  return status;
}

static ArchbirdStatus process_assignment(JsModule *module, TSNode node,
                                         size_t *out_object) {
  TSNode left = unwrap(ab_tree_sitter_child(node, "left"));
  TSNode right = ab_tree_sitter_child(node, "right");
  TSNode base;
  JsSlice key;
  JsSlice name;
  JsSlice origin;
  size_t value = SIZE_MAX;
  size_t object;
  ArchbirdStatus status = process_expression(module, right, &value);
  if (status != ARCHBIRD_OK)
    return status;
  *out_object = value;
  if (module_exports(module, left)) {
    TSNode unwrapped_right = unwrap(right);
    module->root = value;
    module->saw_commonjs = 1;
    module->root_value_export =
        (value < module->object_count && module->objects[value].module_value) ||
        ab_tree_sitter_node_type(unwrapped_right, "string") ||
        ab_tree_sitter_node_type(unwrapped_right, "number") ||
        ab_tree_sitter_node_type(unwrapped_right, "true") ||
        ab_tree_sitter_node_type(unwrapped_right, "false") ||
        ab_tree_sitter_node_type(unwrapped_right, "null") ||
        ab_tree_sitter_node_type(unwrapped_right, "undefined") ||
        ab_tree_sitter_node_type(unwrapped_right, "regex");
    module->root_value_has_origin =
        module->root_value_export &&
        identifier(module->scan, unwrapped_right, &module->root_value_origin);
    module->root_value_start = ts_node_start_byte(right);
    module->root_value_end = ts_node_end_byte(right);
    module->root_value_evidence_kind = "commonjs-module-value";
    module->root_dynamic =
        value == SIZE_MAX &&
        !(ab_tree_sitter_node_type(unwrap(right), "string") ||
          ab_tree_sitter_node_type(unwrap(right), "number") ||
          ab_tree_sitter_node_type(unwrap(right), "true") ||
          ab_tree_sitter_node_type(unwrap(right), "false") ||
          ab_tree_sitter_node_type(unwrap(right), "null") ||
          ab_tree_sitter_node_type(unwrap(right), "undefined") ||
          ab_tree_sitter_node_type(unwrap(right), "regex"));
    module->root_dynamic_start = ts_node_start_byte(right);
    module->root_dynamic_end = ts_node_end_byte(right);
    return ARCHBIRD_OK;
  }
  if (identifier(module->scan, left, &name)) {
    if (slice_literal(&name, "exports")) {
      module->exports_alias = value;
      module->saw_commonjs = 1;
    } else if (slice_literal(&name, "module")) {
      module->module_builtin = 0;
    } else if (slice_literal(&name, "Object")) {
      module->object_builtin = 0;
    } else if (slice_literal(&name, "require")) {
      module->require_builtin = 0;
    } else {
      status = set_binding(module, &name, value);
    }
    return status;
  }
  if (!static_member(module, left, &base, &key)) {
    object = object_for(module, ab_tree_sitter_child(left, "object"));
    return object == SIZE_MAX
               ? ARCHBIRD_OK
               : mark_unknown(module, object, "dynamic-property-key", left);
  }
  object = object_for(module, base);
  if (object == SIZE_MAX)
    return ARCHBIRD_OK;
  status = set_property(
      module, object, &key,
      identifier(module->scan, unwrap(right), &origin) ? &origin : NULL,
      "assignment");
  if (module_exports(module, base) || object == module->exports_alias)
    module->saw_commonjs = 1;
  return status;
}

static ArchbirdStatus process_expression(JsModule *module, TSNode node,
                                         size_t *out_object) {
  const char *type;
  JsSlice name;
  JsBinding *row;
  node = unwrap(node);
  *out_object = SIZE_MAX;
  if (ts_node_is_null(node))
    return ARCHBIRD_OK;
  type = ts_node_type(node);
  if (identifier(module->scan, node, &name)) {
    if (slice_literal(&name, "exports"))
      *out_object = module->exports_alias;
    else if ((row = find_binding(module, &name)) != NULL)
      *out_object = row->object;
    return ARCHBIRD_OK;
  }
  if (strcmp(type, "object") == 0)
    return object_literal(module, node, out_object);
  if (strcmp(type, "array") == 0 || strcmp(type, "new_expression") == 0) {
    ArchbirdStatus status = new_object(module, out_object);
    if (status == ARCHBIRD_OK)
      module->objects[*out_object].module_value = 1;
    return status;
  }
  if (strcmp(type, "assignment_expression") == 0)
    return process_assignment(module, node, out_object);
  if (strcmp(type, "function_expression") == 0 ||
      strcmp(type, "generator_function") == 0 ||
      strcmp(type, "arrow_function") == 0 || strcmp(type, "class") == 0 ||
      strcmp(type, "class_expression") == 0) {
    ArchbirdStatus status = new_object(module, out_object);
    if (status == ARCHBIRD_OK)
      module->objects[*out_object].module_value = 1;
    return status;
  }
  if (strcmp(type, "call_expression") == 0) {
    TSNode function = unwrap(ab_tree_sitter_child(node, "function"));
    TSNode arguments = ab_tree_sitter_child(node, "arguments");
    if (identifier(module->scan, function, &name) &&
        slice_literal(&name, "require"))
      return require_object(module, node, arguments, out_object);
    return object_call(module, node, function, arguments, out_object);
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus process_declaration(JsModule *module, TSNode node) {
  uint32_t count = ts_node_named_child_count(node);
  uint32_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode declarator = ts_node_named_child(node, index);
    TSNode name_node;
    TSNode value_node;
    JsSlice name;
    size_t value = SIZE_MAX;
    if (!ab_tree_sitter_node_type(declarator, "variable_declarator"))
      continue;
    name_node = ab_tree_sitter_child(declarator, "name");
    value_node = ab_tree_sitter_child(declarator, "value");
    if (!identifier(module->scan, name_node, &name))
      continue;
    status = process_expression(module, value_node, &value);
    if (status != ARCHBIRD_OK)
      break;
    if (slice_literal(&name, "module"))
      module->module_builtin = 0;
    else if (slice_literal(&name, "Object"))
      module->object_builtin = 0;
    else if (slice_literal(&name, "require"))
      module->require_builtin = 0;
    else if (slice_literal(&name, "exports")) {
      module->exports_alias = value;
      module->saw_commonjs = 1;
    } else
      status = set_binding(module, &name, value);
  }
  return status;
}

static ArchbirdStatus process_import_bindings(JsModule *module, TSNode node) {
  TSNode source_node = ab_tree_sitter_child(node, "source");
  JsSlice source;
  uint32_t count;
  uint32_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!string_value(module->scan, source_node, &source))
    return ARCHBIRD_OK;
  count = ts_node_named_child_count(node);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode clause = ts_node_named_child(node, index);
    uint32_t nested_count;
    uint32_t nested;
    if (!ab_tree_sitter_node_type(clause, "import_clause"))
      continue;
    nested_count = ts_node_named_child_count(clause);
    for (nested = 0; status == ARCHBIRD_OK && nested < nested_count; nested++) {
      TSNode item = ts_node_named_child(clause, nested);
      JsSlice local;
      size_t object;
      if (!ab_tree_sitter_node_type(item, "namespace_import") ||
          ts_node_named_child_count(item) != 1 ||
          !identifier(module->scan, ts_node_named_child(item, 0), &local))
        continue;
      status = new_object(module, &object);
      if (status == ARCHBIRD_OK)
        status = add_reexport(module, object, &source);
      if (status == ARCHBIRD_OK)
        status = set_binding(module, &local, object);
    }
  }
  return status;
}

static int node_mentions(AbTreeSitterScan *scan, TSNode node,
                         const char *name) {
  uint32_t count;
  uint32_t index;
  JsSlice value;
  if (identifier(scan, node, &value) && slice_literal(&value, name))
    return 1;
  count = ts_node_named_child_count(node);
  for (index = 0; index < count; index++)
    if (node_mentions(scan, ts_node_named_child(node, index), name))
      return 1;
  return 0;
}

static int module_value_expression(JsModule *module, TSNode node,
                                   JsSlice *origin, int *has_origin) {
  const char *type;
  size_t object;
  node = unwrap(node);
  *has_origin = 0;
  type = ts_node_is_null(node) ? "" : ts_node_type(node);
  if (identifier(module->scan, node, origin)) {
    object = object_for(module, node);
    *has_origin = 1;
    return object < module->object_count &&
           module->objects[object].module_value;
  }
  return strcmp(type, "function_expression") == 0 ||
         strcmp(type, "generator_function") == 0 ||
         strcmp(type, "arrow_function") == 0 || strcmp(type, "class") == 0 ||
         strcmp(type, "class_expression") == 0 ||
         strcmp(type, "new_expression") == 0 || strcmp(type, "array") == 0 ||
         strcmp(type, "string") == 0 || strcmp(type, "number") == 0 ||
         strcmp(type, "true") == 0 || strcmp(type, "false") == 0 ||
         strcmp(type, "null") == 0 || strcmp(type, "undefined") == 0 ||
         strcmp(type, "regex") == 0;
}

static ArchbirdStatus conditional_module_values(JsModule *module, TSNode node) {
  uint32_t count;
  uint32_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(node, "assignment_expression")) {
    TSNode left = unwrap(ab_tree_sitter_child(node, "left"));
    TSNode right = ab_tree_sitter_child(node, "right");
    JsSlice origin;
    int has_origin;
    if (module_exports(module, left) &&
        module_value_expression(module, right, &origin, &has_origin)) {
      static const uint8_t default_bytes[] = "default";
      JsSlice name = {default_bytes, sizeof(default_bytes) - 1,
                      ts_node_start_byte(right), ts_node_end_byte(right)};
      status = add_export_fact(module->scan, &name, right, NULL,
                               has_origin ? &origin : NULL,
                               "conditional-commonjs-module-value");
    }
  }
  count = ts_node_named_child_count(node);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++)
    status =
        conditional_module_values(module, ts_node_named_child(node, index));
  return status;
}

static ArchbirdStatus process_commonjs(JsModule *module, TSNode program) {
  uint32_t count = ts_node_named_child_count(program);
  uint32_t index;
  ArchbirdStatus status = new_object(module, &module->root);
  if (status != ARCHBIRD_OK)
    return status;
  module->exports_alias = module->root;
  module->module_builtin = 1;
  module->object_builtin = 1;
  module->require_builtin = 1;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode statement = ts_node_named_child(program, index);
    const char *type = ts_node_type(statement);
    if (strcmp(type, "lexical_declaration") == 0 ||
        strcmp(type, "variable_declaration") == 0)
      status = process_declaration(module, statement);
    else if (strcmp(type, "function_declaration") == 0 ||
             strcmp(type, "generator_function_declaration") == 0 ||
             strcmp(type, "class_declaration") == 0) {
      JsSlice name;
      size_t object;
      if (identifier(module->scan, ab_tree_sitter_child(statement, "name"),
                     &name)) {
        status = new_object(module, &object);
        if (status == ARCHBIRD_OK)
          module->objects[object].module_value = 1;
        if (status == ARCHBIRD_OK)
          status = set_binding(module, &name, object);
      }
    } else if (strcmp(type, "import_statement") == 0) {
      status = process_import_bindings(module, statement);
    } else if (strcmp(type, "expression_statement") == 0 &&
               ts_node_named_child_count(statement)) {
      size_t unused;
      status = process_expression(module, ts_node_named_child(statement, 0),
                                  &unused);
    } else if (strcmp(type, "export_statement") != 0 &&
               strcmp(type, "import_statement") != 0 &&
               (node_mentions(module->scan, statement, "module") ||
                node_mentions(module->scan, statement, "exports"))) {
      module->saw_commonjs = 1;
      status = conditional_module_values(module, statement);
      if (status == ARCHBIRD_OK)
        status =
            mark_unknown(module, module->root,
                         "conditional-or-nested-commonjs-mutation", statement);
    }
  }
  return status;
}

static ArchbirdStatus add_named_fact(AbTreeSitterScan *scan, const char *domain,
                                     const char *kind, const JsSlice *name,
                                     TSNode anchor, AbFact **out) {
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      scan->builder, domain, kind, "syntax-structure", name->start, name->end,
      name->data, name->length, name->data, name->length, out);
  if (status == ARCHBIRD_OK && !ts_node_is_null(anchor))
    status = ab_tree_sitter_track_fact_region(scan, *out, anchor);
  if (status == ARCHBIRD_OK && !ts_node_is_null(anchor))
    status = ab_tree_sitter_add_line(scan, *out, anchor);
  else if (status == ARCHBIRD_OK) {
    uint64_t line = 1;
    size_t offset;
    for (offset = 0; offset < name->start && offset < scan->source_length;
         offset++)
      if (scan->source[offset] == '\n')
        line++;
    status = ab_fact_add_u64_attribute(scan->engine, *out, "line", line);
  }
  return status;
}

static ArchbirdStatus add_export_fact(AbTreeSitterScan *scan,
                                      const JsSlice *name, TSNode anchor,
                                      const JsSlice *origin,
                                      const JsSlice *origin_name,
                                      const char *evidence_kind) {
  AbFact *fact = NULL;
  ArchbirdStatus status =
      add_named_fact(scan, "exports", "export", name, anchor, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "evidence_kind",
                                          (const uint8_t *)evidence_kind,
                                          strlen(evidence_kind));
  if (status == ARCHBIRD_OK && origin && origin->length)
    status = ab_fact_add_string_attribute(scan->engine, fact, "origin",
                                          origin->data, origin->length);
  if (status == ARCHBIRD_OK && origin_name && origin_name->length)
    status =
        ab_fact_add_string_attribute(scan->engine, fact, "origin_name",
                                     origin_name->data, origin_name->length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(scan->engine, fact, "local_definition",
                                       origin && origin->length ? 0 : 1);
  return status;
}

static ArchbirdStatus add_route_fact(AbTreeSitterScan *scan, const char *kind,
                                     const JsSlice *origin, TSNode anchor) {
  AbFact *fact = NULL;
  return add_named_fact(scan, "module-reexports", kind, origin, anchor, &fact);
}

static ArchbirdStatus add_unknown_fact(AbTreeSitterScan *scan,
                                       const char *reason, size_t start,
                                       size_t end) {
  AbFact *fact = NULL;
  return ab_bundle_builder_add_fact(
      scan->builder, "module-surface-unknowns", "unknown", "syntax-structure",
      start, end, (const uint8_t *)reason, strlen(reason),
      (const uint8_t *)reason, strlen(reason), &fact);
}

static TSNode child_type(TSNode node, const char *type) {
  uint32_t count = ts_node_child_count(node);
  uint32_t index;
  for (index = 0; index < count; index++)
    if (strcmp(ts_node_type(ts_node_child(node, index)), type) == 0)
      return ts_node_child(node, index);
  return (TSNode){0};
}

static ArchbirdStatus add_import_name(AbTreeSitterScan *scan,
                                      const JsSlice *module,
                                      const JsSlice *imported,
                                      const JsSlice *local, TSNode anchor,
                                      const char *kind) {
  AbFact *fact = NULL;
  ArchbirdStatus status =
      add_named_fact(scan, "imported-names", kind, imported, anchor, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "module",
                                          module->data, module->length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "imported",
                                          imported->data, imported->length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "local",
                                          local->data, local->length);
  return status;
}

static ArchbirdStatus extract_import(AbTreeSitterScan *scan, TSNode node) {
  static const uint8_t default_name[] = "default";
  static const uint8_t namespace_name[] = "*";
  TSNode source_node = ab_tree_sitter_child(node, "source");
  JsSlice source;
  AbFact *fact = NULL;
  ArchbirdStatus status;
  uint32_t count;
  uint32_t index;
  if (!string_value(scan, source_node, &source))
    return ARCHBIRD_OK;
  status = add_named_fact(scan, "imported-name-groups", "module", &source,
                          source_node, &fact);
  count = ts_node_named_child_count(node);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode clause = ts_node_named_child(node, index);
    uint32_t nested_count;
    uint32_t nested;
    if (!ab_tree_sitter_node_type(clause, "import_clause"))
      continue;
    nested_count = ts_node_named_child_count(clause);
    for (nested = 0; status == ARCHBIRD_OK && nested < nested_count; nested++) {
      TSNode item = ts_node_named_child(clause, nested);
      JsSlice imported;
      JsSlice local;
      if (identifier(scan, item, &local)) {
        imported.data = default_name;
        imported.length = sizeof(default_name) - 1;
        imported.start = local.start;
        imported.end = local.end;
        status =
            add_import_name(scan, &source, &imported, &local, item, "default");
      } else if (ab_tree_sitter_node_type(item, "namespace_import") &&
                 ts_node_named_child_count(item) == 1 &&
                 identifier(scan, ts_node_named_child(item, 0), &local)) {
        imported.data = namespace_name;
        imported.length = sizeof(namespace_name) - 1;
        imported.start = local.start;
        imported.end = local.end;
        status = add_import_name(scan, &source, &imported, &local, item,
                                 "namespace");
      } else if (ab_tree_sitter_node_type(item, "named_imports")) {
        uint32_t spec_count = ts_node_named_child_count(item);
        uint32_t spec;
        for (spec = 0; status == ARCHBIRD_OK && spec < spec_count; spec++) {
          TSNode row = ts_node_named_child(item, spec);
          TSNode name_node = ab_tree_sitter_child(row, "name");
          TSNode alias_node = ab_tree_sitter_child(row, "alias");
          if (!static_name(scan, name_node, &imported))
            continue;
          local = imported;
          if (!ts_node_is_null(alias_node))
            (void)static_name(scan, alias_node, &local);
          status =
              add_import_name(scan, &source, &imported, &local, row, "named");
        }
      }
    }
  }
  return status;
}

static ArchbirdStatus export_declaration(AbTreeSitterScan *scan,
                                         TSNode declaration) {
  const char *type = ts_node_type(declaration);
  ArchbirdStatus status = ARCHBIRD_OK;
  if (strcmp(type, "lexical_declaration") == 0 ||
      strcmp(type, "variable_declaration") == 0) {
    uint32_t count = ts_node_named_child_count(declaration);
    uint32_t index;
    for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
      TSNode row = ts_node_named_child(declaration, index);
      TSNode name_node = ab_tree_sitter_child(row, "name");
      JsSlice name;
      if (identifier(scan, name_node, &name))
        status = add_export_fact(scan, &name, name_node, NULL, &name,
                                 "esm-declaration");
      else if (ab_tree_sitter_node_type(row, "variable_declarator"))
        status = add_unknown_fact(scan, "destructured-esm-export",
                                  ts_node_start_byte(name_node),
                                  ts_node_end_byte(name_node));
    }
  } else {
    TSNode name_node = ab_tree_sitter_child(declaration, "name");
    JsSlice name;
    if (identifier(scan, name_node, &name))
      status = add_export_fact(scan, &name, name_node, NULL, &name,
                               "esm-declaration");
    else
      status = add_unknown_fact(scan, "unnamed-esm-declaration",
                                ts_node_start_byte(declaration),
                                ts_node_end_byte(declaration));
  }
  return status;
}

static ArchbirdStatus extract_export(AbTreeSitterScan *scan, TSNode node) {
  static const uint8_t default_name[] = "default";
  TSNode source_node = ab_tree_sitter_child(node, "source");
  TSNode declaration = ab_tree_sitter_child(node, "declaration");
  JsSlice source;
  int has_source = string_value(scan, source_node, &source);
  ArchbirdStatus status = ARCHBIRD_OK;
  TSNode default_node;
  uint32_t count;
  uint32_t index;
  default_node = child_type(node, "default");
  if (!ts_node_is_null(default_node)) {
    JsSlice name;
    name.data = default_name;
    name.length = sizeof(default_name) - 1;
    name.start = ts_node_start_byte(default_node);
    name.end = ts_node_end_byte(default_node);
    return add_export_fact(scan, &name, default_node, NULL, NULL,
                           "esm-default");
  }
  if (!ts_node_is_null(declaration))
    return export_declaration(scan, declaration);
  count = ts_node_named_child_count(node);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode child = ts_node_named_child(node, index);
    if (ab_tree_sitter_node_type(child, "export_clause")) {
      uint32_t spec_count = ts_node_named_child_count(child);
      uint32_t spec;
      for (spec = 0; status == ARCHBIRD_OK && spec < spec_count; spec++) {
        TSNode row = ts_node_named_child(child, spec);
        TSNode name_node = ab_tree_sitter_child(row, "name");
        TSNode alias_node = ab_tree_sitter_child(row, "alias");
        JsSlice original;
        JsSlice exported;
        if (!static_name(scan, name_node, &original))
          continue;
        exported = original;
        if (!ts_node_is_null(alias_node))
          (void)static_name(scan, alias_node, &exported);
        status = add_export_fact(
            scan, &exported, row, has_source ? &source : NULL, &original,
            has_source ? "esm-named-reexport" : "esm-local-list");
      }
    } else if (ab_tree_sitter_node_type(child, "namespace_export") &&
               ts_node_named_child_count(child) == 1) {
      JsSlice exported;
      static const uint8_t all_name[] = "*";
      JsSlice original = {all_name, sizeof(all_name) - 1, 0, 0};
      TSNode name_node = ts_node_named_child(child, 0);
      if (static_name(scan, name_node, &exported))
        status =
            add_export_fact(scan, &exported, child, has_source ? &source : NULL,
                            &original, "esm-namespace-reexport");
    }
  }
  if (status == ARCHBIRD_OK && has_source) {
    int has_surface = 0;
    for (index = 0; index < count; index++) {
      TSNode child = ts_node_named_child(node, index);
      if (ab_tree_sitter_node_type(child, "export_clause") ||
          ab_tree_sitter_node_type(child, "namespace_export"))
        has_surface = 1;
    }
    if (!has_surface)
      status = add_route_fact(scan, "esm-star", &source, source_node);
  }
  if (status == ARCHBIRD_OK && has_source) {
    AbFact *fact = NULL;
    status = add_named_fact(scan, "imports", "reexport", &source, source_node,
                            &fact);
  }
  return status;
}

static ArchbirdStatus emit_commonjs(JsModule *module) {
  JsObject *root;
  ArchbirdStatus status = ARCHBIRD_OK;
  size_t index;
  if (!module->saw_commonjs)
    return ARCHBIRD_OK;
  if (module->root_value_export) {
    static const uint8_t default_bytes[] = "default";
    JsSlice name = {default_bytes, sizeof(default_bytes) - 1,
                    module->root_value_start, module->root_value_end};
    status = add_export_fact(
        module->scan, &name, (TSNode){0}, NULL,
        module->root_value_has_origin ? &module->root_value_origin : NULL,
        module->root_value_evidence_kind);
  }
  if (module->root >= module->object_count)
    return status != ARCHBIRD_OK ? status
           : module->root_dynamic
               ? add_unknown_fact(module->scan, "dynamic-module-exports",
                                  module->root_dynamic_start,
                                  module->root_dynamic_end)
               : ARCHBIRD_OK;
  root = &module->objects[module->root];
  for (index = 0; status == ARCHBIRD_OK && index < root->property_count;
       index++) {
    JsProperty *row = &root->properties[index];
    status = add_export_fact(module->scan, &row->name, (TSNode){0}, NULL,
                             row->has_origin_name ? &row->origin_name : NULL,
                             row->evidence_kind);
  }
  for (index = 0; status == ARCHBIRD_OK && index < root->reexport_count;
       index++)
    status = add_route_fact(module->scan, "commonjs-require",
                            &root->reexports[index], (TSNode){0});
  for (index = 0; status == ARCHBIRD_OK && index < root->unknown_count; index++)
    status = add_unknown_fact(module->scan, root->unknowns[index].reason,
                              root->unknowns[index].start,
                              root->unknowns[index].end);
  return status;
}

static void module_free(JsModule *module) {
  size_t index;
  for (index = 0; index < module->object_count; index++) {
    ab_free(module->scan->engine, module->objects[index].properties);
    ab_free(module->scan->engine, module->objects[index].reexports);
    ab_free(module->scan->engine, module->objects[index].unknowns);
  }
  ab_free(module->scan->engine, module->objects);
  ab_free(module->scan->engine, module->bindings);
}

ArchbirdStatus ab_tree_sitter_extract_ecmascript_module(AbTreeSitterScan *scan,
                                                        TSNode program) {
  JsModule module;
  uint32_t count;
  uint32_t index;
  ArchbirdStatus status;
  if (!scan || !ab_tree_sitter_node_type(program, "program"))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&module, 0, sizeof(module));
  module.scan = scan;
  status = process_commonjs(&module, program);
  count = ts_node_named_child_count(program);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode node = ts_node_named_child(program, index);
    if (ab_tree_sitter_node_type(node, "import_statement"))
      status = extract_import(scan, node);
    else if (ab_tree_sitter_node_type(node, "export_statement"))
      status = extract_export(scan, node);
  }
  if (status == ARCHBIRD_OK)
    status = emit_commonjs(&module);
  module_free(&module);
  return status;
}
