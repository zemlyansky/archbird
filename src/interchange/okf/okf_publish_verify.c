#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

#define VERIFY_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus verify_status_ = (expression);                              \
    if (verify_status_ != ARCHBIRD_OK)                                         \
      return verify_status_;                                                   \
  } while (0)

typedef struct VerifyPath {
  AbString name;
  AbString slug;
  AbString path;
} VerifyPath;

typedef struct VerifyPathSet {
  VerifyPath *items;
  size_t count;
} VerifyPathSet;

typedef struct VerifyPaths {
  VerifyPathSet projects;
  VerifyPathSet requirements;
  VerifyPathSet facts;
  VerifyPathSet checks;
  VerifyPathSet attestations;
} VerifyPaths;

static const AbValue EMPTY_ARRAY = {.kind = AB_VALUE_ARRAY};
static const AbString EMPTY_TEXT = {(char *)"", 0};
static const AbString NOT_DECLARED = {(char *)"not_declared", 12};

static const AbValue *array_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return !value ? &EMPTY_ARRAY : value->kind == AB_VALUE_ARRAY ? value : NULL;
}

static const AbString *text_or_empty(const AbValue *object, const char *name) {
  const AbValue *value = ab_value_member(object, name);
  return !value                           ? &EMPTY_TEXT
         : value->kind == AB_VALUE_STRING ? &value->as.text
                                          : NULL;
}

static int path_compare(const void *left, const void *right) {
  return ab_string_compare(&((const VerifyPath *)left)->name,
                           &((const VerifyPath *)right)->name);
}

static int string_compare(const void *left, const void *right) {
  return ab_string_compare((const AbString *)left, (const AbString *)right);
}

static void path_set_free(AbOkfPublication *pub, VerifyPathSet *set) {
  size_t index;
  for (index = 0; index < set->count; index++) {
    ab_string_free(pub->engine, &set->items[index].name);
    ab_string_free(pub->engine, &set->items[index].slug);
    ab_string_free(pub->engine, &set->items[index].path);
  }
  ab_free(pub->engine, set->items);
  memset(set, 0, sizeof(*set));
}

static void paths_free(AbOkfPublication *pub, VerifyPaths *paths) {
  path_set_free(pub, &paths->attestations);
  path_set_free(pub, &paths->checks);
  path_set_free(pub, &paths->facts);
  path_set_free(pub, &paths->requirements);
  path_set_free(pub, &paths->projects);
}

static const VerifyPath *path_find(const VerifyPathSet *set,
                                   const AbString *name) {
  VerifyPath key = {0};
  if (!set || !set->count || !name)
    return NULL;
  key.name = *name;
  return (const VerifyPath *)bsearch(&key, set->items, set->count,
                                     sizeof(*set->items), path_compare);
}

static ArchbirdStatus path_suffix(AbOkfPublication *pub, VerifyPath *row) {
  char digest[65];
  AbBuffer buffer;
  ArchbirdStatus status = ab_okf_pub_sha256((const uint8_t *)row->name.data,
                                            row->name.length, digest);
  ab_buffer_init(&buffer, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, row->slug.data, row->slug.length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "-");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, digest, 12);
  if (status == ARCHBIRD_OK) {
    ab_string_free(pub->engine, &row->slug);
    status = ab_okf_pub_buffer_string(pub, &buffer, &row->slug);
  }
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus make_paths(AbOkfPublication *pub, AbString *names,
                                 size_t count, const char *prefix,
                                 VerifyPathSet *out) {
  size_t index;
  size_t other;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!count)
    return ARCHBIRD_OK;
  out->items = (VerifyPath *)ab_calloc(pub->engine, count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building verification OKF paths");
  out->count = count;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    status = ab_okf_pub_copy(pub, &out->items[index].name, names[index].data,
                             names[index].length);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_slug(pub, &names[index], &out->items[index].slug);
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->items, count, sizeof(*out->items), path_compare);
  for (index = 1; status == ARCHBIRD_OK && index < count; index++)
    if (ab_string_equal(&out->items[index - 1].name, &out->items[index].name))
      status =
          ab_okf_pub_error(pub, "duplicate verification OKF entity identity");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    size_t collisions = 0;
    AbBuffer path;
    for (other = 0; other < count; other++)
      if (ab_string_equal(&out->items[index].slug, &out->items[other].slug))
        collisions++;
    if (collisions > 1)
      status = path_suffix(pub, &out->items[index]);
    ab_buffer_init(&path, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&path, prefix);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&path, out->items[index].slug.data,
                                out->items[index].slug.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&path, ".md");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_buffer_string(pub, &path, &out->items[index].path);
    ab_buffer_free(&path);
  }
  if (status != ARCHBIRD_OK)
    path_set_free(pub, out);
  return status;
}

static ArchbirdStatus paths_from_rows(AbOkfPublication *pub,
                                      const AbValue *rows, const char *field,
                                      const char *prefix, VerifyPathSet *out) {
  AbString *names = NULL;
  size_t index;
  ArchbirdStatus status;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid verification OKF inventory");
  if (rows->as.array.count) {
    names = (AbString *)ab_calloc(pub->engine, rows->as.array.count,
                                  sizeof(*names));
    if (!names)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory reading verification names");
  }
  for (index = 0; index < rows->as.array.count; index++) {
    const AbString *name = ab_okf_pub_text(&rows->as.array.items[index], field);
    if (!name) {
      ab_free(pub->engine, names);
      return ab_okf_pub_error(pub, "invalid named verification entity");
    }
    names[index] = *name;
  }
  status = make_paths(pub, names, rows->as.array.count, prefix, out);
  ab_free(pub->engine, names);
  return status;
}

