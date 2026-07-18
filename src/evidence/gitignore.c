#include "gitignore.h"

#include "archbird_internal.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

typedef enum AbIgnoreTokenKind {
  AB_IGNORE_LITERAL = 1,
  AB_IGNORE_ANY = 2,
  AB_IGNORE_STAR = 3,
  AB_IGNORE_GLOBSTAR = 4,
  AB_IGNORE_GLOBSTAR_DIRECTORY = 5,
  AB_IGNORE_CLASS = 6
} AbIgnoreTokenKind;

struct AbIgnoreToken {
  AbIgnoreTokenKind kind;
  unsigned char literal;
  size_t start;
  size_t end;
};

enum { AB_IGNORE_MAX_RULES = 100000, AB_IGNORE_MAX_PATTERN_BYTES = 65536 };

static int repository_path_valid(const char *path, size_t length) {
  size_t segment = 0;
  size_t index;
  if (!length || path[0] == '/' || path[length - 1] == '/' ||
      (length >= 2 &&
       ((path[0] >= 'A' && path[0] <= 'Z') ||
        (path[0] >= 'a' && path[0] <= 'z')) &&
       path[1] == ':'))
    return 0;
  for (index = 0; index <= length; index++) {
    if (index < length && path[index] != '/') {
      if (path[index] == '\0' || path[index] == '\\')
        return 0;
      continue;
    }
    if (index == segment || (index - segment == 1 && path[segment] == '.') ||
        (index - segment == 2 && path[segment] == '.' &&
         path[segment + 1] == '.'))
      return 0;
    segment = index + 1;
  }
  return 1;
}

static size_t unescaped_trailing_space_start(const char *line, size_t length) {
  while (length && line[length - 1] == ' ') {
    size_t slash = length - 1;
    while (slash && line[slash - 1] == '\\')
      slash--;
    if (((length - 1) - slash) % 2)
      break;
    length--;
  }
  return length;
}

static int escaped_at(const char *text, size_t index) {
  size_t slashes = 0;
  while (index && text[index - 1] == '\\') {
    slashes++;
    index--;
  }
  return (slashes % 2) != 0;
}

