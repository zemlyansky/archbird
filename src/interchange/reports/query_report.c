#include "map_reports.h"

#include "archbird_internal.h"
#include "report_utils.h"

#include <stdlib.h>
#include <string.h>

#define QUERY_REPORT_WIDTH 116u

#define QUERY_TRY(expression)                                                  \
  do {                                                                         \
    status = (expression);                                                     \
    if (status != ARCHBIRD_OK)                                                 \
      goto cleanup;                                                            \
  } while (0)

static const char *const query_resolution_kinds[] = {
    "unique", "candidate", "ambiguous", "unresolved",
    "method", "builtin",   "external"};

typedef struct QueryReportFile {
  const AbValue *row;
  const AbString *path;
  const AbString *layer;
  const AbString *language;
  const AbValue *symbols;
  size_t distance;
  AbReportStringList incoming;
} QueryReportFile;

typedef struct QueryReportSymbol {
  const AbValue *row;
  const AbString *name;
  size_t line;
  size_t priority;
  int incoming;
} QueryReportSymbol;

typedef struct QueryReportTest {
  const AbValue *row;
  const AbString *evidence;
  const AbString *classification;
  const AbString *provenance;
  const AbString *confidence;
  const AbString *evidence_scope;
  const AbString *target_role;
  const AbValue *target;
  const AbString *group;
  const AbString *path;
  const AbString *selector;
  size_t line;
  size_t route_count;
  size_t seed_distance;
  size_t symbol_distance;
  size_t ranking_affinity;
} QueryReportTest;

typedef struct QueryCollapsedTestGroup {
  const AbString *provenance;
  const AbString *confidence;
  const AbString *evidence_scope;
  const AbString *target_role;
  const AbString *target_path;
  const AbString *target_symbol;
  const AbString *sample_path;
  const AbString *sample_selector;
  size_t sample_line;
  size_t seed_distance;
  size_t symbol_distance;
  size_t count;
} QueryCollapsedTestGroup;

typedef struct QueryContextPolicy {
  const AbString *profile;
  const AbValue *provenance;
  const AbValue *confidence;
  size_t max_seed_distance;
  const AbString *candidate;
  const AbString *conservative;
  size_t file_quota;
  size_t symbol_call_quota;
  size_t symbol_reference_quota;
  size_t test_match_quota;
  size_t file_offset;
  size_t symbol_call_offset;
  size_t symbol_reference_offset;
  size_t test_match_offset;
} QueryContextPolicy;

static ArchbirdStatus query_schema_error(ArchbirdEngine *engine,
                                         const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static ArchbirdStatus render_retrieval_summary(ArchbirdEngine *engine,
                                               const AbValue *retrieval,
                                               int compact, AbBuffer *out) {
  const AbString *contract = ab_report_string(retrieval, "contract");
  const AbString *confidence = ab_report_string(retrieval, "confidence");
  const AbValue *hits = ab_report_array(retrieval, "hits");
  size_t matched = ab_report_size(retrieval, "matched_count", SIZE_MAX);
  size_t candidates = ab_report_size(retrieval, "candidate_count", SIZE_MAX);
  size_t limit = ab_report_size(retrieval, "limit", SIZE_MAX);
  size_t shown;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!contract || !confidence || !hits || matched == SIZE_MAX ||
      candidates == SIZE_MAX || limit == SIZE_MAX ||
      !ab_report_string_equal(confidence, "candidate"))
    return query_schema_error(engine, "query retrieval evidence is malformed");
  QUERY_TRY(ab_report_literal_line(out, "## Candidate seeds"));
  QUERY_TRY(ab_report_blank(out));
  QUERY_TRY(ab_report_linef(
      out,
      "Deterministic `%.*s` ranking; selected=%zu (limit=%zu), matched=%zu, "
      "indexed=%zu. "
      "These seeds are advisory; graph routes retain their own evidence class.",
      (int)contract->length, contract->data, hits->as.array.count, limit,
      matched, candidates));
  QUERY_TRY(ab_report_blank(out));
  QUERY_TRY(ab_report_literal_line(out, "```text"));
  shown = compact && hits->as.array.count > 5 ? 5 : hits->as.array.count;
  for (index = 0; index < shown; index++) {
    const AbValue *hit = &hits->as.array.items[index];
    const AbString *kind = ab_report_string(hit, "kind");
    const AbString *path = ab_report_string(hit, "path");
    const AbString *name = ab_report_string(hit, "name");
    const AbValue *reasons = ab_report_array(hit, "reasons");
    size_t score = ab_report_size(hit, "score", SIZE_MAX);
    const AbString *field;
    const AbString *match;
    const AbString *term;
    if (!kind || (!path && !name) || !reasons || !reasons->as.array.count ||
        score == SIZE_MAX)
      return query_schema_error(engine, "query retrieval hit is malformed");
    field = ab_report_string(&reasons->as.array.items[0], "field");
    match = ab_report_string(&reasons->as.array.items[0], "match");
    term = ab_report_string(&reasons->as.array.items[0], "term");
    if (!field || !match || !term)
      return query_schema_error(engine, "query retrieval reason is malformed");
    QUERY_TRY(ab_report_appendf(out, "%zu. %.*s ", index + 1, (int)kind->length,
                                kind->data));
    if (path)
      QUERY_TRY(ab_buffer_append(out, path->data, path->length));
    if (name && (!path || !ab_string_equal(path, name))) {
      if (path)
        QUERY_TRY(ab_buffer_literal(out, ":"));
      QUERY_TRY(ab_buffer_append(out, name->data, name->length));
    }
    QUERY_TRY(ab_report_appendf(out, " score=%zu [%.*s:%.*s term=%.*s]", score,
                                (int)field->length, field->data,
                                (int)match->length, match->data,
                                (int)term->length, term->data));
    QUERY_TRY(ab_buffer_literal(out, "\n"));
  }
  if (hits->as.array.count > shown)
    QUERY_TRY(ab_report_linef(out, "… %zu candidate seeds omitted by detail",
                              hits->as.array.count - shown));
  QUERY_TRY(ab_report_literal_line(out, "```"));
  QUERY_TRY(ab_report_blank(out));
cleanup:
  return status;
}

static const AbValue *optional_array(const AbValue *object, const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_ARRAY};
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_ARRAY ? value : &empty;
}

static const AbValue *optional_object(const AbValue *object, const char *name) {
  static const AbValue empty = {.kind = AB_VALUE_OBJECT};
  const AbValue *value = ab_value_member(object, name);
  return value && value->kind == AB_VALUE_OBJECT ? value : &empty;
}

static const AbString *string_or_empty(const AbValue *object,
                                       const char *name) {
  static char empty_data[] = "";
  static const AbString empty = {empty_data, 0};
  const AbValue *value = ab_value_member(object, name);
  if (!value)
    return &empty;
  return value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

static int string_in_array(const AbValue *array, const AbString *value) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY || !value)
    return 0;
  for (index = 0; index < array->as.array.count; index++) {
    const AbValue *item = &array->as.array.items[index];
    if (item->kind == AB_VALUE_STRING && ab_string_equal(&item->as.text, value))
      return 1;
  }
  return 0;
}

static ArchbirdStatus context_count(ArchbirdEngine *engine,
                                    const AbValue *object, const char *field,
                                    size_t fallback, size_t *out) {
  const AbValue *value = object ? ab_value_member(object, field) : NULL;
  uint64_t count;
  if (!value) {
    *out = fallback;
    return ARCHBIRD_OK;
  }
  if (!ab_value_u64(value, &count) || count > SIZE_MAX)
    return query_schema_error(engine,
                              "query context count is not a valid size");
  *out = (size_t)count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus query_context_policy(ArchbirdEngine *engine,
                                           const AbValue *metadata,
                                           QueryContextPolicy *out) {
  static char change_data[] = "change";
  static const AbString change = {change_data, 6};
  static char collapse_data[] = "collapse";
  static const AbString collapse = {collapse_data, 8};
  static char expand_data[] = "expand";
  static const AbString expand = {expand_data, 6};
  static char exclude_data[] = "exclude";
  static const AbString exclude = {exclude_data, 7};
  const AbValue *context = ab_value_member(metadata, "context");
  const AbValue *quotas;
  const AbValue *offsets;
  const AbValue *value;
  size_t default_quota;
  memset(out, 0, sizeof(*out));
  if (!context) {
    static const AbValue empty = {.kind = AB_VALUE_OBJECT};
    context = &empty;
  }
  if (context->kind != AB_VALUE_OBJECT)
    return query_schema_error(engine, "query.query.context must be an object");
  value = ab_value_member(context, "profile");
  out->profile =
      value && value->kind == AB_VALUE_STRING ? &value->as.text : &change;
  if (!ab_report_string_equal(out->profile, "exact") &&
      !ab_report_string_equal(out->profile, "change") &&
      !ab_report_string_equal(out->profile, "architecture") &&
      !ab_report_string_equal(out->profile, "audit"))
    return query_schema_error(engine, "query context profile is invalid");
  out->provenance = ab_value_member(context, "provenance");
  out->confidence = ab_value_member(context, "confidence");
  if ((out->provenance && out->provenance->kind != AB_VALUE_ARRAY) ||
      (out->confidence && out->confidence->kind != AB_VALUE_ARRAY))
    return query_schema_error(engine,
                              "query context evidence filters are invalid");
  if (ab_report_string_equal(out->profile, "exact")) {
    out->max_seed_distance = 0;
    out->candidate = &exclude;
    out->conservative = &exclude;
    default_quota = 12;
  } else if (ab_report_string_equal(out->profile, "change")) {
    out->max_seed_distance = 1;
    out->candidate = &expand;
    out->conservative = &collapse;
    default_quota = 24;
  } else if (ab_report_string_equal(out->profile, "architecture")) {
    out->max_seed_distance = SIZE_MAX;
    out->candidate = &expand;
    out->conservative = &collapse;
    default_quota = 64;
  } else {
    out->max_seed_distance = SIZE_MAX;
    out->candidate = &expand;
    out->conservative = &expand;
    default_quota = SIZE_MAX;
  }
  value = ab_value_member(context, "max_seed_distance");
  if (value) {
    uint64_t distance;
    if (!ab_value_u64(value, &distance) || distance > SIZE_MAX)
      return query_schema_error(engine,
                                "query context seed distance is invalid");
    out->max_seed_distance = (size_t)distance;
  }
  value = ab_value_member(context, "conservative");
  if (value) {
    if (value->kind != AB_VALUE_STRING)
      return query_schema_error(engine, "query conservative policy is invalid");
    out->conservative = &value->as.text;
  }
  value = ab_value_member(context, "candidate");
  if (value) {
    if (value->kind != AB_VALUE_STRING)
      return query_schema_error(engine, "query candidate policy is invalid");
    out->candidate = &value->as.text;
  }
  if (!ab_report_string_equal(out->candidate, "collapse") &&
      !ab_report_string_equal(out->candidate, "expand") &&
      !ab_report_string_equal(out->candidate, "exclude"))
    return query_schema_error(engine, "query candidate policy is invalid");
  if (!ab_report_string_equal(out->conservative, "collapse") &&
      !ab_report_string_equal(out->conservative, "expand") &&
      !ab_report_string_equal(out->conservative, "exclude"))
    return query_schema_error(engine, "query conservative policy is invalid");
  quotas = ab_value_member(context, "quotas");
  offsets = ab_value_member(context, "offsets");
  if ((quotas && quotas->kind != AB_VALUE_OBJECT) ||
      (offsets && offsets->kind != AB_VALUE_OBJECT))
    return query_schema_error(engine, "query context counts are invalid");
  {
    ArchbirdStatus status =
        context_count(engine, quotas, "files", default_quota, &out->file_quota);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, quotas, "symbol_calls", default_quota,
                             &out->symbol_call_quota);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, quotas, "symbol_references", default_quota,
                             &out->symbol_reference_quota);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, quotas, "test_matches", default_quota,
                             &out->test_match_quota);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, offsets, "files", 0, &out->file_offset);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, offsets, "symbol_calls", 0,
                             &out->symbol_call_offset);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, offsets, "symbol_references", 0,
                             &out->symbol_reference_offset);
    if (status == ARCHBIRD_OK)
      status = context_count(engine, offsets, "test_matches", 0,
                             &out->test_match_offset);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int policy_evidence_allowed(const QueryContextPolicy *policy,
                                   const AbString *provenance,
                                   const AbString *confidence) {
  if (!provenance || !confidence)
    return 1;
  if (policy->provenance && !string_in_array(policy->provenance, provenance))
    return 0;
  if (policy->confidence)
    return string_in_array(policy->confidence, confidence);
  if (ab_report_string_equal(policy->profile, "exact"))
    return ab_report_string_equal(confidence, "exact");
  return 1;
}

