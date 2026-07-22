#include "retrieval.h"

#include "archbird_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct RetrievalField {
  const char *name;
  const AbString *value;
  unsigned weight;
} RetrievalField;

typedef struct RetrievalCandidate {
  AbRetrievalHit hit;
  unsigned strengths[AB_RETRIEVAL_MAX_TERMS];
  unsigned weights[AB_RETRIEVAL_MAX_TERMS];
} RetrievalCandidate;

static int ascii_alnum(unsigned char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9');
}

static unsigned char ascii_fold(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                      : value;
}

static int slice_equal(const char *left, size_t left_length, const char *right,
                       size_t right_length) {
  size_t index;
  if (left_length != right_length)
    return 0;
  for (index = 0; index < left_length; index++)
    if (ascii_fold((unsigned char)left[index]) !=
        ascii_fold((unsigned char)right[index]))
      return 0;
  return 1;
}

static int slice_prefix(const char *text, size_t text_length,
                        const char *prefix, size_t prefix_length) {
  return text_length >= prefix_length &&
         slice_equal(text, prefix_length, prefix, prefix_length);
}

static int slice_contains(const char *text, size_t text_length,
                          const char *needle, size_t needle_length) {
  size_t index;
  if (!needle_length || needle_length > text_length)
    return 0;
  for (index = 0; index + needle_length <= text_length; index++)
    if (slice_equal(text + index, needle_length, needle, needle_length))
      return 1;
  return 0;
}

static size_t bounded_edit_distance(const char *left, size_t left_length,
                                    const char *right, size_t right_length,
                                    size_t bound) {
  size_t previous[65];
  size_t current[65];
  size_t row;
  size_t column;
  if (left_length > 64 || right_length > 64 ||
      left_length > right_length + bound || right_length > left_length + bound)
    return bound + 1;
  for (column = 0; column <= right_length; column++)
    previous[column] = column;
  for (row = 1; row <= left_length; row++) {
    size_t minimum = SIZE_MAX;
    current[0] = row;
    for (column = 1; column <= right_length; column++) {
      size_t deletion = previous[column] + 1;
      size_t insertion = current[column - 1] + 1;
      size_t substitution =
          previous[column - 1] +
          (ascii_fold((unsigned char)left[row - 1]) ==
                   ascii_fold((unsigned char)right[column - 1])
               ? 0
               : 1);
      size_t value = deletion < insertion ? deletion : insertion;
      if (substitution < value)
        value = substitution;
      current[column] = value;
      if (value < minimum)
        minimum = value;
    }
    if (minimum > bound)
      return bound + 1;
    memcpy(previous, current, (right_length + 1) * sizeof(*previous));
  }
  return previous[right_length];
}

static int term_exists(const AbRetrievalResult *result, const char *data,
                       size_t length) {
  size_t index;
  for (index = 0; index < result->term_count; index++)
    if (slice_equal(result->terms[index].data, result->terms[index].length,
                    data, length))
      return 1;
  return 0;
}

static int common_natural_language_term(const AbString *term) {
  static const char *const words[] = {"a",  "all",  "an",  "and", "are",  "as",
                                      "at", "be",   "by",  "for", "from", "i",
                                      "in", "into", "is",  "it",  "of",   "on",
                                      "or", "that", "the", "to",  "with"};
  size_t index;
  for (index = 0; index < sizeof(words) / sizeof(words[0]); index++)
    if (slice_equal(term->data, term->length, words[index],
                    strlen(words[index])))
      return 1;
  return 0;
}

static void compact_common_terms(ArchbirdEngine *engine,
                                 AbRetrievalResult *result) {
  size_t index;
  size_t retained = 0;
  size_t write = 0;
  if (result->term_count <= 1)
    return;
  for (index = 0; index < result->term_count; index++)
    retained += !common_natural_language_term(&result->terms[index]);
  if (!retained)
    return;
  for (index = 0; index < result->term_count; index++) {
    if (common_natural_language_term(&result->terms[index])) {
      ab_string_free(engine, &result->terms[index]);
      continue;
    }
    if (write != index) {
      result->terms[write] = result->terms[index];
      memset(&result->terms[index], 0, sizeof(result->terms[index]));
    }
    write++;
  }
  result->term_count = write;
}

