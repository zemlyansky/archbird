#include "verify_model.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expression)                                                        \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal) {
  return ab_string_copy(engine, out, literal, strlen(literal));
}

static ArchbirdStatus copy_string(ArchbirdEngine *engine, AbString *out,
                                  const AbString *source) {
  return ab_string_copy(engine, out, source ? source->data : "",
                        source ? source->length : 0);
}

void ab_verify_evidence_free(ArchbirdEngine *engine,
                             AbVerifyEvidence *evidence) {
  ab_string_free(engine, &evidence->provenance);
  ab_string_free(engine, &evidence->project);
  ab_string_free(engine, &evidence->path);
  ab_string_free(engine, &evidence->sha256);
  ab_string_free(engine, &evidence->detail);
  memset(evidence, 0, sizeof(*evidence));
}

void ab_verify_fact_item_free(ArchbirdEngine *engine, AbVerifyFactItem *item) {
  size_t index;
  if (!item)
    return;
  ab_string_free(engine, &item->key);
  ab_string_free(engine, &item->label);
  ab_value_free(engine, &item->value);
  for (index = 0; item->attributes && index < item->attribute_count; index++) {
    ab_string_free(engine, &item->attributes[index].name);
    ab_value_free(engine, &item->attributes[index].value);
  }
  ab_free(engine, item->attributes);
  for (index = 0; item->evidence && index < item->evidence_count; index++)
    ab_verify_evidence_free(engine, &item->evidence[index]);
  ab_free(engine, item->evidence);
  ab_string_free(engine, &item->state);
  ab_string_free(engine, &item->message);
  memset(item, 0, sizeof(*item));
}

void ab_verify_fact_free(ArchbirdEngine *engine, AbVerifyFactSet *fact) {
  size_t index;
  if (!fact)
    return;
  ab_string_free(engine, &fact->name);
  ab_string_free(engine, &fact->shape);
  ab_string_free(engine, &fact->provenance);
  ab_string_free(engine, &fact->project);
  for (index = 0; fact->items && index < fact->item_count; index++)
    ab_verify_fact_item_free(engine, &fact->items[index]);
  ab_free(engine, fact->items);
  ab_string_free(engine, &fact->state);
  ab_string_free(engine, &fact->message);
  memset(fact, 0, sizeof(*fact));
}

ArchbirdStatus ab_verify_fact_init(ArchbirdEngine *engine,
                                   AbVerifyFactSet *fact, const AbString *name,
                                   const char *shape, const char *provenance,
                                   const AbString *project) {
  ArchbirdStatus status;
  if (!engine || !fact || !name || !shape || !provenance)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(fact, 0, sizeof(*fact));
  status = copy_string(engine, &fact->name, name);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->shape, shape);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->provenance, provenance);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &fact->project, project);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->state, "current");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->message, "");
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(engine, fact);
  return status;
}

ArchbirdStatus
ab_verify_evidence_init(ArchbirdEngine *engine, AbVerifyEvidence *evidence,
                        const char *provenance, const AbString *project,
                        const AbString *path, uint64_t line, const char *sha256,
                        const char *detail, size_t detail_length) {
  ArchbirdStatus status;
  if (!engine || !evidence || !provenance || !sha256 || !detail)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(evidence, 0, sizeof(*evidence));
  status = copy_literal(engine, &evidence->provenance, provenance);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &evidence->project, project);
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &evidence->path, path);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &evidence->sha256, sha256);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &evidence->detail, detail, detail_length);
  if (status == ARCHBIRD_OK)
    evidence->line = line;
  else
    ab_verify_evidence_free(engine, evidence);
  return status;
}

int ab_verify_evidence_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyEvidence *left = (const AbVerifyEvidence *)left_raw;
  const AbVerifyEvidence *right = (const AbVerifyEvidence *)right_raw;
  int compared = ab_string_compare(&left->provenance, &right->provenance);
  if (!compared)
    compared = ab_string_compare(&left->project, &right->project);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  if (!compared)
    compared = (left->line > right->line) - (left->line < right->line);
  if (!compared)
    compared = ab_string_compare(&left->sha256, &right->sha256);
  if (!compared)
    compared = ab_string_compare(&left->detail, &right->detail);
  return compared;
}

