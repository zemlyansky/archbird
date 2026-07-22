#include "act_internal.h"

#include <stdlib.h>
#include <string.h>

#define ACT_TRY(expression)                                                    \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *value) {
  return ab_string_copy(engine, out, value, strlen(value));
}

static ArchbirdStatus copy_string(ArchbirdEngine *engine, AbString *out,
                                  const AbString *value) {
  return ab_string_copy(engine, out, value ? value->data : "",
                        value ? value->length : 0);
}

static ArchbirdStatus formatted_string(ArchbirdEngine *engine, AbString *out,
                                       const char *prefix,
                                       const AbString *middle,
                                       const char *suffix) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK && middle)
    status = ab_buffer_append(&buffer, middle->data, middle->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, suffix);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static const AbValue *operand(const AbValue *check, const char *name) {
  const AbValue *operands = ab_value_member(check, "operands");
  return operands ? ab_value_member(operands, name) : NULL;
}

static ArchbirdStatus copy_attributes(ArchbirdEngine *engine,
                                      AbProjectionItem *target,
                                      const AbProjectionItem *source) {
  size_t index;
  target->attribute_count = source->attribute_count;
  if (target->attribute_count > SIZE_MAX / sizeof(*target->attributes))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many Act fact attributes");
  if (target->attribute_count) {
    target->attributes = (AbObjectField *)ab_calloc(
        engine, target->attribute_count, sizeof(*target->attributes));
    if (!target->attributes)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory copying Act fact attributes");
  }
  for (index = 0; index < target->attribute_count; index++) {
    ArchbirdStatus status = copy_string(engine, &target->attributes[index].name,
                                        &source->attributes[index].name);
    if (status == ARCHBIRD_OK)
      status = ab_value_copy(engine, &target->attributes[index].value,
                             &source->attributes[index].value);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus copy_item(ArchbirdEngine *engine,
                                AbProjectionItem *target,
                                const AbProjectionItem *source,
                                const AbString *key) {
  ArchbirdStatus status =
      ab_projection_item_init(engine, target, key ? key : &source->key,
                              key ? key : &source->label, &source->value);
  size_t index;
  if (status == ARCHBIRD_OK)
    status = copy_attributes(engine, target, source);
  if (status == ARCHBIRD_OK && (source->state.length != 7 ||
                                memcmp(source->state.data, "current", 7) != 0))
    status = ab_projection_item_set_state(engine, target, source->state.data,
                                          source->message.data);
  for (index = 0; status == ARCHBIRD_OK && index < source->evidence_count;
       index++)
    status = ab_projection_item_add_evidence(engine, target,
                                             &source->evidence[index]);
  if (status != ARCHBIRD_OK)
    ab_projection_item_free(engine, target);
  return status;
}

static const AbString *mapped_key(const AbValue *aliases,
                                  const AbString *source) {
  const AbValue *mapped =
      aliases ? ab_value_member(aliases, source->data) : NULL;
  return mapped && mapped->kind == AB_VALUE_STRING ? &mapped->as.text : source;
}

static int projection_selected(const char *selection, const AbValue *keys,
                               const AbString *key) {
  size_t index;
  int present = 0;
  if (!selection || strcmp(selection, "all") == 0)
    return 1;
  if (keys && keys->kind == AB_VALUE_ARRAY) {
    for (index = 0; index < keys->as.array.count; index++) {
      const AbValue *value = &keys->as.array.items[index];
      if (value->kind == AB_VALUE_STRING &&
          ab_string_equal(&value->as.text, key)) {
        present = 1;
        break;
      }
    }
  }
  return strcmp(selection, "include") == 0 ? present : !present;
}

ArchbirdStatus ab_act_project_fact(ArchbirdEngine *engine,
                                   const AbProjectionData *source,
                                   const AbString *name, const AbValue *aliases,
                                   const char *selection, const AbValue *keys,
                                   AbProjectionData *out) {
  ArchbirdStatus status = ab_projection_data_init(
      engine, out, name, source->shape.data, "derived", &source->project);
  size_t index;
  if (status == ARCHBIRD_OK &&
      (source->state.length != 7 ||
       memcmp(source->state.data, "current", 7) != 0)) {
    ab_string_free(engine, &out->state);
    ab_string_free(engine, &out->message);
    status = copy_string(engine, &out->state, &source->state);
    if (status == ARCHBIRD_OK)
      status = copy_string(engine, &out->message, &source->message);
  }
  for (index = 0; status == ARCHBIRD_OK && index < source->item_count;
       index++) {
    AbProjectionItem item = {0};
    const AbString *key = mapped_key(aliases, &source->items[index].key);
    if (!projection_selected(selection, keys, key))
      continue;
    status = copy_item(engine, &item, &source->items[index], key);
    if (status == ARCHBIRD_OK)
      status = ab_projection_data_add_item(engine, out, &item);
    if (status != ARCHBIRD_OK)
      ab_projection_item_free(engine, &item);
  }
  if (status == ARCHBIRD_OK)
    status = ab_projection_data_finish(engine, out);
  if (status != ARCHBIRD_OK)
    ab_projection_data_free(engine, out);
  return status;
}

static AbProjectionItem *find_item(AbProjectionData *fact,
                                   const AbString *key) {
  size_t index;
  if (!fact || !key)
    return NULL;
  for (index = 0; index < fact->item_count; index++) {
    if (ab_string_equal(&fact->items[index].key, key))
      return &fact->items[index];
  }
  return NULL;
}

static const AbProjectionItem *find_const_item(const AbProjectionData *fact,
                                               const AbString *key) {
  return find_item((AbProjectionData *)fact, key);
}

static const AbProjectionItem *find_item_by_label(const AbProjectionData *fact,
                                                  const AbString *label) {
  size_t index;
  if (!fact || !label)
    return NULL;
  for (index = 0; index < fact->item_count; index++) {
    if (ab_string_equal(&fact->items[index].label, label))
      return &fact->items[index];
  }
  return NULL;
}

static void remove_item(ArchbirdEngine *engine, AbProjectionData *fact,
                        const AbString *key) {
  size_t index;
  for (index = 0; index < fact->item_count; index++) {
    if (!ab_string_equal(&fact->items[index].key, key))
      continue;
    ab_projection_item_free(engine, &fact->items[index]);
    if (index + 1 < fact->item_count)
      memmove(&fact->items[index], &fact->items[index + 1],
              (fact->item_count - index - 1) * sizeof(*fact->items));
    fact->item_count--;
    memset(&fact->items[fact->item_count], 0, sizeof(*fact->items));
    return;
  }
}

static ArchbirdStatus copy_fact_named(ArchbirdEngine *engine,
                                      const AbProjectionData *source,
                                      const AbString *name,
                                      AbProjectionData *out) {
  static const AbValue empty_aliases = {.kind = AB_VALUE_OBJECT};
  return ab_act_project_fact(engine, source, name, &empty_aliases, "all", NULL,
                             out);
}

static ArchbirdStatus coverage_init(ArchbirdEngine *engine,
                                    const AbActVerification *verification,
                                    const AbString *fact_name,
                                    const AbProjectionData *fact,
                                    AbActCoverage *coverage) {
  const AbValue *definition =
      fact ? ab_act_verification_operand_definition(verification, fact_name)
           : NULL;
  const AbValue *kind = definition ? ab_value_member(definition, "kind") : NULL;
  const char *unknown = "unmodeled_evidence_frontier";
  ArchbirdStatus status;
  memset(coverage, 0, sizeof(*coverage));
  if (!fact) {
    coverage->classification = "partial";
    status = formatted_string(engine, &coverage->domain,
                              "observation:", fact_name, "");
    if (status == ARCHBIRD_OK)
      status =
          copy_literal(engine, &coverage->provider, "constraint-observation");
    coverage->unknown = "semantic_repair";
    return status;
  }
  status = formatted_string(engine, &coverage->domain, "fact:", fact_name, "");
  if (status == ARCHBIRD_OK)
    status = formatted_string(
        engine, &coverage->provider, "operand:",
        kind && kind->kind == AB_VALUE_STRING ? &kind->as.text : NULL,
        kind && kind->kind == AB_VALUE_STRING ? "" : "unknown");
  if (status != ARCHBIRD_OK)
    return status;
  if (fact->state.length != 7 || memcmp(fact->state.data, "current", 7) != 0) {
    coverage->classification = "partial";
    coverage->unknown = "unavailable_fact_evidence";
    return ARCHBIRD_OK;
  }
  if (ab_value_string_is(kind, "python_enum") ||
      ab_value_string_is(kind, "python_set") ||
      ab_value_string_is(kind, "c_enum") ||
      ab_value_string_is(kind, "c_designated_initializer") ||
      ab_value_string_is(kind, "c_macro_set") ||
      ab_value_string_is(kind, "literal_set") ||
      ab_value_string_is(kind, "literal_values") ||
      ab_value_string_is(kind, "literal_relation")) {
    coverage->classification = "complete";
    coverage->unknown = NULL;
    return ARCHBIRD_OK;
  }
  coverage->classification = "bounded";
  if (ab_value_string_is(kind, "provider_surface"))
    unknown = "unmapped_dynamic_lookup";
  else if (ab_value_string_is(kind, "symbols"))
    unknown = "dynamic_symbol_creation";
  else if (ab_value_string_is(kind, "file_edges") ||
           ab_value_string_is(kind, "component_edges"))
    unknown = "dynamic_dispatch";
  else if (ab_value_string_is(kind, "test_routes"))
    unknown = "runtime_branch_coverage";
  coverage->unknown = unknown;
  return ARCHBIRD_OK;
}

static void coverage_free(ArchbirdEngine *engine, AbActCoverage *coverage) {
  ab_string_free(engine, &coverage->domain);
  ab_string_free(engine, &coverage->provider);
  memset(coverage, 0, sizeof(*coverage));
}

static ArchbirdStatus coverage_render(AbBuffer *buffer,
                                      const AbActCoverage *coverage) {
  ACT_TRY(ab_buffer_literal(buffer, "{\"classification\":"));
  ACT_TRY(ab_buffer_json_string(buffer, coverage->classification,
                                strlen(coverage->classification)));
  ACT_TRY(ab_buffer_literal(buffer, ",\"domain\":"));
  ACT_TRY(ab_buffer_json_string(buffer, coverage->domain.data,
                                coverage->domain.length));
  ACT_TRY(ab_buffer_literal(buffer, ",\"providers\":["));
  ACT_TRY(ab_buffer_json_string(buffer, coverage->provider.data,
                                coverage->provider.length));
  ACT_TRY(ab_buffer_literal(buffer, "],\"unknowns\":["));
  if (coverage->unknown)
    ACT_TRY(ab_buffer_json_string(buffer, coverage->unknown,
                                  strlen(coverage->unknown)));
  return ab_buffer_literal(buffer, "]}");
}

static ArchbirdStatus
digest_identity(ArchbirdEngine *engine, const char *first_name,
                const AbString *first_value, const char *second_name,
                const AbString *second_value, const char *third_name,
                const AbString *third_value, char output[65]) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_literal(&buffer, "{");
#define IDENTITY_FIELD(name, value, comma)                                     \
  do {                                                                         \
    if (status == ARCHBIRD_OK && (comma))                                      \
      status = ab_buffer_literal(&buffer, ",");                                \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_json_string(&buffer, (name), strlen(name));           \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_literal(&buffer, ":");                                \
    if (status == ARCHBIRD_OK)                                                 \
      status = ab_buffer_json_string(&buffer, (value)->data, (value)->length); \
  } while (0)
  IDENTITY_FIELD(first_name, first_value, 0);
  IDENTITY_FIELD(second_name, second_value, 1);
  IDENTITY_FIELD(third_name, third_value, 1);
#undef IDENTITY_FIELD
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, output);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus candidate_id(ArchbirdEngine *engine,
                                   const AbString *fingerprint,
                                   const AbString *project,
                                   const AbString *path, AbString *out) {
  char digest[65];
  ArchbirdStatus status = digest_identity(
      engine, "finding", fingerprint, "path", path, "project", project, digest);
  if (status == ARCHBIRD_OK) {
    char id[27] = "candidate:";
    memcpy(id + 10, digest, 16);
    id[26] = '\0';
    status = ab_string_copy(engine, out, id, 26);
  }
  return status;
}

static ArchbirdStatus unknown_id(ArchbirdEngine *engine,
                                 const AbString *fingerprint, const char *code,
                                 const AbString *scope, AbString *out) {
  AbString code_string = {(char *)code, strlen(code)};
  char digest[65];
  ArchbirdStatus status =
      digest_identity(engine, "code", &code_string, "finding", fingerprint,
                      "scope", scope, digest);
  if (status == ARCHBIRD_OK) {
    char id[25] = "unknown:";
    memcpy(id + 8, digest, 16);
    id[24] = '\0';
    status = ab_string_copy(engine, out, id, 24);
  }
  return status;
}

static int candidate_compare(const void *left_raw, const void *right_raw) {
  const AbActCandidate *left = (const AbActCandidate *)left_raw;
  const AbActCandidate *right = (const AbActCandidate *)right_raw;
  int compared = ab_string_compare(&left->project, &right->project);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  if (!compared)
    compared = ab_string_compare(&left->id, &right->id);
  return compared;
}

static int unknown_compare(const void *left_raw, const void *right_raw) {
  const AbActUnknown *left = (const AbActUnknown *)left_raw;
  const AbActUnknown *right = (const AbActUnknown *)right_raw;
  return ab_string_compare(&left->id, &right->id);
}

static ArchbirdStatus collect_candidates(ArchbirdEngine *engine,
                                         AbActProposalData *proposal,
                                         const AbProjectionData *actual,
                                         const AbProjectionItem *target,
                                         const AbString *finding_key) {
  size_t item_start = 0;
  size_t item_end = actual->item_count;
  size_t item_index;
  if (target) {
    item_start = (size_t)(target - actual->items);
    item_end = item_start + 1;
  }
  for (item_index = item_start; item_index < item_end; item_index++) {
    const AbProjectionItem *item = &actual->items[item_index];
    size_t evidence_index;
    for (evidence_index = 0; evidence_index < item->evidence_count;
         evidence_index++) {
      const AbProjectionEvidence *evidence = &item->evidence[evidence_index];
      AbActCandidate *candidate = NULL;
      size_t index;
      ArchbirdStatus status;
      if (!evidence->project.length || !evidence->path.length ||
          !ab_string_equal(&evidence->project, &actual->project))
        continue;
      for (index = 0; index < proposal->candidate_count; index++) {
        if (ab_string_equal(&proposal->candidates[index].project,
                            &evidence->project) &&
            ab_string_equal(&proposal->candidates[index].path,
                            &evidence->path)) {
          candidate = &proposal->candidates[index];
          break;
        }
      }
      if (!candidate) {
        AbActCandidate *resized;
        if (proposal->candidate_count == SIZE_MAX / sizeof(*resized))
          return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "too many Act candidates");
        resized = (AbActCandidate *)ab_realloc(engine, proposal->candidates,
                                               (proposal->candidate_count + 1) *
                                                   sizeof(*resized));
        if (!resized)
          return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory storing Act candidates");
        proposal->candidates = resized;
        candidate = &proposal->candidates[proposal->candidate_count++];
        memset(candidate, 0, sizeof(*candidate));
        candidate->kind = target ? "edit_site" : "peer_site";
        status = copy_string(engine, &candidate->project, &evidence->project);
        if (status == ARCHBIRD_OK)
          status = copy_string(engine, &candidate->path, &evidence->path);
        if (status == ARCHBIRD_OK)
          status =
              candidate_id(engine, &proposal->fingerprint, &candidate->project,
                           &candidate->path, &candidate->id);
        if (status == ARCHBIRD_OK) {
          if (target)
            status = formatted_string(engine, &candidate->reason,
                                      "current evidence for ", finding_key, "");
          else {
            AbBuffer reason;
            ab_buffer_init(&reason, engine);
            status = ab_buffer_literal(&reason, "peer evidence in ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&reason, actual->name.data,
                                        actual->name.length);
            if (status == ARCHBIRD_OK)
              status = ab_buffer_literal(&reason, " for missing ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&reason, finding_key->data,
                                        finding_key->length);
            if (status == ARCHBIRD_OK)
              status = ab_string_copy(engine, &candidate->reason,
                                      (const char *)reason.data, reason.length);
            ab_buffer_free(&reason);
          }
        }
        if (status != ARCHBIRD_OK)
          return status;
      }
      status = ab_act_evidence_list_add(engine, &candidate->evidence, evidence);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (item_index = 0; item_index < proposal->candidate_count; item_index++)
    ab_act_evidence_list_finish(&proposal->candidates[item_index].evidence);
  if (proposal->candidate_count > 1)
    qsort(proposal->candidates, proposal->candidate_count,
          sizeof(*proposal->candidates), candidate_compare);
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_unknown(ArchbirdEngine *engine,
                                  AbActProposalData *proposal, const char *code,
                                  const AbString *scope, const char *message,
                                  const AbActEvidenceList *evidence) {
  AbActUnknown row = {0};
  AbActUnknown *resized;
  ArchbirdStatus status =
      unknown_id(engine, &proposal->fingerprint, code, scope, &row.id);
  size_t index;
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &row.scope, scope);
  row.code = code;
  row.message = message;
  for (index = 0; status == ARCHBIRD_OK && index < evidence->count; index++)
    status = ab_act_evidence_list_add(engine, &row.evidence,
                                      &evidence->items[index]);
  if (status == ARCHBIRD_OK)
    ab_act_evidence_list_finish(&row.evidence);
  if (status != ARCHBIRD_OK) {
    ab_string_free(engine, &row.id);
    ab_string_free(engine, &row.scope);
    ab_act_evidence_list_free(&row.evidence);
    return status;
  }
  for (index = 0; index < proposal->unknown_count; index++) {
    if (ab_string_equal(&proposal->unknowns[index].id, &row.id)) {
      ab_string_free(engine, &row.id);
      ab_string_free(engine, &row.scope);
      ab_act_evidence_list_free(&row.evidence);
      return ARCHBIRD_OK;
    }
  }
  if (proposal->unknown_count == SIZE_MAX / sizeof(*resized)) {
    ab_string_free(engine, &row.id);
    ab_string_free(engine, &row.scope);
    ab_act_evidence_list_free(&row.evidence);
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many Act unknowns");
  }
  resized = (AbActUnknown *)ab_realloc(engine, proposal->unknowns,
                                       (proposal->unknown_count + 1) *
                                           sizeof(*proposal->unknowns));
  if (!resized) {
    ab_string_free(engine, &row.id);
    ab_string_free(engine, &row.scope);
    ab_act_evidence_list_free(&row.evidence);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing Act unknowns");
  }
  proposal->unknowns = resized;
  proposal->unknowns[proposal->unknown_count++] = row;
  return ARCHBIRD_OK;
}

static int slice_compare(const void *left_raw, const void *right_raw) {
  const AbActSliceEntry *left = (const AbActSliceEntry *)left_raw;
  const AbActSliceEntry *right = (const AbActSliceEntry *)right_raw;
  int compared = strcmp(left->kind, right->kind);
  if (!compared)
    compared = ab_string_compare(&left->name, &right->name);
  if (!compared)
    compared = memcmp(left->sha256, right->sha256, 64);
  return compared;
}

static ArchbirdStatus add_slice(ArchbirdEngine *engine,
                                AbActProposalData *proposal, const char *kind,
                                const AbString *name, const char sha256[65]) {
  AbActSliceEntry *resized;
  ArchbirdStatus status;
  if (proposal->slice_count == SIZE_MAX / sizeof(*resized))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many Act evidence-slice entries");
  resized = (AbActSliceEntry *)ab_realloc(engine, proposal->slice,
                                          (proposal->slice_count + 1) *
                                              sizeof(*proposal->slice));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing Act evidence slice");
  proposal->slice = resized;
  memset(&proposal->slice[proposal->slice_count], 0, sizeof(*proposal->slice));
  proposal->slice[proposal->slice_count].kind = kind;
  memcpy(proposal->slice[proposal->slice_count].sha256, sha256, 65);
  status =
      copy_string(engine, &proposal->slice[proposal->slice_count].name, name);
  if (status == ARCHBIRD_OK)
    proposal->slice_count++;
  return status;
}

static ArchbirdStatus render_slice_rows(AbBuffer *buffer,
                                        const AbActProposalData *proposal) {
  size_t index;
  ACT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < proposal->slice_count; index++) {
    const AbActSliceEntry *row = &proposal->slice[index];
    if (index)
      ACT_TRY(ab_buffer_literal(buffer, ","));
    ACT_TRY(ab_buffer_literal(buffer, "{\"kind\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->kind, strlen(row->kind)));
    ACT_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->name.data, row->name.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->sha256, 64));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus finish_slice(ArchbirdEngine *engine,
                                   AbActProposalData *proposal) {
  AbBuffer buffer;
  uint8_t digest[32];
  ArchbirdStatus status;
  if (proposal->slice_count > 1)
    qsort(proposal->slice, proposal->slice_count, sizeof(*proposal->slice),
          slice_compare);
  ab_buffer_init(&buffer, engine);
  status = render_slice_rows(&buffer, proposal);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(buffer.data, buffer.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, proposal->slice_sha256);
  ab_buffer_free(&buffer);
  return status;
}

static int preserved_compare(const void *left_raw, const void *right_raw) {
  const AbActPreserved *left = (const AbActPreserved *)left_raw;
  const AbActPreserved *right = (const AbActPreserved *)right_raw;
  return ab_string_compare(&ab_value_member(left->constraint, "id")->as.text,
                           &ab_value_member(right->constraint, "id")->as.text);
}

static ArchbirdStatus collect_preserved(ArchbirdEngine *engine,
                                        AbActProposalData *proposal) {
  size_t index;
  const AbString *origin =
      &ab_value_member(proposal->origin_constraint, "id")->as.text;
  for (index = 0; index < proposal->verification->constraints->as.array.count;
       index++) {
    const AbValue *check =
        &proposal->verification->constraints->as.array.items[index];
    const AbValue *id = ab_value_member(check, "id");
    const AbValue *severity = ab_value_member(check, "severity");
    const AbValue *status_value = ab_value_member(check, "status");
    AbActPreserved *resized;
    ArchbirdStatus status;
    if (ab_string_equal(&id->as.text, origin) ||
        !ab_value_string_is(severity, "error") ||
        !(ab_value_string_is(status_value, "pass") ||
          ab_value_string_is(status_value, "waived") ||
          ab_value_string_is(status_value, "not_applicable")))
      continue;
    if (proposal->preserved_count == SIZE_MAX / sizeof(*resized))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many preserved Act constraints");
    resized = (AbActPreserved *)ab_realloc(engine, proposal->preserved,
                                           (proposal->preserved_count + 1) *
                                               sizeof(*proposal->preserved));
    if (!resized)
      return archbird_error_set(
          engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
          "out of memory storing preserved Act constraints");
    proposal->preserved = resized;
    proposal->preserved[proposal->preserved_count].constraint = check;
    status = ab_act_value_digest(
        engine, check, proposal->preserved[proposal->preserved_count].sha256);
    if (status != ARCHBIRD_OK)
      return status;
    proposal->preserved_count++;
  }
  if (proposal->preserved_count > 1)
    qsort(proposal->preserved, proposal->preserved_count,
          sizeof(*proposal->preserved), preserved_compare);
  return ARCHBIRD_OK;
}

static ArchbirdStatus build_slice(ArchbirdEngine *engine,
                                  AbActProposalData *proposal) {
  const AbValue *tool_name =
      ab_value_member(proposal->verification->tool, "name");
  const AbValue *tool_digest =
      ab_value_member(proposal->verification->tool, "implementation_sha256");
  const AbValue *policy_project =
      ab_value_member(proposal->verification->policy, "project");
  const AbValue *policy_digest = ab_value_member(proposal->verification->policy,
                                                 "constraint_policy_sha256");
  const AbValue *expected = operand(proposal->origin_constraint, "expected");
  const AbValue *actual = operand(proposal->origin_constraint, "actual");
  const AbValue *mapping = operand(proposal->origin_constraint, "mapping");
  const AbValue *names[2] = {expected, actual};
  size_t index;
  ArchbirdStatus status =
      add_slice(engine, proposal, "verification_tool", &tool_name->as.text,
                tool_digest->as.text.data);
  if (status == ARCHBIRD_OK)
    status = add_slice(engine, proposal, "policy", &policy_project->as.text,
                       policy_digest->as.text.data);
  if (status == ARCHBIRD_OK)
    status =
        add_slice(engine, proposal, "constraint",
                  &ab_value_member(proposal->origin_constraint, "id")->as.text,
                  proposal->origin_constraint_sha256);
  if (status == ARCHBIRD_OK)
    status = add_slice(engine, proposal, "finding", &proposal->fingerprint,
                       proposal->origin_finding_sha256);
  for (index = 0; status == ARCHBIRD_OK && index < 2; index++) {
    const AbValue *name = names[index];
    const AbProjectionData *fact = NULL;
    const AbValue *fact_value;
    const AbValue *observation;
    char digest[65];
    if (!name || name->kind != AB_VALUE_STRING || !name->as.text.length ||
        (index == 1 && expected && expected->kind == AB_VALUE_STRING &&
         ab_string_equal(&expected->as.text, &name->as.text)))
      continue;
    fact_value = ab_act_verification_fact_value(proposal->verification,
                                                &name->as.text, &fact);
    if (fact_value) {
      status =
          add_slice(engine, proposal, "fact", &name->as.text, fact->sha256);
      continue;
    }
    observation =
        ab_act_verification_observation(proposal->verification, &name->as.text);
    if (observation) {
      status = ab_act_value_digest(engine, observation, digest);
      if (status == ARCHBIRD_OK)
        status =
            add_slice(engine, proposal, "observation", &name->as.text, digest);
    }
  }
  if (status == ARCHBIRD_OK && mapping && mapping->kind == AB_VALUE_STRING &&
      mapping->as.text.length) {
    const AbValue *row =
        ab_act_verification_mapping(proposal->verification, &mapping->as.text);
    char digest[65];
    status = row ? ab_act_value_digest(engine, row, digest)
                 : archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                      ARCHBIRD_NO_OFFSET,
                                      "Act origin mapping is unavailable");
    if (status == ARCHBIRD_OK)
      status =
          add_slice(engine, proposal, "mapping", &mapping->as.text, digest);
  }
  if (status == ARCHBIRD_OK)
    status = finish_slice(engine, proposal);
  return status;
}

static ArchbirdStatus
initialize_postcondition(ArchbirdEngine *engine, AbActProposalData *proposal,
                         const char *id, const char *strength,
                         const char *assertion, const AbString *expected,
                         const AbString *actual) {
  const AbValue *origin_id = ab_value_member(proposal->origin_constraint, "id");
  AbBuffer id_buffer;
  ArchbirdStatus status;
  proposal->has_postcondition = 1;
  proposal->postcondition.id = id;
  proposal->postcondition.strength = strength;
  proposal->postcondition.assertion = assertion;
  proposal->postcondition.owner =
      ab_value_member(proposal->origin_constraint, "owner");
  proposal->postcondition.requirements =
      ab_value_member(proposal->origin_constraint, "requirements");
  proposal->postcondition.tags =
      ab_value_member(proposal->origin_constraint, "tags");
  proposal->postcondition.minimum = operand(proposal->origin_constraint, "min");
  proposal->postcondition.maximum = operand(proposal->origin_constraint, "max");
  proposal->postcondition.exact = operand(proposal->origin_constraint, "exact");
  proposal->postcondition.required_routes =
      operand(proposal->origin_constraint, "required_routes");
  ab_buffer_init(&id_buffer, engine);
  status = ab_buffer_literal(&id_buffer, "ACT-");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&id_buffer, origin_id->as.text.data,
                              origin_id->as.text.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&id_buffer, "-");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&id_buffer, proposal->fingerprint.data, 12);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &proposal->postcondition.constraint_id,
                            (const char *)id_buffer.data, id_buffer.length);
  ab_buffer_free(&id_buffer);
  if (status == ARCHBIRD_OK)
    status = formatted_string(engine, &proposal->postcondition.rationale,
                              "Derived postcondition for finding ",
                              &proposal->fingerprint, ".");
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &proposal->postcondition.expected, expected);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &proposal->postcondition.actual, actual);
  return status;
}

