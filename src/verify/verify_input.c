#include "verify_runtime.h"

#include <string.h>

typedef struct InputContext {
  ArchbirdEngine *engine;
  const AbVerifySuiteView *suite;
} InputContext;

static ArchbirdStatus invalid(InputContext *context, const char *where,
                              const char *message) {
  return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                            ARCHBIRD_NO_OFFSET, "%s: %s", where, message);
}

static int string_equal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static int name_in(const AbString *name, const char *const *allowed,
                   size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_equal(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus reject_unknown(InputContext *context,
                                     const AbValue *object, const char *where,
                                     const char *const *allowed, size_t count) {
  size_t index;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return invalid(context, where, "expected object");
  for (index = 0; index < object->as.object.count; index++) {
    const AbString *name = &object->as.object.fields[index].name;
    if (!name_in(name, allowed, count))
      return archbird_error_set(context->engine, ARCHBIRD_INVALID_SCHEMA,
                                ARCHBIRD_NO_OFFSET, "%s: unknown field '%.*s'",
                                where, (int)name->length, name->data);
  }
  return ARCHBIRD_OK;
}

static const AbValue *named_row(const AbValue *rows, const AbString *name) {
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "name");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, name))
      return row;
  }
  return NULL;
}

const AbValue *ab_verify_input_project(const AbVerifyInputView *input,
                                       const AbString *name) {
  return input ? named_row(input->projects, name) : NULL;
}

const AbValue *ab_verify_input_source(const AbValue *project,
                                      const AbString *path) {
  const AbValue *sources = ab_value_member(project, "sources");
  size_t index;
  if (!sources || sources->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < sources->as.array.count; index++) {
    const AbValue *row = &sources->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return row;
  }
  return NULL;
}

const AbValue *ab_verify_input_provided_fact(const AbVerifyInputView *input,
                                             const AbString *name) {
  return input ? named_row(input->provided_facts, name) : NULL;
}

const AbValue *ab_verify_input_attestation(const AbVerifyInputView *input,
                                           const AbString *name) {
  return input ? named_row(input->attestations, name) : NULL;
}

