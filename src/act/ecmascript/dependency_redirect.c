#include "dependency_redirect_internal.h"

#include "act_source.h"
#include "artifact_validation.h"
#include "tree_sitter/api.h"

#include <stdint.h>
#include <string.h>

#define AB_ACT_ECMASCRIPT_MAX_REDIRECT_SITES 4096u

const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_typescript(void);
const TSLanguage *tree_sitter_tsx(void);

typedef struct AbActEcmascriptRedirectSite {
  const AbString *path;
  size_t start;
  size_t end;
  AbString expected;
  const AbString *replacement;
} AbActEcmascriptRedirectSite;

typedef struct AbActEcmascriptRedirectBinding {
  const AbString *path;
  AbString local;
  size_t import_start;
  size_t import_end;
  int aliased;
  int has_call;
} AbActEcmascriptRedirectBinding;

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

static ArchbirdStatus reject(AbActContext *context, const char *message) {
  return archbird_error_set(ab_act_executor_engine(context),
                            ARCHBIRD_POLICY_REJECTED, ARCHBIRD_NO_OFFSET,
                            "act ECMAScript dependency redirect: %s", message);
}

static int ecmascript_language(const AbValue *file) {
  const AbValue *language = field(file, "language");
  return text_is(language, "javascript") || text_is(language, "typescript") ||
         text_is(language, "tsx");
}

