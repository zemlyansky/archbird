#include "json_value.h"
#include "sha256.h"
#include "verify_runtime.h"

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

int main(void) {
  ArchbirdEngine *engine = NULL;
  AbValue envelope = {0};
  AbVerificationContext context = {0};
  AbBuffer output;
  Input input = {0};
  const AbValue *suite;
  const AbValue *evidence;
  size_t index;
  ArchbirdStatus status;
  if (!read_stdin(&input))
    return 2;
  status = archbird_engine_create(NULL, &engine);
  ab_buffer_init(&output, engine);
  if (status == ARCHBIRD_OK)
    status = ab_json_value_decode(engine, input.data, input.length, &envelope);
  suite = ab_value_member(&envelope, "suite");
  evidence = ab_value_member(&envelope, "input");
  context.engine = engine;
  if (status == ARCHBIRD_OK)
    status = ab_verify_suite_validate(engine, suite, &context.suite);
  if (status == ARCHBIRD_OK) {
    AbBuffer canonical;
    uint8_t digest[32];
    ab_buffer_init(&canonical, engine);
    status = ab_value_render(&canonical, suite);
    if (status == ARCHBIRD_OK)
      status = archbird_sha256(canonical.data, canonical.length, digest);
    if (status == ARCHBIRD_OK)
      archbird_sha256_hex(digest, context.suite.sha256);
    ab_buffer_free(&canonical);
  }
  if (status == ARCHBIRD_OK)
    status = ab_verify_input_validate(engine, &context.suite, evidence,
                                      &context.input);
  if (status == ARCHBIRD_OK)
    status = ab_verify_extract_all(&context);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&output, "[");
  for (index = 0; status == ARCHBIRD_OK && index < context.fact_count;
       index++) {
    if (index)
      status = ab_buffer_literal(&output, ",");
    if (status == ARCHBIRD_OK)
      status = ab_verify_fact_render(&output, &context.facts[index], 1);
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&output, "]\n");
  if (status == ARCHBIRD_OK)
    (void)fwrite(output.data, 1, output.length, stdout);
  else
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
  ab_buffer_free(&output);
  ab_verification_context_free(&context);
  ab_value_free(engine, &envelope);
  archbird_engine_destroy(engine);
  free(input.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