static ArchbirdStatus evidence_from_item(ArchbirdEngine *engine,
                                         AbActEvidenceList *list,
                                         const AbProjectionItem *item) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < item->evidence_count;
       index++)
    status = ab_act_evidence_list_add(engine, list, &item->evidence[index]);
  return status;
}

static ArchbirdStatus evidence_from_fact(ArchbirdEngine *engine,
                                         AbActEvidenceList *list,
                                         const AbProjectionData *fact) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < fact->item_count; index++)
    status = evidence_from_item(engine, list, &fact->items[index]);
  return status;
}

static ArchbirdStatus evidence_from_fact_context(ArchbirdEngine *engine,
                                                 AbActEvidenceList *list,
                                                 const AbProjectionData *fact) {
  size_t item_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (item_index = 0; status == ARCHBIRD_OK && item_index < fact->item_count;
       item_index++) {
    const AbProjectionItem *item = &fact->items[item_index];
    size_t evidence_index;
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < item->evidence_count;
         evidence_index++) {
      const AbProjectionEvidence *evidence = &item->evidence[evidence_index];
      if (!evidence->path.length)
        status = ab_act_evidence_list_add(engine, list, evidence);
    }
  }
  return status;
}

ArchbirdStatus ab_act_proposal_compile(ArchbirdEngine *engine,
                                       AbActVerification *verification,
                                       const char *fingerprint,
                                       size_t fingerprint_length,
                                       AbActProposalData *out) {
  const AbValue *fingerprint_value;
  const AbValue *finding;
  const AbValue *check = NULL;
  const AbValue *assertion;
  const AbValue *comparison;
  const AbValue *actual_value;
  const AbValue *expected_value;
  const AbValue *mapping_value;
  const AbProjectionData *actual = NULL;
  const AbProjectionData *expected = NULL;
  const AbValue *mapping = NULL;
  AbProjectionData projected = {0};
  AbActEvidenceList finding_evidence = {0};
  ArchbirdStatus status;
  int strength = 0; /* 0 oracle-only, 1 exact, 2 constrained */
  if (!engine || !verification || !fingerprint || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (!ab_value_string_is(ab_value_member(verification->policy, "kind"), "all"))
    return archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "Act preservation requires a complete constraint-policy evaluation");
  memset(out, 0, sizeof(*out));
  out->verification = verification;
  if (fingerprint_length != 64)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "Act finding fingerprint must be lowercase SHA-256");
  status = ab_string_copy(engine, &out->fingerprint, fingerprint,
                          fingerprint_length);
  fingerprint_value = NULL;
  if (status == ARCHBIRD_OK) {
    size_t index;
    for (index = 0; index < fingerprint_length; index++) {
      unsigned char byte = (unsigned char)fingerprint[index];
      if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
        status = archbird_error_set(
            engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
            "Act finding fingerprint must be lowercase SHA-256");
        break;
      }
    }
  }
  if (status != ARCHBIRD_OK)
    goto fail;
  finding =
      ab_act_verification_finding(verification, &out->fingerprint, &check);
  if (!finding || !check) {
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "Act finding must match exactly one result");
    goto fail;
  }
  fingerprint_value = ab_value_member(finding, "fingerprint");
  (void)fingerprint_value;
  if (!ab_value_string_is(ab_value_member(finding, "disposition"), "open") ||
      !ab_value_string_is(ab_value_member(finding, "applicability"),
                          "applicable")) {
    status = archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                                "Act finding must be open and applicable");
    goto fail;
  }
  if (!ab_value_string_is(ab_value_member(finding, "evidence_state"),
                          "current")) {
    status = archbird_error_set(
        engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
        "Act finding evidence must be current; refresh verification evidence "
        "before deriving a change proposal");
    goto fail;
  }
  out->origin_constraint = check;
  out->origin_finding = finding;
  status = ab_act_value_digest(engine, check, out->origin_constraint_sha256);
  if (status == ARCHBIRD_OK)
    status = ab_act_value_digest(engine, finding, out->origin_finding_sha256);
  assertion = ab_value_member(check, "assert");
  comparison = ab_value_member(finding, "comparison");
  actual_value = operand(check, "actual");
  expected_value = operand(check, "expected");
  mapping_value = operand(check, "mapping");
  if (status == ARCHBIRD_OK && actual_value &&
      actual_value->kind == AB_VALUE_STRING)
    status = copy_string(engine, &out->actual_name, &actual_value->as.text);
  if (status != ARCHBIRD_OK)
    goto fail;
  if (actual_value && actual_value->kind == AB_VALUE_STRING &&
      actual_value->as.text.length)
    ab_act_verification_fact_value(verification, &actual_value->as.text,
                                   &actual);
  if (expected_value && expected_value->kind == AB_VALUE_STRING &&
      expected_value->as.text.length)
    ab_act_verification_fact_value(verification, &expected_value->as.text,
                                   &expected);
  if (mapping_value && mapping_value->kind == AB_VALUE_STRING &&
      mapping_value->as.text.length)
    mapping =
        ab_act_verification_mapping(verification, &mapping_value->as.text);
  status = coverage_init(engine, verification,
                         actual_value && actual_value->kind == AB_VALUE_STRING
                             ? &actual_value->as.text
                             : &ab_value_member(check, "id")->as.text,
                         actual, &out->coverage);
  if (status == ARCHBIRD_OK)
    status = ab_act_evidence_list_add_array(
        engine, &out->evidence, ab_value_member(finding, "evidence"));
  if (status == ARCHBIRD_OK)
    status = ab_act_evidence_list_add_array(
        engine, &out->evidence, ab_value_member(check, "witnesses"));
  if (status == ARCHBIRD_OK)
    status = ab_act_evidence_list_add_array(
        engine, &finding_evidence, ab_value_member(finding, "evidence"));
  if (status != ARCHBIRD_OK)
    goto fail;

  if (ab_value_string_is(ab_value_member(finding, "evidence_state"),
                         "current") &&
      (ab_value_string_is(assertion, "set_equal") ||
       ab_value_string_is(assertion, "mapped_set_equal") ||
       ab_value_string_is(assertion, "values_equal") ||
       ab_value_string_is(assertion, "mapped_values_equal") ||
       ab_value_string_is(assertion, "subset") ||
       ab_value_string_is(assertion, "required_subset") ||
       ab_value_string_is(assertion, "required_values")) &&
      expected && actual &&
      (ab_value_string_is(comparison, "missing") ||
       ab_value_string_is(comparison, "extra") ||
       ab_value_string_is(comparison, "different"))) {
    const AbValue *aliases =
        mapping ? ab_value_member(mapping, "actual_to_expected") : NULL;
    const AbString *key = &ab_value_member(finding, "key")->as.text;
    const AbProjectionItem *expected_item = find_const_item(expected, key);
    AbProjectionItem *actual_item;
    AbString projection_name = {0};
    AbString expected_name = {0};
    status =
        formatted_string(engine, &projection_name, "after.",
                         &(AbString){out->fingerprint.data, 12}, ".actual");
    if (status == ARCHBIRD_OK)
      status = ab_act_project_fact(engine, actual, &projection_name, aliases,
                                   "all", NULL, &projected);
    actual_item = status == ARCHBIRD_OK ? find_item(&projected, key) : NULL;
    if (status == ARCHBIRD_OK)
      status = formatted_string(engine, &expected_name, "proposal.",
                                &(AbString){out->fingerprint.data, 12},
                                ".expected_after");
    if (status == ARCHBIRD_OK)
      status = copy_fact_named(engine, &projected, &expected_name,
                               &out->literal_fact);
    if (status == ARCHBIRD_OK &&
        (ab_value_string_is(comparison, "missing") ||
         ab_value_string_is(comparison, "different"))) {
      if (!expected_item) {
        ab_projection_data_free(engine, &out->literal_fact);
      } else {
        AbProjectionItem copied = {0};
        remove_item(engine, &out->literal_fact, key);
        status = copy_item(engine, &copied, expected_item, key);
        if (status == ARCHBIRD_OK)
          status =
              ab_projection_data_add_item(engine, &out->literal_fact, &copied);
        if (status != ARCHBIRD_OK)
          ab_projection_item_free(engine, &copied);
      }
    } else if (status == ARCHBIRD_OK &&
               ab_value_string_is(comparison, "extra")) {
      remove_item(engine, &out->literal_fact, key);
    }
    if (status == ARCHBIRD_OK && out->literal_fact.name.data) {
      status = ab_projection_data_finish(engine, &out->literal_fact);
      out->has_literal_fact = status == ARCHBIRD_OK;
    }
    if (status == ARCHBIRD_OK && out->has_literal_fact) {
      out->has_projection = 1;
      out->projection.name = projection_name;
      memset(&projection_name, 0, sizeof(projection_name));
      status =
          copy_string(engine, &out->projection.source, &actual_value->as.text);
      out->projection.aliases = aliases;
    }
    if (status == ARCHBIRD_OK && out->has_literal_fact)
      status = initialize_postcondition(
          engine, out, "origin-fact-transition", "exact_fact",
          (ab_value_string_is(assertion, "values_equal") ||
           ab_value_string_is(assertion, "mapped_values_equal") ||
           ab_value_string_is(assertion, "required_values"))
              ? "values_equal"
              : "set_equal",
          &out->literal_fact.name, &out->projection.name);
    if (status == ARCHBIRD_OK && out->has_postcondition)
      status = evidence_from_fact(engine, &out->postcondition.evidence,
                                  &out->literal_fact);
    if (status == ARCHBIRD_OK && out->has_postcondition)
      status =
          ab_act_evidence_list_add_array(engine, &out->postcondition.evidence,
                                         ab_value_member(finding, "evidence"));
    if (status == ARCHBIRD_OK && out->has_postcondition)
      status =
          ab_act_evidence_list_add_array(engine, &out->postcondition.evidence,
                                         ab_value_member(check, "witnesses"));
    if (status == ARCHBIRD_OK && out->has_postcondition) {
      ab_act_evidence_list_finish(&out->postcondition.evidence);
      status = collect_candidates(engine, out, &projected, actual_item, key);
      strength = 1;
    }
    ab_string_free(engine, &projection_name);
    ab_string_free(engine, &expected_name);
  } else if (ab_value_string_is(ab_value_member(finding, "evidence_state"),
                                "current") &&
             (ab_value_string_is(assertion, "required_edges") ||
              ab_value_string_is(assertion, "forbidden_edges") ||
              ab_value_string_is(assertion, "allowed_edges")) &&
             expected && actual) {
    const AbString *key = &ab_value_member(finding, "key")->as.text;
    const AbProjectionItem *item = ab_value_string_is(comparison, "missing")
                                       ? find_item_by_label(expected, key)
                                       : find_item_by_label(actual, key);
    if (item) {
      AbString name = {0};
      AbProjectionItem copied = {0};
      status =
          formatted_string(engine, &name, "proposal.",
                           &(AbString){out->fingerprint.data, 12}, ".relation");
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_init(
            engine, &out->literal_fact, &name, "relation", "derived",
            item->evidence_count ? &item->evidence[0].project : NULL);
      if (status == ARCHBIRD_OK)
        status = copy_item(engine, &copied, item, NULL);
      if (status == ARCHBIRD_OK)
        status =
            ab_projection_data_add_item(engine, &out->literal_fact, &copied);
      if (status == ARCHBIRD_OK)
        status = ab_projection_data_finish(engine, &out->literal_fact);
      out->has_literal_fact = status == ARCHBIRD_OK;
      if (status == ARCHBIRD_OK)
        status = initialize_postcondition(
            engine, out, "origin-relation-transition", "constrained_choice",
            ab_value_string_is(comparison, "missing") ? "required_edges"
                                                      : "forbidden_edges",
            &out->literal_fact.name, &actual_value->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_act_evidence_list_add_array(
            engine, &out->postcondition.evidence,
            ab_value_member(finding, "evidence"));
      if (status == ARCHBIRD_OK)
        status =
            ab_act_evidence_list_add_array(engine, &out->postcondition.evidence,
                                           ab_value_member(check, "witnesses"));
      if (status == ARCHBIRD_OK)
        status = evidence_from_item(engine, &out->postcondition.evidence, item);
      if (status == ARCHBIRD_OK) {
        ab_act_evidence_list_finish(&out->postcondition.evidence);
        status = collect_candidates(
            engine, out, actual,
            ab_value_string_is(comparison, "missing") ? NULL : item, key);
        strength = 2;
      }
      ab_string_free(engine, &name);
    }
  } else if (ab_value_string_is(ab_value_member(finding, "evidence_state"),
                                "current") &&
             (ab_value_string_is(assertion, "acyclic") ||
              ab_value_string_is(assertion, "cardinality") ||
              ab_value_string_is(assertion, "min_test_routes")) &&
             actual) {
    AbString empty = {0};
    status = initialize_postcondition(
        engine, out, "origin-constraint-transition", "constrained_choice",
        assertion->as.text.data, &empty, &actual_value->as.text);
    if (status == ARCHBIRD_OK)
      status =
          ab_act_evidence_list_add_array(engine, &out->postcondition.evidence,
                                         ab_value_member(finding, "evidence"));
    if (status == ARCHBIRD_OK)
      status =
          ab_act_evidence_list_add_array(engine, &out->postcondition.evidence,
                                         ab_value_member(check, "witnesses"));
    if (status == ARCHBIRD_OK)
      status = evidence_from_fact_context(engine, &out->postcondition.evidence,
                                          actual);
    if (status == ARCHBIRD_OK) {
      ab_act_evidence_list_finish(&out->postcondition.evidence);
      status = collect_candidates(engine, out, actual, NULL,
                                  &ab_value_member(finding, "key")->as.text);
      strength = 2;
    }
  }
  if (status != ARCHBIRD_OK)
    goto fail;
  {
    size_t candidate_index;
    for (candidate_index = 0; candidate_index < out->candidate_count;
         candidate_index++) {
      size_t evidence_index;
      for (evidence_index = 0;
           evidence_index < out->candidates[candidate_index].evidence.count;
           evidence_index++) {
        status = ab_act_evidence_list_add(
            engine, &out->evidence,
            &out->candidates[candidate_index].evidence.items[evidence_index]);
        if (status != ARCHBIRD_OK)
          goto fail;
      }
    }
  }
  ab_act_evidence_list_finish(&out->evidence);
  status = add_unknown(
      engine, out, strength ? "semantic_implementation" : "semantic_repair",
      strength
          ? &out->actual_name
          : (out->actual_name.length ? &out->actual_name
                                     : &ab_value_member(check, "id")->as.text),
      strength
          ? "The structural fact transition does not determine implementation "
            "semantics."
          : "The finding determines an oracle outcome but not a static edit.",
      &finding_evidence);
  if (status == ARCHBIRD_OK && out->coverage.unknown)
    status = add_unknown(
        engine, out, out->coverage.unknown, &out->coverage.domain,
        out->coverage.unknown &&
                strcmp(out->coverage.unknown, "unmapped_dynamic_lookup") == 0
            ? "Evidence coverage retains the unmapped dynamic lookup frontier."
        : out->coverage.unknown &&
                strcmp(out->coverage.unknown, "dynamic_symbol_creation") == 0
            ? "Evidence coverage retains the dynamic symbol creation frontier."
        : out->coverage.unknown &&
                strcmp(out->coverage.unknown, "dynamic_dispatch") == 0
            ? "Evidence coverage retains the dynamic dispatch frontier."
        : out->coverage.unknown &&
                strcmp(out->coverage.unknown, "runtime_branch_coverage") == 0
            ? "Evidence coverage retains the runtime branch coverage frontier."
        : out->coverage.unknown &&
                strcmp(out->coverage.unknown, "unavailable_fact_evidence") == 0
            ? "Evidence coverage retains the unavailable fact evidence "
              "frontier."
        : out->coverage.unknown &&
                strcmp(out->coverage.unknown, "semantic_repair") == 0
            ? "Evidence coverage retains the semantic repair frontier."
            : "Evidence coverage retains the unmodeled evidence frontier.",
        &finding_evidence);
  if (status == ARCHBIRD_OK && out->candidate_count) {
    AbActEvidenceList candidate_evidence = {0};
    size_t candidate_index;
    for (candidate_index = 0;
         status == ARCHBIRD_OK && candidate_index < out->candidate_count;
         candidate_index++) {
      size_t evidence_index;
      for (evidence_index = 0;
           status == ARCHBIRD_OK &&
           evidence_index < out->candidates[candidate_index].evidence.count;
           evidence_index++)
        status = ab_act_evidence_list_add(
            engine, &candidate_evidence,
            &out->candidates[candidate_index].evidence.items[evidence_index]);
    }
    ab_act_evidence_list_finish(&candidate_evidence);
    if (status == ARCHBIRD_OK)
      status = add_unknown(
          engine, out, "ownership_unasserted", &actual->project,
          "Candidate paths are evidence, not edit authorization or ownership.",
          &candidate_evidence);
    ab_act_evidence_list_free(&candidate_evidence);
  }
  if (status == ARCHBIRD_OK && out->unknown_count > 1)
    qsort(out->unknowns, out->unknown_count, sizeof(*out->unknowns),
          unknown_compare);
  if (status == ARCHBIRD_OK)
    status = collect_preserved(engine, out);
  if (status == ARCHBIRD_OK)
    status = build_slice(engine, out);
  ab_act_evidence_list_free(&finding_evidence);
  ab_projection_data_free(engine, &projected);
  if (status != ARCHBIRD_OK)
    goto fail;
  return ARCHBIRD_OK;

fail:
  ab_act_evidence_list_free(&finding_evidence);
  ab_projection_data_free(engine, &projected);
  ab_act_proposal_data_free(out);
  return status;
}

