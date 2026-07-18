#include "lexical/r/scanner.h"

#include "lexical/tokenizer.h"
#include "sha256.h"

#include <string.h>

static const uint8_t *token_data(const AbTokenList *tokens, size_t index) {
  return tokens->source + tokens->items[index].start;
}

static size_t token_length(const AbTokenList *tokens, size_t index) {
  return tokens->items[index].end - tokens->items[index].start;
}

static int line_prefix_is_space(const AbTokenList *tokens, size_t index) {
  size_t cursor = tokens->items[index].start;
  while (cursor > 0 && tokens->source[cursor - 1] != '\n' &&
         tokens->source[cursor - 1] != '\r')
    cursor--;
  while (cursor < tokens->items[index].start) {
    uint8_t value = tokens->source[cursor++];
    if (value != ' ' && value != '\t' && value != '\v' && value != '\f')
      return 0;
  }
  return 1;
}

static ArchbirdStatus add_line(AbBundleBuilder *builder, AbFact *fact,
                               size_t line) {
  return ab_fact_add_u64_attribute(builder->engine, fact, "line", line);
}

static ArchbirdStatus add_r_symbol(AbBundleBuilder *builder,
                                   const AbTokenList *tokens,
                                   size_t name_index) {
  const AbToken *token = &tokens->items[name_index];
  const uint8_t *name = token_data(tokens, name_index);
  size_t length = token_length(tokens, name_index);
  AbFact *fact;
  ArchbirdStatus status = ab_bundle_builder_add_fact(
      builder, "symbols", "function", "lexical-occurrence", token->start,
      token->end, name, length, name, length, &fact);
  if (status == ARCHBIRD_OK)
    status = add_line(builder, fact, token->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(builder->engine, fact, "scope",
                                          (const uint8_t *)"function", 8);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(builder->engine, fact, "signature",
                                          name, length);
  return status;
}

static ArchbirdStatus add_r_name_fact(AbBundleBuilder *builder,
                                      const AbTokenList *tokens,
                                      size_t token_index, size_t skip,
                                      const char *domain, const char *kind) {
  const AbToken *token = &tokens->items[token_index];
  const uint8_t *name = token_data(tokens, token_index) + skip;
  size_t length = token_length(tokens, token_index) - skip;
  AbFact *fact;
  ArchbirdStatus status;
  if (!length)
    return ARCHBIRD_OK;
  status = ab_bundle_builder_add_fact(
      builder, domain, kind, "lexical-occurrence", token->start + skip,
      token->end, name, length, name, length, &fact);
  if (status == ARCHBIRD_OK)
    status = add_line(builder, fact, token->line);
  return status;
}

static size_t leading_dots(const AbTokenList *tokens, size_t index) {
  const uint8_t *name = token_data(tokens, index);
  size_t length = token_length(tokens, index);
  size_t count = 0;
  while (count < length && name[count] == '.')
    count++;
  return count;
}

static ArchbirdStatus scan_r_symbols(AbBundleBuilder *builder,
                                     const AbTokenList *tokens) {
  size_t index;
  for (index = 0; index + 2 < tokens->count; index++) {
    int assignment;
    size_t function_index;
    if (tokens->items[index].kind != AB_TOKEN_IDENTIFIER ||
        !line_prefix_is_space(tokens, index))
      continue;
    assignment = ab_token_equals(tokens, index + 1, "=");
    function_index = index + 2;
    if (!assignment && index + 3 < tokens->count &&
        ab_token_equals(tokens, index + 1, "<") &&
        ab_token_equals(tokens, index + 2, "-")) {
      assignment = 1;
      function_index = index + 3;
    }
    if (!assignment || function_index >= tokens->count ||
        !ab_token_equals(tokens, function_index, "function"))
      continue;
    {
      ArchbirdStatus status = add_r_symbol(builder, tokens, index);
      if (status != ARCHBIRD_OK)
        return status;
      if (token_data(tokens, index)[0] != '.') {
        status =
            add_r_name_fact(builder, tokens, index, 0, "exports", "export");
        if (status != ARCHBIRD_OK)
          return status;
      }
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus scan_r_calls(AbBundleBuilder *builder,
                                   const AbTokenList *tokens) {
  size_t index;
  for (index = 0; index + 1 < tokens->count; index++) {
    size_t skip;
    if (tokens->items[index].kind != AB_TOKEN_IDENTIFIER ||
        !ab_token_equals(tokens, index + 1, "("))
      continue;
    skip = leading_dots(tokens, index);
    {
      ArchbirdStatus status =
          add_r_name_fact(builder, tokens, index, skip, "calls", "call");
      if (status != ARCHBIRD_OK)
        return status;
    }
    if (ab_token_equals(tokens, index, ".Call") && index + 2 < tokens->count &&
        tokens->items[index + 2].kind == AB_TOKEN_IDENTIFIER) {
      ArchbirdStatus status =
          add_r_name_fact(builder, tokens, index + 2, 0, "calls", "call");
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_scan_r_file(ArchbirdEngine *engine,
                              const AbSourceManifest *manifest,
                              const AbManifestFile *file, const uint8_t *source,
                              size_t source_length,
                              const uint8_t implementation_sha256[32],
                              AbProviderBundle *out_bundle) {
  static const char config_identity[] = "archbird-native-r-lexical-v1";
  static const char boundary_calls[] =
      "identifier followed by '(' plus the first bare .Call target";
  static const char boundary_exports[] =
      "non-dot-prefixed top-level function assignments";
  static const char boundary_symbols[] =
      "line-leading name <- function and name = function assignments";
  AbBundleBuilder builder;
  AbTokenList tokens;
  uint8_t configuration_sha256[32];
  ArchbirdStatus status;
  memset(&builder, 0, sizeof(builder));
  memset(&tokens, 0, sizeof(tokens));
  memset(out_bundle, 0, sizeof(*out_bundle));
  status = archbird_sha256((const uint8_t *)config_identity,
                           strlen(config_identity), configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_init_file(
      &builder, engine, manifest, file, "archbird-native-r-lexical", "1",
      implementation_sha256, configuration_sha256);
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
  status = ab_tokenize(engine, source, source_length, AB_LEX_R, &tokens);
  if (status == ARCHBIRD_OK)
    status = scan_r_symbols(&builder, &tokens);
  if (status == ARCHBIRD_OK)
    status = scan_r_calls(&builder, &tokens);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&builder, out_bundle);
done:
  ab_token_list_free(&tokens);
  ab_bundle_builder_abort(&builder);
  return status;
}
