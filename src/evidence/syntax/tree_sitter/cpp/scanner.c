#include "syntax/tree_sitter/cpp/scanner.h"

#include "syntax/tree_sitter/scanner.h"

#include <string.h>

const TSLanguage *tree_sitter_cpp(void);

static int name_node(const AbTreeSitterScan *scan, TSNode node) {
  return ab_tree_sitter_node_has_text(scan, node) &&
         (ab_tree_sitter_node_type(node, "identifier") ||
          ab_tree_sitter_node_type(node, "field_identifier") ||
          ab_tree_sitter_node_type(node, "type_identifier") ||
          ab_tree_sitter_node_type(node, "namespace_identifier") ||
          ab_tree_sitter_node_type(node, "destructor_name") ||
          ab_tree_sitter_node_type(node, "operator_name"));
}

static TSNode leaf_name(const AbTreeSitterScan *scan, TSNode node) {
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
    TSNode child;
    if (name_node(scan, node))
      return node;
    child = ab_tree_sitter_child(node, "name");
    if (ts_node_is_null(child))
      child = ab_tree_sitter_child(node, "declarator");
    if (ts_node_is_null(child))
      break;
    node = child;
  }
  return (TSNode){0};
}

static TSNode function_name(const AbTreeSitterScan *scan, TSNode node) {
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
    TSNode child;
    if (ab_tree_sitter_node_type(node, "function_declarator"))
      return leaf_name(scan, ab_tree_sitter_child(node, "declarator"));
    child = ab_tree_sitter_child(node, "declarator");
    if (ts_node_is_null(child))
      break;
    node = child;
  }
  return (TSNode){0};
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan,
                                 const AbTreeSitterFrame *frame, TSNode owner,
                                 TSNode name, const char *kind,
                                 TSNode *out_name, AbFact **out_fact) {
  AbFact *fact;
  ArchbirdStatus status;
  static const char *const containers[] = {
      "class_specifier", "struct_specifier", "union_specifier",
      "namespace_definition", "function_definition"};
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (ts_node_is_null(name))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_qualified_fact(
      scan, "symbols", kind, owner, name, containers,
      sizeof(containers) / sizeof(containers[0]), &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK && frame->has_enclosing)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  if (status == ARCHBIRD_OK) {
    *out_name = name;
    *out_fact = fact;
  }
  return status;
}

static ArchbirdStatus add_function(AbTreeSitterScan *scan,
                                   const AbTreeSitterFrame *frame,
                                   TSNode *out_name, AbFact **out_fact) {
  TSNode name =
      function_name(scan, ab_tree_sitter_child(frame->node, "declarator"));
  const char *kind =
      frame->context & AB_TS_CONTEXT_CLASS ? "method" : "function";
  return add_symbol(scan, frame, frame->node, name, kind, out_name, out_fact);
}

static ArchbirdStatus add_declaration(AbTreeSitterScan *scan,
                                      const AbTreeSitterFrame *frame) {
  TSNode name =
      function_name(scan, ab_tree_sitter_child(frame->node, "declarator"));
  TSNode ignored;
  AbFact *ignored_fact;
  if (ts_node_is_null(name))
    return ARCHBIRD_OK;
  return add_symbol(scan, frame, frame->node, name, "declaration", &ignored,
                    &ignored_fact);
}

static ArchbirdStatus add_call(AbTreeSitterScan *scan,
                               const AbTreeSitterFrame *frame) {
  TSNode function = ab_tree_sitter_child(frame->node, "function");
  TSNode name = function;
  const char *domain = "calls";
  const char *kind = "call";
  AbFact *fact;
  ArchbirdStatus status;
  if (ab_tree_sitter_node_type(function, "field_expression")) {
    name = ab_tree_sitter_child(function, "field");
    domain = "method-calls";
    kind = "method-call";
  } else if (ab_tree_sitter_node_type(function, "qualified_identifier")) {
    name = leaf_name(scan, function);
  }
  if (!name_node(scan, name))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_node_fact(scan, domain, kind, name, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  return status;
}

static ArchbirdStatus add_include(AbTreeSitterScan *scan, TSNode node) {
  TSNode path = ab_tree_sitter_child(node, "path");
  AbFact *fact;
  size_t start;
  size_t end;
  const char *delimiter = "unknown";
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_slice(scan, path, &start, &end) || end <= start)
    return ARCHBIRD_OK;
  if ((scan->source[start] == '"' && scan->source[end - 1] == '"') ||
      (scan->source[start] == '<' && scan->source[end - 1] == '>')) {
    delimiter = scan->source[start] == '<' ? "system" : "local";
    start++;
    end--;
  }
  if (start == end)
    return ARCHBIRD_OK;
  status = ab_bundle_builder_add_fact(scan->builder, "imports", "module",
                                      "syntax-structure", start, end,
                                      scan->source + start, end - start,
                                      scan->source + start, end - start, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_track_fact_region(scan, fact, node);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, path);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "delimiter",
                                          (const uint8_t *)delimiter,
                                          strlen(delimiter));
  return status;
}

static ArchbirdStatus visit_cpp(AbTreeSitterScan *scan,
                                const AbTreeSitterFrame *frame,
                                AbTreeSitterFrame *child_frame) {
  TSNode name = (TSNode){0};
  AbFact *fact = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(frame->node, "class_specifier") ||
      ab_tree_sitter_node_type(frame->node, "struct_specifier") ||
      ab_tree_sitter_node_type(frame->node, "union_specifier")) {
    status =
        add_symbol(scan, frame, frame->node,
                   leaf_name(scan, ab_tree_sitter_child(frame->node, "name")),
                   "class", &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_FUNCTION;
    child_frame->context |= AB_TS_CONTEXT_CLASS;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "namespace_definition")) {
    status =
        add_symbol(scan, frame, frame->node,
                   leaf_name(scan, ab_tree_sitter_child(frame->node, "name")),
                   "namespace", &name, &fact);
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "function_definition")) {
    status = add_function(scan, frame, &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_CLASS;
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "declaration") &&
             !(frame->context & AB_TS_CONTEXT_FUNCTION)) {
    status = add_declaration(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "call_expression")) {
    status = add_call(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "preproc_include")) {
    status = add_include(scan, frame->node);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_cpp_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct and qualified call names without overload resolution"},
      {"imports", "preprocessor include paths without include search"},
      {"method-calls", "field call names without receiver type resolution"},
      {"symbols",
       "functions, declarations, classes, structs, unions, and namespaces"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-cpp",
      "1",
      "archbird-tree-sitter-cpp-v1;runtime=0.26.9;grammar=0.23.4;abi=14",
      "tree-sitter-0.26.9;tree-sitter-cpp-0.23.4;grammar-abi-14",
      "cpp",
      tree_sitter_cpp,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_cpp,
      NULL,
      NULL};
  return ab_tree_sitter_scan_file(engine, manifest, file, source, source_length,
                                  source_manifest_sha256, implementation_sha256,
                                  &descriptor, out_bundle);
}
