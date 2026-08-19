#include "evidence/manifests/cmake_project_manifest.h"

#include "base/archbird_internal.h"
#include "base/utf8.h"

#include <string.h>

typedef struct Span {
  size_t start;
  size_t end;
  int literal;
} Span;

typedef enum BlockKind {
  BLOCK_IF,
  BLOCK_FOREACH,
  BLOCK_WHILE,
  BLOCK_FUNCTION,
  BLOCK_MACRO,
  BLOCK_SCOPE
} BlockKind;

enum { MAX_CMAKE_BLOCK_DEPTH = 256, MAX_PROJECT_ARGUMENTS = 64 };

static int identifier_start(uint8_t byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
         byte == '_';
}

static int identifier_continue(uint8_t byte) {
  return identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static int ascii_case_equal(const uint8_t *text, size_t start, size_t end,
                            const char *literal) {
  size_t index;
  size_t length = strlen(literal);
  if (end - start != length)
    return 0;
  for (index = 0; index < length; index++) {
    unsigned char actual = text[start + index];
    unsigned char wanted = (unsigned char)literal[index];
    if (actual >= 'A' && actual <= 'Z')
      actual = (unsigned char)(actual - 'A' + 'a');
    if (wanted >= 'A' && wanted <= 'Z')
      wanted = (unsigned char)(wanted - 'A' + 'a');
    if (actual != wanted)
      return 0;
  }
  return 1;
}

static size_t bracket_open(const uint8_t *text, size_t length, size_t start,
                           int comment, size_t *out_equals) {
  size_t index = start + (comment ? 2 : 1);
  size_t equals = 0;
  if (start >= length || (comment ? start + 1 >= length : 0) ||
      (comment ? text[start] != '#' || text[start + 1] != '['
               : text[start] != '['))
    return 0;
  while (index < length && text[index] == '=') {
    equals++;
    index++;
  }
  if (index >= length || text[index] != '[')
    return 0;
  *out_equals = equals;
  return index + 1;
}

static size_t bracket_close(const uint8_t *text, size_t length, size_t start,
                            size_t equals) {
  size_t index;
  for (index = start; index < length; index++) {
    size_t count = 0;
    if (text[index] != ']')
      continue;
    while (index + 1 + count < length && text[index + 1 + count] == '=')
      count++;
    if (count == equals && index + 1 + count < length &&
        text[index + 1 + count] == ']')
      return index + count + 2;
  }
  return 0;
}

static int skip_comment(const uint8_t *text, size_t length, size_t *index) {
  size_t equals;
  size_t body = bracket_open(text, length, *index, 1, &equals);
  if (body) {
    size_t close = bracket_close(text, length, body, equals);
    if (!close)
      return 0;
    *index = close;
    return 1;
  }
  while (*index < length && text[*index] != '\n')
    (*index)++;
  return 1;
}

static int skip_space_comments(const uint8_t *text, size_t length,
                               size_t *index) {
  while (*index < length) {
    uint8_t byte = text[*index];
    if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
      (*index)++;
      continue;
    }
    if (byte != '#')
      break;
    if (!skip_comment(text, length, index))
      return 0;
  }
  return 1;
}

static int invocation_end(const uint8_t *text, size_t length, size_t open,
                          size_t *out_end) {
  size_t index = open + 1;
  size_t depth = 1;
  while (index < length) {
    uint8_t byte = text[index];
    if (byte == '#') {
      if (!skip_comment(text, length, &index))
        return 0;
      continue;
    }
    if (byte == '"') {
      index++;
      while (index < length && text[index] != '"') {
        if (text[index] == '\\' && index + 1 < length)
          index += 2;
        else
          index++;
      }
      if (index == length)
        return 0;
      index++;
      continue;
    }
    if (byte == '[') {
      size_t equals;
      size_t body = bracket_open(text, length, index, 0, &equals);
      if (body) {
        size_t close = bracket_close(text, length, body, equals);
        if (!close)
          return 0;
        index = close;
        continue;
      }
    }
    if (byte == '(')
      depth++;
    else if (byte == ')' && !--depth) {
      *out_end = index;
      return 1;
    }
    index++;
  }
  return 0;
}

