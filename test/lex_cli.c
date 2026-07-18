#include "lexical/tokenizer.h"
#include <archbird/archbird.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kind_name(AbTokenKind kind) {
  switch (kind) {
  case AB_TOKEN_IDENTIFIER:
    return "identifier";
  case AB_TOKEN_NUMBER:
    return "number";
  case AB_TOKEN_STRING:
    return "string";
  case AB_TOKEN_TEMPLATE:
    return "template";
  case AB_TOKEN_REGEX:
    return "regex";
  case AB_TOKEN_OPERATOR:
    return "operator";
  }
  return "unknown";
}

static int read_input(uint8_t **out, size_t *out_length) {
  uint8_t *bytes = NULL;
  size_t length = 0;
  size_t capacity = 0;
  for (;;) {
    size_t read_count;
    if (length == capacity) {
      size_t next = capacity ? capacity * 2 : 4096;
      uint8_t *resized = (uint8_t *)realloc(bytes, next);
      if (!resized) {
        free(bytes);
        return 0;
      }
      bytes = resized;
      capacity = next;
    }
    read_count = fread(bytes + length, 1, capacity - length, stdin);
    length += read_count;
    if (read_count == 0) {
      if (ferror(stdin)) {
        free(bytes);
        return 0;
      }
      break;
    }
  }
  *out = bytes;
  *out_length = length;
  return 1;
}

int main(int argc, char **argv) {
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  AbTokenList tokens;
  uint8_t *source = NULL;
  size_t source_length = 0;
  uint32_t flags = 0;
  size_t index;
  if (argc == 2 && strcmp(argv[1], "--c") == 0)
    flags = AB_LEX_C_PREPROCESSOR;
  else if (argc == 2 && strcmp(argv[1], "--js") == 0)
    flags = AB_LEX_JAVASCRIPT;
  else if (argc == 2 && strcmp(argv[1], "--python") == 0)
    flags = AB_LEX_PYTHON;
  else if (argc != 1) {
    fputs("usage: lex_cli [--c|--js|--python]\n", stderr);
    return 2;
  }
  if (!read_input(&source, &source_length)) {
    fputs("failed to read source\n", stderr);
    return 2;
  }
  archbird_engine_options_init(&options);
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK) {
    free(source);
    return 2;
  }
  if (ab_tokenize(engine, source, source_length, flags, &tokens) !=
      ARCHBIRD_OK) {
    fprintf(stderr, "%s\n", archbird_engine_error(engine));
    archbird_engine_destroy(engine);
    free(source);
    return 1;
  }
  for (index = 0; index < tokens.count; index++) {
    const AbToken *token = &tokens.items[index];
    size_t cursor;
    printf("%zu\t%zu\t%zu\t%s\t", token->start, token->end, token->line,
           kind_name(token->kind));
    for (cursor = token->start; cursor < token->end; cursor++)
      printf("%02x", source[cursor]);
    putchar('\n');
  }
  ab_token_list_free(&tokens);
  archbird_engine_destroy(engine);
  free(source);
  return ferror(stdout) ? 2 : 0;
}
