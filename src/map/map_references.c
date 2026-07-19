#include "map_references.h"

#include "archbird_internal.h"
#include "json_value.h"

#include <stdio.h>
#include <string.h>

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static const AbObjectField *fact_attribute(const AbFact *fact,
                                           const char *name) {
  AbString wanted = {(char *)name, strlen(name)};
  size_t low = 0;
  size_t high = fact->attribute_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&fact->attributes[middle].name, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &fact->attributes[middle];
  }
  return NULL;
}

static const AbString *string_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_STRING ? &field->value.as.text
                                                       : NULL;
}

static const AbValue *array_attribute(const AbFact *fact, const char *name) {
  const AbObjectField *field = fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_ARRAY ? &field->value : NULL;
}

static int size_attribute(const AbFact *fact, const char *name, size_t *out) {
  const AbObjectField *field = fact_attribute(fact, name);
  uint64_t value;
  if (!field)
    return 0;
  if (!ab_value_u64(&field->value, &value) || value > SIZE_MAX)
    return -1;
  *out = (size_t)value;
  return 1;
}

static const char *provider_reference_relation(const AbFact *fact) {
  if (string_literal(&fact->kind, "call"))
    return "semantic-call";
  if (string_literal(&fact->kind, "construct"))
    return "semantic-construct";
  if (string_literal(&fact->kind, "decorator"))
    return "semantic-decorator";
  if (string_literal(&fact->kind, "type"))
    return "semantic-type-reference";
  if (string_literal(&fact->kind, "import"))
    return "semantic-import";
  if (string_literal(&fact->kind, "scip-related-reference"))
    return "scip-related-reference";
  if (string_literal(&fact->kind, "scip-implementation"))
    return "scip-implementation";
  if (string_literal(&fact->kind, "scip-type-definition"))
    return "scip-type-definition";
  if (string_literal(&fact->kind, "scip-definition"))
    return "scip-definition";
  return "semantic-reference";
}

static int fact_domain(const AbFact *fact, const char *domain) {
  return string_literal(&fact->domain, domain);
}

static const AbManifestFile *fact_file(const AbMapState *state,
                                       const AbFact *fact) {
  return ab_map_manifest_file(state->manifest, fact->path.data,
                              fact->path.length);
}

static int qualified_extension(const AbString *longer,
                               const AbString *shorter) {
  size_t offset;
  if (longer->length <= shorter->length)
    return 0;
  offset = longer->length - shorter->length;
  return offset && longer->data[offset - 1] == '.' &&
         memcmp(longer->data + offset, shorter->data, shorter->length) == 0;
}

static const AbString *symbol_at_span(const AbMapState *state,
                                      const AbManifestFile *file, size_t start,
                                      size_t end) {
  const AbString *selected = NULL;
  size_t low;
  size_t high;
  size_t index;
  ab_project_merged_fact_range(state->project, &file->path, "symbols", &low,
                               &high);
  for (index = low; index < high; index++) {
    const AbFact *fact = ab_project_merged_fact_by_path(state->project, index);
    if (!fact->has_name || fact->span_start != start || fact->span_end != end)
      continue;
    if (!selected || qualified_extension(&fact->name, selected)) {
      selected = &fact->name;
      continue;
    }
    if (ab_string_equal(selected, &fact->name) ||
        qualified_extension(selected, &fact->name))
      continue;
    return NULL;
  }
  return selected;
}

static const AbFact *named_symbol(const AbMapState *state,
                                  const AbManifestFile *file, const char *name,
                                  size_t name_length,
                                  const char *required_kind) {
  const AbFact *matched = NULL;
  size_t low;
  size_t high;
  size_t index;
  ab_project_merged_fact_range(state->project, &file->path, "symbols", &low,
                               &high);
  for (index = low; index < high; index++) {
    const AbFact *fact = ab_project_merged_fact_by_path(state->project, index);
    if (!fact->has_name || fact->name.length != name_length ||
        (name_length && memcmp(fact->name.data, name, name_length) != 0) ||
        string_literal(&fact->kind, "declaration") ||
        (required_kind && !string_literal(&fact->kind, required_kind)))
      continue;
    if (matched && (!ab_string_equal(&matched->name, &fact->name) ||
                    !ab_string_equal(&matched->kind, &fact->kind)))
      return NULL;
    matched = fact;
  }
  return matched;
}

