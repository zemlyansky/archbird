#include "verify_runtime.h"

#include "json_number.h"
#include "sha256.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AttestationParser {
  AbVerificationContext *context;
  AbBuffer message;
} AttestationParser;

static AbString exact_policy_name = {(char *)"exact", 5};

#define AT_RENDER_TRY(expression)                                              \
  do {                                                                         \
    ArchbirdStatus status__ = (expression);                                    \
    if (status__ != ARCHBIRD_OK)                                               \
      return status__;                                                         \
  } while (0)

static int string_literal(const AbString *value, const char *literal) {
  size_t length = strlen(literal);
  return value && value->length == length &&
         (!length || memcmp(value->data, literal, length) == 0);
}

static ArchbirdStatus parser_invalid(AttestationParser *parser,
                                     const char *message) {
  if (!parser->message.length)
    (void)ab_buffer_literal(&parser->message, message);
  return ARCHBIRD_INVALID_SCHEMA;
}

static int name_allowed(const AbString *name, const char *const *allowed,
                        size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    if (string_literal(name, allowed[index]))
      return 1;
  return 0;
}

static ArchbirdStatus object_fields(AttestationParser *parser,
                                    const AbValue *value,
                                    const char *const *allowed, size_t count,
                                    const char *message) {
  size_t index;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return parser_invalid(parser, message);
  for (index = 0; index < value->as.object.count; index++)
    if (!name_allowed(&value->as.object.fields[index].name, allowed, count))
      return parser_invalid(parser, message);
  return ARCHBIRD_OK;
}

static int lowercase_sha256(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_STRING || value->as.text.length != 64)
    return 0;
  for (index = 0; index < 64; index++) {
    char byte = value->as.text.data[index];
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return 0;
  }
  return 1;
}

static int value_nonblank_string(const AbValue *value) {
  return ab_verify_nonblank(value);
}

static int string_array_valid(const AbValue *value) {
  size_t index;
  size_t previous;
  if (!value || value->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *item = &value->as.array.items[index];
    if (!value_nonblank_string(item))
      return 0;
    for (previous = 0; previous < index; previous++)
      if (ab_string_equal(&item->as.text,
                          &value->as.array.items[previous].as.text))
        return 0;
  }
  return 1;
}

static int scalar_map_valid(const AbValue *value) {
  size_t index;
  if (!value || value->kind != AB_VALUE_OBJECT)
    return 0;
  for (index = 0; index < value->as.object.count; index++) {
    const AbObjectField *field = &value->as.object.fields[index];
    size_t byte;
    int any = 0;
    for (byte = 0; byte < field->name.length; byte++)
      if (field->name.data[byte] != ' ' && field->name.data[byte] != '\t' &&
          field->name.data[byte] != '\r' && field->name.data[byte] != '\n') {
        any = 1;
        break;
      }
    if (!any || field->value.kind > AB_VALUE_STRING)
      return 0;
  }
  return 1;
}

static ArchbirdStatus normalized_path(ArchbirdEngine *engine,
                                      const AbValue *value, AbString *out) {
  AbBuffer buffer;
  size_t index;
  size_t segment = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!ab_verify_path_is_repository(value))
    return ARCHBIRD_INVALID_SCHEMA;
  ab_buffer_init(&buffer, engine);
  for (index = 0; status == ARCHBIRD_OK && index <= value->as.text.length;
       index++) {
    int separator = index == value->as.text.length ||
                    value->as.text.data[index] == '/' ||
                    value->as.text.data[index] == '\\';
    if (!separator)
      continue;
    if (index > segment &&
        !(index - segment == 1 && value->as.text.data[segment] == '.')) {
      if (buffer.length)
        status = ab_buffer_literal(&buffer, "/");
      if (status == ARCHBIRD_OK)
        status = ab_buffer_append(&buffer, value->as.text.data + segment,
                                  index - segment);
    }
    segment = index + 1;
  }
  if (status == ARCHBIRD_OK && !buffer.length)
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus portable_suite_path(ArchbirdEngine *engine,
                                          const AbValue *value, AbString *out) {
  size_t index;
  size_t segment = 0;
  size_t basename = 0;
  int parent = 0;
  if (!value || value->kind != AB_VALUE_STRING || !value->as.text.length)
    return ARCHBIRD_INVALID_SCHEMA;
  for (index = 0; index <= value->as.text.length; index++) {
    int separator = index == value->as.text.length ||
                    value->as.text.data[index] == '/' ||
                    value->as.text.data[index] == '\\';
    if (!separator)
      continue;
    if (index - segment == 2 && value->as.text.data[segment] == '.' &&
        value->as.text.data[segment + 1] == '.')
      parent = 1;
    if (index > segment)
      basename = segment;
    segment = index + 1;
  }
  if (parent) {
    size_t length = value->as.text.length - basename;
    while (length && (value->as.text.data[basename + length - 1] == '/' ||
                      value->as.text.data[basename + length - 1] == '\\'))
      length--;
    return length ? ab_string_copy(engine, out, value->as.text.data + basename,
                                   length)
                  : ARCHBIRD_INVALID_SCHEMA;
  }
  return normalized_path(engine, value, out);
}

static int evidence_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyAttestationEvidenceView *left =
      (const AbVerifyAttestationEvidenceView *)left_raw;
  const AbVerifyAttestationEvidenceView *right =
      (const AbVerifyAttestationEvidenceView *)right_raw;
  int compared = ab_string_compare(&left->role, &right->role);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  if (!compared)
    compared = ab_string_compare(&left->sha256, &right->sha256);
  return compared;
}

static int case_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyAttestationCaseView *left =
      (const AbVerifyAttestationCaseView *)left_raw;
  const AbVerifyAttestationCaseView *right =
      (const AbVerifyAttestationCaseView *)right_raw;
  return ab_string_compare(left->id, right->id);
}

static int observation_compare(const void *left_raw, const void *right_raw) {
  const AbVerifyObservationView *left =
      (const AbVerifyObservationView *)left_raw;
  const AbVerifyObservationView *right =
      (const AbVerifyObservationView *)right_raw;
  return ab_string_compare(left->route, right->route);
}

static void attestation_data_free(ArchbirdEngine *engine,
                                  AbVerifyAttestationDataView *data) {
  size_t index;
  if (!data)
    return;
  for (index = 0; data->evidence && index < data->evidence_count; index++) {
    ab_string_free(engine, &data->evidence[index].role);
    ab_string_free(engine, &data->evidence[index].path);
    ab_string_free(engine, &data->evidence[index].sha256);
  }
  ab_free(engine, data->evidence);
  for (index = 0; data->cases && index < data->case_count; index++)
    ab_free(engine, data->cases[index].observations);
  ab_free(engine, data->cases);
  memset(data, 0, sizeof(*data));
}

static int decimal_u64_saturating(const AbValue *value, uint64_t *out) {
  size_t index;
  uint64_t result = 0;
  if (!value || value->kind != AB_VALUE_INTEGER || !value->as.text.length ||
      value->as.text.data[0] == '-')
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned digit = (unsigned)(value->as.text.data[index] - '0');
    if (digit > 9)
      return 0;
    if (result > (UINT64_MAX - digit) / 10) {
      *out = UINT64_MAX;
      return 1;
    }
    result = result * 10 + digit;
  }
  *out = result;
  return 1;
}

