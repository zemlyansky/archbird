#include "lexical/c/constants.h"

#include "render_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct IntegerBinding {
  AbString name;
  int64_t value;
} IntegerBinding;

typedef struct ExpressionParser {
  const AbTokenList *tokens;
  size_t index;
  size_t end;
  const IntegerBinding *bindings;
  size_t binding_count;
  int valid;
} ExpressionParser;

typedef struct TokenRange {
  size_t start;
  size_t end;
} TokenRange;

static AbString token_text(const AbTokenList *tokens, size_t index) {
  const AbToken *token = &tokens->items[index];
  return (AbString){(char *)tokens->source + token->start,
                    token->end - token->start};
}

static int token_identifier(const AbTokenList *tokens, size_t index) {
  return index < tokens->count &&
         tokens->items[index].kind == AB_TOKEN_IDENTIFIER;
}

static int string_equal(const AbString *left, const AbString *right) {
  return left->length == right->length &&
         (!left->length || memcmp(left->data, right->data, left->length) == 0);
}

static ArchbirdStatus pair_forward(ArchbirdEngine *engine,
                                   const AbTokenList *tokens,
                                   const char *opening, const char *closing,
                                   size_t **out) {
  size_t *pairs = NULL;
  size_t *stack = NULL;
  size_t stack_count = 0;
  size_t index;
  *out = NULL;
  if (!tokens->count)
    return ARCHBIRD_OK;
  if (tokens->count > SIZE_MAX / sizeof(*pairs))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "C token inventory is too large");
  pairs = (size_t *)ab_malloc(engine, tokens->count * sizeof(*pairs));
  stack = (size_t *)ab_malloc(engine, tokens->count * sizeof(*stack));
  if (!pairs || !stack) {
    ab_free(engine, pairs);
    ab_free(engine, stack);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory pairing C tokens");
  }
  for (index = 0; index < tokens->count; index++) {
    pairs[index] = SIZE_MAX;
    if (ab_token_equals(tokens, index, opening))
      stack[stack_count++] = index;
    else if (ab_token_equals(tokens, index, closing) && stack_count)
      pairs[stack[--stack_count]] = index;
  }
  ab_free(engine, stack);
  *out = pairs;
  return ARCHBIRD_OK;
}

static int binding_value(const ExpressionParser *parser, const AbString *name,
                         int64_t *out) {
  size_t index;
  for (index = 0; index < parser->binding_count; index++)
    if (string_equal(&parser->bindings[index].name, name)) {
      *out = parser->bindings[index].value;
      return 1;
    }
  return 0;
}

static int suffix_token(const AbString *value) {
  size_t index;
  if (!value->length)
    return 0;
  for (index = 0; index < value->length; index++) {
    char byte = value->data[index];
    if (byte != 'u' && byte != 'U' && byte != 'l' && byte != 'L')
      return 0;
  }
  return 1;
}

static int parse_number(const AbString *text, int64_t *out) {
  char local[128];
  char *end;
  unsigned long long value;
  if (!text->length || text->length >= sizeof(local))
    return 0;
  memcpy(local, text->data, text->length);
  local[text->length] = '\0';
  errno = 0;
  value = strtoull(local, &end, 0);
  if (errno || end != local + text->length || value > (uint64_t)INT64_MAX)
    return 0;
  *out = (int64_t)value;
  return 1;
}

static int parse_character(const AbString *text, int64_t *out) {
  const unsigned char *bytes = (const unsigned char *)text->data;
  if (text->length == 3 && bytes[0] == '\'' && bytes[2] == '\'') {
    *out = bytes[1];
    return 1;
  }
  if (text->length == 4 && bytes[0] == '\'' && bytes[1] == '\\' &&
      bytes[3] == '\'') {
    switch (bytes[2]) {
    case '0':
      *out = 0;
      return 1;
    case 'n':
      *out = '\n';
      return 1;
    case 'r':
      *out = '\r';
      return 1;
    case 't':
      *out = '\t';
      return 1;
    case '\\':
      *out = '\\';
      return 1;
    case '\'':
      *out = '\'';
      return 1;
    default:
      return 0;
    }
  }
  return 0;
}

