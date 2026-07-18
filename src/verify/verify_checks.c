#include "verify_checks.h"

#include "map_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MappedItem {
  const AbString *key;
  const AbVerifyFactItem *item;
  int collision;
} MappedItem;

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal) {
  return ab_string_copy(engine, out, literal, strlen(literal));
}

static ArchbirdStatus copy_string(ArchbirdEngine *engine, AbString *out,
                                  const AbString *source) {
  return ab_string_copy(engine, out, source ? source->data : "",
                        source ? source->length : 0);
}

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static void string_array_free(ArchbirdEngine *engine, AbStringArray *array) {
  size_t index;
  for (index = 0; array->items && index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static int evidence_compare(const void *left_raw, const void *right_raw) {
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

static ArchbirdStatus evidence_append(ArchbirdEngine *engine,
                                      AbVerifyEvidence **rows, size_t *count,
                                      size_t *capacity,
                                      const AbVerifyEvidence *source) {
  AbVerifyEvidence *resized;
  ArchbirdStatus status;
  if (*count == *capacity) {
    size_t next = *capacity ? *capacity * 2 : 4;
    if (next < *capacity || next > SIZE_MAX / sizeof(**rows))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too much verification finding evidence");
    resized =
        (AbVerifyEvidence *)ab_realloc(engine, *rows, next * sizeof(**rows));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing finding evidence");
    *rows = resized;
    *capacity = next;
  }
  memset(&(*rows)[*count], 0, sizeof(**rows));
  status = ab_verify_evidence_init(
      engine, &(*rows)[*count], source->provenance.data, &source->project,
      &source->path, source->line, source->sha256.data, source->detail.data,
      source->detail.length);
  if (status == ARCHBIRD_OK)
    (*count)++;
  return status;
}

static void evidence_array_free(ArchbirdEngine *engine, AbVerifyEvidence *rows,
                                size_t count) {
  size_t index;
  for (index = 0; rows && index < count; index++)
    ab_verify_evidence_free(engine, &rows[index]);
  ab_free(engine, rows);
}

static int evidence_equal(const AbVerifyEvidence *left,
                          const AbVerifyEvidence *right) {
  return evidence_compare(left, right) == 0;
}

static void evidence_sort_unique(ArchbirdEngine *engine, AbVerifyEvidence *rows,
                                 size_t *count) {
  size_t read_index;
  size_t write_index;
  if (*count < 2)
    return;
  qsort(rows, *count, sizeof(*rows), evidence_compare);
  write_index = 1;
  for (read_index = 1; read_index < *count; read_index++) {
    if (evidence_equal(&rows[write_index - 1], &rows[read_index])) {
      ab_verify_evidence_free(engine, &rows[read_index]);
      continue;
    }
    if (write_index != read_index) {
      rows[write_index] = rows[read_index];
      memset(&rows[read_index], 0, sizeof(rows[read_index]));
    }
    write_index++;
  }
  *count = write_index;
}

static void finding_free(ArchbirdEngine *engine, AbVerifyFinding *finding) {
  if (!finding)
    return;
  ab_string_free(engine, &finding->fingerprint);
  ab_string_free(engine, &finding->comparison);
  ab_string_free(engine, &finding->evidence_state);
  ab_string_free(engine, &finding->applicability);
  ab_string_free(engine, &finding->disposition);
  ab_string_free(engine, &finding->key);
  ab_string_free(engine, &finding->message);
  evidence_array_free(engine, finding->evidence, finding->evidence_count);
  ab_string_free(engine, &finding->waiver);
  ab_string_free(engine, &finding->waiver_note);
  ab_string_free(engine, &finding->baseline_state);
  memset(finding, 0, sizeof(*finding));
}

void ab_verify_check_result_free(ArchbirdEngine *engine,
                                 AbVerifyCheckResult *result) {
  size_t index;
  if (!result)
    return;
  ab_string_free(engine, &result->status);
  for (index = 0; result->findings && index < result->finding_count; index++)
    finding_free(engine, &result->findings[index]);
  ab_free(engine, result->findings);
  string_array_free(engine, &result->coverage);
  evidence_array_free(engine, result->witnesses, result->witness_count);
  memset(result, 0, sizeof(*result));
}

static ArchbirdStatus fact_summary(ArchbirdEngine *engine,
                                   const AbVerifyFactSet *fact,
                                   AbVerifyEvidence *out) {
  AbBuffer detail;
  ArchbirdStatus status;
  ab_buffer_init(&detail, engine);
  status = ab_buffer_literal(&detail, "fact ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, fact->name.data, fact->name.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " shape=");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, fact->shape.data, fact->shape.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " items=");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&detail, fact->item_count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&detail, " state=");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&detail, fact->state.data, fact->state.length);
  if (status == ARCHBIRD_OK) {
    AbString empty = {0};
    status = ab_verify_evidence_init(engine, out, fact->provenance.data,
                                     &fact->project, &empty, 0, fact->sha256,
                                     (const char *)detail.data, detail.length);
  }
  ab_buffer_free(&detail);
  return status;
}

static ArchbirdStatus result_add_witness(ArchbirdEngine *engine,
                                         AbVerifyCheckResult *result,
                                         const AbVerifyEvidence *evidence) {
  return evidence_append(engine, &result->witnesses, &result->witness_count,
                         &result->witness_capacity, evidence);
}

static ArchbirdStatus result_add_fact_witness(ArchbirdEngine *engine,
                                              AbVerifyCheckResult *result,
                                              const AbVerifyFactSet *fact) {
  AbVerifyEvidence evidence = {0};
  ArchbirdStatus status = fact_summary(engine, fact, &evidence);
  if (status == ARCHBIRD_OK)
    status = result_add_witness(engine, result, &evidence);
  ab_verify_evidence_free(engine, &evidence);
  return status;
}

static ArchbirdStatus finding_fingerprint(ArchbirdEngine *engine,
                                          const AbString *check,
                                          const char *comparison,
                                          const AbString *key, AbString *out) {
  AbBuffer canonical;
  uint8_t digest[32];
  char hex[65];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, engine);
  status = ab_buffer_literal(&canonical, "{\"check\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&canonical, check->data, check->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&canonical, ",\"comparison\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&canonical, comparison, strlen(comparison));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&canonical, ",\"key\":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_json_string(&canonical, key->data, key->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&canonical, "}");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK) {
    archbird_sha256_hex(digest, hex);
    status = ab_string_copy(engine, out, hex, 64);
  }
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus
finding_init_n(ArchbirdEngine *engine, AbVerifyFinding *finding,
               const AbValue *check, const char *comparison,
               const AbString *key, const char *message, size_t message_length,
               const char *evidence_state) {
  const AbValue *check_id = ab_value_member(check, "id");
  ArchbirdStatus status;
  memset(finding, 0, sizeof(*finding));
  status = finding_fingerprint(engine, &check_id->as.text, comparison, key,
                               &finding->fingerprint);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->comparison, comparison);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->evidence_state, evidence_state);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->applicability, "applicable");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->disposition, "open");
  if (status == ARCHBIRD_OK)
    status = copy_string(engine, &finding->key, key);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(engine, &finding->message, message, message_length);
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->waiver, "");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->waiver_note, "");
  if (status == ARCHBIRD_OK)
    status = copy_literal(engine, &finding->baseline_state, "none");
  if (status != ARCHBIRD_OK)
    finding_free(engine, finding);
  return status;
}

static ArchbirdStatus
finding_init_literal(ArchbirdEngine *engine, AbVerifyFinding *finding,
                     const AbValue *check, const char *comparison,
                     const AbString *key, const char *message,
                     const char *evidence_state) {
  return finding_init_n(engine, finding, check, comparison, key, message,
                        strlen(message), evidence_state);
}

static ArchbirdStatus finding_add_evidence(ArchbirdEngine *engine,
                                           AbVerifyFinding *finding,
                                           const AbVerifyEvidence *evidence) {
  return evidence_append(engine, &finding->evidence, &finding->evidence_count,
                         &finding->evidence_capacity, evidence);
}

static ArchbirdStatus finding_add_fact_summary(ArchbirdEngine *engine,
                                               AbVerifyFinding *finding,
                                               const AbVerifyFactSet *fact) {
  AbVerifyEvidence evidence = {0};
  ArchbirdStatus status = fact_summary(engine, fact, &evidence);
  if (status == ARCHBIRD_OK)
    status = finding_add_evidence(engine, finding, &evidence);
  ab_verify_evidence_free(engine, &evidence);
  return status;
}

static ArchbirdStatus result_add_finding(ArchbirdEngine *engine,
                                         AbVerifyCheckResult *result,
                                         AbVerifyFinding *finding) {
  AbVerifyFinding *resized;
  if (result->finding_count == result->finding_capacity) {
    size_t capacity =
        result->finding_capacity ? result->finding_capacity * 2 : 4;
    if (capacity < result->finding_capacity ||
        capacity > SIZE_MAX / sizeof(*result->findings))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many verification findings");
    resized = (AbVerifyFinding *)ab_realloc(
        engine, result->findings, capacity * sizeof(*result->findings));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification findings");
    result->findings = resized;
    result->finding_capacity = capacity;
  }
  evidence_sort_unique(engine, finding->evidence, &finding->evidence_count);
  result->findings[result->finding_count++] = *finding;
  memset(finding, 0, sizeof(*finding));
  return ARCHBIRD_OK;
}

static ArchbirdStatus coverage_add(ArchbirdEngine *engine,
                                   AbVerifyCheckResult *result,
                                   const AbString *value) {
  AbString *resized;
  size_t index;
  for (index = 0; index < result->coverage.count; index++)
    if (ab_string_equal(&result->coverage.items[index], value))
      return ARCHBIRD_OK;
  resized = (AbString *)ab_realloc(engine, result->coverage.items,
                                   (result->coverage.count + 1) *
                                       sizeof(*result->coverage.items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing check coverage");
  result->coverage.items = resized;
  memset(&result->coverage.items[result->coverage.count], 0,
         sizeof(*result->coverage.items));
  if (copy_string(engine, &result->coverage.items[result->coverage.count],
                  value) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  result->coverage.count++;
  return ARCHBIRD_OK;
}

static const AbVerifyFactSet *find_fact(const AbVerificationContext *context,
                                        const AbString *name) {
  size_t low = 0;
  size_t high = context->fact_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&context->facts[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &context->facts[middle];
  }
  return NULL;
}

static const AbString *item_attribute(const AbVerifyFactItem *item,
                                      const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        memcmp(item->attributes[index].name.data, name, length) == 0 &&
        item->attributes[index].value.kind == AB_VALUE_STRING)
      return &item->attributes[index].value.as.text;
  return NULL;
}

static const AbString *mapping_target(const AbValue *mapping,
                                      const AbString *key) {
  const AbValue *aliases =
      mapping ? ab_value_member(mapping, "actual_to_expected") : NULL;
  size_t low = 0;
  size_t high;
  if (!aliases || aliases->kind != AB_VALUE_OBJECT)
    return key;
  high = aliases->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&aliases->as.object.fields[middle].name, key);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &aliases->as.object.fields[middle].value.as.text;
  }
  return key;
}

