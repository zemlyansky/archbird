#include "python/tokens.h"

AbPythonNameRef ab_python_token_ref(const AbTokenList *tokens, size_t index) {
  const AbToken *token = &tokens->items[index];
  AbPythonNameRef result = {tokens->source + token->start,
                            token->end - token->start};
  return result;
}

int ab_python_token_identifier(const AbTokenList *tokens, size_t index) {
  return index < tokens->count &&
         tokens->items[index].kind == AB_TOKEN_IDENTIFIER;
}

int ab_python_token_same_line(const AbTokenList *tokens, size_t left,
                              size_t right) {
  return left < tokens->count && right < tokens->count &&
         tokens->items[left].line == tokens->items[right].line;
}

size_t ab_python_token_indent(const AbTokenList *tokens, size_t index) {
  size_t start = tokens->items[index].start;
  size_t cursor = start;
  size_t column = 0;
  while (cursor > 0 && tokens->source[cursor - 1] != '\n' &&
         tokens->source[cursor - 1] != '\r')
    cursor--;
  while (cursor < start) {
    if (tokens->source[cursor] == ' ')
      column++;
    else if (tokens->source[cursor] == '\t')
      column = (column / 8 + 1) * 8;
    else
      break;
    cursor++;
  }
  return column;
}

int ab_python_line_prefix_is_space(const AbTokenList *tokens, size_t index) {
  size_t start = tokens->items[index].start;
  size_t cursor = start;
  while (cursor > 0 && tokens->source[cursor - 1] != '\n' &&
         tokens->source[cursor - 1] != '\r')
    cursor--;
  while (cursor < start) {
    if (tokens->source[cursor] != ' ' && tokens->source[cursor] != '\t' &&
        tokens->source[cursor] != '\f')
      return 0;
    cursor++;
  }
  return 1;
}

int ab_python_previous_line_is_explicit_continuation(const AbTokenList *tokens,
                                                     size_t index) {
  size_t cursor = tokens->items[index].start;
  while (cursor > 0 && tokens->source[cursor - 1] != '\n' &&
         tokens->source[cursor - 1] != '\r')
    cursor--;
  while (cursor > 0 && (tokens->source[cursor - 1] == '\n' ||
                        tokens->source[cursor - 1] == '\r'))
    cursor--;
  while (cursor > 0 && (tokens->source[cursor - 1] == ' ' ||
                        tokens->source[cursor - 1] == '\t' ||
                        tokens->source[cursor - 1] == '\f'))
    cursor--;
  return cursor > 0 && tokens->source[cursor - 1] == '\\';
}

ArchbirdStatus ab_python_source_utf8_validate(ArchbirdEngine *engine,
                                              const uint8_t *source,
                                              size_t source_length) {
  return ab_utf8_validate(engine, source, source_length);
}
