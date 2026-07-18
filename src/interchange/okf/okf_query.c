#include "okf_internal.h"

#include <stdlib.h>
#include <string.h>

static int string_in_array(const AbValue *array, const AbString *value) {
  size_t index;
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *row = &array->as.array.items[index];
    if (row->kind == AB_VALUE_STRING && ab_string_equal(&row->as.text, value))
      return 1;
  }
  return 0;
}

static int folded_in_array(const AbValue *array, const AbString *value) {
  size_t index;
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *row = &array->as.array.items[index];
    const AbString *folded = ab_okf_optional_text(row, "casefold");
    if (folded && ab_string_equal(folded, value))
      return 1;
  }
  return 0;
}

static int bytes_contains(const AbString *haystack, const AbString *needle) {
  size_t index;
  if (!needle->length)
    return 1;
  if (needle->length > haystack->length)
    return 0;
  for (index = 0; index <= haystack->length - needle->length; index++)
    if (!memcmp(haystack->data + index, needle->data, needle->length))
      return 1;
  return 0;
}

static int requirements_match(const AbOkfIndex *index,
                              const AbOkfDocument *document,
                              const AbValue *selectors) {
  size_t link;
  for (link = 0; link < index->requirement_count; link++) {
    const AbOkfRequirementLink *row = &index->requirements[link];
    if (ab_string_equal(&row->concept_id, &document->concept_id) &&
        string_in_array(selectors, &row->requirement_id))
      return 1;
  }
  return 0;
}

static int concepts_match(const AbOkfDocument *document,
                          const AbValue *selectors) {
  size_t item;
  for (item = 0; item < selectors->as.array.count; item++)
    if (ab_map_glob_match(&selectors->as.array.items[item].as.text,
                          &document->concept_id))
      return 1;
  return 0;
}

static int tags_match(const AbOkfDocument *document, const AbValue *selectors) {
  size_t selector;
  for (selector = 0; selector < selectors->as.array.count; selector++) {
    const AbString *needle =
        ab_okf_optional_text(&selectors->as.array.items[selector], "casefold");
    if (!needle || !string_in_array(document->folded_tags, needle))
      return 0;
  }
  return 1;
}

static int text_matches(const AbOkfDocument *document,
                        const AbValue *selectors) {
  size_t selector;
  for (selector = 0; selector < selectors->as.array.count; selector++) {
    const AbString *needle =
        ab_okf_optional_text(&selectors->as.array.items[selector], "casefold");
    if (!needle || !bytes_contains(document->folded_text, needle))
      return 0;
  }
  return 1;
}

static int document_selected(const AbOkfIndex *index,
                             const AbOkfDocument *document,
                             const AbOkfQuery *query) {
  if (!(document->kind.length == 7 &&
        !memcmp(document->kind.data, "concept", 7)))
    return 0;
  if (query->requirements->as.array.count &&
      !requirements_match(index, document, query->requirements))
    return 0;
  if (query->concepts->as.array.count &&
      !concepts_match(document, query->concepts))
    return 0;
  if (query->types->as.array.count &&
      !folded_in_array(query->types, document->folded_type))
    return 0;
  if (query->tags->as.array.count && !tags_match(document, query->tags))
    return 0;
  return !query->text->as.array.count || text_matches(document, query->text);
}

static int validate_strings(const AbValue *array) {
  size_t item;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (item = 0; item < array->as.array.count; item++)
    if (array->as.array.items[item].kind != AB_VALUE_STRING ||
        !array->as.array.items[item].as.text.length)
      return 0;
  return 1;
}

static int validate_folded(const AbValue *array) {
  size_t item;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (item = 0; item < array->as.array.count; item++) {
    const AbValue *row = &array->as.array.items[item];
    const AbString *value = ab_okf_optional_text(row, "value");
    const AbString *folded = ab_okf_optional_text(row, "casefold");
    if (row->kind != AB_VALUE_OBJECT || !value || !value->length || !folded ||
        !folded->length)
      return 0;
  }
  return 1;
}

ArchbirdStatus ab_okf_query_load(ArchbirdEngine *engine,
                                 const uint8_t *query_json, size_t query_length,
                                 const AbOkfIndex *index, AbOkfQuery *out) {
  uint64_t schema;
  size_t document;
  ArchbirdStatus status;
  if (!engine || !index || !out || (!query_json && query_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  if (!query_json && !query_length)
    return ARCHBIRD_OK;
  status = ab_json_value_decode(engine, query_json, query_length, &out->input);
  if (status != ARCHBIRD_OK)
    return status;
  out->concepts = ab_value_member(&out->input, "concepts");
  out->types = ab_value_member(&out->input, "types");
  out->tags = ab_value_member(&out->input, "tags");
  out->text = ab_value_member(&out->input, "text");
  out->requirements = ab_value_member(&out->input, "requirements");
  if (out->input.kind != AB_VALUE_OBJECT ||
      !ab_value_u64(ab_value_member(&out->input, "schema_version"), &schema) ||
      schema != 1 ||
      !ab_okf_text_is(ab_value_member(&out->input, "artifact"),
                      "okf-query-input") ||
      !validate_strings(out->concepts) || !validate_folded(out->types) ||
      !validate_folded(out->tags) || !validate_folded(out->text) ||
      !validate_strings(out->requirements)) {
    status = ab_okf_error(engine, "invalid query selectors");
    goto fail;
  }
  out->active = out->concepts->as.array.count || out->types->as.array.count ||
                out->tags->as.array.count || out->text->as.array.count ||
                out->requirements->as.array.count;
  if (index->document_count) {
    out->selected = (size_t *)ab_calloc(engine, index->document_count,
                                        sizeof(*out->selected));
    if (!out->selected) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory selecting OKF concepts");
      goto fail;
    }
  }
  for (document = 0; document < index->document_count; document++)
    if (document_selected(index, &index->documents[document], out))
      out->selected[out->selected_count++] = document;
  if (out->active && !out->selected_count) {
    status =
        archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                           "OKF query: selectors matched no concepts");
    goto fail;
  }
  return ARCHBIRD_OK;

fail:
  ab_okf_query_free(out);
  return status;
}

void ab_okf_query_free(AbOkfQuery *query) {
  if (!query)
    return;
  ab_free(query->engine, query->selected);
  ab_value_free(query->engine, &query->input);
  memset(query, 0, sizeof(*query));
}
