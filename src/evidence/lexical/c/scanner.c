#include "lexical/c/scanner.h"

#include "lexical/tokenizer.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static const char *const C_CONTROL_WORDS[] = {
    "if",     "for",      "while",    "switch", "return",
    "sizeof", "_Alignof", "_Generic", "do",     "case",
};

static const char *const C_TYPE_WORDS[] = {
    "_Atomic",   "_Bool",    "_Complex", "bool",     "char",      "const",
    "double",    "enum",     "float",    "int",      "int8_t",    "int16_t",
    "int32_t",   "int64_t",  "intptr_t", "long",     "ptrdiff_t", "register",
    "restrict",  "short",    "signed",   "size_t",   "ssize_t",   "struct",
    "union",     "unsigned", "uint8_t",  "uint16_t", "uint32_t",  "uint64_t",
    "uintptr_t", "void",     "volatile", "wchar_t",
};

static AbNameRef token_ref(const AbTokenList *tokens, size_t index) {
  const AbToken *token = &tokens->items[index];
  return (AbNameRef){tokens->source + token->start, token->end - token->start};
}

static int name_equal(AbNameRef left, AbNameRef right) {
  return left.length == right.length &&
         (left.length == 0 || memcmp(left.data, right.data, left.length) == 0);
}

static ArchbirdStatus name_set_add(ArchbirdEngine *engine, AbNameSet *names,
                                   AbNameRef name) {
  AbNameRef *resized;
  size_t index;
  if (!names->engine)
    names->engine = engine;
  else if (names->engine != engine)
    return archbird_error_set(engine, ARCHBIRD_CONFLICT, ARCHBIRD_NO_OFFSET,
                              "lexical name set belongs to another engine");
  for (index = 0; index < names->count; index++) {
    if (name_equal(names->items[index], name))
      return ARCHBIRD_OK;
  }
  if (names->count == names->capacity) {
    size_t capacity = names->capacity ? names->capacity * 2 : 32;
    resized = (AbNameRef *)ab_realloc(engine, names->items,
                                      capacity * sizeof(*names->items));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing lexical name set");
    names->items = resized;
    names->capacity = capacity;
  }
  names->items[names->count++] = name;
  return ARCHBIRD_OK;
}

static int name_set_contains(const AbNameSet *names, AbNameRef name) {
  size_t index;
  for (index = 0; index < names->count; index++) {
    if (name_equal(names->items[index], name))
      return 1;
  }
  return 0;
}

void ab_name_set_free(AbNameSet *names) {
  if (!names)
    return;
  ab_free(names->engine, names->items);
  memset(names, 0, sizeof(*names));
}

static int token_in_words(const AbTokenList *tokens, size_t index,
                          const char *const *words, size_t word_count) {
  size_t word_index;
  for (word_index = 0; word_index < word_count; word_index++) {
    if (ab_token_equals(tokens, index, words[word_index]))
      return 1;
  }
  return 0;
}

static int c_control(const AbTokenList *tokens, size_t index) {
  return token_in_words(tokens, index, C_CONTROL_WORDS,
                        sizeof(C_CONTROL_WORDS) / sizeof(C_CONTROL_WORDS[0]));
}

static int c_type_word(const AbTokenList *tokens, size_t index) {
  return token_in_words(tokens, index, C_TYPE_WORDS,
                        sizeof(C_TYPE_WORDS) / sizeof(C_TYPE_WORDS[0]));
}

static int token_upper(const AbTokenList *tokens, size_t index) {
  AbNameRef name = token_ref(tokens, index);
  size_t cursor;
  int has_cased = 0;
  for (cursor = 0; cursor < name.length; cursor++) {
    uint8_t value = name.data[cursor];
    if (value >= 'a' && value <= 'z')
      return 0;
    if (value >= 'A' && value <= 'Z')
      has_cased = 1;
  }
  return has_cased;
}

static int segment_contains(const AbTokenList *tokens, size_t start, size_t end,
                            const char *literal) {
  size_t index;
  for (index = start; index < end; index++) {
    if (ab_token_equals(tokens, index, literal))
      return 1;
  }
  return 0;
}

