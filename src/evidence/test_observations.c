#include "test_observations.h"

#include "archbird_internal.h"
#include "json_value.h"
#include "render_internal.h"
#include "sha256.h"

#include <string.h>

static ArchbirdStatus fail(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "%s", message);
}

static int string_is(const AbValue *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->kind == AB_VALUE_STRING &&
         value->as.text.length == length &&
         (!length || memcmp(value->as.text.data, literal, length) == 0);
}

static int nonempty_string(const AbValue *value) {
  return value && value->kind == AB_VALUE_STRING && value->as.text.length;
}

static int digest_string(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++)
    if (!((value->as.text.data[index] >= '0' &&
           value->as.text.data[index] <= '9') ||
          (value->as.text.data[index] >= 'a' &&
           value->as.text.data[index] <= 'f')))
      return 0;
  return 1;
}

static int field_allowed(const AbObjectField *field, const char *const *allowed,
                         size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    size_t length = strlen(allowed[index]);
    if (field->name.length == length &&
        !memcmp(field->name.data, allowed[index], length))
      return 1;
  }
  return 0;
}

static int exact_object(const AbValue *value, const char *const *allowed,
                        size_t count) {
  size_t index;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < value->as.object.count; index++)
    if (!field_allowed(&value->as.object.fields[index], allowed, count))
      return 0;
  return 1;
}

static const AbManifestFile *manifest_file(const ArchbirdProject *project,
                                           const AbValue *path) {
  const AbSourceManifest *manifest;
  size_t low = 0;
  size_t high;
  if (!project || !nonempty_string(path))
    return NULL;
  manifest = ab_project_manifest(project);
  high = manifest->file_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared =
        ab_string_compare(&manifest->files[middle].path, &path->as.text);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &manifest->files[middle];
  }
  return NULL;
}

static int pair_order(const AbValue *left, const AbValue *right,
                      const char *first, const char *second) {
  const AbValue *left_first = ab_value_member(left, first);
  const AbValue *right_first = ab_value_member(right, first);
  const AbValue *left_second = ab_value_member(left, second);
  const AbValue *right_second = ab_value_member(right, second);
  int compared = ab_string_compare(&left_first->as.text, &right_first->as.text);
  return compared
             ? compared
             : ab_string_compare(&left_second->as.text, &right_second->as.text);
}

static int triple_order(const AbValue *left, const AbValue *right,
                        const char *first, const char *second,
                        const char *third) {
  const AbValue *left_first = ab_value_member(left, first);
  const AbValue *right_first = ab_value_member(right, first);
  const AbValue *left_second = ab_value_member(left, second);
  const AbValue *right_second = ab_value_member(right, second);
  const AbValue *left_third = ab_value_member(left, third);
  const AbValue *right_third = ab_value_member(right, third);
  int compared = ab_string_compare(&left_first->as.text, &right_first->as.text);
  if (compared)
    return compared;
  compared = ab_string_compare(&left_second->as.text, &right_second->as.text);
  return compared
             ? compared
             : ab_string_compare(&left_third->as.text, &right_third->as.text);
}

