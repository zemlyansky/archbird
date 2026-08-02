#include "act_internal.h"

#include "act_gate_acceptance.h"
#include "act_source.h"
#include "artifact_validation.h"
#include "base64.h"
#include "gate.h"
#include "model.h"
#include "projection_internal.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

#define AB_ACT_MAX_ROWS 4096u
#define AB_ACT_MAX_METADATA (64u * 1024u)
#define AB_ACT_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define AB_ACT_MAX_TOTAL_BYTES (256u * 1024u * 1024u)

typedef struct ActDigestWriter {
  ArchbirdSha256Context context;
  ArchbirdStatus status;
} ActDigestWriter;

static int digest_write(void *user_data, const uint8_t *bytes, size_t length) {
  ActDigestWriter *writer = (ActDigestWriter *)user_data;
  writer->status = archbird_sha256_update(&writer->context, bytes, length);
  return writer->status == ARCHBIRD_OK ? 0 : 1;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, const char *message) {
  return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
                            "act: %s", message);
}

static int array_value(const AbValue *value, size_t minimum) {
  return value && value->kind == AB_VALUE_ARRAY &&
         value->as.array.count >= minimum &&
         value->as.array.count <= AB_ACT_MAX_ROWS;
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
  static const char *const observed_fields[] = {"before_map", "project", "map",
                                                "verification"};
  const AbValue *before = ab_value_member(value, "before_map");
  return (ab_artifact_object_exact(value, fields, 3) ||
          ab_artifact_object_exact(value, observed_fields, 4)) &&
         ab_artifact_bounded_text(ab_value_member(value, "project"),
                                  AB_ACT_MAX_METADATA, 1) &&
         (!before || validate_identity(before, 1)) &&
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
         length <= AB_ACT_MAX_FILE_BYTES &&
         ab_artifact_boolean(ab_value_member(value, "executable")) &&
         (!content ||
          ab_artifact_bounded_text(ab_value_member(value, "content_base64"),
                                   ((AB_ACT_MAX_FILE_BYTES + 2) / 3) * 4, 0));
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

static int sorted_unique_strings(const AbValue *value) {
  size_t index;
  if (!array_value(value, 0))
    return 0;
  for (index = 0; index < value->as.array.count; index++) {
    const AbValue *row = &value->as.array.items[index];
    if (!ab_artifact_bounded_text(row, AB_ACT_MAX_METADATA, 1) ||
        (index && ab_string_compare(&value->as.array.items[index - 1].as.text,
                                    &row->as.text) >= 0))
      return 0;
  }
  return 1;
}

static int validate_projection_definition(ArchbirdEngine *engine,
                                          const AbValue *value) {
  const AbValue *declared_id = ab_value_member(value, "id");
  AbString fallback = {(char *)"act.delta", 9};
  const AbString *id = declared_id && declared_id->kind == AB_VALUE_STRING
                           ? &declared_id->as.text
                           : &fallback;
  AbProjectionPlan plan = {0};
  ArchbirdStatus status;
  if (!value || value->kind != AB_VALUE_OBJECT ||
      !ab_artifact_text_is(ab_value_member(value, "select"), "file_edges"))
    return 0;
  status = ab_projection_plan_compile(engine, value, id, &plan);
  ab_projection_plan_free(engine, &plan);
  return status == ARCHBIRD_OK;
}

static int validate_projection_delta(ArchbirdEngine *engine,
                                     const AbValue *value, int accepted) {
  static const char *const fields[] = {
      "projection",     "allowed_added",        "allowed_removed",
      "status",         "before_result_sha256", "after_result_sha256",
      "observed_added", "observed_removed"};
  const AbValue *allowed_added;
  const AbValue *allowed_removed;
  const AbValue *observed_added;
  const AbValue *observed_removed;
  const AbValue *before_sha;
  const AbValue *after_sha;
  const AbValue *status;
  size_t left;
  size_t right;
  if (!ab_artifact_object_exact(value, fields, 8) ||
      !validate_projection_definition(engine,
                                      ab_value_member(value, "projection")))
    return 0;
  allowed_added = ab_value_member(value, "allowed_added");
  allowed_removed = ab_value_member(value, "allowed_removed");
  observed_added = ab_value_member(value, "observed_added");
  observed_removed = ab_value_member(value, "observed_removed");
  before_sha = ab_value_member(value, "before_result_sha256");
  after_sha = ab_value_member(value, "after_result_sha256");
  status = ab_value_member(value, "status");
  if (!sorted_unique_strings(allowed_added) ||
      !sorted_unique_strings(allowed_removed) ||
      (!allowed_added->as.array.count && !allowed_removed->as.array.count) ||
      !sorted_unique_strings(observed_added) ||
      !sorted_unique_strings(observed_removed))
    return 0;
  for (left = 0; left < allowed_added->as.array.count; left++)
    for (right = 0; right < allowed_removed->as.array.count; right++)
      if (ab_value_equal(&allowed_added->as.array.items[left],
                         &allowed_removed->as.array.items[right]))
        return 0;
  if (!accepted)
    return ab_artifact_text_is(status, "not_evaluated") &&
           before_sha->kind == AB_VALUE_NULL &&
           after_sha->kind == AB_VALUE_NULL &&
           !observed_added->as.array.count && !observed_removed->as.array.count;
  return ab_artifact_text_is(status, "satisfied") &&
         ab_artifact_sha256(before_sha) && ab_artifact_sha256(after_sha) &&
         ab_value_equal(allowed_added, observed_added) &&
         ab_value_equal(allowed_removed, observed_removed);
}

static int validate_acceptance(ArchbirdEngine *engine, const AbValue *value,
                               int accepted) {
  static const char *const fields[] = {"status",
                                       "verification_sha256",
                                       "constraints",
                                       "projection_deltas",
                                       "gate_workspace_sha256",
                                       "gate_results"};
  const AbValue *status;
  const AbValue *verification;
  const AbValue *constraints;
  const AbValue *projection_deltas;
  const AbValue *gate_workspace;
  const AbValue *gate_results;
  size_t index;
  if (!ab_artifact_object_exact(value, fields, 6))
    return 0;
  status = ab_value_member(value, "status");
  verification = ab_value_member(value, "verification_sha256");
  constraints = ab_value_member(value, "constraints");
  projection_deltas = ab_value_member(value, "projection_deltas");
  gate_workspace = ab_value_member(value, "gate_workspace_sha256");
  gate_results = ab_value_member(value, "gate_results");
  if (!array_value(constraints, 0) || !array_value(projection_deltas, 0) ||
      !array_value(gate_results, 0))
    return 0;
  if (accepted) {
    if (!ab_artifact_text_is(status, "satisfied") ||
        !ab_artifact_sha256(verification) ||
        (gate_results->as.array.count && !ab_artifact_sha256(gate_workspace)) ||
        (!gate_results->as.array.count &&
         gate_workspace->kind != AB_VALUE_NULL))
      return 0;
  } else if (!ab_artifact_text_is(status, "not_evaluated") ||
             verification->kind != AB_VALUE_NULL ||
             gate_workspace->kind != AB_VALUE_NULL ||
             gate_results->as.array.count) {
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
  for (index = 0; index < projection_deltas->as.array.count; index++) {
    size_t prior;
    const AbValue *row = &projection_deltas->as.array.items[index];
    if (!validate_projection_delta(engine, row, accepted))
      return 0;
    for (prior = 0; prior < index; prior++)
      if (ab_value_equal(
              ab_value_member(row, "projection"),
              ab_value_member(&projection_deltas->as.array.items[prior],
                              "projection")))
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
                                  AB_ACT_MAX_METADATA, 1) &&
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

static int validate_source_lock(const AbValue *value) {
  static const char *const fields[] = {"path", "sha256", "executable"};
  return ab_artifact_object_exact(value, fields, 3) &&
         ab_artifact_repository_path(ab_value_member(value, "path")) &&
         ab_artifact_sha256(ab_value_member(value, "sha256")) &&
         ab_artifact_boolean(ab_value_member(value, "executable"));
}

static int source_lock_compare(const AbValue *left, const AbValue *right) {
  return ab_string_compare(&ab_value_member(left, "path")->as.text,
                           &ab_value_member(right, "path")->as.text);
}

static size_t source_lock_index(const AbValue *locks, const AbString *path) {
  size_t low = 0;
  size_t high = locks->as.array.count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbValue *row = &locks->as.array.items[middle];
    int compared =
        ab_string_compare(&ab_value_member(row, "path")->as.text, path);
    if (compared < 0)
      low = middle + 1;
    else if (compared > 0)
      high = middle;
    else
      return middle;
  }
  return SIZE_MAX;
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
                                          AbActTransition *out,
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
  if (out->after_length > AB_ACT_MAX_TOTAL_BYTES - *aggregate)
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Act after-source bytes exceed the limit");
  *aggregate += out->after_length;
  return ARCHBIRD_OK;
}

static int transition_covers_path(const AbAct *act, const AbString *path) {
  size_t index;
  for (index = 0; index < act->transition_count; index++) {
    const AbValue *transition = act->transitions[index].record;
    const AbValue *transition_path = ab_value_member(transition, "path");
    const AbValue *source_path = ab_value_member(transition, "source_path");
    if (ab_string_equal(&transition_path->as.text, path) ||
        (source_path->kind == AB_VALUE_STRING &&
         ab_string_equal(&source_path->as.text, path)))
      return 1;
  }
  return 0;
}

static ArchbirdStatus validate_read_coverage(ArchbirdEngine *engine, AbAct *act,
                                             const AbValue *executors) {
  uint8_t *used = NULL;
  size_t executor_index;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (act->source_locks->as.array.count) {
    used = (uint8_t *)ab_calloc(engine, act->source_locks->as.array.count, 1);
    if (!used)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory validating Act source locks");
  }
  for (executor_index = 0;
       status == ARCHBIRD_OK && executor_index < executors->as.array.count;
       executor_index++) {
    const AbValue *reads =
        ab_value_member(&executors->as.array.items[executor_index], "reads");
    for (index = 0; index < reads->as.array.count; index++) {
      const AbString *path = &reads->as.array.items[index].as.text;
      size_t lock_index;
      if (transition_covers_path(act, path))
        continue;
      lock_index = source_lock_index(act->source_locks, path);
      if (lock_index == SIZE_MAX) {
        status = invalid(engine, "executor read has no source lock");
        break;
      }
      used[lock_index] = 1;
    }
  }
  for (index = 0;
       status == ARCHBIRD_OK && index < act->source_locks->as.array.count;
       index++)
    if (!used[index])
      status = invalid(engine, "source lock is not used by an executor");
  ab_free(engine, used);
  return status;
}

static ArchbirdStatus canonical_digest(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       char out[65]) {
  ActDigestWriter writer;
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

static ArchbirdStatus validate_act(ArchbirdEngine *engine, AbAct *act) {
  static const char *const fields[] = {
      "schema_version", "artifact",     "provenance", "tool",  "plan_sha256",
      "source",         "source_locks", "state",      "after", "gates",
      "executors",      "transitions",  "acceptance", "seal"};
  const AbValue *state;
  const AbValue *executors;
  const AbValue *source_locks;
  const AbValue *transitions;
  const AbValue *seal;
  uint64_t schema;
  size_t index;
  size_t aggregate = 0;
  int accepted;
  ArchbirdStatus status;
  if (!ab_artifact_object_exact(&act->document, fields, 14) ||
      !ab_artifact_safe_integer(
          ab_value_member(&act->document, "schema_version"), &schema) ||
      schema != 5 ||
      !ab_artifact_text_is(ab_value_member(&act->document, "artifact"),
                           "act") ||
      !ab_artifact_text_is(ab_value_member(&act->document, "provenance"),
                           "derived") ||
      !validate_tool(ab_value_member(&act->document, "tool")) ||
      !ab_artifact_sha256(ab_value_member(&act->document, "plan_sha256")) ||
      !validate_source(ab_value_member(&act->document, "source")) ||
      !ab_gate_definitions_valid(ab_value_member(&act->document, "gates")))
    return invalid(engine, "document does not satisfy the Act contract");
  state = ab_value_member(&act->document, "state");
  accepted = ab_artifact_text_is(state, "accepted");
  if (!accepted && !ab_artifact_text_is(state, "materialized"))
    return invalid(engine, "state must be materialized or accepted");
  act->source = ab_value_member(&act->document, "source");
  act->gates = ab_value_member(&act->document, "gates");
  act->after = ab_value_member(&act->document, "after");
  act->acceptance = ab_value_member(&act->document, "acceptance");
  if (!validate_after(act->after) ||
      (accepted && act->after->kind == AB_VALUE_NULL) ||
      (!accepted && act->after->kind != AB_VALUE_NULL) ||
      !validate_acceptance(engine, act->acceptance, accepted))
    return invalid(engine, "after-state acceptance is inconsistent");
  if (accepted &&
      !ab_value_equal(
          ab_value_member(act->acceptance, "verification_sha256"),
          ab_value_member(ab_value_member(act->after, "verification"),
                          "sha256")))
    return invalid(engine, "accepted verification identities do not match");
  if (accepted) {
    status = ab_act_gate_acceptance_validate(
        engine, act->gates,
        ab_value_member(act->acceptance, "gate_workspace_sha256"),
        ab_value_member(act->acceptance, "gate_results"));
    if (status != ARCHBIRD_OK)
      return status;
  }
  executors = ab_value_member(&act->document, "executors");
  if (!array_value(executors, 0))
    return invalid(engine, "executors must be an array");
  for (index = 0; index < executors->as.array.count; index++)
    if (!validate_executor(&executors->as.array.items[index]))
      return invalid(engine, "executor ledger is invalid");
  source_locks = ab_value_member(&act->document, "source_locks");
  if (!source_locks || source_locks->kind != AB_VALUE_ARRAY ||
      source_locks->as.array.count > AB_ACT_MAX_SOURCE_PATHS)
    return invalid(engine, "source_locks must be a bounded array");
  act->source_locks = source_locks;
  for (index = 0; index < source_locks->as.array.count; index++) {
    if (!validate_source_lock(&source_locks->as.array.items[index]))
      return invalid(engine, "source lock is invalid");
    if (index && source_lock_compare(&source_locks->as.array.items[index - 1],
                                     &source_locks->as.array.items[index]) >= 0)
      return invalid(engine, "source locks are not uniquely sorted");
  }
  transitions = ab_value_member(&act->document, "transitions");
  if (!array_value(transitions, 0))
    return invalid(engine, "transitions must be an array");
  if (transitions->as.array.count) {
    act->transitions = (AbActTransition *)ab_calloc(
        engine, transitions->as.array.count, sizeof(*act->transitions));
    if (!act->transitions)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory decoding Act transitions");
  }
  act->transition_count = transitions->as.array.count;
  for (index = 0; index < act->transition_count; index++) {
    if (index && transition_compare(&transitions->as.array.items[index - 1],
                                    &transitions->as.array.items[index]) >= 0)
      return invalid(engine, "transitions are not uniquely sorted");
    status = validate_transition(engine, &transitions->as.array.items[index],
                                 &act->transitions[index], &aggregate);
    if (status != ARCHBIRD_OK)
      return status;
  }
  for (index = 0; index < source_locks->as.array.count; index++)
    if (transition_covers_path(
            act, &ab_value_member(&source_locks->as.array.items[index], "path")
                      ->as.text))
      return invalid(engine,
                     "source lock duplicates a transition source state");
  status = validate_read_coverage(engine, act, executors);
  if (status != ARCHBIRD_OK)
    return status;
  seal = ab_value_member(&act->document, "seal");
  if (accepted) {
    static const char *const seal_fields[] = {"content_sha256"};
    char actual[65];
    if (!ab_artifact_object_exact(seal, seal_fields, 1) ||
        !ab_artifact_sha256(ab_value_member(seal, "content_sha256")))
      return invalid(engine, "accepted Act seal is invalid");
    status = seal_digest(engine, &act->document, actual);
    if (status != ARCHBIRD_OK)
      return status;
    if (memcmp(actual, ab_value_member(seal, "content_sha256")->as.text.data,
               64) != 0)
      return invalid(engine, "accepted Act seal does not match content");
  } else if (seal->kind != AB_VALUE_NULL) {
    return invalid(engine, "materialized Act must not be sealed");
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_act_load(ArchbirdEngine *engine, const uint8_t *json,
                           size_t length, AbAct *out) {
  ArchbirdStatus status;
  if (!engine || !json || !length || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_json_value_decode(engine, json, length, &out->document);
  if (status == ARCHBIRD_OK)
    status = validate_act(engine, out);
  if (status == ARCHBIRD_OK)
    status = canonical_digest(engine, json, length, out->sha256);
  if (status != ARCHBIRD_OK)
    ab_act_free(engine, out);
  return status;
}

void ab_act_free(ArchbirdEngine *engine, AbAct *act) {
  size_t index;
  if (!act)
    return;
  for (index = 0; index < act->transition_count; index++)
    ab_free(engine, act->transitions[index].after_bytes);
  ab_free(engine, act->transitions);
  ab_value_free(engine, &act->document);
  memset(act, 0, sizeof(*act));
}