static const AbFact *qualified_member(AbMapState *state,
                                      const AbManifestFile *file,
                                      const AbFact *owner,
                                      const AbString *member,
                                      const char *required_kind) {
  AbBuffer qualified;
  const AbFact *result = NULL;
  ArchbirdStatus status;
  ab_buffer_init(&qualified, state->engine);
  status = ab_buffer_append(&qualified, owner->name.data, owner->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&qualified, ".");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&qualified, member->data, member->length);
  if (status == ARCHBIRD_OK)
    result = named_symbol(state, file, (const char *)qualified.data,
                          qualified.length, required_kind);
  ab_buffer_free(&qualified);
  return result;
}

typedef struct AbInheritedMemberMatch {
  const AbManifestFile *file;
  const AbFact *fact;
  size_t count;
  int incomplete;
} AbInheritedMemberMatch;

static void inherited_member_add(AbInheritedMemberMatch *match,
                                 const AbManifestFile *file,
                                 const AbFact *fact) {
  if (match->fact && match->file == file &&
      ab_string_equal(&match->fact->name, &fact->name))
    return;
  if (!match->fact) {
    match->file = file;
    match->fact = fact;
  }
  match->count++;
}

static int resolve_base_class(AbMapState *state,
                              const AbManifestFile *owner_file,
                              const AbFact *base,
                              const AbManifestFile **out_file,
                              const AbFact **out_class) {
  const AbString *binding = string_attribute(base, "binding");
  const AbString *base_symbol = string_attribute(base, "base_symbol");
  AbMapReferenceResolution resolution;
  ArchbirdStatus status;
  *out_file = NULL;
  *out_class = NULL;
  if (!binding || !base_symbol)
    return 0;
  if (string_literal(binding, "local")) {
    *out_class = named_symbol(state, owner_file, base_symbol->data,
                              base_symbol->length, "class");
    if (*out_class)
      *out_file = owner_file;
    return *out_class != NULL;
  }
  if (!string_literal(binding, "imported"))
    return 0;
  status =
      ab_map_resolve_imported_reference(state, owner_file, base, &resolution);
  if (status != ARCHBIRD_OK || !resolution.exact || !resolution.target ||
      !resolution.target_fact ||
      !string_literal(&resolution.target_fact->kind, "class"))
    return 0;
  *out_file = resolution.target;
  *out_class = resolution.target_fact;
  return 1;
}

