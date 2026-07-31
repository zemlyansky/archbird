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
  const AbValue *replacement_name;
  const AbValue *surface_name;
  SurfaceMakeProvider provider;
  size_t rename_index;
  int asserted;
  int remove_only;
} SurfaceRewrite;

typedef struct SurfaceInsertion {
  const AbValue *key;
  const AbValue **findings;
  size_t finding_count;
  const AbValue *surface_name;
  SurfaceMakeProvider provider;
} SurfaceInsertion;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  return ab_artifact_text_is(value, literal);
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

static int identifier_byte(uint8_t byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
         (byte >= '0' && byte <= '9') || byte == '_';
}

static int portable_identifier(const AbString *value) {
  size_t index;
  if (!value || !value->length ||
      !((value->data[0] >= 'A' && value->data[0] <= 'Z') ||
        (value->data[0] >= 'a' && value->data[0] <= 'z') ||
        value->data[0] == '_'))
    return 0;
  for (index = 1; index < value->length; index++)
    if (!identifier_byte((uint8_t)value->data[index]))
      return 0;
  return 1;
}

static int renamed_text_equal(const AbString *before, const AbString *old_name,
                              const AbString *new_name,
                              const AbString *current) {
  size_t before_offset = 0;
  size_t current_offset = 0;
  size_t replaced = 0;
  while (before_offset < before->length) {
    size_t candidate = before_offset;
    while (candidate + old_name->length <= before->length) {
      int left_boundary =
          candidate == 0 ||
          !identifier_byte((uint8_t)before->data[candidate - 1]);
      int right_boundary =
          candidate + old_name->length == before->length ||
          !identifier_byte((uint8_t)before->data[candidate + old_name->length]);
      if (left_boundary && right_boundary &&
          memcmp(before->data + candidate, old_name->data, old_name->length) ==
              0)
        break;
      candidate++;
    }
    if (candidate + old_name->length > before->length)
      break;
    if (candidate - before_offset > current->length - current_offset ||
        memcmp(before->data + before_offset, current->data + current_offset,
               candidate - before_offset) != 0)
      return 0;
    current_offset += candidate - before_offset;
    if (new_name->length > current->length - current_offset ||
        memcmp(current->data + current_offset, new_name->data,
               new_name->length) != 0)
      return 0;
    current_offset += new_name->length;
    before_offset = candidate + old_name->length;
    replaced++;
  }
  return replaced &&
         before->length - before_offset == current->length - current_offset &&
         memcmp(before->data + before_offset, current->data + current_offset,
                before->length - before_offset) == 0;
}

