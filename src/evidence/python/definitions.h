#ifndef ARCHBIRD_PYTHON_DEFINITIONS_H
#define ARCHBIRD_PYTHON_DEFINITIONS_H

#include "fact_builder.h"
#include "lexical/tokenizer.h"

typedef struct AbPythonDefinition {
  size_t start_token;
  size_t name_token;
  size_t name_start;
  size_t name_end;
  size_t line;
  size_t indent;
  size_t scope_end;
  size_t parent_index;
  const char *kind;
  AbString qualified;
} AbPythonDefinition;

typedef struct AbPythonDefinitions {
  ArchbirdEngine *engine;
  AbPythonDefinition *items;
  size_t count;
  size_t capacity;
} AbPythonDefinitions;

ArchbirdStatus ab_python_definitions_collect(ArchbirdEngine *engine,
                                             const uint8_t *source,
                                             size_t source_length,
                                             AbPythonDefinitions *out);
ArchbirdStatus ab_python_definitions_collect_tokens(ArchbirdEngine *engine,
                                                    const AbTokenList *tokens,
                                                    AbPythonDefinitions *out);

const AbPythonDefinition *
ab_python_definition_at(const AbPythonDefinitions *definitions,
                        size_t name_start, size_t name_end);

const AbPythonDefinition *
ab_python_enclosing_at(const AbPythonDefinitions *definitions,
                       size_t source_offset);

const AbPythonDefinition *
ab_python_definition_parent(const AbPythonDefinitions *definitions,
                            const AbPythonDefinition *definition);

void ab_python_definitions_free(AbPythonDefinitions *definitions);

#endif
