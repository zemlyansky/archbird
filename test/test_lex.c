#include "lexical/tokenizer.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect_tokens(ArchbirdEngine *engine, const char *name,
                          const char *source, uint32_t flags,
                          const char *const *values, const size_t *lines,
                          size_t count) {
  AbTokenList tokens;
  ArchbirdStatus status = ab_tokenize(engine, (const uint8_t *)source,
                                      strlen(source), flags, &tokens);
  size_t index;
  if (status != ARCHBIRD_OK) {
    fprintf(stderr, "FAIL %s: %s\n", name, archbird_engine_error(engine));
    failures++;
    return;
  }
  if (tokens.count != count) {
    fprintf(stderr, "FAIL %s: %zu tokens, expected %zu\n", name, tokens.count,
            count);
    failures++;
  }
  for (index = 0; index < tokens.count && index < count; index++) {
    if (!ab_token_equals(&tokens, index, values[index]) ||
        tokens.items[index].line != lines[index]) {
      fprintf(stderr, "FAIL %s token %zu\n", name, index);
      failures++;
    }
  }
  ab_token_list_free(&tokens);
}

int main(void) {
  static const char *const c_values[] = {"int", "api",    "(", "int", "x", ")",
                                         "{",   "return", "x", ";",   "}"};
  static const size_t c_lines[] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
  static const char *const js_values[] = {
      "input", ".", "replace", "(", "/https?:\\/\\//g", ",", "''", ")", ";",
      "left",  "/", "right",   ";",
  };
  static const size_t js_lines[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  static const char *const jsx_values[] = {
      "<", "div", ">", "Here", "'",      "s",       "code", "<",
      "/", "div", ">", ";",    "return", "'valid'", ";",
  };
  static const size_t jsx_lines[] = {1, 1, 1, 1, 1, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1};
  ArchbirdEngineOptions options;
  ArchbirdEngine *engine = NULL;
  AbTokenList invalid;
  const uint8_t invalid_source[] = {0xc0, 0xaf};
  archbird_engine_options_init(&options);
  if (archbird_engine_create(&options, &engine) != ARCHBIRD_OK)
    return 1;
  expect_tokens(engine, "c-preprocessor",
                "#define fake() 1\n/* fake(); */\nint api(int x) { return x; }",
                AB_LEX_C_PREPROCESSOR, c_values, c_lines,
                sizeof(c_values) / sizeof(c_values[0]));
  expect_tokens(engine, "javascript-regex",
                "input.replace(/https?:\\/\\//g, ''); left / right;",
                AB_LEX_JAVASCRIPT, js_values, js_lines,
                sizeof(js_values) / sizeof(js_values[0]));
  expect_tokens(engine, "javascript-jsx-apostrophe",
                "<div>Here's code</div>; return'valid';", AB_LEX_JAVASCRIPT,
                jsx_values, jsx_lines,
                sizeof(jsx_values) / sizeof(jsx_values[0]));
  if (ab_tokenize(engine, invalid_source, sizeof(invalid_source), 0,
                  &invalid) != ARCHBIRD_INVALID_SCHEMA) {
    fputs("FAIL invalid UTF-8 accepted\n", stderr);
    failures++;
  }
  archbird_engine_destroy(engine);
  if (failures) {
    fprintf(stderr, "%d lexical test(s) failed\n", failures);
    return 1;
  }
  puts("native lexical tokenizer tests passed");
  return 0;
}
