#include "act/c/dependency_redirect.h"

#include "base/artifact_validation.h"

#include <string.h>

#define AB_ACT_C_MAX_REDIRECT_SITES 4096u

typedef struct AbActCRedirectSite {
  const AbString *path;
  uint64_t start;
  uint64_t end;
  const AbString *expected;
  const AbString *replacement;
} AbActCRedirectSite;

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
                            "act C dependency redirect: %s", message);
}

static const AbValue *unique_c_definition(const AbValue *map,
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
    if (!text_is(field(file, "language"), "c") || !symbols ||
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

static int candidate_targets(const AbValue *candidate, const AbString *path,
                             const AbString *symbol) {
  const AbValue *candidate_path = field(candidate, "path");
  const AbValue *candidate_symbol = field(candidate, "symbol");
  return candidate_path && candidate_path->kind == AB_VALUE_STRING &&
         candidate_symbol && candidate_symbol->kind == AB_VALUE_STRING &&
         ab_string_equal(&candidate_path->as.text, path) &&
         ab_string_equal(&candidate_symbol->as.text, symbol);
}

static const AbValue *unique_declaration_file(const AbValue *map,
                                              const AbString *symbol,
                                              const AbString *definition_path) {
  const AbValue *references = field(map, "symbol_references");
  const AbValue *match = NULL;
  size_t index;
  for (index = 0; references && references->kind == AB_VALUE_ARRAY &&
                  index < references->as.array.count;
       index++) {
    const AbValue *row = &references->as.array.items[index];
    const AbValue *name = field(row, "name");
    const AbValue *source = field(row, "source");
    const AbValue *path = field(source, "path");
    const AbValue *candidates = field(row, "candidates");
    if (!name || name->kind != AB_VALUE_STRING ||
        !ab_string_equal(&name->as.text, symbol) ||
        !text_is(field(row, "relation"), "declaration-definition") ||
        !text_is(field(row, "resolution"), "unique") || !path ||
        path->kind != AB_VALUE_STRING || !candidates ||
        candidates->kind != AB_VALUE_ARRAY || candidates->as.array.count != 1 ||
        !candidate_targets(&candidates->as.array.items[0], definition_path,
                           symbol))
      continue;
    if (match && !ab_value_equal(match, path))
      return NULL;
    match = path;
  }
  return match;
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

static const AbString *canonical_include(const AbValue *map,
                                         const AbString *header_path) {
  const AbValue *edges = field(map, "edges");
  const AbString *include = NULL;
  size_t index;
  for (index = 0;
       edges && edges->kind == AB_VALUE_ARRAY && index < edges->as.array.count;
       index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbValue *target = field(edge, "target");
    if (!text_is(field(edge, "kind"), "import") || !target ||
        target->kind != AB_VALUE_STRING ||
        !ab_string_equal(&target->as.text, header_path))
      continue;
    if (!string_array_unique_value(field(edge, "names"), &include))
      return NULL;
  }
  return include;
}

static int site_add(AbActContext *context, AbActCRedirectSite *sites,
                    size_t *count, const AbString *path, uint64_t start,
                    uint64_t end, const AbString *expected,
                    const AbString *replacement) {
  size_t index;
  if (start >= end || expected->length != end - start)
    return 0;
  for (index = 0; index < *count; index++) {
    if (!ab_string_equal(sites[index].path, path) ||
        sites[index].start != start || sites[index].end != end)
      continue;
    return ab_string_equal(sites[index].expected, expected) &&
           ab_string_equal(sites[index].replacement, replacement);
  }
  if (*count >= AB_ACT_C_MAX_REDIRECT_SITES) {
    (void)reject(context, "redirect contains too many exact source sites");
    return 0;
  }
  sites[*count] = (AbActCRedirectSite){path, start, end, expected, replacement};
  (*count)++;
  return 1;
}

static ArchbirdStatus collect_include_sites(AbActContext *context,
                                            const AbValue *relation_sites,
                                            const AbString *replacement_include,
                                            AbActCRedirectSite *sites,
                                            size_t *site_count) {
  size_t index;
  for (index = 0; relation_sites && relation_sites->kind == AB_VALUE_ARRAY &&
                  index < relation_sites->as.array.count;
       index++) {
    const AbValue *site = &relation_sites->as.array.items[index];
    const AbValue *path = field(site, "path");
    const AbValue *name = field(site, "name");
    const AbValue *span = field(site, "span");
    uint64_t start;
    uint64_t end;
    if (!path || path->kind != AB_VALUE_STRING || !name ||
        name->kind != AB_VALUE_STRING || !span ||
        span->kind != AB_VALUE_OBJECT ||
        !ab_artifact_safe_integer(field(span, "start"), &start) ||
        !ab_artifact_safe_integer(field(span, "end"), &end) ||
        !site_add(context, sites, site_count, &path->as.text, start, end,
                  &name->as.text, replacement_include))
      return reject(context,
                    "edge projection has an invalid or conflicting include "
                    "site");
  }
  return *site_count
             ? ARCHBIRD_OK
             : reject(context, "edge projection has no exact include sites");
}

static ArchbirdStatus
collect_call_sites(AbActContext *context,
                   const AbActDependencyRedirect *redirect,
                   AbActCRedirectSite *sites, size_t *site_count) {
  const AbValue *calls = field(redirect->map, "symbol_calls");
  size_t call_count = 0;
  size_t source_count = 0;
  size_t relation_index;
  for (relation_index = 0;
       redirect->relation_sites &&
       redirect->relation_sites->kind == AB_VALUE_ARRAY &&
       relation_index < redirect->relation_sites->as.array.count;
       relation_index++) {
    const AbValue *path = field(
        &redirect->relation_sites->as.array.items[relation_index], "path");
    size_t row_index;
    int source_has_call = 0;
    if (!path || path->kind != AB_VALUE_STRING)
      return reject(context, "edge projection has an invalid source path");
    for (row_index = 0; calls && calls->kind == AB_VALUE_ARRAY &&
                        row_index < calls->as.array.count;
         row_index++) {
      const AbValue *row = &calls->as.array.items[row_index];
      const AbValue *name = field(row, "name");
      const AbValue *source = field(row, "source");
      const AbValue *source_path = field(source, "path");
      const AbValue *candidates = field(row, "candidates");
      const AbValue *evidence = field(row, "evidence");
      size_t evidence_index;
      if (!name || name->kind != AB_VALUE_STRING ||
          !ab_string_equal(&name->as.text, redirect->from_symbol) ||
          !source_path || source_path->kind != AB_VALUE_STRING ||
          !ab_string_equal(&source_path->as.text, &path->as.text))
        continue;
      if (!candidates || candidates->kind != AB_VALUE_ARRAY ||
          candidates->as.array.count != 1) {
        return reject(
            context, "source call does not have one bounded dependency target");
      }
      {
        const AbValue *candidate = &candidates->as.array.items[0];
        const AbValue *candidate_path = field(candidate, "path");
        if (!candidate_path || candidate_path->kind != AB_VALUE_STRING ||
            !ab_act_dependency_redirect_target_matches(
                redirect, &candidate_path->as.text))
          return reject(context,
                        "source call does not target the forbidden relation");
      }
      if (!evidence || evidence->kind != AB_VALUE_ARRAY ||
          !evidence->as.array.count)
        return reject(context, "source call has no exact evidence spans");
      for (evidence_index = 0; evidence_index < evidence->as.array.count;
           evidence_index++) {
        const AbValue *row = &evidence->as.array.items[evidence_index];
        const AbValue *span = field(row, "span");
        uint64_t start;
        uint64_t end;
        if (!span || span->kind != AB_VALUE_OBJECT ||
            !ab_artifact_safe_integer(field(span, "start"), &start) ||
            !ab_artifact_safe_integer(field(span, "end"), &end) ||
            !site_add(context, sites, site_count, &path->as.text, start, end,
                      redirect->from_symbol, redirect->to_symbol))
          return reject(context,
                        "source call has an invalid or conflicting exact span");
        call_count++;
      }
      source_has_call = 1;
    }
    if (!source_has_call)
      return reject(
          context, "an inducing dependency source has no matching symbol call");
    source_count++;
  }
  if (!source_count || !call_count)
    return reject(context, "dependency redirect has no exact call sites");
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_act_c_dependency_redirect(AbActContext *context,
                             const AbActDependencyRedirect *redirect,
                             const AbString *item_id) {
  ArchbirdEngine *engine = ab_act_executor_engine(context);
  AbActCRedirectSite *sites = NULL;
  const AbValue *definition_file = NULL;
  const AbValue *definition_symbol = NULL;
  const AbValue *definition_path;
  const AbValue *declaration_path;
  const AbString *include;
  size_t site_count = 0;
  size_t index;
  ArchbirdStatus status = ab_act_executor_begin(
      context, item_id, "archbird.native.c.redirect-dependency@1");
  if (status == ARCHBIRD_OK) {
    definition_symbol = unique_c_definition(redirect->map, redirect->to_symbol,
                                            &definition_file);
    definition_path = field(definition_file, "path");
    if (!definition_symbol || !definition_path ||
        definition_path->kind != AB_VALUE_STRING)
      status = reject(context,
                      "replacement has no unique exact C function definition");
  } else {
    definition_path = NULL;
  }
  if (status == ARCHBIRD_OK && ab_act_dependency_redirect_target_matches(
                                   redirect, &definition_path->as.text))
    status =
        reject(context, "replacement remains inside the forbidden dependency "
                        "target");
  declaration_path =
      status == ARCHBIRD_OK
          ? unique_declaration_file(redirect->map, redirect->to_symbol,
                                    &definition_path->as.text)
          : NULL;
  if (status == ARCHBIRD_OK && !declaration_path)
    status =
        reject(context, "replacement has no unique exact C declaration header");
  include = status == ARCHBIRD_OK
                ? canonical_include(redirect->map, &declaration_path->as.text)
                : NULL;
  if (status == ARCHBIRD_OK && !include)
    status = reject(
        context,
        "replacement declaration has no unique observed include spelling");
  if (status == ARCHBIRD_OK) {
    sites = (AbActCRedirectSite *)ab_calloc(engine, AB_ACT_C_MAX_REDIRECT_SITES,
                                            sizeof(*sites));
    if (!sites)
      status = ARCHBIRD_OUT_OF_MEMORY;
  }
  if (status == ARCHBIRD_OK)
    status = collect_include_sites(context, redirect->relation_sites, include,
                                   sites, &site_count);
  if (status == ARCHBIRD_OK)
    status = collect_call_sites(context, redirect, sites, &site_count);
  for (index = 0; status == ARCHBIRD_OK && index < site_count; index++)
    status = ab_act_executor_replace_exact(
        context, item_id, sites[index].path, (size_t)sites[index].start,
        (size_t)sites[index].end, (const uint8_t *)sites[index].expected->data,
        sites[index].expected->length,
        (const uint8_t *)sites[index].replacement->data,
        sites[index].replacement->length);
  ab_free(engine, sites);
  return status;
}
