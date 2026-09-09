#include "map/map_internal.h"

#include "evidence/lexical/tokenizer.h"
#include "evidence/project_internal.h"

#include <string.h>

/* This is a conditional-include evaluator, not a macro expander. Unknown
 * compiler builtins, included macro state and unsupported expressions cannot
 * justify deleting an edge. The syntax provider remains build independent. */
typedef struct PpMacro {
  const uint8_t *name;
  size_t length;
  int defined;
  int value;
} PpMacro;

typedef struct PpEnvironment {
  PpMacro *items;
  size_t count;
} PpEnvironment;

typedef struct PpBranch {
  int parent;
  int taken;
  int saw_else;
} PpBranch;

static int pp_not(int value) { return value ? -value : 0; }
static int pp_and(int left, int right) {
  return left == -1 || right == -1 ? -1 : (left == 1 && right == 1 ? 1 : 0);
}
static int pp_or(int left, int right) {
  return left == 1 || right == 1 ? 1 : (left == -1 && right == -1 ? -1 : 0);
}

static PpMacro *pp_lookup(PpEnvironment *environment, const uint8_t *name,
                          size_t length) {
  size_t index;
  for (index = 0; index < environment->count; index++) {
    PpMacro *macro = &environment->items[index];
    if (macro->length == length && !memcmp(macro->name, name, length))
      return macro;
  }
  return NULL;
}

static void pp_set(PpEnvironment *environment, const uint8_t *name,
                   size_t length, int defined, int value) {
  PpMacro *macro = pp_lookup(environment, name, length);
  if (!macro) {
    macro = &environment->items[environment->count++];
    macro->name = name;
    macro->length = length;
  }
  macro->defined = defined;
  macro->value = value;
}

static int pp_integer(const uint8_t *bytes, size_t length) {
  size_t index = 0;
  unsigned base = 10;
  int nonzero = 0;
  size_t digits = 0;
  uint64_t value = 0;
  int is_unsigned = 0;
  if (length > 1 && bytes[0] == '0') {
    base = 8;
    if (bytes[1] == 'x' || bytes[1] == 'X') {
      base = 16;
      index = 2;
    }
  }
  for (; index < length; index++) {
    unsigned digit;
    unsigned char byte = bytes[index];
    if (byte >= '0' && byte <= '9')
      digit = byte - '0';
    else if (byte >= 'a' && byte <= 'f')
      digit = byte - 'a' + 10;
    else if (byte >= 'A' && byte <= 'F')
      digit = byte - 'A' + 10;
    else
      break;
    if (digit >= base)
      return 0;
    /* Out-of-range literals can wrap in a compiler extension. Do not infer
     * their truth value from nonzero digits or invoke signed overflow here. */
    if (value > (UINT64_MAX - digit) / base)
      return 0;
    value = value * base + digit;
    nonzero |= digit != 0;
    digits++;
  }
  if (!digits)
    return 0;
  if (index < length && (bytes[index] == 'u' || bytes[index] == 'U')) {
    is_unsigned = 1;
    index++;
  }
  if (index < length && (bytes[index] == 'l' || bytes[index] == 'L')) {
    uint8_t suffix = bytes[index++];
    if (index < length && bytes[index] == suffix)
      index++;
  }
  if (!is_unsigned && index < length &&
      (bytes[index] == 'u' || bytes[index] == 'U'))
    index++;
  return index == length ? (nonzero ? 1 : -1) : 0;
}

static int pp_expression(const AbTokenList *tokens, size_t *cursor, size_t end,
                         PpEnvironment *environment, unsigned depth);

