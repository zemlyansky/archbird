#include "projection_reports.h"

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
      REPORT_TRY(ab_report_appendf(out, " — %.*s", (int)evidence->detail.length,
                                   evidence->detail.data));
    REPORT_TRY(ab_buffer_literal(out, "\n"));
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus render_group(AbBuffer *out, const AbProjectionItem *item,
                                   int full_detail) {
  const AbString *id = item_text(item, "id");
  const AbString *origin = item_text(item, "origin");
  REPORT_TRY(ab_report_linef(
      out, "- **%.*s** (`%.*s`) — %" PRIu64 " members; %.*s",
      (int)item->label.length, item->label.data, id ? (int)id->length : 0,
      id ? id->data : "", item_u64(item, "member_count"),
      origin ? (int)origin->length : 0, origin ? origin->data : ""));
  return full_detail ? render_evidence(out, item) : ARCHBIRD_OK;
}

static ArchbirdStatus render_node(AbBuffer *out, const AbProjectionItem *item,
                                  int full_detail) {
  const AbString *id = item_text(item, "id");
  const AbString *kind = item_text(item, "entity_kind");
  const AbString *path = item_text(item, "path");
  REPORT_TRY(ab_report_appendf(out, "- `%.*s` — %.*s", id ? (int)id->length : 0,
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

static ArchbirdStatus render_relation(AbBuffer *out,
                                      const AbProjectionItem *item,
                                      int standard_detail, int full_detail) {
  const AbString *source = item_text(item, "source");
  const AbString *target = item_text(item, "target");
  const AbString *family = item_text(item, "family");
  const AbString *kind = item_text(item, "relation_kind");
  REPORT_TRY(ab_report_appendf(
      out, "- `%.*s` → `%.*s` — %.*s/%.*s; witnesses=%" PRIu64,
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
  return ab_report_linef(
      out,
      "- selected=%" PRIu64 "; inventory=%" PRIu64
      "; unsupported-known=%" PRIu64 "; oversized=%" PRIu64 "; assets=%" PRIu64
      "; ignored=%" PRIu64 "; pruned-directories=%" PRIu64,
      item_u64(item, "selected"), item_u64(item, "inventory_files"),
      item_u64(item, "unsupported_known"), item_u64(item, "oversized"),
      item_u64(item, "assets"), item_u64(item, "ignored"),
      item_u64(item, "pruned_directories"));
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

static size_t count_kind(const AbProjectionData *data, const char *kind) {
  size_t index;
  size_t count = 0;
  for (index = 0; index < data->item_count; index++)
    count += item_is(&data->items[index], kind);
  return count;
}

static ArchbirdStatus
render_section(AbBuffer *out, const AbProjectionData *data,
               const char *record_kind, const char *heading, size_t limit,
               int standard_detail, int full_detail, size_t *omitted) {
  size_t index;
  size_t shown = 0;
  size_t total = count_kind(data, record_kind);
  if (!total)
    return ARCHBIRD_OK;
  REPORT_TRY(ab_report_linef(out, "## %s", heading));
  REPORT_TRY(ab_report_blank(out));
  for (index = 0; index < data->item_count && shown < limit; index++) {
    const AbProjectionItem *item = &data->items[index];
    if (!item_is(item, record_kind))
      continue;
    if (!strcmp(record_kind, "group"))
      REPORT_TRY(render_group(out, item, full_detail));
    else if (!strcmp(record_kind, "node"))
      REPORT_TRY(render_node(out, item, full_detail));
    else if (!strcmp(record_kind, "relation"))
      REPORT_TRY(render_relation(out, item, standard_detail, full_detail));
    else if (!strcmp(record_kind, "coverage"))
      REPORT_TRY(render_coverage(out, item));
    else
      REPORT_TRY(render_diagnostic(out, item));
    shown++;
  }
  if (shown < total) {
    *omitted += total - shown;
    REPORT_TRY(ab_report_linef(out, "- … %zu %s records omitted", total - shown,
                               record_kind));
  }
  return ab_report_blank(out);
}

static ArchbirdStatus render_once(const AbProjectionPlan *plan,
                                  const AbProjectionResult *result,
                                  size_t limit, int standard_detail,
                                  int full_detail, AbBuffer *out) {
  const AbValue *group = ab_value_member(&plan->definition, "group_by");
  const AbValue *level = ab_value_member(&plan->definition, "level");
  const char *classification = ab_projection_data_classification(&result->data);
  size_t omitted = 0;
  REPORT_TRY(ab_report_linef(out, "# %.*s architecture evidence",
                             (int)result->data.project.length,
                             result->data.project.data));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(
      out, "Projection `%.*s` · level `%.*s`%s%.*s%s · %s",
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
  REPORT_TRY(render_section(out, &result->data, "group", "Groups", limit,
                            standard_detail, full_detail, &omitted));
  REPORT_TRY(render_section(out, &result->data, "node", "Entities", limit,
                            standard_detail, full_detail, &omitted));
  REPORT_TRY(render_section(out, &result->data, "relation", "Relations", limit,
                            standard_detail, full_detail, &omitted));
  REPORT_TRY(render_section(out, &result->data, "coverage",
                            "Repository coverage", limit, standard_detail,
                            full_detail, &omitted));
  REPORT_TRY(render_section(out, &result->data, "diagnostic", "Diagnostics",
                            limit, standard_detail, full_detail, &omitted));
  REPORT_TRY(render_ledgers(out, &result->data));
  REPORT_TRY(ab_report_literal_line(out, "## Projection completeness"));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(
      out,
      "- Classification: **%s**; exhaustive=%s; unknown=%" PRIu64
      "; unsupported=%" PRIu64,
      classification, !strcmp(classification, "complete") ? "yes" : "no",
      result->data.selection.has_unknown ? result->data.selection.unknown : 0,
      result->data.selection.has_unsupported
          ? result->data.selection.unsupported
          : 0));
  if (omitted)
    REPORT_TRY(ab_report_linef(
        out,
        "- Presentation omitted %zu records; the ProjectionResult remains "
        "unchanged.",
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
      detail > ARCHBIRD_REPORT_DETAIL_FULL || (full_detail && max_chars))
    return ARCHBIRD_INVALID_ARGUMENT;
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
