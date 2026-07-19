#include "python/definitions.h"

#include "python/tokens.h"
#include "render_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum PyScopeKind {
  PY_SCOPE_CLASS = 0,
  PY_SCOPE_FUNCTION = 1
} PyScopeKind;

typedef struct PyScope {
  AbPythonNameRef name;
  size_t indent;
  size_t definition_index;
  PyScopeKind kind;
} PyScope;

static ArchbirdStatus scope_push(ArchbirdEngine *engine, PyScope **scopes,
                                 size_t *count, size_t *capacity,
                                 AbPythonNameRef name, size_t indent,
                                 size_t definition_index, PyScopeKind kind) {
  PyScope *resized;
  if (*count == *capacity) {
    size_t next = *capacity ? *capacity * 2 : 8;
    if (next < *capacity || next > engine->options.max_values)
      next = engine->options.max_values;
    if (next <= *capacity || next > SIZE_MAX / sizeof(**scopes))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "Python definition scope limit exceeded");
    resized = (PyScope *)ab_realloc(engine, *scopes, next * sizeof(**scopes));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing Python scopes");
    *scopes = resized;
    *capacity = next;
  }
  (*scopes)[(*count)++] = (PyScope){name, indent, definition_index, kind};
  return ARCHBIRD_OK;
}

static void scope_pop_to_indent(PyScope *scopes, size_t *count, size_t indent,
                                size_t boundary,
                                AbPythonDefinitions *definitions) {
  while (*count && scopes[*count - 1].indent >= indent) {
    definitions->items[scopes[*count - 1].definition_index].scope_end =
        boundary;
    (*count)--;
  }
}

static int class_scope_present(const PyScope *scopes, size_t count) {
  size_t index;
  for (index = 0; index < count; index++) {
    if (scopes[index].kind == PY_SCOPE_CLASS)
      return 1;
  }
  return 0;
}

static ArchbirdStatus qualified_name(ArchbirdEngine *engine,
                                     const AbTokenList *tokens,
                                     const PyScope *scopes, size_t scope_count,
                                     size_t name_index, AbString *out) {
  AbBuffer buffer;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  ab_buffer_init(&buffer, engine);
  memset(out, 0, sizeof(*out));
  for (index = 0; status == ARCHBIRD_OK && index < scope_count; index++) {
    status = ab_buffer_append(&buffer, scopes[index].name.data,
                              scopes[index].name.length);
    if (status == ARCHBIRD_OK)
      status = ab_buffer_literal(&buffer, ".");
  }
  if (status == ARCHBIRD_OK)
    status = ab_buffer_append(
        &buffer, tokens->source + tokens->items[name_index].start,
        tokens->items[name_index].end - tokens->items[name_index].start);
  if (status == ARCHBIRD_OK)
    status =
        ab_string_copy(engine, out, (const char *)buffer.data, buffer.length);
  ab_buffer_free(&buffer);
  return status;
}

void ab_python_definitions_free(AbPythonDefinitions *definitions) {
  size_t index;
  ArchbirdEngine *engine;
  if (!definitions)
    return;
  engine = definitions->engine;
  for (index = 0; index < definitions->count; index++)
    ab_string_free(engine, &definitions->items[index].qualified);
  ab_free(engine, definitions->items);
  memset(definitions, 0, sizeof(*definitions));
}

static ArchbirdStatus
definition_append(ArchbirdEngine *engine, const AbTokenList *tokens,
                  const PyScope *scopes, size_t scope_count, size_t start_token,
                  size_t name_token, size_t indent, PyScopeKind scope_kind,
                  AbPythonDefinitions *definitions) {
  AbPythonDefinition *resized;
  AbPythonDefinition *definition;
  ArchbirdStatus status;
  if (definitions->count == definitions->capacity) {
    size_t next = definitions->capacity ? definitions->capacity * 2 : 16;
    if (next < definitions->capacity || next > engine->options.max_values)
      next = engine->options.max_values;
    if (next <= definitions->capacity || next > SIZE_MAX / sizeof(*resized))
      return archbird_error_set(engine, ARCHBIRD_LIMIT_EXCEEDED,
                                ARCHBIRD_NO_OFFSET,
                                "Python definition limit exceeded");
    resized = (AbPythonDefinition *)ab_realloc(engine, definitions->items,
                                               next * sizeof(*resized));
    if (!resized)
      return archbird_error_set(engine, ARCHBIRD_OUT_OF_MEMORY,
                                ARCHBIRD_NO_OFFSET,
                                "out of memory storing Python definitions");
    definitions->items = resized;
    definitions->capacity = next;
  }
  definition = &definitions->items[definitions->count];
  memset(definition, 0, sizeof(*definition));
  definition->start_token = start_token;
  definition->name_token = name_token;
  definition->name_start = tokens->items[name_token].start;
  definition->name_end = tokens->items[name_token].end;
  definition->line = tokens->items[name_token].line;
  definition->indent = indent;
  definition->scope_end = tokens->source_length;
  definition->parent_index =
      scope_count ? scopes[scope_count - 1].definition_index : SIZE_MAX;
  definition->kind = scope_kind == PY_SCOPE_CLASS               ? "class"
                     : class_scope_present(scopes, scope_count) ? "method"
                                                                : "function";
  status = qualified_name(engine, tokens, scopes, scope_count, name_token,
                          &definition->qualified);
  if (status == ARCHBIRD_OK)
    definitions->count++;
  return status;
}