static ArchbirdStatus
collect_inherited_member(AbMapState *state, const AbManifestFile *owner_file,
                         const AbFact *owner_class, const AbString *member,
                         size_t depth, AbInheritedMemberMatch *match) {
  size_t low;
  size_t high;
  size_t index;
  if (depth >= 64) {
    match->incomplete = 1;
    return ARCHBIRD_OK;
  }
  ab_project_merged_fact_range(state->project, &owner_file->path, "class-bases",
                               &low, &high);
  for (index = low; index < high; index++) {
    const AbFact *base = ab_project_merged_fact_by_path(state->project, index);
    const AbManifestFile *base_file;
    const AbFact *base_class;
    const AbFact *definition;
    ArchbirdStatus status;
    if (!base || !base->has_name ||
        !ab_string_equal(&base->name, &owner_class->name))
      continue;
    if (!resolve_base_class(state, owner_file, base, &base_file, &base_class)) {
      match->incomplete = 1;
      continue;
    }
    definition = qualified_member(state, base_file, base_class, member, NULL);
    if (definition) {
      inherited_member_add(match, base_file, definition);
      continue;
    }
    status = collect_inherited_member(state, base_file, base_class, member,
                                      depth + 1, match);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int file_module_binding(AbMapState *state, const AbManifestFile *file,
                               const AbString *name,
                               const AbManifestFile *expected) {
  size_t low;
  size_t high;
  size_t index;
  int matched = 0;
  ab_project_merged_fact_range(state->project, &file->path, "module-bindings",
                               &low, &high);
  for (index = low; index < high; index++) {
    const AbFact *fact = ab_project_merged_fact_by_path(state->project, index);
    const AbString *target_module;
    const AbManifestFile *resolved = NULL;
    if (!fact->has_name || !ab_string_equal(&fact->name, name))
      continue;
    target_module = string_attribute(fact, "target_module");
    if (!target_module ||
        ab_map_resolve_import(state->engine, state->manifest, state->config,
                              file, target_module, &resolved) != ARCHBIRD_OK ||
        resolved != expected)
      return 0;
    matched = 1;
  }
  return matched;
}

static const AbMapPackage *matching_package(const AbMapState *state,
                                            const AbManifestFile *source,
                                            const AbString *module) {
  const char *manager = ab_map_language_manager(&source->language);
  AbString external;
  const AbMapPackage *matched = NULL;
  size_t matches = 0;
  size_t index;
  if (!manager[0] || !module->length || module->data[0] == '.')
    return NULL;
  external = ab_map_external_import_name(&source->language, module);
  for (index = 0; index < state->package_count; index++) {
    const AbMapPackage *candidate = &state->packages[index];
    if (strcmp(ab_map_package_manager(&candidate->kind), manager) != 0 ||
        !ab_map_package_alias_matches(candidate, manager, &external))
      continue;
    matched = candidate;
    matches++;
  }
  return matches == 1 ? matched : NULL;
}

static const AbMapExportOrigin *package_origin(const AbMapPackage *package,
                                               const AbString *name) {
  size_t index;
  if (!package)
    return NULL;
  for (index = 0; index < package->export_origin_count; index++)
    if (ab_string_equal(&package->export_origins[index].name, name))
      return &package->export_origins[index];
  return NULL;
}

static const AbFact *exported_symbol(AbMapState *state,
                                     const AbMapPackage *package,
                                     const AbString *name,
                                     const AbManifestFile **out_file) {
  const AbMapExportOrigin *origin = package_origin(package, name);
  const AbManifestFile *file;
  const AbFact *symbol;
  const AbString *target_name;
  if (!origin || origin->paths.count != 1)
    return NULL;
  file = ab_map_manifest_file(state->manifest, origin->paths.items[0].data,
                              origin->paths.items[0].length);
  if (!file)
    return NULL;
  target_name = origin->target_symbol.length ? &origin->target_symbol : name;
  symbol =
      named_symbol(state, file, target_name->data, target_name->length, NULL);
  if (symbol)
    *out_file = file;
  return symbol;
}

static const AbFact *file_exported_symbol(AbMapState *state,
                                          const AbManifestFile **file,
                                          const AbString *name, size_t depth) {
  const AbFact *direct;
  const AbFact *origin = NULL;
  const AbString *module;
  const AbString *origin_name;
  const AbManifestFile *target = NULL;
  size_t low;
  size_t high;
  size_t index;
  ArchbirdStatus status;
  if (!file || !*file || depth >= 64)
    return NULL;
  direct = named_symbol(state, *file, name->data, name->length, NULL);
  if (direct)
    return direct;
  ab_project_merged_fact_range(state->project, &(*file)->path, "export-origins",
                               &low, &high);
  for (index = low; index < high; index++) {
    const AbFact *candidate =
        ab_project_merged_fact_by_path(state->project, index);
    if (!candidate || !candidate->has_name ||
        !ab_string_equal(&candidate->name, name))
      continue;
    if (origin && origin != candidate)
      return NULL;
    origin = candidate;
  }
  if (!origin)
    return NULL;
  module = string_attribute(origin, "origin");
  origin_name = string_attribute(origin, "origin_name");
  if (!module || !module->length)
    return NULL;
  status = ab_map_resolve_import(state->engine, state->manifest, state->config,
                                 *file, module, &target);
  if (status != ARCHBIRD_OK || !target)
    return NULL;
  *file = target;
  return file_exported_symbol(
      state, file, origin_name && origin_name->length ? origin_name : name,
      depth + 1);
}

static const AbManifestFile *candidate_module(AbMapState *state,
                                              const AbManifestFile *source,
                                              const AbFact *fact) {
  const AbString *module = string_attribute(fact, "module");
  const AbManifestFile *target = NULL;
  if (module &&
      ab_map_resolve_import(state->engine, state->manifest, state->config,
                            source, module, &target) == ARCHBIRD_OK)
    return target;
  return NULL;
}

ArchbirdStatus ab_map_resolve_imported_reference(
    AbMapState *state, const AbManifestFile *source, const AbFact *fact,
    AbMapReferenceResolution *out) {
  const AbString *root_module = string_attribute(fact, "root_module");
  const AbString *target_name = string_attribute(fact, "imported");
  const AbValue *steps = array_attribute(fact, "module_steps");
  const AbMapPackage *package;
  const AbManifestFile *current = NULL;
  const AbFact *namespace_symbol = NULL;
  AbBuffer progressive;
  size_t index;
  ArchbirdStatus status;
  int at_root = 1;
  int inferred_receiver = string_literal(&fact->kind, "inferred-receiver-call");
  memset(out, 0, sizeof(*out));
  if (!root_module || !target_name || !steps)
    return archbird_error_set(
        state->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "imported reference requires root_module/imported/module_steps");
  for (index = 0; index < steps->as.array.count; index++)
    if (steps->as.array.items[index].kind != AB_VALUE_STRING)
      return archbird_error_set(
          state->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "imported reference module_steps must contain strings");

  status = ab_map_resolve_import(state->engine, state->manifest, state->config,
                                 source, root_module, &current);
  if (status != ARCHBIRD_OK)
    return status;
  package = matching_package(state, source, root_module);
  ab_buffer_init(&progressive, state->engine);
  status =
      ab_buffer_append(&progressive, root_module->data, root_module->length);
  if (status == ARCHBIRD_OK && !current && package && steps->as.array.count) {
    const AbString *step = &steps->as.array.items[0].as.text;
    namespace_symbol = exported_symbol(state, package, step, &current);
    if (!namespace_symbol ||
        !string_literal(&namespace_symbol->kind, "class")) {
      current = NULL;
      namespace_symbol = NULL;
    } else {
      at_root = 0;
      index = 1;
    }
  } else {
    index = 0;
  }
  for (; status == ARCHBIRD_OK && current && index < steps->as.array.count;
       index++) {
    const AbString *step = &steps->as.array.items[index].as.text;
    if (!namespace_symbol) {
      const AbManifestFile *next = NULL;
      AbString progressive_name;
      if (progressive.length && progressive.data[progressive.length - 1] != '.')
        status = ab_buffer_literal(&progressive, ".");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&progressive, step->data, step->length);
      if (status != ARCHBIRD_OK)
        break;
      progressive_name.data = (char *)progressive.data;
      progressive_name.length = progressive.length;
      status =
          ab_map_resolve_import(state->engine, state->manifest, state->config,
                                source, &progressive_name, &next);
      if (status != ARCHBIRD_OK)
        break;
      if (next && file_module_binding(state, current, step, next)) {
        current = next;
        at_root = 0;
        continue;
      }
      namespace_symbol =
          named_symbol(state, current, step->data, step->length, "class");
      if (!namespace_symbol && at_root)
        namespace_symbol = file_exported_symbol(state, &current, step, 0);
      if (!namespace_symbol && at_root)
        namespace_symbol = exported_symbol(state, package, step, &current);
      if (!namespace_symbol ||
          !string_literal(&namespace_symbol->kind, "class"))
        current = NULL;
    } else {
      namespace_symbol =
          qualified_member(state, current, namespace_symbol, step, "class");
      if (!namespace_symbol)
        current = NULL;
    }
  }
  if (status == ARCHBIRD_OK && !current && package && !steps->as.array.count) {
    const AbFact *target =
        exported_symbol(state, package, target_name, &current);
    if (target) {
      out->target = current;
      out->target_fact = target;
      out->target_symbol = &target->name;
      out->exact = 1;
      out->relation = string_literal(&fact->kind, "decorator-reference")
                          ? "decorator-reference"
                          : "imported-attribute-call";
      if (string_literal(&fact->kind, "decorator-reference") &&
          string_literal(&target->kind, "class"))
        out->callable_fact =
            qualified_member(state, current, target,
                             &(AbString){(char *)"__call__", 8}, "method");
    }
  }
  if (status == ARCHBIRD_OK && current) {
    const AbFact *target =
        namespace_symbol ? qualified_member(state, current, namespace_symbol,
                                            target_name, NULL)
                         : named_symbol(state, current, target_name->data,
                                        target_name->length, NULL);
    if (!target && !namespace_symbol && at_root)
      target = file_exported_symbol(state, &current, target_name, 0);
    if (!target && !namespace_symbol && at_root)
      target = exported_symbol(state, package, target_name, &current);
    if (!target && inferred_receiver && namespace_symbol) {
      AbInheritedMemberMatch inherited = {0};
      status = collect_inherited_member(state, current, namespace_symbol,
                                        target_name, 0, &inherited);
      if (status == ARCHBIRD_OK && inherited.count == 1) {
        current = inherited.file;
        target = inherited.fact;
      }
    }
    if (target) {
      out->target = current;
      out->target_fact = target;
      out->target_symbol = &target->name;
      out->exact = 1;
      out->relation =
          namespace_symbol ? "imported-member-call" : "imported-attribute-call";
      if (string_literal(&fact->kind, "decorator-reference")) {
        out->relation = "decorator-reference";
        if (string_literal(&target->kind, "class"))
          out->callable_fact =
              qualified_member(state, current, target,
                               &(AbString){(char *)"__call__", 8}, "method");
      }
      if (inferred_receiver) {
        /* The provider proved only a bounded syntactic receiver flow.  Even
         * when the mapped owner/member pair is unique, Python factory and
         * fluent calls can return another runtime type.  Retain the exact
         * structural candidate without promoting it to a semantic call. */
        if (namespace_symbol) {
          out->exact = 0;
          out->relation = "inferred-receiver-candidate";
        } else {
          out->target = NULL;
          out->target_fact = NULL;
          out->target_symbol = NULL;
        }
      }
    }
  }
  if (status == ARCHBIRD_OK && !out->target) {
    if (inferred_receiver) {
      ab_buffer_free(&progressive);
      return status;
    }
    out->target = candidate_module(state, source, fact);
    out->target_fact = out->target
                           ? named_symbol(state, out->target, target_name->data,
                                          target_name->length, NULL)
                           : NULL;
    out->relation = "imported-attribute-candidate";
  }
  ab_buffer_free(&progressive);
  return status;
}

ArchbirdStatus ab_map_resolve_imported_name_reference(
    AbMapState *state, const AbManifestFile *source, const AbFact *fact,
    AbMapReferenceResolution *out) {
  const AbString *module = string_attribute(fact, "module");
  const AbString *imported = string_attribute(fact, "imported");
  const AbManifestFile *target = NULL;
  const AbMapPackage *package;
  const AbFact *symbol = NULL;
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  if (!module || !imported)
    return archbird_error_set(
        state->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "imported-name reference requires module and imported attributes");
  if (!imported->length)
    return ARCHBIRD_OK;
  status = ab_map_resolve_import(state->engine, state->manifest, state->config,
                                 source, module, &target);
  if (status != ARCHBIRD_OK)
    return status;
  if (target)
    symbol =
        named_symbol(state, target, imported->data, imported->length, NULL);
  if (!symbol && target)
    symbol = file_exported_symbol(state, &target, imported, 0);
  if (!symbol) {
    package = matching_package(state, source, module);
    if (package)
      symbol = exported_symbol(state, package, imported, &target);
  }
  if (!target)
    return ARCHBIRD_OK;
  out->target = target;
  out->target_symbol = imported;
  out->relation = "imported-name-candidate";
  if (!symbol)
    return ARCHBIRD_OK;
  out->target_fact = symbol;
  out->target_symbol = &symbol->name;
  out->relation = string_literal(&fact->kind, "imported-name-call")
                      ? "imported-name-call"
                      : "imported-name-reference";
  out->exact = 1;
  if (string_literal(&fact->kind, "imported-name-call") &&
      string_literal(&symbol->kind, "class"))
    out->callable_fact = qualified_member(
        state, target, symbol, &(AbString){(char *)"__init__", 8}, "method");
  return ARCHBIRD_OK;
}

ArchbirdStatus
ab_map_resolve_provider_reference(AbMapState *state, const AbFact *fact,
                                  AbMapReferenceResolution *out) {
  const AbString *target_path;
  const AbString *target_symbol;
  const AbString *evidence_state;
  const AbString *mapped_symbol = NULL;
  size_t target_start = 0;
  size_t target_end = 0;
  int has_target_start;
  int has_target_end;
  memset(out, 0, sizeof(*out));
  if (!fact_domain(fact, "reference-targets") &&
      !fact_domain(fact, "semantic-relationships"))
    return archbird_error_set(state->engine, ARCHBIRD_INVALID_ARGUMENT,
                              ARCHBIRD_NO_OFFSET,
                              "provider reference has the wrong domain");
  if (!fact->has_resolution ||
      !string_literal(&fact->resolution.state, "unique"))
    return ARCHBIRD_OK;
  if (fact->resolution.targets.count != 1)
    return archbird_error_set(
        state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "unique provider reference must carry exactly one target identity");
  target_path = string_attribute(fact, "target_path");
  target_symbol = string_attribute(fact, "target_symbol");
  if (!target_path || !target_path->length)
    return ARCHBIRD_OK;
  if (!target_symbol || !target_symbol->length)
    return archbird_error_set(
        state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "mapped provider reference requires target_symbol");
  out->target = ab_map_manifest_file(state->manifest, target_path->data,
                                     target_path->length);
  if (!out->target)
    return ARCHBIRD_OK;
  has_target_start = size_attribute(fact, "target_span_start", &target_start);
  has_target_end = size_attribute(fact, "target_span_end", &target_end);
  if (has_target_start < 0 || has_target_end < 0 ||
      has_target_start != has_target_end ||
      (has_target_start && target_start > target_end))
    return archbird_error_set(
        state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "mapped provider reference has an invalid target span");
  if (has_target_start && target_end > out->target->byte_length) {
    char message[192];
    evidence_state = string_attribute(fact, "evidence_state");
    if (!evidence_state || (!string_literal(evidence_state, "unknown") &&
                            !string_literal(evidence_state, "stale")))
      return archbird_error_set(
          state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "current provider reference exceeds mapped target bytes");
    snprintf(message, sizeof(message),
             "Provider target range %zu..%zu exceeds the current %zu-byte "
             "target; the semantic edge was omitted.",
             target_start, target_end, out->target->byte_length);
    out->target = NULL;
    return ab_map_add_diagnostic(state, "warning",
                                 "provider-target-span-unavailable", message,
                                 target_path);
  }
  if (has_target_start && fact_domain(fact, "reference-targets"))
    mapped_symbol =
        symbol_at_span(state, out->target, target_start, target_end);
  out->target_symbol = fact_domain(fact, "semantic-relationships")
                           ? &fact->name
                           : (mapped_symbol ? mapped_symbol : target_symbol);
  out->relation = provider_reference_relation(fact);
  out->exact = 1;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_map_add_reference_edges(AbMapState *state) {
  static char current_bytes[] = "current";
  static const AbString current = {current_bytes, sizeof(current_bytes) - 1};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < ab_project_merged_fact_count(state->project);
       index++) {
    const AbFact *fact = ab_project_merged_fact(state->project, index);
    const AbManifestFile *source;
    const AbProviderBundle *provider;
    AbMapReferenceResolution resolution;
    const char *kind;
    if (!fact->has_name)
      continue;
    source = fact_file(state, fact);
    if (!source)
      continue;
    if (fact_domain(fact, "reference-targets") ||
        fact_domain(fact, "semantic-relationships")) {
      status = ab_map_resolve_provider_reference(state, fact, &resolution);
    } else if (fact_domain(fact, "name-uses") &&
               (string_literal(&fact->kind, "imported-attribute-call") ||
                string_literal(&fact->kind, "inferred-receiver-call") ||
                string_literal(&fact->kind, "decorator-reference"))) {
      status =
          ab_map_resolve_imported_reference(state, source, fact, &resolution);
    } else if (fact_domain(fact, "name-uses") &&
               (string_literal(&fact->kind, "imported-name-call") ||
                string_literal(&fact->kind, "imported-name-use"))) {
      status = ab_map_resolve_imported_name_reference(state, source, fact,
                                                      &resolution);
    } else {
      continue;
    }
    if (status != ARCHBIRD_OK || !resolution.exact || !resolution.target ||
        !resolution.target_symbol || resolution.target == source)
      continue;
    if (fact_domain(fact, "reference-targets") ||
        fact_domain(fact, "semantic-relationships"))
      kind = resolution.relation;
    else if (string_literal(&fact->kind, "imported-name-call"))
      kind = "imported-call";
    else if (string_literal(&fact->kind, "imported-name-use"))
      kind = "imported-reference";
    else
      kind = string_literal(&fact->kind, "decorator-reference")
                 ? "decorator"
                 : (!strcmp(resolution.relation, "imported-member-call")
                        ? "member-call"
                        : "attribute-call");
    if (fact_domain(fact, "reference-targets") ||
        fact_domain(fact, "semantic-relationships")) {
      const AbString *index_name = string_attribute(fact, "index");
      const AbString *evidence_state = string_attribute(fact, "evidence_state");
      provider = ab_project_merged_fact_provider(state->project, index);
      if (!provider)
        return archbird_error_set(
            state->engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
            "semantic Map edge is missing its evidence provider");
      status = ab_map_graph_add_edge_evidence(
          state->engine, &state->graph, kind, &source->path,
          resolution.target->path.data, resolution.target->path.length,
          resolution.target_symbol,
          index_name ? "semantic-index" : "semantic-provider",
          index_name ? index_name : &provider->producer.name,
          evidence_state ? evidence_state : &current);
    } else {
      status = ab_map_graph_add_edge(
          state->engine, &state->graph, kind, &source->path,
          resolution.target->path.data, resolution.target->path.length,
          resolution.target_symbol);
    }
  }
  if (status == ARCHBIRD_OK)
    ab_map_graph_sort(&state->graph);
  return status;
}