static ArchbirdStatus
query_test_selection_stats(ArchbirdEngine *engine, const AbValue *matches,
                           const QueryContextPolicy *policy, size_t *eligible,
                           size_t *candidate_collapsed,
                           size_t *conservative_collapsed, size_t *excluded) {
  size_t index;
  *eligible = 0;
  *candidate_collapsed = 0;
  *conservative_collapsed = 0;
  *excluded = 0;
  for (index = 0; index < matches->as.array.count; index++) {
    const AbValue *row = &matches->as.array.items[index];
    const AbString *provenance = ab_report_string(row, "provenance");
    const AbString *confidence = ab_report_string(row, "confidence");
    const AbString *evidence_scope = ab_report_string(row, "evidence_scope");
    const AbString *target_role = ab_report_string(row, "target_role");
    const AbValue *target = ab_value_member(row, "target");
    size_t seed_distance = ab_report_size(row, "seed_distance", SIZE_MAX);
    if (provenance || confidence || evidence_scope || target_role || target) {
      if (!provenance || !confidence || !evidence_scope || !target_role ||
          !target)
        return query_schema_error(
            engine, "query test route axes are partially specified");
      if ((!ab_report_string_equal(provenance, "derived") &&
           !ab_report_string_equal(provenance, "asserted") &&
           !ab_report_string_equal(provenance, "observed")) ||
          (!ab_report_string_equal(confidence, "exact") &&
           !ab_report_string_equal(confidence, "candidate") &&
           !ab_report_string_equal(confidence, "conservative") &&
           !ab_report_string_equal(confidence, "unresolved")) ||
          (!ab_report_string_equal(evidence_scope, "case") &&
           !ab_report_string_equal(evidence_scope, "file") &&
           !ab_report_string_equal(evidence_scope, "unresolved")))
        return query_schema_error(engine, "query test route axes are invalid");
      if (!policy_evidence_allowed(policy, provenance, confidence) ||
          (seed_distance != SIZE_MAX &&
           seed_distance > policy->max_seed_distance)) {
        (*excluded)++;
        continue;
      }
      if (ab_report_string_equal(confidence, "candidate") &&
          !ab_report_string_equal(policy->candidate, "expand")) {
        if (ab_report_string_equal(policy->candidate, "collapse"))
          (*candidate_collapsed)++;
        else
          (*excluded)++;
        continue;
      }
      if (ab_report_string_equal(confidence, "conservative") &&
          !ab_report_string_equal(policy->conservative, "expand")) {
        if (ab_report_string_equal(policy->conservative, "collapse"))
          (*conservative_collapsed)++;
        else
          (*excluded)++;
        continue;
      }
    }
    (*eligible)++;
  }
  return ARCHBIRD_OK;
}

static size_t file_index(const QueryReportFile *files, size_t file_count,
                         const AbString *path) {
  size_t index;
  for (index = 0; index < file_count; index++)
    if (ab_string_equal(files[index].path, path))
      return index;
  return SIZE_MAX;
}

static int path_is_mapped(const AbValue *map_files, const AbString *path) {
  size_t index;
  for (index = 0; index < map_files->as.array.count; index++) {
    const AbString *candidate =
        ab_report_string(&map_files->as.array.items[index], "path");
    if (candidate && ab_string_equal(candidate, path))
      return 1;
  }
  return 0;
}

static int nullable_string_equal(const AbString *left, const AbString *right) {
  if (!left || !right)
    return left == right;
  return ab_string_equal(left, right);
}

static ArchbirdStatus
collapsed_test_group_add(ArchbirdEngine *engine,
                         QueryCollapsedTestGroup *groups, size_t *group_count,
                         const QueryReportTest *candidate) {
  const AbValue *target =
      candidate->target && candidate->target->kind == AB_VALUE_OBJECT
          ? candidate->target
          : NULL;
  const AbString *target_path =
      target ? ab_report_string(target, "path") : NULL;
  const AbString *target_symbol =
      target ? ab_report_string(target, "symbol") : NULL;
  size_t index;
  (void)engine;
  for (index = 0; index < *group_count; index++) {
    QueryCollapsedTestGroup *group = &groups[index];
    if (nullable_string_equal(group->provenance, candidate->provenance) &&
        nullable_string_equal(group->confidence, candidate->confidence) &&
        nullable_string_equal(group->evidence_scope,
                              candidate->evidence_scope) &&
        nullable_string_equal(group->target_role, candidate->target_role) &&
        nullable_string_equal(group->target_path, target_path) &&
        nullable_string_equal(group->target_symbol, target_symbol) &&
        group->seed_distance == candidate->seed_distance &&
        group->symbol_distance == candidate->symbol_distance) {
      group->count++;
      return ARCHBIRD_OK;
    }
  }
  groups[*group_count] = (QueryCollapsedTestGroup){
      .provenance = candidate->provenance,
      .confidence = candidate->confidence,
      .evidence_scope = candidate->evidence_scope,
      .target_role = candidate->target_role,
      .target_path = target_path,
      .target_symbol = target_symbol,
      .sample_path = candidate->path,
      .sample_selector = candidate->selector,
      .sample_line = candidate->line,
      .seed_distance = candidate->seed_distance,
      .symbol_distance = candidate->symbol_distance,
      .count = 1,
  };
  (*group_count)++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus
collapsed_test_group_render(AbReportStringList *rows,
                            const QueryCollapsedTestGroup *group) {
  AbBuffer text;
  ArchbirdStatus status;
  ab_buffer_init(&text, rows->engine);
  status = ab_report_appendf(
      &text,
      "… %zu %.*s routes collapsed [provenance=%.*s; scope=%.*s; "
      "target-role=%.*s; seed-distance=",
      group->count, (int)group->confidence->length, group->confidence->data,
      (int)group->provenance->length, group->provenance->data,
      (int)group->evidence_scope->length, group->evidence_scope->data,
      (int)group->target_role->length, group->target_role->data);
  if (status == ARCHBIRD_OK && group->seed_distance == SIZE_MAX)
    status = ab_buffer_literal(&text, "unresolved");
  else if (status == ARCHBIRD_OK)
    status = ab_report_appendf(&text, "%zu", group->seed_distance);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&text, "; symbol-distance=");
  if (status == ARCHBIRD_OK && group->symbol_distance == SIZE_MAX)
    status = ab_buffer_literal(&text, "unresolved");
  else if (status == ARCHBIRD_OK)
    status = ab_report_appendf(&text, "%zu", group->symbol_distance);
  if (status == ARCHBIRD_OK && group->target_path) {
    status = ab_report_appendf(&text, "; target=%.*s",
                               (int)group->target_path->length,
                               group->target_path->data);
    if (status == ARCHBIRD_OK && group->target_symbol)
      status =
          ab_report_appendf(&text, ":%.*s", (int)group->target_symbol->length,
                            group->target_symbol->data);
  }
  if (status == ARCHBIRD_OK)
    status = ab_report_appendf(
        &text, "; top=%.*s:%zu:%.*s]", (int)group->sample_path->length,
        group->sample_path->data, group->sample_line,
        (int)group->sample_selector->length, group->sample_selector->data);
  if (status == ARCHBIRD_OK)
    status = ab_report_list_add(rows, (const char *)text.data, text.length);
  ab_buffer_free(&text);
  return status;
}

static int index_visible(size_t index, size_t start, size_t count) {
  return index >= start && index - start < count;
}

static int symbol_incoming(const QueryReportFile *file, const AbString *name) {
  const char *leaf = name->data;
  size_t leaf_length = name->length;
  size_t index;
  for (index = 0; index < name->length; index++)
    if (name->data[index] == '.') {
      leaf = name->data + index + 1;
      leaf_length = name->length - index - 1;
    }
  for (index = 0; index < file->incoming.count; index++)
    if (file->incoming.items[index].length == leaf_length &&
        (!leaf_length ||
         memcmp(file->incoming.items[index].data, leaf, leaf_length) == 0))
      return 1;
  return 0;
}

static ArchbirdStatus
render_symbol_relation_routes(ArchbirdEngine *engine, const AbValue *query,
                              const char *field, const char *label,
                              int value_reference, size_t offset, size_t limit,
                              AbBuffer *out) {
  const AbValue *calls = optional_array(query, field);
  size_t counts[sizeof(query_resolution_kinds) /
                sizeof(query_resolution_kinds[0])] = {0};
  size_t index;
  size_t available;
  size_t rendered;
  AbReportStringList rows;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!calls->as.array.count)
    return ARCHBIRD_OK;
  if (offset > calls->as.array.count)
    offset = calls->as.array.count;
  available = calls->as.array.count - offset;
  rendered = available < limit ? available : limit;
  ab_report_list_init(&rows, engine);
  for (index = 0; index < calls->as.array.count; index++) {
    const AbValue *row = &calls->as.array.items[index];
    const AbString *resolution = ab_report_string(row, "resolution");
    size_t kind;
    if (row->kind != AB_VALUE_OBJECT || !resolution) {
      status = query_schema_error(engine, "query symbol relation is malformed");
      goto cleanup;
    }
    for (kind = 0; kind < sizeof(query_resolution_kinds) /
                              sizeof(query_resolution_kinds[0]);
         kind++)
      if (ab_report_string_equal(resolution, query_resolution_kinds[kind])) {
        counts[kind]++;
        break;
      }
    if (kind ==
        sizeof(query_resolution_kinds) / sizeof(query_resolution_kinds[0])) {
      status = query_schema_error(engine, "query symbol resolution is invalid");
      goto cleanup;
    }
  }
  status = ab_buffer_literal(out, label);
  for (index = 0;
       status == ARCHBIRD_OK && index < sizeof(query_resolution_kinds) /
                                            sizeof(query_resolution_kinds[0]);
       index++)
    status = ab_report_appendf(out, " %s=%zu", query_resolution_kinds[index],
                               counts[index]);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "\n");
  if (status == ARCHBIRD_OK && offset)
    status = ab_report_list_addf(&rows, "… %zu earlier rows skipped", offset);
  for (index = offset; status == ARCHBIRD_OK && index < offset + rendered;
       index++) {
    const AbValue *row = &calls->as.array.items[index];
    const AbValue *source = ab_report_object(row, "source");
    const AbValue *candidates = ab_report_array(row, "candidates");
    const AbValue *evidence = ab_report_array(row, "evidence");
    const AbString *source_path =
        source ? ab_report_string(source, "path") : NULL;
    const AbString *source_symbol =
        source ? ab_report_string(source, "symbol") : NULL;
    const AbString *source_scope =
        source ? string_or_empty(source, "scope") : NULL;
    const AbString *name = ab_report_string(row, "name");
    const AbString *resolution = ab_report_string(row, "resolution");
    const AbString *context = string_or_empty(row, "context");
    const AbString *container = string_or_empty(row, "container");
    AbBuffer text;
    size_t nested;
    size_t citation_limit;
    if (!source_path || !source_scope || !name || !resolution || !context ||
        !container || !candidates || !evidence) {
      status = query_schema_error(engine, "query symbol relation is malformed");
      goto cleanup;
    }
    ab_buffer_init(&text, engine);
    if (value_reference && source_symbol)
      status = ab_report_appendf(
          &text,
          "%.*s:%.*s --value:%.*s [%.*s; context=%.*s; container=%.*s; "
          "evidence=%zu",
          (int)source_path->length, source_path->data,
          (int)source_symbol->length, source_symbol->data, (int)name->length,
          name->data, (int)resolution->length, resolution->data,
          (int)context->length, context->data, (int)container->length,
          container->data, evidence->as.array.count);
    else if (value_reference)
      status = ab_report_appendf(
          &text,
          "%.*s [%.*s] --value:%.*s [%.*s; context=%.*s; container=%.*s; "
          "evidence=%zu",
          (int)source_path->length, source_path->data,
          (int)source_scope->length, source_scope->data, (int)name->length,
          name->data, (int)resolution->length, resolution->data,
          (int)context->length, context->data, (int)container->length,
          container->data, evidence->as.array.count);
    else if (source_symbol)
      status = ab_report_appendf(&text, "%.*s:%.*s --%.*s [%.*s; evidence=%zu",
                                 (int)source_path->length, source_path->data,
                                 (int)source_symbol->length,
                                 source_symbol->data, (int)name->length,
                                 name->data, (int)resolution->length,
                                 resolution->data, evidence->as.array.count);
    else
      status = ab_report_appendf(
          &text, "%.*s [%.*s] --%.*s [%.*s; evidence=%zu",
          (int)source_path->length, source_path->data,
          (int)source_scope->length, source_scope->data, (int)name->length,
          name->data, (int)resolution->length, resolution->data,
          evidence->as.array.count);
    citation_limit =
        evidence->as.array.count < 4 ? evidence->as.array.count : 4;
    if (status == ARCHBIRD_OK && citation_limit)
      status = ab_buffer_literal(&text, "; at=");
    for (nested = 0; status == ARCHBIRD_OK && nested < citation_limit;
         nested++) {
      const AbValue *citation = &evidence->as.array.items[nested];
      const AbString *provider = ab_report_string(citation, "provider");
      const AbValue *span = ab_report_object(citation, "span");
      size_t line = ab_report_size(citation, "line", 0);
      size_t span_start =
          span ? ab_report_size(span, "start", SIZE_MAX) : SIZE_MAX;
      if (!provider || (!line && span_start == SIZE_MAX)) {
        status = query_schema_error(
            engine, "query symbol relation evidence is malformed");
        break;
      }
      if (nested)
        status = ab_buffer_literal(&text, ",");
      if (status == ARCHBIRD_OK && line)
        status = ab_report_appendf(&text, "%.*s@%zu", (int)provider->length,
                                   provider->data, line);
      else if (status == ARCHBIRD_OK)
        status =
            ab_report_appendf(&text, "%.*s@byte+%zu", (int)provider->length,
                              provider->data, span_start);
    }
    if (status == ARCHBIRD_OK && evidence->as.array.count > citation_limit)
      status = ab_report_appendf(&text, ",…+%zu",
                                 evidence->as.array.count - citation_limit);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&text, "] -> ");
    if (status == ARCHBIRD_OK && !candidates->as.array.count)
      status = ab_buffer_literal(&text, "none");
    for (nested = 0; status == ARCHBIRD_OK &&
                     nested < candidates->as.array.count && nested < 4;
         nested++) {
      const AbValue *candidate = &candidates->as.array.items[nested];
      const AbString *path = ab_report_string(candidate, "path");
      const AbString *symbol = ab_report_string(candidate, "symbol");
      if (!path || !symbol) {
        status = query_schema_error(
            engine, "query symbol relation candidate is malformed");
        break;
      }
      if (nested)
        status = ab_buffer_literal(&text, ",");
      if (status == ARCHBIRD_OK)
        status =
            ab_report_appendf(&text, "%.*s:%.*s", (int)path->length, path->data,
                              (int)symbol->length, symbol->data);
    }
    if (status == ARCHBIRD_OK && candidates->as.array.count > 4)
      status =
          ab_report_appendf(&text, ",…+%zu", candidates->as.array.count - 4);
    if (status == ARCHBIRD_OK)
      status = ab_report_list_add(&rows, (const char *)text.data, text.length);
    ab_buffer_free(&text);
  }
  if (status == ARCHBIRD_OK && calls->as.array.count > offset + rendered)
    status = ab_report_list_addf(&rows, "…+%zu",
                                 calls->as.array.count - offset - rendered);
  if (status == ARCHBIRD_OK)
    status = ab_report_chunks(out, &rows, "  ", QUERY_REPORT_WIDTH);

