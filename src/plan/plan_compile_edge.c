#include "plan_compile_internal.h"

#include "artifact_validation.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus literal(AbBuffer *buffer, const char *value) {
  return ab_buffer_append(buffer, value, strlen(value));
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

static int string_is(const AbString *value, const char *literal_value) {
  size_t length = strlen(literal_value);
  return value && value->length == length &&
         memcmp(value->data, literal_value, length) == 0;
}

static int projection_complete(const AbProjectionData *data) {
  return data && string_is(&data->state, "current") &&
         string_is(&data->shape, "relation") &&
         strcmp(ab_projection_data_classification(data), "complete") == 0 &&
         data->selection.has_truncated && !data->selection.truncated &&
         (!data->selection.has_unknown || !data->selection.unknown) &&
         (!data->selection.has_unsupported || !data->selection.unsupported);
}

static int portable_identifier(const AbString *value) {
  size_t index;
  if (!value || !value->length ||
      !((value->data[0] >= 'A' && value->data[0] <= 'Z') ||
        (value->data[0] >= 'a' && value->data[0] <= 'z') ||
        value->data[0] == '_'))
    return 0;
  for (index = 1; index < value->length; index++)
    if (!((value->data[index] >= 'A' && value->data[index] <= 'Z') ||
          (value->data[index] >= 'a' && value->data[index] <= 'z') ||
          (value->data[index] >= '0' && value->data[index] <= '9') ||
          value->data[index] == '_'))
      return 0;
  return 1;
}

static int literal_selector(const AbValue *value) {
  size_t index;
  if (!ab_artifact_bounded_text(value, 64u * 1024u, 1))
    return 0;
  for (index = 0; index < value->as.text.length; index++)
    if (strchr("*?[]{}", value->as.text.data[index]))
      return 0;
  return 1;
}

static const AbValue *exact_single_string(const AbValue *object,
                                          const char *name) {
  const AbValue *values = field(object, name);
  return values && values->kind == AB_VALUE_ARRAY &&
                 values->as.array.count == 1 &&
                 values->as.array.items[0].kind == AB_VALUE_STRING
             ? &values->as.array.items[0]
             : NULL;
}

static int map_has_file(const AbValue *map, const AbValue *path) {
  const AbValue *files = field(map, "files");
  size_t index;
  for (index = 0;
       files && files->kind == AB_VALUE_ARRAY && index < files->as.array.count;
       index++) {
    const AbValue *candidate = field(&files->as.array.items[index], "path");
    if (candidate && ab_value_equal(candidate, path))
      return 1;
  }
  return 0;
}

static const AbProjectionItem *relation_item(const AbProjectionData *actual,
                                             const AbValue *finding) {
  const AbValue *key = field(finding, "key");
  const AbProjectionItem *match = NULL;
  size_t index;
  if (!key || key->kind != AB_VALUE_STRING)
    return NULL;
  for (index = 0; actual && index < actual->item_count; index++)
    if (actual->items[index].label.length == key->as.text.length &&
        memcmp(actual->items[index].label.data, key->as.text.data,
               key->as.text.length) == 0) {
      if (match)
        return NULL;
      match = &actual->items[index];
    }
  return match;
}

