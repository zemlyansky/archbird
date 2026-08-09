#include "act/rename_internal.h"

#include <string.h>

static int role_is(const AbString *role, const char *literal) {
  size_t length = strlen(literal);
  return role && role->length == length &&
         memcmp(role->data, literal, length) == 0;
}

static int prefix_byte(uint8_t value) {
  return value == 'r' || value == 'R' || value == 'u' || value == 'U';
}

int ab_act_python_rename_replacement_span(const AbRenameEvidence *evidence,
                                          const uint8_t *source,
                                          size_t source_length,
                                          const AbString *old_name,
                                          size_t *start, size_t *end,
                                          const char **out_reason) {
  size_t cursor;
  size_t quote_length;
  uint8_t quote;
  *out_reason = NULL;
  if (!evidence || !source || !old_name || !start || !end || *start >= *end ||
      *end > source_length) {
    *out_reason = "the Python occurrence has an invalid source span";
    return 0;
  }
  if (*end - *start == old_name->length &&
      memcmp(source + *start, old_name->data, old_name->length) == 0)
    return 1;
  if (!role_is(evidence->role, "export")) {
    *out_reason = "the Python occurrence span is not the old identifier";
    return 0;
  }

  cursor = *start;
  while (cursor < *end && prefix_byte(source[cursor]))
    cursor++;
  if (cursor == *end || (source[cursor] != '\'' && source[cursor] != '"')) {
    *out_reason = "the Python export is not one exact string literal";
    return 0;
  }
  quote = source[cursor];
  quote_length = cursor + 2 < *end && source[cursor + 1] == quote &&
                         source[cursor + 2] == quote
                     ? 3
                     : 1;
  if (cursor + quote_length + old_name->length + quote_length != *end ||
      memcmp(source + cursor + quote_length, old_name->data,
             old_name->length) != 0) {
    *out_reason =
        "the Python export string is escaped, concatenated, or not the old "
        "identifier";
    return 0;
  }
  if (memcmp(source + *end - quote_length, source + cursor, quote_length) !=
      0) {
    *out_reason = "the Python export string has inconsistent quotes";
    return 0;
  }
  *start = cursor + quote_length;
  *end -= quote_length;
  return 1;
}