static int item_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyFactItem *left = (const AbVerifyFactItem *)left_raw;
  const AbVerifyFactItem *right = (const AbVerifyFactItem *)right_raw;
  return ab_string_compare(&left->key, &right->key);
}

static ArchbirdStatus append_evidence_copy(ArchbirdEngine *engine,
                                           AbVerifyFactItem *target,
                                           const AbVerifyEvidence *source) {
  AbVerifyEvidence *resized;
  ArchbirdStatus status;
  if (target->evidence_count == target->evidence_capacity) {
    size_t capacity =
        target->evidence_capacity ? target->evidence_capacity * 2 : 4;
    if (capacity < target->evidence_capacity ||
        capacity > SIZE_MAX / sizeof(*target->evidence))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too much verification evidence");
    resized = (AbVerifyEvidence *)ab_realloc(
        engine, target->evidence, capacity * sizeof(*target->evidence));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory merging verification evidence");
    target->evidence = resized;
    target->evidence_capacity = capacity;
  }
  memset(&target->evidence[target->evidence_count], 0,
         sizeof(*target->evidence));
  status = ab_verify_evidence_init(
      engine, &target->evidence[target->evidence_count],
      source->provenance.data, &source->project, &source->path, source->line,
      source->sha256.data, source->detail.data, source->detail.length);
  if (status == ARCHBIRD_OK)
    target->evidence_count++;
  return status;
}