static ArchbirdStatus validate_source(ArchbirdEngine *engine,
                                      const ArchbirdProject *project,
                                      const AbValue *source) {
  static const char *const source_fields[] = {
      "config_sha256", "evidence", "evidence_slice_sha256", "map_input_sha256"};
  static const char *const evidence_fields[] = {"path", "role", "sha256"};
  const AbValue *map_input;
  const AbValue *config;
  const AbValue *slice;
  const AbValue *evidence;
  AbBuffer canonical;
  uint8_t digest[32];
  char hex[65];
  size_t index;
  int runner = 0;
  int subject = 0;
  int inventory = 0;
  ArchbirdStatus status;
  if (!exact_object(source, source_fields,
                    sizeof(source_fields) / sizeof(source_fields[0])))
    return fail(engine, "observations.source shape is invalid");
  map_input = ab_value_member(source, "map_input_sha256");
  config = ab_value_member(source, "config_sha256");
  slice = ab_value_member(source, "evidence_slice_sha256");
  evidence = ab_value_member(source, "evidence");
  if (!digest_string(map_input) || !digest_string(config) ||
      !digest_string(slice) || !evidence || evidence->kind != AB_VALUE_ARRAY ||
      evidence->as.array.count < 3)
    return fail(engine, "observations.source evidence is invalid");
  if (project && memcmp(map_input->as.text.data,
                        archbird_project_map_input_sha256(project), 64) != 0)
    return fail(engine, "observations source does not match current Map input");
  if (project && (!archbird_project_config_sha256(project) ||
                  memcmp(config->as.text.data,
                         archbird_project_config_sha256(project), 64) != 0))
    return fail(engine,
                "observations source does not match current Map config");
  for (index = 0; index < evidence->as.array.count; index++) {
    const AbValue *row = &evidence->as.array.items[index];
    const AbValue *role;
    const AbValue *path;
    const AbValue *sha256;
    const AbManifestFile *file;
    if (!exact_object(row, evidence_fields,
                      sizeof(evidence_fields) / sizeof(evidence_fields[0])))
      return fail(engine, "observations evidence row is invalid");
    role = ab_value_member(row, "role");
    path = ab_value_member(row, "path");
    sha256 = ab_value_member(row, "sha256");
    if (!nonempty_string(role) || !nonempty_string(path) ||
        !digest_string(sha256) ||
        (!string_is(role, "runner") && !string_is(role, "subject") &&
         !string_is(role, "test_inventory")))
      return fail(engine, "observations evidence identity is invalid");
    if (project) {
      char file_hex[65];
      file = manifest_file(project, path);
      if (!file)
        return fail(engine, "observations evidence path is not mapped");
      archbird_sha256_hex(file->sha256, file_hex);
      if (memcmp(file_hex, sha256->as.text.data, 64) != 0)
        return fail(engine, "observations evidence hash is stale");
    }
    runner |= string_is(role, "runner");
    subject |= string_is(role, "subject");
    inventory |= string_is(role, "test_inventory");
    if (index && pair_order(&evidence->as.array.items[index - 1], row, "role",
                            "path") >= 0)
      return fail(engine, "observations evidence must be sorted and unique");
  }
  if (!runner || !subject || !inventory)
    return fail(engine,
                "observations require runner, subject, and test_inventory "
                "evidence");
  ab_buffer_init(&canonical, engine);
  status = ab_value_render(&canonical, evidence);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  ab_buffer_free(&canonical);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_hex(digest, hex);
  if (memcmp(hex, slice->as.text.data, 64) != 0)
    return fail(engine, "observations evidence-slice digest is invalid");
  return ARCHBIRD_OK;
}

