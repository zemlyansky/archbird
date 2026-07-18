#include "syntax/tree_sitter/ecmascript/scanner.h"

#include "render_internal.h"
#include "syntax/tree_sitter/ecmascript/module.h"

#include <string.h>

static int identifier_node(const AbTreeSitterScan *scan, TSNode node) {
  return ab_tree_sitter_node_has_text(scan, node) &&
         (ab_tree_sitter_node_type(node, "identifier") ||
          ab_tree_sitter_node_type(node, "type_identifier") ||
          ab_tree_sitter_node_type(node, "property_identifier") ||
          ab_tree_sitter_node_type(node, "private_property_identifier"));
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan,
                                 const AbTreeSitterFrame *frame, TSNode owner,
                                 TSNode name, const char *kind,
                                 TSNode *out_name, AbFact **out_fact);
static ArchbirdStatus append_static_reference(AbTreeSitterScan *scan,
                                              TSNode node, AbBuffer *out,
                                              int *supported);
static ArchbirdStatus add_explicit_symbol(AbTreeSitterScan *scan,
                                          const AbTreeSitterFrame *frame,
                                          TSNode owner, TSNode anchor,
                                          const AbBuffer *name,
                                          const char *kind, AbFact **out_fact);

static int function_value(TSNode node) {
  return ab_tree_sitter_node_type(node, "arrow_function") ||
         ab_tree_sitter_node_type(node, "function_expression") ||
         ab_tree_sitter_node_type(node, "generator_function");
}

typedef struct EcmaName {
  const uint8_t *data;
  size_t length;
} EcmaName;

typedef struct EcmaDescriptorHelper {
  EcmaName name;
  EcmaName object_parameter;
  EcmaName property_parameter;
  EcmaName value_parameter;
} EcmaDescriptorHelper;

static int identifier_name(const AbTreeSitterScan *scan, TSNode node,
                           EcmaName *out) {
  size_t start;
  size_t end;
  memset(out, 0, sizeof(*out));
  if (!identifier_node(scan, node) ||
      !ab_tree_sitter_node_slice(scan, node, &start, &end) || start == end)
    return 0;
  out->data = scan->source + start;
  out->length = end - start;
  return 1;
}

static int name_equal(const EcmaName *left, const EcmaName *right) {
  return left->length == right->length &&
         (!left->length || memcmp(left->data, right->data, left->length) == 0);
}

static int name_literal(const EcmaName *name, const char *literal) {
  size_t length = strlen(literal);
  return name->length == length &&
         (!length || memcmp(name->data, literal, length) == 0);
}

static int identifier_equal(const AbTreeSitterScan *scan, TSNode node,
                            const EcmaName *expected) {
  EcmaName actual;
  return identifier_name(scan, node, &actual) && name_equal(&actual, expected);
}

static int direct_member_call(const AbTreeSitterScan *scan, TSNode call,
                              const char *object, const char *property) {
  TSNode function = ab_tree_sitter_child(call, "function");
  TSNode base;
  TSNode member;
  EcmaName base_name;
  EcmaName member_name;
  if (!ab_tree_sitter_node_type(function, "member_expression"))
    return 0;
  base = ab_tree_sitter_child(function, "object");
  member = ab_tree_sitter_child(function, "property");
  return identifier_name(scan, base, &base_name) &&
         identifier_name(scan, member, &member_name) &&
         name_literal(&base_name, object) &&
         name_literal(&member_name, property);
}

static TSNode call_argument(TSNode call, uint32_t index) {
  TSNode arguments = ab_tree_sitter_child(call, "arguments");
  return index < ts_node_named_child_count(arguments)
             ? ts_node_named_child(arguments, index)
             : (TSNode){0};
}

static int static_string(const AbTreeSitterScan *scan, TSNode node,
                         EcmaName *out) {
  size_t start;
  size_t end;
  memset(out, 0, sizeof(*out));
  if (!ab_tree_sitter_node_type(node, "string") ||
      !ab_tree_sitter_node_slice(scan, node, &start, &end) ||
      end <= start + 2 ||
      !((scan->source[start] == '\'' && scan->source[end - 1] == '\'') ||
        (scan->source[start] == '"' && scan->source[end - 1] == '"')) ||
      memchr(scan->source + start + 1, '\\', end - start - 2))
    return 0;
  out->data = scan->source + start + 1;
  out->length = end - start - 2;
  return out->length != 0;
}

