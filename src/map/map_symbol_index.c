#include "map_internal.h"

#include <string.h>

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int raw_compare(const char *left, size_t left_length, const char *right,
                       size_t right_length) {
  size_t common = left_length < right_length ? left_length : right_length;
  int compared = common ? memcmp(left, right, common) : 0;
  if (compared)
    return compared < 0 ? -1 : 1;
  return (left_length > right_length) - (left_length < right_length);
}

const AbObjectField *ab_map_fact_attribute(const AbFact *fact,
                                           const char *name) {
  AbString wanted = {(char *)name, strlen(name)};
  size_t low = 0;
  size_t high = fact->attribute_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&fact->attributes[middle].name, &wanted);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &fact->attributes[middle];
  }
  return NULL;
}

const AbString *ab_map_fact_string_attribute(const AbFact *fact,
                                             const char *name) {
  const AbObjectField *field = ab_map_fact_attribute(fact, name);
  return field && field->value.kind == AB_VALUE_STRING ? &field->value.as.text
                                                       : NULL;
}

int ab_map_fact_u64_attribute(const AbFact *fact, const char *name,
                              uint64_t *out) {
  const AbObjectField *field = ab_map_fact_attribute(fact, name);
  uint64_t value = 0;
  size_t index;
  if (!field || field->value.kind != AB_VALUE_INTEGER)
    return 0;
  for (index = 0; index < field->value.as.text.length; index++) {
    unsigned char digit = (unsigned char)field->value.as.text.data[index];
    if (digit < '0' || digit > '9' ||
        value > (UINT64_MAX - (uint64_t)(digit - '0')) / 10)
      return 0;
    value = value * 10 + (uint64_t)(digit - '0');
  }
  *out = value;
  return 1;
}

const char *ab_map_symbol_language_family(const AbManifestFile *file,
                                          size_t *out_length) {
  if (!file->has_language) {
    *out_length = 0;
    return "";
  }
  if (string_literal(&file->language, "c") ||
      string_literal(&file->language, "cpp")) {
    *out_length = 1;
    return "c";
  }
  if (string_literal(&file->language, "javascript") ||
      string_literal(&file->language, "typescript") ||
      string_literal(&file->language, "vue")) {
    *out_length = 10;
    return "javascript";
  }
  *out_length = file->language.length;
  return file->language.data;
}

void ab_map_symbol_leaf(const AbFact *fact, const char **out_leaf,
                        size_t *out_length) {
  size_t index = fact->name.length;
  while (index && fact->name.data[index - 1] != '.')
    index--;
  *out_leaf = fact->name.data + index;
  *out_length = fact->name.length - index;
}

int ab_map_symbol_definition_compare(const AbFact *left, const AbFact *right) {
  const AbString *left_recovery;
  const AbString *right_recovery;
  uint64_t left_nodes = 0;
  uint64_t right_nodes = 0;
  int compared;
  if (!left || !right)
    return (left == NULL) - (right == NULL);
  left_recovery = ab_map_fact_string_attribute(left, "syntax_recovery");
  right_recovery = ab_map_fact_string_attribute(right, "syntax_recovery");
  if ((left_recovery != NULL) != (right_recovery != NULL))
    return left_recovery ? 1 : -1;
  if (left_recovery) {
    compared = ab_string_compare(left_recovery, right_recovery);
    if (compared)
      return compared;
  }
  (void)ab_map_fact_u64_attribute(left, "recovery_nodes", &left_nodes);
  (void)ab_map_fact_u64_attribute(right, "recovery_nodes", &right_nodes);
  if (left_nodes != right_nodes)
    return left_nodes < right_nodes ? -1 : 1;
  if (left->span_start != right->span_start)
    return left->span_start < right->span_start ? -1 : 1;
  if (left->span_end != right->span_end)
    return left->span_end < right->span_end ? -1 : 1;
  compared = ab_string_compare(&left->kind, &right->kind);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->claim, &right->claim);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->key, &right->key);
  return compared ? compared : ab_string_compare(&left->id, &right->id);
}