static int camel_acronym_equal(const char *word, size_t word_length,
                               const char *abbreviation,
                               size_t abbreviation_length) {
  size_t word_index;
  size_t abbreviation_index = 0;
  if (abbreviation_length < 2 || word_length <= abbreviation_length)
    return 0;
  for (word_index = 0; word_index < word_length; word_index++) {
    int initial = word_index == 0;
    int camel = word_index > 0 && word[word_index] >= 'A' &&
                word[word_index] <= 'Z' && word[word_index - 1] >= 'a' &&
                word[word_index - 1] <= 'z';
    if (!initial && !camel)
      continue;
    if (abbreviation_index >= abbreviation_length ||
        ascii_fold((unsigned char)word[word_index]) !=
            ascii_fold((unsigned char)abbreviation[abbreviation_index]))
      return 0;
    abbreviation_index++;
  }
  return abbreviation_index == abbreviation_length;
}

static int ordered_abbreviation(const char *word, size_t word_length,
                                const char *abbreviation,
                                size_t abbreviation_length) {
  size_t word_index = 0;
  size_t abbreviation_index = 0;
  if (abbreviation_length < 4 || word_length <= abbreviation_length ||
      word_length > abbreviation_length * 3 ||
      ascii_fold((unsigned char)word[0]) !=
          ascii_fold((unsigned char)abbreviation[0]))
    return 0;
  while (word_index < word_length && abbreviation_index < abbreviation_length) {
    if (ascii_fold((unsigned char)word[word_index]) ==
        ascii_fold((unsigned char)abbreviation[abbreviation_index]))
      abbreviation_index++;
    word_index++;
  }
  return abbreviation_index == abbreviation_length;
}

static ArchbirdStatus add_terms(ArchbirdEngine *engine,
                                AbRetrievalResult *result,
                                const AbString *query) {
  size_t start = 0;
  size_t index;
  for (index = 0; index <= query->length; index++) {
    int boundary = index == query->length ||
                   !ascii_alnum((unsigned char)query->data[index]);
    if (!boundary)
      continue;
    if (index > start &&
        !term_exists(result, query->data + start, index - start)) {
      if (result->term_count == AB_RETRIEVAL_MAX_TERMS)
        return archbird_error_set(
            engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
            "retrieval query contains more than %d unique terms",
            AB_RETRIEVAL_MAX_TERMS);
      if (ab_string_copy(engine, &result->terms[result->term_count],
                         query->data + start, index - start) != ARCHBIRD_OK)
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory tokenizing retrieval query");
      result->term_count++;
    }
    start = index + 1;
  }
  return ARCHBIRD_OK;
}

static unsigned field_match(const AbString *field, const AbString *term,
                            const char **match) {
  size_t start = 0;
  size_t index;
  unsigned best = 0;
  const char *best_match = NULL;
  if (!field || !field->length)
    return 0;
  if (slice_equal(field->data, field->length, term->data, term->length)) {
    *match = "exact-field";
    return 120;
  }
  for (index = 0; index <= field->length; index++) {
    int camel = index > start && index < field->length &&
                field->data[index] >= 'A' && field->data[index] <= 'Z' &&
                field->data[index - 1] >= 'a' && field->data[index - 1] <= 'z';
    int boundary = index == field->length || camel ||
                   !ascii_alnum((unsigned char)field->data[index]);
    size_t length;
    size_t distance;
    if (!boundary)
      continue;
    length = index - start;
    if (length) {
      if (slice_equal(field->data + start, length, term->data, term->length)) {
        if (best < 100) {
          best = 100;
          best_match = "exact-token";
        }
      } else if (slice_prefix(field->data + start, length, term->data,
                              term->length)) {
        if (best < 72) {
          best = 72;
          best_match = "prefix";
        }
      } else if (term->length >= 3 &&
                 slice_contains(field->data + start, length, term->data,
                                term->length)) {
        if (best < 48) {
          best = 48;
          best_match = "substring";
        }
      } else if (term->length >= 4 && length >= 3) {
        size_t bound = term->length >= 8 ? 2 : 1;
        distance = bounded_edit_distance(field->data + start, length,
                                         term->data, term->length, bound);
        if (distance == 1 && best < 32) {
          best = 32;
          best_match = "edit-1";
        } else if (bound >= 2 && distance == 2 && best < 16) {
          best = 16;
          best_match = "edit-2";
        }
      }
      if (best < 40 && camel_acronym_equal(term->data, term->length,
                                           field->data + start, length)) {
        best = 40;
        best_match = "acronym";
      } else if (best < 28 && ordered_abbreviation(field->data + start, length,
                                                   term->data, term->length)) {
        best = 28;
        best_match = "abbreviation";
      }
    }
    start = camel ? index : index + 1;
  }
  if (best < 44 && term->length >= 3 &&
      slice_contains(field->data, field->length, term->data, term->length)) {
    best = 44;
    best_match = "substring";
  }
  *match = best_match;
  return best;
}

