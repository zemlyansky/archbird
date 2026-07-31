#include "dependency_redirect_internal.h"

#include "act_source.h"
#include "artifact_validation.h"
#include "tree_sitter/api.h"

#include <stdint.h>
#include <string.h>

#define AB_ACT_PYTHON_MAX_REDIRECT_SITES 4096u

const TSLanguage *tree_sitter_python(void);

typedef struct AbActPythonRedirectSite {
  const AbString *path;
  size_t start;
  size_t end;
  AbString expected;
  const AbString *replacement;
} AbActPythonRedirectSite;

typedef struct AbActPythonRedirectBinding {
  const AbString *path;
  AbString local;
  int aliased;
  int has_call;
} AbActPythonRedirectBinding;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         memcmp(value->as.text.data, literal, length) == 0;
}

static int node_is(TSNode node, const char *type) {
  return !ts_node_is_null(node) && strcmp(ts_node_type(node), type) == 0;
}

static ArchbirdStatus reject(AbActContext *context, const char *message) {
  return archbird_error_set(ab_act_executor_engine(context),
                            ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
                            "act Python dependency redirect: %s", message);
}

static const AbValue *unique_python_definition(const AbValue *map,
                                               const AbString *symbol,
                                               const AbValue **out_file) {
  const AbValue *files = field(map, "files");
  const AbValue *match = NULL;
  size_t file_index;
  *out_file = NULL;
  for (file_index = 0; files && files->kind == AB_VALUE_ARRAY &&
                       file_index < files->as.array.count;
       file_index++) {
    const AbValue *file = &files->as.array.items[file_index];
    const AbValue *symbols = field(file, "symbols");
    size_t symbol_index;
    if (!text_is(field(file, "language"), "python") || !symbols ||
        symbols->kind != AB_VALUE_ARRAY)
      continue;
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *candidate = &symbols->as.array.items[symbol_index];
      const AbValue *name = field(candidate, "name");
      if (!name || name->kind != AB_VALUE_STRING ||
          !ab_string_equal(&name->as.text, symbol) ||
          !text_is(field(candidate, "kind"), "function") ||
          field(candidate, "syntax_recovery"))
        continue;
      if (match) {
        *out_file = NULL;
        return NULL;
      }
      match = candidate;
      *out_file = file;
    }
  }
  return match;
}

static int candidate_targets(const AbValue *candidate,
                             const AbActDependencyRedirect *redirect) {
  const AbValue *path = field(candidate, "path");
  const AbValue *symbol = field(candidate, "symbol");
  return path && path->kind == AB_VALUE_STRING && symbol &&
         symbol->kind == AB_VALUE_STRING &&
         ab_string_equal(&symbol->as.text, redirect->from_symbol) &&
         ab_act_dependency_redirect_target_matches(redirect, &path->as.text);
}

static int string_array_unique_value(const AbValue *values,
                                     const AbString **out) {
  size_t index;
  for (index = 0; values && values->kind == AB_VALUE_ARRAY &&
                  index < values->as.array.count;
       index++) {
    const AbValue *value = &values->as.array.items[index];
    if (value->kind != AB_VALUE_STRING || !value->as.text.length)
      return 0;
    if (*out && !ab_string_equal(*out, &value->as.text))
      return 0;
    *out = &value->as.text;
  }
  return 1;
}

static const AbString *canonical_import_module(const AbValue *map,
                                               const AbString *target_path) {
  const AbValue *edges = field(map, "edges");
  const AbString *module = NULL;
  size_t index;
  for (index = 0;
       edges && edges->kind == AB_VALUE_ARRAY && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *target = field(edge, "target");
    if (!text_is(field(edge, "kind"), "import") || !target ||
        target->kind != AB_VALUE_STRING ||
        !ab_string_equal(&target->as.text, target_path))
      continue;
    if (!string_array_unique_value(field(edge, "names"), &module))
      return NULL;
  }
  return module;
}