static int pp_primary(const AbTokenList *tokens, size_t *cursor, size_t end,
                      PpEnvironment *environment, unsigned depth) {
  const AbToken *token;
  PpMacro *macro;
  int value;
  if (*cursor >= end || depth >= 64)
    return 0;
  if (ab_token_equals(tokens, *cursor, "!")) {
    (*cursor)++;
    return pp_not(pp_primary(tokens, cursor, end, environment, depth + 1));
  }
  if (ab_token_equals(tokens, *cursor, "(")) {
    (*cursor)++;
    value = pp_expression(tokens, cursor, end, environment, depth + 1);
    if (*cursor >= end || !ab_token_equals(tokens, (*cursor)++, ")"))
      return 0;
    return value;
  }
  if (ab_token_equals(tokens, *cursor, "defined")) {
    int parenthesized;
    (*cursor)++;
    parenthesized = ab_token_equals(tokens, *cursor, "(");
    if (parenthesized)
      (*cursor)++;
    if (*cursor >= end || tokens->items[*cursor].kind != AB_TOKEN_IDENTIFIER)
      return 0;
    token = &tokens->items[(*cursor)++];
    macro = pp_lookup(environment, tokens->source + token->start,
                      token->end - token->start);
    value = macro ? macro->defined : 0;
    if (parenthesized &&
        (*cursor >= end || !ab_token_equals(tokens, (*cursor)++, ")")))
      return 0;
    return value;
  }
  token = &tokens->items[(*cursor)++];
  if (token->kind == AB_TOKEN_NUMBER)
    return pp_integer(tokens->source + token->start, token->end - token->start);
  if (token->kind != AB_TOKEN_IDENTIFIER)
    return 0;
  macro = pp_lookup(environment, tokens->source + token->start,
                    token->end - token->start);
  return macro ? (macro->defined == -1 ? -1 : macro->value) : 0;
}

static int pp_conjunction(const AbTokenList *tokens, size_t *cursor, size_t end,
                          PpEnvironment *environment, unsigned depth) {
  int value = pp_primary(tokens, cursor, end, environment, depth);
  while (*cursor < end && ab_token_equals(tokens, *cursor, "&&")) {
    (*cursor)++;
    value = pp_and(value, pp_primary(tokens, cursor, end, environment, depth));
  }
  return value;
}

static int pp_expression(const AbTokenList *tokens, size_t *cursor, size_t end,
                         PpEnvironment *environment, unsigned depth) {
  int value = pp_conjunction(tokens, cursor, end, environment, depth);
  while (*cursor < end && ab_token_equals(tokens, *cursor, "||")) {
    (*cursor)++;
    value =
        pp_or(value, pp_conjunction(tokens, cursor, end, environment, depth));
  }
  return value;
}

static int pp_condition(const AbTokenList *tokens, size_t start, size_t end,
                        PpEnvironment *environment) {
  int value = pp_expression(tokens, &start, end, environment, 0);
  return start == end ? value : 0;
}

static void pp_seed(PpEnvironment *environment,
                    const AbMapCompileCommand *command) {
  size_t index;
  if (!command || command->preprocessing_unknown)
    return;
  for (index = 0; index < command->preprocessing_options.count; index++) {
    const AbString *option = &command->preprocessing_options.items[index];
    size_t end = 1;
    int value = 1;
    while (end < option->length && option->data[end] != '=' &&
           option->data[end] != '(')
      end++;
    if (end < option->length)
      value = option->data[end] == '='
                  ? pp_integer((const uint8_t *)option->data + end + 1,
                               option->length - end - 1)
                  : 0;
    pp_set(environment, (const uint8_t *)option->data + 1, end - 1,
           option->data[0] == 'U' ? -1 : 1, value);
  }
}

static void pp_forget(PpEnvironment *environment) {
  size_t index;
  for (index = 0; index < environment->count; index++) {
    environment->items[index].defined = 0;
    environment->items[index].value = 0;
  }
}