ArchbirdStatus ab_verify_fact_add_item(ArchbirdEngine *engine,
                                       AbVerifyFactSet *fact,
                                       AbVerifyFactItem *item) {
  AbVerifyFactItem *resized;
  size_t index;
  ArchbirdStatus status;
  if (!engine || !fact || !item)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < fact->item_count; index++) {
    AbVerifyFactItem *previous = &fact->items[index];
    size_t evidence_index;
    if (!ab_string_equal(&previous->key, &item->key))
      continue;
    for (evidence_index = 0; evidence_index < item->evidence_count;
         evidence_index++) {
      status = append_evidence_copy(engine, previous,
                                    &item->evidence[evidence_index]);
      if (status != ARCHBIRD_OK)
        return status;
    }
    ab_string_free(engine, &fact->state);
    ab_string_free(engine, &fact->message);
    ab_string_free(engine, &previous->state);
    ab_string_free(engine, &previous->message);
    status = copy_literal(engine, &fact->state, "unknown");
    if (status == ARCHBIRD_OK)
      status = copy_literal(engine, &fact->message, "normalization collision");
    if (status == ARCHBIRD_OK)
      status = copy_literal(engine, &previous->state, "unknown");
    if (status == ARCHBIRD_OK)
      status =
          copy_literal(engine, &previous->message, "normalization collision");
    ab_verify_fact_item_free(engine, item);
    return status;
  }
  if (fact->item_count == fact->item_capacity) {
    size_t capacity = fact->item_capacity ? fact->item_capacity * 2 : 8;
    if (capacity < fact->item_capacity ||
        capacity > SIZE_MAX / sizeof(*fact->items))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many verification fact items");
    resized = (AbVerifyFactItem *)ab_realloc(engine, fact->items,
                                             capacity * sizeof(*fact->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification facts");
    fact->items = resized;
    fact->item_capacity = capacity;
  }
  fact->items[fact->item_count++] = *item;
  memset(item, 0, sizeof(*item));
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_verify_evidence_render(AbBuffer *buffer,
                                         const AbVerifyEvidence *evidence) {
  TRY(ab_buffer_literal(buffer, "{\"detail\":"));
  TRY(ab_buffer_json_string(buffer, evidence->detail.data,
                            evidence->detail.length));
  TRY(ab_buffer_literal(buffer, ",\"line\":"));
  TRY(ab_buffer_u64(buffer, evidence->line));
  TRY(ab_buffer_literal(buffer, ",\"path\":"));
  TRY(ab_buffer_json_string(buffer, evidence->path.data,
                            evidence->path.length));
  TRY(ab_buffer_literal(buffer, ",\"project\":"));
  TRY(ab_buffer_json_string(buffer, evidence->project.data,
                            evidence->project.length));
  TRY(ab_buffer_literal(buffer, ",\"provenance\":"));
  TRY(ab_buffer_json_string(buffer, evidence->provenance.data,
                            evidence->provenance.length));
  TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  TRY(ab_buffer_json_string(buffer, evidence->sha256.data,
                            evidence->sha256.length));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_attributes_list(AbBuffer *buffer,
                                             const AbVerifyFactItem *item) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < item->attribute_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_buffer_literal(buffer, "["));
    TRY(ab_buffer_json_string(buffer, item->attributes[index].name.data,
                              item->attributes[index].name.length));
    TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_value_render(buffer, &item->attributes[index].value));
    TRY(ab_buffer_literal(buffer, "]"));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_attributes_object(AbBuffer *buffer,
                                               const AbVerifyFactItem *item) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{"));
  for (index = 0; index < item->attribute_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_buffer_json_string(buffer, item->attributes[index].name.data,
                              item->attributes[index].name.length));
    TRY(ab_buffer_literal(buffer, ":"));
    TRY(ab_value_render(buffer, &item->attributes[index].value));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_evidence_array(AbBuffer *buffer,
                                            const AbVerifyFactItem *item) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < item->evidence_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(ab_verify_evidence_render(buffer, &item->evidence[index]));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_digest_item(AbBuffer *buffer,
                                         const AbVerifyFactItem *item) {
  TRY(ab_buffer_literal(buffer, "{\"attributes\":"));
  TRY(render_attributes_list(buffer, item));
  TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
  TRY(render_evidence_array(buffer, item));
  TRY(ab_buffer_literal(buffer, ",\"key\":"));
  TRY(ab_buffer_json_string(buffer, item->key.data, item->key.length));
  TRY(ab_buffer_literal(buffer, ",\"label\":"));
  TRY(ab_buffer_json_string(buffer, item->label.data, item->label.length));
  TRY(ab_buffer_literal(buffer, ",\"message\":"));
  TRY(ab_buffer_json_string(buffer, item->message.data, item->message.length));
  TRY(ab_buffer_literal(buffer, ",\"state\":"));
  TRY(ab_buffer_json_string(buffer, item->state.data, item->state.length));
  TRY(ab_buffer_literal(buffer, ",\"value\":"));
  TRY(ab_value_render(buffer, &item->value));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_digest_document(AbBuffer *buffer,
                                             const AbVerifyFactSet *fact) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{\"items\":["));
  for (index = 0; index < fact->item_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(render_digest_item(buffer, &fact->items[index]));
  }
  TRY(ab_buffer_literal(buffer, "],\"message\":"));
  TRY(ab_buffer_json_string(buffer, fact->message.data, fact->message.length));
  TRY(ab_buffer_literal(buffer, ",\"name\":"));
  TRY(ab_buffer_json_string(buffer, fact->name.data, fact->name.length));
  TRY(ab_buffer_literal(buffer, ",\"project\":"));
  TRY(ab_buffer_json_string(buffer, fact->project.data, fact->project.length));
  TRY(ab_buffer_literal(buffer, ",\"provenance\":"));
  TRY(ab_buffer_json_string(buffer, fact->provenance.data,
                            fact->provenance.length));
  TRY(ab_buffer_literal(buffer, ",\"shape\":"));
  TRY(ab_buffer_json_string(buffer, fact->shape.data, fact->shape.length));
  TRY(ab_buffer_literal(buffer, ",\"state\":"));
  TRY(ab_buffer_json_string(buffer, fact->state.data, fact->state.length));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_verify_fact_finish(ArchbirdEngine *engine,
                                     AbVerifyFactSet *fact) {
  AbBuffer canonical;
  uint8_t digest[32];
  size_t item_index;
  ArchbirdStatus status;
  if (!engine || !fact)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (fact->item_count > 1)
    qsort(fact->items, fact->item_count, sizeof(*fact->items), item_compare);
  for (item_index = 0; item_index < fact->item_count; item_index++) {
    AbVerifyFactItem *item = &fact->items[item_index];
    size_t write = 0;
    size_t read;
    if (item->evidence_count > 1)
      qsort(item->evidence, item->evidence_count, sizeof(*item->evidence),
            ab_verify_evidence_compare);
    for (read = 0; read < item->evidence_count; read++) {
      if (write && ab_verify_evidence_compare(&item->evidence[write - 1],
                                              &item->evidence[read]) == 0) {
        ab_verify_evidence_free(engine, &item->evidence[read]);
        continue;
      }
      if (write != read) {
        item->evidence[write] = item->evidence[read];
        memset(&item->evidence[read], 0, sizeof(item->evidence[read]));
      }
      write++;
    }
    item->evidence_count = write;
  }
  ab_buffer_init(&canonical, engine);
  status = render_digest_document(&canonical, fact);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, fact->sha256);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus render_output_item(AbBuffer *buffer,
                                         const AbVerifyFactItem *item) {
  TRY(ab_buffer_literal(buffer, "{\"attributes\":"));
  TRY(render_attributes_object(buffer, item));
  TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
  TRY(render_evidence_array(buffer, item));
  TRY(ab_buffer_literal(buffer, ",\"key\":"));
  TRY(ab_buffer_json_string(buffer, item->key.data, item->key.length));
  TRY(ab_buffer_literal(buffer, ",\"label\":"));
  TRY(ab_buffer_json_string(buffer, item->label.data, item->label.length));
  TRY(ab_buffer_literal(buffer, ",\"message\":"));
  TRY(ab_buffer_json_string(buffer, item->message.data, item->message.length));
  TRY(ab_buffer_literal(buffer, ",\"state\":"));
  TRY(ab_buffer_json_string(buffer, item->state.data, item->state.length));
  TRY(ab_buffer_literal(buffer, ",\"value\":"));
  TRY(ab_value_render(buffer, &item->value));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_verify_fact_render(AbBuffer *buffer,
                                     const AbVerifyFactSet *fact,
                                     int include_sha256) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{\"items\":["));
  for (index = 0; index < fact->item_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(render_output_item(buffer, &fact->items[index]));
  }
  TRY(ab_buffer_literal(buffer, "],\"message\":"));
  TRY(ab_buffer_json_string(buffer, fact->message.data, fact->message.length));
  TRY(ab_buffer_literal(buffer, ",\"name\":"));
  TRY(ab_buffer_json_string(buffer, fact->name.data, fact->name.length));
  TRY(ab_buffer_literal(buffer, ",\"project\":"));
  TRY(ab_buffer_json_string(buffer, fact->project.data, fact->project.length));
  TRY(ab_buffer_literal(buffer, ",\"provenance\":"));
  TRY(ab_buffer_json_string(buffer, fact->provenance.data,
                            fact->provenance.length));
  if (include_sha256) {
    TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    TRY(ab_buffer_json_string(buffer, fact->sha256, 64));
  }
  TRY(ab_buffer_literal(buffer, ",\"shape\":"));
  TRY(ab_buffer_json_string(buffer, fact->shape.data, fact->shape.length));
  TRY(ab_buffer_literal(buffer, ",\"state\":"));
  TRY(ab_buffer_json_string(buffer, fact->state.data, fact->state.length));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_verify_fact_unknown(ArchbirdEngine *engine,
                                      AbVerifyFactSet *fact,
                                      const AbString *name,
                                      const AbString *project,
                                      const char *shape, const char *message) {
  AbVerifyFactItem item = {0};
  ArchbirdStatus status =
      ab_verify_fact_init(engine, fact, name, shape, "derived", project);
  if (status == ARCHBIRD_OK) {
    ab_string_free(engine, &fact->state);
    ab_string_free(engine, &fact->message);
    status = copy_literal(engine, &fact->state, "unknown");
  }
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->message, message);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item.key, "extractor");
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &item.label, name);
  item.value.kind = AB_VALUE_NULL;
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item.state, "unknown");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &item.message, message);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_add_item(engine, fact, &item);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_finish(engine, fact);
  if (status != ARCHBIRD_OK) {
    ab_verify_fact_item_free(engine, &item);
    ab_verify_fact_free(engine, fact);
  }
  return status;
}