static int safe_add(int64_t left, int64_t right, int64_t *out) {
  if ((right > 0 && left > INT64_MAX - right) ||
      (right < 0 && left < INT64_MIN - right))
    return 0;
  *out = left + right;
  return 1;
}

static int safe_sub(int64_t left, int64_t right, int64_t *out) {
  if ((right < 0 && left > INT64_MAX + right) ||
      (right > 0 && left < INT64_MIN + right))
    return 0;
  *out = left - right;
  return 1;
}

static int safe_mul(int64_t left, int64_t right, int64_t *out) {
  if (!left || !right) {
    *out = 0;
    return 1;
  }
  if ((left == INT64_MIN && right == -1) || (right == INT64_MIN && left == -1))
    return 0;
  if (left > 0) {
    if ((right > 0 && left > INT64_MAX / right) ||
        (right < 0 && right < INT64_MIN / left))
      return 0;
  } else if ((right > 0 && left < INT64_MIN / right) ||
             (right < 0 && left < INT64_MAX / right)) {
    return 0;
  }
  *out = left * right;
  return 1;
}

static int operator_precedence(const AbTokenList *tokens, size_t index) {
  if (ab_token_equals(tokens, index, "|"))
    return 1;
  if (ab_token_equals(tokens, index, "^"))
    return 2;
  if (ab_token_equals(tokens, index, "&"))
    return 3;
  if (ab_token_equals(tokens, index, "<<") ||
      ab_token_equals(tokens, index, ">>"))
    return 4;
  if (ab_token_equals(tokens, index, "+") ||
      ab_token_equals(tokens, index, "-"))
    return 5;
  if (ab_token_equals(tokens, index, "*") ||
      ab_token_equals(tokens, index, "/") ||
      ab_token_equals(tokens, index, "%"))
    return 6;
  return 0;
}

static int64_t parse_binary(ExpressionParser *parser, int minimum);

static int64_t arithmetic_shift_right(int64_t left, int64_t right) {
  uint64_t magnitude;
  uint64_t divisor = (uint64_t)1 << (unsigned)right;
  if (left >= 0)
    return left / (int64_t)divisor;
  magnitude = (uint64_t)(-(left + 1)) + 1;
  return -(int64_t)((magnitude + divisor - 1) / divisor);
}

static int64_t parse_primary(ExpressionParser *parser) {
  int64_t value = 0;
  AbString token;
  if (parser->index >= parser->end) {
    parser->valid = 0;
    return 0;
  }
  token = token_text(parser->tokens, parser->index);
  if (ab_token_equals(parser->tokens, parser->index, "(")) {
    parser->index++;
    value = parse_binary(parser, 1);
    if (!parser->valid || parser->index >= parser->end ||
        !ab_token_equals(parser->tokens, parser->index, ")")) {
      parser->valid = 0;
      return 0;
    }
    parser->index++;
    return value;
  }
  if (parser->tokens->items[parser->index].kind == AB_TOKEN_NUMBER) {
    parser->index++;
    if (!parse_number(&token, &value))
      parser->valid = 0;
    if (parser->index < parser->end &&
        parser->tokens->items[parser->index].kind == AB_TOKEN_IDENTIFIER) {
      AbString suffix = token_text(parser->tokens, parser->index);
      if (suffix_token(&suffix))
        parser->index++;
    }
    return value;
  }
  if (parser->tokens->items[parser->index].kind == AB_TOKEN_STRING) {
    parser->index++;
    if (!parse_character(&token, &value))
      parser->valid = 0;
    return value;
  }
  if (parser->tokens->items[parser->index].kind == AB_TOKEN_IDENTIFIER) {
    parser->index++;
    if (!binding_value(parser, &token, &value))
      parser->valid = 0;
    return value;
  }
  parser->valid = 0;
  return 0;
}

