#include "act_submission.h"

#include "act_source.h"
#include "artifact_validation.h"
#include "base64.h"
#include "utf8.h"

#include <string.h>

#define AB_ACT_MAX_SUBMITTED_FILE_BYTES (16u * 1024u * 1024u)
#define AB_ACT_MAX_SUBMITTED_BASE64_BYTES                                      \
  ((((AB_ACT_MAX_SUBMITTED_FILE_BYTES) + 2u) / 3u) * 4u)

static const AbValue *field(const AbValue *object, const char *name) {
  return object && object->kind == AB_VALUE_OBJECT
             ? ab_value_member(object, name)
             : NULL;
}

static ArchbirdStatus invalid(ArchbirdEngine *engine, ArchbirdStatus status,
                              const char *message) {
  return archbird_error_set(engine, status, ARCHBIRD_NO_OFFSET,
                            "act executor submissions: %s", message);
}

static ArchbirdStatus validate_row(ArchbirdEngine *engine, const AbValue *row,
                                   AbActSubmission *submission) {
  static const char *const fields[] = {"item_id", "kind", "content_base64"};
  const AbValue *item_id;
  const AbValue *kind;
  const AbValue *content;
  ArchbirdStatus status;
  if (!ab_artifact_object_exact(row, fields, 3))
    return invalid(engine, ARCHBIRD_INVALID_SCHEMA,
                   "item must contain item_id, kind, and content_base64");
  item_id = field(row, "item_id");
  kind = field(row, "kind");
  content = field(row, "content_base64");
  if (!ab_artifact_stable_id(item_id) ||
      !ab_artifact_text_is(kind, "write_file") || !content ||
      content->kind != AB_VALUE_STRING ||
      content->as.text.length > AB_ACT_MAX_SUBMITTED_BASE64_BYTES)
    return invalid(engine, ARCHBIRD_INVALID_SCHEMA,
                   "item identity, kind, or content is invalid");
  submission->item_id = &item_id->as.text;
  status = ab_base64_decode(engine, content->as.text.data,
                            content->as.text.length, &submission->replacement,
                            &submission->replacement_length);
  if (status == ARCHBIRD_OK &&
      submission->replacement_length > AB_ACT_MAX_SUBMITTED_FILE_BYTES)
    status = invalid(engine, ARCHBIRD_LIMIT_EXCEEDED,
                     "replacement exceeds the asserted file limit");
  if (status == ARCHBIRD_OK)
    status = ab_utf8_validate(engine, submission->replacement,
                              submission->replacement_length);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, submission->replacement);
    submission->replacement = NULL;
    submission->replacement_length = 0;
  }
  return status;
}

ArchbirdStatus ab_act_submissions_load(ArchbirdEngine *engine,
                                       const uint8_t *json, size_t length,
                                       AbActSubmissions *out) {
  static const char *const fields[] = {"items"};
  const AbValue *items;
  size_t index;
  size_t previous;
  ArchbirdStatus status;
  if (!engine || (!json && length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!length)
    return ARCHBIRD_OK;
  status = ab_json_value_decode(engine, json, length, &out->document);
  if (status != ARCHBIRD_OK)
    return status;
  if (!ab_artifact_object_exact(&out->document, fields, 1) ||
      !(items = field(&out->document, "items")) ||
      items->kind != AB_VALUE_ARRAY ||
      items->as.array.count > AB_ACT_MAX_TRANSITIONS) {
    status = invalid(engine, ARCHBIRD_INVALID_SCHEMA,
                     "document must contain a bounded items array");
    goto cleanup;
  }
  if (items->as.array.count) {
    out->items = (AbActSubmission *)ab_calloc(engine, items->as.array.count,
                                              sizeof(*out->items));
    if (!out->items) {
      status = invalid(engine, ARCHBIRD_OUT_OF_MEMORY,
                       "out of memory decoding items");
      goto cleanup;
    }
  }
  out->count = items->as.array.count;
  for (index = 0; status == ARCHBIRD_OK && index < out->count; index++)
    status =
        validate_row(engine, &items->as.array.items[index], &out->items[index]);
  for (index = 0; status == ARCHBIRD_OK && index < out->count; index++)
    for (previous = 0; previous < index; previous++)
      if (ab_string_equal(out->items[index].item_id,
                          out->items[previous].item_id)) {
        status =
            invalid(engine, ARCHBIRD_INVALID_SCHEMA, "item IDs must be unique");
        break;
      }
  if (status == ARCHBIRD_OK)
    return ARCHBIRD_OK;

cleanup:
  ab_act_submissions_free(engine, out);
  return status;
}

const AbValue *ab_act_submission_path(const AbValue *operation) {
  const AbValue *action = field(operation, "action");
  const AbValue *path = field(operation, "path");
  if (path && (ab_artifact_text_is(action, "add_symbol") ||
               ab_artifact_text_is(action, "add_test_route")))
    return path;
  return NULL;
}

AbActSubmission *ab_act_submission_take(AbActSubmissions *submissions,
                                        const AbString *item_id) {
  size_t index;
  if (!submissions || !item_id)
    return NULL;
  for (index = 0; index < submissions->count; index++) {
    AbActSubmission *submission = &submissions->items[index];
    if (!submission->consumed &&
        ab_string_equal(submission->item_id, item_id)) {
      submission->consumed = 1;
      return submission;
    }
  }
  return NULL;
}

ArchbirdStatus
ab_act_submissions_require_consumed(ArchbirdEngine *engine,
                                    const AbActSubmissions *submissions) {
  size_t index;
  if (!engine || !submissions)
    return ARCHBIRD_INVALID_ARGUMENT;
  for (index = 0; index < submissions->count; index++)
    if (!submissions->items[index].consumed)
      return invalid(engine, ARCHBIRD_POLICY_REJECTED,
                     "submission does not match an eligible Plan item");
  return ARCHBIRD_OK;
}

void ab_act_submissions_free(ArchbirdEngine *engine,
                             AbActSubmissions *submissions) {
  size_t index;
  if (!submissions)
    return;
  for (index = 0; index < submissions->count; index++)
    ab_free(engine, submissions->items[index].replacement);
  ab_free(engine, submissions->items);
  ab_value_free(engine, &submissions->document);
  memset(submissions, 0, sizeof(*submissions));
}