uint64_t ab_query_retrieval_text_score(const AbRetrievalResult *result,
                                       const AbString *primary,
                                       unsigned primary_weight,
                                       const AbString *secondary,
                                       unsigned secondary_weight) {
  uint64_t score = 0;
  size_t matched_terms = 0;
  size_t term_index;
  if (!result)
    return 0;
  for (term_index = 0; term_index < result->term_count; term_index++) {
    const char *match = NULL;
    unsigned strength =
        field_match(primary, &result->terms[term_index], &match);
    unsigned weight = primary_weight;
    unsigned secondary_strength =
        field_match(secondary, &result->terms[term_index], &match);
    uint64_t rarity;
    uint64_t contribution;
    if (secondary_strength * secondary_weight > strength * weight) {
      strength = secondary_strength;
      weight = secondary_weight;
    }
    if (!strength)
      continue;
    matched_terms++;
    rarity = 64 + (((uint64_t)result->candidate_count + 1) * 256) /
                      (result->document_frequency[term_index] + 1);
    if (rarity > 2048)
      rarity = 2048;
    contribution = (uint64_t)strength * weight * rarity;
    if (result->term_count > 1 && strength < 44)
      contribution /= 4;
    score += contribution;
  }
  return score * (uint64_t)matched_terms * matched_terms;
}