static int64_t parse_unary(ExpressionParser *parser) {
  int negate = 0;
  int invert = 0;
  int64_t value;
  while (parser->index < parser->end) {
    if (ab_token_equals(parser->tokens, parser->index, "+"))
      parser->index++;
    else if (ab_token_equals(parser->tokens, parser->index, "-")) {
      negate = !negate;
      parser->index++;
    } else if (ab_token_equals(parser->tokens, parser->index, "~")) {
      invert = !invert;
      parser->index++;
    } else {
      break;
    }
  }
  value = parse_primary(parser);
  if (!parser->valid)
    return 0;
  if (invert) {
    uint64_t bits = ~(uint64_t)value;
    value = (int64_t)bits;
  }
  if (negate) {
    if (value == INT64_MIN) {
      parser->valid = 0;
      return 0;
    }
    value = -value;
  }
  return value;
}

static int64_t parse_binary(ExpressionParser *parser, int minimum) {
  int64_t left = parse_unary(parser);
  while (parser->valid && parser->index < parser->end) {
    size_t operator_index = parser->index;
    int precedence = operator_precedence(parser->tokens, operator_index);
    int64_t right;
    int64_t result = 0;
    if (precedence < minimum)
      break;
    parser->index++;
    right = parse_binary(parser, precedence + 1);
    if (!parser->valid)
      return 0;
    if (ab_token_equals(parser->tokens, operator_index, "+"))
      parser->valid = safe_add(left, right, &result);
    else if (ab_token_equals(parser->tokens, operator_index, "-"))
      parser->valid = safe_sub(left, right, &result);
    else if (ab_token_equals(parser->tokens, operator_index, "*"))
      parser->valid = safe_mul(left, right, &result);
    else if (ab_token_equals(parser->tokens, operator_index, "/")) {
      parser->valid = right != 0 && !(left == INT64_MIN && right == -1);
      if (parser->valid)
        result = left / right;
    } else if (ab_token_equals(parser->tokens, operator_index, "%")) {
      parser->valid = right != 0 && !(left == INT64_MIN && right == -1);
      if (parser->valid)
        result = left % right;
    } else if (ab_token_equals(parser->tokens, operator_index, "<<")) {
      int64_t factor;
      parser->valid = right >= 0 && right < 63;
      factor = parser->valid ? (int64_t)((uint64_t)1 << (unsigned)right) : 0;
      if (parser->valid)
        parser->valid = safe_mul(left, factor, &result);
    } else if (ab_token_equals(parser->tokens, operator_index, ">>")) {
      parser->valid = right >= 0 && right < 63;
      if (parser->valid)
        result = arithmetic_shift_right(left, right);
    } else if (ab_token_equals(parser->tokens, operator_index, "|"))
      result = (int64_t)((uint64_t)left | (uint64_t)right);
    else if (ab_token_equals(parser->tokens, operator_index, "&"))
      result = (int64_t)((uint64_t)left & (uint64_t)right);
    else
      result = (int64_t)((uint64_t)left ^ (uint64_t)right);
    left = result;
  }
  return left;
}

static int evaluate_expression(const AbTokenList *tokens, size_t start,
                               size_t end, const IntegerBinding *bindings,
                               size_t binding_count, int64_t *out) {
  ExpressionParser parser = {tokens, start, end, bindings, binding_count, 1};
  int64_t value = parse_binary(&parser, 1);
  if (!parser.valid || parser.index != end)
    return 0;
  *out = value;
  return 1;
}

static size_t segment_end(const AbTokenList *tokens, size_t start, size_t end) {
  size_t depth = 0;
  size_t index;
  for (index = start; index < end; index++) {
    if (ab_token_equals(tokens, index, "(") ||
        ab_token_equals(tokens, index, "[") ||
        ab_token_equals(tokens, index, "{"))
      depth++;
    else if ((ab_token_equals(tokens, index, ")") ||
              ab_token_equals(tokens, index, "]") ||
              ab_token_equals(tokens, index, "}")) &&
             depth)
      depth--;
    else if (!depth && ab_token_equals(tokens, index, ","))
      return index;
  }
  return end;
}