void ab_act_proposal_data_free(AbActProposalData *proposal) {
  ArchbirdEngine *engine;
  size_t index;
  if (!proposal)
    return;
  engine = proposal->verification ? proposal->verification->engine : NULL;
  ab_string_free(engine, &proposal->fingerprint);
  ab_string_free(engine, &proposal->actual_name);
  coverage_free(engine, &proposal->coverage);
  for (index = 0; proposal->slice && index < proposal->slice_count; index++)
    ab_string_free(engine, &proposal->slice[index].name);
  ab_free(engine, proposal->slice);
  ab_projection_data_free(engine, &proposal->literal_fact);
  ab_string_free(engine, &proposal->projection.name);
  ab_string_free(engine, &proposal->projection.source);
  ab_string_free(engine, &proposal->postcondition.constraint_id);
  ab_string_free(engine, &proposal->postcondition.rationale);
  ab_string_free(engine, &proposal->postcondition.expected);
  ab_string_free(engine, &proposal->postcondition.actual);
  ab_act_evidence_list_free(&proposal->postcondition.evidence);
  ab_free(engine, proposal->preserved);
  for (index = 0; proposal->candidates && index < proposal->candidate_count;
       index++) {
    ab_string_free(engine, &proposal->candidates[index].id);
    ab_string_free(engine, &proposal->candidates[index].project);
    ab_string_free(engine, &proposal->candidates[index].path);
    ab_string_free(engine, &proposal->candidates[index].reason);
    ab_act_evidence_list_free(&proposal->candidates[index].evidence);
  }
  ab_free(engine, proposal->candidates);
  for (index = 0; proposal->unknowns && index < proposal->unknown_count;
       index++) {
    ab_string_free(engine, &proposal->unknowns[index].id);
    ab_string_free(engine, &proposal->unknowns[index].scope);
    ab_act_evidence_list_free(&proposal->unknowns[index].evidence);
  }
  ab_free(engine, proposal->unknowns);
  ab_act_evidence_list_free(&proposal->evidence);
  memset(proposal, 0, sizeof(*proposal));
}

