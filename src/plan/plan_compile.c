#include <archbird/archbird.h>

#include "archbird_internal.h"
#include "artifact_validation.h"
#include "json_value.h"
#include "plan_compile_internal.h"
#include "plan_internal.h"
#include "projection_internal.h"
#include "render_internal.h"
#include "sha256.h"
#include "verification_artifact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AbPlanCompile {
  ArchbirdEngine *engine;
  const ArchbirdProject *project;
  AbValue map;
  AbValue before_map;
  AbValue request;
  AbVerificationArtifact verification;
  const AbValue *map_project;
  const AbValue *map_files;
  const AbValue *selected_ids;
  const AbValue *renames;
  uint8_t *rename_used;
  const AbValue *redirects;
  uint8_t *redirect_used;
  const AbValue *objective;
  AbPlanItemBuilder builder;
  AbProjectionPlan destructive_plan;
  AbProjectionResult destructive_result;
  int destructive_ready;
} AbPlanCompile;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus fail(AbPlanCompile *context, ArchbirdStatus status,
                           const char *message) {
  return archbird_error_set(context->engine, status, ARCHBIRD_NO_OFFSET,
                            "plan compilation: %s", message);
}

static int write_buffer(void *user_data, const uint8_t *bytes, size_t length) {
  return ab_buffer_append((AbBuffer *)user_data, bytes, length) == ARCHBIRD_OK
             ? 0
             : 1;
}

static int string_equal_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         memcmp(value->data, literal, length) == 0;
}

static int value_string_equal(const AbValue *left, const AbValue *right) {
  return left && right && left->kind == AB_VALUE_STRING &&
         right->kind == AB_VALUE_STRING &&
         ab_string_equal(&left->as.text, &right->as.text);
}

static ArchbirdStatus literal(AbBuffer *buffer, const char *value) {
  return ab_buffer_append(buffer, value, strlen(value));
}

static ArchbirdStatus json_string(AbBuffer *buffer, const AbString *value) {
  return ab_buffer_json_string(buffer, value ? value->data : "",
                               value ? value->length : 0);
}

static ArchbirdStatus json_cstring(AbBuffer *buffer, const char *value) {
  return ab_buffer_json_string(buffer, value, strlen(value));
}

static int request_field_allowed(const AbString *name) {
  return string_equal_literal(name, "constraint_ids") ||
         string_equal_literal(name, "objective") ||
         string_equal_literal(name, "redirects") ||
         string_equal_literal(name, "renames");
}

static ArchbirdStatus validate_request(AbPlanCompile *context) {
  size_t index;
  if (!context->request.kind) {
    context->request.kind = AB_VALUE_OBJECT;
    return ARCHBIRD_OK;
  }
  if (context->request.kind != AB_VALUE_OBJECT)
    return fail(context, ARCHBIRD_INVALID_SCHEMA, "request must be an object");
  for (index = 0; index < context->request.as.object.count; index++)
    if (!request_field_allowed(&context->request.as.object.fields[index].name))
      return fail(context, ARCHBIRD_INVALID_SCHEMA,
                  "request contains an unsupported field");
  context->selected_ids = field(&context->request, "constraint_ids");
  context->renames = field(&context->request, "renames");
  context->redirects = field(&context->request, "redirects");
  context->objective = field(&context->request, "objective");
  if (context->selected_ids) {
    size_t left;
    size_t right;
    if (context->selected_ids->kind != AB_VALUE_ARRAY ||
        context->selected_ids->as.array.count > AB_PLAN_COMPILE_MAX_ROWS)
      return fail(context, ARCHBIRD_INVALID_SCHEMA,
                  "constraint_ids must be a bounded array");
    for (left = 0; left < context->selected_ids->as.array.count; left++) {
      const AbValue *value = &context->selected_ids->as.array.items[left];
      if (!ab_artifact_stable_id(value))
        return fail(context, ARCHBIRD_INVALID_SCHEMA,
                    "constraint_ids contains an invalid identifier");
      for (right = 0; right < left; right++)
        if (ab_value_equal(value,
                           &context->selected_ids->as.array.items[right]))
          return fail(context, ARCHBIRD_INVALID_SCHEMA,
                      "constraint_ids contains a duplicate");
    }
  }
  if (context->objective &&
      !ab_artifact_bounded_text(context->objective, 64u * 1024u, 1))
    return fail(context, ARCHBIRD_INVALID_SCHEMA,
                "objective must be non-empty bounded text");
  if (context->renames) {
    if (context->renames->kind != AB_VALUE_OBJECT ||
        context->renames->as.object.count > AB_PLAN_COMPILE_MAX_ROWS)
      return fail(context, ARCHBIRD_INVALID_SCHEMA,
                  "renames must be a bounded object");
    for (index = 0; index < context->renames->as.object.count; index++) {
      const AbObjectField *rename = &context->renames->as.object.fields[index];
      if (!rename->name.length ||
          !ab_artifact_bounded_text(&rename->value, 256, 1))
        return fail(context, ARCHBIRD_INVALID_SCHEMA,
                    "renames must map non-empty names");
    }
  }
  if (context->redirects) {
    if (context->redirects->kind != AB_VALUE_OBJECT ||
        context->redirects->as.object.count > AB_PLAN_COMPILE_MAX_ROWS)
      return fail(context, ARCHBIRD_INVALID_SCHEMA,
                  "redirects must be a bounded object");
    for (index = 0; index < context->redirects->as.object.count; index++) {
      const AbObjectField *redirect =
          &context->redirects->as.object.fields[index];
      if (!redirect->name.length ||
          !ab_artifact_bounded_text(&redirect->value, 256, 1))
        return fail(context, ARCHBIRD_INVALID_SCHEMA,
                    "redirects must map non-empty names");
    }
  }
  return ARCHBIRD_OK;
}

