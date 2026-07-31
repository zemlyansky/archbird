#include "plan_internal.h"

#include "archbird_internal.h"
#include "artifact_validation.h"
#include "sha256.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AB_PLAN_MAX_ROWS 4096u
#define AB_PLAN_MAX_METADATA (64u * 1024u)
#define AB_PLAN_MAX_OPERATION_TEXT (16u * 1024u * 1024u)

typedef struct PlanDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} PlanDigestWriter;

typedef struct PlanIdIndex {
  const AbString *id;
  size_t index;
} PlanIdIndex;

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  PlanDigestWriter *writer = (PlanDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "plan: %s", message);
}

static int text_is(const AbValue *value, const char *literal) {
  return ab_artifact_text_is(value, literal);
}

static int bounded_text(const AbValue *value, size_t maximum, int nonempty) {
  return ab_artifact_bounded_text(value, maximum, nonempty);
}

static int lowercase_sha256(const AbValue *value) {
  return ab_artifact_sha256(value);
}

static int stable_id(const AbValue *value) {
  return ab_artifact_stable_id(value);
}

static int portable_identifier(const AbValue *value) {
  size_t index;
  if (!bounded_text(value, 256, 1))
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned char byte = (unsigned char)value->as.text.data[index];
    if (index == 0) {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            byte == '_'))
        return 0;
    } else if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                 (byte >= '0' && byte <= '9') || byte == '_')) {
      return 0;
    }
  }
  return 1;
}

static int repository_path(const AbValue *value) {
  return ab_artifact_repository_path(value);
}

static int safe_integer(const AbValue *value, uint64_t *out) {
  return ab_artifact_safe_integer(value, out);
}

static int boolean_value(const AbValue *value) {
  return ab_artifact_boolean(value);
}

static int object_exact(const AbValue *value, const char *const *fields,
                        size_t count) {
  return ab_artifact_object_exact(value, fields, count);
}

static int array_value(const AbValue *value, size_t minimum) {
  return value && value->kind == AB_VALUE_ARRAY &&
         value->as.array.count >= minimum &&
         value->as.array.count <= AB_PLAN_MAX_ROWS;
}

static int string_array(const AbValue *value, int ids, size_t minimum) {
  size_t first;
  size_t second;
  if (!array_value(value, minimum))
    return 0;
  for (first = 0; first < value->as.array.count; first++) {
    const AbValue *row = &value->as.array.items[first];
    if ((ids && !stable_id(row)) ||
        (!ids && !bounded_text(row, AB_PLAN_MAX_METADATA, 1)))
      return 0;
    for (second = 0; second < first; second++)
      if (ab_value_equal(row, &value->as.array.items[second]))
        return 0;
  }
  return 1;
}

static int id_index_compare(const void *left, const void *right) {
  const PlanIdIndex *a = (const PlanIdIndex *)left;
  const PlanIdIndex *b = (const PlanIdIndex *)right;
  return ab_string_compare(a->id, b->id);
}

static size_t id_index_find(const PlanIdIndex *index, size_t count,
                            const AbString *id) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(index[middle].id, id);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return index[middle].index;
  }
  return SIZE_MAX;
}

static int unique_rows(const AbValue *value) {
  size_t first;
  size_t second;
  if (!array_value(value, 0))
    return 0;
  for (first = 0; first < value->as.array.count; first++)
    for (second = 0; second < first; second++)
      if (ab_value_equal(&value->as.array.items[first],
                         &value->as.array.items[second]))
        return 0;
  return 1;
}

static int validate_tool(const AbValue *value) {
  static const char *const fields[] = {"name", "version",
                                       "implementation_sha256"};
  return object_exact(value, fields, 3) &&
         text_is(ab_value_member(value, "name"), "archbird") &&
         bounded_text(ab_value_member(value, "version"), 256, 1) &&
         lowercase_sha256(ab_value_member(value, "implementation_sha256"));
}

