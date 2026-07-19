#ifndef ARCHBIRD_PYTHON_TOKENS_H
#define ARCHBIRD_PYTHON_TOKENS_H

#include "lexical/tokenizer.h"

typedef struct AbPythonNameRef {
  const uint8_t *data;
  size_t length;
} AbPythonNameRef;

AbPythonNameRef ab_python_token_ref(const AbTokenList *tokens, size_t index);
int ab_python_token_identifier(const AbTokenList *tokens, size_t index);
int ab_python_token_same_line(const AbTokenList *tokens, size_t left,
                              size_t right);
size_t ab_python_token_indent(const AbTokenList *tokens, size_t index);
int ab_python_line_prefix_is_space(const AbTokenList *tokens, size_t index);
int ab_python_previous_line_is_explicit_continuation(const AbTokenList *tokens,
                                                     size_t index);
ArchbirdStatus ab_python_source_utf8_validate(ArchbirdEngine *engine,
                                              const uint8_t *source,
                                              size_t source_length);

#endif