static ArchbirdStatus name_add(AbOkfPublication *pub, AbString **items,
                               size_t *count, size_t *capacity,
                               const AbString *name) {
  size_t index;
  size_t next;
  AbString *resized;
  for (index = 0; index < *count; index++)
    if (ab_string_equal(&(*items)[index], name))
      return ARCHBIRD_OK;
  if (*count == *capacity) {
    next = *capacity ? *capacity * 2 : 8;
    if (next > SIZE_MAX / sizeof(*resized) ||
        next > pub->engine->options.max_values)
      return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too many verification requirement names");
    resized =
        (AbString *)ab_realloc(pub->engine, *items, next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory growing requirements");
    memset(resized + *capacity, 0, (next - *capacity) * sizeof(*resized));
    *items = resized;
    *capacity = next;
  }
  VERIFY_TRY(ab_okf_pub_copy(pub, &(*items)[*count], name->data, name->length));
  (*count)++;
  if (*count > 1)
    qsort(*items, *count, sizeof(**items), string_compare);
  return ARCHBIRD_OK;
}

static void names_free(AbOkfPublication *pub, AbString *items, size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(pub->engine, &items[index]);
  ab_free(pub->engine, items);
}

static ArchbirdStatus requirement_paths(AbOkfPublication *pub,
                                        VerifyPathSet *out) {
  AbString *names = NULL;
  size_t count = 0;
  size_t capacity = 0;
  size_t check_index;
  size_t attestation_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (check_index = 0; status == ARCHBIRD_OK &&
                        check_index < pub->verification.checks->as.array.count;
       check_index++) {
    const AbValue *requirements = array_or_empty(
        &pub->verification.checks->as.array.items[check_index], "requirements");
    size_t index;
    if (!requirements) {
      status = ab_okf_pub_error(pub, "invalid verification requirements");
      break;
    }
    for (index = 0;
         status == ARCHBIRD_OK && index < requirements->as.array.count;
         index++) {
      const AbValue *value = &requirements->as.array.items[index];
      if (value->kind != AB_VALUE_STRING)
        status = ab_okf_pub_error(pub, "invalid verification requirement ID");
      else if (value->as.text.length)
        status = name_add(pub, &names, &count, &capacity, &value->as.text);
    }
  }
  for (attestation_index = 0;
       status == ARCHBIRD_OK &&
       attestation_index < pub->verification.attestations->as.array.count;
       attestation_index++) {
    const AbValue *row =
        &pub->verification.attestations->as.array.items[attestation_index];
    const AbValue *attestation =
        ab_okf_pub_member(row, "attestation", AB_VALUE_OBJECT);
    const AbValue *cases =
        attestation ? array_or_empty(attestation, "cases") : &EMPTY_ARRAY;
    size_t case_index;
    if (!cases) {
      status = ab_okf_pub_error(pub, "invalid attestation cases");
      break;
    }
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *requirements =
          array_or_empty(&cases->as.array.items[case_index], "requirements");
      size_t index;
      if (!requirements) {
        status = ab_okf_pub_error(pub, "invalid attestation requirements");
        break;
      }
      for (index = 0;
           status == ARCHBIRD_OK && index < requirements->as.array.count;
           index++) {
        const AbValue *value = &requirements->as.array.items[index];
        if (value->kind != AB_VALUE_STRING)
          status = ab_okf_pub_error(pub, "invalid attestation requirement ID");
        else if (value->as.text.length)
          status = name_add(pub, &names, &count, &capacity, &value->as.text);
      }
    }
  }
  if (status == ARCHBIRD_OK)
    status = make_paths(pub, names, count, "verification/requirements/", out);
  names_free(pub, names, count);
  return status;
}

static ArchbirdStatus build_paths(AbOkfPublication *pub, VerifyPaths *paths) {
  ArchbirdStatus status =
      paths_from_rows(pub, pub->verification.projects, "name",
                      "verification/projects/", &paths->projects);
  if (status == ARCHBIRD_OK)
    status = requirement_paths(pub, &paths->requirements);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, pub->verification.facts, "name",
                             "verification/facts/", &paths->facts);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, pub->verification.checks, "id",
                             "verification/checks/", &paths->checks);
  if (status == ARCHBIRD_OK)
    status =
        paths_from_rows(pub, pub->verification.attestations, "name",
                        "verification/attestations/", &paths->attestations);
  return status;
}

static const AbValue *row_by_name(const AbValue *rows, const char *field,
                                  const AbString *name) {
  size_t index;
  for (index = 0; rows && index < rows->as.array.count; index++) {
    const AbString *candidate =
        ab_okf_pub_text(&rows->as.array.items[index], field);
    if (candidate && ab_string_equal(candidate, name))
      return &rows->as.array.items[index];
  }
  return NULL;
}

static ArchbirdStatus relation_path(AbOkfPublication *pub,
                                    AbOkfPubRelationList *relations,
                                    const char *kind, const AbString *path) {
  AbBuffer target;
  ArchbirdStatus status;
  ab_buffer_init(&target, pub->engine);
  status = ab_buffer_append(&target, path->data, path->length - 3);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, relations, kind,
                                        (const char *)target.data);
  ab_buffer_free(&target);
  return status;
}