static int site_add(AbActContext *context, AbActPythonRedirectSite *sites,
                    size_t *count, const AbString *path, size_t start,
                    size_t end, const uint8_t *expected, size_t expected_length,
                    const AbString *replacement) {
  size_t index;
  if (start >= end || expected_length != end - start)
    return 0;
  for (index = 0; index < *count; index++) {
    if (!ab_string_equal(sites[index].path, path) ||
        sites[index].start != start || sites[index].end != end)
      continue;
    return sites[index].expected.length == expected_length &&
           memcmp(sites[index].expected.data, expected, expected_length) == 0 &&
           ab_string_equal(sites[index].replacement, replacement);
  }
  if (*count >= AB_ACT_PYTHON_MAX_REDIRECT_SITES) {
    (void)reject(context, "redirect contains too many exact source sites");
    return 0;
  }
  sites[*count] = (AbActPythonRedirectSite){
      path, start, end, {(char *)expected, expected_length}, replacement};
  (*count)++;
  return 1;
}

static TSNode exact_import_statement(TSNode root, uint32_t start,
                                     uint32_t end) {
  TSNode node;
  if (start >= end)
    return (TSNode){0};
  node = ts_node_descendant_for_byte_range(root, start, end - 1);
  while (!ts_node_is_null(node) && !node_is(node, "import_from_statement"))
    node = ts_node_parent(node);
  return !ts_node_is_null(node) && ts_node_start_byte(node) == start &&
                 ts_node_end_byte(node) == end
             ? node
             : (TSNode){0};
}

static ArchbirdStatus
collect_import(AbActContext *context, const AbValue *relation_site,
               const AbString *replacement_module, const AbString *from_symbol,
               const AbString *to_symbol, AbActPythonRedirectSite *sites,
               size_t *site_count, AbActPythonRedirectBinding *binding) {
  const AbValue *path = field(relation_site, "path");
  const AbValue *module_name = field(relation_site, "name");
  const AbValue *span = field(relation_site, "span");
  ArchbirdSourceView source;
  TSParser *parser = NULL;
  TSTree *tree = NULL;
  TSNode root;
  TSNode statement;
  TSNode module;
  TSNode imported = (TSNode){0};
  TSNode local = (TSNode){0};
  uint64_t start;
  uint64_t end;
  uint32_t child_count;
  uint32_t child_index;
  size_t binding_count = 0;
  ArchbirdStatus status;
  if (!ab_artifact_repository_path(path) || !module_name ||
      module_name->kind != AB_VALUE_STRING || !span ||
      span->kind != AB_VALUE_OBJECT ||
      !ab_artifact_safe_integer(field(span, "start"), &start) ||
      !ab_artifact_safe_integer(field(span, "end"), &end) || start >= end ||
      end > UINT32_MAX)
    return reject(context, "edge projection has an invalid Python import site");
  status = ab_act_executor_source(context, &path->as.text, &source);
  if (status != ARCHBIRD_OK)
    return status;
  if (source.byte_length > UINT32_MAX || end > source.byte_length)
    return reject(context, "Python import site is outside the current source");
  parser = ts_parser_new();
  if (!parser || !ts_parser_set_language(parser, tree_sitter_python())) {
    status = reject(context, "cannot initialize the Python syntax executor");
    goto cleanup;
  }
  tree = ts_parser_parse_string(parser, NULL, (const char *)source.bytes,
                                (uint32_t)source.byte_length);
  if (!tree) {
    status = reject(context, "cannot parse the current Python source");
    goto cleanup;
  }
  root = ts_tree_root_node(tree);
  if (ts_node_has_error(root)) {
    status = reject(context, "current Python source contains syntax recovery");
    goto cleanup;
  }
  statement = exact_import_statement(root, (uint32_t)start, (uint32_t)end);
  if (ts_node_is_null(statement)) {
    status =
        reject(context, "Python import statement is not an exact syntax node");
    goto cleanup;
  }
  module = ts_node_child_by_field_name(statement, "module_name", 11);
  if (!node_is(module, "dotted_name") ||
      ts_node_end_byte(module) > source.byte_length ||
      ts_node_end_byte(module) - ts_node_start_byte(module) !=
          module_name->as.text.length ||
      memcmp(source.bytes + ts_node_start_byte(module),
             module_name->as.text.data, module_name->as.text.length) != 0) {
    status = reject(context,
                    "Python import statement does not expose one exact module");
    goto cleanup;
  }
  child_count = ts_node_named_child_count(statement);
  for (child_index = 0; child_index < child_count; child_index++) {
    TSNode child = ts_node_named_child(statement, child_index);
    if (ts_node_eq(child, module))
      continue;
    if (node_is(child, "aliased_import")) {
      imported = ts_node_child_by_field_name(child, "name", 4);
      local = ts_node_child_by_field_name(child, "alias", 5);
    } else if (node_is(child, "dotted_name")) {
      imported = child;
      local = child;
    } else {
      status = reject(context,
                      "Python redirect supports one explicit imported name");
      goto cleanup;
    }
    binding_count++;
  }
  if (binding_count != 1 || ts_node_is_null(imported) ||
      ts_node_is_null(local) ||
      ts_node_end_byte(imported) > source.byte_length ||
      ts_node_end_byte(local) > source.byte_length ||
      ts_node_end_byte(imported) - ts_node_start_byte(imported) !=
          from_symbol->length ||
      memcmp(source.bytes + ts_node_start_byte(imported), from_symbol->data,
             from_symbol->length) != 0) {
    status = reject(
        context, "Python import does not bind exactly the redirected symbol");
    goto cleanup;
  }
  if (!site_add(context, sites, site_count, &path->as.text,
                ts_node_start_byte(module), ts_node_end_byte(module),
                source.bytes + ts_node_start_byte(module),
                ts_node_end_byte(module) - ts_node_start_byte(module),
                replacement_module) ||
      !site_add(context, sites, site_count, &path->as.text,
                ts_node_start_byte(imported), ts_node_end_byte(imported),
                source.bytes + ts_node_start_byte(imported),
                ts_node_end_byte(imported) - ts_node_start_byte(imported),
                to_symbol)) {
    status = reject(context,
                    "Python import produces conflicting exact source edits");
    goto cleanup;
  }
  binding->path = &path->as.text;
  binding->local =
      (AbString){(char *)source.bytes + ts_node_start_byte(local),
                 ts_node_end_byte(local) - ts_node_start_byte(local)};
  binding->aliased = !ts_node_eq(imported, local);
  status = ARCHBIRD_OK;

cleanup:
  if (tree)
    ts_tree_delete(tree);
  if (parser)
    ts_parser_delete(parser);
  return status;
}