static ArchbirdStatus binding_add(ArchbirdEngine *engine,
                                  IntegerBinding **bindings, size_t *count,
                                  const AbString *name, int64_t value) {
  IntegerBinding *resized;
  ArchbirdStatus status;
  if (*count == SIZE_MAX / sizeof(**bindings))
    return ARCHBIRD_LIMIT_EXCEEDED;
  resized = (IntegerBinding *)ab_realloc(engine, *bindings,
                                         (*count + 1) * sizeof(**bindings));
  if (!resized)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory storing enum bindings");
  *bindings = resized;
  memset(&(*bindings)[*count], 0, sizeof(**bindings));
  status = ab_string_copy(engine, &(*bindings)[*count].name, name->data,
                          name->length);
  if (status == ARCHBIRD_OK) {
    (*bindings)[*count].value = value;
    (*count)++;
  }
  return status;
}

static void bindings_free(ArchbirdEngine *engine, IntegerBinding *bindings,
                          size_t count) {
  size_t index;
  for (index = 0; index < count; index++)
    ab_string_free(engine, &bindings[index].name);
  ab_free(engine, bindings);
}

static ArchbirdStatus token_range_text(ArchbirdEngine *engine,
                                       const AbTokenList *tokens, size_t start,
                                       size_t end, AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  for (index = start; status == ARCHBIRD_OK && index < end; index++) {
    AbString token = token_text(tokens, index);
    status = ab_buffer_append(&buffer, token.data, token.length);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus
add_constant_value(AbBundleBuilder *builder, const AbTokenList *tokens,
                   const char *kind, const AbString *container,
                   size_t name_index, int known, int64_t value,
                   size_t expression_start, size_t expression_end) {
  AbBuffer key;
  AbFact *fact = NULL;
  AbString name = token_text(tokens, name_index);
  AbString expression = {0};
  const AbToken *token = &tokens->items[name_index];
  ArchbirdStatus status;
  ab_buffer_init(&key, builder->engine);
  status = ab_buffer_u64(&key, container->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, container->data, container->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, name.data, name.length);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact(
        builder, "constant-values", kind, "lexical-occurrence", token->start,
        token->end, key.data, key.length, (const uint8_t *)name.data,
        name.length, &fact);
  if (status == ARCHBIRD_OK)
    ab_fact_set_keyed_correlation(fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(builder->engine, fact, "container",
                                          (const uint8_t *)container->data,
                                          container->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "line", token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        builder->engine, fact, "state",
        (const uint8_t *)(known ? "current" : "unknown"), known ? 7u : 7u);
  if (status == ARCHBIRD_OK && known)
    status = ab_fact_add_i64_attribute(builder->engine, fact, "value", value);
  if (status == ARCHBIRD_OK && !known) {
    status = token_range_text(builder->engine, tokens, expression_start,
                              expression_end, &expression);
    if (status == ARCHBIRD_OK)
      status = ab_fact_add_string_attribute(builder->engine, fact, "expression",
                                            (const uint8_t *)expression.data,
                                            expression.length);
  }
  ab_string_free(builder->engine, &expression);
  ab_buffer_free(&key);
  return status;
}

static AbString enum_container(const AbTokenList *tokens, size_t enum_index,
                               size_t opening, size_t closing) {
  size_t index;
  for (index = enum_index + 1; index < opening; index++)
    if (token_identifier(tokens, index))
      return token_text(tokens, index);
  for (index = closing + 1;
       index < tokens->count && !ab_token_equals(tokens, index, ";"); index++)
    if (token_identifier(tokens, index))
      return token_text(tokens, index);
  return (AbString){(char *)"<anonymous>", 11};
}

static ArchbirdStatus scan_enums(AbBundleBuilder *builder,
                                 const AbTokenList *tokens,
                                 const size_t *braces) {
  size_t enum_index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (enum_index = 0; status == ARCHBIRD_OK && enum_index < tokens->count;
       enum_index++) {
    size_t opening = SIZE_MAX;
    size_t closing;
    size_t index;
    IntegerBinding *bindings = NULL;
    size_t binding_count = 0;
    int64_t next_value = 0;
    int next_known = 1;
    AbString container;
    if (!ab_token_equals(tokens, enum_index, "enum"))
      continue;
    for (index = enum_index + 1;
         index < tokens->count && index < enum_index + 8; index++)
      if (ab_token_equals(tokens, index, "{")) {
        opening = index;
        break;
      }
    if (opening == SIZE_MAX || !braces || braces[opening] == SIZE_MAX)
      continue;
    closing = braces[opening];
    container = enum_container(tokens, enum_index, opening, closing);
    index = opening + 1;
    while (status == ARCHBIRD_OK && index < closing) {
      size_t end = segment_end(tokens, index, closing);
      if (end > index && token_identifier(tokens, index)) {
        AbString name = token_text(tokens, index);
        size_t expression_start =
            index + 1 < end && ab_token_equals(tokens, index + 1, "=")
                ? index + 2
                : end;
        int64_t value = 0;
        int known = expression_start < end
                        ? evaluate_expression(tokens, expression_start, end,
                                              bindings, binding_count, &value)
                        : next_known;
        if (expression_start == end && known)
          value = next_value;
        status = add_constant_value(builder, tokens, "enum-member", &container,
                                    index, known, value, expression_start, end);
        if (status == ARCHBIRD_OK && known)
          status = binding_add(builder->engine, &bindings, &binding_count,
                               &name, value);
        if (known && value != INT64_MAX) {
          next_value = value + 1;
          next_known = 1;
        } else {
          next_known = 0;
        }
      }
      index = end < closing ? end + 1 : closing;
    }
    bindings_free(builder->engine, bindings, binding_count);
    enum_index = closing;
  }
  return status;
}

static size_t declaration_start(const AbTokenList *tokens, size_t equal) {
  size_t index = equal;
  while (index && !ab_token_equals(tokens, index - 1, ";") &&
         !ab_token_equals(tokens, index - 1, "}"))
    index--;
  return index;
}

static size_t initializer_name(const AbTokenList *tokens, size_t equal) {
  size_t start = declaration_start(tokens, equal);
  size_t index;
  size_t brackets = 0;
  size_t candidate = SIZE_MAX;
  for (index = start; index < equal; index++) {
    if (ab_token_equals(tokens, index, "["))
      brackets++;
    else if (ab_token_equals(tokens, index, "]") && brackets)
      brackets--;
    else if (!brackets && token_identifier(tokens, index))
      candidate = index;
  }
  return candidate;
}

static ArchbirdStatus scan_initializers(AbBundleBuilder *builder,
                                        const AbTokenList *tokens,
                                        const size_t *braces) {
  size_t equal;
  size_t brace_depth = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (equal = 0; status == ARCHBIRD_OK && equal + 1 < tokens->count; equal++) {
    size_t opening;
    size_t closing;
    size_t container_index;
    size_t index;
    AbString container;
    if (ab_token_equals(tokens, equal, "}")) {
      if (brace_depth)
        brace_depth--;
      continue;
    }
    if (ab_token_equals(tokens, equal, "{")) {
      brace_depth++;
      continue;
    }
    if (brace_depth || !ab_token_equals(tokens, equal, "=") ||
        !ab_token_equals(tokens, equal + 1, "{") || !braces ||
        braces[equal + 1] == SIZE_MAX)
      continue;
    opening = equal + 1;
    closing = braces[opening];
    container_index = initializer_name(tokens, equal);
    if (container_index == SIZE_MAX)
      continue;
    container = token_text(tokens, container_index);
    index = opening + 1;
    while (status == ARCHBIRD_OK && index < closing) {
      size_t end = segment_end(tokens, index, closing);
      if (end >= index + 5 && ab_token_equals(tokens, index, "[") &&
          token_identifier(tokens, index + 1) &&
          ab_token_equals(tokens, index + 2, "]") &&
          ab_token_equals(tokens, index + 3, "=")) {
        int64_t value = 0;
        int known =
            evaluate_expression(tokens, index + 4, end, NULL, 0, &value);
        status = add_constant_value(builder, tokens, "designated-initializer",
                                    &container, index + 1, known, value,
                                    index + 4, end);
      }
      index = end < closing ? end + 1 : closing;
    }
    equal = closing;
  }
  return status;
}

static int c_identifier_start(uint8_t byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         byte == '_';
}

static int c_identifier_continue(uint8_t byte) {
  return c_identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static int horizontal_space(uint8_t byte) {
  return byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f';
}

static int macro_name_compare(const void *left_raw, const void *right_raw) {
  return ab_string_compare((const AbString *)left_raw,
                           (const AbString *)right_raw);
}

static ArchbirdStatus collect_local_function_macros(ArchbirdEngine *engine,
                                                    const AbTokenList *tokens,
                                                    AbString **out,
                                                    size_t *out_count) {
  const uint8_t *source = tokens->source;
  size_t source_length = tokens->source_length;
  AbString *names = NULL;
  size_t count = 0;
  size_t capacity = 0;
  size_t line = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  *out = NULL;
  *out_count = 0;
  while (line < source_length) {
    size_t end = line;
    size_t cursor;
    size_t name_start;
    while (end < source_length && source[end] != '\r' && source[end] != '\n')
      end++;
    cursor = line;
    while (cursor < end && horizontal_space(source[cursor]))
      cursor++;
    if (cursor >= end || source[cursor++] != '#')
      goto next_line;
    while (cursor < end && horizontal_space(source[cursor]))
      cursor++;
    if (end - cursor < 6 || memcmp(source + cursor, "define", 6) != 0 ||
        (cursor + 6 < end && c_identifier_continue(source[cursor + 6])))
      goto next_line;
    cursor += 6;
    if (cursor >= end || !horizontal_space(source[cursor]))
      goto next_line;
    while (cursor < end && horizontal_space(source[cursor]))
      cursor++;
    if (cursor >= end || !c_identifier_start(source[cursor]))
      goto next_line;
    name_start = cursor++;
    while (cursor < end && c_identifier_continue(source[cursor]))
      cursor++;
    if (cursor >= end || source[cursor] != '(')
      goto next_line;
    if (count == capacity) {
      size_t grown = capacity ? capacity * 2 : 8;
      AbString *resized;
      if (grown > SIZE_MAX / sizeof(*resized)) {
        status = archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                    ARCHBIRD_NO_OFFSET,
                                    "local macro inventory is too large");
        break;
      }
      resized = (AbString *)ab_realloc(engine, names, grown * sizeof(*resized));
      if (!resized) {
        status = archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                    ARCHBIRD_NO_OFFSET,
                                    "out of memory indexing local macros");
        break;
      }
      names = resized;
      capacity = grown;
    }
    names[count++] =
        (AbString){(char *)source + name_start, cursor - name_start};
  next_line:
    if (end < source_length && source[end] == '\r')
      end++;
    if (end < source_length && source[end] == '\n')
      end++;
    line = end;
  }
  if (status == ARCHBIRD_OK && count > 1)
    qsort(names, count, sizeof(*names), macro_name_compare);
  if (status != ARCHBIRD_OK) {
    ab_free(engine, names);
    return status;
  }
  *out = names;
  *out_count = count;
  return ARCHBIRD_OK;
}

static int local_macro_has(const AbString *names, size_t count,
                           const AbString *name) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compared = ab_string_compare(name, &names[middle]);
    if (!compared)
      return 1;
    if (compared < 0)
      high = middle;
    else
      low = middle + 1;
  }
  return 0;
}