static ArchbirdStatus md_header(AbBuffer *body, const char *const *headers,
                                size_t count) {
  size_t index;
  VERIFY_TRY(ab_buffer_literal(body, "| "));
  for (index = 0; index < count; index++) {
    if (index)
      VERIFY_TRY(ab_buffer_literal(body, " | "));
    VERIFY_TRY(ab_buffer_literal(body, headers[index]));
  }
  VERIFY_TRY(ab_buffer_literal(body, " |\n| "));
  for (index = 0; index < count; index++) {
    if (index)
      VERIFY_TRY(ab_buffer_literal(body, " | "));
    VERIFY_TRY(ab_buffer_literal(body, "---"));
  }
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus md_cell(AbBuffer *body) {
  return ab_buffer_literal(body, " | ");
}

static ArchbirdStatus md_end(AbBuffer *body) {
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus dynamic_description(AbOkfPublication *pub, AbString *out,
                                          const char *prefix, uint64_t count,
                                          const char *suffix) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&buffer, count);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, suffix);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static const AbString *evidence_field(const AbValue *row, const char *name) {
  const AbValue *value = ab_value_member(row, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text
                                                 : &EMPTY_TEXT;
}

static int evidence_pointer_compare(const void *left_raw,
                                    const void *right_raw) {
  const AbValue *left = *(const AbValue *const *)left_raw;
  const AbValue *right = *(const AbValue *const *)right_raw;
  const char *before_line[] = {"provenance", "project", "path"};
  const char *after_line[] = {"sha256", "detail"};
  uint64_t left_line = 0;
  uint64_t right_line = 0;
  size_t index;
  int compared;
  for (index = 0; index < sizeof(before_line) / sizeof(before_line[0]);
       index++) {
    compared = ab_string_compare(evidence_field(left, before_line[index]),
                                 evidence_field(right, before_line[index]));
    if (compared)
      return compared;
  }
  (void)ab_value_u64(ab_value_member(left, "line"), &left_line);
  (void)ab_value_u64(ab_value_member(right, "line"), &right_line);
  if (left_line != right_line)
    return left_line < right_line ? -1 : 1;
  for (index = 0; index < sizeof(after_line) / sizeof(after_line[0]); index++) {
    compared = ab_string_compare(evidence_field(left, after_line[index]),
                                 evidence_field(right, after_line[index]));
    if (compared)
      return compared;
  }
  return 0;
}

static ArchbirdStatus evidence_table(AbOkfPublication *pub, AbBuffer *body,
                                     const AbValue *rows) {
  const char *headers[] = {"Provenance", "Project", "Path",
                           "Line",       "Detail",  "SHA-256"};
  const AbValue **ordered = NULL;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid verification evidence table");
  if (!rows->as.array.count)
    return ab_buffer_literal(body, "_None._\n");
  ordered = (const AbValue **)ab_malloc(pub->engine, rows->as.array.count *
                                                         sizeof(*ordered));
  if (!ordered)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory sorting verification evidence");
  for (index = 0; index < rows->as.array.count; index++)
    ordered[index] = &rows->as.array.items[index];
  if (rows->as.array.count > 1)
    qsort(ordered, rows->as.array.count, sizeof(*ordered),
          evidence_pointer_compare);
  status = md_header(body, headers, 6);
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    const AbValue *row = ordered[index];
    const AbString *provenance = ab_okf_pub_text(row, "provenance");
    const AbString *project = text_or_empty(row, "project");
    const AbString *path = text_or_empty(row, "path");
    const AbString *detail = text_or_empty(row, "detail");
    const AbString *sha = text_or_empty(row, "sha256");
    uint64_t line;
    if (!provenance || !project || !path || !detail || !sha ||
        !ab_okf_pub_u64(row, "line", &line)) {
      status = ab_okf_pub_error(pub, "invalid verification evidence row");
      break;
    }
    status = ab_buffer_literal(body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(body, provenance);
    if (status == ARCHBIRD_OK)
      status = md_cell(body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(body, project);
    if (status == ARCHBIRD_OK)
      status = md_cell(body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(body, path);
    if (status == ARCHBIRD_OK)
      status = md_cell(body);
    if (status == ARCHBIRD_OK && line)
      status = ab_buffer_u64(body, line);
    if (status == ARCHBIRD_OK)
      status = md_cell(body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(body, detail);
    if (status == ARCHBIRD_OK)
      status = md_cell(body);
    if (status == ARCHBIRD_OK && sha->length)
      status = ab_okf_pub_code(body, sha);
    if (status == ARCHBIRD_OK)
      status = md_end(body);
  }
  ab_free(pub->engine, ordered);
  return status;
}

static size_t finding_count(const AbValue *checks) {
  size_t index;
  size_t count = 0;
  for (index = 0; checks && index < checks->as.array.count; index++) {
    const AbValue *findings =
        array_or_empty(&checks->as.array.items[index], "findings");
    if (findings)
      count += findings->as.array.count;
  }
  return count;
}

static ArchbirdStatus add_suite(AbOkfPublication *pub,
                                const VerifyPaths *paths) {
  const char *summary_headers[] = {"Blocking", "Checks", "Facts", "Findings",
                                   "Attestations"};
  const char *check_headers[] = {"Check", "Assertion", "Severity", "Status",
                                 "Findings"};
  const AbValue *suite = pub->verification.suite;
  const AbValue *summary =
      ab_okf_pub_member(&pub->verification.root, "summary", AB_VALUE_OBJECT);
  const AbString *name = ab_okf_pub_text(suite, "name");
  const AbString *raw_description = text_or_empty(suite, "description");
  const AbString *suite_sha = ab_okf_pub_text(suite, "sha256");
  int blocking;
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[1] = {{"suite_sha256", {0}}};
  AbString title = {0};
  AbString description = {0};
  AbString tags[] = {{(char *)"verification", 12}, {(char *)"suite", 5}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  const AbValue *blocking_value = ab_value_member(summary, "blocking");
  if (!name || !raw_description || !suite_sha || !summary || !blocking_value ||
      blocking_value->kind != AB_VALUE_BOOL)
    return ab_okf_pub_error(pub, "invalid verification suite publication");
  blocking = blocking_value->as.boolean;
  for (index = 0; status == ARCHBIRD_OK && index < paths->checks.count; index++)
    status = relation_path(pub, &relations, "contains_check",
                           &paths->checks.items[index].path);
  for (index = 0; status == ARCHBIRD_OK && index < paths->projects.count;
       index++)
    status = relation_path(pub, &relations, "contains_project",
                           &paths->projects.items[index].path);
  for (index = 0; status == ARCHBIRD_OK && index < paths->requirements.count;
       index++)
    status = relation_path(pub, &relations, "contains_requirement",
                           &paths->requirements.items[index].path);
  for (index = 0; status == ARCHBIRD_OK && index < paths->facts.count; index++)
    status = relation_path(pub, &relations, "contains_fact",
                           &paths->facts.items[index].path);
  for (index = 0; status == ARCHBIRD_OK && index < paths->attestations.count;
       index++)
    status = relation_path(pub, &relations, "contains_attestation",
                           &paths->attestations.items[index].path);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[0].json, suite_sha);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, &title, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = dynamic_description(
        pub, &description, "Typed verification suite with ",
        pub->verification.checks->as.array.count, " checks.");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# ");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, raw_description);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n# Result summary\n\n");
  if (status == ARCHBIRD_OK)
    status = md_header(&body, summary_headers, 5);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "| ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, blocking ? "true" : "false");
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, pub->verification.checks->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, pub->verification.facts->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&body, finding_count(pub->verification.checks));
  if (status == ARCHBIRD_OK)
    status = md_cell(&body);
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_u64(&body, pub->verification.attestations->as.array.count);
  if (status == ARCHBIRD_OK)
    status = md_end(&body);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Checks\n\n");
  if (status == ARCHBIRD_OK && paths->checks.count)
    status = md_header(&body, check_headers, 5);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < paths->checks.count;
       index++) {
    const VerifyPath *check_path = &paths->checks.items[index];
    const AbValue *check =
        row_by_name(pub->verification.checks, "id", &check_path->name);
    const AbString *id = ab_okf_pub_text(check, "id");
    const AbString *assertion = ab_okf_pub_text(check, "assert");
    const AbString *severity = ab_okf_pub_text(check, "severity");
    const AbString *check_status = ab_okf_pub_text(check, "status");
    const AbValue *findings = array_or_empty(check, "findings");
    if (!id || !assertion || !severity || !check_status || !findings || !check)
      return ab_okf_pub_error(pub, "invalid verification check summary");
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relative_link(&body, "verification/suite.md",
                                        check_path->path.data, id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, assertion);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, severity);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, check_status);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, findings->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Projects\n\n");
  for (index = 0; status == ARCHBIRD_OK && index < paths->projects.count;
       index++) {
    status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relative_link(&body, "verification/suite.md",
                                        paths->projects.items[index].path.data,
                                        &paths->projects.items[index].name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Behavioral attestations\n\n");
  if (status == ARCHBIRD_OK && !paths->attestations.count)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < paths->attestations.count;
       index++) {
    status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status =
          ab_okf_pub_relative_link(&body, "verification/suite.md",
                                   paths->attestations.items[index].path.data,
                                   &paths->attestations.items[index].name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(
        &body, "\nThe canonical verification JSON remains authoritative; this "
               "page is a "
               "deterministic progressive-disclosure projection.\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->verification_source,
                             "verification/suite.md",
                             "Archbird Verification Suite",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "verification_suite",
                             name,
                             tags,
                             2,
                             &relations,
                             extra,
                             1,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_fields_free(pub, extra, 1);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static int verification_project_matches(const AbOkfPublication *pub,
                                        const AbValue *project) {
  const AbValue *map_evidence = ab_value_member(&pub->map, "evidence");
  const AbValue *map_tool = ab_value_member(&pub->map, "tool");
  const AbValue *producer = ab_value_member(project, "producer");
  const AbString *values[12] = {
      ab_okf_pub_text(project, "project"),
      ab_okf_pub_text(&pub->map, "project"),
      ab_okf_pub_text(project, "input_sha256"),
      ab_okf_pub_text(map_evidence, "input_sha256"),
      ab_okf_pub_text(project, "config_sha256"),
      ab_okf_pub_text(map_evidence, "config_sha256"),
      ab_okf_pub_text(producer, "name"),
      ab_okf_pub_text(map_tool, "name"),
      ab_okf_pub_text(producer, "version"),
      ab_okf_pub_text(map_tool, "version"),
      ab_okf_pub_text(producer, "implementation_sha256"),
      ab_okf_pub_text(map_tool, "implementation_sha256")};
  size_t index;
  for (index = 0; index < 12; index += 2)
    if (!values[index] || !values[index + 1] ||
        !ab_string_equal(values[index], values[index + 1]))
      return 0;
  return 1;
}

static ArchbirdStatus add_projects(AbOkfPublication *pub,
                                   const VerifyPaths *paths) {
  const char *headers[] = {"Project",       "Declared revision (asserted)",
                           "Source lock",   "Profile",
                           "Input SHA-256", "Config SHA-256"};
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index < paths->projects.count;
       index++) {
    const VerifyPath *path = &paths->projects.items[index];
    const AbValue *project =
        row_by_name(pub->verification.projects, "name", &path->name);
    const AbString *project_name = ab_okf_pub_text(project, "project");
    const AbString *revision = text_or_empty(project, "revision");
    const AbValue *source_lock = ab_value_member(project, "source_lock");
    const AbString *source_lock_state = text_or_empty(source_lock, "state");
    const AbString *profile = text_or_empty(project, "profile");
    const AbString *input = ab_okf_pub_text(project, "input_sha256");
    const AbString *config = ab_okf_pub_text(project, "config_sha256");
    const AbValue *capabilities = array_or_empty(project, "capabilities");
    int matches = verification_project_matches(pub, project);
    size_t capability;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"verification", 12}, {(char *)"project", 7}};
    AbBuffer body;
    if (!project || !project_name || !revision || !source_lock_state ||
        !profile || !input || !config || !capabilities)
      return ab_okf_pub_error(pub, "invalid verification project row");
    if (!source_lock_state->length)
      source_lock_state = &NOT_DECLARED;
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of_suite",
                                        "verification/suite");
    if (status == ARCHBIRD_OK && matches)
      status = ab_okf_pub_relation_simple(
          pub, &relations, "describes_architecture", "architecture/project");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Verification project binding for ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&value, project_name->data, project_name->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, ".");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, headers, 6);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, project_name);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, revision);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, source_lock_state);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, profile);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, input);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, config);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Capabilities\n\n");
    for (capability = 0;
         status == ARCHBIRD_OK && capability < capabilities->as.array.count;
         capability++) {
      const AbValue *value = &capabilities->as.array.items[capability];
      if (value->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid verification capability");
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, &value->as.text);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, capabilities->as.array.count
                                            ? "\n# Verification suite\n\n"
                                            : "\n\n# Verification suite\n\n");
    if (status == ARCHBIRD_OK) {
      const AbString suite = {(char *)"portable-verification", 21};
      const AbString *suite_name =
          ab_okf_pub_text(pub->verification.suite, "name");
      (void)suite;
      status = ab_okf_pub_relative_link(&body, path->path.data,
                                        "verification/suite.md", suite_name);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
    if (status == ARCHBIRD_OK && matches)
      status = ab_buffer_literal(&body, "\n# Architecture\n\n");
    if (status == ARCHBIRD_OK && matches)
      status = ab_okf_pub_relative_link(&body, path->path.data,
                                        "architecture/project.md",
                                        ab_okf_pub_text(&pub->map, "project"));
    if (status == ARCHBIRD_OK && matches)
      status = ab_buffer_literal(&body, "\n");
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Verification Project",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "verification_project",
                               &path->name,
                               tags,
                               2,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static ArchbirdStatus add_requirements(AbOkfPublication *pub,
                                       const VerifyPaths *paths) {
  size_t requirement_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (requirement_index = 0;
       status == ARCHBIRD_OK && requirement_index < paths->requirements.count;
       requirement_index++) {
    const VerifyPath *path = &paths->requirements.items[requirement_index];
    AbString *checks = NULL;
    size_t check_count = 0;
    size_t check_capacity = 0;
    size_t check_index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"verification", 12},
                       {(char *)"requirement", 11}};
    AbBuffer body;
    for (check_index = 0;
         status == ARCHBIRD_OK &&
         check_index < pub->verification.checks->as.array.count;
         check_index++) {
      const AbValue *check =
          &pub->verification.checks->as.array.items[check_index];
      const AbValue *requirements = array_or_empty(check, "requirements");
      const AbString *id = ab_okf_pub_text(check, "id");
      size_t index;
      if (!requirements || !id)
        return ab_okf_pub_error(pub, "invalid check requirement binding");
      for (index = 0; index < requirements->as.array.count; index++)
        if (requirements->as.array.items[index].kind == AB_VALUE_STRING &&
            ab_string_equal(&requirements->as.array.items[index].as.text,
                            &path->name))
          status = name_add(pub, &checks, &check_count, &check_capacity, id);
    }
    for (check_index = 0; status == ARCHBIRD_OK && check_index < check_count;
         check_index++) {
      const VerifyPath *check_path =
          path_find(&paths->checks, &checks[check_index]);
      if (check_path)
        status =
            relation_path(pub, &relations, "enforced_by", &check_path->path);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Requirement reference used by ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, check_count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " verification checks.");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK &&
        ((path->name.length >= 7 && !memcmp(path->name.data, "http://", 7)) ||
         (path->name.length >= 8 && !memcmp(path->name.data, "https://", 8)))) {
      status = ab_buffer_literal(&body, "External requirement: ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_external_link(&body, &path->name, &path->name);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# Enforced by\n\n");
    if (status == ARCHBIRD_OK && !check_count)
      status = ab_buffer_literal(
          &body, "_No verification check directly names this requirement._\n");
    for (check_index = 0; status == ARCHBIRD_OK && check_index < check_count;
         check_index++) {
      const VerifyPath *check_path =
          path_find(&paths->checks, &checks[check_index]);
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, path->path.data,
                                          check_path->path.data,
                                          &checks[check_index]);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Requirement Reference",
                               &title,
                               NULL,
                               &description,
                               "asserted",
                               "requirement",
                               &path->name,
                               tags,
                               2,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
    names_free(pub, checks, check_count);
  }
  return status;
}

