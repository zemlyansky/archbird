#include "okf_publish_internal.h"

#include <stdlib.h>
#include <string.h>

#define VERIFY_TRY(expression)                                                 \
  do {                                                                         \
    ArchbirdStatus verify_status_ = (expression);                              \
    if (verify_status_ != ARCHBIRD_OK)                                         \
      return verify_status_;                                                   \
  } while (0)

typedef struct VerificationPath {
  AbString name;
  AbString slug;
  AbString path;
} VerificationPath;

typedef struct VerificationPaths {
  VerificationPath *items;
  size_t count;
} VerificationPaths;

typedef struct VerificationInventory {
  VerificationPaths constraints;
  VerificationPaths operands;
  VerificationPaths mappings;
} VerificationInventory;

static const AbValue EMPTY_ARRAY = {.kind = AB_VALUE_ARRAY};
static const AbString EMPTY_TEXT = {(char *)"", 0};

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
  return ab_string_compare(&((const VerificationPath *)left)->name,
                           &((const VerificationPath *)right)->name);
}

static void paths_free(AbOkfPublication *pub, VerificationPaths *paths) {
  size_t index;
  if (!paths)
    return;
  for (index = 0; index < paths->count; index++) {
    ab_string_free(pub->engine, &paths->items[index].slug);
    ab_string_free(pub->engine, &paths->items[index].path);
  }
  ab_free(pub->engine, paths->items);
  memset(paths, 0, sizeof(*paths));
}

static void inventory_free(AbOkfPublication *pub,
                           VerificationInventory *inventory) {
  paths_free(pub, &inventory->mappings);
  paths_free(pub, &inventory->operands);
  paths_free(pub, &inventory->constraints);
}

static const VerificationPath *path_find(const VerificationPaths *paths,
                                         const AbString *name) {
  VerificationPath key = {0};
  if (!paths || !paths->count || !name)
    return NULL;
  key.name = *name;
  return (const VerificationPath *)bsearch(&key, paths->items, paths->count,
                                           sizeof(*paths->items), path_compare);
}

