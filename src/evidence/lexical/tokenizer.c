#include "lexical/tokenizer.h"

#include "utf8.h"

#include <stdlib.h>
#include <string.h>

static int ascii_space(uint8_t value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\v' || value == '\f';
}

static int ascii_alpha(uint8_t value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

static int ascii_digit(uint8_t value) { return value >= '0' && value <= '9'; }

static int identifier_start(uint8_t value) {
  return ascii_alpha(value) || value == '_' || value == '$';
}

static int identifier_continue(uint8_t value) {
  return identifier_start(value) || ascii_digit(value);
}

static int r_identifier_start(uint8_t value) {
  return ascii_alpha(value) || value == '.';
}

static int r_identifier_continue(uint8_t value) {
  return ascii_alpha(value) || ascii_digit(value) || value == '_' ||
         value == '.';
}

static int python_identifier_start(uint8_t value) {
  return ascii_alpha(value) || value == '_' || value >= 0x80;
}

static int python_identifier_continue(uint8_t value) {
  return python_identifier_start(value) || ascii_digit(value);
}

static ArchbirdStatus token_append(ArchbirdEngine *engine, AbTokenList *tokens,
                                   size_t start, size_t end, size_t line,
                                   AbTokenKind kind) {
  AbToken *resized;
  if (tokens->count == tokens->capacity) {
    size_t capacity = tokens->capacity ? tokens->capacity * 2 : 128;
    if (capacity < tokens->capacity || capacity > engine->options.max_values)
      capacity = engine->options.max_values;
    if (capacity <= tokens->capacity)
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "lexical token limit exceeded");
    resized = (AbToken *)ab_realloc(engine, tokens->items,
                                    capacity * sizeof(*tokens->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing lexical tokens");
    tokens->items = resized;
    tokens->capacity = capacity;
  }
  tokens->items[tokens->count++] = (AbToken){start, end, line, kind};
  return ARCHBIRD_OK;
}

static int token_literal(const AbTokenList *tokens, const AbToken *token,
                         const char *literal) {
  size_t length = strlen(literal);
  return token->end - token->start == length &&
         (length == 0 ||
          memcmp(tokens->source + token->start, literal, length) == 0);
}

int ab_token_equals(const AbTokenList *tokens, size_t index,
                    const char *literal) {
  return tokens && index < tokens->count &&
         token_literal(tokens, &tokens->items[index], literal);
}

int ab_token_has_prefix(const AbTokenList *tokens, size_t index,
                        const char *literal) {
  size_t length = strlen(literal);
  const AbToken *token;
  if (!tokens || index >= tokens->count)
    return 0;
  token = &tokens->items[index];
  return token->end - token->start >= length &&
         (length == 0 ||
          memcmp(tokens->source + token->start, literal, length) == 0);
}

static int js_prefix_word(const AbTokenList *tokens, const AbToken *token) {
  static const char *const words[] = {
      "await", "case",   "delete", "do",     "else", "in",    "instanceof",
      "of",    "return", "throw",  "typeof", "void", "yield",
  };
  size_t index;
  for (index = 0; index < sizeof(words) / sizeof(words[0]); index++) {
    if (token_literal(tokens, token, words[index]))
      return 1;
  }
  return 0;
}

static int js_regex_allowed(const AbTokenList *tokens) {
  const AbToken *previous;
  if (tokens->count == 0)
    return 1;
  previous = &tokens->items[tokens->count - 1];
  if (js_prefix_word(tokens, previous))
    return 1;
  if (previous->kind == AB_TOKEN_IDENTIFIER ||
      previous->kind == AB_TOKEN_NUMBER || previous->kind == AB_TOKEN_STRING ||
      previous->kind == AB_TOKEN_TEMPLATE || previous->kind == AB_TOKEN_REGEX)
    return 0;
  return !token_literal(tokens, previous, ")") &&
         !token_literal(tokens, previous, "]") &&
         !token_literal(tokens, previous, "}");
}

static size_t js_regex_end(const uint8_t *source, size_t length, size_t start) {
  size_t cursor = start + 1;
  int escaped = 0;
  int in_class = 0;
  while (cursor < length) {
    uint8_t value = source[cursor];
    if (value == '\r' || value == '\n')
      return 0;
    if (escaped) {
      escaped = 0;
    } else if (value == '\\') {
      escaped = 1;
    } else if (value == '[') {
      in_class = 1;
    } else if (value == ']' && in_class) {
      in_class = 0;
    } else if (value == '/' && !in_class) {
      cursor++;
      while (cursor < length &&
             (ascii_alpha(source[cursor]) || ascii_digit(source[cursor])))
        cursor++;
      return cursor;
    }
    cursor++;
  }
  return 0;
}

static size_t quoted_end(const uint8_t *source, size_t length, size_t start,
                         uint8_t quote) {
  size_t cursor = start + 1;
  while (cursor < length) {
    if (source[cursor] == '\\') {
      if (cursor + 1 >= length)
        return 0;
      cursor += 2;
    } else if (source[cursor] == quote) {
      return cursor + 1;
    } else {
      cursor++;
    }
  }
  return 0;
}

static size_t python_quoted_end(const uint8_t *source, size_t length,
                                size_t start, uint8_t quote) {
  size_t cursor;
  int triple = start + 2 < length && source[start + 1] == quote &&
               source[start + 2] == quote;
  cursor = start + (triple ? 3 : 1);
  while (cursor < length) {
    if (source[cursor] == '\\') {
      if (cursor + 1 >= length)
        return 0;
      cursor += 2;
    } else if (triple && cursor + 2 < length && source[cursor] == quote &&
               source[cursor + 1] == quote && source[cursor + 2] == quote) {
      return cursor + 3;
    } else if (!triple && source[cursor] == quote) {
      return cursor + 1;
    } else {
      cursor++;
    }
  }
  return 0;
}

static size_t number_end(const uint8_t *source, size_t length, size_t start) {
  size_t cursor = start;
  if (cursor + 2 <= length && source[cursor] == '0' &&
      (source[cursor + 1] == 'x' || source[cursor + 1] == 'X')) {
    size_t digits = cursor + 2;
    cursor = digits;
    while (cursor < length &&
           (ascii_digit(source[cursor]) ||
            (source[cursor] >= 'a' && source[cursor] <= 'f') ||
            (source[cursor] >= 'A' && source[cursor] <= 'F')))
      cursor++;
    return cursor == digits ? start + 1 : cursor;
  }
  while (cursor < length && ascii_digit(source[cursor]))
    cursor++;
  if (cursor < length && source[cursor] == '.') {
    cursor++;
    while (cursor < length && ascii_digit(source[cursor]))
      cursor++;
  }
  if (cursor < length && (source[cursor] == 'e' || source[cursor] == 'E')) {
    size_t exponent = cursor;
    cursor++;
    if (cursor < length && (source[cursor] == '+' || source[cursor] == '-'))
      cursor++;
    if (cursor < length && ascii_digit(source[cursor])) {
      while (cursor < length && ascii_digit(source[cursor]))
        cursor++;
    } else {
      cursor = exponent;
    }
  }
  return cursor;
}

/* C preprocessing numbers deliberately accept identifier continuations. This
 * matters before macro expansion: token-paste style harness arguments such as
 * `2d_e2e` are one preprocessing token even though they are not a C numeric
 * literal. Keep this contract scoped to AB_LEX_C_PREPROCESSOR so the other
 * language tokenizers retain their numeric-literal behavior. */
static size_t c_preprocessing_number_end(const uint8_t *source, size_t length,
                                         size_t start) {
  size_t cursor = start + 1;
  while (cursor < length) {
    uint8_t value = source[cursor];
    if (ascii_digit(value) || identifier_start(value) || value == '.') {
      cursor++;
      continue;
    }
    if ((value == '+' || value == '-') && cursor > start &&
        (source[cursor - 1] == 'e' || source[cursor - 1] == 'E' ||
         source[cursor - 1] == 'p' || source[cursor - 1] == 'P')) {
      cursor++;
      continue;
    }
    break;
  }
  return cursor;
}

static size_t operator_end(const uint8_t *source, size_t length, size_t start) {
  static const char *const operators[] = {
      "===", "!==", ">>>", "<<=", ">>=", "**=", "//=", "@=", "=>", "->",
      ":=",  "==",  "!=",  "<=",  ">=",  "++",  "--",  "&&", "||", "<<",
      ">>",  "+=",  "-=",  "*=",  "/=",  "%=",  "**",  "//", "??", "?.",
  };
  size_t index;
  for (index = 0; index < sizeof(operators) / sizeof(operators[0]); index++) {
    size_t width = strlen(operators[index]);
    if (width <= length - start &&
        memcmp(source + start, operators[index], width) == 0)
      return start + width;
  }
  {
    size_t width = ab_utf8_scalar_length(source, length, start);
    return width ? start + width : 0;
  }
}

static int js_embedded_apostrophe(const AbTokenList *tokens,
                                  const uint8_t *source, size_t length,
                                  size_t cursor) {
  static const char *const string_prefixes[] = {
      "await", "case",   "default", "delete",     "do", "else",
      "from",  "import", "in",      "instanceof", "of", "return",
      "throw", "typeof", "void",    "yield",
  };
  const AbToken *previous;
  size_t index;
  if (!cursor || cursor + 1 >= length ||
      !identifier_continue(source[cursor - 1]) ||
      !identifier_continue(source[cursor + 1]) || !tokens->count)
    return 0;
  previous = &tokens->items[tokens->count - 1];
  if (previous->kind != AB_TOKEN_IDENTIFIER || previous->end != cursor)
    return 0;
  for (index = 0; index < sizeof(string_prefixes) / sizeof(string_prefixes[0]);
       index++)
    if (ab_token_equals(tokens, tokens->count - 1, string_prefixes[index]))
      return 0;
  return 1;
}

ArchbirdStatus ab_tokenize(ArchbirdEngine *engine, const uint8_t *source,
                           size_t source_length, uint32_t flags,
                           AbTokenList *out) {
  size_t cursor = 0;
  size_t line = 1;
  int line_prefix = 1;
  if (!engine || (!source && source_length) || !out ||
      (flags &
       ~(AB_LEX_C_PREPROCESSOR | AB_LEX_JAVASCRIPT | AB_LEX_R | AB_LEX_PYTHON)))
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  out->source = source;
  out->source_length = source_length;
  while (cursor < source_length) {
    size_t start = cursor;
    size_t end = 0;
    AbTokenKind kind = AB_TOKEN_OPERATOR;
    uint8_t value = source[cursor];
    if (value >= 0x80 &&
        ab_utf8_scalar_length(source, source_length, cursor) == 0) {
      ab_token_list_free(out);
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, cursor,
                                "source is not valid UTF-8");
    }
    if ((flags & AB_LEX_C_PREPROCESSOR) && line_prefix && value == '#') {
      while (cursor < source_length && source[cursor] != '\r' &&
             source[cursor] != '\n')
        cursor++;
      continue;
    }
    if (ascii_space(value)) {
      while (cursor < source_length && ascii_space(source[cursor])) {
        if (source[cursor] == '\n') {
          line++;
          line_prefix = 1;
        } else if (line_prefix && source[cursor] != '\r') {
          line_prefix = 1;
        }
        cursor++;
      }
      continue;
    }
    line_prefix = 0;
    if (!(flags & (AB_LEX_PYTHON | AB_LEX_R)) && value == '/' &&
        cursor + 1 < source_length && source[cursor + 1] == '/') {
      cursor += 2;
      while (cursor < source_length && source[cursor] != '\r' &&
             source[cursor] != '\n')
        cursor++;
      continue;
    }
    if ((flags & (AB_LEX_R | AB_LEX_PYTHON)) && value == '#') {
      while (cursor < source_length && source[cursor] != '\r' &&
             source[cursor] != '\n')
        cursor++;
      continue;
    }
    if (!(flags & (AB_LEX_PYTHON | AB_LEX_R)) && value == '/' &&
        cursor + 1 < source_length && source[cursor + 1] == '*') {
      size_t scan = cursor + 2;
      while (scan + 1 < source_length &&
             !(source[scan] == '*' && source[scan + 1] == '/'))
        scan++;
      if (scan + 1 < source_length) {
        end = scan + 2;
        while (cursor < end) {
          if (source[cursor] == '\n') {
            line++;
            line_prefix = 1;
          }
          cursor++;
        }
        continue;
      }
    }
    if ((flags & AB_LEX_JAVASCRIPT) && value == '/' && js_regex_allowed(out)) {
      end = js_regex_end(source, source_length, cursor);
      if (end)
        kind = AB_TOKEN_REGEX;
    }
    if (!end && (value == '\'' || value == '"' || value == '`') &&
        !((flags & AB_LEX_JAVASCRIPT) && value == '\'' &&
          js_embedded_apostrophe(out, source, source_length, cursor))) {
      end = (flags & AB_LEX_PYTHON)
                ? python_quoted_end(source, source_length, cursor, value)
                : quoted_end(source, source_length, cursor, value);
      if (end)
        kind = value == '`' ? AB_TOKEN_TEMPLATE : AB_TOKEN_STRING;
    }
    if (!end && ((flags & AB_LEX_R)        ? r_identifier_start(value)
                 : (flags & AB_LEX_PYTHON) ? python_identifier_start(value)
                                           : identifier_start(value))) {
      end = cursor + 1;
      while (end < source_length &&
             ((flags & AB_LEX_R) ? r_identifier_continue(source[end])
              : (flags & AB_LEX_PYTHON)
                  ? python_identifier_continue(source[end])
                  : identifier_continue(source[end])))
        end++;
      kind = AB_TOKEN_IDENTIFIER;
    }
    if (!end && ascii_digit(value)) {
      end = (flags & AB_LEX_C_PREPROCESSOR)
                ? c_preprocessing_number_end(source, source_length, cursor)
                : number_end(source, source_length, cursor);
      kind = AB_TOKEN_NUMBER;
    }
    if (!end)
      end = operator_end(source, source_length, cursor);
    if (!end) {
      ab_token_list_free(out);
      return archbird_error_set(engine, ARCHBIRD_INVALID_SCHEMA, cursor,
                                "source is not valid UTF-8");
    }
    {
      ArchbirdStatus status = token_append(engine, out, start, end, line, kind);
      if (status != ARCHBIRD_OK) {
        ab_token_list_free(out);
        return status;
      }
    }
    while (cursor < end) {
      if (source[cursor] == '\n') {
        line++;
        line_prefix = 1;
      }
      cursor++;
    }
  }
  return ARCHBIRD_OK;
}

void ab_token_list_free(AbTokenList *tokens) {
  ArchbirdEngine *engine;
  if (!tokens)
    return;
  engine = tokens->engine;
  ab_free(engine, tokens->items);
  memset(tokens, 0, sizeof(*tokens));
}