int ab_map_symbol_reference_compare(const void *left_raw,
                                    const void *right_raw) {
  const AbMapSymbolReference *left = (const AbMapSymbolReference *)left_raw;
  const AbMapSymbolReference *right = (const AbMapSymbolReference *)right_raw;
  size_t left_family_length;
  size_t right_family_length;
  const char *left_family =
      ab_map_symbol_language_family(left->file, &left_family_length);
  const char *right_family =
      ab_map_symbol_language_family(right->file, &right_family_length);
  int compared = raw_compare(left_family, left_family_length, right_family,
                             right_family_length);
  if (compared)
    return compared;
  compared = raw_compare(left->leaf, left->leaf_length, right->leaf,
                         right->leaf_length);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->file->path, &right->file->path);
  if (compared)
    return compared;
  compared = ab_string_compare(&left->fact->name, &right->fact->name);
  if (compared)
    return compared;
  return ab_map_symbol_definition_compare(left->fact, right->fact);
}

static int reference_key_compare(const AbMapSymbolReference *symbol,
                                 const char *family, size_t family_length,
                                 const AbString *name) {
  size_t symbol_family_length;
  const char *symbol_family =
      ab_map_symbol_language_family(symbol->file, &symbol_family_length);
  int compared =
      raw_compare(symbol_family, symbol_family_length, family, family_length);
  if (!compared)
    compared = raw_compare(symbol->leaf, symbol->leaf_length, name->data,
                           name->length);
  return compared;
}

void ab_map_symbol_reference_range(const AbMapSymbolReference *symbols,
                                   size_t symbol_count,
                                   const AbManifestFile *source,
                                   const AbString *name, size_t *out_start,
                                   size_t *out_end) {
  size_t family_length;
  const char *family = ab_map_symbol_language_family(source, &family_length);
  size_t low = 0;
  size_t high = symbol_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (reference_key_compare(&symbols[middle], family, family_length, name) <
        0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_start = low;
  high = symbol_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (reference_key_compare(&symbols[middle], family, family_length, name) <=
        0)
      low = middle + 1;
    else
      high = middle;
  }
  *out_end = low;
}

int ab_map_symbol_reference_usable(const AbMapSymbolReference *symbol,
                                   const AbManifestFile *source,
                                   int allow_methods, int allow_declarations) {
  const AbString *scope;
  if ((!allow_declarations &&
       string_literal(&symbol->fact->kind, "declaration")) ||
      (!allow_methods && string_literal(&symbol->fact->kind, "method")))
    return 0;
  scope = ab_map_fact_string_attribute(symbol->fact, "scope");
  return !scope || !string_literal(scope, "local") || symbol->file == source;
}

const AbFact *ab_map_enclosing_symbol(const ArchbirdProject *project,
                                      const AbFact *occurrence) {
  const AbString *enclosing =
      ab_map_fact_string_attribute(occurrence, "enclosing");
  size_t start;
  size_t end;
  size_t index;
  const AbFact *matched = NULL;
  if (!enclosing || !enclosing->length)
    return NULL;
  ab_project_merged_fact_range(project, &occurrence->path, "symbols", &start,
                               &end);
  for (index = start; index < end; index++) {
    const AbFact *candidate = ab_project_merged_fact_by_path(project, index);
    if (!candidate || !candidate->has_name ||
        !ab_string_equal(&candidate->name, enclosing) ||
        string_literal(&candidate->kind, "declaration"))
      continue;
    if (matched)
      return NULL;
    matched = candidate;
  }
  return matched;
}

const AbFact *ab_map_unique_semantic_target(const ArchbirdProject *project,
                                            const AbFact *occurrence) {
  size_t start;
  size_t end;
  size_t index;
  const AbFact *matched = NULL;
  ab_project_merged_fact_range(project, &occurrence->path, "reference-targets",
                               &start, &end);
  for (index = start; index < end; index++) {
    const AbFact *candidate = ab_project_merged_fact_by_path(project, index);
    const AbString *target_path;
    const AbString *target_symbol;
    if (!candidate || candidate->span_start != occurrence->span_start ||
        candidate->span_end != occurrence->span_end ||
        !candidate->has_resolution ||
        !string_literal(&candidate->resolution.state, "unique"))
      continue;
    target_path = ab_map_fact_string_attribute(candidate, "target_path");
    target_symbol = ab_map_fact_string_attribute(candidate, "target_symbol");
    if (!target_path || !target_symbol)
      continue;
    if (matched) {
      const AbString *matched_path =
          ab_map_fact_string_attribute(matched, "target_path");
      const AbString *matched_symbol =
          ab_map_fact_string_attribute(matched, "target_symbol");
      if (!matched_path || !matched_symbol ||
          !ab_string_equal(matched_path, target_path) ||
          !ab_string_equal(matched_symbol, target_symbol))
        return NULL;
      continue;
    }
    matched = candidate;
  }
  return matched;
}
