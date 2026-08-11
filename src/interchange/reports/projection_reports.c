#include "interchange/reports/projection_reports.h"

#include "base/archbird_internal.h"
#include "interchange/reports/report_utils.h"

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
    if (item_is(&data->items[index], kind))
      count++;
  return count;
}

static int text_is(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || !memcmp(value->data, literal, length));
}

typedef struct {
  size_t entities;
  size_t files;
  size_t relations;
  size_t diagnostics;
  size_t errors;
  size_t warnings;
  uint64_t symbols;
} GraphReportSummary;

static ArchbirdStatus graph_report_summary(AbBuffer *out,
                                           const AbProjectionData *data,
                                           GraphReportSummary *summary) {
  size_t index;
  memset(summary, 0, sizeof(*summary));
  for (index = 0; index < data->item_count; index++) {
    const AbProjectionItem *item = &data->items[index];
    if (item_is(item, "node")) {
      const AbString *kind = item_text(item, "entity_kind");
      uint64_t symbol_count = item_u64(item, "symbol_count");
      summary->entities++;
      if (!text_is(kind, "file"))
        continue;
      summary->files++;
      if (UINT64_MAX - summary->symbols < symbol_count)
        return archbird_error_set(out->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "too many indexed symbols in graph report");
      summary->symbols += symbol_count;
    } else if (item_is(item, "relation")) {
      summary->relations++;
    } else if (item_is(item, "diagnostic")) {
      const AbString *severity = item_text(item, "severity");
      summary->diagnostics++;
      if (text_is(severity, "error"))
        summary->errors++;
      else if (text_is(severity, "warning"))
        summary->warnings++;
    }
  }
  return ARCHBIRD_OK;
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
  const AbString *code = item_text(item, "code");
  const AbString *path = item_text(item, "path");
  REPORT_TRY(ab_report_appendf(
      out, "- **%.*s** `%.*s` - %.*s", severity ? (int)severity->length : 0,
      severity ? severity->data : "", code ? (int)code->length : 0,
      code ? code->data : "", (int)item->label.length, item->label.data));
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

static int bytes_equal_literal(const char *data, size_t length,
                               const char *literal) {
  size_t literal_length = strlen(literal);
  return length == literal_length &&
         (!length || !memcmp(data, literal, length));
}

static int bytes_starts_with_literal(const char *data, size_t length,
                                     const char *literal) {
  size_t literal_length = strlen(literal);
  return length >= literal_length &&
         (!literal_length || !memcmp(data, literal, literal_length));
}

static int bytes_ends_with_literal(const char *data, size_t length,
                                   const char *literal) {
  size_t literal_length = strlen(literal);
  return length >= literal_length &&
         (!literal_length ||
          !memcmp(data + length - literal_length, literal, literal_length));
}

static int bytes_contains_literal(const char *data, size_t length,
                                  const char *literal) {
  size_t literal_length = strlen(literal);
  size_t offset;
  if (!literal_length)
    return 1;
  if (length < literal_length)
    return 0;
  for (offset = 0; offset <= length - literal_length; offset++)
    if (!memcmp(data + offset, literal, literal_length))
      return 1;
  return 0;
}

static int text_is_role_family(const AbString *value, const char *family) {
  size_t family_length = strlen(family);
  return value && value->length >= family_length &&
         !memcmp(value->data, family, family_length) &&
         (value->length == family_length || value->data[family_length] == '-');
}

static int path_has_segment(const AbString *path, const char *literal) {
  size_t start = 0;
  size_t end;
  if (!path)
    return 0;
  while (start <= path->length) {
    for (end = start; end < path->length && path->data[end] != '/'; end++)
      ;
    if (bytes_equal_literal(path->data + start, end - start, literal))
      return 1;
    if (end == path->length)
      break;
    start = end + 1;
  }
  return 0;
}

static int path_has_segment_pair(const AbString *path, const char *first,
                                 const char *second) {
  size_t first_length = strlen(first);
  size_t second_length = strlen(second);
  size_t offset;
  if (!path || first_length > SIZE_MAX - second_length - 1)
    return 0;
  for (offset = 0; offset + first_length + 1 + second_length <= path->length;
       offset++) {
    if (offset && path->data[offset - 1] != '/')
      continue;
    if (memcmp(path->data + offset, first, first_length) ||
        path->data[offset + first_length] != '/' ||
        memcmp(path->data + offset + first_length + 1, second, second_length))
      continue;
    if (offset + first_length + 1 + second_length == path->length ||
        path->data[offset + first_length + 1 + second_length] == '/')
      return 1;
  }
  return 0;
}

static void path_leaf(const AbString *path, const char **leaf,
                      size_t *leaf_length) {
  size_t offset;
  *leaf = path ? path->data : NULL;
  *leaf_length = path ? path->length : 0;
  if (!path)
    return;
  for (offset = path->length; offset; offset--) {
    if (path->data[offset - 1] != '/')
      continue;
    *leaf = path->data + offset;
    *leaf_length = path->length - offset;
    return;
  }
}

static size_t leaf_stem_length(const char *leaf, size_t length) {
  size_t offset;
  for (offset = length; offset; offset--)
    if (leaf[offset - 1] == '.')
      return offset - 1;
  return length;
}

static int path_is_test_or_fixture(const AbString *path) {
  static const char *const segments[] = {
      "test",      "tests",    "__tests__",    "spec", "specs",
      "fixture",   "fixtures", "__fixtures__", "mock", "mocks",
      "__mocks__", "testdata", "test-data"};
  const char *leaf;
  size_t leaf_length;
  size_t stem_length;
  size_t index;
  if (!path)
    return 0;
  for (index = 0; index < sizeof(segments) / sizeof(segments[0]); index++)
    if (path_has_segment(path, segments[index]))
      return 1;
  path_leaf(path, &leaf, &leaf_length);
  stem_length = leaf_stem_length(leaf, leaf_length);
  return bytes_equal_literal(leaf, stem_length, "test") ||
         bytes_equal_literal(leaf, stem_length, "spec") ||
         bytes_starts_with_literal(leaf, leaf_length, "test_") ||
         bytes_starts_with_literal(leaf, leaf_length, "test-") ||
         bytes_contains_literal(leaf, leaf_length, ".test.") ||
         bytes_contains_literal(leaf, leaf_length, ".spec.") ||
         bytes_contains_literal(leaf, leaf_length, "_test.") ||
         bytes_contains_literal(leaf, leaf_length, "-test.") ||
         bytes_ends_with_literal(leaf, leaf_length, "_test");
}

static int role_is_test_or_fixture(const AbString *role) {
  return text_is_role_family(role, "test") || text_is(role, "tests") ||
         text_is_role_family(role, "fixture") || text_is(role, "fixtures") ||
         text_is_role_family(role, "mock") || text_is(role, "mocks");
}

static int build_action_leaf(const char *leaf, size_t leaf_length) {
  static const char *const actions[] = {
      "build", "bundle",   "codegen", "compile",    "configure", "generate",
      "pack",  "postpack", "prepack", "prepublish", "release"};
  size_t stem_length = leaf_stem_length(leaf, leaf_length);
  size_t index;
  for (index = 0; index < sizeof(actions) / sizeof(actions[0]); index++) {
    size_t action_length = strlen(actions[index]);
    if (stem_length == action_length &&
        !memcmp(leaf, actions[index], action_length))
      return 1;
    if (stem_length > action_length &&
        !memcmp(leaf, actions[index], action_length) &&
        (leaf[action_length] == '-' || leaf[action_length] == '_'))
      return 1;
  }
  return 0;
}

static int path_is_build_or_artifact(const AbProjectionItem *item,
                                     const AbString *path) {
  static const char *const segments[] = {
      "artifact",  "artifacts",   "build",       "cmake",  "dist",
      "generated", "third_party", "third-party", "vendor", "vendored"};
  static const char *const exact_leaves[] = {
      "BUILD",       "BUILD.bazel",     "CMakeLists.txt", "Dockerfile",
      "GNUmakefile", "Gruntfile.js",    "Makefile",       "SConstruct",
      "WORKSPACE",   "WORKSPACE.bazel", "configure.ac",   "conanfile.py",
      "gulpfile.js", "meson.build",     "setup.cfg",      "setup.py"};
  static const char *const tool_prefixes[] = {
      "babel.",  "esbuild.",  "gulpfile.", "jest.",   "postcss.",
      "rollup.", "tailwind.", "vite.",     "webpack."};
  const AbString *layer_role = item_text(item, "layer_role");
  const char *leaf;
  size_t leaf_length;
  size_t index;
  if (!path)
    return 0;
  if (text_is_role_family(layer_role, "artifact") ||
      text_is_role_family(layer_role, "build") ||
      text_is_role_family(layer_role, "generated") ||
      text_is_role_family(layer_role, "vendor") ||
      text_is(layer_role, "tooling"))
    return 1;
  for (index = 0; index < sizeof(segments) / sizeof(segments[0]); index++)
    if (path_has_segment(path, segments[index]))
      return 1;
  if (path_has_segment_pair(path, "public", "wasm"))
    return 1;
  path_leaf(path, &leaf, &leaf_length);
  for (index = 0; index < sizeof(exact_leaves) / sizeof(exact_leaves[0]);
       index++)
    if (bytes_equal_literal(leaf, leaf_length, exact_leaves[index]))
      return 1;
  if (bytes_contains_literal(leaf, leaf_length, ".config."))
    return 1;
  for (index = 0; index < sizeof(tool_prefixes) / sizeof(tool_prefixes[0]);
       index++)
    if (bytes_starts_with_literal(leaf, leaf_length, tool_prefixes[index]))
      return 1;
  return (path_has_segment(path, "scripts") ||
          path_has_segment(path, "build-scripts") ||
          path_has_segment(path, "build_scripts")) &&
         build_action_leaf(leaf, leaf_length);
}

typedef enum {
  LANDMARK_PRODUCTION = 0,
  LANDMARK_TEST = 1,
  LANDMARK_BUILD = 2,
  LANDMARK_CATEGORY_COUNT = 3
} LandmarkCategory;

typedef struct {
  size_t totals[LANDMARK_CATEGORY_COUNT];
  size_t shown[LANDMARK_CATEGORY_COUNT];
  size_t frontier_total;
  size_t frontier_shown;
} LandmarkReportSummary;

static LandmarkCategory landmark_category(const AbProjectionItem *item) {
  const AbString *path = item_text(item, "path");
  if (path_is_test_or_fixture(path) ||
      role_is_test_or_fixture(item_text(item, "layer_role")))
    return LANDMARK_TEST;
  if (path_is_build_or_artifact(item, path))
    return LANDMARK_BUILD;
  return LANDMARK_PRODUCTION;
}

static int compare_landmark_items(const AbProjectionItem *a,
                                  const AbProjectionItem *b) {
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

static int landmark_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  return compare_landmark_items(a, b);
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

static ArchbirdStatus render_landmark_row(AbBuffer *out,
                                          const AbProjectionItem *item,
                                          int standard_detail) {
  const AbString *path = item_text(item, "path");
  if (standard_detail)
    return ab_report_linef(
        out,
        "- `%.*s` - used-by=%" PRIu64 "; uses=%" PRIu64
        "; relation-witnesses=%" PRIu64 "; symbols=%" PRIu64,
        path ? (int)path->length : (int)item->label.length,
        path ? path->data : item->label.data, item_u64(item, "used_by_count"),
        item_u64(item, "uses_count"), item_u64(item, "relation_witness_count"),
        item_u64(item, "symbol_count"));
  return ab_report_linef(
      out, "- `%.*s` - relations=%" PRIu64 "; symbols=%" PRIu64,
      path ? (int)path->length : (int)item->label.length,
      path ? path->data : item->label.data,
      item_u64(item, "relation_witness_count"), item_u64(item, "symbol_count"));
}

static int nullable_string_compare(const AbString *left,
                                   const AbString *right) {
  if (!left || !left->length)
    return right && right->length ? -1 : 0;
  if (!right || !right->length)
    return 1;
  return ab_string_compare(left, right);
}

static int diagnostic_path_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  int order =
      nullable_string_compare(item_text(a, "path"), item_text(b, "path"));
  if (order)
    return order;
  order = nullable_string_compare(item_text(a, "severity"),
                                  item_text(b, "severity"));
  if (order)
    return order;
  order = nullable_string_compare(item_text(a, "code"), item_text(b, "code"));
  if (order)
    return order;
  order =
      nullable_string_compare(item_text(a, "message"), item_text(b, "message"));
  return order ? order : ab_string_compare(&a->key, &b->key);
}

static int same_diagnostic_cause(const AbProjectionItem *left,
                                 const AbProjectionItem *right) {
  return !nullable_string_compare(item_text(left, "severity"),
                                  item_text(right, "severity")) &&
         !nullable_string_compare(item_text(left, "code"),
                                  item_text(right, "code")) &&
         !nullable_string_compare(item_text(left, "message"),
                                  item_text(right, "message"));
}

typedef struct {
  const AbString *path;
  const AbProjectionItem *node;
  size_t occurrences;
  size_t causes;
} UnresolvedFrontierPath;

static int unresolved_frontier_compare(const void *left, const void *right) {
  const UnresolvedFrontierPath *a = (const UnresolvedFrontierPath *)left;
  const UnresolvedFrontierPath *b = (const UnresolvedFrontierPath *)right;
  if (a->occurrences != b->occurrences)
    return a->occurrences > b->occurrences ? -1 : 1;
  if (a->causes != b->causes)
    return a->causes > b->causes ? -1 : 1;
  if (a->node && b->node) {
    int order = compare_landmark_items(a->node, b->node);
    if (order)
      return order;
  } else if (a->node || b->node) {
    return a->node ? -1 : 1;
  }
  return nullable_string_compare(a->path, b->path);
}

static int file_path_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  int order =
      nullable_string_compare(item_text(a, "path"), item_text(b, "path"));
  return order ? order : ab_string_compare(&a->key, &b->key);
}

