#include "plan_compile_internal.h"

#include "artifact_validation.h"

#include <stdlib.h>
#include <string.h>

typedef struct AbRenderedValue {
  AbBuffer bytes;
} AbRenderedValue;

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus invalid(AbPlanItemBuilder *builder, ArchbirdStatus status,
                              const char *message) {
  return archbird_error_set(builder->engine, status, ARCHBIRD_NO_OFFSET,
                            "plan compilation: %s", message);
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

static int rendered_compare(const void *left_raw, const void *right_raw) {
  const AbRenderedValue *left = (const AbRenderedValue *)left_raw;
  const AbRenderedValue *right = (const AbRenderedValue *)right_raw;
  size_t common = left->bytes.length < right->bytes.length
                      ? left->bytes.length
                      : right->bytes.length;
  int compared =
      common ? memcmp(left->bytes.data, right->bytes.data, common) : 0;
  if (compared)
    return compared;
  return left->bytes.length < right->bytes.length   ? -1
         : left->bytes.length > right->bytes.length ? 1
                                                    : 0;
}

static int rendered_equal(const AbRenderedValue *left,
                          const AbRenderedValue *right) {
  return left->bytes.length == right->bytes.length &&
         (!left->bytes.length ||
          memcmp(left->bytes.data, right->bytes.data, left->bytes.length) == 0);
}

static void rendered_values_free(AbRenderedValue *values, size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_buffer_free(&values[index].bytes);
}

static const AbValue *policy_identity(const AbPlanItemBuilder *builder,
                                      const AbString *id) {
  const AbValue *rows = field(builder->verification->policy, "constraints");
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *candidate = field(row, "id");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, id))
      return row;
  }
  return NULL;
}

