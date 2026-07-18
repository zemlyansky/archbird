#include <archbird/archbird.h>

#include "json_value.h"
#include "render_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Input {
  uint8_t *data;
  size_t length;
  size_t capacity;
} Input;

static int read_stdin(Input *input) {
  uint8_t block[4096];
  size_t count;
  while ((count = fread(block, 1, sizeof(block), stdin)) != 0) {
    uint8_t *resized;
    size_t capacity = input->capacity ? input->capacity : 4096;
    while (capacity < input->length + count)
      capacity *= 2;
    resized = (uint8_t *)realloc(input->data, capacity);
    if (!resized)
      return 0;
    input->data = resized;
    input->capacity = capacity;
    memcpy(input->data + input->length, block, count);
    input->length += count;
  }
  return !ferror(stdin);
}

static int write_stdout(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  return fwrite(bytes, 1, length, stdout) == length ? 0 : 1;
}

int main(int argc, char **argv) {
  ArchbirdEngine *engine = NULL;
  AbValue envelope = {0};
  AbBuffer verification_json;
  Input input = {0};
  const AbValue *verification;
  const AbValue *fingerprint;
  const AbValue *proposal;
  const AbValue *review;
  const AbValue *contract;
  const AbValue *before;
  const AbValue *after;
  ArchbirdStatus status;
  if (!read_stdin(&input))
    return 2;
  status = archbird_engine_create(NULL, &engine);
  ab_buffer_init(&verification_json, engine);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, input.data, input.length, &envelope);
  verification = ab_value_member(&envelope, "verification");
  fingerprint = ab_value_member(&envelope, "fingerprint");
  proposal = ab_value_member(&envelope, "proposal");
  review = ab_value_member(&envelope, "review");
  contract = ab_value_member(&envelope, "contract");
  before = ab_value_member(&envelope, "before");
  after = ab_value_member(&envelope, "after");
  if (status == ARCHBIRD_OK &&
      (argc == 1 || (argc == 2 && !strcmp(argv[1], "proposal-markdown")))) {
    if (!verification || !fingerprint || fingerprint->kind != AB_VALUE_STRING)
      status = ARCHBIRD_INVALID_SCHEMA;
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&verification_json, verification);
    if (status == ARCHBIRD_OK) {
      if (argc == 1)
        status = archbird_change_proposal(
            engine, verification_json.data, verification_json.length,
            fingerprint->as.text.data, fingerprint->as.text.length,
            ARCHBIRD_JSON_TRAILING_NEWLINE, write_stdout, NULL);
      else
        status = archbird_change_proposal_report(
            engine, verification_json.data, verification_json.length,
            fingerprint->as.text.data, fingerprint->as.text.length, 0, 100,
            write_stdout, NULL);
    }
  } else if (status == ARCHBIRD_OK && argc == 2 &&
             (!strcmp(argv[1], "contract") ||
              !strcmp(argv[1], "contract-markdown"))) {
    AbBuffer review_json;
    if (!proposal || !review)
      status = ARCHBIRD_INVALID_SCHEMA;
    ab_buffer_init(&review_json, engine);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&verification_json, proposal);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&review_json, review);
    if (status == ARCHBIRD_OK) {
      if (!strcmp(argv[1], "contract"))
        status = archbird_change_contract(
            engine, verification_json.data, verification_json.length,
            review_json.data, review_json.length,
            ARCHBIRD_JSON_TRAILING_NEWLINE, write_stdout, NULL);
      else
        status = archbird_change_contract_report(
            engine, verification_json.data, verification_json.length,
            review_json.data, review_json.length, write_stdout, NULL);
    }
    ab_buffer_free(&review_json);
  } else if (status == ARCHBIRD_OK && argc == 2 &&
             (!strcmp(argv[1], "verify") || !strcmp(argv[1], "markdown") ||
              !strcmp(argv[1], "sarif") || !strcmp(argv[1], "junit"))) {
    AbBuffer contract_json;
    AbBuffer before_json;
    AbBuffer after_json;
    if (!proposal || !contract || !before || !after)
      status = ARCHBIRD_INVALID_SCHEMA;
    ab_buffer_init(&contract_json, engine);
    ab_buffer_init(&before_json, engine);
    ab_buffer_init(&after_json, engine);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&verification_json, proposal);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&contract_json, contract);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&before_json, before);
    if (status == ARCHBIRD_OK)
      status = ab_value_render(&after_json, after);
    if (status == ARCHBIRD_OK) {
      if (!strcmp(argv[1], "verify"))
        status = archbird_change_verify(
            engine, verification_json.data, verification_json.length,
            contract_json.data, contract_json.length, before_json.data,
            before_json.length, after_json.data, after_json.length,
            ARCHBIRD_JSON_TRAILING_NEWLINE, write_stdout, NULL);
      else {
        ArchbirdChangeFormat format =
            !strcmp(argv[1], "markdown") ? ARCHBIRD_CHANGE_MARKDOWN
            : !strcmp(argv[1], "sarif")  ? ARCHBIRD_CHANGE_SARIF
                                         : ARCHBIRD_CHANGE_JUNIT;
        uint32_t flags =
            format == ARCHBIRD_CHANGE_SARIF
                ? ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE
                : 0;
        status = archbird_change_verify_report(
            engine, verification_json.data, verification_json.length,
            contract_json.data, contract_json.length, before_json.data,
            before_json.length, after_json.data, after_json.length, format,
            flags, write_stdout, NULL);
      }
    }
    ab_buffer_free(&after_json);
    ab_buffer_free(&before_json);
    ab_buffer_free(&contract_json);
  } else if (status == ARCHBIRD_OK) {
    status = ARCHBIRD_INVALID_ARGUMENT;
  }
  if (status != ARCHBIRD_OK)
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
  ab_buffer_free(&verification_json);
  ab_value_free(engine, &envelope);
  archbird_engine_destroy(engine);
  free(input.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