static ArchbirdStatus pair_reverse(ArchbirdEngine *engine,
                                   const AbTokenList *tokens,
                                   const char *opening, const char *closing,
                                   size_t **out_reverse) {
  size_t *reverse;
  size_t *stack;
  size_t stack_count = 0;
  size_t index;
  *out_reverse = NULL;
  if (!tokens->count)
    return ARCHBIRD_OK;
  reverse = (size_t *)ab_malloc(engine, tokens->count * sizeof(*reverse));
  stack = (size_t *)ab_malloc(engine, tokens->count * sizeof(*stack));
  if (!reverse || !stack) {
    ab_free(engine, reverse);
    ab_free(engine, stack);
    return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory pairing lexical tokens");
  }
  for (index = 0; index < tokens->count; index++) {
    reverse[index] = SIZE_MAX;
    if (ab_token_equals(tokens, index, opening)) {
      stack[stack_count++] = index;
    } else if (ab_token_equals(tokens, index, closing) && stack_count) {
      reverse[index] = stack[--stack_count];
    }
  }
  ab_free(engine, stack);
  *out_reverse = reverse;
  return ARCHBIRD_OK;
}

static int signature_no_space_before(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, ",") ||
         ab_token_equals(tokens, index, ";") ||
         ab_token_equals(tokens, index, "(") ||
         ab_token_equals(tokens, index, ")") ||
         ab_token_equals(tokens, index, "[") ||
         ab_token_equals(tokens, index, "]");
}

static int signature_no_space_after(const AbTokenList *tokens, size_t index) {
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

static ArchbirdStatus normalized_signature(ArchbirdEngine *engine,
                                           const AbTokenList *tokens,
                                           size_t start, size_t end,
                                           AbBuffer *out) {
  AbBuffer full;
  size_t index;
  size_t codepoints = 0;
  size_t cursor = 0;
  ab_buffer_init(out, engine);
  ab_buffer_init(&full, engine);
  for (index = start; index < end; index++) {
    const AbToken *token = &tokens->items[index];
    if (index > start && !signature_no_space_before(tokens, index) &&
        !signature_no_space_after(tokens, index - 1)) {
      ArchbirdStatus status = ab_buffer_literal(&full, " ");
      if (status != ARCHBIRD_OK) {
        ab_buffer_free(&full);
        return status;
      }
    }
    {
      ArchbirdStatus status = ab_buffer_append(
          &full, tokens->source + token->start, token->end - token->start);
      if (status != ARCHBIRD_OK) {
        ab_buffer_free(&full);
        return status;
      }
    }
  }
  while (cursor < full.length) {
    codepoints++;
    cursor += utf8_width(full.data[cursor]);
  }
  if (codepoints <= 220) {
    ArchbirdStatus status = ab_buffer_append(out, full.data, full.length);
    ab_buffer_free(&full);
    return status;
  }
  cursor = 0;
  codepoints = 0;
  while (cursor < full.length && codepoints < 219) {
    cursor += utf8_width(full.data[cursor]);
    codepoints++;
  }
  {
    static const uint8_t ellipsis[] = {0xe2, 0x80, 0xa6};
    ArchbirdStatus status = ab_buffer_append(out, full.data, cursor);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_append(out, ellipsis, sizeof(ellipsis));
    ab_buffer_free(&full);
    return status;
  }
}

static ArchbirdStatus add_symbol(AbBundleBuilder *builder,
                                 const AbTokenList *tokens, size_t name_index,
                                 const char *kind, const char *scope,
                                 size_t signature_start, size_t signature_end) {
  const AbToken *name_token = &tokens->items[name_index];
  AbNameRef name = token_ref(tokens, name_index);
  AbFact *fact;
  AbBuffer signature;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, "symbols", kind, "lexical-occurrence", name_token->start,
      name_token->end, name.data, name.length, name.data, name.length, &fact);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_fact_add_u64_attribute(builder->engine, fact, "line",
                                     name_token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        builder->engine, fact, "scope", (const uint8_t *)scope, strlen(scope));
  if (status != ARCHBIRD_OK)
    return status;
  status = normalized_signature(builder->engine, tokens, signature_start,
                                signature_end, &signature);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(builder->engine, fact, "signature",
                                          signature.data, signature.length);
  ab_buffer_free(&signature);
  return status;
}