static const AbValue *unique_definition(const AbValue *map,
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
    if (!ecmascript_language(file) || !symbols ||
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

static int same_directory(const AbString *left, const AbString *right) {
  size_t left_length = left->length;
  size_t right_length = right->length;
  while (left_length && left->data[left_length - 1] != '/')
    left_length--;
  while (right_length && right->data[right_length - 1] != '/')
    right_length--;
  return left_length == right_length &&
         (!left_length || memcmp(left->data, right->data, left_length) == 0);
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

static const AbString *observed_module(const AbValue *map,
                                       const AbString *target_path,
                                       const AbString *source_path) {
  const AbValue *edges = field(map, "edges");
  const AbString *module = NULL;
  size_t index;
  for (index = 0;
       edges && edges->kind == AB_VALUE_ARRAY && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *source = field(edge, "source");
    const AbValue *target = field(edge, "target");
    if (!text_is(field(edge, "kind"), "import") || !source ||
        source->kind != AB_VALUE_STRING || !target ||
        target->kind != AB_VALUE_STRING ||
        !ab_string_equal(&target->as.text, target_path) ||
        !same_directory(&source->as.text, source_path))
      continue;
    if (!string_array_unique_value(field(edge, "names"), &module))
      return NULL;
  }
  return module;
}

static const TSLanguage *syntax_provider_language(const AbValue *provider) {
  const AbValue *name = field(provider, "name");
  if (text_is(name, "archbird-tree-sitter-javascript"))
    return tree_sitter_javascript();
  if (text_is(name, "archbird-tree-sitter-typescript"))
    return tree_sitter_typescript();
  if (text_is(name, "archbird-tree-sitter-tsx"))
    return tree_sitter_tsx();
  return NULL;
}

static int typescript_evidence(const AbValue *evidence) {
  return evidence && evidence->kind == AB_VALUE_OBJECT &&
         text_is(field(evidence, "provider"), "archbird-typescript");
}

static int span(const AbValue *value, uint64_t *start, uint64_t *end) {
  return value && value->kind == AB_VALUE_OBJECT &&
         ab_artifact_safe_integer(field(value, "start"), start) &&
         ab_artifact_safe_integer(field(value, "end"), end) && *start < *end;
}

static TSNode exact_import_specifier(TSNode root, uint32_t start,
                                     uint32_t end) {
  TSNode node;
  if (start >= end)
    return (TSNode){0};
  node = ts_node_descendant_for_byte_range(root, start, end - 1);
  while (!ts_node_is_null(node) &&
         strcmp(ts_node_type(node), "import_specifier") != 0)
    node = ts_node_parent(node);
  if (!ts_node_is_null(node)) {
    TSNode name = ts_node_child_by_field_name(node, "name", 4);
    if (ts_node_is_null(name) || ts_node_start_byte(name) != start ||
        ts_node_end_byte(name) != end)
      return (TSNode){0};
  }
  return node;
}

static int site_add(AbActContext *context, AbActEcmascriptRedirectSite *sites,
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
  if (*count >= AB_ACT_ECMASCRIPT_MAX_REDIRECT_SITES) {
    (void)reject(context, "redirect contains too many exact source sites");
    return 0;
  }
  sites[*count] = (AbActEcmascriptRedirectSite){
      path, start, end, {(char *)expected, expected_length}, replacement};
  (*count)++;
  return 1;
}

static const AbValue *unique_import_fact(const AbValue *map,
                                         const AbString *path,
                                         const AbString *module,
                                         const AbString *from_symbol) {
  const AbValue *facts = field(map, "facts");
  const AbValue *match = NULL;
  size_t same_module_count = 0;
  size_t index;
  for (index = 0;
       facts && facts->kind == AB_VALUE_ARRAY && index < facts->as.array.count;
       index++) {
    const AbValue *fact = &facts->as.array.items[index];
    const AbValue *fact_path = field(fact, "path");
    const AbValue *attributes = field(fact, "attributes");
    const AbValue *fact_module = field(attributes, "module");
    const AbValue *imported = field(attributes, "imported");
    if (!fact_path || fact_path->kind != AB_VALUE_STRING ||
        !ab_string_equal(&fact_path->as.text, path) ||
        !text_is(field(fact, "domain"), "imported-names") || !fact_module ||
        fact_module->kind != AB_VALUE_STRING ||
        !ab_string_equal(&fact_module->as.text, module))
      continue;
    same_module_count++;
    if (imported && imported->kind == AB_VALUE_STRING &&
        ab_string_equal(&imported->as.text, from_symbol) &&
        text_is(field(fact, "kind"), "named") &&
        syntax_provider_language(field(fact, "provider"))) {
      if (match)
        return NULL;
      match = fact;
    }
  }
  return same_module_count == 1 ? match : NULL;
}

static ArchbirdStatus
collect_import(AbActContext *context, const AbActDependencyRedirect *redirect,
               const AbValue *relation_site, const AbString *definition_path,
               AbActEcmascriptRedirectSite *sites, size_t *site_count,
               AbActEcmascriptRedirectBinding *binding) {
  const AbValue *path = field(relation_site, "path");
  const AbValue *module_name = field(relation_site, "name");
  const AbValue *module_span = field(relation_site, "span");
  const AbValue *fact;
  const AbValue *attributes;
  const AbValue *local;
  const AbString *replacement_module;
  const TSLanguage *language;
  ArchbirdSourceView source;
  TSParser *parser = NULL;
  TSTree *tree = NULL;
  TSNode root;
  TSNode specifier;
  TSNode imported_node;
  TSNode alias_node;
  uint64_t module_start;
  uint64_t module_end;
  uint64_t import_start;
  uint64_t import_end;
  ArchbirdStatus status;
  if (!ab_artifact_repository_path(path) || !module_name ||
      module_name->kind != AB_VALUE_STRING ||
      !span(module_span, &module_start, &module_end))
    return reject(context,
                  "edge projection has an invalid ECMAScript import site");
  status = ab_act_executor_source(context, &path->as.text, &source);
  if (status != ARCHBIRD_OK)
    return status;
  if (module_end > source.byte_length ||
      module_end - module_start != module_name->as.text.length ||
      memcmp(source.bytes + (size_t)module_start, module_name->as.text.data,
             module_name->as.text.length) != 0)
    return reject(context,
                  "ECMAScript module span does not match current source");
  fact = unique_import_fact(redirect->map, &path->as.text,
                            &module_name->as.text, redirect->from_symbol);
  attributes = field(fact, "attributes");
  local = field(attributes, "local");
  language = syntax_provider_language(field(fact, "provider"));
  if (!fact || !local || local->kind != AB_VALUE_STRING ||
      !local->as.text.length || !language ||
      !span(field(fact, "span"), &import_start, &import_end) ||
      import_end > source.byte_length ||
      import_end - import_start != redirect->from_symbol->length ||
      memcmp(source.bytes + (size_t)import_start, redirect->from_symbol->data,
             redirect->from_symbol->length) != 0)
    return reject(context,
                  "ECMAScript redirect requires one exact named import");
  if (source.byte_length > UINT32_MAX || import_end > UINT32_MAX)
    return reject(context, "ECMAScript source exceeds the syntax edit limit");
  parser = ts_parser_new();
  if (!parser || !ts_parser_set_language(parser, language)) {
    status =
        reject(context, "cannot initialize the ECMAScript syntax executor");
    goto cleanup;
  }
  tree = ts_parser_parse_string(parser, NULL, (const char *)source.bytes,
                                (uint32_t)source.byte_length);
  if (!tree) {
    status = reject(context, "cannot parse the current ECMAScript source");
    goto cleanup;
  }
  root = ts_tree_root_node(tree);
  if (ts_node_has_error(root)) {
    status =
        reject(context, "current ECMAScript source contains syntax recovery");
    goto cleanup;
  }
  specifier = exact_import_specifier(root, (uint32_t)import_start,
                                     (uint32_t)import_end);
  if (ts_node_is_null(specifier)) {
    status = reject(context,
                    "ECMAScript imported binding is not one exact syntax node");
    goto cleanup;
  }
  imported_node = ts_node_child_by_field_name(specifier, "name", 4);
  alias_node = ts_node_child_by_field_name(specifier, "alias", 5);
  if (ts_node_is_null(imported_node) ||
      ts_node_end_byte(imported_node) > source.byte_length ||
      ts_node_end_byte(imported_node) - ts_node_start_byte(imported_node) !=
          redirect->from_symbol->length ||
      memcmp(source.bytes + ts_node_start_byte(imported_node),
             redirect->from_symbol->data, redirect->from_symbol->length) != 0 ||
      (!ts_node_is_null(alias_node) &&
       (ts_node_end_byte(alias_node) > source.byte_length ||
        ts_node_end_byte(alias_node) - ts_node_start_byte(alias_node) !=
            local->as.text.length ||
        memcmp(source.bytes + ts_node_start_byte(alias_node),
               local->as.text.data, local->as.text.length) != 0)) ||
      (ts_node_is_null(alias_node) &&
       !ab_string_equal(&local->as.text, redirect->from_symbol))) {
    status =
        reject(context, "ECMAScript imported binding does not match syntax "
                        "evidence");
    goto cleanup;
  }
  replacement_module =
      observed_module(redirect->map, definition_path, &path->as.text);
  if (!replacement_module) {
    status = reject(context,
                    "replacement has no unique observed ECMAScript module in "
                    "the source directory");
    goto cleanup;
  }
  if (!site_add(context, sites, site_count, &path->as.text,
                (size_t)module_start, (size_t)module_end,
                source.bytes + (size_t)module_start,
                (size_t)(module_end - module_start), replacement_module) ||
      !site_add(context, sites, site_count, &path->as.text,
                (size_t)import_start, (size_t)import_end,
                source.bytes + (size_t)import_start,
                (size_t)(import_end - import_start), redirect->to_symbol))
    status = reject(
        context, "ECMAScript import produces conflicting exact source edits");
  if (status != ARCHBIRD_OK)
    goto cleanup;
  binding->path = &path->as.text;
  binding->local = local->as.text;
  binding->import_start = (size_t)import_start;
  binding->import_end = (size_t)import_end;
  binding->aliased = !ts_node_is_null(alias_node);
  status = ARCHBIRD_OK;

cleanup:
  if (tree)
    ts_tree_delete(tree);
  if (parser)
    ts_parser_delete(parser);
  return status;
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

static AbActEcmascriptRedirectBinding *
binding_for(AbActEcmascriptRedirectBinding *bindings, size_t binding_count,
            const AbString *path, const AbString *local) {
  AbActEcmascriptRedirectBinding *match = NULL;
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

static int binding_path_has(const AbActEcmascriptRedirectBinding *bindings,
                            size_t binding_count, const AbString *path) {
  size_t index;
  for (index = 0; index < binding_count; index++)
    if (ab_string_equal(bindings[index].path, path))
      return 1;
  return 0;
}

static ArchbirdStatus
collect_calls(AbActContext *context, const AbActDependencyRedirect *redirect,
              AbActEcmascriptRedirectBinding *bindings, size_t binding_count,
              AbActEcmascriptRedirectSite *sites, size_t *site_count) {
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
    AbActEcmascriptRedirectBinding *binding;
    ArchbirdSourceView source_view;
    size_t exact_count = 0;
    size_t evidence_index;
    ArchbirdStatus status;
    if (!name || name->kind != AB_VALUE_STRING || !path ||
        path->kind != AB_VALUE_STRING)
      continue;
    binding =
        binding_for(bindings, binding_count, &path->as.text, &name->as.text);
    if (!binding)
      continue;
    if (!text_is(field(row, "resolution"), "unique") || !candidates ||
        candidates->kind != AB_VALUE_ARRAY || candidates->as.array.count != 1 ||
        !candidate_targets(&candidates->as.array.items[0], redirect) ||
        !evidence || evidence->kind != AB_VALUE_ARRAY)
      return reject(context, "ECMAScript call has no unique redirected target");
    status = ab_act_executor_source(context, &path->as.text, &source_view);
    if (status != ARCHBIRD_OK)
      return status;
    for (evidence_index = 0; evidence_index < evidence->as.array.count;
         evidence_index++) {
      const AbValue *row_evidence = &evidence->as.array.items[evidence_index];
      uint64_t start;
      uint64_t end;
      if (!typescript_evidence(row_evidence))
        continue;
      if (!span(field(row_evidence, "span"), &start, &end) ||
          end > source_view.byte_length ||
          end - start != name->as.text.length ||
          memcmp(source_view.bytes + (size_t)start, name->as.text.data,
                 name->as.text.length) != 0)
        return reject(context, "ECMAScript call has invalid compiler evidence");
      if (!binding->aliased &&
          !site_add(context, sites, site_count, &path->as.text, (size_t)start,
                    (size_t)end, source_view.bytes + (size_t)start,
                    (size_t)(end - start), redirect->to_symbol))
        return reject(
            context, "ECMAScript calls produce conflicting exact source edits");
      exact_count++;
    }
    if (exact_count != 1)
      return reject(
          context,
          "ECMAScript call does not have one exact TypeScript provider span");
    binding->has_call = 1;
    matched++;
  }
  if (!matched)
    return reject(context,
                  "dependency redirect has no exact ECMAScript call sites");
  for (row_index = 0; row_index < binding_count; row_index++)
    if (!bindings[row_index].has_call)
      return reject(
          context,
          "an inducing ECMAScript import has no matching exact symbol call");
  return ARCHBIRD_OK;
}

static int occurrence_is_import(const AbActEcmascriptRedirectBinding *bindings,
                                size_t binding_count, const AbString *path,
                                uint64_t start, uint64_t end) {
  size_t index;
  for (index = 0; index < binding_count; index++)
    if (ab_string_equal(bindings[index].path, path) &&
        bindings[index].import_start == start &&
        bindings[index].import_end == end)
      return 1;
  return 0;
}

static ArchbirdStatus reject_other_references(
    AbActContext *context, const AbActDependencyRedirect *redirect,
    const AbString *definition_path,
    const AbActEcmascriptRedirectBinding *bindings, size_t binding_count) {
  const AbValue *references = field(redirect->map, "symbol_references");
  const AbValue *edges = field(redirect->map, "edges");
  size_t index;
  for (index = 0; references && references->kind == AB_VALUE_ARRAY &&
                  index < references->as.array.count;
       index++) {
    const AbValue *row = &references->as.array.items[index];
    const AbValue *source = field(row, "source");
    const AbValue *path = field(source, "path");
    const AbValue *candidates = field(row, "candidates");
    size_t candidate_index;
    if (!path || path->kind != AB_VALUE_STRING ||
        !binding_path_has(bindings, binding_count, &path->as.text))
      continue;
    for (candidate_index = 0;
         candidates && candidates->kind == AB_VALUE_ARRAY &&
         candidate_index < candidates->as.array.count;
         candidate_index++) {
      const AbValue *candidate = &candidates->as.array.items[candidate_index];
      const AbValue *candidate_path = field(candidate, "path");
      const AbValue *candidate_symbol = field(candidate, "symbol");
      if (candidate_path && candidate_path->kind == AB_VALUE_STRING &&
          candidate_symbol && candidate_symbol->kind == AB_VALUE_STRING &&
          ab_string_equal(&candidate_path->as.text, definition_path) &&
          ab_string_equal(&candidate_symbol->as.text, redirect->from_symbol))
        return reject(context,
                      "ECMAScript dependency has a non-call symbol reference");
    }
  }
  for (index = 0;
       edges && edges->kind == AB_VALUE_ARRAY && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *source = field(edge, "source");
    const AbValue *target = field(edge, "target");
    const AbValue *sites = field(edge, "sites");
    size_t site_index;
    if (!text_is(field(edge, "kind"), "semantic-reference") || !source ||
        source->kind != AB_VALUE_STRING || !target ||
        target->kind != AB_VALUE_STRING ||
        !ab_string_equal(&target->as.text, definition_path) ||
        !binding_path_has(bindings, binding_count, &source->as.text))
      continue;
    for (site_index = 0; sites && sites->kind == AB_VALUE_ARRAY &&
                         site_index < sites->as.array.count;
         site_index++) {
      const AbValue *site = &sites->as.array.items[site_index];
      const AbValue *path = field(site, "path");
      uint64_t start;
      uint64_t end;
      if (!path || path->kind != AB_VALUE_STRING ||
          !span(field(site, "span"), &start, &end) ||
          !occurrence_is_import(bindings, binding_count, &path->as.text, start,
                                end))
        return reject(context,
                      "ECMAScript dependency has an unhandled semantic "
                      "reference");
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_act_ecmascript_dependency_redirect(AbActContext *context,
                                      const AbActDependencyRedirect *redirect,
                                      const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  const AbValue *definition_file = NULL;
  const AbValue *definition_symbol;
  const AbValue *definition_path;
  AbActEcmascriptRedirectSite *sites = NULL;
  AbActEcmascriptRedirectBinding *bindings = NULL;
  size_t binding_count = redirect->relation_sites->as.array.count;
  size_t site_count = 0;
  size_t index;
  ArchbirdStatus status = ab_act_executor_begin(
      context, item_id, "archbird.native.ecmascript.redirect-dependency@1");
  if (status != ARCHBIRD_OK)
    return status;
  definition_symbol =
      unique_definition(redirect->map, redirect->to_symbol, &definition_file);
  definition_path = field(definition_file, "path");
  if (!definition_symbol || !definition_path ||
      definition_path->kind != AB_VALUE_STRING)
    return reject(
        context,
        "replacement has no unique exact ECMAScript function definition");
  if (ab_act_dependency_redirect_target_matches(redirect,
                                                &definition_path->as.text))
    return reject(context,
                  "replacement remains inside the forbidden dependency target");
  sites = (AbActEcmascriptRedirectSite *)ab_calloc(
      engine, AB_ACT_ECMASCRIPT_MAX_REDIRECT_SITES, sizeof(*sites));
  bindings = (AbActEcmascriptRedirectBinding *)ab_calloc(engine, binding_count,
                                                         sizeof(*bindings));
  if (!sites || !bindings)
    status = ARCHBIRD_OUT_OF_MEMORY;
  for (index = 0; status == ARCHBIRD_OK && index < binding_count; index++)
    status = collect_import(
        context, redirect, &redirect->relation_sites->as.array.items[index],
        &definition_path->as.text, sites, &site_count, &bindings[index]);
  if (status == ARCHBIRD_OK)
    status = collect_calls(context, redirect, bindings, binding_count, sites,
                           &site_count);
  if (status == ARCHBIRD_OK)
    status = reject_other_references(
        context, redirect, &definition_path->as.text, bindings, binding_count);
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
