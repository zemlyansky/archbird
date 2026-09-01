#include "evidence/syntax/tree_sitter/c/scanner.h"

#include "evidence/syntax/tree_sitter/scanner.h"

#include "base/render_internal.h"
#include "base/sha256.h"

#include <string.h>

const TSLanguage *tree_sitter_c(void);

typedef struct AbCMacroNames {
  AbString *items;
  size_t count;
  size_t capacity;
} AbCMacroNames;

static int macro_name_equal(const AbTreeSitterScan *scan, const AbString *name,
                            TSNode node) {
  size_t start;
  size_t end;
  return ab_tree_sitter_node_slice(scan, node, &start, &end) &&
         name->length == end - start &&
         !memcmp(name->data, scan->source + start, name->length);
}

static int macro_name_contains(const AbTreeSitterScan *scan, TSNode node) {
  const AbCMacroNames *names = (const AbCMacroNames *)scan->language_data;
  size_t index;
  for (index = 0; names && index < names->count; index++)
    if (macro_name_equal(scan, &names->items[index], node))
      return 1;
  return 0;
}

static ArchbirdStatus macro_name_add(AbTreeSitterScan *scan, TSNode node) {
  AbCMacroNames *names = (AbCMacroNames *)scan->language_data;
  AbString *resized;
  size_t start;
  size_t end;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_slice(scan, node, &start, &end) || start == end)
    return ARCHBIRD_OK;
  if (!names) {
    names = (AbCMacroNames *)ab_calloc(scan->engine, 1, sizeof(*names));
    if (!names)
      return archbird_error_set(scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory tracking C macro names");
    scan->language_data = names;
  }
  if (names->count == names->capacity) {
    size_t capacity = names->capacity ? names->capacity * 2 : 16;
    if (capacity > SIZE_MAX / sizeof(*names->items))
      return ARCHBIRD_LIMIT_EXCEEDED;
    resized = (AbString *)ab_realloc(scan->engine, names->items,
                                     capacity * sizeof(*names->items));
    if (!resized)
      return archbird_error_set(scan->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory tracking C macro names");
    names->items = resized;
    names->capacity = capacity;
  }
  memset(&names->items[names->count], 0, sizeof(*names->items));
  status = ab_string_copy(scan->engine, &names->items[names->count],
                          (const char *)scan->source + start, end - start);
  if (status == ARCHBIRD_OK)
    names->count++;
  return status;
}

static void cleanup_c(AbTreeSitterScan *scan) {
  AbCMacroNames *names = (AbCMacroNames *)scan->language_data;
  size_t index;
  if (!names)
    return;
  for (index = 0; index < names->count; index++)
    ab_string_free(scan->engine, &names->items[index]);
  ab_free(scan->engine, names->items);
  ab_free(scan->engine, names);
  scan->language_data = NULL;
}

static int logical_operator(TSNode node) {
  TSNode operator_node = ab_tree_sitter_child(node, "operator");
  return ab_tree_sitter_node_type(operator_node, "&&") ||
         ab_tree_sitter_node_type(operator_node, "||");
}

static void classify_c_metric(const AbTreeSitterScan *scan, TSNode node,
                              AbTreeSitterMetricNode *out) {
  const char *type = ts_node_type(node);
  TSNode parent = ts_node_parent(node);
  (void)scan;
  if (!strcmp(type, "if_statement")) {
    out->construct = "if";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
    out->flattens_parent_nesting =
        ab_tree_sitter_node_type(parent, "else_clause");
  } else if (!strcmp(type, "for_statement")) {
    out->construct = "for";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "while_statement")) {
    out->construct = "while";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "do_statement")) {
    out->construct = "do-while";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "switch_statement")) {
    out->construct = "switch";
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "case_statement") &&
             !ts_node_is_null(ab_tree_sitter_child(node, "value"))) {
    out->construct = "case";
    out->decision_contribution = 1;
  } else if (!strcmp(type, "conditional_expression")) {
    out->construct = "conditional";
    out->decision_contribution = 1;
    out->starts_control_nesting = 1;
  } else if (!strcmp(type, "binary_expression") && logical_operator(node)) {
    TSNode operator_node = ab_tree_sitter_child(node, "operator");
    out->construct = ab_tree_sitter_node_type(operator_node, "&&")
                         ? "logical-and"
                         : "logical-or";
    out->decision_contribution = 1;
  }
}

