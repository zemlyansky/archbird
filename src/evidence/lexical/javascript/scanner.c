#include "lexical/javascript/scanner.h"

#include "lexical/tokenizer.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

typedef struct JsContext {
  AbBundleBuilder *builder;
  const AbTokenList *tokens;
  size_t *paren_forward;
  size_t *brace_forward;
  uint8_t *definition_indices;
} JsContext;

static const char *const JS_CONTROL_WORDS[] = {
    "if",       "for",  "while", "switch", "return", "sizeof", "_Alignof",
    "_Generic", "do",   "case",  "catch",  "new",    "super",  "typeof",
    "delete",   "void", "await", "yield",  "import",
};

static const char *const JS_CLASS_MODIFIERS[] = {
    "abstract", "accessor",  "async",  "declare",  "get", "override",
    "private",  "protected", "public", "readonly", "set", "static",
};

static const char *const JS_RESERVED_BINDING_WORDS[] = {
    "await",     "break",    "case",       "catch",  "class",   "const",
    "continue",  "debugger", "default",    "delete", "do",      "else",
    "enum",      "export",   "extends",    "false",  "finally", "for",
    "function",  "if",       "implements", "import", "in",      "instanceof",
    "interface", "let",      "new",        "null",   "package", "private",
    "protected", "public",   "return",     "static", "super",   "switch",
    "this",      "throw",    "true",       "try",    "typeof",  "var",
    "void",      "while",    "with",       "yield",
};

static const uint8_t *token_data(const AbTokenList *tokens, size_t index) {
  return tokens->source + tokens->items[index].start;
}

static size_t token_length(const AbTokenList *tokens, size_t index) {
  return tokens->items[index].end - tokens->items[index].start;
}

static int token_in_words(const AbTokenList *tokens, size_t index,
                          const char *const *words, size_t count) {
  size_t word_index;
  for (word_index = 0; word_index < count; word_index++) {
    if (ab_token_equals(tokens, index, words[word_index]))
      return 1;
  }
  return 0;
}

static int js_control(const AbTokenList *tokens, size_t index) {
  return token_in_words(tokens, index, JS_CONTROL_WORDS,
                        sizeof(JS_CONTROL_WORDS) / sizeof(JS_CONTROL_WORDS[0]));
}

static int js_binding_identifier(const AbTokenList *tokens, size_t index) {
  return index < tokens->count &&
         tokens->items[index].kind == AB_TOKEN_IDENTIFIER &&
         !token_in_words(tokens, index, JS_RESERVED_BINDING_WORDS,
                         sizeof(JS_RESERVED_BINDING_WORDS) /
                             sizeof(JS_RESERVED_BINDING_WORDS[0]));
}

/* Lexical evidence must not promote an expression's private self-name into an
   outer declaration.  Recognize declaration positions positively; unusual
   declaration forms can remain syntax-only without creating a false exact
   lexical identity. */
static int js_declaration_context(const AbTokenList *tokens,
                                  size_t keyword_index) {
  size_t cursor = keyword_index;
  while (cursor) {
    size_t previous = cursor - 1;
    if (ab_token_equals(tokens, previous, "async") ||
        ab_token_equals(tokens, previous, "abstract") ||
        ab_token_equals(tokens, previous, "declare")) {
      cursor = previous;
      continue;
    }
    if (ab_token_equals(tokens, previous, "default") && previous > 0 &&
        ab_token_equals(tokens, previous - 1, "export"))
      return 1;
    return ab_token_equals(tokens, previous, "export") ||
           ab_token_equals(tokens, previous, "{") ||
           ab_token_equals(tokens, previous, "}") ||
           ab_token_equals(tokens, previous, ";");
  }
  return 1;
}

static ArchbirdStatus pair_forward(ArchbirdEngine *engine,
                                   const AbTokenList *tokens,
                                   const char *opening, const char *closing,
                                   size_t **out_forward) {
  size_t *forward;
  size_t *stack;
  size_t stack_count = 0;
  size_t index;
  *out_forward = NULL;
  if (!tokens->count)
    return ARCHBIRD_OK;
  forward = (size_t *)ab_malloc(engine, tokens->count * sizeof(*forward));
  stack = (size_t *)ab_malloc(engine, tokens->count * sizeof(*stack));
  if (!forward || !stack) {
    ab_free(engine, forward);
    ab_free(engine, stack);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory pairing JavaScript tokens");
  }
  for (index = 0; index < tokens->count; index++) {
    forward[index] = SIZE_MAX;
    if (ab_token_equals(tokens, index, opening)) {
      stack[stack_count++] = index;
    } else if (ab_token_equals(tokens, index, closing) && stack_count) {
      size_t start = stack[--stack_count];
      forward[start] = index;
    }
  }
  ab_free(engine, stack);
  *out_forward = forward;
  return ARCHBIRD_OK;
}

static int no_space_before(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, ",") ||
         ab_token_equals(tokens, index, ";") ||
         ab_token_equals(tokens, index, "(") ||
         ab_token_equals(tokens, index, ")") ||
         ab_token_equals(tokens, index, "[") ||
         ab_token_equals(tokens, index, "]");
}

static int no_space_after(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, "(") ||
         ab_token_equals(tokens, index, "[");
}

static size_t utf8_width(uint8_t first) {
  if (first < 0x80)
    return 1;
  if (first < 0xe0)
    return 2;
  if (first < 0xf0)
    return 3;
  return 4;
}

static ArchbirdStatus normalize_tokens(ArchbirdEngine *engine,
                                       const AbTokenList *tokens, size_t start,
                                       size_t end, AbBuffer *out) {
  AbBuffer full;
  size_t index;
  size_t cursor = 0;
  size_t codepoints = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(out, engine);
  ab_buffer_init(&full, engine);
  for (index = start; index < end; index++) {
    const AbToken *token = &tokens->items[index];
    if (index > start && !no_space_before(tokens, index) &&
        !no_space_after(tokens, index - 1)) {
      status = ab_buffer_literal(&full, " ");
      if (status != ARCHBIRD_OK)
        goto done;
    }
    status = ab_buffer_append(&full, tokens->source + token->start,
                              token->end - token->start);
    if (status != ARCHBIRD_OK)
      goto done;
  }
  while (cursor < full.length) {
    cursor += utf8_width(full.data[cursor]);
    codepoints++;
  }
  if (codepoints <= 220) {
    status = ab_buffer_append(out, full.data, full.length);
    goto done;
  }
  cursor = 0;
  codepoints = 0;
  while (cursor < full.length && codepoints < 219) {
    cursor += utf8_width(full.data[cursor]);
    codepoints++;
  }
  status = ab_buffer_append(out, full.data, cursor);
  if (status == ARCHBIRD_OK) {
    static const uint8_t ellipsis[] = {0xe2, 0x80, 0xa6};
    status = ab_buffer_append(out, ellipsis, sizeof(ellipsis));
  }
done:
  ab_buffer_free(&full);
  return status;
}