static ArchbirdStatus render_candidate_paths(AbBuffer *operation,
                                             const AbPlanFindingGroup *group,
                                             const AbValue *sites) {
  size_t finding_index;
  size_t site_index;
  size_t written = 0;
  ArchbirdStatus status = literal(operation, "[");
  for (site_index = 0;
       status == ARCHBIRD_OK && sites && sites->kind == AB_VALUE_ARRAY &&
       site_index < sites->as.array.count;
       site_index++) {
    const AbValue *path = field(&sites->as.array.items[site_index], "path");
    size_t prior;
    int duplicate = 0;
    if (!ab_artifact_repository_path(path))
      continue;
    for (prior = 0; prior < site_index; prior++)
      if (ab_value_equal(path, field(&sites->as.array.items[prior], "path"))) {
        duplicate = 1;
        break;
      }
    if (duplicate)
      continue;
    if (written++)
      status = literal(operation, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(operation, path);
  }
  for (finding_index = 0; status == ARCHBIRD_OK && finding_index < group->count;
       finding_index++) {
    const AbValue *evidence = field(group->rows[finding_index], "evidence");
    size_t evidence_index;
    for (evidence_index = 0; status == ARCHBIRD_OK && evidence &&
                             evidence->kind == AB_VALUE_ARRAY &&
                             evidence_index < evidence->as.array.count;
         evidence_index++) {
      const AbValue *path =
          field(&evidence->as.array.items[evidence_index], "path");
      size_t prior_finding;
      size_t prior_evidence;
      size_t prior;
      int duplicate = 0;
      if (!ab_artifact_repository_path(path))
        continue;
      for (prior = 0; sites && sites->kind == AB_VALUE_ARRAY &&
                      prior < sites->as.array.count;
           prior++)
        if (ab_value_equal(path,
                           field(&sites->as.array.items[prior], "path"))) {
          duplicate = 1;
          break;
        }
      for (prior_finding = 0; !duplicate && prior_finding <= finding_index;
           prior_finding++) {
        const AbValue *prior_rows =
            field(group->rows[prior_finding], "evidence");
        size_t limit = !prior_rows || prior_rows->kind != AB_VALUE_ARRAY ? 0
                       : prior_finding == finding_index
                           ? evidence_index
                           : prior_rows->as.array.count;
        for (prior_evidence = 0;
             !duplicate && prior_rows && prior_rows->kind == AB_VALUE_ARRAY &&
             prior_evidence < limit;
             prior_evidence++)
          if (ab_value_equal(
                  path,
                  field(&prior_rows->as.array.items[prior_evidence], "path")))
            duplicate = 1;
      }
      if (duplicate)
        continue;
      if (written++)
        status = literal(operation, ",");
      if (status == ARCHBIRD_OK)
        status = ab_value_render(operation, path);
    }
  }
  if (status == ARCHBIRD_OK)
    status = literal(operation, "]");
  return status;
}

static ArchbirdStatus
render_candidate_sites(ArchbirdEngine *engine, const ArchbirdProject *project,
                       const AbValue *map, const AbValue *sites,
                       AbBuffer *output, size_t *out_count,
                       size_t *out_zero_width) {
  size_t index;
  size_t count = 0;
  size_t zero_width = 0;
  ArchbirdStatus status = literal(output, "[");
  if (!sites || sites->kind != AB_VALUE_ARRAY) {
    *out_count = 0;
    *out_zero_width = 0;
    return literal(output, "]");
  }
  for (index = 0; status == ARCHBIRD_OK && index < sites->as.array.count;
       index++) {
    const AbValue *site = &sites->as.array.items[index];
    const AbValue *fact_id = field(site, "fact_id");
    const AbValue *path = field(site, "path");
    const AbValue *line = field(site, "line");
    const AbValue *name = field(site, "name");
    const AbValue *span = field(site, "span");
    uint64_t start;
    uint64_t end;
    AbPlanSourceLock lock;
    if (!ab_artifact_bounded_text(fact_id, 64u * 1024u, 1) ||
        !ab_artifact_repository_path(path) ||
        !ab_artifact_safe_integer(line, NULL) ||
        !ab_artifact_bounded_text(name, 64u * 1024u, 1) || !span ||
        span->kind != AB_VALUE_OBJECT ||
        !ab_artifact_safe_integer(field(span, "start"), &start) ||
        !ab_artifact_safe_integer(field(span, "end"), &end) || start > end)
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "plan compilation: edge projection contains an invalid source site");
    if (start == end) {
      zero_width++;
      continue;
    }
    status = ab_plan_source_lock(engine, project, map, &path->as.text, &lock);
    if (status != ARCHBIRD_OK)
      break;
    if (end > lock.source.byte_length || end - start > 64u * 1024u)
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "plan compilation: edge projection source site is outside the "
          "source-lock or Plan limit");
    status = ab_utf8_validate(engine, lock.source.bytes + start,
                              (size_t)(end - start));
    if (status != ARCHBIRD_OK)
      break;
    if (count++)
      status = literal(output, ",");
    if (status == ARCHBIRD_OK)
      status = literal(output, "{\"before\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(
          output, (const char *)lock.source.bytes + (size_t)start,
          (size_t)(end - start));
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"end_byte\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(output, end);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"fact_id\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, fact_id);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"line\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, line);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"name\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, name);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, path);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"source_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, lock.sha256);
    if (status == ARCHBIRD_OK)
      status = literal(output, ",\"start_byte\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(output, start);
    if (status == ARCHBIRD_OK)
      status = literal(output, "}");
  }
  if (status == ARCHBIRD_OK)
    status = literal(output, "]");
  *out_count = count;
  *out_zero_width = zero_width;
  return status;
}

