#include "projection_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbGraphInputs {
  AbProjectionData membership;
  AbProjectionData components;
  AbProjectionData symbols;
  AbProjectionData file_edges;
  AbProjectionData symbol_relations;
  AbProjectionData test_routes;
  AbProjectionData build_routes;
  AbProjectionData package_entrypoints;
  int has_components;
  int has_symbols;
  int has_file_edges;
  int has_symbol_relations;
  int has_test_routes;
  int has_build_routes;
  int has_package_entrypoints;
} AbGraphInputs;

typedef struct AbGraphEndpoints {
  AbString *values;
  size_t count;
  size_t capacity;
} AbGraphEndpoints;

typedef struct AbGraphMembershipRef {
  const AbString *group;
  const AbString *node;
} AbGraphMembershipRef;

static int string_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int string_starts(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length >= length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const AbValue *attribute(const AbProjectionItem *item,
                                const char *name) {
  size_t length = strlen(name);
  size_t index;
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        (!length ||
         memcmp(item->attributes[index].name.data, name, length) == 0))
      return &item->attributes[index].value;
  return NULL;
}

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal) {
  return ab_string_copy(engine, out, literal, strlen(literal));
}

static ArchbirdStatus add_attribute(ArchbirdEngine *engine,
                                    AbProjectionItem *item, const char *name,
                                    const AbValue *value) {
  AbObjectField *resized;
  AbObjectField *field;
  ArchbirdStatus status;
  if (item->attribute_count == SIZE_MAX / sizeof(*resized))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many graph projection attributes");
  resized = (AbObjectField *)ab_realloc(engine, item->attributes,
                                        (item->attribute_count + 1) *
                                            sizeof(*item->attributes));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing graph attributes");
  item->attributes = resized;
  field = &item->attributes[item->attribute_count];
  memset(field, 0, sizeof(*field));
  status = copy_literal(engine, &field->name, name);
  if (status == ARCHBIRD_OK)
    status = ab_value_copy(engine, &field->value, value);
  if (status == ARCHBIRD_OK)
    item->attribute_count++;
  else {
    ab_string_free(engine, &field->name);
    ab_value_free(engine, &field->value);
  }
  return status;
}

static ArchbirdStatus add_string_attribute(ArchbirdEngine *engine,
                                           AbProjectionItem *item,
                                           const char *name,
                                           const AbString *value) {
  AbValue row = {.kind = AB_VALUE_STRING, .as.text = *value};
  return add_attribute(engine, item, name, &row);
}

static ArchbirdStatus add_literal_attribute(ArchbirdEngine *engine,
                                            AbProjectionItem *item,
                                            const char *name,
                                            const char *value) {
  AbString text = {(char *)value, strlen(value)};
  return add_string_attribute(engine, item, name, &text);
}

static ArchbirdStatus add_u64_attribute(ArchbirdEngine *engine,
                                        AbProjectionItem *item,
                                        const char *name, uint64_t value) {
  char digits[32];
  int length = snprintf(digits, sizeof(digits), "%" PRIu64, value);
  AbValue row = {0};
  if (length < 1 || (size_t)length >= sizeof(digits))
    return ARCHBIRD_LIMIT_EXCEEDED;
  row.kind = AB_VALUE_INTEGER;
  row.as.text.data = digits;
  row.as.text.length = (size_t)length;
  return add_attribute(engine, item, name, &row);
}

static ArchbirdStatus set_u64_attribute(ArchbirdEngine *engine,
                                        AbProjectionItem *item,
                                        const char *name, uint64_t value) {
  size_t index;
  for (index = 0; index < item->attribute_count; index++)
    if (string_is(&item->attributes[index].name, name)) {
      char digits[32];
      int length = snprintf(digits, sizeof(digits), "%" PRIu64, value);
      AbValue replacement = {0};
      if (length < 1 || (size_t)length >= sizeof(digits))
        return ARCHBIRD_LIMIT_EXCEEDED;
      replacement.kind = AB_VALUE_INTEGER;
      if (ab_string_copy(engine, &replacement.as.text, digits,
                         (size_t)length) != ARCHBIRD_OK)
        return ARCHBIRD_OUT_OF_MEMORY;
      ab_value_free(engine, &item->attributes[index].value);
      item->attributes[index].value = replacement;
      return ARCHBIRD_OK;
    }
  return add_u64_attribute(engine, item, name, value);
}

static uint64_t u64_attribute(const AbProjectionItem *item, const char *name) {
  const AbValue *value = attribute(item, name);
  uint64_t number = 0;
  if (value)
    (void)ab_value_u64(value, &number);
  return number;
}