static int c_callable_boundary(TSNode node) {
  return ab_tree_sitter_node_type(node, "function_definition");
}

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

static int declarator_is_array(TSNode node) {
  size_t depth;
  for (depth = 0; depth < 32 && !ts_node_is_null(node); depth++) {
    TSNode child;
    if (ab_tree_sitter_node_type(node, "array_declarator"))
      return 1;
    child = ab_tree_sitter_child(node, "declarator");
    if (ts_node_is_null(child))
      break;
    node = child;
  }
  return 0;
}

static ArchbirdStatus hash_framed(ArchbirdSha256Context *context,
                                  const void *bytes, size_t length) {
  uint8_t encoded[8];
  uint64_t value = (uint64_t)length;
  size_t index;
  for (index = 0; index < sizeof(encoded); index++)
    encoded[sizeof(encoded) - index - 1] = (uint8_t)(value >> (index * 8));
  if (archbird_sha256_update(context, encoded, sizeof(encoded)) != ARCHBIRD_OK)
    return ARCHBIRD_LIMIT_EXCEEDED;
  return archbird_sha256_update(context, (const uint8_t *)bytes, length);
}

static ArchbirdStatus hash_u64_value(ArchbirdSha256Context *context,
                                     uint64_t value) {
  uint8_t encoded[8];
  size_t index;
  for (index = 0; index < sizeof(encoded); index++)
    encoded[sizeof(encoded) - index - 1] = (uint8_t)(value >> (index * 8));
  return archbird_sha256_update(context, encoded, sizeof(encoded));
}

static int hash_ignored_node(TSNode node) {
  return ab_tree_sitter_node_type(node, "comment");
}

