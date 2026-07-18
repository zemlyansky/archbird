#include "syntax/tree_sitter/python/scanner.h"

#include "syntax/tree_sitter/scanner.h"

#include <string.h>

const TSLanguage *tree_sitter_python(void);

static int node_is_name(const AbTreeSitterScan *scan, TSNode node) {
  return ab_tree_sitter_node_has_text(scan, node) &&
         (ab_tree_sitter_node_type(node, "identifier") ||
          ab_tree_sitter_node_type(node, "dotted_name"));
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan,
                                 const AbTreeSitterFrame *frame, TSNode owner,
                                 const char *kind, TSNode *out_name,
                                 AbFact **out_fact) {
  TSNode name = ab_tree_sitter_child(owner, "name");
  AbFact *fact;
  ArchbirdStatus status;
  static const char *const containers[] = {"class_definition",
                                           "function_definition"};
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (!node_is_name(scan, name))
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

static ArchbirdStatus add_call(AbTreeSitterScan *scan,
                               const AbTreeSitterFrame *frame) {
  TSNode function = ab_tree_sitter_child(frame->node, "function");
  TSNode name = function;
  const char *domain = "calls";
  const char *kind = "free-call";
  AbFact *fact;
  ArchbirdStatus status;
  if (ab_tree_sitter_node_type(function, "attribute")) {
    name = ab_tree_sitter_child(function, "attribute");
    domain = "method-calls";
    kind = "method-call";
  }
  if (!ab_tree_sitter_node_type(name, "identifier") ||
      !ab_tree_sitter_node_has_text(scan, name))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_node_fact(scan, domain, kind, name, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  return status;
}

static ArchbirdStatus add_module_variable(AbTreeSitterScan *scan,
                                          TSNode assignment) {
  TSNode left = ab_tree_sitter_child(assignment, "left");
  TSNode right = ab_tree_sitter_child(assignment, "right");
  AbFact *fact;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_type(left, "identifier") ||
      !ab_tree_sitter_node_has_text(scan, left) ||
      ab_tree_sitter_node_type(right, "identifier"))
    return ARCHBIRD_OK;
  status =
      ab_tree_sitter_add_node_fact(scan, "symbols", "variable", left, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, left);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "scope",
                                          (const uint8_t *)"module", 6);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "signature",
                                          (const uint8_t *)"", 0);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                     (const uint8_t *)ts_node_type(assignment),
                                     strlen(ts_node_type(assignment)));
  return status;
}

static ArchbirdStatus add_import_module(AbTreeSitterScan *scan, TSNode node) {
  AbFact *fact;
  ArchbirdStatus status;
  if (!node_is_name(scan, node))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_node_fact(scan, "imports", "module", node, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, node);
  return status;
}

static ArchbirdStatus add_import(AbTreeSitterScan *scan, TSNode node) {
  uint32_t index;
  uint32_t count;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(node, "import_from_statement"))
    return add_import_module(scan, ab_tree_sitter_child(node, "module_name"));
  count = ts_node_named_child_count(node);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    TSNode child = ts_node_named_child(node, index);
    if (ab_tree_sitter_node_type(child, "aliased_import"))
      child = ab_tree_sitter_child(child, "name");
    status = add_import_module(scan, child);
  }
  return status;
}

static ArchbirdStatus visit_python(AbTreeSitterScan *scan,
                                   const AbTreeSitterFrame *frame,
                                   AbTreeSitterFrame *child_frame) {
  TSNode name = (TSNode){0};
  AbFact *fact = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(frame->node, "class_definition")) {
    status = add_symbol(scan, frame, frame->node, "class", &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_FUNCTION;
    child_frame->context |= AB_TS_CONTEXT_CLASS;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "function_definition")) {
    const char *kind = (frame->context & AB_TS_CONTEXT_CLASS) &&
                               !(frame->context & AB_TS_CONTEXT_FUNCTION)
                           ? "method"
                           : "function";
    status = add_symbol(scan, frame, frame->node, kind, &name, &fact);
    child_frame->context &= ~AB_TS_CONTEXT_CLASS;
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact)
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
  } else if (ab_tree_sitter_node_type(frame->node, "assignment") &&
             !(frame->context &
               (AB_TS_CONTEXT_CLASS | AB_TS_CONTEXT_FUNCTION))) {
    status = add_module_variable(scan, frame->node);
  } else if (ab_tree_sitter_node_type(frame->node, "call")) {
    status = add_call(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "import_statement") ||
             ab_tree_sitter_node_type(frame->node, "import_from_statement")) {
    status = add_import(scan, frame->node);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_python_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct identifier call nodes; no dynamic target resolution"},
      {"imports", "statically named import and from-import module nodes"},
      {"method-calls",
       "attribute call member names; receiver identity remains unresolved"},
      {"symbols",
       "named class/function definitions and simple module assignments with "
       "lexical enclosing context"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-python",
      "2",
      "archbird-tree-sitter-python-v2;runtime=0.26.9;grammar=0.25.0;abi=15",
      "tree-sitter-0.26.9;tree-sitter-python-0.25.0;grammar-abi-15",
      "python",
      tree_sitter_python,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_python};
  return ab_tree_sitter_scan_file(engine, manifest, file, source, source_length,
                                  source_manifest_sha256, implementation_sha256,
                                  &descriptor, out_bundle);
}
