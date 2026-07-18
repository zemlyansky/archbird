#include "map_internal.h"

#include "archbird_internal.h"
#include "pattern.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct PatternList {
  AbPattern **items;
  size_t count;
} PatternList;

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int string_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static const AbObjectField *fact_attribute(const AbFact *fact,
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

static int fact_domain(const AbFact *fact, const char *domain) {
  return string_literal(&fact->domain, domain);
}

static const AbManifestFile *fact_file(const AbMapState *state,
                                       const AbFact *fact) {
  return ab_map_manifest_file(state->manifest, fact->path.data,
                              fact->path.length);
}

static int evidence_compare(const void *left_raw, const void *right_raw) {
  const AbMapParityEvidence *left = (const AbMapParityEvidence *)left_raw;
  const AbMapParityEvidence *right = (const AbMapParityEvidence *)right_raw;
  return ab_string_compare(&left->name, &right->name);
}

static int array_contains(const AbStringArray *array, const AbString *value) {
  size_t index;
  for (index = 0; index < array->count; index++) {
    if (ab_string_equal(&array->items[index], value))
      return 1;
  }
  return 0;
}

static ArchbirdStatus append_unique(ArchbirdEngine *engine,
                                    AbStringArray *array, const char *data,
                                    size_t length) {
  AbString value = {(char *)data, length};
  AbString *resized;
  if (array_contains(array, &value))
    return ARCHBIRD_OK;
  resized = (AbString *)ab_realloc(engine, array->items,
                                   (array->count + 1) * sizeof(*array->items));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory collecting parity values");
  array->items = resized;
  memset(&array->items[array->count], 0, sizeof(*array->items));
  if (ab_string_copy(engine, &array->items[array->count], data, length) !=
      ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  array->count++;
  return ARCHBIRD_OK;
}

static void pattern_list_free(ArchbirdEngine *engine, PatternList *patterns) {
  size_t index;
  for (index = 0; index < patterns->count; index++)
    ab_pattern_free(patterns->items[index]);
  ab_free(engine, patterns->items);
  memset(patterns, 0, sizeof(*patterns));
}

static ArchbirdStatus pattern_list_compile(ArchbirdEngine *engine,
                                           const AbStringArray *sources,
                                           PatternList *out) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (sources->count) {
    out->items =
        (AbPattern **)ab_calloc(engine, sources->count, sizeof(*out->items));
    if (!out->items)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory compiling parity patterns");
  }
  out->count = sources->count;
  for (index = 0; status == ARCHBIRD_OK && index < sources->count; index++)
    status = ab_pattern_compile(engine, &sources->items[index], SIZE_MAX,
                                &out->items[index]);
  if (status != ARCHBIRD_OK)
    pattern_list_free(engine, out);
  return status;
}

static ArchbirdStatus pattern_matches(ArchbirdEngine *engine,
                                      const AbPattern *pattern,
                                      const AbString *value, int *out) {
  size_t count = 0;
  ArchbirdStatus status =
      ab_pattern_scan(engine, pattern, (const uint8_t *)value->data,
                      value->length, SIZE_MAX, NULL, NULL, &count);
  if (status == ARCHBIRD_OK)
    *out = count != 0;
  return status;
}

static ArchbirdStatus selected(ArchbirdEngine *engine,
                               const PatternList *include,
                               const PatternList *exclude, const AbString *name,
                               int *out) {
  size_t index;
  int matched = include->count == 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && !matched && index < include->count;
       index++)
    status = pattern_matches(engine, include->items[index], name, &matched);
  for (index = 0; status == ARCHBIRD_OK && matched && index < exclude->count;
       index++) {
    int rejected = 0;
    status = pattern_matches(engine, exclude->items[index], name, &rejected);
    if (rejected)
      matched = 0;
  }
  if (status == ARCHBIRD_OK)
    *out = matched;
  return status;
}

