#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Input {
  uint8_t *data;
  size_t length;
  size_t capacity;
} Input;

static int stdout_write(void *user_data, const uint8_t *bytes, size_t length) {
  (void)user_data;
  return fwrite(bytes, 1, length, stdout) == length ? 0 : 1;
}

static int read_stdin(Input *input) {
  uint8_t block[4096];
  size_t count;
  while ((count = fread(block, 1, sizeof(block), stdin)) != 0) {
    uint8_t *resized;
    size_t capacity = input->capacity ? input->capacity : 4096;
    if (count > SIZE_MAX - input->length)
      return 0;
    while (capacity < input->length + count) {
      if (capacity > SIZE_MAX / 2)
        return 0;
      capacity *= 2;
    }
    if (capacity != input->capacity) {
      resized = (uint8_t *)realloc(input->data, capacity);
      if (!resized)
        return 0;
      input->data = resized;
      input->capacity = capacity;
    }
    memcpy(input->data + input->length, block, count);
    input->length += count;
  }
  return !ferror(stdin);
}

int main(int argc, char **argv) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  ArchbirdStatus status;
  Input input = {0};
  uint32_t flags = 0;
  int validate_only = 0;
  int validate_manifest = 0;
  int validate_provider = 0;
  int index;
  for (index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--pretty") == 0) {
      flags |= ARCHBIRD_JSON_PRETTY;
    } else if (strcmp(argv[index], "--newline") == 0) {
      flags |= ARCHBIRD_JSON_TRAILING_NEWLINE;
    } else if (strcmp(argv[index], "--validate") == 0) {
      validate_only = 1;
    } else if (strcmp(argv[index], "--manifest") == 0) {
      validate_manifest = 1;
    } else if (strcmp(argv[index], "--provider") == 0) {
      validate_provider = 1;
    } else {
      fprintf(stderr, "unknown option: %s\n", argv[index]);
      return 2;
    }
  }
  if (!read_stdin(&input)) {
    fputs("failed to read stdin\n", stderr);
    free(input.data);
    return 2;
  }
  archbird_engine_options_init(&options);
  status = archbird_engine_create(&options, &engine);
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "failed to create engine: %d\n", status);
    free(input.data);
    return 2;
  }
  if (validate_only + validate_manifest + validate_provider > 1) {
    fputs("choose only one validation mode\n", stderr);
    archbird_engine_destroy(engine);
    free(input.data);
    return 2;
  }
  if (validate_manifest) {
    status =
        archbird_source_manifest_validate(engine, input.data, input.length);
  } else if (validate_provider) {
    status = archbird_provider_facts_validate(engine, input.data, input.length);
  } else if (validate_only) {
    status = archbird_json_validate(engine, input.data, input.length);
  } else {
    status = archbird_json_canonicalize(engine, input.data, input.length, flags,
                                        stdout_write, NULL);
  }
  if (status != ARCHBIRD_OK) {
    size_t offset = archbird_engine_error_offset(engine);
    if (offset == ARCHBIRD_NO_OFFSET) {
      fprintf(stderr, "archbird JSON error %d: %s\n", status,
              archbird_engine_error(engine));
    } else {
      fprintf(stderr, "archbird JSON error %d at byte %zu: %s\n", status,
              offset, archbird_engine_error(engine));
    }
  }
  archbird_engine_destroy(engine);
  free(input.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