static const AbValue *find_mapping(const AbVerificationContext *context,
                                   const AbValue *check) {
  const AbValue *name = ab_value_member(check, "mapping");
  size_t low = 0;
  size_t high;
  if (!name || !context->suite.mappings)
    return NULL;
  high = context->suite.mappings->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(
        &context->suite.mappings->as.object.fields[middle].name,
        &name->as.text);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &context->suite.mappings->as.object.fields[middle].value;
  }
  return NULL;
}

static int mapped_compare(const void *left_raw, const void *right_raw) {
  const MappedItem *left = (const MappedItem *)left_raw;
  const MappedItem *right = (const MappedItem *)right_raw;
  return ab_string_compare(left->key, right->key);
}

static ArchbirdStatus unavailable_finding(ArchbirdEngine *engine,
                                          const AbValue *check,
                                          const AbVerifyFactSet *fact,
                                          AbVerifyFinding *out,
                                          int *unavailable) {
  AbBuffer message;
  AbString key;
  ArchbirdStatus status;
  *unavailable = !string_literal(&fact->state, "current");
  if (!*unavailable)
    return ARCHBIRD_OK;
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&message, "extractor:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, fact->name.data, fact->name.length);
  key.data = (char *)message.data;
  key.length = message.length;
  if (status == ARCHBIRD_OK) {
    AbBuffer detail;
    ab_buffer_init(&detail, engine);
    if (fact->message.length)
      status =
          ab_buffer_append(&detail, fact->message.data, fact->message.length);
    else {
      status = ab_buffer_literal(&detail, "extractor ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&detail, fact->name.data, fact->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&detail, " is ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&detail, fact->state.data, fact->state.length);
    }
    if (status == ARCHBIRD_OK)
      status = finding_init_n(engine, out, check, "different", &key,
                              (const char *)detail.data, detail.length,
                              fact->state.data);
    ab_buffer_free(&detail);
  }
  if (status == ARCHBIRD_OK)
    status = finding_add_fact_summary(engine, out, fact);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus item_unknown_finding(ArchbirdEngine *engine,
                                           const AbValue *check,
                                           const AbVerifyFactItem *item,
                                           AbVerifyFinding *out) {
  AbBuffer message;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&message, engine);
  if (item->message.length)
    status =
        ab_buffer_append(&message, item->message.data, item->message.length);
  else {
    status = ab_buffer_literal(&message, "fact ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&message, item->label.data, item->label.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, " is ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&message, item->state.data, item->state.length);
  }
  if (status == ARCHBIRD_OK)
    status = finding_init_n(engine, out, check, "different", &item->key,
                            (const char *)message.data, message.length,
                            item->state.data);
  for (index = 0; status == ARCHBIRD_OK && index < item->evidence_count;
       index++)
    status = finding_add_evidence(engine, out, &item->evidence[index]);
  ab_buffer_free(&message);
  return status;
}

static int string_compare_qsort(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static int finding_compare_full(const void *left_raw, const void *right_raw) {
  const AbVerifyFinding *left = (const AbVerifyFinding *)left_raw;
  const AbVerifyFinding *right = (const AbVerifyFinding *)right_raw;
  int compared = ab_string_compare(&left->comparison, &right->comparison);
  if (!compared)
    compared = ab_string_compare(&left->key, &right->key);
  if (!compared)
    compared = ab_string_compare(&left->fingerprint, &right->fingerprint);
  return compared;
}

static int finding_compare_key(const void *left_raw, const void *right_raw) {
  const AbVerifyFinding *left = (const AbVerifyFinding *)left_raw;
  const AbVerifyFinding *right = (const AbVerifyFinding *)right_raw;
  return ab_string_compare(&left->key, &right->key);
}

static ArchbirdStatus result_init(ArchbirdEngine *engine,
                                  AbVerifyCheckResult *result,
                                  const AbValue *check,
                                  const AbVerifyFactSet *first,
                                  const AbVerifyFactSet *second) {
  ArchbirdStatus status;
  memset(result, 0, sizeof(*result));
  result->spec = check;
  status = first ? result_add_fact_witness(engine, result, first) : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK && second)
    status = result_add_fact_witness(engine, result, second);
  return status;
}

static ArchbirdStatus result_set_status(ArchbirdEngine *engine,
                                        AbVerifyCheckResult *result) {
  size_t index;
  int applicable = 0;
  int open = 0;
  int failure = 0;
  const char *status;
  ab_string_free(engine, &result->status);
  /* Check witnesses preserve the declared operand order.  Finding evidence is
   * a set and is sorted separately, but the oracle intentionally reports
   * expected before actual even when their digests sort differently. */
  if (result->coverage.count > 1)
    qsort(result->coverage.items, result->coverage.count,
          sizeof(*result->coverage.items), string_compare_qsort);
  for (index = 0; index < result->finding_count; index++) {
    const AbVerifyFinding *finding = &result->findings[index];
    if (!string_literal(&finding->applicability, "applicable"))
      continue;
    applicable = 1;
    if (!string_literal(&finding->disposition, "open"))
      continue;
    open = 1;
    if (string_literal(&finding->evidence_state, "current") &&
        !string_literal(&finding->comparison, "equal"))
      failure = 1;
  }
  if (!result->finding_count)
    status = "pass";
  else if (!applicable)
    status = "not_applicable";
  else if (!open)
    status = "waived";
  else if (failure)
    status = "fail";
  else
    status = "unknown";
  if (!strcmp(status, "not_applicable") && result->coverage.count)
    status = "pass";
  return copy_literal(engine, &result->status, status);
}

static ArchbirdStatus
finding_add_fact_witnesses(ArchbirdEngine *engine, AbVerifyFinding *finding,
                           const AbVerifyFactSet *first,
                           const AbVerifyFactSet *second) {
  ArchbirdStatus status = finding_add_fact_summary(engine, finding, first);
  if (status == ARCHBIRD_OK && second)
    status = finding_add_fact_summary(engine, finding, second);
  return status;
}

static ArchbirdStatus
shape_finding(ArchbirdEngine *engine, const AbValue *check,
              const char *message_prefix, const AbVerifyFactSet *first,
              const AbVerifyFactSet *second, AbVerifyFinding *out) {
  AbBuffer message;
  AbString key = {(char *)"fact-shape", 10};
  ArchbirdStatus status;
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&message, message_prefix);
  if (status == ARCHBIRD_OK && second) {
    status = ab_buffer_literal(&message, "expected=");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&message, first->shape.data, first->shape.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, ", actual=");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&message, second->shape.data, second->shape.length);
  } else if (status == ARCHBIRD_OK) {
    status = ab_buffer_append(&message, first->shape.data, first->shape.length);
  }
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, "different", &key,
                       (const char *)message.data, message.length, "unknown");
  if (status == ARCHBIRD_OK)
    status = finding_add_fact_witnesses(engine, out, first, second);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus
message_finding(ArchbirdEngine *engine, const AbValue *check,
                const char *comparison, const AbString *key, const char *prefix,
                const AbString *label, const AbVerifyFactItem *item,
                AbVerifyFinding *out) {
  AbBuffer message;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&message, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, label->data, label->length);
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, comparison, key,
                       (const char *)message.data, message.length, "current");
  for (index = 0; status == ARCHBIRD_OK && index < item->evidence_count;
       index++)
    status = finding_add_evidence(engine, out, &item->evidence[index]);
  ab_buffer_free(&message);
  return status;
}

static const AbVerifyFactItem *fact_item_find(const AbVerifyFactSet *fact,
                                              const AbString *key) {
  size_t low = 0;
  size_t high = fact->item_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&fact->items[middle].key, key);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &fact->items[middle];
  }
  return NULL;
}

static const MappedItem *mapped_item_find(const MappedItem *items, size_t count,
                                          const AbString *key) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(items[middle].key, key);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else {
      while (middle &&
             ab_string_equal(items[middle - 1].key, items[middle].key))
        middle--;
      return &items[middle];
    }
  }
  return NULL;
}

static ArchbirdStatus mapping_collision_finding(ArchbirdEngine *engine,
                                                const AbValue *check,
                                                const MappedItem *rows,
                                                size_t count,
                                                AbVerifyFinding *out) {
  size_t index;
  ArchbirdStatus status = finding_init_literal(
      engine, out, check, "different", rows[0].key,
      "mapping collision: multiple actual facts map to one expected fact",
      "unknown");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    size_t evidence;
    for (evidence = 0;
         status == ARCHBIRD_OK && evidence < rows[index].item->evidence_count;
         evidence++)
      status = finding_add_evidence(engine, out,
                                    &rows[index].item->evidence[evidence]);
  }
  return status;
}

static ArchbirdStatus
value_difference_finding(ArchbirdEngine *engine, const AbValue *check,
                         const AbString *key, const AbVerifyFactItem *expected,
                         const AbVerifyFactItem *actual, AbVerifyFinding *out) {
  AbBuffer message;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&message, "value differs: expected ");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&message, &expected->value);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, ", actual ");
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&message, &actual->value);
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, "different", key,
                       (const char *)message.data, message.length, "current");
  for (index = 0; status == ARCHBIRD_OK && index < expected->evidence_count;
       index++)
    status = finding_add_evidence(engine, out, &expected->evidence[index]);
  for (index = 0; status == ARCHBIRD_OK && index < actual->evidence_count;
       index++)
    status = finding_add_evidence(engine, out, &actual->evidence[index]);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus
