#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SurfaceMakeProvider {
  const AbValue *path;
  AbString variable;
} SurfaceMakeProvider;

typedef struct SurfaceRewrite {
  const AbValue *finding;
  SurfaceMakeProvider provider;
  AbPlanSourceLock source;
  AbString expected_token;
  AbString replacement_token;
  size_t rename_index;
} SurfaceRewrite;

typedef struct SurfaceInsertion {
  const AbValue *key;
  const AbValue **findings;
  size_t finding_count;
  SurfaceMakeProvider provider;
  AbPlanSourceLock source;
  AbString token;
  AbString anchor_token;
  ArchbirdMakeVariableTokenPosition position;
} SurfaceInsertion;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  return ab_artifact_text_is(value, literal);
}

static int buffer_write(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static size_t common_prefix(const AbString *left, const AbString *right) {
  size_t common = left->length < right->length ? left->length : right->length;
  size_t index = 0;
  while (index < common && left->data[index] == right->data[index])
    index++;
  return index;
}

static int string_compare(const AbString *left, const AbString *right) {
  size_t common = left->length < right->length ? left->length : right->length;
  int compared = common ? memcmp(left->data, right->data, common) : 0;
  if (compared)
    return compared;
  return left->length < right->length   ? -1
         : left->length > right->length ? 1
                                        : 0;
}

static size_t length_distance(const AbString *left, const AbString *right) {
  return left->length > right->length ? left->length - right->length
                                      : right->length - left->length;
}

static int find_rename(const AbValue *renames, const AbString *old_name,
                       const AbValue **out_new_name, size_t *out_index) {
  size_t index;
  if (!renames || renames->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < renames->as.object.count; index++) {
    const AbObjectField *row = &renames->as.object.fields[index];
    if (ab_string_equal(&row->name, old_name)) {
      *out_new_name = &row->value;
      *out_index = index;
      return 1;
    }
  }
  return 0;
}

static const AbValue *find_named_row(const AbValue *rows,
                                     const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

static const AbValue *find_surface(const AbValue *map,
                                   const AbString *surface_name) {
  return find_named_row(field(map, "surfaces"), surface_name);
}

static int parse_make_provider(const AbValue *declaration,
                               SurfaceMakeProvider *out) {
  static const char prefix[] = "make-variable:";
  const AbValue *path = field(declaration, "path");
  const AbValue *source = field(declaration, "source");
  if (!path || path->kind != AB_VALUE_STRING || !source ||
      source->kind != AB_VALUE_STRING ||
      source->as.text.length <= sizeof(prefix) - 1 ||
      memcmp(source->as.text.data, prefix, sizeof(prefix) - 1) != 0)
    return 0;
  out->path = path;
  out->variable.data = source->as.text.data + sizeof(prefix) - 1;
  out->variable.length = source->as.text.length - (sizeof(prefix) - 1);
  return 1;
}

static int same_provider(const SurfaceMakeProvider *provider,
                         const AbValue *declaration) {
  SurfaceMakeProvider candidate;
  return parse_make_provider(declaration, &candidate) &&
         ab_value_equal(provider->path, candidate.path) &&
         ab_string_equal(&provider->variable, &candidate.variable);
}

static int only_make_provider(const AbValue *surface,
                              SurfaceMakeProvider *out) {
  const AbValue *providers = field(surface, "providers");
  return providers && providers->kind == AB_VALUE_ARRAY &&
         providers->as.array.count == 1 &&
         parse_make_provider(&providers->as.array.items[0], out);
}

static int row_has_provider(const AbValue *row,
                            const SurfaceMakeProvider *provider) {
  const AbValue *declarations = field(row, "declarations");
  size_t index;
  if (!declarations || declarations->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < declarations->as.array.count; index++)
    if (same_provider(provider, &declarations->as.array.items[index]))
      return 1;
  return 0;
}

static int target_is_resolved(const AbValue *row);

static int
provider_has_resolved_declaration(const AbValue *surface,
                                  const SurfaceMakeProvider *provider,
                                  const AbString *excluded_name) {
  const AbValue *names = field(surface, "names");
  size_t index;
  if (!names || names->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *name = field(row, "name");
    if (name && name->kind == AB_VALUE_STRING &&
        !ab_string_equal(&name->as.text, excluded_name) &&
        row_has_provider(row, provider) && target_is_resolved(row))
      return 1;
  }
  return 0;
}

static int target_is_resolved(const AbValue *row) {
  const AbValue *candidates = field(row, "candidates");
  return row && text_is(field(row, "declaration"), "declared") &&
         text_is(field(row, "resolution"), "unique") && candidates &&
         candidates->kind == AB_VALUE_ARRAY && candidates->as.array.count == 1;
}

static int target_is_implemented(const AbValue *row) {
  const AbValue *candidates = field(row, "candidates");
  const AbValue *uses = field(row, "uses");
  return row && text_is(field(row, "declaration"), "undeclared") &&
         text_is(field(row, "resolution"), "unique") && candidates &&
         candidates->kind == AB_VALUE_ARRAY &&
         candidates->as.array.count == 1 && uses &&
         uses->kind == AB_VALUE_ARRAY && uses->as.array.count;
}

static int old_is_inactive_make_declaration(const AbValue *row,
                                            SurfaceMakeProvider *out_provider) {
  const AbValue *candidates = field(row, "candidates");
  const AbValue *uses = field(row, "uses");
  const AbValue *declarations = field(row, "declarations");
  if (!row || !text_is(field(row, "declaration"), "declared") ||
      !text_is(field(row, "resolution"), "unresolved") || !candidates ||
      candidates->kind != AB_VALUE_ARRAY || candidates->as.array.count ||
      !uses || uses->kind != AB_VALUE_ARRAY || uses->as.array.count ||
      !declarations || declarations->kind != AB_VALUE_ARRAY ||
      declarations->as.array.count != 1)
    return 0;
  return parse_make_provider(&declarations->as.array.items[0], out_provider);
}

static ArchbirdStatus make_token(ArchbirdEngine *engine, const AbString *name,
                                 int underscored, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = underscored ? ab_buffer_literal(&buffer, "_") : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&buffer, (const uint8_t *)name->data, name->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus try_make_replacement(ArchbirdEngine *engine,
                                           const SurfaceMakeProvider *provider,
                                           const AbPlanSourceLock *source,
                                           const AbString *expected,
                                           const AbString *replacement,
                                           int *out_matched) {
  ArchbirdMakeVariableTokenEditOptions options;
  ArchbirdMakeVariableTokenEditResult result;
  AbBuffer ignored;
  ArchbirdStatus status;
  *out_matched = 0;
  archbird_make_variable_token_edit_options_init(&options);
  options.source_sha256 = source->sha256->as.text.data;
  options.source_sha256_length = source->sha256->as.text.length;
  options.variable = (const uint8_t *)provider->variable.data;
  options.variable_length = provider->variable.length;
  options.expected_token = (const uint8_t *)expected->data;
  options.expected_token_length = expected->length;
  options.replacement_token = (const uint8_t *)replacement->data;
  options.replacement_token_length = replacement->length;
  archbird_make_variable_token_edit_result_init(&result);
  ab_buffer_init(&ignored, engine);
  status = archbird_make_variable_token_edit(
      engine, source->source.bytes, source->source.byte_length, &options,
      &result, buffer_write, &ignored);
  ab_buffer_free(&ignored);
  if (status == ARCHBIRD_OK) {
    *out_matched = 1;
  } else if (status == ARCHBIRD_POLICY_REJECTED && result.matched_tokens == 0) {
    archbird_error_clear(engine);
    status = ARCHBIRD_OK;
  }
  return status;
}

static ArchbirdStatus
resolve_make_replacement(ArchbirdEngine *engine,
                         const SurfaceMakeProvider *provider,
                         const AbString *old_name, const AbString *new_name,
                         const AbPlanSourceLock *source, AbString *out_expected,
                         AbString *out_replacement, int *out_supported) {
  size_t form;
  size_t matches = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_supported = 0;
  for (form = 0; status == ARCHBIRD_OK && form < 2; form++) {
    AbString expected = {0};
    AbString replacement = {0};
    int matched = 0;
    status = make_token(engine, old_name, (int)form, &expected);
    if (status == ARCHBIRD_OK)
      status = make_token(engine, new_name, (int)form, &replacement);
    if (status == ARCHBIRD_OK)
      status = try_make_replacement(engine, provider, source, &expected,
                                    &replacement, &matched);
    if (status == ARCHBIRD_OK && matched) {
      if (matches) {
        status = archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "plan compilation: provider registration has multiple direct "
            "spellings for %.*s",
            (int)old_name->length, old_name->data);
      } else {
        *out_expected = expected;
        *out_replacement = replacement;
        memset(&expected, 0, sizeof(expected));
        memset(&replacement, 0, sizeof(replacement));
        matches++;
      }
    }
    ab_string_free(engine, &expected);
    ab_string_free(engine, &replacement);
  }
  if (status == ARCHBIRD_OK && matches == 1)
    *out_supported = 1;
  else if (status == ARCHBIRD_OK) {
    ab_string_free(engine, out_expected);
    ab_string_free(engine, out_replacement);
  }
  return status;
}

static ArchbirdStatus resolve_make_removal(ArchbirdEngine *engine,
                                           const SurfaceMakeProvider *provider,
                                           const AbString *old_name,
                                           const AbPlanSourceLock *source,
                                           AbString *out_expected,
                                           int *out_supported) {
  AbString empty = {0};
  size_t form;
  size_t matches = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_supported = 0;
  for (form = 0; status == ARCHBIRD_OK && form < 2; form++) {
    AbString expected = {0};
    int matched = 0;
    status = make_token(engine, old_name, (int)form, &expected);
    if (status == ARCHBIRD_OK)
      status = try_make_replacement(engine, provider, source, &expected, &empty,
                                    &matched);
    if (status == ARCHBIRD_OK && matched) {
      if (matches) {
        status = archbird_error_set(
            engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "plan compilation: provider registration has multiple direct "
            "spellings for %.*s",
            (int)old_name->length, old_name->data);
      } else {
        *out_expected = expected;
        memset(&expected, 0, sizeof(expected));
        matches++;
      }
    }
    ab_string_free(engine, &expected);
  }
  if (status == ARCHBIRD_OK && matches == 1)
    *out_supported = 1;
  else if (status == ARCHBIRD_OK)
    ab_string_free(engine, out_expected);
  return status;
}

static ArchbirdStatus
try_make_insertion(ArchbirdEngine *engine, const SurfaceMakeProvider *provider,
                   const AbPlanSourceLock *source, const AbString *token,
                   const AbString *anchor,
                   ArchbirdMakeVariableTokenPosition position,
                   int *out_matched) {
  ArchbirdMakeVariableTokenInsertOptions options;
  ArchbirdMakeVariableTokenInsertResult result;
  AbBuffer ignored;
  ArchbirdStatus status;
  *out_matched = 0;
  archbird_make_variable_token_insert_options_init(&options);
  options.source_sha256 = source->sha256->as.text.data;
  options.source_sha256_length = source->sha256->as.text.length;
  options.variable = (const uint8_t *)provider->variable.data;
  options.variable_length = provider->variable.length;
  options.token = (const uint8_t *)token->data;
  options.token_length = token->length;
  options.anchor_token = (const uint8_t *)anchor->data;
  options.anchor_token_length = anchor->length;
  options.position = position;
  archbird_make_variable_token_insert_result_init(&result);
  ab_buffer_init(&ignored, engine);
  status = archbird_make_variable_token_insert(
      engine, source->source.bytes, source->source.byte_length, &options,
      &result, buffer_write, &ignored);
  ab_buffer_free(&ignored);
  if (status == ARCHBIRD_OK) {
    *out_matched = 1;
  } else if (status == ARCHBIRD_POLICY_REJECTED && result.matched_tokens == 0) {
    archbird_error_clear(engine);
    status = ARCHBIRD_OK;
  }
  return status;
}

static int better_anchor(const AbString *target, const AbString *candidate,
                         const AbString *candidate_token, size_t best_prefix,
                         size_t best_distance, const AbString *best_name,
                         const AbString *best_token, int has_best) {
  size_t prefix = common_prefix(target, candidate);
  size_t distance = length_distance(target, candidate);
  int compared;
  if (!has_best || prefix != best_prefix)
    return !has_best || prefix > best_prefix;
  if (distance != best_distance)
    return distance < best_distance;
  compared = string_compare(candidate, best_name);
  if (compared)
    return compared < 0;
  return string_compare(candidate_token, best_token) < 0;
}

static ArchbirdStatus resolve_make_insertion(
    ArchbirdEngine *engine, const SurfaceMakeProvider *provider,
    const AbString *name, const AbValue *surface,
    const AbPlanSourceLock *source, AbString *out_token, AbString *out_anchor,
    ArchbirdMakeVariableTokenPosition *out_position, int *out_supported) {
  const AbValue *names = field(surface, "names");
  const AbString *best_name = NULL;
  size_t best_prefix = 0;
  size_t best_distance = 0;
  size_t index;
  int has_best = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_supported = 0;
  if (!names || names->kind != AB_VALUE_ARRAY)
    return ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < names->as.array.count;
       index++) {
    const AbValue *row = &names->as.array.items[index];
    const AbValue *candidate = field(row, "name");
    size_t form;
    if (!candidate || candidate->kind != AB_VALUE_STRING ||
        !row_has_provider(row, provider) ||
        ab_string_equal(&candidate->as.text, name))
      continue;
    for (form = 0; status == ARCHBIRD_OK && form < 2; form++) {
      AbString token = {0};
      AbString anchor = {0};
      ArchbirdMakeVariableTokenPosition position =
          string_compare(name, &candidate->as.text) < 0
              ? ARCHBIRD_MAKE_TOKEN_BEFORE
              : ARCHBIRD_MAKE_TOKEN_AFTER;
      int matched = 0;
      status = make_token(engine, name, (int)form, &token);
      if (status == ARCHBIRD_OK)
        status = make_token(engine, &candidate->as.text, (int)form, &anchor);
      if (status == ARCHBIRD_OK)
        status = try_make_insertion(engine, provider, source, &token, &anchor,
                                    position, &matched);
      if (status == ARCHBIRD_OK && matched &&
          better_anchor(name, &candidate->as.text, &anchor, best_prefix,
                        best_distance, best_name, out_anchor, has_best)) {
        ab_string_free(engine, out_token);
        ab_string_free(engine, out_anchor);
        *out_token = token;
        *out_anchor = anchor;
        *out_position = position;
        best_name = &candidate->as.text;
        best_prefix = common_prefix(name, best_name);
        best_distance = length_distance(name, best_name);
        has_best = 1;
        memset(&token, 0, sizeof(token));
        memset(&anchor, 0, sizeof(anchor));
      }
      ab_string_free(engine, &token);
      ab_string_free(engine, &anchor);
    }
  }
  if (status == ARCHBIRD_OK && has_best)
    *out_supported = 1;
  else if (status != ARCHBIRD_OK) {
    ab_string_free(engine, out_token);
    ab_string_free(engine, out_anchor);
  }
  return status;
}

static ArchbirdStatus
analyze_rewrite(ArchbirdEngine *engine, const ArchbirdProject *project,
                const AbValue *map, const AbValue *definition,
                const AbValue *renames, const AbPlanFindingGroup *group,
                SurfaceRewrite *out, int *out_supported) {
  const AbValue *finding = group->representative;
  const AbValue *old_value = field(finding, "key");
  const AbValue *new_value = NULL;
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  const AbValue *old_row;
  const AbValue *new_row;
  size_t rename_index = 0;
  ArchbirdStatus status;
  *out_supported = 0;
  memset(out, 0, sizeof(*out));
  if (!ab_plan_finding_current(finding) ||
      !text_is(field(finding, "comparison"), "unresolved") || !old_value ||
      old_value->kind != AB_VALUE_STRING || !surface_value ||
      surface_value->kind != AB_VALUE_STRING ||
      !find_rename(renames, &old_value->as.text, &new_value, &rename_index) ||
      !new_value || new_value->kind != AB_VALUE_STRING)
    return ARCHBIRD_OK;
  surface = find_surface(map, &surface_value->as.text);
  old_row = find_named_row(field(surface, "names"), &old_value->as.text);
  new_row = find_named_row(field(surface, "names"), &new_value->as.text);
  if (!old_is_inactive_make_declaration(old_row, &out->provider) ||
      !target_is_resolved(new_row))
    return ARCHBIRD_OK;
  status = ab_plan_source_lock(engine, project, map,
                               &out->provider.path->as.text, &out->source);
  if (status == ARCHBIRD_OK)
    status = resolve_make_replacement(engine, &out->provider,
                                      &old_value->as.text, &new_value->as.text,
                                      &out->source, &out->expected_token,
                                      &out->replacement_token, out_supported);
  if (status == ARCHBIRD_OK && *out_supported) {
    out->finding = finding;
    out->rename_index = rename_index;
  }
  return status;
}

static ArchbirdStatus analyze_removal(ArchbirdEngine *engine,
                                      const ArchbirdProject *project,
                                      const AbValue *map,
                                      const AbValue *definition,
                                      const AbPlanFindingGroup *group,
                                      SurfaceRewrite *out, int *out_supported) {
  const AbValue *finding = group->representative;
  const AbValue *old_value = field(finding, "key");
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  const AbValue *old_row;
  ArchbirdStatus status;
  *out_supported = 0;
  memset(out, 0, sizeof(*out));
  if (!ab_plan_finding_current(finding) ||
      !text_is(field(finding, "comparison"), "unresolved") || !old_value ||
      old_value->kind != AB_VALUE_STRING || !surface_value ||
      surface_value->kind != AB_VALUE_STRING)
    return ARCHBIRD_OK;
  surface = find_surface(map, &surface_value->as.text);
  old_row = find_named_row(field(surface, "names"), &old_value->as.text);
  if (!old_is_inactive_make_declaration(old_row, &out->provider) ||
      !provider_has_resolved_declaration(surface, &out->provider,
                                         &old_value->as.text))
    return ARCHBIRD_OK;
  status = ab_plan_source_lock(engine, project, map,
                               &out->provider.path->as.text, &out->source);
  if (status == ARCHBIRD_OK)
    status =
        resolve_make_removal(engine, &out->provider, &old_value->as.text,
                             &out->source, &out->expected_token, out_supported);
  if (status == ARCHBIRD_OK && *out_supported)
    out->finding = finding;
  return status;
}

static void rewrites_free(ArchbirdEngine *engine, SurfaceRewrite *rewrites,
                          size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_string_free(engine, &rewrites[index].expected_token);
    ab_string_free(engine, &rewrites[index].replacement_token);
  }
  ab_free(engine, rewrites);
}

