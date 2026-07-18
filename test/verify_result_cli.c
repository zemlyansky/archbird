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
  AbBuffer suite_json;
  AbBuffer input_json;
  Input input = {0};
  const AbValue *suite;
  const AbValue *evidence;
  ArchbirdStatus status;
  if (!read_stdin(&input))
    return 2;
  status = archbird_engine_create(NULL, &engine);
  ab_buffer_init(&suite_json, engine);
  ab_buffer_init(&input_json, engine);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, input.data, input.length, &envelope);
  suite = ab_value_member(&envelope, "suite");
  evidence = ab_value_member(&envelope, "input");
  if (status == ARCHBIRD_OK && (!suite || !evidence))
    status = ARCHBIRD_INVALID_SCHEMA;
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&suite_json, suite);
  if (status == ARCHBIRD_OK)
    status = ab_value_render(&input_json, evidence);
  if (status == ARCHBIRD_OK && argc == 1)
    status = archbird_verification_analyze(
        engine, suite_json.data, suite_json.length, input_json.data,
        input_json.length, ARCHBIRD_JSON_TRAILING_NEWLINE, write_stdout, NULL);
  else if (status == ARCHBIRD_OK && (argc == 2 || argc == 3)) {
    ArchbirdVerificationFormat format;
    uint32_t flags = 0;
    size_t max_findings = 200;
    if (!strcmp(argv[1], "markdown"))
      format = ARCHBIRD_VERIFICATION_MARKDOWN;
    else if (!strcmp(argv[1], "sarif")) {
      format = ARCHBIRD_VERIFICATION_SARIF;
      flags = ARCHBIRD_JSON_PRETTY | ARCHBIRD_JSON_TRAILING_NEWLINE;
    } else if (!strcmp(argv[1], "junit"))
      format = ARCHBIRD_VERIFICATION_JUNIT;
    else
      status = ARCHBIRD_INVALID_ARGUMENT;
    if (status == ARCHBIRD_OK && argc == 3) {
      char *end = NULL;
      unsigned long long parsed = strtoull(argv[2], &end, 10);
      if (!end || *end || parsed > SIZE_MAX)
        status = ARCHBIRD_INVALID_ARGUMENT;
      else
        max_findings = (size_t)parsed;
    }
    if (status == ARCHBIRD_OK)
      status = archbird_verification_analyze_report(
          engine, suite_json.data, suite_json.length, input_json.data,
          input_json.length, format, max_findings, flags, write_stdout, NULL);
  } else if (status == ARCHBIRD_OK) {
    status = ARCHBIRD_INVALID_ARGUMENT;
  }
  if (status != ARCHBIRD_OK)
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
  ab_buffer_free(&input_json);
  ab_buffer_free(&suite_json);
  ab_value_free(engine, &envelope);
  archbird_engine_destroy(engine);
  free(input.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