compare_facts(ArchbirdEngine *engine, const AbValue *check,
              const AbVerifyFactSet *expected, const AbVerifyFactSet *actual,
              const AbValue *mapping, int values, int allow_missing_expected,
              int allow_extra_actual, AbVerifyCheckResult *result) {
  MappedItem *mapped = NULL;
  size_t mapped_count = actual->item_count;
  size_t index;
  ArchbirdStatus status = result_init(engine, result, check, expected, actual);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_literal(&expected->state, "current") &&
      string_literal(&actual->state, "current") &&
      ((values && (!string_literal(&expected->shape, "values") ||
                   !string_literal(&actual->shape, "values"))) ||
       (!values && (!(string_literal(&expected->shape, "set") ||
                      string_literal(&expected->shape, "values")) ||
                    !(string_literal(&actual->shape, "set") ||
                      string_literal(&actual->shape, "values")))))) {
    AbVerifyFinding finding = {0};
    status =
        shape_finding(engine, check, "incompatible fact shapes: ", expected,
                      actual, &finding);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status == ARCHBIRD_OK)
      status = result_set_status(engine, result);
    return status;
  }
  {
    const AbVerifyFactSet *facts[2] = {expected, actual};
    for (index = 0; status == ARCHBIRD_OK && index < 2; index++) {
      AbVerifyFinding finding = {0};
      int unavailable = 0;
      status = unavailable_finding(engine, check, facts[index], &finding,
                                   &unavailable);
      if (status == ARCHBIRD_OK && unavailable)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
    }
    if (status != ARCHBIRD_OK || result->finding_count) {
      if (status == ARCHBIRD_OK)
        status = result_set_status(engine, result);
      return status;
    }
  }
  if (mapped_count) {
    mapped = (MappedItem *)ab_calloc(engine, mapped_count, sizeof(*mapped));
    if (!mapped)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory mapping verification facts");
  }
  for (index = 0; index < mapped_count; index++) {
    mapped[index].key = mapping_target(mapping, &actual->items[index].key);
    mapped[index].item = &actual->items[index];
  }
  if (mapped_count > 1)
    qsort(mapped, mapped_count, sizeof(*mapped), mapped_compare);
  for (index = 0; status == ARCHBIRD_OK && index < mapped_count;) {
    size_t end = index + 1;
    while (end < mapped_count &&
           ab_string_equal(mapped[index].key, mapped[end].key))
      end++;
    if (end - index > 1) {
      AbVerifyFinding finding = {0};
      status = mapping_collision_finding(engine, check, mapped + index,
                                         end - index, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
    }
    index = end;
  }
  if (!allow_missing_expected) {
    for (index = 0; status == ARCHBIRD_OK && index < expected->item_count;
         index++) {
      const AbVerifyFactItem *item = &expected->items[index];
      if (!mapped_item_find(mapped, mapped_count, &item->key)) {
        AbVerifyFinding finding = {0};
        status = message_finding(engine, check, "missing", &item->key,
                                 "missing actual fact ", &item->label, item,
                                 &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(engine, result, &finding);
        finding_free(engine, &finding);
      }
    }
  }
  for (index = 0;
       !allow_extra_actual && status == ARCHBIRD_OK && index < mapped_count;) {
    const MappedItem *row = &mapped[index];
    size_t end = index + 1;
    while (end < mapped_count &&
           ab_string_equal(mapped[index].key, mapped[end].key))
      end++;
    if (!fact_item_find(expected, row->key)) {
      AbVerifyFinding finding = {0};
      status = message_finding(engine, check, "extra", row->key,
                               "unexpected actual fact ", row->key, row->item,
                               &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
    }
    index = end;
  }
  for (index = 0; status == ARCHBIRD_OK && index < expected->item_count;
       index++) {
    const AbVerifyFactItem *expected_item = &expected->items[index];
    const MappedItem *mapped_item =
        mapped_item_find(mapped, mapped_count, &expected_item->key);
    const AbVerifyFactItem *actual_item;
    if (!mapped_item)
      continue;
    actual_item = mapped_item->item;
    if (values && !string_literal(&expected_item->state, "current")) {
      AbVerifyFinding finding = {0};
      status = item_unknown_finding(engine, check, expected_item, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
      continue;
    }
    if (values && !string_literal(&actual_item->state, "current")) {
      AbVerifyFinding finding = {0};
      status = item_unknown_finding(engine, check, actual_item, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
      continue;
    }
    if (values && !ab_value_equal(&expected_item->value, &actual_item->value)) {
      AbVerifyFinding finding = {0};
      status = value_difference_finding(engine, check, &expected_item->key,
                                        expected_item, actual_item, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
      continue;
    }
    status = coverage_add(engine, result, &expected_item->key);
  }
  ab_free(engine, mapped);
  if (status == ARCHBIRD_OK && result->finding_count > 1)
    qsort(result->findings, result->finding_count, sizeof(*result->findings),
          finding_compare_full);
  if (status == ARCHBIRD_OK)
    status = result_set_status(engine, result);
  return status;
}

static int relation_matches(const AbVerifyFactItem *pattern,
                            const AbVerifyFactItem *candidate) {
  static const char *const fields[] = {"source", "kind", "target"};
  size_t index;
  for (index = 0; index < sizeof(fields) / sizeof(fields[0]); index++) {
    static const AbString empty = {NULL, 0};
    const AbString *left = item_attribute(pattern, fields[index]);
    const AbString *right = item_attribute(candidate, fields[index]);
    if (!left)
      left = &empty;
    if (!right)
      right = &empty;
    if (!ab_map_glob_match(left, right))
      return 0;
  }
  return 1;
}

static ArchbirdStatus
relation_finding(ArchbirdEngine *engine, const AbValue *check,
                 const char *comparison, const AbVerifyFactItem *item,
                 const char *prefix, const AbVerifyFactSet *patterns,
                 int include_matching_patterns, AbVerifyFinding *out) {
  AbBuffer message;
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&message, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, item->label.data, item->label.length);
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, comparison, &item->label,
                       (const char *)message.data, message.length, "current");
  for (index = 0; status == ARCHBIRD_OK && index < item->evidence_count;
       index++)
    status = finding_add_evidence(engine, out, &item->evidence[index]);
  if (include_matching_patterns) {
    size_t pattern_index;
    for (pattern_index = 0;
         status == ARCHBIRD_OK && pattern_index < patterns->item_count;
         pattern_index++) {
      const AbVerifyFactItem *pattern = &patterns->items[pattern_index];
      size_t evidence_index;
      if (!relation_matches(pattern, item))
        continue;
      for (evidence_index = 0;
           status == ARCHBIRD_OK && evidence_index < pattern->evidence_count;
           evidence_index++)
        status = finding_add_evidence(engine, out,
                                      &pattern->evidence[evidence_index]);
    }
  }
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus relation_check(ArchbirdEngine *engine,
                                     const AbValue *check,
                                     const AbVerifyFactSet *expected,
                                     const AbVerifyFactSet *actual,
                                     AbVerifyCheckResult *result) {
  const AbValue *assertion = ab_value_member(check, "assert");
  size_t index;
  ArchbirdStatus status = result_init(engine, result, check, expected, actual);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_literal(&expected->state, "current") &&
      string_literal(&actual->state, "current") &&
      (!string_literal(&expected->shape, "relation") ||
       !string_literal(&actual->shape, "relation"))) {
    AbVerifyFinding finding = {0};
    status =
        shape_finding(engine, check, "relation check requires relation facts; ",
                      expected, actual, &finding);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status == ARCHBIRD_OK)
      status = result_set_status(engine, result);
    return status;
  }
  {
    const AbVerifyFactSet *facts[2] = {expected, actual};
    for (index = 0; status == ARCHBIRD_OK && index < 2; index++) {
      AbVerifyFinding finding = {0};
      int unavailable = 0;
      status = unavailable_finding(engine, check, facts[index], &finding,
                                   &unavailable);
      if (status == ARCHBIRD_OK && unavailable)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
    }
    if (status != ARCHBIRD_OK || result->finding_count) {
      if (status == ARCHBIRD_OK)
        status = result_set_status(engine, result);
      return status;
    }
  }
  if (ab_value_string_is(assertion, "required_edges")) {
    for (index = 0; status == ARCHBIRD_OK && index < expected->item_count;
         index++) {
      const AbVerifyFactItem *pattern = &expected->items[index];
      size_t actual_index;
      int matched = 0;
      for (actual_index = 0; actual_index < actual->item_count;
           actual_index++) {
        const AbVerifyFactItem *candidate = &actual->items[actual_index];
        if (!relation_matches(pattern, candidate))
          continue;
        matched = 1;
        status = coverage_add(engine, result, &candidate->key);
        if (status != ARCHBIRD_OK)
          break;
      }
      if (status == ARCHBIRD_OK && !matched) {
        AbVerifyFinding finding = {0};
        status = relation_finding(engine, check, "missing", pattern,
                                  "required relation is absent: ", expected, 0,
                                  &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(engine, result, &finding);
        finding_free(engine, &finding);
      }
    }
  } else if (ab_value_string_is(assertion, "forbidden_edges")) {
    for (index = 0; status == ARCHBIRD_OK && index < actual->item_count;
         index++) {
      const AbVerifyFactItem *candidate = &actual->items[index];
      size_t pattern_index;
      int forbidden = 0;
      for (pattern_index = 0; pattern_index < expected->item_count;
           pattern_index++)
        if (relation_matches(&expected->items[pattern_index], candidate)) {
          forbidden = 1;
          break;
        }
      if (forbidden) {
        AbVerifyFinding finding = {0};
        status = relation_finding(engine, check, "extra", candidate,
                                  "forbidden relation is present: ", expected,
                                  1, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(engine, result, &finding);
        finding_free(engine, &finding);
      } else {
        status = coverage_add(engine, result, &candidate->key);
      }
    }
  } else {
    for (index = 0; status == ARCHBIRD_OK && index < actual->item_count;
         index++) {
      const AbVerifyFactItem *candidate = &actual->items[index];
      size_t pattern_index;
      int allowed = 0;
      for (pattern_index = 0; pattern_index < expected->item_count;
           pattern_index++)
        if (relation_matches(&expected->items[pattern_index], candidate)) {
          allowed = 1;
          break;
        }
      if (allowed) {
        status = coverage_add(engine, result, &candidate->key);
      } else {
        AbVerifyFinding finding = {0};
        status = relation_finding(
            engine, check, "extra", candidate,
            "relation is outside the allowed matrix: ", expected, 0, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(engine, result, &finding);
        finding_free(engine, &finding);
      }
    }
  }
  if (status == ARCHBIRD_OK && result->finding_count > 1)
    qsort(result->findings, result->finding_count, sizeof(*result->findings),
          finding_compare_key);
  if (status == ARCHBIRD_OK)
    status = result_set_status(engine, result);
  return status;
}

typedef struct VerifyGraphEdge {
  const AbString *source;
  const AbString *target;
  size_t source_index;
  size_t target_index;
} VerifyGraphEdge;

typedef struct VerifyDfsFrame {
  size_t node;
  size_t next_edge;
} VerifyDfsFrame;

static int string_pointer_compare(const void *left_raw, const void *right_raw) {
  const AbString *const *left = (const AbString *const *)left_raw;
  const AbString *const *right = (const AbString *const *)right_raw;
  return ab_string_compare(*left, *right);
}

static int graph_edge_compare(const void *left_raw, const void *right_raw) {
  const VerifyGraphEdge *left = (const VerifyGraphEdge *)left_raw;
  const VerifyGraphEdge *right = (const VerifyGraphEdge *)right_raw;
  int compared = ab_string_compare(left->source, right->source);
  if (!compared)
    compared = ab_string_compare(left->target, right->target);
  return compared;
}

static size_t node_index(const AbString *const *nodes, size_t count,
                         const AbString *name) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(nodes[middle], name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return middle;
  }
  return SIZE_MAX;
}

static int rotation_less(const size_t *cycle, size_t length, size_t left,
                         size_t right, const AbString *const *nodes) {
  size_t index;
  for (index = 0; index < length; index++) {
    int compared = ab_string_compare(nodes[cycle[(left + index) % length]],
                                     nodes[cycle[(right + index) % length]]);
    if (compared)
      return compared < 0;
  }
  return 0;
}

static ArchbirdStatus
cycle_finding(ArchbirdEngine *engine, const AbValue *check,
              const AbVerifyFactSet *actual, const AbString *const *nodes,
              const size_t *cycle, size_t cycle_length, AbVerifyFinding *out) {
  size_t best = 0;
  size_t rotation;
  size_t edge;
  AbBuffer key_buffer;
  AbBuffer message;
  AbString key;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (rotation = 1; rotation < cycle_length; rotation++)
    if (rotation_less(cycle, cycle_length, rotation, best, nodes))
      best = rotation;
  ab_buffer_init(&key_buffer, engine);
  ab_buffer_init(&message, engine);
  for (edge = 0; status == ARCHBIRD_OK && edge <= cycle_length; edge++) {
    const AbString *node = nodes[cycle[(best + edge) % cycle_length]];
    if (edge)
      status = ab_buffer_literal(&key_buffer, " -> ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&key_buffer, node->data, node->length);
  }
  key.data = (char *)key_buffer.data;
  key.length = key_buffer.length;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, "dependency cycle: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, key.data, key.length);
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, "different", &key,
                       (const char *)message.data, message.length, "current");
  for (edge = 0; status == ARCHBIRD_OK && edge < cycle_length; edge++) {
    const AbString *source = nodes[cycle[(best + edge) % cycle_length]];
    const AbString *target = nodes[cycle[(best + edge + 1) % cycle_length]];
    size_t item_index;
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < actual->item_count;
         item_index++) {
      const AbVerifyFactItem *item = &actual->items[item_index];
      const AbString *item_source = item_attribute(item, "source");
      const AbString *item_target = item_attribute(item, "target");
      size_t evidence_index;
      if (!item_source || !item_target ||
          !ab_string_equal(source, item_source) ||
          !ab_string_equal(target, item_target))
        continue;
      for (evidence_index = 0;
           status == ARCHBIRD_OK && evidence_index < item->evidence_count;
           evidence_index++)
        status =
            finding_add_evidence(engine, out, &item->evidence[evidence_index]);
    }
  }
  ab_buffer_free(&key_buffer);
  ab_buffer_free(&message);
  return status;
}

static int result_has_finding_key(const AbVerifyCheckResult *result,
                                  const AbString *key) {
  size_t index;
  for (index = 0; index < result->finding_count; index++)
    if (ab_string_equal(&result->findings[index].key, key))
      return 1;
  return 0;
}

static ArchbirdStatus acyclic_check(ArchbirdEngine *engine,
                                    const AbValue *check,
                                    const AbVerifyFactSet *actual,
                                    AbVerifyCheckResult *result) {
  const AbString **nodes = NULL;
  size_t node_count = 0;
  VerifyGraphEdge *edges = NULL;
  size_t edge_count = 0;
  size_t *offsets = NULL;
  unsigned char *colors = NULL;
  VerifyDfsFrame *frames = NULL;
  size_t *stack = NULL;
  size_t index;
  ArchbirdStatus status = result_init(engine, result, check, actual, NULL);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_literal(&actual->state, "current") &&
      !string_literal(&actual->shape, "relation")) {
    AbVerifyFinding finding = {0};
    status =
        shape_finding(engine, check, "acyclic requires a relation fact, got ",
                      actual, NULL, &finding);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status == ARCHBIRD_OK)
      status = result_set_status(engine, result);
    return status;
  }
  {
    AbVerifyFinding finding = {0};
    int unavailable = 0;
    status = unavailable_finding(engine, check, actual, &finding, &unavailable);
    if (status == ARCHBIRD_OK && unavailable)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status != ARCHBIRD_OK || unavailable) {
      if (status == ARCHBIRD_OK)
        status = result_set_status(engine, result);
      return status;
    }
  }
  if (actual->item_count > SIZE_MAX / 2 / sizeof(*nodes))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "relation graph is too large");
  if (actual->item_count) {
    nodes = (const AbString **)ab_calloc(engine, actual->item_count * 2,
                                         sizeof(*nodes));
    edges = (VerifyGraphEdge *)ab_calloc(engine, actual->item_count,
                                         sizeof(*edges));
    if (!nodes || !edges) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory evaluating relation cycles");
      goto cleanup;
    }
  }
  for (index = 0; index < actual->item_count; index++) {
    const AbString *source = item_attribute(&actual->items[index], "source");
    const AbString *target = item_attribute(&actual->items[index], "target");
    static const AbString empty = {NULL, 0};
    if (!source)
      source = &empty;
    if (!target)
      target = &empty;
    nodes[node_count++] = source;
    nodes[node_count++] = target;
    edges[edge_count].source = source;
    edges[edge_count].target = target;
    edge_count++;
  }
  if (node_count > 1)
    qsort(nodes, node_count, sizeof(*nodes), string_pointer_compare);
  {
    size_t write = 0;
    for (index = 0; index < node_count; index++)
      if (!write || !ab_string_equal(nodes[write - 1], nodes[index]))
        nodes[write++] = nodes[index];
    node_count = write;
  }
  if (edge_count > 1)
    qsort(edges, edge_count, sizeof(*edges), graph_edge_compare);
  {
    size_t write = 0;
    for (index = 0; index < edge_count; index++) {
      if (write &&
          ab_string_equal(edges[write - 1].source, edges[index].source) &&
          ab_string_equal(edges[write - 1].target, edges[index].target))
        continue;
      edges[write++] = edges[index];
    }
    edge_count = write;
  }
  for (index = 0; index < edge_count; index++) {
    edges[index].source_index =
        node_index(nodes, node_count, edges[index].source);
    edges[index].target_index =
        node_index(nodes, node_count, edges[index].target);
  }
  offsets = (size_t *)ab_calloc(engine, node_count + 1, sizeof(*offsets));
  colors = (unsigned char *)ab_calloc(engine, node_count, sizeof(*colors));
  frames = (VerifyDfsFrame *)ab_calloc(engine, node_count, sizeof(*frames));
  stack = (size_t *)ab_calloc(engine, node_count, sizeof(*stack));
  if ((node_count && (!offsets || !colors || !frames || !stack))) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory traversing relation graph");
    goto cleanup;
  }
  for (index = 0; index < edge_count; index++)
    offsets[edges[index].source_index + 1]++;
  for (index = 1; index <= node_count; index++)
    offsets[index] += offsets[index - 1];
  for (index = 0; status == ARCHBIRD_OK && index < node_count; index++) {
    size_t depth = 0;
    if (colors[index])
      continue;
    colors[index] = 1;
    stack[0] = index;
    frames[0].node = index;
    frames[0].next_edge = offsets[index];
    depth = 1;
    while (status == ARCHBIRD_OK && depth) {
      VerifyDfsFrame *frame = &frames[depth - 1];
      size_t end = offsets[frame->node + 1];
      if (frame->next_edge >= end) {
        colors[frame->node] = 2;
        depth--;
        continue;
      }
      {
        size_t target = edges[frame->next_edge++].target_index;
        if (colors[target] == 0) {
          colors[target] = 1;
          stack[depth] = target;
          frames[depth].node = target;
          frames[depth].next_edge = offsets[target];
          depth++;
        } else if (colors[target] == 1) {
          size_t start = 0;
          AbVerifyFinding finding = {0};
          while (start < depth && stack[start] != target)
            start++;
          if (start == depth) {
            status = archbird_error_set(
                engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                "relation cycle traversal lost its active node");
            break;
          }
          status = cycle_finding(engine, check, actual, nodes, stack + start,
                                 depth - start, &finding);
          if (status == ARCHBIRD_OK &&
              !result_has_finding_key(result, &finding.key))
            status = result_add_finding(engine, result, &finding);
          finding_free(engine, &finding);
        }
      }
    }
  }
  if (status == ARCHBIRD_OK && !result->finding_count)
    for (index = 0; status == ARCHBIRD_OK && index < actual->item_count;
         index++)
      status = coverage_add(engine, result, &actual->items[index].key);
  if (status == ARCHBIRD_OK && result->finding_count > 1)
    qsort(result->findings, result->finding_count, sizeof(*result->findings),
          finding_compare_key);
  if (status == ARCHBIRD_OK)
    status = result_set_status(engine, result);