static ArchbirdStatus grow_candidates(ArchbirdEngine *engine,
                                      RetrievalCandidate **items, size_t *count,
                                      size_t *capacity) {
  RetrievalCandidate *resized;
  size_t next = *capacity ? *capacity * 2 : 128;
  if (next < *capacity || next > SIZE_MAX / sizeof(**items))
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "too many retrieval candidates");
  resized =
      (RetrievalCandidate *)ab_realloc(engine, *items, next * sizeof(**items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting retrieval candidates");
  *items = resized;
  *capacity = next;
  (void)count;
  return ARCHBIRD_OK;
}

static void consider_candidate_field(RetrievalCandidate *candidate,
                                     const AbRetrievalResult *result,
                                     const RetrievalField *field) {
  size_t term_index;
  for (term_index = 0; term_index < result->term_count; term_index++) {
    const char *match = NULL;
    unsigned strength =
        field_match(field->value, &result->terms[term_index], &match);
    unsigned weighted = strength * field->weight;
    unsigned previous =
        candidate->strengths[term_index] * candidate->weights[term_index];
    if (!weighted || weighted <= previous)
      continue;
    candidate->strengths[term_index] = strength;
    candidate->weights[term_index] = field->weight;
    {
      size_t reason_index = 0;
      while (reason_index < candidate->hit.reason_count &&
             candidate->hit.reasons[reason_index].term_index != term_index)
        reason_index++;
      if (reason_index == candidate->hit.reason_count)
        candidate->hit.reason_count++;
      candidate->hit.reasons[reason_index] =
          (AbRetrievalReason){field->name, field->value, match,
                              term_index,  strength,     field->weight};
    }
  }
}

static ArchbirdStatus
add_candidate(ArchbirdEngine *engine, const AbRetrievalResult *result,
              RetrievalCandidate **items, size_t *count, size_t *capacity,
              AbRetrievalKind kind, const AbValue *row, const AbString *path,
              const AbString *name, const AbString *symbol_kind, size_t line,
              size_t source_index, const RetrievalField *fields,
              size_t field_count) {
  RetrievalCandidate *candidate;
  size_t field_index;
  if (*count == *capacity) {
    ArchbirdStatus status = grow_candidates(engine, items, count, capacity);
    if (status != ARCHBIRD_OK)
      return status;
  }
  candidate = &(*items)[(*count)++];
  memset(candidate, 0, sizeof(*candidate));
  candidate->hit.kind = kind;
  candidate->hit.row = row;
  candidate->hit.path = path;
  candidate->hit.name = name;
  candidate->hit.symbol_kind = symbol_kind;
  candidate->hit.line = line;
  candidate->hit.source_index = source_index;
  for (field_index = 0; field_index < field_count; field_index++)
    consider_candidate_field(candidate, result, &fields[field_index]);
  return ARCHBIRD_OK;
}

static const AbString *text_member(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

static const AbValue *optional_array_member(const AbValue *object,
                                            const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  const AbValue *value = ab_value_member(object, name);
  return value ? value : &empty;
}

static const AbValue *projection_attribute(const AbProjectionItem *item,
                                           const char *name) {
  size_t index;
  size_t length = strlen(name);
  for (index = 0; index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        !memcmp(item->attributes[index].name.data, name, length))
      return &item->attributes[index].value;
  return NULL;
}

static ArchbirdStatus projection_index(ArchbirdEngine *engine,
                                       const AbProjectionItem *item,
                                       const char *name, size_t limit,
                                       size_t *out) {
  uint64_t value;
  if (!ab_value_u64(projection_attribute(item, name), &value) ||
      value >= limit || value > SIZE_MAX)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "search projection contains an invalid index");
  *out = (size_t)value;
  return ARCHBIRD_OK;
}

static ArchbirdStatus collect_candidates(ArchbirdEngine *engine,
                                         const AbValue *map,
                                         const AbProjectionData *domain,
                                         const AbRetrievalResult *result,
                                         RetrievalCandidate **out,
                                         size_t *out_count) {
  RetrievalCandidate *items = NULL;
  size_t count = 0;
  size_t capacity = 0;
  const AbValue *files = ab_value_member(map, "files");
  const AbValue *components = ab_value_member(map, "components");
  const AbValue *packages = ab_value_member(map, "packages");
  const AbValue *artifacts = ab_value_member(map, "artifacts");
  const AbValue *calls = ab_value_member(map, "call_resolutions");
  unsigned char *file_selected = NULL;
  unsigned char *symbol_selected = NULL;
  unsigned char *component_selected = NULL;
  unsigned char *package_selected = NULL;
  unsigned char *artifact_selected = NULL;
  size_t *symbol_offsets = NULL;
  size_t symbol_count = 0;
  size_t call_index = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out = NULL;
  *out_count = 0;
  if (!files || files->kind != AB_VALUE_ARRAY || !components ||
      components->kind != AB_VALUE_ARRAY || !packages ||
      packages->kind != AB_VALUE_ARRAY || !artifacts ||
      artifacts->kind != AB_VALUE_ARRAY || !calls ||
      calls->kind != AB_VALUE_ARRAY)
    return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                              ARCHBIRD_NO_OFFSET,
                              "retrieval input Map collections are invalid");
  for (index = 0; index < calls->as.array.count; index++) {
    const AbString *source =
        text_member(&calls->as.array.items[index], "source");
    const AbString *name = text_member(&calls->as.array.items[index], "name");
    const AbString *previous =
        index ? text_member(&calls->as.array.items[index - 1], "source") : NULL;
    if (!source || !name ||
        (previous && ab_string_compare(previous, source) > 0))
      return archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "retrieval input Map calls must be valid and source-sorted");
  }
  symbol_offsets = (size_t *)ab_calloc(engine, files->as.array.count + 1,
                                       sizeof(*symbol_offsets));
  file_selected = (unsigned char *)ab_calloc(
      engine, files->as.array.count ? files->as.array.count : 1, 1);
  component_selected = (unsigned char *)ab_calloc(
      engine, components->as.array.count ? components->as.array.count : 1, 1);
  package_selected = (unsigned char *)ab_calloc(
      engine, packages->as.array.count ? packages->as.array.count : 1, 1);
  artifact_selected = (unsigned char *)ab_calloc(
      engine, artifacts->as.array.count ? artifacts->as.array.count : 1, 1);
  if (!symbol_offsets || !file_selected || !component_selected ||
      !package_selected || !artifact_selected) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory indexing search projection");
    goto cleanup;
  }
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *symbols =
        ab_value_member(&files->as.array.items[index], "symbols");
    if (!symbols || symbols->kind != AB_VALUE_ARRAY ||
        symbol_count > SIZE_MAX - symbols->as.array.count) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "retrieval input Map symbols are invalid");
      goto cleanup;
    }
    symbol_offsets[index] = symbol_count;
    symbol_count += symbols->as.array.count;
  }
  symbol_offsets[files->as.array.count] = symbol_count;
  symbol_selected =
      (unsigned char *)ab_calloc(engine, symbol_count ? symbol_count : 1, 1);
  if (!symbol_selected) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory indexing searchable symbols");
    goto cleanup;
  }
  for (index = 0; index < domain->item_count; index++) {
    const AbProjectionItem *item = &domain->items[index];
    const AbValue *kind = projection_attribute(item, "entity_kind");
    size_t source_index = 0;
    if (!kind || kind->kind != AB_VALUE_STRING) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "search projection item has no entity_kind");
      goto cleanup;
    }
    if (ab_projection_value_is(kind, "file")) {
      status = projection_index(engine, item, "source_index",
                                files->as.array.count, &source_index);
      if (status == ARCHBIRD_OK)
        file_selected[source_index] = 1;
    } else if (ab_projection_value_is(kind, "symbol")) {
      size_t nested_index = 0;
      const AbValue *symbols;
      status = projection_index(engine, item, "source_index",
                                files->as.array.count, &source_index);
      symbols =
          status == ARCHBIRD_OK
              ? ab_value_member(&files->as.array.items[source_index], "symbols")
              : NULL;
      if (status == ARCHBIRD_OK &&
          (!symbols || symbols->kind != AB_VALUE_ARRAY))
        status = archbird_error_set(
            engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
            "search projection references an invalid symbol collection");
      if (status == ARCHBIRD_OK)
        status = projection_index(engine, item, "nested_index",
                                  symbols->as.array.count, &nested_index);
      if (status == ARCHBIRD_OK)
        symbol_selected[symbol_offsets[source_index] + nested_index] = 1;
    } else if (ab_projection_value_is(kind, "component")) {
      status = projection_index(engine, item, "source_index",
                                components->as.array.count, &source_index);
      if (status == ARCHBIRD_OK)
        component_selected[source_index] = 1;
    } else if (ab_projection_value_is(kind, "package")) {
      status = projection_index(engine, item, "source_index",
                                packages->as.array.count, &source_index);
      if (status == ARCHBIRD_OK)
        package_selected[source_index] = 1;
    } else if (ab_projection_value_is(kind, "artifact")) {
      status = projection_index(engine, item, "source_index",
                                artifacts->as.array.count, &source_index);
      if (status == ARCHBIRD_OK)
        artifact_selected[source_index] = 1;
    } else {
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "search projection contains an unsupported entity kind");
    }
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbValue *file = &files->as.array.items[index];
    const AbString *path = text_member(file, "path");
    const AbString *layer = text_member(file, "layer");
    const AbString *language = text_member(file, "language");
    const AbValue *symbols = ab_value_member(file, "symbols");
    RetrievalField file_fields[3];
    size_t symbol_index;
    if (!path || !layer || !language || !symbols ||
        symbols->kind != AB_VALUE_ARRAY) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "retrieval input Map file is invalid");
      break;
    }
    file_fields[0] = (RetrievalField){"file.path", path, 10};
    file_fields[1] = (RetrievalField){"file.layer", layer, 3};
    file_fields[2] = (RetrievalField){"file.language", language, 2};
    if (file_selected[index])
      status = add_candidate(engine, result, &items, &count, &capacity,
                             AB_RETRIEVAL_FILE, file, path, path, NULL, 0,
                             index, file_fields, 3);
    while (status == ARCHBIRD_OK && file_selected[index] &&
           call_index < calls->as.array.count) {
      const AbValue *call = &calls->as.array.items[call_index];
      const AbString *source = text_member(call, "source");
      const AbString *name = text_member(call, "name");
      int compared;
      RetrievalField field;
      if (!source || !name) {
        status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "retrieval input Map call is invalid");
        break;
      }
      compared = ab_string_compare(source, path);
      if (compared < 0) {
        call_index++;
        continue;
      }
      if (compared > 0)
        break;
      field = (RetrievalField){"file.call", name, 5};
      consider_candidate_field(&items[count - 1], result, &field);
      call_index++;
    }
    for (symbol_index = 0;
         status == ARCHBIRD_OK && symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbString *name = text_member(symbol, "name");
      const AbString *kind = text_member(symbol, "kind");
      const AbString *scope = text_member(symbol, "scope");
      const AbString *signature = text_member(symbol, "signature");
      const AbValue *line_value = ab_value_member(symbol, "line");
      uint64_t line;
      RetrievalField fields[7];
      size_t field_count = 0;
      if (!symbol_selected[symbol_offsets[index] + symbol_index])
        continue;
      if (!name || !kind || !scope || !line_value ||
          !ab_value_u64(line_value, &line) || line > SIZE_MAX) {
        status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "retrieval input Map symbol is invalid");
        break;
      }
      fields[field_count++] = (RetrievalField){"symbol.name", name, 14};
      fields[field_count++] = (RetrievalField){"symbol.kind", kind, 2};
      fields[field_count++] = (RetrievalField){"symbol.scope", scope, 1};
      fields[field_count++] = (RetrievalField){"file.path", path, 6};
      fields[field_count++] = (RetrievalField){"file.layer", layer, 3};
      fields[field_count++] = (RetrievalField){"file.language", language, 3};
      if (signature)
        fields[field_count++] =
            (RetrievalField){"symbol.signature", signature, 4};
      status = add_candidate(engine, result, &items, &count, &capacity,
                             AB_RETRIEVAL_SYMBOL, symbol, path, name, kind,
                             (size_t)line, index, fields, field_count);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < components->as.array.count;
       index++) {
    const AbValue *row = &components->as.array.items[index];
    const AbString *name = text_member(row, "name");
    const AbString *description = text_member(row, "description");
    RetrievalField fields[2];
    size_t field_count = 0;
    if (!component_selected[index])
      continue;
    if (!name) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "retrieval input Map component is invalid");
      break;
    }
    fields[field_count++] = (RetrievalField){"component.name", name, 12};
    if (description)
      fields[field_count++] =
          (RetrievalField){"component.description", description, 5};
    status = add_candidate(engine, result, &items, &count, &capacity,
                           AB_RETRIEVAL_COMPONENT, row, NULL, name, NULL, 0,
                           index, fields, field_count);
  }
  for (index = 0; status == ARCHBIRD_OK && index < packages->as.array.count;
       index++) {
    const AbValue *row = &packages->as.array.items[index];
    const AbString *name = text_member(row, "name");
    const AbString *identity = text_member(row, "identity");
    const AbString *manifest = text_member(row, "manifest");
    RetrievalField fields[3];
    size_t field_count = 0;
    const AbValue *aliases;
    const AbValue *dependencies;
    const AbValue *exports;
    size_t item_index;
    if (!package_selected[index])
      continue;
    if (!name || !identity || !manifest) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "retrieval input Map package is invalid");
      break;
    }
    fields[field_count++] = (RetrievalField){"package.name", name, 12};
    fields[field_count++] = (RetrievalField){"package.identity", identity, 12};
    fields[field_count++] = (RetrievalField){"package.manifest", manifest, 5};
    status = add_candidate(engine, result, &items, &count, &capacity,
                           AB_RETRIEVAL_PACKAGE, row, manifest, identity, NULL,
                           0, index, fields, field_count);
    if (status != ARCHBIRD_OK)
      break;
    aliases = optional_array_member(row, "aliases");
    dependencies = optional_array_member(row, "dependencies");
    exports = optional_array_member(row, "exports");
    if (aliases->kind != AB_VALUE_ARRAY ||
        dependencies->kind != AB_VALUE_ARRAY ||
        exports->kind != AB_VALUE_ARRAY) {
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "retrieval input Map package metadata is invalid");
      break;
    }
    for (item_index = 0; item_index < aliases->as.array.count; item_index++) {
      const AbValue *value = &aliases->as.array.items[item_index];
      RetrievalField field;
      if (value->kind != AB_VALUE_STRING) {
        status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "retrieval package alias is invalid");
        break;
      }
      field = (RetrievalField){"package.alias", &value->as.text, 10};
      consider_candidate_field(&items[count - 1], result, &field);
    }
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < dependencies->as.array.count;
         item_index++) {
      const AbString *dependency =
          text_member(&dependencies->as.array.items[item_index], "name");
      RetrievalField field;
      if (!dependency) {
        status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "retrieval package dependency is invalid");
        break;
      }
      field = (RetrievalField){"package.dependency", dependency, 4};
      consider_candidate_field(&items[count - 1], result, &field);
    }
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < exports->as.array.count;
         item_index++) {
      const AbValue *value = &exports->as.array.items[item_index];
      RetrievalField field;
      if (value->kind != AB_VALUE_STRING) {
        status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                    ARCHBIRD_NO_OFFSET,
                                    "retrieval package export is invalid");
        break;
      }
      field = (RetrievalField){"package.export", &value->as.text, 7};
      consider_candidate_field(&items[count - 1], result, &field);
    }
  }
  for (index = 0; status == ARCHBIRD_OK && index < artifacts->as.array.count;
       index++) {
    const AbValue *row = &artifacts->as.array.items[index];
    const AbString *name = text_member(row, "name");
    const AbString *output = text_member(row, "output");
    RetrievalField fields[2];
    if (!artifact_selected[index])
      continue;
    if (!name || !output) {
      status = archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA,
                                  ARCHBIRD_NO_OFFSET,
                                  "retrieval input Map artifact is invalid");
      break;
    }
    fields[0] = (RetrievalField){"artifact.name", name, 12};
    fields[1] = (RetrievalField){"artifact.output", output, 8};
    status = add_candidate(engine, result, &items, &count, &capacity,
                           AB_RETRIEVAL_ARTIFACT, row, output, name, NULL, 0,
                           index, fields, 2);
  }