static int descriptor_getter_uses_parameter(const AbTreeSitterScan *scan,
                                            TSNode descriptor,
                                            const EcmaName *value_parameter) {
  uint32_t count;
  uint32_t index;
  if (!ab_tree_sitter_node_type(descriptor, "object"))
    return 0;
  count = ts_node_named_child_count(descriptor);
  for (index = 0; index < count; index++) {
    TSNode pair = ts_node_named_child(descriptor, index);
    TSNode key;
    TSNode value;
    EcmaName key_name;
    if (!ab_tree_sitter_node_type(pair, "pair"))
      continue;
    key = ab_tree_sitter_child(pair, "key");
    value = ab_tree_sitter_child(pair, "value");
    if (identifier_name(scan, key, &key_name) &&
        name_literal(&key_name, "get") &&
        identifier_equal(scan, value, value_parameter))
      return 1;
  }
  return 0;
}

static int node_declares_name(const AbTreeSitterScan *scan, TSNode node,
                              const EcmaName *name) {
  const char *type = ts_node_type(node);
  uint32_t count;
  uint32_t index;
  if ((strcmp(type, "function_declaration") == 0 ||
       strcmp(type, "generator_function_declaration") == 0 ||
       strcmp(type, "class_declaration") == 0 ||
       strcmp(type, "variable_declarator") == 0) &&
      identifier_equal(scan, ab_tree_sitter_child(node, "name"), name))
    return 1;
  if (strcmp(type, "assignment_expression") == 0 &&
      identifier_equal(scan, ab_tree_sitter_child(node, "left"), name))
    return 1;
  if (strcmp(type, "function_declaration") == 0 ||
      strcmp(type, "generator_function_declaration") == 0 ||
      strcmp(type, "function_expression") == 0 ||
      strcmp(type, "generator_function") == 0 ||
      strcmp(type, "arrow_function") == 0)
    return 0;
  count = ts_node_named_child_count(node);
  for (index = 0; index < count; index++)
    if (node_declares_name(scan, ts_node_named_child(node, index), name))
      return 1;
  return 0;
}

static int descriptor_call_statement(const AbTreeSitterScan *scan,
                                     TSNode statement,
                                     const EcmaDescriptorHelper *helper) {
  TSNode node;
  const char *type;
  if (!ab_tree_sitter_node_type(statement, "expression_statement") ||
      ts_node_named_child_count(statement) != 1)
    return 0;
  node = ts_node_named_child(statement, 0);
  type = ts_node_type(node);
  if (strcmp(type, "call_expression") != 0)
    return 0;
  return direct_member_call(scan, node, "Object", "defineProperty") &&
         ts_node_named_child_count(ab_tree_sitter_child(node, "arguments")) ==
             3 &&
         identifier_equal(scan, call_argument(node, 0),
                          &helper->object_parameter) &&
         identifier_equal(scan, call_argument(node, 1),
                          &helper->property_parameter) &&
         descriptor_getter_uses_parameter(scan, call_argument(node, 2),
                                          &helper->value_parameter);
}

static int descriptor_call_in_body(const AbTreeSitterScan *scan, TSNode body,
                                   const EcmaDescriptorHelper *helper) {
  uint32_t count;
  uint32_t index;
  if (!ab_tree_sitter_node_type(body, "statement_block"))
    return 0;
  count = ts_node_named_child_count(body);
  for (index = 0; index < count; index++)
    if (descriptor_call_statement(scan, ts_node_named_child(body, index),
                                  helper))
      return 1;
  return 0;
}