ArchbirdStatus
ab_python_definitions_collect_tokens(ArchbirdEngine *engine,
                                     const AbTokenList *tokens,
                                     AbPythonDefinitions *definitions) {
  PyScope *scopes = NULL;
  size_t scope_count = 0;
  size_t scope_capacity = 0;
  size_t parens = 0;
  size_t brackets = 0;
  size_t braces = 0;
  size_t index;
  ArchbirdStatus status = ARCHBIRD_OK;
  if (!engine || !tokens || !definitions)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(definitions, 0, sizeof(*definitions));
  definitions->engine = engine;
  for (index = 0; status == ARCHBIRD_OK && index + 1 < tokens->count; index++) {
    size_t start = index;
    size_t name_index;
    size_t indent;
    PyScopeKind kind;
    if (!parens && !brackets && !braces &&
        ab_python_line_prefix_is_space(tokens, index) &&
        !ab_python_previous_line_is_explicit_continuation(tokens, index))
      scope_pop_to_indent(scopes, &scope_count,
                          ab_python_token_indent(tokens, index),
                          tokens->items[index].start, definitions);
    if (ab_token_equals(tokens, index, "async") && index + 2 < tokens->count &&
        ab_python_token_same_line(tokens, index, index + 1) &&
        ab_token_equals(tokens, index + 1, "def")) {
      name_index = index + 2;
      kind = PY_SCOPE_FUNCTION;
    } else if (ab_token_equals(tokens, index, "def")) {
      name_index = index + 1;
      kind = PY_SCOPE_FUNCTION;
    } else if (ab_token_equals(tokens, index, "class")) {
      name_index = index + 1;
      kind = PY_SCOPE_CLASS;
    } else {
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
      continue;
    }
    if (!ab_python_token_identifier(tokens, name_index))
      continue;
    indent = ab_python_token_indent(tokens, start);
    status = definition_append(engine, tokens, scopes, scope_count, start,
                               name_index, indent, kind, definitions);
    if (status == ARCHBIRD_OK)
      status = scope_push(engine, &scopes, &scope_count, &scope_capacity,
                          ab_python_token_ref(tokens, name_index), indent,
                          definitions->count - 1, kind);
    index = name_index;
  }
  scope_pop_to_indent(scopes, &scope_count, 0, tokens->source_length,
                      definitions);
  ab_free(engine, scopes);
  return status;
}

ArchbirdStatus ab_python_definitions_collect(ArchbirdEngine *engine,
                                             const uint8_t *source,
                                             size_t source_length,
                                             AbPythonDefinitions *out) {
  AbTokenList tokens;
  ArchbirdStatus status;
  if (!engine || (!source && source_length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(&tokens, 0, sizeof(tokens));
  memset(out, 0, sizeof(*out));
  out->engine = engine;
  status = ab_tokenize(engine, source, source_length, AB_LEX_PYTHON, &tokens);
  if (status == ARCHBIRD_OK)
    status = ab_python_definitions_collect_tokens(engine, &tokens, out);
  ab_token_list_free(&tokens);
  if (status != ARCHBIRD_OK)
    ab_python_definitions_free(out);
  return status;
}

const AbPythonDefinition *
ab_python_definition_at(const AbPythonDefinitions *definitions,
                        size_t name_start, size_t name_end) {
  size_t low = 0;
  size_t high = definitions ? definitions->count : 0;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    const AbPythonDefinition *candidate = &definitions->items[middle];
    if (candidate->name_start < name_start)
      low = middle + 1;
    else if (candidate->name_start > name_start)
      high = middle;
    else
      return candidate->name_end == name_end ? candidate : NULL;
  }
  return NULL;
}

const AbPythonDefinition *
ab_python_enclosing_at(const AbPythonDefinitions *definitions,
                       size_t source_offset) {
  size_t low = 0;
  size_t high = definitions ? definitions->count : 0;
  size_t index;
  if (!high)
    return NULL;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (definitions->items[middle].name_start <= source_offset)
      low = middle + 1;
    else
      high = middle;
  }
  if (!low)
    return NULL;
  index = low - 1;
  for (;;) {
    const AbPythonDefinition *candidate = &definitions->items[index];
    if (source_offset < candidate->scope_end)
      return candidate;
    if (candidate->parent_index == SIZE_MAX)
      return NULL;
    index = candidate->parent_index;
  }
}

const AbPythonDefinition *
ab_python_definition_parent(const AbPythonDefinitions *definitions,
                            const AbPythonDefinition *definition) {
  if (!definitions || !definition || definition->parent_index == SIZE_MAX ||
      definition->parent_index >= definitions->count)
    return NULL;
  return &definitions->items[definition->parent_index];
}