static void insertions_free(ArchbirdEngine *engine,
                            SurfaceInsertion *insertions, size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    ab_free(engine, insertions[index].findings);
    ab_string_free(engine, &insertions[index].token);
    ab_string_free(engine, &insertions[index].anchor_token);
  }
  ab_free(engine, insertions);
}

static ArchbirdStatus render_rewrite_operation(ArchbirdEngine *engine,
                                               const SurfaceRewrite *rewrite,
                                               AbBuffer *out) {
  ArchbirdStatus status;
  ab_buffer_init(out, engine);
  status = ab_buffer_literal(out, "{\"action\":\"edit_make_variable_token\","
                                  "\"expected_token\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, rewrite->expected_token.data,
                                   rewrite->expected_token.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, rewrite->provider.path);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"replacement_token\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, rewrite->replacement_token.data,
                                   rewrite->replacement_token.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"source_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, rewrite->source.sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"variable\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, rewrite->provider.variable.data,
                                   rewrite->provider.variable.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static ArchbirdStatus
render_insertion_operation(ArchbirdEngine *engine,
                           const SurfaceInsertion *insertion, AbBuffer *out) {
  ArchbirdStatus status;
  ab_buffer_init(out, engine);
  status = ab_buffer_literal(out, "{\"action\":\"insert_make_variable_token\","
                                  "\"anchor_token\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, insertion->anchor_token.data,
                                   insertion->anchor_token.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, insertion->provider.path);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        out, insertion->position == ARCHBIRD_MAKE_TOKEN_BEFORE
                 ? ",\"position\":\"before\",\"source_sha256\":"
                 : ",\"position\":\"after\",\"source_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, insertion->source.sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"token\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, insertion->token.data,
                                   insertion->token.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"variable\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, insertion->provider.variable.data,
                                   insertion->provider.variable.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static int insertion_comparison(const AbValue *finding) {
  const AbValue *comparison = field(finding, "comparison");
  return text_is(comparison, "missing") || text_is(comparison, "unregistered");
}

static ArchbirdStatus analyze_insertions(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbValue *definition, const AbPlanFindingGroups *groups,
    SurfaceInsertion *insertions, size_t *out_count, int *out_supported) {
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  SurfaceMakeProvider provider;
  uint8_t *consumed = NULL;
  size_t insertion_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_count = 0;
  *out_supported = 0;
  if (!surface_value || surface_value->kind != AB_VALUE_STRING ||
      !groups->count)
    return ARCHBIRD_OK;
  surface = find_surface(map, &surface_value->as.text);
  if (!only_make_provider(surface, &provider))
    return ARCHBIRD_OK;
  consumed = (uint8_t *)ab_calloc(engine, groups->count, sizeof(*consumed));
  if (!consumed)
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "plan compilation: out of memory grouping provider-surface issues");
  for (index = 0; status == ARCHBIRD_OK && index < groups->count; index++) {
    SurfaceInsertion *insertion;
    const AbValue *finding;
    const AbValue *key;
    const AbValue *target;
    size_t group_index;
    size_t finding_count = 0;
    size_t finding_index = 0;
    int has_missing = 0;
    int supported = 0;
    if (consumed[index])
      continue;
    finding = groups->groups[index].representative;
    key = field(finding, "key");
    if (!ab_plan_finding_current(finding) || !insertion_comparison(finding) ||
        !key || key->kind != AB_VALUE_STRING)
      break;
    for (group_index = index; group_index < groups->count; group_index++) {
      const AbPlanFindingGroup *group = &groups->groups[group_index];
      const AbValue *candidate = group->representative;
      const AbValue *candidate_key = field(candidate, "key");
      if (!candidate_key || candidate_key->kind != AB_VALUE_STRING ||
          !ab_string_equal(&candidate_key->as.text, &key->as.text))
        continue;
      if (!ab_plan_finding_current(candidate) ||
          !insertion_comparison(candidate))
        break;
      if (text_is(field(candidate, "comparison"), "missing"))
        has_missing = 1;
      finding_count += group->count;
    }
    if (group_index != groups->count || !has_missing)
      break;
    insertion = &insertions[insertion_count];
    insertion->findings = (const AbValue **)ab_calloc(
        engine, finding_count, sizeof(*insertion->findings));
    if (!insertion->findings) {
      status = archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "plan compilation: out of memory retaining provider-surface issues");
      break;
    }
    insertion->key = key;
    insertion->finding_count = finding_count;
    insertion->provider = provider;
    for (group_index = index; group_index < groups->count; group_index++) {
      const AbPlanFindingGroup *group = &groups->groups[group_index];
      const AbValue *candidate_key = field(group->representative, "key");
      size_t row_index;
      if (!candidate_key || candidate_key->kind != AB_VALUE_STRING ||
          !ab_string_equal(&candidate_key->as.text, &key->as.text))
        continue;
      consumed[group_index] = 1;
      for (row_index = 0; row_index < group->count; row_index++)
        insertion->findings[finding_index++] = group->rows[row_index];
    }
    target = find_named_row(field(surface, "names"), &key->as.text);
    if (!target_is_implemented(target))
      break;
    status = ab_plan_source_lock(engine, project, map, &provider.path->as.text,
                                 &insertion->source);
    if (status == ARCHBIRD_OK)
      status = resolve_make_insertion(engine, &provider, &key->as.text, surface,
                                      &insertion->source, &insertion->token,
                                      &insertion->anchor_token,
                                      &insertion->position, &supported);
    if (status != ARCHBIRD_OK || !supported)
      break;
    insertion_count++;
  }
  if (status == ARCHBIRD_OK && index == groups->count) {
    *out_count = insertion_count;
    *out_supported = insertion_count != 0;
  }
  ab_free(engine, consumed);
  return status;
}