static ArchbirdStatus add_targeted(AbPlanItemBuilder *builder,
                                   const AbString *id) {
  size_t index;
  for (index = 0; index < builder->targeted_count; index++)
    if (ab_string_equal(builder->targeted[index], id))
      return ARCHBIRD_OK;
  if (builder->targeted_count >= AB_PLAN_COMPILE_MAX_ROWS)
    return invalid(builder, ARCHBIRD_LIMIT_EXCEEDED,
                   "too many targeted constraints");
  builder->targeted[builder->targeted_count++] = id;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_origin(AbPlanItemBuilder *builder,
                                    const AbString *constraint_id,
                                    const AbValue *constraint_result_sha,
                                    const AbValue *finding,
                                    AbRenderedValue *out) {
  const AbValue *fingerprint = field(finding, "fingerprint");
  ArchbirdStatus status;
  ab_buffer_init(&out->bytes, builder->engine);
  if (finding && !ab_artifact_sha256(fingerprint))
    return invalid(builder, ARCHBIRD_CONFLICT,
                   "finding fingerprint is unavailable");
  status = literal(&out->bytes, "{\"constraint_id\":");
  if (status == ARCHBIRD_OK)
    status = json_string(&out->bytes, constraint_id);
  if (status == ARCHBIRD_OK)
    status = literal(&out->bytes, ",\"constraint_result_sha256\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&out->bytes, constraint_result_sha);
  if (status == ARCHBIRD_OK)
    status = literal(&out->bytes, ",\"issue_fingerprint\":");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&out->bytes,
                             finding ? fingerprint : constraint_result_sha);
  if (status == ARCHBIRD_OK)
    status = literal(&out->bytes, "}");
  return status;
}

static ArchbirdStatus collect_origins(AbPlanItemBuilder *builder,
                                      const AbString *constraint_id,
                                      const AbValue *constraint_result_sha,
                                      const AbPlanItemSpec *spec,
                                      AbRenderedValue **out_values,
                                      size_t *out_count) {
  AbRenderedValue *values;
  size_t source_count = spec->finding_count ? spec->finding_count : 1;
  size_t index;
  size_t retained;
  ArchbirdStatus status = ARCHBIRD_OK;
  values = (AbRenderedValue *)ab_calloc(builder->engine, source_count,
                                        sizeof(*values));
  if (!values)
    return invalid(builder, ARCHBIRD_OUT_OF_MEMORY,
                   "out of memory collecting Plan origins");
  for (index = 0; status == ARCHBIRD_OK && index < source_count; index++)
    status = render_origin(builder, constraint_id, constraint_result_sha,
                           spec->finding_count ? spec->findings[index] : NULL,
                           &values[index]);
  if (status == ARCHBIRD_OK && source_count > 1)
    qsort(values, source_count, sizeof(*values), rendered_compare);
  retained = 0;
  for (index = 0; status == ARCHBIRD_OK && index < source_count; index++) {
    if (retained && rendered_equal(&values[retained - 1], &values[index])) {
      ab_buffer_free(&values[index].bytes);
      continue;
    }
    if (retained != index)
      values[retained] = values[index];
    retained++;
  }
  if (status != ARCHBIRD_OK) {
    rendered_values_free(values, source_count);
    ab_free(builder->engine, values);
    return status;
  }
  *out_values = values;
  *out_count = retained;
  return ARCHBIRD_OK;
}

static ArchbirdStatus collect_evidence(AbPlanItemBuilder *builder,
                                       const AbPlanItemSpec *spec,
                                       AbRenderedValue **out_values,
                                       size_t *out_count) {
  AbRenderedValue *values;
  size_t count = 0;
  size_t finding_index;
  size_t index;
  size_t retained;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (finding_index = 0; finding_index < spec->finding_count;
       finding_index++) {
    const AbValue *evidence = field(spec->findings[finding_index], "evidence");
    if (evidence && evidence->kind == AB_VALUE_ARRAY) {
      if (evidence->as.array.count > AB_PLAN_COMPILE_MAX_ROWS - count)
        return invalid(builder, ARCHBIRD_LIMIT_EXCEEDED,
                       "too many Plan evidence rows");
      count += evidence->as.array.count;
    }
  }
  if (!count) {
    *out_values = NULL;
    *out_count = 0;
    return ARCHBIRD_OK;
  }
  values =
      (AbRenderedValue *)ab_calloc(builder->engine, count, sizeof(*values));
  if (!values)
    return invalid(builder, ARCHBIRD_OUT_OF_MEMORY,
                   "out of memory collecting Plan evidence");
  index = 0;
  for (finding_index = 0;
       status == ARCHBIRD_OK && finding_index < spec->finding_count;
       finding_index++) {
    const AbValue *evidence = field(spec->findings[finding_index], "evidence");
    size_t evidence_index;
    if (!evidence || evidence->kind != AB_VALUE_ARRAY)
      continue;
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
         evidence_index++, index++) {
      ab_buffer_init(&values[index].bytes, builder->engine);
      status = ab_value_render(&values[index].bytes,
                               &evidence->as.array.items[evidence_index]);
    }
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(values, count, sizeof(*values), rendered_compare);
  retained = 0;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (retained && rendered_equal(&values[retained - 1], &values[index])) {
      ab_buffer_free(&values[index].bytes);
      continue;
    }
    if (retained != index)
      values[retained] = values[index];
    retained++;
  }
  if (status != ARCHBIRD_OK) {
    rendered_values_free(values, count);
    ab_free(builder->engine, values);
    return status;
  }
  *out_values = values;
  *out_count = retained;
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_rows(AbBuffer *output,
                                  const AbRenderedValue *values, size_t count) {
  size_t index;
  ArchbirdStatus status = literal(output, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = literal(output, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(output, values[index].bytes.data,
                                values[index].bytes.length);
  }
  if (status == ARCHBIRD_OK)
    status = literal(output, "]");
  return status;
}

static ArchbirdStatus
operation_digest(AbPlanItemBuilder *builder, const AbString *constraint_id,
                 const AbRenderedValue *origins, size_t origin_count,
                 const AbBuffer *operation, char out[65]) {
  AbBuffer identity;
  ArchbirdStatus status;
  ab_buffer_init(&identity, builder->engine);
  status = literal(&identity, "{\"constraint_id\":");
  if (status == ARCHBIRD_OK)
    status = json_string(&identity, constraint_id);
  if (status == ARCHBIRD_OK)
    status = literal(&identity, ",\"operation\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&identity, operation->data, operation->length);
  if (status == ARCHBIRD_OK)
    status = literal(&identity, ",\"origins\":");
  if (status == ARCHBIRD_OK)
    status = render_rows(&identity, origins, origin_count);
  if (status == ARCHBIRD_OK)
    status = literal(&identity, "}");
  if (status == ARCHBIRD_OK)
    status = ab_artifact_json_sha256(builder->engine, identity.data,
                                     identity.length, out);
  ab_buffer_free(&identity);
  return status;
}

static ArchbirdStatus unknown_digest(AbPlanItemBuilder *builder,
                                     const char item_id[26], const char *reason,
                                     char out[65]) {
  AbBuffer identity;
  ArchbirdStatus status;
  ab_buffer_init(&identity, builder->engine);
  status = literal(&identity, "{\"item_id\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&identity, item_id);
  if (status == ARCHBIRD_OK)
    status = literal(&identity, ",\"statement\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&identity, reason);
  if (status == ARCHBIRD_OK)
    status = literal(&identity, "}");
  if (status == ARCHBIRD_OK)
    status = ab_artifact_json_sha256(builder->engine, identity.data,
                                     identity.length, out);
  ab_buffer_free(&identity);
  return status;
}

static ArchbirdStatus append_unknown(AbPlanItemBuilder *builder,
                                     const char item_id[26],
                                     const AbString *constraint_id,
                                     const char *reason, char unknown_id[29]) {
  char digest[65];
  ArchbirdStatus status = unknown_digest(builder, item_id, reason, digest);
  if (status != ARCHBIRD_OK)
    return status;
  memcpy(unknown_id, "unknown-", 8);
  memcpy(unknown_id + 8, digest, 20);
  unknown_id[28] = '\0';
  if (!builder->first_unknown)
    status = literal(&builder->unknowns, ",");
  if (status == ARCHBIRD_OK)
    status = literal(&builder->unknowns, "{\"constraint_id\":");
  if (status == ARCHBIRD_OK)
    status = json_string(&builder->unknowns, constraint_id);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->unknowns, ",\"id\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->unknowns, unknown_id);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->unknowns, ",\"item_id\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->unknowns, item_id);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->unknowns, ",\"statement\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->unknowns, reason);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->unknowns, "}");
  if (status == ARCHBIRD_OK) {
    builder->first_unknown = 0;
    builder->unknown_count++;
  }
  return status;
}

static size_t unique_reason_count(const AbPlanItemSpec *spec) {
  size_t index;
  size_t prior;
  size_t count = 0;
  for (index = 0; index < spec->reason_count; index++) {
    int duplicate = 0;
    if (!spec->reasons[index] || !*spec->reasons[index])
      continue;
    for (prior = 0; prior < index; prior++)
      if (spec->reasons[prior] &&
          strcmp(spec->reasons[prior], spec->reasons[index]) == 0) {
        duplicate = 1;
        break;
      }
    if (!duplicate)
      count++;
  }
  return count;
}

ArchbirdStatus
ab_plan_item_builder_init(AbPlanItemBuilder *builder, ArchbirdEngine *engine,
                          const AbVerificationArtifact *verification) {
  if (!builder || !engine || !verification)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(builder, 0, sizeof(*builder));
  builder->engine = engine;
  builder->verification = verification;
  builder->first_item = 1;
  builder->first_unknown = 1;
  ab_buffer_init(&builder->items, engine);
  ab_buffer_init(&builder->unknowns, engine);
  builder->targeted = (const AbString **)ab_calloc(
      engine, AB_PLAN_COMPILE_MAX_ROWS, sizeof(*builder->targeted));
  if (!builder->targeted) {
    ab_plan_item_builder_free(builder);
    return archbird_error_set(
        engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "plan compilation: out of memory collecting targeted constraints");
  }
  return ARCHBIRD_OK;
}

void ab_plan_item_builder_free(AbPlanItemBuilder *builder) {
  if (!builder)
    return;
  ab_free(builder->engine, builder->targeted);
  ab_buffer_free(&builder->unknowns);
  ab_buffer_free(&builder->items);
  memset(builder, 0, sizeof(*builder));
}

int ab_plan_item_builder_targeted(const AbPlanItemBuilder *builder,
                                  const AbString *constraint_id) {
  size_t index;
  for (index = 0; builder && index < builder->targeted_count; index++)
    if (ab_string_equal(builder->targeted[index], constraint_id))
      return 1;
  return 0;
}

ArchbirdStatus ab_plan_item_builder_append(AbPlanItemBuilder *builder,
                                           const AbPlanItemSpec *spec) {
  const AbValue *id_value;
  const AbValue *identity;
  const AbValue *result_sha;
  AbRenderedValue *origins = NULL;
  AbRenderedValue *evidence = NULL;
  char (*unknown_ids)[29] = NULL;
  char operation_sha[65];
  char item_id[26];
  size_t origin_count = 0;
  size_t evidence_count = 0;
  size_t reason_count;
  size_t reason_index;
  size_t unique_index = 0;
  ArchbirdStatus status;
  if (!builder || !spec || !spec->constraint || !spec->statement ||
      !spec->provenance || !spec->operation ||
      (spec->finding_count && !spec->findings) ||
      (spec->reason_count && !spec->reasons))
    return ARCHBIRD_INVALID_ARGUMENT;
  id_value = field(spec->constraint, "id");
  identity = id_value && id_value->kind == AB_VALUE_STRING
                 ? policy_identity(builder, &id_value->as.text)
                 : NULL;
  result_sha = field(identity, "constraint_result_sha256");
  if (!id_value || id_value->kind != AB_VALUE_STRING ||
      !ab_artifact_sha256(result_sha))
    return invalid(builder, ARCHBIRD_CONFLICT,
                   "constraint result identity is unavailable");
  reason_count = unique_reason_count(spec);
  if (spec->executable && reason_count)
    return invalid(builder, ARCHBIRD_CONFLICT,
                   "executable item has non-executable reasons");
  if (!spec->executable && !reason_count)
    return invalid(builder, ARCHBIRD_CONFLICT,
                   "non-executable item has no reason");
  status = collect_origins(builder, &id_value->as.text, result_sha, spec,
                           &origins, &origin_count);
  if (status == ARCHBIRD_OK)
    status = collect_evidence(builder, spec, &evidence, &evidence_count);
  if (status == ARCHBIRD_OK)
    status = operation_digest(builder, &id_value->as.text, origins,
                              origin_count, spec->operation, operation_sha);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  memcpy(item_id, "plan-", 5);
  memcpy(item_id + 5, operation_sha, 20);
  item_id[25] = '\0';
  if (reason_count) {
    unknown_ids = (char (*)[29])ab_calloc(builder->engine, reason_count,
                                          sizeof(*unknown_ids));
    if (!unknown_ids) {
      status = invalid(builder, ARCHBIRD_OUT_OF_MEMORY,
                       "out of memory collecting Plan unknowns");
      goto cleanup;
    }
    for (reason_index = 0;
         status == ARCHBIRD_OK && reason_index < spec->reason_count;
         reason_index++) {
      size_t prior;
      int duplicate = 0;
      const char *reason = spec->reasons[reason_index];
      if (!reason || !*reason)
        continue;
      for (prior = 0; prior < reason_index; prior++)
        if (spec->reasons[prior] && strcmp(spec->reasons[prior], reason) == 0) {
          duplicate = 1;
          break;
        }
      if (duplicate)
        continue;
      status = append_unknown(builder, item_id, &id_value->as.text, reason,
                              unknown_ids[unique_index++]);
    }
  }
  if (status != ARCHBIRD_OK)
    goto cleanup;
  if (!builder->first_item)
    status = literal(&builder->items, ",");
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, "{\"acceptance\":{\"constraints\":[");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&builder->items, id_value);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, "]},\"depends_on\":[],\"evidence\":");
  if (status == ARCHBIRD_OK)
    status = render_rows(&builder->items, evidence, evidence_count);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"executable\":");
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, spec->executable ? "true" : "false");
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"id\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->items, item_id);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"non_executable_reasons\":[");
  unique_index = 0;
  for (reason_index = 0;
       status == ARCHBIRD_OK && reason_index < spec->reason_count;
       reason_index++) {
    size_t prior;
    int duplicate = 0;
    const char *reason = spec->reasons[reason_index];
    if (!reason || !*reason)
      continue;
    for (prior = 0; prior < reason_index; prior++)
      if (spec->reasons[prior] && strcmp(spec->reasons[prior], reason) == 0) {
        duplicate = 1;
        break;
      }
    if (duplicate)
      continue;
    if (unique_index++)
      status = literal(&builder->items, ",");
    if (status == ARCHBIRD_OK)
      status = json_cstring(&builder->items, reason);
  }
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, "],\"operation\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&builder->items, spec->operation->data,
                              spec->operation->length);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"origins\":");
  if (status == ARCHBIRD_OK)
    status = render_rows(&builder->items, origins, origin_count);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"provenance\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->items, spec->provenance);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"statement\":");
  if (status == ARCHBIRD_OK)
    status = json_cstring(&builder->items, spec->statement);
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, ",\"unknowns\":[");
  for (reason_index = 0; status == ARCHBIRD_OK && reason_index < reason_count;
       reason_index++) {
    if (reason_index)
      status = literal(&builder->items, ",");
    if (status == ARCHBIRD_OK)
      status = json_cstring(&builder->items, unknown_ids[reason_index]);
  }
  if (status == ARCHBIRD_OK)
    status = literal(&builder->items, "]}");
  if (status == ARCHBIRD_OK) {
    builder->first_item = 0;
    builder->item_count++;
    status = add_targeted(builder, &id_value->as.text);
  }
cleanup:
  ab_free(builder->engine, unknown_ids);
  rendered_values_free(evidence, evidence_count);
  ab_free(builder->engine, evidence);
  rendered_values_free(origins, origin_count);
  ab_free(builder->engine, origins);
  return status;
}