static const char *symbol_scope(const AbTokenList *tokens, size_t start,
                                size_t end, size_t name_index,
                                const AbNameSet *public_names) {
  if (name_set_contains(public_names, token_ref(tokens, name_index)))
    return "public";
  if (segment_contains(tokens, start, end, "static"))
    return "local";
  return "global";
}

static ArchbirdStatus collect_typedef_names(ArchbirdEngine *engine,
                                            const AbTokenList *tokens,
                                            AbNameSet *names) {
  size_t start = 0;
  size_t brace_depth = 0;
  size_t index;
  for (index = 0; index < tokens->count; index++) {
    size_t cursor;
    int found_pointer = 0;
    size_t candidate = SIZE_MAX;
    if (ab_token_equals(tokens, index, "{"))
      brace_depth++;
    else if (ab_token_equals(tokens, index, "}"))
      brace_depth = brace_depth ? brace_depth - 1 : 0;
    if (!ab_token_equals(tokens, index, ";") || brace_depth != 0)
      continue;
    if (start == index || !segment_contains(tokens, start, index, "typedef")) {
      start = index + 1;
      continue;
    }
    for (cursor = start; cursor + 2 < index; cursor++) {
      if (ab_token_equals(tokens, cursor, "*") &&
          tokens->items[cursor + 1].kind == AB_TOKEN_IDENTIFIER &&
          ab_token_equals(tokens, cursor + 2, ")")) {
        ArchbirdStatus status =
            name_set_add(engine, names, token_ref(tokens, cursor + 1));
        if (status != ARCHBIRD_OK)
          return status;
        found_pointer = 1;
      }
    }
    if (!found_pointer) {
      size_t paren_depth = 0;
      for (cursor = start; cursor < index; cursor++) {
        if (ab_token_equals(tokens, cursor, "(")) {
          paren_depth++;
        } else if (ab_token_equals(tokens, cursor, ")")) {
          paren_depth = paren_depth ? paren_depth - 1 : 0;
        } else if (paren_depth == 0 &&
                   tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER &&
                   !c_type_word(tokens, cursor) &&
                   !ab_token_equals(tokens, cursor, "typedef")) {
          candidate = cursor;
        }
      }
      if (candidate != SIZE_MAX) {
        ArchbirdStatus status =
            name_set_add(engine, names, token_ref(tokens, candidate));
        if (status != ARCHBIRD_OK)
          return status;
      }
    }
    start = index + 1;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_declarations(AbBundleBuilder *builder,
                                        const AbTokenList *tokens,
                                        const AbNameSet *public_names,
                                        uint8_t *declaration_indices) {
  int *linkage_stack = NULL;
  size_t linkage_count = 0;
  size_t brace_depth = 0;
  size_t start = 0;
  size_t index;
  if (tokens->count) {
    linkage_stack = (int *)ab_calloc(builder->engine, tokens->count,
                                     sizeof(*linkage_stack));
    if (!linkage_stack)
      return archbird_error_set(builder->engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory scanning C declarations");
  }
  for (index = 0; index < tokens->count; index++) {
    if (ab_token_equals(tokens, index, "{")) {
      int linkage = index >= 2 &&
                    ab_token_equals(tokens, index - 2, "extern") &&
                    (ab_token_equals(tokens, index - 1, "\"C\"") ||
                     ab_token_equals(tokens, index - 1, "'C'"));
      linkage_stack[linkage_count++] = linkage;
      if (linkage) {
        if (brace_depth == 0)
          start = index + 1;
      } else {
        brace_depth++;
      }
      continue;
    }
    if (ab_token_equals(tokens, index, "}")) {
      int linkage = linkage_count ? linkage_stack[--linkage_count] : 0;
      if (!linkage)
        brace_depth = brace_depth ? brace_depth - 1 : 0;
      if (brace_depth == 0)
        start = index + 1;
      continue;
    }
    if (!ab_token_equals(tokens, index, ";") || brace_depth != 0)
      continue;
    if (!segment_contains(tokens, start, index, "=") &&
        !segment_contains(tokens, start, index, "typedef")) {
      size_t cursor;
      size_t paren_depth = 0;
      for (cursor = start; cursor + 1 < index; cursor++) {
        if (ab_token_equals(tokens, cursor, "(")) {
          paren_depth++;
        } else if (ab_token_equals(tokens, cursor, ")")) {
          paren_depth = paren_depth ? paren_depth - 1 : 0;
        } else if (paren_depth == 0 &&
                   tokens->items[cursor].kind == AB_TOKEN_IDENTIFIER &&
                   !c_control(tokens, cursor) && !token_upper(tokens, cursor) &&
                   ab_token_equals(tokens, cursor + 1, "(")) {
          ArchbirdStatus status = add_symbol(
              builder, tokens, cursor, "declaration",
              symbol_scope(tokens, start, index, cursor, public_names), start,
              index);
          if (status != ARCHBIRD_OK) {
            ab_free(builder->engine, linkage_stack);
            return status;
          }
          declaration_indices[cursor] = 1;
          break;
        }
      }
    }
    start = index + 1;
  }
  ab_free(builder->engine, linkage_stack);
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_definitions(AbBundleBuilder *builder,
                                       const AbTokenList *tokens,
                                       const size_t *paren_reverse,
                                       const AbNameSet *public_names,
                                       uint8_t *definition_indices) {
  size_t brace_depth = 0;
  size_t boundary = 0;
  size_t index;
  for (index = 0; index < tokens->count; index++) {
    if (ab_token_equals(tokens, index, "}")) {
      brace_depth = brace_depth ? brace_depth - 1 : 0;
      if (brace_depth == 0)
        boundary = index + 1;
      continue;
    }
    if (ab_token_equals(tokens, index, ";") && brace_depth == 0) {
      boundary = index + 1;
      continue;
    }
    if (!ab_token_equals(tokens, index, "{"))
      continue;
    if (brace_depth == 0 && index > 1 &&
        ab_token_equals(tokens, index - 1, ")") &&
        paren_reverse[index - 1] != SIZE_MAX && paren_reverse[index - 1] > 0) {
      size_t name_index = paren_reverse[index - 1] - 1;
      if (tokens->items[name_index].kind == AB_TOKEN_IDENTIFIER &&
          !c_control(tokens, name_index) &&
          !ab_token_equals(tokens, name_index, "TEST") &&
          !token_upper(tokens, name_index) &&
          !ab_token_has_prefix(tokens, name_index, "EM_") &&
          !segment_contains(tokens, boundary, name_index, "=") &&
          !segment_contains(tokens, boundary, name_index, "typedef")) {
        ArchbirdStatus status =
            add_symbol(builder, tokens, name_index, "function",
                       symbol_scope(tokens, boundary, name_index, name_index,
                                    public_names),
                       boundary, index);
        if (status != ARCHBIRD_OK)
          return status;
        definition_indices[name_index] = 1;
      }
    }
    brace_depth++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_call_fact(AbBundleBuilder *builder,
                                    const AbTokenList *tokens, size_t index) {
  AbNameRef name = token_ref(tokens, index);
  const AbToken *token = &tokens->items[index];
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, "calls", "call", "lexical-occurrence", token->start, token->end,
      name.data, name.length, name.data, name.length, &fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "line", token->line);
  return status;
}

static ArchbirdStatus scan_calls(AbBundleBuilder *builder,
                                 const AbTokenList *tokens,
                                 const AbNameSet *typedef_names,
                                 const uint8_t *declaration_indices,
                                 const uint8_t *definition_indices) {
  size_t index;
  for (index = 0; index + 1 < tokens->count; index++) {
    AbNameRef name;
    if (tokens->items[index].kind != AB_TOKEN_IDENTIFIER ||
        !ab_token_equals(tokens, index + 1, "("))
      continue;
    name = token_ref(tokens, index);
    if (c_control(tokens, index) || c_type_word(tokens, index) ||
        name_set_contains(typedef_names, name) || token_upper(tokens, index) ||
        definition_indices[index] || declaration_indices[index])
      continue;
    if (index > 0 && (ab_token_equals(tokens, index - 1, "#") ||
                      ab_token_equals(tokens, index - 1, ".")))
      continue;
    {
      ArchbirdStatus status = add_call_fact(builder, tokens, index);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_export_fact(AbBundleBuilder *builder,
                                      const AbTokenList *tokens,
                                      size_t string_index) {
  const AbToken *token = &tokens->items[string_index];
  const uint8_t *name = tokens->source + token->start + 1;
  size_t length = token->end - token->start - 2;
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, "exports", "export", "lexical-occurrence", token->start + 1,
      token->end - 1, name, length, name, length, &fact);
  if (status == ARCHBIRD_OK)
    status =
        ab_fact_add_u64_attribute(builder->engine, fact, "line", token->line);
  return status;
}

static ArchbirdStatus scan_exports(AbBundleBuilder *builder,
                                   const AbTokenList *tokens) {
  size_t index;
  for (index = 0; index < tokens->count; index++) {
    size_t string_index = SIZE_MAX;
    if (index + 2 < tokens->count &&
        ab_token_equals(tokens, index, "DECLARE_NAPI_METHOD") &&
        ab_token_equals(tokens, index + 1, "(") &&
        tokens->items[index + 2].kind == AB_TOKEN_STRING) {
      string_index = index + 2;
    } else if (index + 5 < tokens->count &&
               ab_token_equals(tokens, index, "{") &&
               tokens->items[index + 1].kind == AB_TOKEN_STRING &&
               ab_token_equals(tokens, index + 2, ",") &&
               (ab_token_equals(tokens, index + 3, "NULL") ||
                ab_token_equals(tokens, index + 3, "nullptr")) &&
               ab_token_equals(tokens, index + 4, ",") &&
               tokens->items[index + 5].kind == AB_TOKEN_IDENTIFIER &&
               ab_token_has_prefix(tokens, index + 5, "napi_")) {
      string_index = index + 1;
    }
    if (string_index != SIZE_MAX) {
      ArchbirdStatus status = add_export_fact(builder, tokens, string_index);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_c_collect_public_names(ArchbirdEngine *engine,
                                         const uint8_t *source,
                                         size_t source_length,
                                         AbNameSet *names) {
  AbTokenList tokens;
  size_t *brace_reverse = NULL;
  uint8_t *transparent_open = NULL;
  uint8_t *transparent_close = NULL;
  size_t depth = 0;
  size_t start = 0;
  size_t index;
  ArchbirdStatus status = ab_tokenize(engine, source, source_length,
                                      AB_LEX_C_PREPROCESSOR, &tokens);
  if (status != ARCHBIRD_OK)
    return status;
  status = pair_reverse(engine, &tokens, "{", "}", &brace_reverse);
  if (status != ARCHBIRD_OK)
    goto done;
  transparent_open = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  transparent_close = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  if (tokens.count && (!transparent_open || !transparent_close)) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory scanning public C header");
    goto done;
  }
  for (index = 0; index < tokens.count; index++) {
    if (ab_token_equals(&tokens, index, "}") &&
        brace_reverse[index] != SIZE_MAX) {
      size_t opening = brace_reverse[index];
      if (opening >= 2 && ab_token_equals(&tokens, opening - 2, "extern") &&
          tokens.items[opening - 1].kind == AB_TOKEN_STRING &&
          (ab_token_equals(&tokens, opening - 1, "\"C\"") ||
           ab_token_equals(&tokens, opening - 1, "'C'"))) {
        transparent_open[opening] = 1;
        transparent_close[index] = 1;
      }
    }
  }
  for (index = 0; index < tokens.count; index++) {
    if (transparent_open[index] || transparent_close[index]) {
      start = index + 1;
      continue;
    }
    if (ab_token_equals(&tokens, index, "{")) {
      depth++;
      continue;
    }
    if (ab_token_equals(&tokens, index, "}")) {
      depth = depth ? depth - 1 : 0;
      if (depth == 0)
        start = index + 1;
      continue;
    }
    if (!ab_token_equals(&tokens, index, ";") || depth != 0)
      continue;
    if (!segment_contains(&tokens, start, index, "=") &&
        !segment_contains(&tokens, start, index, "typedef") &&
        !segment_contains(&tokens, start, index, "static")) {
      size_t cursor;
      size_t paren_depth = 0;
      for (cursor = start; cursor + 1 < index; cursor++) {
        if (ab_token_equals(&tokens, cursor, "(")) {
          paren_depth++;
        } else if (ab_token_equals(&tokens, cursor, ")")) {
          paren_depth = paren_depth ? paren_depth - 1 : 0;
        } else if (paren_depth == 0 &&
                   tokens.items[cursor].kind == AB_TOKEN_IDENTIFIER &&
                   !c_control(&tokens, cursor) &&
                   !token_upper(&tokens, cursor) &&
                   ab_token_equals(&tokens, cursor + 1, "(")) {
          status = name_set_add(engine, names, token_ref(&tokens, cursor));
          if (status != ARCHBIRD_OK)
            goto done;
          break;
        }
      }
    }
    start = index + 1;
  }
done:
  ab_free(engine, transparent_open);
  ab_free(engine, transparent_close);
  ab_free(engine, brace_reverse);
  ab_token_list_free(&tokens);
  return status;
}

ArchbirdStatus ab_scan_c_file(
    ArchbirdEngine *engine, const AbSourceManifest *manifest,
    const AbManifestFile *file, const uint8_t *source, size_t source_length,
    const uint8_t source_manifest_sha256[32], const AbNameSet *public_names,
    const uint8_t implementation_sha256[32], AbProviderBundle *out_bundle) {
  static const char config_identity[] = "archbird-native-c-lexical-v1";
  static const char boundary_calls[] =
      "direct identifier calls excluding controls, known types, typedefs, "
      "declarations, definitions, uppercase macros, and member access";
  static const char boundary_exports[] =
      "DECLARE_NAPI_METHOD and napi_property_descriptor literal names";
  static const char boundary_symbols[] =
      "top-level function definitions and prototypes inferred from tokens";
  AbBundleBuilder builder;
  AbTokenList tokens;
  AbNameSet typedef_names = {0};
  size_t *paren_reverse = NULL;
  uint8_t *declaration_indices = NULL;
  uint8_t *definition_indices = NULL;
  uint8_t configuration_sha256[32];
  ArchbirdStatus status;
  memset(&builder, 0, sizeof(builder));
  memset(&tokens, 0, sizeof(tokens));
  memset(out_bundle, 0, sizeof(*out_bundle));
  status = archbird_sha256((const uint8_t *)config_identity,
                           strlen(config_identity), configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_init_file_manifest(
      &builder, engine, manifest, file, source_manifest_sha256,
      "archbird-native-c-lexical", "1", implementation_sha256,
      configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_add_capability(
      &builder, "calls", "bounded", "lexical-occurrence", boundary_calls);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(
        &builder, "exports", "bounded", "lexical-occurrence", boundary_exports);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_add_capability(
        &builder, "symbols", "bounded", "lexical-occurrence", boundary_symbols);
  if (status != ARCHBIRD_OK)
    goto done;
  status = ab_tokenize(engine, source, source_length, AB_LEX_C_PREPROCESSOR,
                       &tokens);
  if (status != ARCHBIRD_OK)
    goto done;
  status = collect_typedef_names(engine, &tokens, &typedef_names);
  if (status == ARCHBIRD_OK)
    status = pair_reverse(engine, &tokens, "(", ")", &paren_reverse);
  if (status != ARCHBIRD_OK)
    goto done;
  declaration_indices = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  definition_indices = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  if (tokens.count && (!declaration_indices || !definition_indices)) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory scanning C source");
    goto done;
  }
  status =
      scan_declarations(&builder, &tokens, public_names, declaration_indices);
  if (status == ARCHBIRD_OK)
    status = scan_definitions(&builder, &tokens, paren_reverse, public_names,
                              definition_indices);
  if (status == ARCHBIRD_OK)
    status = scan_calls(&builder, &tokens, &typedef_names, declaration_indices,
                        definition_indices);
  if (status == ARCHBIRD_OK)
    status = scan_exports(&builder, &tokens);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&builder, out_bundle);
done:
  ab_free(engine, paren_reverse);
  ab_free(engine, declaration_indices);
  ab_free(engine, definition_indices);
  ab_name_set_free(&typedef_names);
  ab_token_list_free(&tokens);
  ab_bundle_builder_abort(&builder);
  return status;
}