static AbActPythonRedirectBinding *
binding_for(AbActPythonRedirectBinding *bindings, size_t binding_count,
            const AbString *path, const AbString *local) {
  AbActPythonRedirectBinding *match = NULL;
  size_t index;
  for (index = 0; index < binding_count; index++)
    if (ab_string_equal(bindings[index].path, path) &&
        ab_string_equal(&bindings[index].local, local)) {
      if (match)
        return NULL;
      match = &bindings[index];
    }
  return match;
}

static int python_ast_evidence(const AbValue *evidence) {
  return evidence && evidence->kind == AB_VALUE_OBJECT &&
         text_is(field(evidence, "provider"), "archbird-python-ast");
}

static ArchbirdStatus
collect_calls(AbActContext *context, const AbActDependencyRedirect *redirect,
              AbActPythonRedirectBinding *bindings, size_t binding_count,
              AbActPythonRedirectSite *sites, size_t *site_count) {
  const AbValue *calls = field(redirect->map, "symbol_calls");
  size_t row_index;
  size_t matched = 0;
  for (row_index = 0; calls && calls->kind == AB_VALUE_ARRAY &&
                      row_index < calls->as.array.count;
       row_index++) {
    const AbValue *row = &calls->as.array.items[row_index];
    const AbValue *name = field(row, "name");
    const AbValue *source = field(row, "source");
    const AbValue *path = field(source, "path");
    const AbValue *candidates = field(row, "candidates");
    const AbValue *evidence = field(row, "evidence");
    AbActPythonRedirectBinding *binding;
    ArchbirdSourceView source_view;
    size_t evidence_index;
    ArchbirdStatus status;
    if (!name || name->kind != AB_VALUE_STRING || !path ||
        path->kind != AB_VALUE_STRING)
      continue;
    binding =
        binding_for(bindings, binding_count, &path->as.text, &name->as.text);
    if (!binding)
      continue;
    if (!text_is(field(row, "binding"), "imported") ||
        !text_is(field(row, "resolution"), "unique") || !candidates ||
        candidates->kind != AB_VALUE_ARRAY || candidates->as.array.count != 1 ||
        !candidate_targets(&candidates->as.array.items[0], redirect) ||
        !evidence || evidence->kind != AB_VALUE_ARRAY ||
        !evidence->as.array.count)
      return reject(context,
                    "Python call has no unique redirected dependency target");
    status = ab_act_executor_source(context, &path->as.text, &source_view);
    if (status != ARCHBIRD_OK)
      return status;
    for (evidence_index = 0; evidence_index < evidence->as.array.count;
         evidence_index++) {
      const AbValue *row_evidence = &evidence->as.array.items[evidence_index];
      const AbValue *span = field(row_evidence, "span");
      uint64_t start;
      uint64_t end;
      if (!python_ast_evidence(row_evidence) || !span ||
          span->kind != AB_VALUE_OBJECT ||
          !ab_artifact_safe_integer(field(span, "start"), &start) ||
          !ab_artifact_safe_integer(field(span, "end"), &end) || start >= end ||
          end > source_view.byte_length ||
          end - start != name->as.text.length ||
          memcmp(source_view.bytes + (size_t)start, name->as.text.data,
                 name->as.text.length) != 0)
        return reject(context, "Python call has invalid exact evidence");
      if (!binding->aliased &&
          !site_add(context, sites, site_count, &path->as.text, (size_t)start,
                    (size_t)end, (const uint8_t *)name->as.text.data,
                    name->as.text.length, redirect->to_symbol))
        return reject(context,
                      "Python calls produce conflicting exact source edits");
    }
    binding->has_call = 1;
    matched++;
  }
  if (!matched)
    return reject(context,
                  "dependency redirect has no exact Python call sites");
  for (row_index = 0; row_index < binding_count; row_index++)
    if (!bindings[row_index].has_call)
      return reject(
          context,
          "an inducing Python import has no matching exact symbol call");
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_act_python_dependency_redirect(AbActContext *context,
                                  const AbActDependencyRedirect *redirect,
                                  const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *definition_file = NULL;
  const AbValue *definition_symbol;
  const AbValue *definition_path;
  const AbString *module;
  AbActPythonRedirectSite *sites = NULL;
  AbActPythonRedirectBinding *bindings = NULL;
  size_t binding_count = redirect->relation_sites->as.array.count;
  size_t site_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  definition_symbol = unique_python_definition(
      redirect->map, redirect->to_symbol, &definition_file);
  definition_path = field(definition_file, "path");
  if (!definition_symbol || !definition_path ||
      definition_path->kind != AB_VALUE_STRING)
    return reject(context,
                  "replacement has no unique exact Python function definition");
  if (ab_act_dependency_redirect_target_matches(redirect,
                                                &definition_path->as.text))
    return reject(context,
                  "replacement remains inside the forbidden dependency target");
  module = canonical_import_module(redirect->map, &definition_path->as.text);
  if (!module)
    return reject(context,
                  "replacement definition has no unique observed Python import "
                  "module");
  sites = (AbActPythonRedirectSite *)ab_calloc(
      engine, AB_ACT_PYTHON_MAX_REDIRECT_SITES, sizeof(*sites));
  bindings = (AbActPythonRedirectBinding *)ab_calloc(engine, binding_count,
                                                     sizeof(*bindings));
  if (!sites || !bindings)
    status = ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; status == ARCHBIRD_OK && index < binding_count; index++)
    status = collect_import(context,
                            &redirect->relation_sites->as.array.items[index],
                            module, redirect->from_symbol, redirect->to_symbol,
                            sites, &site_count, &bindings[index]);
  if (status == ARCHBIRD_OK)
    status = collect_calls(context, redirect, bindings, binding_count, sites,
                           &site_count);
  for (index = 0; status == ARCHBIRD_OK && index < site_count; index++)
    status = ab_act_executor_replace_exact(
        context, item_id, sites[index].path, sites[index].start,
        sites[index].end, (const uint8_t *)sites[index].expected.data,
        sites[index].expected.length,
        (const uint8_t *)sites[index].replacement->data,
        sites[index].replacement->length);
  ab_free(engine, bindings);
  ab_free(engine, sites);
  return status;
}