static const AbProjectionItem *
file_node_for_path(const AbProjectionItem *const *files, size_t file_count,
                   const AbString *path) {
  size_t low = 0;
  size_t high = file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int order = nullable_string_compare(item_text(files[middle], "path"), path);
    if (order < 0)
      low = middle + 1;
    else
      high = middle;
  }
  if (low < file_count &&
      !nullable_string_compare(item_text(files[low], "path"), path))
    return files[low];
  return NULL;
}

static ArchbirdStatus
render_unresolved_frontier(AbBuffer *out, const AbProjectionData *data,
                           const AbProjectionItem *const *files,
                           size_t file_count, size_t limit,
                           LandmarkReportSummary *summary) {
  const AbProjectionItem **items = NULL;
  UnresolvedFrontierPath *paths = NULL;
  size_t item_count = 0;
  size_t path_count = 0;
  size_t index;
  size_t shown;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; index < data->item_count; index++)
    if (item_is(&data->items[index], "diagnostic") &&
        text_is(item_text(&data->items[index], "code"), "unresolved-import") &&
        item_text(&data->items[index], "path") &&
        item_text(&data->items[index], "path")->length)
      item_count++;
  if (!item_count)
    return ARCHBIRD_OK;
  items = (const AbProjectionItem **)ab_calloc(out->engine, item_count,
                                               sizeof(*items));
  paths = (UnresolvedFrontierPath *)ab_calloc(out->engine, item_count,
                                              sizeof(*paths));
  if (!items || !paths) {
    ab_free(out->engine, items);
    ab_free(out->engine, paths);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  item_count = 0;
  for (index = 0; index < data->item_count; index++)
    if (item_is(&data->items[index], "diagnostic") &&
        text_is(item_text(&data->items[index], "code"), "unresolved-import") &&
        item_text(&data->items[index], "path") &&
        item_text(&data->items[index], "path")->length)
      items[item_count++] = &data->items[index];
  qsort(items, item_count, sizeof(*items), diagnostic_path_compare);
  for (index = 0; index < item_count;) {
    size_t end = index + 1;
    size_t causes = 1;
    const AbString *path = item_text(items[index], "path");
    while (end < item_count &&
           !nullable_string_compare(path, item_text(items[end], "path"))) {
      if (!same_diagnostic_cause(items[end - 1], items[end]))
        causes++;
      end++;
    }
    paths[path_count].path = path;
    paths[path_count].node = file_node_for_path(files, file_count, path);
    paths[path_count].occurrences = end - index;
    paths[path_count].causes = causes;
    path_count++;
    index = end;
  }
  qsort(paths, path_count, sizeof(*paths), unresolved_frontier_compare);
  shown = path_count < limit ? path_count : limit;
  summary->frontier_total = path_count;
  summary->frontier_shown = shown;
  if (shown) {
    status = ab_report_literal_line(out, "### Unresolved frontier");
    if (status == ARCHBIRD_OK)
      status = ab_report_blank(out);
    if (status == ARCHBIRD_OK)
      status = ab_report_linef(
          out,
          "Canonical `unresolved-import` evidence by source path; showing %zu "
          "of %zu paths. This is a second lens, so a path can also appear "
          "above.",
          shown, path_count);
    if (status == ARCHBIRD_OK)
      status = ab_report_blank(out);
  }
  for (index = 0; status == ARCHBIRD_OK && index < shown; index++) {
    const UnresolvedFrontierPath *path = &paths[index];
    status =
        ab_report_appendf(out, "- `%.*s` - unresolved-imports=%zu; causes=%zu",
                          (int)path->path->length, path->path->data,
                          path->occurrences, path->causes);
    if (status == ARCHBIRD_OK && path->node)
      status = ab_report_appendf(
          out, "; relation-witnesses=%" PRIu64 "; symbols=%" PRIu64,
          item_u64(path->node, "relation_witness_count"),
          item_u64(path->node, "symbol_count"));
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(out, "\n");
  }
  if (status == ARCHBIRD_OK && shown)
    status = ab_report_blank(out);
  ab_free(out->engine, paths);
  ab_free(out->engine, items);
  return status;
}