static ArchbirdStatus append_token(AbIgnoreSet *set, AbIgnoreRule *rule,
                                   AbIgnoreToken token) {
  AbIgnoreToken *resized;
  if (rule->token_count == SIZE_MAX / sizeof(*rule->tokens))
    return archbird_error_set(set->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "ignore pattern contains too many tokens");
  resized = (AbIgnoreToken *)ab_realloc(set->engine, rule->tokens,
                                        (rule->token_count + 1) *
                                            sizeof(*rule->tokens));
  if (!resized)
    return archbird_error_set(set->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory compiling ignore pattern");
  rule->tokens = resized;
  rule->tokens[rule->token_count++] = token;
  if (rule->token_count > set->max_tokens)
    set->max_tokens = rule->token_count;
  return ARCHBIRD_OK;
}

static ArchbirdStatus compile_rule(AbIgnoreSet *set, AbIgnoreRule *rule) {
  size_t index = 0;
  ArchbirdStatus status = ARCHBIRD_OK;
  while (status == ARCHBIRD_OK && index < rule->pattern.length) {
    const char *pattern = rule->pattern.data;
    size_t length = rule->pattern.length;
    AbIgnoreToken token;
    memset(&token, 0, sizeof(token));
    if (pattern[index] == '\\' && index + 1 < length) {
      token.kind = AB_IGNORE_LITERAL;
      token.literal = (unsigned char)pattern[index + 1];
      index += 2;
    } else if (pattern[index] == '?') {
      token.kind = AB_IGNORE_ANY;
      index++;
    } else if (pattern[index] == '[') {
      size_t end = index + 1;
      if (end < length && (pattern[end] == '!' || pattern[end] == '^'))
        end++;
      if (end < length && pattern[end] == ']')
        end++;
      while (end < length && pattern[end] != ']') {
        if (pattern[end] == '\\' && end + 1 < length)
          end += 2;
        else
          end++;
      }
      if (end < length) {
        token.kind = AB_IGNORE_CLASS;
        token.start = index + 1;
        token.end = end;
        index = end + 1;
      } else {
        token.kind = AB_IGNORE_LITERAL;
        token.literal = '[';
        index++;
      }
    } else if (pattern[index] == '*') {
      size_t end = index;
      int left_boundary = index == 0 || pattern[index - 1] == '/';
      while (end < length && pattern[end] == '*')
        end++;
      if (end - index >= 2 && left_boundary && end < length &&
          pattern[end] == '/') {
        token.kind = AB_IGNORE_GLOBSTAR_DIRECTORY;
        index = end + 1;
      } else if (end - index >= 2 && left_boundary && end == length) {
        token.kind = AB_IGNORE_GLOBSTAR;
        index = end;
      } else {
        token.kind = AB_IGNORE_STAR;
        index = end;
      }
    } else {
      token.kind = AB_IGNORE_LITERAL;
      token.literal = (unsigned char)pattern[index++];
    }
    status = append_token(set, rule, token);
  }
  return status;
}

static ArchbirdStatus append_source(AbIgnoreSet *set, const char *path,
                                    size_t path_length, const uint8_t *bytes,
                                    size_t byte_length, AbIgnoreSource **out) {
  AbIgnoreSource *resized;
  ArchbirdSha256Context context;
  uint8_t digest[32];
  ArchbirdStatus status;
  resized = (AbIgnoreSource *)ab_realloc(set->engine, set->sources,
                                         (set->source_count + 1) *
                                             sizeof(*set->sources));
  if (!resized)
    return archbird_error_set(set->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory recording ignore evidence");
  set->sources = resized;
  *out = &set->sources[set->source_count];
  memset(*out, 0, sizeof(**out));
  status = ab_string_copy(set->engine, &(*out)->path, path, path_length);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_init(&context);
  status = archbird_sha256_update(&context, bytes, byte_length);
  if (status != ARCHBIRD_OK)
    return status;
  archbird_sha256_final(&context, digest);
  archbird_sha256_hex(digest, (*out)->sha256);
  set->source_count++;
  return ARCHBIRD_OK;
}

static ArchbirdStatus append_rule(AbIgnoreSet *set, AbIgnoreSource *source,
                                  const char *path, size_t path_length,
                                  const char *line, size_t length,
                                  size_t line_number) {
  AbIgnoreRule *resized;
  AbIgnoreRule *rule;
  size_t base_length = path_length;
  size_t index;
  int negative = 0;
  int directory_only = 0;
  int basename_only = 1;
  ArchbirdStatus status;
  length = unescaped_trailing_space_start(line, length);
  if (!length || (line[0] == '#' && !escaped_at(line, 0)))
    return ARCHBIRD_OK;
  if (line[0] == '!' && !escaped_at(line, 0)) {
    negative = 1;
    line++;
    length--;
  }
  if (!length)
    return ARCHBIRD_OK;
  if (line[0] == '/' && !escaped_at(line, 0)) {
    line++;
    length--;
    basename_only = 0;
  }
  if (!length)
    return ARCHBIRD_OK;
  if (line[length - 1] == '/' && !escaped_at(line, length - 1)) {
    directory_only = 1;
    length--;
  }
  if (!length)
    return ARCHBIRD_OK;
  for (index = 0; index < length; index++) {
    if (line[index] == '/' && !escaped_at(line, index)) {
      basename_only = 0;
      break;
    }
  }
  while (base_length && path[base_length - 1] != '/')
    base_length--;
  if (base_length)
    base_length--;
  if (set->rule_count >= AB_IGNORE_MAX_RULES)
    return archbird_error_set(set->engine, ARCHBIRD_LIMIT_EXCEEDED,
                              ARCHBIRD_NO_OFFSET,
                              "ignore inputs contain too many rules");
  resized = (AbIgnoreRule *)ab_realloc(
      set->engine, set->rules, (set->rule_count + 1) * sizeof(*set->rules));
  if (!resized)
    return archbird_error_set(set->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory recording ignore rules");
  set->rules = resized;
  rule = &set->rules[set->rule_count];
  memset(rule, 0, sizeof(*rule));
  rule->line = line_number;
  rule->negative = negative;
  rule->directory_only = directory_only;
  rule->basename_only = basename_only;
  status = ab_string_copy(set->engine, &rule->source_path, path, path_length);
  if (status == ARCHBIRD_OK && base_length)
    status = ab_string_copy(set->engine, &rule->base, path, base_length);
  if (status == ARCHBIRD_OK)
    status = ab_string_copy(set->engine, &rule->pattern, line, length);
  if (status == ARCHBIRD_OK)
    status = compile_rule(set, rule);
  if (status != ARCHBIRD_OK) {
    ab_string_free(set->engine, &rule->source_path);
    ab_string_free(set->engine, &rule->base);
    ab_string_free(set->engine, &rule->pattern);
    ab_free(set->engine, rule->tokens);
    memset(rule, 0, sizeof(*rule));
    return status;
  }
  set->rule_count++;
  source->rule_count++;
  return ARCHBIRD_OK;
}

void ab_ignore_set_init(AbIgnoreSet *set, ArchbirdEngine *engine) {
  if (!set)
    return;
  memset(set, 0, sizeof(*set));
  set->engine = engine;
}

ArchbirdStatus ab_ignore_set_add(AbIgnoreSet *set, const char *path,
                                 size_t path_length, const uint8_t *bytes,
                                 size_t byte_length) {
  AbIgnoreSource *source = NULL;
  size_t start = 0;
  size_t line = 1;
  ArchbirdStatus status;
  if (!set || !set->engine || !path ||
      !repository_path_valid(path, path_length) || (!bytes && byte_length))
    return ARCHBIRD_INVALID_ARGUMENT;
  status = append_source(set, path, path_length, bytes, byte_length, &source);
  while (status == ARCHBIRD_OK && start <= byte_length) {
    size_t end = start;
    while (end < byte_length && bytes[end] != '\n')
      end++;
    if (end > start && bytes[end - 1] == '\r')
      end--;
    if (end - start > AB_IGNORE_MAX_PATTERN_BYTES)
      status = archbird_error_set(set->engine, ARCHBIRD_LIMIT_EXCEEDED,
                                  ARCHBIRD_NO_OFFSET,
                                  "ignore pattern exceeds byte limit");
    else
      status = append_rule(set, source, path, path_length,
                           (const char *)bytes + start, end - start, line);
    if (end == byte_length)
      break;
    start = end + 1;
    line++;
  }
  return status;
}

ArchbirdStatus ab_ignore_set_finalize(AbIgnoreSet *set) {
  size_t bytes;
  if (!set || !set->engine)
    return ARCHBIRD_INVALID_ARGUMENT;
  ab_free(set->engine, set->scratch_left);
  ab_free(set->engine, set->scratch_right);
  set->scratch_left = NULL;
  set->scratch_right = NULL;
  if (set->max_tokens == SIZE_MAX)
    return ARCHBIRD_LIMIT_EXCEEDED;
  bytes = set->max_tokens + 1;
  if (!bytes)
    return ARCHBIRD_OK;
  set->scratch_left = (uint8_t *)ab_calloc(set->engine, bytes, 1);
  set->scratch_right = (uint8_t *)ab_calloc(set->engine, bytes, 1);
  if (!set->scratch_left || !set->scratch_right)
    return archbird_error_set(set->engine, ARCHBIRD_OUT_OF_MEMORY,
                              ARCHBIRD_NO_OFFSET,
                              "out of memory preparing ignore matcher");
  return ARCHBIRD_OK;
}

static int class_matches(const AbIgnoreRule *rule, const AbIgnoreToken *token,
                         unsigned char value) {
  size_t index = token->start;
  int negative = 0;
  int matched = 0;
  if (index < token->end &&
      (rule->pattern.data[index] == '!' || rule->pattern.data[index] == '^')) {
    negative = 1;
    index++;
  }
  if (index < token->end && rule->pattern.data[index] == ']') {
    matched = value == ']';
    index++;
  }
  while (index < token->end) {
    unsigned char first;
    unsigned char last;
    if (rule->pattern.data[index] == '\\' && index + 1 < token->end)
      index++;
    first = (unsigned char)rule->pattern.data[index++];
    last = first;
    if (index + 1 < token->end && rule->pattern.data[index] == '-') {
      index++;
      if (rule->pattern.data[index] == '\\' && index + 1 < token->end)
        index++;
      last = (unsigned char)rule->pattern.data[index++];
    }
    if (first <= value && value <= last)
      matched = 1;
  }
  return negative ? !matched : matched;
}

static void epsilon_closure(const AbIgnoreRule *rule, uint8_t *states,
                            int boundary) {
  size_t index;
  for (index = 0; index < rule->token_count; index++) {
    AbIgnoreTokenKind kind;
    if (!states[index])
      continue;
    kind = rule->tokens[index].kind;
    if (kind == AB_IGNORE_STAR || kind == AB_IGNORE_GLOBSTAR ||
        (kind == AB_IGNORE_GLOBSTAR_DIRECTORY && boundary))
      states[index + 1] = 1;
  }
}

static int tokens_match(AbIgnoreSet *set, const AbIgnoreRule *rule,
                        const char *text, size_t length) {
  uint8_t *current = set->scratch_left;
  uint8_t *next = set->scratch_right;
  size_t position;
  if (!current || !next)
    return 0;
  memset(current, 0, rule->token_count + 1);
  current[0] = 1;
  epsilon_closure(rule, current, 1);
  for (position = 0; position < length; position++) {
    unsigned char value = (unsigned char)text[position];
    size_t index;
    uint8_t *swap;
    memset(next, 0, rule->token_count + 1);
    for (index = 0; index < rule->token_count; index++) {
      const AbIgnoreToken *token;
      if (!current[index])
        continue;
      token = &rule->tokens[index];
      switch (token->kind) {
      case AB_IGNORE_LITERAL:
        if (value == token->literal)
          next[index + 1] = 1;
        break;
      case AB_IGNORE_ANY:
        if (value != '/')
          next[index + 1] = 1;
        break;
      case AB_IGNORE_STAR:
        if (value != '/')
          next[index] = 1;
        break;
      case AB_IGNORE_GLOBSTAR:
      case AB_IGNORE_GLOBSTAR_DIRECTORY:
        next[index] = 1;
        break;
      case AB_IGNORE_CLASS:
        if (value != '/' && class_matches(rule, token, value))
          next[index + 1] = 1;
        break;
      }
    }
    epsilon_closure(rule, next, value == '/');
    swap = current;
    current = next;
    next = swap;
  }
  epsilon_closure(rule, current, !length || text[length - 1] == '/');
  if (current != set->scratch_left) {
    memcpy(set->scratch_left, current, rule->token_count + 1);
    current = set->scratch_left;
  }
  return current[rule->token_count] != 0;
}

static int rule_applies(const AbIgnoreRule *rule, const AbString *path,
                        const char **relative, size_t *relative_length) {
  if (!rule->base.length) {
    *relative = path->data;
    *relative_length = path->length;
    return 1;
  }
  if (path->length <= rule->base.length ||
      path->data[rule->base.length] != '/' ||
      memcmp(path->data, rule->base.data, rule->base.length) != 0)
    return 0;
  *relative = path->data + rule->base.length + 1;
  *relative_length = path->length - rule->base.length - 1;
  return 1;
}

static int rule_matches(AbIgnoreSet *set, const AbIgnoreRule *rule,
                        const AbString *path, int directory) {
  const char *relative;
  size_t relative_length;
  if (rule->directory_only && !directory)
    return 0;
  if (!rule_applies(rule, path, &relative, &relative_length))
    return 0;
  if (rule->basename_only) {
    size_t start = relative_length;
    while (start && relative[start - 1] != '/')
      start--;
    relative += start;
    relative_length -= start;
  }
  return tokens_match(set, rule, relative, relative_length);
}

static int decision(AbIgnoreSet *set, const AbString *path, int directory) {
  size_t index;
  int ignored = 0;
  for (index = 0; index < set->rule_count; index++) {
    if (rule_matches(set, &set->rules[index], path, directory))
      ignored = !set->rules[index].negative;
  }
  return ignored;
}

int ab_ignore_set_matches(AbIgnoreSet *set, const AbString *path,
                          int directory) {
  size_t index;
  if (!set || !path || !set->scratch_left || !set->scratch_right)
    return 0;
  for (index = 0; index < path->length; index++) {
    AbString parent;
    if (path->data[index] != '/')
      continue;
    parent.data = path->data;
    parent.length = index;
    if (decision(set, &parent, 1))
      return 1;
  }
  return decision(set, path, directory);
}

void ab_ignore_set_free(AbIgnoreSet *set) {
  size_t index;
  if (!set || !set->engine)
    return;
  for (index = 0; index < set->rule_count; index++) {
    AbIgnoreRule *rule = &set->rules[index];
    ab_string_free(set->engine, &rule->source_path);
    ab_string_free(set->engine, &rule->base);
    ab_string_free(set->engine, &rule->pattern);
    ab_free(set->engine, rule->tokens);
  }
  for (index = 0; index < set->source_count; index++)
    ab_string_free(set->engine, &set->sources[index].path);
  ab_free(set->engine, set->rules);
  ab_free(set->engine, set->sources);
  ab_free(set->engine, set->scratch_left);
  ab_free(set->engine, set->scratch_right);
  memset(set, 0, sizeof(*set));
}