cleanup:
  ab_free(engine, nodes);
  ab_free(engine, edges);
  ab_free(engine, offsets);
  ab_free(engine, colors);
  ab_free(engine, frames);
  ab_free(engine, stack);
  return status;
}

static ArchbirdStatus cardinality_check(ArchbirdEngine *engine,
                                        const AbValue *check,
                                        const AbVerifyFactSet *actual,
                                        AbVerifyCheckResult *result) {
  const AbValue *exact = ab_value_member(check, "exact");
  const AbValue *minimum = ab_value_member(check, "min");
  const AbValue *maximum = ab_value_member(check, "max");
  uint64_t exact_value = 0;
  uint64_t minimum_value = 0;
  uint64_t maximum_value = 0;
  uint64_t count = (uint64_t)actual->item_count;
  int valid;
  size_t index;
  ArchbirdStatus status = result_init(engine, result, check, actual, NULL);
  if (status != ARCHBIRD_OK)
    return status;
  {
    AbVerifyFinding finding = {0};
    int unavailable = 0;
    status = unavailable_finding(engine, check, actual, &finding, &unavailable);
    if (status == ARCHBIRD_OK && unavailable)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status != ARCHBIRD_OK || unavailable) {
      if (status == ARCHBIRD_OK)
        status = result_set_status(engine, result);
      return status;
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < actual->item_count;
       index++) {
    if (!string_literal(&actual->items[index].state, "current")) {
      AbVerifyFinding finding = {0};
      status =
          item_unknown_finding(engine, check, &actual->items[index], &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(engine, result, &finding);
      finding_free(engine, &finding);
    }
  }
  if (status != ARCHBIRD_OK || result->finding_count) {
    if (status == ARCHBIRD_OK)
      status = result_set_status(engine, result);
    return status;
  }
  if (exact)
    (void)ab_value_u64(exact, &exact_value);
  if (minimum)
    (void)ab_value_u64(minimum, &minimum_value);
  if (maximum)
    (void)ab_value_u64(maximum, &maximum_value);
  valid = (!exact || count == exact_value) &&
          (!minimum || count >= minimum_value) &&
          (!maximum || count <= maximum_value);
  if (!valid) {
    AbBuffer message;
    AbString key = {(char *)"cardinality", 11};
    AbVerifyFinding finding = {0};
    ab_buffer_init(&message, engine);
    status = ab_buffer_literal(&message, "fact cardinality is ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&message, count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, "; expected ");
    if (status == ARCHBIRD_OK && exact) {
      status = ab_buffer_literal(&message, "exactly ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&message, exact_value);
    } else if (status == ARCHBIRD_OK) {
      status = ab_buffer_literal(&message, "between ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&message, minimum ? minimum_value : 0);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " and ");
      if (status == ARCHBIRD_OK) {
        if (maximum)
          status = ab_buffer_u64(&message, maximum_value);
        else
          status = ab_buffer_literal(&message, "∞");
      }
    }
    if (status == ARCHBIRD_OK)
      status =
          finding_init_n(engine, &finding, check, "different", &key,
                         (const char *)message.data, message.length, "current");
    if (status == ARCHBIRD_OK)
      status = finding_add_fact_summary(engine, &finding, actual);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    ab_buffer_free(&message);
  } else {
    for (index = 0; status == ARCHBIRD_OK && index < actual->item_count;
         index++)
      status = coverage_add(engine, result, &actual->items[index].key);
  }
  if (status == ARCHBIRD_OK)
    status = result_set_status(engine, result);
  return status;
}

static int string_array_view_contains(const AbString **rows, size_t count,
                                      const AbString *value) {
  size_t index;
  for (index = 0; index < count; index++)
    if (ab_string_equal(rows[index], value))
      return 1;
  return 0;
}

static ArchbirdStatus route_finding(ArchbirdEngine *engine,
                                    const AbValue *check,
                                    const AbVerifyFactSet *actual,
                                    const AbString *route,
                                    AbVerifyFinding *out) {
  AbBuffer key_buffer;
  AbBuffer message;
  AbString key;
  ArchbirdStatus status;
  ab_buffer_init(&key_buffer, engine);
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&key_buffer, "route:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key_buffer, route->data, route->length);
  key.data = (char *)key_buffer.data;
  key.length = key_buffer.length;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&message, "required test route is absent: ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&message, route->data, route->length);
  if (status == ARCHBIRD_OK)
    status =
        finding_init_n(engine, out, check, "missing", &key,
                       (const char *)message.data, message.length, "current");
  if (status == ARCHBIRD_OK)
    status = finding_add_fact_summary(engine, out, actual);
  ab_buffer_free(&key_buffer);
  ab_buffer_free(&message);
  return status;
}

static ArchbirdStatus min_test_routes_check(ArchbirdEngine *engine,
                                            const AbValue *check,
                                            const AbVerifyFactSet *actual,
                                            AbVerifyCheckResult *result) {
  const AbString **routes = NULL;
  size_t route_count = 0;
  const AbValue *required = ab_value_member(check, "required_routes");
  const AbValue *minimum = ab_value_member(check, "min");
  uint64_t minimum_value = 0;
  size_t index;
  ArchbirdStatus status = result_init(engine, result, check, actual, NULL);
  if (status != ARCHBIRD_OK)
    return status;
  if (string_literal(&actual->state, "current") &&
      !string_literal(&actual->shape, "relation")) {
    AbVerifyFinding finding = {0};
    status = shape_finding(engine, check,
                           "min_test_routes requires a relation fact, got ",
                           actual, NULL, &finding);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status == ARCHBIRD_OK)
      status = result_set_status(engine, result);
    return status;
  }
  {
    AbVerifyFinding finding = {0};
    int unavailable = 0;
    status = unavailable_finding(engine, check, actual, &finding, &unavailable);
    if (status == ARCHBIRD_OK && unavailable)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    if (status != ARCHBIRD_OK || unavailable) {
      if (status == ARCHBIRD_OK)
        status = result_set_status(engine, result);
      return status;
    }
  }
  if (actual->item_count) {
    routes = (const AbString **)ab_calloc(engine, actual->item_count,
                                          sizeof(*routes));
    if (!routes)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting test routes");
  }
  for (index = 0; index < actual->item_count; index++) {
    const AbString *target = item_attribute(&actual->items[index], "target");
    if (target && target->length &&
        !string_array_view_contains(routes, route_count, target))
      routes[route_count++] = target;
  }
  if (route_count > 1)
    qsort(routes, route_count, sizeof(*routes), string_pointer_compare);
  if (required)
    for (index = 0; status == ARCHBIRD_OK && index < required->as.array.count;
         index++) {
      const AbString *route = &required->as.array.items[index].as.text;
      if (!string_array_view_contains(routes, route_count, route)) {
        AbVerifyFinding finding = {0};
        status = route_finding(engine, check, actual, route, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(engine, result, &finding);
        finding_free(engine, &finding);
      }
    }
  if (minimum)
    (void)ab_value_u64(minimum, &minimum_value);
  if (status == ARCHBIRD_OK && minimum && route_count < minimum_value) {
    AbBuffer message;
    AbString key = {(char *)"route-cardinality", 17};
    AbVerifyFinding finding = {0};
    ab_buffer_init(&message, engine);
    status = ab_buffer_literal(&message, "test routes=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&message, route_count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, "; required minimum=");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&message, minimum_value);
    if (status == ARCHBIRD_OK)
      status =
          finding_init_n(engine, &finding, check, "missing", &key,
                         (const char *)message.data, message.length, "current");
    if (status == ARCHBIRD_OK)
      status = finding_add_fact_summary(engine, &finding, actual);
    if (status == ARCHBIRD_OK)
      status = result_add_finding(engine, result, &finding);
    finding_free(engine, &finding);
    ab_buffer_free(&message);
  }
  for (index = 0; status == ARCHBIRD_OK && index < route_count; index++) {
    AbBuffer key_buffer;
    AbString key;
    ab_buffer_init(&key_buffer, engine);
    status = ab_buffer_literal(&key_buffer, "route:");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&key_buffer, routes[index]->data,
                                routes[index]->length);
    key.data = (char *)key_buffer.data;
    key.length = key_buffer.length;
    if (status == ARCHBIRD_OK)
      status = coverage_add(engine, result, &key);
    ab_buffer_free(&key_buffer);
  }
  ab_free(engine, routes);
  /* Required routes are emitted in contract order, followed by the aggregate
   * cardinality finding.  This ordering is semantic and matches the reviewed
   * result contract; do not sort it by key. */
  if (status == ARCHBIRD_OK)
    status = result_set_status(engine, result);
  return status;
}

static const AbVerifyAttestationCaseView *
attestation_case_find(const AbVerifyAttestationDataView *attestation,
                      const AbString *id) {
  size_t low = 0;
  size_t high = attestation ? attestation->case_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(attestation->cases[middle].id, id);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &attestation->cases[middle];
  }
  return NULL;
}

static const AbVerifyObservationView *
attestation_observation_find(const AbVerifyAttestationCaseView *case_view,
                             const AbString *route) {
  size_t low = 0;
  size_t high = case_view ? case_view->observation_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(case_view->observations[middle].route, route);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &case_view->observations[middle];
  }
  return NULL;
}

static int value_string_array_contains(const AbValue *array,
                                       const AbString *value) {
  size_t index;
  if (!array)
    return 0;
  if (array->kind == AB_VALUE_STRING)
    return ab_string_equal(&array->as.text, value);
  if (array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++)
    if (ab_string_equal(&array->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

static size_t value_string_list_count(const AbValue *value) {
  if (!value)
    return 0;
  return value->kind == AB_VALUE_STRING ? 1 : value->as.array.count;
}

static const AbString *value_string_list_item(const AbValue *value,
                                              size_t index) {
  return value->kind == AB_VALUE_STRING ? &value->as.text
                                        : &value->as.array.items[index].as.text;
}

static int value_string_array_set_equal(const AbValue *left,
                                        const AbValue *right) {
  size_t index;
  if (!left || !right || left->kind != AB_VALUE_ARRAY ||
      right->kind != AB_VALUE_ARRAY ||
      left->as.array.count != right->as.array.count)
    return 0;
  for (index = 0; index < left->as.array.count; index++)
    if (!value_string_array_contains(right,
                                     &left->as.array.items[index].as.text))
      return 0;
  return 1;
}

static int equality_policy_equal(const AbVerifyEqualityPolicy *left,
                                 const AbVerifyEqualityPolicy *right) {
  const AbValue *left_max =
      left->source ? ab_value_member(left->source, "max_ulp") : NULL;
  const AbValue *right_max =
      right->source ? ab_value_member(right->source, "max_ulp") : NULL;
  return ab_string_equal(left->kind, right->kind) &&
         left->atol == right->atol && left->rtol == right->rtol &&
         left->has_max_ulp == right->has_max_ulp &&
         (!left->has_max_ulp ||
          (left_max && right_max && ab_value_equal(left_max, right_max))) &&
         left->nan_equal == right->nan_equal &&
         left->signed_zero_equal == right->signed_zero_equal;
}

static ArchbirdStatus finding_add_attestation_witnesses(
    ArchbirdEngine *engine, AbVerifyFinding *finding,
    const AbVerifyEvidence *witnesses, size_t witness_count) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < witness_count; index++)
    status = finding_add_evidence(engine, finding, &witnesses[index]);
  return status;
}

static ArchbirdStatus
attestation_finding_n(ArchbirdEngine *engine, const AbValue *check,
                      const char *comparison, const AbString *key,
                      const char *message, size_t message_length,
                      const char *evidence_state, const char *applicability,
                      const AbVerifyCheckResult *result, AbVerifyFinding *out) {
  ArchbirdStatus status =
      finding_init_n(engine, out, check, comparison, key, message,
                     message_length, evidence_state);
  if (status == ARCHBIRD_OK && strcmp(applicability, "applicable")) {
    ab_string_free(engine, &out->applicability);
    status = copy_literal(engine, &out->applicability, applicability);
  }
  if (status == ARCHBIRD_OK)
    status = finding_add_attestation_witnesses(engine, out, result->witnesses,
                                               result->witness_count);
  return status;
}

static ArchbirdStatus
attestation_finding(ArchbirdEngine *engine, const AbValue *check,
                    const char *comparison, const AbString *key,
                    const char *message, const char *evidence_state,
                    const char *applicability,
                    const AbVerifyCheckResult *result, AbVerifyFinding *out) {
  return attestation_finding_n(engine, check, comparison, key, message,
                               strlen(message), evidence_state, applicability,
                               result, out);
}

static ArchbirdStatus
attestation_unavailable_finding(ArchbirdEngine *engine, const AbValue *check,
                                const AbVerifyAttestationState *state,
                                const AbVerifyCheckResult *result,
                                AbVerifyFinding *out) {
  AbBuffer key;
  AbBuffer message;
  AbString key_view;
  ArchbirdStatus status;
  ab_buffer_init(&key, engine);
  ab_buffer_init(&message, engine);
  status = ab_buffer_literal(&key, "attestation:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, state->name.data, state->name.length);
  key_view.data = (char *)key.data;
  key_view.length = key.length;
  if (status == ARCHBIRD_OK && state->message.length)
    status =
        ab_buffer_append(&message, state->message.data, state->message.length);
  else if (status == ARCHBIRD_OK) {
    status = ab_buffer_literal(&message, "attestation ");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&message, state->name.data, state->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&message, " is ");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(&message, state->state.data, state->state.length);
  }
  (void)result;
  if (status == ARCHBIRD_OK)
    status = finding_init_n(engine, out, check, "different", &key_view,
                            (const char *)message.data, message.length,
                            state->state.data);
  if (status == ARCHBIRD_OK)
    status = finding_add_attestation_witnesses(engine, out, state->witnesses,
                                               state->witness_count);
  ab_buffer_free(&key);
  ab_buffer_free(&message);
  return status;
}

static const AbVerifyObservationView *
reference_observation(const AbValue *check,
                      const AbVerifyAttestationCaseView *case_view,
                      const AbString **route) {
  const AbValue *configured = ab_value_member(check, "reference_route");
  static AbString reference = {(char *)"reference", 9};
  const AbVerifyObservationView *result;
  if (configured) {
    *route = &configured->as.text;
    return attestation_observation_find(case_view, *route);
  }
  result = attestation_observation_find(case_view, &reference);
  if (result) {
    *route = &reference;
    return result;
  }
  if (case_view->observation_count == 1) {
    *route = case_view->observations[0].route;
    return &case_view->observations[0];
  }
  *route = NULL;
  return NULL;
}

static ArchbirdStatus append_sorted_names(ArchbirdEngine *engine,
                                          AbBuffer *message,
                                          const AbValue *source,
                                          const AbValue *other,
                                          int absent_from_other) {
  const AbString **rows = NULL;
  size_t count = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (source->as.array.count) {
    rows = (const AbString **)ab_calloc(engine, source->as.array.count,
                                        sizeof(*rows));
    if (!rows)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory sorting requirement links");
  }
  for (index = 0; index < source->as.array.count; index++) {
    const AbString *candidate = &source->as.array.items[index].as.text;
    int contained = value_string_array_contains(other, candidate);
    if ((absent_from_other && !contained) || (!absent_from_other && contained))
      rows[count++] = candidate;
  }
  if (count > 1)
    qsort(rows, count, sizeof(*rows), string_pointer_compare);
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(message, ", ");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_append(message, rows[index]->data, rows[index]->length);
  }
  ab_free(engine, rows);
  return status;
}

static ArchbirdStatus
requirement_links_finding(ArchbirdEngine *engine, const AbValue *check,
                          const AbVerifyAttestationCaseView *reference_case,
                          const AbVerifyAttestationCaseView *actual_case,
                          int missing, const AbVerifyCheckResult *result,
                          AbVerifyFinding *out) {
  AbBuffer key;
  AbBuffer message;
  AbString key_view;
  ArchbirdStatus status;
  ab_buffer_init(&key, engine);
  ab_buffer_init(&message, engine);
  status = ab_buffer_append(&key, reference_case->id->data,
                            reference_case->id->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, "@requirements");
  key_view.data = (char *)key.data;
  key_view.length = key.length;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &message, missing
                      ? "subject case is missing requirement links: "
                      : "subject case adds requirement links absent from the "
                        "reference: ");
  if (status == ARCHBIRD_OK)
    status = append_sorted_names(
        engine, &message,
        missing ? reference_case->requirements : actual_case->requirements,
        missing ? actual_case->requirements : reference_case->requirements, 1);
  if (status == ARCHBIRD_OK)
    status = attestation_finding_n(engine, check,
                                   missing ? "missing" : "different", &key_view,
                                   (const char *)message.data, message.length,
                                   "current", "applicable", result, out);
  ab_buffer_free(&key);
  ab_buffer_free(&message);
  return status;
}

static int attestation_finding_compare(const void *left_raw,
                                       const void *right_raw) {
  const AbVerifyFinding *left = (const AbVerifyFinding *)left_raw;
  const AbVerifyFinding *right = (const AbVerifyFinding *)right_raw;
  int compared = ab_string_compare(&left->applicability, &right->applicability);
  if (!compared)
    compared = ab_string_compare(&left->comparison, &right->comparison);
  if (!compared)
    compared = ab_string_compare(&left->key, &right->key);
  return compared;
}

static ArchbirdStatus attestation_coverage_add(ArchbirdEngine *engine,
                                               AbVerifyCheckResult *result,
                                               const AbString *requirement,
                                               const AbString *case_id,
                                               const AbString *route) {
  AbBuffer key;
  AbString key_view;
  ArchbirdStatus status;
  ab_buffer_init(&key, engine);
  if (requirement) {
    status = ab_buffer_append(&key, requirement->data, requirement->length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&key, ":");
  } else {
    status = ARCHBIRD_OK;
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, case_id->data, case_id->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, "@");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, route->data, route->length);
  key_view.data = (char *)key.data;
  key_view.length = key.length;
  if (status == ARCHBIRD_OK)
    status = coverage_add(engine, result, &key_view);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus
attestation_equal_check(AbVerificationContext *context, const AbValue *check,
                        const AbVerifyAttestationState *expected,
                        const AbVerifyAttestationState *actual,
                        AbVerifyCheckResult *result) {
  ArchbirdEngine *engine = context->engine;
  const AbValue *requested = ab_value_member(check, "requirement");
  const AbValue *required_routes = ab_value_member(check, "required_routes");
  unsigned char *declared = NULL;
  size_t case_index;
  size_t index;
  ArchbirdStatus status =
      result_init(context->engine, result, check, NULL, NULL);
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; status == ARCHBIRD_OK && index < expected->witness_count;
       index++)
    status = result_add_witness(context->engine, result,
                                &expected->witnesses[index]);
  for (index = 0; status == ARCHBIRD_OK && index < actual->witness_count;
       index++)
    status =
        result_add_witness(context->engine, result, &actual->witnesses[index]);
  if (status != ARCHBIRD_OK)
    return status;
  evidence_sort_unique(engine, result->witnesses, &result->witness_count);
  for (index = 0; index < 2; index++) {
    const AbVerifyAttestationState *state = index ? actual : expected;
    if (!string_literal(&state->state, "current") || !state->has_data) {
      AbVerifyFinding finding = {0};
      status = attestation_unavailable_finding(context->engine, check, state,
                                               result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
    }
  }
  if (status != ARCHBIRD_OK || result->finding_count) {
    if (status == ARCHBIRD_OK)
      status = result_set_status(context->engine, result);
    return status;
  }
  if (value_string_list_count(requested)) {
    declared = (unsigned char *)ab_calloc(
        engine, value_string_list_count(requested), 1);
    if (!declared)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory tracking attestation "
                                "requirements");
  }
  for (case_index = 0;
       status == ARCHBIRD_OK && case_index < expected->data.case_count;
       case_index++) {
    const AbVerifyAttestationCaseView *reference_case =
        &expected->data.cases[case_index];
    const AbVerifyAttestationCaseView *actual_case;
    const AbVerifyObservationView *reference;
    const AbString *reference_route;
    const AbString **relevant = NULL;
    size_t relevant_count = 0;
    size_t route_index;
    int traceable = 1;
    if (reference_case->requirements->as.array.count) {
      relevant = (const AbString **)ab_calloc(
          engine, reference_case->requirements->as.array.count,
          sizeof(*relevant));
      if (!relevant) {
        status = archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory selecting attestation "
                                    "requirements");
        break;
      }
    }
    for (index = 0; index < reference_case->requirements->as.array.count;
         index++) {
      const AbString *requirement =
          &reference_case->requirements->as.array.items[index].as.text;
      if (!value_string_list_count(requested) ||
          value_string_array_contains(requested, requirement)) {
        relevant[relevant_count++] = requirement;
        if (requested)
          for (route_index = 0;
               route_index < value_string_list_count(requested); route_index++)
            if (ab_string_equal(requirement,
                                value_string_list_item(requested, route_index)))
              declared[route_index] = 1;
      }
    }
    if (value_string_list_count(requested) && !relevant_count) {
      ab_free(engine, relevant);
      continue;
    }
    if (relevant_count > 1)
      qsort(relevant, relevant_count, sizeof(*relevant),
            string_pointer_compare);
    if (!ab_verify_attestation_case_applicable(reference_case,
                                               &expected->data)) {
      AbVerifyFinding finding = {0};
      status = attestation_finding(
          context->engine, check, "equal", reference_case->id,
          "reference case is not applicable to its profile", "current",
          "not_applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_free(engine, relevant);
      continue;
    }
    if (!ab_verify_attestation_case_applicable(reference_case, &actual->data)) {
      AbVerifyFinding finding = {0};
      status = attestation_finding(
          context->engine, check, "equal", reference_case->id,
          "reference case is not applicable to the subject profile", "current",
          "not_applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_free(engine, relevant);
      continue;
    }
    actual_case = attestation_case_find(&actual->data, reference_case->id);
    if (!actual_case) {
      AbBuffer message;
      AbVerifyFinding finding = {0};
      ab_buffer_init(&message, context->engine);
      status =
          ab_buffer_literal(&message, "subject attestation is missing case ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, reference_case->id->data,
                                  reference_case->id->length);
      if (status == ARCHBIRD_OK)
        status = attestation_finding_n(
            context->engine, check, "missing", reference_case->id,
            (const char *)message.data, message.length, "current", "applicable",
            result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_buffer_free(&message);
      ab_free(engine, relevant);
      continue;
    }
    for (index = 0; index < reference_case->requirements->as.array.count;
         index++)
      if (!value_string_array_contains(
              actual_case->requirements,
              &reference_case->requirements->as.array.items[index].as.text)) {
        AbVerifyFinding finding = {0};
        status =
            requirement_links_finding(context->engine, check, reference_case,
                                      actual_case, 1, result, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(context->engine, result, &finding);
        finding_free(engine, &finding);
        traceable = 0;
        break;
      }
    for (index = 0; status == ARCHBIRD_OK &&
                    index < actual_case->requirements->as.array.count;
         index++)
      if (!value_string_array_contains(
              reference_case->requirements,
              &actual_case->requirements->as.array.items[index].as.text)) {
        AbVerifyFinding finding = {0};
        status =
            requirement_links_finding(context->engine, check, reference_case,
                                      actual_case, 0, result, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(context->engine, result, &finding);
        finding_free(engine, &finding);
        traceable = 0;
        break;
      }
    if (status == ARCHBIRD_OK &&
        (!value_string_array_set_equal(reference_case->requires,
                                       actual_case->requires) ||
         !ab_value_equal(reference_case->requires_parameters,
                         actual_case->requires_parameters))) {
      AbBuffer key;
      AbString key_view;
      AbVerifyFinding finding = {0};
      ab_buffer_init(&key, context->engine);
      status = ab_buffer_append(&key, reference_case->id->data,
                                reference_case->id->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&key, "@applicability");
      key_view.data = (char *)key.data;
      key_view.length = key.length;
      if (status == ARCHBIRD_OK)
        status = attestation_finding(
            context->engine, check, "different", &key_view,
            "subject case applicability contract differs from the reference",
            "current", "applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_buffer_free(&key);
      traceable = 0;
    }
    if (status == ARCHBIRD_OK &&
        !equality_policy_equal(&reference_case->comparison,
                               &actual_case->comparison)) {
      AbBuffer key;
      AbString key_view;
      AbVerifyFinding finding = {0};
      ab_buffer_init(&key, context->engine);
      status = ab_buffer_append(&key, reference_case->id->data,
                                reference_case->id->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&key, "@comparison");
      key_view.data = (char *)key.data;
      key_view.length = key.length;
      if (status == ARCHBIRD_OK)
        status = attestation_finding(
            context->engine, check, "different", &key_view,
            "subject case equality policy differs from the reference",
            "current", "applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_buffer_free(&key);
      traceable = 0;
    }
    if (status == ARCHBIRD_OK &&
        !ab_value_equal(reference_case->input, actual_case->input)) {
      AbVerifyFinding finding = {0};
      status = attestation_finding(context->engine, check, "different",
                                   reference_case->id,
                                   "reference and subject case inputs differ",
                                   "current", "applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_free(engine, relevant);
      continue;
    }
    reference = reference_observation(check, reference_case, &reference_route);
    if (!reference) {
      AbVerifyFinding finding = {0};
      status = attestation_finding(context->engine, check, "different",
                                   reference_case->id,
                                   "reference route is ambiguous or absent",
                                   "unknown", "applicable", result, &finding);
      if (status == ARCHBIRD_OK)
        status = result_add_finding(context->engine, result, &finding);
      finding_free(engine, &finding);
      ab_free(engine, relevant);
      continue;
    }
    for (route_index = 0;
         status == ARCHBIRD_OK &&
         route_index < (required_routes && required_routes->as.array.count
                            ? required_routes->as.array.count
                            : actual_case->observation_count);
         route_index++) {
      const AbString *route =
          required_routes && required_routes->as.array.count
              ? &required_routes->as.array.items[route_index].as.text
              : actual_case->observations[route_index].route;
      const AbVerifyObservationView *observation =
          attestation_observation_find(actual_case, route);
      AbBuffer key;
      AbString key_view;
      ab_buffer_init(&key, context->engine);
      status = ab_buffer_append(&key, reference_case->id->data,
                                reference_case->id->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&key, "@");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&key, route->data, route->length);
      key_view.data = (char *)key.data;
      key_view.length = key.length;
      if (status == ARCHBIRD_OK && !observation) {
        AbBuffer message;
        AbVerifyFinding finding = {0};
        ab_buffer_init(&message, context->engine);
        status = ab_buffer_literal(&message, "subject observation route '");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, route->data, route->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, "' is absent");
        if (status == ARCHBIRD_OK)
          status = attestation_finding_n(context->engine, check, "missing",
                                         &key_view, (const char *)message.data,
                                         message.length, "current",
                                         "applicable", result, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(context->engine, result, &finding);
        finding_free(engine, &finding);
        ab_buffer_free(&message);
      } else if (status == ARCHBIRD_OK &&
                 !ab_verify_attestation_observations_equal(
                     reference, observation, &reference_case->comparison)) {
        AbBuffer message;
        AbVerifyFinding finding = {0};
        ab_buffer_init(&message, context->engine);
        status = ab_buffer_literal(&message,
                                   "outcome differs from reference route '");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, reference_route->data,
                                    reference_route->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, "'");
        if (status == ARCHBIRD_OK)
          status = attestation_finding_n(context->engine, check, "different",
                                         &key_view, (const char *)message.data,
                                         message.length, "current",
                                         "applicable", result, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(context->engine, result, &finding);
        finding_free(engine, &finding);
        ab_buffer_free(&message);
      } else if (status == ARCHBIRD_OK && traceable) {
        if (relevant_count) {
          for (index = 0; status == ARCHBIRD_OK && index < relevant_count;
               index++)
            status = attestation_coverage_add(context->engine, result,
                                              relevant[index],
                                              reference_case->id, route);
        } else {
          status = attestation_coverage_add(context->engine, result, NULL,
                                            reference_case->id, route);
        }
      }
      ab_buffer_free(&key);
    }
    ab_free(engine, relevant);
  }
  if (requested)
    for (index = 0;
         status == ARCHBIRD_OK && index < value_string_list_count(requested);
         index++)
      if (!declared[index]) {
        const AbString *requirement = value_string_list_item(requested, index);
        AbBuffer key;
        AbBuffer message;
        AbString key_view;
        AbVerifyFinding finding = {0};
        ab_buffer_init(&key, context->engine);
        ab_buffer_init(&message, context->engine);
        status = ab_buffer_literal(&key, "requirement:");
        if (status == ARCHBIRD_OK)
          status =
              ab_buffer_append(&key, requirement->data, requirement->length);
        key_view.data = (char *)key.data;
        key_view.length = key.length;
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(
              &message,
              "reference attestation has no case linked to requirement ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, requirement->data,
                                    requirement->length);
        if (status == ARCHBIRD_OK)
          status = attestation_finding_n(context->engine, check, "missing",
                                         &key_view, (const char *)message.data,
                                         message.length, "current",
                                         "applicable", result, &finding);
        if (status == ARCHBIRD_OK)
          status = result_add_finding(context->engine, result, &finding);
        finding_free(engine, &finding);
        ab_buffer_free(&key);
        ab_buffer_free(&message);
      }
  ab_free(engine, declared);
  if (status == ARCHBIRD_OK && result->finding_count > 1)
    qsort(result->findings, result->finding_count, sizeof(*result->findings),
          attestation_finding_compare);
  if (status == ARCHBIRD_OK)
    status = result_set_status(context->engine, result);
  return status;
}

ArchbirdStatus ab_verify_evaluate_check(AbVerificationContext *context,
                                        const AbValue *check,
                                        AbVerifyCheckResult *result) {
  const AbValue *assertion = ab_value_member(check, "assert");
  const AbValue *expected_name = ab_value_member(check, "expected");
  const AbValue *actual_name = ab_value_member(check, "actual");
  if (ab_value_string_is(assertion, "attestation_equal")) {
    const AbVerifyAttestationState *expected_attestation =
        ab_verify_attestation_find(context, &expected_name->as.text);
    const AbVerifyAttestationState *actual_attestation =
        ab_verify_attestation_find(context, &actual_name->as.text);
    if (!expected_attestation || !actual_attestation)
      return archbird_error_set(context->engine, ARCHBIRD_CONFLICT,
                                ARCHBIRD_NO_OFFSET,
                                "validated attestation operand is unavailable");
    return attestation_equal_check(context, check, expected_attestation,
                                   actual_attestation, result);
  }
  const AbVerifyFactSet *expected =
      expected_name ? find_fact(context, &expected_name->as.text) : NULL;
  const AbVerifyFactSet *actual = find_fact(context, &actual_name->as.text);
  const AbValue *mapping = find_mapping(context, check);
  if (!actual || (expected_name && !expected))
    return archbird_error_set(context->engine, ARCHBIRD_CONFLICT,
                              ARCHBIRD_NO_OFFSET,
                              "validated check operand is unavailable");
  if (ab_value_string_is(assertion, "set_equal") ||
      ab_value_string_is(assertion, "mapped_set_equal"))
    return compare_facts(context->engine, check, expected, actual, mapping, 0,
                         0, 0, result);
  if (ab_value_string_is(assertion, "values_equal") ||
      ab_value_string_is(assertion, "mapped_values_equal"))
    return compare_facts(context->engine, check, expected, actual, mapping, 1,
                         0, 0, result);
  if (ab_value_string_is(assertion, "subset"))
    return compare_facts(context->engine, check, expected, actual, mapping, 0,
                         1, 0, result);
  if (ab_value_string_is(assertion, "required_subset"))
    return compare_facts(context->engine, check, expected, actual, mapping, 0,
                         0, 1, result);
  if (ab_value_string_is(assertion, "required_values"))
    return compare_facts(context->engine, check, expected, actual, mapping, 1,
                         0, 1, result);
  if (ab_value_string_is(assertion, "required_edges") ||
      ab_value_string_is(assertion, "forbidden_edges") ||
      ab_value_string_is(assertion, "allowed_edges"))
    return relation_check(context->engine, check, expected, actual, result);
  if (ab_value_string_is(assertion, "acyclic"))
    return acyclic_check(context->engine, check, actual, result);
  if (ab_value_string_is(assertion, "cardinality"))
    return cardinality_check(context->engine, check, actual, result);
  if (ab_value_string_is(assertion, "min_test_routes"))
    return min_test_routes_check(context->engine, check, actual, result);
  return archbird_error_set(context->engine, ARCHBIRD_CONFLICT,
                            ARCHBIRD_NO_OFFSET,
                            "validated verification assertion is unavailable");
}

ArchbirdStatus ab_verify_evaluate_checks(AbVerificationContext *context) {
  ArchbirdEngine *engine;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->engine || !context->suite.checks)
    return ARCHBIRD_INVALID_ARGUMENT;
  engine = context->engine;
  context->check_count = context->suite.checks->as.array.count;
  if (context->check_count > SIZE_MAX / sizeof(*context->checks))
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification checks");
  if (context->check_count) {
    context->checks = (AbVerifyCheckResult *)ab_calloc(
        engine, context->check_count, sizeof(*context->checks));
    if (!context->checks)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing verification checks");
  }
  for (index = 0; status == ARCHBIRD_OK && index < context->check_count;
       index++) {
    const AbValue *check = &context->suite.checks->as.array.items[index];
    status = ab_verify_evaluate_check(context, check, &context->checks[index]);
  }
  return status;
}

ArchbirdStatus ab_verify_apply_waivers(AbVerificationContext *context) {
  ArchbirdEngine *engine;
  const AbValue *waivers;
  unsigned char *used = NULL;
  unsigned char *invalid_reported = NULL;
  size_t check_index;
  size_t waiver_count;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  engine = context->engine;
  waivers = context->suite.waivers;
  waiver_count = waivers ? waivers->as.array.count : 0;
  if (waiver_count) {
    used = (unsigned char *)ab_calloc(engine, waiver_count, 1);
    invalid_reported = (unsigned char *)ab_calloc(engine, waiver_count, 1);
    if (!used || !invalid_reported) {
      ab_free(engine, used);
      ab_free(engine, invalid_reported);
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory applying verification waivers");
    }
  }
  for (check_index = 0;
       status == ARCHBIRD_OK && check_index < context->check_count;
       check_index++) {
    AbVerifyCheckResult *result = &context->checks[check_index];
    const AbValue *check_id = ab_value_member(result->spec, "id");
    size_t finding_index;
    for (finding_index = 0;
         status == ARCHBIRD_OK && finding_index < result->finding_count;
         finding_index++) {
      AbVerifyFinding *finding = &result->findings[finding_index];
      size_t waiver_index;
      size_t first_match = 0;
      size_t match_count = 0;
      for (waiver_index = 0; waiver_index < waiver_count; waiver_index++) {
        const AbValue *waiver = &waivers->as.array.items[waiver_index];
        const AbValue *fingerprint = ab_value_member(waiver, "fingerprint");
        int matches = 0;
        if (fingerprint)
          matches =
              ab_string_equal(&fingerprint->as.text, &finding->fingerprint);
        else {
          const AbValue *waiver_check = ab_value_member(waiver, "check");
          const AbValue *comparison = ab_value_member(waiver, "comparison");
          const AbValue *key = ab_value_member(waiver, "key");
          matches =
              waiver_check && check_id && comparison && key &&
              ab_string_equal(&waiver_check->as.text, &check_id->as.text) &&
              ab_string_equal(&comparison->as.text, &finding->comparison) &&
              ab_string_equal(&key->as.text, &finding->key);
        }
        if (matches) {
          if (!match_count)
            first_match = waiver_index;
          match_count++;
        }
      }
      if (match_count > 1) {
        AbBuffer message;
        size_t emitted = 0;
        ab_buffer_init(&message, context->engine);
        status = ab_buffer_literal(&message, "finding ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, finding->fingerprint.data,
                                    finding->fingerprint.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, " matches: ");
        for (waiver_index = 0;
             status == ARCHBIRD_OK && waiver_index < waiver_count;
             waiver_index++) {
          const AbValue *waiver = &waivers->as.array.items[waiver_index];
          const AbValue *fingerprint = ab_value_member(waiver, "fingerprint");
          const AbValue *waiver_check = ab_value_member(waiver, "check");
          const AbValue *comparison = ab_value_member(waiver, "comparison");
          const AbValue *key = ab_value_member(waiver, "key");
          int matches = fingerprint
                            ? ab_string_equal(&fingerprint->as.text,
                                              &finding->fingerprint)
                            : waiver_check && check_id && comparison && key &&
                                  ab_string_equal(&waiver_check->as.text,
                                                  &check_id->as.text) &&
                                  ab_string_equal(&comparison->as.text,
                                                  &finding->comparison) &&
                                  ab_string_equal(&key->as.text, &finding->key);
          if (matches) {
            const AbValue *id = ab_value_member(waiver, "id");
            if (emitted++)
              status = ab_buffer_literal(&message, ", ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&message, id->as.text.data,
                                        id->as.text.length);
          }
        }
        if (status == ARCHBIRD_OK)
          status = ab_verify_add_diagnostic(
              context, "error", "ambiguous-waiver", (const char *)message.data,
              message.length, "", 0);
        ab_buffer_free(&message);
        continue;
      }
      if (match_count == 1) {
        const AbValue *waiver = &waivers->as.array.items[first_match];
        const AbValue *id = ab_value_member(waiver, "id");
        const AbValue *expires = ab_value_member(waiver, "expires_on");
        const AbValue *until_inputs = ab_value_member(waiver, "until_inputs");
        AbBuffer note;
        int active = 1;
        used[first_match] = 1;
        ab_buffer_init(&note, context->engine);
        if (expires && context->suite.policy_date &&
            memcmp(context->suite.policy_date->as.text.data,
                   expires->as.text.data, 10) > 0) {
          active = 0;
          status = ab_buffer_literal(&note, "expired on ");
          if (status == ARCHBIRD_OK)
            status = ab_buffer_append(&note, expires->as.text.data,
                                      expires->as.text.length);
          if (status == ARCHBIRD_OK)
            status = ab_buffer_literal(&note, " at policy date ");
          if (status == ARCHBIRD_OK)
            status = ab_buffer_append(
                &note, context->suite.policy_date->as.text.data,
                context->suite.policy_date->as.text.length);
        }
        if (status == ARCHBIRD_OK && active && until_inputs) {
          for (waiver_index = 0;
               active && waiver_index < until_inputs->as.object.count;
               waiver_index++) {
            const AbObjectField *boundary =
                &until_inputs->as.object.fields[waiver_index];
            const AbValue *project =
                ab_verify_input_project(&context->input, &boundary->name);
            const AbValue *map =
                project ? ab_value_member(project, "map") : NULL;
            const AbValue *evidence =
                map ? ab_value_member(map, "evidence") : NULL;
            const AbValue *digest =
                evidence ? ab_value_member(evidence, "input_sha256") : NULL;
            if (!digest ||
                !ab_string_equal(&digest->as.text, &boundary->value.as.text)) {
              active = 0;
              status = ab_buffer_literal(
                  &note, "review boundary changed for project ");
              if (status == ARCHBIRD_OK)
                status = ab_buffer_append(&note, boundary->name.data,
                                          boundary->name.length);
            }
          }
        }
        if (status == ARCHBIRD_OK) {
          ab_string_free(engine, &finding->waiver);
          status = copy_string(context->engine, &finding->waiver, &id->as.text);
        }
        if (status == ARCHBIRD_OK && active) {
          ab_string_free(engine, &finding->disposition);
          status =
              copy_literal(context->engine, &finding->disposition, "waived");
        } else if (status == ARCHBIRD_OK) {
          ab_string_free(engine, &finding->waiver_note);
          status = ab_string_copy(context->engine, &finding->waiver_note,
                                  (const char *)note.data, note.length);
          if (status == ARCHBIRD_OK && !invalid_reported[first_match]) {
            AbBuffer message;
            ab_buffer_init(&message, context->engine);
            status = ab_buffer_literal(&message, "waiver ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&message, id->as.text.data,
                                        id->as.text.length);
            if (status == ARCHBIRD_OK)
              status = ab_buffer_literal(&message, ": ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&message, note.data, note.length);
            if (status == ARCHBIRD_OK)
              status = ab_verify_add_diagnostic(
                  context, "warning", "waiver-expired",
                  (const char *)message.data, message.length, "", 0);
            ab_buffer_free(&message);
            if (status == ARCHBIRD_OK)
              invalid_reported[first_match] = 1;
          }
        }
        ab_buffer_free(&note);
      }
    }
    if (status == ARCHBIRD_OK)
      status = result_set_status(context->engine, result);
  }
  for (check_index = 0; status == ARCHBIRD_OK && check_index < waiver_count;
       check_index++) {
    if (!used[check_index]) {
      const AbValue *waiver = &waivers->as.array.items[check_index];
      const AbValue *id = ab_value_member(waiver, "id");
      AbBuffer message;
      ab_buffer_init(&message, context->engine);
      status = ab_buffer_literal(&message, "waiver ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&message, id->as.text.data, id->as.text.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, " matches no current finding");
      if (status == ARCHBIRD_OK)
        status = ab_verify_add_diagnostic(context, "warning", "unused-waiver",
                                          (const char *)message.data,
                                          message.length, "", 0);
      ab_buffer_free(&message);
    }
  }
  ab_free(engine, used);
  ab_free(engine, invalid_reported);
  return status;
}

#define CHECK_RENDER_TRY(expression)                                           \
  do {                                                                         \
    ArchbirdStatus render_status__ = (expression);                             \
    if (render_status__ != ARCHBIRD_OK)                                        \
      return render_status__;                                                  \
  } while (0)

static ArchbirdStatus render_check_string(AbBuffer *buffer,
                                          const AbString *value) {
  return ab_buffer_json_string(buffer, value->data, value->length);
}

static ArchbirdStatus render_check_evidence(AbBuffer *buffer,
                                            const AbVerifyEvidence *evidence) {
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "{\"detail\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &evidence->detail));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"line\":"));
  CHECK_RENDER_TRY(ab_buffer_u64(buffer, evidence->line));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &evidence->path));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &evidence->project));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"provenance\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &evidence->provenance));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &evidence->sha256));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_check_evidence_array(AbBuffer *buffer,
                                                  const AbVerifyEvidence *rows,
                                                  size_t count) {
  size_t index;
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < count; index++) {
    if (index)
      CHECK_RENDER_TRY(ab_buffer_literal(buffer, ","));
    CHECK_RENDER_TRY(render_check_evidence(buffer, &rows[index]));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_check_string_array(AbBuffer *buffer,
                                                const AbStringArray *rows) {
  size_t index;
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < rows->count; index++) {
    if (index)
      CHECK_RENDER_TRY(ab_buffer_literal(buffer, ","));
    CHECK_RENDER_TRY(render_check_string(buffer, &rows->items[index]));
  }
  return ab_buffer_literal(buffer, "]");
}

static ArchbirdStatus render_value_or(AbBuffer *buffer, const AbValue *object,
                                      const char *name, const char *fallback) {
  const AbValue *value = ab_value_member(object, name);
  return value ? ab_value_render(buffer, value)
               : ab_buffer_literal(buffer, fallback);
}

static ArchbirdStatus render_requirement_array(AbBuffer *buffer,
                                               const AbValue *check) {
  const AbValue *value = ab_value_member(check, "requirement");
  if (!value)
    return ab_buffer_literal(buffer, "[]");
  if (value->kind == AB_VALUE_STRING) {
    CHECK_RENDER_TRY(ab_buffer_literal(buffer, "["));
    CHECK_RENDER_TRY(ab_value_render(buffer, value));
    return ab_buffer_literal(buffer, "]");
  }
  return ab_value_render(buffer, value);
}

static ArchbirdStatus render_finding(AbBuffer *buffer,
                                     const AbVerifyFinding *finding) {
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "{\"applicability\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->applicability));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"baseline_state\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->baseline_state));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"comparison\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->comparison));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"disposition\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->disposition));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"evidence\":"));
  CHECK_RENDER_TRY(render_check_evidence_array(buffer, finding->evidence,
                                               finding->evidence_count));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"evidence_state\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->evidence_state));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"fingerprint\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->fingerprint));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"key\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->key));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"message\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->message));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"waiver\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->waiver));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"waiver_note\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &finding->waiver_note));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_verify_check_render(AbBuffer *buffer,
                                      const AbVerifyCheckResult *result) {
  const AbValue *check;
  const AbValue *value;
  size_t index;
  if (!buffer || !result || !result->spec)
    return ARCHBIRD_INVALID_ARGUMENT;
  check = result->spec;
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "{\"assert\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "assert", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"coverage\":"));
  CHECK_RENDER_TRY(render_check_string_array(buffer, &result->coverage));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"findings\":["));
  for (index = 0; index < result->finding_count; index++) {
    if (index)
      CHECK_RENDER_TRY(ab_buffer_literal(buffer, ","));
    CHECK_RENDER_TRY(render_finding(buffer, &result->findings[index]));
  }
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "],\"id\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "id", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"operands\":{\"actual\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "actual", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"exact\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "exact", "null"));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"expected\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "expected", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"mapping\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "mapping", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"max\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "max", "null"));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"min\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "min", "null"));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"reference_route\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "reference_route", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"required_routes\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "required_routes", "[]"));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, "},\"owner\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "owner", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"rationale\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "rationale", "\"\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"requirements\":"));
  CHECK_RENDER_TRY(render_requirement_array(buffer, check));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"severity\":"));
  CHECK_RENDER_TRY(render_value_or(buffer, check, "severity", "\"error\""));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"status\":"));
  CHECK_RENDER_TRY(render_check_string(buffer, &result->status));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"tags\":"));
  value = ab_value_member(check, "tags");
  CHECK_RENDER_TRY(value ? ab_value_render(buffer, value)
                         : ab_buffer_literal(buffer, "[]"));
  CHECK_RENDER_TRY(ab_buffer_literal(buffer, ",\"witnesses\":"));
  CHECK_RENDER_TRY(render_check_evidence_array(buffer, result->witnesses,
                                               result->witness_count));
  return ab_buffer_literal(buffer, "}");
}

#undef CHECK_RENDER_TRY