static int validate_identity(const AbValue *value, int map) {
  static const char *const map_fields[] = {"sha256", "input_sha256",
                                           "configuration_sha256",
                                           "producer_implementation_sha256"};
  static const char *const verification_fields[] = {
      "sha256", "policy_sha256", "producer_implementation_sha256"};
  const char *const *fields = map ? map_fields : verification_fields;
  size_t count = map ? 4 : 3;
  size_t index;
  if (!object_exact(value, fields, count))
    return 0;
  for (index = 0; index < count; index++)
    if (!lowercase_sha256(ab_value_member(value, fields[index])))
      return 0;
  return 1;
}

static int validate_source(const AbValue *value) {
  static const char *const fields[] = {"project", "map", "verification"};
  static const char *const observed_fields[] = {"before_map", "project", "map",
                                                "verification"};
  const AbValue *before = ab_value_member(value, "before_map");
  return (object_exact(value, fields, 3) ||
          object_exact(value, observed_fields, 4)) &&
         bounded_text(ab_value_member(value, "project"), AB_PLAN_MAX_METADATA,
                      1) &&
         (!before || validate_identity(before, 1)) &&
         validate_identity(ab_value_member(value, "map"), 1) &&
         validate_identity(ab_value_member(value, "verification"), 0);
}

static int validate_origin(const AbValue *value) {
  static const char *const fields[] = {
      "constraint_id", "constraint_result_sha256", "issue_fingerprint"};
  return object_exact(value, fields, 3) &&
         stable_id(ab_value_member(value, "constraint_id")) &&
         lowercase_sha256(ab_value_member(value, "constraint_result_sha256")) &&
         lowercase_sha256(ab_value_member(value, "issue_fingerprint"));
}

static int validate_evidence(const AbValue *value) {
  static const char *const fields[] = {"provenance", "project", "path",
                                       "line",       "sha256",  "detail"};
  const AbValue *provenance;
  const AbValue *path;
  const AbValue *sha256;
  if (!object_exact(value, fields, 6))
    return 0;
  provenance = ab_value_member(value, "provenance");
  path = ab_value_member(value, "path");
  sha256 = ab_value_member(value, "sha256");
  return (text_is(provenance, "derived") || text_is(provenance, "asserted") ||
          text_is(provenance, "observed")) &&
         bounded_text(ab_value_member(value, "project"), AB_PLAN_MAX_METADATA,
                      0) &&
         (bounded_text(path, 0, 0) || repository_path(path)) &&
         safe_integer(ab_value_member(value, "line"), NULL) &&
         (bounded_text(sha256, 0, 0) || lowercase_sha256(sha256)) &&
         bounded_text(ab_value_member(value, "detail"), AB_PLAN_MAX_METADATA,
                      0);
}

static int validate_unknown(const AbValue *value) {
  static const char *const fields[] = {"id", "statement", "item_id",
                                       "constraint_id"};
  const AbValue *item;
  const AbValue *constraint;
  if (!object_exact(value, fields, 4))
    return 0;
  item = ab_value_member(value, "item_id");
  constraint = ab_value_member(value, "constraint_id");
  return stable_id(ab_value_member(value, "id")) &&
         bounded_text(ab_value_member(value, "statement"), AB_PLAN_MAX_METADATA,
                      1) &&
         (item->kind == AB_VALUE_NULL || stable_id(item)) &&
         (constraint->kind == AB_VALUE_NULL || stable_id(constraint));
}

static int validate_candidate_site(const AbValue *value) {
  static const char *const fields[] = {
      "fact_id",    "path",     "line",   "source_sha256",
      "start_byte", "end_byte", "before", "name"};
  uint64_t start;
  uint64_t end;
  return object_exact(value, fields, 8) &&
         bounded_text(ab_value_member(value, "fact_id"), AB_PLAN_MAX_METADATA,
                      1) &&
         repository_path(ab_value_member(value, "path")) &&
         safe_integer(ab_value_member(value, "line"), NULL) &&
         lowercase_sha256(ab_value_member(value, "source_sha256")) &&
         safe_integer(ab_value_member(value, "start_byte"), &start) &&
         safe_integer(ab_value_member(value, "end_byte"), &end) &&
         end > start &&
         bounded_text(ab_value_member(value, "before"), AB_PLAN_MAX_METADATA,
                      1) &&
         bounded_text(ab_value_member(value, "name"), AB_PLAN_MAX_METADATA, 1);
}