/* Fact, check, finding, attestation, and diagnostic projections follow. */
static ArchbirdStatus add_facts(AbOkfPublication *pub,
                                const VerifyPaths *paths);
static ArchbirdStatus add_checks(AbOkfPublication *pub,
                                 const VerifyPaths *paths);
static ArchbirdStatus add_findings(AbOkfPublication *pub,
                                   const VerifyPaths *paths);
static ArchbirdStatus add_attestations(AbOkfPublication *pub,
                                       const VerifyPaths *paths);
static ArchbirdStatus add_diagnostics(AbOkfPublication *pub);

ArchbirdStatus ab_okf_pub_verify(AbOkfPublication *pub) {
  VerifyPaths paths = {0};
  ArchbirdStatus status = build_paths(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_suite(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_projects(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_requirements(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_facts(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_checks(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_findings(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_attestations(pub, &paths);
  if (status == ARCHBIRD_OK)
    status = add_diagnostics(pub);
  paths_free(pub, &paths);
  return status;
}

static ArchbirdStatus add_facts(AbOkfPublication *pub,
                                const VerifyPaths *paths) {
  const char *summary_headers[] = {"Shape", "Provenance", "Project",
                                   "State", "Items",      "SHA-256"};
  const char *item_headers[] = {"Key", "Value", "State", "Evidence"};
  size_t fact_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (fact_index = 0; status == ARCHBIRD_OK && fact_index < paths->facts.count;
       fact_index++) {
    const VerifyPath *path = &paths->facts.items[fact_index];
    const AbValue *fact =
        row_by_name(pub->verification.facts, "name", &path->name);
    const AbString *shape = ab_okf_pub_text(fact, "shape");
    const AbString *provenance = ab_okf_pub_text(fact, "provenance");
    const AbString *project = text_or_empty(fact, "project");
    const AbString *state = ab_okf_pub_text(fact, "state");
    const AbString *digest = ab_okf_pub_text(fact, "sha256");
    const AbValue *items = array_or_empty(fact, "items");
    size_t item_index;
    AbOkfPubRelationList relations = {0};
    AbOkfPubField extra[1] = {{"fact_sha256", {0}}};
    AbString title = {0};
    AbString description = {0};
    AbString tags[4] = {
        {(char *)"verification", 12}, {(char *)"fact", 4}, {0}, {0}};
    AbBuffer body;
    if (!fact || !shape || !provenance || !project || !state || !digest ||
        !items)
      return ab_okf_pub_error(pub, "invalid verification fact row");
    tags[2] = *shape;
    tags[3] = *state;
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of_suite",
                                        "verification/suite");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_text(pub, &extra[0].json, digest);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_append(&value, shape->data, shape->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " fact with ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, items->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " items in state ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, state->data, state->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, ".");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 6);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, shape);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, provenance);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, project);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, state);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, items->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, digest);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Items\n\n");
    if (status == ARCHBIRD_OK && items->as.array.count)
      status = md_header(&body, item_headers, 4);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (item_index = 0;
         status == ARCHBIRD_OK && item_index < items->as.array.count;
         item_index++) {
      const AbValue *item = &items->as.array.items[item_index];
      const AbString *key = ab_okf_pub_text(item, "key");
      const AbString *item_state = ab_okf_pub_text(item, "state");
      const AbValue *value = ab_value_member(item, "value");
      const AbValue *evidence = array_or_empty(item, "evidence");
      size_t evidence_index;
      if (!key || !item_state || !value || !evidence)
        return ab_okf_pub_error(pub, "invalid verification fact item");
      status = ab_buffer_literal(&body, "| ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, key);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_json_code(&body, value);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, item_state);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (evidence_index = 0;
           status == ARCHBIRD_OK && evidence_index < evidence->as.array.count;
           evidence_index++) {
        const AbValue *row = &evidence->as.array.items[evidence_index];
        const AbString *evidence_project = text_or_empty(row, "project");
        const AbString *evidence_path = text_or_empty(row, "path");
        uint64_t line;
        AbBuffer label;
        AbString label_text;
        if (!evidence_project || !evidence_path ||
            !ab_okf_pub_u64(row, "line", &line))
          return ab_okf_pub_error(pub, "invalid fact-item evidence");
        ab_buffer_init(&label, pub->engine);
        status = ab_buffer_append(&label, evidence_project->data,
                                  evidence_project->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&label, ":");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&label, evidence_path->data,
                                    evidence_path->length);
        if (status == ARCHBIRD_OK && line)
          status = ab_buffer_literal(&label, ":");
        if (status == ARCHBIRD_OK && line)
          status = ab_buffer_u64(&label, line);
        label_text = (AbString){(char *)label.data, label.length};
        if (status == ARCHBIRD_OK && evidence_index)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &label_text);
        ab_buffer_free(&label);
      }
      if (status == ARCHBIRD_OK)
        status = md_end(&body);
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Typed Fact",
                               &title,
                               NULL,
                               &description,
                               provenance->data,
                               "fact",
                               &path->name,
                               tags,
                               4,
                               &relations,
                               extra,
                               1,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_fields_free(pub, extra, 1);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static ArchbirdStatus finding_path(AbOkfPublication *pub,
                                   const AbString *fingerprint, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "verification/findings/");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, fingerprint->data, fingerprint->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static const AbString *operand_text(const AbValue *operands, const char *name) {
  const AbValue *value = ab_value_member(operands, name);
  return value && value->kind == AB_VALUE_STRING ? &value->as.text
                                                 : &EMPTY_TEXT;
}

static ArchbirdStatus operand_scalar(AbBuffer *body, const AbValue *operands,
                                     const char *name) {
  const AbValue *value = ab_value_member(operands, name);
  if (!value || value->kind == AB_VALUE_NULL)
    return ab_okf_pub_code(body, &EMPTY_TEXT);
  if (value->kind == AB_VALUE_STRING)
    return ab_okf_pub_code(body, &value->as.text);
  return ab_okf_pub_json_code(body, value);
}

static ArchbirdStatus add_check_fact_relation(AbOkfPublication *pub,
                                              AbOkfPubRelationList *relations,
                                              const VerifyPaths *paths,
                                              const char *kind,
                                              const AbString *name) {
  const VerifyPath *target = path_find(&paths->facts, name);
  return target ? relation_path(pub, relations, kind, &target->path)
                : ARCHBIRD_OK;
}

static ArchbirdStatus add_checks(AbOkfPublication *pub,
                                 const VerifyPaths *paths) {
  const char *summary_headers[] = {"Assertion", "Severity", "Owner",
                                   "Status",    "Coverage", "Findings"};
  const char *operand_headers[] = {"Role", "Value"};
  size_t check_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (check_index = 0;
       status == ARCHBIRD_OK && check_index < paths->checks.count;
       check_index++) {
    const VerifyPath *path = &paths->checks.items[check_index];
    const AbValue *check =
        row_by_name(pub->verification.checks, "id", &path->name);
    const AbString *assertion = ab_okf_pub_text(check, "assert");
    const AbString *severity = ab_okf_pub_text(check, "severity");
    const AbString *owner = ab_okf_pub_text(check, "owner");
    const AbString *raw_rationale = ab_okf_pub_text(check, "rationale");
    const AbString *check_status = ab_okf_pub_text(check, "status");
    const AbValue *coverage = array_or_empty(check, "coverage");
    const AbValue *findings = array_or_empty(check, "findings");
    const AbValue *requirements = array_or_empty(check, "requirements");
    const AbValue *witnesses = array_or_empty(check, "witnesses");
    const AbValue *operands =
        ab_okf_pub_member(check, "operands", AB_VALUE_OBJECT);
    const AbString *expected = operand_text(operands, "expected");
    const AbString *actual = operand_text(operands, "actual");
    char check_digest[65];
    AbOkfPubRelationList relations = {0};
    AbOkfPubField extra[1] = {{"check_sha256", {0}}};
    AbString title = {0};
    AbString description = {0};
    AbString tags[4] = {
        {(char *)"verification", 12}, {(char *)"check", 5}, {0}, {0}};
    AbBuffer body;
    size_t index;
    if (!check || !assertion || !severity || !owner || !raw_rationale ||
        !check_status || !coverage || !findings || !requirements ||
        !witnesses || !operands)
      return ab_okf_pub_error(pub, "invalid verification check row");
    tags[2] = *severity;
    tags[3] = *check_status;
    status = ab_okf_pub_relation_simple(pub, &relations, "part_of_suite",
                                        "verification/suite");
    if (status == ARCHBIRD_OK)
      status = add_check_fact_relation(pub, &relations, paths, "expected_fact",
                                       expected);
    if (status == ARCHBIRD_OK)
      status = add_check_fact_relation(pub, &relations, paths, "actual_fact",
                                       actual);
    for (index = 0;
         status == ARCHBIRD_OK && index < requirements->as.array.count;
         index++) {
      const AbValue *value = &requirements->as.array.items[index];
      const VerifyPath *target;
      if (value->kind != AB_VALUE_STRING)
        return ab_okf_pub_error(pub, "invalid check requirement");
      target = path_find(&paths->requirements, &value->as.text);
      if (target)
        status = relation_path(pub, &relations, "governed_by", &target->path);
    }
    for (index = 0; status == ARCHBIRD_OK && index < findings->as.array.count;
         index++) {
      const AbString *fingerprint =
          ab_okf_pub_text(&findings->as.array.items[index], "fingerprint");
      AbString target = {0};
      if (!fingerprint)
        return ab_okf_pub_error(pub, "invalid check finding fingerprint");
      status = finding_path(pub, fingerprint, &target);
      if (status == ARCHBIRD_OK)
        status = relation_path(pub, &relations, "has_finding", &target);
      ab_string_free(pub->engine, &target);
    }
    if (status == ARCHBIRD_OK)
      status = ab_act_value_digest(pub->engine, check, check_digest);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_literal(pub, &extra[0].json, check_digest);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_append(&value, severity->data, severity->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, assertion->data, assertion->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " check: ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&value, check_status->data, check_status->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, ".");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, raw_rationale);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\nThe check definition is asserted; "
                                        "its status, coverage, witnesses, "
                                        "and findings are derived from the "
                                        "canonical verification artifact.\n\n"
                                        "# Contract and result\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 6);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, assertion);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, severity);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, owner);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, check_status);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, coverage->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, findings->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Operands\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, operand_headers, 2);
    {
      const struct {
        const char *label;
        const char *name;
      } scalar_rows[] = {{"Mapping", "mapping"},
                         {"Minimum", "min"},
                         {"Maximum", "max"},
                         {"Exact", "exact"}};
      const VerifyPath *expected_path = path_find(&paths->facts, expected);
      const VerifyPath *actual_path = path_find(&paths->facts, actual);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "| Expected | ");
      if (status == ARCHBIRD_OK && expected_path)
        status = ab_okf_pub_relative_link(&body, path->path.data,
                                          expected_path->path.data, expected);
      else if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, expected);
      if (status == ARCHBIRD_OK)
        status = md_end(&body);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "| Actual | ");
      if (status == ARCHBIRD_OK && actual_path)
        status = ab_okf_pub_relative_link(&body, path->path.data,
                                          actual_path->path.data, actual);
      else if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, actual);
      if (status == ARCHBIRD_OK)
        status = md_end(&body);
      for (index = 0; status == ARCHBIRD_OK && index < 4; index++) {
        status = ab_buffer_literal(&body, "| ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&body, scalar_rows[index].label);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&body, " | ");
        if (status == ARCHBIRD_OK)
          status = operand_scalar(&body, operands, scalar_rows[index].name);
        if (status == ARCHBIRD_OK)
          status = md_end(&body);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Requirements\n\n");
    if (status == ARCHBIRD_OK && !requirements->as.array.count)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0;
         status == ARCHBIRD_OK && index < requirements->as.array.count;
         index++) {
      const AbString *requirement =
          &requirements->as.array.items[index].as.text;
      const VerifyPath *target = path_find(&paths->requirements, requirement);
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, path->path.data,
                                          target->path.data, requirement);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Findings\n\n");
    if (status == ARCHBIRD_OK && !findings->as.array.count)
      status = ab_buffer_literal(&body, "_None._\n");
    for (index = 0; status == ARCHBIRD_OK && index < findings->as.array.count;
         index++) {
      const AbValue *finding = &findings->as.array.items[index];
      const AbString *fingerprint = ab_okf_pub_text(finding, "fingerprint");
      const AbString *message = ab_okf_pub_text(finding, "message");
      AbString target = {0};
      status = finding_path(pub, fingerprint, &target);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, path->path.data, target.data,
                                          message);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
      ab_string_free(pub->engine, &target);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Witnesses\n\n");
    if (status == ARCHBIRD_OK)
      status = evidence_table(pub, &body, witnesses);
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Verification Check",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "verification_check",
                               &path->name,
                               tags,
                               4,
                               &relations,
                               extra,
                               1,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_fields_free(pub, extra, 1);
    ab_okf_pub_relations_free(pub, &relations);
  }
  return status;
}

