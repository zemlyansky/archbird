#include "evidence/syntax/tree_sitter/r/scanner.h"

#include "evidence/syntax/tree_sitter/scanner.h"

#include <string.h>

const TSLanguage *tree_sitter_r(void);

static int operator_is(const AbTreeSitterScan *scan, TSNode node,
                       const char *literal) {
  size_t start;
  size_t end;
  size_t length = strlen(literal);
  return ab_tree_sitter_node_slice(scan, node, &start, &end) &&
         end - start == length &&
         memcmp(scan->source + start, literal, length) == 0;
}

static int node_is(const AbTreeSitterScan *scan, TSNode node,
                   const char *literal) {
  size_t start;
  size_t end;
  size_t length = strlen(literal);
  return ab_tree_sitter_node_slice(scan, node, &start, &end) &&
         end - start == length &&
         memcmp(scan->source + start, literal, length) == 0;
}

static void classify_r_metric(const AbTreeSitterScan *scan, TSNode node,
                              AbTreeSitterMetricNode *out) {
  const char *type = ts_node_type(node);
  if (!strcmp(type, "if_statement")) {
    out->construct = "if";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
    out->flattens_parent_nesting =
        ab_tree_sitter_node_is_field(node, "alternative");
  } else if (!strcmp(type, "for_statement")) {
    out->construct = "for";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "while_statement")) {
    out->construct = "while";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "repeat_statement")) {
    out->construct = "repeat";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "binary_operator")) {
    TSNode op = ab_tree_sitter_child(node, "operator");
    if (operator_is(scan, op, "&&") || operator_is(scan, op, "||") ||
        operator_is(scan, op, "&") || operator_is(scan, op, "|")) {
      out->construct = operator_is(scan, op, "&&")   ? "logical-and"
                       : operator_is(scan, op, "||") ? "logical-or"
                       : operator_is(scan, op, "&")  ? "vector-and"
                                                     : "vector-or";
      out->decision_contribution = 1;
    }
  } else if (!strcmp(type, "call")) {
    TSNode function = ab_tree_sitter_child(node, "function");
    if (ab_tree_sitter_node_type(function, "identifier") &&
        (node_is(scan, function, "switch") ||
         node_is(scan, function, "tryCatch") ||
         node_is(scan, function, "try") || node_is(scan, function, "ifelse"))) {
      out->construct = node_is(scan, function, "switch")     ? "switch"
                       : node_is(scan, function, "tryCatch") ? "try-catch"
                       : node_is(scan, function, "try")      ? "try"
                                                             : "ifelse";
      out->decision_contribution = 1;
    }
  }
}

static int r_callable_boundary(TSNode node) {
  return ab_tree_sitter_node_type(node, "function_definition");
}

static int assignment_function(AbTreeSitterScan *scan, TSNode node,
                               TSNode *out_name, TSNode *out_function) {
  TSNode lhs = ab_tree_sitter_child(node, "lhs");
  TSNode rhs = ab_tree_sitter_child(node, "rhs");
  TSNode op = ab_tree_sitter_child(node, "operator");
  if (ab_tree_sitter_node_type(lhs, "identifier") &&
      ab_tree_sitter_node_has_text(scan, lhs) &&
      ab_tree_sitter_node_type(rhs, "function_definition") &&
      (operator_is(scan, op, "<-") || operator_is(scan, op, "<<-") ||
       operator_is(scan, op, "=") || operator_is(scan, op, ":="))) {
    *out_name = lhs;
    *out_function = rhs;
    return 1;
  }
  if (ab_tree_sitter_node_type(lhs, "function_definition") &&
      ab_tree_sitter_node_type(rhs, "identifier") &&
      ab_tree_sitter_node_has_text(scan, rhs) &&
      (operator_is(scan, op, "->") || operator_is(scan, op, "->>"))) {
    *out_name = rhs;
    *out_function = lhs;
    return 1;
  }
  return 0;
}

static ArchbirdStatus add_function(AbTreeSitterScan *scan, TSNode owner,
                                   TSNode name, AbFact **out_fact) {
  AbFact *fact;
  ArchbirdStatus status;
  *out_fact = NULL;
  status =
      ab_tree_sitter_add_node_fact(scan, "symbols", "function", name, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_symbol_extent(scan, fact, owner);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK)
    *out_fact = fact;
  return status;
}

static ArchbirdStatus add_call(AbTreeSitterScan *scan,
                               const AbTreeSitterFrame *frame) {
  TSNode function = ab_tree_sitter_child(frame->node, "function");
  AbFact *fact;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_type(function, "identifier") ||
      !ab_tree_sitter_node_has_text(scan, function))
    return ARCHBIRD_OK;
  status = ab_tree_sitter_add_node_fact(scan, "calls", "call", function, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, function);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  return status;
}

static ArchbirdStatus visit_r(AbTreeSitterScan *scan,
                              const AbTreeSitterFrame *frame,
                              AbTreeSitterFrame *child_frame) {
  TSNode name = (TSNode){0};
  TSNode function = (TSNode){0};
  AbFact *fact = NULL;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (ab_tree_sitter_node_type(frame->node, "binary_operator") &&
      assignment_function(scan, frame->node, &name, &function)) {
    status = add_function(scan, frame->node, name, &fact);
    child_frame->context |= AB_TS_CONTEXT_FUNCTION;
    if (status == ARCHBIRD_OK && fact) {
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
      status = ab_tree_sitter_start_source_metrics(scan, fact, function,
                                                   child_frame);
    }
  } else if (ab_tree_sitter_node_type(frame->node, "call")) {
    status = add_call(scan, frame);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_r_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbMapConfig *config, const AbManifestFile *file,
    const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct identifier call nodes; no dynamic target resolution"},
      {"file-documentation", "explicit roxygen @file documentation"},
      {"source-metric-contributors",
       "exact R branch and control syntax inside named functions"},
      {"source-metrics",
       "versioned decision, cyclomatic, and control-nesting summaries for "
       "named functions"},
      {"symbols", "identifier assignments whose opposite operand is a "
                  "function_definition"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-r",
      "3",
      "archbird-tree-sitter-r-v3;runtime=0.26.9;grammar=1.3.0;abi=14",
      "tree-sitter-0.26.9;tree-sitter-r-1.3.0;grammar-abi-14",
      "r",
      tree_sitter_r,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_r,
      NULL,
      NULL,
      "source.control-flow.r-v1",
      classify_r_metric,
      r_callable_boundary};
  return ab_tree_sitter_scan_file(
      engine, manifest, config, file, source, source_length,
      source_manifest_sha256, implementation_sha256, &descriptor, out_bundle);
}