static int token_value(const AbValue *value, int nonempty) {
  size_t index;
  if (!bounded_text(value, AB_PLAN_MAX_METADATA, nonempty))
    return 0;
  for (index = 0; index < value->as.text.length; index++)
    if (isspace((unsigned char)value->as.text.data[index]) ||
        value->as.text.data[index] == '#')
      return 0;
  return 1;
}

static int validate_rename_site(const AbValue *value) {
  static const char *const fields[] = {
      "path",   "source_sha256", "start_byte", "end_byte",
      "before", "role",          "fact_ids",   "providers"};
  const AbValue *role;
  uint64_t start;
  uint64_t end;
  if (!object_exact(value, fields, 8))
    return 0;
  role = ab_value_member(value, "role");
  return repository_path(ab_value_member(value, "path")) &&
         lowercase_sha256(ab_value_member(value, "source_sha256")) &&
         safe_integer(ab_value_member(value, "start_byte"), &start) &&
         safe_integer(ab_value_member(value, "end_byte"), &end) &&
         end > start &&
         bounded_text(ab_value_member(value, "before"), AB_PLAN_MAX_METADATA,
                      1) &&
         (text_is(role, "binding") || text_is(role, "declaration") ||
          text_is(role, "export") || text_is(role, "import") ||
          text_is(role, "reference")) &&
         string_array(ab_value_member(value, "fact_ids"), 0, 0) &&
         string_array(ab_value_member(value, "providers"), 0, 0);
}

static int validate_rename_coverage(const AbValue *value, size_t site_count) {
  static const char *const fields[] = {"classification", "exhaustive",
                                       "selected", "unknown", "unsupported"};
  const AbValue *classification;
  uint64_t selected;
  uint64_t unknown;
  uint64_t unsupported;
  if (!object_exact(value, fields, 5))
    return 0;
  classification = ab_value_member(value, "classification");
  return (text_is(classification, "complete") ||
          text_is(classification, "incomplete") ||
          text_is(classification, "unknown")) &&
         boolean_value(ab_value_member(value, "exhaustive")) &&
         safe_integer(ab_value_member(value, "selected"), &selected) &&
         selected == site_count &&
         safe_integer(ab_value_member(value, "unknown"), &unknown) &&
         safe_integer(ab_value_member(value, "unsupported"), &unsupported);
}

static int validate_symbol_projection(const AbValue *value) {
  static const char *const base_fields[] = {"select", "names"};
  static const char *const path_fields[] = {"select", "names", "paths"};
  const AbValue *names;
  const AbValue *paths;
  if (!object_exact(value, base_fields, 2) &&
      !object_exact(value, path_fields, 3))
    return 0;
  names = ab_value_member(value, "names");
  if (!text_is(ab_value_member(value, "select"), "symbol_occurrences") ||
      !array_value(names, 1) || names->as.array.count != 1 ||
      !bounded_text(&names->as.array.items[0], AB_PLAN_MAX_METADATA, 1))
    return 0;
  paths = ab_value_member(value, "paths");
  if (paths) {
    size_t index;
    if (!unique_rows(paths))
      return 0;
    for (index = 0; index < paths->as.array.count; index++)
      if (!repository_path(&paths->as.array.items[index]))
        return 0;
  }
  return 1;
}