static int path_in_edge_sites(const AbValue *sites, const AbValue *path) {
  size_t index;
  for (index = 0;
       sites && sites->kind == AB_VALUE_ARRAY && index < sites->as.array.count;
       index++)
    if (ab_value_equal(field(&sites->as.array.items[index], "path"), path))
      return 1;
  return 0;
}

static ArchbirdStatus render_relation_paths(AbBuffer *output,
                                            const AbValue *sites) {
  size_t index;
  size_t written = 0;
  ArchbirdStatus status = literal(output, "[");
  for (index = 0;
       status == ARCHBIRD_OK && sites && sites->kind == AB_VALUE_ARRAY &&
       index < sites->as.array.count;
       index++) {
    const AbValue *path = field(&sites->as.array.items[index], "path");
    size_t prior;
    int duplicate = 0;
    if (!ab_artifact_repository_path(path))
      continue;
    for (prior = 0; prior < index; prior++)
      if (ab_value_equal(path, field(&sites->as.array.items[prior], "path"))) {
        duplicate = 1;
        break;
      }
    if (duplicate)
      continue;
    if (written++)
      status = literal(output, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(output, path);
  }
  if (status == ARCHBIRD_OK)
    status = literal(output, "]");
  return status;
}

static int redirect_mentions_edge(const AbValue *map,
                                  const AbProjectionItem *relation,
                                  const AbString *symbol) {
  const AbValue *sites = item_attribute(relation, "sites");
  const AbValue *calls = field(map, "symbol_calls");
  size_t index;
  for (index = 0;
       calls && calls->kind == AB_VALUE_ARRAY && index < calls->as.array.count;
       index++) {
    const AbValue *row = &calls->as.array.items[index];
    const AbValue *name = field(row, "name");
    const AbValue *source = field(row, "source");
    const AbValue *path = field(source, "path");
    if (name && name->kind == AB_VALUE_STRING &&
        ab_string_equal(&name->as.text, symbol) && path &&
        path->kind == AB_VALUE_STRING && path_in_edge_sites(sites, path))
      return 1;
  }
  return 0;
}

static ArchbirdStatus
append_edge_item(ArchbirdEngine *engine, const ArchbirdProject *project,
                 const AbValue *map, AbPlanItemBuilder *builder,
                 const AbValue *constraint, const AbPlanFindingGroup *group,
                 const AbValue *definition, const AbProjectionData *actual,
                 const AbValue *redirects, uint8_t *redirect_used) {
  const AbValue *finding = group->representative;
  const AbProjectionItem *relation = relation_item(actual, finding);
  const AbValue *sites = item_attribute(relation, "sites");
  const AbValue *key = field(finding, "key");
  const char *reasons[5];
  char site_reason[256];
  size_t reason_count = 0;
  size_t site_count = 0;
  size_t zero_width = 0;
  size_t redirect_index = 0;
  size_t redirect_matches = 0;
  const AbString *old_symbol = NULL;
  const AbString *new_symbol = NULL;
  AbBuffer candidate_sites;
  AbBuffer operation;
  AbPlanItemSpec spec;
  char statement[1024];
  int statement_length;
  ArchbirdStatus status;
  ab_buffer_init(&candidate_sites, engine);
  ab_buffer_init(&operation, engine);
  if (!ab_plan_finding_current(finding))
    reasons[reason_count++] =
        "Finding evidence is not current executable evidence.";
  if (!projection_complete(actual) || !relation ||
      !string_is(&relation->state, "current")) {
    reasons[reason_count++] =
        "The edge ProjectionResult is not complete, current, and exhaustive.";
    status = literal(&candidate_sites, "[]");
  } else {
    status = render_candidate_sites(engine, project, map, sites,
                                    &candidate_sites, &site_count, &zero_width);
    if (status == ARCHBIRD_OK && !site_count)
      reasons[reason_count++] =
          "The edge projection has no exact inducing source sites.";
    if (status == ARCHBIRD_OK && zero_width) {
      int length =
          snprintf(site_reason, sizeof(site_reason),
                   "%zu edge evidence anchor(s) have no editable source range.",
                   zero_width);
      if (length < 0 || (size_t)length >= sizeof(site_reason))
        status = ARCHBIRD_LIMIT_EXCEEDED;
      else
        reasons[reason_count++] = site_reason;
    }
  }
  if (status == ARCHBIRD_OK && relation && redirects &&
      redirects->kind == AB_VALUE_OBJECT) {
    size_t index;
    for (index = 0; index < redirects->as.object.count; index++) {
      const AbObjectField *row = &redirects->as.object.fields[index];
      if (row->value.kind == AB_VALUE_STRING &&
          redirect_mentions_edge(map, relation, &row->name)) {
        old_symbol = &row->name;
        new_symbol = &row->value.as.text;
        redirect_index = index;
        redirect_matches++;
      }
    }
  }
  if (status == ARCHBIRD_OK && redirect_matches > 1)
    status = archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "plan compilation: multiple asserted redirects match one edge issue");
  if (status == ARCHBIRD_OK && redirect_matches == 1 &&
      (!portable_identifier(old_symbol) || !portable_identifier(new_symbol) ||
       ab_string_equal(old_symbol, new_symbol)))
    status = archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "plan compilation: redirects require distinct portable identifiers");
  if (status == ARCHBIRD_OK && redirect_matches == 1 && !reason_count) {
    status = literal(&operation,
                     "{\"action\":\"redirect_dependency\",\"from_symbol\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&operation, old_symbol->data,
                                     old_symbol->length);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"projection\":");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&operation, definition);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"projection_id\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&operation, actual->name.data,
                                     actual->name.length);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"projection_content_sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&operation, actual->sha256, 64);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"relation\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&operation, relation->label.data,
                                     relation->label.length);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"source_paths\":");
    if (status == ARCHBIRD_OK)
      status = render_relation_paths(&operation, sites);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"to_symbol\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&operation, new_symbol->data,
                                     new_symbol->length);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, "}");
  } else if (status == ARCHBIRD_OK) {
    if (!redirect_matches)
      reasons[reason_count++] =
          "Dependency evidence does not identify the intended replacement "
          "route.";
    status = literal(&operation, "{\"action\":\"manual\",\"candidate_paths\":");
    if (status == ARCHBIRD_OK)
      status = render_candidate_paths(&operation, group, sites);
    if (status == ARCHBIRD_OK)
      status = literal(&operation, ",\"candidate_sites\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&operation, candidate_sites.data,
                                candidate_sites.length);
    if (status == ARCHBIRD_OK)
      status = literal(
          &operation,
          ",\"instructions\":\"Select a reviewed replacement dependency route "
          "and rewrite every exact inducing source site.\"}");
  }
  if (redirect_matches == 1)
    statement_length = snprintf(
        statement, sizeof(statement),
        "Redirect dependency %.*s from %.*s to %.*s.",
        key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
        key && key->kind == AB_VALUE_STRING ? key->as.text.data : "",
        (int)old_symbol->length, old_symbol->data, (int)new_symbol->length,
        new_symbol->data);
  else
    statement_length = snprintf(
        statement, sizeof(statement), "Redirect dependency %.*s.",
        key && key->kind == AB_VALUE_STRING ? (int)key->as.text.length : 0,
        key && key->kind == AB_VALUE_STRING ? key->as.text.data : "");
  if (status == ARCHBIRD_OK &&
      (statement_length < 0 || (size_t)statement_length >= sizeof(statement)))
    status = ARCHBIRD_LIMIT_EXCEEDED;
  memset(&spec, 0, sizeof(spec));
  spec.constraint = constraint;
  spec.findings = group->rows;
  spec.finding_count = group->count;
  spec.statement = statement;
  spec.provenance = redirect_matches == 1 ? "asserted" : "derived";
  spec.operation = &operation;
  spec.executable = redirect_matches == 1 && reason_count == 0;
  spec.reasons = reason_count ? reasons : NULL;
  spec.reason_count = reason_count;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  if (status == ARCHBIRD_OK && redirect_matches == 1)
    redirect_used[redirect_index] = 1;
  ab_buffer_free(&operation);
  ab_buffer_free(&candidate_sites);
  return status;
}

