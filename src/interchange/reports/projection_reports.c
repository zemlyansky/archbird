#include "projection_reports.h"

#include "archbird_internal.h"
#include "report_utils.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REPORT_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static const AbValue *item_attribute(const AbProjectionItem *item,
                                     const char *name) {
  size_t length = strlen(name);
  size_t index;
  for (index = 0; item && index < item->attribute_count; index++)
    if (item->attributes[index].name.length == length &&
        (!length ||
         memcmp(item->attributes[index].name.data, name, length) == 0))
      return &item->attributes[index].value;
  return NULL;
}

static int item_is(const AbProjectionItem *item, const char *record_kind) {
  return ab_projection_value_is(item_attribute(item, "record_kind"),
                                record_kind);
}

static const AbString *item_text(const AbProjectionItem *item,
                                 const char *name) {
  const AbValue *value = item_attribute(item, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text : NULL;
}

static uint64_t item_u64(const AbProjectionItem *item, const char *name) {
  const AbValue *value = item_attribute(item, name);
  uint64_t result = 0;
  if (value)
    (void)ab_value_u64(value, &result);
  return result;
}

static size_t count_kind(const AbProjectionData *data, const char *kind) {
  size_t index;
  size_t count = 0;
  for (index = 0; index < data->item_count; index++)
    count += item_is(&data->items[index], kind);
  return count;
}

static int text_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || !memcmp(value->data, literal, length));
}

static size_t plan_array_count(const AbProjectionPlan *plan,
                               const char *field) {
  const AbValue *value = ab_value_member(&plan->definition, field);
  return value && value->kind == AB_VALUE_ARRAY ? value->as.array.count : 0;
}

static int plan_has(const AbProjectionPlan *plan, const char *field,
                    const char *literal) {
  const AbValue *value = ab_value_member(&plan->definition, field);
  size_t index;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++)
    if (ab_projection_value_is(&value->as.array.items[index], literal))
      return 1;
  return 0;
}