static int validate_operation(const AbValue *value, int *out_manual,
                              int *out_requires_asserted) {
  const AbValue *action;
  *out_manual = 0;
  *out_requires_asserted = 0;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return 0;
  action = ab_value_member(value, "action");
  if (text_is(action, "replace_range")) {
    static const char *const fields[] = {
        "action",   "path",   "source_sha256", "start_byte",
        "end_byte", "before", "replacement"};
    uint64_t start;
    uint64_t end;
    return object_exact(value, fields, 7) &&
           repository_path(ab_value_member(value, "path")) &&
           lowercase_sha256(ab_value_member(value, "source_sha256")) &&
           safe_integer(ab_value_member(value, "start_byte"), &start) &&
           safe_integer(ab_value_member(value, "end_byte"), &end) &&
           start <= end &&
           bounded_text(ab_value_member(value, "before"),
                        AB_PLAN_MAX_OPERATION_TEXT, 0) &&
           bounded_text(ab_value_member(value, "replacement"),
                        AB_PLAN_MAX_OPERATION_TEXT, 0);
  }
  if (text_is(action, "create_file")) {
    static const char *const fields[] = {"action", "path", "content"};
    return object_exact(value, fields, 3) &&
           repository_path(ab_value_member(value, "path")) &&
           bounded_text(ab_value_member(value, "content"),
                        AB_PLAN_MAX_OPERATION_TEXT, 0);
  }
  if (text_is(action, "delete_file")) {
    static const char *const fields[] = {"action", "path", "source_sha256"};
    return object_exact(value, fields, 3) &&
           repository_path(ab_value_member(value, "path")) &&
           lowercase_sha256(ab_value_member(value, "source_sha256"));
  }
  if (text_is(action, "move_file")) {
    static const char *const fields[] = {"action", "source_path",
                                         "destination_path", "source_sha256"};
    const AbValue *source;
    const AbValue *destination;
    if (!object_exact(value, fields, 4))
      return 0;
    source = ab_value_member(value, "source_path");
    destination = ab_value_member(value, "destination_path");
    return repository_path(source) && repository_path(destination) &&
           !ab_value_equal(source, destination) &&
           lowercase_sha256(ab_value_member(value, "source_sha256"));
  }
  if (text_is(action, "edit_json_pointer")) {
    static const char *const insert_fields[] = {
        "action",          "path",       "source_sha256", "pointer",
        "expected_absent", "replacement"};
    static const char *const replace_fields[] = {
        "action",          "path",     "source_sha256", "pointer",
        "expected_absent", "expected", "replacement"};
    const AbValue *absent = ab_value_member(value, "expected_absent");
    *out_requires_asserted = 1;
    if (!boolean_value(absent))
      return 0;
    return object_exact(value,
                        absent->as.boolean ? insert_fields : replace_fields,
                        absent->as.boolean ? 6 : 7) &&
           repository_path(ab_value_member(value, "path")) &&
           lowercase_sha256(ab_value_member(value, "source_sha256")) &&
           bounded_text(ab_value_member(value, "pointer"), AB_PLAN_MAX_METADATA,
                        0);
  }
  if (text_is(action, "edit_make_variable_token")) {
    static const char *const fields[] = {"action",         "path",
                                         "source_sha256",  "variable",
                                         "expected_token", "replacement_token"};
    return object_exact(value, fields, 6) &&
           repository_path(ab_value_member(value, "path")) &&
           lowercase_sha256(ab_value_member(value, "source_sha256")) &&
           portable_identifier(ab_value_member(value, "variable")) &&
           token_value(ab_value_member(value, "expected_token"), 1) &&
           token_value(ab_value_member(value, "replacement_token"), 0);
  }
  if (text_is(action, "insert_make_variable_token")) {
    static const char *const fields[] = {"action",   "path",  "source_sha256",
                                         "variable", "token", "anchor_token",
                                         "position"};
    const AbValue *position;
    if (!object_exact(value, fields, 7))
      return 0;
    position = ab_value_member(value, "position");
    return repository_path(ab_value_member(value, "path")) &&
           lowercase_sha256(ab_value_member(value, "source_sha256")) &&
           portable_identifier(ab_value_member(value, "variable")) &&
           token_value(ab_value_member(value, "token"), 1) &&
           token_value(ab_value_member(value, "anchor_token"), 1) &&
           (text_is(position, "before") || text_is(position, "after"));
  }
  if (text_is(action, "rename_symbol")) {
    static const char *const fields[] = {"action",
                                         "symbol",
                                         "new_name",
                                         "projection",
                                         "projection_result_sha256",
                                         "sites",
                                         "coverage"};
    const AbValue *sites;
    size_t index;
    *out_requires_asserted = 1;
    if (!object_exact(value, fields, 7) ||
        !bounded_text(ab_value_member(value, "symbol"), AB_PLAN_MAX_METADATA,
                      1) ||
        !portable_identifier(ab_value_member(value, "new_name")) ||
        !validate_symbol_projection(ab_value_member(value, "projection")) ||
        !lowercase_sha256(ab_value_member(value, "projection_result_sha256")))
      return 0;
    sites = ab_value_member(value, "sites");
    if (!unique_rows(sites) || sites->as.array.count == 0)
      return 0;
    for (index = 0; index < sites->as.array.count; index++)
      if (!validate_rename_site(&sites->as.array.items[index]))
        return 0;
    return validate_rename_coverage(ab_value_member(value, "coverage"),
                                    sites->as.array.count);
  }
  if (text_is(action, "manual")) {
    static const char *const base_fields[] = {"action", "instructions",
                                              "candidate_paths"};
    static const char *const site_fields[] = {
        "action", "instructions", "candidate_paths", "candidate_sites"};
    const AbValue *paths;
    const AbValue *sites;
    size_t index;
    *out_manual = 1;
    if (!object_exact(value, base_fields, 3) &&
        !object_exact(value, site_fields, 4))
      return 0;
    if (!bounded_text(ab_value_member(value, "instructions"),
                      AB_PLAN_MAX_METADATA, 1))
      return 0;
    paths = ab_value_member(value, "candidate_paths");
    if (!unique_rows(paths))
      return 0;
    for (index = 0; index < paths->as.array.count; index++)
      if (!repository_path(&paths->as.array.items[index]))
        return 0;
    sites = ab_value_member(value, "candidate_sites");
    if (sites) {
      if (!unique_rows(sites))
        return 0;
      for (index = 0; index < sites->as.array.count; index++)
        if (!validate_candidate_site(&sites->as.array.items[index]))
          return 0;
    }
    return 1;
  }
  return 0;
}