static int argument_literal(const uint8_t *text, Span span) {
  size_t index;
  if (!span.literal || span.start == span.end)
    return 0;
  for (index = span.start; index < span.end; index++)
    if (text[index] == '$' || text[index] == '\\' || text[index] == ';' ||
        text[index] == '(' || text[index] == ')' || text[index] == '\0')
      return 0;
  return 1;
}

static int project_arguments(const uint8_t *text, size_t length, size_t open,
                             size_t close, Span *arguments, size_t *out_count) {
  size_t index = open + 1;
  size_t count = 0;
  while (index < close) {
    Span span = {0, 0, 1};
    if (!skip_space_comments(text, close, &index))
      return 0;
    if (index >= close)
      break;
    if (count == MAX_PROJECT_ARGUMENTS)
      return 0;
    if (text[index] == '"') {
      span.start = ++index;
      while (index < close && text[index] != '"') {
        if (text[index] == '\\' && index + 1 < close) {
          span.literal = 0;
          index += 2;
        } else {
          index++;
        }
      }
      if (index == close)
        return 0;
      span.end = index++;
    } else if (text[index] == '[') {
      size_t equals;
      size_t body = bracket_open(text, close, index, 0, &equals);
      size_t end;
      if (!body)
        return 0;
      end = bracket_close(text, close, body, equals);
      if (!end)
        return 0;
      span.start = body;
      span.end = end - equals - 2;
      index = end;
    } else {
      span.start = index;
      while (index < close && text[index] != ' ' && text[index] != '\t' &&
             text[index] != '\r' && text[index] != '\n' && text[index] != '#')
        index++;
      span.end = index;
      if (span.start == span.end)
        return 0;
    }
    arguments[count++] = span;
  }
  *out_count = count;
  (void)length;
  return 1;
}

static int definition_may_shadow_project(const uint8_t *text, size_t length,
                                         size_t open, size_t close) {
  Span arguments[MAX_PROJECT_ARGUMENTS];
  size_t argument_count = 0;
  if (!project_arguments(text, length, open, close, arguments,
                         &argument_count) ||
      !argument_count || !argument_literal(text, arguments[0]))
    return 1;
  return ascii_case_equal(text, arguments[0].start, arguments[0].end,
                          "project");
}

static ArchbirdStatus copy_project_call(ArchbirdEngine *engine,
                                        const uint8_t *text, size_t length,
                                        size_t open, size_t close,
                                        AbCmakeProjectMetadata *metadata) {
  Span arguments[MAX_PROJECT_ARGUMENTS];
  size_t argument_count = 0;
  if (!project_arguments(text, length, open, close, arguments,
                         &argument_count) ||
      !argument_count || !argument_literal(text, arguments[0]))
    return ARCHBIRD_OK;
  if (ab_string_copy(engine, &metadata->name,
                     (const char *)text + arguments[0].start,
                     arguments[0].end - arguments[0].start) != ARCHBIRD_OK)
    return ARCHBIRD_OUT_OF_MEMORY;
  return ARCHBIRD_OK;
}

static int opening_block(const uint8_t *text, size_t start, size_t end,
                         BlockKind *out) {
  if (ascii_case_equal(text, start, end, "if"))
    *out = BLOCK_IF;
  else if (ascii_case_equal(text, start, end, "foreach"))
    *out = BLOCK_FOREACH;
  else if (ascii_case_equal(text, start, end, "while"))
    *out = BLOCK_WHILE;
  else if (ascii_case_equal(text, start, end, "function"))
    *out = BLOCK_FUNCTION;
  else if (ascii_case_equal(text, start, end, "macro"))
    *out = BLOCK_MACRO;
  else if (ascii_case_equal(text, start, end, "block"))
    *out = BLOCK_SCOPE;
  else
    return 0;
  return 1;
}