static ArchbirdStatus render_names(AbBuffer *out,
                                   const AbProjectionItem *item) {
  const AbValue *names = item_attribute(item, "names");
  size_t index;
  if (!names || names->kind != AB_VALUE_ARRAY || !names->as.array.count)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_buffer_literal(out, "; names="));
  for (index = 0; index < names->as.array.count; index++) {
    const AbValue *name = &names->as.array.items[index];
    if (index)
      REPORT_TRY(ab_buffer_literal(out, ", "));
    REPORT_TRY(ab_buffer_append(out, name->as.text.data, name->as.text.length));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_relation_kinds(AbBuffer *out,
                                            const AbProjectionItem *item) {
  const AbValue *kinds = item_attribute(item, "relation_kinds");
  size_t index;
  if (!kinds || kinds->kind != AB_VALUE_ARRAY || !kinds->as.array.count)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_buffer_literal(out, "; kinds="));
  for (index = 0; index < kinds->as.array.count; index++) {
    const AbValue *kind = &kinds->as.array.items[index];
    if (index)
      REPORT_TRY(ab_buffer_literal(out, ", "));
    REPORT_TRY(ab_buffer_append(out, kind->as.text.data, kind->as.text.length));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_evidence(AbBuffer *out,
                                      const AbProjectionItem *item) {
  size_t index;
  for (index = 0; index < item->evidence_count; index++) {
    const AbProjectionEvidence *evidence = &item->evidence[index];
    REPORT_TRY(ab_report_appendf(out, "  - `%.*s`", (int)evidence->path.length,
                                 evidence->path.data));
    if (evidence->line)
      REPORT_TRY(ab_report_appendf(out, ":%" PRIu64, evidence->line));
    if (evidence->detail.length)
      REPORT_TRY(ab_report_appendf(out, " - %.*s", (int)evidence->detail.length,
                                   evidence->detail.data));
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_group(AbBuffer *out, const AbProjectionItem *item,
                                   int full_detail) {
  const AbString *id = item_text(item, "id");
  const AbString *origin = item_text(item, "origin");
  uint64_t members = item_u64(item, "member_count");
  REPORT_TRY(ab_report_linef(
      out, "- **%.*s** (`%.*s`) - %" PRIu64 " member%s; %.*s",
      (int)item->label.length, item->label.data, id ? (int)id->length : 0,
      id ? id->data : "", members, members == 1 ? "" : "s",
      origin ? (int)origin->length : 0, origin ? origin->data : ""));
  return full_detail ? render_evidence(out, item) : ARCHBIRD_OK;
}

static ArchbirdStatus render_node(AbBuffer *out, const AbProjectionItem *item,
                                  int full_detail) {
  const AbString *id = item_text(item, "id");
  const AbString *kind = item_text(item, "entity_kind");
  const AbString *path = item_text(item, "path");
  REPORT_TRY(ab_report_appendf(out, "- `%.*s` - %.*s", id ? (int)id->length : 0,
                               id ? id->data : "", kind ? (int)kind->length : 0,
                               kind ? kind->data : ""));
  if (path)
    REPORT_TRY(
        ab_report_appendf(out, "; path=`%.*s`", (int)path->length, path->data));
  if (item_u64(item, "symbol_count"))
    REPORT_TRY(ab_report_appendf(out, "; symbols=%" PRIu64,
                                 item_u64(item, "symbol_count")));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  return full_detail ? render_evidence(out, item) : ARCHBIRD_OK;
}

static ArchbirdStatus render_membership(AbBuffer *out,
                                        const AbProjectionItem *item,
                                        int full_detail) {
  const AbString *group = item_text(item, "group");
  const AbString *node = item_text(item, "node");
  REPORT_TRY(
      ab_report_linef(out, "- `%.*s` contains `%.*s`",
                      group ? (int)group->length : 0, group ? group->data : "",
                      node ? (int)node->length : 0, node ? node->data : ""));
  return full_detail ? render_evidence(out, item) : ARCHBIRD_OK;
}

static ArchbirdStatus render_relation(AbBuffer *out,
                                      const AbProjectionItem *item,
                                      int standard_detail, int full_detail) {
  const AbString *source = item_text(item, "source");
  const AbString *target = item_text(item, "target");
  const AbString *family = item_text(item, "family");
  const AbString *kind = item_text(item, "relation_kind");
  REPORT_TRY(ab_report_appendf(
      out, "- `%.*s` -> `%.*s` - %.*s/%.*s; witnesses=%" PRIu64,
      source ? (int)source->length : 0, source ? source->data : "",
      target ? (int)target->length : 0, target ? target->data : "",
      family ? (int)family->length : 0, family ? family->data : "",
      kind ? (int)kind->length : 0, kind ? kind->data : "",
      item_u64(item, "witness_count")));
  if (standard_detail)
    REPORT_TRY(render_names(out, item));
  REPORT_TRY(ab_buffer_literal(out, "\n"));
  return full_detail ? render_evidence(out, item) : ARCHBIRD_OK;
}

static ArchbirdStatus render_diagnostic(AbBuffer *out,
                                        const AbProjectionItem *item) {
  const AbString *severity = item_text(item, "severity");
  const AbString *path = item_text(item, "path");
  REPORT_TRY(ab_report_appendf(out, "- **%.*s** %.*s",
                               severity ? (int)severity->length : 0,
                               severity ? severity->data : "",
                               (int)item->label.length, item->label.data));
  if (path && path->length)
    REPORT_TRY(
        ab_report_appendf(out, " (`%.*s`)", (int)path->length, path->data));
  return ab_buffer_literal(out, "\n");
}

static ArchbirdStatus render_coverage(AbBuffer *out,
                                      const AbProjectionItem *item) {
  REPORT_TRY(ab_report_linef(
      out,
      "- selected=%" PRIu64 "; inventory=%" PRIu64
      "; unsupported-known=%" PRIu64 "; oversized=%" PRIu64 "; assets=%" PRIu64
      "; ignored=%" PRIu64 "; pruned-directories=%" PRIu64,
      item_u64(item, "selected"), item_u64(item, "inventory_files"),
      item_u64(item, "unsupported_known"), item_u64(item, "oversized"),
      item_u64(item, "assets"), item_u64(item, "ignored"),
      item_u64(item, "pruned_directories")));
  if (!text_is(&item->state, "current"))
    REPORT_TRY(ab_report_linef(out, "- Coverage frontier: **%.*s** - %.*s",
                               (int)item->state.length, item->state.data,
                               (int)item->message.length, item->message.data));
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_ledgers(AbBuffer *out,
                                     const AbProjectionData *data) {
  size_t index;
  int heading = 0;
  for (index = 0; index < data->item_count; index++) {
    const AbProjectionItem *item = &data->items[index];
    const AbString *family;
    if (!item_is(item, "ledger"))
      continue;
    if (!heading) {
      REPORT_TRY(ab_report_literal_line(out, "## Relation completeness"));
      REPORT_TRY(ab_report_blank(out));
      heading = 1;
    }
    family = item_text(item, "family");
    REPORT_TRY(ab_report_linef(
        out, "- `%.*s`: collapsed=%" PRIu64 ", unknown=%" PRIu64 ", state=%.*s",
        family ? (int)family->length : 0, family ? family->data : "",
        item_u64(item, "collapsed"), item_u64(item, "unknown"),
        (int)item->state.length, item->state.data));
  }
  return heading ? ab_report_blank(out) : ARCHBIRD_OK;
}

static int item_pointer_key_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  return ab_string_compare(&a->key, &b->key);
}

static int landmark_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  uint64_t a_witnesses = item_u64(a, "relation_witness_count");
  uint64_t b_witnesses = item_u64(b, "relation_witness_count");
  uint64_t a_degree = item_u64(a, "used_by_count") + item_u64(a, "uses_count");
  uint64_t b_degree = item_u64(b, "used_by_count") + item_u64(b, "uses_count");
  if (a_witnesses != b_witnesses)
    return a_witnesses > b_witnesses ? -1 : 1;
  if (a_degree != b_degree)
    return a_degree > b_degree ? -1 : 1;
  if (item_u64(a, "symbol_count") != item_u64(b, "symbol_count"))
    return item_u64(a, "symbol_count") > item_u64(b, "symbol_count") ? -1 : 1;
  return ab_string_compare(&a->key, &b->key);
}

static int group_relation_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  if (item_u64(a, "witness_count") != item_u64(b, "witness_count"))
    return item_u64(a, "witness_count") > item_u64(b, "witness_count") ? -1 : 1;
  return ab_string_compare(&a->key, &b->key);
}

static ArchbirdStatus collect_items(AbBuffer *out, const AbProjectionData *data,
                                    const char *record_kind,
                                    const char *entity_kind,
                                    const AbProjectionItem ***items_out,
                                    size_t *count_out) {
  const AbProjectionItem **items;
  size_t count = 0;
  size_t index;
  for (index = 0; index < data->item_count; index++) {
    const AbString *kind = item_text(&data->items[index], "entity_kind");
    if (item_is(&data->items[index], record_kind) &&
        (!entity_kind || text_is(kind, entity_kind)))
      count++;
  }
  items = count ? (const AbProjectionItem **)ab_calloc(out->engine, count,
                                                       sizeof(*items))
                : NULL;
  if (count && !items)
    return ARCHBIRD_OUT_OF_MEMORY;
  count = 0;
  for (index = 0; index < data->item_count; index++) {
    const AbString *kind = item_text(&data->items[index], "entity_kind");
    if (item_is(&data->items[index], record_kind) &&
        (!entity_kind || text_is(kind, entity_kind)))
      items[count++] = &data->items[index];
  }
  *items_out = items;
  *count_out = count;
  return ARCHBIRD_OK;
}

static const AbProjectionItem *group_by_id(const AbProjectionData *data,
                                           const AbString *id) {
  size_t index;
  for (index = 0; index < data->item_count; index++) {
    const AbString *candidate;
    if (!item_is(&data->items[index], "group"))
      continue;
    candidate = item_text(&data->items[index], "id");
    if (candidate && ab_string_equal(candidate, id))
      return &data->items[index];
  }
  return NULL;
}

static ArchbirdStatus render_group_endpoint(AbBuffer *out,
                                            const AbProjectionData *data,
                                            const AbString *id) {
  const AbProjectionItem *group = group_by_id(data, id);
  if (!group)
    return ARCHBIRD_INVALID_SCHEMA;
  return ab_report_appendf(out, "**%.*s**", (int)group->label.length,
                           group->label.data);
}

static ArchbirdStatus render_architecture_groups(AbBuffer *out,
                                                 const AbProjectionData *data,
                                                 size_t limit,
                                                 size_t *omitted) {
  size_t index;
  size_t shown = 0;
  size_t total = 0;
  for (index = 0; index < data->item_count; index++) {
    const AbString *group_by;
    if (!item_is(&data->items[index], "group"))
      continue;
    group_by = item_text(&data->items[index], "group_by");
    if (!text_is(group_by, "inventory"))
      total++;
  }
  if (!total)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_report_literal_line(out, "## Architecture groups"));
  REPORT_TRY(ab_report_blank(out));
  for (index = 0; index < data->item_count && shown < limit; index++) {
    const AbString *group_by;
    if (!item_is(&data->items[index], "group"))
      continue;
    group_by = item_text(&data->items[index], "group_by");
    if (text_is(group_by, "inventory"))
      continue;
    REPORT_TRY(render_group(out, &data->items[index], 0));
    shown++;
  }
  if (shown < total) {
    *omitted += total - shown;
    REPORT_TRY(ab_report_linef(out, "- ... %zu architecture groups omitted",
                               total - shown));
  }
  return ab_report_blank(out);
}

static ArchbirdStatus render_inventory_groups(AbBuffer *out,
                                              const AbProjectionData *data) {
  size_t index;
  int heading = 0;
  for (index = 0; index < data->item_count; index++) {
    const AbProjectionItem *item = &data->items[index];
    if (!item_is(item, "group") ||
        !text_is(item_text(item, "group_by"), "inventory"))
      continue;
    if (!heading) {
      REPORT_TRY(ab_report_literal_line(out, "## Inventories"));
      REPORT_TRY(ab_report_blank(out));
      heading = 1;
    }
    REPORT_TRY(ab_report_linef(out, "- **%.*s**: %" PRIu64,
                               (int)item->label.length, item->label.data,
                               item_u64(item, "member_count")));
  }
  return heading ? ab_report_blank(out) : ARCHBIRD_OK;
}

static ArchbirdStatus render_group_relations(AbBuffer *out,
                                             const AbProjectionData *data,
                                             size_t limit, int standard_detail,
                                             size_t *omitted) {
  const AbProjectionItem **items = NULL;
  size_t count = 0;
  size_t shown;
  size_t index;
  ArchbirdStatus status =
      collect_items(out, data, "group_relation", NULL, &items, &count);
  if (status != ARCHBIRD_OK || !count)
    return status;
  qsort(items, count, sizeof(*items), group_relation_compare);
  shown = count < limit ? count : limit;
  status =
      ab_report_literal_line(out, "## Dependency flow (provider -> consumer)");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  for (index = 0; index < shown; index++) {
    const AbProjectionItem *item = items[index];
    const AbString *source = item_text(item, "source");
    const AbString *target = item_text(item, "target");
    const AbString *family = item_text(item, "family");
    if (status != ARCHBIRD_OK)
      break;
    status = ab_buffer_literal(out, "- ");
    /* Canonical relations are consumer -> provider. */
    if (status == ARCHBIRD_OK)
      status = render_group_endpoint(out, data, target);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, " -> ");
    if (status == ARCHBIRD_OK)
      status = render_group_endpoint(out, data, source);
    if (status == ARCHBIRD_OK)
      status = ab_report_appendf(
          out, " - %.*s; witnesses=%" PRIu64 "; relations=%" PRIu64,
          family ? (int)family->length : 0, family ? family->data : "",
          item_u64(item, "witness_count"), item_u64(item, "relation_count"));
    if (status == ARCHBIRD_OK && standard_detail)
      status = render_relation_kinds(out, item);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "\n");
  }
  if (status == ARCHBIRD_OK && shown < count) {
    *omitted += count - shown;
    status = ab_report_linef(out, "- ... %zu aggregated flows omitted",
                             count - shown);
  }
  ab_free(out->engine, items);
  return status == ARCHBIRD_OK ? ab_report_blank(out) : status;
}

