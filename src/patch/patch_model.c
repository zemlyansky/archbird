#include "patch_internal.h"

#include "artifact_validation.h"
#include "base64.h"
#include "model.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define AB_PATCH_MAX_ROWS 4096u
#define AB_PATCH_MAX_METADATA (64u * 1024u)
#define AB_PATCH_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define AB_PATCH_MAX_TOTAL_BYTES (256u * 1024u * 1024u)

typedef struct PatchDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} PatchDigestWriter;

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  PatchDigestWriter *writer = (PatchDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "patch: %s", message);
}

static int array_value(const AbValue *value, size_t minimum) {
  return value && value->kind == AB_VALUE_ARRAY &&
         value->as.array.count >= minimum &&
         value->as.array.count <= AB_PATCH_MAX_ROWS;
}

static int sorted_string_array(const AbValue *value, int ids, size_t minimum) {
  size_t index;
  if (!array_value(value, minimum))
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *row = &value->as.array.items[index];
    if ((ids && !ab_artifact_stable_id(row)) ||
        (!ids && !ab_artifact_repository_path(row)))
      return 0;
    if (index && ab_string_compare(&value->as.array.items[index - 1].as.text,
                                   &row->as.text) >= 0)
      return 0;
  }
  return 1;
}

static int validate_tool(const AbValue *value) {
  static const char *const fields[] = {"name", "version",
                                       "implementation_sha256"};
  return ab_artifact_object_exact(value, fields, 3) &&
         ab_artifact_text_is(ab_value_member(value, "name"), "archbird") &&
         ab_artifact_bounded_text(ab_value_member(value, "version"), 256, 1) &&
         ab_artifact_sha256(ab_value_member(value, "implementation_sha256"));
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
  if (!ab_artifact_object_exact(value, fields, count))
    return 0;
  for (index = 0; index < count; index++)
    if (!ab_artifact_sha256(ab_value_member(value, fields[index])))
      return 0;
  return 1;
}

static int validate_source(const AbValue *value) {
  static const char *const fields[] = {"project", "map", "verification"};
  return ab_artifact_object_exact(value, fields, 3) &&
         ab_artifact_bounded_text(ab_value_member(value, "project"),
                                  AB_PATCH_MAX_METADATA, 1) &&
         validate_identity(ab_value_member(value, "map"), 1) &&
         validate_identity(ab_value_member(value, "verification"), 0);
}

static int validate_after(const AbValue *value) {
  static const char *const fields[] = {"map", "verification"};
  return value->kind == AB_VALUE_NULL ||
         (ab_artifact_object_exact(value, fields, 2) &&
          validate_identity(ab_value_member(value, "map"), 1) &&
          validate_identity(ab_value_member(value, "verification"), 0));
}

static int validate_file_state(const AbValue *value, int content) {
  static const char *const before_fields[] = {"sha256", "byte_length",
                                              "executable"};
  static const char *const after_fields[] = {"sha256", "byte_length",
                                             "executable", "content_base64"};
  uint64_t length;
  return ab_artifact_object_exact(value, content ? after_fields : before_fields,
                                  content ? 4 : 3) &&
         ab_artifact_sha256(ab_value_member(value, "sha256")) &&
         ab_artifact_safe_integer(ab_value_member(value, "byte_length"),
                                  &length) &&
         length <= AB_PATCH_MAX_FILE_BYTES &&
         ab_artifact_boolean(ab_value_member(value, "executable")) &&
         (!content ||
          ab_artifact_bounded_text(ab_value_member(value, "content_base64"),
                                   ((AB_PATCH_MAX_FILE_BYTES + 2) / 3) * 4, 0));
}

static int validate_constraint_result(const AbValue *value, int accepted) {
  static const char *const fields[] = {"id", "status"};
  const AbValue *status;
  if (!ab_artifact_object_exact(value, fields, 2) ||
      !ab_artifact_stable_id(ab_value_member(value, "id")))
    return 0;
  status = ab_value_member(value, "status");
  if (!accepted)
    return ab_artifact_text_is(status, "not_evaluated");
  return ab_artifact_text_is(status, "pass") ||
         ab_artifact_text_is(status, "waived") ||
         ab_artifact_text_is(status, "not_applicable");
}

