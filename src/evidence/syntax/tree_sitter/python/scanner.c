#include "evidence/syntax/tree_sitter/python/scanner.h"

#include "base/render_internal.h"
#include "evidence/python/definitions.h"
#include "evidence/python/tokens.h"
#include "evidence/syntax/tree_sitter/scanner.h"

#include <string.h>

const TSLanguage *tree_sitter_python(void);

static void classify_python_metric(const AbTreeSitterScan *scan, TSNode node,
                                   AbTreeSitterMetricNode *out) {
  const char *type = ts_node_type(node);
  TSNode parent = ts_node_parent(node);
  (void)scan;
  if (!strcmp(type, "if_statement")) {
    out->construct = "if";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "elif_clause")) {
    out->construct = "elif";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
    out->flattens_parent_nesting = 1;
  } else if (!strcmp(type, "for_statement") || !strcmp(type, "for_in_clause")) {
    out->construct =
        !strcmp(type, "for_in_clause") ? "comprehension-for" : "for";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "while_statement")) {
    out->construct = "while";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "else_clause") &&
             (ab_tree_sitter_node_type(parent, "for_statement") ||
              ab_tree_sitter_node_type(parent, "while_statement"))) {
    out->construct = "loop-else";
    out->decision_contribution = 1;
  } else if (!strcmp(type, "conditional_expression")) {
    out->construct = "conditional";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "boolean_operator")) {
    TSNode operator_node = ab_tree_sitter_child(node, "operator");
    out->construct = ab_tree_sitter_node_type(operator_node, "and")
                         ? "logical-and"
                         : "logical-or";
    out->decision_contribution = 1;
  } else if (!strcmp(type, "try_statement")) {
    out->construct = "try";
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "except_clause")) {
    out->construct = "except";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
    out->flattens_parent_nesting = 1;
  } else if (!strcmp(type, "with_statement")) {
    out->construct = "with";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "assert_statement")) {
    out->construct = "assert";
    out->decision_contribution = 1;
  } else if (!strcmp(type, "match_statement")) {
    out->construct = "match";
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "case_clause")) {
    out->construct = "case";
    out->decision_contribution = 1;
  } else if (!strcmp(type, "if_clause")) {
    out->construct = "comprehension-if";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  }
}

static int python_callable_boundary(TSNode node) {
  return ab_tree_sitter_node_type(node, "function_definition") ||
         ab_tree_sitter_node_type(node, "lambda");
}

static int node_is_name(const AbTreeSitterScan *scan, TSNode node) {
  return ab_tree_sitter_node_has_text(scan, node) &&
         (ab_tree_sitter_node_type(node, "identifier") ||
          ab_tree_sitter_node_type(node, "dotted_name"));
}