static ArchbirdStatus render_landmarks(AbBuffer *out,
                                       const AbProjectionData *data,
                                       const char *heading, size_t limit,
                                       int standard_detail, int connected_only,
                                       size_t *omitted) {
  const AbProjectionItem **items = NULL;
  size_t count = 0;
  size_t total;
  size_t shown;
  size_t index;
  ArchbirdStatus status =
      collect_items(out, data, "node", "file", &items, &count);
  if (status != ARCHBIRD_OK || !count)
    return status;
  total = count;
  if (connected_only) {
    size_t write = 0;
    for (index = 0; index < count; index++)
      if (item_u64(items[index], "relation_witness_count"))
        items[write++] = items[index];
    count = write;
    *omitted += total - count;
  }
  if (!count) {
    ab_free(out->engine, items);
    REPORT_TRY(ab_report_literal_line(out, "## Test routes"));
    REPORT_TRY(ab_report_blank(out));
    REPORT_TRY(ab_report_literal_line(
        out, "- No static or observed test routes were selected."));
    return ab_report_blank(out);
  }
  qsort(items, count, sizeof(*items), landmark_compare);
  shown = count < limit ? count : limit;
  status = ab_report_linef(out, "## %s", heading);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  for (index = 0; index < shown; index++) {
    const AbProjectionItem *item = items[index];
    const AbString *path = item_text(item, "path");
    if (status != ARCHBIRD_OK)
      break;
    if (standard_detail)
      status = ab_report_linef(
          out,
          "- `%.*s` - used-by=%" PRIu64 "; uses=%" PRIu64
          "; relation-witnesses=%" PRIu64 "; symbols=%" PRIu64,
          path ? (int)path->length : (int)item->label.length,
          path ? path->data : item->label.data, item_u64(item, "used_by_count"),
          item_u64(item, "uses_count"),
          item_u64(item, "relation_witness_count"),
          item_u64(item, "symbol_count"));
    else
      status = ab_report_linef(
          out, "- `%.*s` - relations=%" PRIu64 "; symbols=%" PRIu64,
          path ? (int)path->length : (int)item->label.length,
          path ? path->data : item->label.data,
          item_u64(item, "relation_witness_count"),
          item_u64(item, "symbol_count"));
  }
  if (status == ARCHBIRD_OK && shown < count) {
    *omitted += count - shown;
    status =
        ab_report_linef(out, "- ... %zu file landmarks omitted", count - shown);
  }
  ab_free(out->engine, items);
  return status == ARCHBIRD_OK ? ab_report_blank(out) : status;
}