static ArchbirdStatus split_arguments(ArchbirdEngine *engine,
                                      const AbTokenList *tokens, size_t start,
                                      size_t end, TokenRange **out,
                                      size_t *out_count) {
  TokenRange *ranges = NULL;
  size_t count = 0;
  size_t cursor = start;
  *out = NULL;
  *out_count = 0;
  while (cursor < end) {
    size_t finish = segment_end(tokens, cursor, end);
    if (finish == cursor) {
      cursor++;
      continue;
    }
    TokenRange *resized =
        (TokenRange *)ab_realloc(engine, ranges, (count + 1) * sizeof(*ranges));
    if (!resized) {
      ab_free(engine, ranges);
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory splitting macro arguments");
    }
    ranges = resized;
    ranges[count++] = (TokenRange){cursor, finish};
    cursor = finish < end ? finish + 1 : end;
  }
  *out = ranges;
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_macro_argument(AbBundleBuilder *builder,
                                         const AbTokenList *tokens,
                                         const AbString *call,
                                         size_t invocation, size_t ordinal,
                                         TokenRange range) {
  AbBuffer key;
  AbFact *fact = NULL;
  AbString text = {0};
  size_t index;
  size_t identifier_count = 0;
  size_t identifier_index = SIZE_MAX;
  const AbToken *call_token = &tokens->items[invocation];
  ArchbirdStatus status =
      token_range_text(builder->engine, tokens, range.start, range.end, &text);
  for (index = range.start; index < range.end; index++)
    if (token_identifier(tokens, index)) {
      identifier_count++;
      identifier_index = index;
    }
  ab_buffer_init(&key, builder->engine);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, call_token->start);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&key, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_u64(&key, ordinal);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_fact(
        builder, "macro-invocations", "argument", "lexical-occurrence",
        tokens->items[range.start].start, tokens->items[range.end - 1].end,
        key.data, key.length, (const uint8_t *)text.data, text.length, &fact);
  if (status == ARCHBIRD_OK)
    ab_fact_set_keyed_correlation(fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(builder->engine, fact, "call",
                                     (const uint8_t *)call->data, call->length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(builder->engine, fact, "invocation",
                                       call_token->start);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "argument", ordinal);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(builder->engine, fact, "line",
                                       call_token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        builder->engine, fact, "text", (const uint8_t *)text.data, text.length);
  if (status == ARCHBIRD_OK && identifier_count == 1) {
    AbString identifier = token_text(tokens, identifier_index);
    status = ab_fact_add_string_attribute(builder->engine, fact, "identifier",
                                          (const uint8_t *)identifier.data,
                                          identifier.length);
  }
  ab_buffer_free(&key);
  ab_string_free(builder->engine, &text);
  return status;
}

static ArchbirdStatus scan_macro_invocations(AbBundleBuilder *builder,
                                             const AbTokenList *tokens,
                                             const size_t *parentheses,
                                             const AbString *local_macros,
                                             size_t local_macro_count) {
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  for (index = 0; status == ARCHBIRD_OK && index + 1 < tokens->count; index++) {
    AbString call;
    size_t closing;
    TokenRange *arguments = NULL;
    size_t argument_count = 0;
    size_t argument;
    if (!token_identifier(tokens, index) ||
        !ab_token_equals(tokens, index + 1, "("))
      continue;
    call = token_text(tokens, index);
    if (!local_macro_has(local_macros, local_macro_count, &call) ||
        !parentheses || parentheses[index + 1] == SIZE_MAX)
      continue;
    closing = parentheses[index + 1];
    status = split_arguments(builder->engine, tokens, index + 2, closing,
                             &arguments, &argument_count);
    for (argument = 0; status == ARCHBIRD_OK && argument < argument_count;
         argument++)
      status = add_macro_argument(builder, tokens, &call, index, argument,
                                  arguments[argument]);
    ab_free(builder->engine, arguments);
  }
  return status;
}

ArchbirdStatus ab_c_scan_constant_facts(AbBundleBuilder *builder,
                                        const AbTokenList *tokens) {
  size_t *braces = NULL;
  size_t *parentheses = NULL;
  AbString *local_macros = NULL;
  size_t local_macro_count = 0;
  ArchbirdStatus status;
  if (!builder || !builder->engine || !tokens)
    return ARCHBIRD_INVALID_ARGUMENT;
  status = pair_forward(builder->engine, tokens, "{", "}", &braces);
  if (status == ARCHBIRD_OK)
    status = pair_forward(builder->engine, tokens, "(", ")", &parentheses);
  if (status == ARCHBIRD_OK)
    status = collect_local_function_macros(builder->engine, tokens,
                                           &local_macros, &local_macro_count);
  if (status == ARCHBIRD_OK)
    status = scan_enums(builder, tokens, braces);
  if (status == ARCHBIRD_OK)
    status = scan_initializers(builder, tokens, braces);
  if (status == ARCHBIRD_OK)
    status = scan_macro_invocations(builder, tokens, parentheses, local_macros,
                                    local_macro_count);
  ab_free(builder->engine, local_macros);
  ab_free(builder->engine, parentheses);
  ab_free(builder->engine, braces);
  return status;
}