cleanup:
  ab_report_list_free(&rows);
  return status;
}

static size_t symbol_priority(const AbString *scope) {
  if (ab_report_string_equal(scope, "public"))
    return 0;
  if (ab_report_string_equal(scope, "global") ||
      ab_report_string_equal(scope, "class"))
    return 1;
  if (ab_report_string_equal(scope, "function"))
    return 2;
  if (ab_report_string_equal(scope, "method"))
    return 3;
  if (ab_report_string_equal(scope, "local"))
    return 4;
  return 5;
}

static int symbol_compare(const void *left_raw, const void *right_raw) {
  const QueryReportSymbol *left = (const QueryReportSymbol *)left_raw;
  const QueryReportSymbol *right = (const QueryReportSymbol *)right_raw;
  int compared;
  if (left->incoming != right->incoming)
    return left->incoming ? -1 : 1;
  if (left->priority != right->priority)
    return left->priority < right->priority ? -1 : 1;
  compared = ab_string_compare(left->name, right->name);
  if (compared)
    return compared;
  if (left->line != right->line)
    return left->line < right->line ? -1 : 1;
  return 0;
}

static ArchbirdStatus symbol_items(ArchbirdEngine *engine,
                                   const QueryReportFile *file, size_t limit,
                                   AbReportStringList *out) {
  QueryReportSymbol *ranked = NULL;
  size_t count = file->symbols->as.array.count;
  size_t index;
  size_t selected;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (count) {
    ranked = (QueryReportSymbol *)ab_calloc(engine, count, sizeof(*ranked));
    if (!ranked)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory ranking report symbols");
  }
  for (index = 0; index < count; index++) {
    const AbValue *row = &file->symbols->as.array.items[index];
    const AbString *name = ab_report_string(row, "name");
    const AbString *scope = string_or_empty(row, "scope");
    if (row->kind != AB_VALUE_OBJECT || !name || !scope) {
      status =
          query_schema_error(engine, "query.files[].symbols[] is malformed");
      goto cleanup;
    }
    ranked[index].row = row;
    ranked[index].name = name;
    ranked[index].line = ab_report_size(row, "line", 0);
    ranked[index].priority = symbol_priority(scope);
    ranked[index].incoming = symbol_incoming(file, name);
  }
  if (count > 1)
    qsort(ranked, count, sizeof(*ranked), symbol_compare);
  selected = count < limit ? count : limit;
  for (index = 0; status == ARCHBIRD_OK && index < selected; index++)
    status =
        ab_report_list_addf(out, "%.*s@%zu", (int)ranked[index].name->length,
                            ranked[index].name->data, ranked[index].line);
  if (status == ARCHBIRD_OK && count > selected)
    status = ab_report_list_addf(out, "…+%zu", count - selected);
cleanup:
  ab_free(engine, ranked);
  return status;
}

static ArchbirdStatus render_tool_label(AbBuffer *buffer, const AbValue *tool) {
  const AbString *name = ab_report_string(tool, "name");
  const AbString *version = ab_report_string(tool, "version");
  const AbString *digest = ab_report_string(tool, "implementation_sha256");
  if (!name || !version || !digest || digest->length < 16)
    return query_schema_error(buffer->engine,
                              "report tool identity is malformed");
  return ab_report_appendf(buffer, "%.*s %.*s `%.*s`", (int)name->length,
                           name->data, (int)version->length, version->data, 16,
                           digest->data);
}

static size_t surface_summary(const AbValue *surface, const char *field) {
  const AbValue *summary = optional_object(surface, "summary");
  const AbValue *names;
  size_t index;
  size_t count = 0;
  const AbString *wanted;
  if (summary->as.object.count)
    return ab_report_size(summary, field, 0);
  names = optional_array(surface, "names");
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *row = &names->as.array.items[index];
    int ignored = ab_report_bool(row, "ignored", 0);
    const AbString *declaration = ab_report_string(row, "declaration");
    const AbString *resolution = ab_report_string(row, "resolution");
    const AbValue *uses = optional_array(row, "uses");
    if (ignored)
      continue;
    if (strcmp(field, "registered") == 0 &&
        ab_report_string_equal(declaration, "declared"))
      count++;
    else if (strcmp(field, "used") == 0 && uses->as.array.count)
      count++;
    else if (strcmp(field, "unused") == 0 &&
             ab_report_string_equal(declaration, "declared") &&
             !uses->as.array.count)
      count++;
    else if (strcmp(field, "unregistered_use") == 0 &&
             ab_report_string_equal(declaration, "undeclared") &&
             uses->as.array.count)
      count++;
    else {
      wanted = resolution;
      if (wanted && ab_report_string_equal(wanted, field))
        count++;
    }
  }
  return count;
}

static ArchbirdStatus render_query_artifact(ArchbirdEngine *engine,
                                            const AbValue *artifact,
                                            size_t compact_edges,
                                            AbBuffer *out) {
  const AbString *name = ab_report_string(artifact, "name");
  const AbString *output = ab_report_string(artifact, "output");
  const AbValue *artifact_inputs = optional_array(artifact, "inputs");
  const AbValue *loaders = optional_array(artifact, "loaded_by");
  const AbValue *builds = optional_array(artifact, "builds");
  const AbValue *depends = optional_array(artifact, "depends_on");
  AbReportStringList input_paths;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&input_paths, engine);
  if (!name || !output) {
    status = query_schema_error(engine, "query.artifacts[] is malformed");
    goto cleanup;
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < artifact_inputs->as.array.count;
       index++) {
    const AbString *path =
        ab_report_string(&artifact_inputs->as.array.items[index], "path");
    if (!path)
      status = query_schema_error(engine, "query artifact input is malformed");
    else
      status = ab_report_list_add(&input_paths, path->data, path->length);
  }
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(&input_paths);
  if (status == ARCHBIRD_OK)
    status =
        ab_report_appendf(out, "artifact %.*s: %.*s inputs=", (int)name->length,
                          name->data, (int)output->length, output->data);
  if (status == ARCHBIRD_OK && !input_paths.count)
    status = ab_buffer_literal(out, "-");
  else if (status == ARCHBIRD_OK) {
    size_t shown =
        input_paths.count < compact_edges ? input_paths.count : compact_edges;
    for (index = 0; status == ARCHBIRD_OK && index < shown; index++) {
      if (index)
        status = ab_buffer_literal(out, ",");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(out, input_paths.items[index].data,
                                  input_paths.items[index].length);
    }
    if (status == ARCHBIRD_OK && input_paths.count > shown)
      status = ab_report_appendf(out, ",…+%zu", input_paths.count - shown);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, " loaded_by=");
  if (status == ARCHBIRD_OK && !loaders->as.array.count)
    status = ab_buffer_literal(out, "-");
  for (index = 0; status == ARCHBIRD_OK && index < loaders->as.array.count;
       index++) {
    const AbString *path =
        ab_report_string(&loaders->as.array.items[index], "path");
    if (!path) {
      status = query_schema_error(engine, "query artifact loader is malformed");
      break;
    }
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(out, path->data, path->length);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, " builds=");
  if (status == ARCHBIRD_OK && !builds->as.array.count)
    status = ab_buffer_literal(out, "-");
  for (index = 0; status == ARCHBIRD_OK && index < builds->as.array.count;
       index++) {
    const AbValue *build = &builds->as.array.items[index];
    const AbString *source = ab_report_string(build, "source");
    const AbString *target = ab_report_string(build, "target");
    if (!source || !target) {
      status = query_schema_error(engine, "query artifact build is malformed");
      break;
    }
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status =
          ab_report_appendf(out, "%.*s:%.*s", (int)source->length, source->data,
                            (int)target->length, target->data);
  }
  if (status == ARCHBIRD_OK && depends->as.array.count) {
    status = ab_buffer_literal(out, " depends_on=");
    for (index = 0; status == ARCHBIRD_OK && index < depends->as.array.count;
         index++) {
      const AbValue *item = &depends->as.array.items[index];
      if (item->kind != AB_VALUE_STRING) {
        status = query_schema_error(
            engine, "query artifact dependencies are malformed");
        break;
      }
      if (index)
        status = ab_buffer_literal(out, ",");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(out, item->as.text.data, item->as.text.length);
    }
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, "\n");

cleanup:
  ab_report_list_free(&input_paths);
  return status;
}

static int report_list_contains(const AbReportStringList *list,
                                const AbString *value) {
  size_t low = 0;
  size_t high;
  if (!list || !value)
    return 0;
  high = list->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&list->items[middle], value);
    if (!compared)
      return 1;
    if (compared < 0)
      low = middle + 1;
    else
      high = middle;
  }
  return 0;
}

static ArchbirdStatus overlay_selected_paths(ArchbirdEngine *engine,
                                             const AbValue *query,
                                             AbReportStringList *out) {
  const AbValue *files = ab_report_array(query, "files");
  const AbValue *metadata = ab_report_object(query, "query");
  const AbValue *change_set =
      metadata ? ab_value_member(metadata, "change_set") : NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(out, engine);
  if (!files || !metadata)
    return query_schema_error(engine, "query overlay input is malformed");
  for (index = 0; status == ARCHBIRD_OK && index < files->as.array.count;
       index++) {
    const AbString *path =
        ab_report_string(&files->as.array.items[index], "path");
    if (!path)
      status = query_schema_error(engine, "query overlay file is malformed");
    else
      status = ab_report_list_add(out, path->data, path->length);
  }
  if (status == ARCHBIRD_OK && change_set) {
    const AbValue *entries = ab_report_array(change_set, "entries");
    if (!entries)
      status =
          query_schema_error(engine, "query overlay change set is malformed");
    for (index = 0;
         status == ARCHBIRD_OK && entries && index < entries->as.array.count;
         index++) {
      const AbValue *entry = &entries->as.array.items[index];
      const AbString *path = ab_report_string(entry, "path");
      const AbString *previous = ab_report_string(entry, "previous_path");
      if (!path)
        status = query_schema_error(engine,
                                    "query overlay change entry is malformed");
      else
        status = ab_report_list_add(out, path->data, path->length);
      if (status == ARCHBIRD_OK && previous)
        status = ab_report_list_add(out, previous->data, previous->length);
    }
  }
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(out);
  return status;
}

static int overlay_project_allowed(const AbString *project,
                                   const AbReportStringList *aliases) {
  if (!aliases || !aliases->count)
    return 0;
  return !project || !project->length || report_list_contains(aliases, project);
}