static int renamed_text_arrays_equal(const AbValue *before,
                                     const AbValue *current,
                                     const AbString *old_name,
                                     const AbString *new_name) {
  size_t index;
  if (!before || !current || before->kind != AB_VALUE_ARRAY ||
      current->kind != AB_VALUE_ARRAY || !before->as.array.count ||
      before->as.array.count != current->as.array.count)
    return 0;
  for (index = 0; index < before->as.array.count; index++) {
    const AbValue *left = &before->as.array.items[index];
    const AbValue *right = &current->as.array.items[index];
    if (left->kind != AB_VALUE_STRING || right->kind != AB_VALUE_STRING ||
        !renamed_text_equal(&left->as.text, old_name, new_name,
                            &right->as.text))
      return 0;
  }
  return 1;
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

static ArchbirdStatus analyze_rewrite(const AbValue *map,
                                      const AbValue *definition,
                                      const AbValue *renames,
                                      const AbPlanFindingGroup *group,
                                      SurfaceRewrite *out, int *out_supported) {
  const AbValue *finding = group->representative;
  const AbValue *old_value = field(finding, "key");
  const AbValue *new_value = NULL;
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  const AbValue *old_row;
  const AbValue *new_row;
  size_t rename_index = 0;
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
  out->finding = finding;
  out->replacement_name = new_value;
  out->surface_name = surface_value;
  out->rename_index = rename_index;
  out->asserted = 1;
  out->remove_only = row_has_provider(new_row, &out->provider);
  *out_supported = 1;
  return ARCHBIRD_OK;
}

static int observed_target_matches(const AbValue *before_old,
                                   const AbValue *current_target,
                                   const SurfaceMakeProvider *provider,
                                   const AbString *old_name,
                                   const AbString *new_name) {
  return target_is_resolved(current_target) &&
         !row_has_provider(current_target, provider) &&
         ab_value_equal(field(before_old, "candidates"),
                        field(current_target, "candidates")) &&
         ab_value_equal(field(before_old, "uses"),
                        field(current_target, "uses")) &&
         renamed_text_arrays_equal(
             field(before_old, "declaration_signatures"),
             field(current_target, "declaration_signatures"), old_name,
             new_name) &&
         renamed_text_arrays_equal(
             field(before_old, "implementation_signatures"),
             field(current_target, "implementation_signatures"), old_name,
             new_name);
}

static const AbValue *find_observed_target(const AbValue *before_surface,
                                           const AbValue *current_surface,
                                           const SurfaceMakeProvider *provider,
                                           const AbString *old_name,
                                           const AbValue *before_old,
                                           int *out_ambiguous) {
  const AbValue *current_names = field(current_surface, "names");
  const AbValue *matched = NULL;
  size_t index;
  *out_ambiguous = 0;
  if (!current_names || current_names->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < current_names->as.array.count; index++) {
    const AbValue *candidate = &current_names->as.array.items[index];
    const AbValue *name = field(candidate, "name");
    if (!name || name->kind != AB_VALUE_STRING ||
        ab_string_equal(&name->as.text, old_name) ||
        !portable_identifier(&name->as.text) ||
        find_named_row(field(before_surface, "names"), &name->as.text) ||
        !observed_target_matches(before_old, candidate, provider, old_name,
                                 &name->as.text))
      continue;
    if (matched) {
      *out_ambiguous = 1;
      return NULL;
    }
    matched = candidate;
  }
  return matched;
}

static ArchbirdStatus
analyze_observed_rewrite(const AbValue *map, const AbValue *before_map,
                         const AbValue *definition,
                         const AbPlanFindingGroup *group, SurfaceRewrite *out,
                         int *out_supported, int *out_ambiguous) {
  const AbValue *finding = group->representative;
  const AbValue *old_value = field(finding, "key");
  const AbValue *surface_value = field(definition, "name");
  const AbValue *before_surface;
  const AbValue *current_surface;
  const AbValue *before_old;
  const AbValue *current_old;
  const AbValue *target;
  const AbValue *target_name;
  *out_supported = 0;
  *out_ambiguous = 0;
  memset(out, 0, sizeof(*out));
  if (!before_map || !ab_plan_finding_current(finding) ||
      !text_is(field(finding, "comparison"), "unresolved") || !old_value ||
      old_value->kind != AB_VALUE_STRING ||
      !portable_identifier(&old_value->as.text) || !surface_value ||
      surface_value->kind != AB_VALUE_STRING)
    return ARCHBIRD_OK;
  before_surface = find_surface(before_map, &surface_value->as.text);
  current_surface = find_surface(map, &surface_value->as.text);
  before_old =
      find_named_row(field(before_surface, "names"), &old_value->as.text);
  current_old =
      find_named_row(field(current_surface, "names"), &old_value->as.text);
  if (!target_is_resolved(before_old) ||
      !old_is_inactive_make_declaration(current_old, &out->provider) ||
      !row_has_provider(before_old, &out->provider))
    return ARCHBIRD_OK;
  target = find_observed_target(before_surface, current_surface, &out->provider,
                                &old_value->as.text, before_old, out_ambiguous);
  target_name = field(target, "name");
  if (!target_name)
    return ARCHBIRD_OK;
  out->finding = finding;
  out->replacement_name = target_name;
  out->surface_name = surface_value;
  *out_supported = 1;
  return ARCHBIRD_OK;
}

static ArchbirdStatus analyze_removal(const AbValue *map,
                                      const AbValue *definition,
                                      const AbPlanFindingGroup *group,
                                      SurfaceRewrite *out, int *out_supported) {
  const AbValue *finding = group->representative;
  const AbValue *old_value = field(finding, "key");
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  const AbValue *old_row;
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
  out->finding = finding;
  out->surface_name = surface_value;
  *out_supported = 1;
  return ARCHBIRD_OK;
}

static void rewrites_free(ArchbirdEngine *engine, SurfaceRewrite *rewrites) {
  ab_free(engine, rewrites);
}

static void insertions_free(ArchbirdEngine *engine,
                            SurfaceInsertion *insertions, size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_free(engine, insertions[index].findings);
  ab_free(engine, insertions);
}

static ArchbirdStatus render_provider(AbBuffer *out,
                                      const SurfaceMakeProvider *provider) {
  ArchbirdStatus status =
      ab_buffer_literal(out, "{\"kind\":\"make_variable\",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, provider->path);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"variable\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, provider->variable.data,
                                   provider->variable.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static ArchbirdStatus render_rewrite_operation(ArchbirdEngine *engine,
                                               const SurfaceRewrite *rewrite,
                                               const char *action,
                                               AbBuffer *out) {
  ArchbirdStatus status;
  const AbValue *key = field(rewrite->finding, "key");
  int rename = strcmp(action, "rename_provider_capability") == 0;
  ab_buffer_init(out, engine);
  status = ab_buffer_literal(out, "{\"action\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, action, strlen(action));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, rename ? ",\"from\":" : ",\"capability\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, key);
  if (status == ARCHBIRD_OK && rename)
    status = ab_buffer_literal(out, ",\"to\":");
  if (status == ARCHBIRD_OK && rename)
    status = ab_value_render(out, rewrite->replacement_name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"provider\":");
  if (status == ARCHBIRD_OK)
    status = render_provider(out, &rewrite->provider);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"surface\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, rewrite->surface_name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static ArchbirdStatus
render_insertion_operation(ArchbirdEngine *engine,
                           const SurfaceInsertion *insertion, AbBuffer *out) {
  ArchbirdStatus status;
  ab_buffer_init(out, engine);
  status = ab_buffer_literal(out, "{\"action\":\"add_provider_capability\","
                                  "\"capability\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, insertion->key);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"provider\":");
  if (status == ARCHBIRD_OK)
    status = render_provider(out, &insertion->provider);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"surface\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, insertion->surface_name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "}");
  return status;
}

static int insertion_comparison(const AbValue *finding) {
  const AbValue *comparison = field(finding, "comparison");
  return text_is(comparison, "missing") || text_is(comparison, "unregistered");
}

static ArchbirdStatus
analyze_insertions(ArchbirdEngine *engine, const AbValue *map,
                   const AbValue *definition, const AbPlanFindingGroups *groups,
                   SurfaceInsertion *insertions, size_t *out_count,
                   int *out_supported) {
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
    insertion->surface_name = surface_value;
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
    status = render_rewrite_operation(engine, &removals[index],
                                      "remove_provider_capability", &operation);
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

static ArchbirdStatus
append_rewrites(ArchbirdEngine *engine, AbPlanItemBuilder *builder,
                const AbValue *constraint, const AbPlanFindingGroups *groups,
                SurfaceRewrite *rewrites, uint8_t *rename_used) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < groups->count; index++) {
    const AbValue *key = field(rewrites[index].finding, "key");
    const AbValue *new_name = rewrites[index].replacement_name;
    const char *action = rewrites[index].remove_only
                             ? "remove_provider_capability"
                             : "rename_provider_capability";
    AbBuffer operation;
    char statement[1024];
    AbPlanItemSpec spec;
    int length;
    if (rewrites[index].remove_only)
      length = snprintf(
          statement, sizeof(statement),
          "Remove stale provider registration %.*s from %.*s; replacement "
          "%.*s is already registered.",
          (int)key->as.text.length, key->as.text.data,
          (int)rewrites[index].provider.path->as.text.length,
          rewrites[index].provider.path->as.text.data,
          (int)new_name->as.text.length, new_name->as.text.data);
    else
      length = snprintf(
          statement, sizeof(statement),
          "Replace stale provider registration %.*s with %.*s in %.*s.",
          (int)key->as.text.length, key->as.text.data,
          (int)new_name->as.text.length, new_name->as.text.data,
          (int)rewrites[index].provider.path->as.text.length,
          rewrites[index].provider.path->as.text.data);
    if (length < 0 || (size_t)length >= sizeof(statement))
      return archbird_error_set(
          engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
          "plan compilation: provider-surface statement is too long");
    status =
        render_rewrite_operation(engine, &rewrites[index], action, &operation);
    if (status == ARCHBIRD_OK) {
      memset(&spec, 0, sizeof(spec));
      spec.constraint = constraint;
      spec.findings = groups->groups[index].rows;
      spec.finding_count = groups->groups[index].count;
      spec.statement = statement;
      spec.provenance = rewrites[index].asserted ? "asserted" : "derived";
      spec.operation = &operation;
      spec.executable = 1;
      status = ab_plan_item_builder_append(builder, &spec);
    }
    ab_buffer_free(&operation);
    if (status == ARCHBIRD_OK && rewrites[index].asserted)
      rename_used[rewrites[index].rename_index] = 1;
  }
  return status;
}

ArchbirdStatus ab_plan_compile_surface_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    const AbValue *before_map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbValue *renames, uint8_t *rename_used, int *out_handled) {
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  SurfaceRewrite *rewrites = NULL;
  size_t index;
  ArchbirdStatus status;
  (void)project;
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
    status = analyze_insertions(engine, map, definition, &groups, insertions,
                                &insertion_count, &supported);
    if (status == ARCHBIRD_OK && supported) {
      status = append_insertions(engine, builder, constraint, insertions,
                                 insertion_count);
      if (status == ARCHBIRD_OK)
        *out_handled = 1;
    }
    insertions_free(engine, insertions, groups.count);
    if (status == ARCHBIRD_OK && !supported) {
      SurfaceRewrite *observed =
          (SurfaceRewrite *)ab_calloc(engine, groups.count, sizeof(*observed));
      int observed_ambiguous = 0;
      if (!observed) {
        ab_plan_finding_groups_free(engine, &groups);
        return archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "plan compilation: out of memory deriving observed provider-"
            "surface rewrites");
      }
      for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
        int rewrite_supported = 0;
        int rewrite_ambiguous = 0;
        status = analyze_observed_rewrite(
            map, before_map, definition, &groups.groups[index],
            &observed[index], &rewrite_supported, &rewrite_ambiguous);
        if (rewrite_ambiguous)
          observed_ambiguous = 1;
        if (status == ARCHBIRD_OK && !rewrite_supported)
          break;
      }
      if (status == ARCHBIRD_OK && index == groups.count) {
        status = append_rewrites(engine, builder, constraint, &groups, observed,
                                 NULL);
        if (status == ARCHBIRD_OK) {
          *out_handled = 1;
          supported = 1;
        }
      }
      rewrites_free(engine, observed);
      if (observed_ambiguous)
        supported = 1;
    }
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
        status = analyze_removal(map, definition, &groups.groups[index],
                                 &removals[index], &removal_supported);
        if (status == ARCHBIRD_OK && !removal_supported)
          break;
      }
      if (status == ARCHBIRD_OK && index == groups.count) {
        status =
            append_removals(engine, builder, constraint, &groups, removals);
        if (status == ARCHBIRD_OK)
          *out_handled = 1;
      }
      rewrites_free(engine, removals);
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
    status = analyze_rewrite(map, definition, renames, &groups.groups[index],
                             &rewrites[index], &supported);
    if (status == ARCHBIRD_OK && !supported)
      break;
  }
  if (status == ARCHBIRD_OK && index == groups.count) {
    status = append_rewrites(engine, builder, constraint, &groups, rewrites,
                             rename_used);
    if (status == ARCHBIRD_OK)
      *out_handled = 1;
  }
  rewrites_free(engine, rewrites);
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