static void allocate_landmark_slots(const size_t *totals, size_t limit,
                                    size_t *shown) {
  static const LandmarkCategory fill_order[] = {LANDMARK_PRODUCTION,
                                                LANDMARK_TEST, LANDMARK_BUILD};
  size_t requested[LANDMARK_CATEGORY_COUNT];
  size_t remaining;
  size_t index;
  requested[LANDMARK_PRODUCTION] = (limit + 1) / 2;
  requested[LANDMARK_TEST] = (limit + 2) / 4;
  requested[LANDMARK_BUILD] =
      limit - requested[LANDMARK_PRODUCTION] - requested[LANDMARK_TEST];
  for (index = 0; index < LANDMARK_CATEGORY_COUNT; index++)
    shown[index] =
        totals[index] < requested[index] ? totals[index] : requested[index];
  remaining = limit - shown[LANDMARK_PRODUCTION] - shown[LANDMARK_TEST] -
              shown[LANDMARK_BUILD];
  for (index = 0;
       remaining && index < sizeof(fill_order) / sizeof(fill_order[0]);
       index++) {
    LandmarkCategory category = fill_order[index];
    size_t available = totals[category] - shown[category];
    size_t added = available < remaining ? available : remaining;
    shown[category] += added;
    remaining -= added;
  }
}