static ArchbirdStatus overlay_evidence_paths(ArchbirdEngine *engine,
                                             const AbValue *evidence,
                                             const AbReportStringList *selected,
                                             const AbReportStringList *aliases,
                                             int include_lines,
                                             AbReportStringList *matches) {
  size_t index;
  if (!evidence)
    return ARCHBIRD_OK;
  if (evidence->kind != AB_VALUE_ARRAY)
    return query_schema_error(engine,
                              "verification overlay evidence is malformed");
  for (index = 0; index < evidence->as.array.count; index++) {
    const AbValue *row = &evidence->as.array.items[index];
    const AbString *path = ab_report_string(row, "path");
    const AbString *project = ab_report_string(row, "project");
    ArchbirdStatus status;
    if (!path || !project)
      return query_schema_error(
          engine, "verification overlay evidence row is malformed");
    if (!path->length || !overlay_project_allowed(project, aliases) ||
        !report_list_contains(selected, path))
      continue;
    if (include_lines) {
      size_t line = ab_report_size(row, "line", SIZE_MAX);
      if (line == SIZE_MAX)
        return query_schema_error(
            engine, "verification overlay evidence line is malformed");
      if (line)
        status = ab_report_list_addf(matches, "%.*s:%zu", (int)path->length,
                                     path->data, line);
      else
        status = ab_report_list_add(matches, path->data, path->length);
    } else {
      status = ab_report_list_add(matches, path->data, path->length);
    }
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static const AbValue *overlay_fact(const AbValue *facts, const AbString *name) {
  size_t index;
  if (!facts || facts->kind != AB_VALUE_ARRAY || !name || !name->length)
    return NULL;
  for (index = 0; index < facts->as.array.count; index++) {
    const AbString *candidate =
        ab_report_string(&facts->as.array.items[index], "name");
    if (candidate && ab_string_equal(candidate, name))
      return &facts->as.array.items[index];
  }
  return NULL;
}

static ArchbirdStatus overlay_fact_paths(
    ArchbirdEngine *engine, const AbValue *fact, const AbString *wanted_key,
    const AbReportStringList *selected, const AbReportStringList *aliases,
    int include_lines, AbReportStringList *matches) {
  const AbValue *items;
  const AbString *project;
  size_t index;
  if (!fact)
    return ARCHBIRD_OK;
  items = ab_report_array(fact, "items");
  project = ab_report_string(fact, "project");
  if (!items || !project)
    return query_schema_error(engine, "verification overlay fact is malformed");
  if (!overlay_project_allowed(project, aliases))
    return ARCHBIRD_OK;
  for (index = 0; index < items->as.array.count; index++) {
    const AbValue *item = &items->as.array.items[index];
    const AbString *key = ab_report_string(item, "key");
    ArchbirdStatus status;
    if (!key)
      return query_schema_error(engine,
                                "verification overlay fact item is malformed");
    if (wanted_key && !ab_string_equal(key, wanted_key))
      continue;
    status = overlay_evidence_paths(engine, ab_value_member(item, "evidence"),
                                    selected, aliases, include_lines, matches);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus
overlay_operand_paths(ArchbirdEngine *engine, const AbValue *check,
                      const AbValue *facts, const AbString *wanted_key,
                      const AbReportStringList *selected,
                      const AbReportStringList *aliases, int include_lines,
                      AbReportStringList *matches) {
  const AbValue *operands = ab_report_object(check, "operands");
  size_t index;
  if (!operands)
    return query_schema_error(
        engine, "verification overlay check operands are malformed");
  for (index = 0; index < operands->as.object.count; index++) {
    const AbValue *value = &operands->as.object.fields[index].value;
    const AbValue *fact;
    ArchbirdStatus status;
    if (value->kind != AB_VALUE_STRING || !value->as.text.length)
      continue;
    fact = overlay_fact(facts, &value->as.text);
    status = overlay_fact_paths(engine, fact, wanted_key, selected, aliases,
                                include_lines, matches);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus overlay_check_paths(ArchbirdEngine *engine,
                                          const AbValue *check,
                                          const AbValue *facts,
                                          const AbReportStringList *selected,
                                          const AbReportStringList *aliases,
                                          AbReportStringList *matches) {
  const AbValue *findings = ab_report_array(check, "findings");
  size_t index;
  ArchbirdStatus status;
  ab_report_list_init(matches, engine);
  if (!findings)
    return query_schema_error(engine,
                              "verification overlay findings are malformed");
  status = overlay_evidence_paths(engine, ab_value_member(check, "witnesses"),
                                  selected, aliases, 0, matches);
  for (index = 0; status == ARCHBIRD_OK && index < findings->as.array.count;
       index++)
    status = overlay_evidence_paths(
        engine, ab_value_member(&findings->as.array.items[index], "evidence"),
        selected, aliases, 0, matches);
  if (status == ARCHBIRD_OK)
    status = overlay_operand_paths(engine, check, facts, NULL, selected,
                                   aliases, 0, matches);
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(matches);
  return status;
}

static ArchbirdStatus
overlay_finding_paths(ArchbirdEngine *engine, const AbValue *check,
                      const AbValue *finding, const AbValue *facts,
                      const AbReportStringList *selected,
                      const AbReportStringList *aliases, int include_lines,
                      AbReportStringList *matches) {
  const AbString *key = ab_report_string(finding, "key");
  ArchbirdStatus status;
  ab_report_list_init(matches, engine);
  if (!key)
    return query_schema_error(engine,
                              "verification overlay finding is malformed");
  status = overlay_evidence_paths(engine, ab_value_member(finding, "evidence"),
                                  selected, aliases, include_lines, matches);
  if (status == ARCHBIRD_OK)
    status = overlay_operand_paths(engine, check, facts, key, selected, aliases,
                                   include_lines, matches);
  if (status == ARCHBIRD_OK)
    ab_report_list_sort_unique(matches);
  return status;
}

static ArchbirdStatus overlay_constraint_freshness(
    ArchbirdEngine *engine, const AbValue *evaluation,
    const AbString *map_project, const AbString *map_config,
    const AbString *map_inputs, AbReportStringList *aliases, const char **out) {
  const AbString *project = ab_report_string(evaluation, "project");
  const AbString *config = ab_report_string(evaluation, "map_config_sha256");
  const AbString *inputs = ab_report_string(evaluation, "map_input_sha256");
  ArchbirdStatus status;
  if (!project || !config || !inputs)
    return query_schema_error(engine,
                              "verification evaluation identity is malformed");
  if (!ab_string_equal(project, map_project)) {
    *out = "not-applicable";
    return ARCHBIRD_OK;
  }
  status = ab_report_list_add(aliases, project->data, project->length);
  if (status != ARCHBIRD_OK)
    return status;
  ab_report_list_sort_unique(aliases);
  if (!config->length || !inputs->length)
    *out = "unknown";
  else if (ab_string_equal(config, map_config) &&
           ab_string_equal(inputs, map_inputs))
    *out = "current";
  else
    *out = "stale";
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_overlay_requirements(ArchbirdEngine *engine,
                                                  const AbValue *requirements,
                                                  AbBuffer *out) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!requirements || requirements->kind != AB_VALUE_ARRAY)
    return query_schema_error(
        engine, "verification overlay requirements are malformed");
  if (!requirements->as.array.count)
    return ARCHBIRD_OK;
  status = ab_buffer_literal(out, " requirements=");
  for (index = 0; status == ARCHBIRD_OK && index < requirements->as.array.count;
       index++) {
    const AbValue *requirement = &requirements->as.array.items[index];
    if (requirement->kind != AB_VALUE_STRING || !requirement->as.text.length)
      return query_schema_error(
          engine, "verification overlay requirement is malformed");
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(out, requirement->as.text.data,
                                requirement->as.text.length);
  }
  return status;
}

static const char *overlay_producer(const AbValue *verification,
                                    const AbValue *query) {
  const AbValue *verification_tool = ab_report_object(verification, "tool");
  const AbValue *query_tool = ab_report_object(query, "tool");
  const AbString *left =
      verification_tool
          ? ab_report_string(verification_tool, "implementation_sha256")
          : NULL;
  const AbString *right =
      query_tool ? ab_report_string(query_tool, "implementation_sha256") : NULL;
  if (!left || !right || !left->length || !right->length)
    return "unknown";
  return ab_string_equal(left, right) ? "current" : "different";
}

static ArchbirdStatus render_overlay_path_list(const AbReportStringList *paths,
                                               size_t limit, AbBuffer *out) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0;
       status == ARCHBIRD_OK && index < paths->count && index < limit;
       index++) {
    if (index)
      status = ab_buffer_literal(out, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(out, paths->items[index].data,
                                paths->items[index].length);
  }
  if (status == ARCHBIRD_OK && paths->count > limit)
    status = ab_report_appendf(out, ",…+%zu", paths->count - limit);
  return status;
}

static ArchbirdStatus
render_verification_overlay(ArchbirdEngine *engine, const AbValue *map,
                            const AbValue *query, const AbValue *verification,
                            ArchbirdReportDetail detail, AbBuffer *out) {
  const AbValue *evaluations;
  const AbValue *evaluation;
  const AbValue *facts;
  const AbValue *checks;
  const AbValue *summary;
  const AbValue *map_evidence = ab_report_object(map, "evidence");
  const AbString *map_project = ab_report_string(map, "project");
  const AbString *map_config =
      map_evidence ? ab_report_string(map_evidence, "config_sha256") : NULL;
  const AbString *map_inputs =
      map_evidence ? ab_report_string(map_evidence, "input_sha256") : NULL;
  const AbString *policy_name;
  AbReportStringList selected;
  AbReportStringList aliases;
  const char *freshness = "unknown";
  const char *producer = "unknown";
  size_t relevant_checks = 0;
  size_t relevant_findings = 0;
  size_t emitted_checks = 0;
  size_t emitted_findings = 0;
  size_t check_limit = detail == ARCHBIRD_REPORT_DETAIL_COMPACT    ? 8
                       : detail == ARCHBIRD_REPORT_DETAIL_STANDARD ? 24
                                                                   : SIZE_MAX;
  size_t finding_limit = detail == ARCHBIRD_REPORT_DETAIL_COMPACT    ? 8
                         : detail == ARCHBIRD_REPORT_DETAIL_STANDARD ? 32
                                                                     : SIZE_MAX;
  size_t path_limit = detail == ARCHBIRD_REPORT_DETAIL_COMPACT ? 3 : 8;
  size_t index;
  uint64_t schema;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&selected, engine);
  ab_report_list_init(&aliases, engine);
  if (!verification || verification->kind != AB_VALUE_OBJECT ||
      !ab_value_string_is(ab_value_member(verification, "artifact"),
                          "verification") ||
      !ab_value_u64(ab_value_member(verification, "schema_version"), &schema) ||
      schema != 2)
    return query_schema_error(
        engine, "verification overlay must be a canonical schema-2 result");
  evaluations = ab_report_array(verification, "evaluations");
  evaluation = NULL;
  if (evaluations)
    for (index = 0; index < evaluations->as.array.count; index++) {
      const AbValue *row = &evaluations->as.array.items[index];
      const AbString *id = ab_report_string(row, "id");
      if (id && id->length == 7 && !memcmp(id->data, "current", 7)) {
        evaluation = row;
        break;
      }
    }
  facts = ab_report_array(verification, "operands");
  checks = ab_report_array(verification, "constraints");
  summary = ab_report_object(verification, "summary");
  policy_name = evaluation ? ab_report_string(evaluation, "project") : NULL;
  if (!evaluation || !facts || !checks || !summary || !policy_name ||
      !map_project || !map_config || !map_inputs) {
    status = query_schema_error(engine,
                                "verification overlay structure is malformed");
    goto cleanup;
  }
  status = overlay_selected_paths(engine, query, &selected);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  status =
      overlay_constraint_freshness(engine, evaluation, map_project, map_config,
                                   map_inputs, &aliases, &freshness);
  if (status != ARCHBIRD_OK)
    goto cleanup;
  producer = overlay_producer(verification, query);
  for (index = 0; status == ARCHBIRD_OK && index < checks->as.array.count;
       index++) {
    const AbValue *check = &checks->as.array.items[index];
    const AbValue *findings = ab_report_array(check, "findings");
    AbReportStringList paths;
    size_t finding_index;
    status =
        overlay_check_paths(engine, check, facts, &selected, &aliases, &paths);
    if (status != ARCHBIRD_OK) {
      ab_report_list_free(&paths);
      break;
    }
    if (paths.count)
      relevant_checks++;
    for (finding_index = 0;
         paths.count && findings && finding_index < findings->as.array.count;
         finding_index++) {
      AbReportStringList finding_paths;
      status = overlay_finding_paths(
          engine, check, &findings->as.array.items[finding_index], facts,
          &selected, &aliases, 0, &finding_paths);
      if (status == ARCHBIRD_OK && finding_paths.count)
        relevant_findings++;
      ab_report_list_free(&finding_paths);
      if (status != ARCHBIRD_OK)
        break;
    }
    ab_report_list_free(&paths);
  }
  if (status != ARCHBIRD_OK)
    goto cleanup;
  status = ab_report_literal_line(out, "## Architecture constraints");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "Verification `%.*s`; evidence=%s; producer=%s; relevant=%zu/%zu "
        "constraints; findings=%zu; policy-blocking=%s.",
        (int)policy_name->length, policy_name->data, freshness, producer,
        relevant_checks, checks->as.array.count, relevant_findings,
        ab_report_bool(summary, "blocking", 0) ? "yes" : "no");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (!relevant_checks && status == ARCHBIRD_OK)
    status = ab_report_literal_line(
        out,
        "No constraint has exact source-path evidence in this change slice.");
  if (relevant_checks && status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```text");
  for (index = 0; status == ARCHBIRD_OK && index < checks->as.array.count;
       index++) {
    const AbValue *check = &checks->as.array.items[index];
    const AbValue *findings = ab_report_array(check, "findings");
    const AbString *id = ab_report_string(check, "id");
    const AbString *check_status = ab_report_string(check, "status");
    const AbString *severity = ab_report_string(check, "severity");
    const AbString *owner = ab_report_string(check, "owner");
    const AbValue *requirements = ab_value_member(check, "requirements");
    AbReportStringList paths;
    size_t finding_index;
    size_t check_findings = 0;
    status =
        overlay_check_paths(engine, check, facts, &selected, &aliases, &paths);
    if (status != ARCHBIRD_OK) {
      ab_report_list_free(&paths);
      break;
    }
    if (!paths.count) {
      ab_report_list_free(&paths);
      continue;
    }
    for (finding_index = 0;
         findings && finding_index < findings->as.array.count;
         finding_index++) {
      AbReportStringList finding_paths;
      status = overlay_finding_paths(
          engine, check, &findings->as.array.items[finding_index], facts,
          &selected, &aliases, 0, &finding_paths);
      if (status == ARCHBIRD_OK && finding_paths.count)
        check_findings++;
      ab_report_list_free(&finding_paths);
      if (status != ARCHBIRD_OK)
        break;
    }
    if (status != ARCHBIRD_OK) {
      ab_report_list_free(&paths);
      break;
    }
    if (emitted_checks++ >= check_limit) {
      ab_report_list_free(&paths);
      continue;
    }
    if (!id || !check_status || !severity || !owner) {
      ab_report_list_free(&paths);
      status = query_schema_error(
          engine, "verification overlay constraint is malformed");
      break;
    }
    status = ab_report_appendf(
        out, "%.*s %.*s %.*s owner=%.*s", (int)check_status->length,
        check_status->data, (int)severity->length, severity->data,
        (int)id->length, id->data, (int)owner->length, owner->data);
    if (status == ARCHBIRD_OK)
      status = render_overlay_requirements(engine, requirements, out);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(out, " findings=%zu paths=", check_findings);
    if (status == ARCHBIRD_OK)
      status = render_overlay_path_list(&paths, path_limit, out);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "\n");
    for (finding_index = 0; status == ARCHBIRD_OK && findings &&
                            finding_index < findings->as.array.count;
         finding_index++) {
      const AbValue *finding = &findings->as.array.items[finding_index];
      const AbString *comparison = ab_report_string(finding, "comparison");
      const AbString *key = ab_report_string(finding, "key");
      const AbString *message = ab_report_string(finding, "message");
      const AbString *disposition = ab_report_string(finding, "disposition");
      const AbString *evidence_state =
          ab_report_string(finding, "evidence_state");
      const AbString *applicability =
          ab_report_string(finding, "applicability");
      AbReportStringList finding_paths;
      status = overlay_finding_paths(engine, check, finding, facts, &selected,
                                     &aliases, 1, &finding_paths);
      if (status != ARCHBIRD_OK) {
        ab_report_list_free(&finding_paths);
        break;
      }
      if (!finding_paths.count || emitted_findings++ >= finding_limit) {
        ab_report_list_free(&finding_paths);
        continue;
      }
      if (!comparison || !key || !message || !disposition || !evidence_state ||
          !applicability)
        status = query_schema_error(
            engine, "verification overlay finding fields are malformed");
      if (status == ARCHBIRD_OK)
        status = ab_report_appendf(
            out, "  %.*s %.*s [%.*s,%.*s,%.*s]: %.*s @ ",
            (int)comparison->length, comparison->data, (int)key->length,
            key->data, (int)disposition->length, disposition->data,
            (int)evidence_state->length, evidence_state->data,
            (int)applicability->length, applicability->data,
            (int)message->length, message->data);
      if (status == ARCHBIRD_OK)
        status = render_overlay_path_list(&finding_paths, path_limit, out);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(out, "\n");
      ab_report_list_free(&finding_paths);
    }
    ab_report_list_free(&paths);
  }
  if (relevant_checks && status == ARCHBIRD_OK && relevant_checks > check_limit)
    status =
        ab_report_linef(out, "… %zu relevant constraints omitted by detail",
                        relevant_checks - check_limit);
  if (relevant_checks && status == ARCHBIRD_OK &&
      relevant_findings > finding_limit)
    status = ab_report_linef(out, "… %zu relevant findings omitted by detail",
                             relevant_findings - finding_limit);
  if (relevant_checks && status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "```");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);

cleanup:
  ab_report_list_free(&aliases);
  ab_report_list_free(&selected);
  return status;
}

static ArchbirdStatus render_query_view(
    ArchbirdEngine *engine, const AbValue *map, const AbValue *query,
    const AbValue *verification, size_t node_limit, int include_neighborhood,
    int include_routed, int compact_header, ArchbirdQueryView view,
    ArchbirdReportDetail detail, AbBuffer *out) {
  const AbValue *map_files = optional_array(map, "files");
  const AbValue *files = optional_array(query, "files");
  const AbValue *edges = optional_array(query, "edges");
  const AbValue *metadata = ab_report_object(query, "query");
  const AbValue *evidence = ab_report_object(query, "evidence");
  const AbValue *source_tool = ab_report_object(query, "source_tool");
  const AbValue *tool = ab_report_object(query, "tool");
  const AbValue *discovery = optional_object(query, "discovery");
  const AbString *project = ab_report_string(query, "project");
  const AbString *direction;
  const AbString *config;
  const AbString *inputs;
  const AbValue *focus;
  const AbValue *seeds;
  const AbValue *seed_identities;
  const AbValue *change_set;
  const AbValue *retrieval;
  const AbString *scope;
  const AbString *producer_policy;
  const AbString *producer_compatibility;
  int change_view = view == ARCHBIRD_QUERY_VIEW_CHANGES;
  int full_detail = detail == ARCHBIRD_REPORT_DETAIL_FULL;
  int compact_detail = detail == ARCHBIRD_REPORT_DETAIL_COMPACT;
  QueryReportFile *report_files = NULL;
  QueryContextPolicy policy;
  size_t compact_symbols =
      ab_report_size(optional_object(map, "limits"), "compact_symbols", 10);
  size_t compact_edges =
      ab_report_size(optional_object(map, "limits"), "compact_edge_names", 12);
  size_t index;
  size_t omitted;
  size_t eligible_files = 0;
  size_t visible_start = 0;
  size_t visible_count = 0;
  size_t test_eligible_count = 0;
  size_t test_emitted_count = 0;
  size_t test_candidate_collapsed_count = 0;
  size_t test_conservative_collapsed_count = 0;
  size_t test_excluded_count = 0;
  size_t test_start = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!metadata || !evidence || !source_tool || !tool || !project ||
      !ab_value_string_is(ab_value_member(query, "artifact"), "query"))
    return query_schema_error(engine, "query report input is malformed");
  direction = ab_report_string(metadata, "direction");
  config = ab_report_string(evidence, "config_sha256");
  inputs = ab_report_string(evidence, "input_sha256");
  focus = optional_array(metadata, "focus");
  seeds = optional_array(metadata, "seeds");
  seed_identities = optional_array(metadata, "seed_identities");
  change_set = ab_value_member(metadata, "change_set");
  retrieval = ab_value_member(metadata, "retrieval");
  if (retrieval && retrieval->kind != AB_VALUE_OBJECT)
    return query_schema_error(engine, "query retrieval must be an object");
  scope = string_or_empty(metadata, "scope");
  producer_policy = string_or_empty(metadata, "producer_policy");
  producer_compatibility = string_or_empty(metadata, "producer_compatibility");
  if (!direction || !config || !inputs || config->length < 16 ||
      inputs->length < 16 || node_limit > files->as.array.count)
    return query_schema_error(engine, "query report evidence is malformed");
  status = query_context_policy(engine, metadata, &policy);
  if (status != ARCHBIRD_OK)
    return status;
  if (compact_detail) {
    if (compact_symbols > 5)
      compact_symbols = 5;
    if (compact_edges > 5)
      compact_edges = 5;
  } else if (full_detail) {
    compact_symbols = SIZE_MAX;
    compact_edges = SIZE_MAX;
  }
  if (files->as.array.count) {
    report_files = (QueryReportFile *)ab_calloc(engine, files->as.array.count,
                                                sizeof(*report_files));
    if (!report_files)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory indexing query report");
  }
  for (index = 0; index < files->as.array.count; index++) {
    QueryReportFile *file = &report_files[index];
    file->row = &files->as.array.items[index];
    file->path = ab_report_string(file->row, "path");
    file->layer = ab_report_string(file->row, "layer");
    file->language = ab_report_string(file->row, "language");
    file->symbols = ab_report_array(file->row, "symbols");
    file->distance = ab_report_size(file->row, "distance", SIZE_MAX);
    ab_report_list_init(&file->incoming, engine);
    if (!file->path || !file->layer || !file->language || !file->symbols ||
        file->distance == SIZE_MAX) {
      status = query_schema_error(engine, "query.files[] is malformed");
      goto cleanup;
    }
  }
  for (index = 0; index < edges->as.array.count; index++) {
    const AbValue *edge = &edges->as.array.items[index];
    const AbString *target = ab_report_string(edge, "target");
    const AbValue *names = optional_array(edge, "names");
    size_t target_index =
        target ? file_index(report_files, files->as.array.count, target)
               : SIZE_MAX;
    size_t name_index;
    if (target_index == SIZE_MAX)
      continue;
    for (name_index = 0; name_index < names->as.array.count; name_index++) {
      const AbValue *name = &names->as.array.items[name_index];
      if (name->kind != AB_VALUE_STRING) {
        status = query_schema_error(engine, "query.edges[].names is malformed");
        goto cleanup;
      }
      QUERY_TRY(ab_report_list_add(&report_files[target_index].incoming,
                                   name->as.text.data, name->as.text.length));
    }
  }
  for (index = 0; index < files->as.array.count; index++)
    ab_report_list_sort_unique(&report_files[index].incoming);
  while (eligible_files < files->as.array.count &&
         report_files[eligible_files].distance <= policy.max_seed_distance)
    eligible_files++;
  visible_start =
      policy.file_offset < eligible_files ? policy.file_offset : eligible_files;
  visible_count = eligible_files - visible_start;
  if (visible_count > policy.file_quota)
    visible_count = policy.file_quota;
  if (visible_count > node_limit)
    visible_count = node_limit;
  {
    const AbValue *matches = optional_array(query, "test_matches");
    status = query_test_selection_stats(
        engine, matches, &policy, &test_eligible_count,
        &test_candidate_collapsed_count, &test_conservative_collapsed_count,
        &test_excluded_count);
    if (status != ARCHBIRD_OK)
      goto cleanup;
    test_start = policy.test_match_offset < test_eligible_count
                     ? policy.test_match_offset
                     : test_eligible_count;
    test_emitted_count = test_eligible_count - test_start;
    if (test_emitted_count > policy.test_match_quota)
      test_emitted_count = policy.test_match_quota;
  }

  QUERY_TRY(ab_report_linef(out,
                            change_view ? "# Change brief: %.*s"
                                        : "# Focused architecture map: %.*s",
                            (int)project->length, project->data));
  QUERY_TRY(ab_report_blank(out));
  QUERY_TRY(
      ab_buffer_literal(out, change_view ? "Change seeds: `" : "Focus: `"));
  {
    size_t focus_limit =
        compact_header && focus->as.array.count > 3 ? 3 : focus->as.array.count;
    for (index = 0; index < focus_limit; index++) {
      const AbValue *item = &focus->as.array.items[index];
      if (item->kind != AB_VALUE_STRING) {
        status = query_schema_error(engine, "query focus is malformed");
        goto cleanup;
      }
      if (index)
        QUERY_TRY(ab_buffer_literal(out, ", "));
      QUERY_TRY(
          ab_buffer_append(out, item->as.text.data, item->as.text.length));
    }
    if (focus->as.array.count > focus_limit)
      QUERY_TRY(ab_report_appendf(out, ", …+%zu",
                                  focus->as.array.count - focus_limit));
  }
  QUERY_TRY(ab_report_appendf(out,
                              "`; direction=%.*s; depth=%zu; test-depth=%zu; ",
                              (int)direction->length, direction->data,
                              ab_report_size(metadata, "depth", 0),
                              ab_report_size(metadata, "test_depth", 0)));
  {
    size_t symbol_seed_count = 0;
    size_t file_seed_count = 0;
    for (index = 0; index < seed_identities->as.array.count; index++) {
      const AbString *kind =
          ab_report_string(&seed_identities->as.array.items[index], "kind");
      if (ab_report_string_equal(kind, "symbol"))
        symbol_seed_count++;
      else if (ab_report_string_equal(kind, "file"))
        file_seed_count++;
      else {
        status =
            query_schema_error(engine, "query seed identity kind is invalid");
        goto cleanup;
      }
    }
    if (seed_identities->as.array.count)
      QUERY_TRY(ab_report_appendf(out, "symbol-seeds=%zu; file-seeds=%zu; ",
                                  symbol_seed_count, file_seed_count));
    else
      QUERY_TRY(
          ab_report_appendf(out, "file-seeds=%zu; ", seeds->as.array.count));
  }
  QUERY_TRY(
      ab_report_appendf(out, "selected-files=%zu.\n", files->as.array.count));
  QUERY_TRY(ab_report_blank(out));
  QUERY_TRY(ab_report_appendf(
      out,
      change_view ? "Selection: profile=%.*s; max-seed-distance="
                  : "Context: profile=%.*s; max-seed-distance=",
      (int)policy.profile->length, policy.profile->data));
  if (policy.max_seed_distance == SIZE_MAX)
    QUERY_TRY(ab_buffer_literal(out, "all"));
  else
    QUERY_TRY(ab_report_appendf(out, "%zu", policy.max_seed_distance));
  QUERY_TRY(ab_report_appendf(
      out, "; candidate=%.*s; conservative=%.*s; files=%zu/%zu",
      (int)policy.candidate->length, policy.candidate->data,
      (int)policy.conservative->length, policy.conservative->data,
      visible_count, eligible_files));
  if (visible_start)
    QUERY_TRY(ab_report_appendf(out, "; file-offset=%zu", visible_start));
  QUERY_TRY(ab_buffer_literal(out, ".\n"));
  QUERY_TRY(ab_report_blank(out));
  QUERY_TRY(ab_buffer_literal(out, change_view ? "Source snapshot: map tool "
                                               : "Evidence: map tool "));
  QUERY_TRY(render_tool_label(out, source_tool));
  QUERY_TRY(ab_buffer_literal(out, "; query tool "));
  QUERY_TRY(render_tool_label(out, tool));
  QUERY_TRY(ab_report_linef(
      out,
      "; config `%.*s`; inputs `%.*s`. Static routes only; ambiguous and "
      "unresolved calls are reported, not guessed.",
      16, config->data, 16, inputs->data));
  QUERY_TRY(ab_report_blank(out));
  if (change_view) {
    const char *compatibility_data = producer_compatibility->length
                                         ? producer_compatibility->data
                                         : "unknown";
    size_t compatibility_length = producer_compatibility->length
                                      ? producer_compatibility->length
                                      : sizeof("unknown") - 1;
    const char *policy_data =
        producer_policy->length ? producer_policy->data : "compatible";
    size_t policy_length = producer_policy->length ? producer_policy->length
                                                   : sizeof("compatible") - 1;
    QUERY_TRY(ab_report_linef(
        out,
        "Producer compatibility: %.*s under `%.*s` policy. Source freshness "
        "is not inferred from a saved Query; run `archbird freshness` against "
        "the live repository when consuming a snapshot.",
        (int)compatibility_length, compatibility_data, (int)policy_length,
        policy_data));
    QUERY_TRY(ab_report_blank(out));
  }
  if (retrieval)
    QUERY_TRY(render_retrieval_summary(engine, retrieval, compact_detail, out));
  if (change_view && change_set) {
    const AbValue *source = ab_report_object(change_set, "source");
    const AbValue *entries = ab_report_array(change_set, "entries");
    const AbString *kind = source ? ab_report_string(source, "kind") : NULL;
    const AbString *identity =
        source ? ab_report_string(source, "identity") : NULL;
    size_t mapped = 0;
    size_t shown;
    if (!source || !entries || !kind || !identity) {
      status = query_schema_error(engine, "query change set is malformed");
      goto cleanup;
    }
    for (index = 0; index < entries->as.array.count; index++) {
      const AbString *path =
          ab_report_string(&entries->as.array.items[index], "path");
      if (!path) {
        status = query_schema_error(engine, "query change entry is malformed");
        goto cleanup;
      }
      mapped += path_is_mapped(map_files, path);
    }
    QUERY_TRY(ab_report_literal_line(out, "## Changed paths"));
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_linef(
        out,
        "Source: %.*s `%.*s`; entries=%zu; mapped=%zu; "
        "outside-current-map=%zu.",
        (int)kind->length, kind->data, (int)identity->length, identity->data,
        entries->as.array.count, mapped, entries->as.array.count - mapped));
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_literal_line(out, "```text"));
    shown = entries->as.array.count;
    if (compact_detail && shown > compact_edges)
      shown = compact_edges;
    for (index = 0; index < shown; index++) {
      const AbValue *entry = &entries->as.array.items[index];
      const AbString *path = ab_report_string(entry, "path");
      const AbString *change_status = ab_report_string(entry, "status");
      const AbString *previous = ab_report_string(entry, "previous_path");
      if (!path || !change_status) {
        status = query_schema_error(engine, "query change entry is malformed");
        goto cleanup;
      }
      if (previous)
        QUERY_TRY(ab_report_linef(
            out, "%.*s %.*s <- %.*s%s", (int)change_status->length,
            change_status->data, (int)path->length, path->data,
            (int)previous->length, previous->data,
            path_is_mapped(map_files, path) ? "" : " [outside map]"));
      else
        QUERY_TRY(ab_report_linef(
            out, "%.*s %.*s%s", (int)change_status->length, change_status->data,
            (int)path->length, path->data,
            path_is_mapped(map_files, path) ? "" : " [outside map]"));
    }
    if (entries->as.array.count > shown)
      QUERY_TRY(ab_report_linef(out, "… %zu changed paths omitted by detail",
                                entries->as.array.count - shown));
    QUERY_TRY(ab_report_literal_line(out, "```"));
    QUERY_TRY(ab_report_blank(out));
  }
  if (change_view && verification)
    QUERY_TRY(render_verification_overlay(engine, map, query, verification,
                                          detail, out));
  if (discovery->as.object.count && (!change_view || full_detail)) {
    const AbValue *coverage = ab_report_object(discovery, "coverage");
    size_t inventory = ab_report_size(coverage, "inventory_files", SIZE_MAX);
    size_t selected = ab_report_size(coverage, "selected", SIZE_MAX);
    size_t unsupported =
        ab_report_size(coverage, "unsupported_known", SIZE_MAX);
    if (!coverage || inventory == SIZE_MAX || selected == SIZE_MAX ||
        unsupported == SIZE_MAX) {
      status =
          query_schema_error(engine, "query discovery evidence is malformed");
      goto cleanup;
    }
    QUERY_TRY(ab_report_linef(
        out,
        "Discovery scope: inventory=%zu; selected=%zu; mapped=%zu; "
        "unsupported-known=%zu.",
        inventory, selected, map_files->as.array.count, unsupported));
    QUERY_TRY(ab_report_blank(out));
    if (unsupported) {
      QUERY_TRY(ab_report_linef(
          out,
          "Coverage warning: %zu known source files are outside the current "
          "language-provider surface.",
          unsupported));
      QUERY_TRY(ab_report_blank(out));
    }
  }

  {
    const AbValue *components = optional_array(query, "components");
    const AbValue *packages = optional_array(query, "packages");
    size_t component_limit =
        compact_header && components->as.array.count > compact_edges
            ? compact_edges
            : components->as.array.count;
    size_t package_limit =
        compact_header && packages->as.array.count > compact_edges
            ? compact_edges
            : packages->as.array.count;
    if (components->as.array.count) {
      QUERY_TRY(ab_buffer_literal(out, change_view ? "Affected components: "
                                                   : "Components: "));
      for (index = 0; index < component_limit; index++) {
        const AbValue *item = &components->as.array.items[index];
        if (item->kind != AB_VALUE_STRING) {
          status = query_schema_error(engine, "query components are malformed");
          goto cleanup;
        }
        if (index)
          QUERY_TRY(ab_buffer_literal(out, ", "));
        QUERY_TRY(
            ab_buffer_append(out, item->as.text.data, item->as.text.length));
      }
      if (components->as.array.count > component_limit)
        QUERY_TRY(ab_report_appendf(
            out, ", …+%zu", components->as.array.count - component_limit));
      QUERY_TRY(ab_buffer_literal(out, "\n"));
    }
    if (packages->as.array.count) {
      QUERY_TRY(ab_buffer_literal(out, change_view ? "Affected packages: "
                                                   : "Packages: "));
      for (index = 0; index < package_limit; index++) {
        const AbValue *package = &packages->as.array.items[index];
        const AbString *identity = ab_report_string(package, "identity");
        const AbString *version = string_or_empty(package, "version");
        if (!identity || !version) {
          status = query_schema_error(engine, "query packages are malformed");
          goto cleanup;
        }
        if (index)
          QUERY_TRY(ab_buffer_literal(out, ", "));
        QUERY_TRY(ab_buffer_append(out, identity->data, identity->length));
        if (version->length)
          QUERY_TRY(ab_report_appendf(out, "@%.*s", (int)version->length,
                                      version->data));
      }
      if (packages->as.array.count > package_limit)
        QUERY_TRY(ab_report_appendf(out, ", …+%zu",
                                    packages->as.array.count - package_limit));
      QUERY_TRY(ab_buffer_literal(out, "\n"));
    }
    if (components->as.array.count || packages->as.array.count)
      QUERY_TRY(ab_report_blank(out));
  }

  if (include_neighborhood) {
    QUERY_TRY(ab_report_literal_line(
        out, change_view ? "## Affected code" : "## Ranked neighborhood"));
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_literal_line(out, "```text"));
    if (!visible_count)
      QUERY_TRY(ab_report_literal_line(out, "none within output budget"));
    for (index = 0; index < visible_count; index++) {
      QueryReportFile *file = &report_files[visible_start + index];
      AbReportStringList symbols;
      const char *marker = string_in_array(seeds, file->path) ? " seed" : "";
      size_t edge_index;
      QUERY_TRY(ab_report_linef(out, "d=%zu%s %.*s layer=%.*s language=%.*s",
                                file->distance, marker, (int)file->path->length,
                                file->path->data, (int)file->layer->length,
                                file->layer->data, (int)file->language->length,
                                file->language->data));
      ab_report_list_init(&symbols, engine);
      status = symbol_items(engine, file, compact_symbols, &symbols);
      if (status == ARCHBIRD_OK)
        status =
            ab_report_chunks(out, &symbols, "  symbols: ", QUERY_REPORT_WIDTH);
      ab_report_list_free(&symbols);
      if (status != ARCHBIRD_OK)
        goto cleanup;
      /* Edge rows are canonical by kind/source/target; collect per kind so a
       * source's routes remain lexically ordered exactly as in the oracle. */
      for (edge_index = 0; edge_index < edges->as.array.count;) {
        const AbValue *first = &edges->as.array.items[edge_index];
        const AbString *kind = ab_report_string(first, "kind");
        AbReportStringList routes;
        size_t scan = edge_index;
        if (!kind) {
          status = query_schema_error(engine, "query.edges[] is malformed");
          goto cleanup;
        }
        ab_report_list_init(&routes, engine);
        while (scan < edges->as.array.count) {
          const AbValue *edge = &edges->as.array.items[scan];
          const AbString *edge_kind = ab_report_string(edge, "kind");
          const AbString *source = ab_report_string(edge, "source");
          const AbString *target = ab_report_string(edge, "target");
          const AbValue *names = optional_array(edge, "names");
          if (!edge_kind || !source || !target) {
            status = query_schema_error(engine, "query.edges[] is malformed");
            break;
          }
          if (!ab_string_equal(edge_kind, kind))
            break;
          if (ab_string_equal(source, file->path)) {
            size_t target_index =
                file_index(report_files, files->as.array.count, target);
            if (!path_is_mapped(map_files, target) ||
                (target_index != SIZE_MAX &&
                 index_visible(target_index, visible_start, visible_count)))
              status =
                  ab_report_list_addf(&routes, "%.*s(%zu)", (int)target->length,
                                      target->data, names->as.array.count);
          }
          if (status != ARCHBIRD_OK)
            break;
          scan++;
        }
        if (status == ARCHBIRD_OK && routes.count) {
          AbBuffer prefix;
          ab_report_list_sort(&routes);
          ab_buffer_init(&prefix, engine);
          status = ab_report_appendf(&prefix, "  %.*s: ", (int)kind->length,
                                     kind->data);
          if (status == ARCHBIRD_OK)
            status = ab_report_chunks(out, &routes, (const char *)prefix.data,
                                      QUERY_REPORT_WIDTH);
          ab_buffer_free(&prefix);
        }
        ab_report_list_free(&routes);
        if (status != ARCHBIRD_OK)
          goto cleanup;
        edge_index = scan > edge_index ? scan : edge_index + 1;
      }
    }
    omitted = files->as.array.count - visible_count;
    if (omitted)
      QUERY_TRY(ab_report_linef(
          out,
          "… %zu files omitted by context policy, continuation offset, or "
          "output budget",
          omitted));
    QUERY_TRY(ab_report_literal_line(out, "```"));
  }
  if (include_routed) {
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_literal_line(out, change_view
                                              ? "## Routes, tests, and delivery"
                                              : "## Routed evidence"));
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_literal_line(out, "```text"));
    if (ab_report_string_equal(scope, "symbol")) {
      const AbValue *local_calls = optional_array(query, "symbol_calls");
      const AbValue *local_references =
          optional_array(query, "symbol_references");
      QUERY_TRY(ab_report_literal_line(out, "symbol-local relations:"));
      if (!local_calls->as.array.count && !local_references->as.array.count)
        QUERY_TRY(ab_report_literal_line(
            out, "  none; no symbol-local static relation was established"));
    }
    QUERY_TRY(render_symbol_relation_routes(
        engine, query, "symbol_calls", "symbol-calls", 0,
        policy.symbol_call_offset, policy.symbol_call_quota, out));
    QUERY_TRY(render_symbol_relation_routes(
        engine, query, "symbol_references", "symbol-references", 1,
        policy.symbol_reference_offset, policy.symbol_reference_quota, out));
    {
      const AbValue *resolutions = optional_array(query, "call_resolutions");
      size_t counts[sizeof(query_resolution_kinds) /
                    sizeof(query_resolution_kinds[0])] = {0};
      size_t resolution_index;
      if (ab_report_string_equal(scope, "symbol"))
        QUERY_TRY(ab_report_literal_line(
            out,
            "file fallback (selected files; not proof of focus-symbol use):"));
      QUERY_TRY(ab_buffer_literal(out, "calls"));
      for (resolution_index = 0; resolution_index < resolutions->as.array.count;
           resolution_index++) {
        const AbValue *row = &resolutions->as.array.items[resolution_index];
        const AbString *source = ab_report_string(row, "source");
        const AbString *kind = ab_report_string(row, "kind");
        size_t kind_index;
        size_t source_index =
            source ? file_index(report_files, files->as.array.count, source)
                   : SIZE_MAX;
        if (source_index == SIZE_MAX ||
            !index_visible(source_index, visible_start, visible_count))
          continue;
        for (kind_index = 0; kind_index < sizeof(query_resolution_kinds) /
                                              sizeof(query_resolution_kinds[0]);
             kind_index++)
          if (ab_report_string_equal(kind, query_resolution_kinds[kind_index]))
            counts[kind_index] += ab_report_size(row, "count", 0);
      }
      for (resolution_index = 0;
           resolution_index <
           sizeof(query_resolution_kinds) / sizeof(query_resolution_kinds[0]);
           resolution_index++)
        QUERY_TRY(ab_report_appendf(out, " %s=%zu",
                                    query_resolution_kinds[resolution_index],
                                    counts[resolution_index]));
      QUERY_TRY(ab_buffer_literal(out, "\n"));
      if (change_view && (counts[2] || counts[3]))
        QUERY_TRY(ab_report_linef(
            out,
            "uncertainty: ambiguous=%zu unresolved=%zu; these calls are "
            "retained as unknown rather than assigned a target",
            counts[2], counts[3]));
    }
    {
      const AbValue *matches = optional_array(query, "test_matches");
      if (matches->as.array.count) {
        QueryReportTest *ranked =
            matches->as.array.count
                ? (QueryReportTest *)ab_calloc(engine, matches->as.array.count,
                                               sizeof(*ranked))
                : NULL;
        QueryCollapsedTestGroup *collapsed_groups =
            matches->as.array.count ? (QueryCollapsedTestGroup *)ab_calloc(
                                          engine, matches->as.array.count,
                                          sizeof(*collapsed_groups))
                                    : NULL;
        size_t evidence_counts[6] = {0};
        size_t provenance_counts[3] = {0};
        size_t confidence_counts[4] = {0};
        int classified = 0;
        size_t match_index;
        size_t eligible_count = 0;
        size_t candidate_collapsed_count = 0;
        size_t conservative_collapsed_count = 0;
        size_t collapsed_group_count = 0;
        size_t excluded_count = 0;
        size_t start;
        size_t limit;
        AbReportStringList rows;
        if (matches->as.array.count && (!ranked || !collapsed_groups)) {
          status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                      ARCHBIRD_NO_OFFSET,
                                      "out of memory ranking query tests");
          ab_free(engine, ranked);
          ab_free(engine, collapsed_groups);
          goto cleanup;
        }
        ab_report_list_init(&rows, engine);
        for (match_index = 0; match_index < matches->as.array.count;
             match_index++) {
          const AbValue *row = &matches->as.array.items[match_index];
          const AbString *evidence_kind = ab_report_string(row, "evidence");
          const AbString *classification =
              ab_report_string(row, "classification");
          const AbString *provenance = ab_report_string(row, "provenance");
          const AbString *confidence = ab_report_string(row, "confidence");
          const AbString *evidence_scope =
              ab_report_string(row, "evidence_scope");
          const AbString *target_role = ab_report_string(row, "target_role");
          const AbValue *target = ab_value_member(row, "target");
          size_t legacy_rank = SIZE_MAX;
          QueryReportTest candidate = {0};
          if (classification)
            classified = 1;
          if ((provenance || confidence || evidence_scope || target_role ||
               target) &&
              (!provenance || !confidence || !evidence_scope || !target_role ||
               !target)) {
            status = query_schema_error(
                engine, "query test route axes are partially specified");
            break;
          }
          if (provenance && confidence && evidence_scope && target_role &&
              target) {
            if (ab_report_string_equal(provenance, "observed"))
              provenance_counts[0]++;
            else if (ab_report_string_equal(provenance, "asserted"))
              provenance_counts[1]++;
            else if (ab_report_string_equal(provenance, "derived"))
              provenance_counts[2]++;
            else {
              status = query_schema_error(
                  engine, "query test route provenance is invalid");
              break;
            }
            if (ab_report_string_equal(confidence, "exact"))
              confidence_counts[0]++;
            else if (ab_report_string_equal(confidence, "candidate"))
              confidence_counts[1]++;
            else if (ab_report_string_equal(confidence, "conservative"))
              confidence_counts[2]++;
            else if (ab_report_string_equal(confidence, "unresolved"))
              confidence_counts[3]++;
            else {
              status = query_schema_error(
                  engine, "query test route confidence is invalid");
              break;
            }
            if (!ab_report_string_equal(evidence_scope, "case") &&
                !ab_report_string_equal(evidence_scope, "file") &&
                !ab_report_string_equal(evidence_scope, "unresolved")) {
              status =
                  query_schema_error(engine, "query test scope is invalid");
              break;
            }
          }
          if (classification)
            legacy_rank =
                ab_report_string_equal(classification, "observed")       ? 0
                : ab_report_string_equal(classification, "direct")       ? 1
                : ab_report_string_equal(classification, "asserted")     ? 2
                : ab_report_string_equal(classification, "candidate")    ? 3
                : ab_report_string_equal(classification, "conservative") ? 4
                : ab_report_string_equal(classification, "unresolved")   ? 5
                                                                         : 6;
          else
            legacy_rank =
                ab_report_string_equal(evidence_kind, "configured")   ? 0
                : ab_report_string_equal(evidence_kind, "static")     ? 1
                : ab_report_string_equal(evidence_kind, "unresolved") ? 2
                                                                      : 3;
          candidate.row = row;
          candidate.evidence = evidence_kind;
          candidate.classification = classification;
          candidate.provenance = provenance;
          candidate.confidence = confidence;
          candidate.evidence_scope = evidence_scope;
          candidate.target_role = target_role;
          candidate.target = target;
          candidate.group = ab_report_string(row, "group");
          candidate.path = ab_report_string(row, "path");
          candidate.selector = ab_report_string(row, "selector");
          candidate.line = ab_report_size(row, "line", 0);
          candidate.seed_distance =
              ab_report_size(row, "seed_distance", SIZE_MAX);
          candidate.symbol_distance =
              ab_report_size(row, "symbol_distance", SIZE_MAX);
          candidate.route_count = optional_array(row, "route")->as.array.count;
          candidate.ranking_affinity =
              ab_report_size(row, "ranking_affinity", 0);
          if (!candidate.evidence || !candidate.group || !candidate.path ||
              !candidate.selector) {
            status =
                query_schema_error(engine, "query.test_matches[] is malformed");
            break;
          }
          if (legacy_rank < (classification ? 6u : 3u))
            evidence_counts[legacy_rank]++;
          if (provenance && confidence) {
            if (!policy_evidence_allowed(&policy, provenance, confidence) ||
                (candidate.seed_distance != SIZE_MAX &&
                 candidate.seed_distance > policy.max_seed_distance)) {
              excluded_count++;
              continue;
            }
            if (ab_report_string_equal(confidence, "candidate") &&
                !ab_report_string_equal(policy.candidate, "expand")) {
              if (ab_report_string_equal(policy.candidate, "collapse")) {
                candidate_collapsed_count++;
                status = collapsed_test_group_add(engine, collapsed_groups,
                                                  &collapsed_group_count,
                                                  &candidate);
              } else {
                excluded_count++;
              }
              if (status != ARCHBIRD_OK)
                break;
              continue;
            }
            if (ab_report_string_equal(confidence, "conservative") &&
                !ab_report_string_equal(policy.conservative, "expand")) {
              if (ab_report_string_equal(policy.conservative, "collapse")) {
                conservative_collapsed_count++;
                status = collapsed_test_group_add(engine, collapsed_groups,
                                                  &collapsed_group_count,
                                                  &candidate);
              } else {
                excluded_count++;
              }
              if (status != ARCHBIRD_OK)
                break;
              continue;
            }
          }
          ranked[eligible_count++] = candidate;
        }
        if (status == ARCHBIRD_OK && classified)
          status = ab_report_linef(
              out,
              "test-matches observed=%zu direct=%zu asserted=%zu candidate=%zu "
              "conservative=%zu unresolved=%zu",
              evidence_counts[0], evidence_counts[1], evidence_counts[2],
              evidence_counts[3], evidence_counts[4], evidence_counts[5]);
        else if (status == ARCHBIRD_OK)
          status = ab_report_linef(
              out, "test-matches configured=%zu static=%zu unresolved=%zu",
              evidence_counts[0], evidence_counts[1], evidence_counts[2]);
        if (status == ARCHBIRD_OK &&
            provenance_counts[0] + provenance_counts[1] + provenance_counts[2])
          status = ab_report_linef(
              out,
              "route-provenance observed=%zu asserted=%zu derived=%zu; "
              "confidence exact=%zu candidate=%zu conservative=%zu "
              "unresolved=%zu",
              provenance_counts[0], provenance_counts[1], provenance_counts[2],
              confidence_counts[0], confidence_counts[1], confidence_counts[2],
              confidence_counts[3]);
        start = policy.test_match_offset < eligible_count
                    ? policy.test_match_offset
                    : eligible_count;
        limit = eligible_count - start;
        if (limit > policy.test_match_quota)
          limit = policy.test_match_quota;
        test_eligible_count = eligible_count;
        test_emitted_count = limit;
        test_candidate_collapsed_count = candidate_collapsed_count;
        test_conservative_collapsed_count = conservative_collapsed_count;
        test_excluded_count = excluded_count;
        test_start = start;
        if (status == ARCHBIRD_OK)
          status = ab_report_linef(
              out,
              "route-selection emitted=%zu eligible=%zu "
              "candidate-collapsed=%zu conservative-collapsed=%zu "
              "excluded=%zu offset=%zu",
              limit, eligible_count, candidate_collapsed_count,
              conservative_collapsed_count, excluded_count, start);
        for (match_index = start;
             status == ARCHBIRD_OK && match_index < start + limit;
             match_index++) {
          const QueryReportTest *match = &ranked[match_index];
          const AbValue *route = optional_array(match->row, "route");
          const AbValue *configured =
              optional_array(match->row, "configured_targets");
          const AbValue *target =
              match->target && match->target->kind == AB_VALUE_OBJECT
                  ? match->target
                  : NULL;
          const AbString *target_path =
              target ? ab_report_string(target, "path") : NULL;
          const AbString *target_symbol =
              target ? ab_report_string(target, "symbol") : NULL;
          AbBuffer text;
          const AbString *label =
              match->classification ? match->classification : match->evidence;
          size_t route_index;
          ab_buffer_init(&text, engine);
          status = ab_report_appendf(
              &text, "%.*s:%.*s:%zu:%.*s [%.*s", (int)match->group->length,
              match->group->data, (int)match->path->length, match->path->data,
              match->line, (int)match->selector->length, match->selector->data,
              (int)label->length, label->data);
          if (status == ARCHBIRD_OK && match->classification)
            status = ab_report_appendf(&text, "; source=%.*s",
                                       (int)match->evidence->length,
                                       match->evidence->data);
          if (status == ARCHBIRD_OK)
            status = ab_buffer_literal(&text, "]");
          if (status == ARCHBIRD_OK && match->provenance)
            status = ab_report_appendf(
                &text,
                " [provenance=%.*s; confidence=%.*s; scope=%.*s; "
                "seed-distance=",
                (int)match->provenance->length, match->provenance->data,
                (int)match->confidence->length, match->confidence->data,
                (int)match->evidence_scope->length,
                match->evidence_scope->data);
          if (status == ARCHBIRD_OK && match->provenance &&
              match->seed_distance == SIZE_MAX)
            status = ab_buffer_literal(&text, "unresolved");
          else if (status == ARCHBIRD_OK && match->provenance)
            status = ab_report_appendf(&text, "%zu", match->seed_distance);
          if (status == ARCHBIRD_OK && match->provenance)
            status = ab_buffer_literal(&text, "; symbol-distance=");
          if (status == ARCHBIRD_OK && match->provenance &&
              match->symbol_distance == SIZE_MAX)
            status = ab_buffer_literal(&text, "unresolved");
          else if (status == ARCHBIRD_OK && match->provenance)
            status = ab_report_appendf(&text, "%zu", match->symbol_distance);
          if (status == ARCHBIRD_OK && match->provenance)
            status = ab_report_appendf(&text, "; target-role=%.*s",
                                       (int)match->target_role->length,
                                       match->target_role->data);
          if (status == ARCHBIRD_OK && match->provenance)
            status = ab_report_appendf(&text, "; affinity=%zu",
                                       match->ranking_affinity);
          if (status == ARCHBIRD_OK && match->provenance && target_path) {
            status =
                ab_report_appendf(&text, "; target=%.*s",
                                  (int)target_path->length, target_path->data);
            if (status == ARCHBIRD_OK && target_symbol)
              status =
                  ab_report_appendf(&text, ":%.*s", (int)target_symbol->length,
                                    target_symbol->data);
          }
          if (status == ARCHBIRD_OK && match->provenance)
            status = ab_buffer_literal(&text, "]");
          if (status == ARCHBIRD_OK)
            status = ab_buffer_literal(&text, " -> ");
          if (status == ARCHBIRD_OK && !route->as.array.count)
            status = ab_buffer_literal(&text, "unresolved");
          for (route_index = 0;
               status == ARCHBIRD_OK && route_index < route->as.array.count;
               route_index++) {
            const AbValue *part = &route->as.array.items[route_index];
            if (part->kind != AB_VALUE_STRING) {
              status =
                  query_schema_error(engine, "query test route is malformed");
              break;
            }
            if (route_index)
              status = ab_buffer_literal(&text, " -> ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&text, part->as.text.data,
                                        part->as.text.length);
          }
          if (status == ARCHBIRD_OK && configured->as.array.count) {
            status = ab_buffer_literal(&text, " {configured: ");
            for (route_index = 0; status == ARCHBIRD_OK &&
                                  route_index < configured->as.array.count;
                 route_index++) {
              const AbValue *part = &configured->as.array.items[route_index];
              if (part->kind != AB_VALUE_STRING) {
                status = query_schema_error(
                    engine, "query configured targets are malformed");
                break;
              }
              if (route_index)
                status = ab_buffer_literal(&text, ",");
              if (status == ARCHBIRD_OK)
                status = ab_buffer_append(&text, part->as.text.data,
                                          part->as.text.length);
            }
            if (status == ARCHBIRD_OK)
              status = ab_buffer_literal(&text, "}");
          }
          if (status == ARCHBIRD_OK)
            status =
                ab_report_list_add(&rows, (const char *)text.data, text.length);
          ab_buffer_free(&text);
        }
        if (status == ARCHBIRD_OK && start)
          status =
              ab_report_list_addf(&rows, "… %zu earlier rows skipped", start);
        if (status == ARCHBIRD_OK && eligible_count > start + limit)
          status = ab_report_list_addf(&rows, "…+%zu",
                                       eligible_count - start - limit);
        for (match_index = 0;
             status == ARCHBIRD_OK && match_index < collapsed_group_count;
             match_index++)
          status = collapsed_test_group_render(&rows,
                                               &collapsed_groups[match_index]);
        if (status == ARCHBIRD_OK && candidate_collapsed_count)
          status = ab_report_list_addf(
              &rows, "… expand %zu candidate routes with `--candidate expand`",
              candidate_collapsed_count);
        if (status == ARCHBIRD_OK && conservative_collapsed_count)
          status = ab_report_list_addf(
              &rows,
              "… expand %zu conservative routes with `--conservative expand`",
              conservative_collapsed_count);
        if (status == ARCHBIRD_OK)
          status = ab_report_chunks(out, &rows, "tests: ", QUERY_REPORT_WIDTH);
        ab_report_list_free(&rows);
        ab_free(engine, ranked);
        ab_free(engine, collapsed_groups);
        if (status != ARCHBIRD_OK)
          goto cleanup;
      }
    }
    {
      const AbValue *builds = optional_array(query, "builds");
      AbReportStringList rows;
      ab_report_list_init(&rows, engine);
      for (index = 0; status == ARCHBIRD_OK && index < builds->as.array.count;
           index++) {
        const AbValue *route = &builds->as.array.items[index];
        const AbString *source = ab_report_string(route, "source");
        const AbString *name = ab_report_string(route, "name");
        if (!source || !name)
          status = query_schema_error(engine, "query.builds[] is malformed");
        else
          status =
              ab_report_list_addf(&rows, "%.*s:%.*s", (int)source->length,
                                  source->data, (int)name->length, name->data);
      }
      if (status == ARCHBIRD_OK && rows.count)
        status = ab_report_chunks(out, &rows, "builds: ", QUERY_REPORT_WIDTH);
      ab_report_list_free(&rows);
      if (status != ARCHBIRD_OK)
        goto cleanup;
    }
    {
      const AbValue *artifacts = optional_array(query, "artifacts");
      for (index = 0; index < artifacts->as.array.count; index++) {
        status = render_query_artifact(
            engine, &artifacts->as.array.items[index], compact_edges, out);
        if (status != ARCHBIRD_OK)
          goto cleanup;
      }
    }
    {
      const AbValue *surfaces = optional_array(query, "surfaces");
      for (index = 0; index < surfaces->as.array.count; index++) {
        const AbValue *surface = &surfaces->as.array.items[index];
        const AbString *name = ab_report_string(surface, "name");
        if (!name) {
          status = query_schema_error(engine, "query.surfaces[] is malformed");
          goto cleanup;
        }
        QUERY_TRY(ab_report_linef(
            out,
            "surface %.*s: registered=%zu used=%zu unused=%zu unregistered=%zu "
            "unresolved=%zu ambiguous=%zu",
            (int)name->length, name->data,
            surface_summary(surface, "registered"),
            surface_summary(surface, "used"),
            surface_summary(surface, "unused"),
            surface_summary(surface, "unregistered_use"),
            surface_summary(surface, "unresolved"),
            surface_summary(surface, "ambiguous")));
      }
    }
    QUERY_TRY(ab_report_literal_line(out, "```"));
  }
  {
    const AbValue *requested = ab_value_member(metadata, "context");
    const AbValue *symbol_calls = optional_array(query, "symbol_calls");
    const AbValue *symbol_references =
        optional_array(query, "symbol_references");
    size_t call_start = policy.symbol_call_offset < symbol_calls->as.array.count
                            ? policy.symbol_call_offset
                            : symbol_calls->as.array.count;
    size_t reference_start =
        policy.symbol_reference_offset < symbol_references->as.array.count
            ? policy.symbol_reference_offset
            : symbol_references->as.array.count;
    size_t call_emitted = symbol_calls->as.array.count - call_start;
    size_t reference_emitted =
        symbol_references->as.array.count - reference_start;
    size_t file_emitted = include_neighborhood ? visible_count : 0;
    if (call_emitted > policy.symbol_call_quota)
      call_emitted = policy.symbol_call_quota;
    if (reference_emitted > policy.symbol_reference_quota)
      reference_emitted = policy.symbol_reference_quota;
    if (!include_routed) {
      call_emitted = 0;
      reference_emitted = 0;
      test_emitted_count = 0;
    }
    QUERY_TRY(ab_report_blank(out));
    QUERY_TRY(ab_report_literal_line(
        out, change_view ? "## Evidence limits" : "## Selection manifest"));
    QUERY_TRY(ab_report_blank(out));
    if (!change_view || full_detail) {
      QUERY_TRY(ab_buffer_literal(out, "Requested policy: `"));
      if (requested)
        QUERY_TRY(ab_value_render(out, requested));
      else
        QUERY_TRY(ab_buffer_literal(out, "{}"));
      QUERY_TRY(ab_buffer_literal(out, "`.\n"));
    }
    QUERY_TRY(ab_report_appendf(
        out,
        change_view ? "Policy: profile=%.*s; candidate=%.*s; "
                      "conservative=%.*s; max-seed-distance="
                    : "Effective policy: profile=%.*s; candidate=%.*s; "
                      "conservative=%.*s; max-seed-distance=",
        (int)policy.profile->length, policy.profile->data,
        (int)policy.candidate->length, policy.candidate->data,
        (int)policy.conservative->length, policy.conservative->data));
    if (policy.max_seed_distance == SIZE_MAX)
      QUERY_TRY(ab_buffer_literal(out, "all.\n"));
    else
      QUERY_TRY(ab_report_appendf(out, "%zu.\n", policy.max_seed_distance));
    QUERY_TRY(ab_report_linef(
        out,
        change_view
            ? "Shown: files=%zu/%zu eligible (%zu canonical); "
              "symbol-calls=%zu/%zu; symbol-references=%zu/%zu; tests=%zu/%zu."
            : "Emitted: files=%zu/%zu canonical=%zu; symbol-calls=%zu/%zu; "
              "symbol-references=%zu/%zu; test-matches=%zu/%zu.",
        file_emitted, eligible_files, files->as.array.count, call_emitted,
        symbol_calls->as.array.count, reference_emitted,
        symbol_references->as.array.count, test_emitted_count,
        test_eligible_count));
    QUERY_TRY(ab_report_linef(
        out,
        change_view ? "Collapsed or excluded: candidate-tests=%zu; "
                      "conservative-tests=%zu; excluded-tests=%zu; "
                      "files-before-offset=%zu; tests-before-offset=%zu."
                    : "Not emitted: tests-candidate-collapsed=%zu; "
                      "tests-conservative-collapsed=%zu; tests-excluded=%zu; "
                      "files-before-offset=%zu; tests-before-offset=%zu.",
        test_candidate_collapsed_count, test_conservative_collapsed_count,
        test_excluded_count, visible_start, test_start));
    if (change_view)
      QUERY_TRY(ab_report_literal_line(
          out,
          "The canonical Query JSON retains every selected fact; this brief "
          "only changes their human presentation."));
    if ((include_neighborhood &&
         visible_start + visible_count < eligible_files) ||
        (include_routed &&
         call_start + call_emitted < symbol_calls->as.array.count) ||
        (include_routed && reference_start + reference_emitted <
                               symbol_references->as.array.count) ||
        (include_routed &&
         test_start + test_emitted_count < test_eligible_count) ||
        (include_routed && (test_candidate_collapsed_count ||
                            test_conservative_collapsed_count))) {
      QUERY_TRY(ab_buffer_literal(out, "Continue with: `archbird query"));
      if (visible_start + visible_count < eligible_files)
        QUERY_TRY(ab_report_appendf(out, " --context-offset files=%zu",
                                    visible_start + visible_count));
      if (include_routed &&
          call_start + call_emitted < symbol_calls->as.array.count)
        QUERY_TRY(ab_report_appendf(out, " --context-offset symbol_calls=%zu",
                                    call_start + call_emitted));
      if (include_routed && reference_start + reference_emitted <
                                symbol_references->as.array.count)
        QUERY_TRY(ab_report_appendf(out,
                                    " --context-offset symbol_references=%zu",
                                    reference_start + reference_emitted));
      if (include_routed &&
          test_start + test_emitted_count < test_eligible_count)
        QUERY_TRY(ab_report_appendf(out, " --context-offset test_matches=%zu",
                                    test_start + test_emitted_count));
      if (include_routed && test_candidate_collapsed_count)
        QUERY_TRY(ab_buffer_literal(out, " --candidate expand"));
      if (include_routed && test_conservative_collapsed_count)
        QUERY_TRY(ab_buffer_literal(out, " --conservative expand"));
      QUERY_TRY(ab_buffer_literal(out, " …` using the same selectors.\n"));
    }
  }