static int validate_item(const AbValue *value) {
  static const char *const fields[] = {"id",
                                       "statement",
                                       "provenance",
                                       "origins",
                                       "evidence",
                                       "depends_on",
                                       "operation",
                                       "acceptance",
                                       "unknowns",
                                       "executable",
                                       "non_executable_reasons"};
  static const char *const acceptance_fields[] = {"constraints"};
  const AbValue *origins;
  const AbValue *evidence;
  const AbValue *acceptance;
  const AbValue *executable;
  const AbValue *provenance;
  const AbValue *reasons;
  size_t index;
  int manual;
  int requires_asserted;
  if (!object_exact(value, fields, 11) ||
      !stable_id(ab_value_member(value, "id")) ||
      !bounded_text(ab_value_member(value, "statement"), AB_PLAN_MAX_METADATA,
                    1))
    return 0;
  provenance = ab_value_member(value, "provenance");
  if (!text_is(provenance, "derived") && !text_is(provenance, "asserted"))
    return 0;
  origins = ab_value_member(value, "origins");
  if (!unique_rows(origins) || origins->as.array.count == 0)
    return 0;
  for (index = 0; index < origins->as.array.count; index++)
    if (!validate_origin(&origins->as.array.items[index]))
      return 0;
  evidence = ab_value_member(value, "evidence");
  if (!unique_rows(evidence))
    return 0;
  for (index = 0; index < evidence->as.array.count; index++)
    if (!validate_evidence(&evidence->as.array.items[index]))
      return 0;
  if (!string_array(ab_value_member(value, "depends_on"), 1, 0) ||
      !string_array(ab_value_member(value, "unknowns"), 1, 0) ||
      !validate_operation(ab_value_member(value, "operation"), &manual,
                          &requires_asserted))
    return 0;
  acceptance = ab_value_member(value, "acceptance");
  if (!object_exact(acceptance, acceptance_fields, 1) ||
      !string_array(ab_value_member(acceptance, "constraints"), 1, 1))
    return 0;
  executable = ab_value_member(value, "executable");
  reasons = ab_value_member(value, "non_executable_reasons");
  if (!boolean_value(executable) || !string_array(reasons, 0, 0))
    return 0;
  if (executable->as.boolean) {
    if (manual || reasons->as.array.count ||
        (requires_asserted && !text_is(provenance, "asserted")))
      return 0;
  } else if (!reasons->as.array.count) {
    return 0;
  }
  return 1;
}