static int validate_acceptance(const AbValue *value, int accepted) {
  static const char *const fields[] = {"status", "verification_sha256",
                                       "constraints"};
  const AbValue *status;
  const AbValue *verification;
  const AbValue *constraints;
  size_t index;
  if (!ab_artifact_object_exact(value, fields, 3))
    return 0;
  status = ab_value_member(value, "status");
  verification = ab_value_member(value, "verification_sha256");
  constraints = ab_value_member(value, "constraints");
  if (!array_value(constraints, 0))
    return 0;
  if (accepted) {
    if (!ab_artifact_text_is(status, "satisfied") ||
        !ab_artifact_sha256(verification))
      return 0;
  } else if (!ab_artifact_text_is(status, "not_evaluated") ||
             verification->kind != AB_VALUE_NULL) {
    return 0;
  }
  for (index = 0; index < constraints->as.array.count; index++) {
    const AbValue *row = &constraints->as.array.items[index];
    if (!validate_constraint_result(row, accepted))
      return 0;
    if (index &&
        ab_string_compare(
            &ab_value_member(&constraints->as.array.items[index - 1], "id")
                 ->as.text,
            &ab_value_member(row, "id")->as.text) >= 0)
      return 0;
  }
  return 1;
}

static int validate_executor(const AbValue *value) {
  static const char *const fields[] = {"capability",    "implementation_sha256",
                                       "deterministic", "item_ids",
                                       "matches",       "skipped",
                                       "unsupported",   "reads",
                                       "writes"};
  uint64_t matches;
  uint64_t skipped;
  uint64_t unsupported;
  return ab_artifact_object_exact(value, fields, 9) &&
         ab_artifact_bounded_text(ab_value_member(value, "capability"),
                                  AB_PATCH_MAX_METADATA, 1) &&
         ab_artifact_sha256(ab_value_member(value, "implementation_sha256")) &&
         ab_artifact_boolean(ab_value_member(value, "deterministic")) &&
         sorted_string_array(ab_value_member(value, "item_ids"), 1, 1) &&
         ab_artifact_safe_integer(ab_value_member(value, "matches"),
                                  &matches) &&
         ab_artifact_safe_integer(ab_value_member(value, "skipped"),
                                  &skipped) &&
         ab_artifact_safe_integer(ab_value_member(value, "unsupported"),
                                  &unsupported) &&
         sorted_string_array(ab_value_member(value, "reads"), 0, 0) &&
         sorted_string_array(ab_value_member(value, "writes"), 0, 0);
}

static int transition_compare(const AbValue *left, const AbValue *right) {
  const AbValue *left_path = ab_value_member(left, "path");
  const AbValue *right_path = ab_value_member(right, "path");
  int compared = ab_string_compare(&left_path->as.text, &right_path->as.text);
  if (compared)
    return compared;
  return ab_string_compare(&ab_value_member(left, "kind")->as.text,
                           &ab_value_member(right, "kind")->as.text);
}