static void pp_evaluate(const AbTokenList *tokens, PpEnvironment *environment,
                        PpBranch *stack, signed char *states) {
  size_t index;
  size_t depth = 0;
  int active = 1;
  int malformed = 0;
  for (index = 0; index < tokens->count; index++) {
    size_t end;
    size_t operand;
    size_t directive;
    int condition;
    states[index] = (signed char)active;
    if (!ab_token_equals(tokens, index, "#") ||
        (index && tokens->items[index - 1].line == tokens->items[index].line))
      continue;
    {
      size_t byte = tokens->items[index].start;
      /* A directive ends at an unspliced physical newline. Expressions with
       * spliced tokens remain unknown, but cannot swallow later directives. */
      for (; byte < tokens->source_length; byte++) {
        if (tokens->source[byte] == '\n' &&
            (byte == 0 || (tokens->source[byte - 1] != '\\' &&
                           !(byte > 1 && tokens->source[byte - 1] == '\r' &&
                             tokens->source[byte - 2] == '\\'))))
          break;
      }
      end = index + 1;
      while (end < tokens->count && tokens->items[end].start < byte)
        end++;
    }
    directive = index + 1;
    operand = index + 2;
    if (ab_token_equals(tokens, directive, "if") ||
        ab_token_equals(tokens, directive, "ifdef") ||
        ab_token_equals(tokens, directive, "ifndef")) {
      if (ab_token_equals(tokens, directive, "if")) {
        condition = pp_condition(tokens, operand, end, environment);
      } else {
        PpMacro *macro = NULL;
        if (operand + 1 == end &&
            tokens->items[operand].kind == AB_TOKEN_IDENTIFIER) {
          const AbToken *name = &tokens->items[operand];
          macro = pp_lookup(environment, tokens->source + name->start,
                            name->end - name->start);
        }
        condition = macro ? macro->defined : 0;
        if (ab_token_equals(tokens, directive, "ifndef"))
          condition = pp_not(condition);
      }
      stack[depth++] = (PpBranch){active, condition, 0};
      active = pp_and(active, condition);
    } else if (ab_token_equals(tokens, directive, "else") ||
               ab_token_equals(tokens, directive, "elif")) {
      if (!depth || stack[depth - 1].saw_else) {
        malformed = 1;
      } else {
        PpBranch *branch = &stack[depth - 1];
        int is_else = ab_token_equals(tokens, directive, "else");
        condition =
            is_else ? 1 : pp_condition(tokens, operand, end, environment);
        active =
            pp_and(branch->parent, pp_and(pp_not(branch->taken), condition));
        branch->taken = pp_or(branch->taken, condition);
        branch->saw_else = is_else;
      }
    } else if (ab_token_equals(tokens, directive, "endif")) {
      if (!depth)
        malformed = 1;
      else
        active = stack[--depth].parent;
    } else if (active != -1 && (ab_token_equals(tokens, directive, "define") ||
                                ab_token_equals(tokens, directive, "undef"))) {
      if (operand < end && tokens->items[operand].kind == AB_TOKEN_IDENTIFIER) {
        const AbToken *name = &tokens->items[operand];
        int undef = ab_token_equals(tokens, directive, "undef");
        int value =
            undef ? -1
                  : (operand + 2 == end &&
                             tokens->items[operand + 1].kind == AB_TOKEN_NUMBER
                         ? pp_integer(tokens->source +
                                          tokens->items[operand + 1].start,
                                      tokens->items[operand + 1].end -
                                          tokens->items[operand + 1].start)
                         : 0);
        pp_set(environment, tokens->source + name->start,
               name->end - name->start, active == 0 ? 0 : (undef ? -1 : 1),
               active == 0 ? 0 : value);
      }
    } else if (active != -1 &&
               (ab_token_equals(tokens, directive, "include") ||
                ab_token_equals(tokens, directive, "include_next"))) {
      /* Even a resolved header can redefine command-line macros. Without an
       * ordered expansion witness, following conditions must stay unknown. */
      pp_forget(environment);
    }
    while (++index < end)
      states[index] =
          (signed char)(ab_token_equals(tokens, directive, "include")
                            ? states[directive - 1]
                            : active);
    index--;
  }
  if (depth || malformed)
    memset(states, 0, tokens->count);
}

static int pp_state_at(const AbTokenList *tokens, const signed char *states,
                       size_t offset) {
  size_t low = 0;
  size_t high = tokens->count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (tokens->items[middle].end <= offset)
      low = middle + 1;
    else
      high = middle;
  }
  return low < tokens->count ? states[low] : 0;
}

static int pp_space(uint8_t value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\v' || value == '\f';
}

