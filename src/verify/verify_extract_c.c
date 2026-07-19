#include "verify_runtime.h"

#include "lexical/tokenizer.h"
#include "sha256.h"

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

typedef struct SourceContext {
  const AbValue *spec;
  const AbValue *project_name;
  const AbValue *path;
  const AbString *text;
  char sha256[65];
} SourceContext;

static AbString token_string(const AbTokenList *tokens, size_t index) {
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

static ArchbirdStatus source_context(AbVerificationContext *context,
                                     const AbObjectField *extractor,
                                     SourceContext *out) {
  const AbValue *project;
  const AbValue *source;
  const AbValue *text;
  uint8_t digest[32];
  ArchbirdStatus status;
  memset(out, 0, sizeof(*out));
  out->spec = &extractor->value;
  out->project_name = ab_value_member(out->spec, "project");
  out->path = ab_value_member(out->spec, "path");
  project =
      ab_verify_input_project(&context->input, &out->project_name->as.text);
  source =
      project ? ab_verify_input_source(project, &out->path->as.text) : NULL;
  text = source ? ab_value_member(source, "text") : NULL;
  if (!text || text->kind != AB_VALUE_STRING)
    return archbird_error_set(
        context->engine, ARCHBIRD_INVALID_SCHEMA, ARCHBIRD_NO_OFFSET,
        "verification input is missing source %.*s:%.*s",
        (int)out->project_name->as.text.length, out->project_name->as.text.data,
        (int)out->path->as.text.length, out->path->as.text.data);
  out->text = &text->as.text;
  status = archbird_sha256((const uint8_t *)out->text->data, out->text->length,
                           digest);
  if (status == ARCHBIRD_OK)
    archbird_sha256_hex(digest, out->sha256);
  return status;
}

static ArchbirdStatus copy_literal(ArchbirdEngine *engine, AbString *out,
                                   const char *literal) {
  return ab_string_copy(engine, out, literal, strlen(literal));
}

static ArchbirdStatus evidence_for(AbVerificationContext *context,
                                   const SourceContext *source, uint64_t line,
                                   const char *detail, size_t detail_length,
                                   AbVerifyEvidence *out) {
  return ab_verify_evidence_init(
      context->engine, out, "derived", &source->project_name->as.text,
      &source->path->as.text, line, source->sha256, detail, detail_length);
}

static ArchbirdStatus pair_tokens(ArchbirdEngine *engine,
                                  const AbTokenList *tokens,
                                  const char *opening, const char *closing,
                                  size_t **out) {
  size_t *pairs;
  size_t *stack;
  size_t stack_count = 0;
  size_t index;
  *out = NULL;
  if (!tokens->count)
    return ARCHBIRD_OK;
  if (tokens->count > SIZE_MAX / sizeof(*pairs))
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "verification token inventory is too large");
  pairs = (size_t *)ab_malloc(engine, tokens->count * sizeof(*pairs));
  stack = (size_t *)ab_malloc(engine, tokens->count * sizeof(*stack));
  if (!pairs || !stack) {
    ab_free(engine, pairs);
    ab_free(engine, stack);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory pairing verification tokens");
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

static int safe_shift_left(int64_t left, int64_t right, int64_t *out) {
  int64_t factor;
  if (right < 0 || right >= 63)
    return 0;
  factor = (int64_t)((uint64_t)1 << (unsigned)right);
  return safe_mul(left, factor, out);
}

static int64_t arithmetic_shift_right(int64_t left, int64_t right) {
  uint64_t magnitude;
  uint64_t divisor = (uint64_t)1 << (unsigned)right;
  if (left >= 0)
    return left / (int64_t)divisor;
  magnitude = (uint64_t)(-(left + 1)) + 1;
  return -(int64_t)((magnitude + divisor - 1) / divisor);
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

static int64_t parse_primary(ExpressionParser *parser) {
  int64_t value = 0;
  AbString token;
  if (parser->index >= parser->end) {
    parser->valid = 0;
    return 0;
  }
  token = token_string(parser->tokens, parser->index);
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
      AbString suffix = token_string(parser->tokens, parser->index);
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
    if (ab_token_equals(parser->tokens, parser->index, "+")) {
      parser->index++;
    } else if (ab_token_equals(parser->tokens, parser->index, "-")) {
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
  if (invert)
    value = (int64_t) ~(uint64_t)value;
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
    } else if (ab_token_equals(parser->tokens, operator_index, "<<"))
      parser->valid = safe_shift_left(left, right, &result);
    else if (ab_token_equals(parser->tokens, operator_index, ">>")) {
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

static ArchbirdStatus expression_text(ArchbirdEngine *engine,
                                      const AbTokenList *tokens, size_t start,
                                      size_t end, AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  for (index = start; status == ARCHBIRD_OK && index < end; index++) {
    AbString token = token_string(tokens, index);
    status = ab_buffer_append(&buffer, token.data, token.length);
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus integer_value(ArchbirdEngine *engine, int64_t value,
                                    AbValue *out) {
  char text[32];
  int length = snprintf(text, sizeof(text), "%" PRId64, value);
  if (length < 0 || (size_t)length >= sizeof(text))
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "failed to render verification integer");
  out->kind = AB_VALUE_INTEGER;
  return ab_string_copy(engine, &out->as.text, text, (size_t)length);
}

static size_t segment_end(const AbTokenList *tokens, size_t start, size_t end) {
  int depth = 0;
  size_t index;
  for (index = start; index < end; index++) {
    if (ab_token_equals(tokens, index, "(") ||
        ab_token_equals(tokens, index, "[") ||
        ab_token_equals(tokens, index, "{"))
      depth++;
    else if (ab_token_equals(tokens, index, ")") ||
             ab_token_equals(tokens, index, "]") ||
             ab_token_equals(tokens, index, "}"))
      depth--;
    else if (depth == 0 && ab_token_equals(tokens, index, ","))
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
    return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "too many enum value bindings");
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

static ArchbirdStatus
add_source_item(AbVerificationContext *context, AbVerifyFactSet *fact,
                const SourceContext *source, const AbString *raw_name,
                uint64_t line, const AbValue *value, const char *state,
                const char *message, const char *detail, size_t detail_length) {
  AbString normalized = {0};
  int selected = 0;
  AbVerifyFactItem item = {0};
  AbVerifyEvidence evidence = {0};
  ArchbirdStatus status = ab_verify_normalized_name(
      context->engine, source->spec, raw_name, &normalized, &selected);
  if (status != ARCHBIRD_OK || !selected) {
    ab_string_free(context->engine, &normalized);
    return status;
  }
  status = ab_verify_item_init(context->engine, &item, &normalized, &normalized,
                               value);
  if (status == ARCHBIRD_OK && strcmp(state, "current") != 0)
    status = ab_verify_item_set_state(context->engine, &item, state, message);
  if (status == ARCHBIRD_OK)
    status =
        evidence_for(context, source, line, detail, detail_length, &evidence);
  if (status == ARCHBIRD_OK)
    status = ab_verify_item_add_evidence(context->engine, &item, &evidence);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_add_item(context->engine, fact, &item);
  ab_verify_evidence_free(context->engine, &evidence);
  ab_string_free(context->engine, &normalized);
  if (status != ARCHBIRD_OK)
    ab_verify_fact_item_free(context->engine, &item);
  return status;
}

static ArchbirdStatus unknown_for(AbVerificationContext *context,
                                  const AbObjectField *extractor,
                                  AbVerifyFactSet *fact, const char *shape,
                                  const char *format, const AbString *name,
                                  size_t count) {
  char message[512];
  (void)snprintf(message, sizeof(message), format, (int)name->length,
                 name->data, count);
  return ab_verify_fact_unknown(
      context->engine, fact, &extractor->name,
      &ab_value_member(&extractor->value, "project")->as.text, shape, message);
}

static ArchbirdStatus extract_enum(AbVerificationContext *context,
                                   const AbObjectField *extractor,
                                   AbVerifyFactSet *fact) {
  SourceContext source;
  AbTokenList tokens = {0};
  size_t *braces = NULL;
  const AbValue *wanted = ab_value_member(&extractor->value, "name");
  size_t candidate_open = SIZE_MAX;
  size_t candidate_close = SIZE_MAX;
  size_t candidates = 0;
  size_t index;
  IntegerBinding *bindings = NULL;
  size_t binding_count = 0;
  int64_t next_value = 0;
  int next_known = 1;
  ArchbirdStatus status = source_context(context, extractor, &source);
  if (status == ARCHBIRD_OK)
    status = ab_tokenize(context->engine, (const uint8_t *)source.text->data,
                         source.text->length, AB_LEX_C_PREPROCESSOR, &tokens);
  if (status == ARCHBIRD_OK)
    status = pair_tokens(context->engine, &tokens, "{", "}", &braces);
  for (index = 0; status == ARCHBIRD_OK && index < tokens.count; index++) {
    size_t opening = SIZE_MAX;
    size_t position;
    int matched = 0;
    if (!ab_token_equals(&tokens, index, "enum"))
      continue;
    for (position = index + 1; position < tokens.count && position < index + 6;
         position++)
      if (ab_token_equals(&tokens, position, "{")) {
        opening = position;
        break;
      }
    if (opening == SIZE_MAX || !braces || braces[opening] == SIZE_MAX)
      continue;
    for (position = index + 1; position < opening; position++) {
      AbString candidate = token_string(&tokens, position);
      if (token_identifier(&tokens, position) &&
          string_equal(&candidate, &wanted->as.text))
        matched = 1;
    }
    for (position = braces[opening] + 1;
         position < tokens.count && !ab_token_equals(&tokens, position, ";");
         position++) {
      AbString candidate = token_string(&tokens, position);
      if (token_identifier(&tokens, position) &&
          string_equal(&candidate, &wanted->as.text))
        matched = 1;
    }
    if (matched) {
      candidates++;
      candidate_open = opening;
      candidate_close = braces[opening];
    }
  }
  if (status == ARCHBIRD_OK && candidates != 1) {
    status = unknown_for(context, extractor, fact, "values",
                         "expected one C enum '%.*s', found %zu",
                         &wanted->as.text, candidates);
    goto done;
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_verify_fact_init(context->engine, fact, &extractor->name, "values",
                            "derived", &source.project_name->as.text);
  index = candidate_open + 1;
  while (status == ARCHBIRD_OK && index < candidate_close) {
    size_t end = segment_end(&tokens, index, candidate_close);
    if (end > index && token_identifier(&tokens, index)) {
      AbString raw_name = token_string(&tokens, index);
      size_t expression_start =
          index + 1 < end && ab_token_equals(&tokens, index + 1, "=")
              ? index + 2
              : end;
      int64_t value = 0;
      int known = expression_start < end
                      ? evaluate_expression(&tokens, expression_start, end,
                                            bindings, binding_count, &value)
                      : next_known;
      AbValue item_value = {0};
      AbString expression = {0};
      const char *state = "current";
      const char *message = "";
      if (expression_start == end && known)
        value = next_value;
      if (known) {
        status = integer_value(context->engine, value, &item_value);
        if (status == ARCHBIRD_OK)
          status = binding_add(context->engine, &bindings, &binding_count,
                               &raw_name, value);
        if (value == INT64_MAX)
          next_known = 0;
        else {
          next_value = value + 1;
          next_known = 1;
        }
      } else {
        status = expression_start < end
                     ? expression_text(context->engine, &tokens,
                                       expression_start, end, &expression)
                     : copy_literal(context->engine, &expression, "<implicit>");
        item_value.kind = AB_VALUE_STRING;
        if (status == ARCHBIRD_OK) {
          item_value.as.text = expression;
          memset(&expression, 0, sizeof(expression));
        }
        state = "unknown";
        message = "unsupported enum expression";
        next_known = 0;
      }
      if (status == ARCHBIRD_OK)
        status = add_source_item(context, fact, &source, &raw_name,
                                 tokens.items[index].line, &item_value, state,
                                 message, raw_name.data, raw_name.length);
      ab_value_free(context->engine, &item_value);
      ab_string_free(context->engine, &expression);
    }
    index = end < candidate_close ? end + 1 : candidate_close;
  }
  if (status == ARCHBIRD_OK && !fact->item_count) {
    ab_verify_fact_free(context->engine, fact);
    status = unknown_for(context, extractor, fact, "values",
                         "C enum '%.*s' has no selected members%zu",
                         &wanted->as.text, 0);
  } else if (status == ARCHBIRD_OK) {
    status = ab_verify_fact_finish(context->engine, fact);
  }
done:
  if (status != ARCHBIRD_OK && fact->name.data)
    ab_verify_fact_free(context->engine, fact);
  bindings_free(context->engine, bindings, binding_count);
  ab_free(context->engine, braces);
  ab_token_list_free(&tokens);
  return status;
}

static size_t initializer_candidates(const AbTokenList *tokens,
                                     const size_t *braces, const AbString *name,
                                     size_t *out_open, size_t *out_close) {
  size_t count = 0;
  size_t index;
  for (index = 0; index < tokens->count; index++) {
    size_t position;
    int seen_equal = 0;
    AbString candidate = token_string(tokens, index);
    if (!token_identifier(tokens, index) || !string_equal(&candidate, name))
      continue;
    for (position = index + 1;
         position < tokens->count && position < index + 80; position++) {
      if (ab_token_equals(tokens, position, ";"))
        break;
      if (ab_token_equals(tokens, position, "="))
        seen_equal = 1;
      else if (seen_equal && ab_token_equals(tokens, position, "{") && braces &&
               braces[position] != SIZE_MAX) {
        *out_open = position;
        *out_close = braces[position];
        count++;
        break;
      }
    }
  }
  return count;
}

static ArchbirdStatus extract_initializer(AbVerificationContext *context,
                                          const AbObjectField *extractor,
                                          AbVerifyFactSet *fact) {
  SourceContext source;
  AbTokenList tokens = {0};
  size_t *braces = NULL;
  const AbValue *wanted = ab_value_member(&extractor->value, "name");
  size_t opening = SIZE_MAX;
  size_t closing = SIZE_MAX;
  size_t candidates = 0;
  size_t index;
  ArchbirdStatus status = source_context(context, extractor, &source);
  if (status == ARCHBIRD_OK)
    status = ab_tokenize(context->engine, (const uint8_t *)source.text->data,
                         source.text->length, AB_LEX_C_PREPROCESSOR, &tokens);
  if (status == ARCHBIRD_OK)
    status = pair_tokens(context->engine, &tokens, "{", "}", &braces);
  if (status == ARCHBIRD_OK)
    candidates = initializer_candidates(&tokens, braces, &wanted->as.text,
                                        &opening, &closing);
  if (status == ARCHBIRD_OK && candidates != 1) {
    status = unknown_for(context, extractor, fact, "values",
                         "expected one initializer '%.*s', found %zu",
                         &wanted->as.text, candidates);
    goto done;
  }
  if (status == ARCHBIRD_OK)
    status =
        ab_verify_fact_init(context->engine, fact, &extractor->name, "values",
                            "derived", &source.project_name->as.text);
  index = opening + 1;
  while (status == ARCHBIRD_OK && index < closing) {
    size_t end = segment_end(&tokens, index, closing);
    if (end >= index + 5 && ab_token_equals(&tokens, index, "[") &&
        token_identifier(&tokens, index + 1) &&
        ab_token_equals(&tokens, index + 2, "]") &&
        ab_token_equals(&tokens, index + 3, "=")) {
      AbString raw_name = token_string(&tokens, index + 1);
      int64_t value;
      int known = evaluate_expression(&tokens, index + 4, end, NULL, 0, &value);
      AbValue item_value = {0};
      AbString expression = {0};
      const char *state = "current";
      const char *message = "";
      if (known)
        status = integer_value(context->engine, value, &item_value);
      else {
        status = expression_text(context->engine, &tokens, index + 4, end,
                                 &expression);
        item_value.kind = AB_VALUE_STRING;
        if (status == ARCHBIRD_OK) {
          item_value.as.text = expression;
          memset(&expression, 0, sizeof(expression));
        }
        state = "unknown";
        message = "unresolved initializer expression";
      }
      if (status == ARCHBIRD_OK)
        status = add_source_item(context, fact, &source, &raw_name,
                                 tokens.items[index].line, &item_value, state,
                                 message, raw_name.data, raw_name.length);
      ab_value_free(context->engine, &item_value);
      ab_string_free(context->engine, &expression);
    }
    index = end < closing ? end + 1 : closing;
  }
  if (status == ARCHBIRD_OK && !fact->item_count) {
    ab_verify_fact_free(context->engine, fact);
    status = unknown_for(context, extractor, fact, "values",
                         "initializer '%.*s' has no selected designators%zu",
                         &wanted->as.text, 0);
  } else if (status == ARCHBIRD_OK) {
    status = ab_verify_fact_finish(context->engine, fact);
  }
done:
  if (status != ARCHBIRD_OK && fact->name.data)
    ab_verify_fact_free(context->engine, fact);
  ab_free(context->engine, braces);
  ab_token_list_free(&tokens);
  return status;
}

typedef struct TokenRange {
  size_t start;
  size_t end;
} TokenRange;

static ArchbirdStatus split_ranges(ArchbirdEngine *engine,
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
    if (finish > cursor) {
      TokenRange *resized = (TokenRange *)ab_realloc(
          engine, ranges, (count + 1) * sizeof(*ranges));
      if (!resized) {
        ab_free(engine, ranges);
        return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                  ARCHBIRD_NO_OFFSET,
                                  "out of memory splitting macro arguments");
      }
      ranges = resized;
      ranges[count++] = (TokenRange){cursor, finish};
    }
    cursor = finish < end ? finish + 1 : end;
  }
  *out = ranges;
  *out_count = count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus macro_detail(ArchbirdEngine *engine, const AbString *call,
                                   const AbString *selector,
                                   const AbString *raw, AbString *out) {
  AbBuffer buffer;
  ArchbirdStatus status;
  ab_buffer_init(&buffer, engine);
  status = ab_buffer_append(&buffer, call->data, call->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "(");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, selector->data, selector->length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&buffer, "): ");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&buffer, raw->data, raw->length);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

static ArchbirdStatus extract_macro_set(AbVerificationContext *context,
                                        const AbObjectField *extractor,
                                        AbVerifyFactSet *fact) {
  SourceContext source;
  AbTokenList tokens = {0};
  size_t *parentheses = NULL;
  const AbValue *call = ab_value_member(&extractor->value, "call");
  const AbValue *wanted = ab_value_member(&extractor->value, "selector");
  const AbValue *selector_index_value =
      ab_value_member(&extractor->value, "selector_argument");
  const AbValue *value_index_value =
      ab_value_member(&extractor->value, "values_from_argument");
  uint64_t selector_index = 0;
  uint64_t value_index = 1;
  size_t matched = 0;
  size_t index;
  ArchbirdStatus status = source_context(context, extractor, &source);
  if (selector_index_value)
    (void)ab_value_u64(selector_index_value, &selector_index);
  if (value_index_value)
    (void)ab_value_u64(value_index_value, &value_index);
  if (status == ARCHBIRD_OK)
    status = ab_tokenize(context->engine, (const uint8_t *)source.text->data,
                         source.text->length, 0, &tokens);
  if (status == ARCHBIRD_OK)
    status = pair_tokens(context->engine, &tokens, "(", ")", &parentheses);
  if (status == ARCHBIRD_OK)
    status = ab_verify_fact_init(context->engine, fact, &extractor->name, "set",
                                 "derived", &source.project_name->as.text);
  for (index = 0; status == ARCHBIRD_OK && index + 1 < tokens.count; index++) {
    TokenRange *arguments = NULL;
    size_t argument_count = 0;
    AbString selector_text = {0};
    size_t argument_index;
    AbString token = token_string(&tokens, index);
    if (!token_identifier(&tokens, index) ||
        !string_equal(&token, &call->as.text) ||
        !ab_token_equals(&tokens, index + 1, "(") || !parentheses ||
        parentheses[index + 1] == SIZE_MAX)
      continue;
    status = split_ranges(context->engine, &tokens, index + 2,
                          parentheses[index + 1], &arguments, &argument_count);
    if (status != ARCHBIRD_OK)
      break;
    if (argument_count <= selector_index || argument_count <= value_index) {
      ab_free(context->engine, arguments);
      continue;
    }
    status = expression_text(context->engine, &tokens,
                             arguments[selector_index].start,
                             arguments[selector_index].end, &selector_text);
    if (status != ARCHBIRD_OK ||
        !string_equal(&selector_text, &wanted->as.text)) {
      ab_string_free(context->engine, &selector_text);
      ab_free(context->engine, arguments);
      continue;
    }
    matched++;
    for (argument_index = (size_t)value_index;
         status == ARCHBIRD_OK && argument_index < argument_count;
         argument_index++) {
      TokenRange range = arguments[argument_index];
      size_t token_index;
      size_t identifier_count = 0;
      size_t identifier_index = SIZE_MAX;
      AbString raw = {0};
      AbString detail = {0};
      for (token_index = range.start; token_index < range.end; token_index++)
        if (token_identifier(&tokens, token_index)) {
          identifier_count++;
          identifier_index = token_index;
        }
      status = expression_text(context->engine, &tokens, range.start, range.end,
                               &raw);
      if (status == ARCHBIRD_OK)
        status = macro_detail(context->engine, &call->as.text, &selector_text,
                              &raw, &detail);
      if (identifier_count == 1 && status == ARCHBIRD_OK) {
        AbString name = token_string(&tokens, identifier_index);
        status = add_source_item(context, fact, &source, &name,
                                 tokens.items[identifier_index].line, NULL,
                                 "current", "", detail.data, detail.length);
      } else if (status == ARCHBIRD_OK) {
        AbBuffer key_buffer;
        AbString key = {0};
        AbVerifyFactItem item = {0};
        AbVerifyEvidence evidence = {0};
        ab_buffer_init(&key_buffer, context->engine);
        status = ab_buffer_literal(&key_buffer, "unknown:");
        if (status == ARCHBIRD_OK)
          status = ab_buffer_append(&key_buffer, raw.data, raw.length);
        if (status == ARCHBIRD_OK)
          status =
              ab_string_copy(context->engine, &key,
                             (const char *)key_buffer.data, key_buffer.length);
        ab_buffer_free(&key_buffer);
        if (status == ARCHBIRD_OK)
          status =
              ab_verify_item_init(context->engine, &item, &key, &raw, NULL);
        if (status == ARCHBIRD_OK)
          status =
              ab_verify_item_set_state(context->engine, &item, "unknown",
                                       "macro member is not one identifier");
        if (status == ARCHBIRD_OK)
          status = evidence_for(context, &source, tokens.items[index].line,
                                detail.data, detail.length, &evidence);
        if (status == ARCHBIRD_OK)
          status =
              ab_verify_item_add_evidence(context->engine, &item, &evidence);
        if (status == ARCHBIRD_OK)
          status = ab_verify_fact_add_item(context->engine, fact, &item);
        ab_verify_fact_item_free(context->engine, &item);
        ab_verify_evidence_free(context->engine, &evidence);
        ab_string_free(context->engine, &key);
      }
      ab_string_free(context->engine, &detail);
      ab_string_free(context->engine, &raw);
    }
    ab_string_free(context->engine, &selector_text);
    ab_free(context->engine, arguments);
  }
  if (status == ARCHBIRD_OK && matched != 1) {
    char message[512];
    (void)snprintf(message, sizeof(message),
                   "expected one %.*s call selecting '%.*s', found %zu",
                   (int)call->as.text.length, call->as.text.data,
                   (int)wanted->as.text.length, wanted->as.text.data, matched);
    ab_verify_fact_free(context->engine, fact);
    status =
        ab_verify_fact_unknown(context->engine, fact, &extractor->name,
                               &source.project_name->as.text, "set", message);
  } else if (status == ARCHBIRD_OK) {
    status = ab_verify_fact_finish(context->engine, fact);
  }
  if (status != ARCHBIRD_OK && fact->name.data)
    ab_verify_fact_free(context->engine, fact);
  ab_free(context->engine, parentheses);
  ab_token_list_free(&tokens);
  return status;
}

ArchbirdStatus ab_verify_extract_c(AbVerificationContext *context,
                                   const AbObjectField *extractor,
                                   AbVerifyFactSet *fact) {
  const AbValue *kind = ab_value_member(&extractor->value, "kind");
  if (ab_verify_string_is(kind, "c_enum"))
    return extract_enum(context, extractor, fact);
  if (ab_verify_string_is(kind, "c_designated_initializer"))
    return extract_initializer(context, extractor, fact);
  if (ab_verify_string_is(kind, "c_macro_set"))
    return extract_macro_set(context, extractor, fact);
  return archbird_error_set(context->engine, ARCHBIRD_CONFLICT,
                            ARCHBIRD_NO_OFFSET,
                            "unsupported native C verification extractor");
}
