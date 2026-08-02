#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SurfaceMakeProvider {
  const AbValue *definition_sha256;
  const AbValue *path;
  AbString variable;
} SurfaceMakeProvider;

typedef struct SurfaceProvider {
  const AbValue *definition_sha256;
  const AbValue *path;
  const char *kind;
  AbString variable;
} SurfaceProvider;

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
  const AbValue *implementation_path;
  const AbValue *surface_name;
  SurfaceProvider provider;
} SurfaceInsertion;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static int text_is(const AbValue *value, const char *literal) {
  return ab_artifact_text_is(value, literal);
}

static int portable_identifier(const AbString *value);

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
  const AbValue *definition_sha256 = field(declaration, "definition_sha256");
  const AbValue *path = field(declaration, "path");
  const AbValue *source = field(declaration, "source");
  if (!ab_artifact_sha256(definition_sha256) || !path ||
      path->kind != AB_VALUE_STRING || !source ||
      source->kind != AB_VALUE_STRING ||
      source->as.text.length <= sizeof(prefix) - 1 ||
      memcmp(source->as.text.data, prefix, sizeof(prefix) - 1) != 0)
    return 0;
  out->definition_sha256 = definition_sha256;
  out->path = path;
  out->variable.data = source->as.text.data + sizeof(prefix) - 1;
  out->variable.length = source->as.text.length - (sizeof(prefix) - 1);
  return 1;
}

static int parse_provider(const AbValue *declaration, SurfaceProvider *out) {
  static const char make_prefix[] = "make-variable:";
  const AbValue *definition_sha256 = field(declaration, "definition_sha256");
  const AbValue *path = field(declaration, "path");
  const AbValue *source = field(declaration, "source");
  memset(out, 0, sizeof(*out));
  if (!ab_artifact_sha256(definition_sha256) || !path ||
      path->kind != AB_VALUE_STRING || !source ||
      source->kind != AB_VALUE_STRING)
    return 0;
  out->definition_sha256 = definition_sha256;
  out->path = path;
  if (text_is(source, "file-pattern")) {
    out->kind = "file_pattern";
    return 1;
  }
  if (text_is(source, "exports")) {
    out->kind = "exports";
    return 1;
  }
  if (source->as.text.length <= sizeof(make_prefix) - 1 ||
      memcmp(source->as.text.data, make_prefix, sizeof(make_prefix) - 1) != 0)
    return 0;
  out->kind = "make_variable";
  out->variable.data = source->as.text.data + sizeof(make_prefix) - 1;
  out->variable.length = source->as.text.length - (sizeof(make_prefix) - 1);
  return portable_identifier(&out->variable);
}

static int same_provider(const SurfaceMakeProvider *provider,
                         const AbValue *declaration) {
  SurfaceMakeProvider candidate;
  return parse_make_provider(declaration, &candidate) &&
         ab_value_equal(provider->definition_sha256,
                        candidate.definition_sha256) &&
         ab_value_equal(provider->path, candidate.path) &&
         ab_string_equal(&provider->variable, &candidate.variable);
}

static int row_has_declaring_provider(const AbValue *row,
                                      const AbValue *provider);

static const AbValue *find_file(const AbValue *map, const AbString *path) {
  const AbValue *files = field(map, "files");
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbValue *candidate = field(row, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return row;
  }
  return NULL;
}

static int path_has_suffix(const AbString *path, const char *suffix) {
  size_t length = strlen(suffix);
  return path && path->length >= length &&
         memcmp(path->data + path->length - length, suffix, length) == 0;
}

static int file_has_napi_wrapper(const AbValue *file,
                                 const AbString *capability) {
  const AbValue *symbols = field(file, "symbols");
  char wrapper[262];
  size_t index;
  size_t matches = 0;
  if (!capability || capability->length > sizeof(wrapper) - 6)
    return 0;
  memcpy(wrapper, "napi_", 5);
  memcpy(wrapper + 5, capability->data, capability->length);
  wrapper[5 + capability->length] = '\0';
  for (index = 0; symbols && symbols->kind == AB_VALUE_ARRAY &&
                  index < symbols->as.array.count;
       index++) {
    const AbValue *symbol = &symbols->as.array.items[index];
    const AbValue *name = field(symbol, "name");
    if (name && name->kind == AB_VALUE_STRING &&
        name->as.text.length == 5 + capability->length &&
        memcmp(name->as.text.data, wrapper, 5 + capability->length) == 0 &&
        text_is(field(symbol, "kind"), "function") &&
        !field(symbol, "syntax_recovery"))
      matches++;
  }
  return matches == 1;
}

static int same_provider_definition(const SurfaceProvider *left,
                                    const SurfaceProvider *right) {
  return strcmp(left->kind, right->kind) == 0 &&
         ab_value_equal(left->definition_sha256, right->definition_sha256);
}

static int provider_definition_is_unique(const AbValue *providers,
                                         const SurfaceProvider *provider) {
  size_t index;
  size_t matches = 0;
  for (index = 0; providers && providers->kind == AB_VALUE_ARRAY &&
                  index < providers->as.array.count;
       index++) {
    SurfaceProvider candidate;
    if (parse_provider(&providers->as.array.items[index], &candidate) &&
        same_provider_definition(&candidate, provider))
      matches++;
  }
  return matches == 1;
}