static ArchbirdStatus prepare_python(AbTreeSitterScan *scan) {
  static const char encoded_source[] =
      "inapplicable: Tree-sitter Python requires UTF-8 source bytes; the "
      "encoding-aware host provider may still contribute syntax evidence";
  AbPythonDefinitions *definitions;
  ArchbirdStatus status;
  status = ab_python_source_utf8_validate(scan->engine, scan->source,
                                          scan->source_length);
  if (status == ARCHBIRD_INVALID_SCHEMA) {
    size_t offset = archbird_engine_error_offset(scan->engine);
    archbird_error_clear(scan->engine);
    scan->inapplicable_code = "tree-sitter-python-encoding-inapplicable";
    scan->inapplicable_message = encoded_source;
    if (offset < scan->source_length) {
      scan->inapplicable_start = offset;
      scan->inapplicable_end = offset + 1;
      scan->has_inapplicable_span = 1;
    }
    return ARCHBIRD_OK;
  }
  if (status != ARCHBIRD_OK)
    return status;
  definitions =
      (AbPythonDefinitions *)ab_calloc(scan->engine, 1, sizeof(*definitions));
  if (!definitions)
    return archbird_error_set(
        scan->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory preparing Python lexical scope evidence");
  status = ab_python_definitions_collect(scan->engine, scan->source,
                                         scan->source_length, definitions);
  if (status == ARCHBIRD_INVALID_SCHEMA) {
    archbird_error_clear(scan->engine);
    ab_python_definitions_free(definitions);
    ab_free(scan->engine, definitions);
    return ARCHBIRD_OK;
  }
  if (status != ARCHBIRD_OK) {
    ab_free(scan->engine, definitions);
    return status;
  }
  scan->language_data = definitions;
  return ARCHBIRD_OK;
}

static void cleanup_python(AbTreeSitterScan *scan) {
  AbPythonDefinitions *definitions = (AbPythonDefinitions *)scan->language_data;
  if (!definitions)
    return;
  ab_python_definitions_free(definitions);
  ab_free(scan->engine, definitions);
  scan->language_data = NULL;
}

static ArchbirdStatus add_lexical_enclosing(AbTreeSitterScan *scan,
                                            AbFact *fact, size_t source_offset,
                                            const AbTreeSitterFrame *fallback) {
  const AbPythonDefinition *definition = ab_python_enclosing_at(
      (const AbPythonDefinitions *)scan->language_data, source_offset);
  if (!definition)
    return ab_tree_sitter_add_enclosing(scan, fact, fallback);
  return ab_fact_add_string_attribute(
      scan->engine, fact, "enclosing",
      (const uint8_t *)definition->qualified.data,
      definition->qualified.length);
}

static ArchbirdStatus add_symbol(AbTreeSitterScan *scan,
                                 const AbTreeSitterFrame *frame, TSNode owner,
                                 const char *kind, TSNode *out_name,
                                 AbFact **out_fact) {
  TSNode name = ab_tree_sitter_child(owner, "name");
  AbFact *fact;
  ArchbirdStatus status;
  size_t name_start;
  size_t name_end;
  const AbPythonDefinition *definition = NULL;
  static const char *const containers[] = {"class_definition",
                                           "function_definition"};
  *out_name = (TSNode){0};
  *out_fact = NULL;
  if (!node_is_name(scan, name))
    return ARCHBIRD_OK;
  if (ab_tree_sitter_node_slice(scan, name, &name_start, &name_end) &&
      scan->language_data)
    definition = ab_python_definition_at(
        (const AbPythonDefinitions *)scan->language_data, name_start, name_end);
  if (definition) {
    kind = definition->kind;
    status = ab_bundle_builder_add_fact(
        scan->builder, "symbols", kind, "syntax-structure", name_start,
        name_end, (const uint8_t *)definition->qualified.data,
        definition->qualified.length,
        (const uint8_t *)definition->qualified.data,
        definition->qualified.length, &fact);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_track_fact_region(scan, fact, owner);
  } else {
    status = ab_tree_sitter_add_qualified_fact(
        scan, "symbols", kind, owner, name, containers,
        sizeof(containers) / sizeof(containers[0]), &fact);
  }
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, fact, name);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_symbol_extent(scan, fact, owner);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(scan->engine, fact, "syntax_kind",
                                          (const uint8_t *)ts_node_type(owner),
                                          strlen(ts_node_type(owner)));
  if (status == ARCHBIRD_OK) {
    const AbPythonDefinition *parent =
        definition
            ? ab_python_definition_parent(
                  (const AbPythonDefinitions *)scan->language_data, definition)
            : NULL;
    if (parent)
      status = ab_fact_add_string_attribute(
          scan->engine, fact, "enclosing",
          (const uint8_t *)parent->qualified.data, parent->qualified.length);
    else if (!definition && frame->has_enclosing)
      status = ab_tree_sitter_add_enclosing(scan, fact, frame);
  }
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
    status = add_lexical_enclosing(scan, fact, ts_node_start_byte(name), frame);
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
    status = ab_tree_sitter_add_symbol_extent(scan, fact, assignment);
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
    ab_tree_sitter_suspend_source_metrics(child_frame);
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
    if (status == ARCHBIRD_OK && fact) {
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
      status = ab_tree_sitter_start_source_metrics(scan, fact, frame->node,
                                                   child_frame);
    }
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
    const AbMapConfig *config, const AbManifestFile *file,
    const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls", "direct identifier call nodes; no dynamic target resolution"},
      {"imports", "statically named import and from-import module nodes"},
      {"method-calls",
       "attribute call member names; receiver identity remains unresolved"},
      {"source-metric-contributors",
       "exact Python branch and control syntax inside named function and "
       "method definitions"},
      {"source-metrics",
       "versioned decision, cyclomatic, and control-nesting summaries for "
       "named function and method definitions"},
      {"symbols",
       "named class/function definitions and simple module assignments with "
       "lexical enclosing context"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-python",
      "4",
      "archbird-tree-sitter-python-v4;runtime=0.26.9;grammar=0.25.0;abi=15",
      "tree-sitter-0.26.9;tree-sitter-python-0.25.0;grammar-abi-15",
      "python",
      tree_sitter_python,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_python,
      prepare_python,
      cleanup_python,
      "source.control-flow.python-v1",
      classify_python_metric,
      python_callable_boundary};
  return ab_tree_sitter_scan_file(
      engine, manifest, config, file, source, source_length,
      source_manifest_sha256, implementation_sha256, &descriptor, out_bundle);
}