static int descriptor_helper(const AbTreeSitterScan *scan, TSNode declaration,
                             EcmaDescriptorHelper *out) {
  TSNode parameters;
  TSNode body;
  uint32_t count;
  memset(out, 0, sizeof(*out));
  if (!ab_tree_sitter_node_type(declaration, "function_declaration") ||
      !identifier_name(scan, ab_tree_sitter_child(declaration, "name"),
                       &out->name))
    return 0;
  parameters = ab_tree_sitter_child(declaration, "parameters");
  count = ts_node_named_child_count(parameters);
  if (count < 3 ||
      !identifier_name(scan, ts_node_named_child(parameters, 0),
                       &out->object_parameter) ||
      !identifier_name(scan, ts_node_named_child(parameters, 1),
                       &out->property_parameter) ||
      !identifier_name(scan, ts_node_named_child(parameters, 2),
                       &out->value_parameter) ||
      name_literal(&out->object_parameter, "Object") ||
      name_literal(&out->property_parameter, "Object") ||
      name_literal(&out->value_parameter, "Object"))
    return 0;
  for (uint32_t index = 0; index < count; index++) {
    EcmaName parameter;
    if (!identifier_name(scan, ts_node_named_child(parameters, index),
                         &parameter) ||
        name_literal(&parameter, "Object"))
      return 0;
  }
  body = ab_tree_sitter_child(declaration, "body");
  if (node_declares_name(scan, body,
                         &(EcmaName){(const uint8_t *)"Object", 6}) ||
      node_declares_name(scan, body, &out->object_parameter) ||
      node_declares_name(scan, body, &out->property_parameter) ||
      node_declares_name(scan, body, &out->value_parameter))
    return 0;
  return descriptor_call_in_body(scan, body, out);
}

static int top_level_name_is_unique(const AbTreeSitterScan *scan,
                                    TSNode program, TSNode declaration,
                                    const EcmaName *name) {
  uint32_t count = ts_node_named_child_count(program);
  uint32_t index;
  size_t declarations = 0;
  for (index = 0; index < count; index++) {
    TSNode statement = ts_node_named_child(program, index);
    if (ts_node_eq(statement, declaration)) {
      declarations++;
      continue;
    }
    if (node_declares_name(scan, statement, name))
      return 0;
  }
  return declarations == 1;
}

static ArchbirdStatus
emit_descriptor_helper_call(AbTreeSitterScan *scan, TSNode call,
                            const EcmaDescriptorHelper *helper) {
  TSNode object = call_argument(call, 0);
  TSNode key_node = call_argument(call, 1);
  TSNode value = call_argument(call, 2);
  EcmaName key;
  AbTreeSitterFrame frame;
  AbBuffer name;
  AbFact *fact = NULL;
  int supported = 1;
  ArchbirdStatus status;
  if (!identifier_equal(scan, ab_tree_sitter_child(call, "function"),
                        &helper->name) ||
      ts_node_named_child_count(ab_tree_sitter_child(call, "arguments")) < 3 ||
      !static_string(scan, key_node, &key) ||
      !(function_value(value) || identifier_node(scan, value)))
    return ARCHBIRD_OK;
  memset(&frame, 0, sizeof(frame));
  frame.node = call;
  ab_buffer_init(&name, scan->engine);
  status = append_static_reference(scan, object, &name, &supported);
  if (status == ARCHBIRD_OK && supported)
    status = ab_buffer_literal(&name, ".");
  if (status == ARCHBIRD_OK && supported)
    status = ab_buffer_append(&name, key.data, key.length);
  if (status == ARCHBIRD_OK && supported)
    status = add_explicit_symbol(scan, &frame, call, key_node, &name, "method",
                                 &fact);
  if (status == ARCHBIRD_OK && fact)
    status =
        ab_fact_add_string_attribute(scan->engine, fact, "registration_kind",
                                     (const uint8_t *)"descriptor-helper", 17);
  if (status == ARCHBIRD_OK && fact)
    status =
        ab_fact_add_string_attribute(scan->engine, fact, "registration_helper",
                                     helper->name.data, helper->name.length);
  ab_buffer_free(&name);
  return status;
}