static ArchbirdStatus pp_file(AbMapState *state, AbMapNamedReference *imports,
                              size_t count) {
  const AbManifestFile *file = imports[0].file;
  const uint8_t *source = ab_project_source_bytes(
      state->project, (size_t)(file - state->manifest->files));
  AbTokenList tokens = {0};
  PpEnvironment environment = {0};
  PpBranch *stack = NULL;
  signed char *states = NULL;
  size_t command_index;
  size_t file_index = (size_t)(file - state->manifest->files);
  size_t start = state->compile_file_direct_context_offsets
                     ? state->compile_file_direct_context_offsets[file_index]
                     : 0;
  size_t end = state->compile_file_direct_context_offsets
                   ? state->compile_file_direct_context_offsets[file_index + 1]
                   : 0;
  size_t index;
  size_t option_count = 0;
  size_t runs = 0;
  uint8_t *logical_source = NULL;
  int joined = 0;
  ArchbirdStatus status;
  if (start == end) {
    /* Header search roots are not a macro environment: includers can mutate
     * macros in order. Preserve the build-independent structural projection
     * when there is no direct compilation context to evaluate. */
    for (index = 0; index < count; index++)
      imports[index].preprocessing = 2;
    return ARCHBIRD_OK;
  }
  if (!source)
    return ARCHBIRD_OK;
  for (index = 0; index + 1 < file->byte_length; index++) {
    size_t splice_end = index + 1;
    if (source[index] != '\\')
      continue;
    if (source[splice_end] == '\r' && splice_end + 1 < file->byte_length)
      splice_end++;
    if (source[splice_end] != '\n')
      continue;
    splice_end++;
    if (!logical_source) {
      logical_source = ab_malloc(state->engine, file->byte_length);
      if (!logical_source)
        return ARCHBIRD_OUT_OF_MEMORY;
      memcpy(logical_source, source, file->byte_length);
    }
    /* Splicing precedes comment recognition. Preserve byte offsets while
     * joining logical lines; token-pasting splices need compiler evidence. */
    if (index && splice_end < file->byte_length &&
        !pp_space(source[index - 1]) && !pp_space(source[splice_end]))
      joined = 1;
    memset(logical_source + index, ' ', splice_end - index);
    index = splice_end - 1;
  }
  status = ab_tokenize(state->engine, logical_source ? logical_source : source,
                       file->byte_length, 0, &tokens);
  if (status != ARCHBIRD_OK)
    goto done;
  for (command_index = start; command_index < end; command_index++) {
    const AbMapCompileContext *context =
        &state->compile_contexts
             [state->compile_file_direct_context_ids[command_index]];
    const AbMapCompileCommand *command =
        &state->compile_commands[context->representative_command];
    if (command->preprocessing_options.count > option_count)
      option_count = command->preprocessing_options.count;
  }
  if (option_count > SIZE_MAX - tokens.count ||
      option_count + tokens.count > SIZE_MAX / sizeof(*environment.items)) {
    status = ARCHBIRD_LIMIT_EXCEEDED;
    goto done;
  }
  environment.items = ab_calloc(state->engine, option_count + tokens.count,
                                sizeof(*environment.items));
  stack = ab_calloc(state->engine, tokens.count, sizeof(*stack));
  states = ab_calloc(state->engine, tokens.count, sizeof(*states));
  if (((tokens.count + option_count) && !environment.items) ||
      (tokens.count && (!stack || !states))) {
    status = ARCHBIRD_OUT_OF_MEMORY;
    goto done;
  }
  for (command_index = start; command_index <= end; command_index++) {
    const AbMapCompileContext *context =
        command_index < end
            ? &state->compile_contexts
                   [state->compile_file_direct_context_ids[command_index]]
            : NULL;
    const AbMapCompileCommand *command =
        context ? &state->compile_commands[context->representative_command]
                : NULL;
    if (!command && runs)
      break;
    environment.count = 0;
    pp_seed(&environment, command);
    pp_evaluate(&tokens, &environment, stack, states);
    if (joined)
      memset(states, 0, tokens.count);
    for (index = 0; index < count; index++) {
      int active = pp_state_at(&tokens, states, imports[index].span_start);
      if (!runs)
        imports[index].preprocessing = active;
      else if (imports[index].preprocessing != active)
        imports[index].preprocessing = 0;
    }
    runs++;
  }
  for (index = 0; status == ARCHBIRD_OK && index < count; index++)
    if (!imports[index].preprocessing) {
      status = ab_map_add_diagnostic(state, "warning", "c-preprocessor-unknown",
                                     "include activity is unknown: conditional "
                                     "expression, build variants, "
                                     "line splicing or included macro state "
                                     "requires preprocessing evidence",
                                     &file->path);
      break;
    }
done:
  ab_free(state->engine, states);
  ab_free(state->engine, stack);
  ab_free(state->engine, environment.items);
  ab_token_list_free(&tokens);
  ab_free(state->engine, logical_source);
  return status;
}

ArchbirdStatus ab_map_condition_imports(AbMapState *state,
                                        AbMapNamedReference *imports,
                                        size_t count) {
  size_t start = 0;
  while (start < count) {
    size_t end = start + 1;
    const AbManifestFile *file = imports[start].file;
    ArchbirdStatus status = ARCHBIRD_OK;
    while (end < count && imports[end].file == file)
      end++;
    if ((file->language.length == 1 && !memcmp(file->language.data, "c", 1)) ||
        (file->language.length == 3 && !memcmp(file->language.data, "cpp", 3)))
      status = pp_file(state, imports + start, end - start);
    if (status != ARCHBIRD_OK)
      return status;
    start = end;
  }
  return ARCHBIRD_OK;
}