static ArchbirdStatus snake_case(ArchbirdEngine *engine, const AbString *input,
                                 AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  for (index = 0; status == ARCHBIRD_OK && index < input->length; index++) {
    unsigned char current = (unsigned char)input->data[index];
    unsigned char previous = index ? (unsigned char)input->data[index - 1] : 0;
    unsigned char next =
        index + 1 < input->length ? (unsigned char)input->data[index + 1] : 0;
    if (current >= 0x80) {
      status = archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "Unicode parity case normalization requires a Unicode provider");
      break;
    }
    if (current == '-')
      current = '_';
    if (current >= 'A' && current <= 'Z' && index &&
        ((previous >= 'a' && previous <= 'z') ||
         (previous >= '0' && previous <= '9') ||
         ((previous >= 'A' && previous <= 'Z') &&
          (next >= 'a' && next <= 'z'))))
      status = ab_buffer_literal(&buffer, "_");
    if (status == ARCHBIRD_OK) {
      char byte = (char)tolower(current);
      status = ab_buffer_append(&buffer, &byte, 1);
    }
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus lower_case(ArchbirdEngine *engine, const AbString *input,
                                 AbString *out) {
  char *value;
  size_t index;
  value = (char *)ab_malloc(engine, input->length + 1);
  if (!value)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory normalizing parity value");
  for (index = 0; index < input->length; index++) {
    unsigned char byte = (unsigned char)input->data[index];
    if (byte >= 0x80) {
      ab_free(engine, value);
      return archbird_error_set(
          engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
          "Unicode parity case normalization requires a Unicode provider");
    }
    value[index] = (char)tolower(byte);
  }
  value[input->length] = '\0';
  out->data = value;
  out->length = input->length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus normalize(ArchbirdEngine *engine,
                                const AbConfigParity *spec,
                                const AbString *input, AbString *out) {
  AbString view = *input;
  AbString transformed = {0};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; index < spec->strip_prefixes.count; index++) {
    const AbString *prefix = &spec->strip_prefixes.items[index];
    if (prefix->length <= view.length &&
        memcmp(view.data, prefix->data, prefix->length) == 0) {
      view.data += prefix->length;
      view.length -= prefix->length;
      break;
    }
  }
  for (index = 0; index < spec->strip_suffixes.count; index++) {
    const AbString *suffix = &spec->strip_suffixes.items[index];
    if (suffix->length <= view.length &&
        memcmp(view.data + view.length - suffix->length, suffix->data,
               suffix->length) == 0) {
      view.length -= suffix->length;
      break;
    }
  }
  if (string_literal(&spec->case_name, "lower"))
    status = lower_case(engine, &view, &transformed);
  else if (string_literal(&spec->case_name, "snake"))
    status = snake_case(engine, &view, &transformed);
  else
    status = ab_string_copy(engine, &transformed, view.data, view.length);
  for (index = 0; status == ARCHBIRD_OK && index < spec->alias_count; index++) {
    if (ab_string_equal(&transformed, &spec->aliases[index].key)) {
      ab_string_free(engine, &transformed);
      status =
          ab_string_copy(engine, &transformed, spec->aliases[index].value.data,
                         spec->aliases[index].value.length);
      break;
    }
  }
  if (status == ARCHBIRD_OK)
    *out = transformed;
  else
    ab_string_free(engine, &transformed);
  return status;
}

static AbMapParityEvidence *evidence_row(AbMapState *state,
                                         AbMapParityMember *member,
                                         const AbString *name,
                                         ArchbirdStatus *status) {
  AbMapParityEvidence *resized;
  size_t index;
  for (index = 0; index < member->evidence_count; index++) {
    if (ab_string_equal(&member->evidence[index].name, name))
      return &member->evidence[index];
  }
  resized = (AbMapParityEvidence *)ab_realloc(state->engine, member->evidence,
                                              (member->evidence_count + 1) *
                                                  sizeof(*member->evidence));
  if (!resized) {
    *status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                 ARCHBIRD_NO_OFFSET,
                                 "out of memory collecting parity evidence");
    return NULL;
  }
  member->evidence = resized;
  memset(&member->evidence[member->evidence_count], 0,
         sizeof(*member->evidence));
  *status = ab_string_copy(state->engine,
                           &member->evidence[member->evidence_count].name,
                           name->data, name->length);
  if (*status != ARCHBIRD_OK)
    return NULL;
  member->evidence_count++;
  return &member->evidence[member->evidence_count - 1];
}

static ArchbirdStatus add_evidence(AbMapState *state,
                                   const AbConfigParity *parity,
                                   AbMapParityMember *member,
                                   const AbString *raw, const char *location,
                                   size_t location_length,
                                   const AbStringArray *ignored) {
  AbString normalized = {0};
  AbMapParityEvidence *row;
  AbBuffer witness;
  ArchbirdStatus status = normalize(state->engine, parity, raw, &normalized);
  if (status != ARCHBIRD_OK || !normalized.length ||
      array_contains(ignored, &normalized)) {
    ab_string_free(state->engine, &normalized);
    return status;
  }
  row = evidence_row(state, member, &normalized, &status);
  ab_buffer_init(&witness, state->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&witness, raw->data, raw->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&witness, "@");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&witness, location, location_length);
  if (status == ARCHBIRD_OK)
    status = append_unique(state->engine, &row->locations,
                           (const char *)witness.data, witness.length);
  ab_buffer_free(&witness);
  ab_string_free(state->engine, &normalized);
  return status;
}