static ArchbirdStatus copy_evidence(ArchbirdEngine *engine,
                                    AbProjectionItem *target,
                                    const AbProjectionItem *source) {
  size_t index;
  for (index = 0; index < source->evidence_count; index++) {
    ArchbirdStatus status = ab_projection_item_add_evidence(
        engine, target, &source->evidence[index]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int state_rank(const AbString *state) {
  if (string_is(state, "unknown"))
    return 2;
  if (string_is(state, "stale"))
    return 1;
  return 0;
}

static ArchbirdStatus preserve_weaker_state(ArchbirdEngine *engine,
                                            AbProjectionItem *target,
                                            const AbProjectionItem *source) {
  if (state_rank(&source->state) > state_rank(&target->state))
    return ab_projection_item_set_state(engine, target, source->state.data,
                                        source->message.data);
  return ARCHBIRD_OK;
}

static ArchbirdStatus buffer_identity(ArchbirdEngine *engine,
                                      const char *record_kind,
                                      const AbString *kind,
                                      const AbString *identity, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, record_kind);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, kind->data, kind->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, identity->data, identity->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus graph_item(AbProjectionContext *context,
                                 AbProjectionData *graph, const char *record,
                                 const AbString *kind, const AbString *identity,
                                 const AbString *label,
                                 const AbProjectionItem *source,
                                 AbProjectionItem **out) {
  AbString key = {0};
  AbProjectionItem item = {0};
  AbProjectionItem *existing = NULL;
  ArchbirdStatus status =
      buffer_identity(context->engine, record, kind, identity, &key);
  if (status == ARCHBIRD_OK)
    status =
        ab_projection_data_find_item(context->engine, graph, &key, &existing);
  if (status == ARCHBIRD_OK && existing && source)
    status = copy_evidence(context->engine, existing, source);
  if (status == ARCHBIRD_OK && existing && source)
    status = preserve_weaker_state(context->engine, existing, source);
  if (status == ARCHBIRD_OK && existing) {
    *out = existing;
    ab_string_free(context->engine, &key);
    return ARCHBIRD_OK;
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_item_init(context->engine, &item, &key, label, NULL);
  if (status == ARCHBIRD_OK)
    status =
        add_literal_attribute(context->engine, &item, "record_kind", record);
  if (status == ARCHBIRD_OK)
    status = add_string_attribute(context->engine, &item, "id", identity);
  if (status == ARCHBIRD_OK)
    status = add_string_attribute(context->engine, &item, "entity_kind", kind);
  if (status == ARCHBIRD_OK && source)
    status = copy_evidence(context->engine, &item, source);
  if (status == ARCHBIRD_OK && source)
    status = ab_projection_item_set_state(
        context->engine, &item, source->state.data, source->message.data);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_add_item(context->engine, graph, &item);
  if (status == ARCHBIRD_OK)
    *out = &graph->items[graph->item_count - 1];
  else
    ab_projection_item_free(context->engine, &item);
  ab_string_free(context->engine, &key);
  return status;
}

static ArchbirdStatus evaluate_select(AbProjectionContext *context,
                                      const char *id, const char *select,
                                      AbProjectionData *out) {
  AbObjectField field = {0};
  AbValue definition = {.kind = AB_VALUE_OBJECT};
  AbProjectionPlan plan = {0};
  AbString plan_id = {(char *)id, strlen(id)};
  ArchbirdStatus status;
  field.name.data = (char *)"select";
  field.name.length = 6;
  field.value.kind = AB_VALUE_STRING;
  field.value.as.text.data = (char *)select;
  field.value.as.text.length = strlen(select);
  definition.as.object.fields = &field;
  definition.as.object.count = 1;
  status =
      ab_projection_plan_compile(context->engine, &definition, &plan_id, &plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_extract_map(context->engine, context->map,
                                       context->resolution, &plan, out);
  ab_projection_plan_free(context->engine, &plan);
  return status;
}

static void inputs_free(ArchbirdEngine *engine, AbGraphInputs *inputs) {
  ab_projection_data_free(engine, &inputs->membership);
  ab_projection_data_free(engine, &inputs->components);
  ab_projection_data_free(engine, &inputs->symbols);
  ab_projection_data_free(engine, &inputs->file_edges);
  ab_projection_data_free(engine, &inputs->symbol_relations);
  ab_projection_data_free(engine, &inputs->test_routes);
  ab_projection_data_free(engine, &inputs->build_routes);
  ab_projection_data_free(engine, &inputs->package_entrypoints);
  memset(inputs, 0, sizeof(*inputs));
}

static int request_has(const AbProjectionPlan *plan, const char *field,
                       const char *value) {
  const AbValue *array = ab_value_member(&plan->definition, field);
  size_t index;
  for (index = 0; array && index < array->as.array.count; index++)
    if (ab_projection_value_is(&array->as.array.items[index], value))
      return 1;
  return 0;
}

static ArchbirdStatus inputs_load(AbProjectionContext *context,
                                  const AbProjectionPlan *plan,
                                  AbGraphInputs *inputs) {
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const AbValue *group = ab_value_member(&plan->definition, "group_by");
  int file_relations = request_has(plan, "relations", "imports") ||
                       request_has(plan, "relations", "declarations") ||
                       request_has(plan, "relations", "packages") ||
                       request_has(plan, "relations", "bridges");
  ArchbirdStatus status = evaluate_select(
      context, "graph-membership", "component_membership", &inputs->membership);
  if (status == ARCHBIRD_OK && (ab_projection_value_is(level, "component") ||
                                ab_projection_value_is(group, "component"))) {
    status = evaluate_select(context, "graph-components", "components",
                             &inputs->components);
    inputs->has_components = status == ARCHBIRD_OK &&
                             !string_is(&inputs->components.state, "unknown");
  }
  if (status == ARCHBIRD_OK && ab_projection_value_is(level, "symbol")) {
    status = evaluate_select(context, "graph-symbols", "symbol_entities",
                             &inputs->symbols);
    inputs->has_symbols =
        status == ARCHBIRD_OK && !string_is(&inputs->symbols.state, "unknown");
  }
  if (status == ARCHBIRD_OK && file_relations) {
    status = evaluate_select(context, "graph-file-edges", "file_edges",
                             &inputs->file_edges);
    inputs->has_file_edges = status == ARCHBIRD_OK &&
                             !string_is(&inputs->file_edges.state, "unknown");
  }
  if (status == ARCHBIRD_OK && (request_has(plan, "relations", "calls") ||
                                request_has(plan, "relations", "references"))) {
    status = evaluate_select(context, "graph-symbol-relations",
                             "symbol_relations", &inputs->symbol_relations);
    inputs->has_symbol_relations =
        status == ARCHBIRD_OK &&
        !string_is(&inputs->symbol_relations.state, "unknown");
  }
  if (status == ARCHBIRD_OK && request_has(plan, "relations", "tests")) {
    status = evaluate_select(context, "graph-test-routes", "test_routes",
                             &inputs->test_routes);
    inputs->has_test_routes = status == ARCHBIRD_OK &&
                              !string_is(&inputs->test_routes.state, "unknown");
  }
  if (status == ARCHBIRD_OK && request_has(plan, "relations", "builds")) {
    status = evaluate_select(context, "graph-build-routes", "build_routes",
                             &inputs->build_routes);
    inputs->has_build_routes =
        status == ARCHBIRD_OK &&
        !string_is(&inputs->build_routes.state, "unknown");
  }
  if (status == ARCHBIRD_OK && request_has(plan, "relations", "packages")) {
    status =
        evaluate_select(context, "graph-package-entrypoints",
                        "package_entrypoints", &inputs->package_entrypoints);
    inputs->has_package_entrypoints =
        status == ARCHBIRD_OK &&
        !string_is(&inputs->package_entrypoints.state, "unknown");
  }
  return status;
}

static const AbProjectionData *
unavailable_node_inventory(const AbProjectionPlan *plan,
                           const AbGraphInputs *inputs) {
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const AbValue *group = ab_value_member(&plan->definition, "group_by");
  if (string_is(&inputs->membership.state, "unknown"))
    return &inputs->membership;
  if ((ab_projection_value_is(level, "component") ||
       ab_projection_value_is(group, "component")) &&
      string_is(&inputs->components.state, "unknown"))
    return &inputs->components;
  if (ab_projection_value_is(level, "symbol") &&
      string_is(&inputs->symbols.state, "unknown"))
    return &inputs->symbols;
  return NULL;
}

static const AbProjectionItem *membership_file(const AbGraphInputs *inputs,
                                               const AbString *path) {
  size_t index;
  for (index = 0; index < inputs->membership.item_count; index++)
    if (ab_string_equal(&inputs->membership.items[index].key, path))
      return &inputs->membership.items[index];
  return NULL;
}

static const AbProjectionItem *configured_component(const AbGraphInputs *inputs,
                                                    const AbString *name) {
  size_t index;
  for (index = 0; index < inputs->components.item_count; index++)
    if (ab_string_equal(&inputs->components.items[index].key, name))
      return &inputs->components.items[index];
  return NULL;
}

static AbString top_area(const AbString *path) {
  AbString area = *path;
  size_t index;
  for (index = 0; index < area.length; index++)
    if (area.data[index] == '/') {
      area.length = index;
      return area;
    }
  return (AbString){(char *)".", 1};
}

static const char *peripheral_kind(const AbString *identity) {
  if (string_starts(identity, "package:"))
    return "package";
  if (string_starts(identity, "build:"))
    return "build";
  if (string_starts(identity, "builtin:"))
    return "builtin";
  if (string_starts(identity, "unresolved:"))
    return "unresolved";
  return "external";
}

static const char *inventory_label(const AbString *kind) {
  if (string_is(kind, "build"))
    return "Builds";
  if (string_is(kind, "package"))
    return "Packages";
  if (string_is(kind, "builtin"))
    return "Built-ins";
  if (string_is(kind, "external"))
    return "External";
  if (string_is(kind, "unresolved"))
    return "Unresolved";
  return NULL;
}

static ArchbirdStatus node_id(ArchbirdEngine *engine, const char *kind,
                              const AbString *identity, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, kind);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, identity->data, identity->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_node(AbProjectionContext *context,
                               AbProjectionData *graph, const char *kind,
                               const AbString *identity, const AbString *label,
                               const AbString *path,
                               const AbProjectionItem *source,
                               AbString *out_id) {
  AbString kind_string = {(char *)kind, strlen(kind)};
  AbString id = {0};
  AbProjectionItem *item = NULL;
  ArchbirdStatus status = node_id(context->engine, kind, identity, &id);
  if (status == ARCHBIRD_OK)
    status = graph_item(context, graph, "node", &kind_string, &id, label,
                        source, &item);
  if (status == ARCHBIRD_OK && path && !attribute(item, "path"))
    status = add_string_attribute(context->engine, item, "path", path);
  if (status == ARCHBIRD_OK && out_id)
    status = ab_string_copy(context->engine, out_id, id.data, id.length);
  ab_string_free(context->engine, &id);
  return status;
}

static ArchbirdStatus
add_group_membership(AbProjectionContext *context, AbProjectionData *graph,
                     const AbString *node, const char *group_by,
                     const AbString *identity, const AbString *label,
                     const char *origin, const AbProjectionItem *source,
                     const AbProjectionItem *metadata) {
  AbString group_kind = {(char *)group_by, strlen(group_by)};
  AbString group_id = {0};
  AbString group_key = {0};
  AbProjectionItem *group = NULL;
  AbProjectionItem *membership = NULL;
  AbBuffer membership_identity;
  AbString membership_key;
  AbString membership_kind = {(char *)"membership", 10};
  int new_membership = 0;
  ArchbirdStatus status =
      node_id(context->engine, group_by, identity, &group_id);
  if (status == ARCHBIRD_OK)
    status = graph_item(context, graph, "group", &group_kind, &group_id, label,
                        source, &group);
  if (status == ARCHBIRD_OK && !attribute(group, "group_by"))
    status =
        add_literal_attribute(context->engine, group, "group_by", group_by);
  if (status == ARCHBIRD_OK && !attribute(group, "origin"))
    status = add_literal_attribute(context->engine, group, "origin", origin);
  if (status == ARCHBIRD_OK && !strcmp(group_by, "inventory") &&
      !attribute(group, "inventory_kind"))
    status = add_string_attribute(context->engine, group, "inventory_kind",
                                  identity);
  if (status == ARCHBIRD_OK && metadata)
    status = copy_evidence(context->engine, group, metadata);
  if (status == ARCHBIRD_OK && metadata)
    status = preserve_weaker_state(context->engine, group, metadata);
  if (status == ARCHBIRD_OK && metadata) {
    size_t attribute_index;
    for (attribute_index = 0;
         status == ARCHBIRD_OK && attribute_index < metadata->attribute_count;
         attribute_index++)
      if (!attribute(group, metadata->attributes[attribute_index].name.data))
        status = add_attribute(context->engine, group,
                               metadata->attributes[attribute_index].name.data,
                               &metadata->attributes[attribute_index].value);
  }
  ab_buffer_init(&membership_identity, context->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&membership_identity, group_id.data,
                                   group_id.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&membership_identity, ",");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&membership_identity, node->data, node->length);
  membership_key.data = (char *)membership_identity.data;
  membership_key.length = membership_identity.length;
  if (status == ARCHBIRD_OK)
    status = graph_item(context, graph, "membership", &membership_kind,
                        &membership_key, label, source, &membership);
  if (status == ARCHBIRD_OK)
    new_membership = !attribute(membership, "group");
  if (status == ARCHBIRD_OK && !attribute(membership, "group"))
    status =
        add_string_attribute(context->engine, membership, "group", &group_id);
  if (status == ARCHBIRD_OK && !attribute(membership, "node"))
    status = add_string_attribute(context->engine, membership, "node", node);
  if (status == ARCHBIRD_OK && new_membership)
    status = buffer_identity(context->engine, "group", &group_kind, &group_id,
                             &group_key);
  if (status == ARCHBIRD_OK && new_membership)
    status = ab_projection_data_find_item(context->engine, graph, &group_key,
                                          &group);
  if (status == ARCHBIRD_OK && new_membership && !group)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK && new_membership)
    status = set_u64_attribute(context->engine, group, "member_count",
                               u64_attribute(group, "member_count") + 1);
  ab_buffer_free(&membership_identity);
  ab_string_free(context->engine, &group_key);
  ab_string_free(context->engine, &group_id);
  return status;
}

static ArchbirdStatus group_node(AbProjectionContext *context,
                                 const AbProjectionPlan *plan,
                                 const AbGraphInputs *inputs,
                                 AbProjectionData *graph,
                                 const AbProjectionItem *source,
                                 const AbString *node, const AbString *path) {
  const AbValue *group = ab_value_member(&plan->definition, "group_by");
  const AbProjectionItem *file;
  const AbValue *values;
  size_t index;
  if (!group)
    return ARCHBIRD_OK;
  file = membership_file(inputs, path);
  if (ab_projection_value_is(group, "directory")) {
    AbString area = top_area(path);
    return add_group_membership(context, graph, node, "directory", &area, &area,
                                "discovered", source, NULL);
  }
  if (!file)
    return add_group_membership(
        context, graph, node, "directory", &(AbString){(char *)"external", 8},
        &(AbString){(char *)"External", 8}, "derived", source, NULL);
  if (ab_projection_value_is(group, "language") ||
      ab_projection_value_is(group, "layer")) {
    const char *name =
        ab_projection_value_is(group, "language") ? "language" : "layer";
    const AbValue *value = attribute(file, name);
    const AbValue *layer_origin = attribute(file, "layer_origin");
    const char *origin =
        name[0] == 'l' && layer_origin &&
                ab_projection_value_is(layer_origin, "configuration")
            ? "configured"
            : "discovered";
    if (!value || value->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    return add_group_membership(context, graph, node, name, &value->as.text,
                                &value->as.text, origin, source, NULL);
  }
  values = attribute(file, "components");
  if (!values || values->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!values->as.array.count) {
    AbString area = top_area(path);
    AbBuffer identity;
    AbString key;
    ArchbirdStatus status;
    ab_buffer_init(&identity, context->engine);
    status = ab_buffer_literal(&identity, "unassigned:");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&identity, area.data, area.length);
    key.data = (char *)identity.data;
    key.length = identity.length;
    if (status == ARCHBIRD_OK)
      status = add_group_membership(context, graph, node, "component", &key,
                                    &area, "unassigned", source, NULL);
    ab_buffer_free(&identity);
    return status;
  }
  for (index = 0; index < values->as.array.count; index++) {
    const AbString *name = &values->as.array.items[index].as.text;
    ArchbirdStatus status = add_group_membership(
        context, graph, node, "component", name, name, "configured", source,
        configured_component(inputs, name));
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_graph_nodes(AbProjectionContext *context,
                                      const AbProjectionPlan *plan,
                                      const AbGraphInputs *inputs,
                                      AbProjectionData *graph) {
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const AbProjectionData *source;
  size_t index;
  if (ab_projection_value_is(level, "component"))
    source = &inputs->components;
  else if (ab_projection_value_is(level, "symbol"))
    source = &inputs->symbols;
  else
    source = &inputs->membership;
  for (index = 0; index < source->item_count; index++) {
    const AbProjectionItem *row = &source->items[index];
    const AbValue *path_value = attribute(row, "path");
    const AbString *path = path_value && path_value->kind == AB_VALUE_STRING
                               ? &path_value->as.text
                               : &row->key;
    const char *kind =
        ab_projection_value_is(level, "component")
            ? "component"
            : (ab_projection_value_is(level, "symbol") ? "symbol" : "file");
    const AbString *identity =
        ab_projection_value_is(level, "symbol") ? &row->key : &row->key;
    AbString id = {0};
    size_t attribute_index;
    ArchbirdStatus status =
        add_node(context, graph, kind, identity, &row->label, path, row, &id);
    if (status != ARCHBIRD_OK)
      return status;
    for (attribute_index = 0;
         status == ARCHBIRD_OK && attribute_index < row->attribute_count;
         attribute_index++) {
      AbProjectionItem *target = NULL;
      AbString node_key = {0};
      AbString node_kind = {(char *)kind, strlen(kind)};
      status =
          buffer_identity(context->engine, "node", &node_kind, &id, &node_key);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_find_item(context->engine, graph, &node_key,
                                              &target);
      if (status == ARCHBIRD_OK &&
          !attribute(target, row->attributes[attribute_index].name.data))
        status = add_attribute(context->engine, target,
                               row->attributes[attribute_index].name.data,
                               &row->attributes[attribute_index].value);
      ab_string_free(context->engine, &node_key);
    }
    if (status == ARCHBIRD_OK && !ab_projection_value_is(level, "component"))
      status = group_node(context, plan, inputs, graph, row, &id, path);
    ab_string_free(context->engine, &id);
    if (status != ARCHBIRD_OK)
      return status;
  }
  if (ab_projection_value_is(level, "component"))
    for (index = 0; index < inputs->membership.item_count; index++) {
      const AbProjectionItem *file = &inputs->membership.items[index];
      const AbValue *components = attribute(file, "components");
      AbString area;
      AbBuffer identity;
      AbString unassigned;
      ArchbirdStatus status;
      if (!components || components->kind != AB_VALUE_ARRAY)
        return ARCHBIRD_INVALID_SCHEMA;
      if (components->as.array.count)
        continue;
      area = top_area(&file->key);
      ab_buffer_init(&identity, context->engine);
      status = ab_buffer_append(&identity, area.data, area.length);
      unassigned.data = (char *)identity.data;
      unassigned.length = identity.length;
      if (status == ARCHBIRD_OK)
        status = add_node(context, graph, "unassigned", &unassigned, &area,
                          NULL, file, NULL);
      ab_buffer_free(&identity);
      if (status != ARCHBIRD_OK)
        return status;
    }
  return ARCHBIRD_OK;
}

static ArchbirdStatus endpoint_add(ArchbirdEngine *engine,
                                   AbGraphEndpoints *endpoints,
                                   const char *kind, const AbString *identity) {
  AbString *resized;
  ArchbirdStatus status;
  if (endpoints->count == endpoints->capacity) {
    size_t capacity = endpoints->capacity ? endpoints->capacity * 2 : 4;
    if (capacity < endpoints->capacity ||
        capacity > SIZE_MAX / sizeof(*endpoints->values))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many graph relation endpoints");
    resized = (AbString *)ab_realloc(engine, endpoints->values,
                                     capacity * sizeof(*endpoints->values));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing graph endpoints");
    memset(resized + endpoints->capacity, 0,
           (capacity - endpoints->capacity) * sizeof(*resized));
    endpoints->values = resized;
    endpoints->capacity = capacity;
  }
  status =
      node_id(engine, kind, identity, &endpoints->values[endpoints->count]);
  if (status == ARCHBIRD_OK)
    endpoints->count++;
  return status;
}

static void endpoints_free(ArchbirdEngine *engine,
                           AbGraphEndpoints *endpoints) {
  size_t index;
  for (index = 0; index < endpoints->count; index++)
    ab_string_free(engine, &endpoints->values[index]);
  ab_free(engine, endpoints->values);
  memset(endpoints, 0, sizeof(*endpoints));
}

static ArchbirdStatus endpoints_for_path(AbProjectionContext *context,
                                         const AbProjectionPlan *plan,
                                         const AbGraphInputs *inputs,
                                         const AbString *path,
                                         AbGraphEndpoints *endpoints) {
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const AbProjectionItem *file = membership_file(inputs, path);
  const AbValue *components;
  size_t index;
  if (!ab_projection_value_is(level, "component"))
    return endpoint_add(context->engine, endpoints,
                        file ? "file" : peripheral_kind(path), path);
  if (!file)
    return endpoint_add(context->engine, endpoints, peripheral_kind(path),
                        path);
  components = attribute(file, "components");
  if (!components || components->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!components->as.array.count) {
    AbString area = top_area(path);
    return endpoint_add(context->engine, endpoints, "unassigned", &area);
  }
  for (index = 0; index < components->as.array.count; index++) {
    ArchbirdStatus status =
        endpoint_add(context->engine, endpoints, "component",
                     &components->as.array.items[index].as.text);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus ensure_endpoint_node(AbProjectionContext *context,
                                           AbProjectionData *graph,
                                           const AbString *id,
                                           const AbString *label,
                                           const AbProjectionItem *source) {
  const char *colon = memchr(id->data, ':', id->length);
  AbString kind;
  AbString identity;
  const char *known_kind;
  if (!colon)
    return ARCHBIRD_INVALID_SCHEMA;
  kind.data = id->data;
  kind.length = (size_t)(colon - id->data);
  identity.data = (char *)colon + 1;
  identity.length = id->length - kind.length - 1;
  if (kind.length == 4 && !memcmp(kind.data, "file", 4))
    known_kind = "file";
  else if (kind.length == 6 && !memcmp(kind.data, "symbol", 6))
    known_kind = "symbol";
  else if (kind.length == 9 && !memcmp(kind.data, "component", 9))
    known_kind = "component";
  else if (kind.length == 10 && !memcmp(kind.data, "unassigned", 10))
    known_kind = "unassigned";
  else if (kind.length == 5 && !memcmp(kind.data, "build", 5))
    known_kind = "build";
  else if (kind.length == 7 && !memcmp(kind.data, "package", 7))
    known_kind = "package";
  else if (kind.length == 7 && !memcmp(kind.data, "builtin", 7))
    known_kind = "builtin";
  else if (kind.length == 10 && !memcmp(kind.data, "unresolved", 10))
    known_kind = "unresolved";
  else
    known_kind = "external";
  return add_node(
      context, graph, known_kind, &identity, label, NULL,
      (!strcmp(known_kind, "file") || !strcmp(known_kind, "symbol") ||
       !strcmp(known_kind, "component") || !strcmp(known_kind, "unassigned"))
          ? NULL
          : source,
      NULL);
}

static ArchbirdStatus relation_key(ArchbirdEngine *engine, const char *family,
                                   const AbString *kind, const AbString *source,
                                   const AbString *target, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, family, strlen(family));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, kind->data, kind->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, source->data, source->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ",");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&buffer, target->data, target->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "]");
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_relation(AbProjectionContext *context,
                                   AbProjectionData *graph, const char *family,
                                   const AbString *kind, const AbString *source,
                                   const AbString *target,
                                   const AbProjectionItem *witness,
                                   uint64_t *collapsed) {
  AbString relation_kind = {(char *)"relation", 8};
  AbString identity = {0};
  AbProjectionItem *item = NULL;
  ArchbirdStatus status;
  if (ab_string_equal(source, target)) {
    (*collapsed)++;
    return ARCHBIRD_OK;
  }
  status =
      relation_key(context->engine, family, kind, source, target, &identity);
  if (status == ARCHBIRD_OK)
    status = graph_item(context, graph, "relation", &relation_kind, &identity,
                        &witness->label, witness, &item);
  if (status == ARCHBIRD_OK && !attribute(item, "source"))
    status = add_string_attribute(context->engine, item, "source", source);
  if (status == ARCHBIRD_OK && !attribute(item, "target"))
    status = add_string_attribute(context->engine, item, "target", target);
  if (status == ARCHBIRD_OK && !attribute(item, "relation_kind"))
    status = add_string_attribute(context->engine, item, "relation_kind", kind);
  if (status == ARCHBIRD_OK && !attribute(item, "family"))
    status = add_literal_attribute(context->engine, item, "family", family);
  if (status == ARCHBIRD_OK) {
    const AbValue *names = attribute(witness, "names");
    if (names && !attribute(item, "names"))
      status = add_attribute(context->engine, item, "names", names);
  }
  if (status == ARCHBIRD_OK) {
    const AbValue *resolution = attribute(witness, "resolution");
    if (resolution && !attribute(item, "resolution"))
      status = add_attribute(context->engine, item, "resolution", resolution);
  }
  if (status == ARCHBIRD_OK)
    status = set_u64_attribute(context->engine, item, "witness_count",
                               u64_attribute(item, "witness_count") + 1);
  ab_string_free(context->engine, &identity);
  return status;
}

static int membership_ref_compare(const void *left, const void *right) {
  const AbGraphMembershipRef *a = (const AbGraphMembershipRef *)left;
  const AbGraphMembershipRef *b = (const AbGraphMembershipRef *)right;
  int compared = ab_string_compare(a->node, b->node);
  return compared ? compared : ab_string_compare(a->group, b->group);
}

static size_t membership_ref_lower_bound(const AbGraphMembershipRef *rows,
                                         size_t count, const AbString *node) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (ab_string_compare(rows[middle].node, node) < 0)
      low = middle + 1;
    else
      high = middle;
  }
  return low;
}

static ArchbirdStatus add_peripheral_memberships(AbProjectionContext *context,
                                                 const AbProjectionPlan *plan,
                                                 AbProjectionData *graph) {
  const AbValue *group_by = ab_value_member(&plan->definition, "group_by");
  size_t initial_count = graph->item_count;
  size_t index;
  if (!group_by)
    return ARCHBIRD_OK;
  for (index = 0; index < initial_count; index++) {
    const AbProjectionItem *node = &graph->items[index];
    const AbValue *record = attribute(node, "record_kind");
    const AbValue *kind = attribute(node, "entity_kind");
    const AbValue *id = attribute(node, "id");
    const char *label;
    AbString label_text;
    ArchbirdStatus status;
    if (!ab_projection_value_is(record, "node") || !kind ||
        kind->kind != AB_VALUE_STRING || !id || id->kind != AB_VALUE_STRING)
      continue;
    label = inventory_label(&kind->as.text);
    if (!label)
      continue;
    label_text.data = (char *)label;
    label_text.length = strlen(label);
    status = add_group_membership(context, graph, &id->as.text, "inventory",
                                  &kind->as.text, &label_text, "derived", NULL,
                                  NULL);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_group_relation(AbProjectionContext *context,
                                         AbProjectionData *graph,
                                         size_t canonical_index,
                                         const AbString *source_group,
                                         const AbString *target_group) {
  const AbProjectionItem *canonical = &graph->items[canonical_index];
  const AbValue *family = attribute(canonical, "family");
  const AbValue *kind = attribute(canonical, "relation_kind");
  AbString item_kind = {(char *)"group_relation", 14};
  AbString identity = {0};
  AbProjectionItem *item = NULL;
  uint64_t current;
  uint64_t witnesses;
  uint64_t name_occurrences = 0;
  ArchbirdStatus status;
  if (!family || family->kind != AB_VALUE_STRING || !kind ||
      kind->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_SCHEMA;
  status = relation_key(context->engine, family->as.text.data, &family->as.text,
                        source_group, target_group, &identity);
  if (status == ARCHBIRD_OK)
    status = graph_item(context, graph, "group_relation", &item_kind, &identity,
                        &canonical->label, NULL, &item);
  canonical = &graph->items[canonical_index];
  family = attribute(canonical, "family");
  kind = attribute(canonical, "relation_kind");
  if (status == ARCHBIRD_OK && (!family || family->kind != AB_VALUE_STRING ||
                                !kind || kind->kind != AB_VALUE_STRING))
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK)
    status = copy_evidence(context->engine, item, canonical);
  if (status == ARCHBIRD_OK)
    status = preserve_weaker_state(context->engine, item, canonical);
  if (status == ARCHBIRD_OK && !attribute(item, "source"))
    status =
        add_string_attribute(context->engine, item, "source", source_group);
  if (status == ARCHBIRD_OK && !attribute(item, "target"))
    status =
        add_string_attribute(context->engine, item, "target", target_group);
  if (status == ARCHBIRD_OK && !attribute(item, "relation_kind"))
    status = add_string_attribute(context->engine, item, "relation_kind",
                                  &family->as.text);
  if (status == ARCHBIRD_OK && !attribute(item, "family"))
    status =
        add_string_attribute(context->engine, item, "family", &family->as.text);
  canonical = &graph->items[canonical_index];
  if (attribute(canonical, "names")) {
    const AbValue *names = attribute(canonical, "names");
    if (names->kind != AB_VALUE_ARRAY)
      status = ARCHBIRD_INVALID_SCHEMA;
    else
      name_occurrences = (uint64_t)names->as.array.count;
  }
  witnesses = u64_attribute(canonical, "witness_count");
  current = u64_attribute(item, "witness_count");
  if (status == ARCHBIRD_OK && UINT64_MAX - current < witnesses)
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK)
    status = set_u64_attribute(context->engine, item, "witness_count",
                               current + witnesses);
  current = u64_attribute(item, "relation_count");
  if (status == ARCHBIRD_OK && current == UINT64_MAX)
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK)
    status =
        set_u64_attribute(context->engine, item, "relation_count", current + 1);
  current = u64_attribute(item, "name_occurrence_count");
  if (status == ARCHBIRD_OK && UINT64_MAX - current < name_occurrences)
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK)
    status = set_u64_attribute(context->engine, item, "name_occurrence_count",
                               current + name_occurrences);
  current = u64_attribute(item, "relation_kind_occurrence_count");
  if (status == ARCHBIRD_OK && current == UINT64_MAX)
    status = ARCHBIRD_LIMIT_EXCEEDED;
  if (status == ARCHBIRD_OK)
    status = set_u64_attribute(context->engine, item,
                               "relation_kind_occurrence_count", current + 1);
  ab_string_free(context->engine, &identity);
  return status;
}

static ArchbirdStatus
annotate_node_connectivity(AbProjectionContext *context,
                           AbProjectionData *graph,
                           const AbProjectionItem *relation) {
  const AbValue *source = attribute(relation, "source");
  const AbValue *target = attribute(relation, "target");
  const AbValue *kind = attribute(relation, "relation_kind");
  const AbValue *family = attribute(relation, "family");
  const AbValue *ids[] = {source, target};
  const char *counts[] = {"uses_count", "used_by_count"};
  uint64_t witnesses = u64_attribute(relation, "witness_count");
  size_t index;
  if (!source || source->kind != AB_VALUE_STRING || !target ||
      target->kind != AB_VALUE_STRING || !kind ||
      kind->kind != AB_VALUE_STRING || !family ||
      family->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index < 2; index++) {
    const char *colon =
        memchr(ids[index]->as.text.data, ':', ids[index]->as.text.length);
    AbString entity_kind;
    AbString key = {0};
    AbProjectionItem *node = NULL;
    ArchbirdStatus status;
    if (!colon)
      return ARCHBIRD_INVALID_SCHEMA;
    entity_kind.data = ids[index]->as.text.data;
    entity_kind.length = (size_t)(colon - ids[index]->as.text.data);
    status = buffer_identity(context->engine, "node", &entity_kind,
                             &ids[index]->as.text, &key);
    if (status == ARCHBIRD_OK)
      status =
          ab_projection_data_find_item(context->engine, graph, &key, &node);
    if (status == ARCHBIRD_OK && !node)
      status = ARCHBIRD_INVALID_SCHEMA;
    if (status == ARCHBIRD_OK)
      status = set_u64_attribute(context->engine, node, counts[index],
                                 u64_attribute(node, counts[index]) + 1);
    if (status == ARCHBIRD_OK)
      status = set_u64_attribute(
          context->engine, node, "relation_witness_count",
          u64_attribute(node, "relation_witness_count") + witnesses);
    ab_string_free(context->engine, &key);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static AbValue *mutable_attribute(AbProjectionItem *item, const char *name) {
  size_t index;
  for (index = 0; index < item->attribute_count; index++)
    if (string_is(&item->attributes[index].name, name))
      return &item->attributes[index].value;
  return NULL;
}

static ArchbirdStatus allocate_string_array_attribute(ArchbirdEngine *engine,
                                                      AbProjectionItem *item,
                                                      const char *name,
                                                      size_t count) {
  AbValue empty = {.kind = AB_VALUE_ARRAY};
  AbValue *array;
  ArchbirdStatus status = add_attribute(engine, item, name, &empty);
  if (status != ARCHBIRD_OK)
    return status;
  array = mutable_attribute(item, name);
  if (!array || array->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!count)
    return ARCHBIRD_OK;
  array->as.array.items =
      (AbValue *)ab_calloc(engine, count, sizeof(*array->as.array.items));
  if (!array->as.array.items)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->as.array.count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus group_relation_item(AbProjectionContext *context,
                                          AbProjectionData *graph,
                                          const AbProjectionItem *canonical,
                                          const AbString *source_group,
                                          const AbString *target_group,
                                          AbProjectionItem **out) {
  const AbValue *family = attribute(canonical, "family");
  const AbValue *kind = attribute(canonical, "relation_kind");
  AbString item_kind = {(char *)"group_relation", 14};
  AbString identity = {0};
  AbString key = {0};
  ArchbirdStatus status;
  if (!family || family->kind != AB_VALUE_STRING || !kind ||
      kind->kind != AB_VALUE_STRING)
    return ARCHBIRD_INVALID_SCHEMA;
  status = relation_key(context->engine, family->as.text.data, &family->as.text,
                        source_group, target_group, &identity);
  if (status == ARCHBIRD_OK)
    status = buffer_identity(context->engine, "group_relation", &item_kind,
                             &identity, &key);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_find_item(context->engine, graph, &key, out);
  if (status == ARCHBIRD_OK && !*out)
    status = ARCHBIRD_INVALID_SCHEMA;
  ab_string_free(context->engine, &key);
  ab_string_free(context->engine, &identity);
  return status;
}

static ArchbirdStatus fill_group_relation_arrays(
    AbProjectionContext *context, AbProjectionData *graph,
    const AbProjectionItem *canonical, const AbString *source_group,
    const AbString *target_group, size_t *key_cursors, size_t *name_cursors,
    size_t *kind_cursors) {
  AbProjectionItem *group_relation = NULL;
  const AbValue *names = attribute(canonical, "names");
  const AbValue *kind = attribute(canonical, "relation_kind");
  AbValue *keys;
  AbValue *group_names;
  AbValue *group_kinds;
  size_t item_index;
  size_t name_index;
  ArchbirdStatus status = group_relation_item(
      context, graph, canonical, source_group, target_group, &group_relation);
  if (status != ARCHBIRD_OK)
    return status;
  item_index = (size_t)(group_relation - graph->items);
  keys = mutable_attribute(group_relation, "canonical_relation_keys");
  group_names = mutable_attribute(group_relation, "names");
  group_kinds = mutable_attribute(group_relation, "relation_kinds");
  if (!keys || keys->kind != AB_VALUE_ARRAY || !group_names ||
      group_names->kind != AB_VALUE_ARRAY || !group_kinds ||
      group_kinds->kind != AB_VALUE_ARRAY || !kind ||
      kind->kind != AB_VALUE_STRING ||
      key_cursors[item_index] >= keys->as.array.count)
    return ARCHBIRD_INVALID_SCHEMA;
  keys->as.array.items[key_cursors[item_index]].kind = AB_VALUE_STRING;
  status = ab_string_copy(
      context->engine, &keys->as.array.items[key_cursors[item_index]].as.text,
      canonical->key.data, canonical->key.length);
  if (status == ARCHBIRD_OK)
    key_cursors[item_index]++;
  if (status == ARCHBIRD_OK &&
      kind_cursors[item_index] >= group_kinds->as.array.count)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK) {
    group_kinds->as.array.items[kind_cursors[item_index]].kind =
        AB_VALUE_STRING;
    status = ab_string_copy(
        context->engine,
        &group_kinds->as.array.items[kind_cursors[item_index]].as.text,
        kind->as.text.data, kind->as.text.length);
    if (status == ARCHBIRD_OK)
      kind_cursors[item_index]++;
  }
  for (name_index = 0;
       status == ARCHBIRD_OK && names && name_index < names->as.array.count;
       name_index++) {
    const AbValue *name = &names->as.array.items[name_index];
    if (name->kind != AB_VALUE_STRING ||
        name_cursors[item_index] >= group_names->as.array.count)
      return ARCHBIRD_INVALID_SCHEMA;
    group_names->as.array.items[name_cursors[item_index]].kind =
        AB_VALUE_STRING;
    status = ab_string_copy(
        context->engine,
        &group_names->as.array.items[name_cursors[item_index]].as.text,
        name->as.text.data, name->as.text.length);
    if (status == ARCHBIRD_OK)
      name_cursors[item_index]++;
  }
  return status;
}

static int string_value_compare(const void *left, const void *right) {
  const AbValue *a = (const AbValue *)left;
  const AbValue *b = (const AbValue *)right;
  return ab_string_compare(&a->as.text, &b->as.text);
}

static ArchbirdStatus normalize_group_relation_arrays(
    ArchbirdEngine *engine, AbProjectionData *graph, const size_t *key_cursors,
    const size_t *name_cursors, const size_t *kind_cursors) {
  size_t index;
  for (index = 0; index < graph->item_count; index++) {
    AbProjectionItem *item = &graph->items[index];
    AbValue *keys;
    AbValue *names;
    AbValue *kinds;
    size_t read;
    size_t write = 0;
    if (!ab_projection_value_is(attribute(item, "record_kind"),
                                "group_relation"))
      continue;
    keys = mutable_attribute(item, "canonical_relation_keys");
    names = mutable_attribute(item, "names");
    kinds = mutable_attribute(item, "relation_kinds");
    if (!keys || keys->kind != AB_VALUE_ARRAY || !names ||
        names->kind != AB_VALUE_ARRAY || !kinds ||
        kinds->kind != AB_VALUE_ARRAY ||
        key_cursors[index] != keys->as.array.count ||
        name_cursors[index] != names->as.array.count ||
        kind_cursors[index] != kinds->as.array.count)
      return ARCHBIRD_INVALID_SCHEMA;
    if (keys->as.array.count > 1)
      qsort(keys->as.array.items, keys->as.array.count,
            sizeof(*keys->as.array.items), string_value_compare);
    if (names->as.array.count > 1)
      qsort(names->as.array.items, names->as.array.count,
            sizeof(*names->as.array.items), string_value_compare);
    if (kinds->as.array.count > 1)
      qsort(kinds->as.array.items, kinds->as.array.count,
            sizeof(*kinds->as.array.items), string_value_compare);
    for (read = 0; read < names->as.array.count; read++) {
      if (write && ab_string_equal(&names->as.array.items[write - 1].as.text,
                                   &names->as.array.items[read].as.text)) {
        ab_value_free(engine, &names->as.array.items[read]);
        continue;
      }
      if (write != read) {
        names->as.array.items[write] = names->as.array.items[read];
        memset(&names->as.array.items[read], 0,
               sizeof(names->as.array.items[read]));
      }
      write++;
    }
    names->as.array.count = write;
    write = 0;
    for (read = 0; read < kinds->as.array.count; read++) {
      if (write && ab_string_equal(&kinds->as.array.items[write - 1].as.text,
                                   &kinds->as.array.items[read].as.text)) {
        ab_value_free(engine, &kinds->as.array.items[read]);
        continue;
      }
      if (write != read) {
        kinds->as.array.items[write] = kinds->as.array.items[read];
        memset(&kinds->as.array.items[read], 0,
               sizeof(kinds->as.array.items[read]));
      }
      write++;
    }
    kinds->as.array.count = write;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_group_relations(AbProjectionContext *context,
                                          AbProjectionData *graph) {
  AbGraphMembershipRef *memberships = NULL;
  size_t membership_count = 0;
  size_t relation_count = 0;
  size_t member_index = 0;
  size_t initial_count = graph->item_count;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; index < initial_count; index++) {
    const AbValue *record = attribute(&graph->items[index], "record_kind");
    membership_count += ab_projection_value_is(record, "membership");
    relation_count += ab_projection_value_is(record, "relation");
  }
  if (membership_count)
    memberships = (AbGraphMembershipRef *)ab_calloc(
        context->engine, membership_count, sizeof(*memberships));
  if (membership_count && !memberships)
    return ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; index < initial_count; index++) {
    const AbProjectionItem *item = &graph->items[index];
    if (ab_projection_value_is(attribute(item, "record_kind"), "membership")) {
      const AbValue *group = attribute(item, "group");
      const AbValue *node = attribute(item, "node");
      if (!group || group->kind != AB_VALUE_STRING || !node ||
          node->kind != AB_VALUE_STRING) {
        status = ARCHBIRD_INVALID_SCHEMA;
        break;
      }
      memberships[member_index].group = &group->as.text;
      memberships[member_index].node = &node->as.text;
      member_index++;
    }
  }
  if (status == ARCHBIRD_OK && membership_count)
    qsort(memberships, membership_count, sizeof(*memberships),
          membership_ref_compare);
  for (index = 0;
       status == ARCHBIRD_OK && index < initial_count && relation_count;
       index++) {
    const AbProjectionItem *relation = &graph->items[index];
    const AbValue *source;
    const AbValue *target;
    size_t source_start;
    size_t source_end;
    size_t target_start;
    size_t target_end;
    size_t source_index;
    size_t target_index;
    if (!ab_projection_value_is(attribute(relation, "record_kind"), "relation"))
      continue;
    source = attribute(relation, "source");
    target = attribute(relation, "target");
    if (!source || source->kind != AB_VALUE_STRING || !target ||
        target->kind != AB_VALUE_STRING) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    status = annotate_node_connectivity(context, graph, relation);
    if (status != ARCHBIRD_OK)
      break;
    if (!membership_count)
      continue;
    source_start = membership_ref_lower_bound(memberships, membership_count,
                                              &source->as.text);
    source_end = source_start;
    while (source_end < membership_count &&
           ab_string_equal(memberships[source_end].node, &source->as.text))
      source_end++;
    target_start = membership_ref_lower_bound(memberships, membership_count,
                                              &target->as.text);
    target_end = target_start;
    while (target_end < membership_count &&
           ab_string_equal(memberships[target_end].node, &target->as.text))
      target_end++;
    if (source_start == source_end || target_start == target_end) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    for (source_index = source_start;
         status == ARCHBIRD_OK && source_index < source_end; source_index++)
      for (target_index = target_start;
           status == ARCHBIRD_OK && target_index < target_end; target_index++)
        if (!ab_string_equal(memberships[source_index].group,
                             memberships[target_index].group))
          status = add_group_relation(context, graph, index,
                                      memberships[source_index].group,
                                      memberships[target_index].group);
  }
  if (status == ARCHBIRD_OK && membership_count) {
    size_t *key_cursors = (size_t *)ab_calloc(
        context->engine, graph->item_count, sizeof(*key_cursors));
    size_t *name_cursors = (size_t *)ab_calloc(
        context->engine, graph->item_count, sizeof(*name_cursors));
    size_t *kind_cursors = (size_t *)ab_calloc(
        context->engine, graph->item_count, sizeof(*kind_cursors));
    if (!key_cursors || !name_cursors || !kind_cursors)
      status = ARCHBIRD_OUT_OF_MEMORY;
    for (index = 0; status == ARCHBIRD_OK && index < graph->item_count;
         index++) {
      AbProjectionItem *item = &graph->items[index];
      uint64_t keys;
      uint64_t names;
      uint64_t kinds;
      if (!ab_projection_value_is(attribute(item, "record_kind"),
                                  "group_relation"))
        continue;
      keys = u64_attribute(item, "relation_count");
      names = u64_attribute(item, "name_occurrence_count");
      kinds = u64_attribute(item, "relation_kind_occurrence_count");
      if (keys > SIZE_MAX || names > SIZE_MAX || kinds > SIZE_MAX)
        status = ARCHBIRD_LIMIT_EXCEEDED;
      if (status == ARCHBIRD_OK)
        status = allocate_string_array_attribute(
            context->engine, item, "canonical_relation_keys", (size_t)keys);
      if (status == ARCHBIRD_OK)
        status = allocate_string_array_attribute(context->engine, item, "names",
                                                 (size_t)names);
      if (status == ARCHBIRD_OK)
        status = allocate_string_array_attribute(
            context->engine, item, "relation_kinds", (size_t)kinds);
    }
    for (index = 0;
         status == ARCHBIRD_OK && index < initial_count && relation_count;
         index++) {
      const AbProjectionItem *relation = &graph->items[index];
      const AbValue *source;
      const AbValue *target;
      size_t source_start;
      size_t source_end;
      size_t target_start;
      size_t target_end;
      size_t source_index;
      size_t target_index;
      if (!ab_projection_value_is(attribute(relation, "record_kind"),
                                  "relation"))
        continue;
      source = attribute(relation, "source");
      target = attribute(relation, "target");
      if (!source || source->kind != AB_VALUE_STRING || !target ||
          target->kind != AB_VALUE_STRING) {
        status = ARCHBIRD_INVALID_SCHEMA;
        break;
      }
      source_start = membership_ref_lower_bound(memberships, membership_count,
                                                &source->as.text);
      source_end = source_start;
      while (source_end < membership_count &&
             ab_string_equal(memberships[source_end].node, &source->as.text))
        source_end++;
      target_start = membership_ref_lower_bound(memberships, membership_count,
                                                &target->as.text);
      target_end = target_start;
      while (target_end < membership_count &&
             ab_string_equal(memberships[target_end].node, &target->as.text))
        target_end++;
      if (source_start == source_end || target_start == target_end) {
        status = ARCHBIRD_INVALID_SCHEMA;
        break;
      }
      for (source_index = source_start;
           status == ARCHBIRD_OK && source_index < source_end; source_index++)
        for (target_index = target_start;
             status == ARCHBIRD_OK && target_index < target_end; target_index++)
          if (!ab_string_equal(memberships[source_index].group,
                               memberships[target_index].group))
            status = fill_group_relation_arrays(
                context, graph, relation, memberships[source_index].group,
                memberships[target_index].group, key_cursors, name_cursors,
                kind_cursors);
    }
    if (status == ARCHBIRD_OK)
      status = normalize_group_relation_arrays(
          context->engine, graph, key_cursors, name_cursors, kind_cursors);
    ab_free(context->engine, kind_cursors);
    ab_free(context->engine, name_cursors);
    ab_free(context->engine, key_cursors);
  }
  ab_free(context->engine, memberships);
  return status;
}

static const char *file_edge_family(const AbString *kind) {
  if (string_is(kind, "import"))
    return "imports";
  if (string_is(kind, "declaration"))
    return "declarations";
  if (string_is(kind, "external") || string_starts(kind, "package"))
    return "packages";
  if (string_starts(kind, "bridge:"))
    return "bridges";
  return NULL;
}

static ArchbirdStatus
add_path_relation(AbProjectionContext *context, const AbProjectionPlan *plan,
                  const AbGraphInputs *inputs, AbProjectionData *graph,
                  const char *family, const AbString *kind,
                  const AbString *source_path, const AbString *target_path,
                  const AbProjectionItem *witness, uint64_t *collapsed) {
  AbGraphEndpoints sources = {0};
  AbGraphEndpoints targets = {0};
  size_t source_index;
  size_t target_index;
  ArchbirdStatus status =
      endpoints_for_path(context, plan, inputs, source_path, &sources);
  if (status == ARCHBIRD_OK)
    status = endpoints_for_path(context, plan, inputs, target_path, &targets);
  for (source_index = 0; status == ARCHBIRD_OK && source_index < sources.count;
       source_index++) {
    status = ensure_endpoint_node(context, graph, &sources.values[source_index],
                                  source_path, witness);
    for (target_index = 0;
         status == ARCHBIRD_OK && target_index < targets.count;
         target_index++) {
      status = ensure_endpoint_node(
          context, graph, &targets.values[target_index], target_path, witness);
      if (status == ARCHBIRD_OK)
        status = add_relation(
            context, graph, family, kind, &sources.values[source_index],
            &targets.values[target_index], witness, collapsed);
    }
  }
  endpoints_free(context->engine, &targets);
  endpoints_free(context->engine, &sources);
  return status;
}

static ArchbirdStatus add_file_relations(AbProjectionContext *context,
                                         const AbProjectionPlan *plan,
                                         const AbGraphInputs *inputs,
                                         AbProjectionData *graph,
                                         uint64_t collapsed[8]) {
  size_t index;
  for (index = 0;
       inputs->has_file_edges && index < inputs->file_edges.item_count;
       index++) {
    const AbProjectionItem *row = &inputs->file_edges.items[index];
    const AbValue *kind = attribute(row, "kind");
    const AbValue *source = attribute(row, "source");
    const AbValue *target = attribute(row, "target");
    const char *family;
    size_t family_index;
    if (!kind || !source || !target || kind->kind != AB_VALUE_STRING ||
        source->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    family = file_edge_family(&kind->as.text);
    if (!family || !request_has(plan, "relations", family))
      continue;
    family_index = !strcmp(family, "imports")
                       ? 0
                       : (!strcmp(family, "declarations")
                              ? 1
                              : (!strcmp(family, "packages") ? 2 : 3));
    {
      ArchbirdStatus status = add_path_relation(
          context, plan, inputs, graph, family, &kind->as.text,
          &source->as.text, &target->as.text, row, &collapsed[family_index]);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_symbol_relations(AbProjectionContext *context,
                                           const AbProjectionPlan *plan,
                                           const AbGraphInputs *inputs,
                                           AbProjectionData *graph,
                                           uint64_t collapsed[8]) {
  const AbValue *level = ab_value_member(&plan->definition, "level");
  size_t index;
  for (index = 0; inputs->has_symbol_relations &&
                  index < inputs->symbol_relations.item_count;
       index++) {
    const AbProjectionItem *row = &inputs->symbol_relations.items[index];
    const AbValue *kind = attribute(row, "kind");
    const AbValue *source = attribute(row, "source");
    const AbValue *target = attribute(row, "target");
    const AbValue *source_path = attribute(row, "source_path");
    const AbValue *target_path = attribute(row, "target_path");
    const char *family;
    if (!kind || !source || !target || kind->kind != AB_VALUE_STRING ||
        source->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING ||
        !source_path || source_path->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    family = string_is(&kind->as.text, "call") ? "calls" : "references";
    if (!request_has(plan, "relations", family))
      continue;
    if (ab_projection_value_is(level, "symbol")) {
      AbString source_id = {0};
      AbString target_id = {0};
      const char *target_kind =
          target_path && target_path->kind == AB_VALUE_STRING
              ? "symbol"
              : peripheral_kind(&target->as.text);
      ArchbirdStatus status =
          node_id(context->engine, "symbol", &source->as.text, &source_id);
      if (status == ARCHBIRD_OK)
        status =
            node_id(context->engine, target_kind, &target->as.text, &target_id);
      if (status == ARCHBIRD_OK)
        status = ensure_endpoint_node(context, graph, &source_id,
                                      &source->as.text, row);
      if (status == ARCHBIRD_OK)
        status = group_node(context, plan, inputs, graph, NULL, &source_id,
                            &source_path->as.text);
      if (status == ARCHBIRD_OK)
        status = ensure_endpoint_node(context, graph, &target_id,
                                      &target->as.text, row);
      if (status == ARCHBIRD_OK && target_path &&
          target_path->kind == AB_VALUE_STRING)
        status = group_node(context, plan, inputs, graph, NULL, &target_id,
                            &target_path->as.text);
      if (status == ARCHBIRD_OK)
        status = add_relation(context, graph, family, &kind->as.text,
                              &source_id, &target_id, row,
                              &collapsed[!strcmp(family, "calls") ? 4 : 5]);
      ab_string_free(context->engine, &target_id);
      ab_string_free(context->engine, &source_id);
      if (status != ARCHBIRD_OK)
        return status;
    } else if (target_path && target_path->kind == AB_VALUE_STRING) {
      ArchbirdStatus status = add_path_relation(
          context, plan, inputs, graph, family, &kind->as.text,
          &source_path->as.text, &target_path->as.text, row,
          &collapsed[!strcmp(family, "calls") ? 4 : 5]);
      if (status != ARCHBIRD_OK)
        return status;
    } else {
      AbGraphEndpoints sources = {0};
      AbString target_id = {0};
      size_t source_index;
      ArchbirdStatus status = endpoints_for_path(
          context, plan, inputs, &source_path->as.text, &sources);
      if (status == ARCHBIRD_OK)
        status = node_id(context->engine, peripheral_kind(&target->as.text),
                         &target->as.text, &target_id);
      if (status == ARCHBIRD_OK)
        status = ensure_endpoint_node(context, graph, &target_id,
                                      &target->as.text, row);
      for (source_index = 0;
           status == ARCHBIRD_OK && source_index < sources.count;
           source_index++) {
        status =
            ensure_endpoint_node(context, graph, &sources.values[source_index],
                                 &source_path->as.text, row);
        if (status == ARCHBIRD_OK)
          status = add_relation(context, graph, family, &kind->as.text,
                                &sources.values[source_index], &target_id, row,
                                &collapsed[!strcmp(family, "calls") ? 4 : 5]);
      }
      ab_string_free(context->engine, &target_id);
      endpoints_free(context->engine, &sources);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_test_relations(AbProjectionContext *context,
                                         const AbProjectionPlan *plan,
                                         const AbGraphInputs *inputs,
                                         AbProjectionData *graph,
                                         uint64_t collapsed[8]) {
  size_t index;
  for (index = 0;
       inputs->has_test_routes && index < inputs->test_routes.item_count;
       index++) {
    const AbProjectionItem *row = &inputs->test_routes.items[index];
    const AbValue *kind = attribute(row, "kind");
    const AbValue *source = attribute(row, "source");
    const AbValue *source_path = attribute(row, "source_path");
    const AbValue *target = attribute(row, "target");
    if (!kind || !source || !source_path || !target ||
        kind->kind != AB_VALUE_STRING || source->kind != AB_VALUE_STRING ||
        source_path->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    {
      ArchbirdStatus status = add_path_relation(
          context, plan, inputs, graph, "tests", &kind->as.text,
          &source_path->as.text, &target->as.text, row, &collapsed[6]);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_build_relations(AbProjectionContext *context,
                                          const AbProjectionPlan *plan,
                                          const AbGraphInputs *inputs,
                                          AbProjectionData *graph,
                                          uint64_t collapsed[8]) {
  size_t index;
  for (index = 0;
       inputs->has_build_routes && index < inputs->build_routes.item_count;
       index++) {
    const AbProjectionItem *row = &inputs->build_routes.items[index];
    const AbValue *kind = attribute(row, "kind");
    const AbValue *source = attribute(row, "source");
    const AbValue *target = attribute(row, "target");
    AbString source_id = {0};
    AbGraphEndpoints targets = {0};
    size_t target_index;
    ArchbirdStatus status;
    if (!kind || !source || !target || kind->kind != AB_VALUE_STRING ||
        source->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    status = node_id(context->engine, "build", &source->as.text, &source_id);
    if (status == ARCHBIRD_OK)
      status = ensure_endpoint_node(context, graph, &source_id,
                                    &source->as.text, row);
    if (status == ARCHBIRD_OK)
      status =
          endpoints_for_path(context, plan, inputs, &target->as.text, &targets);
    for (target_index = 0;
         status == ARCHBIRD_OK && target_index < targets.count;
         target_index++) {
      status = ensure_endpoint_node(
          context, graph, &targets.values[target_index], &target->as.text, row);
      if (status == ARCHBIRD_OK)
        status =
            add_relation(context, graph, "builds", &kind->as.text, &source_id,
                         &targets.values[target_index], row, &collapsed[7]);
    }
    endpoints_free(context->engine, &targets);
    ab_string_free(context->engine, &source_id);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_package_entrypoints(AbProjectionContext *context,
                                              const AbProjectionPlan *plan,
                                              const AbGraphInputs *inputs,
                                              AbProjectionData *graph,
                                              uint64_t collapsed[8]) {
  size_t index;
  for (index = 0; inputs->has_package_entrypoints &&
                  index < inputs->package_entrypoints.item_count;
       index++) {
    const AbProjectionItem *row = &inputs->package_entrypoints.items[index];
    const AbValue *kind = attribute(row, "kind");
    const AbValue *source = attribute(row, "source");
    const AbValue *target = attribute(row, "target");
    AbString source_id = {0};
    AbGraphEndpoints targets = {0};
    size_t target_index;
    ArchbirdStatus status;
    if (!kind || !source || !target || kind->kind != AB_VALUE_STRING ||
        source->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING)
      return ARCHBIRD_INVALID_SCHEMA;
    status = node_id(context->engine, "package", &source->as.text, &source_id);
    if (status == ARCHBIRD_OK)
      status = ensure_endpoint_node(context, graph, &source_id,
                                    &source->as.text, row);
    if (status == ARCHBIRD_OK)
      status =
          endpoints_for_path(context, plan, inputs, &target->as.text, &targets);
    for (target_index = 0;
         status == ARCHBIRD_OK && target_index < targets.count;
         target_index++) {
      status = ensure_endpoint_node(
          context, graph, &targets.values[target_index], &target->as.text, row);
      if (status == ARCHBIRD_OK)
        status =
            add_relation(context, graph, "packages", &kind->as.text, &source_id,
                         &targets.values[target_index], row, &collapsed[2]);
    }
    endpoints_free(context->engine, &targets);
    ab_string_free(context->engine, &source_id);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_diagnostics(AbProjectionContext *context,
                                      AbProjectionData *graph) {
  const AbValue *rows = ab_value_member(context->map, "diagnostics");
  AbString kind = {(char *)"diagnostic", 10};
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "project Map has no diagnostic inventory");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *code = ab_value_member(row, "code");
    const AbValue *message = ab_value_member(row, "message");
    const AbValue *path = ab_value_member(row, "path");
    const AbValue *severity = ab_value_member(row, "severity");
    AbBuffer identity;
    AbString key;
    AbProjectionItem *item = NULL;
    ArchbirdStatus status;
    if (!row || row->kind != AB_VALUE_OBJECT || !code ||
        code->kind != AB_VALUE_STRING || !message ||
        message->kind != AB_VALUE_STRING || !path ||
        path->kind != AB_VALUE_STRING || !severity ||
        severity->kind != AB_VALUE_STRING)
      return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "Map contains an invalid diagnostic row");
    ab_buffer_init(&identity, context->engine);
    status = ab_buffer_u64(&identity, (uint64_t)index);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&identity, ":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&identity, code->as.text.data, code->as.text.length);
    key.data = (char *)identity.data;
    key.length = identity.length;
    if (status == ARCHBIRD_OK)
      status = graph_item(context, graph, "diagnostic", &kind, &key,
                          &message->as.text, NULL, &item);
    if (status == ARCHBIRD_OK)
      status = add_attribute(context->engine, item, "code", code);
    if (status == ARCHBIRD_OK)
      status = add_attribute(context->engine, item, "message", message);
    if (status == ARCHBIRD_OK)
      status = add_attribute(context->engine, item, "path", path);
    if (status == ARCHBIRD_OK)
      status = add_attribute(context->engine, item, "severity", severity);
    ab_buffer_free(&identity);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_discovery_coverage(AbProjectionContext *context,
                                             AbProjectionData *graph) {
  static const char *const counts[] = {
      "assets",
      "ignored",
      "inventory_files",
      "oversized",
      "pruned_directories",
      "selected",
      "unsupported_known",
  };
  AbString kind = {(char *)"discovery", 9};
  AbString identity = {(char *)"repository", 10};
  AbString label = {(char *)"Repository discovery", 20};
  const AbValue *discovery = ab_value_member(context->map, "discovery");
  const AbValue *coverage =
      discovery ? ab_value_member(discovery, "coverage") : NULL;
  const AbValue *profile =
      discovery ? ab_value_member(discovery, "profile") : NULL;
  const AbValue *sha = discovery ? ab_value_member(discovery, "sha256") : NULL;
  AbProjectionItem *item = NULL;
  uint64_t oversized = 0;
  uint64_t unsupported = 0;
  size_t index;
  ArchbirdStatus status = graph_item(context, graph, "coverage", &kind,
                                     &identity, &label, NULL, &item);
  if (status != ARCHBIRD_OK)
    return status;
  status = add_literal_attribute(context->engine, item, "completeness_scope",
                                 "contextual");
  if (status != ARCHBIRD_OK)
    return status;
  if (!coverage || coverage->kind != AB_VALUE_OBJECT || !profile ||
      profile->kind != AB_VALUE_OBJECT || !sha ||
      sha->kind != AB_VALUE_STRING) {
    return ab_projection_item_set_state(
        context->engine, item, "unknown",
        "project Map has no complete discovery coverage");
  }
  for (index = 0; index < sizeof(counts) / sizeof(counts[0]); index++) {
    const AbValue *value = ab_value_member(coverage, counts[index]);
    uint64_t number = 0;
    if (!value || !ab_value_u64(value, &number))
      return archbird_error_set(
          context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "Map contains an invalid discovery coverage record");
    status = add_attribute(context->engine, item, counts[index], value);
    if (status != ARCHBIRD_OK)
      return status;
    if (!strcmp(counts[index], "oversized"))
      oversized = number;
    else if (!strcmp(counts[index], "unsupported_known"))
      unsupported = number;
  }
  status = add_attribute(context->engine, item, "profile", profile);
  if (status == ARCHBIRD_OK)
    status = add_attribute(context->engine, item, "sha256", sha);
  if (status == ARCHBIRD_OK && (oversized || unsupported))
    status = ab_projection_item_set_state(
        context->engine, item, "unknown",
        "repository discovery contains unsupported or oversized inputs");
  return status;
}

static ArchbirdStatus annotate_evidence_quality(AbProjectionContext *context,
                                                AbProjectionData *graph) {
  size_t index;
  for (index = 0; index < graph->item_count; index++) {
    AbProjectionItem *item = &graph->items[index];
    const AbValue *record = attribute(item, "record_kind");
    const char *classification;
    ArchbirdStatus status;
    if (!record || (!ab_projection_value_is(record, "node") &&
                    !ab_projection_value_is(record, "relation")))
      continue;
    classification =
        string_is(&item->state, "current")
            ? "direct"
            : (string_is(&item->state, "unknown") ? "unknown" : "stale");
    status = add_literal_attribute(context->engine, item, "evidence_class",
                                   classification);
    if (status == ARCHBIRD_OK)
      status = add_u64_attribute(context->engine, item, "evidence_count",
                                 (uint64_t)item->evidence_count);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_ledgers(AbProjectionContext *context,
                                  const AbProjectionPlan *plan,
                                  const AbGraphInputs *inputs,
                                  AbProjectionData *graph,
                                  const uint64_t collapsed[8]) {
  static const char *const families[] = {
      "imports", "declarations", "packages", "bridges",
      "calls",   "references",   "tests",    "builds",
  };
  AbString ledger_kind = {(char *)"relation", 8};
  size_t index;
  for (index = 0; index < sizeof(families) / sizeof(families[0]); index++) {
    AbString family = {(char *)families[index], strlen(families[index])};
    AbProjectionItem *item = NULL;
    const AbProjectionData *source = NULL;
    uint64_t unknown = 0;
    size_t source_index;
    ArchbirdStatus status;
    if (!request_has(plan, "relations", families[index]))
      continue;
    status = graph_item(context, graph, "ledger", &ledger_kind, &family,
                        &family, NULL, &item);
    if (status == ARCHBIRD_OK)
      status = add_string_attribute(context->engine, item, "family", &family);
    if (status == ARCHBIRD_OK)
      status = add_u64_attribute(context->engine, item, "collapsed",
                                 collapsed[index]);
    if (index < 4)
      source = &inputs->file_edges;
    else if (index < 6)
      source = &inputs->symbol_relations;
    else if (index == 6)
      source = &inputs->test_routes;
    else
      source = &inputs->build_routes;
    if (source && source->selection.has_unsupported)
      unknown = source->selection.unsupported;
    if (source && string_is(&source->state, "unknown") && !unknown)
      unknown = 1;
    for (source_index = 0; source && source_index < source->item_count;
         source_index++) {
      const AbProjectionItem *row = &source->items[source_index];
      const AbValue *kind = attribute(row, "kind");
      const char *row_family =
          source == &inputs->file_edges && kind && kind->kind == AB_VALUE_STRING
              ? file_edge_family(&kind->as.text)
              : (source == &inputs->symbol_relations && kind &&
                         kind->kind == AB_VALUE_STRING
                     ? (string_is(&kind->as.text, "call") ? "calls"
                                                          : "references")
                     : NULL);
      if ((source != &inputs->file_edges ||
           (row_family && !strcmp(row_family, families[index]))) &&
          (source != &inputs->symbol_relations ||
           (row_family && !strcmp(row_family, families[index]))) &&
          string_is(&row->state, "unknown"))
        unknown++;
    }
    if (index == 2)
      for (source_index = 0;
           source_index < inputs->package_entrypoints.item_count;
           source_index++)
        if (string_is(&inputs->package_entrypoints.items[source_index].state,
                      "unknown"))
          unknown++;
    if (status == ARCHBIRD_OK)
      status = add_u64_attribute(context->engine, item, "unknown", unknown);
    if (status == ARCHBIRD_OK && unknown)
      status = ab_projection_item_set_state(
          context->engine, item, "unknown",
          "relation family contains incomplete evidence");
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static uint64_t unknown_structural_records(const AbProjectionData *graph) {
  uint64_t unsupported = 0;
  size_t index;
  for (index = 0; index < graph->item_count; index++) {
    const AbValue *record = attribute(&graph->items[index], "record_kind");
    if (!ab_projection_value_is(record, "coverage") &&
        string_is(&graph->items[index].state, "unknown")) {
      if (unsupported == UINT64_MAX)
        break;
      unsupported++;
    }
  }
  return unsupported;
}

static uint64_t structural_record_count(const AbProjectionData *graph) {
  uint64_t count = 0;
  size_t index;
  for (index = 0; index < graph->item_count; index++)
    if (!ab_projection_value_is(
            attribute(&graph->items[index], "completeness_scope"),
            "contextual")) {
      if (count == UINT64_MAX)
        break;
      count++;
    }
  return count;
}

static int inputs_truncated(const AbGraphInputs *inputs) {
  const AbProjectionData *facts[] = {
      &inputs->membership,   &inputs->components,          &inputs->symbols,
      &inputs->file_edges,   &inputs->symbol_relations,    &inputs->test_routes,
      &inputs->build_routes, &inputs->package_entrypoints,
  };
  size_t index;
  for (index = 0; index < sizeof(facts) / sizeof(facts[0]); index++)
    if (facts[index]->selection.has_truncated &&
        facts[index]->selection.truncated)
      return 1;
  return 0;
}

ArchbirdStatus ab_projection_extract_graph(AbProjectionContext *context,
                                           const AbProjectionPlan *plan,
                                           AbProjectionData *out) {
  const AbValue *project = ab_value_member(context->map, "project");
  AbGraphInputs inputs = {0};
  uint64_t collapsed[8] = {0};
  uint64_t unsupported;
  const AbProjectionData *unavailable;
  ArchbirdStatus status;
  status = inputs_load(context, plan, &inputs);
  unavailable =
      status == ARCHBIRD_OK ? unavailable_node_inventory(plan, &inputs) : NULL;
  if (status == ARCHBIRD_OK && unavailable) {
    status = ab_projection_data_unknown(
        context->engine, out, &plan->id, &project->as.text, "graph",
        unavailable->message.length
            ? unavailable->message.data
            : "project Map has no complete graph node inventory");
    inputs_free(context->engine, &inputs);
    return status;
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_init(context->engine, out, &plan->id, "graph",
                                     "derived", &project->as.text);
  if (status == ARCHBIRD_OK)
    status = add_graph_nodes(context, plan, &inputs, out);
  if (status == ARCHBIRD_OK)
    status = add_file_relations(context, plan, &inputs, out, collapsed);
  if (status == ARCHBIRD_OK)
    status = add_symbol_relations(context, plan, &inputs, out, collapsed);
  if (status == ARCHBIRD_OK)
    status = add_test_relations(context, plan, &inputs, out, collapsed);
  if (status == ARCHBIRD_OK)
    status = add_build_relations(context, plan, &inputs, out, collapsed);
  if (status == ARCHBIRD_OK)
    status = add_package_entrypoints(context, plan, &inputs, out, collapsed);
  if (status == ARCHBIRD_OK)
    status = add_peripheral_memberships(context, plan, out);
  if (status == ARCHBIRD_OK)
    status = add_group_relations(context, out);
  if (status == ARCHBIRD_OK)
    status = add_discovery_coverage(context, out);
  if (status == ARCHBIRD_OK && request_has(plan, "overlays", "diagnostics"))
    status = add_diagnostics(context, out);
  if (status == ARCHBIRD_OK &&
      request_has(plan, "overlays", "evidence-quality"))
    status = annotate_evidence_quality(context, out);
  if (status == ARCHBIRD_OK)
    status = add_ledgers(context, plan, &inputs, out, collapsed);
  unsupported = unknown_structural_records(out);
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_completeness_exact(
        context->engine, out, "graph_record", structural_record_count(out),
        structural_record_count(out), 0, unsupported,
        inputs_truncated(&inputs));
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(context->engine, out);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(context->engine, out);
  inputs_free(context->engine, &inputs);
  return status;
}
