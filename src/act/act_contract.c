#include "act_internal.h"

#include <stdlib.h>
#include <string.h>

#define CONTRACT_TRY(expression)                                               \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

typedef struct AbActReview {
  AbValue root;
  const AbValue *objective;
  const AbValue *owner;
  const AbValue *rationale;
  const AbValue *preserve_constraints;
  const AbValue *selected_candidates;
} AbActReview;

static int nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

static int strings_unique(const AbValue *rows) {
  size_t index;
  size_t previous;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *value = &rows->as.array.items[index];
    if (!nonblank(value))
      return 0;
    for (previous = 0; previous < index; previous++) {
      if (ab_string_equal(&value->as.text,
                          &rows->as.array.items[previous].as.text))
        return 0;
    }
  }
  return 1;
}

static ArchbirdStatus review_load(ArchbirdEngine *engine, const uint8_t *json,
                                  size_t json_length, AbActReview *out) {
  static const char *const allowed[] = {
      "objective",           "owner", "preserve_constraints", "rationale",
      "selected_candidates",
  };
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, json_length, &out->root);
  if (status != ARCHBIRD_OK)
    return status;
  out->objective = ab_value_member(&out->root, "objective");
  out->owner = ab_value_member(&out->root, "owner");
  out->rationale = ab_value_member(&out->root, "rationale");
  out->preserve_constraints =
      ab_value_member(&out->root, "preserve_constraints");
  out->selected_candidates = ab_value_member(&out->root, "selected_candidates");
  if (!ab_act_object_fields_allowed(&out->root, allowed,
                                    sizeof(allowed) / sizeof(allowed[0])) ||
      !nonblank(out->objective) || !nonblank(out->owner) ||
      !nonblank(out->rationale) || !strings_unique(out->preserve_constraints) ||
      !strings_unique(out->selected_candidates)) {
    ab_value_free(engine, &out->root);
    memset(out, 0, sizeof(*out));
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "invalid architecture change review input");
  }
  return ARCHBIRD_OK;
}

static const AbValue *row_by_id(const AbValue *rows, const AbString *id) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *value = ab_value_member(row, "id");
    if (value && value->kind == AB_VALUE_STRING &&
        ab_string_equal(&value->as.text, id))
      return row;
  }
  return NULL;
}

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbValue *const *left = (const AbValue *const *)left_raw;
  const AbValue *const *right = (const AbValue *const *)right_raw;
  return ab_string_compare(&(*left)->as.text, &(*right)->as.text);
}

static ArchbirdStatus sorted_strings(ArchbirdEngine *engine,
                                     const AbValue *rows,
                                     const AbValue ***out_rows,
                                     size_t *out_count) {
  const AbValue **result = NULL;
  size_t index;
  size_t count = rows->as.array.count;
  if (count > SIZE_MAX / sizeof(*result))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many reviewed Act values");
  if (count) {
    result = (const AbValue **)ab_malloc(engine, count * sizeof(*result));
    if (!result)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting reviewed Act values");
  }
  for (index = 0; index < count; index++)
    result[index] = &rows->as.array.items[index];
  if (count > 1)
    qsort(result, count, sizeof(*result), string_pointer_compare);
  *out_rows = result;
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
validate_review_selection(ArchbirdEngine *engine,
                          const AbActProposalView *proposal,
                          const AbActReview *review) {
  size_t index;
  for (index = 0; index < review->preserve_constraints->as.array.count;
       index++) {
    const AbString *id =
        &review->preserve_constraints->as.array.items[index].as.text;
    if (!row_by_id(proposal->preserved, id))
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "review selects an unknown preserved constraint");
  }
  for (index = 0; index < review->selected_candidates->as.array.count;
       index++) {
    const AbString *id =
        &review->selected_candidates->as.array.items[index].as.text;
    if (!row_by_id(proposal->candidates, id))
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET,
                                "review selects an unknown change candidate");
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_string_values(AbBuffer *buffer,
                                           const AbValue *rows, int sorted) {
  const AbValue **values = NULL;
  size_t count = rows->as.array.count;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (sorted) {
    status = sorted_strings(buffer->engine, rows, &values, &count);
    if (status != ARCHBIRD_OK)
      return status;
  }
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *value =
        sorted ? values[index] : &rows->as.array.items[index];
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, value);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(buffer->engine, values);
  return status;
}