static ArchbirdStatus append_insertions(ArchbirdEngine *engine,
                                        AbPlanItemBuilder *builder,
                                        const AbValue *constraint,
                                        SurfaceInsertion *insertions,
                                        size_t count) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    SurfaceInsertion *insertion = &insertions[index];
    AbBuffer operation;
    char statement[1024];
    AbPlanItemSpec spec;
    int length = snprintf(statement, sizeof(statement),
                          "Register required provider capability %.*s in %.*s.",
                          (int)insertion->key->as.text.length,
                          insertion->key->as.text.data,
                          (int)insertion->provider.path->as.text.length,
                          insertion->provider.path->as.text.data);
    if (length < 0 || (size_t)length >= sizeof(statement))
      return archbird_error_set(
          engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
          "plan compilation: provider-surface statement is too long");
    status = render_insertion_operation(engine, insertion, &operation);
    if (status == ARCHBIRD_OK) {
      memset(&spec, 0, sizeof(spec));
      spec.constraint = constraint;
      spec.findings = insertion->findings;
      spec.finding_count = insertion->finding_count;
      spec.statement = statement;
      spec.provenance = "derived";
      spec.operation = &operation;
      spec.executable = 1;
      status = ab_plan_item_builder_append(builder, &spec);
    }
    ab_buffer_free(&operation);
  }
  return status;
}