static ArchbirdStatus add_descriptor_helper_symbols(AbTreeSitterScan *scan,
                                                    TSNode program) {
  uint32_t count = ts_node_named_child_count(program);
  uint32_t helper_index;
  uint32_t call_index;
  for (helper_index = 0; helper_index < count; helper_index++) {
    TSNode declaration = ts_node_named_child(program, helper_index);
    EcmaDescriptorHelper helper;
    if (!descriptor_helper(scan, declaration, &helper) ||
        !top_level_name_is_unique(scan, program, declaration, &helper.name))
      continue;
    for (call_index = 0; call_index < count; call_index++) {
      TSNode statement = ts_node_named_child(program, call_index);
      TSNode expression;
      ArchbirdStatus status;
      if (!ab_tree_sitter_node_type(statement, "expression_statement") ||
          ts_node_named_child_count(statement) != 1)
        continue;
      expression = ts_node_named_child(statement, 0);
      if (!ab_tree_sitter_node_type(expression, "call_expression"))
        continue;
      status = emit_descriptor_helper_call(scan, expression, &helper);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_node_text(AbTreeSitterScan *scan, TSNode node,
                                       int unquote, AbBuffer *out,
                                       int *supported) {
  size_t start;
  size_t end;
  if (!ab_tree_sitter_node_has_text(scan, node) ||
      !ab_tree_sitter_node_slice(scan, node, &start, &end)) {
    *supported = 0;
    return ARCHBIRD_OK;
  }
  if (unquote && end - start >= 2 &&
      ((scan->source[start] == '\'' && scan->source[end - 1] == '\'') ||
       (scan->source[start] == '"' && scan->source[end - 1] == '"'))) {
    start++;
    end--;
  }
  if (start == end) {
    *supported = 0;
    return ARCHBIRD_OK;
  }
  return ab_buffer_append(out, scan->source + start, end - start);
}

/* Build only statically named references. Dynamic computed properties and
   call-result receivers are deliberately omitted rather than guessed. */
static ArchbirdStatus append_static_reference(AbTreeSitterScan *scan,
                                              TSNode node, AbBuffer *out,
                                              int *supported) {
  ArchbirdStatus status;
  if (identifier_node(scan, node) || ab_tree_sitter_node_type(node, "this") ||
      ab_tree_sitter_node_type(node, "super"))
    return append_node_text(scan, node, 0, out, supported);
  if (ab_tree_sitter_node_type(node, "member_expression")) {
    TSNode object = ab_tree_sitter_child(node, "object");
    TSNode property = ab_tree_sitter_child(node, "property");
    status = append_static_reference(scan, object, out, supported);
    if (status != ARCHBIRD_OK || !*supported)
      return status;
    status = ab_buffer_literal(out, ".");
    if (status != ARCHBIRD_OK)
      return status;
    return append_node_text(scan, property, 0, out, supported);
  }
  if (ab_tree_sitter_node_type(node, "subscript_expression")) {
    TSNode object = ab_tree_sitter_child(node, "object");
    TSNode index = ab_tree_sitter_child(node, "index");
    int unquote = ab_tree_sitter_node_type(index, "string");
    if (!unquote && !ab_tree_sitter_node_type(index, "number")) {
      *supported = 0;
      return ARCHBIRD_OK;
    }
    status = append_static_reference(scan, object, out, supported);
    if (status != ARCHBIRD_OK || !*supported)
      return status;
    status = ab_buffer_literal(out, ".");
    if (status != ARCHBIRD_OK)
      return status;
    return append_node_text(scan, index, unquote, out, supported);
  }
  *supported = 0;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_explicit_symbol(AbTreeSitterScan *scan,
                                          const AbTreeSitterFrame *frame,
                                          TSNode owner, TSNode anchor,
                                          const AbBuffer *name,
                                          const char *kind, AbFact **out_fact) {
  size_t start;
  size_t end;
  AbFact *fact;
  ArchbirdStatus status;
  *out_fact = NULL;
  if (!name->length || !ab_tree_sitter_node_slice(scan, anchor, &start, &end) ||
      start == end)
    return ARCHBIRD_OK;
  status = ab_bundle_builder_add_fact(
      scan->builder, "symbols", kind, "syntax-structure", start, end,
      name->data, name->length, name->data, name->length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_track_fact_region(scan, fact, owner);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, anchor);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK && frame->has_enclosing)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  if (status == ARCHBIRD_OK)
    *out_fact = fact;
  return status;
}

static ArchbirdStatus add_assignment_function(AbTreeSitterScan *scan,
                                              const AbTreeSitterFrame *frame,
                                              TSNode *out_name,
                                              AbFact **out_fact) {
  TSNode value = ab_tree_sitter_child(frame->node, "right");
  TSNode left = ab_tree_sitter_child(frame->node, "left");
  TSNode anchor = left;
  AbBuffer name;
  int supported = 1;
  ArchbirdStatus status;
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (!function_value(value))
    return ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(left, "member_expression"))
    anchor = ab_tree_sitter_child(left, "property");
  else if (ab_tree_sitter_node_type(left, "subscript_expression"))
    anchor = ab_tree_sitter_child(left, "index");
  ab_buffer_init(&name, scan->engine);
  status = append_static_reference(scan, left, &name, &supported);
  if (status == ARCHBIRD_OK && supported)
    status = add_explicit_symbol(
        scan, frame, frame->node, anchor, &name,
        ab_tree_sitter_node_type(left, "identifier") ? "function" : "method",
        out_fact);
  if (status == ARCHBIRD_OK && *out_fact)
    *out_name = anchor;
  ab_buffer_free(&name);
  return status;
}

static ArchbirdStatus add_pair_function(AbTreeSitterScan *scan,
                                        const AbTreeSitterFrame *frame,
                                        TSNode *out_name, AbFact **out_fact) {
  TSNode value = ab_tree_sitter_child(frame->node, "value");
  TSNode key = ab_tree_sitter_child(frame->node, "key");
  if (!function_value(value)) {
    *out_name = (TSNode){0};
    *out_fact = NULL;
    return ARCHBIRD_OK;
  }
  return add_symbol(scan, frame, frame->node, key, "method", out_name,
                    out_fact);
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan,
                                 const AbTreeSitterFrame *frame, TSNode owner,
                                 TSNode name, const char *kind,
                                 TSNode *out_name, AbFact **out_fact) {
  AbFact *fact;
  ArchbirdStatus status;
  int named_class =
      ab_tree_sitter_node_type(owner, "class_declaration") ||
      ab_tree_sitter_node_type(owner, "abstract_class_declaration") ||
      ab_tree_sitter_node_type(owner, "class") ||
      ab_tree_sitter_node_type(owner, "class_expression");
  int frame_qualifies = frame->has_enclosing && !named_class;
  static const char *const containers[] = {"class_declaration",
                                           "abstract_class_declaration",
                                           "class",
                                           "class_expression",
                                           "function_declaration",
                                           "function_expression",
                                           "generator_function_declaration",
                                           "generator_function",
                                           "method_definition",
                                           "pair",
                                           "variable_declarator"};
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (!identifier_node(scan, name))
    return ARCHBIRD_OK;
  if (frame_qualifies) {
    size_t start;
    size_t end;
    AbBuffer qualified;
    ab_buffer_init(&qualified, scan->engine);
    status =
        ab_buffer_append(&qualified, frame->enclosing, frame->enclosing_length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&qualified, ".");
    if (status == ARCHBIRD_OK &&
        ab_tree_sitter_node_slice(scan, name, &start, &end) && start < end)
      status = ab_buffer_append(&qualified, scan->source + start, end - start);
    else if (status == ARCHBIRD_OK)
      status = ARCHBIRD_INVALID_SCHEMA;
    if (status == ARCHBIRD_OK)
      status = add_explicit_symbol(scan, frame, owner, name, &qualified, kind,
                                   &fact);
    ab_buffer_free(&qualified);
  } else {
    status = ab_tree_sitter_add_qualified_fact(
        scan, "symbols", kind, owner, name, containers,
        named_class ? 0 : sizeof(containers) / sizeof(containers[0]), &fact);
  }
  if (status == ARCHBIRD_OK)
    status = frame_qualifies ? ARCHBIRD_OK
                             : ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK && !frame_qualifies)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK) {
    *out_name = name;
    *out_fact = fact;
  }
  return status;
}

