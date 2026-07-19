#include "lexical/python/scanner.h"

#include "lexical/tokenizer.h"
#include "python/definitions.h"
#include "python/tokens.h"
#include "render_internal.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct PyContext {
  AbBundleBuilder *builder;
  const AbTokenList *tokens;
  const AbPythonDefinitions *definitions;
  uint8_t *definition_names;
  int init_module;
} PyContext;

static int path_has_suffix(const AbString *path, const char *suffix) {
  size_t length = strlen(suffix);
  return path->length >= length &&
         memcmp(path->data + path->length - length, suffix, length) == 0;
}

static int init_module_path(const AbString *path) {
  static const char root[] = "__init__.py";
  static const char nested[] = "/__init__.py";
  return (path->length == sizeof(root) - 1 &&
          !memcmp(path->data, root, sizeof(root) - 1)) ||
         path_has_suffix(path, nested);
}

static int no_space_before(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, ",") ||
         ab_token_equals(tokens, index, ":") ||
         ab_token_equals(tokens, index, ";") ||
         ab_token_equals(tokens, index, ".") ||
         ab_token_equals(tokens, index, "(") ||
         ab_token_equals(tokens, index, ")") ||
         ab_token_equals(tokens, index, "[") ||
         ab_token_equals(tokens, index, "]");
}

static int no_space_after(const AbTokenList *tokens, size_t index) {
  return ab_token_equals(tokens, index, ".") ||
         ab_token_equals(tokens, index, "(") ||
         ab_token_equals(tokens, index, "[");
}