static ArchbirdStatus js_signature(JsContext *context, size_t name_index,
                                   AbBuffer *out) {
  const AbTokenList *tokens = context->tokens;
  ab_buffer_init(out, context->builder->engine);
  if (name_index + 1 >= tokens->count ||
      !ab_token_equals(tokens, name_index + 1, "(") ||
      context->paren_forward[name_index + 1] == SIZE_MAX) {
    return ab_buffer_append(out, token_data(tokens, name_index),
                            token_length(tokens, name_index));
  }
  return normalize_tokens(context->builder->engine, tokens, name_index,
                          context->paren_forward[name_index + 1] + 1, out);
}

static ArchbirdStatus qualified_name(ArchbirdEngine *engine,
                                     const uint8_t *prefix,
                                     size_t prefix_length, const uint8_t *name,
                                     size_t name_length, AbBuffer *out) {
  ArchbirdStatus status;
  ab_buffer_init(out, engine);
  status = ab_buffer_append(out, prefix, prefix_length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(out, ".");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(out, name, name_length);
  return status;
}

static void unquoted_token(const AbTokenList *tokens, size_t index,
                           const uint8_t **out_data, size_t *out_length) {
  const AbToken *token = &tokens->items[index];
  size_t trim = token->kind == AB_TOKEN_STRING ? 1 : 0;
  *out_data = tokens->source + token->start + trim;
  *out_length = token->end - token->start - trim * 2;
}

static ArchbirdStatus add_symbol_with_identity(
    JsContext *context, size_t token_index, const uint8_t *name,
    size_t name_length, const char *kind, const char *scope,
    const uint8_t *signature, size_t signature_length, int identity_partial) {
  const AbToken *token = &context->tokens->items[token_index];
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      context->builder, "symbols", kind, "lexical-occurrence", token->start,
      token->end, name, name_length, name, name_length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact, "line",
                                       token->line);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(context->builder->engine, fact, "scope",
                                     (const uint8_t *)scope, strlen(scope));
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_string_attribute(context->builder->engine, fact,
                                     "signature", signature, signature_length);
  if (status == ARCHBIRD_OK && identity_partial)
    status = ab_fact_add_string_attribute(context->builder->engine, fact,
                                          "identity_state",
                                          (const uint8_t *)"partial", 7);
  if (status == ARCHBIRD_OK)
    context->definition_indices[token_index] = 1;
  return status;
}

static ArchbirdStatus add_symbol(JsContext *context, size_t token_index,
                                 const uint8_t *name, size_t name_length,
                                 const char *kind, const char *scope,
                                 const uint8_t *signature,
                                 size_t signature_length) {
  return add_symbol_with_identity(context, token_index, name, name_length, kind,
                                  scope, signature, signature_length, 0);
}

static ArchbirdStatus add_simple_symbol(JsContext *context, size_t token_index,
                                        const char *kind, const char *scope) {
  AbBuffer signature;
  ArchbirdStatus status = js_signature(context, token_index, &signature);
  if (status == ARCHBIRD_OK)
    status = add_symbol(context, token_index,
                        token_data(context->tokens, token_index),
                        token_length(context->tokens, token_index), kind, scope,
                        signature.data, signature.length);
  ab_buffer_free(&signature);
  return status;
}

static ArchbirdStatus add_partial_simple_symbol(JsContext *context,
                                                size_t token_index,
                                                const char *kind,
                                                const char *scope) {
  AbBuffer signature;
  ArchbirdStatus status = js_signature(context, token_index, &signature);
  if (status == ARCHBIRD_OK)
    status = add_symbol_with_identity(
        context, token_index, token_data(context->tokens, token_index),
        token_length(context->tokens, token_index), kind, scope, signature.data,
        signature.length, 1);
  ab_buffer_free(&signature);
  return status;
}

static size_t find_token(const AbTokenList *tokens, size_t start, size_t limit,
                         size_t distance, const char *literal) {
  size_t end = start + distance < limit ? start + distance : limit;
  size_t index;
  for (index = start; index < end; index++) {
    if (ab_token_equals(tokens, index, literal))
      return index;
  }
  return SIZE_MAX;
}

static size_t js_method_body(JsContext *context, size_t close_paren,
                             size_t limit) {
  size_t cursor = close_paren + 1;
  int typed;
  if (cursor < limit && ab_token_equals(context->tokens, cursor, "?"))
    cursor++;
  typed = cursor < limit && ab_token_equals(context->tokens, cursor, ":");
  if (typed)
    cursor++;
  while (cursor < limit) {
    if (ab_token_equals(context->tokens, cursor, "{")) {
      size_t close = context->brace_forward[cursor];
      if (close == SIZE_MAX)
        return SIZE_MAX;
      if (typed && close + 1 < limit &&
          (ab_token_equals(context->tokens, close + 1, "{") ||
           ab_token_equals(context->tokens, close + 1, "|") ||
           ab_token_equals(context->tokens, close + 1, "&") ||
           ab_token_equals(context->tokens, close + 1, "[") ||
           ab_token_equals(context->tokens, close + 1, "]") ||
           ab_token_equals(context->tokens, close + 1, "?"))) {
        cursor = close + 1;
        continue;
      }
      return cursor;
    }
    if (ab_token_equals(context->tokens, cursor, ";") ||
        ab_token_equals(context->tokens, cursor, "=") ||
        ab_token_equals(context->tokens, cursor, "=>"))
      return SIZE_MAX;
    cursor++;
  }
  return SIZE_MAX;
}

static size_t class_member_index(const AbTokenList *tokens, size_t cursor,
                                 size_t limit) {
  while (cursor < limit && token_in_words(tokens, cursor, JS_CLASS_MODIFIERS,
                                          sizeof(JS_CLASS_MODIFIERS) /
                                              sizeof(JS_CLASS_MODIFIERS[0])))
    cursor++;
  if (cursor + 1 < limit && ab_token_equals(tokens, cursor, "#"))
    cursor++;
  return cursor < limit && tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER
             ? cursor
             : SIZE_MAX;
}

static int looks_like_class_member(const AbTokenList *tokens, size_t cursor,
                                   size_t limit) {
  size_t member = class_member_index(tokens, cursor, limit);
  size_t look;
  if (member == SIZE_MAX)
    return 0;
  look = member + 1;
  if (look < limit && (ab_token_equals(tokens, look, "?") ||
                       ab_token_equals(tokens, look, "!")))
    look++;
  if (look < limit && ab_token_equals(tokens, look, "("))
    return 1;
  while (look < limit &&
         tokens->items[look].line == tokens->items[member].line) {
    if (ab_token_equals(tokens, look, "=") ||
        ab_token_equals(tokens, look, ";"))
      return 1;
    look++;
  }
  return 0;
}