static ArchbirdStatus add_variable_function(AbTreeSitterScan *scan,
                                            const AbTreeSitterFrame *frame,
                                            TSNode *out_name,
                                            AbFact **out_fact) {
  TSNode value = ab_tree_sitter_child(frame->node, "value");
  TSNode name = ab_tree_sitter_child(frame->node, "name");
  if (!function_value(value)) {
    *out_name = (TSNode){0};
    *out_fact = NULL;
    return ARCHBIRD_OK;
  }
  return add_symbol(scan, frame, frame->node, name, "function", out_name,
                    out_fact);
}

static ArchbirdStatus add_call(AbTreeSitterScan *scan,
                               const AbTreeSitterFrame *frame) {
  TSNode function = ab_tree_sitter_child(frame->node, "function");
  TSNode name = function;
  const char *domain = "calls";
  const char *kind = "call";
  AbFact *fact;
  ArchbirdStatus status;
  if (ab_tree_sitter_node_type(function, "member_expression") ||
      ab_tree_sitter_node_type(function, "subscript_expression")) {
    name = ab_tree_sitter_child(function, "property");
    domain = "method-calls";
    kind = "call";
  }
  if (!identifier_node(scan, name))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_node_fact(scan, domain, kind, name, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  return status;
}

static ArchbirdStatus add_import(AbTreeSitterScan *scan, TSNode node) {
  TSNode source = ab_tree_sitter_child(node, "source");
  AbFact *fact;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_type(source, "string"))
    return ARCHBIRD_OK;
  {
    size_t start;
    size_t end;
    if (!ab_tree_sitter_node_slice(scan, source, &start, &end) ||
        end <= start + 2)
      return ARCHBIRD_OK;
  }
  status =
      ab_tree_sitter_add_quoted_fact(scan, "imports", "import", source, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, source);
  return status;
}