static const AbString *
proposal_origin_fingerprint(const AbOkfPublication *pub) {
  const AbValue *finding =
      pub->has_proposal
          ? ab_okf_pub_member(pub->proposal.origin, "finding", AB_VALUE_OBJECT)
          : NULL;
  return finding ? ab_okf_pub_text(finding, "fingerprint") : NULL;
}

static ArchbirdStatus proposal_path_value(AbOkfPublication *pub,
                                          AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, "changes/proposals/");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, pub->proposal.sha256, 64);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, ".md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus add_findings(AbOkfPublication *pub,
                                   const VerifyPaths *paths) {
  const char *headers[] = {"Check",         "Comparison",  "Evidence",
                           "Applicability", "Disposition", "Baseline"};
  size_t check_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (check_index = 0; status == ARCHBIRD_OK &&
                        check_index < pub->verification.checks->as.array.count;
       check_index++) {
    const AbValue *check =
        &pub->verification.checks->as.array.items[check_index];
    const AbString *check_id = ab_okf_pub_text(check, "id");
    const VerifyPath *check_path = path_find(&paths->checks, check_id);
    const AbValue *findings = array_or_empty(check, "findings");
    size_t finding_index;
    if (!check_id || !check_path || !findings)
      return ab_okf_pub_error(pub, "invalid verification finding inventory");
    for (finding_index = 0;
         status == ARCHBIRD_OK && finding_index < findings->as.array.count;
         finding_index++) {
      const AbValue *finding = &findings->as.array.items[finding_index];
      const AbString *fingerprint = ab_okf_pub_text(finding, "fingerprint");
      const AbString *message = ab_okf_pub_text(finding, "message");
      const AbString *comparison = ab_okf_pub_text(finding, "comparison");
      const AbString *evidence_state =
          ab_okf_pub_text(finding, "evidence_state");
      const AbString *applicability = ab_okf_pub_text(finding, "applicability");
      const AbString *disposition = ab_okf_pub_text(finding, "disposition");
      const AbString *baseline = text_or_empty(finding, "baseline_state");
      const AbString *key = ab_okf_pub_text(finding, "key");
      const AbValue *evidence = array_or_empty(finding, "evidence");
      const AbString *proposal_fingerprint = proposal_origin_fingerprint(pub);
      int has_proposal = proposal_fingerprint && fingerprint &&
                         ab_string_equal(proposal_fingerprint, fingerprint);
      AbString path = {0};
      AbString proposal_path = {0};
      AbOkfPubRelationList relations = {0};
      AbOkfPubField extra[1] = {{"fingerprint", {0}}};
      AbString title = {0};
      AbString description = {0};
      AbString tags[5] = {
          {(char *)"verification", 12}, {(char *)"finding", 7}, {0}, {0}, {0}};
      AbBuffer body;
      if (!fingerprint || !message || !comparison || !evidence_state ||
          !applicability || !disposition || !baseline || !key || !evidence)
        return ab_okf_pub_error(pub, "invalid verification finding row");
      tags[2] = *comparison;
      tags[3] = *evidence_state;
      tags[4] = *disposition;
      status = finding_path(pub, fingerprint, &path);
      if (status == ARCHBIRD_OK)
        status =
            relation_path(pub, &relations, "finding_of", &check_path->path);
      if (status == ARCHBIRD_OK && has_proposal)
        status = proposal_path_value(pub, &proposal_path);
      if (status == ARCHBIRD_OK && has_proposal)
        status = relation_path(pub, &relations, "has_change_proposal",
                               &proposal_path);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_json_text(pub, &extra[0].json, fingerprint);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_copy(pub, &title, message->data, message->length);
      if (status == ARCHBIRD_OK) {
        AbBuffer value;
        ab_buffer_init(&value, pub->engine);
        status = ab_buffer_append(&value, comparison->data, comparison->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&value, " finding ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&value, key->data, key->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&value, " from check ");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&value, check_id->data, check_id->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&value, ".");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_buffer_string(pub, &value, &description);
        ab_buffer_free(&value);
      }
      ab_buffer_init(&body, pub->engine);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "# ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_plain(&body, message);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n\n");
      if (status == ARCHBIRD_OK)
        status = md_header(&body, headers, 6);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "| ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, path.data,
                                          check_path->path.data, check_id);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, comparison);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, evidence_state);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, applicability);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, disposition);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, baseline);
      if (status == ARCHBIRD_OK)
        status = md_end(&body);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n# Key\n\n");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, key);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n\n# Evidence\n\n");
      if (status == ARCHBIRD_OK)
        status = evidence_table(pub, &body, evidence);
      if (status == ARCHBIRD_OK && has_proposal)
        status = ab_buffer_literal(&body, "\n# Change proposal\n\n");
      if (status == ARCHBIRD_OK && has_proposal) {
        AbString label = {(char *)"Derived change proposal", 23};
        status = ab_okf_pub_relative_link(&body, path.data, proposal_path.data,
                                          &label);
      }
      if (status == ARCHBIRD_OK && has_proposal)
        status = ab_buffer_literal(&body, "\n");
      if (status == ARCHBIRD_OK) {
        AbOkfConceptSpec spec = {&pub->verification_source,
                                 path.data,
                                 "Archbird Verification Finding",
                                 &title,
                                 NULL,
                                 &description,
                                 "derived",
                                 "verification_finding",
                                 fingerprint,
                                 tags,
                                 5,
                                 &relations,
                                 extra,
                                 1,
                                 &body};
        status = ab_okf_pub_add_concept(pub, &spec);
      }
      ab_buffer_free(&body);
      ab_string_free(pub->engine, &path);
      ab_string_free(pub->engine, &proposal_path);
      ab_string_free(pub->engine, &title);
      ab_string_free(pub->engine, &description);
      ab_okf_pub_fields_free(pub, extra, 1);
      ab_okf_pub_relations_free(pub, &relations);
    }
  }
  return status;
}