static ArchbirdStatus validate_transition(ArchbirdEngine *engine,
                                          const AbValue *value,
                                          AbPatchTransition *out,
                                          size_t *aggregate) {
  static const char *const fields[] = {"item_ids",    "kind",   "path",
                                       "source_path", "before", "after"};
  const AbValue *kind;
  const AbValue *path;
  const AbValue *source_path;
  const AbValue *before;
  const AbValue *after;
  uint64_t declared_length;
  uint8_t digest[32];
  char digest_hex[65];
  ArchbirdStatus status;
  if (!ab_artifact_object_exact(value, fields, 6) ||
      !sorted_string_array(ab_value_member(value, "item_ids"), 1, 1))
    return invalid(engine, "transition has invalid fields");
  kind = ab_value_member(value, "kind");
  path = ab_value_member(value, "path");
  source_path = ab_value_member(value, "source_path");
  before = ab_value_member(value, "before");
  after = ab_value_member(value, "after");
  if (!ab_artifact_repository_path(path))
    return invalid(engine, "transition path is invalid");
  if (ab_artifact_text_is(kind, "create")) {
    if (source_path->kind != AB_VALUE_NULL || before->kind != AB_VALUE_NULL ||
        !validate_file_state(after, 1))
      return invalid(engine, "create transition is inconsistent");
  } else if (ab_artifact_text_is(kind, "modify")) {
    if (source_path->kind != AB_VALUE_NULL || !validate_file_state(before, 0) ||
        !validate_file_state(after, 1))
      return invalid(engine, "modify transition is inconsistent");
  } else if (ab_artifact_text_is(kind, "delete")) {
    if (source_path->kind != AB_VALUE_NULL || !validate_file_state(before, 0) ||
        after->kind != AB_VALUE_NULL)
      return invalid(engine, "delete transition is inconsistent");
  } else if (ab_artifact_text_is(kind, "move")) {
    if (!ab_artifact_repository_path(source_path) ||
        ab_value_equal(path, source_path) || !validate_file_state(before, 0) ||
        !validate_file_state(after, 1))
      return invalid(engine, "move transition is inconsistent");
  } else {
    return invalid(engine, "transition kind is unsupported");
  }
  out->record = value;
  if (after->kind == AB_VALUE_NULL)
    return ARCHBIRD_OK;
  status = ab_base64_decode(
      engine, ab_value_member(after, "content_base64")->as.text.data,
      ab_value_member(after, "content_base64")->as.text.length,
      &out->after_bytes, &out->after_length);
  if (status != ARCHBIRD_OK)
    return status;
  if (!ab_artifact_safe_integer(ab_value_member(after, "byte_length"),
                                &declared_length) ||
      declared_length != out->after_length)
    return invalid(engine, "transition after byte length is inconsistent");
  status = archbird_sha256(out->after_bytes, out->after_length, digest);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_hex(digest, digest_hex);
  if (memcmp(digest_hex, ab_value_member(after, "sha256")->as.text.data, 64) !=
      0)
    return invalid(engine, "transition after SHA-256 is inconsistent");
  if (out->after_length > AB_PATCH_MAX_TOTAL_BYTES - *aggregate)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Patch after-source bytes exceed the limit");
  *aggregate += out->after_length;
  return ARCHBIRD_OK;
}

static ArchbirdStatus canonical_digest(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       char out[65]) {
  PatchDigestWriter writer;
  uint8_t digest[32];
  ArchbirdStatus status;
  archbird_sha256_init(&writer.context);
  writer.status = ARCHBIRD_OK;
  status = archbird_json_canonicalize(engine, json, length, 0, digest_write,
                                      &writer);
  if (status == ARCHBIRD_OK)
    status = writer.status;
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_final(&writer.context, digest);
  archbird_sha256_hex(digest, out);
  return ARCHBIRD_OK;
}

static ArchbirdStatus seal_digest(ArchbirdEngine *engine,
                                  const AbValue *document, char out[65]) {
  AbValue copy = {0};
  AbValue *seal;
  AbBuffer encoded;
  ArchbirdStatus status;
  ab_buffer_init(&encoded, engine);
  status = ab_value_copy(engine, &copy, document);
  seal =
      status == ARCHBIRD_OK ? (AbValue *)ab_value_member(&copy, "seal") : NULL;
  if (status == ARCHBIRD_OK && !seal)
    status = ARCHBIRD_CONFLICT;
  if (status == ARCHBIRD_OK) {
    ab_value_free(engine, seal);
    memset(seal, 0, sizeof(*seal));
    status = ab_value_render(&encoded, &copy);
  }
  if (status == ARCHBIRD_OK)
    status = canonical_digest(engine, encoded.data, encoded.length, out);
  ab_buffer_free(&encoded);
  ab_value_free(engine, &copy);
  return status;
}