static ArchbirdStatus path_suffix(AbOkfPublication *pub,
                                  VerificationPath *row) {
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

static ArchbirdStatus paths_build(AbOkfPublication *pub, const AbString *names,
                                  size_t count, const char *prefix,
                                  VerificationPaths *out) {
  size_t index;
  size_t other;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!count)
    return ARCHBIRD_OK;
  if (count > SIZE_MAX / sizeof(*out->items) ||
      count > pub->engine->options.max_values)
    return archbird_error_set(pub->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification publication paths");
  out->items =
      (VerificationPath *)ab_calloc(pub->engine, count, sizeof(*out->items));
  if (!out->items)
    return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory building verification paths");
  out->count = count;
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    out->items[index].name = names[index];
    status = ab_okf_pub_slug(pub, &names[index], &out->items[index].slug);
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(out->items, count, sizeof(*out->items), path_compare);
  for (index = 1; status == ARCHBIRD_OK && index < count; index++)
    if (ab_string_equal(&out->items[index - 1].name, &out->items[index].name))
      status =
          ab_okf_pub_error(pub, "duplicate verification publication identity");
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
    paths_free(pub, out);
  return status;
}

static ArchbirdStatus paths_from_rows(AbOkfPublication *pub,
                                      const AbValue *rows, const char *field,
                                      const char *prefix,
                                      VerificationPaths *out) {
  AbString *names = NULL;
  size_t index;
  ArchbirdStatus status;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return ab_okf_pub_error(pub, "invalid verification publication inventory");
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
  status = paths_build(pub, names, rows->as.array.count, prefix, out);
  ab_free(pub->engine, names);
  return status;
}

static ArchbirdStatus paths_from_object(AbOkfPublication *pub,
                                        const AbValue *object,
                                        const char *prefix,
                                        VerificationPaths *out) {
  AbString *names = NULL;
  size_t index;
  ArchbirdStatus status;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return ab_okf_pub_error(pub, "invalid verification publication object");
  if (object->as.object.count) {
    names = (AbString *)ab_calloc(pub->engine, object->as.object.count,
                                  sizeof(*names));
    if (!names)
      return archbird_error_set(pub->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory reading verification keys");
  }
  for (index = 0; index < object->as.object.count; index++)
    names[index] = object->as.object.fields[index].name;
  status = paths_build(pub, names, object->as.object.count, prefix, out);
  ab_free(pub->engine, names);
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

static const AbValue *object_value(const AbValue *object,
                                   const AbString *name) {
  size_t index;
  for (index = 0; object && index < object->as.object.count; index++)
    if (ab_string_equal(&object->as.object.fields[index].name, name))
      return &object->as.object.fields[index].value;
  return NULL;
}

static ArchbirdStatus relation_path(AbOkfPublication *pub,
                                    AbOkfPubRelationList *relations,
                                    const char *kind, const char *path) {
  AbBuffer target;
  size_t length = strlen(path);
  ArchbirdStatus status;
  if (length < 3 || memcmp(path + length - 3, ".md", 3))
    return ab_okf_pub_error(pub, "invalid verification relation path");
  ab_buffer_init(&target, pub->engine);
  status = ab_buffer_append(&target, path, length - 3);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, relations, kind,
                                        (const char *)target.data);
  ab_buffer_free(&target);
  return status;
}

static ArchbirdStatus title_with_name(AbOkfPublication *pub, AbString *out,
                                      const char *prefix,
                                      const AbString *name) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, pub->engine);
  status = ab_buffer_literal(&buffer, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, name->data, name->length);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_buffer_string(pub, &buffer, out);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus append_field(AbBuffer *body, const char *label,
                                   const AbValue *object, const char *field) {
  const AbValue *value = ab_value_member(object, field);
  if (!value)
    return ARCHBIRD_OK;
  VERIFY_TRY(ab_buffer_literal(body, "| "));
  VERIFY_TRY(ab_buffer_literal(body, label));
  VERIFY_TRY(ab_buffer_literal(body, " | "));
  VERIFY_TRY(ab_okf_pub_json_code(body, value));
  return ab_buffer_literal(body, " |\n");
}

static ArchbirdStatus table_start(AbBuffer *body) {
  return ab_buffer_literal(body, "| Field | Value |\n| --- | --- |\n");
}

static ArchbirdStatus add_policy(AbOkfPublication *pub,
                                 const VerificationInventory *inventory) {
  const AbValue *policy = pub->verification.policy;
  const AbValue *summary =
      ab_okf_pub_member(&pub->verification.root, "summary", AB_VALUE_OBJECT);
  const AbString *project = ab_okf_pub_text(policy, "project");
  const AbString *policy_sha =
      ab_okf_pub_text(policy, "constraint_policy_sha256");
  AbOkfPubRelationList relations = {0};
  AbOkfPubField extra[2] = {{"constraint_policy_sha256", {0}},
                            {"verification_result_sha256", {0}}};
  AbString title = {(char *)"Architecture constraints", 24};
  AbString description = {(char *)"Reviewed architectural policy evaluated "
                                  "over exhaustive operands.",
                          65};
  AbString tags[] = {{(char *)"architecture", 12},
                     {(char *)"constraints", 11},
                     {(char *)"verification", 12}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status;
  if (!policy || !summary || !project || !policy_sha)
    return ab_okf_pub_error(pub, "invalid verification policy publication");
  status =
      relation_path(pub, &relations, "evaluates", "verification/project.md");
  for (index = 0; status == ARCHBIRD_OK && index < inventory->constraints.count;
       index++)
    status = relation_path(pub, &relations, "contains",
                           inventory->constraints.items[index].path.data);
  for (index = 0; status == ARCHBIRD_OK && index < inventory->operands.count;
       index++)
    status = relation_path(pub, &relations, "uses",
                           inventory->operands.items[index].path.data);
  if (status == ARCHBIRD_OK)
    status = relation_path(pub, &relations, "contains",
                           "verification/diagnostics.md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(pub, &extra[0].json, policy_sha);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_text(
        pub, &extra[1].json,
        ab_okf_pub_text(&pub->verification.root, "verification_result_sha256"));
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Architecture constraints\n\n");
  if (status == ARCHBIRD_OK)
    status = table_start(&body);
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Project", policy, "project");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Constraint policy SHA-256", policy,
                          "constraint_policy_sha256");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Selection", policy, "kind");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Configured", policy, "configured_count");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Evaluated", policy, "evaluated_count");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n# Result summary\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_json_code(&body, summary);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n# Constraints\n\n");
  if (status == ARCHBIRD_OK && !inventory->constraints.count)
    status = ab_buffer_literal(&body, "_None evaluated._\n");
  for (index = 0; status == ARCHBIRD_OK && index < inventory->constraints.count;
       index++) {
    status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_relative_link(
          &body, "verification/policy.md",
          inventory->constraints.items[index].path.data,
          &inventory->constraints.items[index].name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->verification_source,
                             "verification/policy.md",
                             "Archbird Constraint Policy",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "constraint-policy",
                             project,
                             tags,
                             3,
                             &relations,
                             extra,
                             2,
                             &body};
    status = ab_okf_pub_add_concept(pub, &spec);
  }
  ab_buffer_free(&body);
  ab_okf_pub_fields_free(pub, extra, 2);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus add_evaluation(AbOkfPublication *pub) {
  const AbValue *evaluation = pub->verification.evaluation;
  const AbString *project = ab_okf_pub_text(evaluation, "project");
  AbOkfPubRelationList relations = {0};
  AbString title = {0};
  AbString description = {(char *)"Map identity consumed by the constraint "
                                  "evaluation.",
                          51};
  AbString tags[] = {{(char *)"architecture", 12},
                     {(char *)"evaluation", 10},
                     {(char *)"verification", 12}};
  AbBuffer body;
  ArchbirdStatus status;
  if (!project)
    return ab_okf_pub_error(pub, "invalid verification evaluation");
  status = title_with_name(pub, &title, "Constraint evaluation: ", project);
  if (status == ARCHBIRD_OK)
    status = relation_path(pub, &relations, "part_of_policy",
                           "verification/policy.md");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_relation_simple(pub, &relations, "evaluates",
                                        "architecture/project");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Evaluated project\n\n");
  if (status == ARCHBIRD_OK)
    status = table_start(&body);
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Project", evaluation, "project");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Map input SHA-256", evaluation,
                          "map_input_sha256");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Map config SHA-256", evaluation,
                          "map_config_sha256");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Map producer implementation", evaluation,
                          "map_producer_implementation_sha256");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Resolution SHA-256", evaluation,
                          "resolution_sha256");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->verification_source,
                             "verification/project.md",
                             "Archbird Constraint Evaluation",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "constraint-evaluation",
                             project,
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
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus add_operands(AbOkfPublication *pub,
                                   const VerificationInventory *inventory) {
  size_t index;
  for (index = 0; index < inventory->operands.count; index++) {
    const VerificationPath *path = &inventory->operands.items[index];
    const AbValue *row =
        row_by_name(pub->verification.operands, "name", &path->name);
    const AbValue *definition =
        object_value(pub->verification.operand_definitions, &path->name);
    const AbValue *items = row ? array_or_empty(row, "items") : NULL;
    const AbString *message = row ? text_or_empty(row, "message") : NULL;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"operand", 7},
                       {(char *)"projection", 10},
                       {(char *)"verification", 12}};
    AbBuffer body;
    ArchbirdStatus status;
    if (!row || !definition || !items || !message)
      return ab_okf_pub_error(pub, "invalid verification operand");
    status = title_with_name(pub, &title, "Operand: ", &path->name);
    if (status == ARCHBIRD_OK)
      status = message->length
                   ? ab_okf_pub_copy(pub, &description, message->data,
                                     message->length)
                   : ab_okf_pub_literal(
                         pub, &description,
                         "Exhaustive typed operand for constraint evaluation.");
    if (status == ARCHBIRD_OK)
      status = relation_path(pub, &relations, "part_of_policy",
                             "verification/policy.md");
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = table_start(&body);
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Project", row, "project");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Shape", row, "shape");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "State", row, "state");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Provenance", row, "provenance");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Operand SHA-256", row, "sha256");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Definition\n\n");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_code(&body, definition);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n# Values\n\n");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_u64(&body, items->as.array.count);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, " exhaustive values.\n");
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Constraint Operand",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "constraint-operand",
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
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_mappings(AbOkfPublication *pub,
                                   const VerificationInventory *inventory) {
  size_t index;
  for (index = 0; index < inventory->mappings.count; index++) {
    const VerificationPath *path = &inventory->mappings.items[index];
    const AbValue *mapping =
        object_value(pub->verification.mappings, &path->name);
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {
        (char *)"Reviewed correspondence used by a constraint predicate.", 55};
    AbString tags[] = {{(char *)"mapping", 7}, {(char *)"verification", 12}};
    AbBuffer body;
    ArchbirdStatus status;
    if (!mapping)
      return ab_okf_pub_error(pub, "invalid verification mapping");
    status = title_with_name(pub, &title, "Mapping: ", &path->name);
    if (status == ARCHBIRD_OK)
      status = relation_path(pub, &relations, "part_of_policy",
                             "verification/policy.md");
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_code(&body, mapping);
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Constraint Mapping",
                               &title,
                               NULL,
                               &description,
                               "asserted",
                               "constraint-mapping",
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
    ab_okf_pub_relations_free(pub, &relations);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
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

static ArchbirdStatus add_finding(AbOkfPublication *pub,
                                  const VerificationPath *constraint_path,
                                  const AbValue *finding) {
  const AbString *fingerprint = ab_okf_pub_text(finding, "fingerprint");
  const AbString *message = ab_okf_pub_text(finding, "message");
  AbOkfPubRelationList relations = {0};
  AbString path = {0};
  AbString title = {0};
  AbString description = {0};
  AbString tags[] = {{(char *)"finding", 7}, {(char *)"verification", 12}};
  AbBuffer body;
  ArchbirdStatus status;
  if (!fingerprint || !message)
    return ab_okf_pub_error(pub, "invalid verification finding");
  status = finding_path(pub, fingerprint, &path);
  if (status == ARCHBIRD_OK)
    status = title_with_name(pub, &title, "Finding: ", fingerprint);
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_copy(pub, &description, message->data, message->length);
  if (status == ARCHBIRD_OK)
    status = relation_path(pub, &relations, "finding_of",
                           constraint_path->path.data);
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Finding\n\n");
  if (status == ARCHBIRD_OK)
    status = ab_okf_pub_plain(&body, message);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "\n\n");
  if (status == ARCHBIRD_OK)
    status = table_start(&body);
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Fingerprint", finding, "fingerprint");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Key", finding, "key");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Comparison", finding, "comparison");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Applicability", finding, "applicability");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Disposition", finding, "disposition");
  if (status == ARCHBIRD_OK)
    status = append_field(&body, "Evidence state", finding, "evidence_state");
  if (status == ARCHBIRD_OK) {
    AbOkfConceptSpec spec = {&pub->verification_source,
                             path.data,
                             "Archbird Constraint Finding",
                             &title,
                             NULL,
                             &description,
                             "derived",
                             "constraint-finding",
                             fingerprint,
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
  ab_string_free(pub->engine, &title);
  ab_string_free(pub->engine, &path);
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

static ArchbirdStatus add_constraints(AbOkfPublication *pub,
                                      const VerificationInventory *inventory) {
  size_t index;
  for (index = 0; index < inventory->constraints.count; index++) {
    const VerificationPath *path = &inventory->constraints.items[index];
    const AbValue *row =
        row_by_name(pub->verification.constraints, "id", &path->name);
    const AbValue *operands = row ? ab_value_member(row, "operands") : NULL;
    const AbValue *findings = row ? array_or_empty(row, "findings") : NULL;
    const AbString *rationale = row ? text_or_empty(row, "rationale") : NULL;
    AbOkfPubRelationList relations = {0};
    AbString title = {0};
    AbString description = {0};
    AbString tags[] = {{(char *)"architecture", 12},
                       {(char *)"constraint", 10},
                       {(char *)"verification", 12}};
    AbBuffer body;
    size_t field_index;
    size_t finding_index;
    ArchbirdStatus status;
    if (!row || !operands || operands->kind != AB_VALUE_OBJECT || !findings ||
        !rationale)
      return ab_okf_pub_error(pub, "invalid verification constraint");
    status = title_with_name(pub, &title, "Constraint: ", &path->name);
    if (status == ARCHBIRD_OK)
      status = rationale->length
                   ? ab_okf_pub_copy(pub, &description, rationale->data,
                                     rationale->length)
                   : ab_okf_pub_literal(pub, &description,
                                        "Reviewed architectural constraint.");
    if (status == ARCHBIRD_OK)
      status = relation_path(pub, &relations, "part_of_policy",
                             "verification/policy.md");
    for (field_index = 0;
         status == ARCHBIRD_OK && field_index < operands->as.object.count;
         field_index++) {
      const AbValue *value = &operands->as.object.fields[field_index].value;
      const VerificationPath *target;
      if (value->kind != AB_VALUE_STRING || !value->as.text.length)
        continue;
      target = path_find(&inventory->operands, &value->as.text);
      if (!target)
        target = path_find(&inventory->mappings, &value->as.text);
      if (target)
        status = relation_path(pub, &relations, "uses", target->path.data);
    }
    ab_buffer_init(&body, pub->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "# ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, &path->name);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_plain(&body, rationale);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n\n");
    if (status == ARCHBIRD_OK)
      status = table_start(&body);
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Status", row, "status");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Assertion", row, "assert");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Severity", row, "severity");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Owner", row, "owner");
    if (status == ARCHBIRD_OK)
      status = append_field(&body, "Coverage", row, "coverage");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Operands\n\n");
    for (field_index = 0;
         status == ARCHBIRD_OK && field_index < operands->as.object.count;
         field_index++) {
      const AbObjectField *field = &operands->as.object.fields[field_index];
      const VerificationPath *target = NULL;
      status = ab_buffer_literal(&body, "* **");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_plain(&body, &field->name);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, ":** ");
      if (field->value.kind == AB_VALUE_STRING)
        target = path_find(&inventory->operands, &field->value.as.text);
      if (!target && field->value.kind == AB_VALUE_STRING)
        target = path_find(&inventory->mappings, &field->value.as.text);
      if (status == ARCHBIRD_OK && target)
        status = ab_okf_pub_relative_link(
            &body, path->path.data, target->path.data, &field->value.as.text);
      else if (status == ARCHBIRD_OK)
        status = ab_okf_pub_json_code(&body, &field->value);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
    }
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n# Findings\n\n");
    if (status == ARCHBIRD_OK && !findings->as.array.count)
      status = ab_buffer_literal(&body, "_None._\n");
    for (finding_index = 0;
         status == ARCHBIRD_OK && finding_index < findings->as.array.count;
         finding_index++) {
      const AbValue *finding = &findings->as.array.items[finding_index];
      const AbString *fingerprint = ab_okf_pub_text(finding, "fingerprint");
      const AbString *message = ab_okf_pub_text(finding, "message");
      AbString finding_target = {0};
      if (!fingerprint || !message)
        status = ab_okf_pub_error(pub, "invalid constraint finding");
      if (status == ARCHBIRD_OK)
        status = finding_path(pub, fingerprint, &finding_target);
      if (status == ARCHBIRD_OK)
        status =
            relation_path(pub, &relations, "has_finding", finding_target.data);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "* ");
      if (status == ARCHBIRD_OK)
        status = ab_okf_pub_relative_link(&body, path->path.data,
                                          finding_target.data, message);
      if (status == ARCHBIRD_OK)
        status = ab_buffer_literal(&body, "\n");
      ab_string_free(pub->engine, &finding_target);
    }
    if (status == ARCHBIRD_OK) {
      AbOkfConceptSpec spec = {&pub->verification_source,
                               path->path.data,
                               "Archbird Architecture Constraint",
                               &title,
                               NULL,
                               &description,
                               "derived",
                               "constraint",
                               &path->name,
                               tags,
                               3,
                               &relations,
                               NULL,
                               0,
                               &body};
      status = ab_okf_pub_add_concept(pub, &spec);
    }
    for (finding_index = 0;
         status == ARCHBIRD_OK && finding_index < findings->as.array.count;
         finding_index++)
      status = add_finding(pub, path, &findings->as.array.items[finding_index]);
    ab_buffer_free(&body);
    ab_string_free(pub->engine, &description);
    ab_string_free(pub->engine, &title);
    ab_okf_pub_relations_free(pub, &relations);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_diagnostics(AbOkfPublication *pub) {
  const AbValue *diagnostics =
      array_or_empty(&pub->verification.root, "diagnostics");
  AbOkfPubRelationList relations = {0};
  AbString title = {(char *)"Verification diagnostics", 24};
  AbString description = {
      (char *)"Evidence-quality diagnostics from constraint evaluation.", 56};
  AbString entity = {(char *)"verification", 12};
  AbString tags[] = {{(char *)"diagnostics", 11}, {(char *)"verification", 12}};
  AbBuffer body;
  size_t index;
  ArchbirdStatus status;
  if (!diagnostics)
    return ab_okf_pub_error(pub, "invalid verification diagnostics");
  status = relation_path(pub, &relations, "part_of_policy",
                         "verification/policy.md");
  ab_buffer_init(&body, pub->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&body, "# Verification diagnostics\n\n");
  if (status == ARCHBIRD_OK && !diagnostics->as.array.count)
    status = ab_buffer_literal(&body, "_None._\n");
  for (index = 0; status == ARCHBIRD_OK && index < diagnostics->as.array.count;
       index++) {
    status = ab_buffer_literal(&body, "* ");
    if (status == ARCHBIRD_OK)
      status = ab_okf_pub_json_code(&body, &diagnostics->as.array.items[index]);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&body, "\n");
  }
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
  ab_okf_pub_relations_free(pub, &relations);
  return status;
}

ArchbirdStatus ab_okf_pub_verify(AbOkfPublication *pub) {
  VerificationInventory inventory = {0};
  ArchbirdStatus status;
  if (!pub || !pub->has_verification)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = paths_from_rows(pub, pub->verification.constraints, "id",
                           "verification/constraints/", &inventory.constraints);
  if (status == ARCHBIRD_OK)
    status = paths_from_rows(pub, pub->verification.operands, "name",
                             "verification/operands/", &inventory.operands);
  if (status == ARCHBIRD_OK)
    status = paths_from_object(pub, pub->verification.mappings,
                               "verification/mappings/", &inventory.mappings);
  if (status == ARCHBIRD_OK)
    status = add_policy(pub, &inventory);
  if (status == ARCHBIRD_OK)
    status = add_evaluation(pub);
  if (status == ARCHBIRD_OK)
    status = add_operands(pub, &inventory);
  if (status == ARCHBIRD_OK)
    status = add_mappings(pub, &inventory);
  if (status == ARCHBIRD_OK)
    status = add_constraints(pub, &inventory);
  if (status == ARCHBIRD_OK)
    status = add_diagnostics(pub);
  inventory_free(pub, &inventory);
  return status;
}

#undef VERIFY_TRY