static int nonnegative_number(const AbValue *value, double *out) {
  size_t index;
  double result = 0.0;
  if (!value)
    return 0;
  if (value->kind == AB_VALUE_REAL) {
    if (value->as.real < 0.0)
      return 0;
    *out = value->as.real;
    return 1;
  }
  if (value->kind != AB_VALUE_INTEGER || !value->as.text.length ||
      value->as.text.data[0] == '-')
    return 0;
  for (index = 0; index < value->as.text.length; index++) {
    unsigned digit = (unsigned)(value->as.text.data[index] - '0');
    if (digit > 9 || result > (DBL_MAX - (double)digit) / 10.0)
      return 0;
    result = result * 10.0 + (double)digit;
  }
  *out = result;
  return 1;
}

static ArchbirdStatus parse_policy(AttestationParser *parser,
                                   const AbValue *source,
                                   AbVerifyEqualityPolicy *out) {
  static const char *const allowed[] = {
      "atol", "kind", "max_ulp", "nan_equal", "rtol", "signed_zero_equal",
  };
  const AbValue *kind;
  const AbValue *value;
  memset(out, 0, sizeof(*out));
  out->signed_zero_equal = 1;
  out->source = source;
  if (!source) {
    out->kind = &exact_policy_name;
    return ARCHBIRD_OK;
  }
  if (object_fields(parser, source, allowed, 6,
                    "attestation case comparison: invalid object") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  kind = ab_value_member(source, "kind");
  if (!kind)
    out->kind = &exact_policy_name;
  else if (!value_nonblank_string(kind))
    return parser_invalid(parser, "attestation case comparison.kind: expected "
                                  "non-empty string");
  else
    out->kind = &kind->as.text;
  if (!string_literal(out->kind, "exact") &&
      !string_literal(out->kind, "exact_bits") &&
      !string_literal(out->kind, "float"))
    return parser_invalid(parser,
                          "attestation case comparison.kind: unsupported "
                          "comparison");
  if (!string_literal(out->kind, "float") && source->as.object.count > 1)
    return parser_invalid(parser,
                          "attestation case comparison: tolerance fields "
                          "require kind 'float'");
  value = ab_value_member(source, "atol");
  if (value && !nonnegative_number(value, &out->atol))
    return parser_invalid(parser,
                          "attestation case comparison.atol: expected finite "
                          "number >= 0");
  value = ab_value_member(source, "rtol");
  if (value && !nonnegative_number(value, &out->rtol))
    return parser_invalid(parser,
                          "attestation case comparison.rtol: expected finite "
                          "number >= 0");
  value = ab_value_member(source, "max_ulp");
  if (value) {
    if (!decimal_u64_saturating(value, &out->max_ulp))
      return parser_invalid(parser,
                            "attestation case comparison.max_ulp: expected "
                            "integer >= 0");
    out->has_max_ulp = 1;
  }
  value = ab_value_member(source, "nan_equal");
  if (value) {
    if (value->kind != AB_VALUE_BOOL)
      return parser_invalid(parser,
                            "attestation case comparison.nan_equal: expected "
                            "boolean");
    out->nan_equal = value->as.boolean;
  }
  value = ab_value_member(source, "signed_zero_equal");
  if (value) {
    if (value->kind != AB_VALUE_BOOL)
      return parser_invalid(
          parser,
          "attestation case comparison.signed_zero_equal: expected boolean");
    out->signed_zero_equal = value->as.boolean;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus parse_observation(AttestationParser *parser,
                                        const AbValue *source,
                                        AbVerifyObservationView *out) {
  static const char *const allowed[] = {"outcome", "route"};
  static const char *const outcome_allowed[] = {"kind", "phase", "type",
                                                "value"};
  static const char *const outcome_kinds[] = {
      "error",   "exhaustion", "ok",         "rejected",
      "timeout", "trap",       "unlinkable", "unsupported",
  };
  const AbValue *outcome;
  const AbValue *value;
  size_t index;
  int known = 0;
  memset(out, 0, sizeof(*out));
  if (object_fields(parser, source, allowed, 2,
                    "attestation case observation: invalid object") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  value = ab_value_member(source, "route");
  outcome = ab_value_member(source, "outcome");
  if (!value_nonblank_string(value) ||
      object_fields(parser, outcome, outcome_allowed, 4,
                    "attestation case observation.outcome: invalid object") !=
          ARCHBIRD_OK)
    return parser_invalid(parser,
                          "attestation case observation: route and outcome "
                          "are required");
  out->route = &value->as.text;
  value = ab_value_member(outcome, "phase");
  if (!value_nonblank_string(value))
    return parser_invalid(parser, "attestation case observation.outcome.phase: "
                                  "expected non-empty string");
  out->phase = &value->as.text;
  value = ab_value_member(outcome, "kind");
  if (!value_nonblank_string(value))
    return parser_invalid(parser, "attestation case observation.outcome.kind: "
                                  "expected non-empty string");
  out->kind = &value->as.text;
  for (index = 0; index < sizeof(outcome_kinds) / sizeof(outcome_kinds[0]);
       index++)
    if (string_literal(out->kind, outcome_kinds[index])) {
      known = 1;
      break;
    }
  if (!known)
    return parser_invalid(parser, "attestation case observation.outcome.kind: "
                                  "unsupported outcome");
  value = ab_value_member(outcome, "type");
  if (value && value->kind != AB_VALUE_STRING)
    return parser_invalid(parser, "attestation case observation.outcome.type: "
                                  "expected string");
  out->type_name = value ? &value->as.text : NULL;
  out->value = ab_value_member(outcome, "value");
  if (string_literal(out->kind, "ok") && !out->value)
    return parser_invalid(parser, "attestation case observation.outcome.value: "
                                  "required for ok outcome");
  if (!string_literal(out->kind, "ok") && out->value)
    return parser_invalid(parser,
                          "attestation case observation.outcome.value: only "
                          "ok outcomes may carry a value");
  return ARCHBIRD_OK;
}

static ArchbirdStatus attestation_digest(ArchbirdEngine *engine,
                                         const AbValue *root, char out[65]) {
  AbBuffer canonical;
  uint8_t digest[32];
  ArchbirdStatus status;
  ab_buffer_init(&canonical, engine);
  status = ab_value_render(&canonical, root);
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus
evidence_slice_digest(ArchbirdEngine *engine,
                      const AbVerifyAttestationEvidenceView *rows, size_t count,
                      char out[65]) {
  AbBuffer canonical;
  uint8_t digest[32];
  size_t index;
  ArchbirdStatus status;
  ab_buffer_init(&canonical, engine);
  status = ab_buffer_literal(&canonical, "[");
  for (index = 0; status == ARCHBIRD_OK && index < count; index++) {
    if (index)
      status = ab_buffer_literal(&canonical, ",");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&canonical, "{\"path\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&canonical, rows[index].path.data,
                                     rows[index].path.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&canonical, ",\"role\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&canonical, rows[index].role.data,
                                     rows[index].role.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&canonical, ",\"sha256\":");
    if (status == ARCHBIRD_OK)
      status = ab_buffer_json_string(&canonical, rows[index].sha256.data,
                                     rows[index].sha256.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&canonical, "}");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&canonical, "]");
  if (status == ARCHBIRD_OK)
    status = archbird_sha256(canonical.data, canonical.length, digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out);
  ab_buffer_free(&canonical);
  return status;
}

static ArchbirdStatus parse_attestation(AttestationParser *parser,
                                        const AbValue *root,
                                        AbVerifyAttestationDataView *out) {
  static const char *const root_allowed[] = {"cases", "producer", "profile",
                                             "schema_version", "suite"};
  static const char *const producer_allowed[] = {
      "evidence", "evidence_slice_sha256", "map_input_sha256", "project",
      "revision",
  };
  static const char *const evidence_allowed[] = {"path", "role", "sha256"};
  static const char *const profile_allowed[] = {"capabilities", "id",
                                                "parameters"};
  static const char *const case_allowed[] = {
      "comparison",          "id",           "input",
      "observations",        "requirements", "requires",
      "requires_parameters",
  };
  const AbValue *producer;
  const AbValue *profile;
  const AbValue *evidence;
  const AbValue *cases;
  const AbValue *value;
  uint64_t schema = 0;
  size_t index;
  int has_runner = 0;
  int has_bundle = 0;
  int has_subject = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  char slice[65];
  memset(out, 0, sizeof(*out));
  out->root = root;
  if (object_fields(parser, root, root_allowed, 5,
                    "attestation: expected object with known fields") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  if (!ab_value_u64(ab_value_member(root, "schema_version"), &schema) ||
      schema != 1)
    return parser_invalid(parser, "attestation.schema_version: expected 1");
  value = ab_value_member(root, "suite");
  if (!value_nonblank_string(value))
    return parser_invalid(parser,
                          "attestation.suite: expected non-empty string");
  out->suite = &value->as.text;
  producer = ab_value_member(root, "producer");
  if (object_fields(
          parser, producer, producer_allowed, 5,
          "attestation.producer: expected object with known fields") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  value = ab_value_member(producer, "project");
  if (!value_nonblank_string(value))
    return parser_invalid(
        parser, "attestation.producer.project: expected non-empty string");
  out->project = &value->as.text;
  value = ab_value_member(producer, "revision");
  if (value && value->kind != AB_VALUE_STRING)
    return parser_invalid(parser,
                          "attestation.producer.revision: expected string");
  out->revision = value ? &value->as.text : NULL;
  value = ab_value_member(producer, "map_input_sha256");
  if (!lowercase_sha256(value))
    return parser_invalid(parser,
                          "attestation.producer.map_input_sha256: expected "
                          "lowercase SHA-256");
  out->map_input_sha256 = &value->as.text;
  value = ab_value_member(producer, "evidence_slice_sha256");
  if (!lowercase_sha256(value))
    return parser_invalid(
        parser,
        "attestation.producer.evidence_slice_sha256: expected lowercase "
        "SHA-256");
  out->evidence_slice_sha256 = &value->as.text;
  evidence = ab_value_member(producer, "evidence");
  if (!evidence || evidence->kind != AB_VALUE_ARRAY ||
      !evidence->as.array.count)
    return parser_invalid(
        parser, "attestation.producer.evidence: expected at least one row");
  if (evidence->as.array.count > SIZE_MAX / sizeof(*out->evidence))
    return archbird_error_set(parser->context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many attestation evidence rows");
  out->evidence = (AbVerifyAttestationEvidenceView *)ab_calloc(
      parser->context->engine, evidence->as.array.count,
      sizeof(*out->evidence));
  if (!out->evidence)
    return archbird_error_set(parser->context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory loading attestation evidence");
  out->evidence_count = evidence->as.array.count;
  for (index = 0; status == ARCHBIRD_OK && index < out->evidence_count;
       index++) {
    const AbValue *row = &evidence->as.array.items[index];
    const AbValue *role;
    const AbValue *path;
    const AbValue *sha256;
    if (object_fields(parser, row, evidence_allowed, 3,
                      "attestation.producer.evidence row: invalid object") !=
        ARCHBIRD_OK) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    role = ab_value_member(row, "role");
    path = ab_value_member(row, "path");
    sha256 = ab_value_member(row, "sha256");
    if (!value_nonblank_string(role) || !lowercase_sha256(sha256)) {
      status = parser_invalid(
          parser, "attestation.producer.evidence: invalid role or SHA-256");
      break;
    }
    status = normalized_path(parser->context->engine, path,
                             &out->evidence[index].path);
    if (status == ARCHBIRD_INVALID_SCHEMA)
      status =
          parser_invalid(parser, "attestation.producer.evidence.path: expected "
                                 "repository-relative file path");
    if (status == ARCHBIRD_OK)
      status =
          ab_string_copy(parser->context->engine, &out->evidence[index].role,
                         role->as.text.data, role->as.text.length);
    if (status == ARCHBIRD_OK)
      status =
          ab_string_copy(parser->context->engine, &out->evidence[index].sha256,
                         sha256->as.text.data, sha256->as.text.length);
  }
  if (status != ARCHBIRD_OK)
    return status;
  qsort(out->evidence, out->evidence_count, sizeof(*out->evidence),
        evidence_compare);
  for (index = 0; index < out->evidence_count; index++) {
    if (index &&
        !evidence_compare(&out->evidence[index - 1], &out->evidence[index]))
      return parser_invalid(parser,
                            "attestation.producer.evidence: duplicate rows");
    has_runner |= string_literal(&out->evidence[index].role, "runner");
    has_bundle |= string_literal(&out->evidence[index].role, "case_bundle");
    has_subject |= string_literal(&out->evidence[index].role, "subject");
  }
  if (!has_runner || !has_bundle || !has_subject)
    return parser_invalid(
        parser, "attestation.producer.evidence: missing required role");
  status = evidence_slice_digest(parser->context->engine, out->evidence,
                                 out->evidence_count, slice);
  if (status != ARCHBIRD_OK)
    return status;
  if (memcmp(slice, out->evidence_slice_sha256->data, 64) != 0)
    return parser_invalid(
        parser, "attestation.producer.evidence_slice_sha256: does not match "
                "evidence rows");
  profile = ab_value_member(root, "profile");
  if (object_fields(parser, profile, profile_allowed, 3,
                    "attestation.profile: expected object with known fields") !=
      ARCHBIRD_OK)
    return ARCHBIRD_INVALID_SCHEMA;
  value = ab_value_member(profile, "id");
  if (!value_nonblank_string(value))
    return parser_invalid(parser,
                          "attestation.profile.id: expected non-empty string");
  out->profile = &value->as.text;
  value = ab_value_member(profile, "capabilities");
  if (!string_array_valid(value))
    return parser_invalid(
        parser, "attestation.profile.capabilities: expected unique strings");
  out->capabilities = value;
  value = ab_value_member(profile, "parameters");
  if (!value)
    value = NULL;
  else if (!scalar_map_valid(value))
    return parser_invalid(
        parser, "attestation.profile.parameters: expected scalar map");
  out->parameters = value;
  cases = ab_value_member(root, "cases");
  if (!cases || cases->kind != AB_VALUE_ARRAY || !cases->as.array.count)
    return parser_invalid(parser,
                          "attestation.cases: expected at least one case");
  if (cases->as.array.count > SIZE_MAX / sizeof(*out->cases))
    return archbird_error_set(parser->context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET, "too many attestation cases");
  out->cases = (AbVerifyAttestationCaseView *)ab_calloc(
      parser->context->engine, cases->as.array.count, sizeof(*out->cases));
  if (!out->cases)
    return archbird_error_set(parser->context->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory loading attestation cases");
  out->case_count = cases->as.array.count;
  for (index = 0; status == ARCHBIRD_OK && index < out->case_count; index++) {
    const AbValue *row = &cases->as.array.items[index];
    AbVerifyAttestationCaseView *case_view = &out->cases[index];
    const AbValue *observations;
    size_t observation_index;
    if (object_fields(parser, row, case_allowed, 7,
                      "attestation case: invalid object") != ARCHBIRD_OK) {
      status = ARCHBIRD_INVALID_SCHEMA;
      break;
    }
    value = ab_value_member(row, "id");
    if (!value_nonblank_string(value)) {
      status = parser_invalid(parser,
                              "attestation case.id: expected non-empty string");
      break;
    }
    case_view->id = &value->as.text;
    case_view->requirements = ab_value_member(row, "requirements");
    if (!case_view->requirements) {
      static const AbValue empty_array = {.kind = AB_VALUE_ARRAY};
      case_view->requirements = &empty_array;
    } else if (!string_array_valid(case_view->requirements)) {
      status = parser_invalid(
          parser, "attestation case.requirements: expected unique strings");
      break;
    }
    case_view->required_capabilities = ab_value_member(row, "requires");
    if (!case_view->required_capabilities) {
      static const AbValue empty_array = {.kind = AB_VALUE_ARRAY};
      case_view->required_capabilities = &empty_array;
    } else if (!string_array_valid(case_view->required_capabilities)) {
      status = parser_invalid(
          parser, "attestation case.requires: expected unique strings");
      break;
    }
    case_view->requires_parameters =
        ab_value_member(row, "requires_parameters");
    if (!case_view->requires_parameters) {
      static const AbValue empty_object = {.kind = AB_VALUE_OBJECT};
      case_view->requires_parameters = &empty_object;
    } else if (!scalar_map_valid(case_view->requires_parameters)) {
      status = parser_invalid(
          parser, "attestation case.requires_parameters: expected scalar map");
      break;
    }
    case_view->input = ab_value_member(row, "input");
    if (!case_view->input) {
      status =
          parser_invalid(parser, "attestation case.input: field is required");
      break;
    }
    status = parse_policy(parser, ab_value_member(row, "comparison"),
                          &case_view->comparison);
    if (status != ARCHBIRD_OK)
      break;
    observations = ab_value_member(row, "observations");
    if (!observations || observations->kind != AB_VALUE_ARRAY ||
        !observations->as.array.count) {
      status = parser_invalid(
          parser, "attestation case.observations: expected at least one row");
      break;
    }
    if (observations->as.array.count >
        SIZE_MAX / sizeof(*case_view->observations)) {
      status = archbird_error_set(parser->context->engine,
                                  ARCHBIRD_LIMIT_EXCEEDED, ARCHBIRD_NO_OFFSET,
                                  "too many attestation observations");
      break;
    }
    case_view->observations = (AbVerifyObservationView *)ab_calloc(
        parser->context->engine, observations->as.array.count,
        sizeof(*case_view->observations));
    if (!case_view->observations) {
      status = archbird_error_set(parser->context->engine,
                                  ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                                  "out of memory loading observations");
      break;
    }
    case_view->observation_count = observations->as.array.count;
    for (observation_index = 0;
         status == ARCHBIRD_OK &&
         observation_index < case_view->observation_count;
         observation_index++)
      status = parse_observation(
          parser, &observations->as.array.items[observation_index],
          &case_view->observations[observation_index]);
    if (status != ARCHBIRD_OK)
      break;
    qsort(case_view->observations, case_view->observation_count,
          sizeof(*case_view->observations), observation_compare);
    for (observation_index = 1;
         observation_index < case_view->observation_count; observation_index++)
      if (!observation_compare(&case_view->observations[observation_index - 1],
                               &case_view->observations[observation_index])) {
        status = parser_invalid(
            parser, "attestation case.observations: duplicate routes");
        break;
      }
  }
  if (status != ARCHBIRD_OK)
    return status;
  qsort(out->cases, out->case_count, sizeof(*out->cases), case_compare);
  for (index = 1; index < out->case_count; index++)
    if (!case_compare(&out->cases[index - 1], &out->cases[index]))
      return parser_invalid(parser, "attestation.cases: duplicate case ID");
  return attestation_digest(parser->context->engine, root, out->sha256);
}

static int verify_evidence_compare(const void *left_raw,
                                   const void *right_raw) {
  const AbVerifyEvidence *left = (const AbVerifyEvidence *)left_raw;
  const AbVerifyEvidence *right = (const AbVerifyEvidence *)right_raw;
  int compared = ab_string_compare(&left->provenance, &right->provenance);
  if (!compared)
    compared = ab_string_compare(&left->project, &right->project);
  if (!compared)
    compared = ab_string_compare(&left->path, &right->path);
  if (!compared)
    compared = (left->line > right->line) - (left->line < right->line);
  if (!compared)
    compared = ab_string_compare(&left->sha256, &right->sha256);
  if (!compared)
    compared = ab_string_compare(&left->detail, &right->detail);
  return compared;
}

static ArchbirdStatus state_add_witness(AbVerificationContext *context,
                                        AbVerifyAttestationState *state,
                                        const AbString *path,
                                        const char *sha256, const char *detail,
                                        size_t detail_length) {
  AbVerifyEvidence *resized;
  ArchbirdStatus status;
  if (state->witness_count == state->witness_capacity) {
    size_t next = state->witness_capacity ? state->witness_capacity * 2 : 4;
    if (next < state->witness_capacity ||
        next > SIZE_MAX / sizeof(*state->witnesses))
      return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "too much attestation evidence");
    resized = (AbVerifyEvidence *)ab_realloc(context->engine, state->witnesses,
                                             next * sizeof(*state->witnesses));
    if (!resized)
      return archbird_error_set(context->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing attestation evidence");
    state->witnesses = resized;
    state->witness_capacity = next;
  }
  memset(&state->witnesses[state->witness_count], 0, sizeof(*state->witnesses));
  status = ab_verify_evidence_init(
      context->engine, &state->witnesses[state->witness_count], "observed",
      &state->project, path, 0, sha256, detail, detail_length);
  if (status == ARCHBIRD_OK)
    state->witness_count++;
  return status;
}

static const AbObjectField *object_named_field(const AbValue *object,
                                               const AbString *name) {
  size_t low = 0;
  size_t high;
  if (!object || object->kind != AB_VALUE_OBJECT)
    return NULL;
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
      return &object->as.object.fields[middle];
  }
  return NULL;
}

static const AbValue *map_file(const AbValue *map, const AbString *path) {
  const AbValue *files = ab_value_member(map, "files");
  size_t index;
  if (!files || files->kind != AB_VALUE_ARRAY)
    return NULL;
  for (index = 0; index < files->as.array.count; index++) {
    const AbValue *row = &files->as.array.items[index];
    const AbValue *candidate = ab_value_member(row, "path");
    if (candidate && candidate->kind == AB_VALUE_STRING &&
        ab_string_equal(&candidate->as.text, path))
      return row;
  }
  return NULL;
}

static int value_array_set_equal(const AbValue *left, const AbValue *right) {
  size_t index;
  size_t candidate;
  if (!left || !right || left->kind != AB_VALUE_ARRAY ||
      right->kind != AB_VALUE_ARRAY ||
      left->as.array.count != right->as.array.count)
    return 0;
  for (index = 0; index < left->as.array.count; index++) {
    int found = 0;
    for (candidate = 0; candidate < right->as.array.count; candidate++)
      if (ab_value_equal(&left->as.array.items[index],
                         &right->as.array.items[candidate])) {
        found = 1;
        break;
      }
    if (!found)
      return 0;
  }
  return 1;
}

static ArchbirdStatus message_part(AbBuffer *message, const char *prefix,
                                   const AbString *left,
                                   const AbString *right) {
  ArchbirdStatus status;
  if (message->length)
    status = ab_buffer_literal(message, "; ");
  else
    status = ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, prefix);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, " '");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(message, left ? left->data : "",
                              left ? left->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, "' != '");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(message, right ? right->data : "",
                              right ? right->length : 0);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, "'");
  return status;
}

static ArchbirdStatus evidence_message(AbBuffer *message, const AbString *role,
                                       const AbString *path,
                                       const char *detail) {
  ArchbirdStatus status =
      message->length ? ab_buffer_literal(message, "; ") : ARCHBIRD_OK;
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(message, role->data, role->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(message, path->data, path->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, ": ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(message, detail);
  return status;
}

static ArchbirdStatus append_message_literal(AbBuffer *message,
                                             const char *detail) {
  ArchbirdStatus status =
      message->length ? ab_buffer_literal(message, "; ") : ARCHBIRD_OK;
  return status == ARCHBIRD_OK ? ab_buffer_literal(message, detail) : status;
}

static int string_state_priority(const AbString *state) {
  if (string_literal(state, "stale"))
    return 2;
  if (string_literal(state, "unknown"))
    return 1;
  return 0;
}

static ArchbirdStatus set_state(AbVerificationContext *context,
                                AbVerifyAttestationState *state,
                                const char *value) {
  AbString candidate = {(char *)value, strlen(value)};
  if (state->state.data &&
      string_state_priority(&state->state) >= string_state_priority(&candidate))
    return ARCHBIRD_OK;
  ab_string_free(context->engine, &state->state);
  return ab_string_copy(context->engine, &state->state, value, strlen(value));
}

static ArchbirdStatus current_evidence_hash(const AbValue *project_input,
                                            const AbString *path, char out[65],
                                            const char **state,
                                            const AbString **error) {
  const AbValue *source = ab_verify_input_source(project_input, path);
  const AbValue *map = ab_value_member(project_input, "map");
  const AbValue *row;
  const AbValue *value;
  uint8_t digest[32];
  *state = "current";
  *error = NULL;
  if (source) {
    value = ab_value_member(source, "text");
    if (value) {
      ArchbirdStatus status = archbird_sha256(
          (const uint8_t *)value->as.text.data, value->as.text.length, digest);
      if (status != ARCHBIRD_OK)
        return status;
      archbird_sha256_hex(digest, out);
      return ARCHBIRD_OK;
    }
    value = ab_value_member(source, "sha256");
    if (value) {
      memcpy(out, value->as.text.data, 64);
      out[64] = '\0';
      return ARCHBIRD_OK;
    }
    *state = "stale";
    *error = &ab_value_member(source, "error")->as.text;
    return ARCHBIRD_OK;
  }
  row = map_file(map, path);
  value = row ? ab_value_member(row, "sha256") : NULL;
  if (lowercase_sha256(value)) {
    memcpy(out, value->as.text.data, 64);
    out[64] = '\0';
    return ARCHBIRD_OK;
  }
  *state = "unknown";
  return ARCHBIRD_OK;
}

static void attestation_state_free(ArchbirdEngine *engine,
                                   AbVerifyAttestationState *state) {
  size_t index;
  if (!state)
    return;
  ab_string_free(engine, &state->name);
  ab_string_free(engine, &state->project);
  ab_string_free(engine, &state->state);
  ab_string_free(engine, &state->message);
  attestation_data_free(engine, &state->data);
  for (index = 0; state->witnesses && index < state->witness_count; index++)
    ab_verify_evidence_free(engine, &state->witnesses[index]);
  ab_free(engine, state->witnesses);
  memset(state, 0, sizeof(*state));
}

void ab_verify_attestations_free(AbVerificationContext *context) {
  size_t index;
  if (!context)
    return;
  for (index = 0; context->attestations && index < context->attestation_count;
       index++)
    attestation_state_free(context->engine, &context->attestations[index]);
  ab_free(context->engine, context->attestations);
  context->attestations = NULL;
  context->attestation_count = 0;
}

ArchbirdStatus ab_verify_attestations_load(AbVerificationContext *context) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!context || !context->engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  context->attestation_count =
      context->suite.attestations ? context->suite.attestations->as.object.count
                                  : 0;
  if (!context->attestation_count)
    return ARCHBIRD_OK;
  if (context->attestation_count > SIZE_MAX / sizeof(*context->attestations))
    return archbird_error_set(context->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many verification attestations");
  context->attestations = (AbVerifyAttestationState *)ab_calloc(
      context->engine, context->attestation_count,
      sizeof(*context->attestations));
  if (!context->attestations)
    return archbird_error_set(
        context->engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
        "out of memory loading verification attestations");
  for (index = 0; status == ARCHBIRD_OK && index < context->attestation_count;
       index++) {
    const AbObjectField *spec =
        &context->suite.attestations->as.object.fields[index];
    const AbValue *input =
        ab_verify_input_attestation(&context->input, &spec->name);
    const AbValue *input_path = ab_value_member(input, "path");
    const AbValue *input_error = ab_value_member(input, "error");
    const AbValue *document = ab_value_member(input, "document");
    const AbValue *project_name = ab_value_member(&spec->value, "project");
    const AbObjectField *project_spec;
    const AbValue *project_input;
    const AbValue *map;
    const AbValue *map_evidence;
    AbVerifyAttestationState *state = &context->attestations[index];
    AbString portable_path = {0};
    AttestationParser parser;
    AbBuffer message;
    size_t evidence_index;
    memset(&parser, 0, sizeof(parser));
    parser.context = context;
    ab_buffer_init(&parser.message, context->engine);
    ab_buffer_init(&message, context->engine);
    status = ab_string_copy(context->engine, &state->name, spec->name.data,
                            spec->name.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(context->engine, &state->project,
                              project_name->as.text.data,
                              project_name->as.text.length);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(context->engine, &state->state, "unknown", 7);
    if (status == ARCHBIRD_OK)
      status = portable_suite_path(context->engine, input_path, &portable_path);
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&parser.message);
      ab_buffer_free(&message);
      ab_string_free(context->engine, &portable_path);
      break;
    }
    if (input_error) {
      status = ab_string_copy(context->engine, &state->message,
                              input_error->as.text.data,
                              input_error->as.text.length);
      ab_buffer_free(&parser.message);
      ab_buffer_free(&message);
      ab_string_free(context->engine, &portable_path);
      continue;
    }
    status = parse_attestation(&parser, document, &state->data);
    if (status == ARCHBIRD_INVALID_SCHEMA) {
      status = ab_string_copy(context->engine, &state->message,
                              (const char *)parser.message.data,
                              parser.message.length);
      attestation_data_free(context->engine, &state->data);
      ab_buffer_free(&parser.message);
      ab_buffer_free(&message);
      ab_string_free(context->engine, &portable_path);
      continue;
    }
    if (status != ARCHBIRD_OK) {
      ab_buffer_free(&parser.message);
      ab_buffer_free(&message);
      ab_string_free(context->engine, &portable_path);
      break;
    }
    state->has_data = 1;
    ab_string_free(context->engine, &state->state);
    status = ab_string_copy(context->engine, &state->state, "current", 7);
    if (status == ARCHBIRD_OK) {
      AbBuffer detail;
      ab_buffer_init(&detail, context->engine);
      status = ab_buffer_literal(&detail, "attestation ");
      if (status == ARCHBIRD_OK)
        status =
            ab_buffer_append(&detail, state->name.data, state->name.length);
      if (status == ARCHBIRD_OK)
        status = state_add_witness(context, state, &portable_path,
                                   state->data.sha256,
                                   (const char *)detail.data, detail.length);
      ab_buffer_free(&detail);
    }
    project_spec =
        object_named_field(context->suite.projects, &project_name->as.text);
    project_input =
        ab_verify_input_project(&context->input, &project_name->as.text);
    map = ab_value_member(project_input, "map");
    map_evidence = ab_value_member(map, "evidence");
    state->whole_map_matches = ab_string_equal(
        state->data.map_input_sha256,
        &ab_value_member(map_evidence, "input_sha256")->as.text);
    if (!ab_string_equal(state->data.suite, &context->suite.name->as.text))
      status = message_part(&message, "suite", state->data.suite,
                            &context->suite.name->as.text);
    if (status == ARCHBIRD_OK &&
        !ab_string_equal(state->data.project, &project_name->as.text))
      status = message_part(&message, "project", state->data.project,
                            &project_name->as.text);
    if (status == ARCHBIRD_OK && project_spec) {
      const AbValue *expected =
          ab_value_member(&project_spec->value, "revision");
      if (expected && expected->as.text.length &&
          (!state->data.revision ||
           !ab_string_equal(state->data.revision, &expected->as.text)))
        status = message_part(&message, "revision", state->data.revision,
                              &expected->as.text);
      expected = ab_value_member(&project_spec->value, "profile");
      if (status == ARCHBIRD_OK && expected && expected->as.text.length &&
          !ab_string_equal(state->data.profile, &expected->as.text))
        status = message_part(&message, "profile", state->data.profile,
                              &expected->as.text);
      expected = ab_value_member(&project_spec->value, "capabilities");
      if (status == ARCHBIRD_OK && expected && expected->as.array.count &&
          !value_array_set_equal(state->data.capabilities, expected))
        status = append_message_literal(
            &message, "attested capabilities differ from project contract");
      expected = ab_value_member(&project_spec->value, "parameters");
      if (status == ARCHBIRD_OK && expected && expected->as.object.count &&
          (!state->data.parameters ||
           !ab_value_equal(state->data.parameters, expected)))
        status = append_message_literal(
            &message, "attested parameters differ from project contract");
    }
    if (status == ARCHBIRD_OK && message.length)
      status = set_state(context, state, "stale");
    for (evidence_index = 0;
         status == ARCHBIRD_OK && evidence_index < state->data.evidence_count;
         evidence_index++) {
      const AbVerifyAttestationEvidenceView *row =
          &state->data.evidence[evidence_index];
      char current[65] = {0};
      const char *current_state;
      const AbString *read_error;
      status = state_add_witness(context, state, &row->path, row->sha256.data,
                                 row->role.data, row->role.length);
      if (status == ARCHBIRD_OK)
        status = current_evidence_hash(project_input, &row->path, current,
                                       &current_state, &read_error);
      if (status != ARCHBIRD_OK)
        break;
      if (!strcmp(current_state, "unknown")) {
        status = set_state(context, state, "unknown");
        if (status == ARCHBIRD_OK)
          status = evidence_message(
              &message, &row->role, &row->path,
              "path is absent from saved map and no project root is available");
      } else if (!strcmp(current_state, "stale")) {
        status = set_state(context, state, "stale");
        if (status == ARCHBIRD_OK) {
          status = evidence_message(&message, &row->role, &row->path,
                                    "cannot read current evidence");
          if (status == ARCHBIRD_OK && read_error && read_error->length) {
            status = ab_buffer_literal(&message, ": ");
            if (status == ARCHBIRD_OK)
              status = ab_buffer_append(&message, read_error->data,
                                        read_error->length);
          }
        }
      } else if (memcmp(current, row->sha256.data, 64) != 0) {
        status = set_state(context, state, "stale");
        if (status == ARCHBIRD_OK)
          status = evidence_message(&message, &row->role, &row->path,
                                    "content digest changed");
      }
    }
    if (status == ARCHBIRD_OK && state->witness_count > 1)
      qsort(state->witnesses, state->witness_count, sizeof(*state->witnesses),
            verify_evidence_compare);
    if (status == ARCHBIRD_OK)
      status = ab_string_copy(context->engine, &state->message,
                              (const char *)message.data, message.length);
    ab_buffer_free(&parser.message);
    ab_buffer_free(&message);
    ab_string_free(context->engine, &portable_path);
  }
  return status;
}

const AbVerifyAttestationState *
ab_verify_attestation_find(const AbVerificationContext *context,
                           const AbString *name) {
  size_t low = 0;
  size_t high = context ? context->attestation_count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(&context->attestations[middle].name, name);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return &context->attestations[middle];
  }
  return NULL;
}

static int array_contains_string(const AbValue *array, const AbString *value) {
  size_t index;
  if (!array || array->kind != AB_VALUE_ARRAY)
    return 0;
  for (index = 0; index < array->as.array.count; index++)
    if (ab_string_equal(&array->as.array.items[index].as.text, value))
      return 1;
  return 0;
}

int ab_verify_attestation_case_applicable(
    const AbVerifyAttestationCaseView *case_view,
    const AbVerifyAttestationDataView *attestation) {
  size_t index;
  if (!case_view || !attestation)
    return 0;
  for (index = 0; index < case_view->required_capabilities->as.array.count;
       index++)
    if (!array_contains_string(
            attestation->capabilities,
            &case_view->required_capabilities->as.array.items[index].as.text))
      return 0;
  for (index = 0; index < case_view->requires_parameters->as.object.count;
       index++) {
    const AbObjectField *required =
        &case_view->requires_parameters->as.object.fields[index];
    const AbValue *actual =
        attestation->parameters
            ? ab_value_member(attestation->parameters, required->name.data)
            : NULL;
    if (!actual || !ab_value_equal(&required->value, actual))
      return 0;
  }
  return 1;
}

static int one_field_tag(const AbValue *value, const char *name,
                         const AbValue **out) {
  if (!value || value->kind != AB_VALUE_OBJECT || value->as.object.count != 1 ||
      !string_literal(&value->as.object.fields[0].name, name))
    return 0;
  *out = &value->as.object.fields[0].value;
  return 1;
}

static int hex_bits(const AbValue *value, uint64_t *out) {
  const AbValue *tag;
  size_t index;
  uint64_t bits = 0;
  if (!one_field_tag(value, "float_bits", &tag) ||
      tag->kind != AB_VALUE_STRING || tag->as.text.length != 16)
    return 0;
  for (index = 0; index < 16; index++) {
    unsigned digit;
    char byte = tag->as.text.data[index];
    if (byte >= '0' && byte <= '9')
      digit = (unsigned)(byte - '0');
    else if (byte >= 'a' && byte <= 'f')
      digit = (unsigned)(byte - 'a' + 10);
    else if (byte >= 'A' && byte <= 'F')
      digit = (unsigned)(byte - 'A' + 10);
    else
      return 0;
    bits = (bits << 4) | digit;
  }
  *out = bits;
  return 1;
}

static int exact_bits_value_equal(const AbValue *expected,
                                  const AbValue *actual) {
  uint64_t left_bits;
  uint64_t right_bits;
  int left_float = expected->kind == AB_VALUE_REAL;
  int right_float = actual->kind == AB_VALUE_REAL;
  size_t index;
  if (left_float)
    memcpy(&left_bits, &expected->as.real, sizeof(left_bits));
  else
    left_float = hex_bits(expected, &left_bits);
  if (right_float)
    memcpy(&right_bits, &actual->as.real, sizeof(right_bits));
  else
    right_float = hex_bits(actual, &right_bits);
  if (left_float || right_float)
    return left_float && right_float && left_bits == right_bits;
  if (expected->kind == AB_VALUE_ARRAY && actual->kind == AB_VALUE_ARRAY) {
    if (expected->as.array.count != actual->as.array.count)
      return 0;
    for (index = 0; index < expected->as.array.count; index++)
      if (!exact_bits_value_equal(&expected->as.array.items[index],
                                  &actual->as.array.items[index]))
        return 0;
    return 1;
  }
  if (expected->kind == AB_VALUE_OBJECT && actual->kind == AB_VALUE_OBJECT) {
    if (expected->as.object.count != actual->as.object.count)
      return 0;
    for (index = 0; index < expected->as.object.count; index++)
      if (!ab_string_equal(&expected->as.object.fields[index].name,
                           &actual->as.object.fields[index].name) ||
          !exact_bits_value_equal(&expected->as.object.fields[index].value,
                                  &actual->as.object.fields[index].value))
        return 0;
    return 1;
  }
  return ab_value_equal(expected, actual);
}

static int bits_is_nan(uint64_t bits) {
  return (bits & UINT64_C(0x7ff0000000000000)) ==
             UINT64_C(0x7ff0000000000000) &&
         (bits & UINT64_C(0x000fffffffffffff)) != 0;
}

static int bits_is_finite(uint64_t bits) {
  return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

static int float_encoding(const AbValue *value, double *out,
                          uint64_t *out_bits) {
  const AbValue *tag;
  uint64_t bits;
  if (value->kind == AB_VALUE_REAL) {
    *out = value->as.real;
    memcpy(out_bits, out, sizeof(*out_bits));
    return 1;
  }
  if (hex_bits(value, &bits)) {
    memcpy(out, &bits, sizeof(bits));
    *out_bits = bits;
    return 1;
  }
  if (!one_field_tag(value, "float", &tag) || tag->kind != AB_VALUE_STRING)
    return 0;
  if (string_literal(&tag->as.text, "nan"))
    bits = UINT64_C(0x7ff8000000000000);
  else if (string_literal(&tag->as.text, "+inf"))
    bits = UINT64_C(0x7ff0000000000000);
  else if (string_literal(&tag->as.text, "-inf"))
    bits = UINT64_C(0xfff0000000000000);
  else if (string_literal(&tag->as.text, "-0"))
    bits = UINT64_C(0x8000000000000000);
  else
    return 0;
  memcpy(out, &bits, sizeof(bits));
  *out_bits = bits;
  return 1;
}

static uint64_t ordered_float_bits(uint64_t bits) {
  return bits & UINT64_C(0x8000000000000000)
             ? ~bits
             : bits | UINT64_C(0x8000000000000000);
}

static int float_value_equal(const AbValue *expected, const AbValue *actual,
                             const AbVerifyEqualityPolicy *policy) {
  double left;
  double right;
  uint64_t left_bits;
  uint64_t right_bits;
  int left_float = float_encoding(expected, &left, &left_bits);
  int right_float = float_encoding(actual, &right, &right_bits);
  size_t index;
  if (left_float || right_float) {
    double difference;
    double scale;
    double tolerance;
    if (!left_float || !right_float)
      return 0;
    if (bits_is_nan(left_bits) || bits_is_nan(right_bits))
      return policy->nan_equal && bits_is_nan(left_bits) &&
             bits_is_nan(right_bits);
    if ((left_bits << 1) == 0 && (right_bits << 1) == 0 &&
        !policy->signed_zero_equal)
      return (left_bits >> 63) == (right_bits >> 63);
    if (left_bits == right_bits || left == right)
      return 1;
    if (!bits_is_finite(left_bits) || !bits_is_finite(right_bits))
      return 0;
    if (policy->has_max_ulp) {
      uint64_t ordered_left = ordered_float_bits(left_bits);
      uint64_t ordered_right = ordered_float_bits(right_bits);
      uint64_t distance = ordered_left > ordered_right
                              ? ordered_left - ordered_right
                              : ordered_right - ordered_left;
      if (distance <= policy->max_ulp)
        return 1;
    }
    difference = left > right ? left - right : right - left;
    left = left < 0.0 ? -left : left;
    right = right < 0.0 ? -right : right;
    scale = left > right ? left : right;
    tolerance = policy->rtol * scale;
    if (policy->atol > tolerance)
      tolerance = policy->atol;
    return difference <= tolerance;
  }
  if (expected->kind == AB_VALUE_ARRAY && actual->kind == AB_VALUE_ARRAY) {
    if (expected->as.array.count != actual->as.array.count)
      return 0;
    for (index = 0; index < expected->as.array.count; index++)
      if (!float_value_equal(&expected->as.array.items[index],
                             &actual->as.array.items[index], policy))
        return 0;
    return 1;
  }
  if (expected->kind == AB_VALUE_OBJECT && actual->kind == AB_VALUE_OBJECT) {
    if (expected->as.object.count != actual->as.object.count)
      return 0;
    for (index = 0; index < expected->as.object.count; index++)
      if (!ab_string_equal(&expected->as.object.fields[index].name,
                           &actual->as.object.fields[index].name) ||
          !float_value_equal(&expected->as.object.fields[index].value,
                             &actual->as.object.fields[index].value, policy))
        return 0;
    return 1;
  }
  return ab_value_equal(expected, actual);
}

int ab_verify_attestation_observations_equal(
    const AbVerifyObservationView *expected,
    const AbVerifyObservationView *actual,
    const AbVerifyEqualityPolicy *policy) {
  AbString empty = {0};
  const AbString *left_type =
      expected->type_name ? expected->type_name : &empty;
  const AbString *right_type = actual->type_name ? actual->type_name : &empty;
  if (!ab_string_equal(expected->phase, actual->phase) ||
      !ab_string_equal(expected->kind, actual->kind) ||
      !ab_string_equal(left_type, right_type))
    return 0;
  if (!string_literal(expected->kind, "ok"))
    return 1;
  if (string_literal(policy->kind, "exact"))
    return ab_value_equal(expected->value, actual->value);
  if (string_literal(policy->kind, "exact_bits"))
    return exact_bits_value_equal(expected->value, actual->value);
  return float_value_equal(expected->value, actual->value, policy);
}

static ArchbirdStatus render_real(AbBuffer *buffer, double value) {
  char number[AB_JSON_REAL_BUFFER_SIZE];
  size_t length = 0;
  ArchbirdStatus status =
      ab_json_real_format(buffer->engine, value, number, &length);
  return status == ARCHBIRD_OK ? ab_buffer_append(buffer, number, length)
                               : status;
}

static ArchbirdStatus render_evidence(AbBuffer *buffer,
                                      const AbVerifyEvidence *row) {
  AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"detail\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, row->detail.data, row->detail.length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"line\":"));
  AT_RENDER_TRY(ab_buffer_u64(buffer, row->line));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"path\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, row->path.data, row->path.length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, row->project.data, row->project.length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"provenance\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, row->provenance.data,
                                      row->provenance.length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, row->sha256.data, row->sha256.length));
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus render_policy(AbBuffer *buffer,
                                    const AbVerifyEqualityPolicy *policy) {
  AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"kind\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, policy->kind->data, policy->kind->length));
  if (string_literal(policy->kind, "float")) {
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"atol\":"));
    AT_RENDER_TRY(render_real(buffer, policy->atol));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"max_ulp\":"));
    if (policy->has_max_ulp) {
      const AbValue *raw =
          policy->source ? ab_value_member(policy->source, "max_ulp") : NULL;
      AT_RENDER_TRY(raw ? ab_value_render(buffer, raw)
                        : ab_buffer_u64(buffer, policy->max_ulp));
    } else {
      AT_RENDER_TRY(ab_buffer_literal(buffer, "null"));
    }
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"nan_equal\":"));
    AT_RENDER_TRY(
        ab_buffer_literal(buffer, policy->nan_equal ? "true" : "false"));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"rtol\":"));
    AT_RENDER_TRY(render_real(buffer, policy->rtol));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"signed_zero_equal\":"));
    AT_RENDER_TRY(ab_buffer_literal(
        buffer, policy->signed_zero_equal ? "true" : "false"));
  }
  return ab_buffer_literal(buffer, "}");
}