static int closing_block(const uint8_t *text, size_t start, size_t end,
                         BlockKind *out) {
  if (ascii_case_equal(text, start, end, "endif"))
    *out = BLOCK_IF;
  else if (ascii_case_equal(text, start, end, "endforeach"))
    *out = BLOCK_FOREACH;
  else if (ascii_case_equal(text, start, end, "endwhile"))
    *out = BLOCK_WHILE;
  else if (ascii_case_equal(text, start, end, "endfunction"))
    *out = BLOCK_FUNCTION;
  else if (ascii_case_equal(text, start, end, "endmacro"))
    *out = BLOCK_MACRO;
  else if (ascii_case_equal(text, start, end, "endblock"))
    *out = BLOCK_SCOPE;
  else
    return 0;
  return 1;
}

ArchbirdStatus ab_cmake_project_metadata(ArchbirdEngine *engine,
                                         const uint8_t *text, size_t length,
                                         AbCmakeProjectMetadata *out) {
  BlockKind blocks[MAX_CMAKE_BLOCK_DEPTH];
  size_t block_count = 0;
  size_t project_calls = 0;
  size_t index = 0;
  int malformed = 0;
  int project_shadowed = 0;
  int nested_project = 0;
  ArchbirdStatus status;
  if (!engine || (!text && length) || !out)
    return ARCHBIRD_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  status = ab_utf8_validate(engine, text, length);
  if (length >= 3 && text[0] == 0xef && text[1] == 0xbb && text[2] == 0xbf)
    index = 3;
  while (status == ARCHBIRD_OK && !malformed && index < length) {
    size_t name_start;
    size_t name_end;
    size_t open;
    size_t close;
    BlockKind block;
    if (!skip_space_comments(text, length, &index)) {
      malformed = 1;
      break;
    }
    if (index == length)
      break;
    if (!identifier_start(text[index])) {
      /* A top-level CMake listfile consists of command invocations, comments,
       * and whitespace. Reject stray tokens instead of finding a project-like
       * substring inside malformed quoted, bracketed, or variable text. */
      malformed = 1;
      break;
    }
    name_start = index++;
    while (index < length && identifier_continue(text[index]))
      index++;
    name_end = index;
    if (!skip_space_comments(text, length, &index)) {
      malformed = 1;
      break;
    }
    if (index == length || text[index] != '(') {
      malformed = 1;
      break;
    }
    open = index;
    if (!invocation_end(text, length, open, &close)) {
      malformed = 1;
      break;
    }
    index = close + 1;
    if (closing_block(text, name_start, name_end, &block)) {
      if (!block_count || blocks[block_count - 1] != block)
        malformed = 1;
      else
        block_count--;
      continue;
    }
    if (ascii_case_equal(text, name_start, name_end, "project")) {
      if (block_count == 0) {
        project_calls++;
        if (project_calls == 1 && !project_shadowed)
          status = copy_project_call(engine, text, length, open, close, out);
        else {
          ab_string_free(engine, &out->name);
          out->identity_unsupported = 1;
        }
      } else
        nested_project = 1;
    }
    if (block_count == 0 && !project_calls &&
        ascii_case_equal(text, name_start, name_end, "return")) {
      /* A root return ends listfile processing, so a later lexical project()
       * call is unreachable. A return after project() does not erase the
       * identity already established by CMake. */
      out->identity_unsupported = 1;
      break;
    }
    if (opening_block(text, name_start, name_end, &block)) {
      if ((block == BLOCK_FUNCTION || block == BLOCK_MACRO) &&
          definition_may_shadow_project(text, length, open, close))
        project_shadowed = 1;
      if (block_count == MAX_CMAKE_BLOCK_DEPTH)
        malformed = 1;
      else
        blocks[block_count++] = block;
    }
  }
  if (status == ARCHBIRD_OK && (malformed || block_count)) {
    ab_string_free(engine, &out->name);
    out->identity_invalid = 1;
    out->identity_unsupported = 0;
  } else if (status == ARCHBIRD_OK && !out->name.length &&
             (project_calls || nested_project || out->identity_unsupported)) {
    out->identity_unsupported = 1;
  }
  if (status != ARCHBIRD_OK)
    ab_cmake_project_metadata_free(engine, out);
  return status;
}

void ab_cmake_project_metadata_free(ArchbirdEngine *engine,
                                    AbCmakeProjectMetadata *metadata) {
  if (!metadata)
    return;
  ab_string_free(engine, &metadata->name);
  memset(metadata, 0, sizeof(*metadata));
}