static ArchbirdStatus append_removals(ArchbirdEngine *engine,
                                      AbPlanItemBuilder *builder,
                                      const AbValue *constraint,
                                      const AbPlanFindingGroups *groups,
                                      SurfaceRewrite *removals) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < groups->count; index++) {
    const AbValue *key = field(removals[index].finding, "key");
    AbBuffer operation;
    char statement[1024];
    AbPlanItemSpec spec;
    int length = snprintf(statement, sizeof(statement),
                          "Remove stale provider registration %.*s from %.*s.",
                          (int)key->as.text.length, key->as.text.data,
                          (int)removals[index].provider.path->as.text.length,
                          removals[index].provider.path->as.text.data);
    if (length < 0 || (size_t)length >= sizeof(statement))
      return archbird_error_set(
          engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
          "plan compilation: provider-surface statement is too long");
    status = render_rewrite_operation(engine, &removals[index], &operation);
    if (status == ARCHBIRD_OK) {
      memset(&spec, 0, sizeof(spec));
      spec.constraint = constraint;
      spec.findings = groups->groups[index].rows;
      spec.finding_count = groups->groups[index].count;
      spec.statement = statement;
      spec.provenance = "derived";
      spec.operation = &operation;
      spec.executable = 1;
      status = ab_plan_item_builder_append(builder, &spec);
    }
    ab_buffer_free(&operation);
  }
  return status;
}