static ArchbirdStatus
render_attestation_data_public(AbBuffer *buffer,
                               const AbVerifyAttestationDataView *data) {
  size_t index;
  AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"cases\":["));
  for (index = 0; index < data->case_count; index++) {
    const AbVerifyAttestationCaseView *row = &data->cases[index];
    size_t observation_index;
    if (index)
      AT_RENDER_TRY(ab_buffer_literal(buffer, ","));
    AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"comparison\":"));
    AT_RENDER_TRY(render_policy(buffer, &row->comparison));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"id\":"));
    AT_RENDER_TRY(
        ab_buffer_json_string(buffer, row->id->data, row->id->length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"input\":"));
    AT_RENDER_TRY(ab_value_render(buffer, row->input));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"observations\":["));
    for (observation_index = 0; observation_index < row->observation_count;
         observation_index++) {
      const AbVerifyObservationView *observation =
          &row->observations[observation_index];
      if (observation_index)
        AT_RENDER_TRY(ab_buffer_literal(buffer, ","));
      AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"outcome\":{\"kind\":"));
      AT_RENDER_TRY(ab_buffer_json_string(buffer, observation->kind->data,
                                          observation->kind->length));
      AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"phase\":"));
      AT_RENDER_TRY(ab_buffer_json_string(buffer, observation->phase->data,
                                          observation->phase->length));
      AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"type\":"));
      AT_RENDER_TRY(ab_buffer_json_string(
          buffer, observation->type_name ? observation->type_name->data : "",
          observation->type_name ? observation->type_name->length : 0));
      if (string_literal(observation->kind, "ok")) {
        AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"value\":"));
        AT_RENDER_TRY(ab_value_render(buffer, observation->value));
      }
      AT_RENDER_TRY(ab_buffer_literal(buffer, "},\"route\":"));
      AT_RENDER_TRY(ab_buffer_json_string(buffer, observation->route->data,
                                          observation->route->length));
      AT_RENDER_TRY(ab_buffer_literal(buffer, "}"));
    }
    AT_RENDER_TRY(ab_buffer_literal(buffer, "],\"requirements\":"));
    AT_RENDER_TRY(ab_value_render(buffer, row->requirements));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"requires\":"));
    AT_RENDER_TRY(ab_value_render(buffer, row->required_capabilities));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"requires_parameters\":"));
    AT_RENDER_TRY(ab_value_render(buffer, row->requires_parameters));
    AT_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  AT_RENDER_TRY(ab_buffer_literal(buffer, "],\"evidence\":["));
  for (index = 0; index < data->evidence_count; index++) {
    if (index)
      AT_RENDER_TRY(ab_buffer_literal(buffer, ","));
    AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"path\":"));
    AT_RENDER_TRY(ab_buffer_json_string(buffer, data->evidence[index].path.data,
                                        data->evidence[index].path.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"role\":"));
    AT_RENDER_TRY(ab_buffer_json_string(buffer, data->evidence[index].role.data,
                                        data->evidence[index].role.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
    AT_RENDER_TRY(ab_buffer_json_string(buffer,
                                        data->evidence[index].sha256.data,
                                        data->evidence[index].sha256.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, "}"));
  }
  AT_RENDER_TRY(ab_buffer_literal(buffer, "],\"evidence_slice_sha256\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, data->evidence_slice_sha256->data,
                                      data->evidence_slice_sha256->length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"map_input_sha256\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, data->map_input_sha256->data,
                                      data->map_input_sha256->length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"profile\":{\"capabilities\":"));
  AT_RENDER_TRY(ab_value_render(buffer, data->capabilities));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"id\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, data->profile->data,
                                      data->profile->length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"parameters\":"));
  AT_RENDER_TRY(data->parameters ? ab_value_render(buffer, data->parameters)
                                 : ab_buffer_literal(buffer, "{}"));
  AT_RENDER_TRY(ab_buffer_literal(buffer, "},\"project\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, data->project->data,
                                      data->project->length));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"revision\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, data->revision ? data->revision->data : "",
                            data->revision ? data->revision->length : 0));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"sha256\":"));
  AT_RENDER_TRY(ab_buffer_json_string(buffer, data->sha256, 64));
  AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"suite\":"));
  AT_RENDER_TRY(
      ab_buffer_json_string(buffer, data->suite->data, data->suite->length));
  return ab_buffer_literal(buffer, "}");
}

