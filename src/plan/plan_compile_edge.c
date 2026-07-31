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

static ArchbirdStatus
append_edge_item(ArchbirdEngine *engine, const ArchbirdProject *project,
                 const AbValue *map, AbPlanItemBuilder *builder,
                 const AbValue *constraint, const AbPlanFindingGroup *group,
                 const AbProjectionData *actual) {
  const AbValue *finding = group->representative;
  const AbProjectionItem *relation = relation_item(actual, finding);
  const AbValue *sites = item_attribute(relation, "sites");
  const AbValue *key = field(finding, "key");
  const char *reasons[5];
  char site_reason[256];
  size_t reason_count = 0;
  size_t site_count = 0;
  size_t zero_width = 0;
  AbBuffer candidate_sites;
  AbBuffer operation;
  AbPlanItemSpec spec;
  char statement[1024];
  int statement_length;
  ArchbirdStatus status;
  ab_buffer_init(&candidate_sites, engine);
  ab_buffer_init(&operation, engine);
  reasons[reason_count++] =
      "Dependency evidence does not identify the intended replacement route.";
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
  if (status == ARCHBIRD_OK)
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
  spec.provenance = "derived";
  spec.operation = &operation;
  spec.executable = 0;
  spec.reasons = reasons;
  spec.reason_count = reason_count;
  if (status == ARCHBIRD_OK)
    status = ab_plan_item_builder_append(builder, &spec);
  ab_buffer_free(&operation);
  ab_buffer_free(&candidate_sites);
  return status;
}

ArchbirdStatus ab_plan_compile_edge_constraint(
    ArchbirdEngine *engine, const ArchbirdProject *project, const AbValue *map,
    AbPlanItemBuilder *builder, const AbValue *constraint,
    const AbValue *definition, const AbProjectionData *actual,
    int *out_handled) {
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
  for (index = 0; status == ARCHBIRD_OK && index < groups.count; index++)
    status = append_edge_item(engine, project, map, builder, constraint,
                              &groups.groups[index], actual);
  ab_plan_finding_groups_free(engine, &groups);
  return status;
}