static int constraint_selected(const AbPlanCompile *context,
                               const AbString *id) {
  size_t index;
  if (!context->selected_ids)
    return 1;
  for (index = 0; index < context->selected_ids->as.array.count; index++)
    if (ab_string_equal(id,
                        &context->selected_ids->as.array.items[index].as.text))
      return 1;
  return 0;
}

static ArchbirdStatus validate_selected_ids(AbPlanCompile *context) {
  size_t requested;
  size_t index;
  if (!context->selected_ids)
    return ARCHBIRD_OK;
  for (requested = 0; requested < context->selected_ids->as.array.count;
       requested++) {
    const AbString *wanted =
        &context->selected_ids->as.array.items[requested].as.text;
    int found = 0;
    for (index = 0; index < context->verification.constraints->as.array.count;
         index++) {
      const AbValue *id = field(
          &context->verification.constraints->as.array.items[index], "id");
      if (id && id->kind == AB_VALUE_STRING &&
          ab_string_equal(wanted, &id->as.text)) {
        found = 1;
        break;
      }
    }
    if (!found)
      return fail(context, ARCHBIRD_INVALID_SCHEMA,
                  "request names an unknown constraint");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_input_binding(AbPlanCompile *context,
                                             const uint8_t *map_json,
                                             size_t map_length) {
  const AbValue *evidence = field(&context->map, "evidence");
  const AbValue *map_tool = field(&context->map, "tool");
  const AbValue *evaluation = context->verification.evaluation;
  const AbValue *policy = context->verification.policy;
  const AbValue *verification_tool = context->verification.tool;
  const AbValue *verification_project = field(evaluation, "project");
  const char *project_input =
      archbird_project_map_input_sha256(context->project);
  const char *project_config = archbird_project_config_sha256(context->project);
  char map_sha[65];
  ArchbirdStatus status;
  if (!evidence || !map_tool || !evaluation || !policy || !verification_tool ||
      !context->map_project || context->map_project->kind != AB_VALUE_STRING ||
      !verification_project ||
      !value_string_equal(context->map_project, verification_project))
    return fail(context, ARCHBIRD_CONFLICT,
                "Map, project, and Verification identities differ");
  status =
      ab_artifact_json_sha256(context->engine, map_json, map_length, map_sha);
  if (status != ARCHBIRD_OK)
    return status;
  if (!project_input || !project_config ||
      !ab_artifact_text_is(field(policy, "kind"), "all") ||
      !ab_artifact_sha256(
          field(&context->verification.root, "verification_result_sha256")) ||
      !ab_artifact_sha256(field(policy, "constraint_policy_sha256")) ||
      !ab_artifact_sha256(field(map_tool, "implementation_sha256")) ||
      !ab_artifact_sha256(field(verification_tool, "implementation_sha256")) ||
      !ab_artifact_text_is(field(&context->map, "artifact"), "map") ||
      !ab_artifact_text_is(field(&context->verification.root, "artifact"),
                           "verification"))
    return fail(context, ARCHBIRD_CONFLICT,
                "Plan inputs are not complete canonical artifacts");
  if (memcmp(project_input, field(evidence, "input_sha256")->as.text.data,
             64) != 0 ||
      memcmp(project_config, field(evidence, "config_sha256")->as.text.data,
             64) != 0 ||
      !value_string_equal(field(evaluation, "map_input_sha256"),
                          field(evidence, "input_sha256")) ||
      !value_string_equal(field(evaluation, "map_config_sha256"),
                          field(evidence, "config_sha256")) ||
      !value_string_equal(
          field(evaluation, "map_producer_implementation_sha256"),
          field(map_tool, "implementation_sha256")))
    return fail(context, ARCHBIRD_CONFLICT,
                "Project, Map, and Verification source bytes differ");
  (void)map_sha;
  return ARCHBIRD_OK;
}

static int map_has_error_diagnostic(const AbValue *map) {
  const AbValue *diagnostics = field(map, "diagnostics");
  size_t index;
  if (!diagnostics || diagnostics->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < diagnostics->as.array.count; index++)
    if (ab_artifact_text_is(
            field(&diagnostics->as.array.items[index], "severity"), "error"))
      return 1;
  return 0;
}

static ArchbirdStatus validate_before_map(AbPlanCompile *context) {
  const AbValue *before_project;
  const AbValue *before_evidence;
  const AbValue *before_tool;
  const AbValue *current_evidence;
  const AbValue *current_tool;
  if (!context->before_map.kind)
    return ARCHBIRD_OK;
  before_project = field(&context->before_map, "project");
  before_evidence = field(&context->before_map, "evidence");
  before_tool = field(&context->before_map, "tool");
  current_evidence = field(&context->map, "evidence");
  current_tool = field(&context->map, "tool");
  if (!before_project || !before_evidence || !before_tool ||
      !value_string_equal(before_project, context->map_project) ||
      !value_string_equal(field(before_evidence, "config_sha256"),
                          field(current_evidence, "config_sha256")) ||
      !value_string_equal(field(before_tool, "implementation_sha256"),
                          field(current_tool, "implementation_sha256")))
    return fail(context, ARCHBIRD_CONFLICT,
                "before and current Maps have incompatible identities");
  if (map_has_error_diagnostic(&context->before_map) ||
      map_has_error_diagnostic(&context->map))
    return fail(context, ARCHBIRD_CONFLICT,
                "observed planning requires Maps without error diagnostics");
  return ARCHBIRD_OK;
}

static ArchbirdStatus source_lock(AbPlanCompile *context, const AbString *path,
                                  const char **out_sha,
                                  ArchbirdSourceView *out_source) {
  AbPlanSourceLock lock;
  ArchbirdStatus status = ab_plan_source_lock(context->engine, context->project,
                                              &context->map, path, &lock);
  if (status == ARCHBIRD_OK) {
    *out_sha = lock.sha256->as.text.data;
    *out_source = lock.source;
  }
  return status;
}

static const AbValue *
constraint_actual_definition(const AbPlanCompile *context,
                             const AbValue *constraint,
                             const AbProjectionData **out_fact) {
  const AbValue *operands = field(constraint, "operands");
  const AbValue *actual = field(operands, "actual");
  if (out_fact)
    *out_fact = NULL;
  if (!actual || actual->kind != AB_VALUE_STRING)
    return NULL;
  if (out_fact)
    ab_verification_artifact_fact_value(&context->verification,
                                        &actual->as.text, out_fact);
  return ab_verification_artifact_operand_definition(&context->verification,
                                                     &actual->as.text);
}

static const char *constraint_form(const AbValue *constraint,
                                   const AbValue *definition) {
  const AbValue *select = field(definition, "select");
  const AbValue *assertion = field(constraint, "assert");
  const AbValue *operands = field(constraint, "operands");
  if (!select || !assertion)
    return "unsupported";
  if (ab_artifact_text_is(select, "inventory_paths") &&
      ab_artifact_text_is(assertion, "cardinality")) {
    const AbValue *exact = field(operands, "exact");
    uint64_t value;
    if (exact && ab_value_u64(exact, &value) && value == 0)
      return "forbidden_paths";
  }
  if (ab_artifact_text_is(select, "mapped_paths") &&
      ab_artifact_text_is(assertion, "required_subset"))
    return "required_paths";
  if (ab_artifact_text_is(select, "symbols") &&
      ab_artifact_text_is(assertion, "required_subset"))
    return "required_symbols";
  if (ab_artifact_text_is(select, "symbols"))
    return "symbol_set";
  if (ab_artifact_text_is(select, "component_membership"))
    return "component_membership";
  if (ab_artifact_text_is(select, "file_metrics"))
    return "max_file_bytes";
  if (ab_artifact_text_is(select, "component_edges"))
    return "component_edges";
  if (ab_artifact_text_is(select, "file_edges"))
    return "file_edges";
  if (ab_artifact_text_is(select, "package_entrypoints"))
    return "package_entrypoints";
  if (ab_artifact_text_is(select, "provider_surface"))
    return "provider_surface";
  if (ab_artifact_text_is(select, "test_routes"))
    return "test_routes";
  return "unsupported";
}

static const AbPlanFindingGroup *
finding_group_for_key(const AbPlanFindingGroups *groups, const AbString *key) {
  const AbPlanFindingGroup *match = NULL;
  size_t index;
  if (!groups || !key)
    return NULL;
  if (groups->count == 1)
    return &groups->groups[0];
  for (index = 0; index < groups->count; index++) {
    const AbPlanFindingGroup *group = &groups->groups[index];
    const AbValue *finding = group->representative;
    const AbValue *candidate = field(finding, "key");
    if (!candidate || candidate->kind != AB_VALUE_STRING ||
        !ab_string_equal(&candidate->as.text, key))
      continue;
    if (match)
      return NULL;
    match = group;
  }
  return match;
}

static ArchbirdStatus
render_string_array_from_evidence_paths(AbBuffer *buffer,
                                        const AbPlanFindingGroup *group) {
  size_t finding_index;
  int first = 1;
  ArchbirdStatus status = literal(buffer, "[");
  if (!group)
    return status == ARCHBIRD_OK ? literal(buffer, "]") : status;
  for (finding_index = 0; status == ARCHBIRD_OK && finding_index < group->count;
       finding_index++) {
    const AbValue *evidence = field(group->rows[finding_index], "evidence");
    size_t index;
    if (!evidence || evidence->kind != AB_VALUE_ARRAY)
      continue;
    for (index = 0; status == ARCHBIRD_OK && index < evidence->as.array.count;
         index++) {
      const AbValue *path = field(&evidence->as.array.items[index], "path");
      size_t prior_finding;
      int duplicate = 0;
      if (!ab_artifact_repository_path(path))
        continue;
      for (prior_finding = 0; !duplicate && prior_finding <= finding_index;
           prior_finding++) {
        const AbValue *prior_evidence =
            field(group->rows[prior_finding], "evidence");
        size_t prior;
        size_t limit =
            !prior_evidence || prior_evidence->kind != AB_VALUE_ARRAY ? 0
            : prior_finding == finding_index                          ? index
                                             : prior_evidence->as.array.count;
        for (prior = 0; !duplicate && prior < limit; prior++) {
          const AbValue *other =
              field(&prior_evidence->as.array.items[prior], "path");
          if (other && ab_value_equal(path, other))
            duplicate = 1;
        }
      }
      if (duplicate)
        continue;
      if (!first)
        status = literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(buffer, path);
      first = 0;
    }
  }
  if (status == ARCHBIRD_OK)
    status = literal(buffer, "]");
  return status;
}

static ArchbirdStatus manual_operation(AbPlanCompile *context,
                                       const AbPlanFindingGroup *group,
                                       const char *instructions,
                                       AbBuffer *out) {
  ArchbirdStatus status;
  ab_buffer_init(out, context->engine);
  status = literal(out, "{\"action\":\"manual\",\"candidate_paths\":");
  if (status == ARCHBIRD_OK)
    status = render_string_array_from_evidence_paths(out, group);
  if (status == ARCHBIRD_OK)
    status = literal(out, ",\"instructions\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(out, instructions);
  if (status == ARCHBIRD_OK)
    status = literal(out, "}");
  return status;
}

static const char *manual_instructions(const char *form) {
  if (strcmp(form, "required_paths") == 0)
    return "Provide reviewed file content and an exact destination.";
  if (strcmp(form, "required_symbols") == 0)
    return "Provide the signature, implementation, and destination.";
  if (strcmp(form, "component_membership") == 0)
    return "Select an exact file move or an asserted component-path edit.";
  if (strcmp(form, "max_file_bytes") == 0)
    return "Select declarations to extract or simplify and their rewrites.";
  if (strcmp(form, "component_edges") == 0 || strcmp(form, "file_edges") == 0)
    return "Select a replacement dependency route and exact source rewrites.";
  if (strcmp(form, "package_entrypoints") == 0)
    return "Select the package entrypoint edit and its reviewed target.";
  if (strcmp(form, "provider_surface") == 0)
    return "Provide the reviewed bridge or registration transformation.";
  if (strcmp(form, "test_routes") == 0)
    return "Provide the reviewed test route or test template.";
  return "Review the Verification evidence and provide a source-locked "
         "transformation.";
}

static const char *manual_reason(const char *form) {
  if (strcmp(form, "required_paths") == 0)
    return "Verification requires a path but does not define its content.";
  if (strcmp(form, "required_symbols") == 0)
    return "Verification requires a symbol but does not define its code.";
  if (strcmp(form, "component_membership") == 0)
    return "Component evidence does not uniquely determine a file move or "
           "policy edit.";
  if (strcmp(form, "max_file_bytes") == 0)
    return "A file-size violation does not determine a behavior-preserving "
           "edit.";
  if (strcmp(form, "component_edges") == 0 || strcmp(form, "file_edges") == 0)
    return "Dependency evidence does not identify the intended replacement "
           "route.";
  if (strcmp(form, "package_entrypoints") == 0)
    return "Entrypoint evidence does not provide an exact manifest edit.";
  if (strcmp(form, "provider_surface") == 0)
    return "Provider evidence does not define bridge code or registration "
           "syntax.";
  if (strcmp(form, "test_routes") == 0)
    return "Route evidence does not define test code or registration syntax.";
  return "This constraint form has no deterministic Plan operator.";
}

static ArchbirdStatus append_manual(AbPlanCompile *context,
                                    const AbValue *constraint,
                                    const AbPlanFindingGroup *group,
                                    const char *form,
                                    const char *override_reason) {
  const AbValue *finding = group ? group->representative : NULL;
  const AbValue *id = field(constraint, "id");
  const AbValue *key = field(finding, "key");
  AbBuffer operation;
  char statement[1024];
  const char *reason = override_reason ? override_reason : manual_reason(form);
  const char *reasons[1];
  AbPlanItemSpec spec;
  int length;
  ArchbirdStatus status;
  if (!id || id->kind != AB_VALUE_STRING)
    return fail(context, ARCHBIRD_CONFLICT, "constraint has no identifier");
  length = snprintf(
      statement, sizeof(statement), "Resolve %.*s%s%.*s.",
      (int)id->as.text.length, id->as.text.data,
      key && key->kind == AB_VALUE_STRING ? ": " : "",
      key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
      key && key->kind == AB_VALUE_STRING ? key->as.text.data : "");
  if (length < 0 || (size_t)length >= sizeof(statement))
    return fail(context, ARCHBIRD_LIMIT_EXCEEDED,
                "generated item statement is too long");
  status =
      manual_operation(context, group, manual_instructions(form), &operation);
  reasons[0] = reason;
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = group ? group->rows : NULL;
  spec.finding_count = group ? group->count : 0;
  spec.statement = statement;
  spec.provenance = "derived";
  spec.operation = &operation;
  spec.executable = 0;
  spec.reasons = reasons;
  spec.reason_count = 1;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(&context->builder, &spec);
  ab_buffer_free(&operation);
  return status;
}

static ArchbirdStatus prepare_destructive_graph(AbPlanCompile *context) {
  static const uint8_t definition_json[] =
      "{\"id\":\"plan-destructive-relations\",\"level\":\"file\","
      "\"relations\":[\"builds\",\"bridges\",\"calls\",\"declarations\","
      "\"imports\",\"packages\",\"references\",\"tests\"],"
      "\"select\":\"graph\"}";
  AbValue definition = {0};
  const AbValue *id;
  ArchbirdStatus status;
  if (context->destructive_ready)
    return ARCHBIRD_OK;
  status = ab_json_value_decode(context->engine, definition_json,
                                sizeof(definition_json) - 1, &definition);
  id = status == ARCHBIRD_OK ? field(&definition, "id") : NULL;
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_compile(
        context->engine, &definition, &id->as.text, &context->destructive_plan);
  if (status == ARCHBIRD_OK)
    status = ab_projection_plan_evaluate(
        context->engine, &context->destructive_plan, &context->map, NULL,
        &context->destructive_result);
  ab_value_free(context->engine, &definition);
  if (status == ARCHBIRD_OK)
    context->destructive_ready = 1;
  return status;
}

static const AbValue *item_attribute(const AbProjectionItem *item,
                                     const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0)
      return &item->attributes[index].value;
  return NULL;
}

static int destructive_graph_complete(const AbPlanCompile *context) {
  const AbProjectionData *fact = &context->destructive_result.data;
  return strcmp(ab_projection_data_classification(fact), "complete") == 0 &&
         fact->selection.has_truncated && !fact->selection.truncated;
}

static int path_has_consumers(const AbPlanCompile *context,
                              const AbString *path) {
  const AbProjectionData *fact = &context->destructive_result.data;
  size_t index;
  for (index = 0; index < fact->item_count; index++) {
    const AbProjectionItem *item = &fact->items[index];
    const AbValue *record_kind = item_attribute(item, "record_kind");
    const AbValue *source = item_attribute(item, "source");
    const AbValue *target = item_attribute(item, "target");
    const char prefix[] = "file:";
    if (!ab_artifact_text_is(record_kind, "relation") || !source || !target ||
        source->kind != AB_VALUE_STRING || target->kind != AB_VALUE_STRING ||
        target->as.text.length != sizeof(prefix) - 1 + path->length ||
        memcmp(target->as.text.data, prefix, sizeof(prefix) - 1) != 0 ||
        memcmp(target->as.text.data + sizeof(prefix) - 1, path->data,
               path->length) != 0)
      continue;
    if (!ab_value_equal(source, target))
      return 1;
  }
  return 0;
}

static ArchbirdStatus append_forbidden_paths(AbPlanCompile *context,
                                             const AbValue *constraint,
                                             const AbProjectionData *actual) {
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  const AbPlanFindingGroup *first_group;
  size_t index;
  ArchbirdStatus status =
      ab_plan_finding_groups_collect(context->engine, findings, &groups);
  if (status != ARCHBIRD_OK)
    return status;
  first_group = groups.count ? &groups.groups[0] : NULL;
  if (!actual || !string_equal_literal(&actual->state, "current") ||
      !string_equal_literal(&actual->shape, "set") ||
      strcmp(ab_projection_data_classification(actual), "complete") != 0 ||
      !actual->selection.has_truncated || actual->selection.truncated) {
    status = append_manual(
        context, constraint, first_group, "forbidden_paths",
        "Forbidden-path evidence is not current, complete, exhaustive, and "
        "untruncated.");
    goto cleanup;
  }
  if (!actual->item_count) {
    status = append_manual(
        context, constraint, first_group, "forbidden_paths",
        "The failing forbidden-path constraint has no concrete projected "
        "path.");
    goto cleanup;
  }
  status = prepare_destructive_graph(context);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  for (index = 0; index < actual->item_count; index++) {
    const AbProjectionItem *item = &actual->items[index];
    const AbPlanFindingGroup *group =
        finding_group_for_key(&groups, &item->key);
    const AbValue *finding = group ? group->representative : NULL;
    const char *sha = NULL;
    ArchbirdSourceView source;
    AbBuffer operation;
    char statement[1024];
    int length;
    const char *reason = NULL;
    if (!finding) {
      status = fail(context, ARCHBIRD_CONFLICT,
                    "forbidden-path operand and finding identities do not "
                    "correspond");
      goto cleanup;
    }
    if (!string_equal_literal(&item->state, "current") ||
        !ab_artifact_repository_path(
            &(AbValue){.kind = AB_VALUE_STRING, .as.text = item->key})) {
      status = append_manual(context, constraint, group, "forbidden_paths",
                             "Forbidden-path projection contains a non-current "
                             "or non-concrete item.");
      if (status != ARCHBIRD_OK)
        goto cleanup;
      continue;
    }
    status = source_lock(context, &item->key, &sha, &source);
    if (status != ARCHBIRD_OK)
      goto cleanup;
    if (!destructive_graph_complete(context))
      reason = "Destructive relation evidence is not complete and exhaustive.";
    else if (path_has_consumers(context, &item->key))
      reason = "Known consumers require a reviewed rewrite before deleting "
               "this path.";
    ab_buffer_init(&operation, context->engine);
    if (reason) {
      status =
          literal(&operation, "{\"action\":\"manual\",\"candidate_paths\":[");
      if (status == ARCHBIRD_OK)
        status = json_string(&operation, &item->key);
      if (status == ARCHBIRD_OK)
        status =
            literal(&operation, "],\"instructions\":\"Rewrite every consumer, "
                                "then remove the source-locked path.\"}");
    } else {
      status = literal(&operation, "{\"action\":\"delete_file\",\"path\":");
      if (status == ARCHBIRD_OK)
        status = json_string(&operation, &item->key);
      if (status == ARCHBIRD_OK)
        status = literal(&operation, ",\"source_sha256\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(&operation, sha, 64);
      if (status == ARCHBIRD_OK)
        status = literal(&operation, "}");
    }
    length = snprintf(statement, sizeof(statement), "%s forbidden path %.*s.",
                      reason ? "Review removal of" : "Delete",
                      (int)item->key.length, item->key.data);
    if (status == ARCHBIRD_OK &&
        (length < 0 || (size_t)length >= sizeof(statement)))
      status = fail(context, ARCHBIRD_LIMIT_EXCEEDED,
                    "generated path statement is too long");
    if (status == ARCHBIRD_OK) {
      const char *reasons[] = {reason};
      AbPlanItemSpec spec = {
          .constraint = constraint,
          .findings = group->rows,
          .finding_count = group->count,
          .statement = statement,
          .provenance = "derived",
          .operation = &operation,
          .executable = reason == NULL,
          .reasons = reason ? reasons : NULL,
          .reason_count = reason ? 1 : 0,
      };
      status = ab_plan_item_builder_append(&context->builder, &spec);
    }
    ab_buffer_free(&operation);
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
cleanup:
  ab_plan_finding_groups_free(context->engine, &groups);
  return status;
}

static ArchbirdStatus compile_constraint(AbPlanCompile *context,
                                         const AbValue *constraint) {
  const AbValue *id = field(constraint, "id");
  const AbValue *status_value = field(constraint, "status");
  const AbProjectionData *actual = NULL;
  const AbValue *definition =
      constraint_actual_definition(context, constraint, &actual);
  const char *form = constraint_form(constraint, definition);
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  size_t index;
  int handled = 0;
  ArchbirdStatus status;
  if (!id || id->kind != AB_VALUE_STRING ||
      !constraint_selected(context, &id->as.text))
    return ARCHBIRD_OK;
  if (ab_artifact_text_is(status_value, "pass") ||
      ab_artifact_text_is(status_value, "waived") ||
      ab_artifact_text_is(status_value, "not_applicable"))
    return ARCHBIRD_OK;
  status = ab_plan_compile_symbol_constraint(
      context->engine, context->project, &context->map, &context->verification,
      &context->builder, constraint, definition, actual, context->renames,
      context->rename_used, &handled);
  if (status != ARCHBIRD_OK || handled)
    return status;
  status = ab_plan_compile_edge_constraint(
      context->engine, context->project, &context->map, &context->builder,
      constraint, definition, actual, context->redirects,
      context->redirect_used, &handled);
  if (status != ARCHBIRD_OK || handled)
    return status;
  status = ab_plan_compile_surface_constraint(
      context->engine, context->project, &context->map,
      context->before_map.kind ? &context->before_map : NULL, &context->builder,
      constraint, definition, context->renames, context->rename_used, &handled);
  if (status != ARCHBIRD_OK || handled)
    return status;
  if (strcmp(form, "forbidden_paths") == 0)
    return append_forbidden_paths(context, constraint, actual);
  if (!findings || findings->kind != AB_VALUE_ARRAY ||
      !findings->as.array.count)
    return append_manual(context, constraint, NULL, form, NULL);
  status = ab_plan_finding_groups_collect(context->engine, findings, &groups);
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++) {
    const AbValue *finding = groups.groups[index].representative;
    const char *reason =
        ab_plan_finding_current(finding)
            ? NULL
            : "Finding evidence is not current executable evidence.";
    status =
        append_manual(context, constraint, &groups.groups[index], form, reason);
  }
  ab_plan_finding_groups_free(context->engine, &groups);
  return status;
}

static int targeted_contains(const AbPlanCompile *context, const AbString *id) {
  return ab_plan_item_builder_targeted(&context->builder, id);
}

static ArchbirdStatus render_map_identity(AbPlanCompile *context,
                                          AbBuffer *output, const AbValue *map,
                                          const uint8_t *map_json,
                                          size_t map_length) {
  const AbValue *evidence = field(map, "evidence");
  const AbValue *map_tool = field(map, "tool");
  char map_sha[65];
  ArchbirdStatus status =
      ab_artifact_json_sha256(context->engine, map_json, map_length, map_sha);
  if (status == ARCHBIRD_OK)
    status = literal(output, "{\"configuration_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, field(evidence, "config_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"input_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, field(evidence, "input_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"producer_implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, field(map_tool, "implementation_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(output, map_sha, 64);
  if (status == ARCHBIRD_OK)
    status = literal(output, "}");
  return status;
}

static ArchbirdStatus render_source_identity(AbPlanCompile *context,
                                             AbBuffer *output,
                                             const uint8_t *map_json,
                                             size_t map_length,
                                             const uint8_t *before_map_json,
                                             size_t before_map_length) {
  const AbValue *policy = context->verification.policy;
  const AbValue *verification_tool = context->verification.tool;
  ArchbirdStatus status = literal(output, "{");
  if (status == ARCHBIRD_OK && context->before_map.kind)
    status = literal(output, "\"before_map\":");
  if (status == ARCHBIRD_OK && context->before_map.kind)
    status = render_map_identity(context, output, &context->before_map,
                                 before_map_json, before_map_length);
  if (status == ARCHBIRD_OK && context->before_map.kind)
    status = literal(output, ",");
  if (status == ARCHBIRD_OK)
    status = literal(output, "\"map\":");
  if (status == ARCHBIRD_OK)
    status = render_map_identity(context, output, &context->map, map_json,
                                 map_length);
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"project\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, context->map_project);
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"verification\":{\"policy_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, field(policy, "constraint_policy_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"producer_implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output,
                             field(verification_tool, "implementation_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, ",\"sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(output, field(&context->verification.root,
                                           "verification_result_sha256"));
  if (status == ARCHBIRD_OK)
    status = literal(output, "}}");
  return status;
}

static ArchbirdStatus render_plan(AbPlanCompile *context,
                                  const uint8_t *map_json, size_t map_length,
                                  const uint8_t *before_map_json,
                                  size_t before_map_length, uint32_t json_flags,
                                  ArchbirdWriteFn write_fn, void *user_data) {
  AbBuffer rendered;
  AbBuffer canonical;
  AbPlan validated = {0};
  size_t index;
  int first;
  ArchbirdStatus status;
  ab_buffer_init(&rendered, context->engine);
  ab_buffer_init(&canonical, context->engine);
  status = literal(&rendered, "{\"artifact\":\"plan\",\"items\":[");
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_render_items(&context->builder, &rendered);
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, "],\"objective\":");
  if (status == ARCHBIRD_OK) {
    if (context->objective)
      status = ab_value_render(&rendered, context->objective);
    else if ((context->renames && context->renames->as.object.count) ||
             (context->redirects && context->redirects->as.object.count))
      status =
          literal(&rendered,
                  "\"Apply the reviewed transformations and satisfy selected "
                  "constraints without regressing preserved constraints.\"");
    else
      status =
          literal(&rendered, "\"Satisfy selected Verification constraints "
                             "without regressing preserved constraints.\"");
  }
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, ",\"preserved_constraints\":[");
  first = 1;
  for (index = 0; status == ARCHBIRD_OK &&
                  index < context->verification.constraints->as.array.count;
       index++) {
    const AbValue *id =
        field(&context->verification.constraints->as.array.items[index], "id");
    if (!id || id->kind != AB_VALUE_STRING ||
        targeted_contains(context, &id->as.text))
      continue;
    if (!first)
      status = literal(&rendered, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&rendered, id);
    first = 0;
  }
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, "],\"provenance\":");
  if (status == ARCHBIRD_OK)
    status = literal(
        &rendered,
        (context->renames && context->renames->as.object.count) ||
                (context->redirects && context->redirects->as.object.count)
            ? "\"asserted\""
            : "\"derived\"");
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, ",\"schema_version\":3,\"source\":");
  if (status == ARCHBIRD_OK)
    status = render_source_identity(context, &rendered, map_json, map_length,
                                    before_map_json, before_map_length);
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, ",\"tool\":{\"implementation_sha256\":");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_json_string(&rendered, archbird_implementation_sha256(), 64);
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, ",\"name\":\"archbird\",\"version\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&rendered, ARCHBIRD_VERSION);
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, "},\"unknowns\":[");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&rendered, context->builder.unknowns.data,
                              context->builder.unknowns.length);
  if (status == ARCHBIRD_OK)
    status = literal(&rendered, "]}");
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(context->engine, rendered.data,
                                        rendered.length, 0, write_buffer,
                                        &canonical);
  if (status == ARCHBIRD_OK)
    status = ab_plan_load(context->engine, canonical.data, canonical.length,
                          &validated);
  if (status == ARCHBIRD_OK)
    status = archbird_json_canonicalize(context->engine, canonical.data,
                                        canonical.length, json_flags, write_fn,
                                        user_data);
  ab_plan_free(context->engine, &validated);
  ab_buffer_free(&canonical);
  ab_buffer_free(&rendered);
  return status;
}

