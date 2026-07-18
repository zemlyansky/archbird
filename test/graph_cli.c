#include <archbird/archbird.h>

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
  ArchbirdGraphOptions options;
  ArchbirdStatus status;
  Input input = {0};
  if (argc < 3 || argc > 4 || !read_stdin(&input))
    return 2;
  archbird_graph_options_init(&options);
  if (!strcmp(argv[1], "graphml"))
    options.format = ARCHBIRD_GRAPH_GRAPHML;
  else if (!strcmp(argv[1], "mermaid"))
    options.format = ARCHBIRD_GRAPH_MERMAID;
  else if (!strcmp(argv[1], "json"))
    options.format = ARCHBIRD_GRAPH_JSON;
  else
    return 2;
  if (!strcmp(argv[2], "components"))
    options.view = ARCHBIRD_GRAPH_COMPONENTS;
  else if (!strcmp(argv[2], "files"))
    options.view = ARCHBIRD_GRAPH_FILES;
  else if (!strcmp(argv[2], "symbols"))
    options.view = ARCHBIRD_GRAPH_SYMBOLS;
  else
    return 2;
  if (argc == 4) {
    if (!strcmp(argv[3], "LR"))
      options.direction = ARCHBIRD_GRAPH_LR;
    else if (!strcmp(argv[3], "RL"))
      options.direction = ARCHBIRD_GRAPH_RL;
    else if (!strcmp(argv[3], "TB"))
      options.direction = ARCHBIRD_GRAPH_TB;
    else if (!strcmp(argv[3], "BT"))
      options.direction = ARCHBIRD_GRAPH_BT;
    else
      return 2;
  }
  status = archbird_engine_create(NULL, &engine);
  if (status == ARCHBIRD_OK)
    status = archbird_map_export_graph(engine, input.data, input.length,
                                       &options, write_stdout, NULL);
  if (status != ARCHBIRD_OK)
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
  archbird_engine_destroy(engine);
  free(input.data);
  return status == ARCHBIRD_OK ? 0 : 1;
}