static ArchbirdStatus render_proposal_ids(AbBuffer *buffer,
                                          const AbValue *rows) {
  size_t index;
  CONTRACT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < rows->as.array.count; index++) {
    if (index)
      CONTRACT_TRY(ab_buffer_literal(buffer, ","));
    CONTRACT_TRY(ab_value_render(
        buffer, ab_value_member(&rows->as.array.items[index], "id")));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus
render_preserved_constraints(AbBuffer *buffer,
                             const AbActProposalView *proposal,
                             const AbValue *selected) {
  const AbValue **values = NULL;
  size_t count;
  size_t index;
  ArchbirdStatus status =
      sorted_strings(buffer->engine, selected, &values, &count);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    const AbValue *row =
        row_by_id(proposal->preserved, &values[index]->as.text);
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_value_render(buffer, row);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(buffer->engine, values);
  return status;
}

static ArchbirdStatus
render_contract_document(AbBuffer *buffer, const AbActProposalView *proposal,
                         const AbActReview *review, int include_sha256,
                         const char sha256[65]) {
  const AbValue *origin_constraint =
      ab_value_member(proposal->origin, "constraint");
  const AbValue *finding = ab_value_member(proposal->origin, "finding");
  const AbValue *fingerprint = ab_value_member(finding, "fingerprint");
  CONTRACT_TRY(ab_buffer_literal(buffer, "{\"acknowledged_unknowns\":"));
  CONTRACT_TRY(render_proposal_ids(buffer, proposal->unknowns));
  CONTRACT_TRY(ab_buffer_literal(
      buffer, ",\"artifact\":\"change-contract\",\"objective\":"));
  CONTRACT_TRY(ab_value_render(buffer, review->objective));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"origin\":{\"constraint\":"));
  CONTRACT_TRY(ab_value_render(buffer, origin_constraint));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"fingerprint\":"));
  CONTRACT_TRY(ab_value_render(buffer, fingerprint));
  CONTRACT_TRY(ab_buffer_literal(buffer, "},\"owner\":"));
  CONTRACT_TRY(ab_value_render(buffer, review->owner));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"postconditions\":"));
  CONTRACT_TRY(render_proposal_ids(buffer, proposal->postconditions));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"preserved_constraints\":"));
  CONTRACT_TRY(render_preserved_constraints(buffer, proposal,
                                            review->preserve_constraints));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"proposal_sha256\":"));
  CONTRACT_TRY(ab_buffer_json_string(buffer, proposal->sha256, 64));
  CONTRACT_TRY(
      ab_buffer_literal(buffer, ",\"provenance\":\"asserted\",\"rationale\":"));
  CONTRACT_TRY(ab_value_render(buffer, review->rationale));
  CONTRACT_TRY(ab_buffer_literal(buffer, ",\"schema_version\":2,"
                                         "\"selected_candidates\":"));
  CONTRACT_TRY(render_string_values(buffer, review->selected_candidates, 1));
  if (include_sha256) {
    CONTRACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    CONTRACT_TRY(ab_buffer_json_string(buffer, sha256, 64));
  }
  return ab_buffer_literal(
      buffer,
      ",\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION "\"}}");
}

ArchbirdStatus ab_act_contract_create_json(ArchbirdEngine *engine,
                                           const AbActProposalView *proposal,
                                           const uint8_t *review_json,
                                           size_t review_length,
                                           AbBuffer *out) {
  AbActReview review = {0};
  AbBuffer payload;
  uint8_t digest[32];
  char sha256[65];
  ArchbirdStatus status;
  if (!engine || !proposal || (!review_json && review_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&payload, engine);
  status = review_load(engine, review_json, review_length, &review);
  if (status == ARCHBIRD_OK)
    status = validate_review_selection(engine, proposal, &review);
  if (status == ARCHBIRD_OK)
    status = render_contract_document(&payload, proposal, &review, 0, NULL);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(payload.data, payload.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, sha256);
  if (status == ARCHBIRD_OK)
    status = render_contract_document(out, proposal, &review, 1, sha256);
  ab_buffer_free(&payload);
  ab_value_free(engine, &review.root);
  return status;
}