cleanup:
  ab_free(engine, artifact_selected);
  ab_free(engine, package_selected);
  ab_free(engine, component_selected);
  ab_free(engine, symbol_selected);
  ab_free(engine, file_selected);
  ab_free(engine, symbol_offsets);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, items);
    return status;
  }
  *out = items;
  *out_count = count;
  return ARCHBIRD_OK;
}

static int hit_compare(const void *left_raw, const void *right_raw) {
  const AbRetrievalHit *left = (const AbRetrievalHit *)left_raw;
  const AbRetrievalHit *right = (const AbRetrievalHit *)right_raw;
  int compared;
  if (left->score != right->score)
    return left->score < right->score ? 1 : -1;
  if (left->kind != right->kind)
    return left->kind < right->kind ? -1 : 1;
  if (left->path || right->path) {
    static const AbString empty = {NULL, 0};
    compared = ab_string_compare(left->path ? left->path : &empty,
                                 right->path ? right->path : &empty);
    if (compared)
      return compared;
  }
  if (left->name || right->name) {
    static const AbString empty = {NULL, 0};
    compared = ab_string_compare(left->name ? left->name : &empty,
                                 right->name ? right->name : &empty);
    if (compared)
      return compared;
  }
  return (left->line > right->line) - (left->line < right->line);
}