static ArchbirdStatus signature(AbBuffer *buffer, const AbTokenList *tokens,
                                size_t start, size_t end) {
  static const uint8_t ellipsis[] = {0xe2, 0x80, 0xa6};
  size_t index;
  for (index = start; index < end; index++) {
    const AbToken *token = &tokens->items[index];
    size_t width = token->end - token->start;
    int space = index > start && !no_space_before(tokens, index) &&
                !no_space_after(tokens, index - 1);
    if (buffer->length + (space ? 1u : 0u) + width > 217)
      return ab_buffer_append(buffer, ellipsis, sizeof(ellipsis));
    if (space) {
      ArchbirdStatus status = ab_buffer_literal(buffer, " ");
      if (status != ARCHBIRD_OK)
        return status;
    }
    {
      ArchbirdStatus status =
          ab_buffer_append(buffer, tokens->source + token->start, width);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static size_t definition_end(const AbTokenList *tokens, size_t start) {
  size_t parens = 0;
  size_t brackets = 0;
  size_t braces = 0;
  size_t index;
  for (index = start; index < tokens->count; index++) {
    if (ab_token_equals(tokens, index, "("))
      parens++;
    else if (ab_token_equals(tokens, index, ")"))
      parens = parens ? parens - 1 : 0;
    else if (ab_token_equals(tokens, index, "["))
      brackets++;
    else if (ab_token_equals(tokens, index, "]"))
      brackets = brackets ? brackets - 1 : 0;
    else if (ab_token_equals(tokens, index, "{"))
      braces++;
    else if (ab_token_equals(tokens, index, "}"))
      braces = braces ? braces - 1 : 0;
    else if (!parens && !brackets && !braces &&
             ab_token_equals(tokens, index, ":"))
      return index;
    if (index > start && !parens && !brackets && !braces &&
        tokens->items[index].line > tokens->items[start].line)
      break;
  }
  return start + 1;
}

static ArchbirdStatus add_name_fact(PyContext *context, const char *domain,
                                    const char *kind, size_t name_index,
                                    const uint8_t *key, size_t key_length,
                                    AbFact **out_fact) {
  const AbToken *token = &context->tokens->items[name_index];
  return ab_bundle_builder_add_fact(
      context->builder, domain, kind, "lexical-occurrence", token->start,
      token->end, key, key_length, context->tokens->source + token->start,
      token->end - token->start, out_fact);
}

static ArchbirdStatus add_simple_fact(PyContext *context, const char *domain,
                                      const char *kind, size_t name_index) {
  AbPythonNameRef name = ab_python_token_ref(context->tokens, name_index);
  AbFact *fact = NULL;
  return add_name_fact(context, domain, kind, name_index, name.data,
                       name.length, &fact);
}

static int public_name(const AbTokenList *tokens, size_t index) {
  AbPythonNameRef name = ab_python_token_ref(tokens, index);
  return name.length && name.data[0] != '_';
}

static ArchbirdStatus add_export(PyContext *context, size_t name_index) {
  if (!public_name(context->tokens, name_index))
    return ARCHBIRD_OK;
  return add_simple_fact(context, "exports", "name", name_index);
}

static ArchbirdStatus add_reexport(PyContext *context, size_t name_index) {
  ArchbirdStatus status;
  if (!public_name(context->tokens, name_index))
    return ARCHBIRD_OK;
  status = add_simple_fact(context, "reexport-candidates", "name", name_index);
  if (status == ARCHBIRD_OK && context->init_module)
    status = add_export(context, name_index);
  return status;
}

static ArchbirdStatus add_module_variable(PyContext *context,
                                          size_t name_index) {
  AbPythonNameRef name = ab_python_token_ref(context->tokens, name_index);
  AbFact *fact = NULL;
  ArchbirdStatus status =
      add_name_fact(context, "symbols", "variable", name_index, name.data,
                    name.length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact, "line",
                                       context->tokens->items[name_index].line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        context->builder->engine, fact, "scope", (const uint8_t *)"module", 6);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(context->builder->engine, fact,
                                          "signature", (const uint8_t *)"", 0);
  return status;
}

static ArchbirdStatus add_symbol(PyContext *context,
                                 const AbPythonDefinition *definition) {
  size_t start_index = definition->start_token;
  size_t name_index = definition->name_token;
  const AbToken *name = &context->tokens->items[name_index];
  AbBuffer rendered_signature;
  AbFact *fact = NULL;
  size_t end = definition_end(context->tokens, start_index);
  ArchbirdStatus status;
  status = ab_bundle_builder_add_fact(
      context->builder, "symbols", definition->kind, "lexical-occurrence",
      name->start, name->end, (const uint8_t *)definition->qualified.data,
      definition->qualified.length, (const uint8_t *)definition->qualified.data,
      definition->qualified.length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_u64_attribute(context->builder->engine, fact, "line",
                                       name->line);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        context->builder->engine, fact, "scope",
        (const uint8_t *)definition->kind, strlen(definition->kind));
  ab_buffer_init(&rendered_signature, context->builder->engine);
  if (status == ARCHBIRD_OK)
    status = signature(&rendered_signature, context->tokens, start_index, end);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(context->builder->engine, fact,
                                          "signature", rendered_signature.data,
                                          rendered_signature.length);
  ab_buffer_free(&rendered_signature);
  if (status == ARCHBIRD_OK && definition->indent == 0)
    status = add_export(context, name_index);
  if (status == ARCHBIRD_OK)
    context->definition_names[name_index] = 1;
  return status;
}

static ArchbirdStatus scan_symbols(PyContext *context) {
  size_t index;
  for (index = 0; index < context->definitions->count; index++) {
    ArchbirdStatus status =
        add_symbol(context, &context->definitions->items[index]);
    if (status != ARCHBIRD_OK)
      return status;
  }
  return ARCHBIRD_OK;
}

static int call_keyword(const AbTokenList *tokens, size_t index) {
  static const char *const words[] = {
      "and",   "as",       "assert", "async",  "await",  "case",    "class",
      "def",   "del",      "elif",   "else",   "except", "finally", "for",
      "from",  "global",   "if",     "import", "in",     "is",      "lambda",
      "match", "nonlocal", "not",    "or",     "pass",   "raise",   "return",
      "try",   "while",    "with",   "yield",
  };
  size_t word;
  for (word = 0; word < sizeof(words) / sizeof(words[0]); word++) {
    if (ab_token_equals(tokens, index, words[word]))
      return 1;
  }
  return 0;
}

static ArchbirdStatus scan_calls(PyContext *context) {
  size_t index;
  for (index = 0; index + 1 < context->tokens->count; index++) {
    const char *domain;
    const char *kind;
    if (!ab_python_token_identifier(context->tokens, index) ||
        context->definition_names[index] ||
        call_keyword(context->tokens, index) ||
        !ab_token_equals(context->tokens, index + 1, "("))
      continue;
    if (index > 0 && ab_token_equals(context->tokens, index - 1, ".")) {
      domain = "method-calls";
      kind = "method-call";
    } else {
      domain = "calls";
      kind = "free-call";
    }
    {
      ArchbirdStatus status = add_simple_fact(context, domain, kind, index);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_module_fact(PyContext *context, size_t start,
                                      size_t end, const AbBuffer *module,
                                      const char *domain) {
  AbFact *fact = NULL;
  return ab_bundle_builder_add_fact(
      context->builder, domain, "module", "lexical-occurrence", start, end,
      module->data, module->length, module->data, module->length, &fact);
}

static ArchbirdStatus
module_append_token(AbBuffer *module, const AbTokenList *tokens, size_t index) {
  const AbToken *token = &tokens->items[index];
  return ab_buffer_append(module, tokens->source + token->start,
                          token->end - token->start);
}

static size_t statement_end(const AbTokenList *tokens, size_t start) {
  size_t parens = 0;
  size_t index;
  size_t line = tokens->items[start].line;
  for (index = start + 1; index < tokens->count; index++) {
    if (ab_token_equals(tokens, index, "("))
      parens++;
    else if (ab_token_equals(tokens, index, ")"))
      parens = parens ? parens - 1 : 0;
    else if (!parens && ab_token_equals(tokens, index, ";"))
      return index;
    if (!parens && tokens->items[index].line > line)
      return index;
  }
  return tokens->count;
}

static ArchbirdStatus scan_plain_import(PyContext *context, size_t start,
                                        size_t end) {
  size_t cursor = start + 1;
  while (cursor < end) {
    AbBuffer module;
    size_t module_start;
    size_t module_end;
    size_t local;
    ArchbirdStatus status;
    while (cursor < end && (ab_token_equals(context->tokens, cursor, ",") ||
                            ab_token_equals(context->tokens, cursor, "(")))
      cursor++;
    if (!ab_python_token_identifier(context->tokens, cursor))
      break;
    module_start = cursor;
    local = cursor;
    ab_buffer_init(&module, context->builder->engine);
    status = module_append_token(&module, context->tokens, cursor++);
    while (status == ARCHBIRD_OK && cursor + 1 < end &&
           ab_token_equals(context->tokens, cursor, ".") &&
           ab_python_token_identifier(context->tokens, cursor + 1)) {
      status = ab_buffer_literal(&module, ".");
      if (status == ARCHBIRD_OK)
        status = module_append_token(&module, context->tokens, cursor + 1);
      cursor += 2;
    }
    module_end = context->tokens->items[cursor - 1].end;
    if (status == ARCHBIRD_OK)
      status =
          add_module_fact(context, context->tokens->items[module_start].start,
                          module_end, &module, "imports");
    if (cursor + 1 < end && ab_token_equals(context->tokens, cursor, "as") &&
        ab_python_token_identifier(context->tokens, cursor + 1)) {
      local = cursor + 1;
      cursor += 2;
    }
    if (status == ARCHBIRD_OK)
      status = add_reexport(context, local);
    ab_buffer_free(&module);
    if (status != ARCHBIRD_OK)
      return status;
    while (cursor < end && !ab_token_equals(context->tokens, cursor, ","))
      cursor++;
  }
  return ARCHBIRD_OK;
}

static ArchbirdStatus add_imported_name(PyContext *context, size_t name_index,
                                        const AbBuffer *module) {
  AbBuffer key;
  AbFact *fact = NULL;
  char length[32];
  int written;
  ArchbirdStatus status;
  written = snprintf(length, sizeof(length), "%zu:", module->length);
  if (written < 0 || (size_t)written >= sizeof(length))
    return archbird_error_set(context->builder->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "Python import module length is too large");
  ab_buffer_init(&key, context->builder->engine);
  status = ab_buffer_append(&key, (const uint8_t *)length, (size_t)written);
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(&key, module->data, module->length);
  if (status == ARCHBIRD_OK)
    status = module_append_token(&key, context->tokens, name_index);
  if (status == ARCHBIRD_OK)
    status = add_name_fact(context, "imported-names", "member", name_index,
                           key.data, key.length, &fact);
  if (status == ARCHBIRD_OK)
    status = ab_fact_add_string_attribute(
        context->builder->engine, fact, "module", module->data, module->length);
  ab_buffer_free(&key);
  return status;
}

static ArchbirdStatus scan_from_import(PyContext *context, size_t start,
                                       size_t end) {
  AbBuffer module;
  size_t cursor = start + 1;
  size_t module_start = cursor;
  size_t module_end = cursor;
  size_t import_index = SIZE_MAX;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&module, context->builder->engine);
  while (cursor < end) {
    if (ab_token_equals(context->tokens, cursor, "import")) {
      import_index = cursor;
      break;
    }
    if (ab_token_equals(context->tokens, cursor, ".") ||
        ab_python_token_identifier(context->tokens, cursor)) {
      status = module_append_token(&module, context->tokens, cursor);
      module_end = context->tokens->items[cursor].end;
      if (status != ARCHBIRD_OK)
        goto done;
    }
    cursor++;
  }
  if (import_index == SIZE_MAX || !module.length)
    goto done;
  status = add_module_fact(context, context->tokens->items[module_start].start,
                           module_end, &module, "imports");
  if (status == ARCHBIRD_OK)
    status =
        add_module_fact(context, context->tokens->items[module_start].start,
                        module_end, &module, "imported-name-groups");
  cursor = import_index + 1;
  while (status == ARCHBIRD_OK && cursor < end) {
    size_t original;
    size_t local;
    while (cursor < end && (ab_token_equals(context->tokens, cursor, ",") ||
                            ab_token_equals(context->tokens, cursor, "(") ||
                            ab_token_equals(context->tokens, cursor, ")")))
      cursor++;
    if (cursor >= end)
      break;
    if (ab_token_equals(context->tokens, cursor, "*")) {
      cursor++;
      continue;
    }
    if (!ab_python_token_identifier(context->tokens, cursor))
      break;
    original = cursor;
    local = original;
    cursor++;
    if (cursor + 1 < end && ab_token_equals(context->tokens, cursor, "as") &&
        ab_python_token_identifier(context->tokens, cursor + 1)) {
      local = cursor + 1;
      cursor += 2;
    }
    status = add_imported_name(context, original, &module);
    if (status == ARCHBIRD_OK)
      status = add_reexport(context, local);
  }
done:
  ab_buffer_free(&module);
  return status;
}

static ArchbirdStatus scan_imports(PyContext *context) {
  size_t index;
  for (index = 0; index < context->tokens->count; index++) {
    size_t end;
    ArchbirdStatus status;
    if (ab_token_equals(context->tokens, index, "from")) {
      end = statement_end(context->tokens, index);
      status = scan_from_import(context, index, end);
    } else if (ab_token_equals(context->tokens, index, "import") &&
               !(index > 0 &&
                 ab_token_equals(context->tokens, index - 1, "from"))) {
      end = statement_end(context->tokens, index);
      status = scan_plain_import(context, index, end);
    } else {
      continue;
    }
    if (status != ARCHBIRD_OK)
      return status;
    if (end > index)
      index = end - 1;
  }
  return ARCHBIRD_OK;
}

static int simple_name_assignment(const AbTokenList *tokens,
                                  size_t equal_index) {
  size_t cursor = equal_index + 1;
  if (cursor >= tokens->count ||
      !ab_python_token_same_line(tokens, equal_index, cursor) ||
      !ab_python_token_identifier(tokens, cursor))
    return 0;
  cursor++;
  return cursor >= tokens->count ||
         !ab_python_token_same_line(tokens, equal_index, cursor) ||
         ab_token_equals(tokens, cursor, ";");
}

static ArchbirdStatus scan_assignments(PyContext *context) {
  size_t index;
  for (index = 0; index < context->tokens->count; index++) {
    size_t cursor;
    size_t parens = 0;
    size_t brackets = 0;
    size_t braces = 0;
    int assigned = 0;
    if (!ab_python_token_identifier(context->tokens, index) ||
        ab_python_token_indent(context->tokens, index) != 0 ||
        !ab_python_line_prefix_is_space(context->tokens, index))
      continue;
    for (cursor = index + 1;
         cursor < context->tokens->count &&
         ab_python_token_same_line(context->tokens, index, cursor);
         cursor++) {
      if (ab_token_equals(context->tokens, cursor, "("))
        parens++;
      else if (ab_token_equals(context->tokens, cursor, ")"))
        parens = parens ? parens - 1 : 0;
      else if (ab_token_equals(context->tokens, cursor, "["))
        brackets++;
      else if (ab_token_equals(context->tokens, cursor, "]"))
        brackets = brackets ? brackets - 1 : 0;
      else if (ab_token_equals(context->tokens, cursor, "{"))
        braces++;
      else if (ab_token_equals(context->tokens, cursor, "}"))
        braces = braces ? braces - 1 : 0;
      else if (!parens && !brackets && !braces &&
               ab_token_equals(context->tokens, cursor, "=")) {
        assigned = 1;
        break;
      }
      if (!parens && !brackets && !braces &&
          (ab_token_equals(context->tokens, cursor, ".") ||
           ab_token_equals(context->tokens, cursor, ",") ||
           ab_token_equals(context->tokens, cursor, ";")))
        break;
    }
    if (assigned) {
      ArchbirdStatus status = ARCHBIRD_OK;
      if (!simple_name_assignment(context->tokens, cursor))
        status = add_module_variable(context, index);
      if (status == ARCHBIRD_OK)
        status = add_export(context, index);
      if (status != ARCHBIRD_OK)
        return status;
    }
  }
  return ARCHBIRD_OK;
}

ArchbirdStatus ab_scan_python_file(ArchbirdEngine *engine,
                                   const AbSourceManifest *manifest,
                                   const AbManifestFile *file,
                                   const uint8_t *source, size_t source_length,
                                   const uint8_t implementation_sha256[32],
                                   AbProviderBundle *out_bundle) {
  static const char config_identity[] = "archbird-native-python-lexical-v3";
  static const char boundary[] =
      "conservative Python token patterns; no grammar, binding, dynamic name, "
      "descriptor, or target resolution";
  static const char *const domains[] = {
      "calls",   "exports",      "imported-name-groups", "imported-names",
      "imports", "method-calls", "reexport-candidates",  "symbols",
  };
  AbBundleBuilder builder;
  AbTokenList tokens;
  AbPythonDefinitions definitions;
  PyContext context;
  uint8_t configuration_sha256[32];
  size_t index;
  ArchbirdStatus status;
  memset(&builder, 0, sizeof(builder));
  memset(&tokens, 0, sizeof(tokens));
  memset(&definitions, 0, sizeof(definitions));
  definitions.engine = engine;
  memset(&context, 0, sizeof(context));
  memset(out_bundle, 0, sizeof(*out_bundle));
  status = archbird_sha256((const uint8_t *)config_identity,
                           strlen(config_identity), configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  status = ab_bundle_builder_init_file(
      &builder, engine, manifest, file, "archbird-native-python-lexical", "2",
      implementation_sha256, configuration_sha256);
  if (status != ARCHBIRD_OK)
    return status;
  for (index = 0; index < sizeof(domains) / sizeof(domains[0]); index++) {
    status = ab_bundle_builder_add_capability(
        &builder, domains[index], "bounded", "lexical-occurrence", boundary);
    if (status != ARCHBIRD_OK)
      goto done;
  }
  status = ab_tokenize(engine, source, source_length, AB_LEX_PYTHON, &tokens);
  if (status == ARCHBIRD_INVALID_SCHEMA) {
    static const char unavailable[] =
        "lexical evidence unavailable because an encoded source token could "
        "not be represented as UTF-8";
    ab_bundle_builder_abort(&builder);
    archbird_error_clear(engine);
    status = ab_bundle_builder_init_file(
        &builder, engine, manifest, file, "archbird-native-python-lexical", "2",
        implementation_sha256, configuration_sha256);
    for (index = 0;
         status == ARCHBIRD_OK && index < sizeof(domains) / sizeof(domains[0]);
         index++)
      status = ab_bundle_builder_add_capability(
          &builder, domains[index], "none", "lexical-occurrence", unavailable);
    if (status == ARCHBIRD_OK)
      status = ab_bundle_builder_add_diagnostic(
          &builder, "note", "python-lexical-encoding-inapplicable",
          "Python lexical provider could not decode a source token; other "
          "providers may still contribute evidence",
          0, 0, 0);
    if (status == ARCHBIRD_OK)
      status = ab_bundle_builder_finish(&builder, out_bundle);
    goto done;
  }
  if (status != ARCHBIRD_OK)
    goto done;
  status = ab_python_definitions_collect_tokens(engine, &tokens, &definitions);
  if (status != ARCHBIRD_OK)
    goto done;
  context.builder = &builder;
  context.tokens = &tokens;
  context.definitions = &definitions;
  context.init_module = init_module_path(&file->path);
  context.definition_names = (uint8_t *)ab_calloc(engine, tokens.count, 1);
  if (tokens.count && !context.definition_names) {
    status =
        archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY, ARCHBIRD_NO_OFFSET,
                           "out of memory scanning Python source");
    goto done;
  }
  status = scan_symbols(&context);
  if (status == ARCHBIRD_OK)
    status = scan_calls(&context);
  if (status == ARCHBIRD_OK)
    status = scan_imports(&context);
  if (status == ARCHBIRD_OK)
    status = scan_assignments(&context);
  if (status == ARCHBIRD_OK)
    status = ab_bundle_builder_finish(&builder, out_bundle);
done:
  ab_free(engine, context.definition_names);
  ab_python_definitions_free(&definitions);
  ab_token_list_free(&tokens);
  ab_bundle_builder_abort(&builder);
  return status;
}