static const AbObjectField *line_attribute(const AbFact *fact) {
  return fact_attribute(fact, "line");
}

static int kind_selected(const AbStringArray *kinds, const AbFact *fact) {
  return !kinds->count || array_contains(kinds, &fact->kind);
}

static const AbMapPackage *find_package(const AbMapState *state,
                                        const AbString *name) {
  size_t index;
  for (index = 0; index < state->package_count; index++) {
    if (ab_string_equal(&state->packages[index].name, name))
      return &state->packages[index];
  }
  return NULL;
}

static ArchbirdStatus analyze_member(AbMapState *state,
                                     const AbConfigParity *parity,
                                     const AbConfigParityMember *spec,
                                     const AbStringArray *ignored,
                                     AbMapParityMember *out) {
  PatternList include = {0};
  PatternList exclude = {0};
  size_t index;
  ArchbirdStatus status = ab_string_copy(state->engine, &out->label,
                                         spec->label.data, spec->label.length);
  if (status == ARCHBIRD_OK)
    status = pattern_list_compile(state->engine, &spec->include, &include);
  if (status == ARCHBIRD_OK)
    status = pattern_list_compile(state->engine, &spec->exclude, &exclude);
  if (status == ARCHBIRD_OK && string_literal(&spec->source, "symbols")) {
    for (index = 0; status == ARCHBIRD_OK &&
                    index < ab_project_merged_fact_count(state->project);
         index++) {
      const AbFact *fact = ab_project_merged_fact(state->project, index);
      const AbManifestFile *file;
      int keep = 0;
      AbBuffer location;
      const AbObjectField *line;
      if (!fact->has_name || !fact_domain(fact, "symbols") ||
          !kind_selected(&spec->kinds, fact))
        continue;
      file = fact_file(state, fact);
      if (!file || !file->has_layer ||
          !ab_string_equal(&file->layer, &spec->layer))
        continue;
      status = selected(state->engine, &include, &exclude, &fact->name, &keep);
      if (!keep || status != ARCHBIRD_OK)
        continue;
      ab_buffer_init(&location, state->engine);
      status = ab_buffer_append(&location, file->path.data, file->path.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&location, ":");
      line = line_attribute(fact);
      if (status == ARCHBIRD_OK && line && line->value.kind == AB_VALUE_INTEGER)
        status = ab_buffer_append(&location, line->value.as.text.data,
                                  line->value.as.text.length);
      if (status == ARCHBIRD_OK)
        status =
            add_evidence(state, parity, out, &fact->name,
                         (const char *)location.data, location.length, ignored);
      ab_buffer_free(&location);
    }
  } else if (status == ARCHBIRD_OK &&
             string_literal(&spec->source, "package")) {
    const AbMapPackage *package = find_package(state, &spec->package);
    if (!package)
      status = archbird_error_set(state->engine, ARCHBIRD_CONFLICT,
                                  ARCHBIRD_NO_OFFSET,
                                  "configured parity package is absent");
    for (index = 0; status == ARCHBIRD_OK && index < package->exports.count;
         index++) {
      int keep = 0;
      status = selected(state->engine, &include, &exclude,
                        &package->exports.items[index], &keep);
      if (status == ARCHBIRD_OK && keep)
        status = add_evidence(
            state, parity, out, &package->exports.items[index],
            package->manifest.data, package->manifest.length, ignored);
    }
  } else if (status == ARCHBIRD_OK) {
    AbBuffer kind;
    ab_buffer_init(&kind, state->engine);
    status = ab_buffer_literal(&kind, "bridge:");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&kind, spec->bridge.data, spec->bridge.length);
    for (index = 0; status == ARCHBIRD_OK && index < state->graph.edge_count;
         index++) {
      const AbMapEdgeMention *edge = &state->graph.edges[index];
      int keep = 0;
      if (edge->kind.length != kind.length ||
          memcmp(edge->kind.data, kind.data, kind.length) != 0)
        continue;
      status = selected(state->engine, &include, &exclude, &edge->name, &keep);
      if (status == ARCHBIRD_OK && keep)
        status =
            add_evidence(state, parity, out, &edge->name, edge->source->data,
                         edge->source->length, ignored);
    }
    ab_buffer_free(&kind);
  }
  if (status == ARCHBIRD_OK && out->evidence_count > 1)
    qsort(out->evidence, out->evidence_count, sizeof(*out->evidence),
          evidence_compare);
  for (index = 0; status == ARCHBIRD_OK && index < out->evidence_count;
       index++) {
    AbMapParityEvidence *evidence = &out->evidence[index];
    if (evidence->locations.count > 1)
      qsort(evidence->locations.items, evidence->locations.count,
            sizeof(*evidence->locations.items), string_compare);
    status = append_unique(state->engine, &out->values, evidence->name.data,
                           evidence->name.length);
  }
  pattern_list_free(state->engine, &include);
  pattern_list_free(state->engine, &exclude);
  return status;
}