static int nullable_string_equal(const AbString *left, const AbString *right) {
  return (!left && !right) || (left && right && ab_string_equal(left, right));
}

static int hit_identity_equal(const AbRetrievalHit *left,
                              const AbRetrievalHit *right) {
  return left->kind == right->kind && left->line == right->line &&
         nullable_string_equal(left->path, right->path) &&
         nullable_string_equal(left->name, right->name);
}

static int hit_location_equal(const AbRetrievalHit *left,
                              const AbRetrievalHit *right) {
  if (left->path || right->path)
    return nullable_string_equal(left->path, right->path);
  return left->kind == right->kind &&
         nullable_string_equal(left->name, right->name);
}

static int selected_contains(const AbRetrievalHit *selected, size_t count,
                             const AbRetrievalHit *candidate,
                             int location_only) {
  size_t index;
  for (index = 0; index < count; index++)
    if (location_only ? hit_location_equal(&selected[index], candidate)
                      : hit_identity_equal(&selected[index], candidate))
      return 1;
  return 0;
}

static ArchbirdStatus diversify_hits(ArchbirdEngine *engine,
                                     AbRetrievalResult *result) {
  AbRetrievalHit *selected;
  size_t target = result->hit_count;
  size_t diverse_target = target >= 3 ? (target * 2 + 2) / 3 : target;
  size_t count = 0;
  size_t index;
  if (target <= 1)
    return ARCHBIRD_OK;
  selected = (AbRetrievalHit *)ab_malloc(engine, target * sizeof(*selected));
  if (!selected)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory diversifying retrieval results");
  for (index = 0; index < result->matched_count && count < diverse_target;
       index++) {
    if (selected_contains(selected, count, &result->hits[index], 1))
      continue;
    selected[count++] = result->hits[index];
  }
  for (index = 0; index < result->matched_count && count < target; index++) {
    if (selected_contains(selected, count, &result->hits[index], 0))
      continue;
    selected[count++] = result->hits[index];
  }
  qsort(selected, count, sizeof(*selected), hit_compare);
  memcpy(result->hits, selected, count * sizeof(*selected));
  result->hit_count = count;
  ab_free(engine, selected);
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_query_retrieve(ArchbirdEngine *engine, const AbValue *map,
                                 const AbProjectionData *domain,
                                 const AbValue *queries, size_t limit,
                                 AbRetrievalResult *out) {
  RetrievalCandidate *candidates = NULL;
  size_t candidate_count = 0;
  size_t index;
  size_t term_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  memset(out, 0, sizeof(*out));
  out->limit = limit;
  if (!domain ||
      strcmp(ab_projection_data_classification(domain), "complete") ||
      !queries || queries->kind != AB_VALUE_ARRAY || !queries->as.array.count ||
      !limit)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "retrieval requires search strings and limit > 0");
  for (index = 0; index < queries->as.array.count; index++) {
    const AbValue *query = &queries->as.array.items[index];
    if (query->kind != AB_VALUE_STRING || !query->as.text.length ||
        query->as.text.length > 1024) {
      status = archbird_error_set(
          engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
          "retrieval queries must be 1..1024 byte strings");
      goto cleanup;
    }
    status = add_terms(engine, out, &query->as.text);
    if (status != ARCHBIRD_OK)
      goto cleanup;
  }
  compact_common_terms(engine, out);
  if (!out->term_count) {
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "retrieval query contains no searchable terms");
    goto cleanup;
  }
  status = collect_candidates(engine, map, domain, out, &candidates,
                              &candidate_count);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  out->candidate_count = candidate_count;
  for (index = 0; index < candidate_count; index++)
    for (term_index = 0; term_index < out->term_count; term_index++)
      if (candidates[index].strengths[term_index])
        out->document_frequency[term_index]++;
  for (index = 0; index < candidate_count; index++) {
    RetrievalCandidate *candidate = &candidates[index];
    size_t matched_terms = 0;
    for (term_index = 0; term_index < out->term_count; term_index++) {
      uint64_t rarity;
      uint64_t contribution;
      if (!candidate->strengths[term_index])
        continue;
      matched_terms++;
      rarity = 64 + (((uint64_t)candidate_count + 1) * 256) /
                        (out->document_frequency[term_index] + 1);
      if (rarity > 2048)
        rarity = 2048;
      contribution = (uint64_t)candidate->strengths[term_index] *
                     candidate->weights[term_index] * rarity;
      if (out->term_count > 1 && candidate->strengths[term_index] < 44)
        contribution /= 4;
      candidate->hit.score += contribution;
    }
    candidate->hit.score *= (uint64_t)matched_terms * matched_terms;
  }
  for (index = 0; index < candidate_count; index++)
    if (candidates[index].hit.score)
      out->matched_count++;
  if (!out->matched_count)
    goto cleanup;
  out->hit_count = out->matched_count < limit ? out->matched_count : limit;
  out->hits = (AbRetrievalHit *)ab_malloc(engine, out->matched_count *
                                                      sizeof(*out->hits));
  if (!out->hits) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory ranking retrieval candidates");
    goto cleanup;
  }
  {
    size_t write = 0;
    for (index = 0; index < candidate_count; index++)
      if (candidates[index].hit.score)
        out->hits[write++] = candidates[index].hit;
  }
  qsort(out->hits, out->matched_count, sizeof(*out->hits), hit_compare);
  status = diversify_hits(engine, out);
cleanup:
  ab_free(engine, candidates);
  if (status != ARCHBIRD_OK)
    ab_query_retrieval_free(engine, out);
  return status;
}

void ab_query_retrieval_free(ArchbirdEngine *engine,
                             AbRetrievalResult *result) {
  size_t index;
  for (index = 0; index < result->term_count; index++)
    ab_string_free(engine, &result->terms[index]);
  ab_free(engine, result->hits);
  memset(result, 0, sizeof(*result));
}

const char *ab_query_retrieval_kind_name(AbRetrievalKind kind) {
  switch (kind) {
  case AB_RETRIEVAL_FILE:
    return "file";
  case AB_RETRIEVAL_SYMBOL:
    return "symbol";
  case AB_RETRIEVAL_COMPONENT:
    return "component";
  case AB_RETRIEVAL_PACKAGE:
    return "package";
  case AB_RETRIEVAL_ARTIFACT:
    return "artifact";
  }
  return "unknown";
}