static ArchbirdStatus validate_document(ArchbirdEngine *engine,
                                        const ArchbirdProject *project,
                                        const AbValue *document) {
  static const char *const root_fields[] = {
      "artifact",   "cases",          "producer", "project",
      "provenance", "schema_version", "source"};
  static const char *const producer_fields[] = {"configuration_sha256",
                                                "implementation_sha256",
                                                "input_sha256",
                                                "name",
                                                "runtime",
                                                "version"};
  static const char *const case_fields[] = {"group", "path", "selector",
                                            "symbols"};
  static const char *const symbol_fields[] = {"hits", "path", "symbol"};
  const AbValue *schema;
  const AbValue *project_name;
  const AbValue *producer;
  const AbValue *cases;
  uint64_t schema_number;
  size_t case_index;
  if (!exact_object(document, root_fields,
                    sizeof(root_fields) / sizeof(root_fields[0])) ||
      !string_is(ab_value_member(document, "artifact"),
                 "archbird-test-symbol-observations") ||
      !string_is(ab_value_member(document, "provenance"), "observed"))
    return fail(engine, "invalid test-symbol observations");
  schema = ab_value_member(document, "schema_version");
  project_name = ab_value_member(document, "project");
  producer = ab_value_member(document, "producer");
  cases = ab_value_member(document, "cases");
  if (!ab_value_u64(schema, &schema_number) || schema_number != 1 ||
      !nonempty_string(project_name) ||
      (project && !ab_string_equal(&project_name->as.text,
                                   &ab_project_manifest(project)->project)) ||
      !exact_object(producer, producer_fields,
                    sizeof(producer_fields) / sizeof(producer_fields[0])) ||
      !nonempty_string(ab_value_member(producer, "name")) ||
      !nonempty_string(ab_value_member(producer, "version")) ||
      !digest_string(ab_value_member(producer, "implementation_sha256")) ||
      !digest_string(ab_value_member(producer, "configuration_sha256")) ||
      (ab_value_member(producer, "input_sha256") &&
       !digest_string(ab_value_member(producer, "input_sha256"))) ||
      (ab_value_member(producer, "runtime") &&
       !nonempty_string(ab_value_member(producer, "runtime"))) ||
      !cases || cases->kind != AB_VALUE_ARRAY || !cases->as.array.count)
    return fail(engine, "test-symbol observation identity is invalid");
  if (validate_source(engine, project, ab_value_member(document, "source")) !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  for (case_index = 0; case_index < cases->as.array.count; case_index++) {
    const AbValue *row = &cases->as.array.items[case_index];
    const AbValue *group;
    const AbValue *path;
    const AbValue *selector;
    const AbValue *symbols;
    size_t symbol_index;
    if (!exact_object(row, case_fields,
                      sizeof(case_fields) / sizeof(case_fields[0])))
      return fail(engine, "observed test case shape is invalid");
    group = ab_value_member(row, "group");
    path = ab_value_member(row, "path");
    selector = ab_value_member(row, "selector");
    symbols = ab_value_member(row, "symbols");
    if (!nonempty_string(group) || !nonempty_string(path) ||
        !nonempty_string(selector) ||
        (project && !manifest_file(project, path)) || !symbols ||
        symbols->kind != AB_VALUE_ARRAY || !symbols->as.array.count)
      return fail(engine, "observed test case is invalid");
    if (case_index && triple_order(&cases->as.array.items[case_index - 1], row,
                                   "group", "path", "selector") >= 0)
      return fail(engine, "observed test cases must be sorted and unique");
    for (symbol_index = 0; symbol_index < symbols->as.array.count;
         symbol_index++) {
      const AbValue *symbol = &symbols->as.array.items[symbol_index];
      const AbValue *target_path;
      const AbValue *target_symbol;
      const AbValue *hits;
      uint64_t hit_count;
      if (!exact_object(symbol, symbol_fields,
                        sizeof(symbol_fields) / sizeof(symbol_fields[0])))
        return fail(engine, "observed symbol-hit shape is invalid");
      target_path = ab_value_member(symbol, "path");
      target_symbol = ab_value_member(symbol, "symbol");
      hits = ab_value_member(symbol, "hits");
      if (!nonempty_string(target_path) || !nonempty_string(target_symbol) ||
          (project && !manifest_file(project, target_path)) ||
          !ab_value_u64(hits, &hit_count) || !hit_count || hit_count > SIZE_MAX)
        return fail(engine, "observed symbol hit is invalid");
      if (symbol_index && pair_order(&symbols->as.array.items[symbol_index - 1],
                                     symbol, "path", "symbol") >= 0)
        return fail(engine, "observed symbol hits must be sorted and unique");
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_decode_test_symbol_observations(
    ArchbirdEngine *engine, const ArchbirdProject *project,
    const uint8_t *input, size_t input_length, AbValue *out) {
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, input, input_length, out);
  if (status == ARCHBIRD_OK)
    status = validate_document(engine, project, out);
  if (status != ARCHBIRD_OK)
    ab_value_free(engine, out);
  return status;
}

ArchbirdStatus archbird_test_symbol_observations_validate(
    ArchbirdEngine *engine, const uint8_t *input, size_t input_length) {
  AbValue document = {0};
  ArchbirdStatus status;
  if (!engine || (!input && input_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = ab_decode_test_symbol_observations(engine, NULL, input, input_length,
                                              &document);
  ab_value_free(engine, &document);
  return status;
}