static int suite_project_exists(const AbVerifySuiteView *suite,
                                const AbString *name) {
  size_t low = 0;
  size_t high = suite->projects->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(
        &suite->projects->as.object.fields[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static int suite_named_exists(const AbValue *object, const AbString *name) {
  size_t low = 0;
  size_t high;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return 0;
  high = object->as.object.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&object->as.object.fields[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_map(InputContext *context, const AbValue *map,
                                   const char *where) {
  const AbValue *artifact;
  const AbValue *schema;
  const AbValue *project;
  const AbValue *evidence;
  uint64_t version;
  if (!map || map->kind != AB_VALUE_OBJECT)
    return invalid(context, where, "map must be an object");
  artifact = ab_value_member(map, "artifact");
  schema = ab_value_member(map, "schema_version");
  project = ab_value_member(map, "project");
  evidence = ab_value_member(map, "evidence");
  if (!ab_verify_string_is(artifact, "map") ||
      !ab_value_u64(schema, &version) || version < ARCHBIRD_MAP_SCHEMA_MIN ||
      version > ARCHBIRD_MAP_SCHEMA_CURRENT || !ab_verify_nonblank(project) ||
      !evidence || evidence->kind != AB_VALUE_OBJECT ||
      !ab_verify_nonblank(ab_value_member(evidence, "config_sha256")) ||
      !ab_verify_nonblank(ab_value_member(evidence, "input_sha256")))
    return invalid(context, where, "invalid Archbird map evidence");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_sources(InputContext *context,
                                       const AbValue *sources,
                                       const char *where) {
  static const char *const allowed[] = {"error", "path", "sha256", "text"};
  size_t index;
  if (!sources || sources->kind != AB_VALUE_ARRAY)
    return invalid(context, where, "expected source array");
  for (index = 0; index < sources->as.array.count; index++) {
    const AbValue *row = &sources->as.array.items[index];
    const AbValue *path;
    const AbValue *text;
    const AbValue *sha256;
    const AbValue *error;
    size_t previous;
    if (reject_unknown(context, row, where, allowed, 4) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    path = ab_value_member(row, "path");
    text = ab_value_member(row, "text");
    sha256 = ab_value_member(row, "sha256");
    error = ab_value_member(row, "error");
    if (!ab_verify_path_is_repository(path) ||
        ((text != NULL) + (sha256 != NULL) + (error != NULL) != 1) ||
        (text && text->kind != AB_VALUE_STRING) ||
        (sha256 &&
         (sha256->kind != AB_VALUE_STRING || sha256->as.text.length != 64)) ||
        (error && error->kind != AB_VALUE_STRING))
      return invalid(context, where,
                     "sources require a repository path and exactly one of "
                     "UTF-8 text, lowercase SHA-256, or read error");
    if (sha256) {
      size_t digit;
      for (digit = 0; digit < sha256->as.text.length; digit++) {
        char byte = sha256->as.text.data[digit];
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
          return invalid(context, where, "invalid source SHA-256");
      }
    }
    if (text && text->as.text.length > 16000000)
      return invalid(context, where, "verification source exceeds 16 MB");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&sources->as.array.items[previous], "path");
      if (old && ab_string_equal(&path->as.text, &old->as.text))
        return invalid(context, where, "duplicate source path");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_projects(InputContext *context,
                                        const AbValue *projects) {
  static const char *const allowed[] = {"map", "name", "sources"};
  size_t index;
  if (!projects || projects->kind != AB_VALUE_ARRAY ||
      projects->as.array.count != context->suite->projects->as.object.count)
    return invalid(context, "input.projects",
                   "expected exactly one row per suite project");
  for (index = 0; index < projects->as.array.count; index++) {
    const AbValue *row = &projects->as.array.items[index];
    const AbValue *name;
    size_t previous;
    if (reject_unknown(context, row, "input.projects", allowed, 3) !=
        ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    name = ab_value_member(row, "name");
    if (!name || name->kind != AB_VALUE_STRING ||
        !suite_project_exists(context->suite, &name->as.text))
      return invalid(context, "input.projects", "unknown or missing project");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&projects->as.array.items[previous], "name");
      if (old && ab_string_equal(&name->as.text, &old->as.text))
        return invalid(context, "input.projects", "duplicate project");
    }
    if (validate_map(context, ab_value_member(row, "map"),
                     "input.projects.map") != ARCHBIRD_OK ||
        validate_sources(context, ab_value_member(row, "sources"),
                         "input.projects.sources") != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_provided_facts(InputContext *context,
                                              const AbValue *facts) {
  static const char *const allowed[] = {
      "items", "message", "name", "producer", "project", "shape", "state",
  };
  size_t index;
  if (!facts)
    return ARCHBIRD_OK;
  if (facts->kind != AB_VALUE_ARRAY)
    return invalid(context, "input.provided_facts", "expected array");
  for (index = 0; index < facts->as.array.count; index++) {
    const AbValue *row = &facts->as.array.items[index];
    const AbValue *name;
    const AbValue *project;
    const AbValue *items;
    size_t previous;
    if (reject_unknown(context, row, "input.provided_facts", allowed,
                       sizeof(allowed) / sizeof(allowed[0])) != ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    name = ab_value_member(row, "name");
    project = ab_value_member(row, "project");
    items = ab_value_member(row, "items");
    if (!name || name->kind != AB_VALUE_STRING ||
        !suite_named_exists(context->suite->extractors, &name->as.text) ||
        !project || project->kind != AB_VALUE_STRING ||
        !suite_project_exists(context->suite, &project->as.text) || !items ||
        items->kind != AB_VALUE_ARRAY ||
        !ab_verify_nonblank(ab_value_member(row, "shape")) ||
        !ab_verify_nonblank(ab_value_member(row, "state")) ||
        !ab_value_member(row, "producer") ||
        ab_value_member(row, "producer")->kind != AB_VALUE_OBJECT)
      return invalid(context, "input.provided_facts",
                     "invalid provided fact-set envelope");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&facts->as.array.items[previous], "name");
      if (old && ab_string_equal(&name->as.text, &old->as.text))
        return invalid(context, "input.provided_facts",
                       "duplicate fact-set name");
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_attestations(InputContext *context,
                                            const AbValue *rows) {
  static const char *const allowed[] = {"document", "error", "name", "path"};
  size_t expected = context->suite->attestations
                        ? context->suite->attestations->as.object.count
                        : 0;
  size_t index;
  if (!rows || rows->kind != AB_VALUE_ARRAY || rows->as.array.count != expected)
    return invalid(context, "input.attestations",
                   "expected exactly one row per suite attestation");
  for (index = 0; index < rows->as.array.count; index++) {
    const AbValue *row = &rows->as.array.items[index];
    const AbValue *name;
    const AbValue *path;
    const AbValue *document;
    const AbValue *error;
    size_t previous;
    if (reject_unknown(context, row, "input.attestations", allowed, 4) !=
        ARCHBIRD_OK)
      return ARCHBIRD_INVALID_SCHEMA;
    name = ab_value_member(row, "name");
    path = ab_value_member(row, "path");
    document = ab_value_member(row, "document");
    error = ab_value_member(row, "error");
    if (!name || name->kind != AB_VALUE_STRING ||
        !suite_named_exists(context->suite->attestations, &name->as.text) ||
        !path || path->kind != AB_VALUE_STRING || !!document == !!error ||
        (document && document->kind != AB_VALUE_OBJECT) ||
        (error && error->kind != AB_VALUE_STRING))
      return invalid(context, "input.attestations",
                     "invalid attestation input row");
    for (previous = 0; previous < index; previous++) {
      const AbValue *old =
          ab_value_member(&rows->as.array.items[previous], "name");
      if (old && ab_string_equal(&name->as.text, &old->as.text))
        return invalid(context, "input.attestations", "duplicate attestation");
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_verify_input_validate(ArchbirdEngine *engine,
                                        const AbVerifySuiteView *suite,
                                        const AbValue *root,
                                        AbVerifyInputView *out) {
  static const char *const allowed[] = {
      "artifact",       "attestations",   "baseline",   "projects",
      "provided_facts", "schema_version", "suite_path",
  };
  InputContext context;
  const AbValue *schema;
  uint64_t version;
  ArchbirdStatus status;
  if (!engine || !suite || !root || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  context.engine = engine;
  context.suite = suite;
  status = reject_unknown(&context, root, "verification input", allowed,
                          sizeof(allowed) / sizeof(allowed[0]));
  schema = ab_value_member(root, "schema_version");
  if (status == ARCHBIRD_OK &&
      (!ab_value_u64(schema, &version) || version != 1 ||
       !ab_verify_string_is(ab_value_member(root, "artifact"),
                            "verification-input") ||
       !ab_verify_nonblank(ab_value_member(root, "suite_path"))))
    status = invalid(&context, "verification input",
                     "invalid schema, artifact, or suite_path");
  out->engine = engine;
  out->root = root;
  out->suite_path = ab_value_member(root, "suite_path");
  out->projects = ab_value_member(root, "projects");
  out->provided_facts = ab_value_member(root, "provided_facts");
  out->attestations = ab_value_member(root, "attestations");
  out->baseline = ab_value_member(root, "baseline");
  if (status == ARCHBIRD_OK)
    status = validate_projects(&context, out->projects);
  if (status == ARCHBIRD_OK)
    status = validate_provided_facts(&context, out->provided_facts);
  if (status == ARCHBIRD_OK)
    status = validate_attestations(&context, out->attestations);
  if (status == ARCHBIRD_OK && out->baseline &&
      out->baseline->kind != AB_VALUE_OBJECT &&
      out->baseline->kind != AB_VALUE_NULL)
    status = invalid(&context, "input.baseline", "expected object or null");
  return status;
}
