#include "syntax/tree_sitter/c/scanner.h"

#include "syntax/tree_sitter/scanner.h"

#include <string.h>

const TSLanguage *tree_sitter_c(void);

static TSNode declarator_identifier(TSNode node) {
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
    TSNode child;
    if (ab_tree_sitter_node_type(node, "identifier"))
      return node;
    child = ab_tree_sitter_child(node, "declarator");
    if (ts_node_is_null(child))
      break;
    node = child;
  }
  return (TSNode){0};
}

static TSNode function_declarator(TSNode node) {
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
    TSNode child;
    if (ab_tree_sitter_node_type(node, "function_declarator"))
      return node;
    child = ab_tree_sitter_child(node, "declarator");
    if (ts_node_is_null(child))
      break;
    node = child;
  }
  return (TSNode){0};
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan, TSNode owner,
                                 const char *kind, TSNode *out_name,
                                 AbFact **out_fact) {
  TSNode function =
      function_declarator(ab_tree_sitter_child(owner, "declarator"));
  TSNode name;
  AbFact *fact;
  size_t start;
  size_t end;
  ArchbirdStatus status;
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (ts_node_is_null(function))
    return ARCHBIRD_OK;
  name = declarator_identifier(function);
  if (!ab_tree_sitter_node_slice(scan, name, &start, &end) || start == end)
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_named_fact(
      scan, "symbols", kind, name, scan->source + start, end - start, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK) {
    *out_name = name;
    *out_fact = fact;
  }
  return status;
}

static ArchbirdStatus add_call(AbTreeSitterScan *scan,
                               const AbTreeSitterFrame *frame) {
  TSNode function = ab_tree_sitter_child(frame->node, "function");
  AbFact *fact;
  size_t start;
  size_t end;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_type(function, "identifier") ||
      !ab_tree_sitter_node_slice(scan, function, &start, &end) || start == end)
    return ARCHBIRD_OK;
  status =
      ab_tree_sitter_add_named_fact(scan, "calls", "call", function,
                                    scan->source + start, end - start, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, function);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  return status;
}

static int node_is_field(TSNode parent, const char *field, TSNode node) {
  TSNode value = ab_tree_sitter_child(parent, field);
  return !ts_node_is_null(value) && ts_node_eq(value, node);
}

static ArchbirdStatus add_node_text_attribute(AbTreeSitterScan *scan,
                                              AbFact *fact, const char *name,
                                              TSNode node) {
  size_t start;
  size_t end;
  if (ts_node_is_null(node) ||
      !ab_tree_sitter_node_slice(scan, node, &start, &end) || start == end)
    return ARCHBIRD_OK;
  return ab_fact_add_string_attribute(scan->engine, fact, name,
                                      scan->source + start, end - start);
}

static TSNode initializer_container(TSNode node) {
  TSNode current = ts_node_parent(node);
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(current); depth++) {
    if (ab_tree_sitter_node_type(current, "init_declarator"))
      return declarator_identifier(ab_tree_sitter_child(current, "declarator"));
    if (ab_tree_sitter_node_type(current, "declaration") ||
        ab_tree_sitter_node_type(current, "function_definition") ||
        ab_tree_sitter_node_type(current, "compound_statement"))
      break;
    current = ts_node_parent(current);
  }
  return (TSNode){0};
}

static ArchbirdStatus add_value_reference(AbTreeSitterScan *scan,
                                          const AbTreeSitterFrame *frame) {
  TSNode node = frame->node;
  TSNode parent = ts_node_parent(node);
  TSNode container = (TSNode){0};
  const char *context = NULL;
  AbFact *fact;
  size_t start;
  size_t end;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_type(node, "identifier") || ts_node_is_null(parent))
    return ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(parent, "init_declarator") &&
      node_is_field(parent, "value", node)) {
    context = "initializer";
    container =
        declarator_identifier(ab_tree_sitter_child(parent, "declarator"));
  } else if (ab_tree_sitter_node_type(parent, "initializer_list")) {
    context = "initializer";
    container = initializer_container(node);
  } else if (ab_tree_sitter_node_type(parent, "initializer_pair") &&
             node_is_field(parent, "value", node)) {
    context = "initializer";
    container = initializer_container(node);
  } else if (ab_tree_sitter_node_type(parent, "assignment_expression") &&
             node_is_field(parent, "right", node)) {
    context = "assignment";
    container = ab_tree_sitter_child(parent, "left");
  } else if (ab_tree_sitter_node_type(parent, "argument_list")) {
    TSNode call = ts_node_parent(parent);
    if (ab_tree_sitter_node_type(call, "call_expression")) {
      context = "argument";
      container = ab_tree_sitter_child(call, "function");
    }
  } else if (ab_tree_sitter_node_type(parent, "return_statement") &&
             ts_node_named_child_count(parent) == 1 &&
             ts_node_eq(ts_node_named_child(parent, 0), node)) {
    context = "return";
  }
  if (!context || !ab_tree_sitter_node_slice(scan, node, &start, &end) ||
      start == end)
    return ARCHBIRD_OK;
  status =
      ab_tree_sitter_add_named_fact(scan, "symbol-references", "value", node,
                                    scan->source + start, end - start, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, node);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(scan->engine, fact, "context",
                                     (const uint8_t *)context, strlen(context));
  if (status == ARCHBIRD_OK)
    status = add_node_text_attribute(scan, fact, "container", container);
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

static ArchbirdStatus visit_c(AbTreeSitterScan *scan,
                              const AbTreeSitterFrame *frame,
                              AbTreeSitterFrame *child_frame) {
  TSNode name = (TSNode){0};
  AbFact *fact = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(frame->node, "function_definition")) {
    status = add_symbol(scan, frame->node, "function", &name, &fact);
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "declaration") &&
             !(frame->context & AB_TS_CONTEXT_FUNCTION)) {
    status = add_symbol(scan, frame->node, "declaration", &name, &fact);
  } else if (ab_tree_sitter_node_type(frame->node, "call_expression")) {
    status = add_call(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "preproc_include")) {
    status = add_include(scan, frame->node);
  } else if (ab_tree_sitter_node_type(frame->node, "identifier")) {
    status = add_value_reference(scan, frame);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_c_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls",
       "direct identifier call_expression nodes; no preprocessing, function-"
       "pointer resolution, or semantic target"},
      {"imports",
       "preproc_include path nodes; no include search or conditional-"
       "preprocessor evaluation"},
      {"symbol-references",
       "direct identifier values in initializers, assignments, call "
       "arguments, and returns; target identity remains unresolved syntax "
       "evidence"},
      {"symbols",
       "function definitions and top-level function declarations; recovered "
       "ranges reduce coverage to partial"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-c",
      "1",
      "archbird-tree-sitter-c-v1;runtime=0.26.9;grammar=0.24.2;abi=15",
      "tree-sitter-0.26.9;tree-sitter-c-0.24.2;grammar-abi-15",
      "c",
      tree_sitter_c,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_c};
  return ab_tree_sitter_scan_file(engine, manifest, file, source, source_length,
                                  source_manifest_sha256, implementation_sha256,
                                  &descriptor, out_bundle);
}