static ArchbirdStatus render_evidence_accounting(AbBuffer *out,
                                                 const AbProjectionData *data) {
  size_t direct = 0;
  size_t unknown = 0;
  size_t stale = 0;
  size_t diagnostics = 0;
  uint64_t witnesses = 0;
  size_t index;
  for (index = 0; index < data->item_count; index++) {
    const AbProjectionItem *item = &data->items[index];
    const AbString *classification;
    if (item_is(item, "diagnostic")) {
      diagnostics++;
      continue;
    }
    if (!item_is(item, "node") && !item_is(item, "relation"))
      continue;
    classification = item_text(item, "evidence_class");
    if (text_is(classification, "direct"))
      direct++;
    else if (text_is(classification, "unknown"))
      unknown++;
    else
      stale++;
    if (UINT64_MAX - witnesses < item_u64(item, "evidence_count"))
      return archbird_error_set(out->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many graph evidence witnesses");
    witnesses += item_u64(item, "evidence_count");
  }
  REPORT_TRY(ab_report_literal_line(out, "## Evidence accounting"));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(
      out,
      "- Entities/relations by evidence class: direct=%zu; unknown=%zu; "
      "stale=%zu.",
      direct, unknown, stale));
  REPORT_TRY(ab_report_linef(
      out, "- Attached evidence witnesses=%" PRIu64 "; diagnostics=%zu.",
      witnesses, diagnostics));
  return ab_report_blank(out);
}