static ArchbirdStatus add_attestations(AbOkfPublication *pub,
                                       const VerifyPaths *paths) {
  const char *summary_headers[] = {"Project", "State", "Revision",
                                   "Profile", "Cases", "Slice SHA-256"};
  const char *case_headers[] = {"Case", "Requirements", "Comparison",
                                "Routes/outcomes"};
  size_t attestation_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (attestation_index = 0;
       status == ARCHBIRD_OK && attestation_index < paths->attestations.count;
       attestation_index++) {
    const VerifyPath *path = &paths->attestations.items[attestation_index];
    const AbValue *row =
        row_by_name(pub->verification.attestations, "name", &path->name);
    const AbValue *attestation =
        ab_okf_pub_member(row, "attestation", AB_VALUE_OBJECT);
    const AbValue *cases =
        attestation ? array_or_empty(attestation, "cases") : NULL;
    const AbValue *profile =
        attestation ? ab_okf_pub_member(attestation, "profile", AB_VALUE_OBJECT)
                    : NULL;
    const AbString *project = text_or_empty(row, "project");
    const AbString *state = ab_okf_pub_text(row, "state");
    const AbString *revision = text_or_empty(attestation, "revision");
    const AbString *profile_id = text_or_empty(profile, "id");
    const AbString *slice = text_or_empty(attestation, "evidence_slice_sha256");
    AbString *requirements = NULL;
    size_t requirement_count = 0;
    size_t requirement_capacity = 0;
    size_t case_index;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[3] = {
        {(char *)"verification", 12}, {(char *)"attestation", 11}, {0}};
    AbBuffer body;
    if (!row || !attestation || !cases || !profile || !project || !state ||
        !revision || !profile_id || !slice) {
      status = ab_okf_pub_error(pub, "invalid behavioral attestation row");
      break;
    }
    tags[2] = *state;
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *case_row = &cases->as.array.items[case_index];
      const AbValue *case_requirements =
          array_or_empty(case_row, "requirements");
      size_t requirement_index;
      if (!case_requirements) {
        status = ab_okf_pub_error(
            pub, "invalid behavioral attestation requirements");
        break;
      }
      for (requirement_index = 0;
           status == ARCHBIRD_OK &&
           requirement_index < case_requirements->as.array.count;
           requirement_index++) {
        const AbValue *value =
            &case_requirements->as.array.items[requirement_index];
        if (value->kind != AB_VALUE_STRING)
          status = ab_okf_pub_error(
              pub, "invalid behavioral attestation requirement ID");
        else if (path_find(&paths->requirements, &value->as.text))
          status = name_add(pub, &requirements, &requirement_count,
                            &requirement_capacity, &value->as.text);
      }
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relation_simple(pub, &relations, "part_of_suite",
                                          "verification/suite");
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < requirement_count;
         case_index++) {
      const VerifyPath *target =
          path_find(&paths->requirements, &requirements[case_index]);
      status =
          relation_path(pub, &relations, "observes_requirement", &target->path);
    }
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_copy(pub, &title, path->name.data, path->name.length);
    if (status == ARCHBIRD_OK) {
      AbBuffer value;
      ab_buffer_init(&value, pub->engine);
      status = ab_buffer_literal(&value, "Observed behavior bundle with ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_u64(&value, cases->as.array.count);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, " cases in state ");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&value, state->data, state->length);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&value, ".");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_buffer_string(pub, &value, &description);
      ab_buffer_free(&value);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = md_header(&body, summary_headers, 6);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, project);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, state);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, revision);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, profile_id);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, cases->as.array.count);
    if (status == ARCHBIRD_OK)
      status = md_cell(&body);
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_code(&body, slice);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Cases\n\n");
    if (status == ARCHBIRD_OK && cases->as.array.count)
      status = md_header(&body, case_headers, 4);
    else if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "_None._\n");
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < cases->as.array.count;
         case_index++) {
      const AbValue *case_row = &cases->as.array.items[case_index];
      const AbString *id = text_or_empty(case_row, "id");
      const AbValue *case_requirements =
          array_or_empty(case_row, "requirements");
      const AbValue *comparison =
          ab_okf_pub_member(case_row, "comparison", AB_VALUE_OBJECT);
      const AbString *comparison_kind = text_or_empty(comparison, "kind");
      const AbValue *observations = array_or_empty(case_row, "observations");
      size_t index;
      if (!id || !case_requirements || !comparison || !comparison_kind ||
          !observations) {
        status = ab_okf_pub_error(pub, "invalid behavioral attestation case");
        break;
      }
      status = ab_buffer_literal(&body, "| ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, id);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (index = 0;
           status == ARCHBIRD_OK && index < case_requirements->as.array.count;
           index++) {
        const AbValue *value = &case_requirements->as.array.items[index];
        if (value->kind != AB_VALUE_STRING) {
          status = ab_okf_pub_error(
              pub, "invalid behavioral attestation requirement");
          break;
        }
        if (index)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &value->as.text);
      }
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_code(&body, comparison_kind);
      if (status == ARCHBIRD_OK)
        status = md_cell(&body);
      for (index = 0;
           status == ARCHBIRD_OK && index < observations->as.array.count;
           index++) {
        const AbValue *observation = &observations->as.array.items[index];
        const AbValue *outcome =
            ab_okf_pub_member(observation, "outcome", AB_VALUE_OBJECT);
        const AbString *route = text_or_empty(observation, "route");
        const AbString *kind = text_or_empty(outcome, "kind");
        const AbString *phase = text_or_empty(outcome, "phase");
        AbBuffer rendered;
        AbString rendered_text;
        if (!outcome || !route || !kind || !phase) {
          status = ab_okf_pub_error(pub, "invalid behavioral observation");
          break;
        }
        ab_buffer_init(&rendered, pub->engine);
        status = ab_buffer_append(&rendered, route->data, route->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&rendered, ":");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&rendered, kind->data, kind->length);
        if (status == ARCHBIRD_OK)
          status = ab_buffer_literal(&rendered, "/");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&rendered, phase->data, phase->length);
        rendered_text = (AbString){(char *)rendered.data, rendered.length};
        if (status == ARCHBIRD_OK && index)
          status = ab_buffer_literal(&body, "<br>");
        if (status == ARCHBIRD_OK)
          status = ab_okf_pub_code(&body, &rendered_text);
        ab_buffer_free(&rendered);
      }
      if (status == ARCHBIRD_OK)
        status = md_end(&body);
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Requirements\n\n");
    if (status == ARCHBIRD_OK && !requirement_count)
      status = ab_buffer_literal(&body, "_None._\n");
    for (case_index = 0;
         status == ARCHBIRD_OK && case_index < requirement_count;
         case_index++) {
      const VerifyPath *target =
          path_find(&paths->requirements, &requirements[case_index]);
      status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status =
            ab_okf_pub_relative_link(&body, path->path.data, target->path.data,
                                     &requirements[case_index]);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Behavioral Attestation",
                               &title,
                               NULL,
                               &description,
                               "observed",
                               "attestation",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &title);
    ab_string_free(pub->engine, &description);
    ab_okf_pub_relations_free(pub, &relations);
    names_free(pub, requirements, requirement_count);
  }
  return status;
}