static ArchbirdStatus append_missing_edge_item(
    ArchbirdEngine *engine, const AbValue *map, AbPlanItemBuilder *builder,
    const AbValue *constraint, const AbValue *definition,
    const AbProjectionData *actual, const AbPlanFindingGroups *groups,
    int *out_supported) {
  const AbValue *assertion = field(constraint, "assert");
  const AbValue *operands = field(constraint, "operands");
  const AbValue *minimum = field(operands, "min");
  const AbValue *source = exact_single_string(definition, "from_paths");
  const AbValue *target = exact_single_string(definition, "to_paths");
  const AbValue *relation = exact_single_string(definition, "kind_patterns");
  const AbValue *name = exact_single_string(definition, "name_patterns");
  const AbPlanFindingGroup *group =
      groups && groups->count == 1 ? &groups->groups[0] : NULL;
  const AbValue *finding = group ? group->representative : NULL;
  uint64_t min_count = 0;
  AbBuffer operation;
  AbPlanItemSpec spec;
  const char *reasons[] = {
      "The required dependency does not define its source edit."};
  char statement[1024];
  int length;
  ArchbirdStatus status;
  *out_supported = 0;
  if (!ab_artifact_text_is(assertion, "cardinality") ||
      !ab_artifact_safe_integer(minimum, &min_count) || min_count != 1 ||
      !projection_complete(actual) || actual->item_count != 0 || !group ||
      !ab_plan_finding_current(finding) ||
      !ab_artifact_repository_literal_path(source) ||
      !ab_artifact_repository_literal_path(target) ||
      ab_value_equal(source, target) || !literal_selector(relation) ||
      (field(definition, "name_patterns") && !literal_selector(name)) ||
      !map_has_file(map, source) || !map_has_file(map, target))
    return ARCHBIRD_OK;
  ab_buffer_init(&operation, engine);
  status = literal(&operation, "{\"action\":\"add_dependency\",\"relation\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&operation, relation);
  if (status == ARCHBIRD_OK && name)
    status = literal(&operation, ",\"name\":");
  if (status == ARCHBIRD_OK && name)
    status = ab_value_render(&operation, name);
  if (status == ARCHBIRD_OK)
    status = literal(&operation, ",\"source_path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&operation, source);
  if (status == ARCHBIRD_OK)
    status = literal(&operation, ",\"target_path\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&operation, target);
  if (status == ARCHBIRD_OK)
    status = literal(&operation, "}");
  if (name)
    length = snprintf(statement, sizeof(statement),
                      "Add required %.*s dependency from %.*s to %.*s "
                      "(%.*s).",
                      (int)relation->as.text.length, relation->as.text.data,
                      (int)source->as.text.length, source->as.text.data,
                      (int)target->as.text.length, target->as.text.data,
                      (int)name->as.text.length, name->as.text.data);
  else
    length = snprintf(statement, sizeof(statement),
                      "Add required %.*s dependency from %.*s to %.*s.",
                      (int)relation->as.text.length, relation->as.text.data,
                      (int)source->as.text.length, source->as.text.data,
                      (int)target->as.text.length, target->as.text.data);
  if (status == ARCHBIRD_OK &&
      (length < 0 || (size_t)length >= sizeof(statement)))
    status = archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "plan compilation: required-edge statement is too long");
  if (status == ARCHBIRD_OK) {
    memset(&spec, 0, sizeof(spec));
    spec.constraint = constraint;
    spec.findings = group->rows;
    spec.finding_count = group->count;
    spec.statement = statement;
    spec.provenance = "derived";
    spec.operation = &operation;
    spec.executable = 0;
    spec.reasons = reasons;
    spec.reason_count = 1;
    status = ab_plan_item_builder_append(builder, &spec);
  }
  ab_buffer_free(&operation);
  if (status == ARCHBIRD_OK)
    *out_supported = 1;
  return status;
}

ArchbirdStatus ab_plan_compile_edge_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    AbPlanItemBuilder *builder, const AbValue *constraint,
    const AbValue *definition, const AbProjectionData *actual,
    const AbValue *redirects, uint8_t *redirect_used, int *out_handled) {
  const AbValue *select = field(definition, "select");
  const AbValue *findings = field(constraint, "findings");
  AbPlanFindingGroups groups = {0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_handled = 0;
  if (!ab_artifact_text_is(select, "component_edges") &&
      !ab_artifact_text_is(select, "file_edges"))
    return ARCHBIRD_OK;
  *out_handled = 1;
  if (!findings || findings->kind != AB_VALUE_ARRAY ||
      !findings->as.array.count)
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "plan compilation: failing edge constraint has no issue evidence");
  status = ab_plan_finding_groups_collect(engine, findings, &groups);
  if (status == ARCHBIRD_OK && ab_artifact_text_is(select, "file_edges")) {
    int supported = 0;
    status = append_missing_edge_item(engine, map, builder, constraint,
                                      definition, actual, &groups, &supported);
    if (status != ARCHBIRD_OK || supported) {
      ab_plan_finding_groups_free(engine, &groups);
      return status;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++)
    status = append_edge_item(engine, project, map, builder, constraint,
                              &groups.groups[index], definition, actual,
                              redirects, redirect_used);
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