static ArchbirdStatus
render_section(AbBuffer *out, const AbProjectionData *data,
               const char *record_kind, const char *heading, size_t limit,
               int standard_detail, int full_detail, size_t *omitted) {
  const AbProjectionItem **items = NULL;
  size_t total = 0;
  size_t shown;
  size_t index;
  ArchbirdStatus status =
      collect_items(out, data, record_kind, NULL, &items, &total);
  if (status != ARCHBIRD_OK || !total)
    return status;
  qsort(items, total, sizeof(*items), item_pointer_key_compare);
  shown = total < limit ? total : limit;
  status = ab_report_linef(out, "## %s", heading);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  for (index = 0; index < shown; index++) {
    const AbProjectionItem *item = items[index];
    if (status != ARCHBIRD_OK)
      break;
    if (!strcmp(record_kind, "group"))
      status = render_group(out, item, full_detail);
    else if (!strcmp(record_kind, "membership"))
      status = render_membership(out, item, full_detail);
    else if (!strcmp(record_kind, "node"))
      status = render_node(out, item, full_detail);
    else if (!strcmp(record_kind, "relation") ||
             !strcmp(record_kind, "group_relation"))
      status = render_relation(out, item, standard_detail, full_detail);
    else if (!strcmp(record_kind, "coverage"))
      status = render_coverage(out, item);
    else
      status = render_diagnostic(out, item);
  }
  if (status == ARCHBIRD_OK && shown < total) {
    *omitted += total - shown;
    status = ab_report_linef(out, "- ... %zu %s records omitted", total - shown,
                             record_kind);
  }
  ab_free(out->engine, items);
  return status == ARCHBIRD_OK ? ab_report_blank(out) : status;
}