ArchbirdStatus ab_tree_sitter_visit_ecmascript(AbTreeSitterScan *scan,
                                               const AbTreeSitterFrame *frame,
                                               AbTreeSitterFrame *child_frame) {
  TSNode name = (TSNode){0};
  AbFact *fact = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(frame->node, "program")) {
    status = add_descriptor_helper_symbols(scan, frame->node);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_extract_ecmascript_module(scan, frame->node);
  } else if (ab_tree_sitter_node_type(frame->node, "class_declaration") ||
             ab_tree_sitter_node_type(frame->node,
                                      "abstract_class_declaration")) {
    status = add_symbol(scan, frame, frame->node,
                        ab_tree_sitter_child(frame->node, "name"), "class",
                        &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_FUNCTION;
    child_frame->context |= AB_TS_CONTEXT_CLASS;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "class") ||
             ab_tree_sitter_node_type(frame->node, "class_expression")) {
    status = add_symbol(scan, frame, frame->node,
                        ab_tree_sitter_child(frame->node, "name"), "class",
                        &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_FUNCTION;
    child_frame->context |= AB_TS_CONTEXT_CLASS;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "function_declaration") ||
             ab_tree_sitter_node_type(frame->node,
                                      "generator_function_declaration")) {
    status = add_symbol(scan, frame, frame->node,
                        ab_tree_sitter_child(frame->node, "name"), "function",
                        &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_CLASS;
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "method_definition")) {
    status = add_symbol(scan, frame, frame->node,
                        ab_tree_sitter_child(frame->node, "name"), "method",
                        &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_CLASS;
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "variable_declarator")) {
    status = add_variable_function(scan, frame, &name, &fact);
    if (status == ARCHBIRD_OK && fact) {
      child_frame->context &= ~AB_TS_CONTEXT_CLASS;
      child_frame->context |= AB_TS_CONTEXT_FUNCTION;
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
    }
  } else if (ab_tree_sitter_node_type(frame->node, "assignment_expression")) {
    status = add_assignment_function(scan, frame, &name, &fact);
    if (status == ARCHBIRD_OK && fact) {
      child_frame->context &= ~AB_TS_CONTEXT_CLASS;
      child_frame->context |= AB_TS_CONTEXT_FUNCTION;
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
    }
  } else if (ab_tree_sitter_node_type(frame->node, "pair")) {
    status = add_pair_function(scan, frame, &name, &fact);
    if (status == ARCHBIRD_OK && fact) {
      child_frame->context &= ~AB_TS_CONTEXT_CLASS;
      child_frame->context |= AB_TS_CONTEXT_FUNCTION;
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
    }
  } else if (ab_tree_sitter_node_type(frame->node, "call_expression")) {
    status = add_call(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "import_statement")) {
    status = add_import(scan, frame->node);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_ecmascript_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32],
    const AbTreeSitterDescriptor *descriptor, AbProviderBundle *out_bundle) {
  return ab_tree_sitter_scan_file(engine, manifest, file, source, source_length,
                                  source_manifest_sha256, implementation_sha256,
                                  descriptor, out_bundle);
}