static ArchbirdStatus render_candidates(AbBuffer *buffer,
                                        const AbActProposalData *proposal) {
  size_t index;
  ACT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < proposal->candidate_count; index++) {
    const AbActCandidate *row = &proposal->candidates[index];
    if (index)
      ACT_TRY(ab_buffer_literal(buffer, ","));
    ACT_TRY(ab_buffer_literal(buffer, "{\"coverage\":"));
    ACT_TRY(coverage_render(buffer, &proposal->coverage));
    ACT_TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
    ACT_TRY(ab_act_evidence_list_render(buffer, &row->evidence));
    ACT_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->id.data, row->id.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"kind\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->kind, strlen(row->kind)));
    ACT_TRY(ab_buffer_literal(buffer, ",\"path\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->path.data, row->path.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"project\":"));
    ACT_TRY(
        ab_buffer_json_string(buffer, row->project.data, row->project.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"reason\":"));
    ACT_TRY(
        ab_buffer_json_string(buffer, row->reason.data, row->reason.length));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_tags(AbBuffer *buffer, const AbValue *tags) {
  size_t count =
      tags && tags->kind == AB_VALUE_ARRAY ? tags->as.array.count : 0;
  const AbString **values = NULL;
  size_t index;
  size_t write = 0;
  ArchbirdStatus status;
  AbString act = {(char *)"act", 3};
  if (count == SIZE_MAX / sizeof(*values))
    return archbird_error_set(buffer->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many Act tags");
  values = (const AbString **)ab_malloc(buffer->engine,
                                        (count + 1) * sizeof(*values));
  if (!values)
    return archbird_error_set(buffer->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory rendering Act tags");
  for (index = 0; index < count; index++)
    values[index] = &tags->as.array.items[index].as.text;
  values[count++] = &act;
  for (index = 1; index < count; index++) {
    const AbString *value = values[index];
    size_t position = index;
    while (position && ab_string_compare(values[position - 1], value) > 0) {
      values[position] = values[position - 1];
      position--;
    }
    values[position] = value;
  }
  status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (write && ab_string_compare(values[index], values[index - 1]) == 0)
      continue;
    if (write)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, values[index]->data,
                                     values[index]->length);
    write++;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  ab_free(buffer->engine, values);
  return status;
}

static ArchbirdStatus render_nullable(AbBuffer *buffer, const AbValue *value) {
  return value ? ab_value_render(buffer, value)
               : ab_buffer_literal(buffer, "null");
}

static ArchbirdStatus
render_postcondition_check(AbBuffer *buffer,
                           const AbActPostcondition *postcondition) {
  ACT_TRY(ab_buffer_literal(buffer, "{\"actual\":"));
  ACT_TRY(ab_buffer_json_string(buffer, postcondition->actual.data,
                                postcondition->actual.length));
  ACT_TRY(ab_buffer_literal(buffer, ",\"assert\":"));
  ACT_TRY(ab_buffer_json_string(buffer, postcondition->assertion,
                                strlen(postcondition->assertion)));
  ACT_TRY(ab_buffer_literal(buffer, ",\"exact\":"));
  ACT_TRY(render_nullable(buffer, postcondition->exact));
  ACT_TRY(ab_buffer_literal(buffer, ",\"expected\":"));
  ACT_TRY(ab_buffer_json_string(buffer, postcondition->expected.data,
                                postcondition->expected.length));
  ACT_TRY(ab_buffer_literal(buffer, ",\"id\":"));
  ACT_TRY(ab_buffer_json_string(buffer, postcondition->constraint_id.data,
                                postcondition->constraint_id.length));
  ACT_TRY(ab_buffer_literal(buffer, ",\"mapping\":\"\",\"max\":"));
  ACT_TRY(render_nullable(buffer, postcondition->maximum));
  ACT_TRY(ab_buffer_literal(buffer, ",\"min\":"));
  ACT_TRY(render_nullable(buffer, postcondition->minimum));
  ACT_TRY(ab_buffer_literal(buffer, ",\"owner\":"));
  ACT_TRY(ab_value_render(buffer, postcondition->owner));
  ACT_TRY(ab_buffer_literal(buffer, ",\"rationale\":"));
  ACT_TRY(ab_buffer_json_string(buffer, postcondition->rationale.data,
                                postcondition->rationale.length));
  ACT_TRY(ab_buffer_literal(buffer,
                            ",\"reference_route\":\"\",\"required_routes\":"));
  ACT_TRY(postcondition->required_routes
              ? ab_value_render(buffer, postcondition->required_routes)
              : ab_buffer_literal(buffer, "[]"));
  ACT_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
  ACT_TRY(ab_value_render(buffer, postcondition->requirements));
  ACT_TRY(ab_buffer_literal(buffer, ",\"severity\":\"error\",\"tags\":"));
  ACT_TRY(render_tags(buffer, postcondition->tags));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_postconditions(AbBuffer *buffer,
                                            const AbActProposalData *proposal) {
  ACT_TRY(ab_buffer_literal(buffer, "["));
  if (proposal->has_postcondition) {
    const AbActPostcondition *row = &proposal->postcondition;
    ACT_TRY(ab_buffer_literal(buffer, "{\"constraint\":"));
    ACT_TRY(render_postcondition_check(buffer, row));
    ACT_TRY(ab_buffer_literal(buffer, ",\"coverage\":"));
    ACT_TRY(coverage_render(buffer, &proposal->coverage));
    ACT_TRY(ab_buffer_literal(buffer, ",\"derivation_strength\":"));
    ACT_TRY(
        ab_buffer_json_string(buffer, row->strength, strlen(row->strength)));
    ACT_TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
    ACT_TRY(ab_act_evidence_list_render(buffer, &row->evidence));
    ACT_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->id, strlen(row->id)));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_preserved(AbBuffer *buffer,
                                       const AbActProposalData *proposal) {
  size_t index;
  ACT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < proposal->preserved_count; index++) {
    const AbActPreserved *row = &proposal->preserved[index];
    if (index)
      ACT_TRY(ab_buffer_literal(buffer, ","));
    ACT_TRY(ab_buffer_literal(buffer, "{\"id\":"));
    ACT_TRY(ab_value_render(buffer, ab_value_member(row->constraint, "id")));
    ACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->sha256, 64));
    ACT_TRY(ab_buffer_literal(buffer, ",\"status\":"));
    ACT_TRY(
        ab_value_render(buffer, ab_value_member(row->constraint, "status")));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_projection(AbBuffer *buffer,
                                        const AbActProposalData *proposal) {
  ACT_TRY(ab_buffer_literal(buffer, "["));
  if (proposal->has_projection) {
    ACT_TRY(ab_buffer_literal(buffer, "{\"aliases\":"));
    ACT_TRY(proposal->projection.aliases
                ? ab_value_render(buffer, proposal->projection.aliases)
                : ab_buffer_literal(buffer, "{}"));
    ACT_TRY(ab_buffer_literal(buffer, ",\"keys\":[],\"name\":"));
    ACT_TRY(ab_buffer_json_string(buffer, proposal->projection.name.data,
                                  proposal->projection.name.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"selection\":\"all\",\"source\":"));
    ACT_TRY(ab_buffer_json_string(buffer, proposal->projection.source.data,
                                  proposal->projection.source.length));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_unknowns(AbBuffer *buffer,
                                      const AbActProposalData *proposal) {
  size_t index;
  ACT_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < proposal->unknown_count; index++) {
    const AbActUnknown *row = &proposal->unknowns[index];
    if (index)
      ACT_TRY(ab_buffer_literal(buffer, ","));
    ACT_TRY(ab_buffer_literal(buffer, "{\"code\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->code, strlen(row->code)));
    ACT_TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
    ACT_TRY(ab_act_evidence_list_render(buffer, &row->evidence));
    ACT_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->id.data, row->id.length));
    ACT_TRY(ab_buffer_literal(buffer, ",\"message\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->message, strlen(row->message)));
    ACT_TRY(ab_buffer_literal(buffer, ",\"scope\":"));
    ACT_TRY(ab_buffer_json_string(buffer, row->scope.data, row->scope.length));
    ACT_TRY(ab_buffer_literal(buffer, "}"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus
render_proposal_document(AbBuffer *buffer, const AbActProposalData *proposal,
                         int include_sha256, const char sha256[65]) {
  const AbValue *constraint_id =
      ab_value_member(proposal->origin_constraint, "id");
  const AbValue *assertion =
      ab_value_member(proposal->origin_constraint, "assert");
  const AbValue *tool_impl =
      ab_value_member(proposal->verification->tool, "implementation_sha256");
  const AbValue *policy_project =
      ab_value_member(proposal->verification->policy, "project");
  const AbValue *policy_sha = ab_value_member(proposal->verification->policy,
                                              "constraint_policy_sha256");
  ACT_TRY(ab_buffer_literal(
      buffer, "{\"artifact\":\"change-proposal\",\"candidates\":"));
  ACT_TRY(render_candidates(buffer, proposal));
  ACT_TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
  ACT_TRY(ab_act_evidence_list_render(buffer, &proposal->evidence));
  ACT_TRY(ab_buffer_literal(buffer, ",\"evidence_slice\":{\"entries\":"));
  ACT_TRY(render_slice_rows(buffer, proposal));
  ACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  ACT_TRY(ab_buffer_json_string(buffer, proposal->slice_sha256, 64));
  ACT_TRY(ab_buffer_literal(buffer, "},\"facts\":["));
  if (proposal->has_literal_fact)
    ACT_TRY(ab_projection_data_render(buffer, &proposal->literal_fact, 1));
  ACT_TRY(ab_buffer_literal(buffer, "],\"mutable_sources\":["));
  if (proposal->actual_name.length)
    ACT_TRY(ab_buffer_json_string(buffer, proposal->actual_name.data,
                                  proposal->actual_name.length));
  ACT_TRY(ab_buffer_literal(buffer, "],\"origin\":{\"actual\":"));
  ACT_TRY(ab_buffer_json_string(buffer, proposal->actual_name.data,
                                proposal->actual_name.length));
  ACT_TRY(ab_buffer_literal(buffer, ",\"assert\":"));
  ACT_TRY(ab_value_render(buffer, assertion));
  ACT_TRY(ab_buffer_literal(buffer, ",\"constraint\":"));
  ACT_TRY(ab_value_render(buffer, constraint_id));
  ACT_TRY(ab_buffer_literal(buffer, ",\"constraint_sha256\":"));
  ACT_TRY(
      ab_buffer_json_string(buffer, proposal->origin_constraint_sha256, 64));
  ACT_TRY(ab_buffer_literal(buffer, ",\"finding\":"));
  ACT_TRY(ab_value_render(buffer, proposal->origin_finding));
  ACT_TRY(ab_buffer_literal(buffer, ",\"finding_sha256\":"));
  ACT_TRY(ab_buffer_json_string(buffer, proposal->origin_finding_sha256, 64));
  ACT_TRY(ab_buffer_literal(buffer, "},\"postconditions\":"));
  ACT_TRY(render_postconditions(buffer, proposal));
  ACT_TRY(ab_buffer_literal(buffer, ",\"preserved_invariants\":"));
  ACT_TRY(render_preserved(buffer, proposal));
  ACT_TRY(ab_buffer_literal(buffer, ",\"projections\":"));
  ACT_TRY(render_projection(buffer, proposal));
  ACT_TRY(ab_buffer_literal(
      buffer, ",\"provenance\":\"derived\",\"schema_version\":2"));
  if (include_sha256) {
    ACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    ACT_TRY(ab_buffer_json_string(buffer, sha256, 64));
  }
  ACT_TRY(ab_buffer_literal(buffer, ",\"source\":{\"evaluation\":"));
  ACT_TRY(ab_value_render(buffer, proposal->verification->evaluation));
  ACT_TRY(ab_buffer_literal(buffer, ",\"policy\":{\"project\":"));
  ACT_TRY(ab_value_render(buffer, policy_project));
  ACT_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  ACT_TRY(ab_value_render(buffer, policy_sha));
  ACT_TRY(ab_buffer_literal(buffer, "},\"verification_sha256\":"));
  ACT_TRY(ab_buffer_json_string(buffer, proposal->verification->sha256, 64));
  ACT_TRY(ab_buffer_literal(buffer,
                            ",\"verification_tool_implementation_sha256\":"));
  ACT_TRY(ab_value_render(buffer, tool_impl));
  ACT_TRY(ab_buffer_literal(
      buffer,
      "},\"tool\":{\"implementation_sha256\":\"" ARCHBIRD_IMPLEMENTATION_SHA256
      "\",\"name\":\"archbird\",\"version\":\"" ARCHBIRD_VERSION
      "\"},\"unknowns\":"));
  ACT_TRY(render_unknowns(buffer, proposal));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_act_proposal_render_json(AbBuffer *buffer,
                                           const AbActProposalData *proposal) {
  AbBuffer payload;
  uint8_t digest[32];
  char sha256[65];
  ArchbirdStatus status;
  if (!buffer || !buffer->engine || !proposal)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_build_identity_validate(buffer->engine);
  if (status != ARCHBIRD_OK)
    return status;
  ab_buffer_init(&payload, buffer->engine);
  status = render_proposal_document(&payload, proposal, 0, NULL);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(payload.data, payload.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, sha256);
  if (status == ARCHBIRD_OK)
    status = render_proposal_document(buffer, proposal, 1, sha256);
  ab_buffer_free(&payload);
  return status;
}