static ArchbirdStatus render_once(const AbProjectionPlan *plan,
                                  const AbProjectionResult *result,
                                  size_t limit, int standard_detail,
                                  int full_detail, AbBuffer *out) {
  const AbValue *group = ab_value_member(&plan->definition, "group_by");
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const char *classification = ab_projection_data_classification(&result->data);
  int tests_only = plan_array_count(plan, "relations") == 1 &&
                   plan_has(plan, "relations", "tests");
  int evidence_only = !plan_array_count(plan, "relations") &&
                      plan_has(plan, "overlays", "evidence-quality");
  size_t omitted = 0;
  size_t landmark_limit = limit < 12 ? limit : 12;
  REPORT_TRY(ab_report_linef(out, "# %.*s architecture evidence",
                             (int)result->data.project.length,
                             result->data.project.data));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(
      out, "Projection `%.*s` · level `%.*s`%s%.*s%s · graph %s",
      (int)plan->id.length, plan->id.data,
      level && level->kind == AB_VALUE_STRING ? (int)level->as.text.length : 0,
      level && level->kind == AB_VALUE_STRING ? level->as.text.data : "",
      group ? " · group `" : "", group ? (int)group->as.text.length : 0,
      group ? group->as.text.data : "", group ? "`" : "", classification));
  REPORT_TRY(ab_report_blank(out));
  if (result->data.message.length) {
    REPORT_TRY(ab_report_linef(out, "> %.*s", (int)result->data.message.length,
                               result->data.message.data));
    REPORT_TRY(ab_report_blank(out));
  }
  if (full_detail) {
    REPORT_TRY(render_section(out, &result->data, "group", "Groups", limit,
                              standard_detail, 1, &omitted));
    REPORT_TRY(render_section(out, &result->data, "membership", "Memberships",
                              limit, standard_detail, 1, &omitted));
    REPORT_TRY(render_section(out, &result->data, "node", "Entities", limit,
                              standard_detail, 1, &omitted));
    REPORT_TRY(render_section(out, &result->data, "relation", "Canonical uses",
                              limit, standard_detail, 1, &omitted));
    REPORT_TRY(render_section(out, &result->data, "group_relation",
                              "Aggregated group relations", limit,
                              standard_detail, 1, &omitted));
  } else {
    REPORT_TRY(render_architecture_groups(out, &result->data, limit, &omitted));
    REPORT_TRY(render_group_relations(out, &result->data, limit,
                                      standard_detail, &omitted));
    if (evidence_only)
      REPORT_TRY(render_evidence_accounting(out, &result->data));
    else
      REPORT_TRY(render_landmarks(
          out, &result->data,
          tests_only ? "Test route landmarks" : "File landmarks",
          landmark_limit, standard_detail, tests_only, &omitted));
    REPORT_TRY(render_inventory_groups(out, &result->data));
    REPORT_TRY(ab_report_literal_line(out, "## Presentation accounting"));
    REPORT_TRY(ab_report_blank(out));
    REPORT_TRY(ab_report_linef(
        out,
        "- Canonical entities: %zu; canonical relations: %zu; aggregated "
        "group flows: %zu; displayed landmarks: at most %zu.",
        count_kind(&result->data, "node"),
        count_kind(&result->data, "relation"),
        count_kind(&result->data, "group_relation"), landmark_limit));
    REPORT_TRY(ab_report_blank(out));
  }
  REPORT_TRY(render_section(out, &result->data, "coverage",
                            "Repository coverage", limit, standard_detail,
                            full_detail, &omitted));
  REPORT_TRY(render_section(out, &result->data, "diagnostic", "Diagnostics",
                            limit, standard_detail, full_detail, &omitted));
  REPORT_TRY(render_ledgers(out, &result->data));
  REPORT_TRY(ab_report_literal_line(out, "## Graph completeness"));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(
      out,
      "- Classification: **%s**; exhaustive=%s; "
      "unknown-structural-records=%" PRIu64
      "; unsupported-structural-records=%" PRIu64,
      classification, !strcmp(classification, "complete") ? "yes" : "no",
      result->data.selection.has_unknown ? result->data.selection.unknown : 0,
      result->data.selection.has_unsupported
          ? result->data.selection.unsupported
          : 0));
  if (omitted)
    REPORT_TRY(ab_report_linef(
        out,
        "- Presentation omitted %zu display records; the exhaustive "
        "ProjectionResult is unchanged.",
        omitted));
  REPORT_TRY(
      ab_report_linef(out, "- Projection result: `%s`", result->result_sha256));
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_projection_report_markdown(ArchbirdEngine *engine,
                                             const AbProjectionPlan *plan,
                                             const AbProjectionResult *result,
                                             ArchbirdReportDetail detail,
                                             size_t max_chars, AbBuffer *out) {
  AbBuffer candidate;
  size_t low = 0;
  size_t high =
      detail == ARCHBIRD_REPORT_DETAIL_COMPACT
          ? 12
          : (detail == ARCHBIRD_REPORT_DETAIL_STANDARD ? 48 : SIZE_MAX);
  size_t best = 0;
  int found = 0;
  int standard_detail = detail != ARCHBIRD_REPORT_DETAIL_COMPACT;
  int full_detail = detail == ARCHBIRD_REPORT_DETAIL_FULL;
  ArchbirdStatus status;
  if (!engine || !plan || !result || !out ||
      !ab_projection_value_is(ab_value_member(&plan->definition, "select"),
                              "graph") ||
      !ab_projection_value_is(
          &(AbValue){.kind = AB_VALUE_STRING, .as.text = result->data.shape},
          "graph") ||
      detail < ARCHBIRD_REPORT_DETAIL_COMPACT ||
      detail > ARCHBIRD_REPORT_DETAIL_FULL)
    return ARCHBIRD_INVALID_ARGUMENT;
  if (full_detail && max_chars)
    return archbird_error_set(
        engine, ARCHBIRD_INVALID_ARGUMENT, ARCHBIRD_NO_OFFSET,
        "projection.max_chars cannot be combined with full detail");
  if (!max_chars)
    return render_once(plan, result, high, standard_detail, full_detail, out);
  high = high == SIZE_MAX ? result->data.item_count : high;
  ab_buffer_init(&candidate, engine);
  while (low <= high) {
    size_t middle = low + (high - low) / 2;
    candidate.length = 0;
    status = render_once(plan, result, middle, standard_detail, 0, &candidate);
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&candidate);
      return status;
    }
    if (ab_report_codepoints(candidate.data, candidate.length) <= max_chars) {
      found = 1;
      best = middle;
      low = middle + 1;
    } else if (!middle) {
      break;
    } else {
      high = middle - 1;
    }
  }
  candidate.length = 0;
  if (found)
    status = render_once(plan, result, best, standard_detail, 0, &candidate);
  else
    status = archbird_error_set(
        engine, ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
        "Markdown budget is too small for the graph projection summary");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, candidate.data, candidate.length);
  ab_buffer_free(&candidate);
  return status;
}

#undef REPORT_TRY