static ArchbirdStatus
render_categorized_landmarks(AbBuffer *out, const AbProjectionData *data,
                             const AbProjectionItem **items, size_t count,
                             size_t limit, int standard_detail, size_t *omitted,
                             LandmarkReportSummary *summary) {
  static const char *const headings[LANDMARK_CATEGORY_COUNT] = {
      "Production and API candidates", "Tests and fixtures",
      "Build and artifact paths"};
  size_t emitted[LANDMARK_CATEGORY_COUNT] = {0};
  size_t category;
  size_t index;
  size_t shown_total = 0;
  size_t frontier_limit = limit < 4 ? limit : 4;
  ArchbirdStatus status;
  memset(summary, 0, sizeof(*summary));
  for (index = 0; index < count; index++)
    summary->totals[landmark_category(items[index])]++;
  allocate_landmark_slots(summary->totals, limit, summary->shown);
  status = ab_report_literal_line(out, "## File landmarks");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "Selected files by presentation category: production/API=%zu; "
        "test/fixture=%zu; build/artifact=%zu.",
        summary->totals[LANDMARK_PRODUCTION], summary->totals[LANDMARK_TEST],
        summary->totals[LANDMARK_BUILD]);
  if (status == ARCHBIRD_OK)
    status = ab_report_literal_line(
        out,
        "Ranked within each category by relation witnesses, then graph degree, "
        "symbols, and path. These are orientation signals, not correctness "
        "claims.");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  for (category = 0;
       status == ARCHBIRD_OK && category < LANDMARK_CATEGORY_COUNT;
       category++) {
    if (!summary->shown[category])
      continue;
    status = ab_report_linef(out, "### %s", headings[category]);
    if (status == ARCHBIRD_OK)
      status = ab_report_blank(out);
    for (index = 0; status == ARCHBIRD_OK && index < count &&
                    emitted[category] < summary->shown[category];
         index++) {
      if ((size_t)landmark_category(items[index]) != category)
        continue;
      status = render_landmark_row(out, items[index], standard_detail);
      emitted[category]++;
    }
    if (status == ARCHBIRD_OK)
      status = ab_report_blank(out);
    shown_total += emitted[category];
  }
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "Category shortlist: production/API=%zu/%zu; test/fixture=%zu/%zu; "
        "build/artifact=%zu/%zu.",
        summary->shown[LANDMARK_PRODUCTION],
        summary->totals[LANDMARK_PRODUCTION], summary->shown[LANDMARK_TEST],
        summary->totals[LANDMARK_TEST], summary->shown[LANDMARK_BUILD],
        summary->totals[LANDMARK_BUILD]);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK) {
    qsort(items, count, sizeof(*items), file_path_compare);
    status = render_unresolved_frontier(out, data, items, count, frontier_limit,
                                        summary);
  }
  *omitted += count - shown_total;
  return status;
}