ArchbirdStatus
archbird_plan_compile(ArchbirdEngine *engine, const ArchbirdProject *project,
                      const uint8_t *map_json, size_t map_length,
                      const uint8_t *before_map_json, size_t before_map_length,
                      const uint8_t *verification_json,
                      size_t verification_length, const uint8_t *request_json,
                      size_t request_length, uint32_t json_flags,
                      ArchbirdWriteFn write_fn, void *user_data) {
  AbPlanCompile context;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !project || !map_json || !map_length ||
      (!before_map_json && before_map_length) || !verification_json ||
      !verification_length || (!request_json && request_length) || !write_fn ||
      (json_flags & ~(ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE)))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&context, 0, sizeof(context));
  context.engine = engine;
  context.project = project;
  status = ab_json_value_decode(engine, map_json, map_length, &context.map);
  if (status == ARCHBIRD_OK)
    status = ab_projection_map_validate(engine, &context.map, "Plan Map");
  if (status == ARCHBIRD_OK && before_map_length)
    status = ab_json_value_decode(engine, before_map_json, before_map_length,
                                  &context.before_map);
  if (status == ARCHBIRD_OK && before_map_length)
    status = ab_projection_map_validate(engine, &context.before_map,
                                        "Plan before Map");
  if (status == ARCHBIRD_OK && request_length)
    status = ab_json_value_decode(engine, request_json, request_length,
                                  &context.request);
  if (status == ARCHBIRD_OK)
    status = validate_request(&context);
  if (status == ARCHBIRD_OK)
    status = ab_verification_artifact_load(
        engine, verification_json, verification_length, &context.verification);
  context.map_project =
      status == ARCHBIRD_OK ? field(&context.map, "project") : NULL;
  context.map_files =
      status == ARCHBIRD_OK ? field(&context.map, "files") : NULL;
  if (status == ARCHBIRD_OK)
    status = validate_input_binding(&context, map_json, map_length);
  if (status == ARCHBIRD_OK)
    status = validate_before_map(&context);
  if (status == ARCHBIRD_OK)
    status = validate_selected_ids(&context);
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_init(&context.builder, engine,
                                       &context.verification);
  if (status == ARCHBIRD_OK && context.renames &&
      context.renames->as.object.count) {
    context.rename_used =
        (uint8_t *)ab_calloc(engine, context.renames->as.object.count, 1);
    if (!context.rename_used)
      status = fail(&context, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory tracking asserted renames");
  }
  if (status == ARCHBIRD_OK && context.redirects &&
      context.redirects->as.object.count) {
    context.redirect_used =
        (uint8_t *)ab_calloc(engine, context.redirects->as.object.count, 1);
    if (!context.redirect_used)
      status = fail(&context, ARCHBIRD_OUT_OF_MEMORY,
                    "out of memory tracking asserted redirects");
  }
  for (index = 0; status == ARCHBIRD_OK &&
                  index < context.verification.constraints->as.array.count;
       index++)
    status = compile_constraint(
        &context, &context.verification.constraints->as.array.items[index]);
  for (index = 0; status == ARCHBIRD_OK && context.renames &&
                  index < context.renames->as.object.count;
       index++)
    if (!context.rename_used[index])
      status = fail(&context, ARCHBIRD_INVALID_SCHEMA,
                    "asserted rename does not match one selected constraint");
  for (index = 0; status == ARCHBIRD_OK && context.redirects &&
                  index < context.redirects->as.object.count;
       index++)
    if (!context.redirect_used[index])
      status = fail(&context, ARCHBIRD_INVALID_SCHEMA,
                    "asserted redirect does not match one selected constraint");
  if (status == ARCHBIRD_OK)
    status = render_plan(&context, map_json, map_length, before_map_json,
                         before_map_length, json_flags, write_fn, user_data);
  ab_projection_result_free(engine, &context.destructive_result);
  ab_projection_plan_free(engine, &context.destructive_plan);
  ab_free(engine, context.rename_used);
  ab_free(engine, context.redirect_used);
  ab_plan_item_builder_free(&context.builder);
  ab_verification_artifact_free(&context.verification);
  ab_value_free(engine, &context.request);
  ab_value_free(engine, &context.before_map);
  ab_value_free(engine, &context.map);
  return status;
}