cleanup:
  if (report_files) {
    for (index = 0; index < files->as.array.count; index++)
      ab_report_list_free(&report_files[index].incoming);
  }
  ab_free(engine, report_files);
  return status;
}

static ArchbirdStatus
render_query_once(ArchbirdEngine *engine, const AbValue *map,
                  const AbValue *query, const AbValue *verification,
                  size_t node_limit, ArchbirdQueryView view,
                  ArchbirdReportDetail detail, AbBuffer *out) {
  return render_query_view(engine, map, query, verification, node_limit, 1, 1,
                           0, view, detail, out);
}

static ArchbirdStatus
render_query_budget_summary(ArchbirdEngine *engine, const AbValue *query,
                            size_t budget, size_t node_limit,
                            int include_neighborhood, int include_routed,
                            AbBuffer *out) {
  const AbValue *files = optional_array(query, "files");
  AbReportStringList omitted;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_report_list_init(&omitted, engine);
  if (!include_neighborhood)
    status = ab_report_list_add(&omitted, "Ranked neighborhood", 19);
  if (status == ARCHBIRD_OK && !include_routed)
    status = ab_report_list_add(&omitted, "Routed evidence", 15);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(out, "## Compact projection");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "Budget=%zu characters; sections=%d/2; ranked-file node cap=%zu; "
        "canonical-files=%zu.",
        budget, include_neighborhood + include_routed, node_limit,
        files->as.array.count);
  if (status == ARCHBIRD_OK && omitted.count)
    status = ab_report_chunks(
        out, &omitted, "Omitted complete sections: ", QUERY_REPORT_WIDTH);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(
        out,
        "The canonical Query IR remains complete. Increase `--max-chars` or "
        "request JSON for every selected fact.");
  ab_report_list_free(&omitted);
  return status;
}