static int provider_has_executor(const AbValue *map, const AbValue *providers,
                                 const SurfaceProvider *provider,
                                 const AbString *capability) {
  const AbValue *file;
  if (!provider_definition_is_unique(providers, provider))
    return 0;
  if (strcmp(provider->kind, "make_variable") == 0)
    return 1;
  file = find_file(map, &provider->path->as.text);
  if (!file || !text_is(field(file, "language"), "c"))
    return 0;
  if (strcmp(provider->kind, "file_pattern") == 0)
    return path_has_suffix(&provider->path->as.text, ".h");
  return strcmp(provider->kind, "exports") == 0 &&
         path_has_suffix(&provider->path->as.text, ".c") &&
         file_has_napi_wrapper(file, capability);
}

static int same_grounding_target(const SurfaceProvider *left,
                                 const SurfaceProvider *right) {
  if (strcmp(left->kind, right->kind) != 0 ||
      !ab_value_equal(left->path, right->path))
    return 0;
  if (strcmp(left->kind, "make_variable") == 0)
    return ab_string_equal(&left->variable, &right->variable);
  return 1;
}

static int missing_providers_have_distinct_targets(const AbValue *map,
                                                   const AbValue *providers,
                                                   const AbValue *target,
                                                   const AbString *capability) {
  size_t index;
  for (index = 0; index < providers->as.array.count; index++) {
    const AbValue *row = &providers->as.array.items[index];
    SurfaceProvider provider;
    size_t previous;
    if (row_has_declaring_provider(target, row))
      continue;
    if (!parse_provider(row, &provider) ||
        !provider_has_executor(map, providers, &provider, capability))
      return 0;
    for (previous = 0; previous < index; previous++) {
      const AbValue *candidate = &providers->as.array.items[previous];
      SurfaceProvider parsed;
      if (row_has_declaring_provider(target, candidate))
        continue;
      if (!parse_provider(candidate, &parsed) ||
          !provider_has_executor(map, providers, &parsed, capability) ||
          same_grounding_target(&provider, &parsed))
        return 0;
    }
  }
  return 1;
}