ArchbirdStatus ab_verify_attestations_render(AbVerificationContext *context,
                                             AbBuffer *buffer) {
  size_t index;
  if (!context || !buffer)
    return ARCHBIRD_INVALID_ARGUMENT;
  AT_RENDER_TRY(ab_buffer_literal(buffer, "["));
  for (index = 0; index < context->attestation_count; index++) {
    const AbVerifyAttestationState *state = &context->attestations[index];
    size_t witness_index;
    if (index)
      AT_RENDER_TRY(ab_buffer_literal(buffer, ","));
    AT_RENDER_TRY(ab_buffer_literal(buffer, "{\"message\":"));
    AT_RENDER_TRY(ab_buffer_json_string(buffer, state->message.data,
                                        state->message.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"name\":"));
    AT_RENDER_TRY(
        ab_buffer_json_string(buffer, state->name.data, state->name.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"project\":"));
    AT_RENDER_TRY(ab_buffer_json_string(buffer, state->project.data,
                                        state->project.length));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"state\":"));
    AT_RENDER_TRY(
        ab_buffer_json_string(buffer, state->state.data, state->state.length));
    if (state->has_data) {
      AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"attestation\":"));
      AT_RENDER_TRY(render_attestation_data_public(buffer, &state->data));
    }
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"whole_map_matches\":"));
    AT_RENDER_TRY(
        ab_buffer_literal(buffer, state->whole_map_matches ? "true" : "false"));
    AT_RENDER_TRY(ab_buffer_literal(buffer, ",\"witnesses\":["));
    for (witness_index = 0; witness_index < state->witness_count;
         witness_index++) {
      if (witness_index)
        AT_RENDER_TRY(ab_buffer_literal(buffer, ","));
      AT_RENDER_TRY(render_evidence(buffer, &state->witnesses[witness_index]));
    }
    AT_RENDER_TRY(ab_buffer_literal(buffer, "]}"));
  }
  return ab_buffer_literal(buffer, "]");
}

#undef AT_RENDER_TRY