static ArchbirdStatus render_query_budget_candidate(
    ArchbirdEngine *engine, const AbValue *map, const AbValue *query,
    const AbValue *verification, size_t budget, size_t node_limit,
    int include_neighborhood, int include_routed, ArchbirdQueryView view,
    ArchbirdReportDetail detail, AbBuffer *out) {
  ArchbirdStatus status = render_query_view(
      engine, map, query, verification, node_limit, include_neighborhood,
      include_routed, 1, view, detail, out);
  if (status == ARCHBIRD_OK)
    status =
        render_query_budget_summary(engine, query, budget, node_limit,
                                    include_neighborhood, include_routed, out);
  return status;
}

static ArchbirdStatus render_query_budget_length(
    ArchbirdEngine *engine, const AbValue *map, const AbValue *query,
    const AbValue *verification, size_t budget, size_t node_limit,
    int include_neighborhood, int include_routed, ArchbirdQueryView view,
    ArchbirdReportDetail detail, size_t *out_length) {
  AbBuffer candidate;
  ArchbirdStatus status;
  ab_buffer_init(&candidate, engine);
  status = render_query_budget_candidate(
      engine, map, query, verification, budget, node_limit,
      include_neighborhood, include_routed, view, detail, &candidate);
  if (status == ARCHBIRD_OK)
    *out_length = ab_report_codepoints(candidate.data, candidate.length);
  ab_buffer_free(&candidate);
  return status;
}