static int visit_item(const AbValue *items, const PlanIdIndex *item_index,
                      size_t item_count, size_t item, uint8_t *states) {
  const AbValue *dependencies;
  size_t index;
  if (states[item] == 2)
    return 1;
  if (states[item] == 1)
    return 0;
  states[item] = 1;
  dependencies = ab_value_member(&items->as.array.items[item], "depends_on");
  for (index = 0; index < dependencies->as.array.count; index++) {
    size_t dependency = id_index_find(
        item_index, item_count, &dependencies->as.array.items[index].as.text);
    if (dependency == SIZE_MAX || dependency == item ||
        !visit_item(items, item_index, item_count, dependency, states))
      return 0;
  }
  states[item] = 2;
  return 1;
}

static ArchbirdStatus validate_plan_links(ArchbirdEngine *engine,
                                          const AbValue *items,
                                          const AbValue *unknowns,
                                          int *out_valid) {
  PlanIdIndex *item_index = NULL;
  PlanIdIndex *unknown_index = NULL;
  uint8_t *states = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out_valid = 0;
  if (items->as.array.count) {
    item_index = (PlanIdIndex *)ab_calloc(engine, items->as.array.count,
                                          sizeof(*item_index));
    states =
        (uint8_t *)ab_calloc(engine, items->as.array.count, sizeof(*states));
    if (!item_index || !states) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory validating Plan item dependencies");
      goto cleanup;
    }
  }
  if (unknowns->as.array.count) {
    unknown_index = (PlanIdIndex *)ab_calloc(engine, unknowns->as.array.count,
                                             sizeof(*unknown_index));
    if (!unknown_index) {
      status =
          archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                             "out of memory validating Plan unknowns");
      goto cleanup;
    }
  }
  for (index = 0; index < items->as.array.count; index++) {
    const AbValue *id = ab_value_member(&items->as.array.items[index], "id");
    item_index[index].id = &id->as.text;
    item_index[index].index = index;
  }
  for (index = 0; index < unknowns->as.array.count; index++) {
    const AbValue *id = ab_value_member(&unknowns->as.array.items[index], "id");
    unknown_index[index].id = &id->as.text;
    unknown_index[index].index = index;
  }
  if (items->as.array.count > 1)
    qsort(item_index, items->as.array.count, sizeof(*item_index),
          id_index_compare);
  if (unknowns->as.array.count > 1)
    qsort(unknown_index, unknowns->as.array.count, sizeof(*unknown_index),
          id_index_compare);
  for (index = 1; index < items->as.array.count; index++)
    if (ab_string_equal(item_index[index - 1].id, item_index[index].id))
      goto cleanup;
  for (index = 1; index < unknowns->as.array.count; index++)
    if (ab_string_equal(unknown_index[index - 1].id, unknown_index[index].id))
      goto cleanup;
  for (index = 0; index < unknowns->as.array.count; index++) {
    const AbValue *item =
        ab_value_member(&unknowns->as.array.items[index], "item_id");
    if (item->kind == AB_VALUE_STRING &&
        id_index_find(item_index, items->as.array.count, &item->as.text) ==
            SIZE_MAX)
      goto cleanup;
  }
  for (index = 0; index < items->as.array.count; index++) {
    const AbValue *references =
        ab_value_member(&items->as.array.items[index], "unknowns");
    size_t reference;
    for (reference = 0; reference < references->as.array.count; reference++)
      if (id_index_find(unknown_index, unknowns->as.array.count,
                        &references->as.array.items[reference].as.text) ==
          SIZE_MAX)
        goto cleanup;
    if (!visit_item(items, item_index, items->as.array.count, index, states))
      goto cleanup;
  }
  *out_valid = 1;