static int row_has_declaring_provider(const AbValue *row,
                                      const AbValue *provider) {
  const AbValue *declarations = field(row, "declarations");
  size_t index;
  if (!declarations || declarations->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < declarations->as.array.count; index++)
    if (ab_value_equal(&declarations->as.array.items[index], provider))
      return 1;
  return 0;
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

static int target_is_used(const AbValue *row) {
  const AbValue *uses = field(row, "uses");
  return row && uses && uses->kind == AB_VALUE_ARRAY && uses->as.array.count;
}

static int missing_providers_are_exports(const AbValue *providers,
                                         const AbValue *target) {
  size_t index;
  int missing = 0;
  for (index = 0; providers && providers->kind == AB_VALUE_ARRAY &&
                  index < providers->as.array.count;
       index++) {
    SurfaceProvider provider;
    const AbValue *row = &providers->as.array.items[index];
    if (row_has_declaring_provider(target, row))
      continue;
    if (!parse_provider(row, &provider) ||
        strcmp(provider.kind, "exports") != 0)
      return 0;
    missing = 1;
  }
  return missing;
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
        !ab_plan_renamed_text_equal(&left->as.text, old_name, new_name,
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
  if (!insertions)
    return;
  for (index = 0; index < count; index++)
    ab_free(engine, insertions[index].findings);
  ab_free(engine, insertions);
}

static ArchbirdStatus
render_make_provider(AbBuffer *out, const SurfaceMakeProvider *provider) {
  ArchbirdStatus status = ab_buffer_literal(out, "{\"definition_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, provider->definition_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"kind\":\"make_variable\",\"path\":");
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

static ArchbirdStatus
render_insertion_provider(AbBuffer *out, const SurfaceProvider *provider) {
  ArchbirdStatus status = ab_buffer_literal(out, "{\"definition_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, provider->definition_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"kind\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(out, provider->kind, strlen(provider->kind));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ",\"path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(out, provider->path);
  if (status == ARCHBIRD_OK && strcmp(provider->kind, "make_variable") == 0)
    status = ab_buffer_literal(out, ",\"variable\":");
  if (status == ARCHBIRD_OK && strcmp(provider->kind, "make_variable") == 0)
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
    status = render_make_provider(out, &rewrite->provider);
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
    status = render_insertion_provider(out, &insertion->provider);
  if (status == ARCHBIRD_OK)
    if (strcmp(insertion->provider.kind, "file_pattern") == 0) {
      const AbValue *first = insertion->provider.path;
      const AbValue *second = insertion->implementation_path;
      if (ab_string_compare(&first->as.text, &second->as.text) > 0) {
        const AbValue *swapped = first;
        first = second;
        second = swapped;
      }
      status = ab_buffer_literal(out, ",\"source_paths\":[");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(out, first);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(out, ",");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(out, second);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(out, "]");
    }
  if (status == ARCHBIRD_OK &&
      strcmp(insertion->provider.kind, "exports") == 0) {
    status = ab_buffer_literal(out, ",\"source_paths\":[");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(out, insertion->provider.path);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "]");
  }
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
                   SurfaceInsertion **out_insertions, size_t *out_count,
                   int *out_supported) {
  const AbValue *surface_value = field(definition, "name");
  const AbValue *surface;
  const AbValue *providers;
  SurfaceInsertion *insertions = NULL;
  uint8_t *consumed = NULL;
  size_t capacity = 0;
  size_t insertion_count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_count = 0;
  *out_insertions = NULL;
  *out_supported = 0;
  if (!surface_value || surface_value->kind != AB_VALUE_STRING ||
      !groups->count)
    return ARCHBIRD_OK;
  surface = find_surface(map, &surface_value->as.text);
  providers = field(surface, "providers");
  if (!providers || providers->kind != AB_VALUE_ARRAY ||
      !providers->as.array.count ||
      groups->count > SIZE_MAX / providers->as.array.count)
    return ARCHBIRD_OK;
  capacity = groups->count * providers->as.array.count;
  insertions =
      (SurfaceInsertion *)ab_calloc(engine, capacity, sizeof(*insertions));
  if (!insertions)
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "plan compilation: out of memory deriving provider-surface "
        "insertions");
  consumed = (uint8_t *)ab_calloc(engine, groups->count, sizeof(*consumed));
  if (!consumed) {
    ab_free(engine, insertions);
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "plan compilation: out of memory grouping provider-surface issues");
  }
  for (index = 0; status == ARCHBIRD_OK && index < groups->count; index++) {
    const AbValue *finding;
    const AbValue *key;
    const AbValue *target;
    size_t group_index;
    size_t provider_index;
    size_t insertion_start;
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
    target = find_named_row(field(surface, "names"), &key->as.text);
    if (!target_is_resolved(target) && !target_is_implemented(target) &&
        !(target_is_used(target) &&
          missing_providers_are_exports(providers, target)))
      break;
    if (!missing_providers_have_distinct_targets(map, providers, target,
                                                 &key->as.text))
      break;
    insertion_start = insertion_count;
    for (provider_index = 0; provider_index < providers->as.array.count;
         provider_index++) {
      const AbValue *provider = &providers->as.array.items[provider_index];
      SurfaceProvider parsed;
      SurfaceInsertion *insertion;
      if (row_has_declaring_provider(target, provider))
        continue;
      if (!parse_provider(provider, &parsed)) {
        status = ARCHBIRD_OK;
        break;
      }
      insertion = &insertions[insertion_count];
      insertion->findings = (const AbValue **)ab_calloc(
          engine, finding_count, sizeof(*insertion->findings));
      if (!insertion->findings) {
        status = archbird_error_set(
            engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "plan compilation: out of memory retaining provider-surface "
            "issues");
        break;
      }
      insertion->key = key;
      insertion->finding_count = finding_count;
      if (strcmp(parsed.kind, "file_pattern") == 0) {
        const AbValue *candidates = field(target, "candidates");
        if (!candidates || candidates->kind != AB_VALUE_ARRAY ||
            candidates->as.array.count != 1) {
          status = ARCHBIRD_OK;
          break;
        }
        insertion->implementation_path = &candidates->as.array.items[0];
      }
      insertion->surface_name = surface_value;
      insertion->provider = parsed;
      insertion_count++;
    }
    if (status != ARCHBIRD_OK || provider_index != providers->as.array.count) {
      insertion_count = 0;
      break;
    }
    for (group_index = index; group_index < groups->count; group_index++) {
      const AbPlanFindingGroup *group = &groups->groups[group_index];
      const AbValue *candidate_key = field(group->representative, "key");
      size_t row_index;
      if (!candidate_key || candidate_key->kind != AB_VALUE_STRING ||
          !ab_string_equal(&candidate_key->as.text, &key->as.text))
        continue;
      consumed[group_index] = 1;
      for (provider_index = insertion_start; provider_index < insertion_count;
           provider_index++) {
        SurfaceInsertion *insertion = &insertions[provider_index];
        for (row_index = 0; row_index < group->count; row_index++)
          insertion->findings[finding_index + row_index] =
              group->rows[row_index];
      }
      finding_index += group->count;
    }
  }
  if (status == ARCHBIRD_OK && index == groups->count) {
    *out_insertions = insertions;
    *out_count = insertion_count;
    *out_supported = insertion_count != 0;
    insertions = NULL;
  }
  insertions_free(engine, insertions, capacity);
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
    SurfaceInsertion *insertions = NULL;
    size_t insertion_count = 0;
    int supported = 0;
    status = analyze_insertions(engine, map, definition, &groups, &insertions,
                                &insertion_count, &supported);
    if (status == ARCHBIRD_OK && supported) {
      status = append_insertions(engine, builder, constraint, insertions,
                                 insertion_count);
      if (status == ARCHBIRD_OK)
        *out_handled = 1;
    }
    insertions_free(engine, insertions, insertion_count);
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