static ArchbirdStatus scan_object_methods(JsContext *context, size_t opening,
                                          size_t closing, const uint8_t *prefix,
                                          size_t prefix_length) {
  size_t cursor = opening + 1;
  while (cursor < closing) {
    const uint8_t *key;
    size_t key_length;
    AbBuffer qualified;
    if (ab_token_equals(context->tokens, cursor, "{") &&
        context->brace_forward[cursor] != SIZE_MAX) {
      cursor = context->brace_forward[cursor] + 1;
      continue;
    }
    if (context->tokens->items[cursor].kind != AB_TOKEN_IDENTIFIER &&
        context->tokens->items[cursor].kind != AB_TOKEN_STRING) {
      cursor++;
      continue;
    }
    /* A quoted object key is one property-name segment.  Preserve its spelling
       so embedded dots cannot be mistaken for qualification separators and so
       syntax/lexical providers correlate the same span. */
    key = token_data(context->tokens, cursor);
    key_length = token_length(context->tokens, cursor);
    {
      ArchbirdStatus status =
          qualified_name(context->builder->engine, prefix, prefix_length, key,
                         key_length, &qualified);
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (cursor + 1 < closing &&
        ab_token_equals(context->tokens, cursor + 1, "(") &&
        context->paren_forward[cursor + 1] != SIZE_MAX) {
      size_t paren_close = context->paren_forward[cursor + 1];
      if (paren_close + 1 < closing &&
          ab_token_equals(context->tokens, paren_close + 1, "{") &&
          context->brace_forward[paren_close + 1] != SIZE_MAX) {
        AbBuffer signature;
        ArchbirdStatus status = js_signature(context, cursor, &signature);
        if (status == ARCHBIRD_OK)
          status =
              add_symbol(context, cursor, qualified.data, qualified.length,
                         "method", "method", signature.data, signature.length);
        ab_buffer_free(&signature);
        ab_buffer_free(&qualified);
        if (status != ARCHBIRD_OK)
          return status;
        cursor = context->brace_forward[paren_close + 1] + 1;
        continue;
      }
    }
    if (cursor + 2 < closing &&
        ab_token_equals(context->tokens, cursor + 1, ":")) {
      size_t value_index = cursor + 2;
      if (ab_token_equals(context->tokens, value_index, "{") &&
          context->brace_forward[value_index] != SIZE_MAX) {
        size_t nested_close = context->brace_forward[value_index];
        ArchbirdStatus status =
            scan_object_methods(context, value_index, nested_close,
                                qualified.data, qualified.length);
        ab_buffer_free(&qualified);
        if (status != ARCHBIRD_OK)
          return status;
        cursor = nested_close + 1;
        continue;
      }
      if (ab_token_equals(context->tokens, value_index, "function")) {
        ArchbirdStatus status =
            add_symbol(context, cursor, qualified.data, qualified.length,
                       "method", "method", qualified.data, qualified.length);
        ab_buffer_free(&qualified);
        if (status != ARCHBIRD_OK)
          return status;
        /* Skip the complete function value. Otherwise the next iteration
           mistakes the `function` keyword for another object key and emits a
           false `<object>.function` method. */
        {
          size_t scan = value_index + 1;
          if (scan < closing && ab_token_equals(context->tokens, scan, "*"))
            scan++;
          if (scan < closing &&
              context->tokens->items[scan].kind == AB_TOKEN_IDENTIFIER)
            scan++;
          if (scan < closing && ab_token_equals(context->tokens, scan, "(") &&
              context->paren_forward[scan] != SIZE_MAX) {
            size_t after = context->paren_forward[scan] + 1;
            if (after < closing &&
                ab_token_equals(context->tokens, after, "{") &&
                context->brace_forward[after] != SIZE_MAX) {
              cursor = context->brace_forward[after] + 1;
              continue;
            }
          }
        }
        cursor = value_index + 1;
        continue;
      }
    }
    ab_buffer_free(&qualified);
    cursor++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_type_symbol(JsContext *context, size_t keyword_index,
                                      size_t name_index) {
  AbBuffer signature;
  const char *kind =
      ab_token_equals(context->tokens, keyword_index, "interface") ? "interface"
      : ab_token_equals(context->tokens, keyword_index, "type")    ? "type"
                                                                   : "enum";
  ArchbirdStatus status;
  ab_buffer_init(&signature, context->builder->engine);
  status =
      ab_buffer_append(&signature, token_data(context->tokens, keyword_index),
                       token_length(context->tokens, keyword_index));
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&signature, " ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&signature, token_data(context->tokens, name_index),
                         token_length(context->tokens, name_index));
  if (status == ARCHBIRD_OK)
    status =
        add_symbol(context, name_index, token_data(context->tokens, name_index),
                   token_length(context->tokens, name_index), kind, "global",
                   signature.data, signature.length);
  ab_buffer_free(&signature);
  return status;
}

static ArchbirdStatus scan_interface_methods(JsContext *context,
                                             size_t name_index) {
  size_t body_open = find_token(context->tokens, name_index + 1,
                                context->tokens->count, 40, "{");
  size_t cursor;
  size_t body_close;
  if (body_open == SIZE_MAX || context->brace_forward[body_open] == SIZE_MAX)
    return ARCHBIRD_OK;
  body_close = context->brace_forward[body_open];
  cursor = body_open + 1;
  while (cursor < body_close) {
    if (context->tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER &&
        cursor + 1 < body_close &&
        ab_token_equals(context->tokens, cursor + 1, "(")) {
      AbBuffer qualified;
      AbBuffer signature;
      ArchbirdStatus status = qualified_name(
          context->builder->engine, token_data(context->tokens, name_index),
          token_length(context->tokens, name_index),
          token_data(context->tokens, cursor),
          token_length(context->tokens, cursor), &qualified);
      if (status == ARCHBIRD_OK)
        status = js_signature(context, cursor, &signature);
      else
        ab_buffer_init(&signature, context->builder->engine);
      if (status == ARCHBIRD_OK)
        status = add_symbol(context, cursor, qualified.data, qualified.length,
                            "method", "declaration", signature.data,
                            signature.length);
      ab_buffer_free(&qualified);
      ab_buffer_free(&signature);
      if (status != ARCHBIRD_OK)
        return status;
      cursor = context->paren_forward[cursor + 1] == SIZE_MAX
                   ? cursor + 1
                   : context->paren_forward[cursor + 1] + 1;
      continue;
    }
    cursor++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_qualified_method(JsContext *context,
                                           size_t class_index,
                                           size_t method_index, int arrow_field,
                                           int identity_partial) {
  AbBuffer qualified;
  AbBuffer signature;
  ArchbirdStatus status = qualified_name(
      context->builder->engine, token_data(context->tokens, class_index),
      token_length(context->tokens, class_index),
      token_data(context->tokens, method_index),
      token_length(context->tokens, method_index), &qualified);
  if (arrow_field) {
    ab_buffer_init(&signature, context->builder->engine);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(&signature,
                                token_data(context->tokens, method_index),
                                token_length(context->tokens, method_index));
  } else if (status == ARCHBIRD_OK) {
    status = js_signature(context, method_index, &signature);
  } else {
    ab_buffer_init(&signature, context->builder->engine);
  }
  if (status == ARCHBIRD_OK)
    status = add_symbol_with_identity(
        context, method_index, qualified.data, qualified.length, "method",
        "method", signature.data, signature.length, identity_partial);
  ab_buffer_free(&qualified);
  ab_buffer_free(&signature);
  return status;
}

static ArchbirdStatus scan_class(JsContext *context, size_t class_index,
                                 size_t keyword_index, int identity_partial) {
  AbBuffer signature;
  size_t body_open;
  size_t body_close;
  size_t cursor;
  ArchbirdStatus status;
  ab_buffer_init(&signature, context->builder->engine);
  status = ab_buffer_literal(&signature, "class ");
  if (status == ARCHBIRD_OK)
    status =
        ab_buffer_append(&signature, token_data(context->tokens, class_index),
                         token_length(context->tokens, class_index));
  if (status == ARCHBIRD_OK)
    status = add_symbol_with_identity(
        context, class_index, token_data(context->tokens, class_index),
        token_length(context->tokens, class_index), "class", "class",
        signature.data, signature.length, identity_partial);
  ab_buffer_free(&signature);
  if (status != ARCHBIRD_OK)
    return status;
  body_open = find_token(context->tokens, keyword_index + 1,
                         context->tokens->count, 20, "{");
  if (body_open == SIZE_MAX || context->brace_forward[body_open] == SIZE_MAX)
    return ARCHBIRD_OK;
  body_close = context->brace_forward[body_open];
  cursor = body_open + 1;
  while (cursor < body_close) {
    size_t method_index =
        class_member_index(context->tokens, cursor, body_close);
    size_t after_name;
    if (method_index == SIZE_MAX) {
      cursor++;
      continue;
    }
    after_name = method_index + 1;
    if (after_name < body_close &&
        (ab_token_equals(context->tokens, after_name, "?") ||
         ab_token_equals(context->tokens, after_name, "!")))
      after_name++;
    if (after_name < body_close &&
        ab_token_equals(context->tokens, after_name, "(") &&
        context->paren_forward[after_name] != SIZE_MAX) {
      size_t body = js_method_body(context, context->paren_forward[after_name],
                                   body_close);
      if (body != SIZE_MAX) {
        status = add_qualified_method(context, class_index, method_index, 0,
                                      identity_partial);
        if (status != ARCHBIRD_OK)
          return status;
        cursor = context->brace_forward[body] + 1;
        continue;
      }
    }
    {
      size_t scan = after_name;
      size_t end = body_close;
      int saw_assignment = 0;
      int saw_arrow = 0;
      while (scan < body_close) {
        if (scan > after_name &&
            context->tokens->items[scan].line >
                context->tokens->items[method_index].line &&
            looks_like_class_member(context->tokens, scan, body_close)) {
          end = scan;
          break;
        }
        if (ab_token_equals(context->tokens, scan, ";")) {
          end = scan;
          break;
        }
        if (ab_token_equals(context->tokens, scan, "="))
          saw_assignment = 1;
        else if (ab_token_equals(context->tokens, scan, "=>") && saw_assignment)
          saw_arrow = 1;
        else if (ab_token_equals(context->tokens, scan, "{") &&
                 context->brace_forward[scan] != SIZE_MAX)
          scan = context->brace_forward[scan];
        scan++;
      }
      if (saw_arrow) {
        status = add_qualified_method(context, class_index, method_index, 1,
                                      identity_partial);
        if (status != ARCHBIRD_OK)
          return status;
      }
      cursor = end < body_close && ab_token_equals(context->tokens, end, ";")
                   ? end + 1
                   : end;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_declared_symbols(JsContext *context,
                                            const size_t *brace_depths) {
  size_t index;
  for (index = 0; index < context->tokens->count; index++) {
    if (ab_token_equals(context->tokens, index, "function") &&
        js_declaration_context(context->tokens, index)) {
      size_t name_index = index + 1;
      if (name_index < context->tokens->count &&
          ab_token_equals(context->tokens, name_index, "*"))
        name_index++;
      if (name_index < context->tokens->count &&
          context->tokens->items[name_index].kind == AB_TOKEN_IDENTIFIER) {
        ArchbirdStatus status =
            brace_depths[index] > 0
                ? add_partial_simple_symbol(context, name_index, "function",
                                            "function")
                : add_simple_symbol(context, name_index, "function",
                                    "function");
        if (status != ARCHBIRD_OK)
          return status;
      }
    } else if ((ab_token_equals(context->tokens, index, "interface") ||
                ab_token_equals(context->tokens, index, "type") ||
                ab_token_equals(context->tokens, index, "enum")) &&
               brace_depths[index] == 0 && index + 1 < context->tokens->count &&
               context->tokens->items[index + 1].kind == AB_TOKEN_IDENTIFIER &&
               !(index > 0 &&
                 ab_token_equals(context->tokens, index - 1, "import"))) {
      ArchbirdStatus status = add_type_symbol(context, index, index + 1);
      if (status != ARCHBIRD_OK)
        return status;
      if (ab_token_equals(context->tokens, index, "interface")) {
        status = scan_interface_methods(context, index + 1);
        if (status != ARCHBIRD_OK)
          return status;
      }
    } else if (ab_token_equals(context->tokens, index, "class") &&
               js_declaration_context(context->tokens, index) &&
               index + 1 < context->tokens->count &&
               context->tokens->items[index + 1].kind == AB_TOKEN_IDENTIFIER &&
               !ab_token_equals(context->tokens, index + 1, "extends") &&
               !ab_token_equals(context->tokens, index + 1, "implements")) {
      ArchbirdStatus status =
          scan_class(context, index + 1, index, brace_depths[index] > 0);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static int direct_function_initializer(const JsContext *context, size_t start) {
  const AbTokenList *tokens = context->tokens;
  size_t cursor = start;
  if (cursor < tokens->count && ab_token_equals(tokens, cursor, "async"))
    cursor++;
  if (cursor >= tokens->count)
    return 0;
  if (ab_token_equals(tokens, cursor, "function"))
    return 1;
  if (tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER)
    return cursor + 1 < tokens->count &&
           ab_token_equals(tokens, cursor + 1, "=>");
  if (ab_token_equals(tokens, cursor, "(") &&
      context->paren_forward[cursor] != SIZE_MAX) {
    size_t closing = context->paren_forward[cursor];
    return closing + 1 < tokens->count &&
           ab_token_equals(tokens, closing + 1, "=>");
  }
  return 0;
}

static ArchbirdStatus scan_top_level_bindings(JsContext *context) {
  size_t depth = 0;
  size_t index = 0;
  while (index < context->tokens->count) {
    if (ab_token_equals(context->tokens, index, "{")) {
      depth++;
    } else if (ab_token_equals(context->tokens, index, "}")) {
      depth = depth ? depth - 1 : 0;
    } else if (depth == 0 &&
               (ab_token_equals(context->tokens, index, "const") ||
                ab_token_equals(context->tokens, index, "let") ||
                ab_token_equals(context->tokens, index, "var")) &&
               index + 3 < context->tokens->count) {
      size_t name_index = index + 1;
      size_t assignment = SIZE_MAX;
      size_t cursor = index + 2;
      if (!js_binding_identifier(context->tokens, name_index)) {
        index++;
        continue;
      }
      while (cursor < context->tokens->count &&
             !ab_token_equals(context->tokens, cursor, ";")) {
        if (ab_token_equals(context->tokens, cursor, "=")) {
          assignment = cursor;
          break;
        }
        if (ab_token_equals(context->tokens, cursor, "{") &&
            context->brace_forward[cursor] != SIZE_MAX)
          cursor = context->brace_forward[cursor];
        cursor++;
      }
      if (assignment != SIZE_MAX && assignment + 1 < context->tokens->count) {
        size_t start = assignment + 1;
        if (ab_token_equals(context->tokens, start, "{") &&
            context->brace_forward[start] != SIZE_MAX) {
          size_t close = context->brace_forward[start];
          ArchbirdStatus status = scan_object_methods(
              context, start, close, token_data(context->tokens, name_index),
              token_length(context->tokens, name_index));
          if (status != ARCHBIRD_OK)
            return status;
          index = close;
        } else if (direct_function_initializer(context, start)) {
          ArchbirdStatus status = add_symbol(
              context, name_index, token_data(context->tokens, name_index),
              token_length(context->tokens, name_index), "function", "function",
              token_data(context->tokens, name_index),
              token_length(context->tokens, name_index));
          if (status != ARCHBIRD_OK)
            return status;
        } else if (ab_token_equals(context->tokens, start, "class") &&
                   find_token(context->tokens, start + 1,
                              context->tokens->count, 20, "{") != SIZE_MAX) {
          ArchbirdStatus status = scan_class(context, name_index, start, 0);
          if (status != ARCHBIRD_OK)
            return status;
        }
      }
    }
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_token_fact(AbBundleBuilder *builder,
                                     const AbTokenList *tokens,
                                     size_t token_index, size_t trim,
                                     const char *domain, const char *kind) {
  const AbToken *token = &tokens->items[token_index];
  const uint8_t *name = token_data(tokens, token_index) + trim;
  size_t length = token_length(tokens, token_index) - trim * 2;
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, domain, kind, "lexical-occurrence", token->start + trim,
      token->end - trim, name, length, name, length, &fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "line", token->line);
  return status;
}

static ArchbirdStatus add_buffer_fact(AbBundleBuilder *builder,
                                      const AbTokenList *tokens,
                                      size_t witness_index, const char *domain,
                                      const char *kind, const AbBuffer *name) {
  const AbToken *token = &tokens->items[witness_index];
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, domain, kind, "lexical-occurrence", token->start, token->end,
      name->data, name->length, name->data, name->length, &fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "line", token->line);
  return status;
}

static ArchbirdStatus scan_calls(JsContext *context) {
  size_t index;
  for (index = 0; index + 1 < context->tokens->count; index++) {
    const char *domain;
    if (context->tokens->items[index].kind != AB_TOKEN_IDENTIFIER ||
        !ab_token_equals(context->tokens, index + 1, "(") ||
        js_control(context->tokens, index) ||
        context->definition_indices[index])
      continue;
    domain = index > 0 && (ab_token_equals(context->tokens, index - 1, ".") ||
                           ab_token_equals(context->tokens, index - 1, "?."))
                 ? "method-calls"
                 : "calls";
    {
      ArchbirdStatus status = add_token_fact(context->builder, context->tokens,
                                             index, 0, domain, "call");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index + 2 < context->tokens->count; index++) {
    if ((ab_token_equals(context->tokens, index, "ccall") ||
         ab_token_equals(context->tokens, index, "cwrap")) &&
        ab_token_equals(context->tokens, index + 1, "(") &&
        context->tokens->items[index + 2].kind == AB_TOKEN_STRING) {
      ArchbirdStatus status = add_token_fact(context->builder, context->tokens,
                                             index + 2, 1, "calls", "call");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_imports(JsContext *context) {
  size_t index;
  for (index = 0; index < context->tokens->count; index++) {
    size_t string_index = SIZE_MAX;
    if (ab_token_equals(context->tokens, index, "require") &&
        index + 2 < context->tokens->count &&
        ab_token_equals(context->tokens, index + 1, "(") &&
        context->tokens->items[index + 2].kind == AB_TOKEN_STRING) {
      string_index = index + 2;
    } else if (ab_token_equals(context->tokens, index, "from") &&
               index + 1 < context->tokens->count &&
               context->tokens->items[index + 1].kind == AB_TOKEN_STRING) {
      string_index = index + 1;
    } else if (ab_token_equals(context->tokens, index, "import") &&
               index + 1 < context->tokens->count) {
      if (context->tokens->items[index + 1].kind == AB_TOKEN_STRING)
        string_index = index + 1;
      else if (index + 2 < context->tokens->count &&
               ab_token_equals(context->tokens, index + 1, "(") &&
               context->tokens->items[index + 2].kind == AB_TOKEN_STRING)
        string_index = index + 2;
    }
    if (string_index != SIZE_MAX) {
      ArchbirdStatus status =
          add_token_fact(context->builder, context->tokens, string_index, 1,
                         "imports", "import");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_export_token(JsContext *context, size_t index) {
  size_t trim = context->tokens->items[index].kind == AB_TOKEN_STRING ? 1 : 0;
  const AbToken *token = &context->tokens->items[index];
  const uint8_t *name = token_data(context->tokens, index) + trim;
  size_t length = token_length(context->tokens, index) - trim * 2;
  AbFact *fact = NULL;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      context->builder, "exports", "export", "lexical-occurrence",
      token->start + trim, token->end - trim, name, length, name, length,
      &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact, "line",
                                       token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact,
                                       "local_definition", 1);
  return status;
}

static ArchbirdStatus add_reexport_token(JsContext *context,
                                         size_t exported_index,
                                         size_t origin_name_index,
                                         size_t origin_index) {
  const AbToken *token = &context->tokens->items[exported_index];
  const uint8_t *exported = token_data(context->tokens, exported_index);
  const uint8_t *origin_name = token_data(context->tokens, origin_name_index);
  const uint8_t *origin;
  size_t exported_length = token_length(context->tokens, exported_index);
  size_t origin_name_length = token_length(context->tokens, origin_name_index);
  size_t origin_length;
  AbFact *fact = NULL;
  ArchbirdStatus status;
  unquoted_token(context->tokens, origin_index, &origin, &origin_length);
  status = ab_bundle_builder_add_fact(
      context->builder, "exports", "export", "lexical-occurrence", token->start,
      token->end, exported, exported_length, exported, exported_length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact, "line",
                                       token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(context->builder->engine, fact,
                                          "origin", origin, origin_length);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(context->builder->engine, fact,
                                          "origin_name", origin_name,
                                          origin_name_length);
  return status;
}

static ArchbirdStatus add_module_reexport_token(JsContext *context,
                                                size_t origin_index,
                                                const char *kind) {
  return add_token_fact(context->builder, context->tokens, origin_index, 1,
                        "module-reexports", kind);
}

static int declaration_kind(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, "function") ||
         ab_token_equals(tokens, index, "class") ||
         ab_token_equals(tokens, index, "const") ||
         ab_token_equals(tokens, index, "let") ||
         ab_token_equals(tokens, index, "var") ||
         ab_token_equals(tokens, index, "interface") ||
         ab_token_equals(tokens, index, "type") ||
         ab_token_equals(tokens, index, "enum");
}

static ArchbirdStatus scan_esm_exports(JsContext *context) {
  size_t index;
  for (index = 0; index + 1 < context->tokens->count; index++) {
    size_t cursor;
    size_t default_index = SIZE_MAX;
    int is_default = 0;
    if (!ab_token_equals(context->tokens, index, "export"))
      continue;
    cursor = index + 1;
    if (ab_token_equals(context->tokens, cursor, "default")) {
      is_default = 1;
      default_index = cursor++;
    }
    if (is_default) {
      ArchbirdStatus status = add_export_token(context, default_index);
      if (status != ARCHBIRD_OK)
        return status;
      continue;
    }
    if (cursor >= context->tokens->count) {
      continue;
    }
    if (ab_token_equals(context->tokens, cursor, "type") &&
        cursor + 1 < context->tokens->count &&
        ab_token_equals(context->tokens, cursor + 1, "{"))
      cursor++;
    if (declaration_kind(context->tokens, cursor)) {
      cursor++;
      if (cursor < context->tokens->count &&
          context->tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER) {
        ArchbirdStatus status = add_export_token(context, cursor);
        if (status != ARCHBIRD_OK)
          return status;
      }
    } else if (ab_token_equals(context->tokens, cursor, "{") &&
               context->brace_forward[cursor] != SIZE_MAX) {
      size_t close = context->brace_forward[cursor];
      size_t origin_index = SIZE_MAX;
      size_t part_start = cursor + 1;
      size_t part_end;
      if (close + 2 < context->tokens->count &&
          ab_token_equals(context->tokens, close + 1, "from") &&
          context->tokens->items[close + 2].kind == AB_TOKEN_STRING)
        origin_index = close + 2;
      for (part_end = part_start; part_end <= close; part_end++) {
        if (part_end == close ||
            ab_token_equals(context->tokens, part_end, ",")) {
          size_t part_cursor;
          size_t first = SIZE_MAX;
          size_t last = SIZE_MAX;
          int has_as = 0;
          size_t specifier_start = part_start;
          if (part_start + 1 < part_end &&
              ab_token_equals(context->tokens, part_start, "type") &&
              !ab_token_equals(context->tokens, part_start + 1, "as") &&
              context->tokens->items[part_start + 1].kind ==
                  AB_TOKEN_IDENTIFIER)
            specifier_start++;
          for (part_cursor = specifier_start; part_cursor < part_end;
               part_cursor++) {
            if (context->tokens->items[part_cursor].kind ==
                AB_TOKEN_IDENTIFIER) {
              if (first == SIZE_MAX)
                first = part_cursor;
              last = part_cursor;
            }
            if (ab_token_equals(context->tokens, part_cursor, "as"))
              has_as = 1;
          }
          if (first != SIZE_MAX) {
            ArchbirdStatus status =
                origin_index == SIZE_MAX
                    ? add_export_token(context, has_as ? last : first)
                    : add_reexport_token(context, has_as ? last : first, first,
                                         origin_index);
            if (status != ARCHBIRD_OK)
              return status;
          }
          part_start = part_end + 1;
        }
      }
    } else if (ab_token_equals(context->tokens, cursor, "*") &&
               cursor + 2 < context->tokens->count &&
               ab_token_equals(context->tokens, cursor + 1, "from") &&
               context->tokens->items[cursor + 2].kind == AB_TOKEN_STRING) {
      ArchbirdStatus status =
          add_module_reexport_token(context, cursor + 2, "esm-star");
      if (status != ARCHBIRD_OK)
        return status;
    } else if (ab_token_equals(context->tokens, cursor, "*") &&
               cursor + 4 < context->tokens->count &&
               ab_token_equals(context->tokens, cursor + 1, "as") &&
               context->tokens->items[cursor + 2].kind == AB_TOKEN_IDENTIFIER &&
               ab_token_equals(context->tokens, cursor + 3, "from") &&
               context->tokens->items[cursor + 4].kind == AB_TOKEN_STRING) {
      ArchbirdStatus status =
          add_reexport_token(context, cursor + 2, cursor, cursor + 4);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus object_export_keys(JsContext *context, size_t opening,
                                         size_t closing) {
  size_t depth = 0;
  size_t index = opening + 1;
  int expect_key = 1;
  while (index < closing) {
    if (ab_token_equals(context->tokens, index, "{") ||
        ab_token_equals(context->tokens, index, "[") ||
        ab_token_equals(context->tokens, index, "(")) {
      depth++;
    } else if (ab_token_equals(context->tokens, index, "}") ||
               ab_token_equals(context->tokens, index, "]") ||
               ab_token_equals(context->tokens, index, ")")) {
      depth = depth ? depth - 1 : 0;
    } else if (depth == 0 && ab_token_equals(context->tokens, index, ",")) {
      expect_key = 1;
    } else if (depth == 0 && expect_key &&
               (context->tokens->items[index].kind == AB_TOKEN_IDENTIFIER ||
                context->tokens->items[index].kind == AB_TOKEN_STRING)) {
      if (index == 0 || !ab_token_equals(context->tokens, index - 1, ".")) {
        ArchbirdStatus status = add_export_token(context, index);
        if (status != ARCHBIRD_OK)
          return status;
      }
      expect_key = 0;
    }
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_commonjs_exports(JsContext *context) {
  size_t index = 0;
  while (index < context->tokens->count) {
    if (index + 3 < context->tokens->count &&
        ab_token_equals(context->tokens, index, "module") &&
        ab_token_equals(context->tokens, index + 1, ".") &&
        ab_token_equals(context->tokens, index + 2, "exports")) {
      size_t cursor = index + 3;
      if (cursor + 1 < context->tokens->count &&
          ab_token_equals(context->tokens, cursor, ".") &&
          context->tokens->items[cursor + 1].kind == AB_TOKEN_IDENTIFIER) {
        ArchbirdStatus status = add_export_token(context, cursor + 1);
        if (status != ARCHBIRD_OK)
          return status;
      } else if (cursor + 1 < context->tokens->count &&
                 ab_token_equals(context->tokens, cursor, "=")) {
        size_t value_index = cursor + 1;
        if (ab_token_equals(context->tokens, value_index, "{") &&
            context->brace_forward[value_index] != SIZE_MAX) {
          ArchbirdStatus status = object_export_keys(
              context, value_index, context->brace_forward[value_index]);
          if (status != ARCHBIRD_OK)
            return status;
        } else if (value_index + 2 < context->tokens->count &&
                   ab_token_equals(context->tokens, value_index, "require") &&
                   ab_token_equals(context->tokens, value_index + 1, "(") &&
                   context->tokens->items[value_index + 2].kind ==
                       AB_TOKEN_STRING) {
          ArchbirdStatus status = add_module_reexport_token(
              context, value_index + 2, "commonjs-require");
          if (status != ARCHBIRD_OK)
            return status;
        }
      }
    } else if (index + 2 < context->tokens->count &&
               ab_token_equals(context->tokens, index, "exports") &&
               ab_token_equals(context->tokens, index + 1, ".") &&
               context->tokens->items[index + 2].kind == AB_TOKEN_IDENTIFIER) {
      ArchbirdStatus status = add_export_token(context, index + 2);
      if (status != ARCHBIRD_OK)
        return status;
    }
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_message_pair(JsContext *context, size_t key_index,
                                       size_t value_index,
                                       const char *direction) {
  const uint8_t *key;
  const uint8_t *value;
  size_t key_length;
  size_t value_length;
  AbBuffer pair;
  ArchbirdStatus status;
  unquoted_token(context->tokens, key_index, &key, &key_length);
  unquoted_token(context->tokens, value_index, &value, &value_length);
  ab_buffer_init(&pair, context->builder->engine);
  status = ab_buffer_append(&pair, key, key_length);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_literal(&pair, ":");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&pair, value, value_length);
  if (status == ARCHBIRD_OK)
    status = add_buffer_fact(context->builder, context->tokens, value_index,
                             "messages", direction, &pair);
  ab_buffer_free(&pair);
  return status;
}

static ArchbirdStatus object_message_pairs(JsContext *context, size_t opening,
                                           size_t closing) {
  size_t depth = 0;
  size_t index = opening + 1;
  while (index + 2 < closing) {
    if (ab_token_equals(context->tokens, index, "{") ||
        ab_token_equals(context->tokens, index, "[") ||
        ab_token_equals(context->tokens, index, "(")) {
      depth++;
    } else if (ab_token_equals(context->tokens, index, "}") ||
               ab_token_equals(context->tokens, index, "]") ||
               ab_token_equals(context->tokens, index, ")")) {
      depth = depth ? depth - 1 : 0;
    } else if (depth == 0 &&
               (context->tokens->items[index].kind == AB_TOKEN_IDENTIFIER ||
                context->tokens->items[index].kind == AB_TOKEN_STRING) &&
               ab_token_equals(context->tokens, index + 1, ":") &&
               context->tokens->items[index + 2].kind == AB_TOKEN_STRING) {
      ArchbirdStatus status =
          add_message_pair(context, index, index + 2, "send");
      if (status != ARCHBIRD_OK)
        return status;
    }
    index++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_star_message(JsContext *context, size_t value_index) {
  const uint8_t *value;
  size_t value_length;
  AbBuffer pair;
  ArchbirdStatus status;
  unquoted_token(context->tokens, value_index, &value, &value_length);
  ab_buffer_init(&pair, context->builder->engine);
  status = ab_buffer_literal(&pair, "*:");
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&pair, value, value_length);
  if (status == ARCHBIRD_OK)
    status = add_buffer_fact(context->builder, context->tokens, value_index,
                             "messages", "receive", &pair);
  ab_buffer_free(&pair);
  return status;
}

static ArchbirdStatus scan_messages(JsContext *context) {
  size_t index;
  for (index = 0; index + 2 < context->tokens->count; index++) {
    if (ab_token_equals(context->tokens, index, "postMessage") &&
        ab_token_equals(context->tokens, index + 1, "(") &&
        ab_token_equals(context->tokens, index + 2, "{") &&
        context->brace_forward[index + 2] != SIZE_MAX) {
      ArchbirdStatus status = object_message_pairs(
          context, index + 2, context->brace_forward[index + 2]);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index + 4 < context->tokens->count; index++) {
    if (context->tokens->items[index].kind == AB_TOKEN_IDENTIFIER &&
        ab_token_equals(context->tokens, index + 1, ".") &&
        context->tokens->items[index + 2].kind == AB_TOKEN_IDENTIFIER &&
        (ab_token_equals(context->tokens, index + 3, "==") ||
         ab_token_equals(context->tokens, index + 3, "===")) &&
        context->tokens->items[index + 4].kind == AB_TOKEN_STRING) {
      ArchbirdStatus status =
          add_message_pair(context, index + 2, index + 4, "receive");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  for (index = 0; index + 1 < context->tokens->count; index++) {
    size_t close;
    size_t key_index = SIZE_MAX;
    size_t body_open;
    size_t body_close;
    size_t cursor;
    if (!ab_token_equals(context->tokens, index, "switch") ||
        !ab_token_equals(context->tokens, index + 1, "(") ||
        context->paren_forward[index + 1] == SIZE_MAX)
      continue;
    close = context->paren_forward[index + 1];
    for (cursor = index + 2; cursor + 1 < close; cursor++) {
      if (ab_token_equals(context->tokens, cursor, ".") &&
          context->tokens->items[cursor + 1].kind == AB_TOKEN_IDENTIFIER)
        key_index = cursor + 1;
    }
    body_open = close + 1;
    if (body_open >= context->tokens->count ||
        !ab_token_equals(context->tokens, body_open, "{") ||
        context->brace_forward[body_open] == SIZE_MAX)
      continue;
    body_close = context->brace_forward[body_open];
    for (cursor = body_open + 1; cursor + 1 < body_close; cursor++) {
      if (ab_token_equals(context->tokens, cursor, "{") &&
          context->brace_forward[cursor] != SIZE_MAX) {
        cursor = context->brace_forward[cursor];
        continue;
      }
      if (ab_token_equals(context->tokens, cursor, "case") &&
          context->tokens->items[cursor + 1].kind == AB_TOKEN_STRING) {
        ArchbirdStatus status =
            key_index == SIZE_MAX
                ? add_star_message(context, cursor + 1)
                : add_message_pair(context, key_index, cursor + 1, "receive");
        if (status != ARCHBIRD_OK)
          return status;
      }
    }
  }
  return ARCHBIRD_OK;
}

static int ascii_ci_equal(uint8_t value, char wanted) {
  if (value >= 'A' && value <= 'Z')
    value = (uint8_t)(value - 'A' + 'a');
  return value == (uint8_t)wanted;
}

static int script_name_at(const uint8_t *source, size_t length, size_t offset) {
  static const char name[] = "script";
  size_t index;
  if (sizeof(name) - 1 > length - offset)
    return 0;
  for (index = 0; index < sizeof(name) - 1; index++) {
    if (!ascii_ci_equal(source[offset + index], name[index]))
      return 0;
  }
  return 1;
}

static ArchbirdStatus vue_script_copy(ArchbirdEngine *engine,
                                      const uint8_t *source, size_t length,
                                      uint8_t **out) {
  uint8_t *copy;
  size_t index;
  size_t cursor = 0;
  copy = (uint8_t *)ab_malloc(engine, length ? length : 1);
  if (!copy)
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory extracting Vue scripts");
  for (index = 0; index < length; index++)
    copy[index] = source[index] == '\n' ? '\n' : ' ';
  while (cursor + 8 <= length) {
    size_t tag_end;
    size_t content_start;
    size_t close;
    if (source[cursor] != '<' || !script_name_at(source, length, cursor + 1)) {
      cursor++;
      continue;
    }
    if (cursor + 7 >= length ||
        (source[cursor + 7] != '>' && source[cursor + 7] != ' ' &&
         source[cursor + 7] != '\t' && source[cursor + 7] != '\r' &&
         source[cursor + 7] != '\n')) {
      cursor++;
      continue;
    }
    tag_end = cursor + 7;
    while (tag_end < length && source[tag_end] != '>')
      tag_end++;
    if (tag_end == length)
      break;
    content_start = tag_end + 1;
    close = content_start;
    while (close + 9 <= length) {
      if (source[close] == '<' && source[close + 1] == '/' &&
          script_name_at(source, length, close + 2))
        break;
      close++;
    }
    if (close + 9 > length)
      break;
    memcpy(copy + content_start, source + content_start, close - content_start);
    cursor = close + 8;
  }
  *out = copy;
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_scan_js_file(ArchbirdEngine *engine,
                               const AbSourceManifest *manifest,
                               const AbManifestFile *file,
                               const uint8_t *source, size_t source_length,
                               const uint8_t implementation_sha256[32],
                               AbProviderBundle *out_bundle) {
  static const char config_identity[] = "archbird-native-js-lexical-v1";
  static const char bounded[] = "conservative JavaScript token patterns";
  static const char *const domains[] = {
      "calls",        "exports",          "imports", "messages",
      "method-calls", "module-reexports", "symbols",
  };
  AbBundleBuilder builder;
  AbTokenList tokens;
  JsContext context;
  size_t *brace_depths = NULL;
  uint8_t *vue_source = NULL;
  const uint8_t *scan_source = source;
  uint8_t configuration_sha256[32];
  size_t index;
  size_t depth = 0;
  ArchbirdStatus status;
  memset(&builder, 0, sizeof(builder));
  memset(&tokens, 0, sizeof(tokens));
  memset(&context, 0, sizeof(context));
  memset(out_bundle, 0, sizeof(*out_bundle));
  status = archbird_sha256((const uint8_t *)config_identity,
                           strlen(config_identity), configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_init_file(
      &builder, engine, manifest, file, "archbird-native-js-lexical", "1",
      implementation_sha256, configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; index < sizeof(domains) / sizeof(domains[0]); index++) {
    status = ab_bundle_builder_add_capability(
        &builder, domains[index], "bounded", "lexical-occurrence", bounded);
    if (status != ARCHBIRD_OK)
      goto done;
  }
  if (file->has_language && file->language.length == 3 &&
      memcmp(file->language.data, "vue", 3) == 0) {
    status = vue_script_copy(engine, source, source_length, &vue_source);
    if (status != ARCHBIRD_OK)
      goto done;
    scan_source = vue_source;
  }
  status = ab_tokenize(engine, scan_source, source_length, AB_LEX_JAVASCRIPT,
                       &tokens);
  if (status != ARCHBIRD_OK)
    goto done;
  status = pair_forward(engine, &tokens, "(", ")", &context.paren_forward);
  if (status == ARCHBIRD_OK)
    status = pair_forward(engine, &tokens, "{", "}", &context.brace_forward);
  if (status != ARCHBIRD_OK)
    goto done;
  context.definition_indices = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  brace_depths =
      (size_t *)ab_calloc(engine, tokens.count, sizeof(*brace_depths));
  if (tokens.count && (!context.definition_indices || !brace_depths)) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory scanning JavaScript source");
    goto done;
  }
  context.builder = &builder;
  context.tokens = &tokens;
  for (index = 0; index < tokens.count; index++) {
    if (ab_token_equals(&tokens, index, "}"))
      depth = depth ? depth - 1 : 0;
    brace_depths[index] = depth;
    if (ab_token_equals(&tokens, index, "{"))
      depth++;
  }
  status = scan_declared_symbols(&context, brace_depths);
  if (status == ARCHBIRD_OK)
    status = scan_top_level_bindings(&context);
  if (status == ARCHBIRD_OK)
    status = scan_calls(&context);
  if (status == ARCHBIRD_OK)
    status = scan_imports(&context);
  if (status == ARCHBIRD_OK)
    status = scan_esm_exports(&context);
  if (status == ARCHBIRD_OK)
    status = scan_commonjs_exports(&context);
  if (status == ARCHBIRD_OK)
    status = scan_messages(&context);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&builder, out_bundle);
done:
  ab_free(engine, vue_source);
  ab_free(engine, brace_depths);
  ab_free(engine, context.definition_indices);
  ab_free(engine, context.paren_forward);
  ab_free(engine, context.brace_forward);
  ab_token_list_free(&tokens);
  ab_bundle_builder_abort(&builder);
  return status;
}