static ArchbirdStatus add_diagnostics(AbOkfPublication *pub) {
  const char *headers[] = {"Diagnostic"};
  const AbValue *rows = array_or_empty(&pub->verification.root, "diagnostics");
  const AbString *suite_name = ab_okf_pub_text(pub->verification.suite, "name");
  AbOkfPubRelationList relations = {0};
  AbString title = {(char *)"Verification diagnostics", 24};
  AbString entity = {(char *)"verification", 12};
  AbString description = {0};
  AbString tags[] = {{(char *)"verification", 12}, {(char *)"diagnostics", 11}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status;
  if (!rows || !suite_name)
    return ab_okf_pub_error(pub, "invalid verification diagnostics");
  status = ab_okf_pub_relation_simple(pub, &relations, "part_of_suite",
                                      "verification/suite");
  if (status == ARCHBIRD_OK)
    status = dynamic_description(pub, &description, "", rows->as.array.count,
                                 " canonical verification diagnostics.");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Verification diagnostics\n\n");
  if (status == ARCHBIRD_OK && rows->as.array.count)
    status = md_header(&body, headers, 1);
  else if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < rows->as.array.count;
       index++) {
    status = ab_buffer_literal(&body, "| ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_code(&body, &rows->as.array.items[index]);
    if (status == ARCHBIRD_OK)
      status = md_end(&body);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Verification suite\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relative_link(&body, "verification/diagnostics.md",
                                      "verification/suite.md", suite_name);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->verification_source,
                             "verification/diagnostics.md",
                             "Archbird Verification Diagnostics",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "diagnostics",
                             &entity,
                             tags,
                             2,
                             &relations,
                             NULL,
                             0,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_string_free(pub->engine, &description);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}