static ArchbirdStatus ignored_values(AbMapState *state,
                                     const AbConfigParity *spec,
                                     AbStringArray *out) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < spec->ignore.count;
       index++) {
    AbString normalized = {0};
    status =
        normalize(state->engine, spec, &spec->ignore.items[index], &normalized);
    if (status == ARCHBIRD_OK)
      status =
          append_unique(state->engine, out, normalized.data, normalized.length);
    ab_string_free(state->engine, &normalized);
  }
  if (status == ARCHBIRD_OK && out->count > 1)
    qsort(out->items, out->count, sizeof(*out->items), string_compare);
  return status;
}

static void free_temporary_strings(ArchbirdEngine *engine,
                                   AbStringArray *array) {
  size_t index;
  for (index = 0; index < array->count; index++)
    ab_string_free(engine, &array->items[index]);
  ab_free(engine, array->items);
  memset(array, 0, sizeof(*array));
}

static ArchbirdStatus analyze_parity(AbMapState *state,
                                     const AbConfigParity *spec,
                                     AbMapParity *out) {
  AbStringArray ignored = {0};
  AbStringArray union_values = {0};
  size_t member_index;
  ArchbirdStatus status = ab_string_copy(state->engine, &out->name,
                                         spec->name.data, spec->name.length);
  out->enforce = spec->enforce;
  if (status == ARCHBIRD_OK)
    status = ignored_values(state, spec, &ignored);
  if (status == ARCHBIRD_OK && spec->member_count) {
    out->members = (AbMapParityMember *)ab_calloc(
        state->engine, spec->member_count, sizeof(*out->members));
    if (!out->members)
      status = archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory collecting parity members");
  }
  out->member_count = status == ARCHBIRD_OK ? spec->member_count : 0;
  for (member_index = 0;
       status == ARCHBIRD_OK && member_index < spec->member_count;
       member_index++) {
    size_t value_index;
    status = analyze_member(state, spec, &spec->members[member_index], &ignored,
                            &out->members[member_index]);
    for (value_index = 0; status == ARCHBIRD_OK &&
                          value_index < out->members[member_index].values.count;
         value_index++)
      status = append_unique(
          state->engine, &union_values,
          out->members[member_index].values.items[value_index].data,
          out->members[member_index].values.items[value_index].length);
  }
  if (status == ARCHBIRD_OK && union_values.count > 1)
    qsort(union_values.items, union_values.count, sizeof(*union_values.items),
          string_compare);
  for (member_index = 0;
       status == ARCHBIRD_OK && member_index < spec->member_count;
       member_index++) {
    size_t value_index;
    AbMapParityMember *member = &out->members[member_index];
    for (value_index = 0;
         status == ARCHBIRD_OK && value_index < union_values.count;
         value_index++) {
      if (!array_contains(&member->values, &union_values.items[value_index]))
        status = append_unique(state->engine, &member->missing,
                               union_values.items[value_index].data,
                               union_values.items[value_index].length);
    }
  }
  if (status == ARCHBIRD_OK) {
    size_t value_index;
    for (value_index = 0; value_index < union_values.count; value_index++) {
      int shared = 1;
      for (member_index = 0; member_index < spec->member_count; member_index++)
        shared = shared && array_contains(&out->members[member_index].values,
                                          &union_values.items[value_index]);
      if (shared)
        status = append_unique(state->engine, &out->shared,
                               union_values.items[value_index].data,
                               union_values.items[value_index].length);
      if (status != ARCHBIRD_OK)
        break;
    }
  }
  if (status == ARCHBIRD_OK && out->enforce) {
    int mismatch = 0;
    AbBuffer message;
    for (member_index = 0; member_index < out->member_count; member_index++)
      mismatch = mismatch || out->members[member_index].missing.count != 0;
    if (mismatch) {
      size_t *order = (size_t *)ab_malloc(state->engine,
                                          out->member_count * sizeof(*order));
      size_t index;
      if (!order)
        status = archbird_error_set(
            state->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
            "out of memory rendering parity diagnostic");
      for (index = 0; status == ARCHBIRD_OK && index < out->member_count;
           index++)
        order[index] = index;
      for (index = 1; status == ARCHBIRD_OK && index < out->member_count;
           index++) {
        size_t key = order[index];
        size_t cursor = index;
        while (cursor &&
               ab_string_compare(&out->members[order[cursor - 1]].label,
                                 &out->members[key].label) > 0) {
          order[cursor] = order[cursor - 1];
          cursor--;
        }
        order[cursor] = key;
      }
      ab_buffer_init(&message, state->engine);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&message, out->name.data, out->name.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&message, ": ");
      for (index = 0; status == ARCHBIRD_OK && index < out->member_count;
           index++) {
        AbMapParityMember *member = &out->members[order[index]];
        if (index)
          status = ab_buffer_literal(&message, ", ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&message, member->label.data,
                                    member->label.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&message, " missing ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_u64(&message, member->missing.count);
      }
      if (status == ARCHBIRD_OK)
        status = ab_map_add_diagnostic(state, "error", "parity-mismatch",
                                       (const char *)message.data, NULL);
      ab_buffer_free(&message);
      ab_free(state->engine, order);
    }
  }
  free_temporary_strings(state->engine, &ignored);
  free_temporary_strings(state->engine, &union_values);
  return status;
}