static ArchbirdStatus
render_query_budgeted(ArchbirdEngine *engine, const AbValue *map,
                      const AbValue *query, const AbValue *verification,
                      size_t budget, ArchbirdQueryView view,
                      ArchbirdReportDetail detail, AbBuffer *out) {
  const AbValue *files = optional_array(query, "files");
  size_t minimum_length = 0;
  size_t visible_count = 0;
  int include_neighborhood = 0;
  int include_routed = 0;
  ArchbirdStatus status =
      render_query_budget_length(engine, map, query, verification, budget, 0, 0,
                                 0, view, detail, &minimum_length);
  if (status != ARCHBIRD_OK)
    return status;
  if (minimum_length > budget)
    return archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "query.max_chars: %zu is too small; minimum navigable projection is "
        "%zu",
        budget, minimum_length);
  {
    size_t length = 0;
    include_neighborhood = 1;
    status = render_query_budget_length(engine, map, query, verification,
                                        budget, 0, include_neighborhood,
                                        include_routed, view, detail, &length);
    if (status != ARCHBIRD_OK)
      return status;
    if (length > budget)
      include_neighborhood = 0;
  }
  {
    size_t length = 0;
    include_routed = 1;
    status = render_query_budget_length(engine, map, query, verification,
                                        budget, 0, include_neighborhood,
                                        include_routed, view, detail, &length);
    if (status != ARCHBIRD_OK)
      return status;
    if (length > budget)
      include_routed = 0;
  }
  if (include_neighborhood && files->as.array.count) {
    size_t low = 1;
    size_t high = files->as.array.count;
    while (low <= high) {
      size_t middle = low + (high - low) / 2;
      size_t length = 0;
      status = render_query_budget_length(
          engine, map, query, verification, budget, middle,
          include_neighborhood, include_routed, view, detail, &length);
      if (status != ARCHBIRD_OK)
        return status;
      if (length <= budget) {
        visible_count = middle;
        low = middle + 1;
      } else {
        high = middle - 1;
      }
    }
  }
  return render_query_budget_candidate(engine, map, query, verification, budget,
                                       visible_count, include_neighborhood,
                                       include_routed, view, detail, out);
}