static ArchbirdStatus hash_syntax_node(const AbTreeSitterScan *scan,
                                       TSNode node,
                                       ArchbirdSha256Context *context,
                                       size_t depth) {
  uint32_t child_count;
  uint32_t index;
  size_t retained = 0;
  const char *type;
  ArchbirdStatus status;
  if (depth >= 256)
    return ARCHBIRD_LIMIT_EXCEEDED;
  type = ts_node_type(node);
  status = hash_framed(context, type, strlen(type));
  if (status != ARCHBIRD_OK)
    return status;
  child_count = ts_node_child_count(node);
  for (index = 0; index < child_count; index++)
    if (!hash_ignored_node(ts_node_child(node, index)))
      retained++;
  status = hash_u64_value(context, retained);
  if (status != ARCHBIRD_OK)
    return status;
  if (retained == 0) {
    size_t start;
    size_t end;
    if (!ab_tree_sitter_node_slice(scan, node, &start, &end))
      return ARCHBIRD_CONFLICT;
    return hash_framed(context, scan->source + start, end - start);
  }
  for (index = 0; index < child_count; index++) {
    TSNode child = ts_node_child(node, index);
    const char *field;
    if (hash_ignored_node(child))
      continue;
    field = ts_node_field_name_for_child(node, index);
    status =
        hash_framed(context, field ? field : "", field ? strlen(field) : 0);
    if (status == ARCHBIRD_OK)
      status = hash_syntax_node(scan, child, context, depth + 1);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus syntax_digest(const AbTreeSitterScan *scan, TSNode node,
                                    char digest_hex[65]) {
  ArchbirdSha256Context context;
  uint8_t digest[32];
  ArchbirdStatus status;
  archbird_sha256_init(&context);
  status = hash_syntax_node(scan, node, &context, 0);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, digest_hex);
  return ARCHBIRD_OK;
}

static ArchbirdStatus ordered_container_id(AbTreeSitterScan *scan,
                                           const AbTreeSitterFrame *frame,
                                           TSNode name, AbBuffer *identity) {
  size_t start;
  size_t end;
  ArchbirdStatus status;
  if (!ab_tree_sitter_node_slice(scan, name, &start, &end) || start == end)
    return ARCHBIRD_CONFLICT;
  status = ab_buffer_append(identity, scan->file->path.data,
                            scan->file->path.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(identity, ":");
  if (status == ARCHBIRD_OK && frame->has_enclosing) {
    status =
        ab_buffer_append(identity, frame->enclosing, frame->enclosing_length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(identity, "/");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(identity, scan->source + start, end - start);
  return status;
}

static int ordered_row_dynamic(const AbTreeSitterScan *scan, TSNode node) {
  const char *type = ts_node_type(node);
  return strncmp(type, "preproc_", 8) == 0 ||
         (ab_tree_sitter_node_type(node, "identifier") &&
          macro_name_contains(scan, node));
}

static ArchbirdStatus add_ordered_array(AbTreeSitterScan *scan,
                                        const AbTreeSitterFrame *frame) {
  TSNode declarator = ab_tree_sitter_child(frame->node, "declarator");
  TSNode value = ab_tree_sitter_child(frame->node, "value");
  TSNode name;
  AbBuffer identity;
  AbFact *container_fact = NULL;
  size_t name_start;
  size_t name_end;
  uint32_t child_count;
  uint32_t index;
  uint64_t row_count = 0;
  int dynamic = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!scan->config || !scan->config->ordered_container_count)
    return ARCHBIRD_OK;
  if (!declarator_is_array(declarator) ||
      !ab_tree_sitter_node_type(value, "initializer_list"))
    return ARCHBIRD_OK;
  name = declarator_identifier(declarator);
  if (!ab_tree_sitter_node_slice(scan, name, &name_start, &name_end) ||
      name_start == name_end)
    return ARCHBIRD_OK;
  child_count = ts_node_named_child_count(value);
  for (index = 0; index < child_count; index++) {
    TSNode row = ts_node_named_child(value, index);
    if (ordered_row_dynamic(scan, row))
      dynamic = 1;
    else
      row_count++;
  }
  ab_buffer_init(&identity, scan->engine);
  status = ordered_container_id(scan, frame, name, &identity);
  if (status == ARCHBIRD_OK) {
    AbString language = {(char *)"c", 1};
    AbString container_name = {(char *)identity.data + scan->file->path.length +
                                   1,
                               identity.length - scan->file->path.length - 1};
    if (!ab_map_config_ordered_container_selected(
            scan->config, &language, &scan->file->path, &container_name)) {
      ab_buffer_free(&identity);
      return ARCHBIRD_OK;
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact(
        scan->builder, "ordered-containers", "sequence", "syntax-structure",
        ts_node_start_byte(value), ts_node_end_byte(value), identity.data,
        identity.length, scan->source + name_start, name_end - name_start,
        &container_fact);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_track_fact_region(scan, container_fact, value);
  if (status == ARCHBIRD_OK)
    status = ab_tree_sitter_add_line(scan, container_fact, value);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(scan->engine, container_fact, "container",
                                     identity.data, identity.length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(scan->engine, container_fact,
                                       "declared_count", row_count);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        scan->engine, container_fact, "normalization",
        (const uint8_t *)"tree-sitter-c-syntax-v1",
        strlen("tree-sitter-c-syntax-v1"));
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        scan->engine, container_fact, "population_state",
        (const uint8_t *)(dynamic ? "dynamic" : "closed"),
        strlen(dynamic ? "dynamic" : "closed"));
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        scan->engine, container_fact, "state",
        (const uint8_t *)(dynamic ? "unknown" : "current"),
        strlen(dynamic ? "unknown" : "current"));
  for (index = 0; status == ARCHBIRD_OK && index < child_count; index++) {
    TSNode row = ts_node_named_child(value, index);
    AbBuffer key;
    AbFact *row_fact = NULL;
    char digest[65];
    if (ordered_row_dynamic(scan, row))
      continue;
    status = syntax_digest(scan, row, digest);
    ab_buffer_init(&key, scan->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&key, identity.data, identity.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&key, ":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&key, digest, 64);
    if (status == ARCHBIRD_OK)
      status = ab_bundle_builder_add_fact(
          scan->builder, "ordered-rows", "row", "syntax-structure",
          ts_node_start_byte(row), ts_node_end_byte(row), key.data, key.length,
          scan->source + name_start, name_end - name_start, &row_fact);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_track_fact_region(scan, row_fact, row);
    if (status == ARCHBIRD_OK)
      status = ab_tree_sitter_add_line(scan, row_fact, row);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(scan->engine, row_fact, "container",
                                            identity.data, identity.length);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(scan->engine, row_fact, "digest",
                                            (const uint8_t *)digest, 64);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(
          scan->engine, row_fact, "normalization",
          (const uint8_t *)"tree-sitter-c-syntax-v1",
          strlen("tree-sitter-c-syntax-v1"));
    if (status == ARCHBIRD_OK)
      status =
          ab_fact_add_u64_attribute(scan->engine, row_fact, "ordinal", index);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(scan->engine, row_fact, "state",
                                            (const uint8_t *)"current",
                                            strlen("current"));
    ab_buffer_free(&key);
  }
  ab_buffer_free(&identity);
  return status;
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
    status = ab_tree_sitter_add_symbol_extent(scan, fact, owner);
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
    if (status == ARCHBIRD_OK && fact) {
      ab_tree_sitter_set_enclosing_fact(child_frame, fact);
      status = ab_tree_sitter_start_source_metrics(scan, fact, frame->node,
                                                   child_frame);
    }
  } else if (ab_tree_sitter_node_type(frame->node, "declaration") &&
             !(frame->context & AB_TS_CONTEXT_FUNCTION)) {
    status = add_symbol(scan, frame->node, "declaration", &name, &fact);
  } else if (ab_tree_sitter_node_type(frame->node, "call_expression")) {
    status = add_call(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "preproc_include")) {
    status = add_include(scan, frame->node);
  } else if (ab_tree_sitter_node_type(frame->node, "preproc_def") ||
             ab_tree_sitter_node_type(frame->node, "preproc_function_def")) {
    status = macro_name_add(scan, ab_tree_sitter_child(frame->node, "name"));
  } else if (ab_tree_sitter_node_type(frame->node, "init_declarator")) {
    status = add_ordered_array(scan, frame);
  } else if (ab_tree_sitter_node_type(frame->node, "identifier")) {
    status = add_value_reference(scan, frame);
  }
  return status;
}

ArchbirdStatus ab_scan_tree_sitter_c_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbMapConfig *config, const AbManifestFile *file,
    const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32],
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const AbTreeSitterCapabilitySpec capabilities[] = {
      {"calls",
       "direct identifier call_expression nodes; no preprocessing, function-"
       "pointer resolution, or semantic target"},
      {"file-documentation", "explicit Doxygen @file documentation"},
      {"imports",
       "preproc_include path nodes; no include search or conditional-"
       "preprocessor evaluation"},
      {"ordered-containers",
       "initialized C arrays with statically enumerable initializer rows; "
       "preprocessor-generated populations remain dynamic"},
      {"ordered-rows",
       "ordered initializer rows in statically enumerable C arrays; syntax "
       "digests are language-local"},
      {"source-metric-contributors",
       "exact C branch and control syntax inside named function definitions"},
      {"source-metrics",
       "versioned decision, cyclomatic, and control-nesting summaries for "
       "named function definitions"},
      {"symbol-references",
       "direct identifier values in initializers, assignments, call "
       "arguments, and returns; target identity remains unresolved syntax "
       "evidence"},
      {"symbols",
       "function definitions and top-level function declarations; recovered "
       "ranges reduce coverage to partial"}};
  static const AbTreeSitterDescriptor descriptor = {
      "archbird-tree-sitter-c",
      "4",
      "archbird-tree-sitter-c-v4;runtime=0.26.9;grammar=0.24.2;abi=15",
      "tree-sitter-0.26.9;tree-sitter-c-0.24.2;grammar-abi-15",
      "c",
      tree_sitter_c,
      capabilities,
      sizeof(capabilities) / sizeof(capabilities[0]),
      visit_c,
      NULL,
      cleanup_c,
      "source.control-flow.c-v1",
      classify_c_metric,
      c_callable_boundary};
  return ab_tree_sitter_scan_file(
      engine, manifest, config, file, source, source_length,
      source_manifest_sha256, implementation_sha256, &descriptor, out_bundle);
}
