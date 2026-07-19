#ifndef ARCHBIRD_LEX_H
#define ARCHBIRD_LEX_H

#include "archbird_internal.h"

typedef enum AbTokenKind {
  AB_TOKEN_IDENTIFIER = 0,
  AB_TOKEN_NUMBER = 1,
  AB_TOKEN_STRING = 2,
  AB_TOKEN_TEMPLATE = 3,
  AB_TOKEN_REGEX = 4,
  AB_TOKEN_OPERATOR = 5
} AbTokenKind;

typedef struct AbToken {
  size_t start;
  size_t end;
  size_t line;
  AbTokenKind kind;
} AbToken;

typedef struct AbTokenList {
  ArchbirdEngine *engine;
  const uint8_t *source;
  size_t source_length;
  AbToken *items;
  size_t count;
  size_t capacity;
} AbTokenList;

enum AbLexFlags {
  AB_LEX_C_PREPROCESSOR = 1u << 0,
  AB_LEX_JAVASCRIPT = 1u << 1,
  AB_LEX_R = 1u << 2,
  AB_LEX_PYTHON = 1u << 3
};

ArchbirdStatus ab_tokenize(ArchbirdEngine *engine, const uint8_t *source,
                           size_t source_length, uint32_t flags,
                           AbTokenList *out);

void ab_token_list_free(AbTokenList *tokens);

int ab_token_equals(const AbTokenList *tokens, size_t index,
                    const char *literal);

int ab_token_has_prefix(const AbTokenList *tokens, size_t index,
                        const char *literal);

#endif