static ArchbirdStatus validate_patch(ArchbirdEngine *engine, AbPatch *patch) {
  static const char *const fields[] = {
      "schema_version", "artifact",    "provenance", "tool",
      "plan_sha256",    "source",      "state",      "after",
      "executors",      "transitions", "acceptance", "seal"};
  const AbValue *state;
  const AbValue *executors;
  const AbValue *transitions;
  const AbValue *seal;
  uint64_t schema;
  size_t index;
  size_t aggregate = 0;
  int accepted;
  ArchbirdStatus status;
  if (!ab_artifact_object_exact(&patch->document, fields, 12) ||
      !ab_artifact_safe_integer(
          ab_value_member(&patch->document, "schema_version"), &schema) ||
      schema != 1 ||
      !ab_artifact_text_is(ab_value_member(&patch->document, "artifact"),
                           "patch") ||
      !ab_artifact_text_is(ab_value_member(&patch->document, "provenance"),
                           "derived") ||
      !validate_tool(ab_value_member(&patch->document, "tool")) ||
      !ab_artifact_sha256(ab_value_member(&patch->document, "plan_sha256")) ||
      !validate_source(ab_value_member(&patch->document, "source")))
    return invalid(engine, "document does not satisfy the Patch contract");
  state = ab_value_member(&patch->document, "state");
  accepted = ab_artifact_text_is(state, "accepted");
  if (!accepted && !ab_artifact_text_is(state, "materialized"))
    return invalid(engine, "state must be materialized or accepted");
  patch->source = ab_value_member(&patch->document, "source");
  patch->after = ab_value_member(&patch->document, "after");
  patch->acceptance = ab_value_member(&patch->document, "acceptance");
  if (!validate_after(patch->after) ||
      (accepted && patch->after->kind == AB_VALUE_NULL) ||
      (!accepted && patch->after->kind != AB_VALUE_NULL) ||
      !validate_acceptance(patch->acceptance, accepted))
    return invalid(engine, "after-state acceptance is inconsistent");
  if (accepted &&
      !ab_value_equal(
          ab_value_member(patch->acceptance, "verification_sha256"),
          ab_value_member(ab_value_member(patch->after, "verification"),
                          "sha256")))
    return invalid(engine, "accepted verification identities do not match");
  executors = ab_value_member(&patch->document, "executors");
  if (!array_value(executors, 0))
    return invalid(engine, "executors must be an array");
  for (index = 0; index < executors->as.array.count; index++)
    if (!validate_executor(&executors->as.array.items[index]))
      return invalid(engine, "executor ledger is invalid");
  transitions = ab_value_member(&patch->document, "transitions");
  if (!array_value(transitions, 0))
    return invalid(engine, "transitions must be an array");
  if (transitions->as.array.count) {
    patch->transitions = (AbPatchTransition *)ab_calloc(
        engine, transitions->as.array.count, sizeof(*patch->transitions));
    if (!patch->transitions)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding Patch transitions");
  }
  patch->transition_count = transitions->as.array.count;
  for (index = 0; index < patch->transition_count; index++) {
    if (index && transition_compare(&transitions->as.array.items[index - 1],
                                    &transitions->as.array.items[index]) >= 0)
      return invalid(engine, "transitions are not uniquely sorted");
    status = validate_transition(engine, &transitions->as.array.items[index],
                                 &patch->transitions[index], &aggregate);
    if (status != ARCHBIRD_OK)
      return status;
  }
  seal = ab_value_member(&patch->document, "seal");
  if (accepted) {
    static const char *const seal_fields[] = {"content_sha256"};
    char actual[65];
    if (!ab_artifact_object_exact(seal, seal_fields, 1) ||
        !ab_artifact_sha256(ab_value_member(seal, "content_sha256")))
      return invalid(engine, "accepted Patch seal is invalid");
    status = seal_digest(engine, &patch->document, actual);
    if (status != ARCHBIRD_OK)
      return status;
    if (memcmp(actual, ab_value_member(seal, "content_sha256")->as.text.data,
               64) != 0)
      return invalid(engine, "accepted Patch seal does not match content");
  } else if (seal->kind != AB_VALUE_NULL) {
    return invalid(engine, "materialized Patch must not be sealed");
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_patch_load(ArchbirdEngine *engine, const uint8_t *json,
                             size_t length, AbPatch *out) {
  ArchbirdStatus status;
  if (!engine || !json || !length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, length, &out->document);
  if (status == ARCHBIRD_OK)
    status = validate_patch(engine, out);
  if (status == ARCHBIRD_OK)
    status = canonical_digest(engine, json, length, out->sha256);
  if (status != ARCHBIRD_OK)
    ab_patch_free(engine, out);
  return status;
}

void ab_patch_free(ArchbirdEngine *engine, AbPatch *patch) {
  size_t index;
  if (!patch)
    return;
  for (index = 0; index < patch->transition_count; index++)
    ab_free(engine, patch->transitions[index].after_bytes);
  ab_free(engine, patch->transitions);
  ab_value_free(engine, &patch->document);
  memset(patch, 0, sizeof(*patch));
}
