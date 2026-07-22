#include "verify_model.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

int ab_verify_string_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         (!length || memcmp(value->as.text.data, literal, length) == 0);
}

int ab_verify_nonblank(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    char byte = value->as.text.data[index];
    if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
      return 1;
  }
  return 0;
}

int ab_verify_path_is_repository(const AbValue *value) {
  const char *path;
  size_t length;
  size_t index;
  size_t segment = 0;
  if (!ab_verify_nonblank(value))
    return 0;
  path = value->as.text.data;
  length = value->as.text.length;
  if (path[0] == '/' || path[0] == '\\' || path[length - 1] == '/' ||
      path[length - 1] == '\\' ||
      (length >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':'))
    return 0;
  for (index = 0; index <= length; index++) {
    if (index < length && path[index] != '/' && path[index] != '\\') {
      if (path[index] == '\0')
        return 0;
      continue;
    }
    if (index == segment || (index - segment == 1 && path[segment] == '.') ||
        (index - segment == 2 && path[segment] == '.' &&
         path[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

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
  ab_free(engine, fact->item_slots);
  ab_string_free(engine, &fact->state);
  ab_string_free(engine, &fact->message);
  ab_string_free(engine, &fact->selection.unit);
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
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &fact->selection.unit, "item");
  if (status != ARCHBIRD_OK)
    ab_verify_fact_free(engine, fact);
  return status;
}

ArchbirdStatus
ab_verify_fact_selection_exact(ArchbirdEngine *engine, AbVerifyFactSet *fact,
                               const char *unit, uint64_t universe,
                               uint64_t selected, uint64_t excluded,
                               uint64_t unsupported, int truncated) {
  ArchbirdStatus status;
  if (!engine || !fact || !unit || selected > universe || excluded > universe ||
      selected + excluded < selected || selected + excluded != universe)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_string_free(engine, &fact->selection.unit);
  status = copy_literal(engine, &fact->selection.unit, unit);
  if (status != ARCHBIRD_OK)
    return status;
  fact->selection.universe = universe;
  fact->selection.selected = selected;
  fact->selection.excluded = excluded;
  fact->selection.unsupported = unsupported;
  fact->selection.truncated = !!truncated;
  fact->selection.has_universe = 1;
  fact->selection.has_selected = 1;
  fact->selection.has_excluded = 1;
  fact->selection.has_unsupported = 1;
  fact->selection.has_truncated = 1;
  return ARCHBIRD_OK;
}

const char *
ab_verify_fact_selection_classification(const AbVerifyFactSet *fact) {
  uint64_t accounted;
  if (!fact ||
      (fact->state.length == 7 && memcmp(fact->state.data, "unknown", 7) == 0))
    return "unknown";
  if (!(fact->state.length == 7 && memcmp(fact->state.data, "current", 7) == 0))
    return "incomplete";
  if ((fact->selection.has_truncated && fact->selection.truncated) ||
      (fact->selection.has_unsupported && fact->selection.unsupported) ||
      (fact->selection.has_unknown && fact->selection.unknown))
    return "incomplete";
  if (fact->selection.has_selected && fact->selection.has_evaluated &&
      fact->selection.has_unknown) {
    accounted = fact->selection.evaluated + fact->selection.unknown;
    if (accounted < fact->selection.evaluated ||
        accounted != fact->selection.selected)
      return "incomplete";
  }
  if (fact->selection.has_universe && fact->selection.has_selected &&
      fact->selection.has_evaluated && fact->selection.has_excluded &&
      fact->selection.has_unsupported && fact->selection.has_unknown &&
      fact->selection.has_truncated)
    return "complete";
  return "bounded";
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

static int attribute_compare(const void *left_raw, const void *right_raw) {
  const AbObjectField *left = (const AbObjectField *)left_raw;
  const AbObjectField *right = (const AbObjectField *)right_raw;
  return ab_string_compare(&left->name, &right->name);
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

static uint64_t fact_key_hash(const AbString *key) {
  uint64_t hash = UINT64_C(14695981039346656037);
  size_t index;
  for (index = 0; index < key->length; index++) {
    hash ^= (unsigned char)key->data[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static void fact_index_insert(AbVerifyFactSet *fact, size_t item_index) {
  size_t slot = (size_t)fact_key_hash(&fact->items[item_index].key) &
                (fact->item_slot_count - 1);
  while (fact->item_slots[slot])
    slot = (slot + 1) & (fact->item_slot_count - 1);
  fact->item_slots[slot] = item_index + 1;
}

static ArchbirdStatus fact_index_reserve(ArchbirdEngine *engine,
                                         AbVerifyFactSet *fact,
                                         size_t required_items) {
  size_t capacity = fact->item_slot_count ? fact->item_slot_count : 16;
  size_t *slots;
  size_t index;
  while (required_items > capacity - capacity / 4) {
    if (capacity > SIZE_MAX / 2)
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many verification fact identities");
    capacity *= 2;
  }
  if (capacity == fact->item_slot_count)
    return ARCHBIRD_OK;
  slots = (size_t *)ab_calloc(engine, capacity, sizeof(*slots));
  if (!slots)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory indexing verification facts");
  ab_free(engine, fact->item_slots);
  fact->item_slots = slots;
  fact->item_slot_count = capacity;
  for (index = 0; index < fact->item_count; index++)
    fact_index_insert(fact, index);
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_verify_fact_find_item(ArchbirdEngine *engine,
                                        AbVerifyFactSet *fact,
                                        const AbString *key,
                                        AbVerifyFactItem **out) {
  size_t slot;
  ArchbirdStatus status;
  if (!engine || !fact || !key || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  *out = NULL;
  if (!fact->item_count)
    return ARCHBIRD_OK;
  status = fact_index_reserve(engine, fact, fact->item_count);
  if (status != ARCHBIRD_OK)
    return status;
  slot = (size_t)fact_key_hash(key) & (fact->item_slot_count - 1);
  while (fact->item_slots[slot]) {
    size_t item_index = fact->item_slots[slot] - 1;
    if (ab_string_equal(&fact->items[item_index].key, key)) {
      *out = &fact->items[item_index];
      return ARCHBIRD_OK;
    }
    slot = (slot + 1) & (fact->item_slot_count - 1);
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_verify_fact_add_item(ArchbirdEngine *engine,
                                       AbVerifyFactSet *fact,
                                       AbVerifyFactItem *item) {
  AbVerifyFactItem *resized;
  AbVerifyFactItem *previous;
  ArchbirdStatus status;
  if (!engine || !fact || !item)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_verify_fact_find_item(engine, fact, &item->key, &previous);
  if (status != ARCHBIRD_OK)
    return status;
  if (previous) {
    size_t evidence_index;
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
  status = fact_index_reserve(engine, fact, fact->item_count + 1);
  if (status != ARCHBIRD_OK)
    return status;
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
  fact_index_insert(fact, fact->item_count - 1);
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
  ab_free(engine, fact->item_slots);
  fact->item_slots = NULL;
  fact->item_slot_count = 0;
  if (fact->item_count > 1)
    qsort(fact->items, fact->item_count, sizeof(*fact->items), item_compare);
  for (item_index = 0; item_index < fact->item_count; item_index++) {
    AbVerifyFactItem *item = &fact->items[item_index];
    size_t write = 0;
    size_t read;
    if (item->attribute_count > 1)
      qsort(item->attributes, item->attribute_count, sizeof(*item->attributes),
            attribute_compare);
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
  if (!(fact->state.length == 7 &&
        memcmp(fact->state.data, "unknown", 7) == 0)) {
    uint64_t evaluated = 0;
    uint64_t unknown = 0;
    for (item_index = 0; item_index < fact->item_count; item_index++) {
      const AbString *state = &fact->items[item_index].state;
      if (state->length == 7 && memcmp(state->data, "unknown", 7) == 0)
        unknown++;
      else
        evaluated++;
    }
    if (!fact->selection.has_selected) {
      fact->selection.selected = (uint64_t)fact->item_count;
      fact->selection.has_selected = 1;
    }
    fact->selection.evaluated = evaluated;
    fact->selection.unknown = unknown;
    fact->selection.has_evaluated = 1;
    fact->selection.has_unknown = 1;
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

ArchbirdStatus ab_verify_fact_render_content(AbBuffer *buffer,
                                             const AbVerifyFactSet *fact) {
  size_t index;
  TRY(ab_buffer_literal(buffer, "{\"items\":["));
  for (index = 0; index < fact->item_count; index++) {
    if (index)
      TRY(ab_buffer_literal(buffer, ","));
    TRY(render_output_item(buffer, &fact->items[index]));
  }
  TRY(ab_buffer_literal(buffer, "],\"message\":"));
  TRY(ab_buffer_json_string(buffer, fact->message.data, fact->message.length));
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
    status = copy_literal(engine, &item.key, "projection");
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