cleanup:
  ab_free(engine, states);
  ab_free(engine, unknown_index);
  ab_free(engine, item_index);
  return status;
}

static ArchbirdStatus validate_plan(ArchbirdEngine *engine, AbPlan *plan,
                                    int *out_valid) {
  static const char *const fields[] = {
      "schema_version", "artifact",  "provenance", "tool",
      "source",         "objective", "items",      "preserved_constraints",
      "unknowns"};
  const AbValue *schema;
  const AbValue *provenance;
  const AbValue *items;
  const AbValue *unknowns;
  uint64_t schema_number;
  size_t index;
  ArchbirdStatus status;
  *out_valid = 0;
  if (!object_exact(&plan->document, fields, 9))
    return ARCHBIRD_OK;
  schema = ab_value_member(&plan->document, "schema_version");
  provenance = ab_value_member(&plan->document, "provenance");
  if (!safe_integer(schema, &schema_number) || schema_number != 2 ||
      !text_is(ab_value_member(&plan->document, "artifact"), "plan") ||
      (!text_is(provenance, "derived") && !text_is(provenance, "asserted")) ||
      !validate_tool(ab_value_member(&plan->document, "tool")) ||
      !validate_source(ab_value_member(&plan->document, "source")) ||
      !bounded_text(ab_value_member(&plan->document, "objective"),
                    AB_PLAN_MAX_METADATA, 1) ||
      !string_array(ab_value_member(&plan->document, "preserved_constraints"),
                    1, 0))
    return ARCHBIRD_OK;
  items = ab_value_member(&plan->document, "items");
  if (!unique_rows(items))
    return ARCHBIRD_OK;
  for (index = 0; index < items->as.array.count; index++)
    if (!validate_item(&items->as.array.items[index]))
      return ARCHBIRD_OK;
  unknowns = ab_value_member(&plan->document, "unknowns");
  if (!unique_rows(unknowns))
    return ARCHBIRD_OK;
  for (index = 0; index < unknowns->as.array.count; index++)
    if (!validate_unknown(&unknowns->as.array.items[index]))
      return ARCHBIRD_OK;
  status = validate_plan_links(engine, items, unknowns, out_valid);
  if (status != ARCHBIRD_OK || !*out_valid)
    return status;
  plan->source = ab_value_member(&plan->document, "source");
  plan->items = items;
  plan->preserved_constraints =
      ab_value_member(&plan->document, "preserved_constraints");
  plan->unknowns = unknowns;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_plan_load(ArchbirdEngine *engine, const uint8_t *json,
                            size_t length, AbPlan *out) {
  PlanDigestWriter writer;
  uint8_t digest[32];
  ArchbirdStatus status;
  int valid = 0;
  if (!engine || !json || !length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, length, &out->document);
  if (status == ARCHBIRD_OK)
    status = validate_plan(engine, out, &valid);
  if (status == ARCHBIRD_OK && !valid)
    status = invalid(engine, "document does not satisfy the Plan contract");
  if (status == ARCHBIRD_OK) {
    archbird_sha256_init(&writer.context);
    writer.status = ARCHBIRD_OK;
    status = archbird_json_canonicalize(engine, json, length, 0, digest_write,
                                        &writer);
    if (status == ARCHBIRD_OK)
      status = writer.status;
    if (status == ARCHBIRD_OK) {
      archbird_sha256_final(&writer.context, digest);
      archbird_sha256_hex(digest, out->sha256);
    }
  }
  if (status != ARCHBIRD_OK)
    ab_plan_free(engine, out);
  return status;
}

void ab_plan_free(ArchbirdEngine *engine, AbPlan *plan) {
  if (!plan)
    return;
  ab_value_free(engine, &plan->document);
  memset(plan, 0, sizeof(*plan));
}