ArchbirdStatus ab_query_report_markdown_view(ArchbirdEngine *engine,
                                             const AbValue *map,
                                             const AbValue *query,
                                             ArchbirdQueryView view,
                                             ArchbirdReportDetail detail,
                                             size_t max_chars, AbBuffer *out) {
  return ab_query_report_markdown_view_with_verification(
      engine, map, query, NULL, view, detail, max_chars, out);
}

ArchbirdStatus ab_query_report_markdown_view_with_verification(
    ArchbirdEngine *engine, const AbValue *map, const AbValue *query,
    const AbValue *verification, ArchbirdQueryView view,
    ArchbirdReportDetail detail, size_t max_chars, AbBuffer *out) {
  const AbValue *files;
  size_t low;
  size_t high;
  size_t best_count = SIZE_MAX;
  size_t best_length = 0;
  ArchbirdStatus status;
  uint64_t schema;
  if (!engine || !map || !query || !out || view < ARCHBIRD_QUERY_VIEW_FOCUSED ||
      view > ARCHBIRD_QUERY_VIEW_CHANGES ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL ||
      (verification && view != ARCHBIRD_QUERY_VIEW_CHANGES))
    return ARCHBIRD_INVALID_ARGUMENT;
  if (map->kind != AB_VALUE_OBJECT ||
      !ab_value_string_is(ab_value_member(map, "artifact"), "map") ||
      !ab_value_u64(ab_value_member(map, "schema_version"), &schema) ||
      schema < ARCHBIRD_MAP_SCHEMA_MIN || schema > ARCHBIRD_MAP_SCHEMA_CURRENT)
    return query_schema_error(engine,
                              "query report source must be an Archbird map "
                              "schema " ARCHBIRD_MAP_SCHEMA_SUPPORTED_TEXT);
  if (query->kind != AB_VALUE_OBJECT ||
      !ab_value_string_is(ab_value_member(query, "artifact"), "query"))
    return query_schema_error(engine,
                              "query report input must be an Archbird query");
  files = ab_report_array(query, "files");
  if (!files)
    return query_schema_error(engine, "query.files must be an array");
  if (!max_chars)
    return render_query_once(engine, map, query, verification,
                             files->as.array.count, view, detail, out);
  low = 0;
  high = files->as.array.count;
  while (low <= high) {
    size_t middle = low + (high - low) / 2;
    AbBuffer candidate;
    size_t length;
    ab_buffer_init(&candidate, engine);
    status = render_query_once(engine, map, query, verification, middle, view,
                               detail, &candidate);
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&candidate);
      return status;
    }
    length = ab_report_codepoints(candidate.data, candidate.length);
    ab_buffer_free(&candidate);
    if (length <= max_chars) {
      best_count = middle;
      best_length = length;
      low = middle + 1;
    } else {
      if (!middle)
        break;
      high = middle - 1;
    }
  }
  if (best_count == SIZE_MAX) {
    AbBuffer minimum;
    ab_buffer_init(&minimum, engine);
    status = render_query_once(engine, map, query, verification, 0, view,
                               detail, &minimum);
    ab_buffer_free(&minimum);
    if (status != ARCHBIRD_OK)
      return status;
    return render_query_budgeted(engine, map, query, verification, max_chars,
                                 view, detail, out);
  }
  (void)best_length;
  return render_query_once(engine, map, query, verification, best_count, view,
                           detail, out);
}

ArchbirdStatus ab_query_report_markdown(ArchbirdEngine *engine,
                                        const AbValue *map,
                                        const AbValue *query, size_t max_chars,
                                        AbBuffer *out) {
  return ab_query_report_markdown_view(
      engine, map, query, ARCHBIRD_QUERY_VIEW_FOCUSED,
      ARCHBIRD_REPORT_DETAIL_STANDARD, max_chars, out);
}