static ArchbirdStatus render_landmarks(AbBuffer *out,
                                       const AbProjectionData *data,
                                       const char *heading, size_t limit,
                                       int standard_detail, int connected_only,
                                       size_t *omitted,
                                       LandmarkReportSummary *summary) {
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
  if (!connected_only) {
    status = render_categorized_landmarks(out, data, items, count, limit,
                                          standard_detail, omitted, summary);
    ab_free(out->engine, items);
    return status;
  }
  shown = count < limit ? count : limit;
  status = ab_report_linef(out, "## %s", heading);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  for (index = 0; status == ARCHBIRD_OK && index < shown; index++)
    status = render_landmark_row(out, items[index], standard_detail);
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

enum { DIAGNOSTIC_REPRESENTATIVE_LIMIT = 3 };

typedef struct {
  const AbString *path;
  size_t occurrences;
} DiagnosticRepresentative;

typedef struct {
  const AbProjectionItem *item;
  size_t occurrences;
  size_t distinct_paths;
  DiagnosticRepresentative representatives[DIAGNOSTIC_REPRESENTATIVE_LIMIT];
  size_t representative_count;
} DiagnosticGroup;

static int diagnostic_severity_rank(const AbString *severity) {
  if (text_is(severity, "error"))
    return 0;
  if (text_is(severity, "warning"))
    return 1;
  if (text_is(severity, "info"))
    return 2;
  return 3;
}

static int diagnostic_cause_path_compare(const void *left, const void *right) {
  const AbProjectionItem *a = *(const AbProjectionItem *const *)left;
  const AbProjectionItem *b = *(const AbProjectionItem *const *)right;
  const AbString *a_severity = item_text(a, "severity");
  const AbString *b_severity = item_text(b, "severity");
  int a_rank = diagnostic_severity_rank(a_severity);
  int b_rank = diagnostic_severity_rank(b_severity);
  int order;
  if (a_rank != b_rank)
    return a_rank < b_rank ? -1 : 1;
  order = nullable_string_compare(a_severity, b_severity);
  if (order)
    return order;
  order = nullable_string_compare(item_text(a, "code"), item_text(b, "code"));
  if (order)
    return order;
  order =
      nullable_string_compare(item_text(a, "message"), item_text(b, "message"));
  if (order)
    return order;
  order = nullable_string_compare(item_text(a, "path"), item_text(b, "path"));
  return order ? order : ab_string_compare(&a->key, &b->key);
}

static int diagnostic_group_compare(const void *left, const void *right) {
  const DiagnosticGroup *a = (const DiagnosticGroup *)left;
  const DiagnosticGroup *b = (const DiagnosticGroup *)right;
  const AbString *a_severity = item_text(a->item, "severity");
  const AbString *b_severity = item_text(b->item, "severity");
  int a_rank = diagnostic_severity_rank(a_severity);
  int b_rank = diagnostic_severity_rank(b_severity);
  int order;
  if (a_rank != b_rank)
    return a_rank < b_rank ? -1 : 1;
  if (a->occurrences != b->occurrences)
    return a->occurrences > b->occurrences ? -1 : 1;
  if (a->distinct_paths != b->distinct_paths)
    return a->distinct_paths > b->distinct_paths ? -1 : 1;
  order = nullable_string_compare(a_severity, b_severity);
  if (order)
    return order;
  order = nullable_string_compare(item_text(a->item, "code"),
                                  item_text(b->item, "code"));
  if (order)
    return order;
  return nullable_string_compare(item_text(a->item, "message"),
                                 item_text(b->item, "message"));
}

static int
diagnostic_representative_better(const DiagnosticRepresentative *left,
                                 const DiagnosticRepresentative *right) {
  if (left->occurrences != right->occurrences)
    return left->occurrences > right->occurrences;
  return nullable_string_compare(left->path, right->path) < 0;
}

static void diagnostic_group_add_representative(DiagnosticGroup *group,
                                                const AbString *path,
                                                size_t occurrences) {
  DiagnosticRepresentative candidate = {path, occurrences};
  size_t position;
  size_t move;
  if (group->representative_count < DIAGNOSTIC_REPRESENTATIVE_LIMIT) {
    position = group->representative_count++;
  } else {
    position = group->representative_count - 1;
    if (!diagnostic_representative_better(&candidate,
                                          &group->representatives[position]))
      return;
  }
  group->representatives[position] = candidate;
  for (move = position; move && diagnostic_representative_better(
                                    &group->representatives[move],
                                    &group->representatives[move - 1]);
       move--) {
    DiagnosticRepresentative swap = group->representatives[move - 1];
    group->representatives[move - 1] = group->representatives[move];
    group->representatives[move] = swap;
  }
}

typedef struct {
  const char *label;
  const char *hint;
} DiagnosticReviewGuidance;

static DiagnosticReviewGuidance
diagnostic_review_guidance(const AbString *code) {
  if (text_is(code, "unresolved-import"))
    return (DiagnosticReviewGuidance){
        "unresolved-import",
        "Check `layers[].import_roots` and "
        "`packages[].{path,identity,aliases}` against package/workspace "
        "evidence before classifying the name as internal or external."};
  if (text_is(code, "package-export-surface-partial"))
    return (DiagnosticReviewGuidance){
        "package-export-surface-partial",
        "Inspect the package manifest export syntax and cited entrypoint; "
        "this warning marks incomplete static export coverage, not a proven "
        "missing runtime export."};
  if (text_is(code, "python-ast-inapplicable"))
    return (DiagnosticReviewGuidance){
        "python-ast-inapplicable",
        "Check the cited file's Python dialect and provider support; this is "
        "source/provider coverage, not a project-configuration claim."};
  if (code &&
      bytes_starts_with_literal(code->data, code->length, "tree-sitter-"))
    return (DiagnosticReviewGuidance){
        "tree-sitter-*",
        "Inspect the cited source and parser recovery before treating that "
        "file's extracted structure as complete."};
  return (DiagnosticReviewGuidance){0};
}

static ArchbirdStatus render_diagnostic_guidance(AbBuffer *out,
                                                 const DiagnosticGroup *groups,
                                                 size_t shown) {
  size_t index;
  int heading = 0;
  for (index = 0; index < shown; index++) {
    const AbString *code = item_text(groups[index].item, "code");
    DiagnosticReviewGuidance guidance = diagnostic_review_guidance(code);
    size_t previous;
    if (!guidance.hint)
      continue;
    for (previous = 0; previous < index; previous++) {
      DiagnosticReviewGuidance candidate =
          diagnostic_review_guidance(item_text(groups[previous].item, "code"));
      if (candidate.label && !strcmp(guidance.label, candidate.label))
        break;
    }
    if (previous < index)
      continue;
    if (!heading) {
      REPORT_TRY(ab_report_literal_line(out, "Review guidance:"));
      heading = 1;
    }
    REPORT_TRY(
        ab_report_linef(out, "- `%s`: %s", guidance.label, guidance.hint));
  }
  return heading ? ab_report_blank(out) : ARCHBIRD_OK;
}

static ArchbirdStatus
render_diagnostic_representatives(AbBuffer *out, const DiagnosticGroup *group,
                                  int standard_detail) {
  size_t visible = group->representative_count;
  size_t index;
  if (!standard_detail && visible > 2)
    visible = 2;
  REPORT_TRY(ab_buffer_literal(out, "  - Representative paths: "));
  for (index = 0; index < visible; index++) {
    const DiagnosticRepresentative *representative =
        &group->representatives[index];
    if (index)
      REPORT_TRY(ab_buffer_literal(out, ", "));
    if (representative->path && representative->path->length)
      REPORT_TRY(ab_report_appendf(out, "`%.*s`",
                                   (int)representative->path->length,
                                   representative->path->data));
    else
      REPORT_TRY(ab_buffer_literal(out, "(project-level)"));
    REPORT_TRY(ab_report_appendf(out, " (%zu)", representative->occurrences));
  }
  if (visible < group->distinct_paths)
    REPORT_TRY(ab_report_appendf(
        out, "; +%zu more path%s", group->distinct_paths - visible,
        group->distinct_paths - visible == 1 ? "" : "s"));
  return ab_buffer_literal(out, "\n");
}

static ArchbirdStatus
render_diagnostic_groups(AbBuffer *out, const AbProjectionData *data,
                         size_t limit, int standard_detail, size_t *omitted) {
  const AbProjectionItem **items = NULL;
  DiagnosticGroup *groups = NULL;
  size_t total = 0;
  size_t group_count = 0;
  size_t shown;
  size_t shown_records = 0;
  size_t index;
  ArchbirdStatus status =
      collect_items(out, data, "diagnostic", NULL, &items, &total);
  if (status != ARCHBIRD_OK || !total)
    return status;
  groups = (DiagnosticGroup *)ab_calloc(out->engine, total, sizeof(*groups));
  if (!groups) {
    ab_free(out->engine, items);
    return ARCHBIRD_OUT_OF_MEMORY;
  }
  qsort(items, total, sizeof(*items), diagnostic_cause_path_compare);
  for (index = 0; index < total;) {
    DiagnosticGroup *group = &groups[group_count++];
    size_t end = index + 1;
    size_t path_start;
    group->item = items[index];
    while (end < total && same_diagnostic_cause(items[index], items[end]))
      end++;
    group->occurrences = end - index;
    for (path_start = index; path_start < end;) {
      const AbString *path = item_text(items[path_start], "path");
      size_t path_end = path_start + 1;
      while (path_end < end &&
             !nullable_string_compare(path, item_text(items[path_end], "path")))
        path_end++;
      group->distinct_paths++;
      diagnostic_group_add_representative(group, path, path_end - path_start);
      path_start = path_end;
    }
    index = end;
  }
  qsort(groups, group_count, sizeof(*groups), diagnostic_group_compare);
  shown = group_count < limit ? group_count : limit;
  status = ab_report_literal_line(out, "## Diagnostics");
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK)
    status = ab_report_linef(
        out,
        "%zu canonical record%s grouped into %zu exact cause%s by severity, "
        "code, and message; showing %zu. Counts summarize canonical JSON "
        "without changing it.",
        total, total == 1 ? "" : "s", group_count, group_count == 1 ? "" : "s",
        shown);
  if (status == ARCHBIRD_OK)
    status = ab_report_blank(out);
  if (status == ARCHBIRD_OK && standard_detail)
    status = render_diagnostic_guidance(out, groups, shown);
  for (index = 0; status == ARCHBIRD_OK && index < shown; index++) {
    const DiagnosticGroup *group = &groups[index];
    const AbString *severity = item_text(group->item, "severity");
    const AbString *code = item_text(group->item, "code");
    const AbString *message = item_text(group->item, "message");
    status = ab_report_linef(
        out, "- **%.*s** `%.*s` - occurrences=%zu; paths=%zu; %.*s",
        severity ? (int)severity->length : 0, severity ? severity->data : "",
        code ? (int)code->length : 0, code ? code->data : "",
        group->occurrences, group->distinct_paths,
        message ? (int)message->length : 0, message ? message->data : "");
    if (status == ARCHBIRD_OK)
      status = render_diagnostic_representatives(out, group, standard_detail);
    shown_records += group->occurrences;
  }
  if (status == ARCHBIRD_OK && shown < group_count) {
    size_t omitted_records = total - shown_records;
    status = ab_report_linef(
        out, "- ... %zu cause group%s (%zu canonical record%s) omitted",
        group_count - shown, group_count - shown == 1 ? "" : "s",
        omitted_records, omitted_records == 1 ? "" : "s");
  }
  *omitted += total - shown_records;
  ab_free(out->engine, groups);
  ab_free(out->engine, items);
  return status == ARCHBIRD_OK ? ab_report_blank(out) : status;
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
  GraphReportSummary summary;
  LandmarkReportSummary landmarks = {0};
  size_t omitted = 0;
  size_t landmark_limit = limit < 12 ? limit : 12;
  REPORT_TRY(graph_report_summary(out, &result->data, &summary));
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
          landmark_limit, standard_detail, tests_only, &omitted, &landmarks));
    REPORT_TRY(render_inventory_groups(out, &result->data));
    REPORT_TRY(ab_report_literal_line(out, "## Presentation accounting"));
    REPORT_TRY(ab_report_blank(out));
    if (!evidence_only && !tests_only)
      REPORT_TRY(ab_report_linef(
          out,
          "- Canonical entities: %zu; canonical relations: %zu; aggregated "
          "group flows: %zu; category shortlist: production/API=%zu/%zu, "
          "test/fixture=%zu/%zu, build/artifact=%zu/%zu; unresolved "
          "frontier=%zu/%zu.",
          count_kind(&result->data, "node"),
          count_kind(&result->data, "relation"),
          count_kind(&result->data, "group_relation"),
          landmarks.shown[LANDMARK_PRODUCTION],
          landmarks.totals[LANDMARK_PRODUCTION], landmarks.shown[LANDMARK_TEST],
          landmarks.totals[LANDMARK_TEST], landmarks.shown[LANDMARK_BUILD],
          landmarks.totals[LANDMARK_BUILD], landmarks.frontier_shown,
          landmarks.frontier_total));
    else
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
  if (full_detail)
    REPORT_TRY(render_section(out, &result->data, "diagnostic", "Diagnostics",
                              limit, standard_detail, 1, &omitted));
  else
    REPORT_TRY(render_diagnostic_groups(out, &result->data, limit,
                                        standard_detail, &omitted));
  REPORT_TRY(render_ledgers(out, &result->data));
  REPORT_TRY(ab_report_literal_line(out, "## Graph completeness"));
  REPORT_TRY(ab_report_blank(out));
  REPORT_TRY(ab_report_linef(out,
                             "Result: files=%zu; indexed-symbols=%" PRIu64
                             "; entities=%zu; relations=%zu; diagnostics=%zu "
                             "(errors=%zu warnings=%zu).",
                             summary.files, summary.symbols, summary.entities,
                             summary.relations, summary.diagnostics,
                             summary.errors, summary.warnings));
  REPORT_TRY(ab_report_linef(
      out,
      "Evidence: graph=%s; exhaustive=%s; unknown=%" PRIu64
      "; unsupported=%" PRIu64 "; presentation-omitted=%zu; projection=`%s`.",
      classification, !strcmp(classification, "complete") ? "yes" : "no",
      result->data.selection.has_unknown ? result->data.selection.unknown : 0,
      result->data.selection.has_unsupported
          ? result->data.selection.unsupported
          : 0,
      omitted, result->result_sha256));
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