ArchbirdStatus ab_map_analyze_parity(AbMapState *state) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (state->config->parity_count) {
    state->parity = (AbMapParity *)ab_calloc(
        state->engine, state->config->parity_count, sizeof(*state->parity));
    if (!state->parity)
      return archbird_error_set(state->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory collecting parity results");
  }
  for (index = 0; status == ARCHBIRD_OK && index < state->config->parity_count;
       index++) {
    state->parity_count++;
    status = analyze_parity(state, &state->config->parity[index],
                            &state->parity[index]);
  }
  return status;
}

static ArchbirdStatus render_strings(AbBuffer *buffer,
                                     const AbStringArray *values) {
  size_t index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (index = 0; status == ARCHBIRD_OK && index < values->count; index++) {
    if (index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(buffer, values->items[index].data,
                                     values->items[index].length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}

ArchbirdStatus ab_map_render_parity(AbBuffer *buffer, const AbMapState *state) {
  size_t parity_index;
  ArchbirdStatus status = ab_buffer_literal(buffer, "[");
  for (parity_index = 0;
       status == ARCHBIRD_OK && parity_index < state->parity_count;
       parity_index++) {
    const AbMapParity *parity = &state->parity[parity_index];
    size_t member_index;
    if (parity_index)
      status = ab_buffer_literal(buffer, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "{\"enforce\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, parity->enforce ? "true" : "false");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"members\":[");
    for (member_index = 0;
         status == ARCHBIRD_OK && member_index < parity->member_count;
         member_index++) {
      const AbMapParityMember *member = &parity->members[member_index];
      size_t evidence_index;
      if (member_index)
        status = ab_buffer_literal(buffer, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "{\"evidence\":{");
      for (evidence_index = 0;
           status == ARCHBIRD_OK && evidence_index < member->evidence_count;
           evidence_index++) {
        const AbMapParityEvidence *evidence = &member->evidence[evidence_index];
        if (evidence_index)
          status = ab_buffer_literal(buffer, ",");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_json_string(buffer, evidence->name.data,
                                         evidence->name.length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(buffer, ":");
        if (status == ARCHBIRD_OK)
          status = render_strings(buffer, &evidence->locations);
      }
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "},\"label\":");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_json_string(buffer, member->label.data,
                                       member->label.length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"missing\":");
      if (status == ARCHBIRD_OK)
        status = render_strings(buffer, &member->missing);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, ",\"values\":");
      if (status == ARCHBIRD_OK)
        status = render_strings(buffer, &member->values);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(buffer, "}");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "],\"name\":");
    if (status == ARCHBIRD_OK)
      status =
          ab_buffer_json_string(buffer, parity->name.data, parity->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, ",\"shared\":");
    if (status == ARCHBIRD_OK)
      status = render_strings(buffer, &parity->shared);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(buffer, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(buffer, "]");
  return status;
}