ArchbirdStatus ab_plan_compile_surface_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    AbPlanItemBuilder *builder, const AbValue *constraint,
    const AbValue *definition, const AbValue *renames, uint8_t *rename_used,
    int *out_handled) {
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  SurfaceRewrite *rewrites = NULL;
  size_t index;
  ArchbirdStatus status;
  *out_handled = 0;
  if (!definition ||
      !text_is(field(definition, "select"), "provider_surface") || !findings ||
      findings->kind != AB_VALUE_ARRAY || !findings->as.array.count)
    return ARCHBIRD_OK;
  status = ab_plan_finding_groups_collect(engine, findings, &groups);
  if (status != ARCHBIRD_OK)
    return status;
  if (!renames ||
      (renames->kind == AB_VALUE_OBJECT && !renames->as.object.count)) {
    SurfaceInsertion *insertions = (SurfaceInsertion *)ab_calloc(
        engine, groups.count, sizeof(*insertions));
    size_t insertion_count = 0;
    int supported = 0;
    if (!insertions) {
      ab_plan_finding_groups_free(engine, &groups);
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "plan compilation: out of memory deriving provider-surface "
          "insertions");
    }
    status = analyze_insertions(engine, project, map, definition, &groups,
                                insertions, &insertion_count, &supported);
    if (status == ARCHBIRD_OK && supported) {
      status = append_insertions(engine, builder, constraint, insertions,
                                 insertion_count);
      if (status == ARCHBIRD_OK)
        *out_handled = 1;
    }
    insertions_free(engine, insertions, groups.count);
    if (status == ARCHBIRD_OK && !supported) {
      SurfaceRewrite *removals =
          (SurfaceRewrite *)ab_calloc(engine, groups.count, sizeof(*removals));
      if (!removals) {
        ab_plan_finding_groups_free(engine, &groups);
        return archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "plan compilation: out of memory deriving provider-surface "
            "removals");
      }
      for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
        int removal_supported = 0;
        status = analyze_removal(engine, project, map, definition,
                                 &groups.groups[index], &removals[index],
                                 &removal_supported);
        if (status == ARCHBIRD_OK && !removal_supported)
          break;
      }
      if (status == ARCHBIRD_OK && index == groups.count) {
        status =
            append_removals(engine, builder, constraint, &groups, removals);
        if (status == ARCHBIRD_OK)
          *out_handled = 1;
      }
      rewrites_free(engine, removals, groups.count);
    }
    ab_plan_finding_groups_free(engine, &groups);
    return status;
  }
  rewrites =
      (SurfaceRewrite *)ab_calloc(engine, groups.count, sizeof(*rewrites));
  if (!rewrites) {
    ab_plan_finding_groups_free(engine, &groups);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "plan compilation: out of memory deriving "
                              "provider-surface rewrites");
  }
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
    int supported = 0;
    status =
        analyze_rewrite(engine, project, map, definition, renames,
                        &groups.groups[index], &rewrites[index], &supported);
    if (status == ARCHBIRD_OK && !supported)
      break;
  }
  if (status == ARCHBIRD_OK && index == groups.count) {
    for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
      const AbValue *key = field(rewrites[index].finding, "key");
      const AbValue *new_name =
          &renames->as.object.fields[rewrites[index].rename_index].value;
      const AbValue *rows[] = {rewrites[index].finding};
      AbBuffer operation;
      char statement[1024];
      AbPlanItemSpec spec;
      int length = snprintf(
          statement, sizeof(statement),
          "Replace stale provider registration %.*s with %.*s in %.*s.",
          (int)key->as.text.length, key->as.text.data,
          (int)new_name->as.text.length, new_name->as.text.data,
          (int)rewrites[index].provider.path->as.text.length,
          rewrites[index].provider.path->as.text.data);
      if (length < 0 || (size_t)length >= sizeof(statement)) {
        status = archbird_error_set(
            engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
            "plan compilation: provider-surface statement is too long");
        break;
      }
      status = render_rewrite_operation(engine, &rewrites[index], &operation);
      if (status == ARCHBIRD_OK) {
        memset(&spec, 0, sizeof(spec));
        spec.constraint = constraint;
        spec.findings = rows;
        spec.finding_count = 1;
        spec.statement = statement;
        spec.provenance = "asserted";
        spec.operation = &operation;
        spec.executable = 1;
        status = ab_plan_item_builder_append(builder, &spec);
      }
      ab_buffer_free(&operation);
      if (status == ARCHBIRD_OK)
        rename_used[rewrites[index].rename_index] = 1;
    }
    if (status == ARCHBIRD_OK)
      *out_handled = 1;
  }
  rewrites_free(engine, rewrites, groups.count);
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
